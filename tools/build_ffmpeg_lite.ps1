# build_ffmpeg_lite.ps1 - 构建 RLPlayer 用的【精简版】ffmpeg + ffprobe（P1）
#
# 为什么自建（结论与实测见 P1-调查.md）：
#   * 官方 full_build 的 ffmpeg.exe/ffprobe.exe 各 212 MB，占 1.0.8 下载量 29.7%；
#     预编译 essentials 仍要 100 MB/个；BtbN LGPL 静态版更大、shared 版要带 7 个 DLL。
#   * 自建（--disable-everything + 全部音频解码器 + 全部 demuxer/muxer + 少量滤镜）
#     实测 12.18 + 11.97 MB，且只依赖系统 DLL（KERNEL32/SHELL32/bcrypt/UCRT api-set）。
#
# 产物：<OutDir>\ffmpeg.exe、ffprobe.exe、MANIFEST.txt（都在 gitignore 的 build\ 下）
# 工具链：MSYS2 base(bash/sed/awk) + winlibs GCC(ucrt) + nasm，全是免安装 zip/tar，
#         首次约 317 MB 下载，之后复用 <WorkDir> 缓存（绝不提交进 git）。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_ffmpeg_lite.ps1
#   ... -RecomputeSet    # 重跑「全解码器探针构建」求音频解码器全集（默认用缓存）
#   ... -Force           # 即使清单已缓存也重建
#
# 未做到/不确定（铁律 4）：
#   * 工具链 URL 用 releases/latest/download 别名（GCC/ffmpeg/nasm 版本在脚本里断言），
#     不是逐字节钉死的 URL，也没有对下载物做 SHA256 校验；离线机器要先把 4 个压缩包
#     放进 <WorkDir>（脚本会直接用缓存）。
#   * 只做 Windows x86_64 静态构建；不做 cross、不做 shared、不签名。
param(
    [string]$OutDir = "",
    [string]$WorkDir = "",
    [switch]$Force,
    [switch]$RecomputeSet
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if (-not $OutDir)  { $OutDir  = Join-Path $root 'build\ffmpeg-lite' }
if (-not $WorkDir) { $WorkDir = Join-Path $root 'build\_fflite' }

$LF = [string][char]10
$CRLF = [string][char]13 + [string][char]10

$FFmpegVer = '9.0'
$NasmVer   = '2.16.03'
$urlMsys   = 'https://github.com/msys2/msys2-installer/releases/latest/download/msys2-base-x86_64-latest.tar.xz'
$urlWinlib = 'https://github.com/brechtsanders/winlibs_mingw/releases/latest/download/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip'
$urlNasm   = "https://www.nasm.us/pub/nasm/releasebuilds/$NasmVer/win64/nasm-$NasmVer-win64.zip"
$urlFFmpeg = "https://ffmpeg.org/releases/ffmpeg-$FFmpegVer.tar.xz"

function Fail($msg) { Write-Host "FAILED: $msg"; exit 1 }
function Step($msg) { Write-Host "=== $msg ===" }

# D:\a\b -> /d/a/b：MSYS bash 只认这种形式
function To-Posix([string]$p) {
    $s = $p -replace '\\', '/'
    if ($s -match '^([A-Za-z]):(.*)$') {
        $drive = $matches[1].ToLower()
        $rest  = $matches[2]
        if (-not $rest.StartsWith('/')) { $rest = '/' + $rest }
        return "/$drive$rest"
    }
    return $s
}

# bash 脚本必须 LF 落盘：CRLF 会让 configure/make 报 syntax error
function Write-Bash([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text.Replace($CRLF, $LF),
        (New-Object System.Text.UTF8Encoding($false)))
}

function Get-File([string]$url, [string]$dest) {
    if (Test-Path $dest) {
        Write-Host ("cached  " + $dest + "  (" + [math]::Round((Get-Item $dest).Length / 1MB, 1) + " MB)")
        return
    }
    Write-Host "GET     $url"
    & curl.exe -L --retry 3 --max-time 3600 -o $dest $url
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dest) -or (Get-Item $dest).Length -lt 1024) {
        Fail "下载失败：$url 。可手工下载后放到 $dest 再重跑（离线机器就是这个流程）"
    }
    Write-Host ("got     " + [math]::Round((Get-Item $dest).Length / 1MB, 1) + " MB")
}

