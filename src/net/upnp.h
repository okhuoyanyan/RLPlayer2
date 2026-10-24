// upnp.h — UPnP/DLNA 媒体渲染器发现与本地 HTTP WAV 供源（winsock2 直连，平台白名单豁免）
//
// 契约要点（ROADMAP P5a）：
//   1) SSDP M-SEARCH 发现 MediaRenderer 设备与解析 AVTransport / ConnectionManager 控制地址；
//   2) 本地 HTTP 供源（UpnpHttpServer）：绑定 0.0.0.0 的可配置端口（默认 8912）；
//      路由 /stream/<token>.wav，随机 token 防目录遍历；
//      支持 Range 请求（bytes=start-end / bytes=start-），渲染器普遍需要 seek；
//      记录访问日志（谁取、取了多少字节）。
#pragma once
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "net/upnp_control.h"
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace rta::net {

struct UpnpDevice {
    std::string name;              // friendlyName
    std::string model;             // modelName
    std::string udn;
    std::string location;          // 设备描述 URL
    std::string host;
    int         port = 0;
    std::string control;           // AVTransport 控制地址（相对或绝对）
    std::string controlConnection; // ConnectionManager 控制地址（相对或绝对）

    std::string av_transport_url() const {
        return resolve_url(location, control);
    }
    std::string connection_manager_url() const {
        return resolve_url(location, controlConnection);
    }
};

namespace upnp_detail {
inline std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
inline std::string trim2(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
inline std::string header_of(const std::string& msg, const char* key) {
    const std::string l = lower(msg), k = lower(std::string(key) + ":");
    size_t p = l.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    const size_t e = msg.find('\n', p);
    return trim2(msg.substr(p, (e == std::string::npos ? msg.size() : e) - p));
}
inline std::string xml_tag(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    size_t p = xml.find(open);
    if (p == std::string::npos) return "";
    p += open.size();
    const size_t e = xml.find(close, p);
    if (e == std::string::npos) return "";
    return trim2(xml.substr(p, e - p));
}

// 在 XML 的 <serviceList> 中查找指定 serviceType 对应的 <controlURL>
inline std::string extract_service_control_url(const std::string& xml, const char* serviceSubstr) {
    size_t p = xml.find(serviceSubstr);
    if (p == std::string::npos) return "";
    size_t c2 = xml.find("<controlURL>", p);
    if (c2 == std::string::npos) return "";
    size_t e2 = xml.find("</controlURL>", c2);
    if (e2 == std::string::npos) return "";
    return trim2(xml.substr(c2 + 12, e2 - c2 - 12));
}

inline bool http_get(const std::string& host, int port, const std::string& path, std::string& out) {
    WSADATA w;
    bool wsaInit = (WSAStartup(MAKEWORD(2, 2), &w) == 0);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (wsaInit) WSACleanup();
        return false;
    }
    DWORD tv = 1500;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = ::inet_addr(host.c_str());
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        ::closesocket(s);
        if (wsaInit) WSACleanup();
        return false;
    }
    char req[512];
    std::snprintf(req, sizeof(req),
                  "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nUser-Agent: RLPlayer/1\r\n\r\n",
                  path.c_str(), host.c_str(), port);
    if (::send(s, req, (int)std::strlen(req), 0) <= 0) {
        ::closesocket(s);
        if (wsaInit) WSACleanup();
        return false;
    }
    char buf[8192];
    int r;
    out.clear();
    while ((r = ::recv(s, buf, sizeof(buf), 0)) > 0) {
        out.append(buf, (size_t)r);
        if (out.size() > 524288) break;
    }
    ::closesocket(s);
    if (wsaInit) WSACleanup();
    return !out.empty();
}

// ---------------- 供源发送：整块发完（2026-10-25，外部评审 P0-B）----------------
// 现场形态（本文件旧 561-564 行）：文件分支把 fread 的【一整块】交给一次 ::send，
// 返回值 nSend < nRead 时块尾直接丢弃，下一轮 fread 读的却是文件的【下一块】。
// 阻塞 socket 也会部分发送（内核发送缓冲满 / SO_SNDTIMEO），于是：
//   丢掉的字节数 = 块尾那几十 KB 里的任意几个 ⟹ 之后的流从那里开始整体错位。
// 对 WAV 这种"按字节解释采样"的格式，错位 = 从错位点起整条流变成噪声，
// 而且 Content-Length 依然对得上（对端不会报错），是最难从日志看出来的形态。
// 缓冲分支（旧 567-574 行）当年写对了，但它在 nSend<=0 时同样直接 break，
// WSAEWOULDBLOCK 一出现就截断 —— 所以两条分支现在共用下面这一份实现。
//
// 约定（调用方负责把 WSAGetLastError 映射成这三态）：
//   > 0 本次实际发出的字节数（可能远小于请求量）
//     0 暂时发不出去（WSAEWOULDBLOCK / WSAEINTR 之类），可短暂等待后重试
//    -1 永久错误（对端关闭 / 其它 socket 错误），放弃这一块
// 用模板而不是 std::function：供源每个 64KB 块要调用上千次，且测试要能塞桩。
template <class SendOnce, class SleepMs>
inline size_t send_block_all(SendOnce&& sendOnce, const char* buf, size_t len,
                             SleepMs&& sleepMs, const std::atomic<bool>* stop,
                             int stallBudgetMs = 5000) {
    size_t done = 0;
    int stalledMs = 0;
    while (done < len) {
        if (stop && stop->load()) break;
        const size_t left = len - done;
        const int ask = (int)std::min<size_t>(left, (size_t)1u << 20);   // 单次最多 1 MiB
        const int r = sendOnce(buf + done, ask);
        if (r > 0) {
            done += (size_t)r;
            stalledMs = 0;
            continue;
        }
        if (r < 0) break;                       // 永久错误：这一块发不完了
        if (++stalledMs > stallBudgetMs) break; // 1ms/次 ⟹ 迭代次数 ≈ 毫秒预算
        sleepMs(1);
    }
    return done;
}

// ★ 文件供源主循环（生产 handle_client 的 isFile 分支直接调用它，不是测试另抄一份）：
// 读一块 → 整块发完 → 下一块。返回实际发出的字节数。
// 关键不变量：某块没发完就【立刻停止】，绝不再读下一块 —— 否则又是旧实现那种块间丢字节。
template <class SendOnce, class SleepMs>
inline int64_t pump_file(std::FILE* f, int64_t toSend, size_t chunkBytes,
                         SendOnce&& sendOnce, SleepMs&& sleepMs,
                         const std::atomic<bool>* stop, int stallBudgetMs = 5000) {
    if (!f || toSend <= 0) return 0;
    if (chunkBytes == 0) chunkBytes = 65536;
    std::vector<unsigned char> chunk(chunkBytes);
    int64_t sent = 0;
    while (toSend > 0) {
        if (stop && stop->load()) break;
        const size_t want = (size_t)std::min<int64_t>(toSend, (int64_t)chunk.size());
        const size_t got = std::fread(chunk.data(), 1, want, f);
        if (got == 0) break;                    // EOF / 读错误
        const size_t ok = send_block_all(sendOnce, (const char*)chunk.data(), got,
                                        sleepMs, stop, stallBudgetMs);
        sent += (int64_t)ok;
        toSend -= (int64_t)ok;
        if (ok < got) break;                    // ★ 本块没发完 = 连接已废，不能再读下一块
    }
    return sent;
}

// 内存缓冲供源：同一套"整块发完"语义（不分块读，只分次 send）。
template <class SendOnce, class SleepMs>
inline int64_t pump_buffer(const unsigned char* data, int64_t toSend,
                           SendOnce&& sendOnce, SleepMs&& sleepMs,
                           const std::atomic<bool>* stop, int stallBudgetMs = 5000) {
    if (!data || toSend <= 0) return 0;
    return (int64_t)send_block_all(sendOnce, (const char*)data, (size_t)toSend,
                                   sleepMs, stop, stallBudgetMs);
}
} // namespace upnp_detail

