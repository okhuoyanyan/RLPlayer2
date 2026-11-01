// pipeline.h — 完整 DSP 播放管线
//
// 信号链顺序（对齐 HQPlayer 手册 4~7 章的架构）：
//   1) 解码缓冲（源采样率，按声道解交错为 double 平面）
//   2) Matrix 路由/混合      —— 增益可为 dB 或线性；负增益 = 反相（M/S 处理）
//   3) 每输出声道处理链       —— IIR EQ（双二阶级联）+ 延迟
//   4) 卷积                   —— 均匀分块 FFT，零延迟
//   5) 重采样到目标采样率      —— 1x / Nx 可选不同滤波器
//   6) 音量                   —— 在目标采样率上，带渐变
//   7) 抖动/噪声整形 → 输出位深
//
// 路径判定：
//   * 逐位直通：以上【全部】旁路、位深不变、整数格式 ⟹ 原始整数样本原样搬运
//   * 无损展宽：位深增加、其余全部旁路      ⟹ 左移补齐，数值不变
//   * 完整 DSP：任一处理启用
#pragma once
#include "audio/wav.h"
#include "dsp/resampler.h"
#include "dsp/pcm_pack.h"
#include "dsp/dither.h"
#include "dsp/convolution.h"
#include "dsp/matrix.h"
#include "dsp/volume.h"
#include "dsp/gpu_bridge.h"
#include "dsp/loudness.h"
#include "dsp/peq.h"          // 261024-02 #6：20 段参数均衡（输入采样率、现有文件 EQ 之前）
#include "dsp/dsd_rate_convert.h"   // 2026-10-27：DSD 异速率支路（DSD64→128/256…），PCM 路径不引用它
#include "dsp/replaygain.h"         // 2026-10-27：ReplayGain 增益（只用 apply_gain / loudness_volume_db，纯头文件）
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <memory>

namespace rta::engine {

namespace dsp = rta::dsp;
using rta::audio::AudioBuffer;

// 单个输出声道的处理链
struct ChannelProcess {
    std::vector<dsp::Biquad> eq;      // 级联双二阶（对应手册 "iir" 插件）
    int    delaySamples = 0;          // 延迟（对应手册 "delay" 插件）
    std::vector<double> ir;           // 卷积 IR（对应 WAV 卷积 / 卷积引擎）
    bool   useGpu = false;            // true 时优先走 GPU 卷积（不可用则自动退回 CPU）
    double gainDb = 0.0;              // 本声道增益（左右平衡 / 多声道校正）；0 = 不动
};

struct PipelineConfig {
    // ---- 路径控制 ----
    bool     forceBitPerfect = false;

    // ---- 重采样 ----
    uint32_t outRate    = 0;          // 0 = 与源相同
    int      taps       = 262144;
    bool     gpuResample = true;    // 用 GPU 做多相升频（rtagpu.dll 缺失时自动退回 CPU，输出逐位相同）
    bool     twoStage    = true;    // 大比率升频拆两级（同参数下 CPU 运算量约 1/4，可听带内一致）
    int      stage2Taps  = 64;      // 两段式第二级抽头数
    double   attenDb    = 180.0;
    double   transition = 0.04;
    dsp::WindowKind filterWindow = dsp::WindowKind::Kaiser;   // 命名滤波器（--filter）
    dsp::PhaseKind  filterPhase  = dsp::PhaseKind::Linear;
    // ★ 2026-10-29（两轮审查 F22）：切趾（Apodizing）三要素。
    //   以前 ResamplerConfig 有这三个字段、FilterPreset 也有，但 PipelineConfig 没有 ⟹
    //   GUI/CLI 三处手工逐字段复制时全部漏掉，apod-20k 与关闭 apod 的 Kaiser 实测只差
    //   5.96e-08（24-bit 量化范围），而正确的 apod 与 Kaiser 差 0.008727 ⟹ 用户选了
    //   「切趾」实际什么也没发生（假成功）。现在统一走 apply_filter_preset()。
    bool     apodizing = false;
    double   apodF1    = 0.0;      // 滚降起始（相对输入 Nyquist 的比例）
    double   apodF2    = 0.0;      // 滚降终止（相对输入 Nyquist 的比例）
    // 2026-10-25（音质线）：每相位抽头地板。0 = 老行为（T = taps/L，L 大时 T 会小到 4）；
    // 新预设（*-tight）用它把 T 与 L 解耦，否则 44.1→384k 时通带 20 kHz 掉 5 dB。
    int      minTapsPerInput = 0;
    // ★ 2026-10-30（261030-01 §4.3 抽头反解）：true = 按「该源该预设的最大质量点」反解 T，
    //   不再用固定预算 taps/L（那会让 DSD1024 的 T 掉到 32，通带塌 −4 dB）。
    //   默认 false 保持既有行为逐位不变；播放路径（rtaweb）显式打开。
    bool     autoTaps = false;
    // ★ 2026-10-30（261030-01 D-11）：允许走定点卷积。**只是允许** ——
    //   真正的闸门在 prepare() 里：源必须是整数 PCM ≤24 bit、输出 ≤24 bit、且无任何处理。
    //   闸门不满足时自动退回 double（实测那种情况下定点反而差 194 dB，见 test_fixed_conv §3b）。
    bool     useFixed = false;

    // 等响度补偿（手册 4.2 Loudness）。依 cfg.volumeDb 决定提升量：
    // 音量在上界及以上时提升量为 0 ⟹ 自动旁路 ⟹ 比特完美不受影响。
    bool                loudnessEnabled = false;
    dsp::LoudnessConfig loudness;

    // ---- 输出 ----
    int      outBits    = 0;          // 0 = 与源相同
    int      outChannels= 0;          // 0 = 与源相同
    // ★ 2026-10-30：**源文件的真实位深**（0 = 未知，退回按缓冲自身的 bits 判断）。
    //
    // 为什么必须有这个字段：解码器交给管线的缓冲位深是【容器位深】，不是源精度。
    //   MP3/AAC 经 ffmpeg 解出来是 pcm_f32le（bits=32、isFloat=true），而它的真实源是
    //   16 bit。旧判定只看 in.bits ⟹ 32→24 被当成「位深降低」、且 float 一律判死，
    //   于是一个 16-bit 有损源在 24-bit 输出下**永远**报「非 bit-perfect」，
    //   而同一台设备上 16-bit WAV 报的是「全程逐位一致」（实测同一会话内两种结论并存）。
    //   判据必须问「源有多深」，不是「容器有多宽」。
    int      sourceBits = 0;
    dsp::Shaper shaper  = dsp::Shaper::None;
    uint64_t seed       = 0xC0FFEEull;

    // ---- Matrix / 处理链 ----
    std::vector<dsp::MatrixRoute> matrix;      // 空 = 单位路由
    std::vector<ChannelProcess>   channel;     // 按【输出】声道索引
    size_t   convBlock = 1024;                 // 卷积分块大小（2 的幂）

