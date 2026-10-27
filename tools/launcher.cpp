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
#include <commctrl.h>   // SetWindowSubclass（WebView2 子窗口的边缘命中放行，见下）
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "wv2_host.h"
#include "platform/host_msg.h"     // 261024-02 #2：postMessage 协议解析 + 外链协议白名单（可单测）
#include "version.h"               // RLPLAYER_VERSION（单一版本号真相源）
#include "crash_dump.h"            // 崩溃转储 + OS/CPU/tester 诊断（1.0.5 可观测性）
#include "audio/wasapi_out.h"      // 输出端点数（横幅用，只读枚举，不开设备）

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")   // SetWindowSubclass

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

// 261024-05 #1（用户反馈：点 X 别退出，改隐藏到托盘）：
//   g_exiting      = 已走【正式退出】路径（托盘菜单「退出」/系统关机）。只有它为真时
//                    WM_CLOSE 才允许 DestroyWindow —— 平时 WM_CLOSE 只隐藏窗口。
//   g_hideHintShown= "仍在后台播放"的气球只弹一次；每次点 X 都弹就成了骚扰。
static bool g_exiting = false;
static bool g_hideHintShown = false;
// 隐藏期间被跳过的"窗口自查"次数：需求 #6 要求隐藏时不影响 resize/像素逻辑。
// 计数器只为了在恢复显示时打一行可核查的数字（隐藏时既不刷像素也不进模态拖拽循环）。
static unsigned g_hiddenWatchdogSkips = 0;
static unsigned g_hiddenResizeSkips = 0;

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
// 2026-10-26（261026-04 真机根因）：探测已有实例时必须**连版本一起比对**。
// 旧行为只看"有 RLPlayer 在响应"就接管界面 ⟹ 新版 launcher 会去接管旧版服务，
// 用户装了 1.5.2 却看到 1.5.1 的界面（296 张专辑、满屏问号，改动"全都没生效"）。
// 现在：探测到的版本 ≠ 本程序版本 ⟹ 视为"别人的实例"，不接管，自己起一个。
static std::string g_probeVersion;   // probe_app 成功时填该实例上报的 version

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
    if (resp.find("RLPlayer") == std::string::npos) return false;
    // 取 "version":"x.y.z"
    const std::string key = "\"version\":\"";
    const size_t k = resp.find(key);
    if (k == std::string::npos) { g_probeVersion.clear(); return true; }
    const size_t vs = k + key.size();
    const size_t ve = resp.find('"', vs);
    g_probeVersion = (ve == std::string::npos) ? std::string() : resp.substr(vs, ve - vs);
    return true;
}

static std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? L"." : p.substr(0, s);
}

// UTF-8 窄串 → 宽串（横幅里拼 OS/CPU 品牌用）
// 261024-02 #2：页面 postMessage 是 UTF-16，协议解析统一在 UTF-8 上做（host_msg.h 只认 std::string）
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
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
    // 旧写法 L"\r..." 里的 \r 是回车符（不是目录分隔符）：这条路径永远打不开文件，
    // 于是 theme:blue 的兜底轮询永远拿不到 blue，浅蓝主题会被刷成浅色 --bg（差一圈颜色）。
    const std::wstring path = exe_dir() + L"\\rlplayer-settings.conf";
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

static void install_wv_edge_hittest(HWND top);   // 定义在窗口过程之前（WS 边缘命中，见下）
static void hide_main_window();                 // 261024-05 #1：WM_CLOSE -> 隐藏到托盘（定义在 show_main_window 旁）
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

// rtaweb 自己的退出路径：GET /api/quit（见 tools/rtaweb.cpp:6304）把 g_exitReq 置真，
// 主循环在 50ms 内醒来 → 播放线程 stop+join、WebUi::stop()，正常返回 0。
// 用最朴素的阻塞 socket：服务就在本机，1.2s 超时足够，不引第三方 HTTP 客户端。
static bool http_get_quit(int port) {
    if (port <= 0) return false;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD to = 1200;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    bool ok = false;
    if (::connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        const char* req = "GET /api/quit HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (::send(s, req, (int)strlen(req), 0) > 0) {
            char buf[512];
            ok = ::recv(s, buf, sizeof(buf), 0) > 0;   // 预期 "{\"ok\":true}"
        }
    }
    ::closesocket(s);
    return ok;
}

// 优雅停止 rtaweb 子进程（261024-05 #1 #4）。
// 铁律 2：先做只读检查、绝不 taskkill /IM、绝不枚举/杀进程树 —— 只动【本进程
// CreateProcess 出来的那个句柄】g_child。顺序：
//   1) 发它自己的退出路径 /api/quit，让它自己收尾（播放线程 join、UI stop）
//   2) 等最多 5s
//   3) 只有它真的赖着不走，才对那个句柄 TerminateProcess（最后一道保险，日志留证）
// 返回 true = 它是自己退出的（优雅），false = 用了兜底或没有子进程。
static bool stop_server_graceful(int port) {
    if (!g_child) {
        logf(L"停服务：g_child 为空（已停过一次，或本进程只是接管了已有实例），不碰任何进程");
        return true;
    }
    if (WaitForSingleObject(g_child, 0) == WAIT_OBJECT_0) {
        DWORD c = 0; GetExitCodeProcess(g_child, &c);
        logf(L"停服务：rtaweb 早已退出 code=%lu", c);
        CloseHandle(g_child); g_child = nullptr;
        return true;
    }
    const DWORD childPid = GetProcessId(g_child);
    const bool sent = http_get_quit(port);
    logf(L"停服务：已发 GET /api/quit port=%d 送达=%d （pid=%lu，只等它自己退）",
         port, (int)sent, (unsigned long)childPid);
    const DWORD t0 = GetTickCount();
    if (WaitForSingleObject(g_child, 5000) == WAIT_OBJECT_0) {
        DWORD c = 0; GetExitCodeProcess(g_child, &c);
        logf(L"停服务：rtaweb 优雅退出 code=%lu（耗时 %lu ms）", c, GetTickCount() - t0);
        CloseHandle(g_child); g_child = nullptr;
        return true;
    }
    logf(L"停服务：!! 5s 内未退出（/api/quit 送达=%d），对自家子进程句柄 TerminateProcess 兜底 pid=%lu",
         (int)sent, (unsigned long)childPid);
    TerminateProcess(g_child, 0);
    WaitForSingleObject(g_child, 2000);
    CloseHandle(g_child); g_child = nullptr;
    return false;
}

