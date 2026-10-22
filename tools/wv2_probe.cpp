// wv2_probe.cpp — 能不能不用 WebView2 SDK 直接创建 WebView2 环境？
// 运行库的真实模块是 ...\EBWebView\x64\EmbeddedBrowserWebView.dll，
// 它导出的 CreateWebViewEnvironmentWithOptionsInternal 就是 WebView2Loader.dll 转发的那个函数。
// 这个探针只做一件事：调用它，看完成回调是否被真正回调、能否拿到浏览器版本。
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <string>

static HANDLE g_done = nullptr;
static HRESULT g_hr = E_FAIL;
static std::wstring g_ver;

struct IEnv : IUnknown {
    virtual HRESULT get_BrowserVersionString(LPWSTR* v) = 0;
};

// 完成回调：IUnknown 之后紧跟一个 Invoke(HRESULT, T*)
template <class T>
struct Handler : IUnknown {
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    virtual HRESULT Invoke(HRESULT hr, T* obj) = 0;
};

struct EnvHandler : Handler<IEnv> {
    HRESULT Invoke(HRESULT hr, IEnv* env) override {
        g_hr = hr;
        if (SUCCEEDED(hr) && env) {
            LPWSTR v = nullptr;
            if (SUCCEEDED(env->get_BrowserVersionString(&v)) && v) {
                g_ver = v;
                CoTaskMemFree(v);
            }
        }
        SetEvent(g_done);
        return S_OK;
    }
};

typedef HRESULT(__stdcall* FnCreate)(BOOL isShared, LPCWSTR browserExeFolder, LPCWSTR userData,
                                     IUnknown* options, EnvHandler* handler);

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HKEY hk = nullptr;
    std::wstring dll;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        wchar_t loc[512] = {0}; DWORD len = sizeof(loc); DWORD type = 0;
        wchar_t ver[64] = {0}; DWORD vlen = sizeof(ver);
        if (RegQueryValueExW(hk, L"location", nullptr, &type, (LPBYTE)loc, &len) == ERROR_SUCCESS &&
            RegQueryValueExW(hk, L"pv", nullptr, &type, (LPBYTE)ver, &vlen) == ERROR_SUCCESS) {
            dll = std::wstring(loc) + L"\\" + ver + L"\\EBWebView\\x64\\EmbeddedBrowserWebView.dll";
        }
        RegCloseKey(hk);
    }
    if (dll.empty()) { printf("no runtime path\n"); return 2; }
    wprintf(L"runtime: %s\n", dll.c_str());
    HMODULE h = LoadLibraryW(dll.c_str());
    if (!h) { printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
    auto fn = (FnCreate)GetProcAddress(h, "CreateWebViewEnvironmentWithOptionsInternal");
    if (!fn) { printf("export missing\n"); return 2; }
    printf("export found, calling...\n");

    auto* handler = new EnvHandler();
    const HRESULT hr = fn(FALSE, nullptr, L"D:\\RTA-hqplayer\\build\\_wv2test", nullptr, handler);
    printf("call returned 0x%08lx\n", (unsigned long)hr);
    const DWORD w = WaitForSingleObject(g_done, 15000);
    printf("wait=%lu hr=0x%08lx version=%ls\n", (unsigned long)w, (unsigned long)g_hr, g_ver.c_str());
    return SUCCEEDED(g_hr) ? 0 : 1;
}
