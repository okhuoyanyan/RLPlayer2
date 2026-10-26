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
#include "dsp/dither.h"
#include "dsp/convolution.h"
#include "dsp/matrix.h"
#include "dsp/volume.h"
#include "dsp/gpu_bridge.h"
#include "dsp/loudness.h"
#include "dsp/peq.h"          // 261024-02 #6：20 段参数均衡（输入采样率、现有文件 EQ 之前）
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
    // 2026-10-25（音质线）：每相位抽头地板。0 = 老行为（T = taps/L，L 大时 T 会小到 4）；
    // 新预设（*-tight）用它把 T 与 L 解耦，否则 44.1→384k 时通带 20 kHz 掉 5 dB。
    int      minTapsPerInput = 0;

    // 等响度补偿（手册 4.2 Loudness）。依 cfg.volumeDb 决定提升量：
    // 音量在上界及以上时提升量为 0 ⟹ 自动旁路 ⟹ 比特完美不受影响。
    bool                loudnessEnabled = false;
    dsp::LoudnessConfig loudness;

    // ---- 输出 ----
    int      outBits    = 0;          // 0 = 与源相同
    int      outChannels= 0;          // 0 = 与源相同
    dsp::Shaper shaper  = dsp::Shaper::None;
    uint64_t seed       = 0xC0FFEEull;

    // ---- Matrix / 处理链 ----
    std::vector<dsp::MatrixRoute> matrix;      // 空 = 单位路由
    std::vector<ChannelProcess>   channel;     // 按【输出】声道索引
    size_t   convBlock = 1024;                 // 卷积分块大小（2 的幂）

    // 20 段参数均衡（261024-02 #6）。在【输入采样率】、现有文件 EQ 之前处理；
    // 由 rtaweb 持有同一个 shared_ptr，UI 改参数时热更新（引擎内部交叉淡入）。
    std::shared_ptr<dsp::PeqEngine> peq;

    // ---- 音量 ----
    bool     volumeEnabled = false;   // 默认关闭，保证直通可证
    double   volumeDb      = 0.0;
    // 过采样峰值保护：检测到插值峰值超过满幅时，在【升频之前】先衰减，
    // 避免后级削波。0 = 不处理（逐位路径永远走不到这里）。
    double   preAttenDb    = 0.0;
    double   volumeRampMs  = 200.0;
};