New-Item -ItemType Directory -Force -Path $WorkDir, $OutDir | Out-Null
$B = To-Posix $WorkDir
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$bash = Join-Path $WorkDir 'msys64\usr\bin\bash.exe'

# ---------------------------------------------------------------- 1. 工具链
Step "工具链：MSYS2 base（bash/sed/awk）"
if (-not (Test-Path $bash)) {
    $arc = Join-Path $WorkDir 'msys2-base.tar.xz'
    Get-File $urlMsys $arc
    & $tar -xf $arc -C $WorkDir
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bash)) { Fail "MSYS2 base 解压失败（缺 $bash）" }
}
# 只验证 bash 能起（msys 的 coreutils 要靠 /usr/bin 在 PATH，-c 里跑 uname 不一定行）
$probe = (& $bash -c 'echo bash-ok' 2>&1) -join ' '
if ($LASTEXITCODE -ne 0 -or $probe -notmatch 'bash-ok') { Fail "bash 跑不起来：$bash（输出：$probe）" }

Step "工具链：winlibs GCC（MinGW-w64 ucrt）"
if (-not (Test-Path (Join-Path $WorkDir 'mingw64\bin\gcc.exe'))) {
    $zip = Join-Path $WorkDir 'winlibs.zip'
    Get-File $urlWinlib $zip
    $tmp = Join-Path $WorkDir '_unzip_mingw'
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    & $tar -xf $zip -C $tmp
    if ($LASTEXITCODE -ne 0) { Fail "winlibs 解压失败" }
    $inner = Get-ChildItem $tmp -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'bin\gcc.exe') } | Select-Object -First 1
    if (-not $inner) { Fail "winlibs 包里没找到 bin\gcc.exe" }
    if (Test-Path (Join-Path $WorkDir 'mingw64')) { Remove-Item (Join-Path $WorkDir 'mingw64') -Recurse -Force }
    Move-Item $inner.FullName (Join-Path $WorkDir 'mingw64')
    Remove-Item $tmp -Recurse -Force
}
$gccVer = @(& (Join-Path $WorkDir 'mingw64\bin\gcc.exe') --version 2>&1)[0]
Write-Host "gcc: $gccVer"
if ($gccVer -notmatch '16\.2\.0') { Fail "GCC 版本不是 16.2.0（实测：$gccVer）—— 换工具链请同步更新 $urlWinlib 与 P1-调查.md" }

Step "工具链：nasm $NasmVer"
if (-not (Test-Path (Join-Path $WorkDir "nasm-$NasmVer\nasm.exe"))) {
    $zip = Join-Path $WorkDir 'nasm.zip'
    Get-File $urlNasm $zip
    & $tar -xf $zip -C $WorkDir
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $WorkDir "nasm-$NasmVer\nasm.exe"))) { Fail "nasm 解压失败" }
}
& (Join-Path $WorkDir "nasm-$NasmVer\nasm.exe") -v | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "nasm 跑不起来" }

# ---------------------------------------------------------------- 2. ffmpeg 源码
Step "ffmpeg $FFmpegVer 源码"
$srcRoot = Join-Path $WorkDir 'src'
$src = Join-Path $srcRoot "ffmpeg-$FFmpegVer"
if (-not (Test-Path (Join-Path $src 'configure'))) {
    $arc = Join-Path $WorkDir "ffmpeg-$FFmpegVer.tar.xz"
    Get-File $urlFFmpeg $arc
    New-Item -ItemType Directory -Force -Path $srcRoot | Out-Null
    & $tar -xf $arc -C $srcRoot
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $src 'configure'))) { Fail "ffmpeg 源码解压失败" }
}