    // 20 段参数均衡（261024-02 #6）。在【输入采样率】、现有文件 EQ 之前处理；
    // 由 rtaweb 持有同一个 shared_ptr，UI 改参数时热更新（引擎内部交叉淡入）。
    std::shared_ptr<dsp::PeqEngine> peq;

    // ---- ReplayGain（261027-01 #2；判据与顺序见 dsp/replaygain.h 文件头）----
    // 一级常数增益，乘在【解交错之后、PEQ/EQ/卷积/升频之前】：
    //   a) 防削波判据用的是源信号峰值（标签里的 peak 就是源峰值），放到升频之后
    //      -peak_dBFS 这条界就不成立了（插值过冲已经产生）；
    //   b) 常数增益与 LTI 处理可交换，但与【量化器不可交换】，必须在抖动/整形之前。
    // ★ 默认 0 dB ⟹ process() 整段跳过（不乘 1.0，"直通"与"乘 1.0"在 −0.0/NaN 上不等价）。
    double   replayGainDb  = 0.0;

    // ---- 音量 ----
    bool     volumeEnabled = false;   // 默认关闭，保证直通可证
    double   volumeDb      = 0.0;
    // 过采样峰值保护：检测到插值峰值超过满幅时，在【升频之前】先衰减，
    // 避免后级削波。0 = 不处理（逐位路径永远走不到这里）。
    double   preAttenDb    = 0.0;
    double   volumeRampMs  = 200.0;
};

// ====================================================================================
// ★ 2026-10-29（两轮审查 F22/F23）：滤波器预设的**唯一**映射入口。
//
// 为什么必须收敛成一个函数：GUI（tools/rtaweb.cpp）、CLI（tools/rtaplay.cpp，两处）
// 各自手工逐字段复制 FilterPreset → PipelineConfig，三份拷贝全部漏掉 apodizing/
// apodF1/apodF2 —— 实测「apod-20k」与「关闭 apod 的 Kaiser」只差 5.96e-08（24-bit
// 量化范围），而正确的 apod 与 Kaiser 应差 0.008727 ⟹ 用户选了切趾，声音一点没变。
// 以后新增任何预设字段，只需要改这里一处（resampler.h 的 apply_preset 是同源参考）。
//
// 只覆盖【滤波器/重采样】相关字段，不碰音量、EQ、卷积、输出格式。
// ====================================================================================
inline void apply_filter_preset(PipelineConfig& cfg, const dsp::FilterPreset& fp) {
    dsp::ResamplerConfig rc;
    dsp::apply_preset(rc, fp);
    cfg.taps = rc.totalTaps;
    cfg.transition = rc.transition;
    cfg.attenDb = rc.attenDb;
    cfg.filterWindow = rc.window;
    cfg.filterPhase = rc.phase;
    cfg.twoStage = rc.twoStage;
    cfg.apodizing = rc.apodizing;
    cfg.apodF1 = rc.apodF1;
    cfg.apodF2 = rc.apodF2;
    cfg.minTapsPerInput = rc.minTapsPerInput;
    if (fp.twoStage && fp.stage2Taps > 0) cfg.stage2Taps = rc.stage2Taps;
}

// Apply the same purpose policy as standalone resamplers and acceptance tools.
// Caller may subsequently apply an explicit user override of twoStage.
inline void apply_resample_purpose(PipelineConfig& cfg, dsp::ResamplePurpose purpose) {
    dsp::ResamplerConfig rc;
    rc.twoStage = cfg.twoStage;
    dsp::apply_resample_purpose(rc, purpose);
    cfg.twoStage = rc.twoStage;
}

struct PipelineStats {
    bool usedLoudness = false;      // 等响度补偿是否实际生效（音量 >= 上界时为 false）
    bool     bitPerfect = false;
    bool     zeroPad    = false;
    uint32_t inRate = 0, outRate = 0;
    int      inBits = 0, outBits = 0;
    int      inChannels = 0, outChannels = 0;
    size_t   inFrames = 0, outFrames = 0;
    int      tapsPerPhase = 0; // first stage, or the sole stage
    int      stage2TapsPerPhase = 0;
    size_t   cpuSimdOutputs = 0;
    size_t   cpuParallelThreads = 0;
    double   latencyInSamples = 0.0;
    // 各阶段是否实际生效
    bool     usedMatrix = false, usedEq = false, usedDelay = false, usedPeq = false;
    bool     usedConvolution = false, usedResample = false, usedVolume = false;
    bool     usedReplayGain = false;       // 本次是否乘了 ReplayGain 增益（0 dB ⟹ false，样本逐位未动）
    bool     usedGpuConvolution = false;   // 本次是否真的走了 GPU
    bool     usedFixed = false;            // 261030-01 D-11：本次流是否走了定点卷积（可观测证据）
    bool     usedGpuResample = false;      // 本块至少一级实际GPU dispatch成功
    uint32_t gpuResampleStageMask = 0;     // leaf=1; cascade first/second=1/2
    uint32_t gpuCompactResampleStageMask = 0; // compact ABI only, zero on legacy fallback
    bool     gpuAllResampleStagesUsed = false; // 当前块全部级均在GPU完成
    bool     usedChannelGain = false;      // 是否用了每声道增益
    std::string gpuStatus;                 // GPU 桥接状态说明
    int      eqBiquads = 0, maxDelaySamples = 0, maxIrLength = 0;
    // 流式状态建立（首次 process 调用时的"重活"）耗时与内存：滤波器设计是起播延迟的
    // 隐藏成本项，必须能实测（大文件卡死排查 2026-10-25）。coeffBytes = 多相系数表字节数。
    double   prepareMs = 0.0;
    size_t   coeffBytes = 0;
};

class Pipeline {
public:
    explicit Pipeline(const PipelineConfig& cfg) : cfg_(cfg) {}

    const PipelineConfig& config() const { return cfg_; }

    bool hasAnyProcessing() const {

        if (cfg_.outRate && cfg_.outRate != 0) { /* 需与源率比较，见下 */ }
        if (!cfg_.matrix.empty()) return true;
        if (cfg_.shaper != dsp::Shaper::None) return true;
        if (cfg_.volumeEnabled && cfg_.volumeDb != 0.0) return true;
        if (cfg_.replayGainDb != 0.0) return true;      // ReplayGain 改样本值 ⟹ 不是直通
        // ★ 2026-10-29（两轮审查 F21）：预衰减漏判。
        //   preAttenDb 是 ISP/调制器余量预衰减（实际信号增益，负 dB），pipeline 在
        //   阶段 2 之前真的乘了 10^(dB/20)。旧代码只把它写在 process() 里、没写进
        //   路径判定 ⟹ 只开 preAttenDb=−3 dB 时仍走「逐位直通」分支（输入 0.5 原样输出
        //   0.5、bitPerfect=1），而正确值是 0.353972892。这是"判据与实际处理不一致"，
        //   不是文案问题：直通分支根本不会执行那段衰减。
        if (cfg_.preAttenDb != 0.0) return true;
        // ★ 2026-10-29（F21 顺带）：等响度补偿也必须参与判定。
        //   旧代码完全没有这一条 ⟹ 音量 −40 dB + 开 Loudness（低音 +6 dB 真的在改样本）
        //   时仍报 bit-perfect=yes。判据与实际处理必须同源（铁律 3）。
        if (cfg_.loudnessEnabled &&
            dsp::loudness_effect_db(cfg_.loudness,
                dsp::replaygain::loudness_volume_db(cfg_.volumeDb, cfg_.replayGainDb)) > 1e-9)
            return true;
        for (const auto& c : cfg_.channel) {
            if (!c.eq.empty() || c.delaySamples != 0 || !c.ir.empty() || c.gainDb != 0.0) return true;
        }
        // ★ 2026-10-29（两轮审查 F20）：PEQ 的「目标为空」与「仍在过渡」必须分开。
        //   用户点 bypass 后 has_effect() 立刻变 false，而引擎内部的 128 样本交叉淡入
        //   还没走完 —— 只看 has_effect() 会让管线在淡入中途提前退到逐位直通，
        //   实测输出从 0.125594378 一步跳到 0.5（阶跃/爆音）。
        if (cfg_.peq && (cfg_.peq->has_effect() || cfg_.peq->transitioning())) return true;
        return false;
    }

