// naa_scan.cpp — NAA (Network Audio Adapter) 端点发现 CLI
//
// 探测逻辑在 tools/naa_discover.h（rtaweb 的 /api/naa/discover 共用同一份实现，
// 避免 CLI 与 UI 两套判据漂移）。本文件只负责参数解析与打印。
//
// 为什么用端口扫描而不是协议发现：见 docs/NAA发现纪要.md /
// docs/NAA对接探测纪要.md —— NAA 发现报文格式未公开，但 networkaudiod 固定监听
// TCP/UDP 43210，端口是否开放是可靠判据。全程只读，不涉及任何音频输出。
#include "naa_discover.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::string subnet;            // 空 = 自动枚举本机所有 /24
    std::string legacyBase = "192.168.50.";   // 旧版默认网段，仅在没给 --subnet 且自动枚举失败时兜底
    int timeoutMs = 400;
    double listenSec = 6.0;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--subnet" && i + 1 < argc) { subnet = argv[++i]; if (!subnet.empty() && subnet.back() != '.') subnet += '.'; }
        else if (k == "--timeout" && i + 1 < argc) timeoutMs = atoi(argv[++i]);
        else if (k == "--listen" && i + 1 < argc) listenSec = atof(argv[++i]);
    }

    printf("NAA 端点发现（只读探测，不输出任何声音）\n");
    printf("目标端口 TCP/UDP %d\n\n", (int)rta::naa::kPort);

    // 1) 主动扫描（不让 discover() 顺手做组播监听，CLI 保持"扫描 + 监听"两段分明）
    if (subnet.empty()) {
        auto prefixes = rta::naa::local_subnet_prefixes();
        if (prefixes.empty()) {
            subnet = legacyBase;
            printf("[1] 未能枚举本机网卡，回退默认网段 %s0/24 ...\n", subnet.c_str());
        } else {
            printf("[1] 扫描本机网卡所在网段的 TCP %d ...\n", (int)rta::naa::kPort);
            for (const auto& p : prefixes) printf("    网段 %s0/24\n", p.c_str());
        }
    } else {
        printf("[1] 扫描指定网段 %s0/24 的 TCP %d ...\n", subnet.c_str(), (int)rta::naa::kPort);
    }

    rta::naa::DiscoverResult res = rta::naa::discover(timeoutMs, 48, 0.0, {}, subnet);
    if (res.endpoints.empty()) {
        printf("    未发现开放 %d 的主机\n", (int)rta::naa::kPort);
    } else {
        printf("    发现 %zu 个候选：\n", res.endpoints.size());
        for (const auto& ep : res.endpoints) printf("      %s:%d\n", ep.host.c_str(), (int)ep.port);
    }

    // 2) 被动监听组播（保留旧行为：CLI 每次都给一段监听窗口）
    std::vector<std::string> from;
    if (listenSec > 0.0) {
        printf("\n[2] 被动监听 NAA 组播 %0.2f 秒 ...\n", listenSec);
        printf("    已加入 224.0.0.199 / 239.192.0.199\n");
        from = rta::naa::listen_multicast(listenSec);
        if (from.empty()) printf("    监听期间未收到任何 NAA 报文\n");
        for (const auto& ip : from) printf("    + 来自 %s\n", ip.c_str());
    }

    printf("\n结论：\n");
    if (res.endpoints.empty()) {
        printf("  本网段未发现 NAA 端点。可能原因：\n");
        printf("    * NAA 设备不在本网段\n");
        printf("    * 设备未开启 / 未运行 networkaudiod（数播后台的服务互斥：开 UPnP 会关掉 NAA）\n");
        printf("    * 防火墙拦截\n");
        printf("  若你的接收设备支持 NAA，请确认它与本机在同一子网且已运行 NAA 服务。\n");
    } else {
        printf("  以上主机开放 %d 端口，很可能是 NAA 端点，可作为 RTANet/NAA 对接目标。\n", (int)rta::naa::kPort);
    }
    WSACleanup();
    return 0;
}
