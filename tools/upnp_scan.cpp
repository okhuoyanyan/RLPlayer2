// upnp_scan.cpp — UPnP/DLNA 媒体渲染器发现（只读探测，不涉及任何音频输出）
//
// 流程：
//   1) 向 239.255.255.250:1900 发 SSDP M-SEARCH（ST = MediaRenderer:1）
//   2) 收集各设备回包的 LOCATION: http://ip:port/desc.xml
//   3) HTTP GET 设备描述，解析 friendlyName / UDN / AVTransport 控制地址
//
// 这是接入"用户现成接收 DAC"的第一步。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

static std::string to_lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
// 取响应头里的某个字段（大小写不敏感）
static std::string header_of(const std::string& msg, const char* key) {
    const std::string lower = to_lower(msg), k = to_lower(std::string(key) + ":");
    size_t p = lower.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = msg.find('\n', p);
    return trim(msg.substr(p, (e == std::string::npos ? msg.size() : e) - p));
}
// 取 XML 中第一个 <tag>...</tag> 的内容
static std::string xml_tag(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    size_t p = xml.find(open);
    if (p == std::string::npos) return "";
    p += open.size();
    size_t e = xml.find(close, p);
    if (e == std::string::npos) return "";
    return trim(xml.substr(p, e - p));
}

static bool http_get(const std::string& host, uint16_t port, const std::string& path, std::string& out) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = ::inet_addr(host.c_str());
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) { ::closesocket(s); return false; }
    char req[512];
    std::snprintf(req, sizeof(req),
                  "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nUser-Agent: RTANet/1\r\n\r\n",
                  path.c_str(), host.c_str(), (unsigned)port);
    if (::send(s, req, (int)std::strlen(req), 0) <= 0) { ::closesocket(s); return false; }
    char buf[8192]; int r; out.clear();
    while ((r = ::recv(s, buf, sizeof(buf), 0)) > 0) out.append(buf, (size_t)r);
    ::closesocket(s);
    return !out.empty();
}

// ---- 自检：起一个只服务固定设备描述的本地 HTTP 服务，验证"抓取 + 解析"两半都正确 ----
static const char* kCannedDevice = R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
    <friendlyName>RTANet SelfTest Renderer</friendlyName>
    <modelName>RTANet-Mock-1</modelName>
    <UDN>uuid:12345678-1234-1234-1234-123456789abc</UDN>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>
        <controlURL>/upnp/control/AVTransport1</controlURL>
      </service>
    </serviceList>
  </device>
</root>
)";

