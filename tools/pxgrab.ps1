# pxgrab.ps1 - 抓取真实窗口像素（CopyFromScreen 到位图，再从内存位图取像素，避免脏 screen DC）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tools\pxgrab.ps1 -Label dark-albums
param([string]$Label = "probe", [string]$Out = "build\px", [switch]$NoTop)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x,int y,int cx,int cy,uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
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
  public static void Top(IntPtr h) { SetWindowPos(h,(IntPtr)(-1),0,0,0,0,0x0001|0x0002|0x0040); SetForegroundWindow(h); }
  public static void Untop(IntPtr h) { SetWindowPos(h,(IntPtr)(-2),0,0,0,0,0x0001|0x0002); }
}
'@
$h = [W]::FindMain()
if ($h -eq [IntPtr]::Zero) { Write-Output "ERROR: 找不到 RLPlayerMain"; exit 2 }
if (-not $NoTop) { [W]::Top($h) }
Start-Sleep -Milliseconds 800
$wr = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$wr)
$cw = $wr.Right - $wr.Left
$ch = $wr.Bottom - $wr.Top
Write-Output ("== {0} == rect={1},{2} size={3}x{4}" -f $Label, $wr.Left, $wr.Top, $cw, $ch)
if (-not (Test-Path -LiteralPath $Out)) { New-Item -ItemType Directory -Path $Out -Force | Out-Null }
$bmp = New-Object System.Drawing.Bitmap($cw, $ch)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($wr.Left, $wr.Top, 0, 0, (New-Object System.Drawing.Size($cw, $ch)))
$g.Dispose()
$png = Join-Path $Out ($Label + ".png")
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$ymid = [int]($ch/2)
$xmid = [int]($cw/2)
function HG($b,$x,$y) { $c = $b.GetPixel($x,$y); return ("{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }
$sb = New-Object System.Text.StringBuilder
$o = "LEFT   "; foreach ($i in 0..7) { $o += ("x+{0}={1} " -f $i, (HG $bmp $i $ymid)) }; [void]$sb.AppendLine($o)
$o = "TOP    "; foreach ($i in 0..7) { $o += ("y+{0}={1} " -f $i, (HG $bmp $xmid $i)) }; [void]$sb.AppendLine($o)
$o = "BOTTOM "; foreach ($i in 0..7) { $o += ("b-{0}={1} " -f $i, (HG $bmp $xmid ($ch-1-$i))) }; [void]$sb.AppendLine($o)
$o = "RIGHT  "; foreach ($i in 0..7) { $o += ("r-{0}={1} " -f $i, (HG $bmp ($cw-1-$i) $ymid)) }; [void]$sb.AppendLine($o)
[void]$sb.AppendLine("-- 左上角 8x8 --")
foreach ($y in 0..7) { $o = ("y+{0}: " -f $y); foreach ($x in 0..7) { $o += ((HG $bmp $x $y) + " ") }; [void]$sb.AppendLine($o) }
[void]$sb.AppendLine("-- 中上 8x8 (x=xmid+0..7) --")
foreach ($y in 0..7) { $o = ("y+{0}: " -f $y); foreach ($dx in 0..7) { $o += ((HG $bmp ($xmid+$dx) $y) + " ") }; [void]$sb.AppendLine($o) }
[void]$sb.AppendLine("-- 中下 8x8 --")
foreach ($y in ($ch-8)..($ch-1)) { $o = ("y-{0}: " -f ($ch-1-$y)); foreach ($dx in 0..7) { $o += ((HG $bmp ($xmid+$dx) $y) + " ") }; [void]$sb.AppendLine($o) }
[void]$sb.AppendLine("-- 左中 8x8 --")
foreach ($y in $ymid..($ymid+7)) { $o = ("y+{0}: " -f ($y-$ymid)); foreach ($x in 0..7) { $o += ((HG $bmp $x $y) + " ") }; [void]$sb.AppendLine($o) }
$sb.ToString() | Out-File -LiteralPath (Join-Path $Out ($Label + ".txt")) -Encoding UTF8
$bmp.Dispose()
Write-Output $sb.ToString()
if (-not $NoTop) { [W]::Untop($h) }
Write-Output "PNG=$png"
