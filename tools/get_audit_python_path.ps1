# Project-local audit wheels. Never install or alter global Python packages.
function Resolve-AuditPythonPath {
 param([string]$BuildDir,[string]$Preferred='')
 if(-not $Preferred){
  $cache=Join-Path $BuildDir 'CMakeCache.txt'
  if(Test-Path -LiteralPath $cache){
   $entry=Select-String -LiteralPath $cache -Pattern '^RTA_CERTIFICATE_PYTHON_PATH:[^=]+=(.+)$'|Select-Object -First 1
   if($entry){$Preferred=$entry.Matches[0].Groups[1].Value}
  }
 }
 if(-not $Preferred){$Preferred=Join-Path $BuildDir 'audit-deps'}
 if(-not (Test-Path -LiteralPath $Preferred -PathType Container)){throw "Project-local audit dependency directory missing: $Preferred. Prepare tools/requirements-audit.txt in a private --target directory and pass -CertificatePythonPath; global packages are not installation evidence."}
 return (Resolve-Path -LiteralPath $Preferred -ErrorAction Stop).ProviderPath
}
