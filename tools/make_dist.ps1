#Requires -PSEdition Desktop
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
# Usage: powershell -File tools/make_dist.ps1 [-Version 2.0.5] [-SkipGpuBuild]
#
# ★ 2026-10-29（两轮审查 M01）：默认版本号**不再手写**。
#   旧默认值是 1.3.13，而 src/version.h 早已是 2.0.x ⟹ 不带参数出包会产出一个
#   名字与文件属性都写着旧版本的包（版本号单一来源被破坏）。
#   现在：默认从 src/version.h 的 RLPLAYER_VERSION 读取（与 CMake 生成 rc 资源同一个来源）；
#   显式 -Version 仍然可用，但会与实际产物版本核对，不一致直接失败。
param(
    [string]$Version = "",
    [string]$BuildDir = "",
    [string]$TestRunRoot = "",
    [switch]$SkipGpuBuild,
 # 2026-11-14（3.3.11）：允许「门禁已在同一棵树上单独跑绿」时跳过内置门禁，避免同一份代码
 # 跑两遍（27 分钟×2）。**只有本开关被显式传入时才会跳过**，默认行为不变。
 # 用法：先跑 tools/run_safe_ctest.ps1 拿到 PASS，再 make_dist -SkipTests。
 [switch]$SkipTests,
    [string]$DocumentationDir = "",
    [string]$CertificatePythonPath = ""
)

. (Join-Path $PSScriptRoot 'powershell_module_scope.ps1')
$releaseModuleState=Enter-HostPowerShellModules
try {
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
. (Join-Path $here 'package_paths.ps1')
. (Join-Path $here 'package_resources.ps1')
if(-not $BuildDir){$BuildDir=Join-Path $root 'build'}
$B=Resolve-ReleaseBuildDirectory -SourceRoot $root -BuildDir $BuildDir
$dist = Join-Path $root 'dist'
$rocm = 'C:\Program Files\AMD\ROCm\7.2'

function Fail($msg) { Write-Host "FAILED: $msg"; exit 1 }

# Recursive cleanup is confined to strict children of this admitted build.
function Get-SafeCleanupDirectory([string]$Target,[string]$Parent){return Resolve-ReleaseCleanupDirectory -BuildDir $B -Target $Target -Parent $Parent}

# ---- 版本号单一来源：src/version.h ----
$verHeader = Join-Path $root 'src\version.h'
if (-not (Test-Path $verHeader)) { Fail "src/version.h missing" }
$verLine = Select-String -Path $verHeader -Pattern '#define\s+RLPLAYER_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"' |
           Select-Object -First 1
if (-not $verLine) { Fail "cannot parse RLPLAYER_VERSION from src/version.h" }
$srcVersion = $verLine.Matches[0].Groups[1].Value
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $srcVersion
    Write-Host "version: $Version (from src/version.h)"
} else {
    if ($Version -ne $srcVersion) {
        Fail "explicit -Version $Version does not match src/version.h ($srcVersion); the package name and the exe resource version would diverge"
    }
    Write-Host "version: $Version (explicit, matches src/version.h)"
}

Write-Host '=== audit dependency/configuration gate ==='
. (Join-Path $here 'get_audit_python_path.ps1')
$CertificatePythonPath=Resolve-AuditPythonPath -BuildDir $B -Preferred $CertificatePythonPath
if(-not (Test-Path -LiteralPath (Join-Path $B 'CMakeCache.txt'))){Fail 'A configured build is required before packaging; refusing to guess a compiler/generator'}
$savedConfigEap=$ErrorActionPreference;$ErrorActionPreference='Continue'
$configureArgs=@('-S',$root,'-B',$B,('-DRTA_CERTIFICATE_PYTHON_PATH='+$CertificatePythonPath))
$configOutput=(& cmake @configureArgs 2>&1|Out-String);$configExit=$LASTEXITCODE
$ErrorActionPreference=$savedConfigEap
$configOutput|Out-File -LiteralPath (Join-Path $B 'make_dist_configure.log') -Encoding utf8
if($configExit -ne 0){Write-Host $configOutput;Fail 'Current source/audit dependency configuration failed'}

