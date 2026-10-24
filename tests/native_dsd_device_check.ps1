# native_dsd_device_check.ps1 -- 真机：通过 /api/play 播 3 秒全零 DSD（ASIO 原生 DSD，数字静音）
#
# 两种模式：
#   A) 默认（原生 DSD）：判据 = /api/signal dsdNative=true / dsdNativeRate=2822400 / type=MSB1|LSB1，
#      日志 NATIVE_DSD OK 里 type=32|33、dsdRate=2822400、ur=0，END error=""。
#      —— 证明走的是【原生 DSD 分支】而不是 DoP（DoP 的载波会是 176400）。
#   B) -ExpectFallback（回落判据）：换一个不支持原生 DSD 的驱动（例如虚拟 ASIO），
#      判据 = 日志 NATIVE_DSD_FALLBACK + 信号链 dsdFallback 非空 + outRate=176400/outBits=24
#      —— 证明"驱动拒绝 DSD ⟹ 显式回落 DoP 并在信号链标出"，而不是静默降级。
#
# 安全（铁律 1）：素材是**全零 DSD** = 恒定电平 = 数字静音（rtaplay 读回峰值约 -240 dBFS）。
# 仍然会以 ASIO 独占接管设备几秒（系统其他声音被静音），所以脚本先打横幅写明设备/时长/电平。
#
# 用法：
#   powershell -NoProfile -File tests\native_dsd_device_check.ps1 -Exe build-nsd\rtaweb.exe
#   powershell -NoProfile -File tests\native_dsd_device_check.ps1 -Exe build-nsd\rtaweb.exe -Device '{33E151FF-AA05-4F03-8285-9444FB933638}' -DeviceLabel 'VB-Matrix VASIO-8' -ExpectFallback
# 退出码：0 = 判据全过；1 = 判据失败；2 = 条件不具备（设备被占用 / 驱动竟然支持原生，未验证）
# 注意：本文件必须是 UTF-8 with BOM。

param(
    [string]$Exe = '',
    [string]$Device = '{112135E1-D565-11D2-854F-00A0C99F5D21}',
    [string]$DeviceLabel = 'AudioByte ASIO',
    [int]$Seconds = 3,
    [int]$Port = 0,
    [int]$ReadyTimeoutSec = 25,
    [switch]$ExpectFallback,
    [switch]$ForceDop,
    [int]$DsdRate = 0
)

$ErrorActionPreference = 'Stop'
$script:fail = 0
$script:pass = 0
function Ck([bool]$ok, [string]$what, [string]$detail = '') {
    if ($ok) { $script:pass++; Write-Host ("  [PASS] " + $what) }
    else { $script:fail++; Write-Host ("  [FAIL] " + $what + "  " + $detail) -ForegroundColor Red }
}

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    foreach ($c in @('build-nsd\rtaweb.exe', 'build\rtaweb.exe')) {
        $p = Join-Path $root $c
        if (Test-Path -LiteralPath $p) { $Exe = $p; break }
    }
}
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe)) { throw "找不到 rtaweb.exe：用 -Exe 指定" }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$logPath = Join-Path (Split-Path -Parent $Exe) 'rlplayer-playback.log'
$rtaplay = Join-Path $root 'build-nsd\rtaplay.exe'
if (-not (Test-Path -LiteralPath $rtaplay)) { $rtaplay = Join-Path $root 'build\rtaplay.exe' }
if (-not (Test-Path -LiteralPath $rtaplay)) { throw "找不到 rtaplay.exe（用来生成全零 DSD 素材）" }

if ($Port -le 0) {
    $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $l.Start(); $Port = ([System.Net.IPEndPoint]$l.LocalEndpoint).Port; $l.Stop()
}

