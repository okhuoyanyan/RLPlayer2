// dynamics.h — 音频体检 B1a：纯算法动态分析器（TT DR / BS.1770-4 LUFS / EBU R128 LRA / 样本峰值）
//
// 用途：对已在内存里的 PCM（double、标称 [-1,1)、任意声道布局）做一次离线体检，
//       不开音频设备、不解码文件。B1b 在此之上接文件解码侧。
//
// 设计要点（三条指标全部按公开规范实现，不自造口径）：
//  1) TT DR：每声道切 3 秒块（尾部不足一块丢弃），块 RMS 降序取前 20% 的块，把这些块的
//     【样本合并】求二阶 RMS₂，DR_ch = 20·log10(全轨峰值 / RMS₂)，结果取各声道较低者。
//     ★ 合并样本的 RMS ≠ 块 RMS 的平均：块 RMS 平均的平方是均值的平方，合并样本 RMS 的平方
//       是均方本身，正弦信号（各块等幅）下两者差 10·log10(π/2·...) ≈ 3 dB 级别 ——
//       这是该口径最常见的实现错误，会把稳态正弦测成 ~0 dB。
//  2) LUFS：ITU-R BS.1770-4。K 加权 = 高频架（模拟原型约 1681.97 Hz、+4.0 dB）级联
//     RLB 高通（约 38.1 Hz），双二阶实现；系数以 48 kHz 规范表为锚点、按 Tustin
//     （s = 2fs·(1−z⁻¹)/(1+z⁻¹)，预畸变内含）重标定到任意 fs —— fs=48k 时反演→正演
//     是恒等变换，系数与规范表一致（test_dynamics 实测全频点 |Δ响应| < 1e-6 dB）。
//     注意：任务书把两级参数写串了（"一阶高通 1681.97 级联高架 3819.6"）；规范表里
//     1681.97 Hz 属于高频架、38 Hz 级属于高通，本实现按 BS.1770-4 表 1 的规范系数做，
//     官方校准点（997 Hz / −23 dBFS / 立体声）实测 −23.0 LUFS ±0.1（见 tests/test_dynamics.cpp）。
//     分块 400 ms、步进 100 ms（75% 交叠）；块响度 = −0.691 + 10·log10(Σ_ch G_i·z_i)；
//     绝对门 −70 LUFS，相对门 =（门内初算积分 −10 LU）再积分。
//  3) LRA：EBU Tech 3342。3 秒短时窗、步进 100 ms；过绝对门 −70 与相对门（积分 −20 LU）后
//     取 P95 − P10（升序线性插值百分位）。
//
// 已知局限（详见 docs/WIP-B1a.md）：
//   - 声道权重 G_i 只实现了立体声 L/R = 1.0，多声道一律按 1.0（BS.1770 的 Ls/Rs = 1.41 未做）；
//   - 素材 < 3 秒时 DR/LRA 口径不成立（返回 0）；< 400 ms 时整体 valid=false。
#pragma once
#include "../dsp/matrix.h"   // Biquad（直接型转置 II）
#include "../dsp/kaiser.h"   // kPi
#include <vector>
#include <algorithm>
#include <functional>
#include <cmath>
#include <cstddef>

