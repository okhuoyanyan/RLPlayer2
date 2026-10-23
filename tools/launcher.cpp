// launcher.cpp — RLPlayer 桌面启动器（Win32 GUI，双击即用）
//
// 行为：
//   1. 若已有实例在端口上运行，直接打开浏览器
//   2. 否则以隐藏方式启动同目录的 rtaweb.exe，等端口就绪后打开浏览器
//   3. 常驻托盘图标，右键菜单：打开界面 / 复制地址 / 退出
//   4. 退出时终止自己启动的子进程
//
// 不使用 PowerShell，不弹控制台窗口。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "wv2_host.h"
#include "version.h"               // RLPLAYER_VERSION（单一版本号真相源）
#include "crash_dump.h"            // 崩溃转储 + OS/CPU/tester 诊断（1.0.5 可观测性）
#include "audio/wasapi_out.h"      // 输出端点数（横幅用，只读枚举，不开设备）

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#define WM_TRAYICON  (WM_APP + 1)
#define ID_TRAY_OPEN  1001
#define ID_TRAY_COPY  1002
#define ID_TRAY_EXIT  1003
#define ID_TRAY_LAN   1004

static const wchar_t* kAppName = L"RLPlayer";
static const int kDefaultPort = 8899;
static const int kTrayId = 1;

static void logf(const wchar_t* fmt, ...);   // 前置声明（open_browser 里也用）
static void darken_edge_window(DWORD pid, DWORD waitMs);   // 定义在下面
static void remember_edge_pid(DWORD pid);                 // 定义在下面

static int   g_port = 0;
static HANDLE g_child = nullptr;
static NOTIFYICONDATAW g_nid{};
static const char* const g_buildTime = __DATE__ " " __TIME__;   // 启动横幅用

// ---------------- 工具 ----------------
// 非阻塞 connect + select：阻塞式 connect 在没有监听者时可能长期挂住，
// 会让启动器卡死在「探测端口」这一步（本轮踩到）。
static bool port_open(int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    ::connect(s, (sockaddr*)&a, sizeof(a));

    fd_set wf, ef;
    FD_ZERO(&wf); FD_SET(s, &wf);
    FD_ZERO(&ef); FD_SET(s, &ef);
    timeval tv{ 0, 150000 };     // 150ms
    const int r = ::select(0, nullptr, &wf, &ef, &tv);
    bool ok = false;
    if (r > 0 && FD_ISSET(s, &wf)) {
        int err = 0; int len = sizeof(err);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        ok = (err == 0);
    }
    ::closesocket(s);
    return ok;
}

// 探一次 /api/ping，判断这个端口上跑的是不是本程序的服务。
// 只看"端口通不通"会把任何监听者都当成自己。
static bool probe_app(int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD to = 1200;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) { ::closesocket(s); return false; }
    const char* req = "GET /api/ping HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (::send(s, req, (int)strlen(req), 0) <= 0) { ::closesocket(s); return false; }
    std::string resp;
    char buf[1024];
    for (;;) {
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, (size_t)n);
        if (resp.size() > 4096) break;
    }
    ::closesocket(s);
    return resp.find("RLPlayer") != std::string::npos;
}

static std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? L"." : p.substr(0, s);
}

// UTF-8 窄串 → 宽串（横幅里拼 OS/CPU 品牌用）
static std::wstring utf8_to_wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"?";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

// 用 Edge 的 --app 模式打开：无地址栏、无标签页、独立任务栏条目，
// 看起来就是一个原生应用窗口，而不是一个浏览器标签。
// 找不到 Edge 时才退回默认浏览器。
// 设置文件里的主题。Edge 的原生标题栏颜色只能由浏览器自身的深浅主题决定，
// 所以启动窗口前必须先知道主题是深还是浅。
static bool theme_is_dark() {
    FILE* f = nullptr;
    const std::wstring path = exe_dir() + L"\\rlplayer-settings.conf";
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return true;   // 默认深色
    char buf[4096] = {0};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    const std::string s(buf, n);
    const size_t p = s.find("theme=");
    if (p == std::string::npos) return true;
    size_t e = s.find_first_of("\r\n", p);
    if (e == std::string::npos) e = s.size();
    const std::string v = s.substr(p + 6, e - (p + 6));
    return !(v == "light" || v == "blue");
}

// 三套主题的实际档位。启动时页面还没加载，只能读 conf；运行期以网页发来的
// theme:xxx 消息为准（那条路径更及时），轮询只是兜底。
static void theme_flags(bool& dark, bool& blue) {
    dark = theme_is_dark();
    blue = false;
    if (dark) return;
    FILE* f = nullptr;
    const std::wstring path = exe_dir() + L"\rlplayer-settings.conf";
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
    char buf[4096] = {0};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    const std::string s(buf, n);
    const size_t p = s.find("theme=");
    if (p == std::string::npos) return;
    size_t e = s.find_first_of("\r\n", p);
    if (e == std::string::npos) e = s.size();
    blue = (s.substr(p + 6, e - (p + 6)) == "blue");
}

