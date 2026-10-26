// room_correction.h — 房间校正算法核心（C1 包）
//
// 测量→校正的完整离线链路：
//   1) generate_sweep         Farina 对数扫频（ESS, Exponential Sine Sweep）
//   2) deconvolve_ir          频域正则化反卷积，从录音恢复房间脉冲响应
//   3) psychoacoustic_smooth  变分数倍频程平滑（低频 1/6 → 高频 1/3 oct）
//   4) target_curve_db        目标曲线（flat / tilt 斜线 / 低频架）
//   5) compute_correction_ir  校正 FIR：平滑幅度−目标 → 限升 → 最小相位 → 截断加窗
//   6) sub_alignment          主/超低 40~120 Hz 带限互相关对齐
//   7) room_layouts           多声道布局预置（2.0~7.1.4，LFE 分频 80 Hz）
//
// 方法学（公开文献，独立实现、未抄任何开源代码）：
//   * A. Farina, "Simultaneous measurement of impulse response and distortion
//     with a swept-sine technique", AES 108th Convention, 2000（对数扫频 + 反卷积）。
//   * 正则化反卷积 / 变分数倍频程心理声学平滑 / 目标曲线校正：REW 用户手册公开
//     描述的工作流（方法学同名，非代码）。
//   * 最小相位：复倒谱同态法（Oppenheim & Schafer 经典方法），
//     h_min = IFFT(exp(FFT(ramp · IFFT(ln|H|))))。
//
// 本文件只做纯算法：不碰设备、不开线程、不 include src/audio/*（铁律 1）。
// 播放扫频与录音回传的接线由上层（C2，经回调注入）负责。
#pragma once
#include "fft.h"      // rta::dsp::fft / cplx / kPi（kPi 由 kaiser.h 经 fft.h 提供）
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <vector>

namespace rta::dsp {

// ---------------- 扫频生成 ----------------

struct SweepSpec { double fs = 48000, f0 = 10, f1 = 22050, durSec = 10; };

// Farina 对数扫频：x(t) = sin( 2π·f0·L·(e^{t/L} − 1) )，L = T / ln(f1/f0)。
// 群延迟 τ(f) = L·ln(f/f0) 随频率对数增长，能量按每倍频程均摊，反卷积后信噪比
// 与频率无关——这是选 ESS 而不是线性扫频的原因。
// 故意【不做】端点淡入淡出（实测反复对比过）：恢复谱 H = |S|²/(|S|²+ε) 的带边
// 形状由扫频泄漏裙决定，端点渐变会把泄漏收窄成陡沿，时域振铃衰减反而变慢
// （±16 样本外旁瓣：无渐变 −42 dB，5 ms 渐变 −35 dB）。纯扫频逐点独立求相位、
// 起点振幅为 0，本函数返回的序列即是反卷积参考本身；播放端若要加电平/渐变
// 防点击，必须把【实际播放的同一份序列】作为参考传回（C2 接线层职责）。
inline std::vector<double> generate_sweep(const SweepSpec& s) {
    std::vector<double> x;
    if (s.fs <= 0.0 || s.f0 <= 0.0 || s.durSec <= 0.0) return x;
    const double f1 = std::min(s.f1, 0.499 * s.fs);   // 超奈奎斯特必混叠，钳掉
    if (f1 <= s.f0) return x;
    const size_t n = (size_t)std::llround(s.durSec * s.fs);
    const double L = s.durSec / std::log(f1 / s.f0);
    x.resize(n);
    for (size_t i = 0; i < n; ++i) {
        // 逐点独立求相位（不做递推累加），长扫频下不积累舍入误差
        const double t = (double)i / s.fs;
        x[i] = std::sin(2.0 * kPi * s.f0 * L * (std::exp(t / L) - 1.0));
    }
    return x;
}

// ---------------- 反卷积 ----------------

struct DeconvOpts { double epsilon = 1e-6; /*正则化(相对 max|S|²)，REW 式*/ size_t padTo = 0; };

inline size_t next_pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

// 频域正则化反卷积：IR = IFFT( Measured · conj(S) / (|S|² + ε·max|S|²) )。
// ε·max|S|² 把扫频带外（|S|→0）频点的逆增益压回有限值，带内 |S|²≫ε·max 时
// 上式≈精确逆——这是 Wiener 式的正则化，不是硬门槛，谱上没有台阶。
// 补零到 ≥ len(measured)+len(sweep) 的下一个 2 的幂，圆周反卷积才不发生时域卷绕。
inline std::vector<double> deconvolve_ir(const std::vector<double>& measured,
                                         const std::vector<double>& sweep, double fs,
                                         const DeconvOpts& opt = {}) {
    (void)fs;   // 预留：诊断输出按 fs 换算时使用
    std::vector<double> ir;
    if (measured.empty() || sweep.empty()) return ir;
    const size_t need = measured.size() + sweep.size();
    const size_t n = next_pow2(need > opt.padTo ? need : opt.padTo);
    std::vector<cplx> M(n, cplx(0.0, 0.0)), S(n, cplx(0.0, 0.0));
    for (size_t i = 0; i < measured.size(); ++i) M[i] = cplx(measured[i], 0.0);
    for (size_t i = 0; i < sweep.size(); ++i) S[i] = cplx(sweep[i], 0.0);
    fft(M);
    fft(S);
    double pmax = 0.0;
    for (const auto& v : S) pmax = std::max(pmax, std::norm(v));
    if (pmax <= 0.0) return ir;
    const double reg = opt.epsilon * pmax;
    std::vector<cplx> H(n);
    for (size_t k = 0; k < n; ++k) H[k] = M[k] * std::conj(S[k]) / (std::norm(S[k]) + reg);
    fft(H, true);
    ir.resize(n);
    for (size_t i = 0; i < n; ++i) ir[i] = H[i].real();
    return ir;
}

inline size_t ir_peak_index(const std::vector<double>& x) {
    size_t p = 0;
    double best = -1.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double a = std::fabs(x[i]);
        if (a > best) { best = a; p = i; }
    }
    return p;
}

