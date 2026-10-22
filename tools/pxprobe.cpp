// pxprobe.cpp — 逐像素测量 RLPlayer 内置 WebView2 窗口的边缘（只读诊断，不出声、不改配置）
//
// 为什么必须有这个工具（第十五轮 + 本轮 UI1 的共同教训）：
//   视觉模型会把 1-4px 的窄条「吞掉」，截图目测判断边缘一律不可靠。第十五轮的
//   「UI 最外一圈白框」就是靠 GetPixel 打点才定位到真凶（WebView2 内缩露出的边带
//   从未被绘制）。本轮用户报的是「跳转界面才出现、没有规律」，说明还有第三层，
//   同样只能靠逐像素 + 逐层隔离来定位。
//
// 为什么不用屏幕 DC 的 GetPixel：
//   GetPixel(screenDC) 读的是【合成后的桌面】—— 窗口被别的程序遮挡时读到的是别人的
//   像素。本轮第一次用 screen DC 跑，左边缘读到资源管理器/宿主窗口的颜色，数据全是
//   假的，差点得出错误结论。正确做法是 GetWindowDC(hwnd) + BitBlt：那才是本窗口
//   自己的像素，与遮挡无关。
//
// 为什么还要 --printclient：
//   光看最终像素分不清「白色是 DWM 画的」还是「我们自己 FillRect 出来的」。本工具把
//   WM_PRINTCLIENT 直接送给窗口过程（要求它把自己画进一块内存 DC），得到的就只是
//   【本程序自己的绘制结果】，不含 DWM 边框与子窗口。两者一对比，责任层就确定了。
//
// 用法：
//   pxprobe.exe --dump               枚举窗口/子窗口（类名、样式、几何）——定位是哪一层
//   pxprobe.exe --scan [label]       四边 0..7 逐像素 + 四角 8x8（主判据）
//   pxprobe.exe --printclient [lbl]  同上，但像素来自 WM_PRINTCLIENT（只含自身绘制）
//   pxprobe.exe --pt X Y             单点
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace {

HWND g_root = nullptr;

BOOL CALLBACK find_root_proc(HWND h, LPARAM) {
    wchar_t cls[128] = {0};
    GetClassNameW(h, cls, 127);
    if (wcscmp(cls, L"RLPlayerMain") == 0 && IsWindowVisible(h)) { g_root = h; return FALSE; }
    return TRUE;
}

HWND find_root() {
    g_root = nullptr;
    EnumWindows(find_root_proc, 0);
    return g_root;
}

struct Shot {
    std::vector<unsigned char> rgb;   // 3 bytes/px
    int w = 0, h = 0;
    bool ok() const { return w > 0 && h > 0 && rgb.size() == (size_t)w * (size_t)h * 3; }
    std::string hex(int x, int y) const {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= w) x = w - 1;
        if (y >= h) y = h - 1;
        const size_t i = ((size_t)y * (size_t)w + (size_t)x) * 3;
        char s[16];
        std::snprintf(s, sizeof(s), "%02X%02X%02X", rgb[i], rgb[i + 1], rgb[i + 2]);
        return s;
    }
};

void read_bits(HDC mdc, HBITMAP bmp, int w, int h, Shot& out) {
    out.w = w; out.h = h;
    out.rgb.assign((size_t)w * (size_t)h * 3, 0);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> raw((size_t)w * (size_t)h * 4);
    if (!GetDIBits(mdc, bmp, 0, (UINT)h, raw.data(), &bi, DIB_RGB_COLORS)) { out.w = out.h = 0; return; }
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; ++i) {
        out.rgb[i * 3 + 0] = raw[i * 4 + 2];
        out.rgb[i * 3 + 1] = raw[i * 4 + 1];
        out.rgb[i * 3 + 2] = raw[i * 4 + 0];
    }
}

// 窗口自身像素（含 DWM 画的非客户区 + 子窗口 WebView2）
bool grab_window(HWND h, Shot& out) {
    RECT rc{};
    if (!GetWindowRect(h, &rc)) return false;
    const int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return false;
    HDC wdc = GetWindowDC(h);       // 不是 screen DC —— 见文件头注
    if (!wdc) return false;
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, ht);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const BOOL blt = BitBlt(mdc, 0, 0, w, ht, wdc, 0, 0, SRCCOPY | CAPTUREBLT);
    if (blt) read_bits(mdc, bmp, w, ht, out);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(h, wdc);
    return out.ok();
}

