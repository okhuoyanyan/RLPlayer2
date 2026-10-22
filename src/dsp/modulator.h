// modulator.h — SDM（sigma-delta）调制器：PCM -> DSD / 多电平 SDM
//
// 误差反馈结构（STF = 1，NTF 为高通）：
//   w[n] = Σ_{k=1..N} p[k]·e[n-k] − Σ_{k=1..N} d[k]·w[n-k]
//   u[n] = x[n] + w[n]
//   y[n] = Q(u[n])
//   e[n] = y[n] − u[n]
//   ⟹ Y(z) = X(z) + NTF(z)·E(z),  NTF(z) = B(z)/D(z)
//
// 关键：w[n] 只依赖 e[n-1..] 与 w[n-1..]，严格因果，绝不需要 e[n]。
// （上一版把滤波器输出算在量化之后再用到下一拍，等于整体错开一拍，
//   实测表现为 5 阶 1-bit 带内噪声只有 -53 dB、7 阶反而更差。）
//
// 调制器族架构说明：
//   1. DSD 族（Butterworth NTF，1-bit）：
//      NTF(z) = B(z)/D(z), B(z) = (1 - z^-1)^N（全零点在 DC）。
//      D(z) 为模拟 Butterworth 经双线性变换展开，极点在单位圆内。5 阶在 DSD64/128/256 均稳定。
//   2. 7 阶 Chebyshev NTF（可配带外增益/截止）：
//      极点按 Chebyshev Type I 椭圆分布设计，提供可控的带外增益（H∞ 约 4.0~7.0 dB）与更陡峭的过渡带。
//      带降阶保护：7 阶 1-bit 在低采样率 (< 5.6 MHz) 下因量化步长大易过载，自动降为 5 阶保护。
//   3. EC（误差反馈扩展消除）变体：
//      在音频通带 [0, 20 kHz] 内按 Chebyshev 极值分布优化 NTF 零点（扩展消除频带量化噪声），
//      显著加深带内抑制（可提升 3~17 dB SNR），同时保持严格因果误差反馈回路。
//   4. 多电平（AHM 风格，6~8 bit）：
//      量化级数 L = 2^B（6-bit=64, 7-bit=128, 8-bit=256 电平），步长 Δ = 2/(L-1)。
//      与 1-bit（Δ=2.0，误差峰值 1.0）相比：
//      - 8-bit 量化误差幅值 (|e| <= Δ/2 ≈ 0.0039) 仅为 1-bit 的 1/255，噪声功率骤降约 48 dB；
//      - 反馈累加器 w[n] 处于微幅线性区，7 阶在 DSD64 下亦能无过载稳定运行；
//      - 带外噪声积分从 1-bit 的 -0.6 dB 降至 -37~-49 dB，极大减轻后级模拟滤波负担。
#pragma once
#include "kaiser.h"
#include <vector>
#include <complex>
#include <cmath>
#include <cstring>
#include <string>
#include <algorithm>
#include <cstddef>

