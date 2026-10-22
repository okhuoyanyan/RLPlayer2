// webui.h — 极简本地 HTTP 控制面（状态查询 + 配置下发）
//
// 设计取舍：
//   * 阻塞式单线程 accept 循环跑在后台线程 —— 本地控制面并发极低，够用且无依赖
//   * 只监听 127.0.0.1（默认）—— 控制面不该默认暴露到网络
//   * API 用 JSON，界面用一个自包含 HTML（无外部资源）
//   * 显式声明 UTF-8，避免中文乱码
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstdio>

namespace rta::net {

struct HttpRequest { std::string method, path, body; };

class WebUi {
public:
    using Handler = std::function<std::string(const HttpRequest&, std::string& contentType)>;
    ~WebUi() { stop(); }
    bool start(uint16_t port, Handler h, bool loopbackOnly = true);
    uint16_t port() const { return port_; }
    void stop();

private:
    void loop();
    void handle(SOCKET c);
    static std::string trim(std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        size_t i = 0; while (i < s.size() && s[i] == ' ') ++i;
        return s.substr(i);
    }
    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{true};
    std::thread thread_;
    Handler handler_;
};

inline bool WebUi::start(uint16_t port, Handler h, bool loopbackOnly) {
    handler_ = std::move(h);
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
    listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) return false;
    int yes = 1;
    ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = loopbackOnly ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
    if (::bind(listen_, (sockaddr*)&a, sizeof(a)) != 0) { ::closesocket(listen_); listen_ = INVALID_SOCKET; return false; }
    if (::listen(listen_, 8) != 0) { ::closesocket(listen_); listen_ = INVALID_SOCKET; return false; }
    sockaddr_in got{}; int gl = sizeof(got);
    ::getsockname(listen_, (sockaddr*)&got, &gl);
    port_ = ntohs(got.sin_port);
    stop_.store(false);
    thread_ = std::thread([this] { loop(); });
    return true;
}

inline void WebUi::stop() {
    stop_.store(true);
    if (listen_ != INVALID_SOCKET) { ::closesocket(listen_); listen_ = INVALID_SOCKET; }
    if (thread_.joinable()) thread_.join();
    WSACleanup();
}

inline void WebUi::loop() {
    while (!stop_.load()) {
        SOCKET c = ::accept(listen_, nullptr, nullptr);
        if (c == INVALID_SOCKET) { if (stop_.load()) break; continue; }
        handle(c);
        ::closesocket(c);
    }
}

inline void WebUi::handle(SOCKET c) {
    std::string buf; char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos && buf.size() < 65536) {
        const int r = ::recv(c, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        buf.append(tmp, (size_t)r);
    }
    const size_t hdrEnd = buf.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return;
    const std::string head = buf.substr(0, hdrEnd);
    std::string body = buf.substr(hdrEnd + 4);

    HttpRequest req;
    {
        std::istringstream is(head); std::string line;
        std::getline(is, line);
        std::istringstream ls(trim(line));
        ls >> req.method >> req.path;
    }
    size_t p = head.find("Content-Length:");
    if (p == std::string::npos) p = head.find("content-length:");
    if (p != std::string::npos) {
        const size_t n = (size_t)atoi(head.c_str() + p + 15);
        while (body.size() < n) {
            const int r = ::recv(c, tmp, sizeof(tmp), 0);
            if (r <= 0) break;
            body.append(tmp, (size_t)r);
        }
    }
    req.body = body;

    std::string ctype = "application/json; charset=utf-8";
    std::string payload = handler_ ? handler_(req, ctype) : std::string("{}");

    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: " << ctype << "\r\n"
         << "Content-Length: " << payload.size() << "\r\n"
         << "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
         << payload;
    const std::string s = resp.str();
    ::send(c, s.c_str(), (int)s.size(), 0);
}

// 内置最小控制页面（自包含；内联脚本里的模板占位符已转义，避免与生成代码冲突）
inline std::string default_page() {
    return std::string(R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<title>RLPlayer 控制台</title><style>
body{font-family:system-ui,Segoe UI,sans-serif;background:#14161a;color:#e6e6e6;margin:0;padding:24px}
h1{font-size:18px;font-weight:600;margin:0 0 16px}
.card{background:#1d2026;border:1px solid #2b2f37;border-radius:10px;padding:16px;margin-bottom:14px}
table{border-collapse:collapse;width:100%;font-size:13px}
td{padding:5px 10px;border-bottom:1px solid #2b2f37}
td:first-child{color:#8b93a1;width:200px}
pre{background:#101216;padding:10px;border-radius:6px;overflow:auto;font-size:12px}
</style></head><body>
<h1>RLPlayer 控制台</h1>
<div class="card"><table id="st"></table></div>
<div class="card"><b>原始状态</b><pre id="raw">加载中…</pre></div>
<script>
async function tick(){
  try{
    const r = await fetch('/api/status'); const j = await r.json();
    const rows = [];
    for (const kv of Object.entries(j.status||{})) rows.push('<tr><td>'+kv[0]+'</td><td>'+kv[1]+'</td></tr>');
    document.getElementById('st').innerHTML = rows.join('');
    document.getElementById('raw').textContent = JSON.stringify(j, null, 2);
  }catch(e){ document.getElementById('raw').textContent = '无法连接: '+e; }
}
tick(); setInterval(tick, 1000);
</script></body></html>)HTML");
}

} // namespace rta::net