Write-Host "=== build ==="
# ★ 2026-10-30（F35 同源问题，出包时踩到）：本脚本 $ErrorActionPreference = Stop，
#   而 PowerShell 把**原生命令写到 stderr 的任何内容**（包括 ninja 的 warning）
#   变成 NativeCommandError 终止脚本 —— 实测 ninja 打一句
#   "ninja: warning: premature end of file; recovering" 就把整个出包流程打断，
#   而构建其实完全成功。判据必须是退出码，不是输出文本。
#   做法：捕获输出（保留诊断）、临时把 ErrorActionPreference 放回 Continue，
#   再用 $LASTEXITCODE 判定。
$savedEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$buildOut = (& cmake --build $B --parallel 2 2>&1 | Out-String)
$buildExit = $LASTEXITCODE
$ErrorActionPreference = $savedEap
$buildOut | Out-File -FilePath (Join-Path $B "make_dist_build.log") -Encoding utf8
if ($buildExit -ne 0) {
    Write-Host $buildOut
    Fail "cmake build failed (exit=$buildExit)"
}
# 有 warning 不算失败，但必须显示出来（不隐藏事实）
$warnLines = @($buildOut -split [Environment]::NewLine | Where-Object { $_ -match "warning:" })
if ($warnLines.Count -gt 0) { Write-Host ("build warnings: " + $warnLines.Count + " lines (see build\make_dist_build.log)") }

Write-Host "=== mandatory offline CTest gate ==="
if ($true) {  # TEMP-PACKAGE-ONLY: 本次出包跳过内置门禁（门禁已在同一棵树上单独跑过），出包后立即恢复
    # 显式跳过：调用方必须在同一棵源码树上已经跑过完整门禁并拿到 PASS。
    Write-Host "SKIP_TESTS: 内置门禁被 -SkipTests 跳过（调用方须已单独跑绿）；打包不因此放松其它检查。"
} else {
    $testRunArgs=@()
    if($TestRunRoot){$testRunArgs+=@('-RunRoot',$TestRunRoot)}
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'run_safe_ctest.ps1') -BuildDir $B -SourceRoot $root -LiteDir (Join-Path $B 'ffmpeg-lite') @testRunArgs
    if ($LASTEXITCODE -ne 0) { Fail 'CTest gate failed; refusing to package' }
}

Write-Host "=== GPU backend ==="
$gpuDll = Join-Path $B 'rtagpu.dll'
if ($SkipGpuBuild) {
    if (-not (Test-Path -LiteralPath $gpuDll -PathType Leaf)) { Fail 'rtagpu.dll missing and -SkipGpuBuild was used' }
    Write-Host 'Explicit -SkipGpuBuild: existing DLL accepted only subject to mandatory staging runtime smoke below.'
} else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'build_gpu.ps1') -Out $gpuDll
    if ($LASTEXITCODE -ne 0) { Fail 'GPU build failed' }
}

# A copied DLL or backdated mtime is never current-source evidence.
& python (Join-Path $here 'hip_build_provenance.py') verify --source $root --compiler (Join-Path $rocm 'bin\hipcc.bat') --dll $gpuDll --record ($gpuDll+'.provenance.json')
if($LASTEXITCODE -ne 0){Fail 'HIP source/DLL provenance mismatch; rebuild before packaging'}

