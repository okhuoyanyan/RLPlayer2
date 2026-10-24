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

// ---------- synthesizeNTF 基础件（2026-10-25，Schreier 式 H∞ 受控综合） ----------
// 三个设计目标都可客观验证：
//   1. H∞（NTF 全频段最大增益，线性幅度）命中目标 —— 由 Butterworth corner 二分保证；
//   2. 带内噪声功率 ∫₀^fB |NTF(f)|²df 最小 —— 零点逐对黄金分割 + 坐标轮换数值优化；
//   3. 任意阶 1..8：B/D/P 长度恒为 order+1（修掉旧实现 order∉{5,7} 的越界读）。

// NTF 在归一化频率（1.0 = Nyquist）上的线性增益
inline double ntf_gain_lin(const NtfPoly& n, double freqNorm) {
    const double w = kPi * freqNorm;
    std::complex<double> num(0.0, 0.0), den(0.0, 0.0);
    for (size_t k = 0; k < n.b.size(); ++k) num += n.b[k] * std::polar(1.0, -w * (double)k);
    for (size_t k = 0; k < n.d.size(); ++k) den += n.d[k] * std::polar(1.0, -w * (double)k);
    return std::abs(num) / (std::abs(den) + 1e-300);
}

// H∞ = max |NTF|（0 < f ≤ Nyquist 网格；NTF(0) 对奇数阶为 0，不必含端点）
inline double ntf_hinf_lin(const NtfPoly& n, int grid = 2048) {
    double m = 0.0;
    for (int i = 1; i <= grid; ++i)
        m = std::max(m, ntf_gain_lin(n, (double)i / (double)grid));
    return m;
}

// 带内噪声功率（线性功率，非 dB；梯形积分意义下的矩形上和，df 足够小即可比较用）
inline double ntf_inband_power(const NtfPoly& n, double fs, double fBandHz, int grid = 1024) {
    const double df = fBandHz / (double)grid;
    double sum = 0.0;
    for (int i = 1; i <= grid; ++i) {
        const double g = ntf_gain_lin(n, (i * df) / (fs * 0.5));
        sum += g * g;
    }
    return sum * df;
}

// 由零点角频率（rad/sample，每对）组装 B(z)。奇数阶带 1 个 DC 零点。
// 结果长度恒为 order+1 —— 这一条就是旧实现越界读的根治。
inline void ntf_build_b(int order, const std::vector<double>& omegas, std::vector<double>& bOut) {
    std::vector<long double> num;
    if (order & 1) num = { 1.0L, -1.0L };
    else           num = { 1.0L };
    for (double w : omegas) {
        const long double c = std::cos((long double)w);
        std::vector<long double> nxt(num.size() + 2, 0.0L);
        for (size_t j = 0; j < num.size(); ++j) {
            nxt[j]     += num[j];
            nxt[j + 1] += num[j] * (-2.0L * c);
            nxt[j + 2] += num[j];
        }
        num.swap(nxt);
    }
    bOut.assign(num.begin(), num.end());
}

// D(z) 的最大根半径（Durand-Kerner，阶 ≤ 8 足够）。★ 为什么必须查：
// corner 远小于 fBand 时，双线性 Butterworth 分母退化为 (1-z^-1)^order 的
// 近似式 —— order 重根恰在单位圆上，任何 ±0.01 量级的系数扰动都会把根打散
// 到圆外一半 ⟹ 误差反馈环的 w 递推发散（实测 EC-7@DSD256：maxW→∞、
// u 钉在 ±1.5、overload 占 58%）。这就是旧"7 阶 1-bit 不稳定"的真根因，
// 与 1-bit 非线性无关。守卫阈值 0.99 实测对应 corner≳(3~5)×fBand。
inline double ntf_max_root_radius(const std::vector<double>& a) {
    const size_t n = a.size() - 1;
    if (n == 0) return 0.0;
    std::vector<std::complex<double>> r(n);
    for (size_t i = 0; i < n; ++i) r[i] = std::polar(0.55 + 0.013 * (double)i, 0.71 * (double)i + 0.4);
    for (int it = 0; it < 400; ++it) {
        double delta = 0.0;
        for (size_t i = 0; i < n; ++i) {
            std::complex<double> num = a[0];
            for (size_t k = 1; k <= n; ++k) num = num * r[i] + a[k];
            std::complex<double> den(1.0, 0.0);
            for (size_t j = 0; j < n; ++j) if (j != i) den *= (r[i] - r[j]);
            const std::complex<double> step = num / (den + std::complex<double>(1e-300, 0.0));
            r[i] -= step;
            delta = std::max(delta, std::abs(step));
        }
        if (delta < 1e-13) break;
    }
    double m = 0.0;
    for (auto& z : r) m = std::max(m, std::abs(z));
    return m;
}

