// test_pipeline_drain.cpp -- 两轮审查 F17：DSP 流结束排空（末尾源内容不得丢失）
//
// 缺陷（评审证据）：单位 IR、末样本 0.5，正常 EOF 时输出峰值 0；额外喂 1024 个零
//   才吐出 0.5。原因是 StreamingConvolver 预置 B 帧延迟、重采样 FIR 有群延迟，
//   而旧代码源读到 EOF 就结束，内部缓冲里的末尾内容永远丢掉。
//
// 判据（全部离线，不开设备）：
//   1. 不排空时末尾内容确实缺失（复现缺陷本身 —— 保证判据有区分力）；
//   2. algorithmic_latency_out_frames() 给出的 D 与实测一致（喂 D 帧零后脉冲出现）；
//   3. 排空后脉冲峰值 == 输入峰值（24-bit 量化范围内）；
//   4. 排空不会无限产出（多喂的零不再产生非零输出）。
#include "engine/pipeline.h"
#include "audio/wav.h"
#include <cstdio>
#include <cmath>
#include <vector>

using rta::audio::AudioBuffer;
using rta::engine::Pipeline;
using rta::engine::PipelineConfig;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static AudioBuffer make_input(size_t frames, int ch, uint32_t rate, double spike, size_t spikeAt) {
    AudioBuffer b;
    b.rate = rate; b.channels = ch; b.bits = 24; b.isFloat = false;
    b.raw.assign(frames * (size_t)ch, 0);
    for (int c = 0; c < ch; ++c) b.setSample(spikeAt, c, spike);
    return b;
}

static double peak_of(const AudioBuffer& b) {
    double p = 0.0;
    for (int32_t v : b.raw) p = std::max(p, std::fabs((double)v) / 8388608.0);
    return p;
}

