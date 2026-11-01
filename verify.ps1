param([switch]$NoUi)
# verify.ps1 - RLPlayer quick self-check (no audio device is opened)
# NOTE: keep this file UTF-8 WITH BOM. PowerShell 5.1 reads a BOM-less UTF-8 file as GBK
#       and fails to parse the Chinese comments ("Missing type name after '['").
$ErrorActionPreference='Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:Path = 'C:\Program Files\AMD\ROCm\7.2\bin;' + $env:Path
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$B = Join-Path $Root 'build'
$RunDir = Join-Path $B ('verify-run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
$rows = @()
function Step($n, $ok, $d) { $script:rows += [pscustomobject]@{ Item=$n; Result=$(if($ok){'PASS'}else{'FAIL'}); Detail=$d } }
# 2026-10-24：端口一律动态取（下面自检会起 rtaweb / rtanet_rx）。
# 为什么：固定 8907/8908/45902 时，上一次自检残留的进程会让新实例 bind 失败并悄悄退出，
# 自检于是报「FAIL」——实际被测对象是健康的（1.3.15 就这么误报过一次 DirectSDM e2e）。
function Get-FreePort { $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0); $l.Start(); $p = $l.LocalEndpoint.Port; $l.Stop(); return $p }
# Only fresh per-run directories are used; retained logs aid diagnosis.
function Start-TestProcess([string]$Exe, [string[]]$Argv, [string]$Cwd, [string]$Tag) {
  $quoted = foreach ($arg in $Argv) {
    if ($arg.Contains('"')) { throw 'Unexpected quote in controlled test argument' }
    '"' + ([regex]::Replace($arg, '(\\+)$', '$1$1')) + '"'
  }
  $stdout = Join-Path $Cwd ($Tag + '.stdout.log')
  $stderr = Join-Path $Cwd ($Tag + '.stderr.log')
  $proc = Start-Process -FilePath $Exe -ArgumentList ($quoted -join ' ') -WorkingDirectory $Cwd -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  # Windows PowerShell 5.1 must retain the native process handle before exit;
  # otherwise PassThru can report a null ExitCode after WaitForExit.
  $null=$proc.Handle
  return [pscustomobject]@{ Process=$proc; Stdout=$stdout; Stderr=$stderr }
}
function Wait-TestProcess($Child, [int]$Millis=20000) {
  if (-not $Child.Process.WaitForExit($Millis)) {
    return [pscustomobject]@{ Exit=-2; Text=('bounded wait expired; left running PID=' + $Child.Process.Id) }
  }
  $text = ((Get-Content -LiteralPath $Child.Stdout -Encoding UTF8 -Raw -ErrorAction SilentlyContinue), (Get-Content -LiteralPath $Child.Stderr -Encoding UTF8 -Raw -ErrorAction SilentlyContinue)) -join "`n"
  return [pscustomobject]@{ Exit=$Child.Process.ExitCode; Text=$text }
}
function Wait-TestListener($Child, [int]$Port) {
  for ($i=0; $i -lt 40; $i++) {
    if ($Child.Process.HasExited) { return $false }
    # A probe connection would consume rtanet_rx's sole accepted control client.
    $listener = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
    if ($listener -and ($listener.OwningProcess -contains $Child.Process.Id)) { return $true }
    Start-Sleep -Milliseconds 100
  }
  return $false
}
function Network-FileCheck([string]$Source, [string]$Dest, [string]$Tag) {
  $port = Get-FreePort
  $child = Start-TestProcess $rx @('--sink','file','--out',$Dest,'--ctrl-port',"$port",'--seconds','8') $RunDir $Tag
  $ready = Wait-TestListener $child $port
  $sendExit = -1
  if ($ready) { $sendText = (& $play $Source --net ("127.0.0.1:" + $port) 2>&1 | Out-String); $sendExit=$LASTEXITCODE }
  $received = Wait-TestProcess $child 20000
  $cmpExit=-1; $comparison='receiver output missing'
  if (Test-Path -LiteralPath $Dest) { $comparison=(& $cmp $Source $Dest 2>&1 | Out-String); $cmpExit=$LASTEXITCODE }
  $detail='ready=' + $ready + ', sender exit=' + $sendExit + ', receiver exit=' + $received.Exit + ', wavcmp exit=' + $cmpExit + ' (full frames; no --prefix)'
  $comparison | Out-File -LiteralPath (Join-Path $RunDir ($Tag + '.wavcmp.log')) -Encoding utf8
  if ($sendText) { $sendText | Out-File -LiteralPath (Join-Path $RunDir ($Tag + '.sender.log')) -Encoding utf8 }
  return [pscustomobject]@{ Ok=($ready -and $sendExit -eq 0 -and $received.Exit -eq 0 -and $cmpExit -eq 0); Detail=$detail }
}
Write-Host '=== RLPlayer self-check (silent: no audio device opened) ==='

