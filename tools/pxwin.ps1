# pxwin.ps1 - 枚举 RLPlayerMain 的子窗口与矩形（定位是哪一层在露白）
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class E {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  public static IntPtr FindMain() {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => { var c=new StringBuilder(256); GetClassName(h,c,256); if (c.ToString()=="RLPlayerMain" && IsWindowVisible(h)) { found=h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static string Dump(IntPtr root) {
    var sb = new StringBuilder();
    RECT rr; GetWindowRect(root, out rr);
    RECT rc; GetClientRect(root, out rc);
    sb.AppendLine("ROOT rect=" + rr.Left + "," + rr.Top + "-" + rr.Right + "," + rr.Bottom + " client=" + rc.Right + "x" + rc.Bottom + " style=0x" + GetWindowLong(root,-16).ToString("X8") + " exstyle=0x" + GetWindowLong(root,-20).ToString("X8"));
    EnumChildWindows(root, (h, lp) => {
      RECT r; GetWindowRect(h, out r);
      var c = new StringBuilder(256); GetClassName(h, c, 256);
      sb.AppendLine("  CHILD hwnd=0x" + h.ToString("X") + " cls=" + c + " visible=" + IsWindowVisible(h) + " style=0x" + GetWindowLong(h,-16).ToString("X8") + " rect=" + r.Left + "," + r.Top + "-" + r.Right + "," + r.Bottom + " rel=" + (r.Left-rr.Left) + "," + (r.Top-rr.Top) + " size=" + (r.Right-r.Left) + "x" + (r.Bottom-r.Top));
      return true;
    }, IntPtr.Zero);
    return sb.ToString();
  }
}
'@
$h = [E]::FindMain()
if ($h -eq [IntPtr]::Zero) { Write-Output "ERROR not found"; exit 2 }
[E]::Dump($h)