// 实信号 IR → 每个 FFT bin 的幅度（dB，相对线性幅度，非 dBFS）。
// 返回 N/2+1 个 bin（N 为补零后的 FFT 长度），bin k 的频率 = k·fs/N。
// 后续平滑/目标曲线都约定输入是这种「半谱」布局。
inline std::vector<double> ir_magnitude_db(const std::vector<double>& ir, double fs) {
    std::vector<double> magDb;
    if (ir.empty() || fs <= 0.0) return magDb;
    const size_t n = next_pow2(ir.size());
    std::vector<cplx> X(n, cplx(0.0, 0.0));
    for (size_t i = 0; i < ir.size(); ++i) X[i] = cplx(ir[i], 0.0);
    fft(X);
    magDb.resize(n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k)
        magDb[k] = 20.0 * std::log10(std::max(std::abs(X[k]), 1e-12));
    return magDb;
}

// ---------------- 心理声学平滑 ----------------

// 变分数倍频程滑动几何平均（dB 域即算术平均），原地修改。
// octLow/octHigh 是分母：低频窗宽 1/octLow 倍频程、高频 1/octHigh 倍频程，
// 在 [200 Hz, 2000 Hz] 之间按 log2(f) 线性过渡（REW 心理声学平滑同思路：
// 低频窄窗保住细节，高频宽窗逼近人耳临界频带宽度，让「听起来平」优先于
// 「仪表平」）。拐点为实现常量，UI 不暴露。
// 输入约定：magDb 必须是 N 点实 FFT 的 N/2+1 个 bin（ir_magnitude_db 的输出）。
// 实现为双指针滑动窗：f_lo/f_hi 随 bin 序号单调不减，均摊 O(N)，
// 否则 2^20 bin × 10 万 bin 宽的高频窗会慢到不可用。
inline void psychoacoustic_smooth(std::vector<double>& magDb, double fs,
                                  double octLow = 6.0, double octHigh = 3.0) {
    const size_t n = magDb.size();
    if (n < 2 || fs <= 0.0 || octLow <= 0.0 || octHigh <= 0.0) return;
    const double binHz = fs / (2.0 * (double)(n - 1));
    const double ln2 = std::log(2.0);
    const double lLo = std::log(200.0) / ln2, lHi = std::log(2000.0) / ln2;
    const std::vector<double> in = magDb;   // 滑动均值读写同一缓冲前先留原值
    size_t lo = 0, hiEx = 1;                // 当前窗 [lo, hiEx)
    double sum = in[0];
    for (size_t k = 0; k < n; ++k) {
        const double fk = std::max((double)k * binHz, 1e-6);
        double t = (std::log(fk) / ln2 - lLo) / (lHi - lLo);
        t = std::min(1.0, std::max(0.0, t));
        const double width = 1.0 / (octLow + (octHigh - octLow) * t);   // 倍频程数
        const double half = 0.5 * width * ln2;
        const double flo = fk * std::exp(-half);
        const double fhi = fk * std::exp(half);
        while (hiEx < n && (double)hiEx * binHz <= fhi) { sum += in[hiEx]; ++hiEx; }
        while (lo < k && (double)lo * binHz < flo) { sum -= in[lo]; ++lo; }
        // 不变量：lo ≤ k < hiEx，窗至少含自身（flo ≤ fk ≤ fhi 保证）
        magDb[k] = sum / (double)(hiEx - lo);
    }
}