namespace rta::analysis {

struct DynamicsResult {
    double drDb = 0.0;           // TT DR（各声道取较低者）
    double lufsIntegrated = 0.0; // BS.1770-4 K 加权积分响度
    double lra = 0.0;            // EBU R128 短期响度 10~95 百分位差
    double peakDbfs = 0.0;       // 样本峰值
    bool   valid = false;
};

// ---- K 加权锚点：ITU-R BS.1770-4 表 1 的 48 kHz 规范系数 ----
// 所有采样率的系数都从这组数出发重标定，保证 48k 下与规范逐位一致。
inline constexpr double kKWeight48ShelfB[3] = { 1.53512485958697, -2.69169618940638, 1.19839281085285 };
inline constexpr double kKWeight48ShelfA[3] = { 1.0, -1.69065929318241, 0.73248077421585 };
inline constexpr double kKWeight48HpB[3]    = { 1.0, -2.0, 1.0 };
inline constexpr double kKWeight48HpA[3]    = { 1.0, -1.99004745483398, 0.99007225036621 };

struct BiquadCoeffs { double b0, b1, b2, a1, a2; };

// 数字 z⁻¹ 域 → 模拟 s 域（Tustin 反演）：s = 2fs(1−z⁻¹)/(1+z⁻¹) ⟹ z⁻¹ = (K−s)/(K+s)
// 分子/分母各化成 s 的二次式 N2 s² + N1 s + N0（K = 2·fs）
inline void digital_to_analog(const double b[3], const double a[3], double K,
                              double& N2, double& N1, double& N0,
                              double& D2, double& D1, double& D0) {
    N2 = b[0] - b[1] + b[2];
    N1 = 2.0 * K * (b[0] - b[2]);
    N0 = K * K * (b[0] + b[1] + b[2]);
    D2 = 1.0 - a[1] + a[2];
    D1 = 2.0 * K * (1.0 - a[2]);
    D0 = K * K * (1.0 + a[1] + a[2]);
}

// 模拟 s 域 → 数字 z⁻¹ 域（Tustin，s = K(1−z⁻¹)/(1+z⁻¹)，K = 2·fs；预畸变内含）
inline BiquadCoeffs analog_to_digital(double N2, double N1, double N0,
                                      double D2, double D1, double D0, double K) {
    const double kk = K * K;
    const double d0 = D2 * kk + D1 * K + D0;   // a0，归一化基准
    BiquadCoeffs c;
    c.b0 = ( N2 * kk + N1 * K + N0) / d0;
    c.b1 = (-2.0 * N2 * kk + 2.0 * N0) / d0;
    c.b2 = ( N2 * kk - N1 * K + N0) / d0;
    c.a1 = (-2.0 * D2 * kk + 2.0 * D0) / d0;
    c.a2 = ( D2 * kk - D1 * K + D0) / d0;
    return c;
}

// K 加权两级双二阶：以 48 kHz 规范系数为锚点重标定到 fs（fs=48k 时 roundtrip 恒等）
inline void k_weight_biquads(double fs, rta::dsp::Biquad& shelf, rta::dsp::Biquad& hp) {
    const double K48 = 2.0 * 48000.0, K = 2.0 * fs;
    double N2, N1, N0, D2, D1, D0;
    digital_to_analog(kKWeight48ShelfB, kKWeight48ShelfA, K48, N2, N1, N0, D2, D1, D0);
    const BiquadCoeffs cs = analog_to_digital(N2, N1, N0, D2, D1, D0, K);
    shelf.set(cs.b0, cs.b1, cs.b2, 1.0, cs.a1, cs.a2);
    digital_to_analog(kKWeight48HpB, kKWeight48HpA, K48, N2, N1, N0, D2, D1, D0);
    const BiquadCoeffs ch = analog_to_digital(N2, N1, N0, D2, D1, D0, K);
    hp.set(ch.b0, ch.b1, ch.b2, 1.0, ch.a1, ch.a2);
}

// 升序线性插值百分位（EBU Tech 3342 的 P10/P95 口径）
inline double percentile_sorted(const std::vector<double>& asc, double q) {
    if (asc.empty()) return 0.0;
    const double pos = q * (double)(asc.size() - 1);
    const size_t i = (size_t)pos;
    const size_t j = (i + 1 < asc.size()) ? i + 1 : i;
    const double t = pos - (double)i;
    return asc[i] * (1.0 - t) + asc[j] * t;
}

inline DynamicsResult measure_dynamics(const double* const* ch, int channels, size_t n, double fs) {
    DynamicsResult r;
    if (!ch || channels <= 0 || n == 0 || fs <= 0.0) return r;

    // ---- 样本峰值（全轨全声道）----
    double peak = 0.0;
    for (int c = 0; c < channels; ++c) {
        if (!ch[c]) continue;
        for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(ch[c][i]));
    }
    r.peakDbfs = (peak > 0.0) ? 20.0 * std::log10(peak) : -300.0;