// H∞ 受控的 NTF 综合：
//   极点：双线性 Butterworth 高通（复用 design_ntf_poly 的分母构造），corner 由
//         二分确定使 H∞ = hinf。左支单调（corner 越小整形越激进、H∞ 越大），
//         先粗扫定位 H∞ 首次降到预算内的 corner，再在其左邻区间二分。
//   零点：在极点固定下逐对黄金分割最小化带内功率，多轮坐标轮换；零点动了 H∞ 会
//         漂移，所以「二分 corner ↔ 优化零点」交替至收敛。
// 运行成本：一次设计 < 0.1 s（每流一次，可忽略）。
inline NtfPoly synthesize_ntf(int order, double fs, double fBandHz, double hinf) {
    if (order < 1) order = 1;
    if (order > 8) order = 8;
    if (!(hinf >= 1.05)) hinf = 1.05;   // H∞=1 即全通、无整形；目标过低时夹回安全下限
    if (hinf > 3.0) hinf = 3.0;         // Lee 判据安全区上界（1-bit 实际建议 ≤1.6）
    const double wBand = 2.0 * kPi * fBandHz / fs;
    const int pairs = order / 2;

    // 零点初值：5/7 阶沿用旧 EC 的手调比例（它们本就接近最优），其它阶均匀铺
    std::vector<double> om((size_t)(pairs > 0 ? pairs : 0), 0.0);
    if (order == 5)      { const double t[2] = { 0.655, 0.942 }; for (int k = 0; k < 2 && k < pairs; ++k) om[(size_t)k] = t[k]; }
    else if (order == 7) { const double t[3] = { 0.468, 0.778, 0.974 }; for (int k = 0; k < 3 && k < pairs; ++k) om[(size_t)k] = t[k]; }
    else for (int k = 0; k < pairs; ++k) om[(size_t)k] = (2.0 * (double)(k + 1)) / (2.0 * (double)pairs + 1.0);
    for (auto& v : om) v *= wBand;

    auto compose = [&](double cornerHz, const std::vector<double>& omegas) {
        NtfPoly out = design_ntf_poly(order, cornerHz, fs);   // 只要它的 Butterworth 分母 d
        ntf_build_b(order, omegas, out.b);
        const size_t n = (size_t)order + 1;
        out.p.assign(n, 0.0);
        for (size_t i = 0; i < n; ++i) out.p[i] = out.b[i] - out.d[i];
        return out;
    };
    auto hinfAt = [&](double cornerHz, const std::vector<double>& omegas) {
        return ntf_hinf_lin(compose(cornerHz, omegas));
    };
    // corner 二分命中 hinf。H∞(corner) 是 U 形（2026-10-25 实测 DSD64：
    // corner=20k(≈fBand) 处谷底 H∞≈1.07，两侧上升），而带内功率 J 随 corner
    // 增大单调下降（corner=100k 时 J=1e-5，corner=20k 时 J=57）——预算买的
    // 是【右支】上最大的可用 corner（不可用 = H∞ 超预算 或 D 根贴单位圆）。
    auto fitCorner = [&](const std::vector<double>& omegas) {
        const double lo = std::max(fBandHz / 20.0, 1.0), hi = fs * 0.45;
        // 记录【最右侧】的 ok→not-ok 边界（左支超预算段直接跳过）：
        double lastOk = -1.0, bA = -1.0, bB = -1.0;
        // 兜底：预算过低时（如 hinf=1.1 在根安全区内无解），退到根安全区内
        // H∞ 最小的 corner —— 绝不能返回 hi（0.45fs：极点贴 z=−1，环路发散）。
        double cBestH = -1.0, bestH = 1e300;
        for (int i = 0; i <= 48; ++i) {
            const double c = std::exp(std::log(lo) + (std::log(hi) - std::log(lo)) * (double)i / 48.0);
            NtfPoly t = compose(c, omegas);
            const double root = ntf_max_root_radius(t.d);
            const double g = ntf_hinf_lin(t);
            if (root <= 0.99 && g < bestH) { bestH = g; cBestH = c; }
            if (root <= 0.99 && g <= hinf) {
                lastOk = c;
            } else if (lastOk > 0.0) {
                bA = lastOk; bB = c;   // 边界，继续向右扫（取最右可用区）
            }
        }
        if (bA < 0.0) return cBestH > 0.0 ? cBestH : hi;  // 无右边界：预算不可达退最小 H∞ 安全点
        // 可用 = H∞≤预算 且 D 根安全（两条都随 corner 增大恶化，右边界处二分有效）
        auto isOk = [&](double c) {
            NtfPoly t = compose(c, omegas);
            return ntf_hinf_lin(t) <= hinf && ntf_max_root_radius(t.d) <= 0.99;
        };
        double a = bA, b = bB;
        for (int it = 0; it < 60; ++it) {
            const double m = std::exp(0.5 * (std::log(a) + std::log(b)));
            if (!isOk(m)) b = m; else a = m;
            if (b / a < 1.000001) break;
        }
        return a;
    };

    auto with_omega = [](std::vector<double> v, size_t k, double w) { v[k] = w; return v; };
    // 从给定零点初值完整跑「零点优化 ↔ corner 重配」交替，返回最终零点。
    // ★ 多起点理由：坐标轮换+黄金分割只有局部收敛性，实测单起点在不同
    //   fs/预算组合下会落入不同局部极小（DSD128@1.5 单起点 SNR 79.6 vs
    //   换起点 92.7）；起点候选取「手调比例」与「均匀铺」两条，按最终带内
    //   功率 J 择优。
    auto optimizeFrom = [&](std::vector<double> om) {
        double corner = fitCorner(om);
        double prevJ = 1e300;
        for (int round = 0; round < 8; ++round) {
            for (int sweep = 0; sweep < 3; ++sweep) {
                for (int k = 0; k < pairs; ++k) {
                    double a = 0.10 * wBand, bM = 0.999 * wBand;   // 下界 0.1wB：低频已由 DC 零点覆盖，零点塌向 DC 无意义
                    const double gr = 0.6180339887498949;
                    double x1 = bM - gr * (bM - a), x2 = a + gr * (bM - a);
                    double f1 = ntf_inband_power(compose(corner, with_omega(om, (size_t)k, x1)), fs, fBandHz);
                    double f2 = ntf_inband_power(compose(corner, with_omega(om, (size_t)k, x2)), fs, fBandHz);
                    for (int it = 0; it < 48; ++it) {
                        if (f1 < f2) { bM = x2; x2 = x1; f2 = f1; x1 = bM - gr * (bM - a);
                                       f1 = ntf_inband_power(compose(corner, with_omega(om, (size_t)k, x1)), fs, fBandHz); }
                        else         { a = x1; x1 = x2; f1 = f2; x2 = a + gr * (bM - a);
                                       f2 = ntf_inband_power(compose(corner, with_omega(om, (size_t)k, x2)), fs, fBandHz); }
                    }
                    om[(size_t)k] = 0.5 * (a + bM);
                }
            }
            const double J = ntf_inband_power(compose(corner, om), fs, fBandHz);
            const bool done = (prevJ < 1e299 && (prevJ - J) / prevJ < 1e-4);
            prevJ = J;
            if (done) break;
            corner = fitCorner(om);     // 零点动了 H∞ 漂移，重新命中预算
        }
        return om;
    };

    // 两个起点各自优化，按最终带内功率择优
    std::vector<double> omA = om;                       // 起点 1：手调比例（5/7 阶）或均匀铺
    std::vector<double> omB((size_t)(pairs > 0 ? pairs : 0), 0.0);
    for (int k = 0; k < pairs; ++k) omB[(size_t)k] = (2.0 * (double)(k + 1)) / (2.0 * (double)pairs + 1.0) * wBand;
    omA = optimizeFrom(omA);
    omB = optimizeFrom(omB);
    // ★ 无论哪条路径胜出，都必须用【最终零点】重配 corner 再组合一次：
    //   否则返回的是旧零点时代的 corner，实测 H∞ 可漂出预算 4 倍（6.17 vs 1.4），
    //   表现为 7 阶 1-bit 环路大过载（overload≈17 万次、SNR 2 dB）。
    const double cornerA = fitCorner(omA);
    const double cornerB = fitCorner(omB);
    const double JA = ntf_inband_power(compose(cornerA, omA), fs, fBandHz);
    const double JB = ntf_inband_power(compose(cornerB, omB), fs, fBandHz);
    return (JA <= JB) ? compose(cornerA, omA) : compose(cornerB, omB);
}

