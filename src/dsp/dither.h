// dither.h — PCM 抖动与噪声整形（10 个选项，均为 RLPlayer 自有实现）
// 名称/类别参考 HQPlayer 的公开手册；不宣称系数、曲线或听感与其专有实现等效。
//
// 结构：误差反馈型噪声整形
//   u[n] = x[n] + sum_{k=1..N} h[k] * e[n-k]      （反馈过去的量化误差）
//   y[n] = Q(u[n] + dither[n])                    （重新量化到目标位深）
//   e[n] = y[n] - u[n]                            （本样本量化误差）
// 由此得  Y(z) = X(z) + NTF(z) * E(z)，其中
//   NTF(z) = 1 + sum_{k>=1} h[k] z^-k
//
// NS 系列取 NTF(z) = (1 - r*z^-1)^N。仅 r=1 时直流处有 N 阶零点；
// r<1 时直流增益为 (1-r)^N，以较浅的整形换取低采样率下受控的高频增益。
//
// ★ 关键：r 必须按【输出采样率】重标定
//   |NTF(f)| = [(1-r)^2 + 4r·sin²(πf/fs)]^(N/2)
//   r=1 时高频端增益是 2^N。fs=44.1k 下 20 kHz 处有 +52 dB ——
//   等于把 15~20 kHz 的白噪放大 400 倍（听感：高频嘶声）；
//   而同样的系数在 fs=705.6k 下 20 kHz 处只有 -28 dB，非常温和。
//   同一组系数不可能同时适合所有采样率。
//   下方 shaper_radius_for_rate() 解完整二次式，将 20 kHz 处的增益限制在
//   +6 dB 预算内，而非错误宣称“可听带绝不放大”。半径还随阶数变化；
//   NS9 在 44.1k/96k 时约为 0.083/0.741，176.4k 起为 1。
//   超声噪声依然可能很高，尤其低位深/高阶组合；自动策略不会给低于24bit选LNS15。
//
// 手册对应关系（严格按手册描述设定阶数，不臆造 NS2/NS3/NS6/NS7/NS8）：
//   NS1   1 阶，温和          NS4  4 阶
//   NS5   5 阶，"相当激进"    NS9  9 阶，"非常激进"
//   LNS15 15 阶。这里仍是二项式误差反馈，不是 HQPlayer 的未知专有曲线。
// shaped 使用一阶高通 TPDF：d[n] = q * (U[n] - U[n-1])。
// 每个样本的边缘分布仍为完整 TPDF（2 LSB 峰峰值），使低电平量化保持线性；
// 其噪声频谱由真实输出测量，不能拿 NS 的 NTF 当作 shaped 的输出噪声曲线。
#pragma once
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（见该头注释）
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

// 公开手册的适用率建议，仅用于解释选择；不是本实现的硬性可用率限制。
inline double shaper_recommended_min_rate(Shaper s) {
    switch (s) {
        case Shaper::NS1: case Shaper::NS9: return 176400.0;
        case Shaper::NS4: case Shaper::Shaped: return 88200.0;
        case Shaper::NS5: return 192000.0;
        case Shaper::LNS15: return 352800.0;
        default: return 0.0;
    }
}

// 必须在设备实际输出率/位深确定后调用。低速率用完整 TPDF 避免量化音调；
// 32bit 维持已有的纯舍入策略，手选的选项始终由调用方保留。
// 高率自动只用 NS9：离线实测 24bit LNS15 的总带超声噪声比 NS9 高约 35dB，
// 而 NS9 的 20Hz–20kHz 残差已很低。LNS15 留给显式手选；这不是对真实 DAC
// 模拟互调的测量，也不把双精度数值噪底当成可听音质提升。
// 8..15bit 的粗量化步长不适合自动高阶整形：8bit NS9 实测在过载后的
// 小信号段仍会顶轨。自动固定 TPDF，手动选择的 NS9/LNS15 不受影响。
inline Shaper auto_pcm_shaper(int bits, double actualRate) {
    if (bits >= 32) return Shaper::None;
    if (bits < 16 || !finite_bits(actualRate) || actualRate < 176400.0) return Shaper::TPDF;
    return Shaper::NS9;
}

// This is a conservative public PCM boundary, not a claim that every rejected
// pair failed. The 8..15-bit sweep found sustained low-level overload in NS9
// at 8/9 bits and in LNS15 through 15 bits. Resolve actual container/DAC bits
// before calling; auto resolves to TPDF here, and pre-DSD doubles are exempt.
inline bool pcm_shaper_precision_supported(Shaper mode, int effectiveBits) {
    return effectiveBits >= 16 || (mode != Shaper::NS9 && mode != Shaper::LNS15);
}
inline const char* pcm_shaper_precision_error() {
    return "NS9/LNS15 不支持低于 16 bit 的 PCM 有效位深；请选择 TPDF，或将 DAC 有效位深和输出容器位深提高至至少 16 bit。";
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
        // 预取前一随机数，使 shaped 首样本也有完整、零均值的 TPDF 分布。
        if (s_ == Shaper::Shaped) ditherPrev_ = rng_.uniform();
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
        double u = x;
        if (order_ > 0) {
            // 1) 误差反馈（只用过去的误差，严格因果）
            for (int k = 1; k <= order_; ++k)
                u += h_[(size_t)k] * err_[(size_t)k - 1];

        }
        // 2) 抖动与误差反馈阶数无关。旧版把 shaped 放在 order>0 的分支，
        // 但 shaped 的反馈阶数是0，整段永远不可达，实际退化成了 none。
        switch (s_) {
            case Shaper::RPDF:   u += (rng_.uniform() - 0.5) * q_;                 break;
            case Shaper::TPDF:   u += (rng_.uniform() + rng_.uniform() - 1.0) * q_; break;
            case Shaper::Gauss1: u += rng_.gauss() * q_ * 0.5;                     break;
            case Shaper::Shaped: {
                const double current = rng_.uniform();
                u += (current - ditherPrev_) * q_;
                ditherPrev_ = current;
                break;
            }
            default: break;
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
        //   不会把大过载误差送回环路。削顶损失取决于输入与反馈幅度，不能声称
        //   一定不足1 LSB；高阶整形仍需要合理前级余量（见 test_dither 第 4 节）。
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
            // 至多15个历史值；连续后移去掉每tap的整数取模。
            // 乘加仍按 k=1..N 原顺序执行，保持既有 NS 输出逐位相同。
            for (int k = order_ - 1; k > 0; --k) err_[(size_t)k] = err_[(size_t)k - 1];
            err_[0] = e;
        }
        return y;
    }

    void reset() {
        std::fill(err_.begin(), err_.end(), 0.0);
        if (s_ == Shaper::Shaped) ditherPrev_ = rng_.uniform();
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
    std::vector<double> ntf_, h_, err_;
    double ditherPrev_ = 0.0;
    long long clamped_ = 0;
    long long overload_ = 0;
    long long nonFinite_ = 0;
    Rng rng_;
};

} // namespace rta::dsp