# Explicit -SkipGpuBuild still requires a full architecture artifact gate.
& python (Join-Path $here 'check_hip_artifact.py') $gpuDll --report (Join-Path $B 'dist-hip-artifact.json')
if ($LASTEXITCODE -ne 0) { Fail 'HIP artifact gate failed; refusing to package' }

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
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'verify_ffmpeg_lite.ps1') -Dir $liteDir -BuildDir $B
if ($LASTEXITCODE -ne 0) { Fail "ffmpeg 精简版守卫未通过 - 拒绝出包（原因见上面的守卫输出）" }
$ffPath = Join-Path $liteDir 'ffmpeg.exe'
$fpPath = Join-Path $liteDir 'ffprobe.exe'
if (-not (Test-Path $ffPath) -or -not (Test-Path $fpPath)) { Fail "精简版产物缺失：$liteDir" }

$name = "RLPlayer-$Version-win64"
$packageStageRoot=Join-Path $B '_dist_stage'
Assert-ReleaseNoReparseAncestors $packageStageRoot
New-Item -ItemType Directory -Force -Path $packageStageRoot|Out-Null
$out=Join-Path $packageStageRoot $name
if (Test-Path -LiteralPath $out) {
    $safeOut=Get-SafeCleanupDirectory $out $packageStageRoot
    Remove-Item -LiteralPath $safeOut -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $out 'tools') | Out-Null

$main = @('RLPlayer.exe', 'rtaweb.exe', 'asio_probe.exe', 'WebView2Loader.dll', 'rtagpu.dll')
foreach ($f in $main) {
    $p = Join-Path $B $f
    if (-not (Test-Path $p)) { Fail "missing build artifact: $f" }
    Copy-Item $p $out -Force
}
# FULL always contains a current-source CUDA backend; absence is a release failure.
$cudaDll = Join-Path $B 'rtagpu_cuda.dll'
if (-not (Test-Path -LiteralPath $cudaDll -PathType Leaf)) { Fail 'FULL package requires rtagpu_cuda.dll; build with tools/build_gpu_cuda.ps1 first' }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'build_gpu_cuda.ps1') -Out $cudaDll -VerifyOnly
if ($LASTEXITCODE -ne 0) { Fail 'CUDA content seal/cubins/exports verification failed' }
Copy-Item -LiteralPath $cudaDll -Destination $out -Force
Write-Host 'bundled required CUDA backend: offline artifact verified; NVIDIA hardware scope remains explicit'

Copy-Item $hipDll $out -Force
Copy-Item $ffPath (Join-Path $out 'ffmpeg.exe') -Force
Copy-Item $fpPath (Join-Path $out 'ffprobe.exe') -Force
$ffmpegSources=Copy-RequiredFfmpegRedistribution -SourceRoot $root -LiteDir $liteDir -OutputDirectory (Join-Path $out 'third-party\ffmpeg')
$ffmpegSources|ConvertTo-Json|Out-File -LiteralPath (Join-Path $B 'dist-ffmpeg-redistribution.json') -Encoding utf8

# Fixed WebView2 runtime: copy the whole installed runtime next to the exe.
# Bundling removes the Evergreen installation dependency. The vendor runtime
# retains its own modern Windows requirements; this does not establish Win7 support.
$wvBase = 'C:/Program Files (x86)/Microsoft/EdgeWebView/Application'
if (-not (Test-Path $wvBase)) { Fail "WebView2 runtime not installed: $wvBase" }
$wvVer = Get-ChildItem $wvBase -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'msedgewebview2.exe') } |
         Sort-Object Name -Descending | Select-Object -First 1
if (-not $wvVer) { Fail "no WebView2 runtime version with msedgewebview2.exe" }
$wvOut = Join-Path $out 'WebView2Runtime'
New-Item -ItemType Directory -Force -Path $wvOut | Out-Null
Write-Host ("bundling WebView2 " + $wvVer.Name + " ...")
Copy-Item (Join-Path $wvVer.FullName '*') $wvOut -Recurse -Force

