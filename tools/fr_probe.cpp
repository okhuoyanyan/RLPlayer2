// tools/fr_probe.cpp — 3.0.10 重测：79 个预设 x 6 组采样率的实测频响
//
// 只读引用 src/dsp/**、src/engine/pipeline.h，不修改 src。全程离线，不打开任何音频设备（铁律 1）。
//
// 生产身份（铁律 3：先证明测的是生产滤波器）
//  * 系数来自 dsp_measure::prototype()，内部走 review2::production_config()：
//      apply_preset() + autoTaps 反解 solve_taps_per_phase()，与
//      Pipeline::prepare_stream() 在 cfg.autoTaps=true 时**同一个函数、同一组入参**反解每相抽头。
//  * 每个 identity 都做三重自检：
//      ① 与 filter_report --coefficients 导出的同一批系数逐点比对（max|delta|、相对误差）
//      ② 与真实 Pipeline::process()（apply_filter_preset + autoTaps + 单位冲激）的冲激响应比对
//      ③ 比对 T / stage2_T / 原型样点数
//    ★ 这正是 261030-04 交接文档 §6.2 的坑：旧探针删掉了 minTapsPerInput 反解，
//      把 long-ip 报成 -282 dB（真实 -201 dB）。②③ 会在那种情况下立刻报不一致。
//
// 测量口径
//  * 幅频：|H(f)| = |DTFT{h}(2f/fs_proto)|；单级 fs_proto = in*L（内插域原型），
//    级联 fs_proto = out（prototype_into 返回真实 process() 冲激）。与 dsp_measure::measure 同源。
//  * 深阻带判据用 double-double 精确求值（review2::direct / rotation），不用普通 FFT。
//  * 群延迟：tau = Re{ sum n*h[n]e^{-jw n} / sum h[n]e^{-jw n} }（相位导数，无解缠问题）。
//  * 预/后振铃：冲激包络高于「峰值 -60 dB」的首/末样点时间。

#include "dsp_measure.h"
#include "dsp/fft.h"
#include "engine/pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using cplx = std::complex<double>;
using dsp_measure::DC;
using dsp_measure::add;
using dsp_measure::db;
using dsp_measure::mag;
using dsp_measure::mul;
using dsp_measure::rotation;

