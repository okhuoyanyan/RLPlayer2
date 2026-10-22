# make_dist.ps1 - build the ONE official release package (Full / self-contained).
#
# Policy since 1.0.2:
#   * There is no "Lite" package any more. Every release is the official
#     release (zheng shi fa bu ban) and must be complete out of the box:
#       - ffmpeg + ffprobe: 自建【精简版】（12.2+12.0 MB，全部音频解码器；见
#         tools/build_ffmpeg_lite.ps1）。出包前由 tools/verify_ffmpeg_lite.ps1 守卫，
#         缺任一必需解码器/格式矩阵不过就拒绝出包。
#       - rtagpu.dll (multi-arch HIP kernels) + amdhip64_7.dll (HIP runtime)
#       - WebView2Loader.dll (built-in UI)
#       - CLI tools under tools\ (DSD, network, probes)
#   * The build is refused unless the compatibility gate passes.
#   * The GPU runtime is smoke-tested from a staging directory with a PATH that
#     contains only System32, i.e. exactly what a clean user machine sees.
#
# Usage: powershell -File tools/make_dist.ps1 [-Version 1.0.2] [-SkipGpuBuild]
param(
    [string]$Version = "1.0.4",
    [switch]$SkipGpuBuild
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$B = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
$rocm = 'C:\Program Files\AMD\ROCm\7.2'

function Fail($msg) { Write-Host "FAILED: $msg"; exit 1 }

Write-Host "=== build ==="
& cmake --build $B
if ($LASTEXITCODE -ne 0) { Fail "cmake build failed" }

Write-Host "=== GPU backend ==="
$gpuDll = Join-Path $B 'rtagpu.dll'
$gpuSrc = @((Join-Path $root 'src\gpu\gpu_conv_hip.cpp'), (Join-Path $root 'src\gpu\gpu_resample_hip.cpp'))
$needGpu = $false
if ($SkipGpuBuild) {
    if (-not (Test-Path $gpuDll)) { Fail "rtagpu.dll missing and -SkipGpuBuild was used" }
} else {
    if (-not (Test-Path $gpuDll)) { $needGpu = $true }
    else {
        foreach ($s in $gpuSrc) { if ((Get-Item $s).LastWriteTime -gt (Get-Item $gpuDll).LastWriteTime) { $needGpu = $true } }
    }
    if ($needGpu) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'build_gpu.ps1') -Out $gpuDll
        if ($LASTEXITCODE -ne 0) { Fail "GPU build failed" }
    } else {
        Write-Host "rtagpu.dll is up to date"
    }
}

$hipDll = Join-Path $rocm 'bin\amdhip64_7.dll'
if (-not (Test-Path $hipDll)) { Fail "HIP runtime not found: $hipDll" }

Write-Host "=== compatibility gate ==="
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'check_compat.ps1') -Dir $B
if ($LASTEXITCODE -ne 0) { Fail "compatibility gate failed - refusing to package" }

# P1（2026-10-22）内置 ffmpeg/ffprobe 一律取【自建精简版】build\ffmpeg-lite：
# 相比 gyan full（212+211.8 MB）体积 −94%，音频解码器一个不少（全部音频解码器 +
# 全部 demuxer/muxer + 临时 WAV 需要的 pcm_s16/24/32le 编码器）。
# 这里先跑守卫：它用精简版自身做裸环境执行 + 解码器白名单 + 格式矩阵（read_any）检查，
# 缺任何一项就 exit 1 —— 缺件绝不静默出一个"看起来完整"的包（铁律 6）。
$liteDir = Join-Path $B 'ffmpeg-lite'
Write-Host "=== ffmpeg 精简版守卫（缺必需解码器即拒绝出包）==="
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'verify_ffmpeg_lite.ps1') -Dir $liteDir
if ($LASTEXITCODE -ne 0) { Fail "ffmpeg 精简版守卫未通过 - 拒绝出包（原因见上面的守卫输出）" }
$ffPath = Join-Path $liteDir 'ffmpeg.exe'
$fpPath = Join-Path $liteDir 'ffprobe.exe'
if (-not (Test-Path $ffPath) -or -not (Test-Path $fpPath)) { Fail "精简版产物缺失：$liteDir" }