# ---------------------------------------------------------------- 3. 组件清单
$srcPosix = To-Posix $src
$envLines = "export PATH=${B}/mingw64/bin:${B}/nasm-${NasmVer}:/usr/bin:/bin" + $LF + "cd $srcPosix" + $LF
function Run-Bash([string]$name, [string]$body) {
    $p = Join-Path $WorkDir $name
    Write-Bash $p ($envLines + $body)
    & $bash $p
    if ($LASTEXITCODE -ne 0) { Fail "$name 失败（退出码 $LASTEXITCODE，日志见 $WorkDir）" }
}
$w = To-Posix $WorkDir

if ((Test-Path (Join-Path $WorkDir 'lst_decoders.txt')) -and -not $Force) {
    Write-Host "组件清单已缓存（-Force 可重建）"
} else {
    Step "组件清单：configure --list-*"
    $listsBody = "for k in decoders demuxers muxers parsers bsfs; do" + $LF +
                 "  ./configure --list-`$k > $w/lst_`$k.txt 2>/dev/null" + $LF +
                 "done" + $LF + "echo lists-ok"
    Run-Bash 'gen_lists.sh' $listsBody
}

Step "解码器全集（一列，LF）"
$allBody = "tr -s ' \t\r' '\n\n\n' < $w/lst_decoders.txt | grep -E '^[a-z0-9_.]+`$' | sort -u > $w/all_decoders.txt" + $LF +
           "echo all=`$(wc -l < $w/all_decoders.txt)"
Run-Bash 'gen_all.sh' $allBody