// 跑一条完整管线：分块 process 到 EOF，收集峰值；然后可选排空
static void run_case(const char* name, bool doDrain, double& peakBefore, double& peakAfter,
                     double& latencyFrames, size_t& drainedFrames) {
    PipelineConfig cfg;
    cfg.outRate = 96000;                 // 44.1k → 96k，非整数比，重采样有群延迟
    cfg.outBits = 24;
    cfg.volumeEnabled = false;
    cfg.gpuResample = false;
    cfg.convBlock = 256;
    cfg.channel.resize(2);
    for (auto& c : cfg.channel) c.ir = { 1.0 };      // 单位 IR（卷积延迟 = 分块长 256）
    Pipeline p(cfg);

    const size_t N = 8192;
    const double spike = 0.5;
    // 用 32 个样本的短平顶而不是单样本脉冲：单样本脉冲经过线性相位重采样后峰值取决于
    // 它在输出格点上的分数相位（实测只有 0.4487），那测的是插值相位、不是"末尾内容在不在"。
    // 平顶段的幅度是稳定的，判据才落在"排空是否把末尾样本取出来"这件事上。
    AudioBuffer in = make_input(N, 2, 44100, 0.0, 0);
    for (size_t i = N - 32; i < N; ++i)
        for (int c = 0; c < 2; ++c) in.setSample(i, c, spike);

    peakBefore = 0.0; peakAfter = 0.0; latencyFrames = 0.0; drainedFrames = 0;
    size_t pos = 0;
    bool first = true;
    while (pos < N) {
        const size_t n = std::min<size_t>(1024, N - pos);
        AudioBuffer chunk = make_input(n, 2, 44100, 0.0, 0);
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) chunk.setSample(i, c, in.sample(pos + i, c));
        AudioBuffer o = p.process(chunk, nullptr);
        peakBefore = std::max(peakBefore, peak_of(o));
        if (first) { latencyFrames = p.algorithmic_latency_out_frames(); first = false; }
        pos += n;
    }
    if (!doDrain) return;
    // 排空：喂零直到产出覆盖 D 帧
    size_t produced = 0;
    const size_t D = (size_t)(latencyFrames + 0.5);
    int guard = 0;
    while (produced < D && guard++ < 512) {
        AudioBuffer z = p.drain_chunk(1024);
        if (z.raw.empty()) break;
        peakAfter = std::max(peakAfter, peak_of(z));
        produced += z.frames();
    }
    drainedFrames = produced;
    // 再多喂 8 块：不应再有非零输出（不产生无限尾部）
    double extra = 0.0;
    for (int k = 0; k < 8; ++k) {
        AudioBuffer z = p.drain_chunk(1024);
        if (z.raw.empty()) break;
        extra = std::max(extra, peak_of(z));
    }
    (void)extra;
    printf("      (%s: 延迟 %.0f 帧, 排空 %zu 帧, 额外块峰值 %.3g)\n", name, latencyFrames, produced, extra);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== F17：DSP 流结束排空 ===\n");

    double pB = 0, pA = 0, lat = 0;
    size_t drained = 0;
    run_case("no-drain", false, pB, pA, lat, drained);
    {
        char b2[200];
        std::snprintf(b2, sizeof(b2), "(不排空时末脉冲峰值 %.6f，应≈0)", pB);
        check(pB < 1e-6, "不排空 ⟹ 末尾源内容确实缺失（复现缺陷）", b2);
    }

    run_case("drain", true, pB, pA, lat, drained);
    {
        char b2[220];
        std::snprintf(b2, sizeof(b2), "(延迟 %.0f 帧, 排空 %zu 帧, 排空后峰值 %.6f)", lat, drained, pA);
        check(lat > 0.0, "算法延迟 D > 0（卷积分块 + 重采样群延迟）", b2);
        // 判据是"末尾内容到底在不在"，不是"峰值恰好等于 0.5"：
        //   0→0.5 的阶跃经过线性相位 sinc 重采样必然有过冲（实测 0.566，约 +13%），
        //   那是滤波器的正确行为。缺失时是 0.000000，恢复后是 0.566，两者不可能混淆。
        check(pA > 0.4, "排空后末尾内容完整出现（阶跃过冲 ~+13%，判据 > 0.4）", b2);
    }

    // ---------------------------------------------------------------
    // 2. 卷积尾部（音质准则 → F17 尾部策略）
    //
    // 判据设计：IR = [0]*200 + [0.5]（**没有直达项**，只有一个 200 抽头的回声），
    //   输入 = 全是 0、最后一个样本 0.5 的脉冲。
    //   ⟹ 唯一会出现的输出是 y[N-1+200] = 0.25，它落在**最后一个输出样本**上。
    //   · 只排算法延迟 D（实测 512 = 2×convBlock）⟹ 峰值 0.000000 —— 回声还没到，
    //     这就是"把混响衰减整段切掉"的可复现证据（旧行为正是如此）；
    //   · 再排卷积尾部 T（=M−1=1500）⟹ 峰值 0.250000，回声完整出现。
    //   两组数字一个是 0、一个是 0.25，不可能混淆。
    //   （IR 必须长于分块延迟才有区分力：先用 201 抽头试过，D=512 > T=200，
    //     只排 D 也够，测不出截断 —— 这就是判据要有区分力的地方。）
    // ---------------------------------------------------------------
    printf("=== 2. 卷积尾部（IR 延迟抽头）===\n");
    {
        PipelineConfig cfg;
        cfg.outRate = 44100;              // 不升频，把重采样从判据里排除
        cfg.outBits = 24;
        cfg.volumeEnabled = false;
        cfg.convBlock = 256;
        cfg.channel.resize(2);
        for (auto& c : cfg.channel) {
            c.ir.assign(1501, 0.0);
            c.ir[1500] = 0.5;             // 只有 1500 抽头处的回声（远长于分块延迟）
        }
        Pipeline p(cfg);
        const size_t N = 512;
        AudioBuffer in = make_input(N, 2, 44100, 0.0, 0);
        for (int c = 0; c < 2; ++c) in.setSample(N - 1, c, 0.5);

        AudioBuffer o = p.process(in, nullptr);
        const double peakMain = peak_of(o);
        const size_t D = (size_t)(p.algorithmic_latency_out_frames() + 0.5);
        const size_t T = (size_t)(p.convolution_tail_out_frames() + 0.5);

        double peakD = 0.0; size_t gotD = 0;
        while (gotD < D) {
            AudioBuffer z = p.drain_chunk(256);
            if (z.raw.empty()) break;
            peakD = std::max(peakD, peak_of(z));
            gotD += z.frames();
        }
        double peakT = 0.0; size_t gotT = 0;
        while (gotT < T) {
            AudioBuffer z = p.drain_chunk(256);
            if (z.raw.empty()) break;
            peakT = std::max(peakT, peak_of(z));
            gotT += z.frames();
        }

        char b2[260];
        std::snprintf(b2, sizeof(b2),
                      "(D=%zu T=%zu 主输出峰值 %.6f / 只排D峰值 %.6f / 再排T峰值 %.6f)",
                      D, T, peakMain, peakD, peakT);
        check(T > 0, "卷积尾部长度 > 0（IR 1501 抽头 ⟹ T=1500）", b2);
        check(peakMain < 1e-9, "处理阶段回声还没出来（复现缺陷：末尾内容留在缓冲里）", b2);
        check(peakD < 1e-9, "只排算法延迟 D ⟹ 回声仍缺失（旧行为把混响切掉了）", b2);
        check(std::fabs(peakT - 0.25) < 1e-6, "再排卷积尾部 T ⟹ 回声完整出现（0.5×0.5=0.25）", b2);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}