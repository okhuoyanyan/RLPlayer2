// naa_scan.cpp — NAA (Network Audio Adapter) 端点发现
//
// 为什么用端口扫描而不是协议发现：
//   NAA 的发现报文格式未公开（Round 1 调研结论：无规范、无源码、无 dissector）。
//   但 networkaudiod 固定监听 TCP/UDP 43210 —— 端口是否开放是可靠且无需协议知识的判据。
//
// 两种探测：
//   1) 主动扫描本网段 TCP 43210（并发，短超时）
//   2) 加入已知组播组被动监听（224.0.0.199 / 239.192.0.199，端口 43210），
//      看有没有周期性的 NAA 通告
//
// 完全只读探测，不涉及任何音频输出。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

static constexpr int NAA_PORT = 43210;

static void ensure_wsa() {
    static bool done = false;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; }
}

// 探测单个主机：TCP 连接 NAA_PORT
static bool probe_host(const std::string& ip, int timeoutMs) {
    ensure_wsa();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)NAA_PORT);
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
    }
    ::closesocket(s);
    return ok;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    ensure_wsa();

    std::string base = "192.168.50.";
    int timeoutMs = 400;
    double listenSec = 6.0;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--subnet" && i + 1 < argc) { base = argv[++i]; if (!base.empty() && base.back() != '.') base += '.'; }
        else if (k == "--timeout" && i + 1 < argc) timeoutMs = atoi(argv[++i]);
        else if (k == "--listen" && i + 1 < argc) listenSec = atof(argv[++i]);
    }

    printf("NAA 端点发现（只读探测，不输出任何声音）\n");
    printf("目标端口 TCP/UDP %d\n\n", NAA_PORT);

    // ---- 1) 主动扫描 ----
    printf("[1] 扫描本网段 %s1-%s254 的 TCP %d ...\n", base.c_str(), base.c_str(), NAA_PORT);
    std::vector<std::string> found;
    std::mutex mu;
    std::atomic<int> next{1};
    const int nThreads = 32;
    std::vector<std::thread> pool;
    for (int t = 0; t < nThreads; ++t) {
        pool.emplace_back([&] {
            for (;;) {
                const int i = next.fetch_add(1);
                if (i > 254) break;
                const std::string ip = base + std::to_string(i);
                if (probe_host(ip, timeoutMs)) {
                    std::lock_guard<std::mutex> lk(mu);
                    found.push_back(ip);
                }
            }
        });
    }
    for (auto& th : pool) th.join();

    if (found.empty()) {
        printf("    未发现开放 %d 的主机\n", NAA_PORT);
    } else {
        printf("    发现 %zu 个候选：\n", found.size());
        for (const auto& ip : found) printf("      %s:%d\n", ip.c_str(), NAA_PORT);
    }

    // ---- 2) 被动监听组播 ----
    printf("\n[2] 被动监听 NAA 组播 %0.2f 秒 ...\n", listenSec);
    {
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            int yes = 1;
            ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((u_short)NAA_PORT);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(s, (sockaddr*)&a, sizeof(a)) == 0) {
                // 加入调研中提取到的两个 IPv4 组播组
                const char* groups[] = { "224.0.0.199", "239.192.0.199" };
                for (const char* g : groups) {
                    ip_mreq m{};
                    ::inet_pton(AF_INET, g, &m.imr_multiaddr);
                    m.imr_interface.s_addr = htonl(INADDR_ANY);
                    ::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof(m));
                }
                DWORD tv = (DWORD)(listenSec * 1000);
                ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
                printf("    已加入 224.0.0.199 / 239.192.0.199\n");
                char buf[8192];
                int n = 0;
                for (;;) {
                    sockaddr_in from{}; int fl = sizeof(from);
                    const int r = ::recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
                    if (r <= 0) break;
                    buf[r] = 0;
                    char ip[64]{}; ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    printf("    + 来自 %s:%u  (%d 字节)\n", ip, ntohs(from.sin_port), r);
                    std::string txt(buf, (size_t)r);
                    // 只打印可打印部分，避免二进制噪声
                    std::string pr;
                    for (unsigned char ch : txt) pr += (ch >= 32 && ch < 127) ? (char)ch : '.';
                    if (pr.size() > 400) pr = pr.substr(0, 400) + "...";
                    printf("      %s\n", pr.c_str());
                    if (++n >= 20) break;
                }
                if (n == 0) printf("    监听期间未收到任何 NAA 报文\n");
            } else {
                printf("    !! 无法绑定 UDP %d（可能被占用）\n", NAA_PORT);
            }
            ::closesocket(s);
        }
    }

    printf("\n结论：\n");
    if (found.empty()) {
        printf("  本网段未发现 NAA 端点。可能原因：\n");
        printf("    * NAA 设备不在本网段\n");
        printf("    * 设备未开启 / 未运行 networkaudiod\n");
        printf("    * 防火墙拦截\n");
        printf("  若你的接收设备支持 NAA，请确认它与本机在同一子网且已运行 NAA 服务。\n");
    } else {
        printf("  以上主机开放 %d 端口，很可能是 NAA 端点，可作为 RTANet/NAA 对接目标。\n", NAA_PORT);
    }
    WSACleanup();
    return 0;
}