// 发一次 SSDP M-SEARCH，收集 MediaRenderer。timeoutMs 建议 1500~2500。
//
// 2026-10-23（用户 261023-04 #11「UPnP 搜不到我的设备，现在应该还开着」）：
//   旧实现只在"默认路由"上发一次组播。本机装了 Clash Verge TUN（198.18.0.0/15 fake-IP）后，
//   默认路由指向 TUN 虚拟网卡，组播包进了隧道 → 局域网设备永远收不到 M-SEARCH（这正是我们复现到的
//   /api/upnp → n=0）。修法：枚举所有本机 IPv4，逐个网卡设置 IP_MULTICAST_IF 并发送 2 次，
//   再一起收响应；同时给出诊断信息（找到几个网卡、哪些被跳过），供 UI 如实显示。
struct UpnpDiscoverDiag {
    int ifaces_tried = 0;
    int msgs_sent = 0;
    std::string note;
};

inline std::vector<std::string> upnp_local_ipv4(std::vector<std::string>* skipped = nullptr) {
    std::vector<std::string> out;
    char host[256] = {0};
    if (::gethostname(host, sizeof(host)) != 0) return out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host, nullptr, &hints, &res) != 0) return out;
    for (addrinfo* p = res; p; p = p->ai_next) {
        if (!p->ai_addr) continue;
        char ip[64] = {0};
        sockaddr_in* sa = (sockaddr_in*)p->ai_addr;
        if (::inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)) == nullptr) continue;
        const std::string s(ip);
        if (s.rfind("127.", 0) == 0 || s.rfind("169.254.", 0) == 0) continue;
        // Clash/Mihomo TUN 的 fake-IP 段与常见虚拟网卡段：组播从这里出去没有意义，跳过但记录下来
        const bool virtual_like = (s.rfind("198.18.", 0) == 0 || s.rfind("198.19.", 0) == 0);
        if (virtual_like) { if (skipped) skipped->push_back(s + "(代理 TUN)"); continue; }
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    ::freeaddrinfo(res);
    return out;
}