static void open_browser(int port) {
    wchar_t url[64];
    swprintf_s(url, L"http://127.0.0.1:%d/", port);

    const wchar_t* cand[] = {
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    for (const wchar_t* exe : cand) {
        if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) continue;
        // 独立的用户数据目录：只有这样 Edge 才会用【我们给的参数】新起一个浏览器进程。
        // 共享用户 profile 时，命令会被转发给已经在跑的 Edge，--force-dark-mode 被忽略，
        // 标题栏就还是白的（本项目实测）。
        // 深浅两套 profile 分开：Edge 会把新命令转发给"同 profile 的已运行进程"，
        // 那样的老进程还带着旧主题的边框参数。两套目录 === 两种窗口外观，互不干扰。
        const bool dark = theme_is_dark();
        const std::wstring prof = exe_dir() + (dark ? L"\\edge-app-dark" : L"\\edge-app-light");
        wchar_t arg[1024];
        swprintf_s(arg, L"\"%s\" --app=%s --user-data-dir=\"%s\" "
                         L"--window-size=1340,880 --window-position=120,60 %s"
                         L"--disable-features=Translate,msEdgeTranslate --no-first-run --no-default-browser-check",
                   exe, url, prof.c_str(), dark ? L"--force-dark-mode " : L"");
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, arg, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            logf(L"已用 Edge app 模式打开界面（主题 %s）", dark ? L"深色" : L"浅色");
            remember_edge_pid(pi.dwProcessId);   // 下次只认这个 PID，绝不按标题乱杀
            CloseHandle(pi.hThread);
            darken_edge_window(pi.dwProcessId, 4000);   // 其他浏览器做兜底
            CloseHandle(pi.hProcess);
            return;
        }
    }
    logf(L"未找到 Edge，退回默认浏览器");
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// 关掉上一次由【本程序】启动的界面进程。
//
// 这里刻意【不再】按窗口标题找窗口：上一版用"标题含 RLPlayer + 类名含 Chrome_WidgetWin"
// 去枚举并 TerminateProcess，结果把同样基于 Chromium 的 DSH 宿主当成了旧窗口杀掉
//（宿主窗口标题里也带项目名），两次把 Agent 自己带走。
// 现在只认一个来源：本程序 CreateProcess 时记下的 PID，且必须确认映像名是 msedge.exe。
static std::wstring edge_pid_path() { return exe_dir() + L"\\edge-app.pid"; }

static void remember_edge_pid(DWORD pid) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, edge_pid_path().c_str(), L"wb") == 0 && f) {
        fprintf(f, "%lu\n", (unsigned long)pid);
        fclose(f);
    }
}
static DWORD read_edge_pid() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, edge_pid_path().c_str(), L"rb") != 0 || !f) return 0;
    unsigned long v = 0;
    if (fscanf(f, "%lu", &v) != 1) v = 0;
    fclose(f);
    return (DWORD)v;
}
// 只认 msedge.exe。别的一律不动 —— 这是防"杀错进程"的最后一道闸。
static bool is_msedge(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH] = {0};
    DWORD n = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
    CloseHandle(h);
    if (!ok) return false;
    const std::wstring s(path);
    return s.size() >= 10 && _wcsicmp(s.c_str() + s.size() - 10, L"msedge.exe") == 0;
}
static int close_app_window() {
    const DWORD pid = read_edge_pid();
    if (!pid) return 0;
    int closed = 0;
    if (is_msedge(pid)) {
        HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (hp) {
            if (WaitForSingleObject(hp, 0) != WAIT_OBJECT_0) {
                logf(L"  结束上一次的界面进程 pid=%lu（确认是 msedge.exe）", (unsigned long)pid);
                TerminateProcess(hp, 0);
                WaitForSingleObject(hp, 2000);
                closed = 1;
            }
            CloseHandle(hp);
        }
    } else if (pid) {
        logf(L"  上一次记录的 pid=%lu 已不是 msedge.exe，跳过（不杀）", (unsigned long)pid);
    }
    _wremove(edge_pid_path().c_str());
    return closed;
}

