# verify_scan_261025.ps1 -- 261025-01 一.1/2/3/5/8 + 外部评审 P0#1 + OGG stream_tags 的判据（真实 HTTP 端到端）
#
# 判据（铁律 3：每条都要能证明"我要证明的事"，并配负向对照）
#   S1 中文/日文路径：根目录**本身**含中文的全角/日文字符；/api/lib/roots 加根 + /api/lib/scan 后必须入库、
#      专辑可见、/api/files 能列目录、重启后索引仍在（宽字符读写）。含 OGG(stream_tags) 标签回退。
#      负向对照 = 同夹具里的 ASCII 专辑（修前也应通过 ⟹ 夹具本身有效）。
#   S2 大库：3000 个 1 秒静音 flac（300 目录 x 10 首）真实扫描；扫描期间每约 200ms 打一次 status 并计时，
#      要求 max < 50ms；给出整库扫描耗时与每首摊薄。负向对照 = 扫描结束后的同端点延迟（空闲基线）。
#   S3 删除/清理：删掉 10 个文件后 rescan，索引里这 10 行必须消失；/api/lib/prune 与 /api/lib/remove 各自生效。
#   S4 切标签不重扫：扫描完成后连续 6 次视图切换，/api/dbg 里不得出现新的 LIBSCAN 记录，每次 < 100ms。
#      正向对照 = 手动 /api/lib/scan 必须让 LIBSCAN 计数 +1（否则"没有新记录"是空判据）。
#
# 纪律：全程不播放（不调用任何 play 端点）=> 不打开音频设备；只结束自己启动的那个 PID。
param(
  [string]$Exe  = 'D:\RTA-scan-wt\build-scan\rtaweb.exe',
  [string]$Base = 'D:\RTA-scan-wt\_t\run',
  [int]$Port    = 8941,
  [int]$Files   = 3000,
  [int]$PerDir  = 10,
  [switch]$SkipZh,
  [switch]$SkipBig
)
$ErrorActionPreference = 'Stop'
$CRLF = [string][char]13 + [string][char]10
$NL   = [string][char]10
$script:fails = 0
$script:rows  = @()

function Say([string]$s) { Write-Host $s }
function Ck([bool]$cond, [string]$what, [string]$detail) {
  $tail = if ($detail) { "  (" + $detail + ")" } else { "" }
  if ($cond) { Say ("  [PASS] " + $what + $tail) } else { Say ("  [FAIL] " + $what + $tail); $script:fails++ }
}
function Row([string]$k, [string]$v) { $script:rows += [pscustomobject]@{ k = $k; v = $v }; Say ("  . " + $k + " = " + $v) }

# ---------------- HTTP（原始 socket，客户端开销最小） ----------------
function Http([int]$p, [string]$path, [string]$method = 'GET', [string]$body = '') {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $c = New-Object System.Net.Sockets.TcpClient
  $c.ReceiveTimeout = 120000
  try { $c.Connect('127.0.0.1', $p) } catch { return [pscustomobject]@{ ms = -1; body = '<CONN-FAIL>' } }
  $ns = $c.GetStream()
  $blen = [Text.Encoding]::UTF8.GetByteCount($body)
  $req = $method + ' ' + $path + ' HTTP/1.1' + $CRLF + 'Host: 127.0.0.1' + $CRLF + 'Connection: close' + $CRLF +
         'Content-Length: ' + $blen + $CRLF + 'Content-Type: application/x-www-form-urlencoded' + $CRLF + $CRLF + $body
  $bytes = [Text.Encoding]::UTF8.GetBytes($req)
  $ns.Write($bytes, 0, $bytes.Length); $ns.Flush()
  $ms = New-Object IO.MemoryStream
  $buf = New-Object byte[] 131072
  try { while (($n = $ns.Read($buf, 0, $buf.Length)) -gt 0) { $ms.Write($buf, 0, $n) } } catch { }
  $c.Close()
  $sw.Stop()
  $all = [Text.Encoding]::UTF8.GetString($ms.ToArray())
  $i = $all.IndexOf($CRLF + $CRLF)
  $b = if ($i -ge 0) { $all.Substring($i + 4) } else { '' }
  return [pscustomobject]@{ ms = $sw.Elapsed.TotalMilliseconds; body = $b }
}
function J([int]$p, [string]$path, [string]$method = 'GET', [string]$body = '') {
  $r = Http $p $path $method $body
  try { return ($r.body | ConvertFrom-Json) } catch { return $null }
}
function Enc([string]$s) { return [uri]::EscapeDataString($s) }

