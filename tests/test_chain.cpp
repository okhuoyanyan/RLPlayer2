// test_chain.cpp — 完整 DSP 链路接入验证（全程离线）
// 判据：
//   全旁路必须仍逐位直通（回归）
//   每个阶段启用后必须【不再】是直通，且效果可被独立测量到
//   组合链路必须跑通
#include "engine/pipeline.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <string>

using namespace rta::engine;
using rta::audio::AudioBuffer;
namespace dsp = rta::dsp;
using dsp::kPi;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static AudioBuffer make_sine(uint32_t rate, int ch, int bits, size_t frames, double hz, double amp) {
    AudioBuffer a; a.rate = rate; a.channels = ch; a.bits = bits; a.isFloat = false;
    a.raw.resize(frames * (size_t)ch);
    const double fs = (double)(1u << (bits - 1));
    for (size_t n = 0; n < frames; ++n)
        for (int c = 0; c < ch; ++c) {
            const double v = amp * std::sin(2.0 * kPi * hz * (double)n / (double)rate) * ((c == 0) ? 1.0 : 0.5);
            double x = v * fs;
            x = (x >= 0) ? std::floor(x + 0.5) : std::ceil(x - 0.5);
            a.raw[n * (size_t)ch + (size_t)c] = (int32_t)x;
        }
    return a;
}

