// http_client.h — 极简 HTTP(S) 客户端（WinHTTP）
//
// 为什么用 WinHTTP 而不是自己写 socket：
//   * Navidrome / Emby 常常跑在 HTTPS 上，自己实现 TLS 不现实
//   * WinHTTP 是系统组件，零额外依赖，自动处理重定向、chunked、代理
//
// 只实现 GET —— 流媒体接入只需要"把原始文件拉下来"。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

namespace rta::net {

struct HttpResponse {
    bool         ok = false;
    int          status = 0;
    std::string  contentType;
    std::string  error;
    std::vector<unsigned char> body;
    size_t       size() const { return body.size(); }
};

inline std::wstring http_widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

inline std::string http_narrow(const wchar_t* w) {
    if (!w) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// 把 URL 里非 ASCII 的路径/查询部分做百分号编码
inline std::string percent_encode_path(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '#' || c == '\\' || c == '^' || c == '|') {
            char b[8]; std::snprintf(b, sizeof(b), "%%%02X", c); o += b;
        } else o += (char)c;
    }
    return o;
}

// 解析 http(s)://host[:port]/path
struct UrlParts { bool https = false; std::wstring host; int port = 0; std::string path; bool valid = false; };

inline UrlParts parse_url(const std::string& url) {
    UrlParts u;
    size_t p = 0;
    if (url.rfind("https://", 0) == 0) { u.https = true;  p = 8; }
    else if (url.rfind("http://", 0) == 0) { u.https = false; p = 7; }
    else return u;

    const size_t slash = url.find('/', p);
    std::string hostport = (slash == std::string::npos) ? url.substr(p) : url.substr(p, slash - p);
    u.path = (slash == std::string::npos) ? "/" : url.substr(slash);

    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        u.host = http_widen(hostport.substr(0, colon));
        u.port = atoi(hostport.c_str() + colon + 1);
    } else {
        u.host = http_widen(hostport);
        u.port = u.https ? 443 : 80;
    }
    u.valid = !u.host.empty();
    return u;
}

// GET，可选附加请求头（每项形如 "名: 值"）
// POST（Emby 认证需要）
//
// 可选全局代理（P1.5）：默认空 = 直连（现状行为不变）。rtaweb 启动时若
// rlplayer-llm.conf 有 proxy=host:port 行则 set_http_proxy —— 对之后所有
// http_get/http_post 生效；emby/subsonic 等既有调用不配置时代理为空，不受影响。
inline std::string& http_proxy_slot() { static std::string p; return p; }
inline std::mutex&  http_proxy_mtx()  { static std::mutex m; return m; }

// 设置全局代理（hostPort 形如 "127.0.0.1:7897"，也容忍 "http://host:port"）；空串 = 直连。
inline void set_http_proxy(const std::string& hostPort) {
    std::lock_guard<std::mutex> lk(http_proxy_mtx());
    http_proxy_slot() = hostPort;
}
// 读当前代理（空 = 直连）。
inline std::string http_proxy() {
    std::lock_guard<std::mutex> lk(http_proxy_mtx());
    return http_proxy_slot();
}

// 纯函数（可离线测）：归一化代理串 —— 剥掉 scheme 与路径，只留 WinHTTP NAMED_PROXY
// 认的 host:port 形态。"http://127.0.0.1:7897" → "127.0.0.1:7897"；"" → ""。
inline std::string http_proxy_normalize(const std::string& proxy) {
    std::string hp = proxy;
    if (hp.rfind("http://", 0) == 0) hp = hp.substr(7);
    else if (hp.rfind("https://", 0) == 0) hp = hp.substr(8);
    const size_t slash = hp.find('/');
    if (slash != std::string::npos) hp = hp.substr(0, slash);
    return hp;
}

