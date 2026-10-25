// pcm_quality_report.cpp — PCM 回放（升频链）客观量测工具（离线，不打开任何音频设备）
//
// 判据设计（铁律 3：指标必须证明要证明的事）：
//   1) 频响/群延迟一律量测【重采样器真正使用的原型】（PolyphaseResampler::prototype_into），
//      不另写一份设计代码 —— 避免"测的是另一支滤波器"。
//   2) 频率一律换算到【输入采样率】轴（听者关心这个轴）：f = x · fsIn · L / 2。
//   3) THD+N 用"单音最小二乘拟合后相减"，不用加窗 FFT 的泄漏底当噪声底
//      （后者会把 -300 dB 的数值噪声读成 -120 dB 的"失真"）。
//   4) 满幅(1.0) 与 -60 dBFS 两档都测：-60 dBFS 不能证明满幅行为。
//
// 用法: pcm_quality_report [--hash|--metrics|--thdn|--dither|--pareto|--all]
#include "dsp/filter_families.h"
#include "dsp/resampler.h"
#include "dsp/dither.h"
#include "dsp/filter_design.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>

using namespace rta::dsp;

struct Proto {
    int    L = 1, M = 1, T = 0;
    double fsIn = 0, fsOut = 0, fsInt = 0, gainDb = 0;
    std::vector<double> h;
    size_t taps() const { return h.size(); }
};
static double norm_of(const Proto& p, double hz) { return 2.0 * hz / p.fsInt; }
static double rel_db(const Proto& p, double hz) { return magnitude_db(p.h, norm_of(p, hz)) - p.gainDb; }

static Proto build_proto(const FilterPreset& fp, uint32_t inR, uint32_t outR, int tapsPerInput = -1) {
    Proto p;
    ResamplerConfig cfg; cfg.inRate = inR; cfg.outRate = outR; cfg.channels = 1;
    apply_preset(cfg, fp);
    if (tapsPerInput >= 0) cfg.minTapsPerInput = tapsPerInput;
    PolyphaseResampler rs(cfg);
    p.L = rs.upFactor(); p.M = rs.downFactor(); p.T = rs.tapsPerPhase();
    p.fsIn = inR; p.fsOut = outR; p.fsInt = (double)inR * (double)p.L;
    p.gainDb = 20.0 * std::log10((double)p.L);
    rs.prototype_into(p.h);
    return p;
}

static double group_delay_samples(const Proto& p, double hz) {
    const double w = kPi * norm_of(p, hz);
    const double wr = std::cos(-w), wi = std::sin(-w);
    double cr = 1.0, ci = 0.0, sr = 0, si = 0, nr = 0, ni = 0;
    for (size_t i = 0; i < p.h.size(); ++i) {
        const double v = p.h[i];
        sr += v * cr; si += v * ci; nr += (double)i * v * cr; ni += (double)i * v * ci;
        const double tr = cr * wr - ci * wi, ti = cr * wi + ci * wr; cr = tr; ci = ti;
        if ((i & 0x3FFF) == 0x3FFF) { const double m = std::sqrt(cr * cr + ci * ci); if (m > 0) { cr /= m; ci /= m; } }
    }
    const double den = sr * sr + si * si;
    return den > 0 ? (nr * sr + ni * si) / den : 0.0;
}

static double cross_hz(const Proto& p, double target, double lo, double hi) {
    if (rel_db(p, lo) <= target) return lo;
    for (int i = 0; i < 70; ++i) { const double m = 0.5 * (lo + hi); if (rel_db(p, m) > target) lo = m; else hi = m; }
    return 0.5 * (lo + hi);
}

// 通带偏差：|dB| 最大值（纹波 + 滚降混在一起，故另给 15 kHz 以下的峰峰值当"纯纹波"）
static void passband_stats(const Proto& p, double hiHz, double* maxdev, double* ripple15k) {
    double worst = 0.0, mn = 1e9, mx = -1e9;
    for (int i = 0; i <= 300; ++i) {
        const double f = 20.0 + (hiHz - 20.0) * (double)i / 300.0;
        const double d = rel_db(p, f);
        worst = std::max(worst, std::fabs(d));
        if (f <= 15000.0) { mn = std::min(mn, d); mx = std::max(mx, d); }
    }
    *maxdev = worst; *ripple15k = (mx > mn) ? (mx - mn) : 0.0;
}

