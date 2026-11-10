param([string]$BuildDir="")
# verify-stream.ps1 - chunk-invariance gate for the streaming pipeline
# Silent: uses --out (offline files) only, never opens an audio device.
$ErrorActionPreference='Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $Root 'tools\package_paths.ps1')
if (-not $BuildDir) { $BuildDir = Join-Path $Root 'build' }
elseif (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $Root $BuildDir }
try { $B = Resolve-ReleaseBuildDirectory -SourceRoot $Root -BuildDir $BuildDir }
catch { Write-Error $_; exit 2 }
$play = Join-Path $B 'rtaplay.exe'; $cmp = Join-Path $B 'wavcmp.exe'
if (-not (Test-Path $play) -or -not (Test-Path $cmp)) { 'missing exes'; exit 2 }
$td = Join-Path $B 'testdata'; New-Item -ItemType Directory -Force -Path $td | Out-Null
$s = Join-Path $td 'vs_src.wav'
$null = & $play --gen $s 4 44100 24 2>&1
$w = Join-Path $td 'vs_whole.wav'
$null = & $play $s --out $w --rate 88200 --bits 24 --taps 16384 2>&1
$fails = 0
foreach ($ck in 8192,1024,777,333) {
  $c = Join-Path $td ('vs_c' + $ck + '.wav')
  $null = & $play $s --out $c --rate 88200 --bits 24 --taps 16384 --chunk $ck 2>&1
  $null = & $cmp $w $c 2>&1
  $ce = $LASTEXITCODE
  if ($ce -eq 0) { Write-Host ('  [PASS] chunk ' + $ck + ' -> bit-identical') -ForegroundColor Green }
  else { Write-Host ('  [FAIL] chunk ' + $ck + ' -> wavcmp exit ' + $ce) -ForegroundColor Red; $fails++ }
}
Write-Host ''
Write-Host ('chunk-invariance: ' + $(if($fails -eq 0){'ALL PASS'}else{$fails.ToString() + ' FAILED'}))
Write-Host 'NOTE: offline conversion only; no audio device was opened.'
exit $fails