function Start-Srv([int]$p, [string]$libDir) {
  New-Item -ItemType Directory -Path $libDir -Force | Out-Null
  $env:RLPLAYER_LIBRARY_DIR = $libDir
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Exe
  $psi.Arguments = '--port ' + $p
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  $script:proc = [System.Diagnostics.Process]::Start($psi)
  for ($k = 0; $k -lt 150; $k++) {
    Start-Sleep -Milliseconds 100
    if ($script:proc.HasExited) { throw ('服务器启动即退出: code=0x' + $script:proc.ExitCode.ToString('X8')) }
    try { $j = J $p '/api/lib/stats'; if ($j -and $j.ok) { return $script:proc } } catch { }
  }
  throw ('服务器未起来: port=' + $p)
}
function Stop-Srv($proc) { if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null } }
function WriteConf([string]$libDir, [int]$p, [string[]]$roots) {
  New-Item -ItemType Directory -Path $libDir -Force | Out-Null
  $body = '# fixture' + $CRLF
  foreach ($r in $roots) { $body += $r + $CRLF }
  [IO.File]::WriteAllText((Join-Path $libDir ('rlplayer-library.conf.p' + $p)), $body, (New-Object Text.UTF8Encoding($false)))
}
function Wait-Scan([int]$p, [int]$timeoutSec = 600) {
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt $timeoutSec) {
    $j = J $p '/api/lib/stats'
    if ($j -and -not $j.scanning) { return ((Get-Date) - $t0).TotalSeconds }
    Start-Sleep -Milliseconds 120
  }
  return -1
}
function NewFlac([string]$path) { [IO.File]::WriteAllBytes($path, [byte[]](0x66, 0x4C, 0x61, 0x43)) }
function DbgScanCount([int]$p) { return ([regex]::Matches((Http $p '/api/dbg').body, 'LIBSCAN')).Count }
function lib_roots_add_and_scan([int]$p, [string]$dir) {
  J $p '/api/lib/roots' 'POST' ('add=' + (Enc $dir)) | Out-Null
  J $p '/api/lib/scan' 'POST' '' | Out-Null
  Wait-Scan $p 300 | Out-Null
  return [int](J $p '/api/lib/stats').tracks
}

$sw0 = [Diagnostics.Stopwatch]::StartNew()
Remove-Item -LiteralPath $Base -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Base -Force | Out-Null
Say ('== verify_scan_261025  exe=' + $Exe)
Say ('   base=' + $Base + '  files=' + $Files)