static double worst_stopband(const Proto& p, double* atHz = nullptr) {
    // ⚠️ 频率轴必须止于内插域 Nyquist：超过 fsInt/2 后 DTFT 镜像回来，
    // 会把通带的 0 dB 读成"阻带最差 0 dB"（本轮在 L=2 上踩过一次）。
    double worst = -1e9, wf = 0.0;
    const double fEdge = p.fsIn * 0.5;          // 镜像带起点 = 输入 Nyquist
    const double fMax  = p.fsInt * 0.5;         // 内插域 Nyquist
    const double fDense = std::min(4.0 * fEdge, fMax);
    for (int i = 1; i <= 240; ++i) {
        const double f = fEdge + (fDense - fEdge) * (double)i / 240.0;
        const double d = rel_db(p, f); if (d > worst) { worst = d; wf = f; }
    }
    if (fDense < fMax)
        for (int i = 0; i <= 160; ++i) {
            const double f = fDense * std::pow(fMax / fDense, (double)i / 160.0);
            const double d = rel_db(p, f); if (d > worst) { worst = d; wf = f; }
        }
    if (atHz) *atHz = wf;
    return worst;
}

struct Ringing { double prePost = 0, prePct = 0, preExtentUs = 0; };
static Ringing ringing(const Proto& p) {
    Ringing r;
    const size_t peak = detail::argmax_abs(p.h);
    double pre = 0, post = 0, tot = 0;
    for (size_t i = 0; i < p.h.size(); ++i) {
        const double e = p.h[i] * p.h[i]; tot += e;
        if (i < peak) pre += e; else if (i > peak) post += e;
    }
    r.prePost = post > 0 ? pre / post : 0.0;
    r.prePct = tot > 0 ? 100.0 * pre / tot : 0.0;
    double acc = 0.0; size_t first = peak;
    for (size_t i = peak; i-- > 0;) { acc += p.h[i] * p.h[i]; if (acc >= 0.99 * pre) { first = i; break; } }
    r.preExtentUs = (double)(peak - first) / (p.fsInt / 1e6);
    return r;
}

struct Fit { double rmsRes = 0, rmsFund = 0, dc = 0; };
static Fit sine_fit(const double* y, size_t n, double w) {
    double G[3][3] = {}, rhs[3] = {}, b[3];
    for (size_t k = 0; k < n; ++k) {
        b[0] = std::cos(w * (double)k); b[1] = std::sin(w * (double)k); b[2] = 1.0;
        for (int i = 0; i < 3; ++i) { rhs[i] += b[i] * y[k]; for (int j = 0; j < 3; ++j) G[i][j] += b[i] * b[j]; }
    }
    double A[3][4];
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) A[i][j] = G[i][j]; A[i][3] = rhs[i]; }
    for (int c = 0; c < 3; ++c) {
        int piv = c; for (int r = c + 1; r < 3; ++r) if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) piv = r;
        if (piv != c) for (int j = 0; j < 4; ++j) std::swap(A[c][j], A[piv][j]);
        const double d = A[c][c]; if (std::fabs(d) < 1e-300) return {};
        for (int j = c; j < 4; ++j) A[c][j] /= d;
        for (int r = 0; r < 3; ++r) if (r != c) { const double f = A[r][c]; for (int j = c; j < 4; ++j) A[r][j] -= f * A[c][j]; }
    }
    const double a = A[0][3], bb = A[1][3], dc = A[2][3];
    double acc = 0.0;
    for (size_t k = 0; k < n; ++k) {
        const double ap = a * std::cos(w * (double)k) + bb * std::sin(w * (double)k) + dc;
        acc += (y[k] - ap) * (y[k] - ap);
    }
    Fit f; f.rmsRes = std::sqrt(acc / (double)n); f.dc = dc;
    f.rmsFund = std::sqrt(0.5 * (a * a + bb * bb));
    return f;
}

