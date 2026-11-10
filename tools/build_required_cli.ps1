# Compile into a new file: a failed/no-output compiler cannot reuse an old EXE.
function Build-RequiredCli {
 param([string]$Compiler,[string]$Source,[string]$Output,[string[]]$Options=@())
 if(-not (Test-Path -LiteralPath $Compiler -PathType Leaf)){throw "Required compiler missing: $Compiler"}
 if(-not (Test-Path -LiteralPath $Source -PathType Leaf)){throw "Required source missing: $Source"}
 $target=[IO.Path]::GetFullPath($Output)
 $temporary=Join-Path (Split-Path -Parent $target) ([IO.Path]::GetFileNameWithoutExtension($target)+'.new-'+[guid]::NewGuid().ToString('N')+'.exe')
 $compileArgs=@($Options)+@($Source,'-o',$temporary)
 $prior=$ErrorActionPreference
 try{
  $ErrorActionPreference='Continue'
  & $Compiler @compileArgs
  $compileExit=$LASTEXITCODE
 }finally{$ErrorActionPreference=$prior}
 if($compileExit -ne 0){throw "Required CLI compilation failed: exit=$compileExit, source=$Source; old output was not accepted"}
 if(-not (Test-Path -LiteralPath $temporary -PathType Leaf) -or (Get-Item -LiteralPath $temporary).Length -le 0){throw "Compiler returned success without a fresh nonempty executable: $Source"}
 Move-Item -LiteralPath $temporary -Destination $target -Force
 $hashes=@()
 foreach($path in @($Source,$target)){
  $stream=[IO.File]::OpenRead($path);$algorithm=[Security.Cryptography.SHA256]::Create()
  try{$hashes+=([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','')}finally{$stream.Dispose();$algorithm.Dispose()}
 }
 Write-Host ('CLI_BUILD_PASS source_sha256='+$hashes[0]+' output_sha256='+$hashes[1])
}