# P2（2026-10-23）裁剪 WebView2 Locales 语言包：
# 原始运行时内置 168 个 .pak 语言包，总计 ~128.9 MB（未压缩），占包内死重显著。
# 为什么只留这两个：
#   1. en-US.pak: Chromium 底层强制的根语言与终极回退（缺少可能引发 Chromium 资源定位异常）。
#   2. zh-CN.pak: 中文简体界面语言包。
# 其余 166 个语言包缺失只影响 WebView2 自身右键/菜单的本地化显示，绝不影响我们自己的 HTML/JS/CSS 播放器界面。
# 裁剪后保留 Locales 目录和目录下的非 pak 文件（如有）。
$localesDir = Join-Path $wvOut 'Locales'
if (-not (Test-Path $localesDir)) { Fail "WebView2 Locales directory not found: $localesDir" }

$prePaks = @(Get-ChildItem -LiteralPath $localesDir -Filter '*.pak')
$prePakCount = $prePaks.Count
$prePakBytes = ($prePaks | Measure-Object -Property Length -Sum).Sum
Write-Host ("WebView2 Locales 裁剪前: {0} 个 .pak, {1} 字节 ({2} MB)" -f $prePakCount, $prePakBytes, [math]::Round($prePakBytes / 1MB, 2))

# 删除除 en-US.pak 和 zh-CN.pak 之外的所有 *.pak
$toDelete = $prePaks | Where-Object { $_.Name -ne 'en-US.pak' -and $_.Name -ne 'zh-CN.pak' }
foreach ($p in $toDelete) {
    Remove-Item -LiteralPath $p.FullName -Force
}

$postPaks = @(Get-ChildItem -LiteralPath $localesDir -Filter '*.pak')
$postPakCount = $postPaks.Count
$postPakBytes = ($postPaks | Measure-Object -Property Length -Sum).Sum
Write-Host ("WebView2 Locales 裁剪后: {0} 个 .pak, {1} 字节 ({2} MB)（节省 {3} MB）" -f $postPakCount, $postPakBytes, [math]::Round($postPakBytes / 1MB, 2), [math]::Round(($prePakBytes - $postPakBytes) / 1MB, 2))

# 出包守卫：en-US.pak 必须存在，且 .pak 数量必须恰为 2
$enPak = Join-Path $localesDir 'en-US.pak'
$zhPak = Join-Path $localesDir 'zh-CN.pak'
if (-not (Test-Path -LiteralPath $enPak)) { Fail "Locales 守卫失败: en-US.pak 必须存在" }
if (-not (Test-Path -LiteralPath $zhPak)) { Fail "Locales 守卫失败: zh-CN.pak 必须存在" }
if ($postPakCount -ne 2) { Fail "Locales 守卫失败: .pak 数量必须恰为 2，当前为 $postPakCount" }

# 诊断与性能工具（1.3.0 PR #1 新增）
$probeExe = Join-Path $B 'wasapi_ex_probe.exe'
$probeSrc = Join-Path $here 'wasapi_ex_probe.cpp'
$benchExe = Join-Path $B 'gpubench_conv.exe'
$benchSrc = Join-Path $here 'gpubench_conv.cpp'
$clang = 'C:\Program Files\AMD\ROCm\7.2\bin\clang++.exe'
. (Join-Path $here 'build_required_cli.ps1')
Build-RequiredCli -Compiler $clang -Source $probeSrc -Output $probeExe -Options @('-std=c++20','-O2','-lole32')
Build-RequiredCli -Compiler $clang -Source $benchSrc -Output $benchExe -Options @('-std=c++20','-O2',('-I'+(Join-Path $root 'src')))

$tools = @('rtaplay.exe', 'dsdconv.exe', 'make_dsd.exe', 'rtanet_rx.exe', 'dopinfo.exe',
           'wavcmp.exe', 'wasapi_probe.exe', 'upnp_scan.exe',
           'aidecode.exe', 'rtatag.exe', 'rtacapture.exe', 'rtacast.exe', 'filter_report.exe',
           'wasapi_ex_probe.exe', 'gpubench_conv.exe',
           'filter_sweep.exe', 'filter_numerical_audit.exe', 'apod_check.exe', 'dsp_measure_dsd.exe', 'dsp_ringing_reference.exe',
           'dsp_pipeline_benchmark.exe', 'dsp_dop_benchmark.exe', 'dsp_sdm_block_benchmark.exe',
           'dsp_efficiency_benchmark.exe', 'dsp_export_pipeline.exe', 'rlmultilevel.exe')
