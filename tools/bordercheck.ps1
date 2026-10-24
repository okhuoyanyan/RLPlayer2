# bordercheck.ps1 — RLPlayer 无边框窗口逐状态取证（结构判据 + 像素判据）
#
# 用法（一行写完，PowerShell 续行符在本文里会被转义，故不示范）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\bordercheck.ps1 -Exe build-border\run\RLPlayer.exe -Theme dark -Out build-border\bordercheck
#
# 为什么要有【结构判据】（铁律 3：判据必须能抓到真问题）：
#   WebView2 子窗口若相对父客户区内缩 5px，那 5px 露出的宿主边带会被 launcher 的
#   WM_ERASEBKGND/WM_PAINT 刷成【当前主题的 --bg】。于是"最外 1..3px == --bg"这条颜色
#   判据在带子存在时反而 PASS —— 这正是 2026-10-24 之前 10 轮都没抓到的原因。
#   所以本脚本同时量 EnumChildWindows 出来的 WebView2 宿主窗口矩形：
#   必须 rel=(0,0)、size==client，四边零间隙，不依赖任何颜色。
#
# 颜色判据的口径（与任务书的差异，有据可查）：
#   任务书要求最外 1..3px == 页面 --bg（dark #08090C / light #f4f6f9 / blue #e9f2ff）。
#   实际页面左右两边是侧栏/右侧栏，用 --bg2（dark #0c0e13 / light #ffffff / blue #f7fbff，
#   见 src/net/ui_page.h:25/41/57）。若只认 --bg，修好之后反而会 FAIL。
#   故判据放宽为"属于该主题的页面背景族 {--bg,--bg2,--bg3}（±2）"，并打印实测色；
#   白色垃圾像素（FFFFFFFF）/未初始化灰（B4B4B4）不在任何一族色表里，照样抓得到。
#   另加"边带台阶"检测：边缘起连续 >=3px 同色、随后一像素突变 >=4，且左右（或上下）
#   两条边在相同偏移出现同一对颜色 —— 那是宿主边带的指纹，正常页面不会有。
#
# 状态覆盖：启动 1s / 启动 5s / 活跃 / 失焦 / 最大化 / 还原 / 改尺寸 / 切主题
# 退出码：0 = 全部 PASS；1 = 有 FAIL；2 = 测试环境本身没起来（进程/窗口/页面）
# 红线：只停自己启动的进程，停之前校验映像名；绝不 force-kill 进程树。

[CmdletBinding()]
param(
    [string]$Exe = '',
    [ValidateSet('dark','light','blue','keep')][string]$Theme = 'dark',
    [string]$Out = '',
    [int]$LoadMinMs = 5000,
    [int]$Pad = 12,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Continue'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Exe) { $Exe = Join-Path $repo 'build-border\run\RLPlayer.exe' }
if (-not $Out) { $Out = Join-Path $repo 'build-border\bordercheck' }
$Exe = [System.IO.Path]::GetFullPath($Exe)
if (-not (Test-Path -LiteralPath $Exe)) { Write-Host ("FATAL 找不到 exe: " + $Exe); exit 2 }

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class BC {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint f);
    [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr m, ref MONITORINFO mi);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int sz);
    [DllImport("dwmapi.dll", EntryPoint="DwmGetWindowAttribute")] public static extern int DwmGetWindowAttributeInt(IntPtr h, int attr, out int v, int sz);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }
}
"@

# ---- DPI：PerMonitorV2 优先（否则坐标被虚拟化，量出来的不是物理像素）----
$dpiOk = $false
try { $dpiOk = [BC]::SetProcessDpiAwarenessContext([IntPtr](-4)) } catch { }
if (-not $dpiOk) { [BC]::SetProcessDPIAware() | Out-Null }

# ---- 主题色表：与 src/net/ui_page.h 的 --bg/--bg2/--bg3 逐字节一致 ----
$Palette = @{
    dark  = @('08090C','0C0E13','11141B')
    light = @('F4F6F9','FFFFFF','EEF1F6')
    blue  = @('E9F2FF','F7FBFF','DCEBFE')
}
# 页面 --bg（顶栏底色）：Detect-Theme 用它判断"当前实际是哪套主题"
$BgMain = @{ dark = '08090C'; light = 'F4F6F9'; blue = 'E9F2FF' }

$script:fails = 0
$script:passes = 0
$script:lines = New-Object System.Collections.ArrayList

