# pxmeasure.py — RLPlayer 内置窗口边缘逐像素测量（只读诊断）
#
# 为什么用屏幕捕获而不是 GetWindowDC/BitBlt：
#   用户看到的是【DWM 合成后的屏幕】。BitBlt(GetWindowDC) 拿的是窗口自己的重定向表面，
#   对 DWM 画的 1px 边框与子窗口的合成结果不一定是同一份数据（本轮实测出现过 B4B4B4
#   垃圾色，与屏幕不一致 ⟹ 那个判据不能反映用户看到的东西）。
#   所以判据固定为：把窗口置顶 → 屏幕捕获 → 从捕获图读像素。
#
# 用法:
#   python tools/pxmeasure.py snap  <label>
#   python tools/pxmeasure.py click <x> <y> <label>      # 窗口相对坐标点击后再捕获
#   python tools/pxmeasure.py sweep <label>              # 扫四边 + 四角 + 边界层归属
import ctypes, ctypes.wintypes as wt, sys, os, time, json
from PIL import ImageGrab, Image

u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)

class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(RECT)]
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(RECT)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, ctypes.c_uint]
u32.SetForegroundWindow.argtypes = [wt.HWND]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
u32.EnumChildWindows.argtypes = [wt.HWND, EnumProc, wt.LPARAM]

HWND_TOPMOST = wt.HWND(-1)
HWND_NOTOPMOST = wt.HWND(-2)
SWP_NOMOVE, SWP_NOSIZE, SWP_SHOWWINDOW = 0x0002, 0x0001, 0x0040

def find_root():
    found = []
    def cb(h, lp):
        buf = ctypes.create_unicode_buffer(128)
        u32.GetClassNameW(h, buf, 128)
        if buf.value == "RLPlayerMain" and u32.IsWindowVisible(h):
            found.append(h)
            return False
        return True
    u32.EnumWindows(EnumProc(cb), 0)
    return found[0] if found else None

def children(root):
    out = []
    def cb(h, lp):
        buf = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, buf, 256)
        r = RECT(); u32.GetWindowRect(h, ctypes.byref(r))
        out.append((buf.value, r.left, r.top, r.right, r.bottom))
        return True
    u32.EnumChildWindows(root, EnumProc(cb), 0)
    return out

def topmost(h, on):
    u32.SetWindowPos(h, HWND_TOPMOST if on else HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE)
    if on:
        u32.SetForegroundWindow(h)

def kind_of(h, on):
    topmost(h, on)

def click(h, wx, wy):
    r = RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    u32.SetCursorPos(r.left + wx, r.top + wy)
    time.sleep(0.08)
    u32.mouse_event(0x0002, 0, 0, 0, None)
    time.sleep(0.05)
    u32.mouse_event(0x0004, 0, 0, 0, None)

def grab(h, outdir):
    r = RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    w, hh = r.right - r.left, r.bottom - r.top
    img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True)
    return img.convert("RGB")

def hexs(im, x, y):
    p = im.getpixel((x, y))
    return "%02X%02X%02X" % p

def report(im, label):
    w, h = im.size
    print(f"== {label} == size={w}x{h}")
    xm, ym = w // 2, h // 2
    print("LEFT   " + " ".join(f"x+{i}={hexs(im,i,ym)}" for i in range(8)))
    print("TOP    " + " ".join(f"y+{i}={hexs(im,xm,i)}" for i in range(8)))
    print("BOTTOM " + " ".join(f"b-{i}={hexs(im,xm,h-1-i)}" for i in range(8)))
    print("RIGHT  " + " ".join(f"r-{i}={hexs(im,w-1-i,ym)}" for i in range(8)))
    print("-- 深度→四边主色（出现次数/总数）--")
    from collections import Counter
    for d in range(8):
        parts = []
        for nm, pts in (("L", [(d, y) for y in range(h)]),
                        ("R", [(w-1-d, y) for y in range(h)]),
                        ("T", [(x, d) for x in range(w)]),
                        ("B", [(x, h-1-d) for x in range(w)])):
            c = Counter(im.getpixel(p) for p in pts)
            (r_, g_, b_), n = c.most_common(1)[0]
            parts.append(f"{nm}:{r_:02X}{g_:02X}{b_:02X}({n}/{len(pts)})")
        print(f"  depth {d}: " + "  ".join(parts))
    print("-- 左上角 8x8 --")
    for y in range(8):
        print(f"  y+{y}: " + " ".join(hexs(im, x, y) for x in range(8)))

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "snap"
    h = find_root()
    if not h:
        print("ERROR: 找不到 RLPlayerMain 窗口")
        return 2
    outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "px")
    outdir = os.path.abspath(outdir)
    os.makedirs(outdir, exist_ok=True)
    topmost(h, True)
    time.sleep(0.45)
    try:
        if mode == "click":
            wx, wy, label = int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
            click(h, wx, wy)
            time.sleep(1.0)
            im = grab(h, outdir)
            im.save(os.path.join(outdir, label + ".png"))
            report(im, label)
            return 0
        label = sys.argv[2] if len(sys.argv) > 2 else "snap"
        im = grab(h, outdir)
        im.save(os.path.join(outdir, label + ".png"))
        r = RECT(); u32.GetWindowRect(h, ctypes.byref(r))
        print(f"window rect={r.left},{r.top}-{r.right},{r.bottom}")
        print("children:")
        for c in children(h):
            print(f"  {c[0]} rel={c[1]-r.left},{c[2]-r.top} size={c[3]-c[1]}x{c[4]-c[2]}")
        report(im, label)
        return 0
    finally:
        topmost(h, False)

sys.exit(main())