# =========================== S1 中文/日文路径 ===========================
if (-not $SkipZh) {
Say ($NL + '--- S1 中文/日文根目录 + OGG(stream_tags) 标签 ---')
$lib1 = Join-Path $Base 'lib1'
$m1   = Join-Path $Base '中文音乐库'
New-Item -ItemType Directory -Path (Join-Path $m1 '专辑甲') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $m1 '日语-アルバム') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $m1 'ascii-album') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $m1 'ogg-专辑') -Force | Out-Null
NewFlac (Join-Path $m1 '专辑甲\第一首.flac')
NewFlac (Join-Path $m1 '专辑甲\第二首（全角括号）.flac')
NewFlac (Join-Path $m1 '日语-アルバム\曲名-A.flac')
NewFlac (Join-Path $m1 'ascii-album\plain.flac')
# OGG：标签只存在于 Vorbis Comment（stream_tags）—— 判据是"标签值生效，而不是退化成文件名"
$oggPath = Join-Path $m1 'ogg-专辑\天鸟.ogg'
& ffmpeg -y -v error -f lavfi -i 'anullsrc=r=44100:cl=stereo' -t 1 -metadata title='天鸟（Cover：奥華子）' -metadata artist='夏微' -metadata album='翻唱集' $oggPath 2>&1 | Out-Null
WriteConf $lib1 $Port @($m1)
$proc = Start-Srv $Port $lib1
try {
  $r = J $Port '/api/lib/roots' 'POST' ('add=' + (Enc $m1))
  Ck ([bool]$r.ok) 'addRoot 一个"根目录本身含中文/日文"的路径' ('ok=' + $r.ok + ' err=' + $r.error)
  J $Port '/api/lib/scan?full=1' 'POST' 'full=1' | Out-Null
  $sec = Wait-Scan $Port
  $st = J $Port '/api/lib/stats'
  Row 'S1 扫描耗时(s)' ([math]::Round($sec, 2))
  Row 'S1 入库曲目数（期望 5）' ([string]$st.tracks)
  Ck ([int]$st.tracks -eq 5) '中文根下 5 个文件（4 flac + 1 ogg）全部入库' ('实际 ' + $st.tracks)
  $al = J $Port '/api/lib/albums'
  $names = @(); foreach ($a in @($al.albums)) { $names += $a.name }
  Row 'S1 专辑名' ($names -join ' | ')
  Ck ($names -contains '专辑甲') '专辑列表含 专辑甲' ($names -join ',')
  Ck ($names -contains '日语-アルバム') '专辑列表含 日语-アルバム' ($names -join ',')
  Ck ($names -contains 'ascii-album') '负向对照：ASCII 专辑也在（夹具有效）' ($names -join ',')
  Ck ($names -contains '翻唱集') 'OGG 的 album 取自 stream_tags（翻唱集）而不是目录名 ogg-专辑' ($names -join ',')
  # OGG 的 title/artist 必须来自 stream_tags
  $srch = J $Port ('/api/lib/search?q=' + (Enc '天鸟'))
  $oggHit = $null
  foreach ($t in @($srch.tracks)) { if ($t.path -like '*天鸟.ogg') { $oggHit = $t } }
  Ck ($null -ne $oggHit) '搜索能命中 OGG（title 不是文件名兜底）' ('hits=' + @($srch.tracks).Count)
  if ($oggHit) {
    Row 'S1 OGG title/artist/album' ($oggHit.title + ' / ' + $oggHit.artist + ' / ' + $oggHit.album)
    Ck ($oggHit.title -eq '天鸟（Cover：奥華子）' -and $oggHit.artist -eq '夏微') 'OGG 的 title/artist 与 ffprobe stream_tags 一致' ($oggHit.title + ' | ' + $oggHit.artist)
  }
  # 索引落盘（宽字符写）
  $idxFile = Get-ChildItem $lib1 -Filter 'rlplayer-index.tsv*' | Select-Object -First 1
  $hasZh = $false
  if ($idxFile) { $hasZh = ([IO.File]::ReadAllText($idxFile.FullName, [Text.Encoding]::UTF8)).Contains('专辑甲') }
  Ck $hasZh '索引 TSV 里落盘了中文路径（不是空索引）'
  # 文件夹视图：同一族窄串 bug（用户反馈"文件夹页 Failed to fetch"）
  $fv = J $Port ('/api/files?dir=' + (Enc (Join-Path $m1 '专辑甲')))
  Ck ([bool]$fv.ok -and @($fv.files).Count -eq 2) '/api/files 能列出中文目录内容' ('ok=' + $fv.ok + ' files=' + @($fv.files).Count + ' err=' + $fv.error)
}
finally { Stop-Srv $proc }
# 重启往返：conf/index 在中文数据目录里、根目录也是中文 —— 修前这里会直接崩（0xC0000409）
$lib1b = Join-Path $Base '数据库中文'
WriteConf $lib1b $Port @($m1)
$proc = Start-Srv $Port $lib1b
try {
  $st = J $Port '/api/lib/stats'
  Row 'S1 重启后曲目数（期望 5）' ([string]$st.tracks)
  Ck ([int]$st.tracks -eq 0 -or [int]$st.tracks -eq 5) '重启不崩（修前：中文根 ⟹ 扫描线程抛异常 ⟹ 进程 0xC0000409）' ('tracks=' + $st.tracks)
  J $Port '/api/lib/scan?full=1' 'POST' 'full=1' | Out-Null
  Wait-Scan $Port | Out-Null
  $st2 = J $Port '/api/lib/stats'
  Row 'S1 中文数据目录 + 中文根：重扫后曲目数' ([string]$st2.tracks)
  Ck ([int]$st2.tracks -eq 5) '数据目录也含中文时 conf/index 仍能正常读写' ('tracks=' + $st2.tracks)
  $idx2 = Get-ChildItem $lib1b -Filter 'rlplayer-index.tsv*' -ErrorAction SilentlyContinue | Select-Object -First 1
  Ck ($null -ne $idx2) '索引写进了含中文的数据目录'
}
finally { Stop-Srv $proc }
}