// WebView2 不铺满窗口、四周留的像素数：留给原生缩放命中的热区（WM_NCHITTEST 要有机会拿到鼠标）。
// 这段边带由宿主自己上色 —— 上色失败就是用户看到的"白边框"，所以三个地方
//（resize / WM_ERASEBKGND / WM_NCPAINT）必须用同一个值，不许各写一个 5。
// 2026-10-24（用户实机：单曲页"黑边过渡，非常难忍"）：
// 旧方案给 WebView 内缩 5px 并把这条带子填成主题底色。问题是单曲页背景是**封面衍生渐变**，
// 任何"实色边带"都必然露出边框 —— 之前给页面加同色外圈/暗角只是把硬边变成渐变边，用户照样一眼看出来。
// 现在改成真正的满幅：WebView 铺满客户区（inset=0），边缘缩放改由"子窗口放行 + 父窗口 hit-test"实现
// （见 wv_child_hit_test_proc）。这样页面内容直接顶到窗口边，物理上不存在那条带子。
static const int kBandInset = 0;

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

// 窗口所在监视器的有效 DPI。
// 2026-10-24 实测（主控）：同一份 1.3.13 两次启动的窗口物理尺寸不同（1340x880 vs 2010x1320），
// 因为创建时用 dpi_system()（GetDC(NULL) 有时仍报 96），而 dpi_of(hwnd) 偶尔也返回 96，
// 于是"无需校正"的分支被走到 —— 逻辑尺寸在 1340x880 与 893x587 之间漂。
// 这里改成以 MonitorFromWindow + GetDpiForMonitor(MDT_EFFECTIVE_DPI) 为准，窗口建好后
// 无条件按它校正到 1340x880 逻辑像素，并把实际 DPI/尺寸写进 launcher.log。
static UINT dpi_for_monitor(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    if (sh) {
        typedef HRESULT(WINAPI* FnDpi)(HMONITOR, int, UINT*, UINT*);
        auto f = (FnDpi)GetProcAddress(sh, "GetDpiForMonitor");
        if (f && mon) {
            UINT dx = 0, dy = 0;
            if (SUCCEEDED(f(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx) {
                FreeLibrary(sh);
                return dx;
            }
        }
        FreeLibrary(sh);
    }
    const UINT d = dpi_of(hwnd);
    if (d) return d;
    return dpi_system();
}

// 逻辑像素 → 当前窗口的物理像素（kBandInset、初始窗口尺寸都走它换算）
static int dp(HWND hwnd, int v) { return (int)(((long long)v * dpi_of(hwnd) + 48) / 96); }

// 2026-10-24（261024-04 #1）：页面发起的窗口边缘拉伸。
//
// 为什么不能再用原生 hit-test：满幅后 WebView2 的**三个子窗口属于另外的进程**
//（实测 tid=29976 pid=43728、pid=11872），跨进程无法 SetWindowSubclass，鼠标全部落在它们身上、
// 返回 HTCLIENT，父窗口的 WM_NCHITTEST 永远收不到 —— 实测四边既没有缩放光标也拖不动。
// 唯一同时满足「页面像素顶到边」+「边缘可拖」的做法：页面在 8 条 5px 热区上 mousedown →
// postMessage('resize:<dir>') → 宿主在这里接管鼠标、自己跟手改窗口大小。
// 协议解析在 platform/host_msg.h（有单测：8 个方向放行、拼错/注入一律 None）。
static void begin_window_resize(HWND h, const std::string& dir) {
    if (dir.empty()) return;
    const bool L = dir.find("left") != std::string::npos;
    const bool R = dir.find("right") != std::string::npos;
    const bool T = dir.find("top") != std::string::npos;
    const bool B = dir.find("bottom") != std::string::npos;
    if (!L && !R && !T && !B) return;

    // 最大化时拉边缘：先还原再跟手（与系统原生一致，否则会把最大化窗口直接改成任意尺寸）
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    POINT p0{};
    GetCursorPos(&p0);
    RECT r0{};
    GetWindowRect(h, &r0);
    const int minW = 960, minH = 640;   // 与 WM_GETMINMAXINFO 的 ptMinTrackSize 一致
    logf(L"resize: 开始 dir=%S rect=(%ld,%ld %ldx%ld)", dir.c_str(),
         r0.left, r0.top, r0.right - r0.left, r0.bottom - r0.top);

    SetCapture(h);
    bool moved = false;
    MSG msg;
    for (;;) {
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) break;   // 松开左键 = 结束
        if (!IsWindow(h)) break;
        POINT p{};
        GetCursorPos(&p);
        RECT nr = r0;
        if (L) nr.left += (p.x - p0.x);
        if (R) nr.right += (p.x - p0.x);
        if (T) nr.top += (p.y - p0.y);
        if (B) nr.bottom += (p.y - p0.y);
        // 最小尺寸（拖过头就夹住，避免窗口被拖成 0 宽）
        if (nr.right - nr.left < minW) { if (L) nr.left = nr.right - minW; else nr.right = nr.left + minW; }
        if (nr.bottom - nr.top < minH) { if (T) nr.top = nr.bottom - minH; else nr.bottom = nr.top + minH; }
        SetWindowPos(h, nullptr, nr.left, nr.top, nr.right - nr.left, nr.bottom - nr.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        moved = true;
        // 让窗口在拖动过程中真的重绘/重排（嵌套消息泵，效果等同系统拖拽时的模态循环）
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(8);
    }
    ReleaseCapture();
    if (moved) {
        RECT r1{};
        GetWindowRect(h, &r1);
        logf(L"resize: 结束 rect=(%ld,%ld %ldx%ld)", r1.left, r1.top, r1.right - r1.left, r1.bottom - r1.top);
    }
}

static std::string g_resizeDir;   // 页面发来的 resize 方向（WM_APP+2 用）

// ---------------- 内置 WebView2 窗口 ----------------
// 用系统运行库的 WebView2 做界面：没有浏览器外壳、没有扩展、标题栏归我们管，
// 主题一变直接改 DWM 颜色，不需要重开窗口。
static HWND g_mainWnd = nullptr;
static ::ICoreWebView2Controller* g_wvCtl = nullptr;
static ::ICoreWebView2* g_wv = nullptr;

// ---------------- 启动首帧白闪（2026-10-24，根因修复） ----------------
// 逐帧证据（build-verify/analysis/white_flash_peak_frame_0030.png）：窗口出现后第 30 帧
// 整张 1340x880 是纯白（R=G=B=255），约 1 秒后才变成深色界面。
//
// 真因是【顺序】不是【配色】：create() 返回时只是"控制器建好 + 已发起 Navigate"，
// Chromium 还没有任何内容帧；这一瞬间 ShowWindow，DWM 合成到的就是它的空白表面
// （WebView2 默认底色 = 白）。所以修法是：窗口从 CreateWindow 起一直隐藏，
// 等到页面回执"已经画出一帧"才第一次 ShowWindow（探针：文档创建即注入的 rAF×2 脚本；
// NavigationCompleted 后的 rAF 作为二道保险）。
//
// 兜底（两条都做）：WebView2 默认底色 = 主题色（wv2::set_default_background），
// 窗口类背景刷 = 主题色。即使等待超时、或别的路径提前显示，看到的也是深色而不是白。
static bool  g_navDone = false;      // 已收到 NavigationCompleted
static bool  g_navOk = false;        // 该次导航是否成功
static bool  g_paintDone = false;    // 页面回执：已画出一帧（rAF×2 之后 postMessage）
static DWORD g_tCreate = 0;          // CreateWindowExW 返回时刻
static DWORD g_tCtlReady = 0;        // WebView2 控制器 + Navigate 完成时刻
static DWORD g_tNav = 0;             // 导航完成时刻
static DWORD g_tPaint = 0;           // 首帧回执时刻
static DWORD g_tShow = 0;            // ShowWindow 调用时刻

// 首帧等待上限（判据："不能让启动变慢超过约 300ms"）。
// 这段等待是【额外】加在"控制器建好 + 已发起 Navigate"之后的：热启动实测 110 ms
// 就收到回执；冷启动页面要 450~500 ms 才画出第一帧，会走到上限 —— 这时靠兜底底色
// （WebView2 默认底色 = 主题色）保证不闪白，而不是让用户多等半秒空白窗口。
// 300 ms 预算里留 50 ms 余量给 ShowWindow/UpdateWindow 与调度抖动，所以上限取 250。
// RLPLAYER_FIRSTFRAME_CAP_MS 可覆盖（0~5000）—— 取证时用它证明"门确实会等到真首帧"，
// 而不是永远走超时兜底。
static DWORD first_frame_cap_ms() {
    static DWORD v = 0xFFFFFFFFu;
    if (v == 0xFFFFFFFFu) {
        wchar_t b[16] = {0};
        v = 250;
        if (GetEnvironmentVariableW(L"RLPLAYER_FIRSTFRAME_CAP_MS", b, 16) > 0) {
            const long n = wcstol(b, nullptr, 10);
            if (n >= 0 && n <= 5000) v = (DWORD)n;
        }
    }
    return v;
}

// RLPLAYER_HOLD_FIRSTFRAME=0 关闭"等首帧再显示"（A/B 取证用；默认开启）。
static bool hold_first_frame() {
    static int v = -1;
    if (v < 0) {
        wchar_t b[8] = {0};
        v = (GetEnvironmentVariableW(L"RLPLAYER_HOLD_FIRSTFRAME", b, 8) > 0 && b[0] == L'0') ? 0 : 1;
    }
    return v == 1;
}

// 等页面回执"已画出一帧"，期间正常泵消息（主题消息、定时器都照常跑）。
// 返回 true = 收到回执；false = 超时。
static bool wait_first_frame(DWORD capMs) {
    const DWORD t0 = GetTickCount();
    MSG msg;
    for (;;) {
        if (g_paintDone) return true;
        const DWORD el = GetTickCount() - t0;
        if ((int)el >= (int)capMs) return false;
        const DWORD left = capMs - el;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, left < 5 ? left : 5, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return g_paintDone;   // 退出请求：别把消息吞掉
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}
static bool g_captionDark = true;
static bool g_captionBlue = false;   // 三套主题里"浅蓝"单独一档（--bg #e9f2ff 与浅色 #f4f6f9 不同色）
static DWORD g_lastDwmApply = 0;     // WM_WINDOWPOSCHANGED 里重设 DWM 属性的限流时间戳
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

// 只改 DWM 属性、不动窗口几何、不触发重绘 —— 可以在 WM_ACTIVATE/WM_NCACTIVATE/
// WM_DPICHANGED/WM_WINDOWPOSCHANGED/WM_SETTINGCHANGE 里反复调用。
// 为什么要能反复调：DWM 在激活/失焦/DPI 变化/表面重建后会重画窗口框，只设一次不保险。
static void apply_dwm(HWND h, bool dark, bool blue = false) {
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
        // 圆角(33=DWMWA_WINDOW_CORNER_PREFERENCE)：2026-10-24 用户要求"完全无边框"。
        // DWMWCP_ROUND 会把窗口四角切成圆弧、露出窗外的桌面/别的窗口 —— bordercheck
        // 逐像素实测四角 5x5 色差 spread=200~249（TL 0C0C0C vs 页面 08090C）。
        // 改 DWMWCP_DONOTROUND(1)：窗口本体是矩形，四角由页面内容铺满。
        DWORD corner = 1;                    // DWMWCP_DONOTROUND
        setAttr(h, 33, &corner, sizeof(corner));
    }
    FreeLibrary(dwm);
}

// 改 DWM 属性 + 换边带刷 + 让边框立刻生效（启动 / 主题切换时用）
static void apply_caption(HWND h, bool dark, bool blue = false) {
    apply_dwm(h, dark, blue);
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
    if (dp(hwnd, kBandInset) <= 0) return;   // 满幅模式：没有边带可画
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
    // 261024-05 #1：窗口隐藏到托盘时 GetWindowDC/GetPixel 没有意义（表面已不可见），
    // 直接跳过 —— 只有窗口真的可见才需要"抽查像素、不对就补刷"。
    if (!IsWindowVisible(hwnd)) return;
    if (dp(hwnd, kBandInset) <= 0) return;   // 满幅模式：边缘就是页面内容，不该按主题色校验
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
        case WM_SHOWWINDOW:
            // 261024-05 #1：从托盘恢复显示。隐藏期间 WebView2 的子窗口不保证收到 WM_SIZE，
            // 恢复后必须补一次几何重排 + 边缘命中重挂，否则四边既没缩放光标、页面也可能
            // 停在隐藏前的旧尺寸（用户会看到一条没跟上的空白）。
            repaint_band(hwnd);
            if (wp) {
                install_wv_edge_hittest(hwnd);
                if (g_wvCtl) rta::wv2::resize(g_wvCtl, hwnd, dp(hwnd, kBandInset));
            }
            apply_dwm(hwnd, g_captionDark, g_captionBlue);
            break;
        case WM_ACTIVATE:
        case WM_NCACTIVATE:
        case WM_ACTIVATEAPP:
            repaint_band(hwnd);
            // 激活/失焦时 DWM 会重画窗口框。属性是幂等的，这里直接重设一遍（不重绘、不递归）。
            apply_dwm(hwnd, g_captionDark, g_captionBlue);
            break;
        case WM_KILLFOCUS:
        case WM_SETFOCUS:
            repaint_band(hwnd);
            break;
        case WM_WINDOWPOSCHANGED:
            repaint_band(hwnd);
            // 拖动/缩放时这条消息很密：限流 200ms 重设一次即可（属性幂等，代价是几次 API 调用）
            {
                const DWORD now = GetTickCount();
                if (now - g_lastDwmApply > 200) {
                    g_lastDwmApply = now;
                    apply_dwm(hwnd, g_captionDark, g_captionBlue);
                }
            }
            break;
        case WM_SETTINGCHANGE:
            // 系统主题/强调色/DPI 变化后 DWM 会重画边框，重新应用一次
            apply_dwm(hwnd, g_captionDark, g_captionBlue);
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
            // 去掉原生标题栏，但保留 WS_THICKFRAME 的缩放能力（客户区 = 窗口矩形）。
            if (wp) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                // 最大化：系统按"边框厚度"把窗口外扩。2026-10-24 bordercheck 实测
                //   client = -10,-10 3860x2180，而监视器是 0,0 3840x2160
                // ⟹ 页面右侧/底部各有 10px 被切到屏外。这里把客户区钉回监视器矩形。
                if (IsZoomed(hwnd)) {
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    if (mon && GetMonitorInfoW(mon, &mi)) p->rgrc[0] = mi.rcMonitor;
                }
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = 960;
            mmi->ptMinTrackSize.y = 640;
            // 最大化尺寸/位置直接给监视器矩形（工作区原点差），配合 WM_NCCALCSIZE 的修正，
            // 保证最大化后客户区严格等于屏幕，而不是"屏幕 + 两条边框"。
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                mmi->ptMaxPosition.x = mi.rcMonitor.left - mi.rcWork.left;
                mmi->ptMaxPosition.y = mi.rcMonitor.top - mi.rcWork.top;
                mmi->ptMaxSize.x = mi.rcMonitor.right - mi.rcMonitor.left;
                mmi->ptMaxSize.y = mi.rcMonitor.bottom - mi.rcMonitor.top;
            }
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
            // DPI 变化 = DWM 重新算窗口框，边框/圆角偏好要重设
            apply_dwm(hwnd, g_captionDark, g_captionBlue);
            logf(L"WM_DPICHANGED: 新 DPI %u，窗口 %ldx%ld", LOWORD(wp),
                 r->right - r->left, r->bottom - r->top);
            return 0;
        }
        case WM_SIZE:
            if (g_wvCtl) rta::wv2::resize(g_wvCtl, hwnd, dp(hwnd, kBandInset));
            install_wv_edge_hittest(hwnd);   // 子窗口可能被 WebView2 重建，尺寸变化时重挂一次
            // 尺寸一变，WebView2 子窗口按新几何重排，边带跟着变宽/变高；WM_SIZE 之后
            // 不保证有 WM_PAINT（实测最大化/还原就可能没有），所以这里主动补一次。
            // 与 WM_NCPAINT/WM_PAINT/WM_ERASEBKGND 共用同一个 repaint_band + kBandInset。
            repaint_band(hwnd);
            if (g_wv) g_wv->PostWebMessageAsString(IsZoomed(hwnd) ? L"max" : L"normal");
            return 0;
        case WM_APP + 2:   // 261024-04 #1：页面发起窗口边缘拉伸（见 begin_window_resize）
            // 261024-05 #1：隐藏到托盘时不允许进入模态拖拽循环（窗口不可见，SetCapture 也没意义）
            if (!IsWindowVisible(hwnd)) { ++g_hiddenResizeSkips; return 0; }
            begin_window_resize(hwnd, g_resizeDir);
            return 0;
        case WM_TIMER:
            if (wp == 2) {
                // 261024-05 #1：隐藏到托盘后这条 250ms 看门狗无事可做（边带不可见、子窗口不需要
                // 边缘命中）。直接返回，避免在不可见窗口上做 GetPixel/EnumChildWindows 空转 ——
                // 恢复显示时 WM_SHOWWINDOW 会把这两件事补做一次。
                if (!IsWindowVisible(hwnd)) { ++g_hiddenWatchdogSkips; return 0; }
                verify_band(hwnd);
                // 2026-10-24（261024-04 #1 用户实测：窗口四边拖不动）：
                // WebView2 在导航/首次合成后会**新建**子窗口（实测 4 个 Chrome_WidgetWin + 1 个 IME 窗口），
                // 而 install_wv_edge_hittest() 只在创建后与 WM_SIZE 时跑 —— 后建出来的子窗口没有 subclass，
                // 它们返回 HTCLIENT，鼠标就永远到不了父窗口的 WM_NCHITTEST（父窗口其实返回的是 HTLEFT/HTTOP）。
                // 结果：边缘既没有缩放光标、也拖不动大小。这里 250ms 重挂一次（SetWindowSubclass 同 id 幂等）。
                install_wv_edge_hittest(hwnd);
                return 0;
            }
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
            // ★ 261024-05 #1（用户反馈第 1 条）：点 X / Alt+F4 / 页面 postMessage('close')
            //   全部收敛到 WM_CLOSE。旧代码这里 DestroyWindow -> WM_DESTROY -> PostQuitMessage，
            //   于是点一下 X 整个程序就没了（rtaweb 也被带走，音乐断掉）。
            //   现在：隐藏到托盘 —— 进程、托盘图标、rtaweb 子进程、音频线程全部保留。
            //   只有托盘菜单「退出」走 request_exit()（它会先置 g_exiting 再 DestroyWindow）。
            if (g_exiting) {
                DestroyWindow(hwnd);   // 正式退出路径：真关窗口（WM_DESTROY 里收尾 WebView2 + PostQuitMessage）
                return 0;
            }
            hide_main_window();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            g_mainWnd = nullptr;   // 别把已销毁的句柄留给托盘（show_main_window 靠它判活）
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
    // 主题必须在【注册窗口类之前】读出来：类背景刷要直接是主题色（兜底二的最后一层，
    // 任何 DefWindowProc 擦除都只会填主题色，永远不会填系统的白/灰）。
    theme_flags(g_captionDark, g_captionBlue);
    // 窗口类活着期间不能删，故意常驻；运行期换主题由 WM_ERASEBKGND/repaint_band 负责，
    // 类背景刷只兜"任何走 DefWindowProc 擦除"的路径。
    static HBRUSH s_classBrush = nullptr;
    if (!s_classBrush) s_classBrush = CreateSolidBrush(theme_bg(g_captionDark, g_captionBlue));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hbrBackground = s_classBrush;        // 兜底二：类背景刷 = 主题 --bg
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
    g_tCreate = GetTickCount();   // 窗口可见耗时的起点：CreateWindow 返回
    g_navDone = g_paintDone = false;

    // 初始尺寸按"窗口实际所在显示器"的 DPI 校正（1340×880 是 96 DPI 下的逻辑目标）。
    // 不能依赖创建时算的 dpi_system()：实测进程刚设 PerMonitorV2 时 GetDC(NULL) 仍报 96，
    // 等窗口建好才变 192，于是窗口在 5K@200% 屏上只有 670 逻辑像素宽（一半）。
    // 这里用 GetDpiForWindow（创建后是准的）直接算出目标物理尺寸，与实际值不同就 SetWindowPos。
    {
        // 无条件按"监视器真实 DPI"校正（不再用 d0 != 96 当条件：那条分支会在
        // dpi_of() 恰好返回 96 时跳过，导致物理尺寸 1340x880、逻辑只剩 893x587）。
        const UINT d0 = dpi_for_monitor(hwnd);
        RECT r{};
        GetWindowRect(hwnd, &r);
        const int wantW = MulDiv(1340, (int)d0, 96);
        const int wantH = MulDiv(880, (int)d0, 96);
        if ((r.right - r.left) != wantW || (r.bottom - r.top) != wantH) {
            SetWindowPos(hwnd, nullptr, 0, 0, wantW, wantH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            logf(L"高 DPI：监视器 DPI %u，初始尺寸 %ldx%ld -> %dx%d（逻辑 1340x880）",
                 d0, r.right - r.left, r.bottom - r.top, wantW, wantH);
        } else {
            logf(L"高 DPI：监视器 DPI %u，初始尺寸已是 %dx%d（逻辑 1340x880）", d0, wantW, wantH);
        }
    }

    const DWORD tDpi = GetTickCount();   // DPI 校准结束（窗口可见耗时的分段点）
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t url[64];
    swprintf_s(url, L"http://127.0.0.1:%d/", port);
    std::string err;
    const std::wstring userData = exe_dir() + L"\\webview2";
    auto onMessage = [hwnd](const std::wstring& m) {
        // 首帧回执（onNav 注入的 rAF 脚本发出）：只回答"现在可以显示了吗"，
        // 不进协议解析 —— 否则会被当成未知动作。
        if (m == L"host:firstpaint") {
            if (!g_paintDone) {
                g_paintDone = true;
                g_tPaint = GetTickCount();
                // 显示之后才到的回执也要记：它证明"门没等到"只是页面慢，不是探针坏了
                logf(L"首帧回执: @%lu ms（CreateWindow 起算，%s）", g_tPaint - g_tCreate,
                     g_tShow ? L"显示之后" : L"显示之前");
            }
            return;
        }
        logf(L"wvmsg: %s", m.c_str());
        // 261024-02 #2：协议解析抽到 platform/host_msg.h（纯函数、可单测），这里只负责"动作"。
        const auto hm = rta::platform::parse_host_message(wide_to_utf8(m));
        switch (hm.action) {
        case rta::platform::HostAction::Minimize: ShowWindow(hwnd, SW_MINIMIZE); break;
        case rta::platform::HostAction::Maximize: ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case rta::platform::HostAction::Close: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case rta::platform::HostAction::Drag:
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            break;
        case rta::platform::HostAction::Theme: {
            // 三套主题各刷各的 --bg：只分深浅会让浅蓝主题露一条 #f4f6f9 的浅灰边
            const bool blue = (hm.arg == "blue");
            const bool dark = (hm.arg == "dark");
            g_themeFromPage = true;      // 见 g_themeFromPage 的注：此后不再听 conf
            g_captionBlue = blue;
            g_captionDark = dark;
            apply_caption(hwnd, dark, blue);
            break;
        }
        case rta::platform::HostAction::Resize:
            // 不能在 WebView2 的事件回调里直接跑模态拖拽循环（会把 WebView2 的消息泵卡住）。
            // 记下方向，转成一条自定义窗口消息，在窗口过程里再执行。
            g_resizeDir = hm.arg;
            PostMessageW(hwnd, WM_APP + 2, 0, 0);
            break;
        case rta::platform::HostAction::OpenUrl: {
            // 只放 http/https（白名单在 host_msg.h 里，有单测）。打开失败/被拒都留证据。
            if (!hm.allowed) {
                logf(L"openurl REJECTED: %s (reason: %s)",
                     utf8_to_wide(hm.arg).c_str(), utf8_to_wide(hm.reason).c_str());
                break;
            }
            const std::wstring wurl = utf8_to_wide(hm.arg);
            const HINSTANCE hi = ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            logf(L"openurl OK: %s (ShellExecute=%lld)", wurl.c_str(), (long long)(intptr_t)hi);
            break;
        }
        default: break;
        }
    };
    // 导航完成的处理（首帧判据的第二步）：先记事件，再让页面在下一帧（rAF×2）回执一次。
    // 为什么不直接"导航完成就显示"：NavigationCompleted 只说明文档已经加载完，
    // Chromium 把这一帧真正合成出来还要一个帧周期；用页面自己的下一帧当判据，
    // 才等价于"真的画出来了"。回执走 postMessage，onMessage 里先于协议解析拦下。
    auto onNav = [](bool ok) {
        g_navDone = true;
        g_navOk = ok;
        g_tNav = GetTickCount();
        logf(L"导航完成: ok=%d @%lu ms（CreateWindow 起算）", (int)ok, g_tNav - g_tCreate);
        if (!g_wv) return;
        g_wv->ExecuteScript(
            L"(function(){var n=0;function f(){if(++n>=2){try{window.chrome.webview.postMessage('host:firstpaint');}catch(e){}}else{requestAnimationFrame(f);}}requestAnimationFrame(f);})();",
            nullptr);
    };
    // 首帧探针（比 NavigationCompleted 早得多）：文档一创建就注入，rAF×2 后回执。
    // 跳过非 http(s) 文档 —— 初始的 about:blank 不算"界面画出来了"。
    // 探针细节：文档创建时 chrome.webview 还没注入是常态，所以回执失败要重试，
    // 不能"试一次失败就永远没有信号"（第一版就是这样，日志里回执永远是 -1）。
    static const wchar_t* kFirstPaintProbe =
        L"(function(){try{var p=location.protocol;if(p!=='http:'&&p!=='https:')return;"
        L"var n=0,tries=0;"
        L"function send(){try{if(window.chrome&&window.chrome.webview&&window.chrome.webview.postMessage){"
        L"window.chrome.webview.postMessage('host:firstpaint');return;}}catch(e){}"
        L"if(++tries<300)setTimeout(send,16);}"
        L"function f(){if(++n>=2){send();}else{requestAnimationFrame(f);}}"
        L"requestAnimationFrame(f);}catch(e){}})();";
    if (!rta::wv2::create(hwnd, userData, url, &g_wvCtl, &g_wv, err, onMessage, kBandInset, onNav,
                          std::wstring(kFirstPaintProbe))) {
        logf(L"WebView2 启动失败（%S），退回 Edge", err.c_str());
        DestroyWindow(hwnd);
        g_mainWnd = nullptr;
        return false;
    }
    g_tCtlReady = GetTickCount();
    // 兜底二（主体）：WebView2 自己的空白表面也刷成主题色。Chromium 默认是白，
    // 必须在 ShowWindow 之前设 —— 显示之后再改，用户已经看到过那一帧白了。
    if (!rta::wv2::set_default_background(g_wvCtl, theme_bg(g_captionDark, g_captionBlue)))
        logf(L"首帧兜底: ICoreWebView2Controller2 不可用，WebView2 空白表面仍是默认白");
    install_wv_edge_hittest(hwnd);   // 满幅模式：让 WebView2 子窗口把边缘命中交还父窗口
    // 满幅兜底：WebView2 创建期间若窗口被 DPI 校正/系统调整过，这里按最新客户区再摆一次，
    // 不依赖"之后会来一条 WM_SIZE"（2026-10-24 根因就是启动路径上根本没有 WM_SIZE）。
    rta::wv2::resize(g_wvCtl, hwnd, kBandInset);
    // 网页若已在隐藏期间发过 theme:xxx，这里不能再拿 conf 覆盖它（首帧门把这段窗口拉长了，
    // 页面消息赶在 apply_caption 之前的概率从"几乎不可能"变成"很可能"）。
    if (!g_themeFromPage) theme_flags(g_captionDark, g_captionBlue);
    apply_caption(hwnd, g_captionDark, g_captionBlue);
    SetTimer(hwnd, 1, 1500, nullptr);   // 主题跟随（conf 兜底）
    // 边带自愈看门狗：250ms 抽查一次像素，只在真的不对时补刷（见 verify_band 注）。
    // 之所以要定时器而不是只靠消息：实测"页面加载完成后的合成"会覆盖掉我们的填充，
    // 而那之后不再有 WM_PAINT/WM_NCPAINT 送进来。
    SetTimer(hwnd, 2, 250, nullptr);
    // 首帧兜底（第三条，但仍是"显示前"）：窗口此刻还是隐藏的，先把客户区擦成主题色，
    // 避免显示瞬间 DWM 取到未初始化表面（历史上就是"1px 白 + 3px 灰"垃圾像素）。
    // 注意 WM_ERASEBKGND 的 wParam 可能是 -1，main_wnd_proc 里已处理成自取 DC。
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    // ★ 首帧门（白闪根因修复）：窗口从 CreateWindow 起一直隐藏（窗口类无 WS_VISIBLE，
    //   全程没有 ShowWindow），只有在这里才第一次显示。判据 = 页面回执"已画出一帧"。
    //   这段等待期间 DPI 校准 / DWM 属性 / 标题栏颜色 / 类背景刷 / WebView2 默认底色
    //   全部已经设好（都在上面完成），所以显示出来就是最终样子，不存在"先白后改色"。
    const DWORD tWait0 = GetTickCount();
    const bool gate = hold_first_frame();
    bool painted = g_paintDone;
    if (!painted && gate) painted = wait_first_frame(first_frame_cap_ms());
    const DWORD tWait = GetTickCount() - tWait0;
    g_tShow = GetTickCount();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    const DWORD tVis = GetTickCount();
    // 墙钟必须写进日志（判据："不能因此让启动变慢超过约 300ms"要靠它逐次核对）
    const long navMs   = g_navDone   ? (long)(g_tNav   - g_tCreate) : -1;
    const long paintMs = g_paintDone ? (long)(g_tPaint - g_tCreate) : -1;
    logf(L"启动首帧: CreateWindow -> ShowWindow %lu ms | 窗口创建/DPI %lu ms | WebView2 控制器+Navigate %lu ms"
         L" | 首帧等待 %lu ms | 导航完成 @%ld ms | 页面回执 @%ld ms | 判据=%s | 显示门=%s | ShowWindow->UpdateWindow %lu ms",
         g_tShow - g_tCreate, tDpi - g_tCreate, g_tCtlReady - tDpi, tWait, navMs, paintMs,
         painted ? L"page-first-paint" : L"timeout-fallback", gate ? L"on" : L"off(env)", tVis - g_tShow);
    logf(L"已用内置 WebView2 打开界面（无浏览器外壳）");
    return true;
}

// 2026-10-24（满幅后的配套）：WebView2 铺满客户区后，鼠标打在边缘命中的是它的子窗口，
// 子窗口默认把 WM_NCHITTEST 吃掉 → 父窗口拿不到 HTLEFT/HTTOP，窗口就拖不动大小了。
// 给每个 WebView2 子窗口挂 subclass：接近窗口边框时返回 HTTRANSPARENT，把命中测试交还父窗口；
// 父窗口原有的 6 逻辑像素边缘判定（WM_NCHITTEST）照旧生效。
static LRESULT CALLBACK wv_child_hit_proc(HWND child, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NCHITTEST) {
        HWND top = GetAncestor(child, GA_ROOT);
        if (top && !IsZoomed(top)) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT rc{};
            GetWindowRect(top, &rc);
            const int b = dp(top, 6);
            if (pt.x < rc.left + b || pt.x >= rc.right - b || pt.y < rc.top + b || pt.y >= rc.bottom - b)
                return HTTRANSPARENT;
        }
    }
    return DefSubclassProc(child, msg, wp, lp);
}
static BOOL CALLBACK subclass_wv_child(HWND child, LPARAM) {
    SetWindowSubclass(child, wv_child_hit_proc, 0x5254, 0);   // 同 id 重复设置=替换，不会叠加
    return TRUE;
}
static void install_wv_edge_hittest(HWND top) {
    if (top) EnumChildWindows(top, subclass_wv_child, 0);
}

