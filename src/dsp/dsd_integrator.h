// dsd_integrator.h — DSD 域抽取器（ROADMAP P0-4）：FIR / IIR / CIC
//
// 作用：把 1-bit 流（DSD64/128/256…）抽取成 PCM，交给现有管线（升频/抖动/SDM 都不变）。
//
// 判据先行（ROADMAP P0-4）：同一素材、同一输出速率下比
//   ① 通带平坦度（0…20 kHz 内 ±0.05 dB）
//   ② 混叠抑制（镜像频率 fold 回音频带后的残留 > 100 dB）
//   ③ THD+N（与 4001 抽头参考 FIR 的带内结果一致）
// 三条都在 tests/test_dsd_path.cpp 里实测，不靠"看起来对"。
//
// 设计思路（三种都是真参数，不是标签）：
//   FIR：Kaiser windowed-sinc，截止取 (通带 + 阻带)/2，抽头数按衰减与过渡带自动算；
//        阻带起点 = outRate − passband —— 只有这一点才能保证「0…passband 不被镜像污染」；
//        passband…outRate/2 之外的折叠落在 20 kHz 以上，不进音频带。
//   IIR：Butterworth 双线性级联（DF2T 双二阶），截止反推自 ±0.05 dB 的平坦度要求；
//        相位非线性（这是 IIR 的固有代价，换来极低延迟）。
//   CIC：无乘法，R = 抽取比、N 级，靠 2^64 无符号回绕实现「溢出在梳状差分里抵消」
//        （硬件 CIC 的标准做法），后面接最小二乘设计的 1/sinc^N 补偿 FIR，
//        否则 20 kHz 处固有 −0.9 dB 下垂。
//
// 已知限制（如实记录）：
//   * CIC 的第一旁瓣在 outRate − passband 处衰减约 N·17.9 dB，N=6 才过 100 dB；
//     级数越低混叠抑制越差，测试里会打印实测值。
//   * IIR 的高频阻带是渐进的，靠阶数堆；数值底噪大约 −150 dBFS 量级。
#pragma once
#include "audio/dsd_reader.h"
#include "kaiser.h"
#include "filter_design.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rta::dsp {

enum class IntegratorKind { Fir, Iir, Cic };

inline const char* integrator_name(IntegratorKind k) {
    switch (k) {
        case IntegratorKind::Fir: return "FIR";
        case IntegratorKind::Iir: return "IIR";
        case IntegratorKind::Cic: return "CIC";
    }
    return "?";
}

struct IntegratorConfig {
    IntegratorKind kind = IntegratorKind::Fir;
    uint32_t dsdRate = 2822400;      // DSD64
    uint32_t outRate = 176400;       // 必须是 dsdRate 的整数分频
    double   passbandHz = 20000.0;
    double   attenDb = 120.0;        // FIR 阻带目标
    int      taps = 0;               // FIR 抽头；0 = 按过渡带自动
    int      order = 10;             // IIR 阶数（偶数，<=20）
    int      cicStages = 6;          // CIC 级数
    bool     cicCompensate = true;   // CIC 后接 1/sinc^N 补偿 FIR
    bool     twoStage = true;        // 两级抽取（CIC 4x + 短 FIR），默认开启（仅对 FIR 且 taps==0 生效）
};

// ---------------- Conversion：DSD → PCM 抽取的「窄 / 宽 / XFi」三组预设 ----------------
//
// 说明（不冒充 HQPlayer）：HQP 没有公开 Conversion 的曲线定义，这里给的是**我们自己的**
// 三组真参数：只改通带上限，阻带起点随之 = outRate − passband，过渡带因此变窄、抽头数变多。
//   narrow：20 kHz 通带（默认，保护最严）
//   wide  ：30 kHz 通带（给 20–30 kHz 留内容）
//   xfi   ：40 kHz 通带（最宽；代价是过渡带最窄、抽头最多）
// 三者在工具/配置里可选，测试里出「截止点 / 过渡带宽度 / 阻带起点 / 抽头 / 平坦度 / 镜像衰减」数值表。
enum class DsdConversion { Narrow, Wide, Xfi };