    // ★ 2026-10-29（两轮审查 F04）：播放中实时改音量，做在【输出采样率、调制/装箱之前】
    //   的 PCM 域 —— 这是 DSD/DoP 唯一数学正确的音量作用点（PCM 增益不能碰 1-bit 载波）。
    //   只影响已建立的流式状态；volumeEnabled=false（CLI 未开音量）时是 no-op。
    //   与起播音量同一套渐变：动态变化会 ramp，0 dB 稳态逐位不变。
    void set_volume_db(double db) const {
        if (ss_.vol) ss_.vol->set_gain_db(db);
    }

    // ---- 流结束排空（★ 2026-10-29 两轮审查 F17）----
    //
    // 缺陷：process() 每块只吐"已经攒够"的样本，而卷积分块器（StreamingConvolver 预置
    //   B 帧延迟）与重采样 FIR（群延迟 (T*L-1)/(2L) 个输入样本）都会把末尾内容留在
    //   内部缓冲里。旧代码在源读到 EOF 时直接 RQ.done ⟹ 末尾 D 帧源内容永远丢掉了
    //   （实测：单位 IR、末样本 0.5，正常 EOF 时输出峰值 0；额外喂 1024 个零才吐出 0.5）。
    //
    // 现在的契约：算法延迟 D 由 algorithmic_latency_out_frames() 如实给出，
    //   起播时先丢掉 D 帧（消除前导空白，保证总时长 == 源时长），
    //   源结束后再 drain_chunk() 喂 D 帧零把内部剩下的样本取出来。
    double algorithmic_latency_out_frames() const {
        if (!ss_.ready) return 0.0;
        const uint32_t inRate  = ss_.inRate ? ss_.inRate : 1u;
        const uint32_t outRate = cfg_.outRate ? cfg_.outRate : inRate;
        double inSamples = 0.0;
        if (ss_.rs) inSamples += std::max(0.0, ss_.rs->latencyInputSamples());
        for (const auto& cv : ss_.conv) if (cv) inSamples += (double)cv->latency();
        const double out = inSamples * (double)outRate / (double)inRate;
        return (out > 0.0) ? out : 0.0;
    }

    // ★ 2026-10-29（音质准则 → F17 尾部策略）：卷积 IR 的自然衰减长度（输出域帧数）。
    //
    // 为什么需要它：源 ⊗ IR 的正确输出长度是 N + M − 1（M = IR 抽头数），而
    //   algorithmic_latency_out_frames() 只给出**延迟** D（卷积分块 B + 重采样群延迟）。
    //   只排空 D 帧 ⟹ 恰好得到 N 帧输出 ⟹ **把 IR 的混响衰减整段切掉**。
    //   房间校正 IR 常见 1.5~6 秒，切掉就是可听的截断（音质准则不允许）。
    //
    // 契约：返回 (M−1) 换算到输出域的帧数；无卷积时返回 0。
    //   生产者在 EOF 后排空 (D + 本函数) 帧，尾部才完整。
    //   上限 30 秒：防止病态 IR 把"结束"拖得没有边界。
    double convolution_tail_out_frames() const {
        size_t m = 0;
        for (const auto& c : cfg_.channel) m = std::max(m, c.ir.size());
        if (m <= 1) return 0.0;
        const uint32_t inRate  = ss_.inRate ? ss_.inRate : 1u;
        const uint32_t outRate = cfg_.outRate ? cfg_.outRate : inRate;
        double tail = (double)(m - 1) * (double)outRate / (double)inRate;
        const double cap = 30.0 * (double)outRate;
        if (tail > cap) tail = cap;
        return tail;
    }

    // FIR ringing after the last source sample, beyond delay compensation.
    // Nonlinear-phase filters report zero compensable delay but still retain
    // their complete causal tail. Callers drain this without trimming the head.
    double resampler_tail_out_frames() const {
        if (!ss_.ready || !ss_.rs) return 0.0;
        const uint32_t inRate = ss_.inRate ? ss_.inRate : 1u;
        const uint32_t outRate = cfg_.outRate ? cfg_.outRate : inRate;
        const double naturalTail = ss_.rs->tailInputSamples() - ss_.rs->latencyInputSamples();
        return std::max(0.0, naturalTail) * (double)outRate / (double)inRate;
    }

    // 喂一块零进管线，取出内部缓冲里剩下的样本。只能在流式状态建立后调用；
    // 未建立（逐位直通/无损展宽路径）时返回空 —— 那两条路径没有算法延迟。
    AudioBuffer drain_chunk(size_t frames = 4096, std::vector<std::vector<double>>* doubleOut = nullptr) const {
        if (!ss_.ready || frames == 0) return AudioBuffer{};
        AudioBuffer z;
        z.rate = ss_.inRate;
        z.channels = ss_.inChannels;
        z.bits = cfg_.outBits ? cfg_.outBits : 24;
        z.isFloat = false;
        z.raw.assign(frames * (size_t)z.channels, 0);
        return process(z, nullptr, doubleOut);
    }

    // 上一块是否至少一级实际在GPU完成；能力查询保留在resampler::gpu_active。
    bool gpuResampleActive() const {
        return lastGpuResampleUsed_ && ss_.rs && ss_.rs->gpu_used_last_block();
    }
    bool twoStageActive() const { return ss_.rs && ss_.rs->two_stage(); }
    int  twoStageFactor() const { return ss_.rs ? ss_.rs->stage1_factor() : 2; }

