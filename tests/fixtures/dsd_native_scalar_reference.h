// Frozen scalar converter class, renamed only. Shared design/config types come
// from production header; no copied filter or modulation implementation.
// Source: D:/RLPlayer-DSP-3.0.6-20261031/source/src/dsp/dsd_rate_convert.h
// Original class SHA256: 52794a960aeb32bbd7c6228c09416c80016b8316219bc8ca0d7d8dcab9596d1b
#pragma once
#include "dsp/dsd_rate_convert.h"
namespace rta::dsp {
class DsdRateConverterScalarReference {
public:
    DsdRateConverterScalarReference(const DsdRatePlan& plan, const DsdRateConvertConfig& cfg, int channels)
        : plan_(plan), cfg_(cfg), ch_(channels < 1 ? 1 : channels) {
        if (!plan_.ok() && plan_.mode != DsdRateMode::Direct)
            throw std::runtime_error("DSD→DSD 速率转换不可用：" + plan_.note);
        gainDb_ = cfg_.headroomAuto ? dsd_convert_safety_headroom_db(1.05, (double)plan_.dstRate)
                                    : std::min(0.0, cfg_.preAttenDb);
        gain_ = std::pow(10.0, gainDb_ / 20.0);
        if (!plan_.needs_dsp()) return;
        passbandHz_ = conversion_passband(cfg_.conversion);

        // ---- 抽取器：源位率 → midRate（每声道一个，状态独立）----
        IntegratorConfig ic = conversion_integrator(cfg_.decimator, cfg_.conversion,
                                                    plan_.srcRate, plan_.midRate,
                                                    cfg_.decimatorAttenDb, cfg_.twoStageDecimator);
        for (int c = 0; c < ch_; ++c) dec_.push_back(std::make_unique<DsdDecimator>(ic));

        // ---- 插值器：midRate → 目标位率（整数倍，L = interpolation）----
        if (plan_.interpolation > 1) {
            ResamplerConfig rc;
            rc.inRate  = plan_.midRate;
            rc.outRate = plan_.dstRate;
            rc.channels = ch_;
            rc.attenDb = cfg_.interpAttenDb;
            // 关键映射（resampler.h 的 transition 是"截止点相对输入 Nyquist 回退的比例"）：
            // 升 L 倍时输入 Nyquist 在内插域位于 1/L；要通带到 passband、阻带从
            // (midRate − passband) 开始 ⟹ 过渡带宽度 = midRate − 2·passband。
            // 抽取器已经把 outRate − passband 以上压到 −120 dB，所以插值器只要在
            // "输入 Nyquist 到 第一镜像" 之间完成过渡即可 ⟹ transition 取 0（截止 = 输入 Nyquist）。
            rc.transition = 0.0;
            rc.twoStage = false;
            const double pass = ic.passbandHz;
            const double transHz = (double)plan_.midRate - 2.0 * pass;   // 136.4 kHz @ DSD64
            const int L = plan_.interpolation;
            int tapsTotal = kaiser_taps_for(cfg_.interpAttenDb, transHz / (double)plan_.dstRate);
            int T = (int)std::ceil((double)tapsTotal / (double)L);
            if (T < 4) T = 4;
            rc.minTapsPerInput = T;                  // 抽头预算与 L 解耦（原型 = T×L）
            rs_ = std::make_unique<PolyphaseResampler>(rc);
            tapsPerPhase_ = T;
        } else {
            tapsPerPhase_ = 0;                       // 降速：抽取器直接落到目标位率
        }

        // ---- 调制器：每声道一个（状态跨调用保持）----
        SdmConfig mc;
        mc.fs       = (double)plan_.dstRate;
        mc.family   = cfg_.modFamily;
        mc.levels   = 2;
        mc.oneBitOutput = true;  // Packed DSD cannot carry AHM multilevel samples.
        mc.order    = cfg_.modOrder;  // Constructor guards retain the actual step-down reason.
        mc.cutoffHz = cfg_.modCutoffHz;
        mc.ditherLsb = cfg_.modDitherLsb;
        mc.ntfHinf  = cfg_.ntfHinf;
        mc.oobGainDb = cfg_.modOobGainDb;
        mc.ahmBits = cfg_.modAhmBits;
        // ★ 只综合一次 NTF，其余声道拷贝（EC 族的 synthesize_ntf 实测 410 ms/声道；
        //   拷贝不改变任何行为：既有 modulator 各声道本来就用同一个固定种子的抖动序列）。
        mod_.push_back(std::make_unique<SdmModulator>(mc));
        for (int c = 1; c < ch_; ++c)
            mod_.push_back(std::make_unique<SdmModulator>(*mod_[0]));
        if (!mod_.empty()) {
            st_.effectiveOrder = mod_[0]->effectiveOrder();
            st_.steppedDown = mod_[0]->steppedDown();
            st_.stepDownReason = mod_[0]->stepDownReason();
        }
        st_.gainDb = gainDb_;
        init_sink(ch_);
    }

