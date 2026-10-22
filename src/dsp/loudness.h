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

class Loudness {
public:
    explicit Loudness(const LoudnessConfig& c = {}) : cfg_(c) {}

    void init(double sampleRate) { fs_ = sampleRate > 0 ? sampleRate : 48000.0; dirty_ = true; }

    // 依当前音量算出实际提升量并重建滤波器系数
    void set_volume_db(double v) {
        if (std::fabs(v - volDb_) < 1e-9) return;
        volDb_ = v;
        dirty_ = true;
    }

    // 插值系数：0 = 满提升（音量 <= 下界），1 = 无提升（音量 >= 上界）
    double scale() const {
        const double span = cfg_.upperBoundDb - cfg_.lowerBoundDb;
        if (span <= 1e-12) return 1.0;
        const double t = (volDb_ - cfg_.lowerBoundDb) / span;
        return std::clamp(t, 0.0, 1.0);
    }
    double bass_db()   const { return cfg_.bassMaxDb   * (1.0 - scale()); }
    double treble_db() const { return cfg_.trebleMaxDb * (1.0 - scale()); }

    void prepare() {
        if (!dirty_) return;
        const double bd = bass_db(), td = treble_db();
        if (bd > 1e-9) { Biquad q; biquad_design::lowShelf(q, fs_, cfg_.bassFreq, cfg_.bassSlope, bd);  bass_ = q; }
        else           { bass_ = Biquad{}; }
        if (td > 1e-9) { Biquad q; biquad_design::highShelf(q, fs_, cfg_.trebleFreq, cfg_.trebleSlope, td); treble_ = q; }
        else           { treble_ = Biquad{}; }
        dirty_ = false;
    }

    // 单声道就地处理
    void process(double* x, size_t n) {
        prepare();
        if (bass_db() <= 1e-9 && treble_db() <= 1e-9) {
            // 无提升时旁路 —— 保证 0 dB 时逐位不变
            return;
        }
        for (size_t i = 0; i < n; ++i) x[i] = treble_.process(bass_.process(x[i]));
    }

    // 清空滤波器状态。注意：Biquad 把系数与延迟状态存在一起，
    // 直接赋单位滤波器会连【系数】一起清掉 —— 必须置 dirty_ 让 prepare() 重建，
    // 否则滤波器会被永久旁路（实测：reset 后所有频点增益都变成 0.00 dB）。
    void reset() { dirty_ = true; bass_ = Biquad{}; treble_ = Biquad{}; }

private:
    LoudnessConfig cfg_;
    double fs_ = 48000.0, volDb_ = 0.0;
    bool dirty_ = true;
    Biquad bass_, treble_;
};

} // namespace rta::dsp
