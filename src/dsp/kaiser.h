// kaiser.h — Kaiser 窗与零阶修正贝塞尔函数 I0
// 用于设计高质量 windowed-sinc 低通原型滤波器（HQPlayer poly-sinc 家族的同族做法）
#pragma once
#include <cmath>
#include <vector>

namespace rta::dsp {

// 显式定义 pi（clang/Windows 下 kPi 默认不暴露）
inline constexpr double kPi = 3.14159265358979323846264338327950288;

// 零阶修正贝塞尔函数 I0(x)，级数展开，精度足以覆盖 beta <= 20
inline double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    const double half = x * 0.5;
    for (int k = 1; k < 64; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// 由阻带衰减(dB)反推 Kaiser beta（Kaiser 经验公式）
inline double kaiser_beta_for_attenuation(double attenDb) {
    if (attenDb > 50.0)
        return 0.1102 * (attenDb - 8.7);
    if (attenDb >= 21.0)
        return 0.5842 * std::pow(attenDb - 21.0, 0.4) + 0.07886 * (attenDb - 21.0);
    return 0.0;
}

// 由阻带衰减与过渡带宽度(归一化到采样率)估算所需抽头数
inline int kaiser_taps_for(double attenDb, double transitionNorm) {
    double n = (attenDb - 8.0) / (2.285 * 2.0 * kPi * transitionNorm);
    int taps = (int)std::ceil(n) + 1;
    if (taps < 3) taps = 3;
    if ((taps & 1) == 0) ++taps;   // 取奇数，保证整数群延迟
    return taps;
}

// Kaiser 窗：长度 n（n>=1），0 <= i <= n-1，对称
inline std::vector<double> kaiser_window(int n, double beta) {
    std::vector<double> w((size_t)n);
    if (n == 1) { w[0] = 1.0; return w; }
    const double denom = bessel_i0(beta);
    const double m = (double)(n - 1);
    for (int i = 0; i < n; ++i) {
        double r = (2.0 * i - m) / m;          // [-1, 1]
        double a = 1.0 - r * r;
        w[(size_t)i] = (a <= 0.0) ? 0.0 : bessel_i0(beta * std::sqrt(a)) / denom;
    }
    return w;
}

} // namespace rta::dsp