// 把 Edge 应用窗口的标题栏/边框改成深色。
// 只设网页的 theme-color 管不到系统标题栏（那是 DWM 画的），
// 所以这里用 DwmSetWindowAttribute 直接改：
//   20 = DWMWA_USE_IMMERSIVE_DARK_MODE（Win10 1809+）
//   35 = DWMWA_CAPTION_COLOR、36 = DWMWA_BORDER_COLOR（Win11 22000+，可给具体颜色）
static void darken_edge_window(DWORD pid, DWORD waitMs) {
    typedef HRESULT(WINAPI* PFN_SET)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    auto setAttr = (PFN_SET)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!setAttr) { FreeLibrary(dwm); return; }

    HWND target = nullptr;
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < waitMs) {
        struct Ctx { DWORD pid; HWND found; } ctx{ pid, nullptr };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp;
            DWORD p = 0; GetWindowThreadProcessId(h, &p);
            if (p != c->pid) return TRUE;
            if (!IsWindowVisible(h)) return TRUE;
            if (GetWindow(h, GW_OWNER)) return TRUE;
            wchar_t cls[64] = {0};
            GetClassNameW(h, cls, 63);
            if (!wcsstr(cls, L"Chrome_WidgetWin")) return TRUE;
            c->found = h; return FALSE;
        }, (LPARAM)&ctx);
        target = ctx.found;
        if (target) break;
        // Edge 经常"启动器进程立刻退出、真正的浏览器进程是别人"，按 PID 找不到。
        // 退回按【窗口标题】找：应用窗口的标题就是网页标题（RLPlayer）。
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(h)) return TRUE;
            if (GetWindow(h, GW_OWNER)) return TRUE;
            wchar_t cls[64] = {0};
            GetClassNameW(h, cls, 63);
            if (!wcsstr(cls, L"Chrome_WidgetWin")) return TRUE;
            wchar_t title[256] = {0};
            GetWindowTextW(h, title, 255);
            if (!wcsstr(title, L"RLPlayer")) return TRUE;
            *(HWND*)lp = h;
            return FALSE;
        }, (LPARAM)&target);
        if (target) break;
        Sleep(100);
    }
    if (!target) { logf(L"  深色标题栏: 没找到 Edge 窗口"); FreeLibrary(dwm); return; }

    BOOL dark = TRUE;
    // Win10 1809~1909 用 19，2004+ 用 20；两个都设，失败的那个会被忽略
    HRESULT h19 = setAttr(target, 19, &dark, sizeof(dark));
    HRESULT h20 = setAttr(target, 20, &dark, sizeof(dark));
    const COLORREF cap = RGB(0x0c, 0x0e, 0x13);   // 与界面顶栏同色
    const COLORREF bd  = RGB(0x0a, 0x0b, 0x0f);
    HRESULT h35 = setAttr(target, 35, &cap, sizeof(cap));
    HRESULT h36 = setAttr(target, 36, &bd, sizeof(bd));
    // 属性改完必须让窗口重画边框，否则要等下次激活才看得到
    SetWindowPos(target, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(target, nullptr, TRUE);
    RECT rc{};
    GetWindowRect(target, &rc);
    logf(L"  深色标题栏: hwnd=0x%p attr19=0x%08lX attr20=0x%08lX cap=0x%08lX border=0x%08lX rect=(%ld,%ld %ldx%ld)",
         (void*)target, (unsigned long)h19, (unsigned long)h20, (unsigned long)h35, (unsigned long)h36,
         rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    FreeLibrary(dwm);
}

static void copy_address(int port) {
    wchar_t url[64];
    swprintf_s(url, L"http://127.0.0.1:%d/", port);
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const size_t bytes = (wcslen(url) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* d = GlobalLock(h);
        if (d) { memcpy(d, url, bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    }
    CloseClipboard();
}

static void tray_balloon(const wchar_t* title, const wchar_t* msg) {
    g_nid.uFlags |= NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, msg, _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
}

// ---------------- 启动服务 ----------------
static void logf(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    // 日志写在启动器同目录，便于排障。正常情况下只有几行。
    const std::wstring path = exe_dir() + L"\\launcher.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", buf);
        fclose(f);
    }
}

// 局域网开关存在同目录的 rlplayer-net.conf 里，重启服务生效（网页端不暴露这个开关，
// 因为网页本身没有鉴权，谁能连上谁就能控制播放）
static std::wstring net_conf_path() { return exe_dir() + L"\\rlplayer-net.conf"; }
static bool lan_enabled() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, net_conf_path().c_str(), L"rb") != 0 || !f) return false;
    char buf[128] = {0};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return std::string(buf, n).find("lan=1") != std::string::npos;
}
static void set_lan(bool on) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, net_conf_path().c_str(), L"wb") == 0 && f) {
        const std::string s = on ? (std::string("lan=1") + (char)10) : (std::string("lan=0") + (char)10);
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
}

static bool spawn_server(int port) {
    const std::wstring exe = exe_dir() + L"\\rtaweb.exe";
    logf(L"spawn_server: exe=%s", exe.c_str());
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logf(L"  !! rtaweb.exe 找不到");
        return false;
    }

    wchar_t cmd[512];
    if (lan_enabled()) swprintf_s(cmd, L"\"%s\" --port %d --lan", exe.c_str(), port);
    else               swprintf_s(cmd, L"\"%s\" --port %d", exe.c_str(), port);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, exe_dir().c_str(), &si, &pi)) {
        logf(L"  !! CreateProcess 失败, GetLastError=%lu", GetLastError());
        return false;
    }
    logf(L"  CreateProcess OK, child PID=%lu", pi.dwProcessId);
    CloseHandle(pi.hThread);
    g_child = pi.hProcess;
    return true;
}

// WebView2 不铺满窗口、四周留的像素数：留给原生缩放命中的热区（WM_NCHITTEST 要有机会拿到鼠标）。
// 这段边带由宿主自己上色 —— 上色失败就是用户看到的"白边框"，所以三个地方
//（resize / WM_ERASEBKGND / WM_NCPAINT）必须用同一个值，不许各写一个 5。
static const int kBandInset = 5;

// ---------------- 高 DPI 支持（2026-10-23） ----------------
// 之前进程是 DPI-UNAWARE：Windows 把整个窗口按 96 DPI 位图拉伸，WebView2 的内容
// 随之被放大 —— 5K/200% 屏上界面就是糊的（实测 GetProcessDpiAwareness() 返回 0）。
// 顺序：PerMonitorV2（Win10 1703+）→ PerMonitor（Win8.1+）→ System（Vista+；Win7 走这条）。
// 一律动态取函数地址：Win7 没有 SetProcessDpiAwarenessContext / GetDpiForWindow。
static void enable_high_dpi() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL(WINAPI* FnCtx)(HANDLE);
        auto f = (FnCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (f && f((HANDLE)(LONG_PTR)-4)) return;    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if (f && f((HANDLE)(LONG_PTR)-3)) return;    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
    }
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    if (sh) {
        typedef HRESULT(WINAPI* FnAw)(int);
        auto f2 = (FnAw)GetProcAddress(sh, "SetProcessDpiAwareness");
        if (f2 && SUCCEEDED(f2(2))) return;          // PROCESS_PER_MONITOR_DPI_AWARE
    }
    SetProcessDPIAware();                            // Vista+ 兜底
}

// 某个窗口当前的 DPI（Win10 用 GetDpiForWindow；Win7/8 退回系统 DPI）
static UINT dpi_of(HWND hwnd) {
    if (hwnd) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            typedef UINT(WINAPI* FnGdw)(HWND);
            auto f = (FnGdw)GetProcAddress(u32, "GetDpiForWindow");
            if (f) { const UINT d = f(hwnd); if (d) return d; }
        }
    }
    HDC dc = GetDC(hwnd);
    const UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96u;
    if (dc) ReleaseDC(hwnd, dc);
    return d ? d : 96u;
}

