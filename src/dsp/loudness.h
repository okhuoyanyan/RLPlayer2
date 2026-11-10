// loudness.h — 音量自适应等响度补偿（手册 4.2 Loudness）
//
// 手册原文：
//   "Loudness is a volume-adaptive loudness control with adjustable parameters.
//    For bass and treble controls, the corner frequency, slope factor and level can be
//    adjusted (Parameter 1 to Parameter 6 respectively). Lower bound (Parameter 7) is
//    volume setting where at or below, set maximum loudness value is reached.
//    Higher bound (Parameter 8) is volume setting where at and above, loudness value
//    reaches 0 dB."
//
// 物理动机：人耳的等响曲线（ISO 226）在小音量下对低频与极高频的敏感度显著下降。
// 音量拧小时若不补偿，听感会"变薄"。Loudness 就是按当前音量补回这部分。
//
// 实现：低音用低架滤波器、高音用高架滤波器；提升量按音量在
// [下界, 上界] 之间线性插值（下界及以下取满提升，上界及以上为 0 dB）。
#pragma once
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（输出进入音频样本）
#include "matrix.h"
#include <algorithm>
#include <cmath>

namespace rta::dsp {

struct LoudnessConfig {
    double bassFreq    = 100.0;   // P1 低音角频率 (Hz)
    double bassSlope   = 0.7;     // P2 低音斜率因子
    double bassMaxDb   = 6.0;     // P3 低音最大提升 (dB)
    double trebleFreq  = 6000.0;  // P4 高音角频率 (Hz)
    double trebleSlope = 0.7;     // P5 高音斜率因子
    double trebleMaxDb = 3.0;     // P6 高音最大提升 (dB)
    double lowerBoundDb= -30.0;   // P7 下界：此音量及以下取满提升
    double upperBoundDb=   0.0;   // P8 上界：此音量及以上为 0 dB
};

// 给定配置与听音音量，实际会施加的最大提升量（dB，0 = 完全旁路）。
// ★ 2026-10-29（两轮审查 F21 顺带）：管线判「是否逐位直通」时必须在**不构造对象**的前提下
//   知道 Loudness 到底动不动样本 —— 音量 >= 上界时提升为 0、process() 直接 return，
//   这时 loudness 是纯旁路（可以逐位直通）；音量低时必须判成"有处理"。
//   旧 hasAnyProcessing() 完全没看 loudnessEnabled ⟹ 开着等响度补偿也能报 bit-perfect=yes。
inline double loudness_effect_db(const LoudnessConfig& c, double volDb) {
    const double span = c.upperBoundDb - c.lowerBoundDb;
    double t = (span <= 1e-12) ? 1.0 : (volDb - c.lowerBoundDb) / span;
    t = std::clamp(t, 0.0, 1.0);
    const double mx = std::max(c.bassMaxDb, c.trebleMaxDb);
    const double v = mx * (1.0 - t);
    return (v > 0.0) ? v : 0.0;
}

class Loudness {
public:
    explicit Loudness(const LoudnessConfig& c = {}) : cfg_(c) {}

    void init(double sampleRate) {
        fs_ = sampleRate > 0 ? sampleRate : 48000.0;
        rampSamples_ = std::max<size_t>(1, (size_t)std::llround(fs_ * 0.020));
        dirty_ = true;
    }

    // Called only on the processing thread. Initial setup is immediate; live
    // changes take 20 ms, preserving the input/output history of both shelves.
    void set_volume_db(double v) {
        if (!std::isfinite(v) || std::fabs(v - volDb_) < 1e-9) return;
        volDb_ = v;
        if (!processed_) { currentDb_ = v; remaining_ = 0; }
        else remaining_ = rampSamples_;
        dirty_ = true;
    }

    double scale() const { return scale_for(volDb_); }
    double bass_db() const { return cfg_.bassMaxDb * (1.0 - scale()); }
    double treble_db() const { return cfg_.trebleMaxDb * (1.0 - scale()); }
    bool active() const {
        return remaining_ != 0 || loudness_effect_db(cfg_, currentDb_) > 1e-9 ||
               bass_.settling() || treble_.settling();
    }

