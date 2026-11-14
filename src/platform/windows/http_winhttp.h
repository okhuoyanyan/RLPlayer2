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
#include <cstdlib>
#include <string>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <memory>

#include "platform/windows/text_win32.h"   // utf8_to_wide（CP_UTF8, 显式长度）
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

inline std::string bounded_header(HINTERNET request, DWORD query, size_t maxBytes) {
    DWORD bytes = 0;
    ::WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes,
                          WINHTTP_NO_HEADER_INDEX);
    if (!bytes || bytes > (maxBytes + 1) * sizeof(wchar_t)) return std::string();
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = (DWORD)(value.size() * sizeof(wchar_t));
    if (!::WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes,
                               WINHTTP_NO_HEADER_INDEX)) return std::string();
    const std::string utf8 = narrow(value.data());
    if (utf8.size() > maxBytes) return std::string();
    for (unsigned char c : utf8) if (c < 0x20 || c == 0x7f) return std::string();
    return utf8;
}

inline void response_metadata(HINTERNET request, HttpResult& result) {
    result.etag = bounded_header(request, WINHTTP_QUERY_ETAG, 1024);
    result.retryAfter = bounded_header(request, WINHTTP_QUERY_RETRY_AFTER, 128);
    result.contentRange = bounded_header(request, WINHTTP_QUERY_CONTENT_RANGE, 256);
    const std::string length = bounded_header(request, WINHTTP_QUERY_CONTENT_LENGTH, 64);
    if (!length.empty()) {
        long long value = 0;
        bool valid = true;
        for (char c : length) {
            if (c < '0' || c > '9' || value > (9223372036854775807LL - (c - '0')) / 10) {
                valid = false; break;
            }
            value = value * 10 + c - '0';
        }
        if (valid) result.contentLength = value;
    }
}

inline void redirect_policy(HINTERNET request, bool allow) {
    if (!allow) {
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
        ::WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    }
}

// 建会话 + 代理 + 超时。约束：WinHttpOpen 之后、WinHttpConnect 之前调用。
inline HINTERNET session_open(int timeoutMs, const std::string& proxyRaw, bool async = false) {
    const std::string hp = normalize_proxy(proxyRaw);
    const bool useProxy = !hp.empty();
    const std::wstring wproxy = useProxy ? widen(hp) : std::wstring();
    HINTERNET ses = ::WinHttpOpen(L"RLPlayer/1.0",
                                  useProxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY
                                           : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  useProxy ? wproxy.c_str() : WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, async ? WINHTTP_FLAG_ASYNC : 0);
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

// Only absolute-deadline requests use this path. All WinHTTP operations are
// initiated on the caller thread; cancellation closes a pending ASYNC request
// only after its initiating API has returned. No watchdog thread is needed.
// Context and read/body buffers survive until the final HANDLE_CLOSING callback.
// Contract: learn.microsoft.com/windows/win32/winhttp/concurrency-in-winhttp
struct DeadlineSession {
    HINTERNET handle;
    explicit DeadlineSession(const std::string& proxy) : handle(session_open(20000, proxy, true)) {}
    ~DeadlineSession() { if (handle) ::WinHttpCloseHandle(handle); }
};
inline std::shared_ptr<DeadlineSession> deadline_session(const std::string& proxy) {
    // One retained session, not one WinHTTP worker pool per short request.
    // A proxy change retires the old session after its active requests finish.
    static std::mutex mutex;
    static std::string currentProxy;
    static std::shared_ptr<DeadlineSession> current;
    std::lock_guard<std::mutex> lock(mutex);
    if (!current || !current->handle || currentProxy != proxy) {
        current = std::make_shared<DeadlineSession>(proxy); currentProxy = proxy;
    }
    return current;
}
struct DeadlineRequest {
    std::mutex mutex;
    std::condition_variable changed;
    DWORD event = 0, error = 0, count = 0;
    bool closed = false, armed = false;
    std::shared_ptr<DeadlineSession> session;
    HINTERNET connection = nullptr, request = nullptr;
    std::vector<unsigned char> readBuffer = std::vector<unsigned char>(64 * 1024);
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
        if (!context) return;
        auto& s = *reinterpret_cast<DeadlineRequest*>(context);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) s.closed = true;
        else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && info && length >= sizeof(WINHTTP_ASYNC_RESULT))
            s.error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
        else if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && info && length >= sizeof(DWORD)) {
            s.count = *static_cast<DWORD*>(info); s.event = status;
        } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) { s.count = length; s.event = status; }
        else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)
            s.event = status;
        // Notify under lock: after releasing it the closing callback never
        // touches context again, so the waiting owner can safely destroy it.
        s.changed.notify_all();
    }
    void reset() { std::lock_guard<std::mutex> lock(mutex); event = count = 0; }
    bool wait(DWORD wanted, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_until(lock, deadline, [&] { return event == wanted || error != 0; }) && error == 0;
    }
    ~DeadlineRequest() {
        if (request) {
            ::WinHttpCloseHandle(request); request = nullptr; // sole owner; never reused
            if (armed) {
                std::unique_lock<std::mutex> lock(mutex);
                // CloseHandle returning is NOT a callback-lifetime fence.
                // The documented final callback is the cleanup fence.
                changed.wait(lock, [&] { return closed; });
            }
        }
        if (connection) ::WinHttpCloseHandle(connection);
    }
};