$name = "RLPlayer-$Version-win64"
$out = Join-Path $dist $name
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $out 'tools') | Out-Null

$main = @('RLPlayer.exe', 'rtaweb.exe', 'asio_probe.exe', 'WebView2Loader.dll', 'rtagpu.dll')
foreach ($f in $main) {
    $p = Join-Path $B $f
    if (-not (Test-Path $p)) { Fail "missing build artifact: $f" }
    Copy-Item $p $out -Force
}
Copy-Item $hipDll $out -Force
Copy-Item $ffPath (Join-Path $out 'ffmpeg.exe') -Force
Copy-Item $fpPath (Join-Path $out 'ffprobe.exe') -Force

# Fixed WebView2 runtime: copy the whole installed runtime next to the exe.
# wv2_host.h prefers .WebView2msedgewebview2.exe, so a clean machine / Win7
# does not depend on a separately installed Evergreen runtime.
$wvBase = 'C:/Program Files (x86)/Microsoft/EdgeWebView/Application'
if (-not (Test-Path $wvBase)) { Fail "WebView2 runtime not installed: $wvBase" }
$wvVer = Get-ChildItem $wvBase -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'msedgewebview2.exe') } |
         Sort-Object Name -Descending | Select-Object -First 1
if (-not $wvVer) { Fail "no WebView2 runtime version with msedgewebview2.exe" }
$wvOut = Join-Path $out 'WebView2Runtime'
New-Item -ItemType Directory -Force -Path $wvOut | Out-Null
Write-Host ("bundling WebView2 " + $wvVer.Name + " ...")
Copy-Item (Join-Path $wvVer.FullName '*') $wvOut -Recurse -Force

$tools = @('rtaplay.exe', 'dsdconv.exe', 'make_dsd.exe', 'rtanet_rx.exe', 'dopinfo.exe',
           'wavcmp.exe', 'wasapi_probe.exe', 'naa_scan.exe', 'naa_probe.exe', 'upnp_scan.exe',
           'aidecode.exe', 'rtatag.exe')
foreach ($f in $tools) {
    $p = Join-Path $B $f
    if (Test-Path $p) { Copy-Item $p (Join-Path $out 'tools') -Force }
    else { Write-Host "WARN: tool not built: $f" }
}

$readme = Get-Content (Join-Path $dist 'README-template.txt') -Raw -Encoding UTF8
$readme = $readme -replace '@VERSION@', $Version
Set-Content -Path (Join-Path $out 'README.txt') -Value $readme -Encoding UTF8

Write-Host "=== GPU staging smoke (PATH = System32 only) ==="
$stage = Join-Path $B '_gpustage_dist'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item (Join-Path $B 'test_gpu_bridge.exe') $stage
Copy-Item (Join-Path $out 'rtagpu.dll') $stage
Copy-Item (Join-Path $out 'amdhip64_7.dll') $stage
$oldPath = $env:PATH
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$gpuOut = (& (Join-Path $stage 'test_gpu_bridge.exe') 2>&1) -join [char]10
$env:PATH = $oldPath
Write-Host $gpuOut
if ($gpuOut -notmatch '\[PASS\]') { Fail "bundled GPU runtime did not initialise (see output above)" }

$zip = Join-Path $dist ($name + '.zip')
if (Test-Path $zip) { Remove-Item $zip -Force }
# tar.exe (bsdtar, built into Win10/11) handles a ~1 GB tree much faster than
# Compress-Archive and does not buffer the whole archive in memory.
$tar = Join-Path $env:SystemRoot 'System32/tar.exe'
if (Test-Path $tar) {
    Push-Location $out
    & $tar -a -c -f $zip *
    $tarRc = $LASTEXITCODE
    Pop-Location
    if ($tarRc -ne 0) { Fail "tar failed to create zip" }
} else {
    Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal
}

Write-Host ''
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
$h = Get-FileHash $zip -Algorithm SHA256
Write-Host ("zip:    " + $zip)
Write-Host ("size:   " + [math]::Round((Get-Item $zip).Length / 1MB, 1) + " MB")
Write-Host ("sha256: " + $h.Hash)
