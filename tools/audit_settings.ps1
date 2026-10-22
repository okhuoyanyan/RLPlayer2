
$ErrorActionPreference='Continue'
$base='http://127.0.0.1:8910'
$file='D:\RTA-hqplayer\build\testdata\t1.wav'
$pass=0; $fail=0
function T($name, $query, $field, $expect) {
  $body='file=' + [uri]::EscapeDataString($file) + '&backend=null&' + $query
  try { $null=Invoke-WebRequest -Uri ($base + '/api/play') -Method POST -Body $body -UseBasicParsing -TimeoutSec 8 } catch { }
  Start-Sleep -Milliseconds 700
  $s = (Invoke-WebRequest -Uri ($base + '/api/signal') -UseBasicParsing -TimeoutSec 8).Content | ConvertFrom-Json
  $v = '(no signal)'
  if ($s.ok) { $v = $s.path.$field }
  $ok = $false
  if ($v -ne $null) {
    if ($v.ToString() -eq $expect) { $ok = $true }
    elseif ($expect -ne '' -and $v.ToString().Contains($expect)) { $ok = $true }
  }
  if ($ok) { $script:pass++; Write-Output ('[PASS] ' + $name + '  ' + $field + '=' + $v) }
  else { $script:fail++; Write-Output ('[FAIL] ' + $name + '  ' + $field + '=' + $v + '  (expect ' + $expect + ')') }
  $null=Invoke-WebRequest -Uri ($base + '/api/stop') -UseBasicParsing -TimeoutSec 8
  Start-Sleep -Milliseconds 150
}
T 'rate 96000'      'rate=96000'                      'outRate'  '96000'
T 'rate src'        'rate=src'                        'outRate'  '44100'
T 'bits 16'         'bits=16'                         'outBits'  '16'
T 'bits 32'         'bits=32'                         'outBits'  '32'
T 'filter short-lp' 'filter=poly-sinc-short-lp'       'filter'   'poly-sinc-short-lp'
T 'taps follow'     'filter=poly-sinc-short-lp'       'taps'     '4095'
T 'phase linear'    'phase=linear'                    'phaseCode' 'linear'
T 'phase mixed'     'phase=intermediate'              'phaseCode' 'intermediate'
T 'taps override'   'taps=8192&filter=poly-sinc-lp'   'taps'     '8192'
T 'twostage off'    'twostage=0'                      'twoStage' 'False'
T 'channels 6'      'channels=6'                      'outCh'    '6'
T 'DSD64 DoP'       'dsd=64'                          'dsdRate'  '2822400'
T 'SDM filter'      'dsd=64&sdmfilt=poly-sinc-short-mp' 'filter' 'poly-sinc-short-mp'
T 'loudness on'     'loudness=1&volume=-30'           'loudness' 'True'
T 'gpu off'         'gpuvendor=off'                   'gpuAvailable' 'False'
T 'isp off'         'isp=0'                           'ispPeak'  '0'
Write-Output ''
Write-Output ('PASS=' + $pass + '  FAIL=' + $fail)