inline HttpResult deadline_request(const HttpRequest& req, const UrlParts& u) {
    HttpResult r;
    if (std::chrono::steady_clock::now() >= req.deadline) { r.error = "HTTP_BUDGET_EXCEEDED"; return r; }
    DeadlineRequest s;
    s.session = deadline_session(req.proxy);
    if (!s.session->handle) { r.error = "WinHttpOpen 失败"; return r; }
    s.connection = ::WinHttpConnect(s.session->handle, u.host.c_str(), (INTERNET_PORT)u.port, 0);
    if (!s.connection) { r.error = "连接失败"; return r; }
    const auto path = widen(percent_encode_path(u.path));
    const bool post = req.method == "POST";
    s.request = ::WinHttpOpenRequest(s.connection, post ? L"POST" : L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!s.request) { r.error = "创建请求失败"; return r; }
    ::WinHttpSetTimeouts(s.request, std::max(1, req.timeoutMs), std::max(1, req.timeoutMs),
        std::max(1, req.timeoutMs), std::max(1, req.timeoutMs));
    redirect_policy(s.request, req.allowRedirects);
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&s);
    if (!::WinHttpSetOption(s.request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
        r.error = "设置请求上下文失败"; return r;
    }
    const DWORD flags = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
        WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE | WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
        WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES;
    if (::WinHttpSetStatusCallback(s.request, DeadlineRequest::callback, flags, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        r.error = "设置请求回调失败"; return r;
    }
    s.armed = true;
    if (post) {
        const auto header = widen(std::string("Content-Type: ") + req.contentType);
        ::WinHttpAddRequestHeaders(s.request, header.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    for (const auto& h : req.headers) {
        const auto header = widen(h);
        ::WinHttpAddRequestHeaders(s.request, header.c_str(), (DWORD)-1L,
            post ? WINHTTP_ADDREQ_FLAG_ADD : WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    auto accepted = [](BOOL ok) { return ok || ::GetLastError() == ERROR_IO_PENDING; };
    auto failed = [&]() {
        r.ok = false;
        // Provider callers historically inspect status rather than ok. Never
        // let an incomplete cancelled HTTP-200 body masquerade as valid data.
        r.status = 0;
        r.body.clear();
        r.error = std::chrono::steady_clock::now() >= req.deadline ? "HTTP_BUDGET_EXCEEDED" : "请求失败";
        return r;
    };
    if (!accepted(::WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            post ? (LPVOID)req.body.data() : WINHTTP_NO_REQUEST_DATA,
            post ? (DWORD)req.body.size() : 0, post ? (DWORD)req.body.size() : 0, context)) ||
        !s.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, req.deadline)) return failed();
    s.reset();
    if (!accepted(::WinHttpReceiveResponse(s.request, nullptr)) ||
        !s.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, req.deadline)) return failed();
    DWORD code = 0, length = sizeof(code);
    ::WinHttpQueryHeaders(s.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &length, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)code;
    wchar_t type[256]{}; length = sizeof(type);
    if (::WinHttpQueryHeaders(s.request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
        type, &length, WINHTTP_NO_HEADER_INDEX)) r.contentType = narrow(type);
    response_metadata(s.request, r);
    for (;;) {
        if (std::chrono::steady_clock::now() >= req.deadline) return failed();
        s.reset();
        if (!accepted(::WinHttpQueryDataAvailable(s.request, nullptr)) ||
            !s.wait(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, req.deadline)) return failed();
        const DWORD available = s.count;
        if (!available) break;
        s.reset();
        if (!accepted(::WinHttpReadData(s.request, s.readBuffer.data(),
                std::min<DWORD>(available, (DWORD)s.readBuffer.size()), nullptr)) ||
            !s.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, req.deadline)) return failed();
        const DWORD got = s.count;
        if (!got) break;
        if (req.onChunk) {
            try { req.onChunk(reinterpret_cast<const char*>(s.readBuffer.data()), got); } catch (...) {}
        }
        if (!req.discardBody) r.body.insert(r.body.end(), s.readBuffer.begin(), s.readBuffer.begin() + got);
    }
    r.ok = r.status >= 200 && r.status < 300;
    if (!r.ok) r.error = "HTTP " + std::to_string(r.status);
    return r;
}

} // namespace detail