// 系统（主屏）DPI —— 用于窗口创建那一刻，那时还没有窗口可问
static UINT dpi_system() {
    HDC dc = GetDC(nullptr);
    const UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96u;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96u;
}

// 逻辑像素 → 当前窗口的物理像素（kBandInset、初始窗口尺寸都走它换算）
static int dp(HWND hwnd, int v) { return (int)(((long long)v * dpi_of(hwnd) + 48) / 96); }

// ---------------- 内置 WebView2 窗口 ----------------
// 用系统运行库的 WebView2 做界面：没有浏览器外壳、没有扩展、标题栏归我们管，
// 主题一变直接改 DWM 颜色，不需要重开窗口。
static HWND g_mainWnd = nullptr;
static ::ICoreWebView2Controller* g_wvCtl = nullptr;
static ::ICoreWebView2* g_wv = nullptr;
static bool g_captionDark = true;
static bool g_captionBlue = false;   // 三套主题里"浅蓝"单独一档（--bg #e9f2ff 与浅色 #f4f6f9 不同色）
// 运行期主题的唯一真相源是【网页】—— 网页知道真主题（localStorage 里的选择优先于 conf），
// 用户点主题按钮时它会立刻发 theme:xxx 消息。conf 文件只在"网页还没发消息"时当启动默认值。
// 不收这个标志的话，1.5 秒轮询会拿 conf 里的旧值把网页刚设的主题覆盖回去：
// 实测 launcher.log 出现「wvmsg: theme:blue」紧跟「标题栏跟随主题 -> 深色」，
// 于是浅蓝页面配了深色边带（2026-10-22 逐像素发现：边带 08090C vs 页面 --bg E9F2FF）。
static bool g_themeFromPage = false;
static HBRUSH g_bandBrush = nullptr;   // WebView2 内缩边带的填充刷(随主题换色,见 apply_caption 注)
// 三套主题的页面 --bg 原值（必须与 ui_page.h 的 CSS 变量逐字节一致）：
// 边带只有刷成【页面 --bg】才真正看不出来。只分深/浅两档时，浅蓝主题会露出
// 一条 #f4f6f9 的浅灰边（与 --bg #e9f2ff 不同色），那也是一圈看得见的框。
static const COLORREF kBgDark  = RGB(0x08, 0x09, 0x0c);   // --bg: #08090c
static const COLORREF kBgLight = RGB(0xf4, 0xf6, 0xf9);   // --bg: #f4f6f9
static const COLORREF kBgBlue  = RGB(0xe9, 0xf2, 0xff);   // --bg: #e9f2ff

static COLORREF theme_bg(bool dark, bool blue) {
    if (dark) return kBgDark;
    return blue ? kBgBlue : kBgLight;
}
static void update_band(COLORREF bg) {
    if (g_bandBrush) DeleteObject(g_bandBrush);
    g_bandBrush = CreateSolidBrush(bg);
}

