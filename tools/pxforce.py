# pxforce.py — 触发重绘后立刻测量（隔离"是否根本没画"与"画了但被盖住"）
import ctypes, ctypes.wintypes as wt, sys, os, time
from PIL import ImageGrab

u32 = ctypes.WinDLL("user32", use_last_error=True)
class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long), ("bottom", ctypes.c_long)]
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(RECT)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
u32.InvalidateRect.argtypes = [wt.HWND, ctypes.c_void_p, wt.BOOL]
u32.UpdateWindow.argtypes = [wt.HWND]
u32.RedrawWindow.argtypes = [wt.HWND, ctypes.c_void_p, wt.HANDLE, ctypes.c_uint]
u32.EnumChildWindows.argtypes = [wt.HWND, EnumProc, wt.LPARAM]

def find_root():
    f = []
    def cb(h, lp):
        b = ctypes.create_unicode_buffer(128); u32.GetClassNameW(h, b, 128)
        if b.value == "RLPlayerMain" and u32.IsWindowVisible(h):
            f.append(h); return False
        return True
    u32.EnumWindows(EnumProc(cb), 0)
    return f[0] if f else None

h = find_root()
if not h:
    print("ERROR no window"); sys.exit(2)
r = RECT(); u32.GetWindowRect(h, ctypes.byref(r))
w, hh = r.right-r.left, r.bottom-r.top

def snap(label):
    img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).convert("RGB")
    def hx(x,y):
        p = img.getpixel((x,y)); return "%02X%02X%02X" % p
    ym, xm = hh//2, w//2
    print(f"== {label} ==")
    print("  LEFT   " + " ".join(f"x+{i}={hx(i,ym)}" for i in range(7)))
    print("  TOP    " + " ".join(f"y+{i}={hx(xm,i)}" for i in range(7)))
    print("  RIGHT  " + " ".join(f"r-{i}={hx(w-1-i,ym)}" for i in range(7)))
    print("  BOTTOM " + " ".join(f"b-{i}={hx(xm,hh-1-i)}" for i in range(7)))

snap("T0 before any force")
# 强制整窗重绘（含 WM_ERASEBKGND）：这是我们自己那条边带刷子的唯一执行时机
u32.InvalidateRect(h, None, True)
u32.UpdateWindow(h)
time.sleep(0.9)
snap("T1 after InvalidateRect+UpdateWindow")
u32.RedrawWindow(h, None, None, 0x0001 | 0x0004 | 0x0100)  # INVALIDATE|ERASE|ALLCHILDREN
time.sleep(0.9)
snap("T2 after RedrawWindow(ALLCHILDREN+ERASE)")
# 移动 1px 再回来：走 WM_SIZE/WM_MOVE，看边带是否被重画
u32.SetWindowPos(h, None, r.left+1, r.top, 0, 0, 0x0001|0x0004)
time.sleep(0.6)
u32.SetWindowPos(h, None, r.left, r.top, 0, 0, 0x0001|0x0004)
time.sleep(0.9)
snap("T3 after 1px move round-trip")
