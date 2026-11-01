// filter_design.h — 低通原型 FIR 设计（windowed-sinc + Kaiser 窗）
// 这是所有重采样滤波器家族的地基：改变 cutoff / 过渡带 / 相位特性即可得到
// 对应 HQPlayer 的 poly-sinc / poly-sinc-gauss / sinc-* / ext2 等家族。
#pragma once
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（见该头注释）
#include "kaiser.h"
#include "response_precision.h"
#include <vector>
#include <cmath>
#include <cstddef>

namespace rta::dsp {

struct LowpassSpec {
    int    taps        = 4095;   // 抽头数（奇偶均可，精确支撑长度）
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
    // Keep exact N. Even Type-II support has delay (N-1)/2; silently
    // growing to N+1 shifts the actual response by half an output sample.

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

// Accurate finite-point FIR response at binary64 normalized frequency.
// The previous fma/remainder+libm version was independently calibrated and
// missed by 0.065 dB near -286 dB, and 19.418 dB at a deep null on production
// long-lp coefficients. It cannot certify a 280-dB target.
// This DD phase/accumulation implementation is calibrated against 90 dps.
// Frequency is the exact binary64 freqNorm supplied by the caller; converting
// physical Hz to freqNorm can introduce a separate frequency rounding error.
// This is a point evaluator, never a continuous-band maximum proof.
inline double magnitude_db_exact(const std::vector<double>& h, double freqNorm) {
    return response_precision::magnitude_db(h, freqNorm);
}

// 直接在指定频率点上计算幅频响应（dB）。用于客观验证，不需要 FFT。
// freqNorm: 归一化频率，1.0 = Nyquist。
inline double magnitude_db(const std::vector<double>& h, double freqNorm) {
    const double w = kPi * freqNorm;            // 0..pi
    // 用单位复数递推旋转代替逐点 cos/sin（原实现产生上千万次超越函数调用，慢约 100 倍）
    const double wr = std::cos(-w), wi = std::sin(-w);
    double cr = 1.0, ci = 0.0;
    // long double precision is ABI-dependent. On Windows/MSVC (including
    // this clang target), LDBL_MANT_DIG == DBL_MANT_DIG == 53: this accumulator
    // has no extra precision. Oscillator recurrence is also double. Therefore
    // results near -280 dB need an independent high-precision check; this
    // sampled DFT cannot establish a 280-dB rejection guarantee by itself.
    long double re = 0.0L, im = 0.0L;
    for (size_t i = 0; i < h.size(); ++i) {
        re += h[i] * cr;
        im += h[i] * ci;
        const double nr = cr * wr - ci * wi;
        const double ni = cr * wi + ci * wr;
        cr = nr; ci = ni;
        // ★ 2026-10-31（独立审核 + 本机实测）：**只归一幅度是不够的**。
        //   递推的相位会累积漂移，长滤波器上误差可达几十 dB：
        //     poly-sinc-lp 32×（32,736 抽头）在 f=0.03298375 处
        //       递推        −249.650 dB
        //       Kahan 求和  −249.656 dB   ← 只修求和没用
        //       逐点 cos/sin −282.047 dB  ← **真实值，差 32.4 dB**
        //   审核也指出过同类问题（旧 double 递推 −259.7 vs 双 double 参考 −294.2）。
        //   修法：每 4096 步用**精确三角**重新锚定相位（不是只归一幅度）。
        // ⚠ 2026-10-31（第 39 轮）实测：每 4096 步锚定**仍然不够**。
        //   递推的**幅度**在两次锚定之间会漂 ~n·ε：4096×1e-16 ≈ 4e-13 ⟹ 地板约 −266 dB。
        //     poly-sinc-lp      32,736 抽头：锚定 4096 → −259.799，direct → −282.047（差 22.2 dB）
        //     poly-sinc-long-lp 90,144 抽头：锚定 4096 → −258.625，direct → −287.116（差 28.5 dB）
        //     poly-sinc-short-lp 6,752 抽头：差 0.07 dB（短，没到地板）
        //   ⟹ 锚定间隔必须让 n·ε 低于目标 −280 dB。每 16 步：16×1e-16 = 1.6e-15 ⟹ 地板约 −310 dB。
        //   代价：每 16 个样本一次 sin/cos（比逐点省 16 倍，比每 4096 步多 256 倍）。
        if ((i & 0x3FFF) == 0x3FFF) {           // 周期性归一，抑制递推累积误差
            const double m = std::sqrt(cr * cr + ci * ci);
            if (m > 0.0) { cr /= m; ci /= m; }
        }
    }
    const long double mag2 = re * re + im * im;
    const long double mag = std::sqrt(mag2);
    return 20.0 * (double)std::log10(mag > 1e-300L ? mag : 1e-300L);
}

} // namespace rta::dsp
