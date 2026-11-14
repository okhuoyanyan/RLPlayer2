param([string]$ReviewRoot = 'D:\RLPlayer-Review4-20261102')
$ErrorActionPreference='Stop'
$compiler='C:\Program Files\AMD\ROCm\7.2\bin\clang++.exe'
$hip='C:\Program Files\AMD\ROCm\7.2\bin\hipcc.bat'
$bin=Join-Path $ReviewRoot 'build-gpu'
$evidence=Join-Path $ReviewRoot 'evidence\gpu'
$src=Join-Path $ReviewRoot 'baseline\src'
$test=Join-Path $ReviewRoot 'source\tests\test_review4_gpu.cpp'
New-Item -ItemType Directory -Path $bin,(Join-Path $bin 'rebuilt'),$evidence -Force | Out-Null
& $compiler -std=c++23 -O3 -march=x86-64-v2 -ffp-contract=off -I $src $test -lpsapi -o (Join-Path $bin 'test_review4_gpu.exe')
if($LASTEXITCODE){throw 'CPU test build failed'}
& $hip --offload-arch=gfx1201 -O3 -ffp-contract=off (Join-Path $ReviewRoot 'source\tools\review4_gpu_probe.cpp') -o (Join-Path $bin 'review4_gpu_probe.exe')
if($LASTEXITCODE){throw 'HIP constant probe build failed'}
& (Join-Path $bin 'review4_gpu_probe.exe') | Tee-Object -FilePath (Join-Path $evidence 'hardware-probe.log')
if($LASTEXITCODE){throw 'HIP constant probe failed'}
& $hip --offload-arch=gfx1201 -O3 -ffp-contract=off (Join-Path $ReviewRoot 'baseline\tests\test_gpu_multiphase_cache.cpp') -o (Join-Path $bin 'review4_gpu_cache.exe')
if($LASTEXITCODE){throw 'HIP cache test build failed'}
& (Join-Path $bin 'review4_gpu_cache.exe') | Tee-Object -FilePath (Join-Path $evidence 'cache-real-hip.log')
if($LASTEXITCODE){throw 'HIP cache test failed'}
& $hip --offload-arch=gfx1201 -O3 -march=x86-64-v2 -ffp-contract=off -shared -I $src (Join-Path $src 'gpu\gpu_conv_hip.cpp') (Join-Path $src 'gpu\gpu_resample_hip.cpp') -o (Join-Path $bin 'rebuilt\rtagpu.dll')
if($LASTEXITCODE){throw 'HIP snapshot DLL build failed'}
# Released DLL is retained unchanged; the review DLL is isolated in rebuilt.
$dist='D:\RTA-hqplayer\dist\RLPlayer-3.1.12-win64'
foreach($dll in @('rtagpu.dll','amdhip64_7.dll')) {if(-not(Test-Path -LiteralPath (Join-Path $bin $dll))){Copy-Item -LiteralPath (Join-Path $dist $dll) -Destination (Join-Path $bin $dll)}}
Copy-Item -LiteralPath (Join-Path $bin 'amdhip64_7.dll'),(Join-Path $bin 'test_review4_gpu.exe') -Destination (Join-Path $bin 'rebuilt')
Push-Location $bin
try {
 & (Join-Path $bin 'test_review4_gpu.exe') exports | Tee-Object -FilePath (Join-Path $evidence 'production-exports.log')
 if($LASTEXITCODE){throw 'Release DLL exports probe failed'}
 & (Join-Path $bin 'test_review4_gpu.exe') | Tee-Object -FilePath (Join-Path $evidence 'production-dll-cpu-gpu.log')
 if($LASTEXITCODE){throw 'Release DLL CPU/GPU test failed'}
 for($sample=1;$sample -le 3;$sample++) {& (Join-Path $bin 'test_review4_gpu.exe') memory | Tee-Object -FilePath (Join-Path $evidence ('memory-baseline-process-'+$sample+'.log'));if($LASTEXITCODE){throw 'Memory test failed'}}
 & (Join-Path $bin 'rebuilt\test_review4_gpu.exe') require-compact | Tee-Object -FilePath (Join-Path $evidence 'baseline-rebuilt-dll-cpu-gpu.log')
 if($LASTEXITCODE){throw 'Snapshot DLL CPU/GPU test failed'}
} finally {Pop-Location}
