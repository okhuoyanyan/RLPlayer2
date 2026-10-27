// wv2_shot.cpp — **只用于 UI 取证**（BUG-3 的 incomplete 黄框提示截图）
//
// 为什么需要它：铁律 11 要求 UI 证据必须来自**真实 WebView2 窗口**的窗口像素
// （CopyFromScreen / PrintWindow），不能拿 CDP / headless 的 DOM 截图充数。
// 但 tools/launcher.cpp 把端口写死成 8899（并且会接管同端口的实例），而本轮的验证实例在
// **8956**（独立库目录 + backend=null + 音量 −60 dB），不能借用用户那个窗口。
// 所以这里用一个最小宿主：同样的窗口类名（RLPlayerMain），但窗口标题独立
// （"RLPlayer HFiX"，抓屏脚本按 class+title 精确定位，绝不误抓用户 8899 的窗口），
// 内部直接复用 rta::wv2::create()（官方 WebView2 SDK + WebView2Loader.dll）。
//
// 不参与产品构建、不进 CMakeLists（本轮按协调要求不登记 CMake），手工编译：
//   clang++ -std=c++20 -O2 -I src -I tools -I third_party/webview2/include tools/wv2_shot.cpp \
//           -o build-hfix/wv2_shot.exe -lole32 -loleaut32 -lversion -luser32 -lgdi32 -lshell32
//
// ⚠️ 踩过的坑（写在这里省下一次重复排查）：
//   1. AddScriptToExecuteOnDocumentCreated 是**异步**注册，紧接着 Navigate 时它可能输给导航，
//      实测脚本一次都没执行（页面收不到 postMessage）。改成 NavigationCompleted 之后用
//      ExecuteScript 显式调用，稳定复现。
//   2. 未定义 UNICODE 时 IDC_ARROW 是 char*，LoadCursorW 需要显式转型。
//   3. Start-Process -RedirectStandardOutput 在本机会因 NO_PROXY/no_proxy 重复键报错 ⟹ 诊断写文件。
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <string>
#include "wv2_host.h"

static const wchar_t* kClass = L"RLPlayerMain";
static const wchar_t* kTitle = L"RLPlayer HFiX";
static const char*    kLog   = "D:\\RTA-hqplayer\\build-hfix\\shot_page.log";
static ICoreWebView2* g_wv   = nullptr;
static HWND           g_hwnd = nullptr;
static bool           g_navDone = false;

static void logw(const wchar_t* fmt, ...) {
    wchar_t buf[8192];
    va_list ap; va_start(ap, fmt);
    vswprintf(buf, 8192, fmt, ap);
    va_end(ap);
    std::wprintf(L"%ls\n", buf);
    std::fflush(stdout);
    if (FILE* f = std::fopen(kLog, "ab")) {
        char u8[16384];
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, (int)sizeof(u8), nullptr, nullptr);
        if (n > 1) std::fwrite(u8, 1, (size_t)n - 1, f);
        std::fwrite("\n", 1, 1, f);
        std::fclose(f);
    }
}

// ExecuteScript 的完成回调：把页面里那句脚本的返回值写进日志（取证"确实调到了"）
class EvalDone : public ICoreWebView2ExecuteScriptCompletedHandler {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2ExecuteScriptCompletedHandler) {
            *ppv = static_cast<ICoreWebView2ExecuteScriptCompletedHandler*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, LPCWSTR result) override {
        logw(L"[eval] hr=0x%08lx result=%ls", (unsigned long)hr, result ? result : L"(null)");
        return S_OK;
    }
private:
    LONG ref_ = 1;
};

