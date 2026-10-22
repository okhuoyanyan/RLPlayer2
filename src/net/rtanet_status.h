// rtanet_status.h — 端点在控制面上回传的状态行（闭环补偿的"回传"这一半）
//
// 控制面已有一条 TCP 双向通道（Round 12 建的握手通道），流开始后复用同一条连接做状态回传：
//
//   S->C  STATUS ts=123456 buf=198.4 drift=+12.7
//
//     ts    : 端点刚交付的最后一个样本的流内绝对样本号
//     buf   : 端点缓冲水位（毫秒）
//     drift : 端点估计的相对漂移（ppm，Round 14 的 DriftEstimator）
//
// 发送端读取这些行，喂给 ClockLoop（Round 19/20），得到速率修正。
// 用【行式文本】而不是二进制：与握手风格一致、可 telnet 观察、便于排障。
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <cmath>

namespace rta::net {

struct EndpointStatus {
    bool     valid = false;
    uint64_t streamTs = 0;
    double   bufferMs = -1.0;      // <0 表示端点未提供
    double   driftPpm = 0.0;
    bool     hasDrift = false;
};

// 端点侧：生成一行状态
inline std::string format_status(uint64_t ts, double bufferMs, double driftPpm, bool hasDrift) {
    char b[160];
    if (hasDrift)
        std::snprintf(b, sizeof(b), "STATUS ts=%llu buf=%.1f drift=%+.1f",
                      (unsigned long long)ts, bufferMs, driftPpm);
    else
        std::snprintf(b, sizeof(b), "STATUS ts=%llu buf=%.1f",
                      (unsigned long long)ts, bufferMs);
    return b;
}

// 发送端侧：解析一行状态
inline EndpointStatus parse_status(const std::string& line) {
    EndpointStatus s;
    if (line.rfind("STATUS", 0) != 0) return s;
    s.valid = true;

    auto pick = [&](const char* key, double& out) -> bool {
        const size_t p = line.find(std::string(key) + "=");
        if (p == std::string::npos) return false;
        out = std::strtod(line.c_str() + p + std::string(key).size() + 1, nullptr);
        return true;
    };

    double v = 0.0;
    if (pick("ts", v))    s.streamTs = (uint64_t)std::llround(v);
    if (pick("buf", v))   s.bufferMs = v;
    if (pick("drift", v)) { s.driftPpm = v; s.hasDrift = true; }
    return s;
}

} // namespace rta::net