// ---------------- 目标曲线 ----------------

struct TargetCurve {
    struct Point { double hz, db; };
    std::vector<Point> points;   // 折线控制点（Hz,dB），hz 升序；段间按 log2(f) 线性插值

    static TargetCurve flat() { return {}; }
    // 高频滚降斜线（−1 dB/oct = 常见房间目标）。锚在 20 Hz~20 kHz 两端各 ±5 dB，
    // 绝对电平由 target_curve_db 的均值归一定，锚点只决定形状。
    static TargetCurve tilt(double dbPerOct = -1.0) {
        TargetCurve t;
        t.points = { {20.0, -dbPerOct * 5.0}, {20000.0, dbPerOct * 5.0} };
        return t;
    }
    // 低频架：cornerHz 以下 +gainDb，cornerHz~4×cornerHz 过渡到 0（约一阶滚降的斜率）
    static TargetCurve bass_shelf(double gainDb = 4.0, double cornerHz = 100.0) {
        TargetCurve t;
        t.points = { {20.0, gainDb}, {cornerHz, gainDb},
                     {cornerHz * 4.0, 0.0}, {20000.0, 0.0} };
        return t;
    }
};

// 目标曲线在半谱 bin 上的取值（nBins = N/2+1，与 ir_magnitude_db 同约定）。
// 按「20 Hz ~ min(20 kHz, 0.45·fs) 内 log 均匀取点的均值 = 0 dB」归一：目标曲线
// 只描述形状不引入整体增益，否则 tilt 会让校正器整体抬/降 20 dB，全吃掉动态余量。
inline std::vector<double> target_curve_db(const TargetCurve& t, double fs, size_t nBins) {
    std::vector<double> out(nBins, 0.0);
    if (nBins < 2 || fs <= 0.0) return out;
    const double binHz = fs / (2.0 * (double)(nBins - 1));
    const double ln2 = std::log(2.0);

    auto shape_at = [&](double f) -> double {
        if (t.points.empty()) return 0.0;
        if (f <= t.points.front().hz) return t.points.front().db;
        if (f >= t.points.back().hz) return t.points.back().db;
        for (size_t i = 1; i < t.points.size(); ++i) {
            if (f <= t.points[i].hz) {
                const double a = std::log(t.points[i - 1].hz) / ln2;
                const double b = std::log(t.points[i].hz) / ln2;
                const double u = (std::log(f) / ln2 - a) / (b - a);
                return t.points[i - 1].db + (t.points[i].db - t.points[i - 1].db) * u;
            }
        }
        return t.points.back().db;
    };

    // 均值归一：512 个 log 均匀频点上的平均置 0
    const double bandLo = 20.0, bandHi = std::max(40.0, std::min(20000.0, 0.45 * fs));
    double mean = 0.0;
    const int nAvg = 512;
    for (int i = 0; i < nAvg; ++i) {
        const double f = bandLo * std::pow(bandHi / bandLo, (double)i / (nAvg - 1));
        mean += shape_at(f);
    }
    mean /= (double)nAvg;

    for (size_t k = 0; k < nBins; ++k)
        out[k] = shape_at(std::max((double)k * binHz, 1e-6)) - mean;
    return out;
}

// ---------------- 最小相位（复倒谱法） ----------------

