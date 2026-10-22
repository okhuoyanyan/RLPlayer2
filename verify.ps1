# verify.ps1 - RLPlayer quick self-check (no audio device is opened)
# NOTE: keep this file UTF-8 WITH BOM. PowerShell 5.1 reads a BOM-less UTF-8 file as GBK
#       and fails to parse the Chinese comments ("Missing type name after '['").
$ErrorActionPreference='Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:Path = 'C:\Program Files\AMD\ROCm\7.2\bin;' + $env:Path
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$B = Join-Path $Root 'build'
$rows = @()
function Step($n, $ok, $d) { $script:rows += [pscustomobject]@{ Item=$n; Result=$(if($ok){'PASS'}else{'FAIL'}); Detail=$d } }
Write-Host '=== RLPlayer self-check (silent: no audio device opened) ==='

# 1 build
$null = cmake -G Ninja -S $Root -B $B -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe' 2>&1
$bo = (cmake --build $B 2>&1 | Out-String)
# 只认真正的编译错误（源码注释里出现 ERROR 字样不该算失败）
$errs = (($bo -split ([char]10)) | Select-String -Pattern 'error:|error generated|ninja: build stopped' | Measure-Object).Count
Step 'CMake build' ($errs -eq 0) ($errs.ToString() + ' errors')

# 2 test suite (judge by exit code, not by parsing text)
Push-Location $B
$ct = (ctest 2>&1 | Out-String)
$ctExit = $LASTEXITCODE
Pop-Location
$sum = ((($ct -split ([char]10)) | Select-String 'tests passed' | Select-Object -First 1) -join ' ').Trim()
if (-not $sum) { $sum = 'exit=' + $ctExit }
$ntest = 0
if ($sum -match 'out of (\d+)') { $ntest = [int]$Matches[1] }
Step ('Test suite (' + $ntest + ')') ($ctExit -eq 0) $sum

# 3 GPU backend（按本机可用工具链编对应后端：AMD=HIP，NVIDIA=CUDA fatbin）
#   两个后端导出同一组符号，管线按 gpu_pref.h 的候选顺序自动挑；所以判据是
#   "至少一条通路编译+加载成功"，而不是"必须存在 rtagpu.dll"。
#   注意：CUDA 组件可以只装编译器那几包（约 140 MB），见 docs/GPU-CUDA后端纪要.md §4。
$dllHip  = Join-Path $B 'rtagpu.dll'
$dllCuda = Join-Path $B 'rtagpu_cuda.dll'
$hipcc = 'C:\Program Files\AMD\ROCm\7.2\bin\hipcc.bat'
$nvccPath = $null
if ($env:CUDA_PATH -and (Test-Path (Join-Path $env:CUDA_PATH 'bin\nvcc.exe'))) {
  $nvccPath = Join-Path $env:CUDA_PATH 'bin\nvcc.exe'
} elseif (Test-Path 'C:\Users\garaxc\Documents\player\toolchain\cuda-12.9\bin\nvcc.exe') {
  $nvccPath = 'C:\Users\garaxc\Documents\player\toolchain\cuda-12.9\bin\nvcc.exe'
} else {
  $n = Get-Command nvcc.exe -ErrorAction SilentlyContinue
  if ($n) { $nvccPath = $n.Source }
}
$gpuOk = $false
if (Test-Path $hipcc) {
  $gout = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tools\build_gpu.ps1') -Out $dllHip 2>&1 | Out-String)
  $okHip = (Test-Path $dllHip) -and ($LASTEXITCODE -eq 0)
  if ($okHip) { $gpuOk = $true }
  Step 'rtagpu.dll (HIP multi-arch)' $okHip ($gout.Trim())
}
if ($nvccPath) {
  $cudaRoot = Split-Path -Parent (Split-Path -Parent $nvccPath)
  $cout = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tools\build_gpu_cuda.ps1') -Out $dllCuda -CudaRoot $cudaRoot 2>&1 | Out-String)
  $okCuda = (Test-Path $dllCuda) -and ($LASTEXITCODE -eq 0)
  if ($okCuda) { $gpuOk = $true }
  Step 'rtagpu_cuda.dll (CUDA fatbin)' $okCuda ($cout.Trim())
}
if (-not $gpuOk) { Step 'GPU backend' $false 'no usable GPU toolchain (hipcc / nvcc)' }

# 4 GPU convolution correctness
# 用 test_gpu_bridge（由 CMake 构建、也进了 ctest）：它走的是真正的 GpuBridge 动态加载路径。
# 旧的 gpu_conv.exe 是手工编的独立 demo，不跟着源码更新，曾经掩盖过
# "gpu_bridge.h 没取符号、GPU 从未真正启用"这个 bug。
$gc = Join-Path $B 'test_gpu_bridge.exe'
if (Test-Path $gc) {
  $g = (& $gc 2>&1 | Out-String)
  $okG = $g -match '\[PASS\]'
  $perf = ((($g -split ([char]10)) | Select-String 'PASS|SKIP|相对' ) -join ' ').Trim()
  Step 'GPU conv == CPU' $okG $perf
} else { Step 'GPU conv == CPU' $false 'test_gpu_bridge.exe missing' }

