# pxprobe.ps1 - 逐像素测真实 WebView2 窗口边缘(只读：不改程序、不改配置)
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tools\pxprobe.ps1 -Label "dark-albums"
# 说明：先把窗口置顶再取屏幕像素 —— GetPixel 取的是合成后的桌面，被遮挡时会读到别的程序的像素
#（2026-10-22 本轮第一次跑就读到资源管理器和 DSH 窗口的颜色，必须置顶才是本窗口）。
param([string]$Label = "probe", [switch]$NoTop)
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] public static extern uint GetPixel(IntPtr dc, int x, int y);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x,int y,int cx,int cy,uint flags);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  public static IntPtr FindMain() {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      var c = new StringBuilder(256); GetClassName(h, c, 256);
      if (c.ToString() == "RLPlayerMain" && IsWindowVisible(h)) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static void Top(IntPtr h) {
    SetWindowPos(h, (IntPtr)(-1), 0,0,0,0, 0x0001|0x0002|0x0040);  // NOSIZE|NOMOVE|SHOWWINDOW -> TOPMOST
    SetForegroundWindow(h);
  }
  public static void Untop(IntPtr h) {
    SetWindowPos(h, (IntPtr)(-2), 0,0,0,0, 0x0001|0x0002);        // NOTOPMOST
  }
  public static string Px(int x, int y) {
    IntPtr dc = GetDC(IntPtr.Zero);
    uint v = GetPixel(dc, x, y);
    ReleaseDC(IntPtr.Zero, dc);
    return (v == 0xFFFFFFFFu) ? "??????" : v.ToString("X6");
  }
  public static string Owner(int x, int y) {
    var p = new POINT { X = x, Y = y };
    IntPtr h = WindowFromPoint(p);
    if (h == IntPtr.Zero) return "none";
    var c = new StringBuilder(256); GetClassName(h, c, 256);
    uint pid; GetWindowThreadProcessId(h, out pid);
    return c + "/pid" + pid;
  }
}
'@
$h = [W]::FindMain()
if ($h -eq [IntPtr]::Zero) { Write-Output "ERROR: 找不到 RLPlayerMain 窗口"; exit 2 }
if (-not $NoTop) { [W]::Top($h) }
Start-Sleep -Milliseconds 700
$wr = New-Object W+RECT
[void][W]::GetWindowRect($h, [ref]$wr)
$cl = New-Object W+RECT
[void][W]::GetClientRect($h, [ref]$cl)
$l = $wr.Left; $tp = $wr.Top; $r = $wr.Right - 1; $b = $wr.Bottom - 1
Write-Output "== $Label =="
Write-Output ("rect = $l,$tp - $($wr.Right),$($wr.Bottom) size=$($wr.Right-$wr.Left)x$($wr.Bottom-$wr.Top) client=$($cl.Right)x$($cl.Bottom)")
Write-Output ("owner(edge px)=" + [W]::Owner($l,$((($tp+$b)/2))) + "  owner(inner)=" + [W]::Owner(($l+20),$((($tp+$b)/2))))
$ymid = [int](($tp + $b) / 2); $xmid = [int](($l + $r) / 2)
$o = ""; for ($i=0;$i -lt 8;$i++) { $o += ("x+{0}={1} " -f $i, [W]::Px(($l+$i), $ymid)) }; Write-Output ("LEFT   " + $o)
$o = ""; for ($i=0;$i -lt 8;$i++) { $o += ("y+{0}={1} " -f $i, [W]::Px($xmid, ($tp+$i))) }; Write-Output ("TOP    " + $o)
$o = ""; for ($i=0;$i -lt 8;$i++) { $o += ("b-{0}={1} " -f $i, [W]::Px($xmid, ($b-$i))) }; Write-Output ("BOTTOM " + $o)
$o = ""; for ($i=0;$i -lt 8;$i++) { $o += ("r-{0}={1} " -f $i, [W]::Px(($r-$i), $ymid)) }; Write-Output ("RIGHT  " + $o)
Write-Output "-- 左上角 8x8 --"
for ($yy=0;$yy -lt 8;$yy++) { $o = ("y+{0}: " -f $yy); for ($xx=0;$xx -lt 8;$xx++) { $o += ([W]::Px(($l+$xx),($tp+$yy)) + " ") }; Write-Output $o }
Write-Output "-- 右下角 8x8 --"
for ($yy=7;$yy -ge 0;$yy--) { $o = ("y-{0}: " -f $yy); for ($xx=7;$xx -ge 0;$xx--) { $o += ([W]::Px(($r-$xx),($b-$yy)) + " ") }; Write-Output $o }
Write-Output "-- 纵向扫 x=left+0..6 @ xmid 上下 3 行(看内缩边界在哪) --"
foreach ($yy in 0..6) { $o = ("y+{0}: " -f $yy); for ($xx=0;$xx -lt 8;$xx++) { $o += ([W]::Px($xmid,($tp+$yy)) + " ") }; Write-Output $o }
if (-not $NoTop) { [W]::Untop($h) }
