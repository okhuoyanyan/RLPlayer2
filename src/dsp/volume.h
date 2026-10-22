// volume.h — 音量控制
//
// 设计要点（对应手册 4.2 “Volume control”）：
//   * 0 dB 必须【逐位精确】：增益恰为 1.0 时不得引入任何数值扰动
//   * 增益变化必须渐变（ramp），否则会产生咔哒声
//   * 手册的 “Fixed volume”：勾选 ≈ −3 dB，灰显 ≈ −6 dB，给 inter-sample 过冲留余量
//   * 本模块不做抖动/整形：管线末端的 Quantizer 才是正确的抖动注入点
//     （在音量之后、量化之前注入，才能让抖动真正作用于最终量化误差）
#pragma once
#include "kaiser.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace rta::dsp {

class VolumeControl {
public:
    void set_sample_rate(double fs) { fs_ = fs > 0 ? fs : 44100.0; update_ramp(); }

    // 目标增益（dB）。−144 dB 及以下视为静音。
    // 注意：必须在这里【重新启动渐变】，否则剩余渐变长度为 0，增益会瞬时跳变
    // ⟹ 实际播放中产生咔哒声。（本 bug 由 test_volume_eq 抓到）
    void set_gain_db(double db) {
        db_ = db;
        target_ = (db <= -144.0) ? 0.0 : std::pow(10.0, db / 20.0);
        remaining_ = rampSamples_;
        if (rampSamples_ == 0) current_ = target_;
    }

    void set_ramp_ms(double ms) { rampMs_ = ms; update_ramp(); }

    // 立即跳到目标增益（用于开始播放前设定，避免开头一段渐变）
    void snap() { current_ = target_; remaining_ = 0; }

    double gain_db() const { return db_; }
    double current_gain() const { return current_; }

    double process(double x) {
        if (remaining_ > 0) {
            const double step = (target_ - current_) / (double)remaining_;
            current_ += step;
            --remaining_;
            if (remaining_ == 0) current_ = target_;
        }
        // 0 dB 直通：乘 1.0 是精确的，但为绝对保证逐位一致，这里显式短路
        if (current_ == 1.0) return x;
        return x * current_;
    }

    void process(double* buf, size_t n) {
        for (size_t i = 0; i < n; ++i) buf[i] = process(buf[i]);
    }

    // 手册 4.2：固定音量的两档
    static constexpr double fixed_volume_grayed_db()  { return -3.0; }   // 灰显
    static constexpr double fixed_volume_checked_db() { return -6.0; }   // 勾选

private:
    void update_ramp() {
        rampSamples_ = (size_t)std::llround(rampMs_ * fs_ / 1000.0);
        remaining_ = rampSamples_;
        if (rampSamples_ == 0) current_ = target_;
    }
    double fs_ = 44100.0;
    double db_ = 0.0;
    double target_ = 1.0, current_ = 1.0;
    double rampMs_ = 200.0;
    size_t rampSamples_ = 0, remaining_ = 0;
};

// 相对满幅峰值 → dBFS，用于 intersample 余量检查
inline double peak_dbfs(const std::vector<double>& x) {
    double p = 0.0;
    for (double v : x) p = std::max(p, std::fabs(v));
    return 20.0 * std::log10(p + 1e-300);
}

} // namespace rta::dsp