// 谐波投影 THD（2..H 次，相对基波）
static double thd_db(const double* y, size_t n, double w0, int H = 20) {
    double pr[2] = {0, 0};
    for (size_t k = 0; k < n; ++k) { pr[0] += y[k] * std::cos(w0 * (double)k); pr[1] += y[k] * std::sin(w0 * (double)k); }
    const double p1 = 2.0 * (pr[0] * pr[0] + pr[1] * pr[1]) / ((double)n * (double)n);
    double p2 = 0.0;
    for (int h = 2; h <= H; ++h) {
        const double w = w0 * h; double re = 0.0, im = 0.0;
        for (size_t k = 0; k < n; ++k) { re += y[k] * std::cos(w * (double)k); im += y[k] * std::sin(w * (double)k); }
        p2 += 2.0 * (re * re + im * im) / ((double)n * (double)n);
    }
    return 10.0 * std::log10((p2 + 1e-300) / (p1 + 1e-300));
}

static uint64_t fnv1a(const std::vector<double>& v) {
    uint64_t h = 1469598103934665603ull;
    for (double d : v) {
        uint64_t b = 0; std::memcpy(&b, &d, sizeof(b));
        for (int i = 0; i < 8; ++i) { h ^= (b >> (8 * i)) & 0xFF; h *= 1099511628211ull; }
    }
    return h;
}

// 单频投影幅度（相干长度下正好等于该分量的峰值幅度）
static double proj_amp(const double* y, size_t n, double w) {
    double re = 0.0, im = 0.0;
    for (size_t k = 0; k < n; ++k) { re += y[k] * std::cos(w * (double)k); im += y[k] * std::sin(w * (double)k); }
    return 2.0 * std::sqrt(re * re + im * im) / (double)n;
}

// ---- THD+N：44.1k -> {192k,384k}，1 kHz，满幅 / -60 dBFS ----
// 分析窗长取【相干长度】：n 必须是 outR/1000 的整数倍（1 kHz 整数个周期），
// 否则未加窗 DFT 的泄漏底会把 THD 读成 -57 dB（本轮实测踩过：所有预设都读出同一个 -57 dB）。
struct ThResult { double thdn = 0, thd = 0, rmsFund = 0, img1 = 0, worstImg = -1e9; };
static ThResult measure_resampler_thdn(const FilterPreset& fp, uint32_t outR, double amp, int tapsPerInput = -1,
                                       size_t inFrames = 24576) {
    ResamplerConfig cfg; cfg.inRate = 44100; cfg.outRate = outR; cfg.channels = 1;
    apply_preset(cfg, fp);
    if (tapsPerInput >= 0) cfg.minTapsPerInput = tapsPerInput;
    PolyphaseResampler rs(cfg);
    std::vector<double> x(inFrames), o(rs.maxOutFrames(inFrames) + 16);
    for (size_t n = 0; n < inFrames; ++n) x[n] = amp * std::sin(2.0 * kPi * 1000.0 * (double)n / 44100.0);
    const double* ip[1] = { x.data() }; double* op[1] = { o.data() };
    const size_t got = rs.process(ip, inFrames, op);
    const size_t settle = got / 4;                   // 跳过启动瞬态（含群延迟）
    const size_t unit = (outR % 1000 == 0) ? (outR / 1000) : 1;   // 1 kHz 相干周期
    size_t n = (got - settle) / unit * unit;
    if (n < unit * 8) n = got - settle;
    const double* y = o.data() + settle;
    const double w0 = 2.0 * kPi * 1000.0 / (double)outR;
    const Fit f = sine_fit(y, n, w0);
    ThResult r;
    r.rmsFund = f.rmsFund;
    r.thdn = 20.0 * std::log10((f.rmsRes + 1e-300) / (f.rmsFund + 1e-300));
    r.thd  = thd_db(y, n, w0);
    // 镜像：k*44100 - 1000（升频后落在输出带内的那些）
    for (int k = 1; k <= 4; ++k) {
        const double fi = (double)k * 44100.0 - 1000.0;
        if (fi >= 0.5 * (double)outR) continue;
        const double wi = 2.0 * kPi * fi / (double)outR;
        const double a = proj_amp(y, n, wi);
        const double lvl = 20.0 * std::log10((a + 1e-300) / (f.rmsFund * std::sqrt(2.0) + 1e-300));
        if (k == 1) r.img1 = lvl;
        r.worstImg = std::max(r.worstImg, lvl);
    }
    return r;
}

static double db_of(double x) { return 20.0 * std::log10(x + 1e-300); }

