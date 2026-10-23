// tools/naa_discover.h — NAA 端点自动发现（只读端口探测，不输出任何声音）
//
// 261024-01 #11：NAA 端点不应该让用户手填 host:port，正常路径应当自动获取。
//
// 为什么用端口扫描而不是协议发现（见 docs/NAA发现纪要.md、docs/NAA对接探测纪要.md）：
//   NAA 的发现报文格式未公开（无规范/无 dissector，主动查询 6 种写法均无回应），
//   但 networkaudiod / rtanet_rx 固定监听 TCP/UDP 43210 —— 端口是否开放是可靠、
//   且不需要协议知识的判据。
//
// 本文件把 tools/naa_scan.cpp 的探测逻辑抽成可复用函数，供 CLI 与 rtaweb 的
// /api/naa/discover 共用（一处实现，两处调用，避免两套判据漂移）：
//   1) 枚举本机所有「已启用 + 有单播 IPv4」网卡的 /24，并发探测 TCP 43210；
//   2) 先探已知主机（127.0.0.1 / 调用方给的线索），再扫网段；
//   3) 一个都没找到时，被动监听 NAA 组播（224.0.0.199 / 239.192.0.199）一小段时间。
// 全程只读：不发送任何音频数据，不打开任何音频设备。
#pragma once

#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rta::naa {

inline constexpr uint16_t kPort = 43210;

struct Endpoint {
    std::string host;
    uint16_t port = kPort;
};

struct DiscoverResult {
    std::vector<Endpoint>    endpoints;      // 开放 TCP 43210 的主机
    std::vector<std::string> subnets;        // 实际扫描过的网段前缀（"192.168.50."）
    int  probed = 0;                         // 探测过的主机数
    bool multicastHeard = false;             // 组播监听是否收到过报文
    std::vector<std::string> multicastFrom;  // 发来组播的主机
    std::string note;                        // 给人看的一句话结论（UI 直接显示）
};

inline void ensure_wsa() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
    });
}

// RFC 2544 基准测试保留段 198.18.0.0/15 —— 代理 TUN（Clash / Meta 等）默认拿它做 fake-IP，
// 不是真实局域网地址，不能当 NAA 端点。
inline bool is_fakeip_range(const std::string& ip) {
    return ip.rfind("198.18.", 0) == 0 || ip.rfind("198.19.", 0) == 0;
}

// 代理 TUN 的判别（本机实测踩到两次）：
//   Clash/Meta 的 TUN 使用 fake-IP，并且**劫持了去往局域网的路由**——
// follow 192.168.50.200 的最优路由是 Meta(198.18.0.1)，于是 connect 立即"成功"，
// 254 个不存在的地址全被当成开放 43210。判据不能只看 "connect 成功"：
//   ① 先问系统"到该地址的最优接口是不是物理网卡"（GetBestInterfaceEx + IfType 白名单）；
//   ② connect 后再看本地源地址：经 TUN 时源地址落在 198.18.0.0/15，直接判否。
inline std::vector<DWORD> physical_if_indices() {
    static std::mutex mu;
    static std::vector<DWORD> cached;
    static std::chrono::steady_clock::time_point cachedAt{};
    std::lock_guard<std::mutex> lk(mu);
    const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - cachedAt).count();
    if (!cached.empty() && age < 60.0) return cached;
    cached.clear();
    ULONG size = 16 * 1024;
    std::vector<char> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG rr = ::GetAdaptersAddresses(AF_INET, flags, nullptr,
                                          (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
        if (rr == ERROR_BUFFER_OVERFLOW) { buf.resize(size); continue; }
        if (rr != NO_ERROR) break;
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
            cached.push_back(a->IfIndex);
        }
        break;
    }
    cachedAt = std::chrono::steady_clock::now();
    return cached;
}

// 到该地址的最优路由是不是走物理网卡（Ethernet / Wi-Fi）？
// 注意：回环地址（127.0.0.1）放行 —— 本机自带的 networkaudiod / rtanet_rx 也算端点。
inline bool routes_via_physical(const std::string& ip) {
    if (ip.rfind("127.", 0) == 0) return true;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::inet_addr(ip.c_str());
    DWORD ifIdx = 0;
    if (::GetBestInterfaceEx((sockaddr*)&a, &ifIdx) != NO_ERROR) return false;
    const auto ids = physical_if_indices();
    return std::find(ids.begin(), ids.end(), ifIdx) != ids.end();
}