// 页面里的取证脚本：开单曲页 + 回报关键状态（是否打开、分析条 HTML 长度、ANL 状态）
static const wchar_t* kShotJs =
    L"(function(){var out={};try{"
    L"out.fn=(typeof openNowPlaying);"
    L"out.np=!!(document.getElementById('npPage'));"
    L"if(typeof openNowPlaying==='function'){openNowPlaying();}"
    L"var pg=document.getElementById('npPage');"
    L"out.on=!!(pg&&pg.classList.contains('on'));"
    L"var g=document.getElementById('npAnlGrid');"
    L"out.grid=g?g.innerHTML.length:-1;"
    L"out.warn=!!document.getElementById('npAnlWarn');"
    L"out.anl=(typeof ANL!=='undefined')?String(ANL.cur):'(no ANL)';"
    L"out.st=(document.getElementById('npAnlSt')||{}).textContent||'';"
    L"}catch(e){out.err=String(e);}"
    L"return JSON.stringify(out);})()";

// 只读汇报（第二次定时器用）：不再调 openNowPlaying（它会顺带触发 AI 整理，重复调没意义）
static const wchar_t* kReportJs =
    L"(function(){var o={};try{"
    L"var pg=document.getElementById('npPage');"
    L"o.on=!!(pg&&pg.classList.contains('on'));"
    L"var g=document.getElementById('npAnlGrid');"
    L"o.grid=g?g.innerHTML.length:-1;"
    L"o.warn=!!document.getElementById('npAnlWarn');"
    L"o.warnTxt=(document.getElementById('npAnlWarn')||{}).textContent||'';"
    L"o.st=(document.getElementById('npAnlSt')||{}).textContent||'';"
    L"o.anl=(typeof ANL!=='undefined')?String(ANL.cur):'(no ANL)';"
    L"}catch(e){o.err=String(e);}return JSON.stringify(o);})()";

static void run_script(const wchar_t* js) {
    if (!g_wv) return;
    auto* done = new EvalDone();
    g_wv->ExecuteScript(js, done);
    done->Release();
}

static LRESULT CALLBACK shot_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) {
        const UINT id = (UINT)w;
        if (id == 1) {
            KillTimer(h, 1);
            logw(L"[host] ExecuteScript(openNowPlaying)");
            run_script(kShotJs);
            SetTimer(h, 2, 3000, nullptr);      // 第二次：等分析结果回来后只读汇报
            return 0;
        }
        if (id == 2) {
            KillTimer(h, 2);
            logw(L"[host] ExecuteScript(report)");
            run_script(kReportJs);
            return 0;
        }
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t** argv) {
    std::wstring url  = (argc > 1) ? argv[1] : L"http://127.0.0.1:8956/";
    std::wstring data = (argc > 2) ? argv[2] : L"D:\\RTA-hqplayer\\build-hfix\\wv2data";
    std::remove(kLog);
    logw(L"[host] start url=%ls", url.c_str());

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = shot_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);   // 未定义 UNICODE 时 IDC_ARROW 是 char*
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassExW(&wc)) { logw(L"[host] RegisterClass failed %lu", GetLastError()); return 2; }

    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, kTitle, WS_OVERLAPPEDWINDOW,
                             60, 40, 1460, 940, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) { logw(L"[host] CreateWindow failed %lu", GetLastError()); return 2; }
    ShowWindow(g_hwnd, SW_SHOW);

    ICoreWebView2Controller* ctl = nullptr;
    std::string err;
    auto onnav = [](bool ok) {
        g_navDone = true;
        logw(L"[host] navigation completed ok=%d", (int)ok);
        if (g_hwnd) SetTimer(g_hwnd, 1, 2500, nullptr);   // 等页面脚本跑完再注入
    };
    auto onmsg = [](const std::wstring& m) { logw(L"[page] %ls", m.c_str()); };

    if (!rta::wv2::create(g_hwnd, data, url, &ctl, &g_wv, err, onmsg, 0, onnav, L"")) {
        logw(L"[host] wv2 create failed: %hs", err.c_str());
        return 3;
    }
    logw(L"[host] webview ok");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (ctl) ctl->Release();
    if (g_wv) g_wv->Release();
    return 0;
}