# 5 UPnP parser self-test
$up = Join-Path $B 'upnp_scan.exe'
if (Test-Path $up) {
  $u = (& $up --selftest 2>&1 | Out-String)
  Step 'UPnP parser self-test' ($u -match 'selftest')  '' 
} else { Step 'UPnP parser self-test' $false 'upnp_scan.exe missing' }

# 6 network end-to-end bit-exact
$play = Join-Path $B 'rtaplay.exe'; $rx = Join-Path $B 'rtanet_rx.exe'; $cmp = Join-Path $B 'wavcmp.exe'
if ((Test-Path $play) -and (Test-Path $rx) -and (Test-Path $cmp)) {
  $td = Join-Path $B 'testdata'; New-Item -ItemType Directory -Force -Path $td | Out-Null
  $src = Join-Path $td 'vfy_src.wav'; $dst = Join-Path $td 'vfy_rx.wav'
  $null = & $play --gen $src 3 44100 24 2>&1
  $job = Start-Job -ScriptBlock { param($e, $o) & $e --sink file --out $o --ctrl-port 45901 --seconds 9 2>&1 } -ArgumentList $rx, $dst
  Start-Sleep -Seconds 2
  $null = & $play $src --net '127.0.0.1:45901' 2>&1
  Wait-Job $job -Timeout 25 | Out-Null
  Stop-Job $job -ErrorAction SilentlyContinue; Remove-Job $job -Force -ErrorAction SilentlyContinue
  $c = (& $cmp $src $dst 2>&1 | Out-String)
  $cmpExit = $LASTEXITCODE
  $line = ((($c -split ([char]10)) | Select-String '[0-9]') -join ' ').Trim()
  Step 'Network e2e bit-exact' ($cmpExit -eq 0) ($line + ' (wavcmp exit=' + $cmpExit + ')')
} else { Step 'Network e2e bit-exact' $false 'exe missing' }

# 7 GPU resample kernel (vanilla-kernel sanity + CPU/GPU equality)
$grt = Join-Path $B 'gpu_resample_test.exe'
if (Test-Path $grt) {
  $g = (& $grt 2>&1 | Out-String)
  $okR = $g -match 'PASS'
  $l = ((($g -split ([char]10)) | Select-String 'PASS|FAIL' | Select-Object -First 1) -join ' ').Trim()
  Step 'GPU resample kernel' $okR $l
} else { Step 'GPU resample kernel' $false 'gpu_resample_test.exe missing' }

# 8 UI self-check (headless Edge + CDP; never plays audio)
$nodeCmd = Get-Command node -ErrorAction SilentlyContinue
if ($nodeCmd -and (Test-Path (Join-Path $Root 'tools\uicheck.js'))) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = Join-Path $B 'rtaweb.exe'; $psi.Arguments = '--port 8907'
  $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.CreateNoWindow = $true
  $wp = New-Object System.Diagnostics.Process; $wp.StartInfo = $psi; $null = $wp.Start()
  $up = $false
  for ($i = 0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 200
    try { $t = New-Object Net.Sockets.TcpClient; $t.Connect('127.0.0.1', 8907); $t.Close(); $up = $true; break } catch {} }
  if ($up) {
    $uo = (& node (Join-Path $Root 'tools\uicheck.js') 'http://127.0.0.1:8907/' (Join-Path $env:TEMP 'rta_uicheck_vfy') 2>&1 | Out-String)
    $uiExit = $LASTEXITCODE
    $ul = ((($uo -split ([char]10)) | Select-String '全部通过|项失败' | Select-Object -First 1) -join ' ').Trim()
    Step 'Web UI self-check' ($uiExit -eq 0) $ul
  } else { Step 'Web UI self-check' $false 'rtaweb did not start' }
  Stop-Process -Id $wp.Id -Force -ErrorAction SilentlyContinue
} else { Step 'Web UI self-check' $false 'node or uicheck.js missing' }

# 9 DSD domain: material generation -> DirectSDM bit-exact -> integrator THD+N
$mk = Join-Path $B 'make_dsd.exe'; $dc = Join-Path $B 'dsdconv.exe'
if ((Test-Path $mk) -and (Test-Path $dc)) {
  $td = Join-Path $B 'testdata'; New-Item -ItemType Directory -Force -Path $td | Out-Null
  $dsf = Join-Path $td 'vfy_dsd64.dsf'; $dop = Join-Path $td 'vfy_dsd64_dop.wav'
  $mo = (& $mk --out $dsf --seconds 1 --verify 2>&1 | Out-String)
  $do = (& $dc --in $dsf --direct --out $dop 2>&1 | Out-String)
  $dcExit = $LASTEXITCODE
  $bitOk = $do -match 'DIRECT_SDM_BITEXACT=1'
  $thd = 0.0
  if ($mo -match 'THD\+N[^=]*=\s*([0-9.]+) dB') { $thd = [double]$Matches[1] }
  Step 'DSD path (DirectSDM + THD+N)' (($dcExit -eq 0) -and $bitOk -and ($thd -ge 80.0)) ('DirectSDM bit-exact=' + $bitOk + ', THD+N ' + $thd + ' dB')
} else { Step 'DSD path (DirectSDM + THD+N)' $false 'make_dsd/dsdconv missing' }