static void apply_caption(HWND h, bool dark, bool blue = false) {
    HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT(WINAPI* PFN_SET)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttr = (PFN_SET)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (setAttr) {
        BOOL d = dark ? TRUE : FALSE;
        setAttr(h, 19, &d, sizeof(d));       // Win10 1809~1909
        setAttr(h, 20, &d, sizeof(d));       // Win10 2004+
        const COLORREF bg = theme_bg(dark, blue);
        COLORREF cap = bg;
        COLORREF txt = dark ? RGB(0xe9, 0xed, 0xf5) : RGB(0x0f, 0x17, 0x2a);
        COLORREF bd  = bg;
        setAttr(h, 35, &cap, sizeof(cap));   // Win11 标题栏底色
        setAttr(h, 36, &txt, sizeof(txt));   // Win11 标题文字色(36=DWMWA_TEXT_COLOR)
        // 边框(34=DWMWA_BORDER_COLOR):不能用 COLOR_NONE —— Win11 24H2+ 对带
        // WS_THICKFRAME 的窗口不认 COLOR_NONE,会回落成系统默认亮色,在深色主题
        // 外面画一圈白框(2026-10-22 实测截图定位)。改设与标题栏同色的显式色,
        // 1px 同色边视觉上等于消失,浅色主题同理。
        setAttr(h, 34, &bd, sizeof(bd));     // DWMWA_BORDER_COLOR = 主题同色
        DWORD corner = 2;                    // DWMWCP_ROUND
        setAttr(h, 33, &corner, sizeof(corner));
    }
    FreeLibrary(dwm);
    // 边带(WebView2 内缩 5px 露出的宿主区域)同刷主题色:第十二轮起窗口类没有
    // 背景刷也没有 WM_PAINT,这片区域从未绘制,显示成创建时的垃圾像素(1px 白
    // +3px 灰)——这正是 1.0.4 起"UI 最外一圈白框"的真凶(2026-10-22 逐像素
    // 测量定位:x=1 FFFFFF、x=2..4 B4B4B4)。颜色取页面 --bg,与内容近似无缝。
    update_band(theme_bg(dark, blue));
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // ★ 2026-10-22（本轮 UI1）：这里原来只有 InvalidateRect(h, nullptr, TRUE)，
    //   它只是"标记脏"，真正擦除要等下一次 BeginPaint。而实测这条路径上
    //   WM_ERASEBKGND 的 wParam 是 -1（不是有效 HDC），FillRect 直接失败 ⟹ 边带
    //   一直没被画过（详见 main_wnd_proc 里 WM_ERASEBKGND 的注）。
    //   换成 RedrawWindow(…|RDW_ERASE|RDW_UPDATENOW) 同步擦一遍，主题一改立刻正确。
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

// 边缘缺陷排查开关：设环境变量 RLPLAYER_TRACE_PAINT=1 才打日志（不影响正常运行）
static bool trace_paint() {
    static int v = -1;
    if (v < 0) {
        wchar_t b[8] = {0};
        v = (GetEnvironmentVariableW(L"RLPLAYER_TRACE_PAINT", b, 8) > 0 && b[0] == L'1') ? 1 : 0;
    }
    return v == 1;
}

// 把 WebView2 内缩露出的边带（4 条矩形）刷成当前主题的页面 --bg。
//
// ★ 为什么必须有一个可反复调用的函数（本轮 UI1 的真根因）
//   实测证据链（RLPLAYER_TRACE_PAINT=1 的 launcher.log + 逐像素快照）：
//   1) 跳转页面那次点击会让窗口收到 WM_ACTIVATE / WM_NCACTIVATE，
//      但【完全没有 WM_PAINT / WM_ERASEBKGND / WM_NCPAINT】；
//   2) 这之后四边 x+1..x+4 立刻变成 FFFFFF + B4B4B4×3（未初始化表面内容）；
//   3) 手动发一次 InvalidateRect + UpdateWindow 就恢复正常。
//   ⟹ DWM 在激活/表面重分配时把窗口的重定向表面重建了，我们那次 GDI 填充随之蒸发，
//     而"填充"只挂在 WM_PAINT/WM_ERASEBKGND 上，那两条消息压根不来。
//   所以把填充抽成 repaint_band()，在"表面可能被重建"的每条消息里都补一次。
static void repaint_band(HWND hwnd) {
    if (!g_bandBrush) update_band(theme_bg(g_captionDark, g_captionBlue));
    HDC dc = GetWindowDC(hwnd);   // 用窗口 DC：边带是客户区，窗口 DC 原点与客户区一致
    if (!dc) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int b = dp(hwnd, kBandInset);
    const RECT topR   { rc.left, rc.top, rc.right, rc.top + b };
    const RECT botR   { rc.left, rc.bottom - b, rc.right, rc.bottom };
    const RECT leftR  { rc.left, rc.top + b, rc.left + b, rc.bottom - b };
    const RECT rightR { rc.right - b, rc.top + b, rc.right, rc.bottom - b };
    FillRect(dc, &topR, g_bandBrush);
    FillRect(dc, &botR, g_bandBrush);
    FillRect(dc, &leftR, g_bandBrush);
    FillRect(dc, &rightR, g_bandBrush);
    ReleaseDC(hwnd, dc);
}

// 校验式自愈：抽 4 个边带像素，只要有一个不是主题色就重刷。
//
// 为什么还需要这一层（上面的 repaint_band 已经覆盖了大部分时机）：
//   实测第一次跳转（专辑→艺术家）时，页面加载完成后的那次 WebView2 合成
//   发生在 WM_ACTIVATE 之后 —— 我们的填充被之后的画面提交覆盖掉了，而那之后
//   Windows 不再送 WM_PAINT/WM_NCPAINT。也就是说"最后一个动笔画边带的人"未必是
//   我们。既然无法穷举所有提交时机，就用最低成本的办法收口：定时抽查像素，
//   不对就补一刀。四条 FillRect 的代价可以忽略，且只有真的不对时才画（无闪烁）。
static void verify_band(HWND hwnd) {
    if (!g_bandBrush) update_band(theme_bg(g_captionDark, g_captionBlue));
    const COLORREF want = theme_bg(g_captionDark, g_captionBlue);
    HDC dc = GetWindowDC(hwnd);
    if (!dc) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    // 取每条边的中间点（避开圆角与缩放手柄）
    const int xs[2] = { rc.left + 2, rc.right - 3 };
    const int ys[2] = { rc.top + 2, rc.bottom - 3 };
    bool bad = false;
    for (int i = 0; i < 2 && !bad; ++i) {
        const int y = (rc.top + rc.bottom) / 2;
        const int x = (rc.left + rc.right) / 2;
        const COLORREF got[4] = {
            GetPixel(dc, xs[i], y),      // 左/右
            GetPixel(dc, x, ys[i]),      // 上/下
            GetPixel(dc, xs[i], ys[i]),  // 角
            GetPixel(dc, x, y),          // 内容中心（只做参考，不参与判定）
        };
        for (int k = 0; k < 3; ++k) {
            if (got[k] != CLR_INVALID && got[k] != want) { bad = true; break; }
        }
    }
    ReleaseDC(hwnd, dc);
    if (bad) {
        repaint_band(hwnd);
        if (trace_paint()) logf(L"verify_band: 发现边带不是主题色，已补刷");
    }
}

static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (trace_paint()) {
        // 全量消息日志（只在 RLPLAYER_TRACE_PAINT=1 时开）：边带被"悄悄弄脏"时，
        // 光看像素分不清是哪条消息触发的意外重绘，必须把消息流本身记下来。
        logf(L"MSG 0x%04X wp=%lld lp=%lld", (unsigned)msg, (long long)wp, (long long)lp);
    }
    switch (msg) {
        case WM_ERASEBKGND: {
            // 只画 WebView2 内缩露出的 5px 边带:主题同色,别露白(见 apply_caption 注)
            //
            // ★★ 2026-10-22 第三层真凶（本轮 UI1，逐像素定位）★★
            //   Windows 会把 WM_ERASEBKGND 的 wParam 送成 (WPARAM)-1 —— 这不是 HDC，
            //   而是"请把背景擦到屏幕/桌面 DC 上"的信号（窗口显示、WM_NCPAINT 全窗重画、
            //   DWM 取窗口快照时都会走这条）。旧代码无条件 FillRect((HDC)wp, …)，
            //   HDC=-1 无效 ⟹ FillRect 静默失败 ⟹ 那 5px 边带【从来没被画过】。
            //   实测证据（launcher.log，RLPLAYER_TRACE_PAINT=1）：
            //     MSG 0x0014 wp=18446744073692859711   ← 0xFFFFFFFFFFFFFFFF = -1
            //     ERASEBKGND client=0,0-1340,880 dark=1
            //   逐像素证据：强制走一次真正的 BeginPaint 擦除后，四边 x+0..x+4 全部变成
            //   主题色（修复前是 FFFFFF + B4B4B4×3 垃圾像素）。
            //   正确做法：wParam 不是有效 DC 时自己取一个 DC，画完释放。
            HDC dc = (HDC)wp;
            bool ownDc = false;
            if (!dc || dc == (HDC)-1 || dc == (HDC)1) {
                dc = GetDC(hwnd);
                ownDc = true;
            }
            if (!g_bandBrush) update_band(theme_bg(g_captionDark, g_captionBlue));
            if (dc) {
                RECT rc{}; GetClientRect(hwnd, &rc);
                FillRect(dc, &rc, g_bandBrush);
                if (ownDc) ReleaseDC(hwnd, dc);
                if (trace_paint())
                    logf(L"ERASEBKGND client=%ld,%ld-%ld,%ld dark=%d blue=%d ownDc=%d",
                         rc.left, rc.top, rc.right, rc.bottom, (int)g_captionDark,
                         (int)g_captionBlue, (int)ownDc);
            }
            // 返回 1 = 已擦除，别让 DefWindowProc 再拿类背景刷（本类没设）盖一遍
            return 1;
        }
        case WM_NCPAINT:
            // WM_NCCALCSIZE 返回 0 后整条非客户区归我们画；走这条消息时把边带补一次。
            repaint_band(hwnd);
            return 0;
        // ---- 表面可能被 DWM 重建的时机：都要补刷一次（本轮 UI1 的修复核心）----
        // 实测这三条是"跳转页面/切换激活状态后白框出现"的实际触发点，
        // 它们之后都没有 WM_PAINT，旧代码于是永远补不上。
        case WM_ACTIVATE:
        case WM_NCACTIVATE:
        case WM_ACTIVATEAPP:
        case WM_SHOWWINDOW:
            repaint_band(hwnd);
            break;
        case WM_KILLFOCUS:
        case WM_SETFOCUS:
        case WM_WINDOWPOSCHANGED:
            repaint_band(hwnd);
            break;
        case WM_PAINT: {
            // 兜底：DefWindowProc 的 BeginPaint 不一定带擦除标志。这里自己补一遍边带，
            // 保证任何一次重绘后边带都是主题色（而不是创建时的垃圾像素）。
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (dc) {
                if (!g_bandBrush) update_band(theme_bg(g_captionDark, g_captionBlue));
                RECT rc{}; GetClientRect(hwnd, &rc);
                FillRect(dc, &rc, g_bandBrush);
                if (trace_paint()) logf(L"PAINT erased band");
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCCALCSIZE:
            // 去掉原生标题栏，但保留 WS_THICKFRAME 的缩放能力。
            if (wp) return 0;
            break;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = 960;
            mmi->ptMinTrackSize.y = 640;
            return 0;
        }
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            if (hit != HTCLIENT || IsZoomed(hwnd)) return hit;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc{}; GetWindowRect(hwnd, &rc);
            // 6 是 96 DPI 下的逻辑宽度；200% 屏上若直接用 6 物理像素，
            // 可拖拽边缘只剩 3 逻辑像素，鼠标很难命中。
            const int b = dp(hwnd, 6);
            const bool L = pt.x < rc.left + b, R = pt.x >= rc.right - b;
            const bool T = pt.y < rc.top + b,  B = pt.y >= rc.bottom - b;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            return HTCLIENT;
        }
        case WM_DPICHANGED: {
            // 跨屏拖到不同缩放的显示器：按系统建议矩形重排。边带宽度由 WM_SIZE 里的
            // dp() 重算，所以只需把窗口几何交给 SetWindowPos。
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SIZE:
            if (g_wvCtl) rta::wv2::resize(g_wvCtl, hwnd, dp(hwnd, kBandInset));
            // 尺寸一变，WebView2 子窗口按新几何重排，边带跟着变宽/变高；WM_SIZE 之后
            // 不保证有 WM_PAINT（实测最大化/还原就可能没有），所以这里主动补一次。
            // 与 WM_NCPAINT/WM_PAINT/WM_ERASEBKGND 共用同一个 repaint_band + kBandInset。
            repaint_band(hwnd);
            if (g_wv) g_wv->PostWebMessageAsString(IsZoomed(hwnd) ? L"max" : L"normal");
            return 0;
        case WM_TIMER:
            if (wp == 2) { verify_band(hwnd); return 0; }
            if (wp == 1) {
                // 网页已经报过主题 ⟹ 以网页为准，conf 轮询到此为止（否则会覆盖网页的选择）
                if (g_themeFromPage) return 0;
                bool dark = true, blue = false;
                theme_flags(dark, blue);
                if (dark != g_captionDark || blue != g_captionBlue) {
                    g_captionDark = dark;
                    g_captionBlue = blue;
                    apply_caption(hwnd, dark, blue);
                    logf(L"标题栏跟随主题 -> %s", dark ? L"深色" : (blue ? L"浅蓝" : L"浅色"));
                }
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            // 主动关掉 WebView2 控制器：否则进程被结束时，浏览器进程会变成孤儿常驻
            if (g_wvCtl) {
                g_wvCtl->Close();
                g_wvCtl->Release();
                g_wvCtl = nullptr;
            }
            if (g_wv) { g_wv->Release(); g_wv = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool open_webview_window(int port) {
    static const wchar_t* kClass = L"RLPlayerMain";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), L"IDI_APPICON", IMAGE_ICON,
                                 32, 32, LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // 无边框窗口：标题栏由网页里的 .top 顶栏承担（QQ 音乐式一体化）。
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"RLPlayer",
                                WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
                                (int)((long long)120 * dpi_system() / 96),
                                (int)((long long)60 * dpi_system() / 96),
                                (int)((long long)1340 * dpi_system() / 96),
                                (int)((long long)880 * dpi_system() / 96),
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;
    g_mainWnd = hwnd;

    // 初始尺寸按"窗口实际所在显示器"的 DPI 校正（1340×880 是 96 DPI 下的逻辑目标）。
    // 不能依赖创建时算的 dpi_system()：实测进程刚设 PerMonitorV2 时 GetDC(NULL) 仍报 96，
    // 等窗口建好才变 192，于是窗口在 5K@200% 屏上只有 670 逻辑像素宽（一半）。
    // 这里用 GetDpiForWindow（创建后是准的）直接算出目标物理尺寸，与实际值不同就 SetWindowPos。
    {
        const UINT d0 = dpi_of(hwnd);
        if (d0 != 96) {
            RECT r{};
            GetWindowRect(hwnd, &r);
            const int wantW = (int)((long long)1340 * d0 / 96);
            const int wantH = (int)((long long)880 * d0 / 96);
            if ((r.right - r.left) != wantW || (r.bottom - r.top) != wantH) {
                SetWindowPos(hwnd, nullptr, 0, 0, wantW, wantH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                logf(L"高 DPI：窗口 DPI %u，初始尺寸 %dx%d → %dx%d",
                     d0, r.right - r.left, r.bottom - r.top, wantW, wantH);
            } else {
                logf(L"高 DPI：窗口 DPI %u，初始尺寸已是 %dx%d", d0, wantW, wantH);
            }
        } else {
            logf(L"高 DPI：窗口 DPI 96（100%%），初始尺寸无需校正");
        }
    }

    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t url[64];
    swprintf_s(url, L"http://127.0.0.1:%d/", port);
    std::string err;
    const std::wstring userData = exe_dir() + L"\\webview2";
    auto onMessage = [hwnd](const std::wstring& m) {
        logf(L"wvmsg: %s", m.c_str());
        if (m == L"min") { ShowWindow(hwnd, SW_MINIMIZE); }
        else if (m == L"max") { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); }
        else if (m == L"close") { PostMessageW(hwnd, WM_CLOSE, 0, 0); }
        else if (m == L"drag") {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } else if (m.rfind(L"theme:", 0) == 0) {
            // 三套主题各刷各的 --bg：只分深浅会让浅蓝主题露一条 #f4f6f9 的浅灰边
            const bool blue = (m == L"theme:blue");
            const bool dark = (m == L"theme:dark");
            g_themeFromPage = true;      // 见 g_themeFromPage 的注：此后不再听 conf
            g_captionBlue = blue;
            g_captionDark = dark;
            apply_caption(hwnd, dark, blue);
        }
    };
    if (!rta::wv2::create(hwnd, userData, url, &g_wvCtl, &g_wv, err, onMessage)) {
        logf(L"WebView2 启动失败（%S），退回 Edge", err.c_str());
        DestroyWindow(hwnd);
        g_mainWnd = nullptr;
        return false;
    }
    theme_flags(g_captionDark, g_captionBlue);
    apply_caption(hwnd, g_captionDark, g_captionBlue);
    SetTimer(hwnd, 1, 1500, nullptr);   // 主题跟随（conf 兜底）
    // 边带自愈看门狗：250ms 抽查一次像素，只在真的不对时补刷（见 verify_band 注）。
    // 之所以要定时器而不是只靠消息：实测"页面加载完成后的合成"会覆盖掉我们的填充，
    // 而那之后不再有 WM_PAINT/WM_NCPAINT 送进来。
    SetTimer(hwnd, 2, 250, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    logf(L"已用内置 WebView2 打开界面（无浏览器外壳）");
    return true;
}

// ---------------- 窗口过程 ----------------
static void show_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"打开播放界面");
    AppendMenuW(m, MF_STRING, ID_TRAY_COPY, L"复制地址");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (lan_enabled() ? MF_CHECKED : 0), ID_TRAY_LAN,
                L"允许局域网访问（重启服务生效）");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"退出");
    SetForegroundWindow(hwnd);
    POINT pt; GetCursorPos(&pt);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TRAYICON:
            if (LOWORD(lp) == WM_RBUTTONUP) show_menu(hwnd);
            else if (LOWORD(lp) == WM_LBUTTONDBLCLK) open_browser(g_port);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_TRAY_OPEN: open_browser(g_port); break;
                case ID_TRAY_COPY: copy_address(g_port); break;
                case ID_TRAY_LAN: {
                    const bool now = !lan_enabled();
                    set_lan(now);
                    logf(L"  局域网访问 -> %d", (int)now);
                    // 重启子进程让 --lan 生效（只杀自己启动的那个子进程）
                    if (g_child) { TerminateProcess(g_child, 0); CloseHandle(g_child); g_child = nullptr; }
                    for (int i = 0; i < 40 && port_open(g_port); ++i) Sleep(50);
                    spawn_server(g_port);
                    for (int i = 0; i < 100; ++i) { Sleep(50); if (port_open(g_port)) break; }
                    tray_balloon(kAppName, now ? L"已允许局域网访问（重启服务完成）" : L"已关闭局域网访问");
                    break;
                }
                case ID_TRAY_EXIT:
                    if (g_child) { TerminateProcess(g_child, 0); CloseHandle(g_child); g_child = nullptr; }
                    Shell_NotifyIconW(NIM_DELETE, &g_nid);
                    PostQuitMessage(0);
                    break;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // ★ 必须在任何窗口/WebView2 创建之前：决定进程的 DPI 感知级别。
    // 漏了它，Windows 会把整个窗口按 96 DPI 位图拉伸（界面发糊，见 enable_high_dpi 的注）。
    enable_high_dpi();
    rta::crash::install();    // 崩溃转储（1.0.5）：dump 落 exe 目录 dumps\，filter 先写日志
    logf(L"=== wWinMain 进入 ===");
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    logf(L"WSAStartup 完成");

    // 已有实例则直接开浏览器。
    // !! 只认"确实是 RLPlayer 服务"的端口：早先只要端口有人监听就当成本程序，
    //    本机别的程序占着 8902 时会把浏览器指到别人的界面上（本轮踩到）。
    bool started = false;
    for (int p = kDefaultPort; p < kDefaultPort + 12; ++p) {
        if (!port_open(p)) continue;
        if (!probe_app(p)) { logf(L"端口 %d 被别的程序占用，跳过", p); continue; }
        // 服务已经在跑：接管界面即可，但【仍然常驻】—— 托盘和主题监听都在这里。
        // 早先这里直接 return 0，于是第二次双击只会开个窗口、没有托盘，主题也不会跟着变。
        logf(L"发现已有实例在 %d，接管界面", p);
        g_port = p;
        g_child = nullptr;     // 不是本进程起的，退出时不要杀它
        started = true;
        break;
    }
    if (!started) logf(L"未发现已有实例，开始启动服务");

    // 选一个空闲端口并启动服务
    for (int p = kDefaultPort; p < kDefaultPort + 12 && !started; ++p) {
        if (port_open(p)) continue;          // 被别的程序占了
        if (!spawn_server(p)) break;
        for (int i = 0; i < 100; ++i) {      // 最多等 5 秒
            Sleep(50);
            if (port_open(p)) { g_port = p; started = true; break; }
            if (g_child && WaitForSingleObject(g_child, 0) == WAIT_OBJECT_0) {
                DWORD c = 0; GetExitCodeProcess(g_child, &c);
                logf(L"  !! rtaweb 提前退出, code=%lu", c);
                break;
            }
        }
        logf(L"  port %d started=%d", p, (int)started);
        if (!started && g_child) { TerminateProcess(g_child, 0); CloseHandle(g_child); g_child = nullptr; }
    }

    if (!started) {
        MessageBoxW(nullptr,
                    L"无法启动播放服务。\n\n"
                    L"请确认 rtaweb.exe 与本程序在同一目录，\n"
                    L"且防火墙未拦截本地端口 8899。",
                    kAppName, MB_ICONERROR | MB_OK);
        return 1;
    }

    // 启动横幅（1.0.5 可观测性）：版本/构建时间/PID/端口/OS/CPU/GPU DLL 在否/输出端点数/内测标识。
    // launcher 不加载 rtagpu（那是播放器进程的事），GPU 一栏如实报告 DLL 文件在否。
    {
        auto dll_here = [](const wchar_t* n) {
            return GetFileAttributesW((exe_dir() + L"\\" + n).c_str()) != INVALID_FILE_ATTRIBUTES;
        };
        const wchar_t* gpu = (dll_here(L"rtagpu.dll") && dll_here(L"amdhip64_7.dll"))
                                 ? L"dll-present(未探活)" : L"dll-missing";
        const std::string tester = rta::diag::read_tester();
        wchar_t msg[1024];
        swprintf_s(msg,
                   L"STARTUP v%S build=%S pid=%lu port=%d os=%s cpu=%s gpu=%s endpoints=%zu%s%S",
                   RLPLAYER_VERSION, g_buildTime,
                   (unsigned long)GetCurrentProcessId(), g_port,
                   utf8_to_wide(rta::diag::os_version_string()).c_str(),
                   utf8_to_wide(rta::diag::cpu_brand()).c_str(),
                   gpu,
                   rta::audio::WasapiOutput::enumerate().size(),
                   tester.empty() ? L"" : L" tester=",
                   tester.empty() ? L"" : utf8_to_wide(tester).c_str());
        logf(L"%s", msg);
    }

    // 隐藏的窗口，仅用于接收托盘消息
    const wchar_t* kClass = L"RLPlayerTray";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kClass, kAppName, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // 用 exe 里内嵌的图标（res/rlplayer.rc），没有资源时退回系统默认
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), L"IDI_APPICON", IMAGE_ICON,
                                    16, 16, LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"RLPlayer —— 右键菜单 / 双击打开界面");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    wchar_t msg[160];
    swprintf_s(msg, L"已在 http://127.0.0.1:%d/ 启动，浏览器已打开。\n托盘图标可再次打开或退出。", g_port);
    tray_balloon(kAppName, msg);

    // 每次双击都开一个【新】窗口：先关掉可能残留的旧窗口/旧浏览器进程，
    // 否则会被转发给老进程，既拿不到深色标题栏参数，也可能停在上一个版本。
    {
        const int n = close_app_window();
        if (n) logf(L"启动前关掉 %d 个旧窗口", n);
    }
    // 优先用内置 WebView2 窗口（无浏览器外壳、标题栏随主题即时变）；失败才退回 Edge。
    if (!open_webview_window(g_port)) open_browser(g_port);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_child) { TerminateProcess(g_child, 0); CloseHandle(g_child); }
    WSACleanup();
    return 0;
}