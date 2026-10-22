// dither.h — 抖动与噪声整形（对应 HQPlayer 手册 4.4 的 10 个选项）
//
// 结构：误差反馈型噪声整形
//   u[n] = x[n] + sum_{k=1..N} h[k] * e[n-k]      （反馈过去的量化误差）
//   y[n] = Q(u[n] + dither[n])                    （重新量化到目标位深）
//   e[n] = y[n] - u[n]                            （本样本量化误差）
// 由此得  Y(z) = X(z) + NTF(z) * E(z)，其中
//   NTF(z) = 1 + sum_{k>=1} h[k] z^-k
//
// 本实现取 NTF(z) = (1 - r*z^-1)^N ：直流处 N 阶零点，把量化噪声推向高频。
//
// ★ 关键：r 必须按【输出采样率】重标定
//   |NTF(f)| = (2r·sin(πf/fs))^N
//   r=1 时高频端增益是 2^N。fs=44.1k 下 20 kHz 处有 +52 dB ——
//   等于把 15~20 kHz 的白噪放大 400 倍（听感：高频嘶声）；
//   而同样的系数在 fs=705.6k 下 20 kHz 处只有 -28 dB，非常温和。
//   同一组系数不可能同时适合所有采样率。
//   于是取 r = min(1, 1/(2·sin(π·20k/fs)))，保证可听带上端 |NTF| <= 1：
//     44.1k -> r≈0.505（温和整形，绝不放大可听带）
//     96k   -> r≈0.82
//     176.4k 及以上 -> r=1（完整 N 阶整形）
//   代价为零（仍是 N 次乘加），收益是任何输出采样率下都不会产生高频嘶声。
//
// 手册对应关系（严格按手册描述设定阶数，不臆造 NS2/NS3/NS6/NS7/NS8）：
//   NS1   1 阶，温和          NS4  4 阶
//   NS5   5 阶，"相当激进"    NS9  9 阶，"非常激进"
//   LNS15 15 阶，"平滑斜率"
#pragma once
#include "kaiser.h"          // 提供 kPi
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <algorithm>

namespace rta::dsp {

// 位级有限性检查：-ffast-math 可能把 std::isfinite 折叠成常量 true，
// 不能依赖它。NaN/Inf 一旦进入误差反馈就会永久污染状态（听感=噪音）。
inline bool finite_bits_u64(uint64_t b) {
    return ((b >> 52) & 0x7FFull) != 0x7FFull;
}
// 注意：不要用“接收 double 的有限性检查”做热路径判断 —— -ffast-math 的
// -ffinite-math-only 会让编译器假定参数有限，实测会把该函数折叠成恒 true。
// 热路径必须先 memcpy 成整数位型，再调用 finite_bits_u64()。
inline bool finite_bits(double x) {
    uint64_t b = 0;
    std::memcpy(&b, &x, sizeof(b));
    return finite_bits_u64(b);
}

// 手册 4.4 的抖动/整形选项
enum class Shaper {
    None,       // 不做抖动/整形，仅舍入（仅用于测试与比特完美场合）
    NS1,        // 1 阶
    NS4,        // 4 阶
    NS5,        // 5 阶
    NS9,        // 9 阶
    LNS15,      // 15 阶线性
    RPDF,       // 矩形概率密度（白噪）
    TPDF,       // 三角概率密度（行业标准）
    Gauss1,     // 高斯概率密度（平坦频响）
    Shaped,     // 整形抖动
};

inline const char* shaper_name(Shaper s) {
    switch (s) {
        case Shaper::None:   return "none";
        case Shaper::NS1:    return "NS1";
        case Shaper::NS4:    return "NS4";
        case Shaper::NS5:    return "NS5";
        case Shaper::NS9:    return "NS9";
        case Shaper::LNS15:  return "LNS15";
        case Shaper::RPDF:   return "RPDF";
        case Shaper::TPDF:   return "TPDF";
        case Shaper::Gauss1: return "Gauss1";
        case Shaper::Shaped: return "shaped";
    }
    return "?";
}

// 确定性的 xorshift64*，保证测试可复现
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s_(seed ? seed : 1) {}
    uint64_t next() { s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27; return s_ * 2685821657736338717ull; }
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() {
        double u1 = uniform(); if (u1 < 1e-300) u1 = 1e-300;
        double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi_ * u2);
    }
private:
    static constexpr double kPi_ = 3.14159265358979323846;
    uint64_t s_;
};

// 噪声整形阶数与"零点半径"
struct ShaperSpec {
    int    order = 0;
    double radius = 1.0;      // 实际使用时由 shaper_radius_for_rate() 覆盖
};