# 10 DirectSDM end-to-end: 播放器 -> RTANet -> 文件，收到的 DoP 载波必须与源逐位一致
# 为什么单独做：前面第 9 项只验「工具链」；这一项走的是真正的播放线程（/api/play -> OutSink）。
if ((Test-Path $mk) -and (Test-Path $dc) -and (Test-Path (Join-Path $B 'rtanet_rx.exe')) -and (Test-Path (Join-Path $B 'wavcmp.exe'))) {
  $td = Join-Path $B 'testdata'
  $dsf2 = Join-Path $td 'vfy_dm.dsf'; $refW = Join-Path $td 'vfy_dm_ref.wav'; $recvW = Join-Path $td 'vfy_dm_recv.wav'
  Remove-Item $recvW -ErrorAction SilentlyContinue
  $null = & $mk --out $dsf2 --seconds 1 2>&1
  $null = & $dc --in $dsf2 --direct --out $refW 2>&1
  $rxJob = Start-Process -FilePath (Join-Path $B 'rtanet_rx.exe') -ArgumentList @('--sink','file','--out',$recvW,'--ctrl-port','45902','--seconds','8') -PassThru -WindowStyle Hidden
  $webP = Start-Process -FilePath (Join-Path $B 'rtaweb.exe') -ArgumentList @('--port','8908') -PassThru -WindowStyle Hidden
  $ready = $false
  for ($i = 0; $i -lt 50; $i++) { Start-Sleep -Milliseconds 200
    try { $t = New-Object Net.Sockets.TcpClient; $t.Connect('127.0.0.1', 8908); $t.Close(); $ready = $true; break } catch {} }
  if ($ready) {
    $body = 'file=' + [uri]::EscapeDataString($dsf2) + '&backend=naa&naahost=127.0.0.1%3A45902&dsd=64&volume=0&directsdm=1'
    try { $null = Invoke-WebRequest -Uri 'http://127.0.0.1:8908/api/play' -Method Post -Body $body -ContentType 'application/x-www-form-urlencoded' -UseBasicParsing -TimeoutSec 10 } catch {}
    try { Wait-Process -Id $rxJob.Id -Timeout 20 -ErrorAction SilentlyContinue } catch {}
  }
  # 只停自己启动的两个进程，且校验映像名（铁律 2）
  foreach ($pp in @($rxJob, $webP)) {
    if ($pp) { $x = Get-Process -Id $pp.Id -ErrorAction SilentlyContinue
      if ($x -and ($x.ProcessName -eq 'rtaweb' -or $x.ProcessName -eq 'rtanet_rx')) { Stop-Process -Id $pp.Id -ErrorAction SilentlyContinue } }
  }
  $cmpOut = ''
  if (Test-Path $recvW) { $cmpOut = (& (Join-Path $B 'wavcmp.exe') $refW $recvW --prefix 2>&1 | Out-String); $cmpExit = $LASTEXITCODE } else { $cmpExit = 1 }
  Step 'DirectSDM e2e (NAA -> file)' ($ready -and ($cmpExit -eq 0)) ((($cmpOut -split ([char]10)) | Select-String '帧|前缀|不一致') -join ' ').Trim()
} else { Step 'DirectSDM e2e (NAA -> file)' $false 'exe missing' }


# 11 distribution compatibility: static CRT, subsystem <= 6.1, no AVX-512
$cc = Join-Path $Root 'tools\check_compat.ps1'
if (Test-Path $cc) {
  $co = (& powershell -NoProfile -ExecutionPolicy Bypass -File $cc -Dir $B 2>&1 | Out-String)
  $ccExit = $LASTEXITCODE
  $cl = ((($co -split ([char]10)) | Select-String 'FAIL count' | Select-Object -First 1) -join ' ').Trim()
  Step 'Distribution compatibility' ($ccExit -eq 0) $cl
} else { Step 'Distribution compatibility' $false 'check_compat.ps1 missing' }

Write-Host ''
$rows | Format-Table -AutoSize | Out-String | Write-Host
$f = ($rows | Where-Object { $_.Result -eq 'FAIL' } | Measure-Object).Count
Write-Host ('FAIL count: ' + $f)
Write-Host 'NOTE: no audio device was opened during this check.'
exit $f