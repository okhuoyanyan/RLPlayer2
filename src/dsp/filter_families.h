// filter_families.h — 滤波器家族：窗函数、相位类型、预设表
//
// 对应 HQPlayer 手册 4.6 的滤波家族结构：
//   * 窗函数决定"家族性格"：Kaiser（poly-sinc 系）、Gaussian（gauss 系）、
//     Blackman-Harris（closed-form / sinc 系高衰减）、Hann（温和）
//   * 相位类型决定"时间行为"：线性相位（对称振铃，前后都有）vs
//     最小相位（无前振铃，能量集中在脉冲起始处）—— 即 -lp / -mp 后缀
//   * 抽头数与过渡带决定"陡峭度"：short / medium / long / xl / sinc-M（百万抽头）
//
// 最小相位实现采用**倒谱法（cepstral / homomorphic）**：
//   线性相位原型 H → 实倒谱 c[n] → 折叠（因果部分加倍、反因果置零）→ 指数还原 → IFFT
//   这是严格的最小相位分解，不是"把脉冲右移"之类的近似。
#pragma once
#include "kaiser.h"
#include "fft.h"
#include "filter_design.h"   // magnitude_db
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#include <cstddef>

namespace rta::dsp {

enum class WindowKind { Kaiser, Gaussian, BlackmanHarris, Hann };
// Linear = 线性相位（对称、有前振铃）；Minimum = 最小相位（能量靠前、无预振铃）；
// Intermediate = 混合相位：两条同幅频响应的冲激响应按比例混合。
// 两条的幅频响应本来就相同（倒谱法只改相位），所以混合后幅频不变、群延迟落在两者之间。
enum class PhaseKind  { Linear, Minimum, Intermediate };

inline const char* window_name(WindowKind w) {
    switch (w) {
        case WindowKind::Kaiser:         return "kaiser";
        case WindowKind::Gaussian:       return "gauss";
        case WindowKind::BlackmanHarris: return "bharris";
        case WindowKind::Hann:           return "hann";
    }
    return "?";
}
inline const char* phase_name(PhaseKind p) {
    return p == PhaseKind::Linear ? "lp" : (p == PhaseKind::Minimum ? "mp" : "mix");
}

// 混合相位的默认比例：0 = 全最小相位，1 = 全线性相位
inline double& phase_mix_ref() { static double v = 0.5; return v; }
inline void   set_phase_mix(double v) { phase_mix_ref() = (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v)); }
inline double phase_mix() { return phase_mix_ref(); }

// ---------------- 窗函数 ----------------
inline std::vector<double> make_window(WindowKind kind, int n, double attenDb) {
    std::vector<double> w((size_t)n, 1.0);
    if (n <= 1) return w;
    const double m = (double)(n - 1);
    switch (kind) {
        case WindowKind::Kaiser:
            return kaiser_window(n, kaiser_beta_for_attenuation(attenDb));
        case WindowKind::Gaussian: {
            // 高斯窗无旁瓣结构、单调滚降；alpha 越大衰减越深、过渡带越宽
            const double alpha = 3.0 + 0.011 * std::max(0.0, attenDb - 60.0);
            for (int i = 0; i < n; ++i) {
                const double r = (2.0 * i - m) / m;
                w[(size_t)i] = std::exp(-0.5 * (alpha * r) * (alpha * r));
            }
            return w;
        }
        case WindowKind::BlackmanHarris:
            for (int i = 0; i < n; ++i) {
                const double t = 2.0 * kPi * i / m;
                w[(size_t)i] = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
            }
            return w;
        case WindowKind::Hann:
            for (int i = 0; i < n; ++i) w[(size_t)i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / m);
            return w;
    }
    return w;
}

