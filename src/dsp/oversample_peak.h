// oversample_peak.h — 过采样峰值（inter-sample over）检测
//
// 为什么需要它：数字满幅在【采样点】上，DAC 重建出来的模拟波形却可能超过满幅。
// 升频时插值出来的中间值同理。这类峰值会让后级削波 —— 听感上是"发硬、发毛"，
// 但看采样值完全正常，很容易被忽略。
//
// 做法：用 8 抽头窗 sinc 在每两个采样之间插值 4 个点，取全局最大绝对值。
// 只做检测，不做修改；要不要衰减由调用方决定（见 pipeline 的 preAttenDb）。
#pragma once
#include <cmath>
#include <cstddef>
#include <vector>
#include <algorithm>

namespace rta::dsp {

struct IspReport {
    double peak = 0.0;        // 线性峰值（1.0 = 满幅）
    double peakDb = -999.0;
    long long overCount = 0;  // 超过满幅的插值点数
    size_t atFrame = 0;
    int    atCh = 0;
    bool   scanned = false;
};

// 4 相 × 8 抽头的插值核（Kaiser 窗 sinc，截止 0.5），
// 相 0 必须退化成 delta（保证"不插值时与原始采样一致"这一条不变）
inline const std::vector<std::vector<double>>& isp_kernel() {
    static std::vector<std::vector<double>> k = [] {
        const int TAPS = 8, PH = 4;
        std::vector<std::vector<double>> t((size_t)PH, std::vector<double>((size_t)TAPS, 0.0));
        for (int p = 0; p < PH; ++p) {
            const double frac = (double)p / (double)PH;
            double sum = 0.0;
            for (int n = 0; n < TAPS; ++n) {
                // 位置：n - (TAPS/2 - 1) - frac
                const double x = (double)n - (double)(TAPS / 2 - 1) - frac;
                double s;
                if (std::fabs(x) < 1e-12) s = 1.0;
                else { const double a = 3.14159265358979323846 * x; s = std::sin(a) / a; }
                // Kaiser(beta=6) 窗
                const double r = (x + (double)(TAPS / 2)) / (double)TAPS;   // 0..1
                const double w = (r <= 0.0 || r >= 1.0) ? 0.0
                               : [](double rr) {
                                     const double beta = 6.0, t2 = 2.0 * rr - 1.0;
                                     const double v = 1.0 - t2 * t2;
                                     double num = 0.0, den = 0.0;
                                     // I0 级数展开（beta 小，前 12 项足够）
                                     double f = 1.0, s1 = 0.0;
                                     for (int m = 0; m < 14; ++m) { s1 += f; f *= (beta * beta / 4.0) / ((double)(m + 1) * (double)(m + 1)); }
                                     num = s1; den = 1.0;
                                     double g = 1.0, s2 = 0.0;
                                     const double z = beta * std::sqrt(std::max(0.0, v));
                                     for (int m = 0; m < 14; ++m) { s2 += g; g *= (z * z / 4.0) / ((double)(m + 1) * (double)(m + 1)); }
                                     return s2 / num;
                                 }(r);
                t[(size_t)p][(size_t)n] = s * w;
                sum += s * w;
            }
            if (sum != 0.0) for (auto& v : t[(size_t)p]) v /= sum;   // 直流增益归一
        }
        // 相 0 强制成 delta，避免引入任何偏差
        for (int n = 0; n < TAPS; ++n) t[0][(size_t)n] = (n == TAPS / 2 - 1) ? 1.0 : 0.0;
        return t;
    }();
    return k;
}

inline IspReport detect_isp(const std::vector<std::vector<double>>& planes, IspReport rep = {}) {
    const auto& K = isp_kernel();
    const int TAPS = (int)K[0].size();
    const int half = TAPS / 2 - 1;
    for (size_t c = 0; c < planes.size(); ++c) {
        const auto& x = planes[c];
        const long long n = (long long)x.size();
        for (long long i = 0; i < n; ++i) {
            for (int p = 0; p < (int)K.size(); ++p) {
                double acc = 0.0;
                for (int k = 0; k < TAPS; ++k) {
                    // 边界用"复制端点"而不是补零：补零会让开头/结尾凭空多出过冲
                    //（实测：常量 0.5 在文件头会被报成 0.5548，属于假警报）
                    long long idx = i + k - half;
                    if (idx < 0) idx = 0;
                    if (idx >= n) idx = n - 1;
                    acc += K[(size_t)p][(size_t)k] * x[(size_t)idx];
                }
                const double a = std::fabs(acc);
                if (a > rep.peak) { rep.peak = a; rep.atFrame = (size_t)i; rep.atCh = (int)c; }
                if (a > 1.0) ++rep.overCount;
            }
        }
    }
    rep.peakDb = rep.peak > 0.0 ? 20.0 * std::log10(rep.peak) : -999.0;
    rep.scanned = true;
    return rep;
}

} // namespace rta::dsp