    // ---- TT DR：3 秒块，前 20% 块合并样本求 RMS₂（块等长 ⟺ 选中块均方的均值开根）----
    const size_t drLen = (size_t)std::llround(3.0 * fs);
    if (drLen > 0 && n >= drLen) {
        const size_t nBlocks = n / drLen;
        const size_t nSel = std::max<size_t>(1, nBlocks / 5);   // 前 20%（至少 1 块）
        bool drInit = false;
        for (int c = 0; c < channels; ++c) {
            if (!ch[c]) continue;
            std::vector<double> ms(nBlocks);
            for (size_t b = 0; b < nBlocks; ++b) {
                const double* x = ch[c] + b * drLen;
                double s = 0.0;
                for (size_t i = 0; i < drLen; ++i) { const double v = x[i]; s += v * v; }
                ms[b] = s / (double)drLen;
            }
            double peakCh = 0.0;
            for (size_t i = 0; i < n; ++i) peakCh = std::max(peakCh, std::fabs(ch[c][i]));
            // 降序取前 20%，把这些块的【样本】合并求二阶 RMS（不是块 RMS 的平均，见文件头）
            std::sort(ms.begin(), ms.end(), std::greater<double>());
            double meanSq2 = 0.0;
            for (size_t b = 0; b < nSel; ++b) meanSq2 += ms[b];
            meanSq2 /= (double)nSel;
            const double rms2 = std::sqrt(meanSq2);
            // 全静音声道（峰值 0）对"取较低者"没有信息量，不参与，避免 −inf 污染
            if (rms2 > 0.0 && peakCh > 0.0) {
                const double drCh = 20.0 * std::log10(peakCh / rms2);
                r.drDb = drInit ? std::min(r.drDb, drCh) : drCh;
                drInit = true;
            }
        }
    }

    // ---- K 加权整轨滤波（每声道独立滤波状态）----
    rta::dsp::Biquad shelfProto, hpProto;
    k_weight_biquads(fs, shelfProto, hpProto);
    std::vector<std::vector<double>> y((size_t)channels);
    for (int c = 0; c < channels; ++c) {
        if (!ch[c]) continue;
        y[(size_t)c].resize(n);
        rta::dsp::Biquad s = shelfProto, h = hpProto;
        for (size_t i = 0; i < n; ++i) y[(size_t)c][i] = h.process(s.process(ch[c][i]));
    }

    // ---- 积分响度：400 ms 块 / 100 ms 步进（75% 交叠），绝对门 −70 + 相对门（积分−10）----
    const size_t blk = (size_t)std::llround(0.4 * fs);
    const size_t stp = (size_t)std::llround(0.1 * fs);
    if (blk == 0 || stp == 0) return r;
    std::vector<double> blkPow;   // 每块 Σ_ch G_i·z_i（线性功率；G_i=1.0，见文件头局限）
    for (size_t start = 0; start + blk <= n; start += stp) {
        double s = 0.0;
        for (int c = 0; c < channels; ++c) {
            if (y[(size_t)c].empty()) continue;
            const double* x = y[(size_t)c].data() + start;
            double e = 0.0;
            for (size_t i = 0; i < blk; ++i) e += x[i] * x[i];
            s += e / (double)blk;
        }
        blkPow.push_back(s);
    }
    if (blkPow.empty()) return r;
    r.valid = true;

    // 块响度 −0.691 + 10log10(p) > −70 ⟺ p > 10^((−70+0.691)/10)
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double ungated = 0.0; size_t nU = 0;
    for (double p : blkPow) if (p > absGate) { ungated += p; ++nU; }
    if (nU == 0) { r.lufsIntegrated = -300.0; return r; }   // 全素材低于绝对门：测不出
    const double relGate = (ungated / (double)nU) / 10.0;   // （门内初算积分 −10 LU）⟹ 功率 ÷10
    double gated = 0.0; size_t nG = 0;
    for (double p : blkPow) if (p > absGate && p > relGate) { gated += p; ++nG; }
    r.lufsIntegrated = (nG > 0) ? (-0.691 + 10.0 * std::log10(gated / (double)nG)) : -300.0;

    // ---- LRA：3 秒短时窗 / 100 ms 步进，绝对门 −70 + 相对门（积分−20），P95 − P10 ----
    const size_t w3 = (size_t)std::llround(3.0 * fs);
    if (w3 > 0 && n >= w3) {
        const double relGateL = std::pow(10.0, (r.lufsIntegrated - 20.0 + 0.691) / 10.0);
        std::vector<double> stL;
        for (size_t start = 0; start + w3 <= n; start += stp) {
            double s = 0.0;
            for (int c = 0; c < channels; ++c) {
                if (y[(size_t)c].empty()) continue;
                const double* x = y[(size_t)c].data() + start;
                double e = 0.0;
                for (size_t i = 0; i < w3; ++i) e += x[i] * x[i];
                s += e / (double)w3;
            }
            const double l = -0.691 + 10.0 * std::log10(s + 1e-300);
            if (l > -70.0 && s > relGateL) stL.push_back(l);
        }
        if (stL.size() >= 2) {
            std::sort(stL.begin(), stL.end());
            r.lra = percentile_sorted(stL, 0.95) - percentile_sorted(stL, 0.10);
        }
    }
    return r;
}

} // namespace rta::analysis
