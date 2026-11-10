param([string]$EvidenceDir='')
$ErrorActionPreference='Stop'
if (-not $EvidenceDir) { $EvidenceDir=Join-Path $env:TEMP ('rlplayer-gate-regression-'+[guid]::NewGuid().ToString('N')) }
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$gate=Join-Path $PSScriptRoot 'check_ctest.ps1'
$results=@()
foreach ($name in @('zero','skip','textskip','fail','pass','mixed','mixed-textskip','partial','unverified-only','unverified-partial','longpartial','inherited-env')) {
 $dir=Join-Path $EvidenceDir $name
 New-Item -ItemType Directory -Force -Path $dir | Out-Null
 $lines=@()
 if($name -ne 'zero') {
  $code=if($name -eq 'fail'){'exit 7'}elseif($name -eq 'skip' -or $name -eq 'textskip' -or $name -eq 'mixed'){'Write-Output SKIP_ME; exit 0'}else{'exit 0'}
  if($name -eq 'inherited-env') {$code='if ($env:RTA_NET_TESTS -ne ''0'' -or $env:RTA_NO_DEVICE -ne ''1'' -or $env:PYTHONOPTIMIZE) {exit 3}; Write-Output PASS; exit 0'}
  if($name -eq 'longpartial') {$code="Write-Output ('x'*4096); Write-Output '[PASS] pure'; Write-Output 'UNVERIFIED_DEVICE hardware'; exit 0"}
  if($name -eq 'unverified-only') {$code="Write-Output 'UNVERIFIED_DEVICE no hardware assertions'; exit 0"}
  if($name -eq 'mixed-textskip') {$code="Write-Output 'UNVERIFIED_DEVICE no hardware assertions'; exit 0"}
  if($name -eq 'unverified-partial') {$code="Write-Output '[PASS] pure'; Write-Output 'UNVERIFIED_DEVICE hardware'; exit 0"}
  if($name -eq 'partial') {$code="Write-Output '[PASS] pure'; Write-Output '[SKIP] hardware'; exit 0"}
  $lines+='add_test(fixture powershell -NoProfile -Command "'+$code+'")'
  if($name -eq 'skip' -or $name -eq 'mixed'){$lines+='set_tests_properties(fixture PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP_ME")'}
  if($name -in @('mixed','mixed-textskip')){$lines+='add_test(executed powershell -NoProfile -Command "exit 0")'}
 }
 [IO.File]::WriteAllLines((Join-Path $dir 'CTestTestfile.cmake'),$lines)
 $ErrorActionPreference='Continue'
 $priorNet=$env:RTA_NET_TESTS; $priorDevice=$env:RTA_NO_DEVICE; $priorOpt=$env:PYTHONOPTIMIZE
 try {
  if($name -eq 'inherited-env') {$env:RTA_NET_TESTS='1';$env:RTA_NO_DEVICE='0';$env:PYTHONOPTIMIZE='1'}
  $text=(& powershell -NoProfile -File $gate -BuildDir $dir 2>&1|Out-String); $rc=$LASTEXITCODE
 } finally {$env:RTA_NET_TESTS=$priorNet;$env:RTA_NO_DEVICE=$priorDevice;$env:PYTHONOPTIMIZE=$priorOpt}
 $ErrorActionPreference='Stop'
 $expectedSuccess=$name -in @('pass','partial','unverified-partial','longpartial','inherited-env')
 $ok=(($rc -eq 0) -eq $expectedSuccess)
 if($name -in @('partial','unverified-partial','longpartial')) {$ok=$ok -and ($text -match 'partial-skip-tests=1')}
 $text|Out-File -LiteralPath (Join-Path $dir 'gate.log') -Encoding utf8
 $results+=[pscustomobject]@{case=$name;exit=$rc;expectedSuccess=$expectedSuccess;correct=$ok}
}
$empty=Join-Path $EvidenceDir 'empty-package'; New-Item -ItemType Directory -Force -Path $empty|Out-Null
$ErrorActionPreference='Continue'
$text=(& powershell -NoProfile -File (Join-Path $PSScriptRoot 'check_compat.ps1') -Dir $empty 2>&1|Out-String);$rc=$LASTEXITCODE
$ErrorActionPreference='Stop'
$text|Out-File -LiteralPath (Join-Path $EvidenceDir 'compat-empty.log') -Encoding utf8
$results+=[pscustomobject]@{case='empty-package';exit=$rc;expectedSuccess=$false;correct=($rc -ne 0)}
$pathLog=(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test_package_paths.ps1') -EvidenceDir (Join-Path $EvidenceDir 'package-paths') 2>&1|Out-String);$pathExit=$LASTEXITCODE
$pathLog|Out-File -LiteralPath (Join-Path $EvidenceDir 'package-paths.log') -Encoding utf8
$results+=[pscustomobject]@{case='package-paths';exit=$pathExit;expectedSuccess=$true;correct=($pathExit -eq 0)}
$licenseLog=(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test_package_resources.ps1') -EvidenceDir (Join-Path $EvidenceDir 'package-license') 2>&1|Out-String);$licenseExit=$LASTEXITCODE
$licenseLog|Out-File -LiteralPath (Join-Path $EvidenceDir 'package-license.log') -Encoding utf8
$results+=[pscustomobject]@{case='package-license';exit=$licenseExit;expectedSuccess=$true;correct=($licenseExit -eq 0)}
$redistLog=(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test_ffmpeg_redistribution.ps1') -EvidenceDir (Join-Path $EvidenceDir 'ffmpeg-redistribution') 2>&1|Out-String);$redistExit=$LASTEXITCODE
$redistLog|Out-File -LiteralPath (Join-Path $EvidenceDir 'ffmpeg-redistribution.log') -Encoding utf8
$results+=[pscustomobject]@{case='ffmpeg-redistribution';exit=$redistExit;expectedSuccess=$true;correct=($redistExit -eq 0)}
$argvLog=(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test_native_config_argv.ps1') -EvidenceDir (Join-Path $EvidenceDir 'native-config-argv') 2>&1|Out-String);$argvExit=$LASTEXITCODE
$argvLog|Out-File -LiteralPath (Join-Path $EvidenceDir 'native-config-argv.log') -Encoding utf8
$results+=[pscustomobject]@{case='native-config-argv';exit=$argvExit;expectedSuccess=$true;correct=($argvExit -eq 0)}
$moduleLog=(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test_powershell_module_scope.ps1') -EvidenceDir (Join-Path $EvidenceDir 'powershell-module-scope') 2>&1|Out-String);$moduleExit=$LASTEXITCODE
$moduleLog|Out-File -LiteralPath (Join-Path $EvidenceDir 'powershell-module-scope.log') -Encoding utf8
$results+=[pscustomobject]@{case='powershell-module-scope';exit=$moduleExit;expectedSuccess=$true;correct=($moduleExit -eq 0)}
$results|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $EvidenceDir 'regression.json') -Encoding UTF8
$results|Format-Table|Out-String|Write-Host
if(@($results|Where-Object {-not $_.correct}).Count){exit 1}
exit 0
