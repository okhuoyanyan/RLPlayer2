# build_gpu.ps1 - build the multi-arch AMD HIP backend (rtagpu.dll).
#
# One fat binary with code objects for every AMD GPU family the compiler can
# target. The runtime picks the matching image; if the actual GPU is not in the
# list (or the driver cannot run it), rtagpu.dll reports "kernel cannot execute"
# and the player falls back to CPU - verified behaviour.
#
# Requires ROCm 7.2 (hipcc). Fails loudly if any requested arch is missing from
# the output: silently shipping a partial fat binary was how "GPU support" could
# turn into "GPU crash" in the field.
#
# Usage: powershell -File tools/build_gpu.ps1 [-Out build\rtagpu.dll]
param([string]$Out = "")

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if ($Out -eq "") { $Out = Join-Path $root 'build\rtagpu.dll' }

$rocm  = 'C:\Program Files\AMD\ROCm\7.2'
$hipcc = Join-Path $rocm 'bin\hipcc.bat'
if (-not (Test-Path $hipcc)) { Write-Host "hipcc not found: $hipcc"; exit 2 }

$archs = @(
    'gfx803', 'gfx900', 'gfx906', 'gfx908', 'gfx90a', 'gfx90c', 'gfx942',
    'gfx1010', 'gfx1011', 'gfx1012',
    'gfx1030', 'gfx1031', 'gfx1032', 'gfx1033', 'gfx1034', 'gfx1035', 'gfx1036',
    'gfx1100', 'gfx1101', 'gfx1102', 'gfx1103', 'gfx1150', 'gfx1151',
    'gfx1200', 'gfx1201'
)
$src = @((Join-Path $root 'src\gpu\gpu_conv_hip.cpp'), (Join-Path $root 'src\gpu\gpu_resample_hip.cpp'))
$hipArgs = @()
foreach ($a in $archs) { $hipArgs += "--offload-arch=$a" }
# Host code stays at the x86-64-v2 distribution baseline (never native/AVX-512).
$hipArgs += @('-O3', '-march=x86-64-v2', '-shared', '-I', (Join-Path $root 'src')) + $src + @('-o', $Out)

$log = Join-Path $root 'build\gpu_build.log'
Write-Host ("building rtagpu.dll for " + $archs.Count + " archs ...")
# Native compilers write warnings to stderr; PowerShell would treat those as
# terminating errors under ErrorActionPreference=Stop. Keep going, judge by exit code.
$eap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $hipcc @hipArgs *> $log
$rc = $LASTEXITCODE
$ErrorActionPreference = $eap
if ($rc -ne 0) {
    Write-Host ("GPU build failed (exit " + $rc + "); tail of " + $log)
    Get-Content $log -Tail 40 | ForEach-Object { Write-Host $_ }
    exit 1
}
if (-not (Test-Path $Out)) { Write-Host "GPU build produced no output"; exit 1 }

# Verify every requested architecture string really made it into the binary.
$bytes = [System.IO.File]::ReadAllBytes($Out)
$text  = [System.Text.Encoding]::ASCII.GetString($bytes)
$missing = @($archs | Where-Object { $text.IndexOf($_, [System.StringComparison]::Ordinal) -lt 0 })
if ($missing.Count -gt 0) {
    Write-Host ("GPU build missing arch code objects: " + ($missing -join ', '))
    exit 1
}
Write-Host ("rtagpu.dll multi-arch OK (" + $archs.Count + " archs): " + [math]::Round((Get-Item $Out).Length / 1KB) + " KB")
exit 0