inline const char* conversion_name(DsdConversion c) {
    switch (c) {
        case DsdConversion::Narrow: return "narrow";
        case DsdConversion::Wide:   return "wide";
        case DsdConversion::Xfi:    return "xfi";
    }
    return "narrow";
}

inline DsdConversion parse_conversion(const std::string& s) {
    if (s == "wide") return DsdConversion::Wide;
    if (s == "xfi")  return DsdConversion::Xfi;
    return DsdConversion::Narrow;
}

inline double conversion_passband(DsdConversion c) {
    switch (c) {
        case DsdConversion::Narrow: return 20000.0;
        case DsdConversion::Wide:   return 30000.0;
        case DsdConversion::Xfi:    return 40000.0;
    }
    return 20000.0;
}

// 生成对应预设的抽取器配置（其余参数保持默认：FIR 120 dB / IIR 10 阶 / CIC 6 级）
inline IntegratorConfig conversion_integrator(IntegratorKind kind, DsdConversion conv,
                                              uint32_t dsdRate, uint32_t outRate,
                                              double attenDb = 120.0,
                                              bool twoStage = true) {
    IntegratorConfig cfg;
    cfg.kind = kind;
    cfg.dsdRate = dsdRate;
    cfg.outRate = outRate;
    cfg.attenDb = attenDb;
    cfg.twoStage = twoStage;
    double p = conversion_passband(conv);
    const double limit = (double)outRate * 0.45;     // 再宽也要给阻带留 10%
    if (p > limit) p = limit;
    cfg.passbandHz = p;
    return cfg;
}

class DsdDecimator {
public:
    explicit DsdDecimator(const IntegratorConfig& cfg) : cfg_(cfg) { design(); }

    const IntegratorConfig& config() const { return cfg_; }
    uint32_t decimation() const { return M_; }
    uint32_t outRate() const { return cfg_.outRate; }
    double stopbandStartHz() const { return stopbandHz_; }
    bool isTwoStage() const {
        return cfg_.kind == IntegratorKind::Fir && cfg_.twoStage && cfg_.taps == 0 &&
               (M_ >= 4) && (M_ % 4 == 0);
    }
    const std::vector<double>& firTaps() const { return isTwoStage() ? h2_ : h_; }
    const std::vector<double>& compTaps() const { return comp_; }