    // 跳转时清空所有 DSP 状态，但不重建滤波器。
    // 重建 Pipeline 会重新设计 26 万抽头的滤波器（秒级），跳转会明显卡顿；
    // 这里只把历史清零，配合调用方的"预卷"（提前解码 0.5 秒再丢掉）即可无拖尾。
    void seek_reset() const {
        if (ss_.rs) ss_.rs->reset();
        // 卷积器按 IR 长度分块，跳转时用同一个 IR 重新装载即可清空历史
        for (size_t c = 0; c < ss_.conv.size(); ++c) {
            if (!ss_.conv[c]) continue;
            if (c < ss_.convGpu.size() && ss_.convGpu[c]) ss_.convGpu[c]->reset(cfg_.channel[c].ir);
            if (c < cfg_.channel.size() && !cfg_.channel[c].ir.empty())
                ss_.conv[c]->reset(cfg_.channel[c].ir);
        }
        for (auto& b : ss_.eq) b.reset();
        for (auto& q : ss_.quant) q.reset();
        for (auto& l : ss_.loud) if (l) l->reset();
        // 2026-10-25：延迟线改持久后，跳转也必须清历史（否则跳转后先吐出上一个位置的尾巴）
        for (auto& d : ss_.delay) d.reset();
        // ★ 2026-10-29（两轮审查 F20 位置 A）：PEQ 是【跨曲共享】的同一个引擎实例
        //   （rtaweb 的 g_peq），跳转必须清它的滤波历史 —— 否则跳转后先吐出旧位置的
        //   拖尾，切歌时同理（前曲脉冲 → 新曲静音仍非零）。系数不动，只清状态。
        if (cfg_.peq) cfg_.peq->reset_state();
    }

    // ★ 2026-10-30：float 缓冲不再「一律判死」。
    //
    // 旧代码两句 「if (in.isFloat) return false;」 把「容器是 float」当成了「链路改了样本」，
    // 这是两件事。float32 缓冲（ffmpeg 解 mp3/aac 的产物）只要**真实源位深 ≤ 输出位深**，
    // 解码值就能被整数输出精确表示 ⟹ 链路本身一个比特都没改。
    // 真正不能承诺的只有一种：不知道源有多深（sourceBits==0 且缓冲是 float）。
    //
    // 另外：float 缓冲的输出必然是整数格式，所以它不是「同格式逐位」而是「无损展宽」，
    // 归到 willBeZeroPad 这一侧 —— 调用方（rtaweb）把 BP 与 Pad 都显示成「逐位一致」。
    bool willBeBitPerfect(const AudioBuffer& in) const {
        if (cfg_.forceBitPerfect) return true;
        if (in.isFloat) return false;          // float→int 是格式转换，不算「同格式逐位」
        const uint32_t orr = cfg_.outRate ? cfg_.outRate : in.rate;
        const int ob = cfg_.outBits ? cfg_.outBits : in.bits;
        const int oc = cfg_.outChannels ? cfg_.outChannels : in.channels;
        return orr == in.rate && ob == in.bits && oc == in.channels && !hasAnyProcessing();
    }

    // 只做位深展宽（源位深 < 输出位深：样本值不变，只在低位补零）。
    // 判据用**真实源位深** sourceBits；未知时：整数缓冲退回 in.bits，float 缓冲则不敢承诺。
    bool willBeZeroPad(const AudioBuffer& in) const {
        if (cfg_.forceBitPerfect) return false;
        const uint32_t orr = cfg_.outRate ? cfg_.outRate : in.rate;
        const int ob = cfg_.outBits ? cfg_.outBits : in.bits;
        const int oc = cfg_.outChannels ? cfg_.outChannels : in.channels;
        if (orr != in.rate || oc != in.channels) return false;
        if (hasAnyProcessing()) return false;
        const int sb = cfg_.sourceBits > 0 ? cfg_.sourceBits : (in.isFloat ? 0 : in.bits);
        if (sb <= 0) return false;
        return ob > sb;
    }