inline std::vector<UpnpDevice> upnp_discover(int timeoutMs = 1800, UpnpDiscoverDiag* diag = nullptr) {
    using namespace upnp_detail;
    std::vector<UpnpDevice> out;

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return out;

    std::vector<std::string> skipped;
    std::vector<std::string> ips = upnp_local_ipv4(&skipped);
    if (ips.empty()) ips.push_back("0.0.0.0");   // 拿不到网卡时退回系统默认路由（老行为）

    const char* msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    to.sin_addr.s_addr = ::inet_addr("239.255.255.250");

    std::vector<SOCKET> socks;
    for (const auto& ip : ips) {
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        in_addr ifaddr{};
        ifaddr.s_addr = ::inet_addr(ip.c_str());
        if (ip != "0.0.0.0") {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(0);
            local.sin_addr = ifaddr;
            // bind 失败也继续（退回任意地址绑定），但组播出口仍按该网卡设置
            (void)::bind(s, (sockaddr*)&local, sizeof(local));
            ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifaddr, sizeof(ifaddr));
        }
        for (int rep = 0; rep < 2; ++rep) {
            ::sendto(s, msg, (int)std::strlen(msg), 0, (sockaddr*)&to, sizeof(to));
            if (diag) diag->msgs_sent++;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        socks.push_back(s);
        if (diag) diag->ifaces_tried++;
    }
    if (diag && !skipped.empty()) {
        std::string sk;
        for (size_t i = 0; i < skipped.size(); ++i) { if (i) sk += ", "; sk += skipped[i]; }
        diag->note = "已跳过虚拟/代理网卡: " + sk;
    }

    const ULONGLONG t0 = GetTickCount64();
    std::vector<std::string> seen_locations;
    while (GetTickCount64() - t0 < (ULONGLONG)timeoutMs && !socks.empty()) {
        fd_set rf;
        FD_ZERO(&rf);
        for (SOCKET s : socks) FD_SET(s, &rf);
        timeval tv{0, 150000};
        const int rc = ::select(0, &rf, nullptr, nullptr, &tv);
        if (rc <= 0) continue;
        for (size_t si = 0; si < socks.size(); ++si) {
            if (!FD_ISSET(socks[si], &rf)) continue;
            char buf[4096];
            sockaddr_in from{};
            int fl = sizeof(from);
            const int n = ::recvfrom(socks[si], buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;
            const std::string resp(buf, (size_t)n);
            std::string loc = header_of(resp, "LOCATION");
            if (loc.empty()) continue;

            const std::string lowerLoc = lower(loc);
            if (lowerLoc.rfind("http://", 0) != 0) continue;
            const size_t hs = 7;
            const size_t slash = loc.find('/', hs);
            const std::string hostport = loc.substr(hs, (slash == std::string::npos ? loc.size() : slash) - hs);
            const std::string path = (slash == std::string::npos) ? "/" : loc.substr(slash);
            std::string host = hostport;
            int port = 80;
            const size_t colon = hostport.find(':');
            if (colon != std::string::npos) {
                host = hostport.substr(0, colon);
                port = atoi(hostport.c_str() + colon + 1);
            }

            bool dup = false;
            for (auto& d : out) if (d.location == loc) { dup = true; break; }
            if (dup) continue;

            UpnpDevice dev;
            dev.location = loc;
            dev.host = host;
            dev.port = port;
            std::string xml;
            if (http_get(host, port, path, xml)) {
                dev.name    = xml_tag(xml, "friendlyName");
                dev.model   = xml_tag(xml, "modelName");
                dev.udn     = xml_tag(xml, "UDN");
                dev.control = extract_service_control_url(xml, "AVTransport");
                if (dev.control.empty()) dev.control = xml_tag(xml, "controlURL");
                dev.controlConnection = extract_service_control_url(xml, "ConnectionManager");
            }
            if (dev.name.empty()) dev.name = host;
            out.push_back(dev);
        }
    }
    for (SOCKET s : socks) ::closesocket(s);
    WSACleanup();
    return out;
}

// 查找本机面向指定目标主机的首选局域网 IP
inline std::string upnp_local_ip_for(const std::string& targetHost, uint16_t targetPort = 80) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return "127.0.0.1";

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return "127.0.0.1"; }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(targetPort ? targetPort : 80);
    dst.sin_addr.s_addr = ::inet_addr(targetHost.c_str());

    std::string out = "127.0.0.1";
    if (::connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        if (::getsockname(s, (sockaddr*)&local, &len) == 0) {
            char buf[64]{};
            ::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
            if (std::strlen(buf) > 0 && std::strcmp(buf, "0.0.0.0") != 0) {
                out = buf;
            }
        }
    }
    ::closesocket(s);
    WSACleanup();
    return out;
}