    void prepare() {
        if (!dirty_) return;
        const double k = 1.0 - scale_for(currentDb_);
        bass_.design(fs_, cfg_.bassFreq, cfg_.bassSlope, cfg_.bassMaxDb * k, false);
        treble_.design(fs_, cfg_.trebleFreq, cfg_.trebleSlope, cfg_.trebleMaxDb * k, true);
        dirty_ = false;
    }

    void process(double* x, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (remaining_) {
                currentDb_ += (volDb_ - currentDb_) / (double)remaining_;
                if (--remaining_ == 0) currentDb_ = volDb_;
                // Update every 16 frames (plus the final frame), independent
                // of caller chunking. Direct-form I keeps physical x/y history
                // when coefficients change, rather than resetting a Biquad.
                if ((tick_ & 15u) == 0 || remaining_ == 0) dirty_ = true;
            }
            prepare();
            x[i] = treble_.process(bass_.process(x[i]));
            ++tick_;
        }
        if (n) processed_ = true;
    }

    void reset() {
        bass_.reset(); treble_.reset();
        currentDb_ = volDb_; remaining_ = 0; tick_ = 0;
        processed_ = false; dirty_ = true;
    }

private:
    // RBJ shelves, same design as matrix.h. Direct-form I makes coefficient
    // updates safe without clearing the signal history. At exactly 0 dB the
    // exact identity preserves input bits after existing corrections decay.
    struct Shelf {
        double b0=1, b1=0, b2=0, a1=0, a2=0;
        double x1=0, x2=0, y1=0, y2=0;
        bool identity = true;
        void reset() { x1=x2=y1=y2=0; }
        void design(double fs, double f, double slope, double db, bool high) {
            identity = db <= 1e-9;
            const double A=std::pow(10.0, db/40.0);
            const double w=2.0*kPi*f/fs, cw=std::cos(w), sw=std::sin(w);
            const double alpha=sw/2.0*std::sqrt((A+1.0/A)*(1.0/slope-1.0)+2.0);
            const double t=2.0*std::sqrt(A)*alpha;
            double aa0;
            if (high) {
                aa0=(A+1)-(A-1)*cw+t;
                b0=A*((A+1)+(A-1)*cw+t)/aa0;
                b1=-2*A*((A-1)+(A+1)*cw)/aa0;
                b2=A*((A+1)+(A-1)*cw-t)/aa0;
                a1=2*((A-1)-(A+1)*cw)/aa0;
                a2=((A+1)-(A-1)*cw-t)/aa0;
            } else {
                aa0=(A+1)+(A-1)*cw+t;
                b0=A*((A+1)-(A-1)*cw+t)/aa0;
                b1=2*A*((A-1)-(A+1)*cw)/aa0;
                b2=A*((A+1)-(A-1)*cw-t)/aa0;
                a1=-2*((A-1)+(A+1)*cw)/aa0;
                a2=((A+1)+(A-1)*cw-t)/aa0;
            }
        }
        bool settling() const {
            return identity && (std::fabs(x1-y1) + std::fabs(x2-y2) > 1e-14);
        }
        double process(double x) {
            // A live move to 0 dB must retain the unity filter's poles until
            // its previous correction has decayed. Immediate identity would
            // discard that history and create an end-of-ramp click.
            const double y=(identity && !settling()) ? x : b0*x+b1*x1+b2*x2-a1*y1-a2*y2;
            x2=x1; x1=x; y2=y1; y1=y;
            return y;
        }
    };
    double scale_for(double db) const {
        const double span=cfg_.upperBoundDb-cfg_.lowerBoundDb;
        if (span <= 1e-12) return 1.0;
        return std::clamp((db-cfg_.lowerBoundDb)/span, 0.0, 1.0);
    }
    LoudnessConfig cfg_;
    double fs_=48000.0, volDb_=0.0, currentDb_=0.0;
    size_t rampSamples_=960, remaining_=0, tick_=0;
    bool dirty_=true, processed_=false;
    Shelf bass_, treble_;
};

} // namespace rta::dsp
