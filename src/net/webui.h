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
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace rta::net {

struct HttpRequest {
    std::string method, path, body;
    std::string host;     // Host 头（DNS rebinding 校验用；空 = 客户端没发，按不通过处理）
};

class WebUi {
public:
    // 返回体 + 内容类型；status 非 200 时输出真正的状态行（2026-10-25：以前**所有**响应都是
    // "HTTP/1.1 200 OK"，连"Method Not Allowed"都以 200 送出 —— 脚本/代理无法区分成功与拒绝）。
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
    // 监听范围与 Host 白名单（见 start()）
    bool  lan_ = false;
    std::string allowedHost_;    // 127.0.0.1,<port>,localhost,<port>,<本机计算机名>（--lan 时校验 Host）
};

// 慢客户端超时：单线程 accept 里任何一次 recv 都可能等一个永不发完头的客户端。
// 2026-10-25 实测（http_probe pipelining）：一个只发请求行不发空行的连接，把**其它所有请求**
// 堵到 recv 超时（默认无限 ⟹ 永久）。500ms 对一个本机控制面绰绰有余。
static constexpr int kRecvTimeoutMs = 500;
// 上限（任务书要求"超长 URL / 超大 body 有没有限制"）：以前 body 由 Content-Length 决定且
// **完全无上限**（实测 64MB 一次分配成功、服务端照收），URL 超过 64KB 则直接连状态行都不回。
static constexpr size_t kMaxHeadBytes = 64 * 1024;     // 请求行 + 头（含 URL）
static constexpr size_t kMaxBodyBytes = 16 * 1024 * 1024;
// 2026-10-26（261026-02 #1）：** /api/upload 必须放行大文件 ** ——
// 拖一首 FLAC（30~200 MB）进来时走的就是这条上传通道；16 MB 的上限会让它 413，
// 前端于是拿到空路径列表，弹出「没有可加入的文件」。这是我自己引入的回归。
// 只对这一条路径放宽（其它端点仍然 16 MB），上限 2 GB（单曲最大也就几百 MB）。
static constexpr size_t kMaxUploadBytes = (size_t)2 * 1024 * 1024 * 1024;

inline std::string http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

// Host 头是否属于本机（allowedHost_ 是逗号分隔的白名单：127.0.0.1,<port>,localhost,<port>,<计算机名>）
inline bool webui_host_allowed_impl(const std::string& host, const std::string& allowed) {
    std::string h = host;
    for (auto& ch : h) ch = (char)std::tolower((unsigned char)ch);
    // 去掉端口（IPv6 的 [::1]:port 这里不做支持：控制面只绑 IPv4）
    const size_t colon = h.find(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    if (h.empty()) return false;
    std::istringstream is(allowed);
    std::string tok;
    while (std::getline(is, tok, ',')) {
        std::string t = tok;
        for (auto& ch : t) ch = (char)std::tolower((unsigned char)ch);
        if (!t.empty() && t == h) return true;
    }
    return false;
}

// handler 想把状态行写成 405/4xx 时调它（默认一律 200）。
// 为什么不用类成员：WebUi 是"每个进程一个实例"的基础设施，而 handler 是自由函数；
// 用 inline 函数 + 函数内 static（C++11 起线程安全初始化）比暴露 extern WebUi* 更省心。
inline std::atomic<int>* webui_status_slot() {
    static std::atomic<int> s{200};
    return &s;
}
inline void webui_set_status(int code) { webui_status_slot()->store(code); }

inline bool WebUi::start(uint16_t port, Handler h, bool loopbackOnly) {
    handler_ = std::move(h);
    lan_ = !loopbackOnly;
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
    // Host 白名单（--lan 时的最小防线，见 handle() 的注释）
    allowedHost_ = "127.0.0.1," + std::to_string((unsigned)port_) +
                   ",localhost," + std::to_string((unsigned)port_);
    {
        char nb[MAX_COMPUTERNAME_LENGTH + 1] = {0}; DWORD nl = sizeof(nb);
        if (::GetComputerNameA(nb, &nl) && nl > 0) allowedHost_ += std::string(",") + nb;
    }
    stop_.store(false);
    thread_ = std::thread([this] { loop(); });
    return true;
}

inline void WebUi::stop() {
    stop_.store(true);
    // 2026-10-25（移植自协作者 mac 分支 7799e09）：先 shutdown 再 close —— 阻塞在 accept 上的线程
    // 只有 shutdown 才能确定性地唤醒（close 唤醒是平台相关的"可能"）。签名换 SD_BOTH 让 accept 立刻返回。
    if (listen_ != INVALID_SOCKET) { ::shutdown(listen_, SD_BOTH); ::closesocket(listen_); listen_ = INVALID_SOCKET; }
    if (thread_.joinable()) thread_.join();
    WSACleanup();
}

// 2026-10-26（261026-02 #3「加载失败，网络异常或服务响应超时」的根因）：
// 以前是**单线程串行 handle()**，而浏览器打开一页要并发几十张封面/头像。
// 实测：首张封面 152 ms、连取 20 张 2156 ms ⟹ 一次列表页会把后面的 /api/lib/album 请求
// 挤到客户端 8 s 超时之外，界面就报「加载失败」。
// 现在改成**有上限的并发**：最多 kMaxWorkers 个连接并行处理，超出的仍回落到本线程串行处理。
// 线程安全前提（已核对）：handler 内部对播放状态(ps.mtx)、库(mtx_)、日志(g_logMx)、
// 索引写盘都持锁；webui 自己的状态只剩 listen_/stop_，只在 start/stop 里动。
static constexpr int kMaxWorkers = 6;

inline void WebUi::loop() {
    // 用 shared_ptr 持有计数：worker 是 detached 的，loop() 退出后它可能还在跑，
    // 按引用捕获会变成悬垂引用（这类崩溃以前踩过一次，不能再犯）。
    auto active = std::make_shared<std::atomic<int>>(0);
    while (!stop_.load()) {
        SOCKET c = ::accept(listen_, nullptr, nullptr);
        if (c == INVALID_SOCKET) { if (stop_.load()) break; continue; }
        if (active->load() < kMaxWorkers) {
            active->fetch_add(1);
            std::thread([this, c, active] {
                handle(c);
                ::closesocket(c);
                active->fetch_sub(1);
            }).detach();
            continue;
        }
        handle(c);
        ::closesocket(c);
    }
}


inline void WebUi::handle(SOCKET c) {
    int status = 200;
    std::string ctype = "application/json; charset=utf-8";
    std::string payload;

    // 统一的"早退"出口：任何解析失败都必须回一个**带状态行的 JSON**，而不是静默关连接
    // （2026-10-25：旧实现所有错误路径都是 `return;` ⟹ 客户端看到的是空响应/连接被重置，
    //  浏览器只报 "Failed to fetch"，查不出原因）。
    auto fail = [&](int code, const std::string& msg) {
        status = code;
        ctype = "application/json; charset=utf-8";
        payload = std::string("{\"ok\":false,\"error\":\"") + msg + "\"}";
    };

    {
        DWORD tv = kRecvTimeoutMs;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        DWORD stv = 5000;   // 发送超时：对方不读也不许把 accept 线程钉在这里
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&stv, sizeof(stv));
    }

    std::string buf; char tmp[4096];
    bool tooBig = false;
    auto findHead = [&]() {
        size_t e = buf.find("\r\n\r\n");
        if (e != std::string::npos) return e;
        e = buf.find("\n\n");           // 宽容：有的脚本只发 \n
        return e;
    };
    size_t hdrEnd = std::string::npos;
    while ((hdrEnd = findHead()) == std::string::npos) {
        if (buf.size() > kMaxHeadBytes) { tooBig = true; break; }
        const int r = ::recv(c, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        buf.append(tmp, (size_t)r);
    }
    if (tooBig) {
        fail(431, "请求头过大（>64KB）：URL 或头部超限");
    } else if (hdrEnd == std::string::npos) {
        fail(400, "请求头不完整（超时 500ms 或客户端提前关闭）");
    } else {
        const size_t sep = (buf.compare(hdrEnd, 4, "\r\n\r\n") == 0) ? 4 : 2;
        const std::string head = buf.substr(0, hdrEnd);
        std::string body = buf.substr(hdrEnd + sep);

        HttpRequest req;
        {
            std::istringstream is(head); std::string line;
            std::getline(is, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream ls(trim(line));
            ls >> req.method >> req.path;
        }
        // ---- Host 头（DNS rebinding 的最小防线）----
        {
            std::istringstream hs(head); std::string line; bool first = true;
            while (std::getline(hs, line)) {
                if (first) { first = false; continue; }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                if (trim(line.substr(0, colon)) == "Host") { req.host = trim(line.substr(colon + 1)); break; }
            }
        }
        // ---- Content-Length：**先判上限再分配** ----
        size_t want = 0;
        bool clBad = false;
        {
            std::istringstream hs(head); std::string line; bool first = true;
            while (std::getline(hs, line)) {
                if (first) { first = false; continue; }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                // 2026-10-27（261026-08）：HTTP 头名**大小写不敏感**（RFC 7230 §3.2）。
                // 旧代码只认规范大小写 "Content-Length"，而 Node fetch/undici 发的是
                // "content-length" ⟹ body 被当成 0 字节，参数静默丢失（子代理第一版自检就踩到）。
                // Chromium/WebView2 发规范大小写所以现网没暴露，但任何脚本/代理/HTTP 客户端
                // 带 body 调 API 都会中招。这里按 RFC 折叠成小写比较。
                std::string hname = trim(line.substr(0, colon));
                for (auto& hc : hname) hc = (char)std::tolower((unsigned char)hc);
                if (hname != "content-length") continue;
                const std::string v = trim(line.substr(colon + 1));
                if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) { clBad = true; break; }
                want = (size_t)strtoull(v.c_str(), nullptr, 10);
                // 上传端点单独放宽（见 kMaxUploadBytes 注释）
                const size_t cap = (req.path.rfind("/api/upload", 0) == 0) ? kMaxUploadBytes : kMaxBodyBytes;
                if (want > cap) { clBad = true; break; }
            }
        }
        if (req.method.empty() || req.path.empty()) {
            fail(400, "请求行无法解析");
        } else if (clBad && want > ((req.path.rfind("/api/upload", 0) == 0) ? kMaxUploadBytes : kMaxBodyBytes)) {
            fail(413, (req.path.rfind("/api/upload", 0) == 0)
                        ? "请求体过大（上传上限 2GB）"
                        : "请求体过大（上限 16MB）");
        } else if (clBad) {
            fail(400, "Content-Length 非法");
        } else if (lan_ && !webui_host_allowed_impl(req.host, allowedHost_)) {
            // ---- Host 白名单 ----
            // --lan 会把控制面绑到 0.0.0.0 且**没有鉴权**，一个恶意网页可以让受害者浏览器去
            // http://<公网域名指向 127.0.0.1>/api/... 做副作用请求（DNS rebinding）。最小防线：
            // Host 必须是我们自己的名字/地址之一。curl/PowerShell 默认都带 Host，本机 UI 也不受影响。
            fail(403, "Host 不被允许：--lan 只接受 127.0.0.1 / localhost / 本机名");
        } else {
            while (body.size() < want) {
                const int r = ::recv(c, tmp, sizeof(tmp), 0);
                if (r <= 0) break;
                body.append(tmp, (size_t)r);
            }
            if (body.size() < want) {
                fail(400, "请求体不完整（Content-Length 与实际收到的字节不符）");
            } else {
                body.resize(want);
                req.body.swap(body);
                webui_status_slot()->store(200);      // 每次请求前复位（handler 可能提前 return）
                payload = handler_ ? handler_(req, ctype) : std::string("{}");
                status = webui_status_slot()->exchange(200);
            }
        }
    }

    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << http_status_text(status) << "\r\n"
         << "Content-Type: " << ctype << "\r\n"
         << "Content-Length: " << payload.size() << "\r\n"
         << "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
         << payload;
    // ★ 2026-10-25（大响应阻塞审查）：用 **MSG_PARTIAL** 分块写。
    //   实测（build-dbg-net 的 sendstall 探针）：旧代码一次 send(剩余全部) 时，对 320KB 的 /app.js
    //   服务端是"一口气全塞进内核"就返回 —— 请求 4KB 读到一个不读的客户端后的剩余 371565 字节
    //   全在服务端的内核缓冲里排着；到 ~1MB 量级才会真正阻塞 accept 线程（45 秒未设超时 ⟹ 全站卡死）。
    //   MSG_PARTIAL 让 send 在"这一轮能塞多少塞多少"后**立刻返回**：剩下的留在用户态缓冲里，
    //   下一轮再给别的连接机会（单线程也能公平轮转）。它与 CLOSE 后内核继续发完已入队的数据兼容。
    const std::string s = resp.str();
    size_t off = 0;
    while (off < s.size()) {
        const size_t left = s.size() - off;
        const int want = (int)(left > (size_t)65536 ? (size_t)65536 : left);
        int n = ::send(c, s.c_str() + off, want, MSG_PARTIAL);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP) {
            n = ::send(c, s.c_str() + off, want, 0);   // 个别 LSP/协议栈不支持 MSG_PARTIAL
        }
        if (n <= 0) break;
        off += (size_t)n;
    }
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