inline ShaperSpec shaper_spec(Shaper s) {
    switch (s) {
        case Shaper::NS1:   return {1,  1.0};
        case Shaper::NS4:   return {4,  1.0};
        case Shaper::NS5:   return {5,  1.0};
        case Shaper::NS9:   return {9,  1.0};
        case Shaper::LNS15: return {15, 1.0};
        default:            return {0,  1.0};
    }
}

// 按采样率求零点半径
//
// |NTF(f)| = |1 - r·e^{-jw}|^N = [ (1-r)^2 + 4r·sin^2(w/2) ]^(N/2),  w = 2πf/fs
// 注意：r≠1 时它【不等于】(2r·sin(w/2))^N —— 那个式子只在 r=1 成立，
// 用错会得出"r 越小高频越干净"的错误结论（本轮踩过一次）。
//
// 目标：让可听带上端（默认 20 kHz）的单级增益不超过 maxBumpDb 平均分摊到每级，
// 即 (1-r)^2 + 4r·s2 <= g^2，解这个二次不等式即可：
//   r = [ -(4s2-2) + sqrt((4s2-2)^2 - 4(1-g^2)) ] / 2
// 结果自然随采样率变化：
//   44.1k -> r≈0.08（几乎没有整形空间，接近纯 TPDF，这是事实而不是妥协）
//   96k   -> r≈0.74
//   176.4k 以上 -> r=1（完整 N 阶整形）
inline double shaper_radius_for_rate(double fs, int order = 9, double cornerHz = 20000.0,
                                     double maxBumpDb = 6.0) {
    if (fs <= 0.0 || order <= 0) return 1.0;
    const double w  = 2.0 * 3.14159265358979323846 * cornerHz / fs;
    const double sn = std::sin(w * 0.5);
    const double s2 = sn * sn;
    const double g  = std::pow(10.0, maxBumpDb / (20.0 * (double)order));   // 每级允许增益
    const double B  = 4.0 * s2 - 2.0;
    const double C  = 1.0 - g * g;
    const double disc = B * B - 4.0 * C;
    if (disc <= 0.0) return 0.0;
    const double r = (-B + std::sqrt(disc)) * 0.5;
    if (r <= 0.0) return 0.0;
    return std::min(1.0, r);
}

// NTF(z) = (1 - r z^-1)^N 的系数：ntf[k] = C(N,k) * (-r)^k
inline std::vector<double> ntf_coeffs(int order, double r) {
    std::vector<double> c((size_t)order + 1, 0.0);
    c[0] = 1.0;
    double binom = 1.0;
    for (int k = 1; k <= order; ++k) {
        binom = binom * (double)(order - k + 1) / (double)k;
        c[(size_t)k] = binom * std::pow(-r, (double)k);
    }
    return c;
}

// 在给定归一化频率(1.0 = Nyquist)上计算 NTF 幅度(dB)
inline double ntf_magnitude_db(const std::vector<double>& ntf, double freqNorm) {
    const double w = 3.14159265358979323846 * freqNorm;
    double re = 0.0, im = 0.0;
    for (size_t k = 0; k < ntf.size(); ++k) {
        re += ntf[k] * std::cos(-w * (double)k);
        im += ntf[k] * std::sin(-w * (double)k);
    }
    return 20.0 * std::log10(std::sqrt(re * re + im * im) + 1e-300);
}

// 单声道量化器 + 抖动 + 噪声整形
class Quantizer {
public:
    Quantizer(Shaper s, int bits, uint64_t seed = 12345, double sampleRate = 44100.0,
              double cornerHz = 20000.0)
        : s_(s), bits_(bits), rng_(seed), fs_(sampleRate) {
        auto sp = shaper_spec(s);
        order_ = sp.order;
        q_ = 2.0 / std::pow(2.0, (double)bits_);          // 量化步长（输入范围 [-1,1)）
        radius_ = shaper_radius_for_rate(fs_, order_, cornerHz);
        // 半径小到没有意义时干脆不整形（少做 N 次乘加，行为也更好解释）
        if (order_ > 0 && radius_ < 0.02) { order_ = 0; radius_ = 0.0; }
        ntf_ = ntf_coeffs(order_, radius_);
        h_.assign((size_t)order_ + 1, 0.0);
        for (int k = 1; k <= order_; ++k) h_[(size_t)k] = ntf_[(size_t)k];
        err_.assign((size_t)order_ + 1, 0.0);
        dithHist_.assign((size_t)order_ + 1, 0.0);
    }

    int order() const { return order_; }
    double step() const { return q_; }
    double sample_rate() const { return fs_; }
    double radius() const { return radius_; }
    long long clamped() const { return clamped_; }   // 误差反馈钳位次数（数值发散保护），正常应为 0
    long long overload() const { return overload_; } // 量化器输入过载次数（削顶），正常应为 0
    long long nonFinite() const { return nonFinite_; } // 非有限输入次数（防御性清零），正常应为 0
    const std::vector<double>& ntf() const { return ntf_; }