// 把窗口抢到前台。
// SetForegroundWindow 受 Windows 的"前台锁"限制：只有当前前台窗口所属线程、或刚收到用户
// 输入的进程才允许抢前台。托盘左键点击/菜单命令本身算输入，但从【隐藏】状态恢复时仍可能
// 被拒（窗口不在前台列表里），于是"点了托盘图标却只看到任务栏闪"。
// 这里退一步借前台线程的输入队列（AttachThreadInput），最后再退一步 FlashWindow。
static void force_foreground(HWND h) {
    if (SetForegroundWindow(h)) return;
    const HWND fg = GetForegroundWindow();
    const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD myTid = GetCurrentThreadId();
    bool attached = false;
    if (fgTid && fgTid != myTid) attached = AttachThreadInput(myTid, fgTid, TRUE) != FALSE;
    SetForegroundWindow(h);
    BringWindowToTop(h);
    if (attached) AttachThreadInput(myTid, fgTid, FALSE);
    if (!SetForegroundWindow(h)) FlashWindow(h, TRUE);   // 前台锁还是不放行：至少让任务栏闪一下
}

// ★ 261024-05 #1：点 X = 隐藏到托盘（不退出）。
//   只做一件事：ShowWindow(SW_HIDE)。刻意【不】碰 WebView2 控制器、不碰 rtaweb、
//   不碰任何定时器 —— 音频在 rtaweb 里跑，跟窗口可见性完全无关（音乐不断）。
//   窗口隐藏后 WM_SIZE/WM_WINDOWPOSCHANGED 都不会再来，所以几何状态原样保留，
//   恢复时不需要重建（隐藏期间的 WM_TIMER 看门狗与页面 resize 请求都被跳过，见下）。
static void hide_main_window() {
    HWND h = g_mainWnd;
    if (!h || !IsWindow(h)) return;
    if (!IsWindowVisible(h)) {
        logf(L"WM_CLOSE -> 窗口本来就不可见，忽略");
        return;
    }
    ShowWindow(h, SW_HIDE);
    logf(L"WM_CLOSE -> 隐藏到托盘（进程/托盘图标/rtaweb 全部保留 child=%s visible=%d iconic=%d）",
         g_child ? L"有" : L"无", (int)(IsWindowVisible(h) != FALSE), (int)(IsIconic(h) != FALSE));
    // 首次隐藏提示一次就够（用户下次就记得了；每次点 X 都弹 = 骚扰）
    if (!g_hideHintShown) {
        g_hideHintShown = true;
        tray_balloon(kAppName, L"仍在后台播放，右键托盘图标退出");
        logf(L"WM_CLOSE -> 首次隐藏，已弹气球提示（仅一次）");
    }
}

