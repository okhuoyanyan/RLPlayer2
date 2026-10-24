# verify_libtag_dirfill.ps1 - 目录结构判据 -> album/discNo 回填的**真实入口**验证
#
# 判据（铁律 3）：不看 API「看着对」，而看两处落点：
#   ① rlplayer-index.tsv（落盘）：BoxSetAlbum 的 4 行 album 列 = BoxSetAlbum、disc 列 = 1/1/2/2
#   ② GET /api/lib/album?k=<key>（接口）：曲目对象 d 属性 = 1,1,2,2
# 回归：带真实 album 标签的曲目不得被目录名覆盖（样本 ArtistC/DirNameDifferent/01-Tagged.flac，
#       目录名与标签故意不同）；misc/ 与分类层 Compilations 不得进库。
#
# 不发声：样本全是 anullsrc 数字静音，且 backend=null / volume=-60（本脚本根本不播放）。
# 不按映像名杀进程：只对自己 Start-Process 拿到的 PID 操作，且优先用 /api/quit 正常退出。
param(
  [string]$BuildDir = "build-libtag",
  [int]$Port = 8942
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe  = Join-Path $root (Join-Path $BuildDir "rtaweb.exe")
if (!(Test-Path $exe)) { throw "找不到 rtaweb.exe: $exe（先 cmake --build $BuildDir）" }

$work = Join-Path $root (Join-Path $BuildDir "libtag_dirfill")
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$data = Join-Path $work "data"      # RLPLAYER_LIBRARY_DIR
$lib  = Join-Path $work "lib"       # 库根
New-Item -ItemType Directory -Path $data -Force | Out-Null

# 1) 样本库：优先复制 build-verify2/lib（上一轮实测用的那份），缺了就按同一结构重建
$src = Join-Path $root "build-verify2\lib"
$silent = { param($p) & ffmpeg -y -loglevel error -f lavfi -i anullsrc=r=44100:cl=stereo -t 1 -c:a flac $p 2>$null }
if (Test-Path $src) {
  Write-Host "[fixture] 复制样本库 build-verify2/lib"
  Copy-Item -Recurse -Force $src $lib
} else {
  Write-Host "[fixture] build-verify2/lib 不存在 -> 重建 ArtistA/BoxSetAlbum{CD1,CD2,misc}, Compilations/SummerHits, ArtistB/SoloAlbum"
  $dirs = @("ArtistA\BoxSetAlbum\CD1","ArtistA\BoxSetAlbum\CD2","ArtistA\BoxSetAlbum\misc",
            "Compilations\SummerHits","ArtistB\SoloAlbum")
  foreach ($d in $dirs) { New-Item -ItemType Directory -Path (Join-Path $lib $d) -Force | Out-Null }
  & $silent (Join-Path $lib "ArtistA\BoxSetAlbum\CD1\01-Track1.flac")
  & $silent (Join-Path $lib "ArtistA\BoxSetAlbum\CD1\02-Track2.flac")
  & $silent (Join-Path $lib "ArtistA\BoxSetAlbum\CD2\01-Track3.flac")
  & $silent (Join-Path $lib "ArtistA\BoxSetAlbum\CD2\02-Track4.flac")
  & $silent (Join-Path $lib "Compilations\SummerHits\01-SongA.flac")
  & $silent (Join-Path $lib "Compilations\SummerHits\02-SongB.flac")
  & $silent (Join-Path $lib "ArtistB\SoloAlbum\01-Intro.flac")
  & $silent (Join-Path $lib "ArtistB\SoloAlbum\02-Outro.flac")
  Set-Content -Path (Join-Path $lib "ArtistA\BoxSetAlbum\misc\booklet.txt") -Value "booklet"
}
# 2) 真实标签回归样本：album 标签与目录名**故意不同**
$tagDir = Join-Path $lib "ArtistC\DirNameDifferent"
New-Item -ItemType Directory -Path $tagDir -Force | Out-Null
$tagged = Join-Path $tagDir "01-Tagged.flac"
& ffmpeg -y -loglevel error -f lavfi -i anullsrc=r=44100:cl=stereo -t 1 -c:a flac -metadata album=RealTagAlbum -metadata artist=ArtistC -metadata title=Tagged -metadata track=1 $tagged 2>$null
if (!(Test-Path $tagged)) { throw "带标签样本生成失败（ffmpeg 不可用？）" }

# 3) 设置：backend=null / volume=-60（铁律 1：默认不可闻；本脚本也不播放）
Set-Content -Path (Join-Path $data "rlplayer-settings.conf") -Value @("backend=null","volume=-60") -Encoding utf8
# 先写好库根，避免 Library::load() 的「首次运行自动探测」把用户真实 Music 目录也扫进来
# （实测：不写这份 conf 时会多扫 5 首用户曲目，判据就不再是「只有样本库」）。
# 文件名与 confLeaf() 同规则：非 8899 端口加 .p<port> 后缀。
Set-Content -Path (Join-Path $data "rlplayer-library.conf.p$Port") -Value $lib -Encoding utf8

$env:RLPLAYER_LIBRARY_DIR = $data
$env:RLPLAYER_PORT = "$Port"
$p = Start-Process -FilePath $exe -ArgumentList @("--port","$Port") -PassThru -WindowStyle Hidden
$procId = $p.Id
$base = "http://127.0.0.1:$Port"
$TAB = [char]9
Write-Host "[run] rtaweb PID=$procId port=$Port data=$data"
try {
  $ready = $false
  for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 100
    try { $ping = Invoke-RestMethod "$base/api/ping" -TimeoutSec 1; if ($ping -match "RLPlayer") { $ready = $true; break } } catch {}
  }
  if (!$ready) { throw "rtaweb 未在 $Port 监听" }

  # 等启动时的自动扫描（无根目录 -> 0 首）结束
  for ($i = 0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 100; $s0 = Invoke-RestMethod "$base/api/lib/stats"; if (-not $s0.scanning) { break } }

  Invoke-RestMethod "$base/api/lib/roots" -Method Post -Body ("add=" + [uri]::EscapeDataString($lib)) | Out-Null
  Invoke-RestMethod "$base/api/lib/scan"  -Method Post -Body "full=1" | Out-Null
  $stats = $null
  for ($i = 0; $i -lt 400; $i++) {
    Start-Sleep -Milliseconds 100
    $stats = Invoke-RestMethod "$base/api/lib/stats"
    if (-not $stats.scanning -and $stats.tracks -gt 0) { break }
  }
  Write-Host ("[scan] tracks={0} albums={1} probed={2} cached={3} scanMs={4} msg={5}" -f $stats.tracks, $stats.albums, $stats.probed, $stats.cached, $stats.lastScanMs, $stats.msg)
  if ([int]$stats.tracks -ne 9) { throw "曲目数 = $($stats.tracks)（期望 9 = 样本库 8 首 + 带标签 1 首；多出来说明扫到了别的根目录）" }
  if ([int]$stats.albums -ne 4) { throw "专辑数 = $($stats.albums)（期望 4：BoxSetAlbum / SummerHits / SoloAlbum / RealTagAlbum）" }

  # ---- ① 落盘断言 ----
  $idx = Get-ChildItem -Path $data -Filter "rlplayer-index*.tsv*" | Select-Object -First 1
  if (!$idx) { throw "没有落盘索引文件（$data）" }
  Write-Host ("[idx] {0}" -f $idx.FullName)
  $lines = Get-Content -Path $idx.FullName -Encoding utf8
  $header = $lines | Where-Object { $_.StartsWith("#path") } | Select-Object -First 1
  Write-Host ("[idx] header = {0}" -f $header)
  $rows = @($lines | Where-Object { $_ -and -not $_.StartsWith("#") })
  $box = @()
  foreach ($r in $rows) {
    $f = $r -split $TAB
    if ($f.Count -ne 16) { throw "列数不是 16：$($f.Count) -> $r" }
    if ($f[0] -like "*BoxSetAlbum*") { $box += ,@($f[0], $f[3], $f[8]) }
  }
  Write-Host "[tsv] BoxSetAlbum 4 行（文件 | album 列 | disc 列）："
  foreach ($b in ($box | Sort-Object { $_[0] })) { Write-Host ("   {0} | album={1} | disc={2}" -f (Split-Path $b[0] -Leaf), $b[1], $b[2]) }
  if ($box.Count -ne 4) { throw "BoxSetAlbum 行数 = $($box.Count)（期望 4）" }
  foreach ($b in $box) {
    if ($b[1] -ne "BoxSetAlbum") { throw "album 列 = $($b[1])（期望 BoxSetAlbum）：$($b[0])" }
    $leaf = Split-Path $b[0] -Leaf
    if ($leaf -eq "01-Track1.flac" -or $leaf -eq "02-Track2.flac") { $want = 1 } else { $want = 2 }
    if ([int]$b[2] -ne $want) { throw "disc 列 = $($b[2])（期望 $want）：$($b[0])" }
  }
  $discOrder = ($box | Sort-Object { $_[0] } | ForEach-Object { $_[2] }) -join "/"
  Write-Host "[tsv] disc 列按路径顺序 = $discOrder"

  $miscRows = @($rows | Where-Object { $_ -like "*\misc\*" })
  if ($miscRows.Count -gt 0) { throw "misc/ 进了库：$($miscRows.Count) 行" }
  $tagRow = @($rows | Where-Object { $_ -like "*01-Tagged.flac*" })[0]
  if (!$tagRow) { throw "带标签样本没进库" }
  $tf = $tagRow -split $TAB
  Write-Host ("[tsv] 带标签样本 album 列 = {0}（期望 RealTagAlbum；目录名是 DirNameDifferent）" -f $tf[3])
  if ($tf[3] -ne "RealTagAlbum") { throw "真实 album 标签被目录名覆盖：$($tf[3])" }

  # ---- ② 接口断言：/api/lib/album 的 d 属性 ----
  $albums = Invoke-RestMethod "$base/api/lib/albums"
  $boxAlb = @($albums.albums | Where-Object { $_.name -eq "BoxSetAlbum" })[0]
  if (!$boxAlb) { throw "接口里找不到专辑 BoxSetAlbum" }
  $det = Invoke-RestMethod ("$base/api/lib/album?k=" + [uri]::EscapeDataString($boxAlb.k))
  $dv = @($det.tracks | ForEach-Object { $_.d })
  Write-Host ("[api] key={0} name={1} n={2} tracks.d = {3}" -f $det.k, $det.name, @($det.tracks).Count, ($dv -join ","))
  foreach ($t in $det.tracks) { Write-Host ("   d={0} n={1} {2}" -f $t.d, $t.n, (Split-Path $t.path -Leaf)) }
  if (($dv -join ",") -ne "1,1,2,2") { throw "d 属性与期望 1,1,2,2 不符：$($dv -join ',')" }
  $tagAlb = @($albums.albums | Where-Object { $_.name -eq "RealTagAlbum" })[0]
  if (!$tagAlb) { throw "接口里找不到专辑 RealTagAlbum（真实标签）" }
  Write-Host ("[api] RealTagAlbum key={0} n={1}（目录名 DirNameDifferent 没变成专辑名）" -f $tagAlb.k, $tagAlb.n)
  $names = @($albums.albums | ForEach-Object { "$($_.name)($($_.n))" }) -join " "
  Write-Host "[api] albums = $names"
  Write-Host "VERIFY-OK"
}
finally {
  try { Invoke-RestMethod "$base/api/quit" -Method Post -TimeoutSec 2 | Out-Null } catch {}
  for ($i = 0; $i -lt 80; $i++) { Start-Sleep -Milliseconds 100; if ($p.HasExited) { break } }
  if (!$p.HasExited) { Write-Warning "quit 后 8s 未退出，只停自己启动的 PID $procId"; Stop-Process -Id $procId -ErrorAction SilentlyContinue }
  else { Write-Host "[exit] rtaweb 已正常退出 exitCode=$($p.ExitCode)" }
}