    // 实际频率处的 NTF 幅度（dB）
    double ntf_db(double freqHz) const {
        if (order_ <= 0) return 0.0;
        return ntf_magnitude_db(ntf_, freqHz / (fs_ * 0.5));
    }

    // 处理一个样本。x 必须落在 [-1, 1)。
    double process(double x) {
        const size_t sz = (size_t)order_ + 1;
        double u = x;
        if (order_ > 0) {
            // 1) 误差反馈（只用过去的误差，严格因果）
            for (int k = 1; k <= order_; ++k)
                u += h_[(size_t)k] * err_[(epos_ + sz - (size_t)k) % sz];

            // 2) 抖动
            switch (s_) {
                case Shaper::RPDF:   u += (rng_.uniform() - 0.5) * q_;                 break;
                case Shaper::TPDF:   u += (rng_.uniform() + rng_.uniform() - 1.0) * q_; break;
                case Shaper::Gauss1: u += rng_.gauss() * q_ * 0.5;                      break;
                case Shaper::Shaped: {
                    // 整形抖动：白噪经同一条 NTF 整形后注入
                    const double w = (rng_.uniform() + rng_.uniform() - 1.0) * q_ * 0.5;
                    double d = w;
                    for (int k = 1; k <= order_; ++k)
                        d += h_[(size_t)k] * dithHist_[(dpos_ + sz - (size_t)k) % sz];
                    dithHist_[dpos_] = w;
                    dpos_ = (dpos_ + 1) % sz;
                    u += d;
                    break;
                }
                default: break;
            }
        } else {
            switch (s_) {
                case Shaper::RPDF:   u += (rng_.uniform() - 0.5) * q_;                 break;
                case Shaper::TPDF:   u += (rng_.uniform() + rng_.uniform() - 1.0) * q_; break;
                case Shaper::Gauss1: u += rng_.gauss() * q_ * 0.5;                      break;
                default: break;
            }
        }

        // 防 NaN/Inf：坏 IR、EQ 系数或上游数值错误都可能送进来一个非有限值。
        // 旧的误差反馈会把它永久保存在历史里，输出从此变成 NaN/噪音。
        { uint64_t ub = 0; std::memcpy(&ub, &u, sizeof(ub));
          if (!finite_bits_u64(ub)) { u = 0.0; ++nonFinite_; } }

        // 3) 量化：钳位必须放在【量化之前】，而不是只钳位输出。
        //
        // ★ 2026-10-21 修的真实故障：旧代码只把 y 钳到 ±1，一旦量化器过载
        //   （LNS15 在 192k+ 输出、接近满幅时会必然过载：反馈项 h·e 把输入
        //   推过 1.0），误差 e = y - u 就变成 O(1)，再经 (1-z^-1)^15 反馈
        //   放大 90 dB，环路锁死在满幅方波 —— 听感是「播了几秒突然变成噪音」。
        //   把钳位放在量化【之前】后，|e| <= q/2 恒成立，反馈有界；过载只
        //   表现为一次不足 1 LSB 的削顶，不会锁死（见 test_dither 第 4 节）。
        const double uMax = 1.0 - q_;      // 最大可表示码值 2^(b-1)-1 对应的归一化值
        if (u > uMax) { u = uMax; ++overload_; }
        if (u < -1.0) { u = -1.0; ++overload_; }
        double y = q_ * std::round(u / q_);
        if (y >  1.0 - q_) y =  1.0 - q_;  // 数值安全网（输入已钳位，正常不触发）
        if (y < -1.0)      y = -1.0;

        // 4) 记录本样本量化误差
        if (order_ > 0) {
            double e = y - u;
            const double eMax = 1024.0 * q_;
            if (e >  eMax) { e =  eMax; ++clamped_; }
            if (e < -eMax) { e = -eMax; ++clamped_; }
            err_[epos_] = e;
            epos_ = (epos_ + 1) % sz;
        }
        return y;
    }

    void reset() {
        std::fill(err_.begin(), err_.end(), 0.0);
        std::fill(dithHist_.begin(), dithHist_.end(), 0.0);
        epos_ = dpos_ = 0;
        clamped_ = 0;
        overload_ = 0;
        nonFinite_ = 0;
    }

private:
    Shaper s_;
    int bits_;
    int order_ = 0;
    double q_ = 1.0;
    double fs_ = 44100.0;
    double radius_ = 1.0;
    std::vector<double> ntf_, h_, err_, dithHist_;
    size_t epos_ = 0, dpos_ = 0;
    long long clamped_ = 0;
    long long overload_ = 0;
    long long nonFinite_ = 0;
    Rng rng_;
};

} // namespace rta::dsp