static double peak_after(const AudioBuffer& b, size_t skip) {
    double p = 0.0;
    for (size_t n = skip; n < b.frames(); ++n) p = std::max(p, std::fabs(b.sample(n, 0)));
    return p;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const uint32_t rate = 48000;
    const size_t N = 48000;

    printf("=== 1. 回归：全旁路仍逐位直通 ===\n");
    {
        auto a = make_sine(rate, 2, 24, 8000, 1000.0, 0.5);
        PipelineConfig cfg;
        Pipeline pl(cfg);
        PipelineStats st;
        auto o = pl.process(a, &st);
        bool same = st.bitPerfect && o.raw.size() == a.raw.size();
        if (same) for (size_t i = 0; i < a.raw.size(); ++i) if (a.raw[i] != o.raw[i]) { same = false; break; }
        check(same, "全旁路逐位相同");
    }

    printf("\n=== 2. EQ 阶段（+6 dB peak @1 kHz）===\n");
    {
        auto a = make_sine(rate, 2, 24, N, 1000.0, 0.25);
        const double base = peak_after(a, 1000);

        PipelineConfig cfg;
        cfg.channel.resize(2);
        dsp::Biquad q;
        dsp::biquad_design::peak(q, (double)rate, 1000.0, 1.0, 6.0);
        cfg.channel[0].eq.push_back(q);
        cfg.channel[1].eq.push_back(q);
        Pipeline pl(cfg);
        PipelineStats st;
        auto o = pl.process(a, &st);
        const double got = peak_after(o, 5000);
        const double gainDb = 20.0 * std::log10(got / base);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(输入峰 %.4f -> 输出峰 %.4f, 实测提升 %.3f dB, 设定 6.000 dB)",
                      base, got, gainDb);
        check(!st.bitPerfect && st.usedEq, "启用 EQ 后不再是直通", buf);
        check(std::fabs(gainDb - 6.0) < 0.05, "EQ 提升量等于设定值");
    }

    printf("\n=== 3. Matrix 阶段（L+R 混合到两声道）===\n");
    {
        auto a = make_sine(rate, 2, 24, 8000, 1000.0, 0.5);
        PipelineConfig cfg;
        cfg.matrix = { {0, 0, 1.0}, {1, 0, 1.0}, {0, 1, 1.0}, {1, 1, 1.0} };   // 双声道都变成 L+R
        Pipeline pl(cfg);
        PipelineStats st;
        auto o = pl.process(a, &st);
        // 输入 R 是 L 的一半，故 L+R = 1.5 * L
        const double l = o.sample(5000, 0), r = o.sample(5000, 1);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(L=%.6f R=%.6f, 两者应相等)", l, r);
        check(!st.bitPerfect && st.usedMatrix, "启用 Matrix 后不再是直通", buf);
        check(std::fabs(l - r) < 1e-12, "两输出声道内容一致");
    }

    printf("\n=== 4. 卷积阶段（IR = 单点 0.5）===\n");
    {
        auto a = make_sine(rate, 2, 24, N, 1000.0, 0.5);
        PipelineConfig cfg;
        cfg.convBlock = 1024;
        cfg.channel.resize(2);
        cfg.channel[0].ir = {0.5};
        cfg.channel[1].ir = {0.5};
        Pipeline pl(cfg);
        PipelineStats st;
        auto o = pl.process(a, &st);
        // 逐样本比：输出应为输入的一半。
        // 判据取"≤1 LSB"而不是逐位相等：IR=0.5 时 raw*0.5 恰好落在 .5 取整边界上，
        // FFT 往返的 1e-16 相对噪声会决定进位方向，导致约四分之一样本差 1 LSB。
        // 这是浮点卷积路径的固有属性，不是缺陷——用逐位相等去判会得到假失败。
        // ★ 流式卷积器有 convBlock 帧的算法延迟（Round 34），比对时必须计入
        const double lsb = 1.0 / 8388608.0;
        const size_t lat = cfg.convBlock;
        double maxDiff = 0.0; size_t over1lsb = 0; const size_t skip = 100;
        for (size_t n = skip + lat; n + 1 < N; ++n) {
            const double d = std::fabs(o.sample(n, 0) - a.sample(n - lat, 0) * 0.5);
            if (d > maxDiff) maxDiff = d;
            if (d > 1.5 * lsb) ++over1lsb;
        }
        char buf[220];
        std::snprintf(buf, sizeof(buf), "(最大偏差 %.3e = %.2f LSB, 超 1.5LSB 的样本 %zu / %zu)",
                      maxDiff, maxDiff / lsb, over1lsb, N - skip);
        check(!st.bitPerfect && st.usedConvolution, "启用卷积后不再是直通", buf);
        check(over1lsb == 0, "卷积结果等于 x0.5（误差 ≤1 LSB，含最后一个不完整块）");
    }

    printf("\n=== 4b. GPU 卷积（不可用时自动退回 CPU，必须仍然正确）===\n");
    {
        auto a = make_sine(rate, 2, 24, N, 1000.0, 0.5);
        PipelineConfig cfg;
        cfg.convBlock = 1024;
        cfg.channel.resize(2);
        cfg.channel[0].ir = {0.5};
        cfg.channel[1].ir = {0.5};
        cfg.channel[0].useGpu = true;
        cfg.channel[1].useGpu = true;
        Pipeline pl(cfg);
        PipelineStats st;
        auto o = pl.process(a, &st);
        const double lsb = 1.0 / 8388608.0;
        // GPU 走整块卷积（无 FIFO 延迟）；CPU 走流式卷积器（有 convBlock 延迟）。
        // 延迟必须按【实际走的路径】取，不能一刀切。
        const size_t lat2 = st.usedGpuConvolution ? 0 : cfg.convBlock;
        double maxDiff = 0.0; size_t over = 0;
        for (size_t n = 100 + lat2; n + 1 < N; ++n) {
            const double d = std::fabs(o.sample(n, 0) - a.sample(n - lat2, 0) * 0.5);
            if (d > maxDiff) maxDiff = d;
            if (d > 1.5 * lsb) ++over;
        }
        char buf[260];
        std::snprintf(buf, sizeof(buf), "(GPU: %s | 走了 GPU=%s | 最大偏差 %.2f LSB, 超限样本 %zu)",
                      st.gpuStatus.empty() ? "未尝试" : st.gpuStatus.c_str(),
                      st.usedGpuConvolution ? "是" : "否（已退回 CPU）",
                      maxDiff / lsb, over);
        check(st.usedConvolution, "卷积阶段生效（GPU 或 CPU 退回）", buf);
        check(over == 0, "GPU 路径结果与 CPU 一致（x0.5，误差 ≤1 LSB）");
    }

    printf("\n=== 5. 音量阶段（-6.0206 dB = x0.5）===\n");
    {
        auto a = make_sine(rate, 2, 24, N, 1000.0, 0.5);
        PipelineConfig cfg;
        cfg.volumeEnabled = true;
        cfg.volumeDb = 20.0 * std::log10(0.5);
        cfg.volumeRampMs = 0.0;
        Pipeline pl(cfg);
        PipelineStats st;
        auto o = pl.process(a, &st);
        size_t bad = 0;
        for (size_t n = 100; n < N; ++n) {
            const double want = std::floor(a.sample(n, 0) * 0.5 * 8388608.0 + 0.5) / 8388608.0;
            // pow(10, 20*log10(0.5)/20) 可能差 1 ulp；该误差在取整边界上会翻 1 LSB。
            if (std::fabs(o.sample(n, 0) - want) > 1.0 / 8388608.0 + 1e-12) ++bad;
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "(不匹配样本 %zu)", bad);
        check(!st.bitPerfect && st.usedVolume, "启用音量后不再是直通", buf);
        check(bad == 0, "音量结果逐样本等于 x0.5");
    }

    printf("\n=== 6. 组合链路（Matrix + EQ + 卷积 + 音量 + 升频）===\n");
    {
        auto a = make_sine(rate, 2, 24, N, 1000.0, 0.25);
        PipelineConfig cfg;
        cfg.outRate = 96000;
        cfg.outBits = 24;
        cfg.shaper = dsp::Shaper::None;
        cfg.taps = 32768;
        cfg.convBlock = 1024;
        cfg.matrix = { {0, 0, 1.0}, {1, 1, 1.0} };
        cfg.channel.resize(2);
        dsp::Biquad q;
        dsp::biquad_design::peak(q, (double)rate, 1000.0, 1.0, 3.0);
        cfg.channel[0].eq.push_back(q);
        cfg.channel[1].eq.push_back(q);
        cfg.channel[0].ir = {0.5, 0.25, 0.125};
        cfg.channel[1].ir = {0.5, 0.25, 0.125};
        cfg.volumeEnabled = true;
        cfg.volumeDb = -3.0;
        Pipeline pl(cfg);
        PipelineStats st;
        auto o = pl.process(a, &st);
        char buf[260];
        std::snprintf(buf, sizeof(buf),
                      "(阶段: matrix=%d eq=%d(%d 个) conv=%d(IR %d) resample=%d volume=%d; %zu -> %zu 帧 @%u Hz)",
                      st.usedMatrix, st.usedEq, st.eqBiquads, st.usedConvolution, st.maxIrLength,
                      st.usedResample, st.usedVolume, st.inFrames, st.outFrames, o.rate);
        check(st.usedMatrix && st.usedEq && st.usedConvolution && st.usedResample && st.usedVolume,
              "五个阶段全部生效", buf);
        check(st.outFrames == (size_t)(N * 2), "升频后帧数为 2 倍");
        const double p = peak_after(o, 20000);
        check(p > 0.0 && std::isfinite(p), "输出有效且非零");
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
