# Process-local module scope; never writes user or machine configuration.
function Enter-HostPowerShellModules {
 $previous=$env:PSModulePath
 try {
  $env:PSModulePath=Join-Path $PSHOME 'Modules'
  Import-Module (Join-Path $env:PSModulePath 'Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
  return [pscustomobject]@{Previous=$previous}
 } catch {$env:PSModulePath=$previous;throw}
}
function Exit-HostPowerShellModules($State) {$env:PSModulePath=$State.Previous}