# =========================== S2 大库扫描 ===========================
if (-not $SkipBig) {
Say ($NL + '--- S2 3000 首大库扫描 + 扫描期间 status 延迟 ---')
$lib2 = Join-Path $Base 'lib2'
$m2   = Join-Path $Base 'Music2'
New-Item -ItemType Directory -Path $m2 -Force | Out-Null
$srcFlac = Join-Path $Base 'one_second_silence.flac'
& ffmpeg -y -v error -f lavfi -i 'anullsrc=r=44100:cl=stereo' -t 1 -c:a flac $srcFlac 2>&1 | Out-Null
if (-not (Test-Path $srcFlac)) { throw 'ffmpeg 未能生成 1 秒静音 flac' }
$dirCount = [int]($Files / $PerDir)
$tgen = [Diagnostics.Stopwatch]::StartNew()
for ($d = 0; $d -lt $dirCount; $d++) {
  $dd = Join-Path $m2 ('Album{0:D4}' -f $d)
  New-Item -ItemType Directory -Path $dd -Force | Out-Null
  for ($t = 0; $t -lt $PerDir; $t++) { [IO.File]::Copy($srcFlac, (Join-Path $dd ('track{0:D2}.flac' -f $t))) }
}
$tgen.Stop()
Row 'S2 夹具生成(s)' ([math]::Round($tgen.Elapsed.TotalSeconds, 1))
WriteConf $lib2 $Port @($m2)
$proc = Start-Srv $Port $lib2
try {
  $statusLat = New-Object System.Collections.ArrayList
  $statsLat  = New-Object System.Collections.ArrayList
  $hasStatus = $true
  $sawScanning = $false
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt 900) {
    $r1 = Http $Port '/api/lib/status'
    $j1 = $null; try { $j1 = $r1.body | ConvertFrom-Json } catch { }
    if ($j1 -and $j1.ok) { [void]$statusLat.Add($r1.ms) } else { $hasStatus = $false }
    $r2 = Http $Port '/api/lib/stats'
    $j2 = $null; try { $j2 = $r2.body | ConvertFrom-Json } catch { }
    if ($j2) {
      [void]$statsLat.Add($r2.ms)
      if ($j2.scanning) { $sawScanning = $true }
      elseif ($sawScanning) { break }
    }
    Start-Sleep -Milliseconds 200
  }
  $scanSec = ((Get-Date) - $t0).TotalSeconds
  $st = J $Port '/api/lib/stats'
  Row 'S2 端点 /api/lib/status 存在' ([string]$hasStatus)
  Row 'S2 扫描总耗时(s)（遍历+ffprobe）' ([math]::Round($scanSec, 1))
  Row 'S2 服务端自报 lastScanMs' ([string]$st.lastScanMs)
  Row 'S2 每首摊薄(ms/首)' ([math]::Round(($st.lastScanMs / [math]::Max(1, [int]$st.total)), 1))
  Row 'S2 入库曲目数' ([string]$st.tracks)
  Row 'S2 probed/cached' ([string]$st.probed + '/' + [string]$st.cached)
  Ck ([int]$st.tracks -eq $Files) '3000 首全部入库' ('实际 ' + $st.tracks)
  if ($statusLat.Count -gt 0) {
    $mx  = ($statusLat | Measure-Object -Maximum).Maximum
    $avg = ($statusLat | Measure-Object -Average).Average
    Row 'S2 /api/lib/status 采样数' ([string]$statusLat.Count)
    Row 'S2 /api/lib/status 最大(ms)' ([math]::Round($mx, 1))
    Row 'S2 /api/lib/status 平均(ms)' ([math]::Round($avg, 1))
    $sorted = $statusLat | Sort-Object
    $p95 = $sorted[[int][math]::Floor(0.95 * ($sorted.Count - 1))]
    $p99 = $sorted[[int][math]::Floor(0.99 * ($sorted.Count - 1))]
    $median = $sorted[[int][math]::Floor(0.50 * ($sorted.Count - 1))]
    Row 'S2 /api/lib/status p50/p95/p99(ms)' ([math]::Round($median, 1).ToString() + ' / ' + [math]::Round($p95, 1).ToString() + ' / ' + [math]::Round($p99, 1).ToString())
    Ck ($p95 -lt 50) '扫描期间 /api/lib/status p95 < 50ms（p 分位排除个别进程创建抖动）' ([math]::Round($p95, 1))
    Ck ($mx -lt 50) '扫描期间 /api/lib/status 最大响应 < 50ms' ([math]::Round($mx, 1))
  }
  $mxs  = ($statsLat | Measure-Object -Maximum).Maximum
  $avgs = ($statsLat | Measure-Object -Average).Average
  Row 'S2 /api/lib/stats 最大(ms)' ([math]::Round($mxs, 1))
  Row 'S2 /api/lib/stats 平均(ms)' ([math]::Round($avgs, 1))
  # ---- S2b 负向对照（判据分离）：缓存重扫（不创建 ffprobe 进程）期间的同端点延迟 ----
  # 为什么要这一条：全量扫描期间 status 的 max 偶尔会冒出 100ms+，那是"8 个 ffprobe 进程同时创建"
  # 把单线程 accept 线程挤出 CPU 的调度抖动，不是锁竞争。缓存重扫不创建任何进程，
  # 若它的 max 回到个位数，就能证明抖动来源是进程创建而不是 Library 的锁。
  $statusLat2 = New-Object System.Collections.ArrayList
  J $Port '/api/lib/scan' 'POST' '' | Out-Null
  $t1 = Get-Date
  $saw2 = $false
  while (((Get-Date) - $t1).TotalSeconds -lt 300) {
    $r3 = Http $Port '/api/lib/status'
    $j3 = $null; try { $j3 = $r3.body | ConvertFrom-Json } catch { }
    if ($j3) {
      if ($j3.ok) { [void]$statusLat2.Add($r3.ms) }
      if ($j3.scanning) { $saw2 = $true } elseif ($saw2) { break }
    }
    Start-Sleep -Milliseconds 50
  }
  if ($statusLat2.Count -gt 0) {
    $mx2 = ($statusLat2 | Measure-Object -Maximum).Maximum
    Row 'S2b 缓存重扫耗时(s)' ([math]::Round(((Get-Date) - $t1).TotalSeconds, 1))
    Row 'S2b 缓存重扫期间 /api/lib/status 最大(ms)' ([math]::Round($mx2, 1))
    Ck ($mx2 -lt 50) '缓存重扫（无进程创建）期间 status 最大 < 50ms（分离调度抖动）' ([math]::Round($mx2, 1))
  }
  $idle = Http $Port '/api/lib/status'
  Row 'S2 负向对照：空闲时 /api/lib/status (ms)' ([math]::Round($idle.ms, 1))

  # =========================== S4 切标签不重扫 ===========================
  Say ($NL + '--- S4 切标签（6 次视图切换）不重扫 ---')
  $c0 = DbgScanCount $Port
  Row 'S4 切换前 /api/dbg 里 LIBSCAN 行数' ([string]$c0)
  $views = @(
    @{ n = 'albums';  p = '/api/lib/albums' },
    @{ n = 'artists'; p = '/api/lib/albums' },
    @{ n = 'tracks';  p = '/api/lib/search?q=' },
    @{ n = 'files';   p = ('/api/files?dir=' + (Enc $m2)) },
    @{ n = 'albums2'; p = '/api/lib/albums' },
    @{ n = 'tracks2'; p = '/api/lib/search?q=' }
  )
  $switchMs = @()
  foreach ($v in $views) {
    $rr = Http $Port $v.p
    $switchMs += $rr.ms
    Say ('   切到 ' + $v.n.PadRight(8) + ' ' + [math]::Round($rr.ms, 1) + ' ms')
  }
  $c1 = DbgScanCount $Port
  $mxSw = ($switchMs | Measure-Object -Maximum).Maximum
  Row 'S4 6 次切换最大耗时(ms)' ([math]::Round($mxSw, 1))
  Row 'S4 切换后 LIBSCAN 行数' ([string]$c1)
  Ck (($c1 - $c0) -eq 0) '切标签没有触发任何新的扫描（LIBSCAN 行数不变）' ('before=' + $c0 + ' after=' + $c1)
  Ck ($mxSw -lt 100) '每次切标签 < 100ms' ([math]::Round($mxSw, 1))
  J $Port '/api/lib/scan' 'POST' '' | Out-Null
  Start-Sleep -Milliseconds 400
  $c2 = DbgScanCount $Port
  Ck (($c2 - $c1) -ge 1) '正向对照：手动 /api/lib/scan 让 LIBSCAN 计数增加' ('before=' + $c1 + ' after=' + $c2)
  Wait-Scan $Port 300 | Out-Null

  # =========================== S3 删除/清理 ===========================
  Say ($NL + '--- S3 删除记录（rescan / prune / remove）---')
  $before = [int](J $Port '/api/lib/stats').tracks
  for ($k = 0; $k -lt 10; $k++) { Remove-Item -LiteralPath (Join-Path $m2 ('Album0000\track{0:D2}.flac' -f $k)) -Force }
  Row 'S3 删除前曲目数' ([string]$before)
  J $Port '/api/lib/scan' 'POST' '' | Out-Null
  Wait-Scan $Port 300 | Out-Null
  $afterScan = [int](J $Port '/api/lib/stats').tracks
  Row 'S3 rescan 后曲目数' ([string]$afterScan)
  Ck ($afterScan -eq ($before - 10)) '删掉 10 个文件后 rescan，索引里这 10 行消失' ('before=' + $before + ' after=' + $afterScan)
  for ($k = 0; $k -lt 10; $k++) { Remove-Item -LiteralPath (Join-Path $m2 ('Album0001\track{0:D2}.flac' -f $k)) -Force }
  $pr = J $Port '/api/lib/prune' 'POST' ''
  $afterPrune = [int](J $Port '/api/lib/stats').tracks
  Row 'S3 /api/lib/prune 返回' (($pr | ConvertTo-Json -Compress))
  Ck ([bool]$pr.ok -and [int]$pr.removed -eq 10 -and $afterPrune -eq ($afterScan - 10)) 'POST /api/lib/prune 清掉 10 条已不存在文件的记录' ('removed=' + $pr.removed + ' tracks=' + $afterPrune)
  $rm = J $Port ('/api/lib/remove?path=' + (Enc (Join-Path $m2 'Album0002'))) 'POST' ''
  $afterRm = [int](J $Port '/api/lib/stats').tracks
  Row 'S3 /api/lib/remove 返回' (($rm | ConvertTo-Json -Compress))
  Ck ([bool]$rm.ok -and [int]$rm.removed -eq 10 -and $afterRm -eq ($afterPrune - 10)) 'POST /api/lib/remove?path=<目录> 移除该目录下 10 条记录' ('removed=' + $rm.removed + ' tracks=' + $afterRm)
  $idxFile2 = Get-ChildItem $lib2 -Filter 'rlplayer-index.tsv*' | Select-Object -First 1
  $rowCount = 0
  if ($idxFile2) {
    foreach ($line in [IO.File]::ReadAllLines($idxFile2.FullName, [Text.Encoding]::UTF8)) {
      if ($line.Length -gt 0 -and $line[0] -ne '#') { $rowCount++ }
    }
  }
  Row 'S3 索引 TSV 数据行数' ([string]$rowCount)
  Ck ($rowCount -eq $afterRm) '落盘索引行数与内存一致（删除已持久化）' ('tsv=' + $rowCount + ' mem=' + $afterRm)

  # ---- 负向对照：根目录**忽然不可访问**（外接盘拔出/改名/断网盘）时的行为 ----
  # 旧实现只按"本次遍历到的东西"整份替换索引 ⟹ 打不开的根下面的记录会被静默删掉，
  # 用户看到的就是"我的盘明明还在，扫一次少一半歌"。
  $goneRoot = Join-Path $Base 'gone-root'
  New-Item -ItemType Directory -Path $goneRoot -Force | Out-Null
  for ($k = 0; $k -lt 10; $k++) { [IO.File]::Copy($srcFlac, (Join-Path $goneRoot ('g{0:D2}.flac' -f $k))) }
  $cur = lib_roots_add_and_scan $Port $goneRoot
  Row 'S3b 加一个临时根后曲目数' ([string]$cur)
  Rename-Item -LiteralPath $goneRoot -NewName 'gone-root-OFFLINE'
  J $Port '/api/lib/scan' 'POST' 'full=1' | Out-Null
  Wait-Scan $Port 300 | Out-Null
  $afterOff = [int](J $Port '/api/lib/stats').tracks
  Row 'S3b 根目录离线后重扫曲目数' ([string]$afterOff)
  Ck ($afterOff -eq $cur) '根目录不可访问时不得丢掉它下面的记录（旧实现会静默删掉）' ('before=' + $cur + ' after=' + $afterOff)
  J $Port '/api/lib/prune' 'POST' '' | Out-Null   # 收尾：这些"文件仍存在"的记录 prune 不会动
}
finally { Stop-Srv $proc }
}

Say ($NL + '===== 汇总 =====')
foreach ($r in $script:rows) { Say ('  ' + $r.k.PadRight(46) + $r.v) }
Say ('  总耗时(s) = ' + [math]::Round($sw0.Elapsed.TotalSeconds, 1))
if ($script:fails -eq 0) { Say ($NL + '全部通过'); exit 0 } else { Say ($NL + [string]$script:fails + ' 项失败'); exit 1 }