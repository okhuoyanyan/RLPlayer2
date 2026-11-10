param([string]$EvidenceDir='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'package_paths.ps1')
if(-not $EvidenceDir){$EvidenceDir=Join-Path $env:TEMP ('rlplayer-package-paths-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path -LiteralPath $EvidenceDir){throw 'Path controls require a fresh owned directory'}
New-Item -ItemType Directory -Path $EvidenceDir|Out-Null
$sourceRoot=Join-Path $EvidenceDir 'source';$externalBuild=Join-Path $EvidenceDir 'external-build'
New-Item -ItemType Directory -Path $sourceRoot,$externalBuild|Out-Null
$cache=Join-Path $externalBuild 'CMakeCache.txt';$results=@()
function Check-PathCase([string]$Name,[scriptblock]$Action,[bool]$Expected){
 $accepted=$true
 try{& $Action|Out-Null}catch{$accepted=$false}
 $script:results+=[pscustomobject]@{case=$Name;expectedAcceptance=$Expected;accepted=$accepted;correct=($accepted -eq $Expected)}
}
[IO.File]::WriteAllText($cache,'CMAKE_HOME_DIRECTORY:INTERNAL='+$sourceRoot+[Environment]::NewLine)
Check-PathCase 'external-build-same-source' {Resolve-ReleaseBuildDirectory $sourceRoot $externalBuild} $true
[IO.File]::WriteAllText($cache,'CMAKE_HOME_DIRECTORY:INTERNAL='+$EvidenceDir+[Environment]::NewLine)
Check-PathCase 'wrong-source-home' {Resolve-ReleaseBuildDirectory $sourceRoot $externalBuild} $false
[IO.File]::WriteAllText($cache,('CMAKE_HOME_DIRECTORY:INTERNAL='+$sourceRoot+[Environment]::NewLine)*2)
Check-PathCase 'ambiguous-duplicate-home' {Resolve-ReleaseBuildDirectory $sourceRoot $externalBuild} $false
[IO.File]::WriteAllText($cache,'CMAKE_HOME_DIRECTORY:INTERNAL='+$sourceRoot+[Environment]::NewLine)
$stage=Join-Path $externalBuild '_dist_stage';$child=Join-Path $stage 'owned-package';$outside=Join-Path $EvidenceDir 'outside'
New-Item -ItemType Directory -Path $child,$outside -Force|Out-Null
Check-PathCase 'strict-build-stage-child' {Resolve-ReleaseCleanupDirectory $externalBuild $child $stage} $true
Check-PathCase 'build-root-is-not-cleanup-child' {Resolve-ReleaseCleanupDirectory $externalBuild $externalBuild $externalBuild} $false
Check-PathCase 'external-cleanup-parent' {Resolve-ReleaseCleanupDirectory $externalBuild $outside $EvidenceDir} $false
Check-PathCase 'source-directory-not-cleanup-parent' {Resolve-ReleaseCleanupDirectory $externalBuild $sourceRoot $EvidenceDir} $false
$link=Join-Path $externalBuild 'junction';New-Item -ItemType Junction -Path $link -Target $outside|Out-Null
$linkChild=Join-Path $outside 'child';New-Item -ItemType Directory -Path $linkChild|Out-Null
Check-PathCase 'reparse-cleanup-ancestor' {Resolve-ReleaseCleanupDirectory $externalBuild (Join-Path $link 'child') $externalBuild} $false
Check-PathCase 'publication-child' {Assert-ReleasePublishChild (Join-Path $outside 'future.zip') $outside} $true
Check-PathCase 'publication-escape' {Assert-ReleasePublishChild (Join-Path $sourceRoot 'future.zip') $outside} $false
$results|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $EvidenceDir 'results.json') -Encoding UTF8
$results|Format-Table|Out-String|Write-Host
if(@($results|Where-Object{-not $_.correct}).Count){exit 1}
Write-Host ('PACKAGE_PATH_CONTROLS_PASS cases='+$results.Count+' recursive_deletions=0')
exit 0