    const DsdRatePlan& plan() const { return plan_; }
    const DsdRateStats& stats() const { return st_; }
    uint32_t midRate() const { return plan_.midRate; }
    int channels() const { return ch_; }
    int interpolation() const { return plan_.interpolation; }
    int decimation() const { return plan_.decimation; }
    int tapsPerPhase() const { return tapsPerPhase_; }
    double gain_db() const { return gainDb_; }
    double passband_hz() const { return passbandHz_; }
    // 插值器原型（内插域，含 ×L 直流增益）。判据用：通带平坦度 / 阻带衰减直接量它，
    // 不靠"看起来对"。空 = 降速路径（无插值器）。
    void interp_prototype_into(std::vector<double>& out) const {
        if (rs_) rs_->prototype_into(out); else out.clear();
    }
    // 插值器组延迟（中间 PCM 样本）→ 换算成目标位数
    double latencyOutBits() const {
        if (!rs_) return 0.0;
        return rs_->latencyInputSamples() * (double)plan_.interpolation;
    }

    // 调用方拿到峰值预扫后可以重设余量（必须在开始处理之前调用）
    void set_pre_atten_db(double db) {
        gainDb_ = std::min(0.0, db);
        gain_ = std::pow(10.0, gainDb_ / 20.0);
        st_.gainDb = gainDb_;
    }

    void reset() {
        for (auto& d : dec_) d->reset();
        if (rs_) rs_->reset();
        for (auto& m : mod_) m->reset();
        st_ = DsdRateStats{};
        st_.gainDb = gainDb_;
        if (!mod_.empty()) {
            st_.effectiveOrder = mod_[0]->effectiveOrder();
            st_.steppedDown = mod_[0]->steppedDown();
            st_.stepDownReason = mod_[0]->stepDownReason();
        }
        outBits_ = 0;
        headConfigured_ = false; finished_ = false; dsdInput_ = false;
        skipLeft_ = 0;
        init_sink(ch_);
    }

    // ---- 1-bit(midRate 之前) → PCM@midRate（每声道追加到 out）----
    size_t to_pcm(const rta::audio::DsdStream& in, std::vector<std::vector<double>>& out) {
        if (!plan_.needs_dsp()) return 0;
        if (in.channels != ch_)
            throw std::runtime_error("DsdRateConverterScalarReference::to_pcm 声道数不匹配");
        if ((int)out.size() != ch_) out.assign((size_t)ch_, {});
        size_t n = 0;
        for (int c = 0; c < ch_; ++c) {
            const size_t before = out[(size_t)c].size();
            dec_[(size_t)c]->process_bits(in.data[(size_t)c].data(), (size_t)in.bits, out[(size_t)c]);
            n = out[(size_t)c].size() - before;
        }
        st_.pcmFrames += (long long)n;
        return n;
    }

