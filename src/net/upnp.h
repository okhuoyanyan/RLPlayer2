// upnp.h — UPnP/DLNA 媒体渲染器发现（只读，不涉及任何音频输出）
//
// 为什么只做"发现"：真正的 DLNA 投放要经过渲染器的解码/重采样，
// 音质不可控（见 docs/音乐平台截流可行性.md 的同类结论）。
// 所以这里如实列出局域网里有哪些设备、控制地址是什么，
// 让用户知道"另一台机器上有什么"，而不是假装能无损投放。
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
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rta::net {

struct UpnpDevice {
    std::string name;        // friendlyName
    std::string model;       // modelName
    std::string udn;
    std::string location;    // 设备描述 URL
    std::string host;
    int         port = 0;
    std::string control;     // AVTransport 控制地址（相对）
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
inline bool http_get(const std::string& host, int port, const std::string& path, std::string& out) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD tv = 1500;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = ::inet_addr(host.c_str());
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) { ::closesocket(s); return false; }
    char req[512];
    std::snprintf(req, sizeof(req),
                  "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nUser-Agent: RLPlayer/1\r\n\r\n",
                  path.c_str(), host.c_str(), port);
    if (::send(s, req, (int)std::strlen(req), 0) <= 0) { ::closesocket(s); return false; }
    char buf[8192];
    int r;
    out.clear();
    while ((r = ::recv(s, buf, sizeof(buf), 0)) > 0) {
        out.append(buf, (size_t)r);
        if (out.size() > 262144) break;
    }
    ::closesocket(s);
    return !out.empty();
}
} // namespace upnp_detail

// 发一次 SSDP M-SEARCH，收集 MediaRenderer。timeoutMs 建议 1500~2500。
inline std::vector<UpnpDevice> upnp_discover(int timeoutMs = 1800) {
    using namespace upnp_detail;
    std::vector<UpnpDevice> out;

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return out;

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return out; }

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
    ::sendto(s, msg, (int)std::strlen(msg), 0, (sockaddr*)&to, sizeof(to));

    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < (ULONGLONG)timeoutMs) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv{0, 150000};
        const int rc = ::select(0, &rf, nullptr, nullptr, &tv);
        if (rc <= 0) continue;
        char buf[4096];
        sockaddr_in from{};
        int fl = sizeof(from);
        const int n = ::recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        const std::string resp(buf, (size_t)n);
        std::string loc = header_of(resp, "LOCATION");
        if (loc.empty()) continue;
        // http://host:port/path
        const std::string lowerLoc = lower(loc);
        if (lowerLoc.rfind("http://", 0) != 0) continue;
        const size_t hs = 7;
        const size_t slash = loc.find('/', hs);
        const std::string hostport = loc.substr(hs, (slash == std::string::npos ? loc.size() : slash) - hs);
        const std::string path = (slash == std::string::npos) ? "/" : loc.substr(slash);
        std::string host = hostport;
        int port = 80;
        const size_t colon = hostport.find(':');
        if (colon != std::string::npos) { host = hostport.substr(0, colon); port = atoi(hostport.c_str() + colon + 1); }

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
            dev.control = xml_tag(xml, "controlURL");
            // controlURL 有时是绝对地址
            if (dev.control.rfind("http://", 0) == 0) {
                const std::string L = lower(dev.control);
                const size_t sl = dev.control.find('/', 7);
                dev.control = (sl == std::string::npos) ? "/" : dev.control.substr(sl);
            }
        }
        if (dev.name.empty()) dev.name = host;
        out.push_back(dev);
    }
    ::closesocket(s);
    WSACleanup();
    return out;
}

} // namespace rta::net