    // Optional double planes stop before PCM quantization. DSD must quantize
    // exactly once, in its sigma-delta modulator, rather than inherit source bits.
    // In that mode the returned AudioBuffer carries format only; st.outFrames
    // and doubleOut contain the actual output. Existing callers are unchanged.
    AudioBuffer process(const AudioBuffer& in, PipelineStats* stOut = nullptr,
                        std::vector<std::vector<double>>* doubleOut = nullptr) const {
        lastGpuResampleUsed_ = false; // Also covers bit-perfect/zero-pad early returns.
        PipelineStats st;
        st.inRate = in.rate; st.inBits = in.bits; st.inFrames = in.frames();
        st.inChannels = in.channels;
        st.outRate = cfg_.outRate ? cfg_.outRate : in.rate;
        st.outBits = cfg_.outBits ? cfg_.outBits : in.bits;
        st.outChannels = cfg_.outChannels ? cfg_.outChannels : in.channels;

        AudioBuffer out;
        out.rate = st.outRate;
        out.channels = st.outChannels;
        out.bits = st.outBits;
        out.isFloat = false;

        // ---- 路径 1：逐位直通 ----
        if (!doubleOut && willBeBitPerfect(in)) {
            out.bits = in.bits; out.channels = in.channels; out.isFloat = in.isFloat;
            out.raw = in.raw;
            st.bitPerfect = true;
            st.outChannels = in.channels;
            st.outFrames = out.frames();
            if (stOut) *stOut = st;
            return out;
        }

        // ---- 路径 2：无损展宽 ----
        if (!doubleOut && willBeZeroPad(in)) {
            const int shift = st.outBits - in.bits;
            out.raw.resize(in.raw.size());
            for (size_t i = 0; i < in.raw.size(); ++i)
                out.raw[i] = (int32_t)((uint32_t)in.raw[i] << shift);
            st.zeroPad = true;
            st.outFrames = out.frames();
            if (stOut) *stOut = st;
            return out;
        }

        // ---- 路径 3：完整 DSP ----
        const int inCh  = in.channels;
        const int outCh = st.outChannels;
        const size_t nIn = in.frames();

        // 首次调用（或配置变化）时建立持久流式状态；此后跨调用保持
        if (!ss_.ready || ss_.inRate != in.rate || ss_.inChannels != in.channels)
            prepare_stream(in, outCh);
        st.prepareMs = ss_.prepareMs;
        st.coeffBytes = ss_.coeffBytes;

        // 解交错
        std::vector<std::vector<double>> planes((size_t)inCh, std::vector<double>(nIn));
        for (size_t n = 0; n < nIn; ++n)
            for (int c = 0; c < inCh; ++c)
                planes[(size_t)c][n] = in.sample(n, c);

        // ---- ReplayGain（最靠输入的一级；见 PipelineConfig::replayGainDb 的三条理由）----
        // ★ 0 dB 必须整段跳过：apply_gain() 在 gainDb==0 时直接 return，一个样本都不碰。
        //   这既是"mode=off 逐位不变"的实现保证，也让 willBeBitPerfect 的结论仍然成立。
        if (cfg_.replayGainDb != 0.0) {
            std::vector<double*> ip;
            for (auto& p2 : planes) ip.push_back(p2.data());
            dsp::replaygain::apply_gain(cfg_.replayGainDb, ip.data(), (int)ip.size(), nIn);
            st.usedReplayGain = true;
        }

        // 过采样峰值保护（在重采样之前衰减，插值出来的峰就不会再溢出）
        if (cfg_.preAttenDb != 0.0) {
            const double g = std::pow(10.0, cfg_.preAttenDb / 20.0);
            for (auto& p : planes) for (auto& val : p) val *= g;
        }

        // ---- 阶段 2：Matrix ----
        std::vector<std::vector<double>> work((size_t)outCh, std::vector<double>(nIn, 0.0));
        if (!cfg_.matrix.empty()) {
            dsp::MatrixMixer mx;
            const std::vector<double*> ip = [&] {
                std::vector<double*> v; for (auto& p : planes) v.push_back(p.data()); return v; }();
            std::vector<double*> op;
            for (auto& w : work) op.push_back(w.data());
            mx.configure(inCh, outCh, cfg_.matrix);
            mx.process(ip.data(), op.data(), nIn);
            st.usedMatrix = true;
        } else {
            // 单位路由：前 min(in,out) 路一一对应，超出部分补零。
            // ★ 2026-10-25 真 BUG 修复（单声道 → 多声道 = 单边）：
            //   旧代码只填 c < min(in,out)，于是【单声道源 + 2 声道输出】时右声道恒为
            //   数字静音 —— 用户听到「只有左边有声」。这不是设计取舍，是漏了分支：
            //   同项目监听链为此显式建了 1→N 矩阵（src/audio/wasapi_capture.h:535-538
            //   注释原文「唯一需要显式展开的是【单声道 → 多声道复制】…不能因为走 DSP
            //   就变单边」），而播放链自己反而没做。
            //   生产触发路径（用户什么都不用改）：
            //     tools/rtaweb.cpp:2802 共享后端 pEff.outChannels = 系统混音声道数
            //     tools/rtaweb.cpp:2909/2910 → cfg.outChannels = outCh（!= in.channels）
            //   实测（tests/test_playback_edges.cpp §1）：mono 16/24-bit → 512 帧里
            //   右声道 512/512 全 0、左声道逐样本正确。
            //   2ch → 6ch 的既有语义（前两路逐位 + 其余静音）保持不变
            //   （tests/test_channel_gain.cpp 第 3 节钉住）。
            const int cp = (inCh < outCh) ? inCh : outCh;
            for (int c = 0; c < cp; ++c) work[(size_t)c] = planes[(size_t)c];
            if (inCh == 1 && outCh > 1)
                for (int c = 1; c < outCh; ++c) work[(size_t)c] = planes[0];
        }

        // ---- 阶段 3 前置：20 段 PEQ（输入采样率，现有文件 EQ 之前）----
        // 独立于 cfg_.channel（即使没有任何通道配置也要生效）；全 0dB/bypass 时快速路径直通。
        // ★ 2026-10-29（F20）：过渡期也必须继续跑 —— 见 hasAnyProcessing() 的注。
        if (cfg_.peq && (cfg_.peq->has_effect() || cfg_.peq->transitioning())) {
            for (int c = 0; c < outCh && c < (int)work.size(); ++c)
                cfg_.peq->process((size_t)c, work[(size_t)c].data(), work[(size_t)c].size());
            st.usedPeq = true;
        }

        // ---- 阶段 3+4：每输出声道 EQ / 延迟 / 卷积 ----
        for (int c = 0; c < outCh && c < (int)cfg_.channel.size(); ++c) {
            const auto& cp = cfg_.channel[(size_t)c];
            auto& w = work[(size_t)c];

            // 每声道增益（左右平衡 / 多声道校正）
            if (cp.gainDb != 0.0) {
                const double g = std::pow(10.0, cp.gainDb / 20.0);
                for (auto& v : w) v *= g;
                st.usedChannelGain = true;
            }

            if (!cp.eq.empty()) {
                // ★ 使用持久状态（不再每次新建并 reset），否则分块边界会清零滤波器状态
                size_t base = 0;
                for (int cc = 0; cc < c; ++cc) base += ss_.eqPerCh[(size_t)cc];
                const size_t cnt = ss_.eqPerCh[(size_t)c];
                for (size_t k = 0; k < cnt; ++k) {
                    dsp::Biquad& q = ss_.eq[base + k];
                    for (auto& v : w) v = q.process(v);
                }
                st.usedEq = true;
                st.eqBiquads += (int)cp.eq.size();
            }
            if (cp.delaySamples > 0) {
                // ★ 2026-10-25 真 BUG 修复：延迟线必须是【持久状态】。
                //   旧代码每块新建一条 DelayLine 再 set_samples（= 清零），等价于
                //   「本块先补 delaySamples 个零、再把本块最后 delaySamples 个样本丢掉」：
                //   实测（tests/test_playback_edges.cpp 第 3 节）2 块 x 256 帧、delay=64 时
                //   64/512 个样本丢失、块边界出现 0（断点/爆音）。
                //   EQ / 卷积 / 重采样 / 量化器都早已改成持久状态，只有延迟线漏了。
                // 现状：tools/rtaweb.cpp 还没有把 delaySamples 接到设置页，
                //   所以这是【潜伏缺陷】（接线后才会听到）；本次一并修掉。
                if ((size_t)c >= ss_.delay.size()) ss_.delay.resize((size_t)outCh);
                dsp::DelayLine& d = ss_.delay[(size_t)c];
                if (d.samples() != (size_t)cp.delaySamples) d.set_samples((size_t)cp.delaySamples);
                for (auto& v : w) v = d.process(v);
                st.usedDelay = true;
                st.maxDelaySamples = std::max(st.maxDelaySamples, cp.delaySamples);
            }
            if (!cp.ir.empty()) {
                // ★ 2026-10-26（261026-07，主线独立复测确认）：GPU 卷积路径在**流式**语义下是错的，已停用。
                //
                // 根因：rta_gpu_convolve 无跨调用状态（每次调用从零做 OLA，且只写 out[0..n)），
                // 而本管线每块只喂 kChunk(=4096) 样 ⟹ "上一块对本块的 IR 拖尾贡献"无处可来，
                // 每个块边界都断裂。它不是音质劣化，是算法性错误。
                //
                // 主线自写探针 tools/gpu_stream_verify.cpp 实测（262144 样、同一 IR，与 CPU 整块比）：
                //   IR=1024   CPU 流 −3047.0 dB(max|Δ|=0) | GPU 流  +2.3 dB(max|Δ|=1.70)
                //   IR=8192   CPU 流 −3056.2 dB(max|Δ|=0) | GPU 流  +2.1 dB(max|Δ|=3.80)
                //   IR=65536  CPU 流 −3063.8 dB(max|Δ|=0) | GPU 流  +3.4 dB(max|Δ|=11.7)
                //   IR=262144 CPU 流 −3073.0 dB(max|Δ|=0) | GPU 流  +1.7 dB(max|Δ|=25.2)
                // 误差能量超过信号本身，且 IR=1024 就已经错 —— 不是"只有长 IR 才出问题"。
                // CPU 的 StreamingConvolver 与整块逐位一致（max|Δ|=0），所以 CPU 路径是对的。
                //
                // 保留 DLL 不删：整块/离线用途（离线转换、大块批处理）在整块语义下是正确的。
                // 治本方向：DLL 增加带跨调用状态的流式入口（保留 irLen−1 个历史输入 + OLA 尾巴延续），
                // 修好后把 kGpuConvEnabled 置 true，并用 tools/gpu_stream_verify.cpp 做门禁
                // （判据：流式 GPU vs 流式 CPU 相对 RMS ≤ −120 dB、max|Δ| ≤ 1e-6，三档 IR）。
                // 2026-10-27（261026-08 #7）：GPU 卷积改用**带跨调用状态的流式会话**
                // （GpuStreamConvolver，latency()==block，与 CPU StreamingConvolver 同一条 FIFO）。
                // 261026-07 的缺陷（每 4096 样边界断裂、误差超过信号能量）已由 DLL 的
                // rta_gpu_convolve_create/_stream/_destroy 修掉，实测逐位一致（max|Δ|=0）。
                //
                // 门控按 IR 长度：实测（262144 样 / 64 块）CPU vs GPU 流式耗时
                //   IR=1024 12.6/32.2ms、8192 14.0/32.3、32768 24.7/32.0、
                //   65536 36.3/33.2、131072 61.4/35.3、262144 108.7/36.6
                // ⟹ IR >= 65536 才有收益（打平到 3 倍快），短 IR 反而慢 1.3~2.9 倍
                //   （真因：每次 kernel 提交+同步约 79 µs 固定开销）。
                constexpr size_t kGpuConvMinIrLen = 65536;
                if (cp.useGpu && cp.ir.size() >= kGpuConvMinIrLen) {
                    auto& gb = dsp::GpuBridge::instance();
                    st.gpuStatus = gb.status();
                    if (c < (int)ss_.convGpu.size() && ss_.convGpu[(size_t)c]) {
                        std::vector<double> tmp(w.size(), 0.0);
                        ss_.convGpu[(size_t)c]->process(w.data(), tmp.data(), w.size());
                        w.swap(tmp);
                        st.usedGpuConvolution = ss_.convGpu[(size_t)c]->on_gpu();
                        st.usedConvolution = true;
                        st.maxIrLength = std::max(st.maxIrLength, (int)cp.ir.size());
                        continue;                  // 该声道处理完毕（GPU 会话，内部失败会自己回退 CPU）
                    }
                }
                // ★ CPU 路径：持久流式卷积器（跨调用保持状态，任意分块一致）
                if (c < (int)ss_.conv.size() && ss_.conv[(size_t)c]) {
                    std::vector<double> tmp2(w.size(), 0.0);
                    ss_.conv[(size_t)c]->process(w.data(), tmp2.data(), w.size());
                    w.swap(tmp2);
                }
                st.usedConvolution = true;
                st.maxIrLength = std::max(st.maxIrLength, (int)cp.ir.size());
            }
        }

        // ---- 阶段 5：重采样 ----
        if (st.outRate != in.rate && ss_.rs) {
            // ★ 使用持久重采样器：相位累加器与历史跨调用保持
            // （原先每次新建 ⟹ outIdx_ 归零 ⟹ 分块边界相位不连续 ⟹ 71% 样本不同）
            dsp::PolyphaseResampler& rs = *ss_.rs;
            st.tapsPerPhase = rs.stage1_taps_per_phase();
            st.stage2TapsPerPhase = rs.stage2_taps_per_phase();
            st.latencyInSamples = rs.latencyInputSamples();
            st.usedResample = true;
            st.usedFixed = ss_.usedFixed;   // D-11：定点路径是否生效（与重采样同源）

            std::vector<const double*> ip;
            for (auto& w : work) ip.push_back(w.data());
            const size_t maxOut = rs.maxOutFrames(nIn);
            std::vector<std::vector<double>> res((size_t)outCh, std::vector<double>(maxOut));
            std::vector<double*> op;
            for (auto& r : res) op.push_back(r.data());
            const size_t produced = rs.process(ip.data(), nIn, op.data());
            st.cpuSimdOutputs = rs.cpu_simd_outputs_last_block();
            st.cpuParallelThreads = rs.cpu_parallel_threads_last_block();
            st.usedGpuResample = rs.gpu_used_last_block();
            lastGpuResampleUsed_ = st.usedGpuResample;
            st.gpuResampleStageMask = rs.gpu_stage_mask_last_block();
            st.gpuCompactResampleStageMask = rs.gpu_compact_stage_mask_last_block();
            st.gpuAllResampleStagesUsed = rs.gpu_all_stages_used_last_block();
            for (auto& r : res) r.resize(produced);
            work.swap(res);
        }

        // ---- 阶段 6：音量 ----
        const size_t nOut = work.empty() ? 0 : work[0].size();
        // ---- 等响度补偿（在音量之后、量化之前）----
        // 音量 >= 上界时 bass_db()/treble_db() 均为 0，Loudness::process 会直接返回，
        // 因此 0 dB 场景下这一段是【完全旁路】，不影响比特完美。
        if (!ss_.loud.empty()) {
            for (int c = 0; c < outCh && c < (int)ss_.loud.size(); ++c) {
                ss_.loud[(size_t)c]->process(work[(size_t)c].data(), work[(size_t)c].size());
            }
            st.usedLoudness = (ss_.loud[0]->bass_db() > 1e-9 || ss_.loud[0]->treble_db() > 1e-9);
        }

        if (cfg_.volumeEnabled && ss_.vol) {
            // 持久音量：渐变跨调用连续，不会在每个分块边界重启。
            // ★ 2026-10-29（F01）：必须按【帧】推进一次公共增益并同时作用于所有声道。
            //   旧写法 for (auto& w : work) vol->process(w.data(), w.size()) 让同一个有状态
            //   对象顺序处理每个声道、每个样本都推进一次计数 ⟹ 同一帧左右增益不同、
            //   渐变墙钟长度随声道数成倍拉长（2ch 200ms → 400ms）。
            std::vector<double*> vp;
            vp.reserve(work.size());
            for (auto& w : work) vp.push_back(w.data());
            ss_.vol->process_planes(vp.data(), (int)work.size(), nOut);
            st.usedVolume = (cfg_.volumeDb != 0.0);
        }

        if (doubleOut) {
            // ⚠ 2026-10-31 试过改成「按容量复用」（assign 而非 move），想让每块省掉
            //   33.6 MB 的分配+清零。实测**更差**：DSD1024 0.541 → 0.527，峰值内存 146 → 162 MB。
            //   原因：拷贝 33.6 MB 比操作系统大块分配（复用同一批页）更贵。保持 move。
            *doubleOut = std::move(work);
            st.outFrames = nOut;
            if (stOut) *stOut = st;
            return out;
        }

        // ---- 阶段 7：抖动/整形 + 量化 ----
        out.raw.resize(nOut * (size_t)outCh);
        std::vector<dsp::Quantizer>& qz = ss_.quant;   // ★ 持久：噪声整形误差历史跨调用保持
        if(qz.empty() && !out.isFloat) {
            std::vector<const double*> planes;
            planes.reserve(work.size());
            for(const auto& channel:work)planes.push_back(channel.data());
            dsp::pack_pcm_planes(planes.data(),outCh,nOut,st.outBits,out.raw.data());
            st.outFrames=out.frames();
            if(stOut)*stOut=st;
            return out;
        }
        for (size_t n = 0; n < nOut; ++n) {
            for (int c = 0; c < outCh; ++c) {
                const double x = work[(size_t)c][n];
                double y;
                if (!qz.empty()) y = qz[(size_t)c].process(x);
                else {
                    const double fs = (double)(1u << (st.outBits - 1));
                    y = std::floor(x * fs + 0.5) / fs;
                }
                out.setSample(n, c, y);
            }
        }
        st.outFrames = out.frames();
        if (stOut) *stOut = st;
        return out;
    }

private:
    // ---- 流式状态（Round 33）：DSP 对象必须跨 process() 调用存活 ----
    // 原先每次调用都重建它们，导致滤波状态在分块边界被清零。
    // 实测后果（Round 32 基线）：升频 71% 样本不同、EQ 6.8%、卷积 10.6%。
    struct StreamState {
        bool   ready = false;
        bool   usedFixed = false;   // D-11：本流是否走了定点卷积
        uint32_t inRate = 0;
        int    inChannels = 0;
        std::unique_ptr<dsp::PolyphaseResampler> rs;
        std::unique_ptr<dsp::VolumeControl>      vol;
        std::vector<dsp::Biquad>                 eq;      // 按输出声道展开的级联
        std::vector<size_t>                      eqPerCh; // 每声道的双二阶个数
        std::vector<dsp::Quantizer>              quant;
        std::vector<std::unique_ptr<dsp::StreamingConvolver>> conv;   // 每输出声道（CPU 兜底）
    // 2026-10-27（261026-08 #7）：GPU 流式卷积会话，每输出声道一个。
    // 与 CPU conv 二选一：GPU 可用且 IR 够长时用它，否则用 conv。
    // 它内部带跨调用状态（latency()==block，与 CPU StreamingConvolver 同一条 FIFO 实现），
    // 所以**不会**再出现 261026-07 那种每 4096 样断裂的问题。
    std::vector<std::unique_ptr<dsp::GpuStreamConvolver>> convGpu;
        std::vector<std::unique_ptr<dsp::Loudness>> loud;             // 每声道（各自持有滤波状态）
        // 每输出声道延迟线（2026-10-25 修复：原先每块新建 ⟹ 每块丢 delaySamples 个样本）
        std::vector<dsp::DelayLine> delay;
        double prepareMs = 0.0;      // prepare_stream() 耗时（滤波器设计 + 状态分配）
        size_t coeffBytes = 0;       // 重采样器系数表字节数（多相 L × 每相抽头 × 8）
    };
    mutable StreamState ss_;

