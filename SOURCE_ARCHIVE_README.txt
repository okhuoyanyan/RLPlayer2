RLPlayer 3.3.11 corresponding-source snapshot

This is a snapshot of the actual working source, including uncommitted changes.
The immutable SHA256 list identifies every delivered file. Existing LICENSE and
third-party notices/copyright text are preserved without relicensing vendors.

Scope: player source, tests, tools, production resources/data, WebView2 build
headers/loader, and the retained SACD/DST/DVD-Audio reference sources/notices.
Build outputs, caches, recordings, local runtime configuration, secret filenames,
source backups and unused icon experiments are intentionally excluded.

This is NOT the complete source of every third-party binary in a player package.
Full upstream FFmpeg/zlib source archives and recipes are provided separately
by the full release's third-party payload; this player-source ZIP complements
those payloads. Proprietary AMD/NVIDIA SDK/runtime and Microsoft runtime sources
are not included here. The build scripts identify external dependencies and
their separate licenses still apply.

CPU application build prerequisites: Windows 11, MSVC/Windows SDK, Python,
CMake, Ninja and the documented Clang C++23 compiler. Run these from this folder
in PowerShell after installing those tools (no audio device is opened by building):

python -m pip install --target .source-audit-deps -r tools/requirements-audit.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
$deps = (Resolve-Path -LiteralPath .source-audit-deps).Path
cmake -G Ninja -S . -B build-source -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe" -DCMAKE_RC_COMPILER="C:/Program Files/AMD/ROCm/7.2/bin/llvm-rc.exe" "-DRTA_CERTIFICATE_PYTHON_PATH=$deps"
cmake --build build-source --parallel 2

GPU kernels and a self-contained distribution additionally require the vendor
SDKs and external dependencies documented in tools/build_gpu*.ps1,
tools/build_ffmpeg_lite.ps1 and tools/make_dist.ps1. Full package generation also
requires the release documentation and runtime payloads prepared by the release
workflow; those generated/non-source payloads are not claimed as part of this ZIP.

Reproducibility: lexical entry order, fixed 1980 timestamps, fixed file modes and
ZIP_STORED (no compressor-version variability). No absolute paths or wall-clock
metadata enter the ZIP. SOURCE_SHA256SUMS.txt excludes itself to avoid recursion.
