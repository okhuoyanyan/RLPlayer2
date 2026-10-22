# pxclick.ps1 - 在 RLPlayer 内置窗口上做真实鼠标点击（窗口相对坐标）
# 用途：真实复现用户操作（切换主题 / 跳转页面），配合 pxprobe 逐像素测量。
# 注：不用 CDP，因为要验的就是「真实宿主窗口」而不是浏览器里的页面。
param(
  [Parameter(Mandatory=$true)][string]$Points,   # 形如 "962,31;300,200"
  [int]$Wait = 600
)
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class C {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x,int y,int cx,int cy,uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  public static IntPtr Find() {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => { var c=new StringBuilder(256); GetClassName(h,c,256); if (c.ToString()=="RLPlayerMain" && IsWindowVisible(h)) { found=h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static void Top(IntPtr h) { SetWindowPos(h,(IntPtr)(-1),0,0,0,0,0x0001|0x0002|0x0040); SetForegroundWindow(h); }
  public static void Untop(IntPtr h) { SetWindowPos(h,(IntPtr)(-2),0,0,0,0,0x0001|0x0002); }
  public static void Click(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(80);
    mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);
    System.Threading.Thread.Sleep(50);
    mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
  }
}
'@
$h = [C]::Find()
if ($h -eq [IntPtr]::Zero) { Write-Output "ERROR: 找不到 RLPlayerMain"; exit 2 }
$wr = New-Object C+RECT
[void][C]::GetWindowRect($h, [ref]$wr)
[C]::Top($h)
Start-Sleep -Milliseconds 400
foreach ($p in ($Points -split ';')) {
  if (-not $p.Trim()) { continue }
  $xy = $p.Split(',')
  $sx = $wr.Left + [int]$xy[0]
  $sy = $wr.Top  + [int]$xy[1]
  [C]::Click($sx, $sy)
  Write-Output ("click window({0},{1}) -> screen({2},{3})" -f $xy[0], $xy[1], $sx, $sy)
  Start-Sleep -Milliseconds $Wait
}
Start-Sleep -Milliseconds 300
[C]::Untop($h)
Write-Output "done"