// 建会话（WinHttpOpen → 超时设置）：代理非空 → WINHTTP_ACCESS_TYPE_NAMED_PROXY 打开，
// 并在会话级用 WinHttpSetOption(WINHTTP_OPTION_PROXY) 显式设置一遍；空 → NO_PROXY（现状）。
// 调用位置约束：WinHttpOpen 之后、WinHttpConnect 之前 —— 代理只影响连接建立方式，
// 请求构造不变；HTTPS 走 CONNECT 隧道由 WinHTTP 处理，无需额外代码。
inline HINTERNET http_session_open(int timeoutMs) {
    const std::string hp = http_proxy_normalize(http_proxy());
    const bool useProxy = !hp.empty();
    const std::wstring wproxy = useProxy ? http_widen(hp) : std::wstring();
    HINTERNET ses = WinHttpOpen(L"RLPlayer/1.0",
                                useProxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY
                                         : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                useProxy ? wproxy.c_str() : WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return nullptr;
    if (useProxy) {
        WINHTTP_PROXY_INFO pi;
        pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        pi.lpszProxy = (LPWSTR)wproxy.c_str();
        pi.lpszProxyBypass = nullptr;
        WinHttpSetOption(ses, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));   // 显式再设一遍（会话级）
    }
    WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    return ses;
}

inline HttpResponse http_post(const std::string& url, const std::string& body,
                              const std::vector<std::string>& headers = {},
                              const char* contentType = "application/json",
                              int timeoutMs = 20000) {
    HttpResponse r;
    const UrlParts u = parse_url(url);
    if (!u.valid) { r.error = "URL 解析失败"; return r; }

    HINTERNET ses = http_session_open(timeoutMs);
    if (!ses) { r.error = "WinHttpOpen 失败"; return r; }
    HINTERNET con = WinHttpConnect(ses, u.host.c_str(), (INTERNET_PORT)u.port, 0);
    if (!con) { WinHttpCloseHandle(ses); r.error = "连接失败"; return r; }

    const std::wstring wpath = http_widen(percent_encode_path(u.path));
    HINTERNET req = WinHttpOpenRequest(con, L"POST", wpath.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses); r.error = "创建请求失败"; return r; }

    const std::wstring wh = http_widen(std::string("Content-Type: ") + contentType);
    WinHttpAddRequestHeaders(req, wh.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    for (const auto& h : headers) {
        const std::wstring w = http_widen(h);
        WinHttpAddRequestHeaders(req, w.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        r.error = "请求失败";
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return r;
    }
    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)code;
    {
        wchar_t ctype[256] = {0}; DWORD cl = sizeof(ctype);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                ctype, &cl, WINHTTP_NO_HEADER_INDEX)) r.contentType = http_narrow(ctype);
    }
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::vector<unsigned char> buf(avail); DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &got) || got == 0) break;
        r.body.insert(r.body.end(), buf.begin(), buf.begin() + got);
    }
    WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    return r;
}
inline HttpResponse http_get(const std::string& url,
                             const std::vector<std::string>& headers = {},
                             int timeoutMs = 20000) {
    HttpResponse r;
    const UrlParts u = parse_url(url);
    if (!u.valid) { r.error = "URL 解析失败"; return r; }

    HINTERNET ses = http_session_open(timeoutMs);
    if (!ses) { r.error = "WinHttpOpen 失败"; return r; }

    HINTERNET con = WinHttpConnect(ses, u.host.c_str(), (INTERNET_PORT)u.port, 0);
    if (!con) { WinHttpCloseHandle(ses); r.error = "连接失败"; return r; }

    const std::wstring wpath = http_widen(percent_encode_path(u.path));
    HINTERNET req = WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses); r.error = "创建请求失败"; return r; }

    for (const auto& h : headers) {
        const std::wstring wh = http_widen(h);
        WinHttpAddRequestHeaders(req, wh.c_str(), (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        r.error = "请求失败";
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return r;
    }

    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)code;

    {
        wchar_t ctype[256] = {0};
        DWORD cl = sizeof(ctype);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                ctype, &cl, WINHTTP_NO_HEADER_INDEX))
            r.contentType = http_narrow(ctype);
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) break;
        if (avail == 0) break;
        std::vector<unsigned char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &got) || got == 0) break;
        r.body.insert(r.body.end(), buf.begin(), buf.begin() + got);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);

    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    return r;
}

inline std::string http_get_text(const std::string& url,
                                 const std::vector<std::string>& headers = {},
                                 int timeoutMs = 20000) {
    auto r = http_get(url, headers, timeoutMs);
    return std::string(r.body.begin(), r.body.end());
}

} // namespace rta::net