inline HttpResult http_request(const HttpRequest& req) {
    HttpResult r;
    const detail::UrlParts u = detail::parse_url(req.url);
    if (!u.valid) { r.error = "URL 解析失败"; return r; }
    if (req.deadline != std::chrono::steady_clock::time_point{}) return detail::deadline_request(req, u);
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
    detail::redirect_policy(hreq, req.allowRedirects);

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
    detail::response_metadata(hreq, r);
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(hreq, &avail) || avail == 0) break;
        std::vector<unsigned char> buf(avail);
        DWORD got = 0;
        if (!::WinHttpReadData(hreq, buf.data(), avail, &got) || got == 0) break;
        // 261024-05：边收边吐。每块先喂给调用方（SSE/流式 JSON），再进 body 兜底 ——
        // 这样即使调用方的解析失败，完整 body 仍然可用（诊断与重试用得上）。
        // 注意：onChunk 抛异常必须挡住 —— 它跑在传输层，抛出去会越过 WinHTTP 的清理路径。
        if (req.onChunk) {
            try { req.onChunk(reinterpret_cast<const char*>(buf.data()), (size_t)got); } catch (...) {}
        }
        // discardBody：调用方只要流（见 platform/http.h 的注释），别再把 372 MB 攒一份到内存
        if (!req.discardBody) r.body.insert(r.body.end(), buf.begin(), buf.begin() + got);
    }

    ::WinHttpCloseHandle(hreq);
    ::WinHttpCloseHandle(con);
    ::WinHttpCloseHandle(ses);

    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    return r;
}

// ---------------------------------------------------------------------------
// 上传通道（PUT/POST）：逐行搬自迁移前的 net/feedback.h（http_put_once / http_post_once）
//
// 保持原样的细节（都是行为的一部分，别"顺手优化"）：
//   * URL 解析：host[:port] + path，path **不做百分号编码**（GitHub Contents 路径是 ASCII）；
//   * 代理三态：AUTOMATIC_PROXY / NO_PROXY / NAMED_PROXY，具名代理同时传 WinHttpOpen 与 proxy 串；
//   * 超时四路同值（连接/发送/接收/解析）；
//   * 头部用 WINHTTP_ADDREQ_FLAG_ADD、(DWORD)-1 长度；
//   * 请求体一次性 WinHttpSendRequest(总长 = dwTotalLength)；
//   * 读循环 WinHttpQueryDataAvailable + WinHttpReadData，**不**做 chunked 解析（WinHTTP 已解）；
//   * 失败文案 = WinHTTP 错误码 -> 中文人话（winhttp_reason），非 2xx 才是 "HTTP <码>"。
// ---------------------------------------------------------------------------
namespace detail {

struct UploadUrl {
    bool https = false;
    std::wstring host;
    int port = 0;
    std::wstring path;
    bool valid = false;
};

inline UploadUrl upload_parse_url(const std::string& url) {
    UploadUrl u;
    size_t p = 0;
    if (url.rfind("https://", 0) == 0) { u.https = true; p = 8; }
    else if (url.rfind("http://", 0) == 0) { u.https = false; p = 7; }
    else return u;
    const size_t slash = url.find('/', p);
    const std::string hp = (slash == std::string::npos) ? url.substr(p) : url.substr(p, slash - p);
    const std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);
    std::string host = hp;
    int port = u.https ? 443 : 80;
    const size_t colon = hp.rfind(':');
    if (colon != std::string::npos) {
        host = hp.substr(0, colon);
        port = std::atoi(hp.c_str() + colon + 1);
    }
    if (host.empty() || port <= 0) return u;
    u.host = utf8_to_wide(host);
    u.path = utf8_to_wide(path);
    u.port = port;
    u.valid = !u.host.empty() && !u.path.empty();
    return u;
}

} // namespace detail