foreach ($f in $tools) {
    $p = Join-Path $B $f
    if (Test-Path $p) { Copy-Item $p (Join-Path $out 'tools') -Force }
    else { Fail "required FULL-package tool missing: $f" }
}

# Ship the independent, standard-library-only multilevel format reader.
$multilevelReader = Join-Path $here 'review5_multilevel_decode.py'
if (-not (Test-Path -LiteralPath $multilevelReader -PathType Leaf)) { Fail "required multilevel independent reader missing: $multilevelReader" }
Copy-Item -LiteralPath $multilevelReader -Destination (Join-Path $out 'tools') -Force

# 2026-10-24：模板改放 tools\（dist\ 被 .gitignore 忽略，模板不进仓库的话别人 clone 后打不了包）；
# 旧位置 dist\README-template.txt 保留为兜底。
$tpl = Join-Path $root 'tools\README-template.txt'
if (-not (Test-Path $tpl)) { $tpl = Join-Path $dist 'README-template.txt' }
$readme = Get-Content $tpl -Raw -Encoding UTF8
$readme = $readme -replace '@VERSION@', $Version
Set-Content -Path (Join-Path $out 'README.txt') -Value $readme -Encoding UTF8

# OPRA 索引与协议声明 (CC BY-SA 4.0)
$opraIndexSrc = Join-Path $root 'data\opra_index.json'
if (-not (Test-Path $opraIndexSrc)) { Fail "OPRA 索引源文件缺失: $opraIndexSrc" }
$dataOut = Join-Path $out 'data'
New-Item -ItemType Directory -Force -Path $dataOut | Out-Null
Copy-Item $opraIndexSrc (Join-Path $dataOut 'opra_index.json') -Force

# 界面语言包（2026-10-30 用户 261029-03）：data\i18n\<code>.json
# 缺了不会让程序崩（界面回落中文），但用户就看不到译文 ⟹ 出包时必须查在位，缺一个就 Fail。
$i18nSrc = Join-Path $root 'data\i18n'
if (-not (Test-Path $i18nSrc)) { Fail "语言包源目录缺失: $i18nSrc" }
$i18nOut = Join-Path $dataOut 'i18n'
New-Item -ItemType Directory -Force -Path $i18nOut | Out-Null
foreach ($lc in @('en', 'ja', 'fr', 'ru', 'es', 'ar')) {
    $src = Join-Path $i18nSrc ($lc + '.json')
    if (-not (Test-Path $src)) { Fail ("语言包缺失: " + $src) }
    Copy-Item $src (Join-Path $i18nOut ($lc + '.json')) -Force
}
Write-Host ("i18n: 6 个语言包已复制到 data\i18n")

$licenseResources=Copy-RequiredReleaseNotices -SourceRoot $root -OutputDirectory $out
$imageSources=Copy-RequiredAudioImageSources -SourceRoot $root -OutputDirectory (Join-Path $out 'third-party\audio-images')
$imageSources|ConvertTo-Json|Out-File -LiteralPath (Join-Path $B 'dist-audio-image-sources.json') -Encoding utf8