namespace {

struct ComboDef {
    const char* id;
    const char* label;
    unsigned in, out;
};
// 沿用 2026-10-27 旧表的六组口径（顺序一致，便于逐项差分）
const ComboDef kCombos[] = {
    {"44k1-96k",    "44.1k -> 96k",              44100,  96000},
    {"44k1-384k",   "44.1k -> 384k",             44100,  384000},
    {"96k-384k",    "96k -> 384k",               96000,  384000},
    {"44k1-705k6",  "44.1k -> 705.6k",           44100,  705600},
    {"48k-384k",    "48k -> 384k",               48000,  384000},
    {"dsd64-pcm384","DSD64 -> PCM176.4k -> 384k",176400, 384000},
};
constexpr int kNumCombos = (int)(sizeof(kCombos) / sizeof(kCombos[0]));

// ---------------------------------------------------------------- DD 复频响
DC spectrum_dd(const std::vector<double>& h, double hz, double fs) {
    DC w = rotation(hz, fs), z{{1.0, 0.0}, {0.0, 0.0}}, sum;
    for (double x : h) {
        sum = add(sum, {mul({x, 0.0}, z.r), mul({x, 0.0}, z.i)});
        z = mul(z, w);
    }
    return sum;
}
double mag_db_dd(const std::vector<double>& h, double hz, double fs) {
    return db(mag(spectrum_dd(h, hz, fs)));
}

// 群延迟（样点，原型率 fs）：tau = Re{ S1/S0 }，S1 = sum n*h[n]e^{-jwn}
struct Moments { cplx s0, s1; };
Moments moments_dd(const std::vector<double>& h, double hz, double fs) {
    DC w = rotation(hz, fs), z{{1.0, 0.0}, {0.0, 0.0}}, s0, s1;
    double n = 0.0;
    for (double x : h) {
        s0 = add(s0, {mul({x, 0.0}, z.r), mul({x, 0.0}, z.i)});
        const double nx = n * x;
        s1 = add(s1, {mul({nx, 0.0}, z.r), mul({nx, 0.0}, z.i)});
        z = mul(z, w);
        n += 1.0;
    }
    return {{s0.r.h + s0.r.l, s0.i.h + s0.i.l}, {s1.r.h + s1.r.l, s1.i.h + s1.i.l}};
}
// 返回 {群延迟(ms), |H|(dB)}
std::pair<double, double> gd_ms_dd(const std::vector<double>& h, double hz, double fs) {
    const Moments m = moments_dd(h, hz, fs);
    const double magv = std::abs(m.s0);
    const double mg = 20.0 * std::log10(magv > 1e-300 ? magv : 1e-300);
    if (magv < 1e-12) return {std::nan(""), mg};
    const double tau = (m.s1 / m.s0).real();   // 样点（原型率）
    return {tau / fs * 1000.0, mg};
}

// ---------------------------------------------------------------- 网格 FFT
struct Grid {
    size_t n = 0;
    double fs = 0, df = 0;
    std::vector<double> db;    // 0..n/2
    std::vector<double> gd;    // ms（|H| 过小时为 NaN）
};

Grid build_grid(const std::vector<double>& h, double fs, unsigned maxPow) {
    Grid g;
    g.fs = fs;
    // n 必须是 2 的幂且 >= 系数个数；理想上取 2 倍过采样（网格更密），内存不够时退回 1 倍。
    size_t n = 1;
    while (n < h.size()) n <<= 1;
    if (n < (size_t)1 << 14) n = (size_t)1 << 14;
    const size_t cap = (size_t)1 << maxPow;
    if (n * 2 <= cap) n <<= 1;
    if (n > cap) n = cap;
    if (n < h.size()) throw std::runtime_error("FFT grid too small for prototype; raise --fft-pow");
    g.n = n;
    g.df = fs / (double)n;
    std::vector<cplx> a(n), b(n);
    for (size_t i = 0; i < h.size(); ++i) {
        a[i] = cplx(h[i], 0.0);
        b[i] = cplx(h[i] * (double)i, 0.0);
    }
    rta::dsp::fft(a);
    rta::dsp::fft(b);
    const size_t half = n / 2;
    g.db.resize(half + 1);
    g.gd.resize(half + 1);
    for (size_t k = 0; k <= half; ++k) {
        const double m = std::abs(a[k]);
        g.db[k] = 20.0 * std::log10(m > 1e-300 ? m : 1e-300);
        const double mm = std::abs(a[k]);
        if (mm > 1e-8) g.gd[k] = (b[k] / a[k]).real() / fs * 1000.0;
        else g.gd[k] = std::nan("");
    }
    return g;
}
inline size_t bin_of(const Grid& g, double hz) {
    const double k = hz / g.df;
    if (k <= 0) return 0;
    if (k >= (double)(g.n / 2)) return g.n / 2;
    return (size_t)std::llround(k);
}

// ---------------------------------------------------------------- 结果
struct Result {
    std::string preset, combo, status = "OK";
    std::string budget = "PRODUCTION(2^21)";
    unsigned in = 0, out = 0;
    int T = 0, stage2T = 0, L = 0, M = 0;
    size_t ntaps = 0;
    bool cascade = false;
    double fs = 0, gridHz = 0;
    size_t fftN = 0;
    double transition = 0, attenDb = 0, target = 0;
    double passEdge = 0, stopEdge = 0;
    double dcDb = 0;
    double a1k = 0, a10k = 0, a20k = 0, a30k = 0;
    double f1db = 0, f3db = 0;
    double ripplePass = 0, rippleWorstHz = 0, ripple20k = 0;
    double passWorst = 0, passWorstHz = 0;
    double fsb80 = 0, stopMin = 0, stopMinHz = 0, stopSkirt = 0, stopMinFull = 0;
    double gd100 = 0, gd1k = 0, gd10k = 0, gdNom = 0;
    double preRingMs = 0, postRingMs = 0, peakMs = 0;
    double imgPeak = 0, imgPeakFull = 0;
    // 自检
    double vRepMaxAbs = std::nan(""), vRepRel = std::nan("");
    size_t vRepN = 0;
    double vPipeMaxRel = std::nan("");
    size_t vPipeN = 0;
    int vPipeT = -1, vPipeT2 = -1;
    // 曲线
    std::vector<double> curveF, curveDb, curveGd;
};

// ---------------------------------------------------------------- 单条测量
Result measure_one(const rta::dsp::FilterPreset& p, const ComboDef& cb,
                   const fs::path& reportDir, unsigned maxPow, unsigned long long maxTaps) {
    Result res;
    res.budget = (maxTaps == 0) ? "PRODUCTION(2^21)" : ("SUPPLEMENT(" + std::to_string(maxTaps) + ")");
    res.preset = p.name;
    res.combo = cb.id;
    res.in = cb.in;
    res.out = cb.out;
    res.transition = p.transition;

    dsp_measure::Result r;
    std::vector<double> h;
    if (maxTaps == 0) {
        h = dsp_measure::prototype(p, cb.in, cb.out, r);
    } else {
        // 补充测量：**只**抬高工程预算 maxPrototypeTaps，其余参数与生产逐项相同
        // （同一 apply_preset、同一 autoTaps 反解）。这不是生产预算，故单独标注。
        auto cfg = review2::production_config(p, cb.in, cb.out);
        cfg.maxPrototypeTaps = (int)maxTaps;
        rta::dsp::PolyphaseResampler rs(cfg);
        rs.prototype_into(h);
        const double up = (double)rs.upFactor();
        for (auto& x : h) x /= up;
        r.T = rs.stage1_taps_per_phase();
        r.stage2T = rs.stage2_taps_per_phase();
        r.L = rs.upFactor(); r.M = rs.downFactor(); r.cascade = rs.two_stage();
        r.fs = review2::prototype_rate(rs, cb.in, cb.out);
        r.taps = h.size();
        const auto mm = rta::dsp::preset_metrics(p, cb.in, cb.in == 96000 ? 40000 : 20000);
        r.passHz = mm.passHz; r.stopHz = (double)cb.in - mm.passHz;
        r.target = mm.ceilingDb; r.excluded = mm.excluded;
        r.finite = !h.empty() && std::all_of(h.begin(), h.end(),
                                            [](double v) { return std::isfinite(v); });
    }
    if (!r.finite) throw std::runtime_error("non-finite prototype");
    res.T = r.T; res.stage2T = r.stage2T; res.L = r.L; res.M = r.M;
    res.ntaps = h.size(); res.cascade = r.cascade; res.fs = r.fs;
    res.attenDb = r.target; res.target = r.target;
    res.passEdge = r.passHz; res.stopEdge = r.stopHz;
    res.dcDb = dsp_measure::direct(h, 0.0, res.fs);
    const double peakAbs = [&] {
        double m = 0;
        for (double x : h) m = std::max(m, std::fabs(x));
        return m;
    }();

    // ---- 自检 ①：与 filter_report --coefficients 导出的系数逐点比对 ----
    if (!reportDir.empty()) {
        const fs::path f = reportDir / (std::to_string(cb.in) + "-" + std::to_string(cb.out) +
                                        "-" + p.name + ".f64");
        std::error_code ec;
        if (fs::exists(f, ec)) {
            std::ifstream in(f, std::ios::binary);
            in.seekg(0, std::ios::end);
            const std::streamoff bytes = in.tellg();
            in.seekg(0);
            const size_t n = (size_t)bytes / sizeof(double);
            std::vector<double> ref(n);
            in.read(reinterpret_cast<char*>(ref.data()), (std::streamsize)(n * sizeof(double)));
            res.vRepN = std::min(n, h.size());
            double worst = 0;
            for (size_t i = 0; i < res.vRepN; ++i)
                worst = std::max(worst, std::fabs(ref[i] - h[i]));
            res.vRepMaxAbs = worst;
            res.vRepRel = peakAbs > 0 ? worst / peakAbs : worst;
            if (n != h.size()) res.status = "SIZE_MISMATCH";
        } else {
            res.vRepMaxAbs = std::nan("");
        }
    }

    // ---- 自检 ②③：真实 Pipeline::process() 的冲激响应 ----
    if (maxTaps == 0) {
        rta::engine::PipelineConfig pc;
        pc.outRate = cb.out;
        pc.outBits = 32;
        pc.outChannels = 1;
        pc.gpuResample = false;
        pc.useFixed = false;
        rta::engine::apply_filter_preset(pc, p);
        pc.autoTaps = true;                    // ★ 生产路径：rtaweb 显式打开
        rta::engine::Pipeline pipe(pc);
        const size_t frames = (size_t)(4LL * std::max(1, r.T) +
                                       4LL * std::max(0, r.stage2T) + 64);
        rta::audio::AudioBuffer buf;
        buf.rate = cb.in;
        buf.channels = 1;
        buf.bits = 32;
        buf.isFloat = true;
        buf.raw.assign(frames * 1, 0);
        buf.setSample(0, 0, 1.0);
        std::vector<std::vector<double>> dbl;
        rta::engine::PipelineStats st;
        pipe.process(buf, &st, &dbl);
        res.vPipeT = st.tapsPerPhase;
        res.vPipeT2 = st.stage2TapsPerPhase;
        if (dbl.empty()) {
            res.vPipeMaxRel = std::nan("");
        } else {
            const std::vector<double>& y = dbl[0];
            double worst = 0;
            const size_t stride = r.cascade ? 1u : (size_t)std::max(1, r.M);
            for (size_t i = 0; i < y.size(); ++i) {
                const size_t j = i * stride;
                if (j >= h.size()) break;
                worst = std::max(worst, std::fabs(y[i] - h[j] * (double)r.L));
                ++res.vPipeN;
            }
            res.vPipeMaxRel = peakAbs > 0 ? worst / (peakAbs * (double)r.L) : worst;
        }
        if (res.vPipeT != r.T || res.vPipeT2 != r.stage2T) res.status = "T_MISMATCH";
    } else {
        res.status = "NON_PRODUCTION_BUDGET";
        res.vPipeMaxRel = std::nan("");
        res.vPipeT = res.vPipeT2 = -1;
    }

    // ---- 网格 ----
    Grid g = build_grid(h, res.fs, maxPow);
    res.fftN = g.n;
    res.gridHz = g.df;
    const double fHi = res.fs * 0.5;
    const double fOutHalf = (double)cb.out * 0.5;
    const double fInHalf = (double)cb.in * 0.5;

    // ---- 定点值（DD） ----
    res.a1k = mag_db_dd(h, 1000.0, res.fs);
    res.a10k = (fHi > 10000.0) ? mag_db_dd(h, 10000.0, res.fs) : 0.0;
    res.a20k = (fHi > 20000.0) ? mag_db_dd(h, 20000.0, res.fs) : 0.0;
    res.a30k = (fHi > 30000.0) ? mag_db_dd(h, 30000.0, res.fs) : 0.0;

    // ---- 通带纹波（网格上取，通带内 |H|≈1，双精度噪声 ~1e-13） ----
    {
        const double lo = 20.0;
        const double hiP = std::min(20000.0, res.passEdge * 0.95);
        double mn = 1e9, mx = -1e9, worst = lo;
        for (size_t k = bin_of(g, lo); k <= bin_of(g, std::max(hiP, lo * 1.01)) && k <= g.n / 2; ++k) {
            const double v = g.db[k];
            if (v < mn) mn = v;
            if (v > mx) { mx = v; worst = g.df * (double)k; }
        }
        res.ripplePass = (mx > -1e8) ? (mx - mn) : 0.0;
        res.rippleWorstHz = worst;
        double mn2 = 1e9, mx2 = -1e9;
        for (size_t k = bin_of(g, lo); k <= bin_of(g, std::min(20000.0, fOutHalf)) && k <= g.n / 2; ++k) {
            mn2 = std::min(mn2, g.db[k]);
            mx2 = std::max(mx2, g.db[k]);
        }
        res.ripple20k = (mx2 > -1e8) ? (mx2 - mn2) : 0.0;
    }
    // 通带内最大偏差（[20 Hz, 设计通带边缘]），与验收工具 filter_sweep 的 pass_db 同口径
    {
        const double hiP = std::min(res.passEdge, fHi);
        for (size_t k = bin_of(g, 20.0); k <= bin_of(g, hiP) && k <= g.n / 2; ++k) {
            const double v = g.db[k];
            if (std::fabs(v) > std::fabs(res.passWorst)) { res.passWorst = v; res.passWorstHz = g.df * (double)k; }
        }
        const double e = mag_db_dd(h, hiP, res.fs);
        if (std::fabs(e) > std::fabs(res.passWorst)) { res.passWorst = e; res.passWorstHz = hiP; }
    }

    // ---- -1 / -3 dB 点（网格定位 + DD 二分细化） ----
    auto crossing = [&](double target) -> double {
        const size_t last = g.n / 2;
        for (size_t k = 1; k <= last; ++k) {
            if (g.db[k] <= target) {
                double lo = g.df * (double)(k - 1), hi = g.df * (double)k;
                for (int it = 0; it < 12; ++it) {
                    const double mid = std::sqrt(lo * hi);
                    if (mag_db_dd(h, mid, res.fs) <= target) hi = mid; else lo = mid;
                }
                return hi;
            }
        }
        return -1.0;
    };
    res.f1db = crossing(-1.0);
    res.f3db = crossing(-3.0103);

    // ---- 阻带起点 fsb80：之后网格点全部 <= -80 dB ----
    double stopStart = res.stopEdge;
    {
        const size_t last = g.n / 2;
        size_t first = last;
        while (first > 0 && g.db[first - 1] <= -80.0) --first;
        if (first < last) {
            double lo = g.df * (double)first, hi = g.df * (double)(first + 1);
            for (int it = 0; it < 12; ++it) {
                const double mid = std::sqrt(lo * hi);
                if (mag_db_dd(h, mid, res.fs) <= -80.0) hi = mid; else lo = mid;
            }
            res.fsb80 = hi;
            stopStart = hi;
        } else {
            res.fsb80 = fHi;
        }
    }

    // ---- 阻带最小值（最差点）：网格候选 + DD 细化 ----
    auto refine_stop = [&](double loHz, double hiHz, double& bestDb, double& bestHz) {
        const size_t k0 = bin_of(g, loHz), k1 = std::min(bin_of(g, hiHz), g.n / 2);
        if (k1 <= k0 || k1 - k0 < 4) { bestDb = 0; bestHz = 0; return; }
        std::vector<std::pair<double, size_t>> cand;
        bestDb = -1e9;
        for (size_t k = k0; k <= k1; ++k) {
            const double v = g.db[k];
            if (v > bestDb) { bestDb = v; bestHz = g.df * (double)k; }
            if (k > k0 && k < k1 && g.db[k] >= g.db[k - 1] && g.db[k] >= g.db[k + 1] && v > -400.0)
                cand.push_back({v, k});
        }
        std::sort(cand.begin(), cand.end(), [](auto a, auto b) { return a.first > b.first; });
        if (cand.size() > 3) cand.resize(3);
        const double half = 1.5 * g.df;
        for (auto& c : cand) {
            const double center = g.df * (double)c.second;
            for (int q = -3; q <= 3; ++q) {
                const double hz = center + half * (double)q / 3.0;
                if (hz < loHz || hz > hiHz) continue;
                const double v = mag_db_dd(h, hz, res.fs);
                if (v > bestDb) { bestDb = v; bestHz = hz; }
            }
        }
        // 端点（审核口径：过渡带端点与 Nyquist 必须纳入）
        for (double hz : {loHz, hiHz}) {
            if (hz <= 0 || hz > fHi) continue;
            const double v = mag_db_dd(h, hz, res.fs);
            if (v > bestDb) { bestDb = v; bestHz = hz; }
        }
        // 二次加密
        const double c2 = bestHz;
        for (int q = -3; q <= 3; ++q) {
            const double hz = c2 + half * (double)q / 12.0;
            if (hz < loHz || hz > hiHz) continue;
            const double v = mag_db_dd(h, hz, res.fs);
            if (v > bestDb) { bestDb = v; bestHz = hz; }
        }
    };
    {
        // 主指标：设计阻带内最差衰减（从设计阻带起点 stopEdge 起）
        double a = 0, b = 0;
        refine_stop(std::max(res.stopEdge, stopStart), std::min(fOutHalf, fHi), a, b);
        res.stopMin = a; res.stopMinHz = b;
        // 旧表口径：fsb80 之后最差（含过渡带裙边）
        double c = 0, d = 0;
        refine_stop(stopStart, std::min(fOutHalf, fHi), c, d);
        res.stopSkirt = c;
        // 全轴（含输出 Nyquist 以上）
        double e = 0, f = 0;
        refine_stop(stopStart, fHi, e, f);
        res.stopMinFull = e;
    }

    // ---- 群延迟 ----
    {
        const auto [g1, m1] = gd_ms_dd(h, 100.0, res.fs);   (void)m1;
        const auto [g2, m2] = gd_ms_dd(h, 1000.0, res.fs);  (void)m2;
        const auto [g3, m3] = gd_ms_dd(h, 10000.0, res.fs); (void)m3;
        res.gd100 = g1; res.gd1k = g2; res.gd10k = g3;
        // 标称：线性相位 = (T*L-1)/(2L) 输入样点；级联 = 两级之和；非线相位 = 0
        if (p.phase == rta::dsp::PhaseKind::Linear) {
            if (r.cascade) {
                int F = 1;
                for (int f : {4, 3, 2}) if (r.L % f == 0 && r.L / f >= 2) { F = f; break; }
                const double l1 = ((double)r.T * F - 1.0) / (2.0 * F);
                const double L2 = r.L / F;
                const double l2 = ((double)r.stage2T * L2 - 1.0) / (2.0 * L2);
                res.gdNom = (l1 + l2 / F) / (double)cb.in * 1000.0;
            } else {
                res.gdNom = ((double)r.T * r.L - 1.0) / (2.0 * r.L) / (double)cb.in * 1000.0;
            }
        } else {
            res.gdNom = 0.0;
        }
    }

    // ---- 预/后振铃（时域冲激包络，-60 dB re 峰值） ----
    {
        size_t pk = 0;
        double pm = 0;
        for (size_t i = 0; i < h.size(); ++i) if (std::fabs(h[i]) > pm) { pm = std::fabs(h[i]); pk = i; }
        const double thr = pm * 1e-3;      // -60 dB
        size_t first = 0, last = h.size() ? h.size() - 1 : 0;
        for (size_t i = 0; i < h.size(); ++i) if (std::fabs(h[i]) > thr) { first = i; break; }
        for (size_t i = h.size(); i-- > 0;) if (std::fabs(h[i]) > thr) { last = i; break; }
        res.peakMs = (double)pk / res.fs * 1000.0;
        res.preRingMs = (double)(pk - std::min(pk, first)) / res.fs * 1000.0;
        res.postRingMs = (double)(last > pk ? last - pk : 0) / res.fs * 1000.0;
    }

    // ---- 镜像带（网格） ----
    {
        double best = -1e9;
        const double lo = std::max(fInHalf, (double)cb.in - 20000.0);
        const double hi = std::min(fOutHalf, (double)cb.in + 20000.0);
        if (hi > lo) for (size_t k = bin_of(g, lo); k <= bin_of(g, hi); ++k) best = std::max(best, g.db[k]);
        else best = 0.0;
        res.imgPeak = best;
        double full = -1e9;
        if (fInHalf < std::min(fOutHalf, fHi))
            for (size_t k = bin_of(g, fInHalf); k <= bin_of(g, std::min(fOutHalf, fHi)); ++k)
                full = std::max(full, g.db[k]);
        else full = 0.0;
        res.imgPeakFull = full;
    }

    // ---- 曲线（供绘图）：对数 500 点 + 0..24k 线性 300 点 ----
    {
        const double fTop = std::min(fHi, 400000.0);
        const double fLo = 10.0;
        auto push = [&](double f) {
            if (f <= 0 || f > fTop) return;
            const size_t k = bin_of(g, f);
            res.curveF.push_back(f);
            res.curveDb.push_back(g.db[k]);
            res.curveGd.push_back(g.gd[k]);
        };
        for (int i = 0; i <= 500; ++i)
            push(fLo * std::pow(fTop / fLo, (double)i / 500.0));
        const double fLin = std::min(24000.0, fTop);
        for (int i = 0; i <= 300; ++i) push(fLin * (double)i / 300.0);
    }
    return res;
}

// ---------------------------------------------------------------- TSV
std::string metrics_header() {
    return "preset\tin_rate\tout_rate\tT\tstage2_T\tL\tM\tprototype_taps\ttopology\tprototype_rate_hz"
           "\tfft_n\tgrid_hz\ttransition\tatten_target_db\tpass_edge_hz\tstop_edge_hz\tdc_db"
           "\ta1k_db\ta10k_db\ta20k_db\ta30k_db\tf_minus1_hz\tf_minus3_hz"
           "\tripple_pass_db\tripple_pass_worst_hz\tripple_20k_db\tpass_worst_db\tpass_worst_hz"
           "\tfsb80_hz\tstop_min_db\tstop_min_hz\tstop_skirt_db\tstop_min_full_db"
           "\tgd100_ms\tgd1k_ms\tgd10k_ms\tgd_nom_ms\tpeak_ms\tpre_ring_ms\tpost_ring_ms"
           "\timage_peak_db\timage_peak_full_db"
           "\tverify_report_max_abs\tverify_report_rel\tverify_report_n"
           "\tverify_pipeline_max_rel\tverify_pipeline_n\tverify_pipeline_T\tverify_pipeline_stage2_T\tstatus\tbudget\n";
}
void write_metrics_row(std::ostream& os, const Result& r) {
    os << std::setprecision(10);
    os << r.preset << '\t' << r.in << '\t' << r.out << '\t' << r.T << '\t' << r.stage2T
       << '\t' << r.L << '\t' << r.M << '\t' << r.ntaps << '\t'
       << (r.cascade ? "cascade" : "single") << '\t' << r.fs << '\t' << r.fftN << '\t' << r.gridHz
       << '\t' << r.transition << '\t' << r.target << '\t' << r.passEdge << '\t' << r.stopEdge
       << '\t' << r.dcDb << '\t' << r.a1k << '\t' << r.a10k << '\t' << r.a20k << '\t' << r.a30k
       << '\t' << r.f1db << '\t' << r.f3db
       << '\t' << r.ripplePass << '\t' << r.rippleWorstHz << '\t' << r.ripple20k
       << '\t' << r.passWorst << '\t' << r.passWorstHz
       << '\t' << r.fsb80 << '\t' << r.stopMin << '\t' << r.stopMinHz << '\t' << r.stopSkirt << '\t' << r.stopMinFull
       << '\t' << r.gd100 << '\t' << r.gd1k << '\t' << r.gd10k << '\t' << r.gdNom
       << '\t' << r.peakMs << '\t' << r.preRingMs << '\t' << r.postRingMs
       << '\t' << r.imgPeak << '\t' << r.imgPeakFull
       << '\t' << r.vRepMaxAbs << '\t' << r.vRepRel << '\t' << r.vRepN
       << '\t' << r.vPipeMaxRel << '\t' << r.vPipeN << '\t' << r.vPipeT << '\t' << r.vPipeT2
       << '\t' << r.status << '\t' << r.budget << '\n';
}

void usage() {
    std::cout <<
        "Usage: fr_probe --out DIR [--report-dir DIR] [--combo ID] [--preset NAME]\n"
        "                [--limit N] [--jobs N] [--fft-pow 14..24] [--max-taps N]\n"
        "  --max-taps N (>0) raises only the engineering coefficient budget; output goes to\n"
        "  metrics-<combo>-supp.tsv and every row is tagged NON_PRODUCTION_BUDGET.\n"
        "  Emits DIR/metrics-<combo>.tsv and DIR/curves-<combo>.tsv plus DIR/selfcheck.txt.\n"
        "  --report-dir points at filter_report --coefficients output for the bit-level cross-check.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        fs::path outDir;
        fs::path reportDir;
        fs::path failedList;
        std::string comboFilter, presetFilter;
        unsigned limit = 0, fftPow = 22;
        unsigned long long maxTaps = 0;
        int jobs = (int)std::thread::hardware_concurrency() - 1;
        if (jobs < 1) jobs = 1;
        if (jobs > 8) jobs = 8;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto val = [&](const char* what) {
                if (++i >= argc) throw std::runtime_error(std::string("missing value for ") + what);
                return std::string(argv[i]);
            };
            if (a == "--out") outDir = fs::u8path(val("--out"));
            else if (a == "--report-dir") reportDir = fs::u8path(val("--report-dir"));
            else if (a == "--combo") comboFilter = val("--combo");
            else if (a == "--preset") presetFilter = val("--preset");
            else if (a == "--limit") limit = (unsigned)std::stoul(val("--limit"));
            else if (a == "--jobs") jobs = std::max(1, std::stoi(val("--jobs")));
            else if (a == "--fft-pow") fftPow = (unsigned)std::stoul(val("--fft-pow"));
            else if (a == "--max-taps") maxTaps = std::stoull(val("--max-taps"));
            else if (a == "--failed-list") failedList = fs::u8path(val("--failed-list"));
            else if (a == "--help" || a == "-h") { usage(); return 0; }
            else throw std::runtime_error("unknown option: " + a);
        }
        if (outDir.empty()) { usage(); return 2; }
        if (fftPow < 14 || fftPow > 24) throw std::runtime_error("--fft-pow must be 14..24");
        fs::create_directories(outDir);