// 探测单个主机：非阻塞 connect + select。只读，不发送应用层数据。
inline bool probe_tcp(const std::string& ip, int timeoutMs) {
    if (!routes_via_physical(ip)) return false;
    ensure_wsa();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)kPort);
    a.sin_addr.s_addr = ::inet_addr(ip.c_str());
    ::connect(s, (sockaddr*)&a, sizeof(a));
    fd_set wf, ef;
    FD_ZERO(&wf); FD_SET(s, &wf);
    FD_ZERO(&ef); FD_SET(s, &ef);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int r = ::select(0, nullptr, &wf, &ef, &tv);
    bool ok = false;
    if (r > 0 && FD_ISSET(s, &wf)) {
        int err = 0; int len = sizeof(err);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        ok = (err == 0);
        if (ok) {
            // 二次确认：经代理 TUN 的连接会以 TUN 的 fake-IP 作源地址（或 0.0.0.0）
            sockaddr_in local{};
            int ll = sizeof(local);
            if (::getsockname(s, (sockaddr*)&local, &ll) == 0) {
                char lip[64]{};
                ::inet_ntop(AF_INET, &local.sin_addr, lip, sizeof(lip));
                const std::string ls(lip);
                if (ls.empty() || ls == "0.0.0.0" || is_fakeip_range(ls)) ok = false;
            }
        }
    }
    ::closesocket(s);
    return ok;
}

// 本机所有"已启用 + 有单播 IPv4"的网卡的 /24 前缀（含尾部点，例如 "192.168.50."）。
//
// ⚠️ 必须只取**物理局域网**网卡，否则假端点会淹没结果（本机实测踩到）：
//   Clash/Meta 之类的 TUN 网卡使用 198.18.0.0/15 fake-IP —— TUN 对**任意**目标
//   TCP 连接都会立即"成功"，于是 198.18.0.1-254 全被当成开放 43210，一次扫出 255 个假端点。
// 规则：
//   * IfType 只接受 Ethernet(6) / IEEE80211(71)（TUN=131 / PPP / 其它一律跳过）；
//   * 跳过回环、169.254 链路本地；
//   * 显式跳过 198.18.0.0/15 与 198.19.0.0/24（RFC 2544 基准测试保留段，代理 TUN 常用）；
//   * 去重；最多 maxSubnets 个（防止多网卡把扫描拖长）。
inline std::vector<std::string> local_subnet_prefixes(size_t maxSubnets = 4) {
    ensure_wsa();
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<char> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG r = ::GetAdaptersAddresses(AF_INET, flags, nullptr,
                                         (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
        if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(size); continue; }
        if (r != NO_ERROR) break;
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            // 只要物理 Ethernet / Wi-Fi：代理 TUN（Clash Meta 的 InterfaceDescription="Meta Tunnel"）
            // 的 IfType=131，会把任意目标都"连成功"，从而制造 255 个假端点。
            if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                char ip[64]{};
                auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
                ::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                const std::string s(ip);
                if (s.rfind("127.", 0) == 0 || s.rfind("169.254.", 0) == 0) continue;
                if (is_fakeip_range(s)) continue;   // 代理 TUN fake-IP 段不是真实局域网
                // "a.b.c.d" → "a.b.c."
                size_t dots = 0, cut = std::string::npos;
                for (size_t i = 0; i < s.size(); ++i) {
                    if (s[i] == '.' && ++dots == 3) { cut = i; break; }
                }
                if (cut == std::string::npos) continue;
                const std::string prefix = s.substr(0, cut + 1);
                if (std::find(out.begin(), out.end(), prefix) == out.end()) out.push_back(prefix);
                if (out.size() >= maxSubnets) return out;
            }
        }
        break;
    }
    return out;
}