// 只有「本窗口过程自己的绘制」：把 WM_PRINTCLIENT 送进去，不含 DWM 边框、不含子窗口
bool grab_printclient(HWND h, Shot& out) {
    RECT rc{};
    if (!GetClientRect(h, &rc)) return false;
    const int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return false;
    HDC hdc = GetDC(h);
    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, ht);
    HGDIOBJ old = SelectObject(mdc, bmp);
    RECT full{0, 0, w, ht};
    // 先刷成「已知的哨兵色」，这样没被画到的地方能一眼看出来（而不是黑或白去冒充结论）
    HBRUSH sentinel = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(mdc, &full, sentinel);
    DeleteObject(sentinel);
    SendMessageW(h, WM_PRINTCLIENT, (WPARAM)mdc, PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
    read_bits(mdc, bmp, w, ht, out);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(h, hdc);
    return out.ok();
}


// 存 24-bit BMP（给 Python 做全图数值分析用；PNG 需要外部库，BMP 手写即可）
bool save_bmp(const Shot& s, const char* path) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    const size_t stride = (((size_t)s.w * 3) + 3) & ~(size_t)3;
    const size_t imgBytes = stride * (size_t)s.h;
    ih.biSize = sizeof(BITMAPINFOHEADER);
    ih.biWidth = s.w;
    ih.biHeight = s.h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)imgBytes;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + (DWORD)imgBytes;
    std::vector<unsigned char> buf(imgBytes, 0);
    for (int y = 0; y < s.h; ++y) {
        for (int x = 0; x < s.w; ++x) {
            const size_t si = ((size_t)y * (size_t)s.w + (size_t)x) * 3;
            const size_t di = (size_t)(s.h - 1 - y) * stride + (size_t)x * 3;
            buf[di + 0] = s.rgb[si + 2];   // B
            buf[di + 1] = s.rgb[si + 1];   // G
            buf[di + 2] = s.rgb[si + 0];   // R
        }
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    return true;
}

void print_dump(HWND root) {
    RECT rr{}; GetWindowRect(root, &rr);
    RECT rc{}; GetClientRect(root, &rc);
    printf("ROOT hwnd=0x%p rect=%ld,%ld-%ld,%ld client=%ldx%ld style=0x%08lX exstyle=0x%08lX\n",
           (void*)root, rr.left, rr.top, rr.right, rr.bottom, rc.right, rc.bottom,
           (unsigned long)GetWindowLongW(root, GWL_STYLE),
           (unsigned long)GetWindowLongW(root, GWL_EXSTYLE));
    struct Ctx { RECT rr; } ctx{ rr };
    EnumChildWindows(root, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        RECT r{}; GetWindowRect(h, &r);
        wchar_t cls[256] = {0}; GetClassNameW(h, cls, 255);
        printf("  CHILD hwnd=0x%p cls=%ls vis=%d style=0x%08lX ex=0x%08lX rel=%ld,%ld size=%ldx%ld\n",
               (void*)h, cls, (int)IsWindowVisible(h),
               (unsigned long)GetWindowLongW(h, GWL_STYLE),
               (unsigned long)GetWindowLongW(h, GWL_EXSTYLE),
               r.left - c->rr.left, r.top - c->rr.top, r.right - r.left, r.bottom - r.top);
        return TRUE;
    }, (LPARAM)&ctx);
}

void print_scan(const Shot& s, const char* kind, const char* label) {
    printf("== %s [%s] == size=%dx%d\n", label, kind, s.w, s.h);
    const int xm = s.w / 2, ym = s.h / 2;
    printf("LEFT   ");
    for (int i = 0; i < 8; ++i) printf("x+%d=%s ", i, s.hex(i, ym).c_str());
    printf("\nTOP    ");
    for (int i = 0; i < 8; ++i) printf("y+%d=%s ", i, s.hex(xm, i).c_str());
    printf("\nBOTTOM ");
    for (int i = 0; i < 8; ++i) printf("b-%d=%s ", i, s.hex(xm, s.h - 1 - i).c_str());
    printf("\nRIGHT  ");
    for (int i = 0; i < 8; ++i) printf("r-%d=%s ", i, s.hex(s.w - 1 - i, ym).c_str());
    printf("\n");
    // 四边「前 8px 一条线」：沿边采样，看是否有 1px 异常贯穿整条边
    printf("EDGE-LEFT-COLUMN  y=100,300,500,700,860 : ");
    for (int y : {100, 300, 500, 700, 860}) {
        printf("| ");
        for (int x = 0; x < 6; ++x) printf("%s ", s.hex(x, y).c_str());
    }
    printf("\nEDGE-TOP-ROW      x=100,400,700,1000,1300 : ");
    for (int x : {100, 400, 700, 1000, 1300}) {
        printf("| ");
        for (int y = 0; y < 6; ++y) printf("%s ", s.hex(x, y).c_str());
    }
    printf("\n-- 左上角 8x8 --\n");
    for (int y = 0; y < 8; ++y) {
        printf("y+%d: ", y);
        for (int x = 0; x < 8; ++x) printf("%s ", s.hex(x, y).c_str());
        printf("\n");
    }
    printf("-- 右下角 8x8 --\n");
    for (int y = 7; y >= 0; --y) {
        printf("y-%d: ", y);
        for (int x = 7; x >= 0; --x) printf("%s ", s.hex(s.w - 1 - x, s.h - 1 - y).c_str());
        printf("\n");
    }
}