struct PipelineStats {
    bool usedLoudness = false;      // 等响度补偿是否实际生效（音量 >= 上界时为 false）
    bool     bitPerfect = false;
    bool     zeroPad    = false;
    uint32_t inRate = 0, outRate = 0;
    int      inBits = 0, outBits = 0;
    int      inChannels = 0, outChannels = 0;
    size_t   inFrames = 0, outFrames = 0;
    int      tapsPerPhase = 0;
    double   latencyInSamples = 0.0;
    // 各阶段是否实际生效
    bool     usedMatrix = false, usedEq = false, usedDelay = false, usedPeq = false;
    bool     usedConvolution = false, usedResample = false, usedVolume = false;
    bool     usedGpuConvolution = false;   // 本次是否真的走了 GPU
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
        for (const auto& c : cfg_.channel) {
            if (!c.eq.empty() || c.delaySamples != 0 || !c.ir.empty() || c.gainDb != 0.0) return true;
        }
        if (cfg_.peq && cfg_.peq->has_effect()) return true;   // PEQ 全 0dB / bypass 不影响直通
        return false;
    }

    // 升频是否真的跑在 GPU 上（DLL 缺失或不可用时为 false，走 CPU）
    bool gpuResampleActive() const { return ss_.rs && ss_.rs->gpu_active(); }
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
            if (c < cfg_.channel.size() && !cfg_.channel[c].ir.empty())
                ss_.conv[c]->reset(cfg_.channel[c].ir);
        }
        for (auto& b : ss_.eq) b.reset();
        for (auto& q : ss_.quant) q.reset();
        for (auto& l : ss_.loud) if (l) l->reset();
        // 2026-10-25：延迟线改持久后，跳转也必须清历史（否则跳转后先吐出上一个位置的尾巴）
        for (auto& d : ss_.delay) d.reset();
    }

    bool willBeBitPerfect(const AudioBuffer& in) const {
        if (cfg_.forceBitPerfect) return true;
        if (in.isFloat) return false;
        const uint32_t orr = cfg_.outRate ? cfg_.outRate : in.rate;
        const int ob = cfg_.outBits ? cfg_.outBits : in.bits;
        const int oc = cfg_.outChannels ? cfg_.outChannels : in.channels;
        return orr == in.rate && ob == in.bits && oc == in.channels && !hasAnyProcessing();
    }

    bool willBeZeroPad(const AudioBuffer& in) const {
        if (cfg_.forceBitPerfect) return false;
        if (in.isFloat) return false;
        const uint32_t orr = cfg_.outRate ? cfg_.outRate : in.rate;
        const int ob = cfg_.outBits ? cfg_.outBits : in.bits;
        const int oc = cfg_.outChannels ? cfg_.outChannels : in.channels;
        return orr == in.rate && ob > in.bits && oc == in.channels && !hasAnyProcessing();
    }

    AudioBuffer process(const AudioBuffer& in, PipelineStats* stOut = nullptr) const {
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
        if (willBeBitPerfect(in)) {
            out.bits = in.bits; out.channels = in.channels; out.isFloat = in.isFloat;
            out.raw = in.raw;
            st.bitPerfect = true;
            st.outChannels = in.channels;
            st.outFrames = out.frames();
            if (stOut) *stOut = st;
            return out;
        }

        // ---- 路径 2：无损展宽 ----
        if (willBeZeroPad(in)) {
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
        if (cfg_.peq && cfg_.peq->has_effect()) {
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
                constexpr bool kGpuConvEnabled = false;
                bool done = false;
                if (kGpuConvEnabled && cp.useGpu) {
                    auto& gb = dsp::GpuBridge::instance();
                    st.gpuStatus = gb.status();
                    if (gb.available()) {
                        std::vector<double> tmp(w.size(), 0.0);
                        if (gb.convolve(w.data(), w.size(), cp.ir.data(), cp.ir.size(),
                                        tmp.data(), (int)cfg_.convBlock) == 0) {
                            w.swap(tmp);
                            done = true;
                            st.usedGpuConvolution = true;
                        }
                    }
                }
                if (done) {
                    st.usedConvolution = true;
                    st.maxIrLength = std::max(st.maxIrLength, (int)cp.ir.size());
                    continue;                      // 该声道处理完毕（GPU 路径）
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
            st.tapsPerPhase = rs.tapsPerPhase();
            st.latencyInSamples = rs.latencyInputSamples();
            st.usedResample = true;

            std::vector<const double*> ip;
            for (auto& w : work) ip.push_back(w.data());
            const size_t maxOut = rs.maxOutFrames(nIn);
            std::vector<std::vector<double>> res((size_t)outCh, std::vector<double>(maxOut));
            std::vector<double*> op;
            for (auto& r : res) op.push_back(r.data());
            const size_t produced = rs.process(ip.data(), nIn, op.data());
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
            // 持久音量：渐变跨调用连续，不会在每个分块边界重启
            for (auto& w : work) ss_.vol->process(w.data(), w.size());
            st.usedVolume = (cfg_.volumeDb != 0.0);
        }

        // ---- 阶段 7：抖动/整形 + 量化 ----
        out.raw.resize(nOut * (size_t)outCh);
        std::vector<dsp::Quantizer>& qz = ss_.quant;   // ★ 持久：噪声整形误差历史跨调用保持
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
        uint32_t inRate = 0;
        int    inChannels = 0;
        std::unique_ptr<dsp::PolyphaseResampler> rs;
        std::unique_ptr<dsp::VolumeControl>      vol;
        std::vector<dsp::Biquad>                 eq;      // 按输出声道展开的级联
        std::vector<size_t>                      eqPerCh; // 每声道的双二阶个数
        std::vector<dsp::Quantizer>              quant;
        std::vector<std::unique_ptr<dsp::StreamingConvolver>> conv;   // 每输出声道
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
            rc.minTapsPerInput = cfg_.minTapsPerInput;   // 2026-10-25：每相位抽头地板透传
            ss_.rs = std::make_unique<dsp::PolyphaseResampler>(rc);
            ss_.rs->set_use_gpu(cfg_.gpuResample);
        }
        if (cfg_.volumeEnabled) {
            ss_.vol = std::make_unique<dsp::VolumeControl>();
            ss_.vol->set_sample_rate((double)outRate);
            ss_.vol->set_ramp_ms(cfg_.volumeRampMs);
            ss_.vol->set_gain_db(cfg_.volumeDb);
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
        // 延迟线按输出声道各一条（长度在首次 process 时按 cfg_.channel[c].delaySamples 定长）
        ss_.delay.assign((size_t)outCh, dsp::DelayLine{});

        ss_.loud.clear();
        if (cfg_.loudnessEnabled) {
            for (int c = 0; c < outCh; ++c) {
                auto L = std::make_unique<dsp::Loudness>(cfg_.loudness);
                L->init((double)outRate);
                L->set_volume_db(cfg_.volumeDb);   // 提升量由音量决定
                ss_.loud.push_back(std::move(L));
            }
        }
        for (int c = 0; c < outCh; ++c) {
            if (c < (int)cfg_.channel.size() && !cfg_.channel[(size_t)c].ir.empty()) {
                size_t blk = cfg_.convBlock;
                if (blk == 0 || (blk & (blk - 1)) != 0) blk = 1024;
                ss_.conv[(size_t)c] = std::make_unique<dsp::StreamingConvolver>(cfg_.channel[(size_t)c].ir, blk);
            }
        }
        ss_.eqPerCh.assign((size_t)outCh, 0);
        for (int c = 0; c < outCh; ++c) {
            if (c < (int)cfg_.channel.size()) {
                for (const auto& q : cfg_.channel[(size_t)c].eq) { ss_.eq.push_back(q); ss_.eq.back().reset(); }
                ss_.eqPerCh[(size_t)c] = cfg_.channel[(size_t)c].eq.size();
            }
        }
        // 多相系数表大小：L 相 × 每相抽头 × 8 字节（两级时第一级另算，量级相同）
        if (ss_.rs) {
            const uint32_t a = in.rate, b = outRate;
            uint32_t g = a, h = b;
            while (h) { const uint32_t t2 = g % h; g = h; h = t2; }
            const size_t L = g ? (size_t)(b / g) : 0;
            ss_.coeffBytes = L * (size_t)ss_.rs->tapsPerPhase() * sizeof(double);
        }
        ss_.prepareMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t_prep_0).count();
        ss_.ready = true;
    }

    PipelineConfig cfg_;
};

} // namespace rta::engine