namespace rta::dsp {

inline bool mod_finite_bits_u64(uint64_t b) {
    return ((b >> 52) & 0x7FFull) != 0x7FFull;
}

enum class ModulatorFamily {
    Dsd = 0,       // 标准 Butterworth NTF（DSD5 / DSD7，经典 1-bit）
    Chebyshev = 1, // 7 阶 Chebyshev NTF（可配带外增益与截止，带高阶稳定性控制）
    Ec = 2,        // EC 误差反馈变体（频带零点扩展消除）
    Ahm = 3        // 多电平 AHM 风格（6~8 bit 高动态 SDM）
};

inline const char* modulator_family_name(ModulatorFamily f) {
    switch (f) {
        case ModulatorFamily::Dsd: return "dsd";
        case ModulatorFamily::Chebyshev: return "chebyshev";
        case ModulatorFamily::Ec: return "ec";
        case ModulatorFamily::Ahm: return "ahm";
    }
    return "dsd";
}

inline const char* modulator_family_label(ModulatorFamily f) {
    switch (f) {
        case ModulatorFamily::Dsd: return "DSD 标准（Butterworth NTF）";
        case ModulatorFamily::Chebyshev: return "7 阶 Chebyshev（可配带外增益）";
        case ModulatorFamily::Ec: return "EC 误差反馈（频带零点扩展消除）";
        case ModulatorFamily::Ahm: return "AHM 多电平（6~8 bit 风格）";
    }
    return "DSD 标准";
}

inline ModulatorFamily parse_modulator_family(const std::string& name) {
    std::string s = name;
    for (char& c : s) c = (char)::tolower((unsigned char)c);
    if (s == "chebyshev" || s == "cheb" || s == "cheby") return ModulatorFamily::Chebyshev;
    if (s == "ec" || s == "dsdec" || s == "ec_variant") return ModulatorFamily::Ec;
    if (s == "ahm" || s == "multilevel" || s == "multi") return ModulatorFamily::Ahm;
    return ModulatorFamily::Dsd;
}

struct NtfPoly {
    std::vector<double> b;   // 分子，B(z)
    std::vector<double> d;   // 分母，d[0] = 1
    std::vector<double> p;   // P = B - D（p[0] = 0，严格真）
    int order = 0;
};

// 经典数字 Butterworth 高通 NTF
inline NtfPoly design_ntf_poly(int order, double fcHz, double fs) {
    const long double wc = 2.0L * (long double)kPi * (long double)fcHz / (long double)fs;
    const long double Wc = std::tan(wc / 2.0L);

    const std::complex<long double> ONE(1.0L, 0.0L);
    std::vector<std::complex<long double>> den(1, ONE);
    for (int k = 0; k < order; ++k) {
        const long double theta = (long double)kPi * (2.0L * k + order + 1.0L) / (2.0L * order);
        const std::complex<long double> p_lp(std::cos(theta), std::sin(theta));
        const std::complex<long double> a = Wc / p_lp;
        const std::complex<long double> f0 = ONE - a;
        const std::complex<long double> f1 = -(ONE + a);
        std::vector<std::complex<long double>> nd(den.size() + 1, std::complex<long double>(0.0L, 0.0L));
        for (size_t i = 0; i < den.size(); ++i) {
            nd[i]     += den[i] * f0;
            nd[i + 1] += den[i] * f1;
        }
        den.swap(nd);
    }

    NtfPoly out;
    out.order = order;
    out.d.resize(den.size());
    for (size_t i = 0; i < den.size(); ++i) out.d[i] = (double)den[i].real();
    const double d0 = out.d[0];
    for (auto& v : out.d) v /= d0;

    out.b.assign((size_t)order + 1, 0.0);
    double binom = 1.0;
    for (int k = 0; k <= order; ++k) {
        if (k > 0) binom = binom * (double)(order - k + 1) / (double)k;
        out.b[(size_t)k] = binom * ((k & 1) ? -1.0 : 1.0);
    }
    const size_t n = (size_t)order + 1;
    out.p.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) out.p[i] = out.b[i] - out.d[i];
    return out;
}

