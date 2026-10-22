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
#include <string>
#include <functional>
#include <cstdio>
#include "WebView2.h"

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

// inset：无边框窗口四周留给原生缩放命中的像素（WebView2 不铺满，鼠标才能在边缘拿到 WM_NCHITTEST）
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

inline bool create(HWND parent, const std::wstring& userDataDir, const std::wstring& url,
                   ICoreWebView2Controller** outCtl, ICoreWebView2** outWv, std::string& err,
                   std::function<void(const std::wstring&)> onMessage = nullptr) {
    // Loader 与 exe 同目录（发布包里也带着它）
    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!loader) { err = "找不到 WebView2Loader.dll"; return false; }
    auto fn = (FnCreateEnv)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!fn) { err = "Loader 缺少导出"; return false; }

    // 优先用包内自带的 WebView2 固定运行时（dist\WebView2Runtime\msedgewebview2.exe）。
    // 找不到才回退到系统安装的 Evergreen 运行时；这样干净系统/Win7 也能双击即用。
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    const size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir.resize(slash);
    const std::wstring fixedDir = exeDir + L"\\WebView2Runtime";
    const std::wstring fixedExe = fixedDir + L"\\msedgewebview2.exe";
    const bool hasFixed = GetFileAttributesW(fixedExe.c_str()) != INVALID_FILE_ATTRIBUTES;

    auto* eh = new EnvHandler();
    eh->done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HRESULT hr = fn(hasFixed ? fixedDir.c_str() : nullptr, userDataDir.c_str(), nullptr, eh);
    if (FAILED(hr) || !pump_until(eh->done_, 20000) || !eh->env_) {
        char b[128];
        std::snprintf(b, sizeof(b), "创建 WebView2 环境失败 (0x%08lx)", (unsigned long)hr);
        err = b;
        CloseHandle(eh->done_); eh->Release();
        return false;
    }
    ICoreWebView2Environment* env = eh->env_;
    CloseHandle(eh->done_); eh->Release();

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
    resize(ctl, parent, 5);
    wv->Navigate(url.c_str());
    *outCtl = ctl;
    *outWv = wv;
    env->Release();
    return true;
}

}  // namespace rta::wv2
