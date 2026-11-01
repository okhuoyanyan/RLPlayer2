// volume.h — 音量控制
//
// 设计要点（对应手册 4.2 “Volume control”）：
//   * 0 dB 必须【逐位精确】：增益恰为 1.0 时不得引入任何数值扰动
//   * 增益变化必须渐变（ramp），否则会产生咔哒声
//   * 手册的 “Fixed volume”：勾选 ≈ −3 dB，灰显 ≈ −6 dB，给 inter-sample 过冲留余量
//   * 本模块不做抖动/整形：管线末端的 Quantizer 才是正确的抖动注入点
//     （在音量之后、量化之前注入，才能让抖动真正作用于最终量化误差）
#pragma once
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（输出进入音频样本）
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

    // ★ 2026-10-29（两轮审查 F01）：起播设定 = 「设目标 + 立刻到位」的显式组合。
    //   为什么必须是一个具名操作：旧 prepare_stream() 只调 set_gain_db()，而 current_
    //   初值是 1.0（单位增益），set_gain_db() 只启动一个从 1.0 降到目标的 200 ms 渐变
    //   ⟹ 用户选 −60 dB 时，开头 200 ms 实际是**满幅**播放（首样本 0.499947969 vs
    //   目标 0.0005，差 59.9991 dB）。这是起播电平事故（铁律 1 的直接来源），不是听感偏好。
    //   起播用 snap 到位；播放中的拖动仍走 set_gain_db() 的渐变（防咔哒）。
    void set_gain_db_snapped(double db) { set_gain_db(db); snap(); }

    double gain_db() const { return db_; }
    double current_gain() const { return current_; }
    bool   ramping() const { return remaining_ > 0; }
    size_t ramp_samples() const { return rampSamples_; }

    // 推进一帧渐变，返回本帧应使用的公共增益
    double step_frame() {
        if (remaining_ > 0) {
            const double step = (target_ - current_) / (double)remaining_;
            current_ += step;
            --remaining_;
            if (remaining_ == 0) current_ = target_;
        }
        return current_;
    }

    double process(double x) {
        const double g = step_frame();
        // 0 dB 直通：乘 1.0 是精确的，但为绝对保证逐位一致，这里显式短路
        if (g == 1.0) return x;
        return x * g;
    }

    void process(double* buf, size_t n) {
        for (size_t i = 0; i < n; ++i) buf[i] = process(buf[i]);
    }

    // ★ 2026-10-29（两轮审查 F01）：多声道必须【按帧】推进一次公共增益。
    //   旧实现是 for (auto& w : work) vol->process(w.data(), w.size()) —— 同一个有状态
    //   对象先跑完整块左声道、再跑右声道，每个样本都推进一次计数：
    //     · 右声道拿到的是渐变的「后半段」⟹ 同一帧左右增益不同（声像会飘）；
    //     · 渐变长度按【样本数】而不是【帧数】计 ⟹ 2 声道 200 ms 变 400 ms，
    //       6 声道变 1200 ms（「单/双/多声道渐变墙钟长度一致」不成立）。
    //   现在：一帧只推进一次，同一帧的全部声道用同一个增益。
    void process_planes(double* const* planes, int channels, size_t frames) {
        if (!planes || channels <= 0 || frames == 0) return;
        // 0 dB 且无渐变：一个样本都不碰（逐位直通的最强保证）
        if (remaining_ == 0 && current_ == 1.0) return;
        // 完全静音：直接写 0（避免 x*0 把 −0.0/NaN 留在缓冲里）
        if (remaining_ == 0 && current_ == 0.0) {
            for (int c = 0; c < channels; ++c)
                if (planes[c]) std::fill(planes[c], planes[c] + frames, 0.0);
            return;
        }
        for (size_t i = 0; i < frames; ++i) {
            const double g = step_frame();
            for (int c = 0; c < channels; ++c) {
                double* p = planes[c];
                if (!p) continue;
                p[i] = (g == 1.0) ? p[i] : p[i] * g;
            }
        }
    }

    // 交错缓冲版本（互操作 / 测试用；语义与 process_planes 完全一致）
    void process_interleaved(double* buf, int channels, size_t frames) {
        if (!buf || channels <= 0 || frames == 0) return;
        if (remaining_ == 0 && current_ == 1.0) return;
        if (remaining_ == 0 && current_ == 0.0) {
            std::fill(buf, buf + frames * (size_t)channels, 0.0);
            return;
        }
        for (size_t i = 0; i < frames; ++i) {
            const double g = step_frame();
            double* f = buf + i * (size_t)channels;
            for (int c = 0; c < channels; ++c) f[c] = (g == 1.0) ? f[c] : f[c] * g;
        }
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
