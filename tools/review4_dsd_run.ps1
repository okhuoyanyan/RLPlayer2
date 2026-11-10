param([switch]$Performance)
$ErrorActionPreference='Stop'
$reviewRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceRoot=Split-Path -Parent $PSScriptRoot
$buildRoot=Join-Path $reviewRoot 'build-dsd'
$evidenceRoot=Join-Path $reviewRoot 'evidence\dsd'
$compiler='C:\Program Files\AMD\ROCm\7.2\bin\clang++.exe'
New-Item -ItemType Directory -Force -Path $buildRoot,$evidenceRoot | Out-Null
$flags=@('-std=c++23','-O3','-march=x86-64-v2','-ffast-math','-fno-finite-math-only','-fhonor-nans','-fhonor-infinities','-fno-math-errno','-ffp-contract=off','-DNDEBUG','-D_CRT_SECURE_NO_WARNINGS',('-I'+(Join-Path $sourceRoot 'src')))
$targets=@(
 @('test_dsd1024_offline','tests/test_dsd1024_offline.cpp'),
 @('test_sdm_multilevel','tests/test_sdm_multilevel.cpp'),
 @('test_sdm_highrate_quality','tests/test_sdm_highrate_quality.cpp'),
 @('test_review4_dsd_contract','tests/test_review4_dsd_contract.cpp'),
 @('dsp_dop_benchmark','tools/dsp_dop_benchmark.cpp'),
 @('review4_dsd_e2e','tools/review4_dsd_e2e.cpp'))
foreach($target in $targets) {
 $exe=Join-Path $buildRoot ($target[0]+'.exe')
 $extra=@();if($target[0] -eq 'dsp_dop_benchmark'){$extra=@('-lbcrypt')}
 & $compiler @flags (Join-Path $sourceRoot $target[1]) @extra '-o' $exe
 if($LASTEXITCODE -ne 0){throw ('Compile failed: '+$target[0])}
}
function Run-Checked([string]$name,[string[]]$toolArgs,[string]$log) {
 & (Join-Path $buildRoot ($name+'.exe')) @toolArgs | Tee-Object -FilePath (Join-Path $evidenceRoot $log)
 if($LASTEXITCODE -ne 0){throw ('Test failed: '+$name+' '+($toolArgs -join ' '))}
}
Run-Checked 'test_dsd1024_offline' @() 'dsd1024_offline.txt'
Run-Checked 'test_review4_dsd_contract' @() 'review4-contract.txt'
foreach($rate in 45158400,49152000) {Run-Checked 'test_sdm_multilevel' @('--rate',"$rate") ("multilevel-$rate.txt")}
Run-Checked 'test_sdm_highrate_quality' @('--full') 'highrate-quality-full.txt'
Run-Checked 'dsp_dop_benchmark' @('--verify-only','--order','7','--family','dsd') 'dop-verify-fixed-default.txt'
Run-Checked 'dsp_dop_benchmark' @('--verify-only','--order','7','--family','dsd','--cutoff','20000') 'dop-verify-fixed-20k.txt'
foreach($block in 127,257) {
 Run-Checked 'review4_dsd_e2e' @('44100','1029',"$block",'7','100000','poly-sinc-short-lp','cascade',(Join-Path $evidenceRoot 'fixture-functional.wav')) ("e2e-functional-block$block.json")
}
if($Performance) {
 foreach($rep in 1..3) {foreach($rate in 44100,48000) {
  Run-Checked 'review4_dsd_e2e' @("$rate","$rate",'4096','7','100000','poly-sinc-short-lp','cascade',(Join-Path $evidenceRoot ("fixture-$rate.wav"))) ("e2e-$rate-rep$rep.json")
 }}
}
$manifest=foreach($target in $targets) {
 [pscustomobject]@{Source=$target[1];SourceSHA256=(Get-FileHash -LiteralPath (Join-Path $sourceRoot $target[1]) -Algorithm SHA256).Hash;ExecutableSHA256=(Get-FileHash -LiteralPath (Join-Path $buildRoot ($target[0]+'.exe')) -Algorithm SHA256).Hash}
}
$deps=foreach($header in 'src/dsp/dop.h','src/dsp/dop_stream.h','src/dsp/modulator.h','src/engine/pipeline.h') {[pscustomobject]@{Path=$header;SHA256=(Get-FileHash -LiteralPath (Join-Path $sourceRoot $header) -Algorithm SHA256).Hash}}
[pscustomobject]@{Compiler=$compiler;Flags=$flags;PerformanceRequested=[bool]$Performance;SharedHeaders=$deps;Artifacts=$manifest} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'manifest.json') -Encoding utf8

