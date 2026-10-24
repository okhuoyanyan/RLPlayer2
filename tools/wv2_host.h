// wv2_host.h — 内置 WebView2 宿主（官方 SDK 头文件 + WebView2Loader.dll）
//
// 早先尝试过"绕过 SDK、直接调运行库内部导出"，环境能创建，但 ICoreWebView2Environment
// 的 vtable 顺序在 155 版运行库上已经不同，创建控制器时崩在 EmbeddedBrowserWebView.dll 里。
// 现在改成官方头文件（third_party/webview2/include/WebView2.h）+ 官方 Loader，
// 类型全部由 SDK 定义，不再有 ABI 猜测。
//
// 相对 msedge --app 的好处：
//   * 没有浏览器外壳/标签/扩展/首次运行引导
//   * 窗口是我们自己的，标题栏颜色可随主题即时改（DWM），不用重开窗口
#pragma once
#include <windows.h>
#include <objbase.h>
#include <winver.h>   // GetFileVersionInfoW / VerQueryValueW（读 msedgewebview2.exe 的版本）
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "WebView2.h"

// 读 exe 文件版本要 version.dll。CMakeLists 里没有 version.lib（P3 约束：只改本头文件），
// 用 pragma 让链接器自己带上；clang++/lld-link 与 MSVC 都认这条指令。
#pragma comment(lib, "version.lib")

namespace rta::wv2 {

class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
        hr_ = hr; env_ = env; if (env_) env_->AddRef();
        if (done_) SetEvent(done_);
        return S_OK;
    }
    HANDLE done_ = nullptr;
    HRESULT hr_ = E_FAIL;
    ICoreWebView2Environment* env_ = nullptr;
private:
    LONG ref_ = 1;
};

class CtrlHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
        hr_ = hr; ctl_ = ctl; if (ctl_) ctl_->AddRef();
        if (done_) SetEvent(done_);
        return S_OK;
    }
    HANDLE done_ = nullptr;
    HRESULT hr_ = E_FAIL;
    ICoreWebView2Controller* ctl_ = nullptr;
private:
    LONG ref_ = 1;
};

// 网页 → 宿主的窗口控制消息（min / max / close / drag / theme:*）
class MessageHandler : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    explicit MessageHandler(std::function<void(const std::wstring&)> cb) : cb_(std::move(cb)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2WebMessageReceivedEventHandler) {
            *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR s = nullptr;
        if (args && SUCCEEDED(args->TryGetWebMessageAsString(&s)) && s) {
            if (cb_) cb_(s);
            CoTaskMemFree(s);
        }
        return S_OK;
    }
private:
    LONG ref_ = 1;
    std::function<void(const std::wstring&)> cb_;
};

typedef HRESULT(__stdcall* FnCreateEnv)(LPCWSTR browserExecutableFolder, LPCWSTR userDataFolder,
                                        ICoreWebView2EnvironmentOptions* options, EnvHandler* handler);

inline bool pump_until(HANDLE h, DWORD timeoutMs) {
    const DWORD t0 = GetTickCount();
    for (;;) {
        const DWORD left = timeoutMs - (GetTickCount() - t0);
        if ((int)left <= 0) return false;
        const DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, left, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) return true;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
}

// inset：WebView2 相对父客户区的内缩像素。默认 0 = 满幅。
// 2026-10-24 根因：create() 末尾原来写死 resize(ctl, parent, 5)，启动路径上之后没有任何
// WM_SIZE 再来把它改成 0，于是四边永久留 5px 宿主带。这条带子只有 launcher 的
// WM_ERASEBKGND/WM_PAINT 上色，DWM 表面重建（激活/失焦/跳转）后就是未初始化像素
// （实测 active=B4B4B4、inactive=F4F7FC）—— 用户看到的"四边白框"就是它。
// 边缘缩放不靠这条带子，而是 wv_child_hit_proc 把边缘命中交还父窗口（见 launcher.cpp）。
inline bool resize(ICoreWebView2Controller* ctl, HWND hwnd, int inset = 0) {
    if (!ctl) return false;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (inset > 0) {
        rc.left += inset; rc.top += inset; rc.right -= inset; rc.bottom -= inset;
    }
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;
    return SUCCEEDED(ctl->put_Bounds(rc));
}