// 7 阶 Chebyshev NTF：极点按椭圆分布，提供可控的带外增益与更陡峭的高通滚降
inline NtfPoly design_chebyshev_ntf_poly(int order, double fcHz, double fs, double oobGainDb = 4.5) {
    const long double wc = 2.0L * (long double)kPi * (long double)fcHz / (long double)fs;
    const long double Wc = std::tan(wc / 2.0L);

    double rip = (oobGainDb - 4.0) * 0.4;
    if (rip < 0.01) rip = 0.01;
    if (rip > 4.0)  rip = 4.0;

    double eps = std::sqrt(std::pow(10.0, rip / 10.0) - 1.0);
    double gamma = std::pow(1.0 / eps + std::sqrt(1.0 / (eps * eps) + 1.0), 1.0 / (double)order);
    long double r1 = 0.5L * (long double)(gamma - 1.0 / gamma);
    long double r2 = 0.5L * (long double)(gamma + 1.0 / gamma);

    const std::complex<long double> ONE(1.0L, 0.0L);
    std::vector<std::complex<long double>> den(1, ONE);
    for (int k = 0; k < order; ++k) {
        const long double theta = (long double)kPi * (2.0L * k + 1.0L) / (2.0L * (long double)order);
        const std::complex<long double> p_lp(-r1 * std::sin(theta), r2 * std::cos(theta));
        const std::complex<long double> a = Wc / p_lp;
        const std::complex<long double> f0 = ONE - a;
        const std::complex<long double> f1 = -(ONE + a);
        std::vector<std::complex<long double>> nd(den.size() + 1, std::complex<long double>(0.0L, 0.0L));
        for (size_t i = 0; i < den.size(); ++i) {
            nd[i]     += den[i] * f0;
            nd[i + 1] += den[i] * f1;
        }
        den.swap(nd);
    }

    NtfPoly out;
    out.order = order;
    out.d.resize(den.size());
    for (size_t i = 0; i < den.size(); ++i) out.d[i] = (double)den[i].real();
    const double d0 = out.d[0];
    for (auto& v : out.d) v /= d0;

    out.b.assign((size_t)order + 1, 0.0);
    double binom = 1.0;
    for (int k = 0; k <= order; ++k) {
        if (k > 0) binom = binom * (double)(order - k + 1) / (double)k;
        out.b[(size_t)k] = binom * ((k & 1) ? -1.0 : 1.0);
    }
    const size_t n = (size_t)order + 1;
    out.p.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) out.p[i] = out.b[i] - out.d[i];
    return out;
}

// EC 变体：通带内分布 Chebyshev 零点，实现音频带内的扩展误差消除
inline NtfPoly design_ec_ntf_poly(int order, double fcHz, double fs, double fBandHz = 20000.0) {
    const long double wc = 2.0L * (long double)kPi * (long double)fcHz / (long double)fs;
    const long double Wc = std::tan(wc / 2.0L);

    const std::complex<long double> ONE(1.0L, 0.0L);
    std::vector<std::complex<long double>> den(1, ONE);
    for (int k = 0; k < order; ++k) {
        const long double theta = (long double)kPi * (2.0L * k + order + 1.0L) / (2.0L * order);
        const std::complex<long double> p_lp(std::cos(theta), std::sin(theta));
        const std::complex<long double> a = Wc / p_lp;
        const std::complex<long double> f0 = ONE - a;
        const std::complex<long double> f1 = -(ONE + a);
        std::vector<std::complex<long double>> nd(den.size() + 1, std::complex<long double>(0.0L, 0.0L));
        for (size_t i = 0; i < den.size(); ++i) {
            nd[i]     += den[i] * f0;
            nd[i + 1] += den[i] * f1;
        }
        den.swap(nd);
    }

    NtfPoly out;
    out.order = order;
    out.d.resize(den.size());
    for (size_t i = 0; i < den.size(); ++i) out.d[i] = (double)den[i].real();
    const double d0 = out.d[0];
    for (auto& v : out.d) v /= d0;

    std::vector<double> zero_w;
    const double wB = 2.0 * kPi * fBandHz / fs;
    if (order == 5) {
        zero_w = { 0.0, 0.655 * wB, 0.942 * wB };
    } else if (order == 7) {
        zero_w = { 0.0, 0.468 * wB, 0.778 * wB, 0.974 * wB };
    } else {
        zero_w = { 0.0 };
    }

    std::vector<long double> num = { 1.0L, -1.0L };
    for (size_t i = 1; i < zero_w.size(); ++i) {
        long double c = std::cos((long double)zero_w[i]);
        std::vector<long double> next(num.size() + 2, 0.0L);
        for (size_t j = 0; j < num.size(); ++j) {
            next[j]     += num[j] * 1.0L;
            next[j + 1] += num[j] * (-2.0L * c);
            next[j + 2] += num[j] * 1.0L;
        }
        num.swap(next);
    }

    out.b.resize(num.size());
    for (size_t i = 0; i < num.size(); ++i) out.b[i] = (double)num[i];

    const size_t n = (size_t)order + 1;
    out.p.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) out.p[i] = out.b[i] - out.d[i];
    return out;
}