function Emit([string]$s) {
    Write-Host $s
    [void]$script:lines.Add($s)
}
function Add-Result([string]$state, [string]$check, [bool]$ok, [string]$detail) {
    $tag = 'FAIL'; if ($ok) { $tag = 'PASS' }
    if ($ok) { $script:passes++ } else { $script:fails++ }
    $line = ('{0} {1,-14} {2,-8} {3}' -f $tag, $state, $check, $detail)
    Emit $line
}
function Hx($c) { '{0:X2}{1:X2}{2:X2}' -f $c.R, $c.G, $c.B }
function InPalette($c, [string[]]$pal, [int]$tol) {
    foreach ($h in $pal) {
        $r = [Convert]::ToInt32($h.Substring(0,2),16)
        $g = [Convert]::ToInt32($h.Substring(2,2),16)
        $b = [Convert]::ToInt32($h.Substring(4,2),16)
        if ([Math]::Abs($c.R - $r) -le $tol -and [Math]::Abs($c.G - $g) -le $tol -and [Math]::Abs($c.B - $b) -le $tol) { return $true }
    }
    return $false
}
function ClrDist($a, $b) {
    $d = [Math]::Abs($a.R - $b.R)
    if ([Math]::Abs($a.G - $b.G) -gt $d) { $d = [Math]::Abs($a.G - $b.G) }
    if ([Math]::Abs($a.B - $b.B) -gt $d) { $d = [Math]::Abs($a.B - $b.B) }
    return $d
}
function RectStr($r) { return ('' + $r.Left + ',' + $r.Top + ' ' + ($r.Right - $r.Left) + 'x' + ($r.Bottom - $r.Top)) }

function Get-WindowInfo([IntPtr]$hwnd) {
    $wr = New-Object BC+RECT;  [void][BC]::GetWindowRect($hwnd, [ref]$wr)
    $cr = New-Object BC+RECT;  [void][BC]::GetClientRect($hwnd, [ref]$cr)
    $pt = New-Object BC+POINT; [void][BC]::ClientToScreen($hwnd, [ref]$pt)
    $mon = New-Object BC+MONITORINFO; $mon.cbSize = 40
    $hmon = [BC]::MonitorFromWindow($hwnd, 2)
    [void][BC]::GetMonitorInfoW($hmon, [ref]$mon)
    $script:kids = @()
    $top = $hwnd
    $cb = [BC+EnumProc]{
        param($h, $l)
        if ([BC]::GetParent($h) -eq $top) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][BC]::GetClassNameW($h, $sb, 256)
            $r = New-Object BC+RECT; [void][BC]::GetWindowRect($h, [ref]$r)
            $script:kids += [pscustomobject]@{
                hw = $h
                cls = $sb.ToString()
                l = ($r.Left - $pt.X); t = ($r.Top - $pt.Y)
                w = ($r.Right - $r.Left); h = ($r.Bottom - $r.Top)
            }
        }
        return $true
    }
    [void][BC]::EnumChildWindows($hwnd, $cb, [IntPtr]::Zero)
    return [pscustomobject]@{
        win = $wr
        cli = [pscustomobject]@{ L = $pt.X; T = $pt.Y; R = ($pt.X + $cr.Right - $cr.Left); B = ($pt.Y + $cr.Bottom - $cr.Top) }
        cliW = ($cr.Right - $cr.Left); cliH = ($cr.Bottom - $cr.Top)
        mon = $mon.rcMonitor
        zoomed = [BC]::IsZoomed($hwnd)
        dpi = [BC]::GetDpiForWindow($hwnd)
        kids = $script:kids
    }
}