static int selftest() {
    printf("=== UPnP 解析自检 ===\n");
    int fails = 0;
    auto chk = [&](bool ok, const char* what, const std::string& detail) {
        printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
        if (!ok) ++fails;
    };

    // 1) SSDP 回包头部解析
    const std::string ssdp =
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: http://192.168.1.50:49152/desc.xml\r\n"
        "SERVER: Linux/4.4 UPnP/1.0 MockRenderer/1.0\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
        "USN: uuid:12345678-1234-1234-1234-123456789abc::urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
    chk(header_of(ssdp, "LOCATION") == "http://192.168.1.50:49152/desc.xml",
        "SSDP LOCATION 解析", "-> " + header_of(ssdp, "LOCATION"));
    chk(header_of(ssdp, "location") == "http://192.168.1.50:49152/desc.xml",
        "字段名大小写不敏感", "");
    chk(header_of(ssdp, "ST") == "urn:schemas-upnp-org:device:MediaRenderer:1", "SSDP ST 解析", "");

    // 2) 设备描述 XML 解析
    chk(xml_tag(kCannedDevice, "friendlyName") == "RTANet SelfTest Renderer",
        "friendlyName 解析", "-> " + xml_tag(kCannedDevice, "friendlyName"));
    chk(xml_tag(kCannedDevice, "modelName") == "RTANet-Mock-1", "modelName 解析", "");
    chk(xml_tag(kCannedDevice, "UDN") == "uuid:12345678-1234-1234-1234-123456789abc", "UDN 解析", "");

    // 3) 真实 HTTP 抓取（本机回环）
    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(0);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(srv, 1) != 0) {
        chk(false, "本地 HTTP 服务启动失败", "");
    } else {
        int alen = sizeof(a);
        ::getsockname(srv, (sockaddr*)&a, &alen);
        const uint16_t port = ntohs(a.sin_port);

        std::thread worker([&] {
            SOCKET c = ::accept(srv, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            char junk[2048]; ::recv(c, junk, sizeof(junk), 0);
            char resp[4096];
            const int n = std::snprintf(resp, sizeof(resp),
                                        "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                        std::strlen(kCannedDevice), kCannedDevice);
            ::send(c, resp, n, 0);
            ::closesocket(c);
        });

        std::string got;
        const bool ok = http_get("127.0.0.1", port, "/desc.xml", got);
        worker.join();
        ::closesocket(srv);

        const size_t body = got.find("\r\n\r\n");
        const std::string doc = (body == std::string::npos) ? got : got.substr(body + 4);
        chk(ok, "本机 HTTP 抓取成功", "");
        chk(xml_tag(doc, "friendlyName") == "RTANet SelfTest Renderer",
            "抓取到的描述能解析出设备名", "-> " + xml_tag(doc, "friendlyName"));
        // AVTransport 控制地址提取
        std::string ctrl;
        const size_t p = doc.find("AVTransport");
        if (p != std::string::npos) {
            const size_t c2 = doc.find("<controlURL>", p);
            if (c2 != std::string::npos) {
                const size_t e2 = doc.find("</controlURL>", c2);
                if (e2 != std::string::npos) ctrl = trim(doc.substr(c2 + 12, e2 - c2 - 12));
            }
        }
        chk(ctrl == "/upnp/control/AVTransport1", "AVTransport 控制地址提取", "-> " + ctrl);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "自检全部通过" : "自检存在失败", fails);
    return fails == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { printf("WSAStartup 失败\n"); return 1; }

    if (argc > 1 && std::strcmp(argv[1], "--selftest") == 0) {
        const int rc = selftest();
        WSACleanup();
        return rc;
    }

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { printf("UDP 套接字失败\n"); return 1; }
    int ttl = 2;
    ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    DWORD tv = 4000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // 依次探测几种 ST；不同设备只响应其中一部分
    const char* sts[] = {
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        "urn:schemas-upnp-org:service:AVTransport:1",
        "ssdp:all",
    };

    std::map<std::string, std::string> locations;   // LOCATION -> USN
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    ::inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);

    printf("UPnP/SSDP 设备发现（只读探测，不会输出任何声音）\n");
    printf("组播目标 239.255.255.250:1900\n\n");

    for (const char* st : sts) {
        char msearch[512];
        std::snprintf(msearch, sizeof(msearch),
                      "M-SEARCH * HTTP/1.1\r\n"
                      "HOST: 239.255.255.250:1900\r\n"
                      "MAN: \"ssdp:discover\"\r\n"
                      "MX: 2\r\n"
                      "ST: %s\r\n\r\n", st);
        ::sendto(s, msearch, (int)std::strlen(msearch), 0, (sockaddr*)&dst, sizeof(dst));
        printf("[ST] %s\n", st);

        char rbuf[4096];
        for (;;) {
            sockaddr_in from{}; int flen = sizeof(from);
            const int r = ::recvfrom(s, rbuf, sizeof(rbuf) - 1, 0, (sockaddr*)&from, &flen);
            if (r <= 0) break;
            rbuf[r] = 0;
            const std::string msg(rbuf, (size_t)r);
            const std::string loc = header_of(msg, "LOCATION");
            const std::string usn = header_of(msg, "USN");
            const std::string srv = header_of(msg, "SERVER");
            if (!loc.empty()) {
                char ip[64]{}; ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                if (!locations.count(loc)) {
                    locations[loc] = usn;
                    printf("  + 来自 %s\n    LOCATION %s\n    USN %s\n    SERVER %s\n",
                           ip, loc.c_str(), usn.c_str(), srv.c_str());
                }
            }
        }
        Sleep(250);
    }

    printf("\n共发现 %zu 个不同的设备描述地址\n", locations.size());
    if (locations.empty()) {
        printf("（未发现设备。可能原因：接收 DAC 不在同一子网 / 未开启 DLNA / 组播被防火墙或代理拦截）\n");
        ::closesocket(s); WSACleanup();
        return 2;
    }

    printf("\n=== 设备详情 ===\n");
    for (const auto& kv : locations) {
        const std::string& loc = kv.first;
        const size_t sp = loc.find("//");
        if (sp == std::string::npos) continue;
        const size_t slash = loc.find('/', sp + 2);
        std::string hostport = loc.substr(sp + 2, (slash == std::string::npos ? loc.size() : slash) - (sp + 2));
        const std::string path = (slash == std::string::npos) ? "/" : loc.substr(slash);
        const size_t colon = hostport.find(':');
        const std::string host = hostport.substr(0, colon);
        const uint16_t port = (colon == std::string::npos) ? 80 : (uint16_t)atoi(hostport.substr(colon + 1).c_str());

        std::string xml;
        if (!http_get(host, port, path, xml)) { printf("  [%s] 无法获取描述\n", host.c_str()); continue; }
        const size_t body = xml.find("\r\n\r\n");
        const std::string doc = (body == std::string::npos) ? xml : xml.substr(body + 4);

        std::string name = xml_tag(doc, "friendlyName");
        if (name.empty()) name = xml_tag(doc, "modelName");
        const std::string udn = xml_tag(doc, "UDN");
        const std::string model = xml_tag(doc, "modelName");

        // AVTransport 的控制地址
        std::string ctrl;
        size_t p = doc.find("AVTransport");
        if (p != std::string::npos) {
            const size_t c = doc.find("<controlURL>", p);
            if (c != std::string::npos) {
                const size_t e = doc.find("</controlURL>", c);
                if (e != std::string::npos) ctrl = trim(doc.substr(c + 12, e - c - 12));
            }
        }
        printf("  %s\n", name.empty() ? "(无名称)" : name.c_str());
        printf("    地址     %s:%u\n", host.c_str(), (unsigned)port);
        printf("    型号     %s\n", model.c_str());
        printf("    UDN      %s\n", udn.c_str());
        printf("    AVTransport 控制路径  %s\n", ctrl.empty() ? "(未找到)" : ctrl.c_str());
    }

    ::closesocket(s);
    WSACleanup();
    return 0;
}
