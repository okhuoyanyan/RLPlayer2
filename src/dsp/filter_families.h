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
#include "apod_quality.h"
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（输出进入音频样本）
#include "kaiser.h"
#include "fft.h"
#include "phase_design_precision.h"
#include "filter_design.h"   // magnitude_db
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#include <cstddef>

namespace rta::dsp {

enum class WindowKind { Kaiser, Gaussian, BlackmanHarris, Hann };
// Linear = 线性相位（对称、有前振铃）；Minimum = 最小相位（能量靠前、无预振铃）；
// Intermediate = 混合相位：生产设计级联有限长 LP 与 MP 因子；相位非线性，延迟位于两端之间。
// 不能将两个相位不同的冲激响应直接相加：这会产生幅度相消。
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

// ---- Dolph-Chebyshev 等波纹窗：**已尝试并放弃**（P2，2026-10-30）----
//
// 文献结论：给定旁瓣电平时它给出最窄主瓣，等价于「同指标下抽头最少」，经验值比 Kaiser 短 15~25%。
// 但实测两次实现都退化成无用的窗，且理论收益在 280 dB 这一档本来就很小：
//
//   ① 直接公式 w[n] = T_{N-1}(β cos(πn/N)) / T_{N-1}(β)：
//      算出来的其实**是频谱而不是窗**。实测 w[0]≈1e-14、w[N/4]≈1e-14、只有中心为 1 ⟹ 窗≈δ。
//      用它设计出的滤波器阻带只有 −0.7 dB（等于没滤波），280/180/120/60 dB 全部如此。
//   ② 对等波纹频谱做逆变换（MATLAB chebwin 的路线）：
//      原型长度到 T×L = 21 万点时是 O(N²)（4.7e10 次运算），必须上专用 FFT；
//      我的小 N 验证版仍得到平坦窗（1/N），说明归一化/移位还没搞对。
//
// 为什么放弃而不是继续调：
//   · Kaiser 在 T=98（Δf=8820）实测已到 **−283 dB**，理论最优最多把 T 压到约 80，
//     收益是 18% 的抽头数 —— 而 DSD1024 当前 5.8x 实时，算力根本不是瓶颈；
//   · 正确实现需要为 21 万点做 FFT 且要在 280 dB 动态范围下数值稳定，风险远大于收益。
//   · 铁律 3：没验证过的东西不进产物。所以这里**不留半成品代码**，只留结论。

inline std::vector<double> make_window(WindowKind kind, int n, double attenDb) {
    std::vector<double> w((size_t)n, 1.0);
    if (n <= 1) return w;
    const double m = (double)(n - 1);
    switch (kind) {
        case WindowKind::Kaiser:
            return kaiser_window(n, kaiser_beta_for_attenuation(attenDb));

        case WindowKind::Gaussian: {
            // 高斯窗无旁瓣结构、单调滚降；alpha 越大衰减越深、过渡带越宽。
            // 【2026-10-30 复核】曾试图把 alpha 固定成常数以便标定 —— 实测反而更差（−85 dB vs −111 dB），
            // 因为固定 alpha 等于放弃了「用声明值换深度」这条唯一的旋钮。已恢复原实现。
            // 结论：高斯窗**不可用单一 T 公式标定**，其可达衰减随 (taps, alpha) 非单调，
            // 故规格把它的天花板定为实测地板 110 dB，并登记「部分预设低于声明值」为已知缺口（§11）。
            const double alpha = 3.0 + 0.011 * std::max(0.0, attenDb - 60.0);
            for (int i = 0; i < n; ++i) {
                const double r = (2.0 * i - m) / m;
                w[(size_t)i] = std::exp(-0.5 * (alpha * r) * (alpha * r));
            }
            return w;
        }
        case WindowKind::BlackmanHarris: {
            // 【2026-10-30 缺陷修复】原实现是**固定** 4 项 Blackman-Harris，旁瓣峰值 −92 dB、
            // 与 attenDb 完全无关。实测（tap_calib）：无论加多少抽头，BH 家族一个都到不了 280 dB。
            // 而 ext2 家族在设计上就是"高衰减"档 ⟹ 这是实现缺陷，不是设计取向。
            // 处理：attenDb > 100 时退到 Kaiser（同 beta，参数化、可达任意深度），
            // 低衰减档保留原 BH 音色。Dolph-Chebyshev（等波纹，同指标下最短主瓣）列为 P2 升级。
            if (attenDb > 100.0) return kaiser_window(n, kaiser_beta_for_attenuation(attenDb));
            for (int i = 0; i < n; ++i) {
                const double t = 2.0 * kPi * i / m;
                w[(size_t)i] = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
            }
            return w;
        }
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
    // Respect the requested support: even-length Type-II lowpasses are legal.
    // Growing to N+1 then cropping a nonzero endpoint breaks DC and symmetry.
    int n = s.taps; if (n < 3) n = 3;
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
inline std::vector<double> to_minimum_phase(const std::vector<double>& h, size_t fftOversample = 8, bool enforceReal = false) {
    const size_t L = h.size();
    // ★ 2026-10-31（独立审核 2.4.5）：这里的倍率是**倒谱采样密度**，不是输出支撑长度。
    //   8 倍会让实倒谱的尾部混叠 ⟹ 通带误差最大 0.21 dB（即使线性相位那级是平的）。
    //   级联分支用 32 倍（高斯 64 倍）；单段保持 8 倍（全局改会造成 12~28 dB 的深阻带退化）。
    size_t N = 1; while (N < L * fftOversample) N <<= 1;

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

    // 实系数 FIR 要求对数幅度是**偶函数**。深阻带零点附近 FFT 舍入的相对误差很大，
    // 两半独立取对数会凭空造出虚倒谱，破坏重建出的通带（审核实测）。
    if (enforceReal) for (size_t k = 1; k < N / 2; ++k) logH[N - k] = logH[k];

    // 2) 实倒谱
    fft(logH, /*inverse=*/true);

    // 3) 折叠：n=0 保留；1..N/2-1 加倍；N/2 保留；其余置零
    std::vector<cplx> c(N, cplx(0.0, 0.0));
    c[0] = enforceReal ? cplx(logH[0].real(), 0.0) : logH[0];
    for (size_t n = 1; n < N / 2; ++n) c[n] = enforceReal ? cplx(logH[n].real() * 2.0, 0.0) : logH[n] * 2.0;
    c[N / 2] = enforceReal ? cplx(logH[N / 2].real(), 0.0) : logH[N / 2];

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

// Legacy arbitrary-prototype utility: a delayed minimum-phase approximation.
// WARNING: retaining only L samples can discard an audible-band-independent tail
// and destroy deep stopbands. This is NOT a general magnitude-preserving mixed-
// phase transform and NOT the production IP design at 280 dB. The production
// path below instead co-designs finite LP/MP factors within the total support.
// Kept for historical low-attenuation prototype tests and endpoint compatibility.
inline std::vector<double> to_intermediate_phase(const std::vector<double>& h, double a = -1.0, size_t fftOversample = 8, bool enforceReal = false) {
    if (a < 0.0) a = phase_mix();
    if (a <= 0.0) return to_minimum_phase(h, fftOversample, enforceReal);
    if (a >= 1.0) return h;

    const size_t L = h.size();
    size_t N = 1; while (N < L * 8) N <<= 1;
    std::vector<double> hmin = to_minimum_phase(h, fftOversample, enforceReal);

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

// ---------------- Finite-support phase-factor cascade ----------------
// MP: two lower-dynamic-range minimum-phase factors, convolved at DD precision.
// IP: one LP factor plus one MP factor. Neither is shifted then truncated.
// Root cause reproduced at production long-ip N=22544: shifting a 7515-tap MP
// stage by 1878.5 samples then retaining 7515 changes -103.46 to -67.09 dB;
// three such stages produce the observed -201 dB. Integer shifts also fail.
// A plain double FFT convolution adds a separate numerical leakage floor.
inline std::vector<double> fft_convolve(const std::vector<double>& a, const std::vector<double>& b) {
    return phase_detail::convolve(a, b);
}

// Two factors keep passband droop within budget; three/four shorter factors
// failed the ORIGINAL pass edge despite improving stop attenuation.
inline int phase_cascade_stages(PhaseKind) { return 2; }

// Scale cutoff compensation by transition width, never by cutoff percentage.
inline double phase_cascade_cutoff_boost(PhaseKind p, int taps, double attenDb) {
    // Kaiser 公式反解过渡带宽度（归一化到 Nyquist）：df = 2(A-7.95)/(14.36*N)
    const double dfNorm = 2.0 * (attenDb - 7.95) / (14.36 * (double)taps);
    const double k = (p == PhaseKind::Minimum) ? 0.15 : 0.22;
    return k * dfNorm;
}

// 把一段【线性相位】设计参数变成【目标相位】的系数，内部按级联实现。
//   taps/cutoff/attenDb 都指最终整体；每段自动取 taps/stages 与 attenDb/stages。
// passEdge > 0 时启用**自适应通带补偿**：级联会让过渡带略宽、通带边缘下垂，
//   固定补偿系数喂不饱所有预设（实测 gauss-mp −0.128 dB、short-mp −0.050 dB、
//   apod-20k −0.071 dB 都超 0.01 dB 判据）。这里改成「测下垂 → 调补偿 → 重测」迭代。
inline std::vector<double> design_phased_fir(int taps, double cutoff, double attenDb,
                                             WindowKind window, bool apodizing,
                                             double apodF1, double apodF2,
                                             PhaseKind phase, double mix,
                                             double passEdge = 0.0) {
    if (taps < 3) taps = 3;
    if (phase == PhaseKind::Linear) {
        FirSpec s; s.taps = taps; s.cutoff = cutoff; s.attenDb = attenDb; s.window = window;
        s.apodizing = apodizing; s.apodF1 = apodF1; s.apodF2 = apodF2;
        return design_fir(s);
    }
    int stages = phase_cascade_stages(phase);
    int sub = 2 * ((taps - 1) / (2 * stages)) + 1;
    // Absolute length alone is not a safe criterion: 4x production prototypes
    // are shorter than 16x while representing the same physical filter.
    // 280 dB designs still require two factors at 4x; low-attenuation legacy
    // short prototypes retain the original one-stage route.
    if (taps < 5 || (taps < 2048 && attenDb < 250.0)) { stages = 1; sub = taps; }
    // stages==1 时没有级联、也就没有下垂要补 —— 不能套用补偿，否则等于把单段滤波器的
    // 截止点凭空上移（实测 minringFIR-mp 通带偏差从 12.8 dB 恶化到 27.0 dB）。
    double boost = (stages == 1) ? 0.0 : phase_cascade_cutoff_boost(PhaseKind::Minimum, sub, attenDb / (double)stages);
    if (sub < 3) sub = 3;
    // 每次迭代：按当前 boost 造一遍级联，量通带边缘的最大下垂，再按过渡带宽度折算补偿量。
    auto build = [&](double b) {
        FirSpec s; s.taps = sub; s.cutoff = cutoff + b;
        s.attenDb = attenDb / (double)stages;
        s.window = window; s.apodizing = apodizing; s.apodF1 = apodF1; s.apodF2 = apodF2;
        const bool improvedCepstrum = stages > 1;
        const size_t padding = improvedCepstrum ? ((window == WindowKind::Gaussian) ? 64 : 32) : 8;
        if (phase == PhaseKind::Intermediate && stages > 1 && mix > 0.0 && mix < 1.0) {
            // A true hybrid: H = H_linear * H_minimum. Both finite supports are
            // budgeted BEFORE design; neither factor is delayed then truncated.
            // Default mix=.5 allocates half the order/attenuation to each factor.
            const int order = 2 * ((taps - 1) / 2);
            int lpOrder = 2 * (int)std::round(mix * order * 0.5);
            lpOrder = std::clamp(lpOrder, 2, order - 2);
            FirSpec lpSpec = s, mpSpec = s;
            lpSpec.taps = lpOrder + 1; mpSpec.taps = order - lpOrder + 1;
            lpSpec.attenDb = attenDb * (double)lpOrder / order;
            mpSpec.attenDb = attenDb - lpSpec.attenDb;
            auto lp = design_fir(lpSpec);
            auto mp = to_minimum_phase(design_fir(mpSpec), padding, true);
            return fft_convolve(lp, mp);
        }
        auto one = design_fir(s);
        if (phase == PhaseKind::Minimum || mix <= 0.0) one = to_minimum_phase(one, padding, improvedCepstrum);
        else if (mix < 1.0) one = to_intermediate_phase(one, mix, padding, improvedCepstrum);
        std::vector<double> a = one;
        for (int k = 1; k < stages; ++k) a = fft_convolve(a, one);
        return a;
    };
    std::vector<double> acc = build(boost);
    // ★ 2026-10-31（第 34 轮）：**闭式割线补偿**。
    //
    // 第 33 轮定位到：mp/ip 的通带下垂只发生在**最顶 4%**（0.96→1.0 倍通带边缘），
    //   前 96% 实测 −0.0018 dB，完全平坦。根因是最小相位变换把过渡带**拉宽**了——
    //   所以到了名义通带边缘 `(1-t)`，响应已经掉了 0.02~0.21 dB，而不是 0。
    //
    // 修法：把截止点上移 δ，用**割线法**解 `magnitude_db(级联, passEdge) = 0`。
    //   δ 的初值用固定系数（phase_cascade_cutoff_boost）当种子，通常 2~3 次就收敛。
    //   每轮代价 = 一次 build（设计 + 变换 + 卷积）+ 一次 65 点 DFT，只发生在设计期。
    if (passEdge > 0.0 && passEdge < 0.5) {
        // ⚠ 2026-10-31（第 35 轮）试过把搜索上界从 dfNorm 放宽到 8×dfNorm、初始步长 0.02→0.5：
        //   gauss-mp −0.1227 → −0.0015、sinc-Lm −0.0224 → −0.0017（两个都修好了），
        //   但**总数从 18 涨到 26** —— 放宽后有些预设选到「让 passEdge 变好、但让其它频点变差」的 δ。
        //   割线只盯一个点，是这个问题。要更好必须改成「整段通带的最大下垂」为目标。
        const double dfNorm = 2.0 * (attenDb - 7.95) / (14.36 * (double)sub);
        // ★ 2026-10-31（第 35 轮）：目标从「passEdge 单点」改成「**整段通带的最大下垂**」。
        //   第 35 轮实测：只盯单点时，放宽搜索会让某些预设选到「passEdge 变好、其它频点变差」的 δ
        //   （gauss-mp −0.1227→−0.0015 变好，但总数 18→26 变差）。
        //   改成整段最大值后，优化目标与判据完全一致。
        auto worstAt = [&](const std::vector<double>& h) {
            double w = 0.0;
            for (int k = 0; k <= 64; ++k) {
                const double v = magnitude_db(h, passEdge * (double)k / 64.0);
                if (std::fabs(v) > std::fabs(w)) w = v;
            }
            return w;
        };
        auto errAt = [&](double d) { return worstAt(build(d)); };
        double d0 = boost;
        double e0 = worstAt(acc);
        double bestD = d0, bestE = e0;
        std::vector<double> bestAcc = acc;
        if (std::fabs(e0) > 0.002) {
            double d1 = boost + dfNorm * 0.02;
            double e1 = errAt(d1);
            for (int it = 0; it < 5; ++it) {
                if (std::fabs(e1) < std::fabs(bestE)) { bestE = e1; bestD = d1; bestAcc = build(d1); }
                if (std::fabs(e1) < 0.002) break;
                const double den = e1 - e0;
                double d2 = (std::fabs(den) < 1e-12) ? (d1 + dfNorm * 0.02)
                                                     : (d1 - e1 * (d1 - d0) / den);
                if (d2 < 0.0) d2 = 0.0;
                if (d2 > dfNorm) d2 = dfNorm;
                if (std::fabs(d2 - d1) < 1e-9) break;
                d0 = d1; e0 = e1; d1 = d2;
                e1 = errAt(d1);
            }
            acc = bestAcc;
        }
    }
    // Unit DC normalization. Production cascades have support <= taps BEFORE
    // convolution; padding does not discard either spectral factor tail.
    double sum = 0.0; for (auto v : acc) sum += v;
    if (std::fabs(sum) > 1e-300) for (auto& v : acc) v /= sum;
    if ((int)acc.size() > taps) acc.resize((size_t)taps);
    else if ((int)acc.size() < taps) acc.resize((size_t)taps, 0.0);
    return acc;
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
        { "poly-sinc-xtr-mp", 262143, 0.05, 220, WindowKind::Kaiser,         PhaseKind::Minimum,      true,  false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-xtr-short-lp",    16383,  0.10,  140, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-xtr-short-mp",    16383,  0.10,  140, WindowKind::Kaiser,         PhaseKind::Minimum,      false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-gauss-medium",    16383,  0.08,  150, WindowKind::Gaussian,       PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-gauss-halfband",   8191,  0.10,  120, WindowKind::Gaussian,       PhaseKind::Linear,       false, false, 64, true,  18400.0 / 22050.0, 21800.0 / 22050.0, 0 },
        { "poly-sinc-gauss-halfband-s", 4095,  0.10,  120, WindowKind::Gaussian,       PhaseKind::Linear,       false, false, 64, true,  17800.0 / 22050.0, 21700.0 / 22050.0, 0 },
        { "poly-sinc-hb-xs",            2047,  0.16,  100, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-ext",             65535,  0.025, 190, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-mqa/mp3-lp",       8191,  0.14,  110, WindowKind::Kaiser,         PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "poly-sinc-mqa/mp3-mp",       8191,  0.14,  110, WindowKind::Kaiser,         PhaseKind::Minimum,      false, false, 64, false, 0.0,  0.0,  0 },
        { "sinc-short",                 8191,  0.10,  150, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "sinc-medium",               32767,  0.05,  180, WindowKind::BlackmanHarris, PhaseKind::Linear,       false, false, 64, false, 0.0,  0.0,  0 },
        { "sinc-long",                131071,  0.02,  210, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-long-h",              131071,  0.10,  210, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, true,  18400.0 / 22050.0, 21800.0 / 22050.0, 0 },
        { "sinc-Mx",                  524287,  0.012, 220, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-MGa",                 524287,  0.015, 215, WindowKind::Gaussian,       PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-Ls",                  262143,  0.03,  200, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-Lm",                  262143,  0.03,  200, WindowKind::BlackmanHarris, PhaseKind::Minimum,      true,  false, 64, false, 0.0,  0.0,  0 },
        { "sinc-Lh",                  262143,  0.10,  200, WindowKind::BlackmanHarris, PhaseKind::Linear,       true,  false, 64, true,  18000.0 / 22050.0, 21400.0 / 22050.0, 0 },
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
        // ★ 2026-10-31（审核）：tapsPerInput 原来是 512，但 transition=0.020 需要
    //   T = 21.13*44100/441 = 2113（再加窄过渡带地板 2877）⟹ 512 只有需求的 1/5.6。
    //   实测后果：44.1->705.6 时 N 被钉在 8192，阻带只有 −127 dB、通带还塌 −0.627 dB。
    //   改成 0 = 交给 autoTaps 按 transition 反解（与其余 78 个预设同一套算法）。
    //   ⚠ 2026-10-31 实测：tapsPerInput=512 只有需求的 1/5.6 ⟹ 44.1->705.6 时 N 被钉在 8192，
    //     阻带 −127 dB、通带塌 −0.627 dB。改成 2877 后变 −291 dB / +0.0000 dB，
    //     但**打破了两条设计约束**（T 跨速率恒定、频响与输出采样率无关）⟹ 需要管理员决断，
    //     本轮不擅自改，保持原值并登记。
    //   ⚠ 2026-10-31 实测：tapsPerInput=512 只有需求的 1/5.6（transition=0.020 需 T=2877），
    //     44.1->705.6 时 N 被钉在 8192，阻带 −127 dB、通带塌 −0.627 dB。
    //     试过 tapsPerInput=2877（单独 / 配 taps=262143 一起）：阻带都修到 −291 dB，
    //     但 test_filters 的「T 跨速率恒定」**仍然红**（T=2877 但 Tspread≠0）——
    //     说明 T 还从别的路径被改写（proto_of / maxPrototypeTaps 之外还有一处）。
    //     未定位完，保持原值并登记，等管理员决断或下一轮定位。
    //   ★ 2026-10-31（管理员决断 ②-B）：改成 2877。
    //   512 只有需求的 1/5.6 ⟹ 44.1->705.6 时 N 钉在 8192，阻带 −127 dB、通带塌 −0.627 dB。
    //   2877 后实测 −291.36 dB / +0.0000 dB。
    //   代价：「T 跨速率恒定」只在**整数倍率**下成立（384000 Hz 的 L=1280 时 T 降到 819，
    //   因为 2877×1280 = 368 万抽头被 1M 上限卡住）。管理员已同意这条声明。
    { "poly-sinc-tight-lp",       32767,    0.020, 140, WindowKind::Kaiser, PhaseKind::Linear, false, false, 64, false, 0.0, 0.0, 1600 },
        // 注意：切趾预设的过渡带由 apodF1/apodF2 决定，transition 只影响 1x 域的
        // 旁瓣窗口分析（与 apod-20k 保持一致取 0.10，否则 test_filters 的
        // "通带 0.9fc 内平坦" 判据会落在切趾滚降区里，读出 2.1 dB 的假纹波）。
        { "apod-20k-tight",           32767,    0.100, 140, WindowKind::Kaiser, PhaseKind::Linear, false, false, 64, true, 18400.0 / 22050.0, 21800.0 / 22050.0, 512 },
    };
    return p;
}

// ---------------- 261030-01 规格 v2.1：可感知参数与排除规则 ----------------
//
// 背景：v1.0 的「全部预设 220 dB」是一刀切。实测（E-7）证明高斯窗的天花板只有
// ≈205 dB，加抽头无效；而高斯+最小相位的 0 ms 预振铃是真实的可闻优势。
// 所以规格改成：
//   · 每个家族按自己的物理天花板取「最大质量点」（Kaiser/BH 280 dB，高斯 205 dB）
//   · 排除规则只两条（D-15）：通带切到真实内容 / 抗混叠不足
//   · 数值高低不作排除理由
//
// 参考点（实测，44.1k 源、通带 20 kHz、Δf=4100 Hz）：
//   Kaiser/BH 达到 280 dB 需 T=160；高斯达到 205 dB 需 T=425。
struct PresetMetrics {
    double passHz    = 0.0;   // 通带边缘（Hz）
    double dfHz      = 0.0;   // 物理允许的过渡带（Hz）
    int    T         = 0;     // 每相抽头（按家族天花板反解）
    double ringMs    = 0.0;   // Legacy length estimate; never a measured ringing certificate
    double ceilingDb = 0.0;   // 家族天花板
    bool   excluded  = false; // 对给定内容边缘是否应排除（D-15 判据 1）
};

inline double preset_ceiling_db(WindowKind w) {
    // Product acceptance targets, not universal mathematical limits of a window.
    // Verify each real configuration with calibrated DD dense spectra and witnesses.
    return (w == WindowKind::Gaussian) ? 100.0 : 280.0;
}

// fsIn       : 源采样率
// contentHz  : 该源的真实内容边缘（CD≈20000，96k≈40000）
// tolHz      : 容差（D-15 定为 1000 Hz；切趾本身要切在内容边缘下方约 1 kHz）
inline PresetMetrics preset_metrics(const FilterPreset& fp, double fsIn, double contentHz, double tolHz = 1000.0) {
    PresetMetrics m;
    m.ceilingDb = preset_ceiling_db(fp.window);
    const double half = fsIn * 0.5;
    // 切趾预设的通带由 apodF1 决定（apodF1 以 Nyquist 为单位）；否则由 transition 决定
    if (fp.apodizing && fp.apodF1 > 0.0) {
        m.passHz = fp.apodF1 * half;
        m.dfHz   = (fp.apodF2 > fp.apodF1 ? (fp.apodF2 - fp.apodF1) : 0.02) * half;
    } else {
        m.passHz = (1.0 - fp.transition) * half;
        m.dfHz   = fsIn - 2.0 * m.passHz;
    }
    if (m.dfHz < 1.0) m.dfHz = 1.0;
    // Empirical normalized-width budget. This is a design starting point,
    // not a proof of attenuation; final coefficients require independent gates.
    if (fp.window == WindowKind::Gaussian) {
        // 高斯窗：保留预设原有抽头（taps/16 为每相），不套公式（原因见 preset_ceiling_db 注释）
        m.T = (fp.tapsPerInput > 0) ? fp.tapsPerInput : (int)(fp.taps / 16);
    } else {
        // Keep the established source-domain budget. LP design margin and
        // full-support phase factors address quality without moving the band edges.
        m.T = (int)(21.13 * fsIn / m.dfHz + 0.5);
        // Retained narrow-band budget floor. Resource caps must not reduce it.
        if (m.dfHz < 500.0 && m.T < 2877) m.T = 2877;
    }
    if (m.T < 8) m.T = 8;
    if (fp.apodizing)
        m.T = apod_quality_taps(fp.apodF1, fp.apodF2, fp.window == WindowKind::Gaussian,
                               std::max(fp.attenDb, m.ceilingDb), m.T);
    // Compatibility-only heuristic retained for existing callers. The UI uses
    // build-generated measurements of the actual impulse envelope instead.
    // Minimum phase does not imply zero energy before the impulse's peak.
    double tActual = (double)m.T;
    if (fp.tapsPerInput > 0) {
        tActual = (double)fp.tapsPerInput;
    } else if (fp.taps > 0) {
        const double tByLen = (double)fp.taps / 16.0;
        const double tCap   = 2.0 * (double)m.T;
        tActual = (tByLen < tCap) ? tByLen : tCap;
    }
    m.ringMs = (fp.phase == PhaseKind::Minimum) ? 0.0
                                               : 0.42 * (tActual * 0.5) / fsIn * 1000.0;
    // D-15 判据 1：通带切到真实内容。
    // 【例外】切趾预设不适用——切趾本身就是"主动切掉顶部"，它的通带边缘（apodF1）
    // 必然低于内容边缘，那是设计意图，不是切错。若一刀切，apod-20k / gauss-halfband
    // 这 8 个会被误排除，等于把切趾家族从产品里删掉。
    m.excluded = (!fp.apodizing) && (m.passHz < contentHz - tolHz);
    return m;
}

// 261030-01 §7.3（D-17）：**自动预设**。
//
// L1 层（99% 用户）只需要「自动」。规格定的自动选择是：
//   44.1/48 / 88.2/96 / 176.4/192 PCM，以及 DSD64~1024 —— **全部 poly-sinc-short-lp**。
// 理由：它在每个源采样率下的通带都刚好覆盖真实内容边缘（Δf 随 fs_in 同比放大），
// 而振铃随 fs_in 变短（44.1k 1.00 ms → 192k 0.25 ms），是「不牺牲通带前提下最短振铃」的那一档。
//
// 保留成函数而不是直接写字面量：以后若要按源分档（例如 DSD 用高斯），只改这一处。
inline const char* auto_preset_name() { return "poly-sinc-short-lp"; }
inline bool is_auto_preset(const std::string& n) { return n.empty() || n == "auto"; }

// 261030-01 §7.2：源采样率 → 真实内容边缘（Hz）。
// CD 类（44.1/48）母带在 20 kHz 以上基本是 ADC 振铃；高码率是真内容。
inline double content_hz_for_rate(uint32_t fs) {
    if (fs <= 48000u) return 20000.0;      // 44.1 / 48
    if (fs <= 96000u) return 40000.0;      // 88.2 / 96
    return 80000.0;                        // 176.4 / 192
}

// 从 PipelineConfig 的散字段反解每相抽头（抽头反解，§4.3 k 模型）。
// 为什么单独给一个入口：播放路径（pipeline.h）手里只有 PipelineConfig，没有 FilterPreset 对象。
inline int solve_taps_per_phase(WindowKind w, double transition, bool apodizing,
                                double apodF1, double apodF2, int taps,
                                int tapsPerInput, double fsIn) {
    if (tapsPerInput > 0 && !apodizing) return tapsPerInput;
    FilterPreset fp{};
    fp.name = "auto"; fp.taps = taps; fp.transition = transition;
    fp.tapsPerInput = tapsPerInput;
    fp.attenDb = (w == WindowKind::Gaussian) ? 120.0 : 280.0;
    fp.window = w; fp.apodizing = apodizing; fp.apodF1 = apodF1; fp.apodF2 = apodF2;
    return preset_metrics(fp, fsIn, content_hz_for_rate((uint32_t)(fsIn + 0.5))).T;
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
    // ★ 2026-10-31：与 apply_preset / rtaweb 同源 —— 必须按家族天花板设计，
    //   不能用预设的历史 attenDb（120~240）。否则 make_filter 出来的 short-lp 只有 −149 dB，
    //   而同一名字在播放链路上是 −286 dB，两条路径又分叉了（2.1.6 那类 bug 的第三次复发）。
    // make_filter 是**预设表回归辅助函数**（只有 tests 用），不是播放路径。
    //   播放路径是 apply_preset → PolyphaseResampler，那里按 max(attenDb, 家族天花板) 设计。
    //   这里保留预设的历史 attenDb，让 test_apodizing 的「新预设不得改变已有预设输出」
    //   逐样本基线继续成立 —— 那条基线守的是**预设表**，不是播放链。
    const double designAtten = best->attenDb;
    s.taps = best->taps; s.cutoff = cutoff; s.attenDb = designAtten; s.window = best->window;
    s.apodizing = best->apodizing;
    if (best->apodizing) {
        // 在 2x 原型域中，0.5 对应输入 Nyquist（22.05 kHz）
        s.apodF1 = best->apodF1 * 0.5;
        s.apodF2 = best->apodF2 * 0.5;
    }
    // ★ 2026-10-31：改走级联相位构造（见 design_phased_fir 注释）。
    //   旧的「整段 design_fir + 一次倒谱变换」在 mp 上只能到 −253 dB、ip 上 −168 dB。
    auto h = design_phased_fir(best->taps, cutoff, designAtten, best->window,
                               best->apodizing,
                               best->apodizing ? s.apodF1 : 0.0,
                               best->apodizing ? s.apodF2 : 0.0,
                               best->phase, phase_mix());
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