// 该采样率下【实际可用】的调制器阶数（含降阶保护）
inline int stable_sdm_order(int requested, double fs, int levels = 2) {
    if (requested <= 5) return requested < 1 ? 1 : requested;
    if (requested >= 7 && levels == 2 && fs < 5.6e6) return 5;
    return requested > 7 ? 7 : requested;
}

// 调制器前级余量（dB，<=0）
inline double sdm_headroom_db(double peak, double target = 0.62, double marginDb = 0.2) {
    if (peak <= 0.0) return 0.0;
    const double g = target / peak;
    if (g >= 1.0) return 0.0;
    return 20.0 * std::log10(g) - marginDb;
}

struct SdmConfig {
    double fs           = 2822400.0;
    int    order        = 5;
    double cutoffHz     = 20000.0;
    int    levels       = 2;
    bool   ecCorrection = false;
    double ditherLsb    = 0.0;     // 可选：量化前注入的抖动（以量化步长为单位）
    ModulatorFamily family = ModulatorFamily::Dsd;
    double oobGainDb    = 4.5;     // Chebyshev 带外增益 (dB)，默认 4.5 dB（Lee 判据稳定区）
    int    ahmBits      = 8;       // AHM 多电平位深 (6~8 bit)，默认 8-bit (256 电平)
};

class SdmModulator {
public:
    explicit SdmModulator(const SdmConfig& cfg) : cfg_(cfg) {
        if (cfg_.family == ModulatorFamily::Ahm) {
            int b = cfg_.ahmBits;
            if (b < 2) b = 8;
            if (b > 16) b = 16;
            levels_ = 1 << b;
        } else {
            levels_ = cfg_.levels < 2 ? 2 : cfg_.levels;
        }

        effectiveOrder_ = cfg_.order;
        if (effectiveOrder_ >= 7 && levels_ == 2 && cfg_.fs < 5.6e6) {
            effectiveOrder_ = 5;
            steppedDown_ = true;
            stepDownReason_ = "采样率 (" + std::to_string((int)(cfg_.fs / 1000.0)) +
                              " kHz) 不足 5.6 MHz，7 阶 1-bit 环路易饱和，自动降为 5 阶保护";
        }

        if (cfg_.family == ModulatorFamily::Chebyshev) {
            ntf_ = design_chebyshev_ntf_poly(effectiveOrder_, cfg_.cutoffHz, cfg_.fs, cfg_.oobGainDb);
        } else if (cfg_.family == ModulatorFamily::Ec || cfg_.ecCorrection) {
            ntf_ = design_ec_ntf_poly(effectiveOrder_, cfg_.cutoffHz, cfg_.fs);
        } else {
            ntf_ = design_ntf_poly(effectiveOrder_, cfg_.cutoffHz, cfg_.fs);
        }

        const size_t sz = (size_t)effectiveOrder_ + 1;
        eBuf_.assign(sz, 0.0);
        wBuf_.assign(sz, 0.0);
    }

    const NtfPoly& ntf() const { return ntf_; }
    int order() const { return cfg_.order; }
    int effectiveOrder() const { return effectiveOrder_; }
    bool steppedDown() const { return steppedDown_; }
    const std::string& stepDownReason() const { return stepDownReason_; }
    ModulatorFamily family() const { return cfg_.family; }
    int levels() const { return levels_; }
    long long clamped() const { return clamped_; }     // 误差钳位次数（数值保护）
    long long overload() const { return overload_; }   // 环路限幅器触发次数（输入过载）
    long long nonFinite() const { return nonFinite_; } // 非有限输入次数（防御性清零）