// ---------------- 运行库双轨选择（版本护栏） ----------------
//
// 历史：以前是"有包内固定版就优先包内，找不到才把 nullptr 交给 Loader（系统 Evergreen）"。
// 两个毛病：
//   * 包内固定版永远拿不到安全更新；
//   * 系统 Evergreen 可能比包内版旧（反过来也一样），旧运行库不认新 Loader 的新接口 ⟹
//     崩在 EmbeddedBrowserWebView.dll（见文件头 2026-10 那次 vtable 事故）。
//
// 现在的策略（RLPLAYER_WEBVIEW2_MODE 可覆盖，默认 auto）：
//   auto    ：系统版存在且版本 >= 包内版 ⟹ 系统版（能持续跟随安全更新）；
//             否则包内固定版（自包含兜底，兼无 Evergreen 的机器）。
//             首选那条创建失败时改试另一条；两条都失败时错误文本同时列出两条路的原因。
//   system  ：强制系统版（验收/排障用，失败不换轨）
//   bundled ：强制包内固定版（验收/排障用，失败不换轨）
// 本机（2026-10-22 核验）系统版与包内版同为 155.0.4283.62：按策略选系统版 —— 同一个 build，
// 行为与选包内版一致，但能跟着 Evergreen 自动更新。
//
// 日志：本头文件是 P3 唯一允许改的文件，所以直接追加到启动器同目录的 launcher.log，
// 与 launcher.cpp 的 logf 同文件、同编码（UTF-8 追加），行首 "WebView2: " 便于 grep。

enum class Track { System, Bundled };

inline std::wstring track_label(Track t) {
    return t == Track::System ? L"系统版" : L"包内固定版";
}

struct Version {
    std::vector<unsigned> parts;
    bool valid() const { return !parts.empty(); }
};

// "155.0.4283.62" → {155, 0, 4283, 62}；夹在文字里的版本号（"v155.0.4283.62 (Official build)"）也能取。
inline Version parse_version(const std::wstring& text) {
    Version v;
    unsigned cur = 0;
    bool in_num = false;
    for (const wchar_t c : text) {
        if (c >= L'0' && c <= L'9') {
            cur = cur * 10u + static_cast<unsigned>(c - L'0');
            in_num = true;
        } else if (in_num) {
            v.parts.push_back(cur);
            cur = 0;
            in_num = false;
        }
    }
    if (in_num) v.parts.push_back(cur);
    return v;
}

