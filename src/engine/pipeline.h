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
#include <vector>
#include <string>
#include <algorithm>
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
    double   attenDb    = 180.0;
    double   transition = 0.04;
    dsp::WindowKind filterWindow = dsp::WindowKind::Kaiser;   // 命名滤波器（--filter）
    dsp::PhaseKind  filterPhase  = dsp::PhaseKind::Linear;

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
    bool     usedMatrix = false, usedEq = false, usedDelay = false;
    bool     usedConvolution = false, usedResample = false, usedVolume = false;
    bool     usedGpuConvolution = false;   // 本次是否真的走了 GPU
    bool     usedChannelGain = false;      // 是否用了每声道增益
    std::string gpuStatus;                 // GPU 桥接状态说明
    int      eqBiquads = 0, maxDelaySamples = 0, maxIrLength = 0;
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
            // 单位路由（声道数不变时才允许）
            for (int c = 0; c < outCh && c < inCh; ++c) work[(size_t)c] = planes[(size_t)c];
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
                dsp::DelayLine d;
                d.set_samples((size_t)cp.delaySamples);
                for (auto& v : w) v = d.process(v);
                st.usedDelay = true;
                st.maxDelaySamples = std::max(st.maxDelaySamples, cp.delaySamples);
            }
            if (!cp.ir.empty()) {
                // 优先走 GPU（整块卷积）；不可用或失败则自动退回 CPU 分块 FFT
                bool done = false;
                if (cp.useGpu) {
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
    };
    mutable StreamState ss_;

    void prepare_stream(const AudioBuffer& in, int outCh) const {
        ss_ = StreamState{};
        ss_.inRate = in.rate;
        ss_.inChannels = in.channels;

        const uint32_t outRate = cfg_.outRate ? cfg_.outRate : in.rate;
        if (outRate != in.rate) {
            dsp::ResamplerConfig rc;
            rc.inRate = in.rate; rc.outRate = outRate; rc.channels = outCh;
            rc.totalTaps = cfg_.taps; rc.attenDb = cfg_.attenDb; rc.transition = cfg_.transition;
            rc.window = cfg_.filterWindow; rc.phase = cfg_.filterPhase;
            rc.twoStage = cfg_.twoStage;
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
        ss_.ready = true;
    }

    PipelineConfig cfg_;
};

} // namespace rta::engine
