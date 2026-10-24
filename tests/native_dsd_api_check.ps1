# native_dsd_api_check.ps1 -- /api/play 的 Native DSD 参数门（HTTP 层，不打开任何音频设备）
#
# 为什么要有这份判据（铁律 3：判据本身先要被验证）：
#   "把原生 DSD 接进 GUI"最容易做成"看起来成功"——用户选了 Native，程序悄悄走 DoP，
#   或者悄悄把音量 / EQ 忽略掉。所以这里在 HTTP 层钉死 4 条**必须被拒绝**的参数组合：
#     1) 非 DSD 源 + dsdnative=1        -> 拒绝（没有 1-bit 流可直通）
#     2) 音量 != 0 dB + dsdnative=1     -> 拒绝（直通路径没有 PCM 音量环节）
#     3) backend != asio + dsdnative=1  -> 拒绝（ASIO IoFormat 是唯一原生 DSD 通道）
#     4) 带 EQ + dsdnative=1            -> 拒绝（1-bit 流不经过 DSP 链）
#   再补 3 条：
#     5) rate=max（升频）+ dsdnative=1  -> 拒绝（Native 按源位率直通）
#     6) 对照：dsdnative=0 的同一文件 + backend=null **不应**被这道门拒绝
#        —— 否则前面几条可能只是"这个接口永远返回 ok:false"（假门禁）。
#     7) 日志证据：/api/dbg 里每条拒绝都要有 PLAY_REJECT dsdnative=1 记录
#
# 这些用例全部止步于参数门 / Null 后端，因此**不会打开任何音频设备**（铁律 1）。
# 真机出声验证另见 tests/native_dsd_device_check.ps1（3 秒全零 DSD = 数字静音）。
#
# 用法（仓库根目录；本机只有 Windows PowerShell 5.1，没有 pwsh）：
#   powershell -NoProfile -File tests\native_dsd_api_check.ps1 -Exe build-nsd\rtaweb.exe
# 退出码 = 失败条数；全部通过 = 0。
# 注意：本文件必须是 **UTF-8 with BOM**，否则 PowerShell 5.1 会按 ANSI 解码，中文注释直接解析失败。

param(
    [string]$Exe = '',
    [int]$Port = 0,
    [int]$ReadyTimeoutSec = 25
)

$ErrorActionPreference = 'Stop'
$script:fail = 0
$script:pass = 0

function Ck([bool]$ok, [string]$what, [string]$detail = '') {
    if ($ok) { $script:pass++; Write-Host ("  [PASS] " + $what) }
    else { $script:fail++; Write-Host ("  [FAIL] " + $what + "  " + $detail) -ForegroundColor Red }
}

# ---------------- 0. 找 exe / 空闲端口 / 临时库目录 ----------------
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    foreach ($c in @('build-nsd\rtaweb.exe', 'build\rtaweb.exe')) {
        $p = Join-Path $root $c
        if (Test-Path -LiteralPath $p) { $Exe = $p; break }
    }
}
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe)) {
    throw "找不到 rtaweb.exe：请用 -Exe 指定（例如 -Exe build-nsd/rtaweb.exe）"
}
$Exe = (Resolve-Path -LiteralPath $Exe).Path

$rtaplay = Join-Path $root 'build-nsd\rtaplay.exe'
if (-not (Test-Path -LiteralPath $rtaplay)) {
    $alt = Join-Path $root 'build\rtaplay.exe'
    if (Test-Path -LiteralPath $alt) { $rtaplay = $alt } else { $rtaplay = '' }
}

if ($Port -le 0) {
    $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    $Port = ([System.Net.IPEndPoint]$l.LocalEndpoint).Port
    $l.Stop()
}

