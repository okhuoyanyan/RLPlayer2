# Strict CTest result gate: discovery, exit status and executed (not skipped) count.
param([Parameter(Mandatory=$true)][string]$BuildDir,[string]$Include='',[string]$Exclude='',[string]$Report='')
$ErrorActionPreference='Stop'
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { throw "Build directory missing: $BuildDir" }
if (-not $Report) { $Report=Join-Path $BuildDir ('ctest-gate-'+[guid]::NewGuid().ToString('N')+'.xml') }
if (Test-Path -LiteralPath $Report) { throw "Report must be fresh: $Report" }
$argsList=@('--test-dir',$BuildDir,'--no-tests=error','--output-on-failure','--test-output-size-passed','10485760','--test-output-size-failed','10485760','--output-junit',$Report)
if ($Include) {$argsList+=@('-R',$Include)}
if ($Exclude) {$argsList+=@('-E',$Exclude)}
$oldDevice=$env:RTA_NO_DEVICE; $oldNet=$env:RTA_NET_TESTS; $oldOpt=$env:PYTHONOPTIMIZE
try {
  $env:RTA_NO_DEVICE='1'; $env:RTA_NET_TESTS='0'; $env:PYTHONOPTIMIZE=''
  $ErrorActionPreference='Continue'
  & ctest @argsList
  $rc=$LASTEXITCODE
  $ErrorActionPreference='Stop'
  if ($rc -ne 0) {throw "CTest failed: exit=$rc"}
  if (-not (Test-Path -LiteralPath $Report -PathType Leaf)) {throw 'CTest did not create a result report'}
  [xml]$xml=Get-Content -LiteralPath $Report -Raw
  $cases=@($xml.SelectNodes('//testcase'))
  $skipped=@($cases | Where-Object {
    $out=$_.SelectSingleNode('system-out')
    $_.SelectSingleNode('skipped') -or ($out -and $out.InnerText -match '(?i)\bSKIP\b|SKIP_ME|UNVERIFIED_(?:REAL|DEVICE)|UNCOVERED' -and $out.InnerText -notmatch '(?i)\bPASS\b')
  }).Count
  $partial=@($cases | Where-Object {
    $out=$_.SelectSingleNode('system-out')
    $out -and $out.InnerText -match '(?i)\bSKIP\b|SKIP_ME|UNVERIFIED_(?:REAL|DEVICE)|UNCOVERED' -and $out.InnerText -match '(?i)\bPASS\b'
  })
  if($partial.Count -gt 0) {
    Write-Host ('UNVERIFIED branches (explicit SKIP alongside passing assertions): '+(($partial | ForEach-Object {$_.name}) -join ', '))
  }
  $failed=@($cases | Where-Object {$_.SelectSingleNode('failure') -or $_.SelectSingleNode('error')}).Count
  $executed=$cases.Count-$skipped
  if ($executed -le 0 -or $failed -gt 0 -or $skipped -gt 0) {throw "Invalid coverage: total=$($cases.Count), executed=$executed, skipped=$skipped, failed=$failed"}
  Write-Host "CTest gate PASS: total=$($cases.Count), executed=$executed, skipped=$skipped (skips are not coverage), failed=$failed; partial-skip-tests=$($partial.Count)"
} finally {$env:RTA_NO_DEVICE=$oldDevice;$env:RTA_NET_TESTS=$oldNet;$env:PYTHONOPTIMIZE=$oldOpt}
