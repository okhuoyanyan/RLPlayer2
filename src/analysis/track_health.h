// track_health.h — 音频体检 B1b：编排层（动态 + 假无损两分析器的统一入口 + JSON 序列化）
//
// 用途：B1a 的两个纯算法分析器（dynamics.h / fake_lossless.h）在这里合成一个「曲目体检」
//       结果：analyze_buffer 吃已解码的 AudioBuffer（测试/已缓冲场景），analyze_file 走
//       read_any 解码（文件场景）。文件级入口保证不抛异常——解码失败/格式不识别都落进
//       status="error" + error 人话原因，调用方（主线 HTTP/后台 job）不需要 try/catch。
//
//       JSON 字段名与 docs/分析器API建议.md（主线接线契约）逐字一致，snake_case；
//       字符串按 JSON 规则转义（写法对齐 tools/rtaweb.cpp 的 jesc：手工拼接 + 转义，
//       不引入 JSON 生成库）。
//
// 已知边界（沿用 B1a，详见 docs/WIP-B1a.md §6）：
//   - 多声道 LUFS 权重只实现 L/R=1.0；素材 <3 s 时 DR/LRA 口径不成立（返回 0）；
//   - 大 DSD 经 read_any 是整曲解码进内存（播放侧已有流式路径，体检侧未接）。
#pragma once
#include "dynamics.h"
#include "fake_lossless.h"
#include "../audio/audio_loader.h"   // read_any（decode 入口，只读使用）
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace rta::analysis {

struct TrackHealth {
    // 动态（dynamics.h 原样透传）
    double drDb = 0.0;
    double lufs = 0.0;
    double lra = 0.0;
    double peakDbfs = 0.0;
    // 假无损（fake_lossless.h 原样透传）
    bool   losslessSuspected = false;
    double cutoffHz = 0.0;
    double confidence = 0.0;
    std::string reason;          // 中文人话，UI 直接显示
    bool   fake24bit = false;
    // 元信息
    double durationSec = 0.0;
    uint32_t fs = 0;
    int channels = 0;
    int bits = 0;
    std::string status;          // "ready" / "error"
    std::string error;           // status=error 时的人话原因
    int64_t analyzedAtMs = 0;    // Unix epoch 毫秒；缓存命中语义见 docs/分析器API建议.md
};

namespace detail {

inline int64_t now_ms() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// JSON 字符串转义（对齐 rtaweb.cpp 的 jesc；补全 \b \f \r 三种短转义）。
// ≥0x20 的字节原样直通 —— reason/路径里的中文按 UTF-8 字节保持不变，合法 JSON。
inline std::string json_escape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

inline std::string jnum(double v, const char* fmt) {
    char b[64];
    std::snprintf(b, sizeof(b), fmt, v);
    return b;
}

} // namespace detail

// 核心入口（解码无关）：对已在内存里的 AudioBuffer 做体检。不抛异常。
inline TrackHealth analyze_buffer(const rta::audio::AudioBuffer& buf) {
    TrackHealth h;
    h.fs = buf.rate;
    h.channels = buf.channels;
    // float 缓冲按 32 位语义处理（与 audio_loader 的位深探测口径一致）：float 无整数 LSB
    // 可言，传 32 只是让 fake24 检查有确定的整数域尺度，不会误触发（实测低 8 位近似均匀）。
    h.bits = buf.isFloat ? 32 : buf.bits;
    h.analyzedAtMs = detail::now_ms();

    const size_t n = buf.frames();
    const int chn = buf.channels;
    if (n == 0 || chn <= 0 || buf.rate == 0) {
        h.status = "error";
        h.error = "解码后无可用样本（帧数、声道数或采样率为 0）";
        return h;
    }
    h.durationSec = (double)n / (double)buf.rate;

    // 交错 int32/float → 平面 double（两个分析器的入参形态）。
    // 10 s @48k 立体声约 7.7 MB；体检是一次性后台动作，这个量级可接受。
    std::vector<std::vector<double>> planes((size_t)chn);
    std::vector<const double*> ptrs((size_t)chn);
    for (int c = 0; c < chn; ++c) {
        planes[(size_t)c].resize(n);
        for (size_t i = 0; i < n; ++i) planes[(size_t)c][i] = buf.sample(i, c);
        ptrs[(size_t)c] = planes[(size_t)c].data();
    }

    const DynamicsResult d = measure_dynamics(ptrs.data(), chn, n, buf.rate);
    const LosslessCheck lc = check_lossless(ptrs.data(), chn, n, buf.rate, h.bits);
    h.drDb = d.drDb;
    h.lufs = d.lufsIntegrated;
    h.lra = d.lra;
    h.peakDbfs = d.peakDbfs;
    h.losslessSuspected = lc.suspected;
    h.cutoffHz = lc.cutoffHz;
    h.confidence = lc.confidence;
    h.reason = lc.reason;
    h.fake24bit = lc.fake24bit;
    // 素材过短时 dynamics.valid=false，但体检本身完成了（DR/LRA=0 表示"口径不成立"），
    // 仍算 ready；error 只留给「根本没拿到样本」的失败。
    h.status = "ready";
    return h;
}

// 文件级入口：read_any 解码 + analyze_buffer。绝不向调用方抛异常。
inline TrackHealth analyze_file(const std::string& path) {
    TrackHealth h;
    try {
        rta::audio::LoadNote note;
        rta::audio::AudioBuffer buf = rta::audio::read_any(path, &note);
        return analyze_buffer(buf);
    } catch (const std::exception& e) {
        h.status = "error";
        h.error = e.what();
    } catch (...) {
        h.status = "error";
        h.error = "未知异常（非 std::exception）";
    }
    h.analyzedAtMs = detail::now_ms();
    return h;
}

// JSON 序列化。字段名与 docs/分析器API建议.md 逐字一致（主线按它接线，勿改名）。
inline std::string track_health_json(const TrackHealth& h) {
    std::ostringstream o;
    o << "{\"dr_db\":"      << detail::jnum(h.drDb, "%.3f")
      << ",\"lufs\":"       << detail::jnum(h.lufs, "%.3f")
      << ",\"lra\":"        << detail::jnum(h.lra, "%.3f")
      << ",\"peak_dbfs\":"  << detail::jnum(h.peakDbfs, "%.3f")
      << ",\"lossless_suspected\":" << (h.losslessSuspected ? "true" : "false")
      << ",\"cutoff_hz\":"  << detail::jnum(h.cutoffHz, "%.1f")
      << ",\"confidence\":" << detail::jnum(h.confidence, "%.3f")
      << ",\"reason\":\""   << detail::json_escape(h.reason) << "\""
      << ",\"fake24\":"     << (h.fake24bit ? "true" : "false")
      << ",\"duration_sec\":" << detail::jnum(h.durationSec, "%.3f")
      << ",\"fs\":"         << (uint64_t)h.fs
      << ",\"channels\":"   << h.channels
      << ",\"bits\":"       << h.bits
      << ",\"status\":\""   << detail::json_escape(h.status) << "\""
      << ",\"error\":\""    << detail::json_escape(h.error) << "\""
      << ",\"analyzed_at_ms\":" << (long long)h.analyzedAtMs
      << "}";
    return o.str();
}

} // namespace rta::analysis