// WinHTTP 错误码 -> 人话（文案与迁移前 net/feedback.h 逐字相同，现场排查靠它）
inline std::string winhttp_reason(unsigned long e) {
    switch (e) {
        case 12001: return "内存/缓冲区不足";
        case 12002: return "请求超时（网络太慢或被墙）";
        case 12005: return "URL 不合法";
        case 12006: return "网络不可用";
        case 12007: return "无法解析主机名（DNS 失败，检查网络）";
        case 12008: return "代理或服务器地址被拒绝";
        case 12017: return "操作被取消";
        case 12019: return "连接被服务器断开";
        case 12029: return "无法连接服务器（防火墙/代理/断网）";
        case 12030: return "连接被拒绝或中断";
        case 12152: return "服务器返回了无法解析的响应";
        case 12175: return "TLS/证书校验失败";
        default:     return "WinHTTP 错误 " + std::to_string((unsigned long)e);
    }
}

inline HttpUploadResult http_upload(const HttpUploadRequest& req) {
    HttpUploadResult r;
    const detail::UploadUrl u = detail::upload_parse_url(req.url);
    if (!u.valid) { r.error = "URL 解析失败"; return r; }

    const std::wstring wproxy = (req.proxyMode == 2) ? utf8_to_wide(req.proxy) : std::wstring();
    const DWORD access = (req.proxyMode == 1) ? WINHTTP_ACCESS_TYPE_NO_PROXY
                       : (req.proxyMode == 2) ? WINHTTP_ACCESS_TYPE_NAMED_PROXY
                                              : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    const std::wstring agent = utf8_to_wide(req.userAgent);
    HINTERNET ses = ::WinHttpOpen(agent.c_str(), access,
                                  (req.proxyMode == 2) ? wproxy.c_str() : WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { r.error = winhttp_reason(::GetLastError()); return r; }
    ::WinHttpSetTimeouts(ses, req.timeoutMs, req.timeoutMs, req.timeoutMs, req.timeoutMs);

    HINTERNET con = ::WinHttpConnect(ses, u.host.c_str(), (INTERNET_PORT)u.port, 0);
    if (!con) { r.error = winhttp_reason(::GetLastError()); ::WinHttpCloseHandle(ses); return r; }

    const std::wstring wmethod = utf8_to_wide(req.method);
    HINTERNET hreq = ::WinHttpOpenRequest(con, wmethod.c_str(), u.path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hreq) {
        r.error = winhttp_reason(::GetLastError());
        ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
        return r;
    }
    for (const auto& h : req.headers) {
        const std::wstring w = utf8_to_wide(h);
        if (!w.empty()) ::WinHttpAddRequestHeaders(hreq, w.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    const bool sent = ::WinHttpSendRequest(hreq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           (LPVOID)req.body.data(), (DWORD)req.body.size(),
                                           (DWORD)req.body.size(), 0) != 0 &&
                      ::WinHttpReceiveResponse(hreq, nullptr) != 0;
    if (!sent) {
        r.error = winhttp_reason(::GetLastError());
        ::WinHttpCloseHandle(hreq); ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
        return r;
    }
    DWORD code = 0, len = sizeof(code);
    ::WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)code;
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(hreq, &avail) || avail == 0) break;
        std::vector<char> buf((size_t)avail);
        DWORD got = 0;
        if (!::WinHttpReadData(hreq, buf.data(), avail, &got) || got == 0) break;
        r.body.append(buf.data(), (size_t)got);
    }
    ::WinHttpCloseHandle(hreq); ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    return r;
}

} // namespace rta::platform