        std::set<std::string> wanted;      // "combo<TAB>preset" from --failed-list
        if (!failedList.empty()) {
            std::ifstream in(failedList);
            if (!in) throw std::runtime_error("cannot read --failed-list");
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                wanted.insert(line);
            }
            if (wanted.empty()) throw std::runtime_error("--failed-list is empty");
        }
        const auto& presets = rta::dsp::filter_presets();
        struct Task { const rta::dsp::FilterPreset* p; const ComboDef* c; };
        std::vector<Task> tasks;
        for (int ci = 0; ci < kNumCombos; ++ci) {
            if (!comboFilter.empty() && comboFilter != kCombos[ci].id) continue;
            for (const auto& p : presets) {
                if (!presetFilter.empty() && presetFilter != p.name) continue;
                if (!wanted.empty() &&
                    !wanted.contains(std::string(kCombos[ci].id) + "	" + p.name)) continue;
                tasks.push_back({&p, &kCombos[ci]});
                if (limit && tasks.size() >= limit) break;
            }
            if (limit && tasks.size() >= limit) break;
        }
        if (tasks.empty()) throw std::runtime_error("zero selected identities");

        std::vector<Result> results(tasks.size());
        std::atomic<size_t> next{0};
        std::atomic<size_t> done{0};
        std::vector<std::thread> pool;
        const auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < jobs; ++t) {
            pool.emplace_back([&] {
                for (;;) {
                    const size_t i = next.fetch_add(1);
                    if (i >= tasks.size()) break;
                    try {
                        results[i] = measure_one(*tasks[i].p, *tasks[i].c, reportDir, fftPow, maxTaps);
                    } catch (const std::exception& e) {
                        results[i].preset = tasks[i].p->name;
                        results[i].combo = tasks[i].c->id;
                        results[i].in = tasks[i].c->in;
                        results[i].out = tasks[i].c->out;
                        results[i].status = std::string("ERROR: ") + e.what();
                        results[i].budget = (maxTaps == 0) ? "PRODUCTION(2^21)"
                                                          : ("SUPPLEMENT(" + std::to_string(maxTaps) + ")");
                    }
                    const size_t n = done.fetch_add(1) + 1;
                    if ((n % 8) == 0 || n == tasks.size())
                        std::fprintf(stderr, "  %zu/%zu (%.1fs)\n", n, tasks.size(),
                                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                }
            });
        }
        for (auto& th : pool) th.join();

        // ---- 落盘 ----
        const std::string suffix = (maxTaps == 0) ? "" : "-supp";
        for (int ci = 0; ci < kNumCombos; ++ci) {
            std::vector<const Result*> rows;
            for (const auto& r : results) if (r.combo == kCombos[ci].id) rows.push_back(&r);
            if (rows.empty()) continue;
            {
                std::ofstream os(outDir / (std::string("metrics-") + kCombos[ci].id + suffix + ".tsv"));
                os << "# RLPlayer 3.0.10 实测频响关键点  combo=" << kCombos[ci].label
                   << "  in=" << kCombos[ci].in << " out=" << kCombos[ci].out << "\n";
                os << "# 系数身份：dsp_measure::prototype()（apply_preset + autoTaps 反解），与 filter_report 同源。\n";
                os << "# 幅频：|H(f)|=|DTFT{h}(2f/fs_proto)|，单级 fs_proto=in*L，级联 fs_proto=out。\n";
                os << metrics_header();
                for (const Result* r : rows) write_metrics_row(os, *r);
            }
            {
                std::ofstream os(outDir / (std::string("curves-") + kCombos[ci].id + suffix + ".tsv"));
                os << "# curves combo=" << kCombos[ci].label << "\n";
                os << "# freq_hz\tmag_db\tgd_ms\t(每段前一行 # <preset>)\n";
                os << std::setprecision(8);
                for (const Result* r : rows) {
                    os << "# " << r->preset << '\n';
                    for (size_t i = 0; i < r->curveF.size(); ++i)
                        os << r->curveF[i] << '\t' << r->curveDb[i] << '\t' << r->curveGd[i] << '\n';
                }
            }
        }

        // ---- 自检汇总 ----
        {
            std::ofstream os(outDir / ("selfcheck" + suffix + ".txt"));
            double worstRep = 0, worstRepRel = 0, worstPipe = 0;
            std::string worstRepName, worstPipeName;
            size_t nRep = 0, nPipe = 0, mismatch = 0;
            for (const auto& r : results) {
                if (r.status.rfind("ERROR", 0) == 0) { ++mismatch; continue; }
                if (std::isfinite(r.vRepMaxAbs)) {
                    ++nRep;
                    if (r.vRepMaxAbs > worstRep) { worstRep = r.vRepMaxAbs; worstRepName = r.preset + "/" + r.combo; }
                    if (r.vRepRel > worstRepRel) worstRepRel = r.vRepRel;
                }
                if (std::isfinite(r.vPipeMaxRel)) {
                    ++nPipe;
                    if (r.vPipeMaxRel > worstPipe) { worstPipe = r.vPipeMaxRel; worstPipeName = r.preset + "/" + r.combo; }
                }
                if (r.status != "OK") ++mismatch;
            }
            os << std::setprecision(6);
            os << "identities=" << results.size() << "\n";
            os << "selfcheck_report_compared=" << nRep << " max_abs=" << worstRep
               << " worst=" << worstRepName << " max_rel=" << worstRepRel << "\n";
            os << "selfcheck_pipeline_compared=" << nPipe << " max_rel=" << worstPipe
               << " worst=" << worstPipeName << "\n";
            os << "non_ok_rows=" << mismatch << "\n";
            std::cout << std::setprecision(6)
                      << "SELFCHECK report_rows=" << nRep << " max_abs=" << worstRep
                      << " (" << worstRepName << ")\n"
                      << "SELFCHECK pipeline_rows=" << nPipe << " max_rel=" << worstPipe
                      << " (" << worstPipeName << ")\n"
                      << "SELFCHECK non_ok_rows=" << mismatch << "\n";
        }
        std::cout << "DONE identities=" << results.size()
                  << " elapsed_s=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
                  << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 2;
    }
}