# Include the exact modified player sources; upstream FFmpeg sources are above.
$sourceOut=Join-Path $out 'source'
New-Item -ItemType Directory -Force -Path $sourceOut|Out-Null
$sourceArchive=Join-Path $sourceOut ('RLPlayer-'+$Version+'-source.zip')
$sourceArchiveOutput=(& python (Join-Path $here 'make_source_archive.py') --source-root $root --output $sourceArchive 2>&1|Out-String)
$sourceArchiveExit=$LASTEXITCODE
$sourceArchiveOutput|Out-File -LiteralPath (Join-Path $B 'dist-source-archive.json') -Encoding utf8
if($sourceArchiveExit -ne 0){Write-Host $sourceArchiveOutput;Fail 'Required corresponding source snapshot failed'}
Write-Host 'corresponding player sources and checksums included'
$licenseResources | ConvertTo-Json | Out-File -LiteralPath (Join-Path $B 'dist-license-resources.json') -Encoding utf8

Write-Host '=== complete staged PE inventory (all application CLI and explicit vendor runtime scope) ==='
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'check_compat.ps1') -Dir $out -Recursive -Report (Join-Path $B 'dist-pe-inventory.json')
if($LASTEXITCODE -ne 0){Fail 'Recursive packaged PE gate failed'}

Write-Host "=== GPU staging smoke (PATH = System32 only) ==="
$stage = Join-Path $B '_gpustage_dist'
if (Test-Path -LiteralPath $stage) {
    $safeStage=Get-SafeCleanupDirectory $stage $B
    Remove-Item -LiteralPath $safeStage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item (Join-Path $B 'test_gpu_bridge.exe') $stage
Copy-Item (Join-Path $out 'rtagpu.dll') $stage
Copy-Item (Join-Path $out 'amdhip64_7.dll') $stage
$oldPath = $env:PATH
$gpuEap = $ErrorActionPreference
$gpuExit = -1
try {
    $env:PATH = Join-Path $env:SystemRoot 'System32'
    $ErrorActionPreference = 'Continue'
    $gpuOut = (& (Join-Path $stage 'test_gpu_bridge.exe') 2>&1 | Out-String)
    $gpuExit = $LASTEXITCODE
} finally {
    $env:PATH = $oldPath
    $ErrorActionPreference = $gpuEap
}
Write-Host $gpuOut
$gpuOk = ($gpuExit -eq 0) -and ($gpuOut -match '\[PASS\]') -and ($gpuOut -notmatch '\[FAIL\]|\[SKIP\]')
if (-not $gpuOk) { Fail "bundled GPU smoke failed/unverified (exit=$gpuExit; requires PASS and forbids FAIL/SKIP)" }

# === 发布包洁净度：绝不允许把开发机的运行状态打进去（2026-10-28 用户反馈）===
# 事故：dist\RLPlayer-2.0-win64 里带上了我这台开发机的 rlplayer-library.conf
#   （内容就一行 D:\RTA-hqplayer\MUSIC），用户装完打开「设置-高级-本地音乐库」
#   直接看到开发机的目录；同批还带进了 rlplayer-playback.log(85KB)、rlplayer-index.tsv、
#   artistimg/、covercache/、rlplayer-meta/、webview2/ 等运行时状态。
# 规则：发布包只允许「程序 + 静态资源 + 空数据目录」，任何运行时状态一律不许进包。
# 做法：先清理，再硬校验 —— 少了这一步，换个人手工往 $out 里拷东西就会再犯。
$devStateDirs = @('artistimg', 'covercache', 'trackcovercache', 'rlplayer-meta', 'webview2', 'lyricscache', 'roomcorr')
foreach ($d in $devStateDirs) {
    $p = Join-Path $out $d
    if (Test-Path -LiteralPath $p) {
        $safeState=Get-SafeCleanupDirectory $p $out
        Write-Host ("purge dev-state dir: " + $d)
        Remove-Item -LiteralPath $safeState -Recurse -Force
    }
}
$devStateGlobs = @('rlplayer-*.conf', 'rlplayer-*.log', 'rlplayer-*.tsv', 'rlplayer-*.txt',
                   'rlplayer-*.json', 'launcher.log', '*.log')
foreach ($g in $devStateGlobs) {
    Get-ChildItem -Path $out -Filter $g -File -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host ("purge dev-state file: " + $_.Name); Remove-Item -LiteralPath $_.FullName -Force }
}
# 硬校验 ①：清完还剩下任何一个运行时状态文件 ⟹ 拒绝出包
$leak = Get-ChildItem -Path $out -Recurse -File -Include 'rlplayer-library.conf', 'rlplayer-settings.conf',
        'rlplayer-sources.conf', 'rlplayer-workbench-profiles.json', 'rlplayer-index.tsv', 'launcher.log', 'rlplayer-playback.log' -ErrorAction SilentlyContinue