// ---------------- 本地 HTTP 供源服务器（WAV 流） ----------------

struct HttpAccessLog {
    std::string clientIp;
    uint16_t    clientPort = 0;
    std::string method;
    std::string path;
    std::string range;
    int         statusCode = 0;
    int64_t     bytesSent = 0;
};

class UpnpHttpServer {
public:
    UpnpHttpServer() = default;
    ~UpnpHttpServer() { stop(); }

    bool start(uint16_t port = 8912, const std::string& bindIp = "0.0.0.0") {
        stop();
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
        wsaStarted_ = true;

        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) { cleanup_wsa(); return false; }

        int yes = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = (bindIp == "0.0.0.0" || bindIp.empty())
                            ? htonl(INADDR_ANY) : ::inet_addr(bindIp.c_str());

        if (::bind(listen_, (sockaddr*)&a, sizeof(a)) != 0) {
            ::closesocket(listen_); listen_ = INVALID_SOCKET; cleanup_wsa(); return false;
        }
        if (::listen(listen_, 16) != 0) {
            ::closesocket(listen_); listen_ = INVALID_SOCKET; cleanup_wsa(); return false;
        }

        sockaddr_in got{}; int gl = sizeof(got);
        ::getsockname(listen_, (sockaddr*)&got, &gl);
        port_ = ntohs(got.sin_port);

        stop_.store(false);
        acceptThread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        stop_.store(true);
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        // 等待所有正在传输的 worker 线程退出
        while (activeWorkers_.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        cleanup_wsa();
    }

    uint16_t port() const { return port_; }
    bool is_running() const { return !stop_.load(); }

    // 注册本地文件，返回路径 "/stream/<token>.wav"
    std::string register_file(const std::string& filePath, const std::string& mimeType = "audio/wav") {
        StreamEntry entry;
        entry.token = generate_random_token(16);
        entry.path = "/stream/" + entry.token + ".wav";
        entry.isFile = true;
        entry.filePath = filePath;
        entry.mimeType = mimeType;
        std::lock_guard<std::mutex> lk(streamMutex_);
        streams_[entry.path] = entry;
        return entry.path;
    }

