# verify_ffmpeg_lite.ps1 - 精简版 ffmpeg/ffprobe 的出包前置守卫（P1）
#
# 它回答三个问题，任何一条不过就 exit 1（make_dist.ps1 据此拒绝出包）：
#   1. 精简版自身能不能跑（含"PATH 只剩 System32"的裸环境——证明不依赖任何包外 DLL）；
#   2. 白名单里的音频解码器一个不缺（tools/ffmpeg_lite_required.txt，与 ctest 同源）；
#   3. 产品解码路径真的能用：跑 test_ffmpeg_matrix.exe（read_any 逐格式断言）。
#
# 用法：
#   powershell -File tools\verify_ffmpeg_lite.ps1                 # 查 build\ffmpeg-lite
#   powershell -File tools\verify_ffmpeg_lite.ps1 -Dir <某目录>    # 查别处（红/绿演示用）
#   ... -SkipMatrix    # 只查二进制与解码器清单（矩阵要另外编，bootstrap 时用）
param(
    [string]$Dir = "",
    [switch]$SkipMatrix
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if (-not $Dir) { $Dir = Join-Path $root 'build\ffmpeg-lite' }

function Fail($msg) { Write-Host "FAILED: $msg"; exit 1 }

$ff = Join-Path $Dir 'ffmpeg.exe'
$fp = Join-Path $Dir 'ffprobe.exe'
Write-Host "=== 守卫：精简版 ffmpeg ($Dir) ==="
if (-not (Test-Path $ff)) { Fail "缺少 $ff（先跑 tools\build_ffmpeg_lite.ps1）" }
if (-not (Test-Path $fp)) { Fail "缺少 $fp（先跑 tools\build_ffmpeg_lite.ps1）" }
Write-Host ("ffmpeg.exe  " + [math]::Round((Get-Item $ff).Length / 1MB, 2) + " MB")
Write-Host ("ffprobe.exe " + [math]::Round((Get-Item $fp).Length / 1MB, 2) + " MB")

# ---- 1. 裸环境可执行性：PATH 只剩 System32（干净用户机看到的样子）----
Write-Host "--- 1/3 裸环境（PATH=System32）可执行性 ---"
$oldPath = $env:PATH
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
try {
    $vOut = @(& $ff -hide_banner -version 2>&1)
    $vRc = $LASTEXITCODE
    $pOut = @(& $fp -hide_banner -version 2>&1)
    $pRc = $LASTEXITCODE
} finally {
    $env:PATH = $oldPath
}
if ($vRc -ne 0) { Fail ("裸环境下 ffmpeg -version 失败（退出码 $vRc）—— 多半缺 DLL：" + ($vOut -join ' ')) }
if ($pRc -ne 0) { Fail ("裸环境下 ffprobe -version 失败（退出码 $pRc）：" + ($pOut -join ' ')) }
Write-Host ("OK  " + $vOut[0])
Write-Host ("OK  " + $pOut[0])

# ---- 2. 解码器白名单 ----
Write-Host "--- 2/3 必需解码器白名单 ---"
$decoders = @()
foreach ($line in @(& $ff -hide_banner -decoders 2>$null)) {
    # 只收"像解码器名"的 token：图例行 "A..... = Audio" 会被解析成 "=" 必须滤掉
    if ($line -match '^\s(A[.A-Z]{5})\s+([a-z0-9_.]+)') { $decoders += $matches[2] }
}
if ($decoders.Count -lt 150) { Fail "只数出 $($decoders.Count) 个解码器——精简版异常（解析或构建坏了）" }
$reqFile = Join-Path $here 'ffmpeg_lite_required.txt'
if (-not (Test-Path $reqFile)) { Fail "缺少白名单 $reqFile" }
$required = @(Get-Content $reqFile | ForEach-Object { $_.Trim() } |
              Where-Object { $_ -and -not $_.StartsWith('#') })
$missing = @($required | Where-Object { $decoders -notcontains $_ })
if ($missing.Count -gt 0) {
    Fail ("精简版缺必需解码器（" + $missing.Count + " 项）：" + ($missing -join ', '))
}
Write-Host ("OK  白名单 " + $required.Count + " 项齐全；解码器共 " + $decoders.Count + " 个")

# ---- 3. 产品解码路径（格式矩阵）----
Write-Host "--- 3/3 格式矩阵（rta::audio::read_any）---"
if ($SkipMatrix) {
    Write-Host "SKIP  -SkipMatrix：矩阵未跑（bootstrap 用；出包流程不许带这个开关）"
} else {
    $mtx = Join-Path $root 'build\test_ffmpeg_matrix.exe'
    if (-not (Test-Path $mtx)) { Fail "缺少 $mtx（先 cmake --build build --target test_ffmpeg_matrix）" }
    $env:RTA_FFMPEG_LITE_DIR = $Dir
    try { $mOut = @(& $mtx 2>&1); $mRc = $LASTEXITCODE } finally { Remove-Item Env:\RTA_FFMPEG_LITE_DIR -ErrorAction SilentlyContinue }
    $mOut | ForEach-Object { Write-Host $_ }
    if ($mRc -ne 0) { Fail ("格式矩阵未通过（退出码 $mRc）—— 见上面 [FAIL] 行") }
    # 注意：-like '*[FAIL]*' 里的方括号是【通配符字符类】，会匹配任何含 F/A/I/L 的行
    # （实测把自己吓红过）。必须用 -match 的正则转义。
    $failLines = @($mOut | Where-Object { $_ -match '\[FAIL\]' })
    if ($failLines.Count -gt 0) { Fail "格式矩阵出现 [FAIL] 行" }
    $sum = @($mOut | Where-Object { $_ -match '\[SUMMARY\]' })
    if ($sum.Count -eq 0) { Fail "格式矩阵没有输出 [SUMMARY]（没真跑？）" }
}

Write-Host "=== 守卫通过：精简版可用，允许出包 ==="
exit 0