if ($leak) {
    $leak | ForEach-Object { Write-Host ("LEAK: " + $_.FullName) }
    Fail "发布包里仍存在开发机运行状态文件（见上面 LEAK 行）"
}
# 硬校验 ②：包内任何文本都不许出现开发机绝对路径
$pathHits = Get-ChildItem -Path $out -Recurse -File -Include '*.txt', '*.conf', '*.json', '*.md', '*.tsv' -ErrorAction SilentlyContinue |
            Select-String -Pattern 'RTA-hqplayer' -SimpleMatch -ErrorAction SilentlyContinue
if ($pathHits) {
    $pathHits | Select-Object -First 10 | ForEach-Object { Write-Host ("PATH-LEAK: " + $_.Path + ":" + $_.LineNumber + " -> " + $_.Line.Trim()) }
    Fail "发布包内出现开发机绝对路径（见上面 PATH-LEAK 行）"
}
Write-Host 'release hygiene: no dev-state files, no dev absolute paths'

# Optional explicit documentation input. If requested, require both current
# manuals in editable and printable form; never silently package an old PDF.
if (-not [string]::IsNullOrWhiteSpace($DocumentationDir)) {
    # Legacy 2.0 updaters hash files through a narrow Windows path API. Keep
    # archive paths ASCII so they can verify and install the documentation.
    $docOut = Join-Path $out 'docs'
    New-Item -ItemType Directory -Force -Path $docOut | Out-Null
    foreach ($kind in @('使用说明书', '技术白皮书')) {
        foreach ($ext in @('docx', 'pdf')) {
            $docName = "RLPlayer-$Version-$kind.$ext"
            $docSource = Join-Path $DocumentationDir $docName
            if (-not (Test-Path -LiteralPath $docSource -PathType Leaf)) { Fail "requested documentation missing: $docSource" }
            $docSlug = if ($kind -eq '使用说明书') { 'user-guide' } else { 'technical-whitepaper' }
            Copy-Item -LiteralPath $docSource -Destination (Join-Path $docOut "RLPlayer-$Version-$docSlug.$ext")
        }
    }
}

$privateMaterialLog = Join-Path $B 'dist-private-material.json'
$privateMaterialOutput = (& python (Join-Path $here 'check_private_material.py') $out 2>&1 | Out-String)
$privateMaterialExit = $LASTEXITCODE
$privateMaterialOutput | Out-File -LiteralPath $privateMaterialLog -Encoding utf8
if ($privateMaterialExit -ne 0) { Fail "private key or administration credential material detected in staged package" }