// ---- 点击（真实鼠标事件）：复现用户操作后立刻逐像素复测 ----
// 为什么不用 CDP：要验的是【真实宿主窗口】的像素（含 DWM 边框与 WebView2 子窗口），
// 而不是浏览器里的页面 —— CDP 截图看不到宿主和边带。
static void do_click(HWND h, int wx, int wy) {
    RECT rc{};
    GetWindowRect(h, &rc);
    const int sx = rc.left + wx, sy = rc.top + wy;
    SetCursorPos(sx, sy);
    Sleep(80);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(50);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    printf("click window(%d,%d) -> screen(%d,%d)\n", wx, wy, sx, sy);
}

static void bring_top(HWND h, bool top) {
    SetWindowPos(h, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (top) SetForegroundWindow(h);
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    HWND h = find_root();
    if (!h) { printf("ERROR: 找不到 RLPlayerMain 窗口（启动器没开？）\n"); return 2; }
    const std::string mode = (argc > 1) ? argv[1] : "--scan";
    const char* label = (argc > 2) ? argv[2] : "-";
    if (mode == "--dump") { print_dump(h); return 0; }
    // --click X Y [waitMs]：真实鼠标点击（窗口相对坐标），用于复现"跳转界面"这类操作
    if (mode == "--click" && argc > 3) {
        const int wx = atoi(argv[2]), wy = atoi(argv[3]);
        const int waitMs = (argc > 4) ? atoi(argv[4]) : 700;
        bring_top(h, true);
        Sleep(350);
        do_click(h, wx, wy);
        Sleep(waitMs);
        bring_top(h, false);
        return 0;
    }
    // --clickscan X Y label [waitMs]：点完立刻测量（一条命令拿到"操作后"的像素）
    if (mode == "--clickscan" && argc > 4) {
        const int wx = atoi(argv[2]), wy = atoi(argv[3]);
        const int waitMs = (argc > 5) ? atoi(argv[5]) : 900;
        bring_top(h, true);
        Sleep(350);
        do_click(h, wx, wy);
        Sleep(waitMs);
        Shot s2;
        if (!grab_window(h, s2)) { printf("ERROR: grab failed\n"); bring_top(h, false); return 3; }
        print_scan(s2, "window-after-click", argv[4]);
        bring_top(h, false);
        return 0;
    }
    Shot s;
    if (mode == "--printclient") {
        if (!grab_printclient(h, s)) { printf("ERROR: WM_PRINTCLIENT 抓图失败\n"); return 3; }
        print_scan(s, "printclient", label);
        return 0;
    }
    // --bmp <path> [label]：存 BMP 给 Python 做全图统计（判据仍以数值为准）
    if (mode == "--bmp" && argc > 2) {
        Shot b;
        bring_top(h, true);
        Sleep(500);
        const bool ok = grab_window(h, b) && save_bmp(b, argv[2]);
        bring_top(h, false);
        printf("bmp %s -> %s (%dx%d)\n", argv[2], ok ? "ok" : "fail", b.w, b.h);
        if (ok && argc > 3) print_scan(b, "bmp", argv[3]);
        return ok ? 0 : 5;
    }
    if (mode == "--top") {
        // 置顶后测量（排除遮挡干扰；GetWindowDC 本身不受遮挡影响，这是双保险）
        bring_top(h, true);
        Sleep(400);
        Shot t;
        const bool ok = grab_window(h, t);
        if (ok) print_scan(t, "window-topmost", label);
        bring_top(h, false);
        return ok ? 0 : 3;
    }
    if (mode == "--pt" && argc > 3) {
        if (!grab_window(h, s)) { printf("ERROR: 抓窗口失败\n"); return 3; }
        printf("pt(%s,%s)=%s\n", argv[2], argv[3], s.hex(atoi(argv[2]), atoi(argv[3])).c_str());
        return 0;
    }
    if (!grab_window(h, s)) { printf("ERROR: 抓窗口失败\n"); return 3; }
    print_scan(s, "window", label);
    return 0;
}