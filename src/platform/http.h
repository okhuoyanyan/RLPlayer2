// http.h — HTTP(S) 传输（平台层契约，跨平台）
//
// 为什么它是平台层而不是 net/ 的一部分：TLS 不可能自己实现（net/http_client.h 的头注已写明）。
// Windows 用 WinHTTP（系统组件、零依赖、自动重定向/chunked/代理），Mac 必须用
// NSURLSession 或 libcurl —— 这是真正的平台差异，因此放在这里。
//
// 只支持 GET/POST：流媒体接入只需要"把原始文件拉下来"和几个 JSON API。
// 本层不做 JSON、不做 URL 语义、不做重试 —— 那些是 net/、meta/ 的事。
//
// 契约要点（Mac 组必须一致）：
//   * 阻塞调用（在调用线程同步跑完整个请求），不回调、不后台线程
//   * 只跟随平台默认重定向策略（WinHTTP 默认跟随；Mac 用 NSURLSession 默认策略）
//   * 超时 timeoutMs 同时作用于连接/发送/接收（WinHTTP 四个超时同值）
//   * 非 2xx 不算异常：照常返回 body，ok=false，error 里有 "HTTP <码>"
//   * 任何失败都要在 error 里留下非空、可读的中文原因（不许静默返回空 body）
#pragma once
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace rta::platform {

struct HttpRequest {
    std::string method = "GET";                    // "GET" / "POST"
    std::string url;                               // http(s)://host[:port]/path
    std::vector<std::string> headers;              // 每项形如 "名: 值"
    std::string body;                              // 仅 POST
    std::string contentType = "application/json";  // 仅 POST，作为 Content-Type 头
    int timeoutMs = 20000;
    std::string proxy;                             // 空 = 直连；"host:port" 或 "http://host:port"
};

struct HttpResult {
    bool ok = false;                  // 2xx
    int status = 0;
    std::string contentType;
    std::string error;
    std::vector<unsigned char> body;
    size_t size() const { return body.size(); }
};

HttpResult http_request(const HttpRequest& req);

// ---------------- 纯函数（跨平台，可离线测）----------------

// 代理串归一化：剥 scheme 与路径，只留 host:port
//  "http://127.0.0.1:7897" -> "127.0.0.1:7897"；"" -> ""
inline std::string normalize_proxy(const std::string& proxy) {
    std::string hp = proxy;
    if (hp.rfind("http://", 0) == 0) hp = hp.substr(7);
    else if (hp.rfind("https://", 0) == 0) hp = hp.substr(8);
    const size_t slash = hp.find('/');
    if (slash != std::string::npos) hp = hp.substr(0, slash);
    return hp;
}

// URL 里非 ASCII 的路径/查询部分做百分号编码（已编码的 % 不再编 —— 防双重编码）
inline std::string percent_encode_path(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '#' ||
            c == '\\' || c == '^' || c == '|') {
            char b[8];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            o += b;
        } else {
            o += (char)c;
        }
    }
    return o;
}

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/http_winhttp.h"
#else
#error "platform/http.h: 本平台尚无实现。Mac 组请按 PLATFORM.md §9（Mac 实现清单）用 NSURLSession 实现 http_request"
#endif