function Save-Shot([string]$name, $L, $T, $R, $B) {
    $sx = [Math]::Max($script:scrL, $L - $Pad)
    $sy = [Math]::Max($script:scrT, $T - $Pad)
    $ex = [Math]::Min($script:scrR, $R + $Pad)
    $ey = [Math]::Min($script:scrB, $B + $Pad)
    $w = $ex - $sx; $h = $ey - $sy
    if ($w -le 0 -or $h -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($sx, $sy, 0, 0, $bmp.Size)
    $path = Join-Path $script:Out ($name + '.png')
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    return [pscustomobject]@{ bmp = $bmp; path = $path; sx = $sx; sy = $sy }
}

# 边缘台阶：从 (x,y) 沿 (dx,dy) 走 n 像素，返回"同色前缀长度 + 突变两侧颜色"
# 找"窄带"：偏移 0..n-1 内出现长度 3..6 的同色段，且其内侧第一个像素颜色差 >=4。
# 不能只看从 0 开始的前缀：实测 active 态是 [0..1]=主题色(DWM 1px 边框) + [2..4]=B4B4B4 垃圾，
# 前缀长度只有 2，正是上一版漏判的那一格。
function Get-EdgeStep($bmp, [int]$x, [int]$y, [int]$dx, [int]$dy, [int]$n) {
    $cols = @()
    for ($i = 0; $i -lt $n; $i++) {
        $px = $x + $dx * $i; $py = $y + $dy * $i
        $cols += $bmp.GetPixel($px, $py)
    }
    for ($i = 0; $i -le 4; $i++) {
        $run = 1
        for ($j = $i + 1; $j -lt $n; $j++) { if ((ClrDist $cols[$j] $cols[$i]) -le 1) { $run++ } else { break } }
        if ($run -ge 3 -and $run -le 6 -and ($i + $run) -lt $n -and (ClrDist $cols[$i + $run] $cols[$i]) -ge 4) {
            return [pscustomobject]@{ start = $i; len = $run; c0 = $cols[$i]; c1 = $cols[$i + $run] }
        }
    }
    return $null
}

# 边缘缩放热区回归：6 逻辑像素内父窗口必须返回 HTLEFT/HTTOP/…，而铺满客户区的 WebView2
# 子窗口必须返回 HTTRANSPARENT（把命中交还父窗口）。满幅改造最容易踩坏的就是这一条。
# 注意先 SetCursorPos：父窗口过程先调 DefWindowProc(WM_NCHITTEST)，它按真实光标位置算。
function Test-HitZone([string]$state, [IntPtr]$hwnd) {
    $info = Get-WindowInfo $hwnd
    $midY = [int](($info.win.Top + $info.win.Bottom) / 2)
    $midX = [int](($info.win.Left + $info.win.Right) / 2)
    # 名字不能叫 LP：PowerShell 里 lp 是 Out-Printer 的内置别名，而别名优先级高于函数。
    function New-LParam([int]$x, [int]$y) { return [IntPtr](((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF))) }
    $lpL = New-LParam ($info.win.Left + 3) $midY
    $lpT = New-LParam $midX ($info.win.Top + 3)
    $lpC = New-LParam $midX $midY
    [void][BC]::SetCursorPos(($info.win.Left + 3), $midY)
    $hitL = [BC]::SendMessage($hwnd, 0x0084, [IntPtr]::Zero, $lpL)
    [void][BC]::SetCursorPos($midX, ($info.win.Top + 3))
    $hitT = [BC]::SendMessage($hwnd, 0x0084, [IntPtr]::Zero, $lpT)
    [void][BC]::SetCursorPos($midX, $midY)
    $hitC = [BC]::SendMessage($hwnd, 0x0084, [IntPtr]::Zero, $lpC)
    $ok = ([int]$hitL -eq 10 -and [int]$hitT -eq 12 -and [int]$hitC -eq 1)
    Add-Result $state 'hit' $ok ('左3px=' + [int]$hitL + '(期望10) 上3px=' + [int]$hitT + '(期望12) 中心=' + [int]$hitC + '(期望1)')
    $wv = $null
    foreach ($k in $info.kids) { if ($k.cls -like 'Chrome_WidgetWin*' -and $k.w -gt 0) { if (-not $wv -or ($k.w * $k.h) -gt ($wv.w * $wv.h)) { $wv = $k } } }
    if ($wv) {
        [void][BC]::SetCursorPos(($info.win.Left + 3), $midY)
        $hc = [int][BC]::SendMessage($wv.hw, 0x0084, [IntPtr]::Zero, $lpL)
        Add-Result $state 'hit-child' ($hc -eq -1) ('WebView2 子窗口边缘 WM_NCHITTEST=' + $hc + '（期望 -1=HTTRANSPARENT，交给父窗口做缩放）')
    } else {
        Add-Result $state 'hit-child' $false '找不到 WebView2 子窗口'
    }
}

# 当前页面主题：顶栏背景固定是 --bg，取顶栏中间偏右的一个像素跟三套 --bg 比，取最近的一个。
# 用途：-Theme 只表达"这次跑哪套"，页面实际主题还可能来自 localStorage；启动态按实测主题判，
# 避免把"用户上次存的是浅色"误判成白框。
function Detect-Theme([IntPtr]$hwnd) {
    $info = Get-WindowInfo $hwnd
    $L = [Math]::Max($info.cli.L, $info.mon.Left); $T = [Math]::Max($info.cli.T, $info.mon.Top)
    $R = [Math]::Min($info.cli.R, $info.mon.Right); $B = [Math]::Min($info.cli.B, $info.mon.Bottom)
    $shot = Save-Shot '_detect' $L $T $R $B
    if (-not $shot) { return 'dark' }
    $ox = $L - $shot.sx; $oy = $T - $shot.sy; $w = $R - $L
    $c = $shot.bmp.GetPixel($ox + [int]($w * 0.5), $oy + 4)
    $shot.bmp.Dispose()
    Remove-Item -LiteralPath $shot.path -ErrorAction SilentlyContinue
    $best = 'dark'; $bd = 999
    foreach ($k in @('dark','light','blue')) {
        $h = $BgMain[$k]
        $r = [Convert]::ToInt32($h.Substring(0,2),16); $g = [Convert]::ToInt32($h.Substring(2,2),16); $b = [Convert]::ToInt32($h.Substring(4,2),16)
        $d = [Math]::Max([Math]::Max([Math]::Abs($c.R-$r), [Math]::Abs($c.G-$g)), [Math]::Abs($c.B-$b))
        if ($d -lt $bd) { $bd = $d; $best = $k }
    }
    if ($bd -gt 12) { return 'keep' }   # 顶栏还不是主题色（页面没画完），不猜
    return $best
}

function Test-State([string]$state, [string]$themeKey, [bool]$isMax, [bool]$checkSize = $false) {
    $info = Get-WindowInfo $script:hwnd
    $pal = $Palette[$themeKey]

    # ---- 结构判据：WebView2 直接子窗口必须满幅 ----
    $wv = $null
    foreach ($k in $info.kids) {
        if ($k.cls -like 'Chrome_WidgetWin*' -and $k.w -gt 0 -and $k.h -gt 0) {
            if (-not $wv -or ($k.w * $k.h) -gt ($wv.w * $wv.h)) { $wv = $k }
        }
    }
    if (-not $wv) {
        Add-Result $state 'cover' $false '找不到 WebView2 子窗口（页面可能没起来）'
    } else {
        $ok = ($wv.l -eq 0 -and $wv.t -eq 0 -and $wv.w -eq $info.cliW -and $wv.h -eq $info.cliH)
        $d = ('wv rel=' + $wv.l + ',' + $wv.t + ' ' + $wv.w + 'x' + $wv.h + ' client=' + $info.cliW + 'x' + $info.cliH)
        if (-not $ok) { $d += '   <-- 宿主边带 ' + [Math]::Abs($wv.l) + 'px（WebView2 未铺满客户区）' }
        Add-Result $state 'cover' $ok $d
    }

    # ---- 逻辑尺寸：1340x880 逻辑像素必须稳定（DPI 竞态回归）----
    if ($checkSize -and -not $isMax) {
        $lw = [Math]::Round($info.cliW * 96.0 / $info.dpi, 1)
        $lh = [Math]::Round($info.cliH * 96.0 / $info.dpi, 1)
        $ok = ([Math]::Abs($lw - 1340) -le 1.5 -and [Math]::Abs($lh - 880) -le 1.5)
        Add-Result $state 'size' $ok ('client=' + $info.cliW + 'x' + $info.cliH + ' dpi=' + $info.dpi + ' 逻辑=' + $lw + 'x' + $lh + '（期望 1340x880）')
    }

    # ---- 最大化：客户区必须严格等于监视器 ----
    if ($isMax) {
        $ok = ([int]$info.cli.L -eq [int]$info.mon.Left -and [int]$info.cli.T -eq [int]$info.mon.Top -and
               [int]($info.cli.R - $info.cli.L) -eq [int]($info.mon.Right - $info.mon.Left) -and
               [int]($info.cli.B - $info.cli.T) -eq [int]($info.mon.Bottom - $info.mon.Top))
        $d = ('client=' + ('' + $info.cli.L + ',' + $info.cli.T + ' ' + ($info.cli.R - $info.cli.L) + 'x' + ($info.cli.B - $info.cli.T)) + ' monitor=' + (RectStr $info.mon))
        Add-Result $state 'screen' $ok $d
    }

    # ---- 截图（可见客户端区域外扩 Pad）----
    $L = [Math]::Max($info.cli.L, $info.mon.Left); $T = [Math]::Max($info.cli.T, $info.mon.T)
    $R = [Math]::Min($info.cli.R, $info.mon.Right); $B = [Math]::Min($info.cli.B, $info.mon.Bottom)
    if ($R -le $L -or $B -le $T) { Add-Result $state 'pixel' $false '可见区域为空'; return }
    $shot = Save-Shot $state $L $T $R $B
    if (-not $shot) { Add-Result $state 'pixel' $false '截图失败'; return }
    $bmp = $shot.bmp
    $ox = $L - $shot.sx; $oy = $T - $shot.sy
    $w = $R - $L; $h = $B - $T
    $fx = @(0.1, 0.3, 0.5, 0.7, 0.9)

    # ---- 颜色判据：最外 1..3px 必须是【该主题页面背景族的颜色】或【与页面内部连续】----
    # 后者是必须的：侧栏选中项、"设置"高亮条这类页面元素可以合法地顶到窗口边，
    # 它们不是白框。判据 = 不在主题色族 且 与内侧 8px 处颜色差 >2 ⟹ 才判是异物。
    $bad = @()
    $repL = @(); $repR = @(); $repT = @(); $repB = @()
    foreach ($f in $fx) {
        $yy = $oy + [int]($h * $f)
        $inL = $bmp.GetPixel([Math]::Min($ox + 8, $ox + $w - 1), $yy)   # 越过 5px 边带取页面内参考
        foreach ($k in 1..3) {
            $c = $bmp.GetPixel($ox + $k, $yy)
            if (-not (InPalette $c $pal 8) -and (ClrDist $c $inL) -gt 6) { $bad += ('L@y' + [int]($h * $f) + ':' + (Hx $c)) }
        }
        $repL += (Hx $bmp.GetPixel($ox + 1, $yy))
    }
    foreach ($f in $fx) {
        $yy = $oy + [int]($h * $f)
        $inR = $bmp.GetPixel([Math]::Max($ox, $ox + $w - 9), $yy)
        foreach ($k in 1..3) {
            $c = $bmp.GetPixel($ox + $w - 1 - $k, $yy)
            if (-not (InPalette $c $pal 8) -and (ClrDist $c $inR) -gt 6) { $bad += ('R@y' + [int]($h * $f) + ':' + (Hx $c)) }
        }
        $repR += (Hx $bmp.GetPixel($ox + $w - 2, $yy))
    }
    foreach ($f in $fx) {
        $xx = $ox + [int]($w * $f)
        $inT = $bmp.GetPixel($xx, [Math]::Min($oy + 8, $oy + $h - 1))
        foreach ($k in 1..3) {
            $c = $bmp.GetPixel($xx, $oy + $k)
            if (-not (InPalette $c $pal 8) -and (ClrDist $c $inT) -gt 6) { $bad += ('T@x' + [int]($w * $f) + ':' + (Hx $c)) }
        }
        $repT += (Hx $bmp.GetPixel($xx, $oy + 1))
    }
    foreach ($f in $fx) {
        $xx = $ox + [int]($w * $f)
        $inB = $bmp.GetPixel($xx, [Math]::Max($oy, $oy + $h - 9))
        foreach ($k in 1..3) {
            $c = $bmp.GetPixel($xx, $oy + $h - 1 - $k)
            if (-not (InPalette $c $pal 8) -and (ClrDist $c $inB) -gt 6) { $bad += ('B@x' + [int]($w * $f) + ':' + (Hx $c)) }
        }
        $repB += (Hx $bmp.GetPixel($xx, $oy + $h - 2))
    }
    $uL = ($repL | Select-Object -Unique) -join '/'
    $uR = ($repR | Select-Object -Unique) -join '/'
    $uT = ($repT | Select-Object -Unique) -join '/'
    $uB = ($repB | Select-Object -Unique) -join '/'
    if ($bad.Count -eq 0) {
        Add-Result $state 'edge' $true ('主题=' + $themeKey + ' L=' + $uL + ' R=' + $uR + ' T=' + $uT + ' B=' + $uB)
    } else {
        Add-Result $state 'edge' $false ('主题=' + $themeKey + ' 非页面底色 ' + $bad[0] + '（共 ' + $bad.Count + ' 处）')
    }

    # ---- 边带台阶（颜色判据的补强）----
    $stepsLR = @(); $stepsTB = @()
    foreach ($f in $fx) {
        $yy = $oy + [int]($h * $f)
        $s = Get-EdgeStep $bmp ($ox) ($yy) 1 0 8
        if ($s) { $stepsLR += ($s.start.ToString() + ':' + $s.len.ToString() + ':' + (Hx $s.c0) + '>' + (Hx $s.c1)) }
        $s2 = Get-EdgeStep $bmp ($ox + $w - 1) ($yy) -1 0 8
        if ($s2) { $stepsLR += ($s2.start.ToString() + ':' + $s2.len.ToString() + ':' + (Hx $s2.c0) + '>' + (Hx $s2.c1)) }
    }
    foreach ($f in $fx) {
        $xx = $ox + [int]($w * $f)
        $s = Get-EdgeStep $bmp ($xx) ($oy) 0 1 8
        if ($s) { $stepsTB += ($s.start.ToString() + ':' + $s.len.ToString() + ':' + (Hx $s.c0) + '>' + (Hx $s.c1)) }
        $s2 = Get-EdgeStep $bmp ($xx) ($oy + $h - 1) 0 -1 8
        if ($s2) { $stepsTB += ($s2.start.ToString() + ':' + $s2.len.ToString() + ':' + (Hx $s2.c0) + '>' + (Hx $s2.c1)) }
    }
    $band = $null
    foreach ($g in ($stepsLR | Group-Object)) { if ($g.Count -ge 4) { $band = $g.Name } }
    if (-not $band) { foreach ($g in ($stepsTB | Group-Object)) { if ($g.Count -ge 4) { $band = $g.Name } } }
    if ($band) {
        $p = $band.Split(':')
        Add-Result $state 'band' $false ('边缘偏移 ' + $p[0] + 'px 起有 ' + $p[1] + 'px 的 ' + $p[2] + '（宿主边带/外框；同一偏移在两侧各 5 处复现）')
    } else {
        Add-Result $state 'band' $true '四边无"同色窄带+突变"'
    }

    # ---- 圆角：四角必须由页面自身像素铺满（DWM 圆角会切掉角，露出窗外内容）----
    $cornerBad = @()
    $cornerRep = @()
    $cs = 5
    $pts = @(
        @{ n='TL'; x=$ox;      y=$oy;      dx=1;  dy=1 },
        @{ n='TR'; x=$ox+$w-1; y=$oy;      dx=-1; dy=1 },
        @{ n='BL'; x=$ox;      y=$oy+$h-1; dx=1;  dy=-1 },
        @{ n='BR'; x=$ox+$w-1; y=$oy+$h-1; dx=-1; dy=-1 }
    )
    if ($isMax) {
        Add-Result $state 'corner' $true '最大化时四角在屏外/系统不圆角，不判圆角'
    } else {
        foreach ($pt in $pts) {
            $mn = @(255,255,255); $mx = @(0,0,0); $ref = $bmp.GetPixel($pt.x, $pt.y)
            for ($i = 0; $i -lt $cs; $i++) {
                for ($j = 0; $j -lt $cs; $j++) {
                    $c = $bmp.GetPixel($pt.x + $pt.dx * $i, $pt.y + $pt.dy * $j)
                    $v = @($c.R, $c.G, $c.B)
                    for ($ch = 0; $ch -lt 3; $ch++) {
                        if ($v[$ch] -lt $mn[$ch]) { $mn[$ch] = $v[$ch] }
                        if ($v[$ch] -gt $mx[$ch]) { $mx[$ch] = $v[$ch] }
                    }
                }
            }
            $spread = [Math]::Max([Math]::Max($mx[0]-$mn[0], $mx[1]-$mn[1]), $mx[2]-$mn[2])
            $cornerRep += ($pt.n + '=' + (Hx $ref))
            if ($spread -gt 4) { $cornerBad += ($pt.n + '(spread=' + $spread + ' ' + (Hx $ref) + ')') }
        }
        $dc = 0
        $hr = [BC]::DwmGetWindowAttributeInt($script:hwnd, 33, [ref]$dc, 4)
        if ($hr -ne 0) { $cornerRep += 'dwmCorner=n/a' }
        else {
            $cornerRep += ('dwmCorner=' + $dc)
            if ($dc -ne 1) { $cornerBad += ('DWM 圆角偏好=' + $dc + '（1=DONOTROUND）') }
        }
        if ($cornerBad.Count -eq 0) { Add-Result $state 'corner' $true ($cornerRep -join ' ') }
        else { Add-Result $state 'corner' $false (($cornerBad -join '; ') + ' | ' + ($cornerRep -join ' ')) }
    }

    Emit ('SHOT ' + $shot.path)
    $bmp.Dispose()
}

function Grab-Rect([IntPtr]$h) {
    $r = New-Object BC+RECT; [void][BC]::GetWindowRect($h, [ref]$r); return $r
}
# 在【当前主题=深色】的顶栏里按颜色找"浅蓝"主题圆点，返回它到窗口右边缘的距离与 y
function Find-SwatchOffset([IntPtr]$h) {
    $wr = Grab-Rect $h
    $w = $wr.Right - $wr.Left
    $shot = Save-Shot '_swatch-scan' $wr.Left $wr.Top $wr.Right $wr.Bottom
    if (-not $shot) { return $null }
    $bmp = $shot.bmp
    # 只认"长横条"：圆点直径 ~27px，会连着几十个像素；选中态的蓝色描边只有 2~3px 宽，
    # 若按单像素取包围盒，描边会把 bbox 拉大、中心算错（上一版就是这么点偏的）。
    # 注意截图带了 Pad 边距：位图坐标 = 窗口坐标 + (shot.sx/sy 偏移)，必须先减掉。
    $ox = $wr.Left - $shot.sx; $oy = $wr.Top - $shot.sy
    $minx = 999999; $maxx = -1; $miny = 999999; $maxy = -1
    $x0 = [Math]::Max($ox, $ox + $w - 600)
    $xEnd = $ox + $w
    $y1 = [Math]::Min($oy + 120, $bmp.Height)
    for ($y = $oy; $y -lt $y1; $y++) {
        $runS = -1
        for ($x = $x0; $x -le $xEnd; $x++) {
            $hit = $false
            if ($x -lt $xEnd) {
                $c = $bmp.GetPixel($x, $y)
                if ($c.B -gt 200 -and $c.G -gt 200 -and ($c.B - $c.R) -gt 15) { $hit = $true }
            }
            if ($hit -and $runS -lt 0) { $runS = $x }
            if (-not $hit -and $runS -ge 0) {
                if (($x - $runS) -ge 12) {
                    if ($runS -lt $minx) { $minx = $runS }
                    if (($x - 1) -gt $maxx) { $maxx = $x - 1 }
                    if ($y -lt $miny) { $miny = $y }
                    if ($y -gt $maxy) { $maxy = $y }
                }
                $runS = -1
            }
        }
    }
    $bmp.Dispose()
    Remove-Item -LiteralPath $shot.path -ErrorAction SilentlyContinue
    if ($maxx -lt 0) { return $null }
    $bw = $maxx - $minx + 1; $bh = $maxy - $miny + 1
    if ($bw -lt 12 -or $bw -gt 60 -or $bh -lt 12 -or $bh -gt 60) { return $null }
    return [pscustomobject]@{ fromRight = ($xEnd - [int](($minx + $maxx) / 2)); y = ([int](($miny + $maxy) / 2) - $oy); w = $bw }
}
function Click-Swatch([IntPtr]$h, [int]$fromRight, [int]$yFromTop, [int]$index, [int]$step) {
    $wr = Grab-Rect $h
    $tx = $wr.Right - $fromRight - ($index * $step)
    $ty = $wr.Top + $yFromTop
    [void][BC]::SetForegroundWindow($h)
    [void][BC]::SetCursorPos($tx, $ty)
    Start-Sleep -Milliseconds 180
    [BC]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [BC]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 1000
}

# SetForegroundWindow 对非前台进程会被拒；AttachThreadInput 到当前前台线程后就能设。
# 这是"活跃/失焦"两个状态能被证伪的前提：上一版 active 抓到的是别的窗口在前台。
function Force-Foreground([IntPtr]$h) {
    if ([BC]::GetForegroundWindow() -eq $h) { return $true }
    $pidTmp = [uint32]0
    $fg = [BC]::GetForegroundWindow()
    $tid = 0
    if ($fg -ne [IntPtr]::Zero) { $tid = [BC]::GetWindowThreadProcessId($fg, [ref]$pidTmp) }
    $me = [BC]::GetCurrentThreadId()
    $attached = $false
    if ($tid -ne 0) { $attached = [BC]::AttachThreadInput($me, $tid, $true) }
    [void][BC]::BringWindowToTop($h)
    [void][BC]::SetForegroundWindow($h)
    if ($attached) { [void][BC]::AttachThreadInput($me, $tid, $false) }
    Start-Sleep -Milliseconds 150
    return ([BC]::GetForegroundWindow() -eq $h)
}

# ================= 准备 =================
if (-not (Test-Path -LiteralPath $Out)) { [void](New-Item -ItemType Directory -Force -Path $Out) }
Get-ChildItem -LiteralPath $Out -Filter '*.png' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
$script:Out = $Out
$scr = [System.Windows.Forms.SystemInformation]::VirtualScreen
$script:scrL = $scr.Left; $script:scrT = $scr.Top; $script:scrR = $scr.Right; $script:scrB = $scr.Bottom

Emit '============================================================'
Emit ('bordercheck  ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Emit ('exe      = ' + $Exe)
Emit ('theme    = ' + $Theme)
Emit ('out      = ' + $Out)
Emit ('screen   = ' + $scr.Left + ',' + $scr.Top + ' ' + $scr.Width + 'x' + $scr.Height)
Emit '------------------------------------------------------------'

$proc = $null
$hwnd = [IntPtr]::Zero
$form = $null
$t0 = Get-Date
try {
    $proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
    $myPid = [uint32]$proc.Id
    Emit ('launched pid=' + $myPid)

    for ($i = 0; $i -lt 160; $i++) {
        Start-Sleep -Milliseconds 100
        $script:foundHwnd = [IntPtr]::Zero
        $script:wantPid = $myPid
        $cb = [BC+EnumProc]{
            param($h, $l)
            $p = [uint32]0; [void][BC]::GetWindowThreadProcessId($h, [ref]$p)
            if ($p -eq $script:wantPid) {
                $sb = New-Object System.Text.StringBuilder 256
                [void][BC]::GetClassNameW($h, $sb, 256)
                if ($sb.ToString() -eq 'RLPlayerMain') { $script:foundHwnd = $h; return $false }
            }
            return $true
        }
        [void][BC]::EnumWindows($cb, [IntPtr]::Zero)
        if ($script:foundHwnd -ne [IntPtr]::Zero) { $hwnd = $script:foundHwnd; break }
        if ($proc.HasExited) { break }
    }
    if ($hwnd -eq [IntPtr]::Zero) { Emit 'FATAL 等不到 RLPlayerMain 主窗口'; throw 'no-window' }
    $script:hwnd = $hwnd
    Emit ('window hwnd=' + $hwnd)

    for ($i = 0; $i -lt 200; $i++) {
        if ([BC]::IsWindowVisible($hwnd)) { break }
        Start-Sleep -Milliseconds 100
    }
    $tVis = Get-Date
    $info0 = Get-WindowInfo $hwnd
    Emit ('第一次可见: dpi=' + $info0.dpi + ' win=' + (RectStr $info0.win) + ' client=' + $info0.cliW + 'x' + $info0.cliH + ' monitor=' + (RectStr $info0.mon))
    [void][BC]::SetWindowPos($hwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0010)

    $themeNow = 'dark'
    if ($Theme -ne 'keep') { $themeNow = $Theme }

    $due = $tVis.AddMilliseconds(1000)
    while ((Get-Date) -lt $due) { Start-Sleep -Milliseconds 50 }
    $det1 = Detect-Theme $hwnd
    if ($det1 -ne 'keep') { $themeNow = $det1 }
    Emit ('启动时页面主题 = ' + $themeNow + '（-Theme ' + $Theme + '）')
    Test-State 'start-1s' $themeNow $false

    $due = $tVis.AddMilliseconds($LoadMinMs)
    while ((Get-Date) -lt $due) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Milliseconds 600
    $det5 = Detect-Theme $hwnd
    if ($det5 -ne 'keep') { $themeNow = $det5 }
    Test-State 'start-5s' $themeNow $false $true
    Test-HitZone 'start-5s' $hwnd

    $sw = Find-SwatchOffset $hwnd
    $step = [int](24 * (Get-WindowInfo $hwnd).dpi / 96)
    if (-not $sw) { Emit 'WARN 没找到"浅蓝"主题圆点，主题切换状态无法执行' }
    else { Emit ('主题圆点: 距右边缘 ' + $sw.fromRight + 'px, y=' + $sw.y + 'px, 直径=' + $sw.w + 'px, 步距=' + $step + 'px') }
    # -Theme 指定的主题：如果页面当前不是它，点一下圆点切过去（这样后续状态按 -Theme 判）
    if ($sw -and $Theme -ne 'keep' -and $themeNow -ne $Theme) {
        $idx = @{ 'blue' = 0; 'light' = 1; 'dark' = 2 }[$Theme]
        Click-Swatch $hwnd $sw.fromRight $sw.y $idx $step
        $themeNow = $Theme
        Emit ('已按 -Theme ' + $Theme + ' 切换页面主题')
    }

    $fgOk = Force-Foreground $hwnd
    Start-Sleep -Milliseconds 700
    $fg = [BC]::GetForegroundWindow()
    Test-State 'active' $themeNow $false
    if (-not $fgOk) { Emit ('NOTE active 状态前台窗口不是主窗口（fg=' + $fg + '），已重试 AttachThreadInput') }

    $probeX = [Math]::Max($script:scrL, $info0.mon.Right - 260)
    $probeY = $info0.mon.Top + 120
    $form = New-Object System.Windows.Forms.Form
    $form.FormBorderStyle = 'FixedToolWindow'
    $form.Text = 'bordercheck-probe'
    $form.StartPosition = 'Manual'
    $form.Location = New-Object System.Drawing.Point($probeX, $probeY)
    $form.Size = New-Object System.Drawing.Size(200, 150)
    $form.TopMost = $true
    $form.Show()
    $form.Activate()
    [System.Windows.Forms.Application]::DoEvents()
    $probeOk = Force-Foreground $form.Handle
    Start-Sleep -Milliseconds 800
    $fg2 = [BC]::GetForegroundWindow()
    Test-State 'inactive' $themeNow $false
    if (-not $probeOk) { Emit ('NOTE inactive 状态探测窗口没拿到前台（fg=' + $fg2 + ' probe=' + $form.Handle + '）') }
    $form.Close(); $form.Dispose(); $form = $null
    [void][BC]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 400

    [void][BC]::ShowWindow($hwnd, 3)
    Start-Sleep -Milliseconds 1300
    Test-State 'maximized' $themeNow $true

    [void][BC]::ShowWindow($hwnd, 9)
    Start-Sleep -Milliseconds 900
    Test-State 'restored' $themeNow $false $true

    $wr = Grab-Rect $hwnd
    [void][BC]::SetWindowPos($hwnd, [IntPtr](-1), $wr.Left, $wr.Top, 1503, 903, 0x0004)
    Start-Sleep -Milliseconds 900
    Test-State 'resized' $themeNow $false

    if ($sw) {
        $sw2 = Find-SwatchOffset $hwnd
        if ($sw2) { $sw = $sw2 }
        foreach ($pair in @(@('light',1), @('blue',0), @('dark',2))) {
            $tname = $pair[0]; $idx = $pair[1]
            Click-Swatch $hwnd $sw.fromRight $sw.y $idx $step
            Test-State ('theme-' + $tname) $tname $false
        }
    } else {
        Add-Result 'theme-light' 'edge' $false '未定位主题圆点，无法切换'
    }

    Emit '------------------------------------------------------------'
    Emit ('SUMMARY PASS=' + $script:passes + ' FAIL=' + $script:fails)
    Emit ('SCREENSHOTS ' + $Out)
    Emit ('elapsed ' + [int]((Get-Date) - $t0).TotalSeconds + 's')
}
catch {
    Emit ('EXCEPTION ' + $_.Exception.Message)
    Emit ('  at ' + $_.InvocationInfo.PositionMessage)
    $script:fails++
}
finally {
    if ($form) { try { $form.Close(); $form.Dispose() } catch { } }
    if ($proc -and -not $KeepRunning) {
        if ($hwnd -ne [IntPtr]::Zero -and [BC]::IsWindowVisible($hwnd)) { [void][BC]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
        $exited = $false
        for ($i = 0; $i -lt 60; $i++) { if ($proc.HasExited) { $exited = $true; break }; Start-Sleep -Milliseconds 100 }
        if (-not $exited) {
            $p2 = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
            if ($p2 -and $p2.ProcessName -eq 'RLPlayer') {
                Emit ('cleanup: WM_CLOSE 后仍在运行，Stop-Process pid=' + $proc.Id)
                Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue
            }
        } else { Emit 'cleanup: 进程已退出' }
        Get-CimInstance Win32_Process -Filter "Name='rtaweb.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ParentProcessId -eq $proc.Id } |
            ForEach-Object {
                $q = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
                if ($q -and $q.ProcessName -eq 'rtaweb') { Stop-Process -Id $_.ProcessId -ErrorAction SilentlyContinue; Emit ('cleanup: 停掉本进程的 rtaweb pid=' + $_.ProcessId) }
            }
    }
}

if ($script:fails -gt 0) { exit 1 } else { exit 0 }
