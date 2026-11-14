# Required original open-source notices. Byte contents are preserved.
function Copy-RequiredReleaseNotices {
 param([Parameter(Mandatory=$true)][string]$SourceRoot,[Parameter(Mandatory=$true)][string]$OutputDirectory)
 $ErrorActionPreference='Stop'
 $names=@('LICENSE','OPRA-NOTICE.txt','THIRD-PARTY-NOTICES.txt')
 # Validate the complete required set before writing any package notice.
 foreach($name in $names){
  $src=Join-Path $SourceRoot $name
  if(-not (Test-Path -LiteralPath $src -PathType Leaf)){throw "FULL package required license/notice missing: $src"}
  $item=Get-Item -LiteralPath $src
  if($item.Length -le 0 -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Invalid empty/reparse license resource: $src"}
 }
 if(-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)){throw 'Package output directory must already exist'}
 foreach($name in $names){
  $src=Join-Path $SourceRoot $name;$dst=Join-Path $OutputDirectory $name
  $before=(Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
  Copy-Item -LiteralPath $src -Destination $dst -Force
  $actual=(Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash
  if($before -ne $actual -or $before -ne (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash){throw "License resource content changed during copy: $name"}
  [pscustomobject]@{File=$name;SHA256=$actual;Bytes=(Get-Item -LiteralPath $dst).Length}
 }
}

# Audio-image adaptations retain both upstream attribution and their source form.
function Copy-RequiredAudioImageSources {
 param([Parameter(Mandatory=$true)][string]$SourceRoot,[Parameter(Mandatory=$true)][string]$OutputDirectory)
 $entries=@('third_party/sacd-reference/COPYING.SACD','third_party/sacd-reference/COPYING.LGPLv2.1',
  'third_party/sacd-reference/NOTICE.txt','third_party/sacd-reference/PROVENANCE.json',
  'third_party/sacd-reference/scarletbook.h','third_party/sacd-reference/scarletbook_read.c',
  'third_party/sacd-reference/dstdec-reference.c','third_party/sacd-reference/pcm-dvda-reference.c',
  'third_party/sacd-reference/dvd_udf-reference.c',
  'src/audio/sacd_reader.h','src/audio/dst_decoder.h','src/audio/dsd_dst_reader.h','src/audio/dvd_audio.h','src/audio/iso_filesystem.h')
 foreach($rel in $entries){
  $src=Join-Path $SourceRoot $rel
  if(-not (Test-Path -LiteralPath $src -PathType Leaf)){throw "Audio-image source/notice missing: $rel"}
  Assert-PlainRedistributionPath $src
  if((Get-Item -LiteralPath $src).Length -le 0){throw "Audio-image source/notice empty: $rel"}
 }
 foreach($rel in $entries){
  $src=Join-Path $SourceRoot $rel;$dst=Join-Path $OutputDirectory $rel
  $before=(Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst)|Out-Null
  Copy-Item -LiteralPath $src -Destination $dst -Force
  if((Get-FileHash -LiteralPath $src).Hash -ne $before -or (Get-FileHash -LiteralPath $dst).Hash -ne $before){throw "Audio-image source changed during packaging: $rel"}
  [pscustomobject]@{File=$rel;SHA256=$before;Bytes=(Get-Item -LiteralPath $dst).Length}
 }
}

function Assert-PlainRedistributionPath([string]$Path) {
 $cursor=[IO.Path]::GetFullPath($Path)
 while($cursor){
  if(Test-Path -LiteralPath $cursor){if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Redistribution reparse path rejected: $cursor"}}
  $next=Split-Path -Parent $cursor;if($next -eq $cursor){break};$cursor=$next
 }
}
function Assert-RedistributionEntry([string]$Name,$Entry,[string]$Path) {
 if(-not ($Entry.sha256 -is [string]) -or $Entry.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
    -not ($Entry.bytes -is [int] -or $Entry.bytes -is [long]) -or $Entry.bytes -le 0){throw "Invalid redistribution hash/size metadata: $Name"}
 if(-not (Test-Path -LiteralPath $Path -PathType Leaf)){throw "Missing redistribution input: $Name"}
 Assert-PlainRedistributionPath $Path
 $file=Get-Item -LiteralPath $Path
 if($file.Length -ne $Entry.bytes -or (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Entry.sha256){throw "Redistribution content mismatch: $Name"}
}
function Copy-RequiredFfmpegRedistribution {
 param([Parameter(Mandatory=$true)][string]$SourceRoot,
       [Parameter(Mandatory=$true)][string]$LiteDir,
       [Parameter(Mandatory=$true)][string]$OutputDirectory)
 $ErrorActionPreference='Stop'
 $inputRoot=Join-Path $LiteDir 'redistribution'
 # 8.3 短路径（ADMINI~1）会让 Get-ChildItem 的 FullName 与 GetFullPath 前缀长度不一致，
 # 导致合法清单被误报“Unlisted”；先让 Get-Item 展开为长路径。
 if(Test-Path -LiteralPath $inputRoot){$inputRoot=(Get-Item -LiteralPath $inputRoot -Force).FullName}
 $manifestPath=Join-Path $inputRoot 'BUILD-MANIFEST.json'
 Assert-PlainRedistributionPath $inputRoot;Assert-PlainRedistributionPath $OutputDirectory
 if(-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)){throw 'FULL requires FFmpeg source redistribution/BUILD-MANIFEST.json; legacy binary-only directory is not enough'}
 Assert-PlainRedistributionPath $manifestPath
 $manifestFile=Get-Item -LiteralPath $manifestPath
 if($manifestFile.Length -le 0 -or $manifestFile.Length -gt 8MB){throw 'Invalid bounded FFmpeg build manifest'}
 $manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding utf8 | ConvertFrom-Json
 if($manifest.schema -ne 1 -or -not $manifest.files -or -not $manifest.binaries){throw 'Unsupported FFmpeg build manifest schema/files/binaries'}
 foreach($name in @('gcc.exe','libz.a','configure-script','nasm.exe')){
  $buildInputEntry=$manifest.build_inputs.PSObject.Properties[$name]
  if(-not $buildInputEntry -or -not ($buildInputEntry.Value -is [string]) -or $buildInputEntry.Value -notmatch '^[a-fA-F0-9]{64}$'){throw "Missing compiler/build-input identity: $name"}
 }
 $required=@('ffmpeg-9.0.tar.xz','zlib-1.3.2.tar.xz','FFmpeg-LICENSE.md','COPYING.GPLv2','COPYING.LGPLv2.1','zlib-LICENSE','configure.log',
             'build_ffmpeg_lite.ps1','verify_ffmpeg_lite.ps1','verify_ffmpeg_png.py','ffmpeg_lite_required.txt','write_ffmpeg_build_manifest.py')
 $listed=@($manifest.files.PSObject.Properties.Name)
 foreach($name in $required){if($listed -cnotcontains $name){throw "Required source/license/recipe missing from manifest: $name"}}
 $validated=@{}
 foreach($entry in $manifest.files.PSObject.Properties){
  $name=$entry.Name
  if([IO.Path]::IsPathRooted($name) -or $name -match '[:*?\x00-\x1f]' -or
     @($name -split '[\\/]'|Where-Object {$_ -in @('','.', '..') -or $_ -match '[. ]$' -or $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:[.].*)?$'}).Count -or
     $name -ieq 'BUILD-MANIFEST.json'){throw "Unsafe redistribution path: $name"}
  $normalized=$name.Replace('/','\')
  if($validated.ContainsKey($normalized)){throw "Duplicate redistribution destination: $name"}
  Assert-RedistributionEntry $name $entry.Value (Join-Path $inputRoot $name)
  $validated[$normalized]=$entry.Value
 }
 $prefix=[IO.Path]::GetFullPath($inputRoot).TrimEnd('\')+'\'
 $actual=@();$directories=New-Object 'System.Collections.Generic.Stack[string]';$directories.Push($inputRoot)
 while($directories.Count){
  foreach($item in Get-ChildItem -LiteralPath $directories.Pop() -Force){
   if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Redistribution contains a reparse entry'}
   if($item.PSIsContainer){$directories.Push($item.FullName)}
   elseif($item.FullName -ine $manifestPath){$actual+=$item.FullName.Substring($prefix.Length)}
  }
 }
 if(@(Compare-Object @($actual|Sort-Object) @($validated.Keys|Sort-Object)).Count){throw 'Unlisted or missing redistribution files; manifest must cover the complete set'}
 foreach($name in @('ffmpeg.exe','ffprobe.exe')){
  $entry=$manifest.binaries.PSObject.Properties[$name]
  if(-not $entry){throw "Manifest binary identity missing: $name"}
  Assert-RedistributionEntry $name $entry.Value (Join-Path $LiteDir $name)
 }
 foreach($name in @('build_ffmpeg_lite.ps1','verify_ffmpeg_lite.ps1','verify_ffmpeg_png.py','ffmpeg_lite_required.txt','write_ffmpeg_build_manifest.py')){
  $original=Join-Path (Join-Path $SourceRoot 'tools') $name
  Assert-PlainRedistributionPath $original
  if(-not (Test-Path -LiteralPath $original -PathType Leaf) -or (Get-FileHash -LiteralPath $original).Hash -ine $validated[$name].sha256){throw "Bundled FFmpeg recipe differs from current source: $name"}
 }
 if(Test-Path -LiteralPath $OutputDirectory){if(@(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count){throw 'Redistribution output must be absent or empty; refusing stale package files'}}
 $manifestHash=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
 New-Item -ItemType Directory -Path $OutputDirectory -Force|Out-Null
 foreach($name in $validated.Keys){
  $dst=Join-Path $OutputDirectory $name;$parent=Split-Path -Parent $dst
  New-Item -ItemType Directory -Path $parent -Force|Out-Null
  Copy-Item -LiteralPath (Join-Path $inputRoot $name) -Destination $dst
  Assert-RedistributionEntry $name $validated[$name] $dst
  Assert-RedistributionEntry $name $validated[$name] (Join-Path $inputRoot $name)
 }
 Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $OutputDirectory 'BUILD-MANIFEST.json')
 if((Get-FileHash -LiteralPath $manifestPath).Hash -ine $manifestHash -or (Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'BUILD-MANIFEST.json')).Hash -ine $manifestHash){throw 'FFmpeg build manifest changed during packaging'}
 foreach($name in @('ffmpeg.exe','ffprobe.exe')){Assert-RedistributionEntry $name $manifest.binaries.PSObject.Properties[$name].Value (Join-Path $LiteDir $name)}
 [pscustomobject]@{Result='PASS_COMPLETE_FFMPEG_REDISTRIBUTION';Files=$validated.Count+1;ManifestSHA256=$manifestHash.ToLowerInvariant();OutputDirectory=$OutputDirectory}
}