# ---------------------------------------------------------------- 4. 探针构建：源码里到底有哪些【音频】解码器
# 为什么要探针构建：gyan full 的 -decoders 名字（g722/atrac3plus/8svx_exp）与源码注册名
# （adpcm_g722/atrac3p/...）不一致，直接抄 full 的清单会漏掉 20 多个真音频解码器。
$audioSet = Join-Path $WorkDir 'all_audio_decoders.txt'
if ((Test-Path $audioSet) -and -not $RecomputeSet) {
    Write-Host ("音频解码器全集已缓存：" + (Get-Content $audioSet).Count + " 个（-RecomputeSet 可重算）")
} else {
    Step "探针构建（--enable-decoder=<全部> --enable-gpl）求音频解码器全集"
    $probeBody = @'
set -e
norm() { tr -s ' 	' '


' < "$1" | grep -E '^[a-z0-9_.]+$' | paste -sd, ; }
make distclean > ../../distclean_probe.log 2>&1 || true
ALL=$(norm ../../all_decoders.txt)
./configure --prefix=../../inst_probe --arch=x86_64 --target-os=mingw32 \
  --disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug \
  --disable-ffplay --disable-avdevice --enable-ffmpeg --enable-ffprobe \
  --enable-static --disable-shared --enable-protocol=file --enable-gpl \
  --enable-decoder="$ALL" --extra-cflags="-O2" --extra-ldflags="-static -s" \
  > ../../cfg_probe.log 2>&1
make -j16 > ../../make_probe.log 2>&1
./ffmpeg.exe -hide_banner -decoders 2>&1 | awk '$1 ~ /^A/ && $2 !~ /=/ {print $2}' | sort -u > ../../all_audio_decoders.txt
echo "audio=$(wc -l < ../../all_audio_decoders.txt)"
'@
    Run-Bash 'probe.sh' $probeBody
    if (-not (Test-Path $audioSet)) { Fail "探针构建没有产出 $audioSet" }
    $n = (Get-Content $audioSet).Count
    if ($n -lt 150) { Fail "探针构建只得到 $n 个音频解码器 —— 清单解析有问题，拒绝继续" }
}

# ---------------------------------------------------------------- 4b. 运行时名 -> configure 组件名
# ffmpeg -decoders 打印 .p.name（运行时名），而 --enable-decoder 吃 configure 组件名；
# 少数格式两者不同（实测 12 个）。不做这一步会「看起来」少 12 个音频解码器
# （第一次实测就掉了 eightsvx_*/adpcm_g72x/atrac3plus/interplay_acm/ra_1xx/wavesynth）。
$alias = @{
    '8svx_exp'     = 'eightsvx_exp'
    '8svx_fib'     = 'eightsvx_fib'
    'acelp.kelvin' = 'acelp_kelvin'
    'atrac3plus'   = 'atrac3p'
    'atrac3plusal' = 'atrac3pal'
    'g722'         = 'adpcm_g722'
    'g726'         = 'adpcm_g726'
    'g726le'       = 'adpcm_g726le'
    'interplayacm' = 'interplay_acm'
    'real_144'     = 'ra_144'
    'real_288'     = 'ra_288'
    'wavesynth'    = 'ffwavesynth'
}
$known = @{}
foreach ($tok in ((Get-Content (Join-Path $WorkDir 'lst_decoders.txt') -Raw) -split '\s+')) {
    if ($tok -match '^[a-z0-9_.]+$') { $known[$tok] = $true }
}
$components = New-Object System.Collections.Generic.List[string]
$unmapped = New-Object System.Collections.Generic.List[string]
foreach ($rt in (Get-Content $audioSet)) {
    $rt = $rt.Trim(); if (-not $rt) { continue }
    $comp = $rt
    if ($alias.ContainsKey($rt)) { $comp = $alias[$rt] }
    if ($known.ContainsKey($comp)) { if (-not $components.Contains($comp)) { $components.Add($comp) } }
    else { $unmapped.Add("$rt($comp)") }
}
if ($unmapped.Count -gt 0) {
    Fail ("有音频解码器无法映射到 configure 组件名：" + ($unmapped -join ', ') + "。ffmpeg 升级后改名了 —— 更新本脚本的别名表，不要静默丢格式")
}
Write-Host ("音频解码器 -> 组件名：" + $components.Count + " 个（别名 " + $alias.Count + " 条）")
$compFile = Join-Path $WorkDir 'all_audio_components.txt'
[IO.File]::WriteAllText($compFile, ($components -join $LF) + $LF, (New-Object System.Text.UTF8Encoding($false)))

# ---------------------------------------------------------------- 5. 精简版构建
Step "精简版构建（--disable-everything + 全部音频解码器）"
$liteBody = @'
set -e
norm() { tr -s ' 	' '


' < "$1" | grep -E '^[a-z0-9_.]+$' | paste -sd, ; }
make distclean > ../../distclean_lite.log 2>&1 || true
DEC=$(norm ../../all_audio_components.txt)
DEM=$(norm ../../lst_demuxers.txt)
MUX=$(norm ../../lst_muxers.txt)
PAR=$(norm ../../lst_parsers.txt)
BSF=$(norm ../../lst_bsfs.txt)
./configure --prefix=../../inst --arch=x86_64 --target-os=mingw32 \
  --disable-everything --disable-autodetect --disable-network \
  --disable-doc --disable-debug --disable-ffplay --disable-avdevice \
  --enable-ffmpeg --enable-ffprobe --enable-static --disable-shared --enable-gpl \
  --enable-protocol=file,pipe \
  --enable-decoder="$DEC,mjpeg,png,bmp,gif" \
  --enable-encoder=pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8,png,mjpeg \
  --enable-demuxer="$DEM" --enable-muxer="$MUX" --enable-parser="$PAR" --enable-bsf="$BSF" \
  --enable-filter=aresample,anull,aformat,null,scale,format,channelmap,pan,atrim,asetpts,setpts,fps,trim,copy,apad \
  --enable-swscale --enable-swresample \
  --extra-cflags="-O2" --extra-ldflags="-static -s" \
  > ../../configure_lite.log 2>&1
make -j16 > ../../make_lite.log 2>&1
mkdir -p ../../out
cp -f ffmpeg.exe ffprobe.exe ../../out/
cp -f ../../configure_lite.log ../../out/configure.log
echo "built ffmpeg=$(stat -c %s ffmpeg.exe) ffprobe=$(stat -c %s ffprobe.exe)"
'@
Run-Bash 'lite.sh' $liteBody

foreach ($exe in 'ffmpeg.exe', 'ffprobe.exe') {
    $p = Join-Path $WorkDir "out\$exe"
    if (-not (Test-Path $p)) { Fail "构建产物缺失：$p" }
    Copy-Item $p (Join-Path $OutDir $exe) -Force
}

# ---------------------------------------------------------------- 6. 自检 + 清单
Step "自检：精简版自己能不能跑、解码器白名单齐不齐"
$ff = Join-Path $OutDir 'ffmpeg.exe'
$fp = Join-Path $OutDir 'ffprobe.exe'
$verLines = @(& $ff -hide_banner -version 2>&1)
if ($LASTEXITCODE -ne 0) { Fail "精简版 ffmpeg -version 失败" }
$ver = $verLines[0]
Write-Host "ffmpeg: $ver"
& $fp -hide_banner -version 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "精简版 ffprobe -version 失败" }

$decoders = @()
foreach ($line in @(& $ff -hide_banner -decoders 2>$null)) {
    if ($line -match '^\s(A[.A-Z]{5})\s+([a-z0-9_.]+)') { $decoders += $matches[2] }
}
if ($decoders.Count -lt 150) { Fail "自检只数出 $($decoders.Count) 个解码器 —— 解析或构建有问题" }

$reqFile = Join-Path $here 'ffmpeg_lite_required.txt'
$required = @(Get-Content $reqFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') })
$missing = @($required | Where-Object { $decoders -notcontains $_ })
if ($missing.Count -gt 0) { Fail ("精简版缺必需解码器：" + ($missing -join ', ') + "（见 $reqFile）") }
Write-Host ("白名单 " + $required.Count + " 项齐全；解码器共 " + $decoders.Count + " 个")

# 覆盖率自检：探针构建测出的音频运行时名，精简版必须一个不少（「不减覆盖」机器可验证）
$haveSet = @{}
foreach ($d in $decoders) { $haveSet[$d] = $true }
$coverMissing = @()
foreach ($rt in (Get-Content $audioSet)) {
    $rt = $rt.Trim()
    if ($rt -and -not $haveSet.ContainsKey($rt)) { $coverMissing.Add($rt) }
}
if ($coverMissing.Count -gt 0) { Fail ("精简版少了音频解码器（与探针全集比对）：" + ($coverMissing -join ', ')) }
Write-Host ("覆盖率：音频全集 " + (Get-Content $audioSet).Count + " 个，精简版全部包含")

$ffSha = (Get-FileHash $ff -Algorithm SHA256).Hash
$fpSha = (Get-FileHash $fp -Algorithm SHA256).Hash
$manifest = @(
    "ffmpeg-lite MANIFEST（tools/build_ffmpeg_lite.ps1 生成）",
    "built_at       : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "ffmpeg_version : $ver",
    "ffmpeg_sha256  : $ffSha",
    "ffprobe_sha256 : $fpSha",
    ("ffmpeg_size   : " + [math]::Round((Get-Item $ff).Length / 1MB, 2) + " MB"),
    ("ffprobe_size  : " + [math]::Round((Get-Item $fp).Length / 1MB, 2) + " MB"),
    ("decoders      : " + $decoders.Count),
    ("required_ok   : " + $required.Count + "/" + $required.Count)
)
Set-Content -Path (Join-Path $OutDir 'MANIFEST.txt') -Value $manifest -Encoding UTF8

Write-Host ''
Write-Host ("产物：" + $OutDir)
Write-Host ("  ffmpeg.exe  " + [math]::Round((Get-Item $ff).Length / 1MB, 2) + " MB  sha256=" + $ffSha)
Write-Host ("  ffprobe.exe " + [math]::Round((Get-Item $fp).Length / 1MB, 2) + " MB  sha256=" + $fpSha)
Write-Host "下一步：powershell -File tools\verify_ffmpeg_lite.ps1   （出包守卫，也会跑格式矩阵）"
