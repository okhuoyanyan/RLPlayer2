# npalign.py — 逐像素量「/」与播放键中心的 x 坐标差（UI3 的唯一判据）
#
# 为什么不能用 getBoundingClientRect 就完事：
#   用户看到的是【渲染后的像素】。字体度量、内边距都会让 DOM 中心与视觉中心差 1-3px；
#   本项目第十五轮已吃过"视觉模型吞掉 1-4px"的亏，判据必须落在像素上。
#
# 定位方法（避免把音量条误认成播放键）：
#   播放键 = 满足 accent 色、且连通区域的外接框近似正方形（宽高都在 30~50px）的那块。
#   "/" = 时长行里一个宽度 1~8px 的独立字形组，取离播放键中心最近的那组。
import ctypes, ctypes.wintypes as wt, sys, os, time
from PIL import ImageGrab
from collections import deque

u32 = ctypes.WinDLL("user32", use_last_error=True)
class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long), ("bottom", ctypes.c_long)]
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(RECT)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
u32.SetForegroundWindow.argtypes = [wt.HWND]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
u32.EnumWindows.argtypes = [EnumProc, wt.LPARAM]

def find_root():
    f = []
    def cb(h, lp):
        b = ctypes.create_unicode_buffer(128); u32.GetClassNameW(h, b, 128)
        if b.value == "RLPlayerMain" and u32.IsWindowVisible(h):
            f.append(h); return False
        return True
    u32.EnumWindows(EnumProc(cb), 0)
    return f[0] if f else None

def click(h, wx, wy):
    r = RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    u32.SetCursorPos(r.left+wx, r.top+wy); time.sleep(0.1)
    u32.mouse_event(0x0002,0,0,0,None); time.sleep(0.05)
    u32.mouse_event(0x0004,0,0,0,None)

h = find_root()
if not h: print("ERROR no window"); sys.exit(2)
u32.SetWindowPos(h, wt.HWND(-1), 0,0,0,0, 0x0001|0x0002); u32.SetForegroundWindow(h)
time.sleep(0.6)

outdir = r"D:\RTA-hqplayer\build\px"
if len(sys.argv) > 2 and sys.argv[1] == "open":
    click(h, int(sys.argv[2]), int(sys.argv[3])); time.sleep(1.8)

r = RECT(); u32.GetWindowRect(h, ctypes.byref(r))
img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).convert("RGB")
label = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != "open" else "np"
img.save(os.path.join(outdir, "np_%s.png" % label))
w, hh = img.size
px = img.load()

def is_accent(p):
    r_, g_, b_ = p
    return b_ > 150 and (b_ - r_) > 60 and (b_ - g_) > 40

# --- 播放键：找 accent 色连通块中"外接框近似正方形 30..50px"的那块
seen = [[False]*w for _ in range(hh)]
best = None
for y in range(hh-320, hh-5):
    for x in range(0, w):
        if seen[y][x] or not is_accent(px[x,y]): continue
        q = deque([(x,y)]); seen[y][x] = True; pts = []
        while q:
            cx, cy = q.popleft(); pts.append((cx,cy))
            for dx,dy in ((1,0),(-1,0),(0,1),(0,-1)):
                nx, ny = cx+dx, cy+dy
                if 0 <= nx < w and 0 <= ny < hh and not seen[ny][nx] and is_accent(px[nx,ny]):
                    seen[ny][nx] = True; q.append((nx,ny))
        if len(pts) < 300: continue
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        bw, bh = max(xs)-min(xs)+1, max(ys)-min(ys)+1
        if 30 <= bw <= 52 and 30 <= bh <= 52 and abs(bw-bh) <= 8:
            best = (min(xs), max(xs), min(ys), max(ys), len(pts), bw, bh)
if not best:
    print("FAIL: 找不到播放键圆形按钮"); u32.SetWindowPos(h, wt.HWND(-2),0,0,0,0,0x0001|0x0002); sys.exit(3)
x0,x1,y0,y1,n,bw,bh = best
play_cx = (x0+x1)/2.0; play_cy = (y0+y1)/2.0
print("PLAY button: x %d..%d (w=%d) y %d..%d (h=%d) px=%d  center=(%.2f, %.2f)"
      % (x0,x1,bw,y0,y1,bh,n,play_cx,play_cy))

# --- "/"：在播放键【上方】的时长行里找窄字形
# 时长行的 y 范围：播放键顶部往上，跳过进度条（进度条在时长行上方 ~46px 处）
ty0 = y0 - 42
ty1 = y0 - 12
# 阈值必须够低：分隔符是 .tsep{opacity:.45}，在深色主题下实际像素只有 ~#3a4048，
# 用 ">70" 这种亮阈值会把它整条漏掉（本轮实测就是这么误判的 —— 把"总时长"的
# 某个笔画当成了 "/"，报出 25px 的假偏差）。所以按"比背景亮"来判，而不是按"够白"。
bg = px[10, ty0 - 2]
def bright(p):
    return (p[0] > bg[0] + 12) or (p[1] > bg[1] + 12) or (p[2] > bg[2] + 12)
cols = {}
for x in range(0, w):
    cnt = 0
    for y in range(ty0, ty1):
        if bright(px[x,y]): cnt += 1
    if cnt >= 4: cols[x] = cnt
groups = []
if cols:
    keys = sorted(cols)
    cur = [keys[0]]
    for x in keys[1:]:
        if x - cur[-1] <= 2: cur.append(x)
        else: groups.append(cur); cur = [x]
    groups.append(cur)
print("时长行(y=%d..%d) 字形组（像素列）:" % (ty0, ty1))
for g in groups:
    gc = (g[0]+g[-1])/2.0
    print("   x %d..%d  w=%2d  center=%7.2f  dx_to_play=%6.2f" % (g[0], g[-1], g[-1]-g[0]+1, gc, abs(gc-play_cx)))

# "/" 的特征：宽 1..8px、且是离播放键中心最近的窄组（斜杠在 12px 字号下约 4-6px 宽）
narrow = [g for g in groups if (g[-1]-g[0]+1) <= 8]
if not narrow:
    print("FAIL: 时长行没找到窄字形（/）"); u32.SetWindowPos(h, wt.HWND(-2),0,0,0,0,0x0001|0x0002); sys.exit(4)
sep = min(narrow, key=lambda g: abs(((g[0]+g[-1])/2.0) - play_cx))
sep_cx = (sep[0]+sep[-1])/2.0
dx = abs(sep_cx - play_cx)
print()
print("SEPARATOR '/': x %d..%d  center=%.2f" % (sep[0], sep[-1], sep_cx))
print("PLAY center   : %.2f" % play_cx)
print("DELTA |dx|    : %.2f px   -> %s" % (dx, "PASS (<=1px)" if dx <= 1.0 else "FAIL (>1px)"))
u32.SetWindowPos(h, wt.HWND(-2),0,0,0,0,0x0001|0x0002)
sys.exit(0 if dx <= 1.0 else 5)