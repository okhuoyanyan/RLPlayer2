param([string]$EvidenceDir='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'package_resources.ps1')
if(-not $EvidenceDir){$EvidenceDir=Join-Path $env:TEMP ('rlplayer-ffmpeg-redist-'+[guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $EvidenceDir|Out-Null
$names=@('ffmpeg-9.0.tar.xz','zlib-1.3.2.tar.xz','FFmpeg-LICENSE.md','COPYING.GPLv2','COPYING.LGPLv2.1','zlib-LICENSE','configure.log',
 'build_ffmpeg_lite.ps1','verify_ffmpeg_lite.ps1','verify_ffmpeg_png.py','ffmpeg_lite_required.txt','write_ffmpeg_build_manifest.py')
$recipes=$names|Select-Object -Last 5;$results=@()
foreach($case in @('valid','uppercase-sha','missing-manifest','missing-source','tampered-source','tampered-binary','unlisted-file','path-escape','ADS-path','changed-recipe','wrong-size','missing-compiler','reparse-directory','reserved-path')){
 $root=Join-Path $EvidenceDir $case;$source=Join-Path $root 'source';$tools=Join-Path $source 'tools';$lite=Join-Path $root 'lite';$redist=Join-Path $lite 'redistribution';$out=Join-Path $root 'output'
 New-Item -ItemType Directory -Path $tools,$redist|Out-Null
 $manifest=[ordered]@{schema=1;files=[ordered]@{};binaries=[ordered]@{};build_inputs=[ordered]@{}}
 foreach($name in @('gcc.exe','libz.a','configure-script','nasm.exe')){$manifest.build_inputs[$name]=('a'*64)}
 foreach($name in $names){
  $path=Join-Path $redist $name;[IO.File]::WriteAllBytes($path,[Text.Encoding]::UTF8.GetBytes('private synthetic resource '+$name))
  $manifest.files[$name]=[ordered]@{sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $path).Length}
  if($recipes -contains $name){Copy-Item -LiteralPath $path -Destination (Join-Path $tools $name)}
 }
 foreach($name in @('ffmpeg.exe','ffprobe.exe')){$path=Join-Path $lite $name;[IO.File]::WriteAllBytes($path,[Text.Encoding]::UTF8.GetBytes('mock never executed '+$name));$manifest.binaries[$name]=[ordered]@{sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $path).Length}}
 switch($case){
  'uppercase-sha' {foreach($name in $names){$manifest.files[$name].sha256=$manifest.files[$name].sha256.ToUpperInvariant()}}
  'missing-source' {Remove-Item -LiteralPath (Join-Path $redist 'zlib-1.3.2.tar.xz')}
  'tampered-source' {[IO.File]::AppendAllText((Join-Path $redist 'ffmpeg-9.0.tar.xz'),'changed')}
  'tampered-binary' {[IO.File]::AppendAllText((Join-Path $lite 'ffmpeg.exe'),'changed')}
  'unlisted-file' {[IO.File]::WriteAllText((Join-Path $redist 'unexpected.txt'),'not declared')}
  'path-escape' {$manifest.files['../escape.txt']=[ordered]@{sha256=('a'*64);bytes=1}}
  'ADS-path' {$manifest.files['bad.txt:stream']=[ordered]@{sha256=('a'*64);bytes=1}}
  'changed-recipe' {[IO.File]::AppendAllText((Join-Path $tools 'build_ffmpeg_lite.ps1'),'changed')}
  'wrong-size' {$manifest.files['COPYING.GPLv2'].bytes+=1}
  'missing-compiler' {$manifest.build_inputs.Remove('gcc.exe')}
  'reparse-directory' {$outside=Join-Path $root 'outside';New-Item -ItemType Directory -Path $outside|Out-Null;New-Item -ItemType Junction -Path (Join-Path $redist 'link') -Target $outside|Out-Null}
  'reserved-path' {$manifest.files['NUL.txt']=[ordered]@{sha256=('a'*64);bytes=1}}
 }
 $manifestPath=Join-Path $redist 'BUILD-MANIFEST.json';$manifest|ConvertTo-Json -Depth 8|Out-File -LiteralPath $manifestPath -Encoding utf8
 if($case -eq 'missing-manifest'){Remove-Item -LiteralPath $manifestPath}
 $accepted=$false;$detail=''
 try{$copy=@(Copy-RequiredFfmpegRedistribution -SourceRoot $source -LiteDir $lite -OutputDirectory $out);$accepted=$true}catch{$detail=$_.Exception.Message}
 $expected=$case -in @('valid','uppercase-sha');$correct=$accepted -eq $expected
 if($accepted){$correct=$correct -and @(Get-ChildItem -LiteralPath $out -File -Recurse).Count -eq 13}
 else{$correct=$correct -and -not (Test-Path -LiteralPath $out)}
 $results+=[pscustomobject]@{case=$case;correct=$correct;accepted=$accepted;detail=$detail}
 Write-Output "$case correct=$correct accepted=$accepted $detail"
}
$results|ConvertTo-Json -Depth 6|Out-File -LiteralPath (Join-Path $EvidenceDir 'regression.json') -Encoding utf8
if(@($results|Where-Object {-not $_.correct}).Count){throw 'FFmpeg redistribution controls failed'}
Write-Output 'PASS 14/14 complete redistribution controls; fixture binaries never executed'
