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
// NTF 设计：数字 Butterworth 高通
//   NTF(z) = B(z)/D(z),  B(z) = (1 − z⁻¹)^N
//   D(z) = Π[(1−a_k) − (1+a_k) z⁻¹],  a_k = Ωc/p_k,  Ωc = tan(π·fc/fs)
//   p_k = 模拟低通 Butterworth 极点（左半平面 ⟹ 极点在单位圆内，环路稳定）
//
// D 的系数用 long double 展开：N=7 时 D 的根全聚在 z≈1 附近，double 精度不够
// （实测 N=7 的 1 kHz 抑制从应有的 −182 dB 退化成 −87 dB）。
#pragma once
#include "kaiser.h"
#include <vector>
#include <complex>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstddef>

namespace rta::dsp {

inline bool mod_finite_bits_u64(uint64_t b) {
    return ((b >> 52) & 0x7FFull) != 0x7FFull;
}

struct NtfPoly {
    std::vector<double> b;   // 分子，B(z) = (1 - z^-1)^N
    std::vector<double> d;   // 分母，d[0] = 1
    std::vector<double> p;   // P = B - D（p[0] = 0，严格真）
    int order = 0;
};

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

// 该采样率下【实际可用】的调制器阶数
//
// 实测（test_modulator 第 5 节）：
//   DSD64  (2.82 MHz) 7 阶 1-bit -> 环路饱和，带内 SNR 只剩 2 dB
//   DSD128 (5.64 MHz) 7 阶 1-bit -> 97.4 dB（可用）
//   DSD256 (11.3 MHz) 7 阶 1-bit -> 102.7 dB
// 原因不是算法错，而是 1-bit 量化误差幅度固定为 2.0，
// 低采样率下 7 阶环路没有足够时钟裕度把误差压回去。
// 所以这里做一次"降阶保护"：达不到条件的请求自动退回 5 阶并如实报告。
inline int stable_sdm_order(int requested, double fs) {
    if (requested <= 5) return requested < 1 ? 1 : requested;
    if (requested >= 7 && fs < 5.6e6) return 5;
    return requested > 7 ? 7 : requested;
}

// 调制器前级余量（dB，<=0）。1-bit 5 阶在 DSD64 上实测 MSA 约 0.65；
// 这里把真峰值压到 target（默认 0.62）以下，并留 marginDb 给 DSP 过冲。
// 只影响接近满幅的曲目，安静曲目返回 0（不衰减）。
inline double sdm_headroom_db(double peak, double target = 0.62, double marginDb = 0.2) {
    if (peak <= 0.0) return 0.0;
    const double g = target / peak;
    if (g >= 1.0) return 0.0;
    return 20.0 * std::log10(g) - marginDb;
}

struct SdmConfig {
    double fs          = 2822400.0;
    int    order       = 5;
    double cutoffHz    = 20000.0;
    int    levels      = 2;
    bool   ecCorrection = false;
    double ditherLsb   = 0.0;     // 可选：量化前注入的抖动（以量化步长为单位）
};

class SdmModulator {
public:
    explicit SdmModulator(const SdmConfig& cfg) : cfg_(cfg) {
        ntf_ = design_ntf_poly(cfg.order, cfg.cutoffHz, cfg.fs);
        const size_t sz = (size_t)cfg.order + 1;
        eBuf_.assign(sz, 0.0);
        wBuf_.assign(sz, 0.0);
        levels_ = cfg.levels < 2 ? 2 : cfg.levels;
    }

    const NtfPoly& ntf() const { return ntf_; }
    int order() const { return cfg_.order; }
    int levels() const { return levels_; }
    long long clamped() const { return clamped_; }     // 误差钳位次数（数值保护）
    long long overload() const { return overload_; }   // 环路限幅器触发次数（输入过载）
    long long nonFinite() const { return nonFinite_; } // 非有限输入次数（防御性清零）


    double process(double x) {
        const size_t sz = (size_t)cfg_.order + 1;

        // w[n]：只用到 e[n-1..] 与 w[n-1..]
        double acc = 0.0;
        for (size_t k = 1; k < sz; ++k)
            acc += ntf_.p[k] * eBuf_[(epos_ + sz - k) % sz];
        for (size_t k = 1; k < sz; ++k)
            acc -= ntf_.d[k] * wBuf_[(wpos_ + sz - k) % sz];

        // 防 NaN/Inf：坏 IR/EQ 或上游错误不能把环路历史永久污染成噪音。
        // 先 memcpy 成整数位型再判，避免 -ffast-math 把检查折叠掉。
        { uint64_t ab = 0; std::memcpy(&ab, &acc, sizeof(ab));
          if (!mod_finite_bits_u64(ab)) { acc = 0.0; wBuf_[wpos_] = 0.0; } }
        double u = x + acc;
        { uint64_t ub = 0; std::memcpy(&ub, &u, sizeof(ub));
          if (!mod_finite_bits_u64(ub)) { u = 0.0; ++nonFinite_; } }
        // ★ 环路限幅器：允许量化器内部 1.5 的瞬时过冲，但绝不让它发散。
        //   旧实现只钳位 e（而且 eMax=64 形同虚设），输入一旦过载，e 变成 O(100)，
        //   经 5/7 阶反馈后锁死在极限环 —— 听感是「播到第一个大动态就变成噪音」。
        //   阈值取 1.5 而不是 1.0：实测 1.0 会在 0.5 满幅时误触发（22 次），
        //   把 DSD64 的 89 dB SNR 打到 48 dB；1.5 在 0.62 以内完全不触发。
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
        // u 已在 [-1.5,1.5]，1-bit 时 |e|<=2.5；多电平时更小。这里只做数值安全网。
        const double eMax = 2.5;
        if (e > eMax)  { e = eMax;  ++clamped_; }
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