# 1 build
# ★ 2026-10-29（两轮审查 F35）：判据改成**每条命令的实际退出码**。
#   旧实现：① cmake 配置的输出被 `$null =` 丢掉、也没判退出码 —— 配置失败会继续用
#   build/ 里的旧产物；② 构建是否成功靠正则匹配输出里的 "error:" 文本，
#   "命令非 0 退出但没有 error: 关键字"（链接器/资源编译器/工具缺失/磁盘满）会被判 PASS。
#   现在：先存输出（诊断用）再判 $LASTEXITCODE；配置失败直接 FAIL 且不再构建。
$cfgLog = Join-Path $B 'configure.log'
$cfgOut = (cmake -G Ninja -S $Root -B $B -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe' 2>&1 | Out-String)
$cfgExit = $LASTEXITCODE
New-Item -ItemType Directory -Force -Path $B | Out-Null
$cfgOut | Out-File -FilePath $cfgLog -Encoding utf8
$cfgTail = ((($cfgOut -split ([char]10)) | Where-Object { $_.Trim() -ne '' } | Select-Object -Last 1) -join ' ').Trim()
Step 'CMake configure' ($cfgExit -eq 0) ('exit=' + $cfgExit + ' ' + $cfgTail)
$bo = ''
if ($cfgExit -eq 0) {
  $bo = (cmake --build $B 2>&1 | Out-String)
  $bExit = $LASTEXITCODE
} else {
  $bExit = -1
}
$buildLog = Join-Path $B 'build.log'
$bo | Out-File -FilePath $buildLog -Encoding utf8
# 错误计数只用于报告，不再当判据
$errs = (($bo -split ([char]10)) | Select-String -Pattern 'error:|error generated|ninja: build stopped' | Measure-Object).Count
Step 'CMake build' ($bExit -eq 0) ('exit=' + $bExit + ', ' + $errs.ToString() + ' error lines')

# 2 test suite (judge by exit code; require a non-empty test set)
# ★ 2026-10-29（两轮审查 F36）：零测试 / 全部被过滤不许冒充通过。
#   --no-tests=error：一个用例都没跑（构建目录空、-E/-R 过滤掉全部）直接判失败。
$oldNoDevice = $env:RTA_NO_DEVICE
try {
  $env:RTA_NO_DEVICE='1'
  Push-Location $RunDir
  try { $ct = (ctest --test-dir $B --no-tests=error --output-on-failure 2>&1 | Out-String); $ctExit=$LASTEXITCODE }
  finally { Pop-Location }
} finally { $env:RTA_NO_DEVICE=$oldNoDevice }
$ct | Out-File -FilePath (Join-Path $B 'ctest.log') -Encoding utf8
$sum = ((($ct -split ([char]10)) | Select-String 'tests passed' | Select-Object -First 1) -join ' ').Trim()
if (-not $sum) { $sum = 'exit=' + $ctExit }
$ntest = 0
if ($sum -match 'out of (\d+)') { $ntest = [int]$Matches[1] }
Step ('Test suite (' + $ntest + ')') (($ctExit -eq 0) -and ($ntest -gt 0)) ($sum + ' (要求 exit=0 且用例数>0)')

Push-Location $RunDir
try {

# 3 GPU library (multi-arch fat binary: every AMD family the compiler can target)
$dll = Join-Path $B 'rtagpu.dll'
if (Test-Path 'C:\Program Files\AMD\ROCm\7.2\bin\hipcc.bat') {
  $gout = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tools\build_gpu.ps1') -Out $dll 2>&1 | Out-String)
  Step 'rtagpu.dll (multi-arch)' ((Test-Path $dll) -and ($LASTEXITCODE -eq 0)) ($gout.Trim())
} else { Step 'rtagpu.dll (multi-arch)' $false 'hipcc missing' }

# 4 GPU convolution correctness
# 用 test_gpu_bridge（由 CMake 构建、也进了 ctest）：它走的是真正的 GpuBridge 动态加载路径。
# 旧的 gpu_conv.exe 是手工编的独立 demo，不跟着源码更新，曾经掩盖过
# "gpu_bridge.h 没取符号、GPU 从未真正启用"这个 bug。
$gc = Join-Path $B 'test_gpu_bridge.exe'
if (Test-Path $gc) {
  $g = (& $gc 2>&1 | Out-String)
  $gpuConvExit=$LASTEXITCODE
  $okG = ($gpuConvExit -eq 0) -and ($g -match '\[PASS\]') -and ($g -notmatch '\[SKIP\]|\[FAIL\]')
  $perf = ((($g -split ([char]10)) | Select-String 'PASS|SKIP|相对' ) -join ' ').Trim()
  Step 'GPU conv == CPU' $okG $perf
} else { Step 'GPU conv == CPU' $false 'test_gpu_bridge.exe missing' }

# 5 UPnP parser self-test
$up = Join-Path $B 'upnp_scan.exe'
if (Test-Path $up) {
  $u = (& $up --selftest 2>&1 | Out-String)
  $upExit=$LASTEXITCODE
  Step 'UPnP parser self-test' (($upExit -eq 0) -and ($u -match 'selftest')) ('exit=' + $upExit)
} else { Step 'UPnP parser self-test' $false 'upnp_scan.exe missing' }

# 6 network end-to-end, full-frame bit equality (file sink only)
$play=Join-Path $B 'rtaplay.exe'; $rx=Join-Path $B 'rtanet_rx.exe'; $cmp=Join-Path $B 'wavcmp.exe'
$td=Join-Path $RunDir 'testdata'; New-Item -ItemType Directory -Force -Path $td | Out-Null
$src=Join-Path $td 'vfy_src.wav'; $dst=Join-Path $td 'vfy_rx.wav'
if ((Test-Path -LiteralPath $play) -and (Test-Path -LiteralPath $rx) -and (Test-Path -LiteralPath $cmp)) {
  $genText=(& $play --gen $src 3 44100 24 2>&1 | Out-String); $genExit=$LASTEXITCODE
  if ($genExit -eq 0) { $net=Network-FileCheck $src $dst 'pcm-network'; Step 'Network e2e bit-exact' $net.Ok $net.Detail }
  else { Step 'Network e2e bit-exact' $false ('generator exit=' + $genExit) }
} else { Step 'Network e2e bit-exact' $false 'exe missing' }

# 7 GPU resample kernel (vanilla-kernel sanity + CPU/GPU equality)
$grt = Join-Path $B 'gpu_resample_test.exe'
if (Test-Path $grt) {
  $g = (& $grt 2>&1 | Out-String)
  $gpuResampleExit=$LASTEXITCODE
  $okR = ($gpuResampleExit -eq 0) -and ($g -match '\[PASS\]') -and ($g -notmatch '\[SKIP\]|\[FAIL\]')
  $l = ((($g -split ([char]10)) | Select-String 'PASS|FAIL' | Select-Object -First 1) -join ' ').Trim()
  Step 'GPU resample kernel' $okR $l
} else { Step 'GPU resample kernel' $false 'gpu_resample_test.exe missing' }

# 8 UI self-check. Audit: uicheck.js has no /api/play call or play-button
# activation. It DOES mutate queue/settings/PEQ, so cwd alone is insufficient:
# rtaweb stores state beside its exe. Stage an isolated exe and library too.
$nodeCmd=Get-Command node -ErrorAction SilentlyContinue
$uiScript=Join-Path $Root 'tools\uicheck.js'
$uiDir=Join-Path $RunDir 'ui'; New-Item -ItemType Directory -Force -Path $uiDir | Out-Null
$uiMedia=Join-Path $uiDir 'media'; New-Item -ItemType Directory -Force -Path $uiMedia | Out-Null
$webExe=Join-Path $B 'rtaweb.exe'
if (Test-Path -LiteralPath $webExe) {
  Copy-Item -LiteralPath $webExe -Destination (Join-Path $uiDir 'rtaweb.exe')
  # OPRA and translations are production static resources, not mocked UI data.
  $uiFixtureOk=$true
  $staticData=Join-Path $Root 'data'
  if (Test-Path -LiteralPath $staticData) { Copy-Item -LiteralPath $staticData -Destination $uiDir -Recurse }
  else { $uiFixtureOk=$false }
  foreach($tool in @('ffmpeg.exe','ffprobe.exe')) {
    $toolSrc=Join-Path (Join-Path $B 'ffmpeg-lite') $tool
    if (Test-Path -LiteralPath $toolSrc) { Copy-Item -LiteralPath $toolSrc -Destination (Join-Path $uiDir $tool) }
    else { $uiFixtureOk=$false }
  }
  # Three distinct, tagged, digitally silent tracks exercise sort/favourites.
  # The RIFF seed stays outside the indexed media directory.
  $uiSeed=Join-Path $uiDir 'fixture-seed.wav'
  $writer=New-Object IO.BinaryWriter([IO.File]::Open($uiSeed,[IO.FileMode]::CreateNew))
  try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([uint32](36+176400))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([uint32]16)
    $writer.Write([uint16]1); $writer.Write([uint16]2); $writer.Write([uint32]44100)
    $writer.Write([uint32]176400); $writer.Write([uint16]4); $writer.Write([uint16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([uint32]176400)
    $writer.Write((New-Object byte[] 176400))
  } finally { $writer.Dispose() }
  $uiFfmpeg=Join-Path $uiDir 'ffmpeg.exe'
  if (Test-Path -LiteralPath $uiFfmpeg) {
    $fixtureNames=@('Alpha fixture','Bravo fixture','Charlie fixture')
    for($track=1;$track -le 3;$track++) {
      $fixtureOut=Join-Path $uiMedia ('0'+$track+'-fixture.wav')
      $fixtureText=(& $uiFfmpeg -hide_banner -loglevel error -y -i $uiSeed -metadata ('title='+$fixtureNames[$track-1]) -metadata 'artist=RLPlayer UI Fixture Artist' -metadata 'album=RLPlayer Offline UI Fixtures' -metadata ('track='+$track+'/3') -c:a pcm_s16le $fixtureOut 2>&1 | Out-String)
      $fixtureExit=$LASTEXITCODE
      if($fixtureExit -ne 0){$uiFixtureOk=$false}
      $fixtureText | Out-File -LiteralPath (Join-Path $uiDir ('fixture-'+$track+'.log')) -Encoding utf8
    }
  }
  Step 'Isolated UI assets/fixtures' $uiFixtureOk 'full static data + ffmpeg/ffprobe + 3 tagged digital-silence WAVs'
  # Production recent_add()/api/recent use four TAB-separated fields and no
  # timestamp column: path, title, artist, album. Seed existing silence files;
  # never start playback just to manufacture UI history.
  $recentFixture=@()
  for($track=1;$track -le 3;$track++) {
    $recentPath=Join-Path $uiMedia ('0'+$track+'-fixture.wav')
    if(Test-Path -LiteralPath $recentPath) {
      $recentAlbum=if($track -eq 2){'Long offline UI album fixture '+('with deliberately overflowing album text '*12)}else{'Offline UI album '+$track}
      $recentFixture+=(($recentPath,$fixtureNames[$track-1],'RLPlayer UI Fixture Artist',$recentAlbum) -join "`t")
    }
  }
  [IO.File]::WriteAllText((Join-Path $uiDir 'rlplayer-recent.txt'),($recentFixture -join "`n")+"`n",(New-Object Text.UTF8Encoding($false)))
  Step 'Isolated UI recent fixture' ($recentFixture.Count -eq 3) '3 existing digital-silence paths; real four-column recent format; includes long album name'
  [IO.File]::WriteAllText((Join-Path $uiDir 'rlplayer-library.conf'), $uiMedia + "`n", (New-Object Text.UTF8Encoding($false)))
  $uiPort=Get-FreePort; $oldLibDir=$env:RLPLAYER_LIBRARY_DIR; $oldIsolate=$env:RLPLAYER_ISOLATE
  try {
    $env:RLPLAYER_LIBRARY_DIR=$uiDir; $env:RLPLAYER_ISOLATE='0'
    $web=Start-TestProcess (Join-Path $uiDir 'rtaweb.exe') @('--port',"$uiPort") $uiDir 'rtaweb'
  } finally { $env:RLPLAYER_LIBRARY_DIR=$oldLibDir; $env:RLPLAYER_ISOLATE=$oldIsolate }
  try {
    $ready=Wait-TestListener $web $uiPort
    if ($ready) {
      try { $state=Invoke-WebRequest -Uri "http://127.0.0.1:$uiPort/api/state" -UseBasicParsing -TimeoutSec 5; Step 'Isolated Web API smoke' ($state.StatusCode -eq 200) 'read-only /api/state, no playback' }
      catch { Step 'Isolated Web API smoke' $false $_.Exception.Message }
      $catalogue=$null
      for($i=0;$i -lt 30;$i++) {
        try {$catalogue=(Invoke-WebRequest -Uri "http://127.0.0.1:$uiPort/api/lib/search?q=" -UseBasicParsing -TimeoutSec 5).Content | ConvertFrom-Json} catch {}
        if($catalogue -and @($catalogue.tracks).Count -ge 3){break}
        Start-Sleep -Milliseconds 200
      }
      $tagged=@($catalogue.tracks | Where-Object { $_.artist -eq 'RLPlayer UI Fixture Artist' -and $_.album -eq 'RLPlayer Offline UI Fixtures' })
      Step 'Isolated UI library tags' (@($catalogue.tracks).Count -eq 3 -and $tagged.Count -eq 3) ('actual scanned tracks='+@($catalogue.tracks).Count+', correct artist/album tags='+$tagged.Count)
      try {
        $history=(Invoke-WebRequest -Uri "http://127.0.0.1:$uiPort/api/recent" -UseBasicParsing -TimeoutSec 5).Content | ConvertFrom-Json
        Step 'Isolated UI recent API' (@($history.items).Count -eq 3) ('actual recent records='+@($history.items).Count+'; seeded without playback')
      } catch {Step 'Isolated UI recent API' $false $_.Exception.Message}
      if ($NoUi) { $script:rows += [pscustomobject]@{ Item='Web UI self-check'; Result='SKIP'; Detail='explicit -NoUi; UI not covered; read-only API checked separately' } }
      elseif ($nodeCmd -and (Test-Path -LiteralPath $uiScript)) {
        $uo=(& node $uiScript "http://127.0.0.1:$uiPort/" (Join-Path $uiDir 'shots') offline-fixtures 2>&1 | Out-String); $uiExit=$LASTEXITCODE
        $uo | Out-File -LiteralPath (Join-Path $uiDir 'uicheck.log') -Encoding utf8
        Step 'Web UI self-check' ($uiExit -eq 0) ('exit=' + $uiExit + '; real local APIs/static data; external knowledge/Discogs responses use explicit offline fixtures; see ' + (Join-Path $uiDir 'uicheck.log'))
      } else { Step 'Web UI self-check' $false 'node or uicheck.js missing' }
    } else { Step 'Isolated Web API smoke' $false 'test rtaweb listener did not start' }
  } finally {
    if (-not $web.Process.HasExited) {
      $ownedListener=Get-NetTCPConnection -LocalPort $uiPort -State Listen -ErrorAction SilentlyContinue
      if ($ownedListener -and ($ownedListener.OwningProcess -contains $web.Process.Id)) {
        try { $null=Invoke-WebRequest -Uri "http://127.0.0.1:$uiPort/api/quit" -Method Post -UseBasicParsing -TimeoutSec 5 } catch {}
      }
    }
    $quit=Wait-TestProcess $web 15000
    Step 'Test rtaweb graceful exit' ($quit.Exit -eq 0) ('exit=' + $quit.Exit + ' ' + $(if($quit.Exit -eq -2){$quit.Text}else{'/api/quit; no forced termination'}))
  }
} else { Step 'Web UI self-check' $false 'rtaweb.exe missing' }

# 9 DSD domain: material generation -> DirectSDM bit-exact -> integrator THD+N
$mk = Join-Path $B 'make_dsd.exe'; $dc = Join-Path $B 'dsdconv.exe'
if ((Test-Path $mk) -and (Test-Path $dc)) {
  $td = Join-Path $RunDir 'testdata'; New-Item -ItemType Directory -Force -Path $td | Out-Null
  $dsf = Join-Path $td 'vfy_dsd64.dsf'; $dop = Join-Path $td 'vfy_dsd64_dop.wav'
  $mo = (& $mk --out $dsf --seconds 1 --amp -6 --verify 2>&1 | Out-String)
  $mkExit=$LASTEXITCODE
  $do = (& $dc --in $dsf --direct --out $dop 2>&1 | Out-String)
  $dcExit = $LASTEXITCODE
  $bitOk = $do -match 'DIRECT_SDM_BITEXACT=1'
  $thd = 0.0
  if ($mo -match 'THD\+N[^=]*=\s*([0-9.]+) dB') { $thd = [double]$Matches[1] }
  Step 'DSD path (DirectSDM + THD+N)' (($mkExit -eq 0) -and ($dcExit -eq 0) -and $bitOk -and ($thd -ge 80.0)) ('generator exit=' + $mkExit + ', converter exit=' + $dcExit + ', DirectSDM bit-exact=' + $bitOk + ', THD+N ' + $thd + ' dB')
} else { Step 'DSD path (DirectSDM + THD+N)' $false 'make_dsd/dsdconv missing' }

# 10 DirectSDM TOOL -> DoP WAV -> rtaplay --net -> RTANet file sink.
# This does not exercise /api/play or the Web playback thread. Full wavcmp is
# mandatory: --prefix could hide an appended silence/extra-frame defect.
if ((Test-Path -LiteralPath $mk) -and (Test-Path -LiteralPath $dc) -and (Test-Path -LiteralPath $play) -and (Test-Path -LiteralPath $rx) -and (Test-Path -LiteralPath $cmp)) {
  $dsf2=Join-Path $td 'vfy_dm.dsf'; $refW=Join-Path $td 'vfy_dm_ref.wav'; $recvW=Join-Path $td 'vfy_dm_recv.wav'
  $mo2=(& $mk --out $dsf2 --seconds 1 --amp -60 2>&1 | Out-String); $mk2Exit=$LASTEXITCODE
  $do2=(& $dc --in $dsf2 --direct --out $refW 2>&1 | Out-String); $dc2Exit=$LASTEXITCODE
  if ($mk2Exit -eq 0 -and $dc2Exit -eq 0 -and $do2 -match 'DIRECT_SDM_BITEXACT=1') {
    $net=Network-FileCheck $refW $recvW 'directsdm-network'
    Step 'DirectSDM tool/RTANet full-frame bit-exact' $net.Ok $net.Detail
  } else { Step 'DirectSDM tool/RTANet full-frame bit-exact' $false ('generator exit=' + $mk2Exit + ', converter exit=' + $dc2Exit) }
} else { Step 'DirectSDM tool/RTANet full-frame bit-exact' $false 'exe missing' }

# 11 distribution compatibility: static CRT, subsystem <= 6.1, no AVX-512
$cc = Join-Path $Root 'tools\check_compat.ps1'
if (Test-Path $cc) {
  $co = (& powershell -NoProfile -ExecutionPolicy Bypass -File $cc -Dir $B 2>&1 | Out-String)
  $ccExit = $LASTEXITCODE
  $cl = ((($co -split ([char]10)) | Select-String 'FAIL count' | Select-Object -First 1) -join ' ').Trim()
  Step 'Distribution compatibility' ($ccExit -eq 0) $cl
} else { Step 'Distribution compatibility' $false 'check_compat.ps1 missing' }

} finally { Pop-Location }

Write-Host ''
$rows | Format-Table -AutoSize | Out-String | Write-Host
$f = ($rows | Where-Object { $_.Result -eq 'FAIL' } | Measure-Object).Count
Write-Host ('FAIL count: ' + $f)
Write-Host 'NOTE: no audio device opened; RTA_NO_DEVICE=1 for ctest; DirectSDM check covers CLI/tools, not Web playback.'
Write-Host ('Run evidence: ' + $RunDir)
exit $f