$tmp = Join-Path $env:TEMP ("rlplayer-nsddev-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$libDir = Join-Path $tmp 'lib'
New-Item -ItemType Directory -Path $libDir -Force | Out-Null
$dsf = Join-Path $tmp 'silent.dsf'
& $rtaplay --gen-dsd $dsf --seconds $Seconds --rate 64 --silent | Out-Host
if (-not ((Test-Path -LiteralPath $dsf) -and ((Get-Item -LiteralPath $dsf).Length -gt 1000))) {
    throw "全零 DSD 素材生成失败：" + $dsf
}

$mode = 'Native DSD'
if ($ExpectFallback) { $mode = 'DoP 回落' }
Write-Host ""
Write-Host "╔══════════════════════════════════════════════════════════════════════╗"
Write-Host ("║ 即将通过 ASIO 播 " + $Seconds + " 秒【全零 DSD64】—— 恒定电平 = 数字静音（" + $mode + "）║")
Write-Host "╚══════════════════════════════════════════════════════════════════════╝"
Write-Host ("  设备 : " + $DeviceLabel + "（" + $Device + "）")
Write-Host ("  时长 : " + $Seconds + " 秒")
Write-Host "  电平 : 全零 1-bit 流 = 恒定电平 = 数字静音（远低于 -60 dBFS，铁律 1）"
Write-Host ("  注意 : ASIO 独占会接管该设备并静音系统其他声音（约 " + $Seconds + " 秒）")
Write-Host ""

function Get-NewLog([int]$tail = 400) {
    if (-not (Test-Path -LiteralPath $logPath)) { return '' }
    return ((Get-Content -LiteralPath $logPath -Encoding UTF8 -Tail $tail) -join [Environment]::NewLine)
}

$oldLib = $env:RLPLAYER_LIBRARY_DIR
$env:RLPLAYER_LIBRARY_DIR = $libDir
# 诊断开关（rtaweb 里同名环境变量）：强制跳过原生 DSD，直接验证 DoP 回落全程。
# 只有 -ForceDop 才设；其余时候保持原样（避免"测试脚本偷偷改了被测行为"）。
$oldForce = $env:RLPLAYER_FORCE_DOP_FALLBACK
if ($ForceDop) { $env:RLPLAYER_FORCE_DOP_FALLBACK = '1' }
elseif ($null -ne $oldForce) { Remove-Item Env:\RLPLAYER_FORCE_DOP_FALLBACK -ErrorAction SilentlyContinue }
$proc = $null
$base = "http://127.0.0.1:$Port"
$rc = 0
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
        try { $r = Invoke-WebRequest -Uri ($base + '/api/status') -UseBasicParsing -TimeoutSec 2
              if ($r.StatusCode -eq 200) { $ready = $true; break } } catch { }
    }
    if (-not $ready) { throw ("rtaweb 没起来（端口 " + $Port + "）") }

    $body = "file=" + [uri]::EscapeDataString($dsf) + "&backend=asio&dsdnative=1&volume=0&rate=src&buffer=100&device=" + [uri]::EscapeDataString($Device)
    if ($DsdRate -ge 64) { $body += ("&dsd=" + $DsdRate) }   # 用来看"设置档位 vs 源位率"的说明是否写对
    $play = (Invoke-WebRequest -Uri ($base + '/api/play') -Method Post -Body $body -ContentType 'application/x-www-form-urlencoded' -UseBasicParsing -TimeoutSec 20).Content | ConvertFrom-Json
    Ck ($play.ok -eq $true) "POST /api/play 接受（参数门通过）" ("ok=" + $play.ok + " err=" + $play.error)

    # 等信号链（原生 DSD：dsdNative=true；回落：dsdFallback 非空），最多 20 秒
    $sig = $null
    for ($i = 0; $i -lt 200; $i++) {
        Start-Sleep -Milliseconds 100
        try { $j = (Invoke-WebRequest -Uri ($base + '/api/signal') -UseBasicParsing -TimeoutSec 5).Content | ConvertFrom-Json
              if ($j.ok -eq $true -and $j.path) {
                  if ($ExpectFallback -and $j.path.dsdFallback) { $sig = $j.path; break }
                  if ((-not $ExpectFallback) -and $j.path.dsdNative -eq $true) { $sig = $j.path; break }
              } } catch { }
    }

    if ($ExpectFallback) {
        if (-not $sig) {
            Write-Host "  -- 没等到 DoP 回落（这个驱动可能真的支持原生 DSD）--"
            (Get-NewLog | Select-String -Pattern 'NATIVE_DSD|FALLBACK' -AllMatches) | ForEach-Object { Write-Host ("      " + $_.Line) }
            Ck $false "应出现 dsdFallback（驱动拒绝原生 DSD ⟹ DoP 回落）"
            $rc = 2
        } else {
            Write-Host ("  信号链 : dsdNative=" + $sig.dsdNative + " dsdFallback=" + $sig.dsdFallback)
            Write-Host ("           outRate=" + $sig.outRate + " outBits=" + $sig.outBits + " carrier=" + $sig.carrier)
            Ck ($sig.dsdFallback -match '原生 DSD (打开|启动)失败|诊断开关') "dsdFallback 写明是原生 DSD 失败/被跳过" ("实际=" + $sig.dsdFallback)
            Ck ($sig.outBits -eq 24) "回落后走 DoP（outBits=24 载波）" ("实际=" + $sig.outBits)
            Ck ($sig.carrier -eq 176400) "DoP 载波率 176400（DSD64/16；注意 outRate 是 DSD 位率）" ("carrier=" + $sig.carrier + " outRate=" + $sig.outRate)
            Ck ($sig.dsdNative -eq $false) "信号链没有假装成原生 DSD（dsdNative=false）" ("实际=" + $sig.dsdNative)
        }
    } else {
        if (-not $sig) {
            Write-Host "  -- 未进入原生 DSD 分支，相关日志 --"
            (Get-NewLog | Select-String -Pattern 'NATIVE_DSD|FALLBACK|END ' -AllMatches) | ForEach-Object { Write-Host ("      " + $_.Line) }
            Ck $false "信号链里应出现 dsdNative=true（原生 DSD 分支）"
            $rc = 2
        } else {
            Write-Host ("  信号链 : dsdNative=" + $sig.dsdNative + " type=" + $sig.dsdNativeType + "(" + $sig.dsdNativeTypeCode + ")" +
                        " rate=" + $sig.dsdNativeRate + " outBits=" + $sig.outBits + " driver=" + $sig.dsdNativeDriver)
            Ck ($sig.dsdNativeRate -eq 2822400) "dsdNativeRate = 2822400（DSD 位率，不是 176400 载波）" ("实际=" + $sig.dsdNativeRate)
            Ck (($sig.dsdNativeType -eq 'MSB1') -or ($sig.dsdNativeType -eq 'LSB1')) "采样类型是 1-bit DSD（MSB1/LSB1）" ("实际=" + $sig.dsdNativeType)
            Ck (($sig.dsdNativeTypeCode -eq 33) -or ($sig.dsdNativeTypeCode -eq 32)) "类型码 32/33" ("实际=" + $sig.dsdNativeTypeCode)
            Ck ($sig.outBits -eq 1) "outBits = 1（1-bit 直送）" ("实际=" + $sig.outBits)
        }
    }

    # 等本次的 END（播完 + 收尾），最多 Seconds+25 秒
    $logTxt = ''
    for ($i = 0; $i -lt (($Seconds + 25) * 5); $i++) {
        Start-Sleep -Milliseconds 200
        $logTxt = Get-NewLog
        $endHit = ($logTxt -split '\r?\n' | Where-Object { $_ -match 'END pos=' -and $_.Contains($dsf) })
        if ($endHit) { break }
    }
    $logTxt = Get-NewLog
    Write-Host ""
    Write-Host "  -- 播放日志（本次相关）--"
    $want = ($logTxt -split '\r?\n' | Where-Object { $_ -match 'NATIVE_DSD|PLAY_REJECT|START file=|END pos=' -and ($_ -match 'NATIVE_DSD' -or $_.Contains($dsf) -or $_.Contains('rlplayer-nsddev')) })
    foreach ($l in $want) { Write-Host ("      " + $l) }

    if ($rc -eq 2) {
        Write-Host "  ==> 条件不具备（设备被占用 / 驱动行为不同），本次未验证"
    } else {
        $endLine = ($logTxt -split '\r?\n' | Where-Object { $_ -match 'END pos=' -and $_.Contains($dsf) } | Select-Object -Last 1)
        Ck ([bool]$endLine -and ($endLine -match 'error=""')) "END error 为空（没有出错）" ("END=" + $endLine)
        if ([bool]$endLine -and ($endLine -notmatch 'error=""')) { $rc = 1 }

        if ($ExpectFallback) {
            $fbLine = ($logTxt -split '\r?\n' | Where-Object { $_ -match 'NATIVE_DSD_FALLBACK' } | Select-Object -Last 1)
            Ck ([bool]$fbLine) "日志里有 NATIVE_DSD_FALLBACK（回落有据可查）" ("log 长度=" + $logTxt.Length)
        } else {
            $ndLine = ($logTxt -split '\r?\n' | Where-Object { $_ -match 'NATIVE_DSD OK' } | Select-Object -Last 1)
            Ck ([bool]$ndLine) "日志里有 NATIVE_DSD OK" ("log 长度=" + $logTxt.Length)
            if ($ndLine) {
                $ur = 0; $ty = 0; $rt = 0
                if ($ndLine -match 'ur=(\d+)') { $ur = [int]$Matches[1] }
                if ($ndLine -match ' type=(\d+)') { $ty = [int]$Matches[1] }
                if ($ndLine -match 'dsdRate=(\d+)') { $rt = [int]$Matches[1] }
                Ck ($ur -eq 0) "欠载 ur=0" ("ur=" + $ur)
                Ck (($ty -eq 32) -or ($ty -eq 33)) "驱动报 1-bit DSD 类型 type=32|33" ("type=" + $ty)
                Ck ($rt -eq 2822400) "驱动按 DSD 位率 2822400 跑" ("dsdRate=" + $rt)
            }
        }
    }
    if ($script:fail -gt 0 -and $rc -ne 2) { $rc = 1 }
} finally {
    if ($proc -and -not $proc.HasExited) {
        try { Invoke-WebRequest -Uri ($base + '/api/quit') -UseBasicParsing -TimeoutSec 2 | Out-Null } catch { }
        for ($i = 0; $i -lt 40; $i++) { if ($proc.HasExited) { break }; Start-Sleep -Milliseconds 200 }
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    }
    if ($null -eq $oldLib) { Remove-Item Env:\RLPLAYER_LIBRARY_DIR -ErrorAction SilentlyContinue }
    else { $env:RLPLAYER_LIBRARY_DIR = $oldLib }
    if ($null -eq $oldForce) { Remove-Item Env:\RLPLAYER_FORCE_DOP_FALLBACK -ErrorAction SilentlyContinue }
    else { $env:RLPLAYER_FORCE_DOP_FALLBACK = $oldForce }
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host ""
Write-Host ("=== PASS " + $script:pass + " / FAIL " + $script:fail + " / rc=" + $rc + " ===")
exit $rc