int main(int argc, char** argv) {
    bool doHash = false, doMetrics = false, doThdn = false, doDither = false, doPareto = false;
    bool doRate = false, doCost = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--hash") doHash = true; else if (a == "--metrics") doMetrics = true;
        else if (a == "--thdn") doThdn = true; else if (a == "--dither") doDither = true;
        else if (a == "--pareto") doPareto = true;
        else if (a == "--rate") doRate = true; else if (a == "--cost") doCost = true;
        else if (a == "--all") { doHash = doMetrics = doThdn = doDither = doPareto = doRate = doCost = true; }
    }
    if (!(doHash || doMetrics || doThdn || doDither || doPareto || doRate || doCost)) doMetrics = true;

    if (doHash) {
        printf("=== 原型系数哈希（既有预设逐位回归基线）===\n");
        for (const auto& fp : filter_presets())
            for (auto io : { std::pair<uint32_t,uint32_t>{44100,88200}, {44100,192000}, {44100,384000}, {44100,705600} })
                { Proto p = build_proto(fp, io.first, io.second);
                  printf("  %-26s %6u->%-6u L=%-5d T=%-6d taps=%-8zu %016llx\n", fp.name, io.first, io.second,
                         p.L, p.T, p.taps(), (unsigned long long)fnv1a(p.h)); }
    }

    const char* kNames[] = { "poly-sinc-short-lp", "poly-sinc-lp", "poly-sinc-long-lp", "poly-sinc-xl",
                             "poly-sinc-gauss-long", "poly-sinc-ext2", "sinc-S", "sinc-M", "sinc-LL",
                             "poly-sinc-short-mp", "poly-sinc-long-mp",
                             "apod-20k", "apod-19k-short", "apod-wide",
                             "poly-sinc-tight-lp", "apod-20k-tight" };

    if (doMetrics) {
        for (auto io : { std::pair<uint32_t,uint32_t>{44100,192000}, {44100,384000} }) {
            printf("\n=== 频响/时域指标：%u -> %u Hz ===\n", io.first, io.second);
            printf("  %-22s %4s %5s %8s %8s %8s %8s %8s %7s %7s %8s %8s\n",
                   "preset", "L", "T", "protoTaps", "ripp15k", "dev.45fs", "f-0.1dB", "fStop", "transHz", "worstSt", "GDdev", "preR%");
            for (const char* nm : kNames) {
                const FilterPreset* fp = find_preset(nm, false); if (!fp) fp = find_preset(nm, true);
                if (!fp) continue;
                Proto p = build_proto(*fp, io.first, io.second);
                double dev45 = 0, rip = 0; passband_stats(p, 0.45 * p.fsIn, &dev45, &rip);
                const double f01 = cross_hz(p, -0.1, 20.0, 0.499 * p.fsIn);
                const double fst = cross_hz(p, -(double)fp->attenDb, 20.0, 0.499 * p.fsIn * 1.5);
                double at = 0; const double ws = worst_stopband(p, &at);
                double gdMin = 1e18, gdMax = -1e18;
                for (int i = 0; i <= 60; ++i) { const double f = 20.0 + (0.45 * p.fsIn - 20.0) * i / 60.0;
                    const double g = group_delay_samples(p, f); gdMin = std::min(gdMin, g); gdMax = std::max(gdMax, g); }
                const Ringing rg = ringing(p);
                printf("  %-22s %4d %5d %8zu %8.4f %8.3f %8.0f %8.0f %8.0f %7.1f %7.3f %8.3f\n",
                       nm, p.L, p.T, p.taps(), rip, rel_db(p, 0.45 * p.fsIn), f01, fst, fst - f01, ws,
                       (gdMax - gdMin) / (double)std::max(1, p.L), rg.prePct);
            }
        }
    }

    if (doThdn) {
        printf("\n=== 实测 THD+N（1 kHz，端到端：输入正弦 -> 重采样器 -> 输出）===\n");
        printf("  %-22s %9s | %-26s | %-26s | %s\n", "preset", "outRate", "0 dBFS THD+N / THD", "-60 dBFS THD+N / THD", "镜像43.1k / 最差");
        for (const char* nm : { "poly-sinc-short-lp", "poly-sinc-lp", "poly-sinc-long-lp", "sinc-M",
                                "poly-sinc-gauss-long", "poly-sinc-ext2", "apod-20k",
                                "poly-sinc-tight-lp", "apod-20k-tight" }) {
            const FilterPreset* fp = find_preset(nm, false); if (!fp) fp = find_preset(nm, true);
            if (!fp) continue;
            for (uint32_t ov : { 192000u, 384000u }) {
                const ThResult a = measure_resampler_thdn(*fp, ov, 1.0);
                const ThResult c = measure_resampler_thdn(*fp, ov, 0.001);
                printf("  %-22s %9u | %9.1f / %8.1f dBFS | %9.1f / %8.1f dBFS | %8.1f / %8.1f dB\n",
                       nm, ov, a.thdn, a.thd, c.thdn, c.thd, a.img1, a.worstImg);
            }
        }
    }

    if (doDither) {
        printf("\n=== 抖动/整形对比（1 kHz 正弦，量化到 24 / 16 bit，44.1k）===\n");
        printf("  %-22s %4s %10s | %10s %10s %10s | %10s\n",
               "shaper", "bit", "level", "THD+N", "THD", "floor", "blkSpread");
        struct Case { Shaper s; const char* n; };
        const Case cases[] = { {Shaper::None, "none(round)"}, {Shaper::RPDF, "RPDF"},
                               {Shaper::TPDF, "TPDF"}, {Shaper::Gauss1, "Gauss1"},
                               {Shaper::NS9, "NS9"}, {Shaper::LNS15, "LNS15"} };
        for (int bits : {24, 16}) {
            for (const Case& c : cases) {
                for (double amp : { 0.5, 0.001, 0.0000316 }) {
                    if (bits == 16 && amp == 0.0000316) continue;
                    Quantizer q(c.s, bits, 20261025, 44100.0);
                    const size_t N = 44100 * 2;   // 2x44100 样本 = 2000 个 1 kHz 周期（相干）
                    std::vector<double> y(N);
                    for (size_t n = 0; n < N; ++n) y[n] = q.process(amp * std::sin(2.0 * kPi * 1000.0 * (double)n / 44100.0));
                    const size_t st = 0, n = N;
                    const double w = 2.0 * kPi * 1000.0 / 44100.0;
                    const Fit f = sine_fit(y.data() + st, n, w);
                    const double thdn = db_of(f.rmsRes / f.rmsFund);
                    const double th = thd_db(y.data() + st, n, w);
                    // 噪声底与"分块残差 RMS 抖动"（噪声调制的代理指标）
                    const size_t blk = 8192; double mn = 1e9, mx = -1e9; int nb = 0;
                    for (size_t s = st; s + blk <= N; s += blk) {
                        const Fit fb = sine_fit(y.data() + s, blk, w);
                        mn = std::min(mn, fb.rmsRes); mx = std::max(mx, fb.rmsRes); ++nb;
                    }
                    const double spread = (nb > 1 && mn > 0) ? db_of(mx / mn) : 0.0;
                    printf("  %-22s %4d %10.6f | %10.1f %10.1f %10.1f | %10.4f\n",
                           c.n, bits, amp, thdn, th, db_of(f.rmsRes), spread);
                }
            }
        }
    }

    if (doPareto) {
        printf("\n=== 抽头预算 / 过渡带 / 阻带的帕累托关系（44.1k 源，Kaiser）===\n");
        printf("  理论: T(每输入样本抽头) ≈ (A-8)/(14.36·tr)  [tr = 过渡带占输入 Nyquist 比例]\n");
        printf("  %-22s %4s %5s %6s %6s %9s %9s\n", "preset", "L", "T", "tr", "A", "T_need", "T/T_need");
        for (const char* nm : kNames) {
            const FilterPreset* fp = find_preset(nm, false); if (!fp) fp = find_preset(nm, true);
            if (!fp) continue;
            for (auto io : { std::pair<uint32_t,uint32_t>{44100,88200}, {44100,384000} }) {
                Proto p = build_proto(*fp, io.first, io.second);
                const double need = (fp->attenDb - 8.0) / (14.36 * std::max(1e-6, fp->transition));
                printf("  %-22s %4d %5d %6.3f %6.0f %9.1f %9.2f\n", nm, p.L, p.T, fp->transition,
                       fp->attenDb, need, (double)p.T / need);
            }
        }
        printf("\n  --- 44.1k -> 384k：只改每相位抽头 T 的实测（Kaiser, A=140, tr=0.02）---\n");
        printf("  %5s %11s %9s %9s %9s %9s %9s\n", "T", "protoTaps", "f-0.1dB", "f-3dB", "f-140dB", "dev20k", "worstSt");
        const FilterPreset* base = find_preset("poly-sinc-tight-lp", false);
        if (base) {
            for (int T : { 4, 8, 16, 32, 64, 128, 256, 512, 1024 }) {
                Proto p = build_proto(*base, 44100, 384000, T);
                double at = 0; const double ws = worst_stopband(p, &at);
                printf("  %5d %11zu %9.0f %9.0f %9.0f %9.3f %9.1f\n", T, p.taps(),
                       cross_hz(p, -0.1, 20.0, 0.499 * p.fsIn), cross_hz(p, -3.0, 20.0, 0.499 * p.fsIn),
                       cross_hz(p, -140.0, 20.0, 0.499 * p.fsIn * 2.0), rel_db(p, 20000.0), ws);
            }
        }
    }
    if (doRate) {
        printf("\n=== 输出采样率不变性：同一预设在不同输出率下的频响（44.1k 源）===\n");
        printf("  %-22s %9s %4s %5s %8s %9s %10s %9s\n", "preset", "outRate", "L", "T", "protoTaps", "f-0.1dB", "dev@20kHz", "worstSt");
        for (const char* nm : { "poly-sinc-lp", "poly-sinc-long-lp", "sinc-M", "poly-sinc-tight-lp", "apod-20k", "apod-20k-tight" }) {
            const FilterPreset* fp = find_preset(nm, false); if (!fp) fp = find_preset(nm, true);
            if (!fp) continue;
            for (uint32_t ov : { 88200u, 192000u, 352800u, 384000u, 705600u }) {
                Proto p2 = build_proto(*fp, 44100, ov);
                printf("  %-22s %9u %4d %5d %8zu %9.0f %10.3f %9.1f\n", nm, ov, p2.L, p2.T, p2.taps(),
                       cross_hz(p2, -0.1, 20.0, 0.499 * p2.fsIn), rel_db(p2, 20000.0), worst_stopband(p2));
            }
        }
    }

    if (doCost) {
        printf("\n=== CPU / 系数表代价（44.1k -> 384k, 立体声, 1 秒音频）===\n");
        printf("  %-22s %5s %9s %9s %10s %12s %10s\n", "preset", "T", "taps/phase", "coefMB", "ms/1s", "x realtime", "MMAC/s");
        for (const char* nm : { "poly-sinc-short-lp", "poly-sinc-lp", "poly-sinc-long-lp", "poly-sinc-xl",
                                "sinc-M", "poly-sinc-tight-lp", "apod-20k-tight" }) {
            const FilterPreset* fp = find_preset(nm, false); if (!fp) fp = find_preset(nm, true);
            if (!fp) continue;
            ResamplerConfig cfg; cfg.inRate = 44100; cfg.outRate = 384000; cfg.channels = 2;
            apply_preset(cfg, *fp);
            auto t0 = std::chrono::steady_clock::now();
            PolyphaseResampler rs(cfg);
            const double prepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const size_t N = 44100;
            std::vector<double> a(N), b(N), oa(rs.maxOutFrames(N) + 16), ob(rs.maxOutFrames(N) + 16);
            for (size_t n = 0; n < N; ++n) { a[n] = 0.4 * std::sin(2.0 * kPi * 997.0 * n / 44100.0); b[n] = -a[n]; }
            const double* ip[2] = { a.data(), b.data() };
            double* op[2] = { oa.data(), ob.data() };
            auto t1 = std::chrono::steady_clock::now();
            const size_t got = rs.process(ip, N, op);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            const double mac = (double)got * 2.0 * (double)rs.tapsPerPhase();
            printf("  %-22s %5d %9d %9.3f %10.1f %12.1f %10.0f   (prepare %.0f ms)\n", nm,
                   rs.tapsPerPhase(), rs.tapsPerPhase(), (double)rs.tapsPerPhase() * (double)rs.upFactor() * 8.0 / 1e6,
                   ms, 1000.0 / ms, mac / (ms / 1000.0) / 1e6, prepMs);
        }
    }

    return 0;
}