// ---------------- 原型设计（线性相位）----------------
struct FirSpec {
    int        taps       = 4095;
    double     cutoff     = 0.5;      // 归一化：1.0 = Nyquist
    double     attenDb    = 180.0;
    WindowKind window     = WindowKind::Kaiser;
    bool       apodizing  = false;
    double     apodF1     = 0.0;      // 归一化滚降起点（1.0 = Nyquist）
    double     apodF2     = 0.0;      // 归一化滚降终点（1.0 = Nyquist）
};

inline std::vector<double> design_fir(const FirSpec& s) {
    int n = s.taps; if (n < 3) n = 3; if ((n & 1) == 0) ++n;
    const auto w = make_window(s.window, n, s.attenDb);
    const double m  = (double)(n - 1) * 0.5;
    std::vector<double> h((size_t)n);
    double sum = 0.0;
    if (!s.apodizing) {
        const double fc = s.cutoff * 0.5;
        for (int i = 0; i < n; ++i) {
            const double x = (double)i - m;
            const double sc = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * x) / (kPi * x);
            h[(size_t)i] = sc * w[(size_t)i];
            sum += h[(size_t)i];
        }
    } else {
        // Apodizing 切趾：在通带高频加入平滑滚降（Raised-Cosine 频域过渡带），大幅压低 20 kHz 附近的预振铃
        double f1 = (s.apodF1 > 0.0) ? s.apodF1 : (s.cutoff * 0.85);
        double f2 = (s.apodF2 > 0.0) ? s.apodF2 : (s.cutoff * 1.05);
        if (f1 > f2) std::swap(f1, f2);
        const double fc = 0.5 * (f1 + f2) * 0.5;
        const double df = 0.5 * (f2 - f1) * 0.5;
        for (int i = 0; i < n; ++i) {
            const double x = (double)i - m;
            const double sinc_val = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * x) / (kPi * x);
            const double denom = 1.0 - 16.0 * df * df * x * x;
            double factor;
            if (std::abs(denom) < 1e-7) {
                factor = kPi / 4.0;
            } else {
                factor = std::cos(2.0 * kPi * df * x) / denom;
            }
            h[(size_t)i] = sinc_val * factor * w[(size_t)i];
            sum += h[(size_t)i];
        }
    }
    if (sum != 0.0) for (auto& v : h) v /= sum;
    return h;
}

// ---------------- 最小相位变换（倒谱法）----------------
// 输入线性相位 FIR，输出同长度的最小相位 FIR（因果，能量集中在起始处）。
inline std::vector<double> to_minimum_phase(const std::vector<double>& h) {
    const size_t L = h.size();
    size_t N = 1; while (N < L * 8) N <<= 1;          // 零填充，抑制倒谱法的截断误差

    std::vector<cplx> H(N, cplx(0.0, 0.0));
    for (size_t i = 0; i < L; ++i) H[i] = cplx(h[i], 0.0);
    fft(H);

    // 1) 对数幅度谱。地板必须取【相对峰值】的值而不是绝对值：
    //    高斯窗没有旁瓣结构，阻带可以深到 -300 dB 以下，若用绝对地板，
    //    深阻带会被整体压平，倒谱里出现大量"假"的 -69 项，反而污染整个最小相位解。
    double peakMag = 0.0;
    for (size_t k = 0; k < N; ++k) peakMag = std::max(peakMag, std::abs(H[k]));
    const double floorMag = std::max(peakMag * 1e-15, 1e-300);
    std::vector<cplx> logH(N);
    for (size_t k = 0; k < N; ++k) {
        const double mag = std::max(std::abs(H[k]), floorMag);
        logH[k] = cplx(std::log(mag), 0.0);
    }

    // 2) 实倒谱
    fft(logH, /*inverse=*/true);

    // 3) 折叠：n=0 保留；1..N/2-1 加倍；N/2 保留；其余置零
    std::vector<cplx> c(N, cplx(0.0, 0.0));
    c[0] = logH[0];
    for (size_t n = 1; n < N / 2; ++n) c[n] = logH[n] * 2.0;
    c[N / 2] = logH[N / 2];

    // 4) 指数还原 -> 最小相位频谱
    fft(c);
    for (auto& v : c) v = std::exp(v);

    // 5) 回到时域，取前 L 点（因果）
    fft(c, /*inverse=*/true);
    std::vector<double> out(L);
    for (size_t i = 0; i < L; ++i) out[i] = c[i].real();

    // 归一化到单位直流增益
    double sum = 0.0; for (auto v : out) sum += v;
    if (std::fabs(sum) > 1e-300) for (auto& v : out) v /= sum;
    return out;
}

