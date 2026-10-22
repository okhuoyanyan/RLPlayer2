// naa_probe.cpp — NAA 控制通道黑盒探针（只连 TCP 43210，不发音频、不打开任何设备）
//
// 目的：搞清 NAA（networkaudiod）在 TCP 连接上的握手顺序与报文封装。
// 背景：实测 5.19 / Linux 端点都在 accept 后**先发 16 字节随机数**，随后等待客户端报文；
//       直接发 XML 收不到任何回应。本工具把几种可能的封装都试一遍，并把收到的字节
//       按 hex + ascii 打出来，便于逐字节对照。
//
// 用法：
//   naa_probe --host 127.0.0.1
//   naa_probe --host 192.168.50.109 --mode token+xml --xml "<protocol></protocol>"
//   naa_probe --matrix                 # 一次跑完全部封装组合
//
// 只做 TCP 交换，不涉及 UDP 音频，也不会发出可听声音。
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

static std::string hexdump(const std::vector<uint8_t>& v, size_t maxBytes = 256) {
    std::string s;
    char b[8];
    const size_t n = v.size() < maxBytes ? v.size() : maxBytes;
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02X ", v[i]);
        s += b;
        if ((i & 15) == 15) s += '\n';
    }
    if (v.size() > maxBytes) s += "...";
    return s;
}

static std::string printable(const std::vector<uint8_t>& v, size_t maxBytes = 512) {
    std::string s;
    const size_t n = v.size() < maxBytes ? v.size() : maxBytes;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = v[i];
        s += (c >= 32 && c < 127) ? (char)c : '.';
    }
    return s;
}

static bool recv_for(SOCKET s, int ms, std::vector<uint8_t>& out, bool* closed = nullptr) {
    out.clear();
    const DWORD t0 = GetTickCount();
    uint8_t buf[4096];
    while ((int)(GetTickCount() - t0) < ms) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{ 0, 100 * 1000 };
        const int sel = select(0, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        const int n = recv(s, (char*)buf, sizeof(buf), 0);
        if (n <= 0) { if (closed) *closed = true; break; }
        out.insert(out.end(), buf, buf + n);
    }
    return !out.empty();
}

static bool send_all(SOCKET s, const std::vector<uint8_t>& v) {
    size_t sent = 0;
    while (sent < v.size()) {
        const int n = send(s, (const char*)v.data() + sent, (int)(v.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static std::vector<uint8_t> bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    std::string host = "127.0.0.1", mode = "silent", xml = "<protocol></protocol>";
    int port = 43210, waitMs = 900;
    bool matrix = false;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](const char* d) -> std::string { return (i + 1 < argc) ? argv[++i] : d; };
        if (k == "--host") host = next("127.0.0.1");
        else if (k == "--port") port = std::atoi(next("43210").c_str());
        else if (k == "--mode") mode = next("silent");
        else if (k == "--xml") xml = next("<protocol></protocol>");
        else if (k == "--wait") waitMs = std::atoi(next("900").c_str());
        else if (k == "--matrix") matrix = true;
        else if (k == "-h" || k == "--help") {
            printf("用法: naa_probe --host <ip> [--port 43210] [--mode silent|xml|token+xml|zeros+xml|len32le|len32be|random16]\n"
                   "                 [--xml \"<protocol></protocol>\"] [--wait 900] [--matrix]\n");
            return 0;
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup 失败\n"); return 2; }

    std::vector<std::string> modes;
    if (matrix) modes = { "silent", "xml", "token+xml", "zeros+xml", "len32le", "len32be", "random16" };
    else modes = { mode };

    for (const auto& m : modes) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)port);
        inet_pton(AF_INET, host.c_str(), &a.sin_addr);
        printf("\n=== %s:%d  mode=%s ===\n", host.c_str(), port, m.c_str());
        if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
            printf("连接失败: %d\n", WSAGetLastError());
            closesocket(s);
            continue;
        }
        // 1) 先收服务端主动发来的东西（实测 16 字节随机数）
        std::vector<uint8_t> greeting;
        bool closed = false;
        recv_for(s, 600, greeting, &closed);
        printf("  服务端主动发送: %zu 字节%s\n", greeting.size(), closed ? "（随后关闭）" : "");
        if (!greeting.empty()) printf("  hex: %s\n  ascii: %s\n", hexdump(greeting).c_str(), printable(greeting).c_str());

        // 2) 按模式构造发送内容
        std::vector<uint8_t> payload;
        if (m == "xml") {
            payload = bytes(xml);
        } else if (m == "token+xml") {
            payload = greeting;
            const auto x = bytes(xml);
            payload.insert(payload.end(), x.begin(), x.end());
        } else if (m == "zeros+xml") {
            payload.assign(16, 0);
            const auto x = bytes(xml);
            payload.insert(payload.end(), x.begin(), x.end());
        } else if (m == "len32le" || m == "len32be") {
            const auto x = bytes(xml);
            const uint32_t n = (uint32_t)x.size();
            payload.resize(4);
            if (m == "len32le") { for (int i = 0; i < 4; ++i) payload[i] = (uint8_t)((n >> (8 * i)) & 0xFF); }
            else { for (int i = 0; i < 4; ++i) payload[i] = (uint8_t)((n >> (8 * (3 - i))) & 0xFF); }
            payload.insert(payload.end(), x.begin(), x.end());
        } else if (m == "random16") {
            payload.assign(16, 0);
            for (auto& b : payload) b = (uint8_t)(rand() & 0xFF);
        }
        if (!payload.empty()) {
            printf("  发送 %zu 字节: %s\n", payload.size(), printable(payload, 160).c_str());
            if (!send_all(s, payload)) printf("  发送失败\n");
        } else {
            printf("  不发送（只观察服务端）\n");
        }

        // 3) 再收回应
        std::vector<uint8_t> resp;
        closed = false;
        recv_for(s, waitMs, resp, &closed);
        printf("  回应: %zu 字节%s\n", resp.size(), closed ? "（连接被关闭）" : "");
        if (!resp.empty()) printf("  hex: %s\n  ascii: %s\n", hexdump(resp).c_str(), printable(resp).c_str());
        closesocket(s);
    }
    WSACleanup();
    return 0;
}
