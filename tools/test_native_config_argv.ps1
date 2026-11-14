param([string]$WorkDir='')
$ErrorActionPreference='Stop'
$cmakeExe=$env:RTA_TEST_CMAKE_EXE
if(-not $cmakeExe){$cmakeExe=(Get-Command cmake.exe -ErrorAction Stop).Source}
if(-not [IO.Path]::IsPathRooted($cmakeExe) -or -not (Test-Path -LiteralPath $cmakeExe -PathType Leaf)){throw 'A real absolute CMake executable is required'}
if(-not $WorkDir){$WorkDir=Join-Path $env:TEMP ('rlplayer-native-argv-'+[guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Force -Path $WorkDir|Out-Null
# The actual CMake native receiver records argv without configuring/building.
$probe=Join-Path $WorkDir 'capture.cmake'
@'
set(text "")
math(EXPR last "${CMAKE_ARGC}-1")
foreach(i RANGE 0 ${last})
  string(APPEND text "${CMAKE_ARGV${i}}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_LIST_DIR}/argv.txt" "${text}")
'@|Set-Content -LiteralPath $probe -Encoding UTF8
$results=@()
foreach($label in @('plain','unicode-space')){
 $CertificatePythonPath=if($label -eq 'plain'){'D:/audit-deps'}else{'D:/验收 依赖/audit deps'}
 & $cmakeExe -P $probe -DRTA_CERTIFICATE_PYTHON_PATH=$CertificatePythonPath
 if($LASTEXITCODE -ne 0){throw 'Native negative receiver failed'}
 $old=@(Get-Content -LiteralPath (Join-Path $WorkDir 'argv.txt') -Encoding UTF8)
 $wanted='-DRTA_CERTIFICATE_PYTHON_PATH='+$CertificatePythonPath
 $results+=[pscustomobject]@{case=$label+'-old-rejected';correct=(@($old|Where-Object {$_ -ceq $wanted}).Count -eq 0);received=($old -join "`n");expected=$wanted}
 $configureArgs=@(('-DRTA_CERTIFICATE_PYTHON_PATH='+$CertificatePythonPath),'-P',$probe)
 & $cmakeExe @configureArgs
 if($LASTEXITCODE -ne 0){throw 'Native positive receiver failed'}
 $fixed=@(Get-Content -LiteralPath (Join-Path $WorkDir 'argv.txt') -Encoding UTF8)
 $results+=[pscustomobject]@{case=$label+'-array-exact';correct=(@($fixed|Where-Object {$_ -ceq $wanted}).Count -eq 1);received=($fixed -join "`n");expected=$wanted}
}
$results|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $WorkDir 'regression.json') -Encoding UTF8
$results|Format-Table case,correct|Out-String|Write-Host
if(@($results|Where-Object {-not $_.correct}).Count){exit 1}
exit 0