// 混合相位 = 最小相位 + 一部分【线性相位延迟】。
//   a = 0 → 最小相位；a = 1 → 与线性相位相同的群延迟；中间 → 群延迟在两者之间。
//
// 为什么不是"两个冲激响应加权求和"：两者相位不同，直接相加会互相抵消，幅频就毁了。
// 也不是"相位逐点加权平均"：主值相位有 2π 折叠，且混合后的响应会超出抽头长度，
// 截断后幅频同样会崩（实测通带误差 20 dB）。
// 正确做法是 H_mix(ω) = H_min(ω)·e^{-jωτ}，τ = a·(L-1)/2：
// 幅度一点不变，只是把能量整体右移 τ，群延迟因此落在两者之间。
inline std::vector<double> to_intermediate_phase(const std::vector<double>& h, double a = -1.0) {
    if (a < 0.0) a = phase_mix();
    if (a <= 0.0) return to_minimum_phase(h);
    if (a >= 1.0) return h;

    const size_t L = h.size();
    size_t N = 1; while (N < L * 8) N <<= 1;
    std::vector<double> hmin = to_minimum_phase(h);

    std::vector<cplx> B(N, cplx(0.0, 0.0));
    for (size_t i = 0; i < hmin.size() && i < L; ++i) B[i] = cplx(hmin[i], 0.0);
    fft(B);

    const double tau = a * (double)(L - 1) * 0.5;      // 分数延迟（样本）
    for (size_t k = 0; k < N; ++k) {
        // 用【带符号频率】算相位：k > N/2 属于负频率，直接拿 k 算会破坏共轭对称，
        // 逆变换出来的实部就是错的（分数延迟尤其明显）。
        const double df = (k <= N / 2) ? (double)k : (double)k - (double)N;
        const double w = -2.0 * kPi * df * tau / (double)N;
        B[k] *= std::polar(1.0, w);
    }
    fft(B, /*inverse=*/true);

    std::vector<double> out(L);
    for (size_t i = 0; i < L; ++i) out[i] = B[i].real();
    double sum = 0.0; for (auto v : out) sum += v;
    if (std::fabs(sum) > 1e-300) for (auto& v : out) v /= sum;
    return out;
}

// ---------------- 预设表（对齐手册家族命名与变体）----------------
struct FilterPreset {
    const char* name;
    int         taps;        // 抽头数（对应 -short/-medium/-long/-xl/sinc-M 等）
    double      transition;  // 过渡带占比（相对理想截止）
    double      attenDb;
    WindowKind  window;
    PhaseKind   phase;
    bool        forNx       = false; // true = 用于 Nx（高码率）路径
    bool        twoStage    = false; // true = 两段式升频（*-2s）
    int         stage2Taps  = 64;    // 第二级每相位抽头数
    bool        apodizing   = false; // true = 切趾（通带高频平滑渐进衰减）
    double      apodF1      = 0.0;   // 滚降起始相对输入 Nyquist 比例（例如 18.4k / 22.05k）
    double      apodF2      = 0.0;   // 滚降终止相对输入 Nyquist 比例（例如 21.8k / 22.05k）
    // 每个【输入样本】的内核长度（抽头数）。0 = 关闭（沿用 taps，历史行为逐位不变）。
    // >0 时重采样器令每相位抽头 T = tapsPerInput、原型长度 = T × L，使频响与输出
    // 采样率解耦（详见 resampler.h ResamplerConfig::minTapsPerInput 的注释）。
    int         tapsPerInput = 0;
};