    // 设计响应（dB）。f 为 Hz；CIC 已含补偿（若启用）。
    double response_db(double f) const {
        if (isTwoStage()) {
            if (f < 1e-12) return 0.0;
            const double sin_num = std::sin(kPi * f * (double)R1_ / (double)cfg_.dsdRate);
            const double sin_den = (double)R1_ * std::sin(kPi * f / (double)cfg_.dsdRate);
            double hcic = (std::fabs(sin_den) < 1e-300) ? 0.0 : std::pow(std::fabs(sin_num / sin_den), (double)cicStages_);
            double db_cic = (hcic > 1e-300) ? 20.0 * std::log10(hcic) : -300.0;
            double db_h2 = magnitude_db(h2_, 2.0 * f / (double)fmid_);
            return db_cic + db_h2;
        }
        switch (cfg_.kind) {
            case IntegratorKind::Fir:
                return magnitude_db(h_, 2.0 * f / (double)cfg_.dsdRate);
            case IntegratorKind::Iir: {
                const double w = 2.0 * kPi * f / (double)cfg_.dsdRate;
                double db = 0.0;
                for (const auto& q : iir_) db += biquad_response_db(q, w);
                return db;
            }
            case IntegratorKind::Cic: {
                const double m = cic_response_linear(f);
                if (m <= 0.0) return -400.0;
                double db = 20.0 * std::log10(m);
                if (cfg_.cicCompensate && !comp_.empty()) {
                    const double c = comp_response(f);
                    if (c > 0.0) db += 20.0 * std::log10(c);
                }
                return db;
            }
        }
        return 0.0;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0);
        ringPos_ = 0;
        for (auto& z : z1_) z = 0.0;
        for (auto& z : z2_) z = 0.0;
        std::fill(integ_.begin(), integ_.end(), (uint64_t)0);
        std::fill(comb_.begin(), comb_.end(), (uint64_t)0);
        std::fill(compRing_.begin(), compRing_.end(), 0.0);
        compPos_ = 0;
        inCount_ = 0;
        midCount_ = 0;
        std::fill(ring2_.begin(), ring2_.end(), 0.0);
        ringPos2_ = 0;
        for (int i = 0; i < 6; ++i) { integ6_[i] = 0; comb6_[i] = 0; }
    }

    // 处理一块 1-bit 流（MSB-first 打包，nbits 个采样），结果追加到 out。
    void process_bits(const uint8_t* packed, size_t nbits, std::vector<double>& out) {
        out.reserve(out.size() + nbits / (size_t)M_ + 4);
        if (isTwoStage()) {
            size_t i = 0;
            while (i < nbits && ((inCount_ & 3) != 0 || (nbits - i) < 4 || (i & 3) != 0)) {
                const bool bit = ((packed[i >> 3] >> (7 - (i & 7))) & 1u) != 0;
                step_two_stage(bit, out);
                ++i;
            }

            while (i + 4 <= nbits && (inCount_ & 3) == 0 && (i & 3) == 0) {
                const uint8_t nibble = ((i & 7) == 0) ? (packed[i >> 3] >> 4) : (packed[i >> 3] & 0x0F);
                step_nibble(nibble, out);
                i += 4;
            }

            while (i < nbits) {
                const bool bit = ((packed[i >> 3] >> (7 - (i & 7))) & 1u) != 0;
                step_two_stage(bit, out);
                ++i;
            }
        } else {
            for (size_t i = 0; i < nbits; ++i) {
                const double v = ((packed[i >> 3] >> (7 - (i & 7))) & 1u) ? 1.0 : -1.0;
                step(v, out);
            }
        }
    }

    // 整条流 → 每声道 PCM。各声道独立（状态在内部按声道重置）。
    std::vector<std::vector<double>> process(const rta::audio::DsdStream& s) {
        std::vector<std::vector<double>> planes((size_t)s.channels);
        for (int c = 0; c < s.channels; ++c) {
            reset();
            process_bits(s.data[(size_t)c].data(), (size_t)s.bits, planes[(size_t)c]);
        }
        return planes;
    }

private:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

    static double biquad_response_db(const Biquad& q, double w) {
        const std::complex<double> z1(std::cos(-w), std::sin(-w));
        const std::complex<double> z2 = z1 * z1;
        const std::complex<double> num = q.b0 + q.b1 * z1 + q.b2 * z2;
        const std::complex<double> den = 1.0 + q.a1 * z1 + q.a2 * z2;
        return 20.0 * std::log10(std::abs(num) / std::abs(den) + 1e-300);
    }

    // CIC 幅频（未补偿）：|sin(pi F) / (R sin(pi F/R))|^N，F 以 outRate 归一
    // 注意：CIC 的幅频以 outRate 为周期，但**不是**关于 F=0.5 对称的
    // （|sin(piF)| 对称，|sin(piF/R)| 不对称），所以这里不能把 F>0.5 折回。
    // 早期版本折回导致 156 kHz 的镜像衰减被误算成 +2.4 dB。
    double cic_response_linear(double f) const {
        double F = std::fmod(f, (double)cfg_.outRate) / (double)cfg_.outRate;
        if (F < 0.0) F += 1.0;
        if (F <= 1e-12 || F >= 1.0 - 1e-12) return 1.0;      // 直流（与整数倍 outRate 的零点区分开）
        const double R = (double)M_;
        const double s1 = std::sin(kPi * F);
        const double s2 = R * std::sin(kPi * F / R);
        if (std::fabs(s2) < 1e-300) return 0.0;
        return std::pow(std::fabs(s1 / s2), (double)cfg_.cicStages);
    }

    double comp_response(double f) const {
        if (comp_.empty()) return 1.0;
        const int K = (int)comp_.size() / 2;
        const double w = 2.0 * kPi * f / (double)cfg_.outRate;
        double acc = 0.0;
        for (int k = -K; k <= K; ++k) acc += comp_[(size_t)(k + K)] * std::cos(w * (double)k);
        return acc;
    }

    static bool solve_dense(std::vector<std::vector<double>>& A, std::vector<double>& b) {
        const size_t n = b.size();
        for (size_t col = 0; col < n; ++col) {
            size_t piv = col;
            for (size_t r = col + 1; r < n; ++r)
                if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
            if (std::fabs(A[piv][col]) < 1e-12) return false;
            std::swap(A[piv], A[col]);
            std::swap(b[piv], b[col]);
            const double d = A[col][col];
            for (size_t c = col; c < n; ++c) A[col][c] /= d;
            b[col] /= d;
            for (size_t r = 0; r < n; ++r) {
                if (r == col) continue;
                const double f = A[r][col];
                if (f == 0.0) continue;
                for (size_t c = col; c < n; ++c) A[r][c] -= f * A[col][c];
                b[r] -= f * b[col];
            }
        }
        return true;
    }

    void design() {
        if (cfg_.outRate == 0 || cfg_.dsdRate == 0)
            throw std::runtime_error("DsdDecimator: 采样率为 0");
        if (cfg_.dsdRate % cfg_.outRate != 0)
            throw std::runtime_error("DsdDecimator: 输出速率 " + std::to_string(cfg_.outRate) +
                                     " 不是 DSD 速率 " + std::to_string(cfg_.dsdRate) + " 的整数分频");
        M_ = cfg_.dsdRate / cfg_.outRate;
        if (cfg_.passbandHz <= 0.0 || cfg_.passbandHz >= cfg_.outRate * 0.5)
            throw std::runtime_error("DsdDecimator: 通带必须落在 (0, outRate/2) 内");
        stopbandHz_ = (double)cfg_.outRate - cfg_.passbandHz;

        switch (cfg_.kind) {
            case IntegratorKind::Fir:
                if (isTwoStage()) design_two_stage();
                else design_fir();
                break;
            case IntegratorKind::Iir:  design_iir(); break;
            case IntegratorKind::Cic:  design_cic(); break;
        }
    }

    void design_fir() {
        const double trans = (stopbandHz_ - cfg_.passbandHz) / (double)cfg_.dsdRate;
        int taps = cfg_.taps > 0 ? cfg_.taps : kaiser_taps_for(cfg_.attenDb, trans);
        if ((taps & 1) == 0) ++taps;
        LowpassSpec sp;
        sp.taps = taps;
        sp.cutoff = (cfg_.passbandHz + stopbandHz_) / (double)cfg_.dsdRate;   // Nyquist 单位
        sp.attenDb = cfg_.attenDb;
        h_ = design_lowpass(sp);
        ring_.assign(h_.size(), 0.0);
        ringPos_ = 0;
    }

    void design_iir() {
        int n = cfg_.order < 2 ? 2 : std::min(cfg_.order, 20);
        if (n & 1) ++n;
        // 由「通带边缘最多 -0.05 dB」反推截止：1/sqrt(1+(f/fc)^2n)
        const double r = std::pow(std::pow(10.0, 0.05 / 10.0) - 1.0, 1.0 / (2.0 * n));
        const double fc = cfg_.passbandHz / r * 1.05;
        if (fc >= cfg_.dsdRate * 0.45)
            throw std::runtime_error("DsdDecimator: IIR 截止频率过高（阶数不足）");
        const double Wc = std::tan(kPi * fc / (double)cfg_.dsdRate);
        iir_.clear();
        for (int k = 0; k < n / 2; ++k) {
            const double theta = kPi * (2.0 * k + n + 1.0) / (2.0 * n);
            const std::complex<double> p = Wc * std::polar(1.0, theta);
            const std::complex<double> zp = (1.0 + p) / (1.0 - p);
            Biquad q;
            q.a1 = -2.0 * zp.real();
            q.a2 = std::norm(zp);
            const double g = (1.0 + q.a1 + q.a2) / 4.0;
            q.b0 = g; q.b1 = 2.0 * g; q.b2 = g;
            iir_.push_back(q);
        }
        iirFc_ = fc;
        z1_.assign(iir_.size(), 0.0);
        z2_.assign(iir_.size(), 0.0);
    }

    void design_cic() {
        const int n = cfg_.cicStages < 1 ? 1 : std::min(cfg_.cicStages, 10);
        cicStages_ = n;
        integ_.assign((size_t)n, 0);
        comb_.assign((size_t)n, 0);
        cicGain_ = std::pow((double)M_, (double)n);
        comp_.clear();
        if (!cfg_.cicCompensate) return;

        // 3 抽头解析补偿：H(f) = 1 + 4c·sin²(π f / outRate)
        // c 取「20 kHz 处正好补回 CIC 下垂」。为什么不用最小二乘 17 抽头：
        // 0…20 kHz 之外的响应不受约束，拟合解在 156 kHz 处有 +74 dB 的增益，
        // 把 1-bit 的带外噪声放大成 −29 dBFS 的巨浪（实测，见 STATUS 本轮记录）。
        // 3 抽头在 Nyquist 处最多 +6.4 dB，有界；带内拟合误差实测 < 0.03 dB。
        const double rise = 1.0 / std::max(1e-6, cic_response_linear(cfg_.passbandHz)) - 1.0;
        const double s2 = std::sin(kPi * cfg_.passbandHz / (double)cfg_.outRate);
        const double c = rise / (4.0 * s2 * s2);
        comp_ = { -c, 1.0 + 2.0 * c, -c };   // H(f) = 1 + 4c·sin²(πf/outRate)
        compRing_.assign(comp_.size(), 0.0);
        compPos_ = 0;
    }

    void design_two_stage() {
        R1_ = 4;
        R2_ = M_ / 4;
        R2PowerOf2_ = (R2_ > 0) && ((R2_ & (R2_ - 1)) == 0);
        R2Mask_ = R2PowerOf2_ ? (R2_ - 1) : 0;
        fmid_ = cfg_.dsdRate / R1_;
        cicStages_ = 6;
        cicGain_ = std::pow(4.0, (double)cicStages_);
        cicInvGain_ = 1.0 / cicGain_;

        const double transNorm = (stopbandHz_ - cfg_.passbandHz) / (double)fmid_;
        const double atten = std::max(cfg_.attenDb, 130.0);
        int taps = kaiser_taps_for(atten, transNorm);
        if ((taps & 1) == 0) ++taps;
        LowpassSpec sp;
        sp.taps = taps;
        sp.cutoff = (cfg_.passbandHz + stopbandHz_) / (double)fmid_;
        sp.attenDb = atten;
        std::vector<double> h_lp = design_lowpass(sp);

        const int Nf = 500;
        double m11 = 0.0, m12 = 0.0, m22 = 0.0, b1 = 0.0, b2 = 0.0;
        for (int i = 1; i <= Nf; ++i) {
            const double f = cfg_.passbandHz * (double)i / (double)Nf;
            const double x = kPi * f / (double)cfg_.dsdRate;
            const double hcic = std::pow(std::cos(x) * std::cos(2.0 * x), (double)cicStages_);
            const double theta = kPi * f / (double)fmid_;
            const double a1 = -4.0 * std::sin(theta) * std::sin(theta);
            const double a2 = -4.0 * std::sin(2.0 * theta) * std::sin(2.0 * theta);
            const double y = 1.0 / hcic - 1.0;
            m11 += a1 * a1; m12 += a1 * a2; m22 += a2 * a2;
            b1 += a1 * y; b2 += a2 * y;
        }
        const double det = m11 * m22 - m12 * m12;
        const double c1 = (b1 * m22 - b2 * m12) / det;
        const double c2 = (m11 * b2 - m12 * b1) / det;
        const double c0 = 1.0 - 2.0 * (c1 + c2);
        const std::vector<double> comp = { c2, c1, c0, c1, c2 };

        h2_.assign(h_lp.size() + comp.size() - 1, 0.0);
        for (size_t i = 0; i < h_lp.size(); ++i)
            for (size_t j = 0; j < comp.size(); ++j)
                h2_[i + j] += h_lp[i] * comp[j];

        double sum = 0.0;
        for (double val : h2_) sum += val;
        if (sum != 0.0) {
            for (double& val : h2_) val /= sum;
        }

        L2_ = h2_.size();
        ring2_.assign(2 * L2_, 0.0);
        ringPos2_ = 0;

        for (int i = 0; i < 6; ++i) { integ6_[i] = 0; comb6_[i] = 0; }
    }

    inline void step_comb(std::vector<double>& out) {
        uint64_t c = integ6_[5];
        uint64_t d0 = c - comb6_[0]; comb6_[0] = c;
        uint64_t d1 = d0 - comb6_[1]; comb6_[1] = d0;
        uint64_t d2 = d1 - comb6_[2]; comb6_[2] = d1;
        uint64_t d3 = d2 - comb6_[3]; comb6_[3] = d2;
        uint64_t d4 = d3 - comb6_[4]; comb6_[4] = d3;
        uint64_t d5 = d4 - comb6_[5]; comb6_[5] = d4;
        double mid = (double)(int64_t)d5 * cicInvGain_;

        ring2_[ringPos2_] = mid;
        ring2_[ringPos2_ + L2_] = mid;
        const bool fire = R2PowerOf2_ ? ((midCount_++ & R2Mask_) == 0)
                                      : ((midCount_++ % (uint64_t)R2_) == 0);
        if (fire) {
            const double* rp = &ring2_[ringPos2_ + 1];
            const double* hp = h2_.data();
            double acc = 0.0;
            #pragma clang loop vectorize(enable)
            for (size_t k = 0; k < L2_; ++k) acc += hp[k] * rp[k];
            out.push_back(acc);
        }
        ringPos2_ = (ringPos2_ == 0) ? (L2_ - 1) : (ringPos2_ - 1);
    }

    inline void step_two_stage(bool bit, std::vector<double>& out) {
        uint64_t x = bit ? 1ull : (uint64_t)-1ll;
        integ6_[0] += x;
        integ6_[1] += integ6_[0];
        integ6_[2] += integ6_[1];
        integ6_[3] += integ6_[2];
        integ6_[4] += integ6_[3];
        integ6_[5] += integ6_[4];
        if ((inCount_++ & 3) == 0) {
            step_comb(out);
        }
    }

    inline void step_nibble(uint8_t nibble, std::vector<double>& out) {
        uint64_t x0 = (nibble & 8) ? 1ull : (uint64_t)-1ll;
        integ6_[0] += x0; integ6_[1] += integ6_[0]; integ6_[2] += integ6_[1];
        integ6_[3] += integ6_[2]; integ6_[4] += integ6_[3]; integ6_[5] += integ6_[4];

        step_comb(out);

        uint64_t x1 = (nibble & 4) ? 1ull : (uint64_t)-1ll;
        integ6_[0] += x1; integ6_[1] += integ6_[0]; integ6_[2] += integ6_[1];
        integ6_[3] += integ6_[2]; integ6_[4] += integ6_[3]; integ6_[5] += integ6_[4];

        uint64_t x2 = (nibble & 2) ? 1ull : (uint64_t)-1ll;
        integ6_[0] += x2; integ6_[1] += integ6_[0]; integ6_[2] += integ6_[1];
        integ6_[3] += integ6_[2]; integ6_[4] += integ6_[3]; integ6_[5] += integ6_[4];

        uint64_t x3 = (nibble & 1) ? 1ull : (uint64_t)-1ll;
        integ6_[0] += x3; integ6_[1] += integ6_[0]; integ6_[2] += integ6_[1];
        integ6_[3] += integ6_[2]; integ6_[4] += integ6_[3]; integ6_[5] += integ6_[4];

        inCount_ += 4;
    }

    inline void step(double v, std::vector<double>& out) {
        if (isTwoStage()) {
            step_two_stage(v > 0.0, out);
            return;
        }
        switch (cfg_.kind) {
            case IntegratorKind::Fir: {
                ring_[ringPos_] = v;
                if (inCount_ % (uint64_t)M_ == 0) {
                    const size_t L = h_.size();
                    double acc = 0.0;
                    size_t k = 0, i = ringPos_;
                    for (;;) {
                        acc += h_[k++] * ring_[i];
                        if (i == 0) break;
                        --i;
                    }
                    for (i = L - 1; k < L; --i) acc += h_[k++] * ring_[i];
                    out.push_back(acc);
                }
                ringPos_ = (ringPos_ + 1) % h_.size();
                break;
            }
            case IntegratorKind::Iir: {
                double x = v;
                for (size_t s = 0; s < iir_.size(); ++s) {
                    const Biquad& q = iir_[s];
                    const double y = q.b0 * x + z1_[s];
                    z1_[s] = q.b1 * x - q.a1 * y + z2_[s];
                    z2_[s] = q.b2 * x - q.a2 * y;
                    x = y;
                }
                if (inCount_ % (uint64_t)M_ == 0) out.push_back(x);
                break;
            }
            case IntegratorKind::Cic: {
                // ±1 用无符号 64 位表示：回绕是定义行为，且会在梳状差分里抵消
                uint64_t x = (v > 0.0) ? 1ull : (uint64_t)-1ll;
                for (auto& s : integ_) { s += x; x = s; }
                if (inCount_ % (uint64_t)M_ == 0) {
                    uint64_t c = x;
                    for (auto& s : comb_) { const uint64_t prev = s; s = c; c = c - prev; }
                    const double y = (double)(int64_t)c / cicGain_;
                    if (comp_.empty()) {
                        out.push_back(y);
                    } else {
                        compRing_[compPos_] = y;
                        const size_t L = comp_.size();
                        double acc = 0.0;
                        size_t k = 0, i = compPos_;
                        for (;;) {
                            acc += comp_[k++] * compRing_[i];
                            if (i == 0) break;
                            --i;
                        }
                        for (i = L - 1; k < L; --i) acc += comp_[k++] * compRing_[i];
                        out.push_back(acc);
                        compPos_ = (compPos_ + 1) % L;
                    }
                }
                break;
            }
        }
        ++inCount_;
    }

    IntegratorConfig cfg_;
    uint32_t M_ = 1;
    double   stopbandHz_ = 0.0;
    double   iirFc_ = 0.0;
    int      cicStages_ = 0;
    double   cicGain_ = 1.0;

    // FIR
    std::vector<double> h_;
    std::vector<double> ring_;
    size_t   ringPos_ = 0;
    uint64_t inCount_ = 0;

    // IIR
    std::vector<Biquad> iir_;
    std::vector<double> z1_, z2_;

    // CIC
    std::vector<uint64_t> integ_, comb_;
    std::vector<double> comp_;
    std::vector<double> compRing_;
    size_t compPos_ = 0;

    // 两级抽取（CIC 4x + 短 FIR）
    uint32_t R1_ = 4;
    uint32_t R2_ = 1;
    uint32_t R2Mask_ = 0;
    bool     R2PowerOf2_ = true;
    uint32_t fmid_ = 0;
    double   cicInvGain_ = 1.0;
    uint64_t integ6_[6] = {0};
    uint64_t comb6_[6] = {0};
    std::vector<double> h2_;
    std::vector<double> ring2_;
    size_t   ringPos2_ = 0;
    size_t   L2_ = 0;
    uint64_t midCount_ = 0;
};

} // namespace rta::dsp
