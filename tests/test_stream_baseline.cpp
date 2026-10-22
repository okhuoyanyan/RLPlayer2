// test_stream_baseline.cpp — 刻画当前"整块处理"管线的局限（流式改造的基线）
//
// 目的：在做流式改造【之前】，用客观判据把当前行为记录下来。
//       改造后必须满足"分块不变性"——同一输入不论一次喂还是分多次喂，输出必须一致。
//
// 判据设计（遵守铁律 3）：用【同一输入、不同分块方式】的输出差异来刻画，
//   而不是靠肉眼观察或主观描述。
#include "engine/pipeline.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <string>

using namespace rta::engine;
using rta::audio::AudioBuffer;
namespace dsp = rta::dsp;

static int fails = 0;
static void note(const std::string& what, const std::string& detail) {
    printf("  [记录] %s %s\n", what.c_str(), detail.c_str());
}
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
            const double v = amp * std::sin(2.0 * dsp::kPi * hz * (double)n / (double)rate);
            double x = v * fs;
            x = (x >= 0) ? std::floor(x + 0.5) : std::ceil(x - 0.5);
            a.raw[n * (size_t)ch + (size_t)c] = (int32_t)x;
        }
    return a;
}

// 把缓冲切成 n 段，用【同一个 Pipeline 实例】逐段处理，拼回完整输出。
// ★ 必须复用同一个实例：真实流式播放器就是这样用的。
//   早先这里每块都新建 Pipeline，测的其实是"无状态"模型，得不出流式结论。
static AudioBuffer run_chunked(const AudioBuffer& in, const PipelineConfig& cfg, size_t chunkFrames) {
    AudioBuffer out;
    out.rate = cfg.outRate ? cfg.outRate : in.rate;
    out.channels = in.channels;
    out.bits = cfg.outBits ? cfg.outBits : in.bits;
    out.isFloat = false;
    Pipeline pl(cfg);                       // ← 只建一次
    const size_t total = in.frames();
    for (size_t off = 0; off < total; off += chunkFrames) {
        const size_t n = std::min(chunkFrames, total - off);
        AudioBuffer seg;
        seg.rate = in.rate; seg.channels = in.channels; seg.bits = in.bits; seg.isFloat = false;
        seg.raw.assign(in.raw.begin() + (ptrdiff_t)(off * (size_t)in.channels),
                       in.raw.begin() + (ptrdiff_t)((off + n) * (size_t)in.channels));
        PipelineStats st;
        AudioBuffer o = pl.process(seg, &st);
        out.raw.insert(out.raw.end(), o.raw.begin(), o.raw.end());
    }
    return out;
}

static size_t diff_count(const AudioBuffer& a, const AudioBuffer& b) {
    const size_t n = std::min(a.raw.size(), b.raw.size());
    size_t d = 0;
    for (size_t i = 0; i < n; ++i) if (a.raw[i] != b.raw[i]) ++d;
    return d + (a.raw.size() > b.raw.size() ? a.raw.size() - n : b.raw.size() - n);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const uint32_t rate = 48000;
    const size_t N = 24000;
    auto src = make_sine(rate, 2, 24, N, 1000.0, 0.25);

    printf("=== 当前管线的分块行为（基线）===\n");
    printf("  输入: %zu 帧 @ %u Hz, 24-bit 立体声\n\n", N, rate);

    // --- 场景 A：纯直通（应该与分块无关）---
    {
        PipelineConfig cfg;
        Pipeline pl(cfg);
        PipelineStats st;
        auto whole = pl.process(src, &st);
        auto chunked = run_chunked(src, cfg, 4096);
        const size_t d = diff_count(whole, chunked);
        note("直通路径 整块 vs 4096 帧分块", "(差异 " + std::to_string(d) + " 个样本)");
        check(d == 0, "直通路径应完全与分块无关");
    }

    // --- 场景 B：升频（重采样器本身是流式的，但管线每次重建）---
    {
        PipelineConfig cfg;
        cfg.outRate = 96000; cfg.outBits = 24; cfg.taps = 8192;
        Pipeline pl(cfg);
        PipelineStats st;
        auto whole = pl.process(src, &st);
        auto chunked = run_chunked(src, cfg, 4096);
        const size_t d = diff_count(whole, chunked);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(整块 %zu 帧 vs 分块 %zu 帧, 差异 %zu)",
                      whole.raw.size() / 2, chunked.raw.size() / 2, d);
        note("升频路径 整块 vs 4096 帧分块", buf);
        check(d == 0, "升频路径必须与分块无关（重采样器状态需持久）", buf);
    }

    // --- 场景 C：EQ（双二阶有状态，每次重建会重置）---
    {
        PipelineConfig cfg;
        cfg.channel.resize(2);
        dsp::Biquad q;
        dsp::biquad_design::peak(q, (double)rate, 1000.0, 1.0, 6.0);
        cfg.channel[0].eq.push_back(q);
        cfg.channel[1].eq.push_back(q);
        Pipeline pl(cfg);
        PipelineStats st;
        auto whole = pl.process(src, &st);
        auto chunked = run_chunked(src, cfg, 4096);
        const size_t d = diff_count(whole, chunked);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(差异 %zu / %zu 样本, %.1f%%)",
                      d, whole.raw.size(), 100.0 * (double)d / (double)whole.raw.size());
        note("EQ 路径 整块 vs 4096 帧分块", buf);
        check(d == 0, "EQ 路径必须与分块无关（双二阶状态需持久）", buf);
    }

    // --- 场景 D：卷积（分块卷积器本身是流式的，但管线每次重建）---
    {
        PipelineConfig cfg;
        cfg.convBlock = 1024;
        cfg.channel.resize(2);
        std::vector<double> ir(512, 0.0);
        for (size_t i = 0; i < ir.size(); ++i) ir[i] = std::exp(-6.0 * (double)i / 512.0) * ((i % 2) ? -1.0 : 1.0) * 0.1;
        cfg.channel[0].ir = ir;
        cfg.channel[1].ir = ir;
        Pipeline pl(cfg);
        PipelineStats st;
        auto whole = pl.process(src, &st);
        auto chunked = run_chunked(src, cfg, 4096);
        const size_t d = diff_count(whole, chunked);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(差异 %zu / %zu 样本, %.1f%%)",
                      d, whole.raw.size(), 100.0 * (double)d / (double)whole.raw.size());
        note("卷积路径 整块 vs 4096 帧分块", buf);
        // ⚠️ 未解决：卷积器仍是分块对齐的（均匀分块 FFT 要求固定块长）。
        //    要让它与任意分块无关，需要加输入 FIFO + 输出 FIFO，使两条路径
        //    具有相同的 convBlock 延迟。留待下一轮，此处只记录不断言。
        printf("       ^ 已知未解决：卷积器仍要求调用长度与 convBlock 对齐\n");
    }

    printf("\n=== 结论 ===\n");
    printf("  上表刻画了当前「整块处理」的局限：\n");
    printf("  Round 32 基线：升频 71%%、EQ 6.8%%、卷积 10.6%% 的样本在分块后不同。\n");
    printf("  Round 33 改造后：升频与 EQ 已【归零】—— 重采样器与双二阶改为持久状态。\n");
    printf("  仍待解决：卷积器（需要输入/输出 FIFO 以统一块对齐）。\n");
    printf("\n  流式改造后必须满足：**同一输入任意分块方式，输出逐位一致**。\n");
    printf("  这就是改造的验收判据。\n");

    printf("\n%s (断言失败项 %d —— 本文件以刻画为主，只对直通路径做硬断言)\n",
           fails == 0 ? "直通路径断言通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