// 抽头数量级参考手册：short≈4k, medium≈32k, long≈256k, xl≈1M, sinc-M≈1M, 16M≈16M
inline const std::vector<FilterPreset>& filter_presets() {
    static const std::vector<FilterPreset> p = {
        // ---- 1. 经典 poly-sinc 家族（Kaiser 窗）----
        { "poly-sinc-short-lp",        4095,    0.10,  120, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-short-mp",        4095,    0.10,  120, WindowKind::Kaiser,         PhaseKind::Minimum,      false, false, 64 },
        { "poly-sinc-short-2s",        4095,    0.10,  120, WindowKind::Kaiser,         PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-lp",             32767,    0.04,  180, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-mp",             32767,    0.04,  180, WindowKind::Kaiser,         PhaseKind::Minimum,      false, false, 64 },
        { "poly-sinc-2s",             32767,    0.04,  180, WindowKind::Kaiser,         PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-mp-2s",          32767,    0.04,  180, WindowKind::Kaiser,         PhaseKind::Minimum,      true,  true,  64 },
        { "poly-sinc-long-lp",       262143,    0.015, 200, WindowKind::Kaiser,         PhaseKind::Linear,       true,  false, 64 },
        { "poly-sinc-long-mp",       262143,    0.015, 200, WindowKind::Kaiser,         PhaseKind::Minimum,      true,  false, 64 },
        { "poly-sinc-long-ip",       262143,    0.015, 200, WindowKind::Kaiser,         PhaseKind::Intermediate, true,  false, 64 },
        { "poly-sinc-long-2s",       262143,    0.015, 200, WindowKind::Kaiser,         PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-xl",           1048575,    0.008, 230, WindowKind::Kaiser,         PhaseKind::Linear,       true,  false, 64 },
        { "poly-sinc-xla",          1048575,    0.010, 220, WindowKind::Kaiser,         PhaseKind::Linear,       true,  false, 64 },
        { "poly-sinc-xtr",          1048575,    0.008, 230, WindowKind::Kaiser,         PhaseKind::Linear,       true,  false, 64 },


        // ---- 2026-10-26（261026-06 用户要求）：补齐 HQPlayer 手册里其余家族变体 ----
        // 依据 HQPlayer 手册「poly-sinc 家族」命名规则：
        //   -short≈4k / 基准≈32k / -long≈262k / -xl≈1M 抽头；-lp 线性、-mp 最小、-ip 中间相位；
        //   -2s 两段式；-hires 高码率优化（过渡带更宽）；-mqa/mp3 为有损源优化（过渡带更宽）。
        // 与已有条目**同一算法**，只是参数不同 —— 逐条验证频响即可，不需要新代码路径。
        // 字段序：name, taps, transition, attenDb, window, phase, forNx, twoStage, stage2Taps,
        //         apodizing, apodF1, apodF2, tapsPerInput
        { "poly-sinc-ext2-short",      32767,  0.06,  180, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-ext2-medium",    131071,  0.03,  200, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-ext2-long",      524287,  0.015, 220, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-ext2-hires-lp",   32767,  0.08,  160, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-ext2-hires-ip",   32767,  0.08,  160, WindowKind::BlackmanHarris, PhaseKind::Intermediate, false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-ext2-hires-mp",   32767,  0.08,  160, WindowKind::BlackmanHarris, PhaseKind::Minimum,      false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-xtr-lp",         524287,  0.008, 230, WindowKind::Kaiser,         PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-xtr-mp",         524287,  0.008, 230, WindowKind::Kaiser,         PhaseKind::Minimum,      true,  false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-xtr-short-lp",    16383,  0.10,  140, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-xtr-short-mp",    16383,  0.10,  140, WindowKind::Kaiser,         PhaseKind::Minimum,      false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-gauss-medium",    16383,  0.08,  150, WindowKind::Gaussian,       PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-gauss-halfband",   8191,  0.06,  120, WindowKind::Gaussian,       PhaseKind::Linear,       false, false, 64, true,  0.44, 0.50, 0 },
        { "poly-sinc-gauss-halfband-s", 4095,  0.08,  110, WindowKind::Gaussian,       PhaseKind::Linear,       false, false, 64, true,  0.42, 0.50, 0 },
        { "poly-sinc-hb-xs",            2047,  0.16,  100, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-ext",             65535,  0.025, 190, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-mqa/mp3-lp",       8191,  0.14,  110, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-mqa/mp3-mp",       8191,  0.14,  110, WindowKind::Kaiser,         PhaseKind::Minimum,      false, false, 64, false, 0.0,  0.0,  0 },
        { "sinc-short",                 8191,  0.10,  150, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "sinc-medium",               32767,  0.05,  180, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "sinc-long",                131071,  0.02,  210, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-long-h",              131071,  0.02,  210, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, true,  0.45, 0.50, 0 },
        { "sinc-Mx",                  524287,  0.012, 220, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-MGa",                 524287,  0.015, 215, WindowKind::Gaussian,       PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-Ls",                  262143,  0.03,  200, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-Lm",                  262143,  0.03,  200, WindowKind::BlackmanHarris, PhaseKind::Minimum,      true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-Lh",                  262143,  0.03,  200, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, true,  0.45, 0.50, 0 },
        { "minringFIR-lp",              4095,  0.12,  120, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },

        // ---- 2. 高斯高分辨率与变体（poly-sinc-gauss 家族，高斯窗）----
        { "poly-sinc-gauss-short",     4095,    0.12,  120, WindowKind::Gaussian,       PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-gauss-long",     32767,    0.05,  180, WindowKind::Gaussian,       PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-gauss-mp",       32767,    0.05,  180, WindowKind::Gaussian,       PhaseKind::Minimum,      false, false, 64 },
        { "poly-sinc-gauss-2s",       32767,    0.05,  180, WindowKind::Gaussian,       PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-gauss-long-2s",  65535,    0.035, 200, WindowKind::Gaussian,       PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-gauss-hires-lp", 65535,    0.035, 200, WindowKind::Gaussian,       PhaseKind::Linear,       true,  false, 64 },
        { "poly-sinc-gauss-hires-mp", 65535,    0.035, 200, WindowKind::Gaussian,       PhaseKind::Minimum,      true,  false, 64 },
        { "poly-sinc-gauss-hires-ip", 65535,    0.035, 200, WindowKind::Gaussian,       PhaseKind::Intermediate, true,  false, 64 },
        { "poly-sinc-gauss-xl",      524287,    0.025, 210, WindowKind::Gaussian,       PhaseKind::Linear,       true,  false, 64 },
        { "poly-sinc-gauss-xla",     524287,    0.030, 200, WindowKind::Gaussian,       PhaseKind::Linear,       true,  false, 64 },

        // ---- 3. ext2 扩展频响变体（Blackman-Harris 窗）----
        { "poly-sinc-ext2",           65535,    0.03,  200, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-ext2-2s",        65535,    0.03,  200, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-ext2-xl",       524287,    0.015, 230, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64 },
        { "poly-sinc-ext2-xla",      524287,    0.018, 220, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64 },

        // ---- 4. 半带（half-band）家族及 hb-*-2s 两段式 ----
        { "poly-sinc-hb",              1023,    0.20,  100, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-hb-s",             511,    0.25,   95, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-hb-m",            2047,    0.15,  115, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-hb-l",            4095,    0.10,  130, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64 },
        { "poly-sinc-hb-2s",           2047,    0.16,  110, WindowKind::Kaiser,         PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-hb-s-2s",         1023,    0.22,  100, WindowKind::Kaiser,         PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-hb-m-2s",         4095,    0.14,  120, WindowKind::Kaiser,         PhaseKind::Linear,       true,  true,  64 },
        { "poly-sinc-hb-l-2s",         8191,    0.08,  135, WindowKind::Kaiser,         PhaseKind::Linear,       true,  true,  64 },

        // ---- 5. sinc 恒定时间与超长系列（sinc-{S,M,MG,L,LL}）----
        { "sinc-S",                   65535,    0.025, 200, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64 },
        { "sinc-M",                  524287,    0.010, 220, WindowKind::Kaiser,         PhaseKind::Linear,       true,  false, 64 },
        { "sinc-MG",                 524287,    0.020, 210, WindowKind::Gaussian,       PhaseKind::Linear,       true,  false, 64 },
        { "sinc-L",                 1048575,    0.006, 160, WindowKind::Kaiser,         PhaseKind::Linear,       true,  false, 64 },
        { "sinc-LL",                2097151,    0.004, 190, WindowKind::Kaiser,         PhaseKind::Linear,       true,  false, 64 },

        // ---- 6. 闭式插值家族（closed-form-{fast,base,M,16M}）----
        { "closed-form-fast",          4095,    0.10,  144, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64 },
        { "closed-form",              16383,    0.06,  150, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64 },
        { "closed-form-M",          1048575,    0.008, 200, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64 },
        { "closed-form-16M",       16777215,    0.003, 240, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64 },

        // ---- 7. 最小振铃特殊类型 ----
        { "minringFIR",                 511,    0.28,   90, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64 },
        { "minringFIR-mp",              511,    0.28,   90, WindowKind::BlackmanHarris, PhaseKind::Minimum,      false, false, 64 },

        // ---- 8. 切趾（Apodizing）家族（通带渐进平滑滚降，压低 20 kHz 预振铃与 MQA/有损编码失真）----
        { "apod-20k",                  4095,    0.10,  120, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, true, 18400.0 / 22050.0, 21800.0 / 22050.0 },
        { "apod-19k-short",            4095,    0.10,  120, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, true, 18000.0 / 22050.0, 21400.0 / 22050.0 },
        { "apod-wide",                 4095,    0.10,  120, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, true, 17800.0 / 22050.0, 21700.0 / 22050.0 },

        // ---- 9. 「内核长度」家族（tapsPerInput）：抽头预算与输出采样率解耦（可选、可回退）----
        // 为什么需要（实测依据见 docs/PCM回放质量-实测与改进.md）：
        //   旧预设的抽头预算 totalTaps 是在【内插域】指定的，每相位抽头 T = totalTaps / L。
        //   44.1k->192k 的 L=640、44.1k->384k 的 L=1280 时，4095 抽头预设只剩 6 / 4 个抽头：
        //   通带 20 kHz 处 -5.1 dB，1 kHz 单音的镜像只压到 -29 dB（端到端 THD+N 实测 -29 dBFS）。
        //   而 44.1k->705.6k 的 L=16 有 255 个抽头，同一预设是砖墙 —— 同一预设在不同
        //   输出采样率下质量天差地别。
        //   这组预设直接指定「每个输入样本的内核长度」T，令原型长度 = T × L：
        //   频响与输出采样率【无关】，代价是每输出样本 T 次乘加、系数表 T × L 个 double。
        //   T=512 时：通带平坦到 21.4 kHz、阻带 >=140 dB、每输出样本 512 次乘加
        //   （对比：sinc-M 在 705.6k 下每输出样本 32767 次乘加）。
        // tapsPerInput 默认 0 ⟹ 其余 50 个预设的系数与输出逐位不变（tests/test_filters 第 9 节有容差基线）。
        { "poly-sinc-tight-lp",       32767,    0.020, 140, WindowKind::Kaiser, PhaseKind::Linear, false, false, 64, false, 0.0, 0.0, 512 },
        // 注意：切趾预设的过渡带由 apodF1/apodF2 决定，transition 只影响 1x 域的
        // 旁瓣窗口分析（与 apod-20k 保持一致取 0.10，否则 test_filters 的
        // "通带 0.9fc 内平坦" 判据会落在切趾滚降区里，读出 2.1 dB 的假纹波）。
        { "apod-20k-tight",           32767,    0.100, 140, WindowKind::Kaiser, PhaseKind::Linear, false, false, 64, true, 18400.0 / 22050.0, 21800.0 / 22050.0, 512 },
    };
    return p;
}

// 按名字查找预设（优先匹配当前路径 Nx/1x）
inline const FilterPreset* find_preset(const std::string& name, bool nxPath = false) {
    const auto& all = filter_presets();
    const FilterPreset* best = nullptr;
    int bestScore = -1;
    for (const auto& p : all) {
        if (name != p.name) continue;
        const int score = (p.forNx == nxPath) ? 2 : 1;
        if (score > bestScore) { bestScore = score; best = &p; }
    }
    return best;
}

// 按名字查找并生成系数
inline std::vector<double> make_filter(const std::string& name, bool nxPath, int* tapsOut = nullptr) {
    const FilterPreset* best = find_preset(name, nxPath);
    if (!best) return {};
    // 截止取半带型（约为 Nyquist 的一半），这才是升频原型的真实形态。
    // 若把 cutoff 取得接近 1.0，滤波器会退化成一个近似冲激（中心抽头占 96% 能量），
    // 前振铃/群延迟等时间域指标将失去意义。
    const double cutoff = 0.5 * (1.0 - best->transition);
    FirSpec s;
    s.taps = best->taps; s.cutoff = cutoff; s.attenDb = best->attenDb; s.window = best->window;
    s.apodizing = best->apodizing;
    if (best->apodizing) {
        // 在 2x 原型域中，0.5 对应输入 Nyquist（22.05 kHz）
        s.apodF1 = best->apodF1 * 0.5;
        s.apodF2 = best->apodF2 * 0.5;
    }
    auto h = design_fir(s);
    if (best->phase == PhaseKind::Minimum) h = to_minimum_phase(h);
    else if (best->phase == PhaseKind::Intermediate) h = to_intermediate_phase(h);
    if (tapsOut) *tapsOut = (int)h.size();
    return h;
}

namespace detail { inline size_t argmax_abs(const std::vector<double>& h) {
    size_t peak = 0; double best = -1.0;
    for (size_t i = 0; i < h.size(); ++i) { const double a = std::fabs(h[i]); if (a > best) { best = a; peak = i; } }
    return peak; } }

// 前振铃 / 后振铃 能量比（以脉冲响应峰值样本为分界）。
//   线性相位（对称）⟹ 前 = 后 ⟹ 比值 ≈ 1.0
//   最小相位       ⟹ 前 ≪ 后 ⟹ 比值 ≪ 1.0
// 注意：不要用「峰值前能量 / 总能量」判定对称性——峰值样本自身往往占近半能量，
// 对称滤波器该比值天然只有 ~0.25，极易误判。
inline double pre_post_ratio(const std::vector<double>& h) {
    const size_t peak = detail::argmax_abs(h);
    double pre = 0.0, post = 0.0;
    for (size_t i = 0; i < h.size(); ++i) {
        const double e = h[i] * h[i];
        if (i < peak) pre += e; else if (i > peak) post += e;
    }
    return post > 0.0 ? pre / post : 0.0;
}

// 峰值样本在脉冲响应中的相对位置（0 = 起始，1 = 末尾）
inline double peak_position(const std::vector<double>& h) {
    if (h.size() < 2) return 0.0;
    return (double)detail::argmax_abs(h) / (double)(h.size() - 1);
}

// 群延迟估计（样本数）：能量的重心
inline double energy_centroid(const std::vector<double>& h) {
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < h.size(); ++i) { const double e = h[i] * h[i]; num += e * (double)i; den += e; }
    return den > 0.0 ? num / den : 0.0;
}

} // namespace rta::dsp