    // 注册内存缓冲（用于单元测试），返回路径 "/stream/<token>.wav"
    std::string register_buffer(std::vector<uint8_t> data, const std::string& mimeType = "audio/wav") {
        StreamEntry entry;
        entry.token = generate_random_token(16);
        entry.path = "/stream/" + entry.token + ".wav";
        entry.isFile = false;
        entry.buffer = std::move(data);
        entry.mimeType = mimeType;
        std::lock_guard<std::mutex> lk(streamMutex_);
        streams_[entry.path] = entry;
        return entry.path;
    }

    void unregister_stream(const std::string& streamPath) {
        std::lock_guard<std::mutex> lk(streamMutex_);
        streams_.erase(streamPath);
    }

    int64_t total_bytes_served() const { return totalBytesServed_.load(); }
    size_t total_requests_served() const { return totalRequestsServed_.load(); }

    std::vector<HttpAccessLog> get_access_logs() const {
        std::lock_guard<std::mutex> lk(logMutex_);
        return accessLogs_;
    }

    void set_access_callback(std::function<void(const HttpAccessLog&)> cb) {
        std::lock_guard<std::mutex> lk(logMutex_);
        accessCallback_ = std::move(cb);
    }

private:
    struct StreamEntry {
        std::string token;
        std::string path;
        bool isFile = false;
        std::string filePath;
        std::vector<uint8_t> buffer;
        std::string mimeType = "audio/wav";
    };

    void cleanup_wsa() {
        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
    }

    void loop() {
        while (!stop_.load()) {
            sockaddr_in clientAddr{};
            int addrLen = sizeof(clientAddr);
            SOCKET c = ::accept(listen_, (sockaddr*)&clientAddr, &addrLen);
            if (c == INVALID_SOCKET) {
                if (stop_.load()) break;
                continue;
            }
            char ipBuf[64]{};
            ::inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
            std::string clientIp = ipBuf;
            uint16_t clientPort = ntohs(clientAddr.sin_port);

            activeWorkers_++;
            std::thread([this, c, clientIp, clientPort] {
                handle_client(c, clientIp, clientPort);
                activeWorkers_--;
            }).detach();
        }
    }

    void handle_client(SOCKET c, const std::string& clientIp, uint16_t clientPort) {
        DWORD tv = 4000;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        // 单次 send 的适配层（P0-B）：把 winsock 错误码映射成 upnp_detail 的三态约定。
        //   WSAEWOULDBLOCK / WSAEINTR → 0  ⟹ 短暂发不出去，等待后重试
        //   其余（含 WSAETIMEDOUT、对端 RST、对端正常关闭）→ -1 ⟹ 放弃，绝不装作发完了
        // 注：阻塞 socket + SO_SNDTIMEO 触发 WSAETIMEDOUT 时，内核实际已发出的字节数不确定，
        // 重试可能重复发送，因此按永久错误处理（宁可让对端看到短流，也不产出重复字节）。
        auto sendOnce = [c](const char* p, int n) -> int {
            const int r = ::send(c, p, n, 0);
            if (r > 0) return r;
            if (r == 0) return -1;                                  // 对端已关闭
            const int e = ::WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAEINTR) return 0;     // 暂时发不出去
            return -1;
        };
        auto sleepMs = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        // 响应头也必须整块发完：头只发一半，对端直接解析失败
        auto sendAll = [&](const char* p, size_t n) {
            return upnp_detail::send_block_all(sendOnce, p, n, sleepMs, &stop_, 3000);
        };

        std::string reqBuf;
        char tmp[4096];
        while (reqBuf.find("\r\n\r\n") == std::string::npos && reqBuf.size() < 65536) {
            int r = ::recv(c, tmp, sizeof(tmp), 0);
            if (r <= 0) break;
            reqBuf.append(tmp, (size_t)r);
        }
        if (reqBuf.empty()) {
            ::closesocket(c);
            return;
        }

        // 解析首行
        std::string method, path;
        {
            size_t firstLineEnd = reqBuf.find("\r\n");
            std::string firstLine = (firstLineEnd != std::string::npos) ? reqBuf.substr(0, firstLineEnd) : reqBuf;
            std::istringstream ls(firstLine);
            ls >> method >> path;
        }

