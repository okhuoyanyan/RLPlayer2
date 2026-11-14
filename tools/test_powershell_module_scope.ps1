param([string]$WorkDir='')
$ErrorActionPreference='Stop'
if(-not $WorkDir){$WorkDir=Join-Path $env:TEMP ('rlplayer-module-scope-'+[guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $WorkDir|Out-Null
$helper=Join-Path $PSScriptRoot 'powershell_module_scope.ps1'
$fixture=Join-Path $WorkDir 'abc.bin';[IO.File]::WriteAllBytes($fixture,[byte[]](97,98,99))
$expected='BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD'
$fake=Join-Path $WorkDir 'poison\Microsoft.PowerShell.Utility';New-Item -ItemType Directory -Path $fake -Force|Out-Null
# A valid higher-priority foreign Utility module forces a real hash failure in both processes.
[IO.File]::WriteAllText((Join-Path $fake 'Microsoft.PowerShell.Utility.psm1'),'function Get-FileHash { throw "incompatible foreign Utility module" }; Export-ModuleMember -Function Get-FileHash')
$child=Join-Path $WorkDir 'child.ps1'
@'
param($Fixture,$Expected)
$ErrorActionPreference='Stop'
try {if((Get-FileHash -LiteralPath $Fixture -Algorithm SHA256).Hash -cne $Expected){exit 4};exit 0}catch{Write-Output $_.Exception.Message;exit 3}
'@|Set-Content -LiteralPath $child -Encoding UTF8
$parent=Join-Path $WorkDir 'parent.ps1'
@'
param($Helper,$Fixture,$Expected,$Child,$Poison,$Mode)
$ErrorActionPreference='Stop'
$env:PSModulePath=$Poison
$saved=$env:PSModulePath
if($Mode -eq 'old') {
 try {Get-FileHash -LiteralPath $Fixture -Algorithm SHA256 | Out-Null}catch{Write-Output ('Parent: '+$_.Exception.Message)}
 & powershell -NoProfile -File $Child $Fixture $Expected
 exit $LASTEXITCODE
}
. $Helper
$state=Enter-HostPowerShellModules
try {
 if((Get-FileHash -LiteralPath $Fixture -Algorithm SHA256).Hash -cne $Expected){throw 'Parent SHA mismatch'}
 & powershell -NoProfile -File $Child $Fixture $Expected
 if($LASTEXITCODE -ne 0){throw 'Child SHA failed'}
}finally{Exit-HostPowerShellModules $state}
if($env:PSModulePath -cne $saved){throw 'Success did not restore caller module path'}
$state=Enter-HostPowerShellModules
try{throw 'injected'}catch{}finally{Exit-HostPowerShellModules $state}
if($env:PSModulePath -cne $saved){throw 'Exception did not restore caller module path'}
Write-Output 'PASS parent SHA, inherited child SHA, success and exception restoration'
exit 0
'@|Set-Content -LiteralPath $parent -Encoding UTF8
$rows=@()
foreach($mode in @('old','fixed')){
 $log=(& powershell -NoProfile -ExecutionPolicy Bypass -File $parent $helper $fixture $expected $child (Split-Path -Parent $fake) $mode 2>&1|Out-String);$rc=$LASTEXITCODE
 $log|Out-File -LiteralPath (Join-Path $WorkDir ($mode+'.log')) -Encoding utf8
 $rows+=[pscustomobject]@{case=$mode;exit=$rc;correct=($(if($mode -eq 'old'){$rc -ne 0}else{$rc -eq 0}));sha256=$expected}
}
$rows|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $WorkDir 'regression.json') -Encoding UTF8
if(@($rows|Where-Object {-not $_.correct}).Count){throw 'Module scope process controls failed'}
Write-Output 'PASS 2/2 actual polluted parent/child process controls'