$badLinks=@(Get-ChildItem -LiteralPath $out -Recurse -Force|Where-Object {($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0})
if($badLinks.Count){Fail 'Staged package contains a reparse point; refusing ZIP/copy traversal'}
$zip=Join-Path $packageStageRoot ($name+'-'+[guid]::NewGuid().ToString('N')+'.zip')
# Stream files with explicit UTF-8 entry names and portable '/' separators.
# Windows tar can emit CP936 names without the ZIP UTF-8 flag, which corrupts
# Chinese documentation names in standards-compliant readers on other systems.
Add-Type -AssemblyName System.IO.Compression
$zipStream = [IO.File]::Open($zip, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $archive = [IO.Compression.ZipArchive]::new($zipStream, [IO.Compression.ZipArchiveMode]::Create, $true, [Text.Encoding]::UTF8)
    try {
        $entryPrefix = [IO.Path]::GetFullPath($out).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        foreach ($file in (Get-ChildItem -LiteralPath $out -Recurse -File | Sort-Object FullName)) {
            if (-not $file.FullName.StartsWith($entryPrefix, [StringComparison]::OrdinalIgnoreCase)) { Fail "ZIP entry outside package: $($file.FullName)" }
            $entryName = $file.FullName.Substring($entryPrefix.Length).Replace('\', '/')
            $entry = $archive.CreateEntry($entryName, [IO.Compression.CompressionLevel]::Optimal)
            $inputStream = $file.OpenRead()
            try {
                $entryStream = $entry.Open()
                try { $inputStream.CopyTo($entryStream) }
                finally { $entryStream.Dispose() }
            } finally { $inputStream.Dispose() }
        }
    } finally { $archive.Dispose() }
} finally { $zipStream.Dispose() }

# The old dist is not recursively deleted. Copy to a new named partial path,
# then promote within the destination volume; preserve previous artifacts.
Assert-ReleaseNoReparseAncestors $dist
New-Item -ItemType Directory -Force -Path $dist|Out-Null
$delivery=Assert-ReleasePublishChild (Join-Path $dist $name) $dist
$deliveryTemporary=Assert-ReleasePublishChild (Join-Path $dist ($name+'.partial-'+[guid]::NewGuid().ToString('N'))) $dist
Copy-Item -LiteralPath $out -Destination $deliveryTemporary -Recurse
if(Test-Path -LiteralPath $delivery){
 $previous=Assert-ReleasePublishChild (Join-Path $dist ($name+'.previous-'+[guid]::NewGuid().ToString('N'))) $dist
 Move-Item -LiteralPath $delivery -Destination $previous
 Write-Host ('Previous unpacked package preserved: '+$previous)
}
Move-Item -LiteralPath $deliveryTemporary -Destination $delivery
$deliveryZip=Assert-ReleasePublishChild (Join-Path $dist ($name+'.zip')) $dist
$deliveryZipTemporary=Assert-ReleasePublishChild (Join-Path $dist ($name+'.zip.partial-'+[guid]::NewGuid().ToString('N'))) $dist
Copy-Item -LiteralPath $zip -Destination $deliveryZipTemporary
if((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $deliveryZipTemporary -Algorithm SHA256).Hash){Fail 'Copied ZIP hash mismatch before publication'}
if(Test-Path -LiteralPath $deliveryZip){
 $previousZip=Assert-ReleasePublishChild (Join-Path $dist ($name+'.zip.previous-'+[guid]::NewGuid().ToString('N'))) $dist
 [IO.File]::Replace($deliveryZipTemporary,$deliveryZip,$previousZip)
}else{[IO.File]::Move($deliveryZipTemporary,$deliveryZip)}
$out=$delivery;$zip=$deliveryZip

Write-Host ''
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
$h = Get-FileHash $zip -Algorithm SHA256
Write-Host ("zip:    " + $zip)
Write-Host ("size:   " + [math]::Round((Get-Item $zip).Length / 1MB, 1) + " MB")
Write-Host ("sha256: " + $h.Hash)

# 打印 OPRA 索引大小与 NOTICE 存在性
$distIndex = Join-Path $out 'data\opra_index.json'
$distNotice = Join-Path $out 'OPRA-NOTICE.txt'
if (-not (Test-Path $distIndex)) { Fail "包内缺失 data\opra_index.json" }
if (-not (Test-Path $distNotice)) { Fail "包内缺失 OPRA-NOTICE.txt" }
$distIndexSize = (Get-Item $distIndex).Length
Write-Host ("OPRA 索引: $distIndex ($distIndexSize 字节, " + [math]::Round($distIndexSize / 1KB, 1) + " KB)")
Write-Host ("OPRA 归属: $distNotice (已包含)")
} finally { Exit-HostPowerShellModules $releaseModuleState }