    // ---- PCM@midRate → 1-bit@目标位率（追加到 out）----
    // pcm 每声道至少 frames 个样本；调用方可以在这里插入过载/越界探针（铁律 7 的接缝）。
    size_t push_pcm(const std::vector<std::vector<double>>& pcm, size_t frames,
                    rta::audio::DsdStream& out) {
        if (!plan_.needs_dsp()) return 0;
        if ((int)pcm.size() != ch_) throw std::runtime_error("DsdRateConverterScalarReference::push_pcm 声道数不匹配");
        if (finished_) throw std::runtime_error("DsdRateConverterScalarReference::push_pcm after finish; reset first");
        if (frames == 0) return 0;
        if (sinkStream_ != &out) { sinkStream_ = &out; align_sink(out); }
        if (!headConfigured_) {
            double delay = rs_ ? rs_->latencyInputSamples() * plan_.interpolation : 0.0;
            if (dsdInput_) delay += dec_[0]->latencyInputBits() * (double)plan_.dstRate / plan_.srcRate;
            skipLeft_ = (size_t)std::llround(delay);
            headConfigured_ = true;
        }

        std::vector<const double*> ip((size_t)ch_);
        for (int c = 0; c < ch_; ++c) ip[(size_t)c] = pcm[(size_t)c].data();

        size_t produced = frames;
        std::vector<std::vector<double>> resampled;
        if (rs_) {
            const size_t cap = rs_->maxOutFrames(frames);
            resampled.assign((size_t)ch_, std::vector<double>(cap, 0.0));
            std::vector<double*> op((size_t)ch_);
            for (int c = 0; c < ch_; ++c) op[(size_t)c] = resampled[(size_t)c].data();
            produced = rs_->process(ip.data(), frames, op.data());
        }
        size_t written = 0;
        for (size_t n = 0; n < produced; ++n) {
            const bool emit = skipLeft_ == 0;
            for (int c = 0; c < ch_; ++c) {
                const double x = ((rs_ ? resampled[(size_t)c][n] : pcm[(size_t)c][n])) * gain_;
                const double ax = std::fabs(x);
                if (ax > st_.inPeak) st_.inPeak = ax;
                const double y = mod_[(size_t)c]->process(x);
                if (emit) put_bit((uint64_t)(outBits_ + (long long)written), (size_t)c, y > 0.0);
            }
            if (emit) ++written; else --skipLeft_;
        }
        outBits_ += (long long)written;
        st_.outBits = outBits_;
        collect_mod_stats();
        return written;
    }

    // ---- 一条龙：1-bit@源位率 → 1-bit@目标位率 ----
    size_t process_bits(const rta::audio::DsdStream& in, rta::audio::DsdStream& out) {
        if (!plan_.needs_dsp()) {                    // Direct：逐位搬运
            copy_bits(in, out);
            return (size_t)in.bits;
        }
        if (finished_) throw std::runtime_error("DsdRateConverterScalarReference::process_bits after finish; reset first");
        dsdInput_ = true;
        std::vector<std::vector<double>> pcm((size_t)ch_);
        const size_t got = to_pcm(in, pcm);
        if (got == 0) return 0;
        return push_pcm(pcm, got, out);
    }

    // Finish preserves the complete causal decimator/interpolator tail, while
    // trimming only linear-phase delay at the head. Converted duration includes
    // natural ringing; it is no longer exactly source duration * rate ratio.
    // Repeated finish calls are idempotent. IIR tails use a documented -300 dB bound.
    void finish(rta::audio::DsdStream& out) {
        if (!finished_ && plan_.needs_dsp()) {
            if (dsdInput_) {
                std::vector<std::vector<double>> tail((size_t)ch_);
                for (int c = 0; c < ch_; ++c) dec_[(size_t)c]->drain(tail[(size_t)c]);
                if (!tail.empty() && !tail[0].empty()) push_pcm(tail, tail[0].size(), out);
            }
            if (rs_ && headConfigured_) {
                const size_t zeros = (size_t)std::ceil(rs_->tailInputSamples());
                if (zeros) {
                    std::vector<std::vector<double>> tail((size_t)ch_, std::vector<double>(zeros, 0.0));
                    push_pcm(tail, zeros, out);
                }
            }
            // A 1-bit modulator keeps producing shaped idle bits at zero input;
            // it cannot be emptied like a finite FIR. Preserve a 2ms quiet tail
            // so stopping the bitstream does not cut its post-transient error
            // cancellation. This is settling silence, not a claim of zero state.
            if (headConfigured_) {
                const size_t settle = (size_t)std::ceil(0.002 * plan_.dstRate);
                for (size_t n = 0; n < settle; ++n) {
                    for (int c = 0; c < ch_; ++c)
                        put_bit((uint64_t)outBits_ + n, (size_t)c, mod_[(size_t)c]->process(0.0) > 0.0);
                }
                outBits_ += (long long)settle;
                st_.outBits = outBits_;
                collect_mod_stats();
            }
            finished_ = true;
        }
        if (sinkStream_ != &out) { sinkStream_ = &out; align_sink(out); }
        out.rate = plan_.dstRate;
        out.channels = ch_;
        out.bits = (uint64_t)outBits_;
        for (int c = 0; c < ch_; ++c) {
            const size_t need = (size_t)((outBits_ + 7) / 8);
            if (out.data[(size_t)c].size() < need) out.data[(size_t)c].resize(need, 0);
        }
        if (out.container.empty()) out.container = "conv";
    }

private:
    void init_sink(int) { sinkStream_ = nullptr; outBits_ = 0; }