    void prepare_stream(const AudioBuffer& in, int outCh) const {
        const auto t_prep_0 = std::chrono::steady_clock::now();
        ss_ = StreamState{};
        ss_.inRate = in.rate;
        ss_.inChannels = in.channels;

        const uint32_t outRate = cfg_.outRate ? cfg_.outRate : in.rate;
        if (outRate != in.rate) {
            dsp::ResamplerConfig rc;
            rc.inRate = in.rate; rc.outRate = outRate; rc.channels = outCh;
            rc.totalTaps = cfg_.taps; rc.attenDb = cfg_.attenDb; rc.transition = cfg_.transition;
            rc.window = cfg_.filterWindow; rc.phase = cfg_.filterPhase;
            rc.twoStage = cfg_.twoStage; rc.stage2Taps = cfg_.stage2Taps;
            // 2026-10-25：每相位抽头地板透传；
            // 2026-10-30（§4.3）：autoTaps 时按源反解 —— 与输出率解耦，DAC 率越高不会越糊。
            rc.minTapsPerInput = cfg_.minTapsPerInput;
            // D-11 闸门：定点只在「24 bit 整数直通」时精确，三条同时满足才启用。
            //   ① 输入是整数缓冲（float 缓冲说明源是浮点，转 int24 必丢精度）
            //   ② 源位深 ≤ 24（未知时用 in.bits）
            //   ③ 无任何处理（音量/EQ/PEQ/卷积/限幅都会改样本，破坏 k/2^23 的精确性）
            ss_.usedFixed = false;
            rc.useFixed = cfg_.useFixed
                       && !in.isFloat
                       && (cfg_.sourceBits > 0 ? cfg_.sourceBits <= 24 : in.bits <= 24)
                       && (cfg_.outBits == 0 || cfg_.outBits <= 24)
                       && !hasAnyProcessing()
                       && (double)((cfg_.autoTaps && rc.minTapsPerInput == 0)
                                   ? dsp::solve_taps_per_phase(cfg_.filterWindow, cfg_.transition, cfg_.apodizing,
                                                               cfg_.apodF1, cfg_.apodF2, cfg_.taps, 0, (double)in.rate)
                                   : rc.minTapsPerInput) * (double)outRate * (double)outCh <= 250e6;
            // 实测（2026-10-30）：闸门工作正常 —— 无处理时 usedFixed=1，加 −0.5 dB 音量后自动退回 double。
            //
            // 【算力闸门】定点用 __int128 标量累加，实测约 200 MMAC/s；double 走 SIMD 约 2 GMAC/s。
            // DSD1024 的 DoP 承载率 2.8224 MHz × T=211 × 2ch = 1.19 GMAC/s ⟹ 定点会慢 6 倍（实测 19 秒渲 3 秒），
            // 实时播放必然欠载。所以再加一条：需要的乘加率超过 250 MMAC/s 时自动退回 double。
            // 此预算回退使用 double 乘累加，不能宣称仍是整数逐位精确；usedFixed 如实上报。
            ss_.usedFixed = rc.useFixed;
            // 定点路径在重采样器层已验证：test_fixed_conv §5 与 double 差 −274.2 dB。
            if (cfg_.autoTaps && rc.minTapsPerInput == 0) {
                rc.minTapsPerInput = dsp::solve_taps_per_phase(
                    cfg_.filterWindow, cfg_.transition, cfg_.apodizing,
                    cfg_.apodF1, cfg_.apodF2, cfg_.taps, 0, (double)in.rate);
            }
            // ★ 2026-10-29（F22）：切趾三要素必须透传，否则"选了切趾等于没选"
            rc.apodizing = cfg_.apodizing;
            rc.apodF1    = cfg_.apodF1;
            rc.apodF2    = cfg_.apodF2;
            ss_.rs = std::make_unique<dsp::PolyphaseResampler>(rc);
            ss_.rs->set_use_gpu(cfg_.gpuResample);
        }
        if (cfg_.volumeEnabled) {
            ss_.vol = std::make_unique<dsp::VolumeControl>();
            ss_.vol->set_sample_rate((double)outRate);
            ss_.vol->set_ramp_ms(cfg_.volumeRampMs);
            // ★ 2026-10-29（F01）：起播必须【立刻到位】，不能从单位增益淡入。
            //   旧写法 set_gain_db() 只启动 200 ms 从 1.0 → 目标的渐变 ⟹ 选 −60 dB 时
            //   开头 200 ms 是满幅（首样本 0.499947969 vs 目标 0.0005，差 59.9991 dB）。
            ss_.vol->set_gain_db_snapped(cfg_.volumeDb);
        }
        const int ob = cfg_.outBits ? cfg_.outBits : in.bits;
        if (cfg_.shaper != dsp::Shaper::None || ob < in.bits) {
            for (int c = 0; c < outCh; ++c)
                // 第四个参数是采样率：整形器的拐点要按输出采样率重标定，
                // 否则 44.1k 输出时 NTF 会在 15~20 kHz 把噪声放大几百倍（见 dither.h 注释）
                ss_.quant.emplace_back(cfg_.shaper, ob, cfg_.seed + (uint64_t)c * 7919ull, (double)outRate);
        }
        ss_.conv.clear();
        ss_.conv.resize((size_t)outCh);      // unique_ptr 不可拷贝，不能用 assign(n, nullptr)
        ss_.convGpu.clear();
        ss_.convGpu.resize((size_t)outCh);
        // 延迟线按输出声道各一条（长度在首次 process 时按 cfg_.channel[c].delaySamples 定长）
        ss_.delay.assign((size_t)outCh, dsp::DelayLine{});

        ss_.loud.clear();
        if (cfg_.loudnessEnabled) {
            for (int c = 0; c < outCh; ++c) {
                auto L = std::make_unique<dsp::Loudness>(cfg_.loudness);
                L->init((double)outRate);
                // 提升量由【实际听音电平】决定：音量 + ReplayGain 造成的电平变化（一次性换算，
                // 不是反馈回路 —— 绝不要把 appliedDb 再喂回 ReplayGain，见 replaygain.h 文件头）。
                L->set_volume_db(dsp::replaygain::loudness_volume_db(cfg_.volumeDb, cfg_.replayGainDb));
                ss_.loud.push_back(std::move(L));
            }
        }
        for (int c = 0; c < outCh; ++c) {
            if (c < (int)cfg_.channel.size() && !cfg_.channel[(size_t)c].ir.empty()) {
                size_t blk = cfg_.convBlock;
                if (blk == 0 || (blk & (blk - 1)) != 0) blk = 1024;
                ss_.conv[(size_t)c] = std::make_unique<dsp::StreamingConvolver>(cfg_.channel[(size_t)c].ir, blk);
                // GPU 流式会话：只在用户勾了 useGpu 且 IR 够长时建（DLL 缺符号/建不起来就留空，
                // 播放时自然走上面的 CPU conv —— GpuStreamConvolver 内部也自带 CPU 兜底）
                if (cfg_.channel[(size_t)c].useGpu && cfg_.channel[(size_t)c].ir.size() >= 65536) {
                    ss_.convGpu[(size_t)c] =
                        std::make_unique<dsp::GpuStreamConvolver>(cfg_.channel[(size_t)c].ir, blk);
                    if (!ss_.convGpu[(size_t)c]->on_gpu()) ss_.convGpu[(size_t)c].reset();
                }
            }
        }
        ss_.eqPerCh.assign((size_t)outCh, 0);
        for (int c = 0; c < outCh; ++c) {
            if (c < (int)cfg_.channel.size()) {
                for (const auto& q : cfg_.channel[(size_t)c].eq) { ss_.eq.push_back(q); ss_.eq.back().reset(); }
                ss_.eqPerCh[(size_t)c] = cfg_.channel[(size_t)c].eq.size();
            }
        }
        // Count the actual stored coefficients of both stages; the outer
        // cascade has no phase table of its own and used to report zero bytes.
        if (ss_.rs) ss_.coeffBytes = ss_.rs->coefficient_count() * sizeof(double);
        ss_.prepareMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t_prep_0).count();
        ss_.ready = true;
    }

