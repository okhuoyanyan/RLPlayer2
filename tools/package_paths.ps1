# Pure admission/path checks shared by packaging and its negative controls.
function Assert-ReleaseNoReparseAncestors([string]$Path){
 $cursor=[IO.Path]::GetFullPath($Path)
 while($cursor){
  if(Test-Path -LiteralPath $cursor){$entry=Get-Item -LiteralPath $cursor -Force -ErrorAction Stop;if(($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Release path refuses reparse ancestor: $cursor"}}
  $next=Split-Path -Parent $cursor;if(-not $next -or $next -eq $cursor){break};$cursor=$next
 }
}
function Resolve-ReleaseBuildDirectory([string]$SourceRoot,[string]$BuildDir){
 $source=[IO.Path]::GetFullPath($SourceRoot).TrimEnd('\','/');$build=[IO.Path]::GetFullPath($BuildDir).TrimEnd('\','/')
 Assert-ReleaseNoReparseAncestors $source;Assert-ReleaseNoReparseAncestors $build
 if(-not (Test-Path -LiteralPath $source -PathType Container) -or -not (Test-Path -LiteralPath $build -PathType Container)){throw 'Source/build directory is missing'}
 $cache=Join-Path $build 'CMakeCache.txt';if(-not (Test-Path -LiteralPath $cache -PathType Leaf)){throw 'Explicit release build lacks CMakeCache.txt'}
 Assert-ReleaseNoReparseAncestors $cache
 $cacheHomeEntries=@(Select-String -LiteralPath $cache -Encoding UTF8 -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$')
 if($cacheHomeEntries.Count -ne 1){throw 'CMakeCache must contain exactly one source-home identity'}
 $cacheSourceRoot=[IO.Path]::GetFullPath($cacheHomeEntries[0].Matches[0].Groups[1].Value).TrimEnd('\','/')
 Assert-ReleaseNoReparseAncestors $cacheSourceRoot
 if(-not $cacheSourceRoot.Equals($source,[StringComparison]::OrdinalIgnoreCase)){throw "CMake source-home mismatch: expected=$source actual=$cacheSourceRoot"}
 return $build
}
function Resolve-ReleaseCleanupDirectory([string]$BuildDir,[string]$Target,[string]$Parent){
 $build=[IO.Path]::GetFullPath($BuildDir).TrimEnd('\','/');$targetAbs=[IO.Path]::GetFullPath($Target).TrimEnd('\','/');$parentAbs=[IO.Path]::GetFullPath($Parent).TrimEnd('\','/')
 if(-not ($parentAbs.Equals($build,[StringComparison]::OrdinalIgnoreCase) -or $parentAbs.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase))){throw "Recursive cleanup parent is outside explicit build: $parentAbs"}
 if(-not $targetAbs.StartsWith($parentAbs+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Recursive cleanup target must be a strict child, never the build or stage root'}
 Assert-ReleaseNoReparseAncestors $targetAbs;Assert-ReleaseNoReparseAncestors $parentAbs
 if(-not (Test-Path -LiteralPath $targetAbs -PathType Container)){throw 'Recursive cleanup target is not an existing directory'}
 return $targetAbs
}
function Assert-ReleasePublishChild([string]$Target,[string]$Parent){
 $parentAbs=[IO.Path]::GetFullPath($Parent).TrimEnd('\','/');$targetAbs=[IO.Path]::GetFullPath($Target).TrimEnd('\','/')
 if(-not $targetAbs.StartsWith($parentAbs+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Publication must stay inside its named dist directory'}
 Assert-ReleaseNoReparseAncestors $targetAbs;Assert-ReleaseNoReparseAncestors $parentAbs
 return $targetAbs
}