// 逐段比较，短的一方补 0（1.2 == 1.2.0）。这就是"系统版 >= 包内版"的护栏判据。
inline int compare_version(const Version& a, const Version& b) {
    const size_t n = (std::max)(a.parts.size(), b.parts.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned x = i < a.parts.size() ? a.parts[i] : 0u;
        const unsigned y = i < b.parts.size() ? b.parts[i] : 0u;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

inline std::wstring env_var(const wchar_t* name) {
    wchar_t buf[1024] = {0};
    const DWORD cap = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    const DWORD n = GetEnvironmentVariableW(name, buf, cap);
    if (n == 0 || n >= cap) return std::wstring();
    return std::wstring(buf, n);
}

inline bool read_reg_sz(HKEY root, const wchar_t* sub, const wchar_t* name, std::wstring& out) {
    wchar_t buf[512] = {0};
    DWORD bytes = (DWORD)sizeof(buf);
    const LSTATUS st = RegGetValueW(root, sub, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                                    nullptr, buf, &bytes);
    if (st != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return false;
    out.assign(buf);
    return !out.empty();
}

// exe 的固定版本信息（"155.0.4283.62"）；读不出返回空串。
inline std::wstring file_version(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return std::wstring();
    std::vector<unsigned char> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return std::wstring();
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT fi_size = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &fi_size) || !fi)
        return std::wstring();
    wchar_t v[64] = {0};
    swprintf_s(v, L"%u.%u.%u.%u",
               static_cast<unsigned>(HIWORD(fi->dwFileVersionMS)),
               static_cast<unsigned>(LOWORD(fi->dwFileVersionMS)),
               static_cast<unsigned>(HIWORD(fi->dwFileVersionLS)),
               static_cast<unsigned>(LOWORD(fi->dwFileVersionLS)));
    return std::wstring(v);
}

struct RuntimeInfo {
    bool found = false;
    std::wstring version;   // 版本号字符串；空 = 读不出
    std::wstring source;    // 版本来自哪里（给日志看）
    std::wstring exe;       // msedgewebview2.exe 的路径（系统版只有扫描到才有）
    std::wstring dir;       // 传给 CreateCoreWebView2EnvironmentWithOptions 的目录（仅包内版）
};

inline std::wstring ver_text(const RuntimeInfo& rt) {
    return rt.version.empty() ? std::wstring(L"版本未知") : rt.version;
}

inline std::wstring describe_runtime(const RuntimeInfo& rt) {
    if (!rt.found) return std::wstring(L"未找到");
    std::wstring s = ver_text(rt);
    if (!rt.source.empty()) s += L"（" + rt.source + L"）";
    return s;
}

// 系统 Evergreen 的版本：优先 EdgeUpdate 注册表里的 pv（唯一权威来源），
// 读不到再扫磁盘上 Application\<ver>\msedgewebview2.exe 取最高版本。
inline RuntimeInfo system_runtime() {
    RuntimeInfo ri;
    struct Probe { HKEY root; const wchar_t* sub; };
    // 64 位系统上的机器级安装通常在 WOW6432Node 下；32 位进程访问 HKLM\SOFTWARE 时会被
    // 重定向到 WOW6432Node，所以两条都探（谁先命中用谁，重复探测无害）。
    const Probe probes[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"},
        {HKEY_CURRENT_USER,  L"Software\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"},
    };
    for (const Probe& p : probes) {
        std::wstring pv;
        if (!read_reg_sz(p.root, p.sub, L"pv", pv)) continue;
        if (!parse_version(pv).valid()) continue;
        ri.found = true;
        ri.version = pv;
        ri.source = L"注册表 pv";
        break;
    }
    if (ri.found) return ri;

    const wchar_t* bases[] = {L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramFiles"};
    Version best;
    for (const wchar_t* b : bases) {
        const std::wstring base = env_var(b);
        if (base.empty()) continue;
        const std::wstring app = base + L"\\Microsoft\\EdgeWebView\\Application";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((app + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;
            const std::wstring exe = app + L"\\" + fd.cFileName + L"\\msedgewebview2.exe";
            if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            const std::wstring fv = file_version(exe);
            const Version v = parse_version(fv);
            if (!v.valid()) continue;
            if (best.valid() && compare_version(v, best) <= 0) continue;
            best = v;
            ri.found = true;
            ri.version = fv;
            ri.source = L"文件版本 " + exe;
            ri.exe = exe;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return ri;
}

// 包内固定版：dist 里的 WebView2Runtime\msedgewebview2.exe（版本只能从文件版本读）。
inline RuntimeInfo bundled_runtime(const std::wstring& exeDir) {
    RuntimeInfo ri;
    ri.dir = exeDir + L"\\WebView2Runtime";
    ri.exe = ri.dir + L"\\msedgewebview2.exe";
    const DWORD attr = GetFileAttributesW(ri.exe.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return ri;
    ri.found = true;
    ri.version = file_version(ri.exe);
    ri.source = L"msedgewebview2.exe 文件版本";
    return ri;
}

struct ModeSel {
    std::wstring mode = L"auto";
    std::wstring raw;
    bool unknown = false;
};

// RLPLAYER_WEBVIEW2_MODE：auto（默认）/ system / bundled；大小写与空白无所谓，拼错按 auto。
inline ModeSel env_mode() {
    ModeSel s;
    s.raw = env_var(L"RLPLAYER_WEBVIEW2_MODE");
    std::wstring m;
    for (wchar_t c : s.raw) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        m.push_back(c);
    }
    if (m.empty()) return s;                 // 未设置 → auto
    if (m == L"auto" || m == L"system" || m == L"bundled") { s.mode = m; return s; }
    s.unknown = true;
    return s;
}

inline std::string to_utf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// 给 err（std::string，经 launcher 的 %S 打印）用的纯 ASCII 描述：中文在 C locale 下会乱码。
inline std::string describe_ascii(const RuntimeInfo& rt) {
    if (!rt.found) return "not-found";
    std::string s = rt.version.empty() ? std::string("version-unknown") : to_utf8(rt.version);
    if (!rt.exe.empty()) s += " " + to_utf8(rt.exe);
    return s;
}

// 只写"运行库选了哪条路"这一行诊断；与 launcher.cpp 的 logf 同一文件、同一编码。
inline void log_line(const std::wstring& exeDir, const std::wstring& line) {
    FILE* f = nullptr;
    const std::wstring path = exeDir + L"\\launcher.log";
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", line.c_str());
        fclose(f);
    }
}

inline std::wstring hr_text(HRESULT hr) {
    wchar_t b[32] = {0};
    swprintf_s(b, L"0x%08lX", static_cast<unsigned long>(hr));
    return std::wstring(b);
}

struct AttemptResult {
    bool ok = false;
    HRESULT hr = E_FAIL;
    std::wstring wide_reason;   // 中文，进 launcher.log
    std::string ascii_reason;   // 只进 err（launcher 用 %S 打，中文经 C locale 会变乱码）
};

// 用指定的一轨创建环境。browserFolder 为 nullptr = 让 Loader 自己找系统 Evergreen。
inline ICoreWebView2Environment* create_env(FnCreateEnv fn, const wchar_t* browserFolder,
                                            const std::wstring& userDataDir, AttemptResult& ar) {
    auto* eh = new EnvHandler();
    eh->done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HRESULT hr = fn(browserFolder, userDataDir.c_str(), nullptr, eh);
    ar.hr = hr;
    if (FAILED(hr)) {
        ar.wide_reason = L"CreateCoreWebView2EnvironmentWithOptions 返回 " + hr_text(hr);
        ar.ascii_reason = "call-failed hr=" + to_utf8(hr_text(hr));
    } else if (!pump_until(eh->done_, 20000)) {
        ar.wide_reason = L"等待环境回调超时（20 秒）";
        ar.ascii_reason = "callback-timeout";
    } else if (!eh->env_) {
        ar.hr = eh->hr_;
        ar.wide_reason = L"环境回调失败 " + hr_text(eh->hr_);
        ar.ascii_reason = "callback-failed hr=" + to_utf8(hr_text(eh->hr_));
    } else {
        ar.ok = true;
    }
    // Invoke 里已经 AddRef 过环境，这里直接把引用交给调用方（与改动前的所有权一致）
    ICoreWebView2Environment* env = ar.ok ? eh->env_ : nullptr;
    CloseHandle(eh->done_);
    eh->Release();
    return env;
}

// 选轨：返回尝试顺序（空 = 两条都不可用）。reason 是"为什么这样选"，写日志用。
inline std::vector<Track> plan_tracks(const std::wstring& mode, const RuntimeInfo& sys,
                                      const RuntimeInfo& bun, std::wstring& reason) {
    std::vector<Track> order;
    if (mode == L"system") {
        order.push_back(Track::System);
        reason = L"RLPLAYER_WEBVIEW2_MODE=system 强制系统版";
        return order;
    }
    if (mode == L"bundled") {
        order.push_back(Track::Bundled);
        reason = L"RLPLAYER_WEBVIEW2_MODE=bundled 强制包内固定版";
        return order;
    }
    if (!sys.found && !bun.found) {
        reason = L"系统版与包内固定版都不存在";
        return order;
    }
    if (sys.found && !bun.found) {
        order.push_back(Track::System);
        reason = L"包内固定版不存在，只能用系统版";
        return order;
    }
    if (!sys.found && bun.found) {
        order.push_back(Track::Bundled);
        reason = L"系统 Evergreen 未安装，只能用包内固定版";
        return order;
    }
    const Version sv = parse_version(sys.version);
    const Version bv = parse_version(bun.version);
    if (sv.valid() && bv.valid()) {
        const int c = compare_version(sv, bv);
        if (c >= 0) {
            order.push_back(Track::System);
            order.push_back(Track::Bundled);
            reason = (c == 0)
                ? L"系统版与包内版同版本 " + sys.version + L"（同一个 build，选系统版以跟随安全更新）"
                : L"系统版 " + sys.version + L" 新于包内版 " + bun.version;
        } else {
            order.push_back(Track::Bundled);
            order.push_back(Track::System);
            reason = L"系统版 " + sys.version + L" 旧于包内版 " + bun.version +
                     L"（版本护栏：旧运行库可能不认新 API，优先包内版）";
        }
    } else {
        order.push_back(Track::Bundled);
        order.push_back(Track::System);
        reason = L"版本号读不出（系统版=" +
                 (sys.version.empty() ? std::wstring(L"未知") : sys.version) + L"，包内版=" +
                 (bun.version.empty() ? std::wstring(L"未知") : bun.version) +
                 L"），无法确认系统版 >= 包内版，优先包内版";
    }
    return order;
}

inline bool create(HWND parent, const std::wstring& userDataDir, const std::wstring& url,
                   ICoreWebView2Controller** outCtl, ICoreWebView2** outWv, std::string& err,
                   std::function<void(const std::wstring&)> onMessage = nullptr, int inset = 0) {
    // Loader 与 exe 同目录（发布包里也带着它）
    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!loader) { err = "找不到 WebView2Loader.dll"; return false; }
    auto fn = (FnCreateEnv)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!fn) { err = "Loader 缺少导出"; return false; }

    // ---------- 运行库双轨选择（策略见上方"运行库双轨选择"注释）----------
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    const size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir.resize(slash);

    const RuntimeInfo sysRt = system_runtime();
    const RuntimeInfo bunRt = bundled_runtime(exeDir);
    const ModeSel sel = env_mode();
    if (sel.unknown) {
        log_line(exeDir, L"WebView2: RLPLAYER_WEBVIEW2_MODE=\"" + sel.raw +
                          L"\" 无法识别，按 auto 处理");
    }
    std::wstring reason;
    const std::vector<Track> order = plan_tracks(sel.mode, sysRt, bunRt, reason);
    log_line(exeDir, L"WebView2: 运行时选择 模式=" + sel.mode +
                     L" | 系统版=" + describe_runtime(sysRt) +
                     L" | 包内版=" + describe_runtime(bunRt) +
                     L" | 首选=" + (order.empty() ? std::wstring(L"无") : track_label(order[0])) +
                     L"（" + reason + L"）");

    if (order.empty()) {
        log_line(exeDir, L"WebView2: 两条运行库路径都不可用：系统版（" + describe_runtime(sysRt) +
                         L"）；包内固定版（" + describe_runtime(bunRt) + L"）");
        err = "WebView2 runtime unavailable: system(" + describe_ascii(sysRt) + "); bundled(" +
              describe_ascii(bunRt) + ")";
        return false;
    }

    // 每条路各留一份失败原因，最后一起写进日志与 err —— 不许只说一句"创建环境失败"
    struct TrackFail { std::wstring wide; std::string ascii; };
    std::vector<TrackFail> fails;
    ICoreWebView2Environment* env = nullptr;
    for (size_t i = 0; i < order.size() && !env; ++i) {
        const Track t = order[i];
        const RuntimeInfo& rt = (t == Track::System) ? sysRt : bunRt;
        if (t == Track::Bundled && !rt.found) {
            const std::wstring w = L"包内固定版不存在（" + rt.exe + L"）";
            fails.push_back({track_label(t) + L" -> " + w, "bundled(" + describe_ascii(rt) + ")"});
            log_line(exeDir, L"WebView2: " + w + L"，跳过（模式=" + sel.mode + L"）");
            continue;
        }
        AttemptResult ar;
        env = create_env(fn, (t == Track::System) ? nullptr : rt.dir.c_str(), userDataDir, ar);
        if (env) {
            log_line(exeDir, L"WebView2: 已选用 " + track_label(t) + L" " + ver_text(rt) +
                             L" | 来源=" + (rt.source.empty() ? std::wstring(L"未知") : rt.source) +
                             L" | 原因=" + reason);
        } else {
            fails.push_back({track_label(t) + L" " + ver_text(rt) + L" -> " + ar.wide_reason,
                             std::string(t == Track::System ? "system(" : "bundled(") +
                                 ar.ascii_reason + ")"});
            log_line(exeDir, L"WebView2: " + track_label(t) + L" " + ver_text(rt) + L" 创建环境失败：" +
                             ar.wide_reason +
                             (i + 1 < order.size() ? L" → 改试 " + track_label(order[i + 1]) : L""));
        }
    }
    if (!env) {
        std::wstring wide = L"WebView2: 两条运行库路径都不可用：";
        std::string ascii = "WebView2 runtime unavailable: ";
        for (size_t i = 0; i < fails.size(); ++i) {
            if (i) { wide += L"；"; ascii += "; "; }
            wide += fails[i].wide;
            ascii += fails[i].ascii;
        }
        if (fails.size() < 2) {
            // 强制单轨时另一条根本没试 —— 也要写明，避免"只说一句失败"看不出另一条的状态
            const Track other = (order[0] == Track::Bundled) ? Track::System : Track::Bundled;
            const RuntimeInfo& ort = (other == Track::System) ? sysRt : bunRt;
            wide += L"；另一条 " + track_label(other) + L"（" + describe_runtime(ort) +
                    L"）未尝试：模式=" + sel.mode + L" 强制单轨";
            ascii += "; other track " + std::string(other == Track::System ? "system(" : "bundled(") +
                     describe_ascii(ort) + ") not tried (mode=" + to_utf8(sel.mode) + " forced)";
        }
        log_line(exeDir, wide);
        err = ascii;
        return false;
    }

    auto* ch = new CtrlHandler();
    ch->done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HRESULT hr2 = env->CreateCoreWebView2Controller(parent, ch);
    if (FAILED(hr2) || !pump_until(ch->done_, 20000) || !ch->ctl_) {
        char b[128];
        std::snprintf(b, sizeof(b), "创建 WebView 控制器失败 (0x%08lx)", (unsigned long)hr2);
        err = b;
        CloseHandle(ch->done_); ch->Release(); env->Release();
        return false;
    }
    ICoreWebView2Controller* ctl = ch->ctl_;
    CloseHandle(ch->done_); ch->Release();

    ICoreWebView2* wv = nullptr;
    if (FAILED(ctl->get_CoreWebView2(&wv)) || !wv) {
        err = "取 CoreWebView2 失败"; ctl->Release(); env->Release();
        return false;
    }
    ICoreWebView2Settings* st = nullptr;
    if (SUCCEEDED(wv->get_Settings(&st)) && st) {
        st->put_AreDefaultContextMenusEnabled(FALSE);
        st->put_IsZoomControlEnabled(FALSE);
        st->put_AreDevToolsEnabled(FALSE);
        st->Release();
    }
    if (onMessage) {
        auto* mh = new MessageHandler(std::move(onMessage));
        EventRegistrationToken tok{};
        wv->add_WebMessageReceived(mh, &tok);
        mh->Release();          // WebView2 持有引用
    }
    ctl->put_IsVisible(TRUE);
    // 满幅：创建完立刻按 inset 摆放（默认 0）。调用方还会在 ShowWindow 前再 resize 一次兜底。
    resize(ctl, parent, inset);
    wv->Navigate(url.c_str());
    *outCtl = ctl;
    *outWv = wv;
    env->Release();
    return true;
}

}  // namespace rta::wv2
