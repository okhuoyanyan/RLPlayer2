// test_fullscale.cpp — 满幅 PCM 经过真实管线后的稳定性
//
// 2026-10-21 现场故障：24-bit 输出 + 默认 shaper=auto（=LNS15）+ rate=max，
// 播到第一个 0 dBFS 峰值后突然变成噪音。根因在 dither.h：旧实现只钳位量化
// 【输出】，过载误差经 15 阶反馈放大 90 dB 后锁死在 ±1。
//
// 这个测试走完整的 pipeline（解码缓冲 -> 重采样 -> LNS15 -> 24-bit），
// 不是只测 Quantizer。判据：
//   1) 稳态最后 0.5 秒的 RMS 必须等于输入正弦的 RMS（0.7071）；
//      锁死/噪声的旧实现会明显偏高（实测 0.93~1.0）。
//   2) 不允许出现长串满幅样本（方波也能"不发散"）。
//   3) 输出绝不越界。
#include "engine/pipeline.h"
#include "dsp/dither.h"
#include "dsp/oversample_peak.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

using namespace rta;
using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static const double kPi2 = 3.14159265358979323846;

struct Result { double lastRms = 0.0, peak = 0.0; long long maxRun = 0; };

static Result run_case(uint32_t outRate, double preAttenDb) {
    const uint32_t inRate = 44100;
    const size_t frames = (size_t)inRate * 4;          // 4 秒足够跨过第一个大峰值
    audio::AudioBuffer in;
    in.rate = inRate; in.channels = 2; in.bits = 16; in.isFloat = false;
    in.raw.resize(frames * 2);
    for (size_t n = 0; n < frames; ++n) {
        const double x = 0.999999 * std::sin(2.0 * kPi2 * 997.0 * (double)n / (double)inRate);
        const int32_t v = (int32_t)std::llround(x * 32767.0);
        in.raw[n * 2] = v; in.raw[n * 2 + 1] = v;
    }

    engine::PipelineConfig cfg;
    cfg.outRate = outRate;
    cfg.outBits = 24;
    cfg.shaper = Shaper::LNS15;
    cfg.volumeEnabled = true;
    cfg.volumeDb = 0.0;
    cfg.taps = 4096;
    cfg.transition = 0.1;
    cfg.attenDb = 100.0;
    cfg.twoStage = true;
    cfg.gpuResample = false;                           // 测试不依赖 GPU
    cfg.preAttenDb = preAttenDb;
    engine::Pipeline pl(cfg);

    std::vector<int32_t> out;
    size_t pos = 0;
    while (pos < in.frames()) {
        const size_t n = std::min<size_t>(4096, in.frames() - pos);
        audio::AudioBuffer c;
        c.rate = in.rate; c.channels = in.channels; c.bits = in.bits;
        c.raw.assign(in.raw.begin() + (long long)(pos * 2),
                     in.raw.begin() + (long long)((pos + n) * 2));
        pos += n;
        audio::AudioBuffer o = pl.process(c, nullptr);
        out.insert(out.end(), o.raw.begin(), o.raw.end());
    }

    const double fs = (double)(1u << 23);               // 24-bit 满幅
    const size_t nF = out.size() / 2;
    Result r;
    long long run = 0;
    double sum = 0.0;
    size_t cnt = 0;
    for (size_t n = 0; n < nF; ++n) {
        const double v = (double)out[n * 2] / fs;
        const double a = std::fabs(v);
        r.peak = std::max(r.peak, a);
        if (a >= 1.0 - 2.0 / fs) { ++run; r.maxRun = std::max(r.maxRun, run); } else run = 0;
        if (n > nF - (size_t)(outRate / 2)) { sum += v * v; ++cnt; }
    }
    r.lastRms = std::sqrt(sum / (double)cnt);
    return r;
}

int main() {
    printf("=== 满幅 PCM -> 重采样 -> LNS15(24-bit) 稳定性 ===\n");
    // 不给 ISP 预衰减是"最坏情况"：播放器在 isp=0 或 ISP 估计不足时就是这条路。
    // 量化器本身必须扛得住，而不是靠前级碰巧把电平压下去。
    for (uint32_t rate : {192000u, 384000u}) {
        const Result r = run_case(rate, 0.0);
        const std::string tag = std::to_string(rate) + " Hz / -0.0 dBFS";
        printf("  %-22s lastRms=%.4f (want 0.7071) peak=%.4f maxRun=%lld\n",
               tag.c_str(), r.lastRms, r.peak, r.maxRun);
        check(std::fabs(r.lastRms - 0.7071) < 0.03, "稳态 RMS 正确 @" + tag,
              std::to_string(r.lastRms));
        check(r.maxRun < 64, "没有长串满幅样本 @" + tag, std::to_string(r.maxRun));
        check(r.peak <= 1.0 + 1e-9, "输出不越界 @" + tag);
    }

    // 对照：ISP 正常工作时也要保持稳定（不应因为修复而回归）
    {
        const Result r = run_case(384000u, -0.2);
        printf("  %-22s lastRms=%.4f peak=%.4f maxRun=%lld\n",
               "384000 Hz / ISP -0.2 dB", r.lastRms, r.peak, r.maxRun);
        check(std::fabs(r.lastRms - 0.7071) < 0.04, "ISP 预衰减下依然稳定");
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