// 组播被动监听：NAA 端点不发周期通告（实测 8 秒 0 报文），所以这只是"万一"的补充证据，
// 不是主判据。返回收到报文的主机列表。
inline std::vector<std::string> listen_multicast(double listenSec) {
    std::vector<std::string> from;
    if (listenSec <= 0.0) return from;
    ensure_wsa();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return from;
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)kPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) == 0) {
        const char* groups[] = { "224.0.0.199", "239.192.0.199" };
        for (const char* g : groups) {
            ip_mreq m{};
            ::inet_pton(AF_INET, g, &m.imr_multiaddr);
            m.imr_interface.s_addr = htonl(INADDR_ANY);
            ::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof(m));
        }
        DWORD tv = (DWORD)(listenSec * 1000.0);
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        char buf[8192];
        for (;;) {
            sockaddr_in src{}; int fl = sizeof(src);
            const int r = ::recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&src, &fl);
            if (r <= 0) break;
            char ip[64]{};
            ::inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
            const std::string h(ip);
            if (std::find(from.begin(), from.end(), h) == from.end()) from.push_back(h);
            if (from.size() >= 16) break;
        }
    }
    ::closesocket(s);
    return from;
}

// 主动发现。timeoutMs = 单机 connect 超时；threads = 并发探测线程数。
// extraHosts：先探这些地址（调用方可以塞入"上次成功过的端点/UPnP 设备地址"做快路）。
// forcedSubnet：非空时只扫这个前缀（CLI 的 --subnet，保持与 naa_scan 旧行为一致）。
inline DiscoverResult discover(int timeoutMs = 300, int threads = 48, double listenSec = 1.5,
                               const std::vector<std::string>& extraHosts = {},
                               const std::string& forcedSubnet = "") {
    ensure_wsa();
    DiscoverResult res;

    // 先探本机回环：本机自己跑 networkaudiod / rtanet_rx 时 127.0.0.1:43210 是合法端点
    //（旧版手动框的默认值就是它）。路由判据对 127.* 放行。
    std::vector<std::string> hosts;
    hosts.push_back("127.0.0.1");
    for (const auto& h : extraHosts) {
        if (!h.empty() && std::find(hosts.begin(), hosts.end(), h) == hosts.end()) hosts.push_back(h);
    }
    if (!forcedSubnet.empty()) {
        std::string base = forcedSubnet;
        if (base.back() != '.') base += '.';
        res.subnets.push_back(base);
        for (int i = 1; i <= 254; ++i) hosts.push_back(base + std::to_string(i));
    } else {
        res.subnets = local_subnet_prefixes();
        for (const auto& base : res.subnets) {
            for (int i = 1; i <= 254; ++i) hosts.push_back(base + std::to_string(i));
        }
    }
    res.probed = (int)hosts.size();

    std::vector<std::string> found;
    std::mutex mu;
    std::atomic<size_t> next{0};
    const int nThreads = std::max(1, std::min(threads, (int)hosts.size()));
    std::vector<std::thread> pool;
    pool.reserve((size_t)nThreads);
    for (int t = 0; t < nThreads; ++t) {
        pool.emplace_back([&] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= hosts.size()) break;
                if (probe_tcp(hosts[i], timeoutMs)) {
                    std::lock_guard<std::mutex> lk(mu);
                    found.push_back(hosts[i]);
                }
            }
        });
    }
    for (auto& th : pool) th.join();

    // 防御性过滤：即使调用方用 forcedSubnet 指定了 fake-IP 段，也不把它当 NAA 端点
    found.erase(std::remove_if(found.begin(), found.end(),
                               [](const std::string& ip) { return is_fakeip_range(ip); }),
                found.end());
    std::sort(found.begin(), found.end());
    for (const auto& ip : found) res.endpoints.push_back(Endpoint{ ip, kPort });

    if (res.endpoints.empty() && listenSec > 0.0) {
        res.multicastFrom = listen_multicast(listenSec);
        res.multicastHeard = !res.multicastFrom.empty();
    }

    // 结论文案：UI 直接显示，必须说清"为什么没发现"，而不是只给一个空列表。
    if (!res.endpoints.empty()) {
        res.note = "发现 " + std::to_string(res.endpoints.size()) +
                   " 个开放 TCP " + std::to_string((int)kPort) + " 的主机（NAA / RTANet 端点）";
    } else {
        std::string subnets;
        for (size_t i = 0; i < res.subnets.size(); ++i) {
            if (i) subnets += "、";
            subnets += res.subnets[i] + "0/24";
        }
        res.note = "未发现 NAA 端点（扫描 " + (subnets.empty() ? std::string("本机网段") : subnets) +
                   ":" + std::to_string((int)kPort) + " 无响应）：NAA 服务未开启（数播开 UPnP 会关掉 NAA）、不同子网或防火墙拦截。";
    }
    return res;
}

} // namespace rta::naa