// 输入 N/2+1 个线性幅度 bin（N 点实谱半谱），输出长度 N 的最小相位 IR。
// 步骤：ln|H| → IFFT → 实 cepstrum → 乘斜坡因果化 → FFT → 逐点复指数 → IFFT。
// 关于 ln 的相位问题：只对【幅度】取对数，对数谱是实偶序列，不存在相位、
// 也就不需要相位展开（unwrap）——这是复倒谱法相对「直接 ln H + 展开相位」
// 的主要工程优点。ln|H| 实偶 ⟹ IFFT 后虚部只是数值残差，直接置零；
// 斜坡 w[n] = [1, 2,2,…,2, 1, 0,…,0] 把非因果镜像折半加到因果侧（n=0 与
// n=N/2 是自共轭项，不能折倍），exp(FFT(w·c)) 的谱与 |H| 逐点同幅、相位满足
// Hilbert 最小相位关系。深度谷点用 1e-7 下限防 ln 发散。
inline std::vector<double> minimum_phase_ir(const std::vector<double>& magLin) {
    const size_t half = magLin.size();
    if (half < 2) return {};
    const size_t n = (half - 1) * 2;   // 调用方保证 half = N/2+1 且 N 为 2 的幂
    std::vector<cplx> c(n, cplx(0.0, 0.0));
    for (size_t k = 0; k < half; ++k) {
        const double l = std::log(std::max(magLin[k], 1e-7));
        c[k] = cplx(l, 0.0);
        if (k > 0 && k + 1 < half) c[n - k] = cplx(l, 0.0);   // 实偶延拓
    }
    fft(c, true);                          // 实 cepstrum（虚部为数值残差）
    for (size_t i = 0; i < n; ++i) c[i] = cplx(c[i].real(), 0.0);
    for (size_t i = 1; i < n / 2; ++i) c[i] *= 2.0;   // 斜坡：因果化
    for (size_t i = n / 2 + 1; i < n; ++i) c[i] = cplx(0.0, 0.0);
    fft(c, false);                         // ln H_min(k)
    for (size_t i = 0; i < n; ++i) c[i] = std::exp(c[i]);   // H_min（Hermitian 性在 exp 下保持）
    fft(c, true);
    std::vector<double> h(n);
    for (size_t i = 0; i < n; ++i) h[i] = c[i].real();
    return h;
}

// ---------------- 校正 IR ----------------

struct CorrectionOpts { double maxBoostDb = 6.0, fMaxHz = 16000; size_t irWindowMs = 300, taps = 8192; };

