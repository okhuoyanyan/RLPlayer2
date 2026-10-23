# build_gpu_cuda.ps1 - build the NVIDIA CUDA backend (rtagpu_cuda.dll).
#
# 与 tools/build_gpu.ps1（AMD HIP 版）对称：一份 DLL 里塞多个 arch 的 code object，
# 运行时由驱动挑选匹配的那份；实际卡不在列表里时 rta_gpu_available() 返回 0，
# 播放器自动退回 CPU（已验证行为）。
#
# 与 HIP 版的三个关键差异（都是 CUDA 侧特有的坑，别照抄 HIP 的写法）：
#   1. Windows 下 nvcc 需要 cl.exe：非 Developer Prompt 里跑必须显式 -ccbin。
#      找不到 cl 时 nvcc 报 "Cannot find compiler 'cl.exe' in PATH"，与源码无关。
#   2. CUDA 不会像 HIP 那样"编错 arch 就静默不执行"——fatbin 里带 PTX 时驱动会 JIT，
#      于是**看起来能跑**。所以既要用 cuobjdump 校验 code object 真的进了二进制，
#      也要在运行时真跑一次平凡 kernel 并读回（见 gpu_conv_cuda.cu）。
#   3. 分发版要求静态 CRT（与主程序 CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded 一致）
#      ⟹ -Xcompiler=/MT；且绝不带 -arch=native 之类的本机优化（CPU 基线是 x86-64-v2）。
#
# Usage:
#   powershell -File tools\build_gpu_cuda.ps1                     # 全 arch（分发用）
#   powershell -File tools\build_gpu_cuda.ps1 -Fast               # 只编本机 arch（开发用）
#   powershell -File tools\build_gpu_cuda.ps1 -Out build\rtagpu_cuda.dll -CudaRoot "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4"
param(
  [string]$Out = "",
  [string]$CudaRoot = "",
  [switch]$Fast
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if ($Out -eq "") { $Out = Join-Path $root 'build\rtagpu_cuda.dll' }
$buildDir = Split-Path -Parent $Out
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

# ---------------- 1. 定位 CUDA Toolkit ----------------
function Find-CudaRoot($explicit) {
  $cands = @()
  if ($explicit) { $cands += $explicit }
  if ($env:CUDA_PATH) { $cands += $env:CUDA_PATH }
  $kit = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA'
  if (Test-Path $kit) {
    $cands += (Get-ChildItem $kit -Directory | Sort-Object Name -Descending | ForEach-Object { $_.FullName })
  }
  foreach ($c in $cands) {
    if ($c -and (Test-Path (Join-Path $c 'bin\nvcc.exe'))) { return $c }
  }
  return $null
}

$cuda = Find-CudaRoot $CudaRoot
if (-not $cuda) {
  Write-Host "nvcc not found. Install the CUDA Toolkit (winget install Nvidia.CUDA) or pass -CudaRoot."
  exit 2
}
$nvcc = Join-Path $cuda 'bin\nvcc.exe'
$cuobjdump = Join-Path $cuda 'bin\cuobjdump.exe'

# ---------------- 2. 定位 MSVC（nvcc 的 host 编译器）----------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$ccbin = $null
if (Test-Path $vswhere) {
  $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($vs) {
    $cl = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending |
          ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe' } |
          Where-Object { Test-Path $_ } |
          Select-Object -First 1
    if ($cl) { $ccbin = Split-Path -Parent $cl }
  }
}
if (-not $ccbin) {
  $clOnPath = (Get-Command cl.exe -ErrorAction SilentlyContinue)
  if ($clOnPath) { $ccbin = Split-Path -Parent $clOnPath.Source }
}
if (-not $ccbin) {
  Write-Host "cl.exe not found (need MSVC x64 toolchain for the host half of the .cu files)."
  exit 2
}

# ---------------- 3. 决定要编哪些 arch ----------------
# 目标：尽可能广。用户 2026-10-22 明确要求「用 CUDA 12 尽可能做支持」——
# CUDA 12.x 仍支持 Maxwell/Pascal/Volta（CUDA 13 已移除），所以选 12.9 做后端，
# 一份 fatbin 覆盖 GTX 9 系到 RTX 50 系。
# 列表本身不硬编码"支持/不支持"：下面用 nvcc --list-gpu-arch 实测过滤，
# 编不了的老 arch 自然被剔除，编得了一个不漏。
$allSms = @(
  50, 52, 53,            # Maxwell  (GTX 9 系；sm_50 是 CUDA 12 能编的最老一档)
  60, 61, 62,            # Pascal   (GTX 10 系 / P40 等)
  70, 72,                # Volta    (V100)
  75,                    # Turing   (RTX 20 系 / T4)
  80, 86, 87,            # Ampere   (A100 / RTX 30 系 / Jetson Orin)
  89,                    # Ada      (RTX 40 系 / L40 —— 本机 4080 在这一档)
  90,                    # Hopper   (H100)
  100, 101, 103, 110, 120, 121   # Blackwell (B200 / GB200 / RTX 50 系)
)
$supported = @()
$listOut = & $nvcc --list-gpu-arch 2>&1 | Out-String
foreach ($sm in $allSms) {
  # 注意：nvcc --list-gpu-arch 输出的是 compute_XX（不是 sm_XX）。
  # 两种写法都接受，但必须带边界，否则 sm_50 会撞上 sm_500 这类更长的编号。
  if ($listOut -match ("(compute|sm)_" + $sm + "(\D|$)")) { $supported += $sm }
}
if ($supported.Count -eq 0) {
  Write-Host "nvcc reported no usable GPU arch (--list-gpu-arch output:)"
  Write-Host $listOut
  exit 1
}
Write-Host ("nvcc supports " + $supported.Count + " of " + $allSms.Count + " requested archs: sm_" + ($supported -join ',sm_'))

if ($Fast) {
  # 开发用：只编本机卡。判定用 nvidia-smi 的 compute capability 不直接可得，
  # 所以按驱动能报出来的型号映射；拿不到就退化成"只编最新一个"。
  $name = (& nvidia-smi --query-gpu=name --format=csv,noheader 2>$null | Select-Object -First 1)
  $map = @{
    'RTX 50' = 120; 'RTX 40' = 89; 'RTX 30' = 86; 'RTX 20' = 75;
    'A100' = 80; 'H100' = 90; 'L40' = 89; 'A10' = 86; 'T4' = 75;
  }
  $pick = $null
  foreach ($k in $map.Keys) { if ($name -and $name.Contains($k)) { $pick = $map[$k]; break } }
  if (-not $pick -or ($supported -notcontains $pick)) { $pick = ($supported | Measure-Object -Maximum).Maximum }
  $archs = @($pick)
  Write-Host ("-Fast: GPU='{0}' -> sm_{1}" -f $name, $pick)
} else {
  $archs = $supported
}

$srcs = @(
  (Join-Path $root 'src\gpu\gpu_conv_cuda.cu'),
  (Join-Path $root 'src\gpu\gpu_resample_cuda.cu')
)

$nvArgs = @('-shared', '-cudart', 'static', '-O3', '-std=c++17', '-ccbin', $ccbin)
# 静态 CRT + 不做本机专属优化（分发基线）
$nvArgs += @('-Xcompiler=/MT', '-Xcompiler=/O2', '-Xcompiler=/EHsc', '-Xcompiler=/utf-8')
foreach ($sm in $archs) {
  $nvArgs += "-gencode=arch=compute_$sm,code=sm_$sm"
}
# 最新那个 arch 额外带 PTX：以后出的卡（比本脚本更晚）能由驱动 JIT，不至于直接不可用
$newest = ($archs | Measure-Object -Maximum).Maximum
$nvArgs += "-gencode=arch=compute_$newest,code=compute_$newest"
$nvArgs += @('-I', (Join-Path $root 'src'))
$nvArgs += $srcs
$nvArgs += @('-o', $Out)

$log = Join-Path $buildDir 'gpu_cuda_build.log'
$ver = (& $nvcc --version 2>&1 | Out-String)
$drv = (& nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>&1 | Out-String)
Write-Host ("building rtagpu_cuda.dll for " + ($archs -join ',') + " (arch count " + $archs.Count + ") ...")
"=== nvcc ===`n$ver`n=== gpu ===`n$drv`n=== ccbin ===`n$ccbin`n=== args ===`n$($nvArgs -join ' ')" | Set-Content -Path $log -Encoding UTF8

# nvcc 把 warning 写 stderr；ErrorActionPreference=Stop 会把它当终止错误。按退出码判定。
$eap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $nvcc @nvArgs *>> $log
$rc = $LASTEXITCODE
$ErrorActionPreference = $eap
if ($rc -ne 0) {
  Write-Host ("CUDA build failed (exit " + $rc + "); tail of " + $log)
  Get-Content $log -Tail 40 | ForEach-Object { Write-Host $_ }
  exit 1
}
if (-not (Test-Path $Out)) { Write-Host "CUDA build produced no output"; exit 1 }

# ---------------- 4. 校验每个请求的 arch 真的进了二进制 ----------------
# 判据取 cuobjdump 的 ELF 清单（不是搜文件名/字符串）：CUDA 的 fatbin 会把
# cubin 名字写成 rtagpu_cuda.<n>.sm_89.cubin，但仅凭文件名匹配容易自欺——
# -lelf 列的是真的被嵌入的 code object。
$dumpOut = & $cuobjdump -lelf $Out 2>&1 | Out-String
$missing = @()
foreach ($sm in $archs) {
  if ($dumpOut -notmatch ("sm_" + $sm + "(\D|$)")) { $missing += ("sm_" + $sm) }
}
if ($missing.Count -gt 0) {
  Write-Host ("CUDA build missing code objects: " + ($missing -join ', '))
  Write-Host "cuobjdump -lelf output:"
  Write-Host $dumpOut
  exit 1
}

# 导出符号校验：管线靠这两个名字动态加载，改名字不会报错、只会静默退回 CPU
$symDump = & $cuobjdump -symbols $Out 2>&1 | Out-String
$needSyms = @('rta_gpu_available', 'rta_gpu_convolve', 'rta_gpu_resample', 'rta_gpu_resample_invalidate')
$missingSym = @($needSyms | Where-Object { $symDump -notmatch [regex]::Escape($_) })
# cuobjdump -symbols 只列 device 符号；host 导出符号用 dumpbin 复核（MSVC 一定在）
if ($ccbin) {
  # dumpbin.exe 与 cl.exe 同目录（<VC>\Tools\MSVC\<ver>\bin\Hostx64\x64\）
  $dumpbin = Join-Path $ccbin 'dumpbin.exe'
  if (Test-Path $dumpbin) {
    $exp = & $dumpbin /exports $Out 2>&1 | Out-String
    $missingSym = @($needSyms | Where-Object { $exp -notmatch [regex]::Escape($_) })
  } else {
    Write-Host "warning: dumpbin.exe not found next to cl.exe; export symbols not verified"
  }
}
if ($missingSym.Count -gt 0) {
  Write-Host ("CUDA build missing exported symbols: " + ($missingSym -join ', '))
  exit 1
}

Write-Host ("rtagpu_cuda.dll OK (" + $archs.Count + " archs, +PTX compute_" + $newest + "): " +
            [math]::Round((Get-Item $Out).Length / 1KB) + " KB -> " + $Out)
exit 0