$tmp = Join-Path $env:TEMP ("rlplayer-nsdapi-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$libDir = Join-Path $tmp 'lib'
New-Item -ItemType Directory -Path $libDir -Force | Out-Null

$flac = Join-Path $tmp 'not-a-dsd.flac'
Set-Content -LiteralPath $flac -Value 'this is not audio; the gate must reject BEFORE reading' -Encoding ascii
$dsf = Join-Path $tmp 'silent-3s.dsf'
$dsfReal = $false
if ($rtaplay) {
    # 全零 DSD（恒定电平 = 数字静音，铁律 1）。生不出来就退化成"占位文件"——
    # 参数门在解码之前，占位文件同样能验证 2/3/4/5 条（只是不能用于真机播放）。
    & $rtaplay --gen-dsd $dsf --seconds 3 --rate 64 --silent 2>&1 | Out-Null
    $dsfReal = (Test-Path -LiteralPath $dsf) -and ((Get-Item -LiteralPath $dsf).Length -gt 1000)
}
if (-not $dsfReal) { Set-Content -LiteralPath $dsf -Value 'placeholder .dsf (gate runs before decode)' -Encoding ascii }

$eq = Join-Path $tmp 'eq.txt'
Set-Content -LiteralPath $eq -Value 'Filter 1: ON PK Fc 1000 Hz Gain 3.0 dB Q 1.0' -Encoding ascii

Write-Host "=== Native DSD /api/play 参数门（HTTP 层，不开设备）==="
Write-Host ("  exe   : " + $Exe)
Write-Host ("  port  : " + $Port + "  （临时库目录 " + $libDir + "）")
Write-Host ("  素材  : " + $dsf + "  (真实全零 DSD64 = " + $dsfReal + ")")

# 子进程用独立的库目录：否则 --port 实例会去扫描用户真实曲库（慢，且与用户无关）
$oldLib = $env:RLPLAYER_LIBRARY_DIR
$env:RLPLAYER_LIBRARY_DIR = $libDir
$proc = $null
$base = "http://127.0.0.1:$Port"

# 用 .NET Process 而不是 Start-Process：本机环境同时存在 NO_PROXY / no_proxy 两个大小写变体，
# Windows PowerShell 5.1 的 Start-Process 会因此抛 "字典中的关键字已添加"（与播放器无关）。
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ('--port ' + $Port)
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = (Split-Path -Parent $Exe)
    $proc = [System.Diagnostics.Process]::Start($psi)

    $ready = $false
    for ($i = 0; $i -lt ($ReadyTimeoutSec * 5); $i++) {
        Start-Sleep -Milliseconds 200
        if ($proc.HasExited) { break }
        try {
            $r = Invoke-WebRequest -Uri ($base + '/api/status') -UseBasicParsing -TimeoutSec 2
            if ($r.StatusCode -eq 200) { $ready = $true; break }
        } catch { }
    }
    if (-not $ready) {
        $err = ''
        try {
            if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
            $err = $proc.StandardError.ReadToEnd() + $proc.StandardOutput.ReadToEnd()
        } catch { }
        throw ("rtaweb 没在 " + $ReadyTimeoutSec + " 秒内起来（端口 " + $Port + "）：" + $err)
    }

    function Post-Play([string]$body) {
        try {
            $r = Invoke-WebRequest -Uri ($base + '/api/play') -Method Post -Body $body -ContentType 'application/x-www-form-urlencoded' -UseBasicParsing -TimeoutSec 20
            return ($r.Content | ConvertFrom-Json)
        } catch {
            return [pscustomobject]@{ ok = $false; error = ("HTTP 异常: " + $_.Exception.Message) }
        }
    }
    $F = [uri]::EscapeDataString($flac)
    $D = [uri]::EscapeDataString($dsf)
    $E = [uri]::EscapeDataString($eq)

    Write-Host ""
    Write-Host "[1] 非 DSD 源 + dsdnative=1"
    $r1 = Post-Play ("file=" + $F + "&backend=asio&dsdnative=1&volume=0&rate=src")
    Ck ($r1.ok -eq $false -and $r1.nativeDsd -eq $true -and $r1.error -match 'DSD 源') "必须拒绝，且原因点名「DSD 源」" ("ok=" + $r1.ok + " err=" + $r1.error)

    Write-Host "[2] 音量 -20 dB + dsdnative=1"
    $r2 = Post-Play ("file=" + $D + "&backend=asio&dsdnative=1&volume=-20&rate=src")
    Ck ($r2.ok -eq $false -and $r2.error -match '音量 0 dB') "必须拒绝，且原因点名「音量 0 dB」" ("ok=" + $r2.ok + " err=" + $r2.error)

    Write-Host "[3] backend=wasapi + dsdnative=1"
    $r3 = Post-Play ("file=" + $D + "&backend=wasapi&dsdnative=1&volume=0&rate=src")
    Ck ($r3.ok -eq $false -and $r3.error -match 'ASIO') "必须拒绝，且原因点名「ASIO」" ("ok=" + $r3.ok + " err=" + $r3.error)

    Write-Host "[4] 带 EQ + dsdnative=1"
    $r4 = Post-Play ("file=" + $D + "&backend=asio&dsdnative=1&volume=0&rate=src&eq=" + $E)
    Ck ($r4.ok -eq $false -and $r4.error -match 'PCM 处理') "必须拒绝，且原因点名「PCM 处理」" ("ok=" + $r4.ok + " err=" + $r4.error)

    Write-Host "[5] rate=max（升频）+ dsdnative=1"
    $r5 = Post-Play ("file=" + $D + "&backend=asio&dsdnative=1&volume=0&rate=max")
    Ck ($r5.ok -eq $false -and $r5.error -match '升频') "必须拒绝，且原因点名「升频」" ("ok=" + $r5.ok + " err=" + $r5.error)

    Write-Host "[6] 对照：dsdnative=0 + backend=null（证明前面不是永远拒绝）"
    $r6 = Post-Play ("file=" + $D + "&backend=null&dsdnative=0&volume=0&rate=src")
    Ck ($r6.ok -eq $true) "dsdnative=0 不应被参数门拒绝（Null 后端，不出声）" ("ok=" + $r6.ok + " err=" + $r6.error)

    Write-Host ""
    Write-Host "[7] 日志证据：/api/dbg 里必须有 5 条 PLAY_REJECT dsdnative=1"
    Start-Sleep -Milliseconds 400
    $dbg = ''
    try { $dbg = (Invoke-WebRequest -Uri ($base + '/api/dbg') -UseBasicParsing -TimeoutSec 10).Content } catch { }
    $nRej = ([regex]::Matches($dbg, 'PLAY_REJECT dsdnative=1')).Count
    Ck ($nRej -ge 5) ("PLAY_REJECT 条数 = " + $nRej + "（期望 >= 5）")
    $rejLines = ($dbg -split '\r?\n' | Where-Object { $_ -match 'PLAY_REJECT dsdnative=1' })
    foreach ($l in $rejLines) { Write-Host ("      " + $l) }
} finally {
    # 收尾：先请它自己退（/api/quit），退不掉才按**自己起的这个 PID**结束。
    # 纪律：绝不按映像名杀进程（铁律 2）。
    if ($proc -and -not $proc.HasExited) {
        try { Invoke-WebRequest -Uri ($base + '/api/quit') -UseBasicParsing -TimeoutSec 2 | Out-Null } catch { }
        for ($i = 0; $i -lt 40; $i++) { if ($proc.HasExited) { break }; Start-Sleep -Milliseconds 200 }
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    }
    if ($null -eq $oldLib) { Remove-Item Env:\RLPLAYER_LIBRARY_DIR -ErrorAction SilentlyContinue }
    else { $env:RLPLAYER_LIBRARY_DIR = $oldLib }
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host ("=== PASS " + $script:pass + " / FAIL " + $script:fail + " ===")
exit $script:fail