// 原理：房间 IR 开窗 → FFT 幅度 → 心理声学平滑 →(平滑幅度/target) 得校正幅度
// （限升 +6dB，fMax 以上渐出）→ Hilbert 最小相位 → 截断到 taps 并加窗 → 通带增益归一。
inline std::vector<double> compute_correction_ir(const std::vector<double>& roomIr, double fs,
                                                 const TargetCurve& target, const CorrectionOpts& o) {
    std::vector<double> h(o.taps == 0 ? 1 : o.taps, 0.0);
    if (roomIr.empty() || fs <= 0.0 || o.taps == 0) return h;

    // (1) 开窗：峰前留 5 ms 捕捉早到成分（房间 IR 的峰≈直达声），窗长 irWindowMs；
    //     两端升余弦渐变，避免把窗沿的硬切断泄漏进幅度谱
    const size_t peak = ir_peak_index(roomIr);
    const size_t pre = (size_t)std::min<double>(0.005 * fs, peak);
    const size_t avail = roomIr.size() - (peak - pre);
    const size_t winLen = std::min((size_t)std::llround(o.irWindowMs * fs / 1000.0), avail);
    if (winLen < 16) return h;
    std::vector<double> seg(winLen, 0.0);
    const size_t fadeIn = std::max<size_t>(1, std::min(winLen, (size_t)(0.002 * fs)));
    const size_t fadeOut = std::max<size_t>(1, std::min(winLen - fadeIn, (size_t)(0.010 * fs)));
    for (size_t i = 0; i < winLen; ++i) {
        double w = 1.0;
        if (i < fadeIn) w = 0.5 - 0.5 * std::cos(kPi * (double)i / (double)fadeIn);
        if (i >= winLen - fadeOut)
            w = std::min(w, 0.5 + 0.5 * std::cos(kPi * (double)(i - (winLen - fadeOut)) / (double)fadeOut));
        seg[i] = roomIr[peak - pre + i] * w;
    }

    // (2)(3) 幅度谱 + 心理声学平滑
    const size_t nF = next_pow2(winLen);
    std::vector<cplx> X(nF, cplx(0.0, 0.0));
    for (size_t i = 0; i < winLen; ++i) X[i] = cplx(seg[i], 0.0);
    fft(X);
    std::vector<double> magDb(nF / 2 + 1);
    for (size_t k = 0; k <= nF / 2; ++k)
        magDb[k] = 20.0 * std::log10(std::max(std::abs(X[k]), 1e-12));
    psychoacoustic_smooth(magDb, fs);

    // (4)(5) 校正幅度 = target − 平滑幅度；只限升不限降（衰减不占重放余量），
    //     fMax 以上线性渐出到 0 dB（2·fMax 或 0.97·奈奎斯特先到为准），
    //     20 Hz 以下线性收回（10 Hz 以下不动），防止次声段吃掉全部动态余量
    const std::vector<double> tgt = target_curve_db(target, fs, magDb.size());
    const double binHz = fs / (double)nF;
    const double fNyq = 0.5 * fs;
    const double fEnd = std::min(2.0 * o.fMaxHz, 0.97 * fNyq);
    std::vector<double> amp(magDb.size(), 0.0);
    for (size_t k = 0; k < magDb.size(); ++k) {
        const double f = (double)k * binHz;
        double db = tgt[k] - magDb[k];
        if (db > o.maxBoostDb) db = o.maxBoostDb;
        if (f > o.fMaxHz && fEnd > o.fMaxHz)
            db *= std::min(1.0, std::max(0.0, (fEnd - f) / (fEnd - o.fMaxHz)));
        if (f < 30.0) db *= std::min(1.0, std::max(0.0, (f - 10.0) / 20.0));
        amp[k] = std::pow(10.0, db / 20.0);
        // 幅度下限 −60 dB：下限越深，ln 越负，复倒谱 IR 的尾巴拖得越长，
        // 截断到 taps（= FFT 长度一半）时幅度谱失真可达 20 dB（实测）。
        // 校正不需要比 −60 dB 更深的谷，物理上也不可信。
        if (amp[k] < 1e-3) amp[k] = 1e-3;
    }

    // (6) Hilbert 最小相位（复倒谱法），把对称的幅度约束变成因果可实现的 FIR
    const std::vector<double> hMin = minimum_phase_ir(amp);

    // (7) 截断到 taps，只对【尾部】做升余渐出。
    //     不能套对称 Kaiser 窗：对称窗峰值在 index taps/2，而最小相位 IR 的能量
    //     全在头部 —— w[0]=I0(0)/I0(β)≈1e-4，会把 IR 头部乘掉 80 dB（实测整条
    //     频谱掉 40~70 dB）。最小相位 IR 只需要处理「硬截断的尾巴」：
    //     后 25% 渐出即可压住截断纹波。
    const size_t use = std::min(o.taps, hMin.size());
    const size_t tail = std::max<size_t>(1, use / 4);
    for (size_t i = 0; i < use; ++i) {
        double w = 1.0;
        if (i >= use - tail)
            w = 0.5 + 0.5 * std::cos(kPi * (double)(i - (use - tail)) / (double)tail);
        h[i] = hMin[i] * w;
    }

    // (8) 通带增益归一：200 Hz~2 kHz 平均线性幅度归到 1.0（0 dB）。
    //     目标曲线只定形状，实际整体增益取决于房间与目标的差，不归一会整体
    //     偏听响；200~2k 是人声/能量主区，也是最不容易被房间峰谷污染的频段
    {
        const size_t nN = next_pow2(2 * o.taps);
        std::vector<cplx> Y(nN, cplx(0.0, 0.0));
        for (size_t i = 0; i < o.taps; ++i) Y[i] = cplx(h[i], 0.0);
        fft(Y);
        double sum = 0.0;
        size_t cnt = 0;
        for (size_t k = 0; k <= nN / 2; ++k) {
            const double f = (double)k * fs / (double)nN;
            if (f >= 200.0 && f <= 2000.0) { sum += std::abs(Y[k]); ++cnt; }
        }
        if (cnt > 0 && sum > 1e-12)
            for (size_t i = 0; i < o.taps; ++i) h[i] *= (double)cnt / sum;
    }
    return h;
}

