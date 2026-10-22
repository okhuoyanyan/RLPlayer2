# restart-servers.ps1 -- stop THIS build's rtaweb + RLPlayer, rebuild, restart
# Safety: only processes whose ExecutablePath is exactly the build copies; rtaweb is only
# stopped when it actually owns the port we started it on. Never touches other instances.
param(
  [switch]$NoBuild,
  [switch]$NoStart,
  [int]$Port = 8899
)
$buildDir = 'D:\RTA-hqplayer\build'
$rtaweb = Join-Path $buildDir 'rtaweb.exe'
$player = Join-Path $buildDir 'RLPlayer.exe'

function Stop-Mine([string]$exe, [string]$name, [switch]$NeedPort, [int]$port) {
  $procs = @(Get-CimInstance Win32_Process -Filter ("Name='" + $name + "'") | Where-Object { $_.ExecutablePath -eq $exe })
  foreach ($pr in $procs) {
    if ($NeedPort) {
      $owner = (Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue).OwningProcess
      if ($owner -ne $pr.ProcessId) { Write-Output ("skip pid=" + $pr.ProcessId + " (not port " + $port + " owner)"); continue }
    }
    $p = Get-Process -Id $pr.ProcessId -ErrorAction SilentlyContinue
    if (-not $p) { continue }
    if ($p.ProcessName -ne ($name -replace '\.exe$','')) { Write-Output ("ABORT: name mismatch pid=" + $pr.ProcessId); return $false }
    Write-Output ("stopping " + $name + " pid=" + $pr.ProcessId)
    if ($name -eq 'RLPlayer.exe') {
      [void]$p.CloseMainWindow()
    } else {
      Stop-Process -Id $pr.ProcessId -ErrorAction SilentlyContinue
    }
  }
  for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 250
    $left = @(Get-CimInstance Win32_Process -Filter ("Name='" + $name + "'") | Where-Object { $_.ExecutablePath -eq $exe })
    if ($left.Count -eq 0) { return $true }
  }
  $left = @(Get-CimInstance Win32_Process -Filter ("Name='" + $name + "'") | Where-Object { $_.ExecutablePath -eq $exe })
  if ($left.Count -ne 0) { Write-Output ("ABORT: " + $name + " still alive, not force-killing"); return $false }
  return $true
}

if (-not (Stop-Mine $player 'RLPlayer.exe')) { exit 2 }
if (-not (Stop-Mine $rtaweb 'rtaweb.exe' -NeedPort -port $Port)) { exit 2 }
Write-Output "all stopped"

if (-not $NoBuild) {
  Push-Location 'D:\RTA-hqplayer'
  $out = cmake --build build 2>&1
  $errs = $out | Select-String -Pattern 'error|FAILED'
  if ($errs) { Write-Output "BUILD ERRORS:"; $errs | Select-Object -First 15 | ForEach-Object { $_.Line } }
  $out | Select-String -Pattern 'Linking CXX executable rtaweb|Linking CXX executable RLPlayer' | ForEach-Object { $_.Line }
  Pop-Location
}
if ($NoStart) { exit 0 }
Start-Process -FilePath $rtaweb -ArgumentList '--port',"$Port" -WindowStyle Hidden
Start-Sleep -Seconds 3
Start-Process -FilePath $player
Start-Sleep -Seconds 8
Write-Output "started"