    mutable bool lastGpuResampleUsed_ = false;
    PipelineConfig cfg_;
};


// ====================================================================================
// DSD 异速率支路（2026-10-27，DSD 调制专线并入主线后落地）
//
// 只处理「DSD 源 + 目标 DSD 位率 ≠ 源位率」这一段：
//   源 1-bit → dsd_integrator 抽取到 midRate PCM → PolyphaseResampler 升到目标位率
//   → SdmModulator（既有 NTF/阶数/抖动/限幅档位）调制回 1-bit → 交回既有 DoP / ASIO 原生通路。
//
// ★ PCM 路径一行未动：本段是纯新增的函数与结构，Pipeline::process(AudioBuffer) 不引用它，
//   也不会因为 include 而改变任何行为（dsd_rate_convert.h 只在调用时才构造滤波器/调制器）。
//
// 用法（rtaweb 侧接线时）：
//   auto d = eng::dsd_rate_branch(src.rate, targetDsdRate);
//   if (d.supported) { ... 走本支路 ... } else if (d.handled) { ps.error = d.why; }
// 分块流式（播放用）直接持有 rta::dsp::DsdRateConverter，逐块 process_bits()；
// 整段（离线转换/测试）用下面的 dsd_rate_convert()。
// ====================================================================================
struct DsdRateBranchDecision {
    bool handled = false;    // false = 同速率（走 DirectSDM 直通，不进本支路）
    bool supported = false;  // false 且 handled = 明确拒绝，why 是带数字的原因
    uint32_t srcRate = 0, dstRate = 0;
    std::string why;
};