// 托盘"打开播放界面"/单击/双击托盘图标：把【已有的无边框 WebView2 窗口】拉到前台。
//
// 2026-10-24：旧实现无条件走 open_browser()，于是每次点托盘都会再开一个 Edge --app 窗口。
// 那个窗口的边框/标题栏归 Edge 和 DWM 管（不是我们的窗口过程），浅色主题下四边就是
// 一圈原生边框 —— 用户报的"四边白框"里，凡是"点托盘打开"的截图都可能来自这条路径。
// 现在只有 WebView2 窗口真的不存在时才退回 Edge（那是 WebView2 运行库缺失的降级路径）。
static void show_main_window(int port) {
    HWND h = g_mainWnd;
    if (h && IsWindow(h)) {
        const bool wasVisible = IsWindowVisible(h) != FALSE;
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);   // 隐藏到托盘后的恢复
        force_foreground(h);
        // 隐藏期间 WebView2 子窗口不保证收到过 WM_SIZE：恢复时补一次几何 + 边缘命中，
        // 否则四边没有缩放光标、页面也可能停在隐藏前的旧尺寸。
        install_wv_edge_hittest(h);
        if (g_wvCtl) rta::wv2::resize(g_wvCtl, h, dp(h, kBandInset));
        apply_dwm(h, g_captionDark, g_captionBlue);
        logf(L"托盘：窗口置前 hwnd=0x%p 之前可见=%d 现在可见=%d（不再另开 Edge 窗口）",
             (void*)h, (int)wasVisible, (int)(IsWindowVisible(h) != FALSE));
        if (g_hiddenWatchdogSkips || g_hiddenResizeSkips) {
            logf(L"托盘：隐藏期间跳过 边带看门狗=%u 次 / 页面 resize 请求=%u 次（都没有在不可见窗口上执行）",
                 g_hiddenWatchdogSkips, g_hiddenResizeSkips);
            g_hiddenWatchdogSkips = 0;
            g_hiddenResizeSkips = 0;
        }
        return;
    }
    logf(L"托盘：WebView2 窗口不存在，退回 Edge --app（该窗口有原生边框，属降级路径）");
    open_browser(port);
}