// ---------------- 主/超低对齐 ----------------

struct SubAlign { double delaySamples = 0, gainDb = 0; };

// 40~120 Hz 带限互相关定延迟差 + 带限 RMS 匹配定增益。
//   delaySamples：正 = 超低比主声道晚到（超低链路需要补这么多样本延迟）；
//   gainDb      ：正 = 超低偏安静（超低通道需要补这么多增益才能与主声道对齐）。
// 带限的理由：分频点附近（40~120 Hz）是主/超低共同发声的频段，对齐只在该带内
// 有意义；带外房间噪声/高频成分只会污染互相关峰。
inline SubAlign sub_alignment(const std::vector<double>& mainIr,
                              const std::vector<double>& subIr, double fs) {
    SubAlign out;
    if (mainIr.empty() || subIr.empty() || fs <= 0.0) return out;
    const size_t n = next_pow2(mainIr.size() + subIr.size());
    auto bandlimited = [&](const std::vector<double>& x) {
        std::vector<cplx> X(n, cplx(0.0, 0.0));
        for (size_t i = 0; i < x.size() && i < n; ++i) X[i] = cplx(x[i], 0.0);
        fft(X);
        for (size_t k = 0; k < n; ++k) {
            double f = (double)k * fs / (double)n;
            if (f > 0.5 * fs) f = fs - f;   // 负频镜像按对称频率判
            if (f < 40.0 || f > 120.0) X[k] = cplx(0.0, 0.0);
        }
        return X;
    };
    std::vector<cplx> M = bandlimited(mainIr), S = bandlimited(subIr);

    // 互相关峰位 = S 相对 M 的滞后（圆周长 ≥ m+s，峰位无卷绕歧义）
    std::vector<cplx> Xc(n);
    for (size_t k = 0; k < n; ++k) Xc[k] = std::conj(M[k]) * S[k];
    fft(Xc, true);
    size_t best = 0;
    double bv = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double v = Xc[i].real();
        if (std::fabs(v) > std::fabs(bv)) { bv = v; best = i; }
    }
    double lag = (double)best;
    if (best > n / 2) lag -= (double)n;   // 反卷绕：峰在下半圆 = 负滞后
    out.delaySamples = lag;

    // 带限能量比：RMS 比 = √(能量比)，20·log10(√) = 10·log10
    fft(M, true);
    fft(S, true);
    double em = 0.0, es = 0.0;
    for (size_t i = 0; i < n; ++i) { em += std::norm(M[i]); es += std::norm(S[i]); }
    if (es > 0.0) out.gainDb = 10.0 * std::log10(em / es);
    return out;
}

// ---------------- 多声道布局预置 ----------------

struct RoomLayout { const char* name; int chCount; bool sub; const char* chName[12]; double crossoverHz; };

// 2.0/2.1/5.1/7.1/5.1.2/7.1.4；带超低的布局 LFE 分频默认 80 Hz（THX 常用值，
// 向导里可改）。通道名顺序即波形通道顺序。
inline std::vector<RoomLayout> room_layouts() {
    std::vector<RoomLayout> v;
    auto add = [&](const char* name, bool sub, double xover,
                   std::initializer_list<const char*> chs) {
        RoomLayout r{};
        r.name = name;
        r.sub = sub;
        r.crossoverHz = xover;
        int i = 0;
        for (const char* c : chs) if (i < 12) r.chName[i++] = c;
        r.chCount = i;
        v.push_back(r);
    };
    add("2.0",   false, 0.0,  { "L", "R" });
    add("2.1",   true,  80.0, { "L", "R", "LFE" });
    add("5.1",   true,  80.0, { "L", "R", "C", "LFE", "Ls", "Rs" });
    add("7.1",   true,  80.0, { "L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs" });
    add("5.1.2", true,  80.0, { "L", "R", "C", "LFE", "Ls", "Rs", "Ltf", "Rtf" });
    add("7.1.4", true,  80.0, { "L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs",
                                "Ltf", "Rtf", "Ltr", "Rtr" });
    return v;
}

} // namespace rta::dsp