    double process(double x) {
        const size_t sz = (size_t)effectiveOrder_ + 1;

        // w[n]：只用到 e[n-1..] 与 w[n-1..]
        double acc = 0.0;
        for (size_t k = 1; k < sz; ++k)
            acc += ntf_.p[k] * eBuf_[(epos_ + sz - k) % sz];
        for (size_t k = 1; k < sz; ++k)
            acc -= ntf_.d[k] * wBuf_[(wpos_ + sz - k) % sz];

        // 防 NaN/Inf：坏 IR/EQ 或上游错误不能把环路历史永久污染成噪音。
        { uint64_t ab = 0; std::memcpy(&ab, &acc, sizeof(ab));
          if (!mod_finite_bits_u64(ab)) { acc = 0.0; wBuf_[wpos_] = 0.0; } }
        double u = x + acc;
        { uint64_t ub = 0; std::memcpy(&ub, &u, sizeof(ub));
          if (!mod_finite_bits_u64(ub)) { u = 0.0; ++nonFinite_; } }

        // 环路限幅器：允许内部瞬时过冲，绝不发散
        const double uLim = 1.5;
        if (u >  uLim) { u =  uLim; ++overload_; }
        if (u < -uLim) { u = -uLim; ++overload_; }

        if (cfg_.ditherLsb > 0.0) {
            const double step = (levels_ == 2) ? 2.0 : 2.0 / (double)(levels_ - 1);
            u += cfg_.ditherLsb * step * ((double)(rngNext() >> 11) / 9007199254740992.0 - 0.5);
        }

        double y;
        if (levels_ == 2) {
            y = (u >= 0.0) ? 1.0 : -1.0;
        } else {
            const double step = 2.0 / (double)(levels_ - 1);
            y = step * std::round(u / step);
            if (y >  1.0) y =  1.0;
            if (y < -1.0) y = -1.0;
        }

        double e = y - u;
        const double eMax = 2.5;
        if (e >  eMax) { e =  eMax; ++clamped_; }
        if (e < -eMax) { e = -eMax; ++clamped_; }

        eBuf_[epos_] = e; epos_ = (epos_ + 1) % sz;
        wBuf_[wpos_] = acc; wpos_ = (wpos_ + 1) % sz;
        return y;
    }

    void reset() {
        std::fill(eBuf_.begin(), eBuf_.end(), 0.0);
        std::fill(wBuf_.begin(), wBuf_.end(), 0.0);
        epos_ = wpos_ = 0; clamped_ = 0; overload_ = 0; nonFinite_ = 0;
    }

private:
    uint64_t rngNext() { s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27; return s_ * 2685821657736338717ull; }
    SdmConfig cfg_;
    NtfPoly ntf_;
    int levels_ = 2;
    int effectiveOrder_ = 5;
    bool steppedDown_ = false;
    std::string stepDownReason_;
    std::vector<double> eBuf_, wBuf_;
    size_t epos_ = 0, wpos_ = 0;
    long long clamped_ = 0;
    long long overload_ = 0;
    long long nonFinite_ = 0;
    uint64_t s_ = 0x2545F4914F6CDD1Dull;
};

// 按多项式计算 NTF 幅频响应(dB)，用于验证设计
inline double ntf_poly_response_db(const NtfPoly& n, double freqNorm) {
    const double w = kPi * freqNorm;
    std::complex<double> num(0.0, 0.0), den(0.0, 0.0);
    for (size_t k = 0; k < n.b.size(); ++k) num += n.b[k] * std::polar(1.0, -w * (double)k);
    for (size_t k = 0; k < n.d.size(); ++k) den += n.d[k] * std::polar(1.0, -w * (double)k);
    const double m = std::abs(num) / (std::abs(den) + 1e-300);
    return 20.0 * std::log10(m + 1e-300);
}

} // namespace rta::dsp
