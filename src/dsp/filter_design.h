// filter_design.h — 低通原型 FIR 设计（windowed-sinc + Kaiser 窗）
// 这是所有重采样滤波器家族的地基：改变 cutoff / 过渡带 / 相位特性即可得到
// 对应 HQPlayer 的 poly-sinc / poly-sinc-gauss / sinc-* / ext2 等家族。
#pragma once
#include "kaiser.h"
#include <vector>
#include <cmath>
#include <cstddef>

namespace rta::dsp {

struct LowpassSpec {
    int    taps        = 4095;   // 抽头数（奇数）
    double cutoff      = 0.5;    // 归一化截止（1.0 = Nyquist，即采样率的一半）
    double attenDb     = 180.0;  // 目标阻带衰减
    bool   minimumPhase= false;  // false=线性相位；true=最小相位（后续用倒谱法实现）
};

// 设计线性相位低通原型。
// cutoff 以 Nyquist 为单位：0.5 表示 0.25*fs（即常见的"半带"以外的常规截止）。
// 返回值已归一化到单位直流增益。
inline std::vector<double> design_lowpass(const LowpassSpec& spec) {
    int n = spec.taps;
    if (n < 3) n = 3;
    if ((n & 1) == 0) ++n;                       // 强制奇数

    const double beta = kaiser_beta_for_attenuation(spec.attenDb);
    const auto w = kaiser_window(n, beta);

    // sinc 在"以采样率为单位"的频域：截止频率 fc(cycles/sample) = cutoff * 0.5
    const double fc = spec.cutoff * 0.5;
    const double m  = (double)(n - 1) * 0.5;

    std::vector<double> h((size_t)n);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        double x = (double)i - m;
        double s = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * x) / (kPi * x);
        h[(size_t)i] = s * w[(size_t)i];
        sum += h[(size_t)i];
    }
    if (sum != 0.0)
        for (auto& v : h) v /= sum;              // 单位直流增益
    return h;
}

// 直接在指定频率点上计算幅频响应（dB）。用于客观验证，不需要 FFT。
// freqNorm: 归一化频率，1.0 = Nyquist。
inline double magnitude_db(const std::vector<double>& h, double freqNorm) {
    const double w = kPi * freqNorm;            // 0..pi
    // 用单位复数递推旋转代替逐点 cos/sin（原实现产生上千万次超越函数调用，慢约 100 倍）
    const double wr = std::cos(-w), wi = std::sin(-w);
    double cr = 1.0, ci = 0.0;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < h.size(); ++i) {
        re += h[i] * cr;
        im += h[i] * ci;
        const double nr = cr * wr - ci * wi;
        const double ni = cr * wi + ci * wr;
        cr = nr; ci = ni;
        if ((i & 0x3FFF) == 0x3FFF) {           // 周期性归一，抑制递推累积误差
            const double m = std::sqrt(cr * cr + ci * ci);
            if (m > 0.0) { cr /= m; ci /= m; }
        }
    }
    double mag = std::sqrt(re * re + im * im);
    return 20.0 * std::log10(mag > 1e-300 ? mag : 1e-300);
}

} // namespace rta::dsp
