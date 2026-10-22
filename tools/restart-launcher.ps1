# restart-launcher.ps1 -- close and restart only THIS build's RLPlayer.exe, then verify
# Safety (iron rule 2): only processes whose ExecutablePath is exactly the build copy.
# Uses CloseMainWindow (WM_CLOSE), never Stop-Process/TerminateProcess. If it will not
# close, abort and leave it to the user.
param([switch]$NoStart)
$exe = 'D:\RTA-hqplayer\build\RLPlayer.exe'
$procs = @(Get-CimInstance Win32_Process -Filter "Name='RLPlayer.exe'" | Where-Object { $_.ExecutablePath -eq $exe })
foreach ($pr in $procs) {
    $p = Get-Process -Id $pr.ProcessId -ErrorAction SilentlyContinue
    if (-not $p) { continue }
    if ($p.ProcessName -ne 'RLPlayer') { Write-Output ("ABORT: pid " + $pr.ProcessId); exit 1 }
    Write-Output ("closing pid=" + $pr.ProcessId)
    [void]$p.CloseMainWindow()
}
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 250
    $left = @(Get-CimInstance Win32_Process -Filter "Name='RLPlayer.exe'" | Where-Object { $_.ExecutablePath -eq $exe })
    if ($left.Count -eq 0) { break }
}
$left = @(Get-CimInstance Win32_Process -Filter "Name='RLPlayer.exe'" | Where-Object { $_.ExecutablePath -eq $exe })
if ($left.Count -ne 0) {
    Write-Output ("ABORT: " + $left.Count + " instances still alive, not force-killing")
    exit 2
}
Write-Output "all closed"
if ($NoStart) { exit 0 }
Start-Process -FilePath $exe
Start-Sleep -Seconds 8
Write-Output "started"
