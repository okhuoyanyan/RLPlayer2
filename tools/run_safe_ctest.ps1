# Windows offline test gate. Every executable runs in a private install/cwd/profile.
param(
 [Parameter(Mandatory=$true)][string]$BuildDir,
 [string]$SourceRoot='',
 [string]$RunRoot='', [string]$FfmpegDir='', [string]$LiteDir='',
 [string]$Manifest='', [string]$Include='.*', [string[]]$Group=@(), [switch]$AuditOnly
)
$ErrorActionPreference='Stop'
if(-not $SourceRoot){$SourceRoot=Split-Path -Parent $PSScriptRoot}
$build=(Resolve-Path -LiteralPath $BuildDir).Path
$source=(Resolve-Path -LiteralPath $SourceRoot).Path
if(-not $RunRoot){$RunRoot=Join-Path $build ('safe-tests-'+[guid]::NewGuid().ToString('N'))}
$argsList=@((Join-Path $PSScriptRoot 'run_safe_tests.py'),'--build',$build,'--source',$source,'--run-root',$RunRoot,'--include',$Include)
if($Manifest){$argsList+=@('--manifest',$Manifest)}
if($AuditOnly){$argsList+='--audit-only'}else{
 if(-not $FfmpegDir){
  $ff=Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
  if(-not $ff){throw 'Offline gate requires an explicit full FFmpeg fixture generator (-FfmpegDir)'}
  $FfmpegDir=Split-Path -Parent $ff.Source
 }
 if(-not $LiteDir){$LiteDir=Join-Path $build 'ffmpeg-lite'}
 foreach($tool in @('ffmpeg.exe','ffprobe.exe')){
  if(-not (Test-Path -LiteralPath (Join-Path $FfmpegDir $tool) -PathType Leaf)){throw "Missing full fixture tool: $FfmpegDir\$tool"}
  if(-not (Test-Path -LiteralPath (Join-Path $LiteDir $tool) -PathType Leaf)){throw "Missing required packaged-lite tool: $LiteDir\$tool; cannot count a skipped decoder branch as coverage"}
 }
 $argsList+=@('--ffmpeg-dir',$FfmpegDir,'--lite-dir',$LiteDir)
 foreach($item in $Group){$argsList+=@('--group',$item)}
}
& python @argsList
if($LASTEXITCODE -ne 0){throw "Safe offline test gate failed: exit=$LASTEXITCODE (see $RunRoot)"}