// ★ 261024-05 #1 #4：全程序【唯一】的正式退出路径（托盘菜单「退出」）。
//   顺序：摘托盘图标 -> 优雅停 rtaweb（/api/quit，见 stop_server_graceful）-> 关主窗口
//   （WM_DESTROY 里 Close/Release WebView2 控制器并 PostQuitMessage）。
//   注意：这里绝不按映像名杀进程、绝不杀进程树（铁律 2）；rtaweb 是"请它自己走"。
static void request_exit(const wchar_t* why) {
    if (g_exiting) return;
    g_exiting = true;
    logf(L"退出：%s", why);
    // 先摘图标，避免留下点了没反应的僵尸图标；结果写日志，探针用它证明"隐藏期间图标没被摘掉"
    const BOOL delOk = Shell_NotifyIconW(NIM_DELETE, &g_nid);
    logf(L"退出：托盘图标 NIM_DELETE=%d", (int)delOk);
    stop_server_graceful(g_port);
    if (g_mainWnd && IsWindow(g_mainWnd)) {
        DestroyWindow(g_mainWnd);   // -> WM_DESTROY：收尾 WebView2 + PostQuitMessage
    } else {
        PostQuitMessage(0);
    }
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
            // 261024-05 #1 #2：左键单击【或】双击都恢复窗口（旧版只认双击，用户单击没反应）
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) show_main_window(g_port);
            else if (LOWORD(lp) == WM_RBUTTONUP) show_menu(hwnd);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_TRAY_OPEN: show_main_window(g_port); break;
                case ID_TRAY_COPY: copy_address(g_port); break;
                case ID_TRAY_LAN: {
                    const bool now = !lan_enabled();
                    set_lan(now);
                    logf(L"  局域网访问 -> %d", (int)now);
                    // 重启子进程让 --lan 生效：同样走优雅路径（/api/quit），不 taskkill /IM
                    stop_server_graceful(g_port);
                    for (int i = 0; i < 40 && port_open(g_port); ++i) Sleep(50);
                    spawn_server(g_port);
                    for (int i = 0; i < 100; ++i) { Sleep(50); if (port_open(g_port)) break; }
                    tray_balloon(kAppName, now ? L"已允许局域网访问（重启服务完成）" : L"已关闭局域网访问");
                    break;
                }
                case ID_TRAY_EXIT:
                    // 261024-05 #1 #3 #4：菜单里的「退出」才是真正的关闭
                    request_exit(L"托盘菜单「退出」");
                    break;
            }
            return 0;
        case WM_CLOSE:
            // 托盘宿主窗口不能被"关掉"：它一没了就再也收不到托盘消息（图标变僵尸）
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 261027-02 #5：在线更新的「落地」这一半在启动器里做。
// 为什么是启动器：更新必须替换 rtaweb.exe / dll / 它自己，而**正在运行的文件不能覆盖**；
// 启动器启动的那一刻，rtaweb 还没起来，全部文件都不被占用 —— 这是唯一的干净时机，
// 也正是用户说的「重启一下就能完成」。实现见 src/net/update_apply.h。
#include "net/update_apply.h"

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
        // 2026-10-26（261026-04 真机根因）：版本不一致就不接管。
        // 现场：用户开着 1.5.1（占 8899），再点 1.5.2 → 旧逻辑"接管界面"，
        // 于是 1.5.2 的 launcher 打开的是 1.5.1 的页面（296 张专辑、满屏问号），
        // 用户看到的全是旧版本，误判"新版本什么都没改"。
        {
            const std::string mine(RLPLAYER_VERSION);
            if (!g_probeVersion.empty() && g_probeVersion != mine) {
                logf(L"端口 %d 上是另一版本（%hs），本程序是 %hs：不接管，改起自己的实例", p, g_probeVersion.c_str(), mine.c_str());
                continue;
            }
            if (g_probeVersion.empty()) logf(L"端口 %d 的实例没上报版本：按旧行为接管", p);
        }
        // 服务已经在跑：接管界面即可，但【仍然常驻】—— 托盘和主题监听都在这里。
        // 早先这里直接 return 0，于是第二次双击只会开个窗口、没有托盘，主题也不会跟着变。
        logf(L"发现已有实例在 %d，接管界面", p);
        g_port = p;
        g_child = nullptr;     // 不是本进程起的，退出时不要杀它
        started = true;
        break;
    }
    // ★ 261027-02 #5：把上一次暂存的更新落地 —— **必须在 rtaweb 起来之前**。
    // 只有"没有别的实例在跑"时才做：否则我们会去改一个正在被别的进程映射的 rtaweb.exe
    // （改名虽然允许，但那个实例会继续跑旧代码，界面与文件对不上）。
    if (!started) {
        const std::string upd = rta::update::apply_pending_on_startup();
        if (!upd.empty()) logf(L"%s", utf8_to_wide(upd).c_str());
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

    // ★ 261027-02 #5：新版本起不来 ⟹ 立刻按台账回滚，再试一次。
    // 回滚判据不靠"感觉"：5 秒窗口内 rtaweb 没监听端口（或子进程提前退出）就算失败。
    if (!started) {
        const std::string rb = rta::update::rollback_pending("新版本启动失败（rtaweb 未在 5 秒内监听端口）");
        if (!rb.empty()) {
            logf(L"%s", utf8_to_wide(rb).c_str());
            for (int p = kDefaultPort; p < kDefaultPort + 12 && !started; ++p) {
                if (port_open(p)) continue;
                if (!spawn_server(p)) break;
                for (int i = 0; i < 100; ++i) {
                    Sleep(50);
                    if (port_open(p)) { g_port = p; started = true; break; }
                    if (g_child && WaitForSingleObject(g_child, 0) == WAIT_OBJECT_0) break;
                }
                if (!started && g_child) { TerminateProcess(g_child, 0); CloseHandle(g_child); g_child = nullptr; }
            }
            if (started) logf(L"回滚完成：旧版本已起在端口 %d", g_port);
        }
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

    // 隐藏的窗口，仅用于接收托盘消息。
    // 261024-05 #1：从 HWND_MESSAGE 改成【从不显示的普通顶层窗口】。
    // 消息窗口不可枚举（FindWindow/EnumWindows 都找不到），排查"托盘点了没反应"时
    // 连窗口句柄都摸不到；普通顶层窗口不显示就没有任务栏/Alt+Tab 条目，行为等价。
    const wchar_t* kClass = L"RLPlayerTray";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"RLPlayerTray", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    logf(L"托盘宿主窗口已创建 hwnd=0x%p class=%s（隐藏，不显示）", (void*)hwnd, kClass);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // 用 exe 里内嵌的图标（res/rlplayer.rc），没有资源时退回系统默认
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), L"IDI_APPICON", IMAGE_ICON,
                                    16, 16, LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"RLPlayer —— 右键菜单 / 单击或双击打开界面");
    // 261024-05 #1：把注册结果写进日志 —— 托盘图标"还在不在"是这条需求的核心断言之一，
    // 探针可以直接对日志，但断言仍以 Shell_NotifyIconGetRect / 进程状态为准（见探针）。
    logf(L"托盘图标注册 NIM_ADD=%d hwnd=0x%p uID=%d", (int)Shell_NotifyIconW(NIM_ADD, &g_nid),
         (void*)hwnd, kTrayId);

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

    // 261024-05 #1 #4：消息循环结束（托盘「退出」或系统关机）—— 优雅停 rtaweb，
    // 不再用 TerminateProcess 抢跑（那条路径现在只作为 stop_server_graceful 里的兜底）。
    stop_server_graceful(g_port);
    WSACleanup();
    return 0;
}