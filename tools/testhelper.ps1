
$ErrorActionPreference="Continue"
[Console]::OutputEncoding=[System.Text.Encoding]::UTF8
function Stop-Test {
  $f = "$env:TEMP\rtaweb_test_pid.txt"
  if(Test-Path $f){ $id=((Get-Content $f) -replace "procId=","").Trim(); if($id){ Stop-Process -Id $id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 400 } }
}
function Start-Test($port){
  $B="D:\RTA-hqplayer\build"
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName="$B\rtaweb.exe"; $psi.Arguments="--port $port"
  $psi.UseShellExecute=$false; $psi.RedirectStandardOutput=$true; $psi.RedirectStandardError=$true; $psi.CreateNoWindow=$true
  $p = New-Object System.Diagnostics.Process; $p.StartInfo=$psi; $null=$p.Start()
  for($i=0;$i -lt 60;$i++){ Start-Sleep -Milliseconds 200; try { $t=New-Object Net.Sockets.TcpClient; $t.Connect("127.0.0.1",$port); $t.Close(); break } catch {} }
  "procId=$($p.Id)" | Out-File "$env:TEMP\rtaweb_test_pid.txt" -Encoding ascii
  return $p
}
function G($u){ try { (Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 15).Content } catch { "ERR "+$_.Exception.Message } }
function P($u,$b){ try { (Invoke-WebRequest -Uri $u -Method Post -Body $b -UseBasicParsing -TimeoutSec 25).Content } catch { "ERR "+$_.Exception.Message } }
function Build {
  $o = cmake --build "D:\RTA-hqplayer\build" 2>&1 | Out-String
  # 注意：clang 的错误行不一定包含 "error:"（例如 "1 error generated."），
  # 只看 "error:" 会漏判 —— 本项目真的因此把编译失败当成功，拿旧二进制测了半天。
  $bad = ($o -split [char]10) | Select-String -Pattern "error:|error generated|ninja: build stopped|Linking CXX executable.*failed|permission denied"
  if($bad){ $bad | ForEach-Object { $_.ToString().Trim() } | Select-Object -First 10; return $false }
  return $true
}
