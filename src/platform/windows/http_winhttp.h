// platform/windows/http_winhttp.h — http.h 的 Windows 实现（WinHTTP）
//
// 逐行搬自 net/http_client.h 的 http_get/http_post（迁移前唯一实现），
// 只把"代理从全局槽读"改成"由请求结构显式传入"，其余调用次序、错误文案、
// 头部标志位（GET 用 ADD|REPLACE、POST 用 ADD）全部保持原样。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

#include "platform/windows/win_text.h"

#pragma comment(lib, "winhttp.lib")

namespace rta::platform {
namespace detail {

// 解析 http(s)://host[:port]/path
struct UrlParts {
    bool https = false;
    std::wstring host;
    int port = 0;
    std::string path;
    bool valid = false;
};

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
        u.host = widen(hostport.substr(0, colon));
        u.port = atoi(hostport.c_str() + colon + 1);
    } else {
        u.host = widen(hostport);
        u.port = u.https ? 443 : 80;
    }
    u.valid = !u.host.empty();
    return u;
}

// 建会话 + 代理 + 超时。约束：WinHttpOpen 之后、WinHttpConnect 之前调用。
inline HINTERNET session_open(int timeoutMs, const std::string& proxyRaw) {
    const std::string hp = normalize_proxy(proxyRaw);
    const bool useProxy = !hp.empty();
    const std::wstring wproxy = useProxy ? widen(hp) : std::wstring();
    HINTERNET ses = ::WinHttpOpen(L"RLPlayer/1.0",
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
        ::WinHttpSetOption(ses, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));   // 显式再设一遍（会话级）
    }
    ::WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    return ses;
}

} // namespace detail

inline HttpResult http_request(const HttpRequest& req) {
    HttpResult r;
    const detail::UrlParts u = detail::parse_url(req.url);
    if (!u.valid) { r.error = "URL 解析失败"; return r; }
    const bool isPost = (req.method == "POST");

    HINTERNET ses = detail::session_open(req.timeoutMs, req.proxy);
    if (!ses) { r.error = "WinHttpOpen 失败"; return r; }

    HINTERNET con = ::WinHttpConnect(ses, u.host.c_str(), (INTERNET_PORT)u.port, 0);
    if (!con) { ::WinHttpCloseHandle(ses); r.error = "连接失败"; return r; }

    const std::wstring wpath = detail::widen(percent_encode_path(u.path));
    HINTERNET hreq = ::WinHttpOpenRequest(con, isPost ? L"POST" : L"GET", wpath.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hreq) {
        ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
        r.error = "创建请求失败";
        return r;
    }

    bool sent = false;
    if (isPost) {
        const std::wstring wh = detail::widen(std::string("Content-Type: ") + req.contentType);
        ::WinHttpAddRequestHeaders(hreq, wh.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        for (const auto& h : req.headers) {
            const std::wstring w = detail::widen(h);
            ::WinHttpAddRequestHeaders(hreq, w.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        }
        sent = ::WinHttpSendRequest(hreq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    (LPVOID)req.body.data(), (DWORD)req.body.size(),
                                    (DWORD)req.body.size(), 0) != 0;
    } else {
        for (const auto& h : req.headers) {
            const std::wstring w = detail::widen(h);
            ::WinHttpAddRequestHeaders(hreq, w.c_str(), (DWORD)-1L,
                                       WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        sent = ::WinHttpSendRequest(hreq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0;
    }
    if (!sent || !::WinHttpReceiveResponse(hreq, nullptr)) {
        r.error = "请求失败";
        ::WinHttpCloseHandle(hreq); ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
        return r;
    }

    DWORD code = 0, len = sizeof(code);
    ::WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)code;
    {
        wchar_t ctype[256] = {0};
        DWORD cl = sizeof(ctype);
        if (::WinHttpQueryHeaders(hreq, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                  ctype, &cl, WINHTTP_NO_HEADER_INDEX))
            r.contentType = detail::narrow(ctype);
    }
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(hreq, &avail) || avail == 0) break;
        std::vector<unsigned char> buf(avail);
        DWORD got = 0;
        if (!::WinHttpReadData(hreq, buf.data(), avail, &got) || got == 0) break;
        r.body.insert(r.body.end(), buf.begin(), buf.begin() + got);
    }

    ::WinHttpCloseHandle(hreq);
    ::WinHttpCloseHandle(con);
    ::WinHttpCloseHandle(ses);

    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    return r;
}

} // namespace rta::platform