inline DsdRateBranchDecision dsd_rate_branch(uint32_t srcDsdRate, uint32_t dstDsdRate) {
    DsdRateBranchDecision d;
    d.srcRate = srcDsdRate;
    d.dstRate = dstDsdRate;
    if (srcDsdRate == 0 || dstDsdRate == 0) { d.why = "DSD 位率为 0（未识别）"; return d; }
    if (srcDsdRate == dstDsdRate) {
        d.why = "源位率 == 目标位率：走 DirectSDM 逐位直通，不重调制";
        return d;
    }
    const dsp::DsdRatePlan plan = dsp::plan_dsd_rate(srcDsdRate, dstDsdRate);
    d.handled = true;
    d.supported = plan.ok();
    d.why = plan.note;
    return d;
}

struct DsdRateBranchResult {
    bool ok = false;
    std::string why;
    dsp::DsdConvertResult conv;
};

// 整段转换（离线 / 测试）。返回 ok=false 时 why 就是给用户看的原因，绝不静默降级。
inline DsdRateBranchResult dsd_rate_convert(const rta::audio::DsdStream& in, uint32_t dstRate,
                                           const dsp::DsdRateConvertConfig& cfg,
                                           rta::audio::DsdStream& out) {
    DsdRateBranchResult r;
    const DsdRateBranchDecision d = dsd_rate_branch(in.rate, dstRate);
    if (!d.handled) {                  // 同速率：逐位搬运（与 DirectSDM 语义一致，不重调制）
        out = in;
        r.ok = true;
        r.why = d.why;
        return r;
    }
    if (!d.supported) { r.why = d.why; return r; }
    try {
        r.conv = dsp::convert_dsd_stream(in, dstRate, cfg, out);
        r.ok = true;
        r.why = d.why;
    } catch (const std::exception& e) {
        r.why = e.what();
    }
    return r;
}

} // namespace rta::engine