        // 提取 Range 头
        std::string rangeHeader;
        {
            std::string lowerReq = upnp_detail::lower(reqBuf);
            size_t p = lowerReq.find("range:");
            if (p != std::string::npos) {
                size_t e = reqBuf.find('\n', p);
                rangeHeader = upnp_detail::trim2(reqBuf.substr(p + 6, (e == std::string::npos ? reqBuf.size() : e) - (p + 6)));
            }
        }

        // 查找对应 stream
        StreamEntry entry;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(streamMutex_);
            auto it = streams_.find(path);
            if (it != streams_.end()) {
                entry = it->second;
                found = true;
            }
        }

        // 校验 token 与防目录遍历
        if (!found || !validate_stream_token(path, entry.token)) {
            const char* notFoundResp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(notFoundResp, std::strlen(notFoundResp));
            record_log(clientIp, clientPort, method, path, rangeHeader, 404, 0);
            ::closesocket(c);
            return;
        }

        int64_t totalSize = 0;
        FILE* fileHandle = nullptr;
        if (entry.isFile) {
            fileHandle = std::fopen(entry.filePath.c_str(), "rb");
            if (!fileHandle) {
                const char* notFoundResp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                sendAll(notFoundResp, std::strlen(notFoundResp));
                record_log(clientIp, clientPort, method, path, rangeHeader, 404, 0);
                ::closesocket(c);
                return;
            }
            _fseeki64(fileHandle, 0, SEEK_END);
            totalSize = _ftelli64(fileHandle);
            _fseeki64(fileHandle, 0, SEEK_SET);
        } else {
            totalSize = (int64_t)entry.buffer.size();
        }

        HttpRange range = parse_http_range(rangeHeader, totalSize);
        StreamResponseHeaders respHeaders = build_stream_response_headers(range, totalSize, entry.mimeType);

        sendAll(respHeaders.headerString.data(), respHeaders.headerString.size());

        int64_t sent = 0;
        if (method == "GET" && (respHeaders.statusCode == 200 || respHeaders.statusCode == 206)) {
            const int64_t toSend = respHeaders.contentLength;
            if (entry.isFile && fileHandle) {
                _fseeki64(fileHandle, respHeaders.rangeStart, SEEK_SET);
                // ★ P0-B：读一块 → **整块发完** → 再读下一块。
                // 旧实现每个块只 send 一次，部分发送时块尾字节被丢弃、下一轮读的是文件下一块
                // ⟹ 流从那里整体错位（长度还对得上，最难发现）。
                sent = upnp_detail::pump_file(fileHandle, toSend, 65536, sendOnce, sleepMs, &stop_);
            } else if (!entry.isFile) {
                sent = upnp_detail::pump_buffer(entry.buffer.data() + respHeaders.rangeStart,
                                                toSend, sendOnce, sleepMs, &stop_);
            }
        }
        if (fileHandle) std::fclose(fileHandle);

        totalBytesServed_ += sent;
        totalRequestsServed_++;
        record_log(clientIp, clientPort, method, path, rangeHeader, respHeaders.statusCode, sent);
        ::closesocket(c);
    }

    void record_log(const std::string& clientIp, uint16_t clientPort,
                    const std::string& method, const std::string& path,
                    const std::string& range, int status, int64_t sent) {
        HttpAccessLog entry;
        entry.clientIp = clientIp;
        entry.clientPort = clientPort;
        entry.method = method;
        entry.path = path;
        entry.range = range;
        entry.statusCode = status;
        entry.bytesSent = sent;
        std::lock_guard<std::mutex> lk(logMutex_);
        accessLogs_.push_back(entry);
        if (accessCallback_) accessCallback_(entry);
    }

    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    bool wsaStarted_ = false;
    std::atomic<bool> stop_{true};
    std::atomic<int> activeWorkers_{0};
    std::thread acceptThread_;

    std::map<std::string, StreamEntry> streams_;
    mutable std::mutex streamMutex_;

    std::atomic<int64_t> totalBytesServed_{0};
    std::atomic<size_t>  totalRequestsServed_{0};

    std::vector<HttpAccessLog> accessLogs_;
    std::function<void(const HttpAccessLog&)> accessCallback_;
    mutable std::mutex logMutex_;
};

} // namespace rta::net