// EC 变体（2026-10-25 重做）：H∞ 受控 + 带内零点数值优化的 NTF 综合。
// 旧实现（写死 5/7 阶两组零点 + 固定 fcHz 的 Butterworth 极点）有两个问题：
//   ① order∉{5,7} 时 b 只有 2 个系数而 p 循环 order+1 次 ⟹ 越界读（UB）；
//   ② 极点 corner 不随 H∞ 预算自适应，7 阶 1-bit 的稳定性全靠"fs≥5.6M 才放行"硬门。
// 新实现见 synthesize_ntf；fcHz 参数保留仅为兼容旧调用（不再起作用）。
inline NtfPoly design_ec_ntf_poly(int order, double fcHz, double fs, double fBandHz = 20000.0,
                                  double hinf = 1.5) {
    (void)fcHz;
    return synthesize_ntf(order, fs, fBandHz, hinf);
}

// 该采样率下【实际可用】的调制器阶数（含降阶保护）
inline int stable_sdm_order(int requested, double fs, int levels = 2, ModulatorFamily family = ModulatorFamily::Dsd) {
    if (requested <= 5) return requested < 1 ? 1 : requested;
    // 1-bit 模式保护：
    // 1. 7 阶 Chebyshev NTF 因极点阻尼比过低（Q≈130），小信号下闭环极点易移出单位圆触发极限环失稳（底噪暴增至负 SNR）；
    // 2. 标准 Butterworth 7 阶在 DSD64 (< 5.6 MHz) 下因量化步长大易过载饱和；
    // 均自动降为 5 阶保护
    if (requested >= 7 && levels == 2) {
        if (family == ModulatorFamily::Chebyshev || fs < 5.6e6) return 5;
    }
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
    // EC 族 NTF 的 H∞ 预算（线性幅度，非 dB）。Lee 判据：1-bit 建议 1.2~1.6，
    // 多电平可放宽到 2.5+。synthesize_ntf 用 Butterworth corner 二分命中该值；
    // 预算越低环路越稳、带内整形越弱，反之亦然。cutoffHz 对 Ec 族不再起作用
    // （corner 完全由该预算决定），保留给 Dsd/Chebyshev 族。
    // 默认 1.35 的实测依据（2026-10-25，probe8 稳态 0.62 幅度 + 满幅瞬态 + 单样本
    // +5.0 脉冲）：1.35 时 EC-5@DSD64 与 EC-7@DSD256 稳态过载增量均为 0；1.40 时
    // 7 阶出现 26 次轨饱和、1.50 时 5.9 万次。SNR 全线（DSD64 105 / DSD256 105 /
    // DSD512 121~127 dB）且跨速率方差最小。
    double ntfHinf      = 1.35;
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
        if (effectiveOrder_ >= 7 && levels_ == 2) {
            if (cfg_.family == ModulatorFamily::Chebyshev) {
                effectiveOrder_ = 5;
                steppedDown_ = true;
                stepDownReason_ = "1-bit 模式下 7 阶 Chebyshev NTF 极点阻尼比过低（Q≈130），小信号下闭环极点易移出单位圆形成极限环，自动降为 5 阶保护";
            } else if (cfg_.fs < 5.6e6) {
                effectiveOrder_ = 5;
                steppedDown_ = true;
                stepDownReason_ = "采样率 (" + std::to_string((int)(cfg_.fs / 1000.0)) +
                                  " kHz) 不足 5.6 MHz，7 阶 1-bit 环路易饱和，自动降为 5 阶保护";
            }
        }

        if (cfg_.family == ModulatorFamily::Chebyshev) {
            ntf_ = design_chebyshev_ntf_poly(effectiveOrder_, cfg_.cutoffHz, cfg_.fs, cfg_.oobGainDb);
        } else if (cfg_.family == ModulatorFamily::Ec || cfg_.ecCorrection) {
            // 2026-10-25：H∞ 受控综合（synthesize_ntf）。旧实现固定 fcHz=100k 的极点
            // 没有 H∞ 约束，7 阶 1-bit 稳定性只能靠 fs≥5.6M 的硬门；新设计的 corner
            // 由预算自适应，实测 DSD256 下 EC-7 满幅/瞬态/长跑判据全绿（见
            // tests/test_modulator_families.cpp 第 3/4 节）。
            ntf_ = synthesize_ntf(effectiveOrder_, cfg_.fs, 20000.0, cfg_.ntfHinf);
            hinfUsed_ = ntf_hinf_lin(ntf_);
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
    // EC 族综合后实际命中的 H∞（其它族 = -1）。供测试/信号链核对设计预算有没有被
    // 二分过程打穿（正常应 ≈ cfg.ntfHinf，偏差 < 0.05）。
    double ntf_hinf_used() const { return hinfUsed_; }
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

        // ★ 抖动注入点（2026-10-25 修复，探针实测定位）：
        // 抖动必须加在【比较器输入】（去相关量化噪声、打散空闲音），但必须从
        // 【反馈误差】中扣除。旧实现 e = y - u（含抖动），代数上
        //   Y(z) = X(z) + D(z) + NTF(z)·E(z)
        // ——抖动项 D 以增益 1 直达输出、完全未被整形。实测后果：ditherLsb=0.0005
        // 把 EC-7@DSD256 的带内底噪从 -123.9 dB 抬到 -92.2 dB，且与阶数/零点优化
        // 完全无关（所有 1-bit 族卡在 ~86 dB、每倍采样率恰 +3 dB 的白噪声签名）。
        // 正确写法：u = x + w + d 参与量化，e = y - (u - d) = y - u + d 进历史 ⟹
        //   Y(z) = X(z) + NTF(z)·(E + D)，抖动与量化噪声一样被 NTF 整形。
        double dith = 0.0;
        if (cfg_.ditherLsb > 0.0) {
            const double step = (levels_ == 2) ? 2.0 : 2.0 / (double)(levels_ - 1);
            dith = cfg_.ditherLsb * step * ((double)(rngNext() >> 11) / 9007199254740992.0 - 0.5);
            u += dith;
            if (u >  uLim) { u =  uLim; ++overload_; }
            if (u < -uLim) { u = -uLim; ++overload_; }
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
        e += dith;   // ★ 抖动只进比较器不进误差 ⟹ 进历史前补回（见上方注入点注释；钳位语义不变）

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
    double hinfUsed_ = -1.0;   // EC 族：综合后实测 H∞；其它族保持 -1
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
