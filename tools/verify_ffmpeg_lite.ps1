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
    [string]$BuildDir = "",
    [switch]$SkipMatrix
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if(-not $BuildDir){$BuildDir=Join-Path $root 'build'}
if(-not $Dir){$Dir=Join-Path $BuildDir 'ffmpeg-lite'}

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
$coverDecoders = @()
$savedEap=$ErrorActionPreference;$ErrorActionPreference='Continue'
$decoderOutput=@(& $ff -hide_banner -decoders 2>&1);$decoderExit=$LASTEXITCODE
$ErrorActionPreference=$savedEap
if($decoderExit -ne 0){Fail ('decoder inventory command failed: exit='+$decoderExit)}
foreach ($line in $decoderOutput) {
    # 只收"像解码器名"的 token：图例行 "A..... = Audio" 会被解析成 "=" 必须滤掉
    if ($line -match '^\s(A[.A-Z]{5})\s+([a-z0-9_.]+)') { $decoders += $matches[2] }
    if ($line -match '^\s(V[.A-Z]{5})\s+([a-z0-9_.]+)') { $coverDecoders += $matches[2] }
}
if ($decoders.Count -lt 150) { Fail "只数出 $($decoders.Count) 个解码器——精简版异常（解析或构建坏了）" }
$reqFile = Join-Path $here 'ffmpeg_lite_required.txt'
if (-not (Test-Path $reqFile)) { Fail "缺少白名单 $reqFile" }
$required = @(Get-Content $reqFile | ForEach-Object { $_.Trim() } |
              Where-Object { $_ -and -not $_.StartsWith('#') })
if($required.Count -eq 0){Fail 'Required decoder whitelist is empty; zero obligations are not proof'}
$missing = @($required | Where-Object { $decoders -notcontains $_ })
if ($missing.Count -gt 0) {
    Fail ("精简版缺必需解码器（" + $missing.Count + " 项）：" + ($missing -join ', '))
}
Write-Host ("OK  白名单 " + $required.Count + " 项齐全；解码器共 " + $decoders.Count + " 个")
foreach ($coverCodec in @('png','mjpeg','bmp','gif')) {
    if ($coverDecoders -notcontains $coverCodec) { Fail "Missing required cover decoder: $coverCodec" }
}

# ---- 2b. 编码器白名单（2026-10-23 新增）----
# 为什么需要：用户反馈「1.3.x 封面不显示」，根因之一是 get_cover 用 -c:v png 转码取图，
# 而精简 ffmpeg 只有 mjpeg 编码器、没有 png 编码器 → 命令静默失败 → 只返回 77 字节占位图。
# 第 2 步只校验解码器，这类「用到编码器但被裁掉」的问题完全漏网。现在把编码器也钉住。
Write-Host "--- 2b/3 必需编码器白名单 ---"
$encoders = @()
$savedEap=$ErrorActionPreference;$ErrorActionPreference='Continue'
$encoderOutput=@(& $ff -hide_banner -encoders 2>&1);$encoderExit=$LASTEXITCODE
$ErrorActionPreference=$savedEap
if($encoderExit -ne 0){Fail ('encoder inventory command failed: exit='+$encoderExit)}
foreach ($line in $encoderOutput) {
    if ($line -match '^\s([AV][.A-Z]{5})\s+([a-z0-9_.]+)') { $encoders += $matches[2] }
}
$requiredEnc = @('pcm_s16le', 'pcm_s24le', 'pcm_s32le', 'pcm_f32le', 'png', 'mjpeg')
$missingEnc = @($requiredEnc | Where-Object { $encoders -notcontains $_ })
if ($missingEnc.Count -gt 0) { Fail ("精简版缺必需编码器（" + $missingEnc.Count + " 项）：" + ($missingEnc -join ", ")) }
Write-Host ("OK  编码器白名单 " + $requiredEnc.Count + " 项齐全；编码器共 " + $encoders.Count + " 个")

# An inventory entry alone is not proof. This independent, tiny fixture checks
# actual decode/encode, attached PNG dimensions, metadata remux and bit-exact
# audio preservation without calling a generator FFmpeg from PATH.
$python = (Get-Command python -ErrorAction Stop).Source
& $python -X utf8 (Join-Path $here 'verify_ffmpeg_png.py') --dir $Dir
if ($LASTEXITCODE -ne 0) { Fail 'Actual PNG encode/decode/attached-art/audio-preservation check failed' }

# Cover extraction is exercised by the fresh generated-media matrix below,
# never an optional pre-existing build/testdata file from another build.
Write-Host '--- 3/3 private generated-media matrix ---'
if($SkipMatrix){
 Write-Host 'UNVERIFIED_MATRIX: tools/codec inventory only; this bootstrap result does not authorize packaging'
 Write-Host 'PASS_OFFLINE_TOOLS_ONLY (actual format/cover matrix not executed)'
 exit 0
}
. (Join-Path $here 'package_paths.ps1')
$BuildDir=Resolve-ReleaseBuildDirectory -SourceRoot $root -BuildDir $BuildDir
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'run_safe_ctest.ps1') -BuildDir $BuildDir -SourceRoot $root -LiteDir $Dir -Include '^ffmpeg_matrix$'
if($LASTEXITCODE -ne 0){Fail 'Actual generated format/cover matrix failed or did not execute'}
Write-Host 'PASS_PACKAGED_FFMPEG_MATRIX: tools + actual generated media decode/remux/cover; unencodable formats remain explicitly uncovered'
exit 0