    void align_sink(rta::audio::DsdStream& out) {
        if ((int)out.data.size() != ch_) out.data.assign((size_t)ch_, {});
        if (out.bits != 0) {
            if (out.bits % 8 != 0)
                throw std::runtime_error("DsdRateConverterScalarReference：目标流不是 8 位对齐，无法追加");
            outBits_ = (long long)out.bits;
            const size_t need = (size_t)(outBits_ / 8);
            for (int c = 0; c < ch_; ++c)
                if (out.data[(size_t)c].size() < need) out.data[(size_t)c].resize(need, 0);
        }
        out.rate = plan_.dstRate;
        out.channels = ch_;
    }

    inline void put_bit(uint64_t pos, size_t c, bool v) {
        auto& buf = sinkStream_->data[c];
        const size_t byte = (size_t)(pos >> 3);
        if (byte >= buf.size()) buf.resize(byte + 1, 0);
        const uint8_t mask = (uint8_t)(0x80u >> (pos & 7u));
        if (v) buf[byte] = (uint8_t)(buf[byte] | mask);
        else   buf[byte] = (uint8_t)(buf[byte] & (uint8_t)~mask);
    }

    void collect_mod_stats() {
        st_.overload = 0; st_.clamped = 0; st_.nonFinite = 0;
        for (auto& m : mod_) {
            st_.overload += m->overload();
            st_.clamped += m->clamped();
            st_.nonFinite += m->nonFinite();
        }
    }

    void copy_bits(const rta::audio::DsdStream& in, rta::audio::DsdStream& out) {
        if (in.channels != ch_) throw std::runtime_error("DsdRateConverterScalarReference::copy_bits channel mismatch");
        if (sinkStream_ != &out) { sinkStream_ = &out; align_sink(out); }
        // process_bits appends. Replacing the destination here silently discarded
        // every earlier chunk when convert_dsd_stream took the Direct branch.
        for (int c = 0; c < ch_; ++c) {
            if ((outBits_ & 7) == 0) {
                const size_t start = (size_t)outBits_ / 8;
                const size_t bytes = (size_t)((in.bits + 7) / 8);
                out.data[(size_t)c].resize(start + bytes);
                std::copy_n(in.data[(size_t)c].begin(), bytes, out.data[(size_t)c].begin() + start);
            } else {
                for (uint64_t n = 0; n < in.bits; ++n)
                    put_bit((uint64_t)outBits_ + n, (size_t)c,
                            ((in.data[(size_t)c][(size_t)(n >> 3)] >> (7 - (n & 7))) & 1u) != 0);
            }
        }
        outBits_ += (long long)in.bits;
        out.bits = (uint64_t)outBits_;
        st_.outBits = outBits_;
    }

    DsdRatePlan plan_;
    DsdRateConvertConfig cfg_;
    int ch_ = 1;
    double gainDb_ = 0.0;
    double gain_ = 1.0;
    int tapsPerPhase_ = 0;
    double passbandHz_ = 20000.0;
    long long outBits_ = 0;
    rta::audio::DsdStream* sinkStream_ = nullptr;
    DsdRateStats st_;
    size_t skipLeft_ = 0;
    bool headConfigured_ = false, finished_ = false, dsdInput_ = false;
    std::vector<std::unique_ptr<DsdDecimator>> dec_;
    std::unique_ptr<PolyphaseResampler> rs_;
    std::vector<std::unique_ptr<SdmModulator>> mod_;
};
} // namespace rta::dsp
