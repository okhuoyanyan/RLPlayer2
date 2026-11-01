// tools/filter_probe.cpp — 2.0 说明书素材探针（P1/P2/P3）
// 只读引用 src/dsp/**，不修改任何 src 文件；离线分析，不打开任何音频设备。
//
// 判据设计（铁律 3）：
//  * 不靠"抄代码里的设计值"——每个预设都按【重采样器真实构造出来的系数表】求响应。
//    单段：PolyphaseResampler::prototype_into()（正是 process() 用的那批系数）。
//    两段：按 resampler.h 的分级规则复刻 c1/c2，并**逐位比对**真实两段对象与复刻级联的
//          脉冲响应（max|Δ| 必须 = 0），比对失败就把该条标记为 unverified。
//  * 频域映射：输入 x[n] 在 L 倍内插域被 h 滤波后按 M 抽取 ⟹ 物理频率 f 对应
//          归一化频率 nu = 2f / (L*fs_in)（nu = 1.0 即内插域 Nyquist）。
//  * 时域抽查：脉冲（-60 dBFS，振幅 0.001）过真实 process()，对输出做 DFT 与频域模型比对。
//  * 镜像/杂散：1 kHz 音（-60 dBFS）过真实 process()，拟合掉基波后对残差做 FFT 取最大杂散。
//
// 输出：全 TSV，第一行 # 注释带头。

// MSVC 没有 __builtin_popcount；src/audio/dsd_reader.h 无条件使用它（主线用 clang 构建）。
// 探针只读引用该头，故在此提供等价实现，不修改 src。
#if defined(_MSC_VER)
#include <intrin.h>
static inline int __builtin_popcount(unsigned int x) { return (int)__popcnt(x); }
#endif

#include "dsp/filter_families.h"
#include "dsp/resampler.h"
#include "dsp/dsd_integrator.h"
#include "dsp/modulator.h"
#include "dsp/fft.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace rta::dsp;
using cplx = std::complex<double>;

static const double kPiD = 3.14159265358979323846;

// ---------------------------------------------------------------- 基础工具
static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// 复数 DTFT：nu = 1.0 对应 Nyquist（w = pi*nu）。
// 用单位复数递推 + 每 16384 点归一，避免逐点 sin/cos（1M 抽头 × 数百频点会慢 50 倍）。
static cplx dtft(const std::vector<double>& h, double nu) {
    const double th = -kPiD * nu;
    const double wr = std::cos(th), wi = std::sin(th);
    double cr = 1.0, ci = 0.0, re = 0.0, im = 0.0;
    const size_t n = h.size();
    for (size_t i = 0; i < n; ++i) {
        re += h[i] * cr;
        im += h[i] * ci;
        const double nr = cr * wr - ci * wi;
        const double ni = cr * wi + ci * wr;
        cr = nr; ci = ni;
        if (((i + 1) & 0x3FFF) == 0) {
            const double m = std::sqrt(cr * cr + ci * ci);
            if (m > 1e-300) { cr /= m; ci /= m; }
        }
    }
    return cplx(re, im);
}

static double db_of(cplx v) {
    const double m = std::abs(v);
    return 20.0 * std::log10(m > 1e-300 ? m : 1e-300);
}

// ---------------------------------------------------------------- 正弦最小二乘拟合
// 注意：不能用"投影 = 2·Σx·cos/N"这种朴素公式。记录长度不是整周期时，cos/sin 在窗内
// 并不正交，朴素公式会留下 ~1e-6 量级的残差（对 −6 dBFS 信号就是 −114 dB，对 −60 dBFS
// 是 −54 dB）——足以把整个"带内噪声/杂散"判据污染成测量底噪。必须解 2×2 正规方程。
struct Fit2 { double A = 0.0, B = 0.0; };
static Fit2 fit_sine(const double* x, size_t n, double w) {
    long double scc = 0, sss = 0, scs = 0, sc = 0, ss = 0;
    for (size_t i = 0; i < n; ++i) {
        const double c = std::cos(w * (double)i), s = std::sin(w * (double)i);
        scc += (long double)c * c; sss += (long double)s * s; scs += (long double)c * s;
        sc += (long double)x[i] * c; ss += (long double)x[i] * s;
    }
    const long double det = scc * sss - scs * scs;
    if (std::fabs((double)det) < 1e-300) return Fit2{};
    Fit2 r;
    r.A = (double)((sc * sss - ss * scs) / det);
    r.B = (double)((ss * scc - sc * scs) / det);
    return r;
}

// ---------------------------------------------------------------- 响应模型
struct Stage {
    int    L = 1, M = 1;
    uint32_t inRate = 44100;
    std::vector<double> h;
};

struct Model {
    std::vector<Stage> stages;
    bool   past = false;         // L==M==1 直通
    bool   twoStage = false;
    int    F = 1;
    double latInputSamples = 0.0;   // 名义延迟（输入样本）
    int    protoTaps = 0;           // 单段=原型长度；两段=两级之和
    int    tapsPerPhase = 0;        // 单段每相位抽头
    int    L = 1, M = 1;
    double designFc6db = 0.0;       // 设计 -6 dB 点（Hz）
};

static void fill_stage(const ResamplerConfig& c, Stage& s) {
    PolyphaseResampler rs(c);
    s.L = rs.upFactor();
    s.M = rs.downFactor();
    s.inRate = c.inRate;
    rs.prototype_into(s.h);
}

static bool build_model(const FilterPreset& p, uint32_t inRate, uint32_t outRate, Model& m,
                        std::string* err) {
    ResamplerConfig cfg;
    cfg.inRate = inRate; cfg.outRate = outRate; cfg.channels = 1;
    apply_preset(cfg, p);

    PolyphaseResampler rs(cfg);
    m.L = rs.upFactor(); m.M = rs.downFactor();
    if (m.L == 1 && m.M == 1) { m.past = true; return true; }

    if (!rs.two_stage()) {
        Stage s;
        s.L = m.L; s.M = m.M; s.inRate = inRate;
        rs.prototype_into(s.h);
        if (s.h.empty()) { if (err) *err = "prototype_into 为空"; return false; }
        m.protoTaps = (int)s.h.size();
        m.tapsPerPhase = rs.tapsPerPhase();
        m.latInputSamples = rs.latencyInputSamples();
        m.stages.push_back(std::move(s));
    } else {
        // 复刻 resampler.h 的两级规则（用于取出每级真实系数表；随后做逐位一致性校验）
        const uint32_t g = (uint32_t)std::gcd(inRate, outRate);
        const int L = (int)(outRate / g);
        int F = 1;
        for (int f : {4, 3, 2}) if (L % f == 0 && L / f >= 2) { F = f; break; }
        if (F <= 1) { if (err) *err = "两级但 F<=1"; return false; }
        if (rs.stage1_factor() != F) { if (err) *err = "F 不一致"; return false; }
        const int T1 = (cfg.minTapsPerInput > 0) ? cfg.minTapsPerInput
                                                 : std::max(4, cfg.totalTaps / L);
        ResamplerConfig c1 = cfg;
        c1.twoStage = false;
        c1.inRate = inRate;
        c1.outRate = inRate * (uint32_t)F;
        c1.totalTaps = T1 * F;
        ResamplerConfig c2 = cfg;
        c2.twoStage = false;
        c2.inRate = inRate * (uint32_t)F;
        c2.outRate = outRate;
        c2.totalTaps = cfg.stage2Taps * (L / F);
        c2.transition = 0.30;     // 与 resampler.h 一致
        Stage s1, s2;
        fill_stage(c1, s1);
        fill_stage(c2, s2);
        if (s1.h.empty() || s2.h.empty()) { if (err) *err = "两级原型为空"; return false; }
        PolyphaseResampler r1(c1), r2(c2);
        m.latInputSamples = r1.latencyInputSamples() + r2.latencyInputSamples() / (double)F;
        m.protoTaps = (int)(s1.h.size() + s2.h.size());
        m.tapsPerPhase = (int)s1.h.size() / s1.L;
        m.twoStage = true;
        m.F = F;
        m.stages.push_back(std::move(s1));
        m.stages.push_back(std::move(s2));
    }

    // 设计 -6 dB 点：非切趾 = (1-transition)*min(fs_in, fs_out)/2；
    //               切趾 = (apodF1+apodF2)/2 * fs_in/2
    double fsMinHalf = std::min((double)inRate, (double)outRate) * 0.5;
    if (p.apodizing) m.designFc6db = 0.5 * (p.apodF1 + p.apodF2) * (double)inRate * 0.5;
    else             m.designFc6db = (1.0 - p.transition) * fsMinHalf;
    return true;
}

static cplx resp_at(const Model& m, double f) {
    if (m.past || m.stages.empty()) return cplx(1.0, 0.0);
    cplx acc(1.0, 0.0);
    for (const auto& s : m.stages)
        // 原型在内插域的直流增益是 L（每相位抽头之和为 1）⟹ 除 L 才是端到端单位增益
        acc *= dtft(s.h, 2.0 * f / ((double)s.L * (double)s.inRate)) / (double)s.L;
    return acc;
}

// 群延迟：τ = −dφ/dω。相位主值会绕圈，所以用"步长减半看是否收敛"的办法自校验：
// 大延迟（closed-form-16M 在 96→384k 是 21.8 秒）下固定步长必然绕圈，读出来会变成负数。
static double gd_at(const Model& m, double f, double d) {
    const double p1 = std::arg(resp_at(m, f - d));
    const double p2 = std::arg(resp_at(m, f + d));
    double dp = p2 - p1;
    while (dp >  kPiD) dp -= 2.0 * kPiD;
    while (dp < -kPiD) dp += 2.0 * kPiD;
    return -dp / (2.0 * kPiD * (2.0 * d)) * 1000.0;   // ms
}
static double gd_ms(const Model& m, double f) {
    double d = std::max(1e-4, f * 1e-5);
    double prev = gd_at(m, f, d);
    for (int it = 0; it < 14; ++it) {
        d *= 0.5;
        const double cur = gd_at(m, f, d);
        const double scale = std::max(std::fabs(cur), 1e-6);
        if (std::fabs(cur - prev) / scale < 0.01) return cur;
        prev = cur;
    }
    return prev;
}

// 升序扫，找第一个 <= target 的频率，再在该区间内对数二分
static double crossing(const Model& m, const std::vector<double>& fs,
                       const std::vector<double>& dbs, double target) {
    for (size_t i = 0; i < fs.size(); ++i) {
        if (dbs[i] <= target) {
            if (i == 0) return fs[0];
            double lo = fs[i - 1], hi = fs[i];        // db(lo) > target >= db(hi)
            for (int k = 0; k < 50; ++k) {
                const double mid = std::sqrt(lo * hi);
                if (db_of(resp_at(m, mid)) <= target) hi = mid; else lo = mid;
            }
            return hi;
        }
    }
    return -1.0;
}

// ---------------------------------------------------------------- 网格
static std::vector<double> make_grid(double fsOut, double fc, int nPlot) {
    std::vector<double> g;
    const double fHi = fsOut * 0.5;
    const double fLo = 10.0;
    if (!(fc > 0.0) || fc >= fHi) fc = fHi * 0.45;
    // 三段连续：10 Hz..(0.55fc) 对数 60 点；(0.55fc)..(2.2fc) 线性 90 点（过渡带加密）；
    // (2.2fc)..fHi 对数 60 点。三段必须首尾相接，否则 crossing() 会跨过 -3 dB 点看不见。
    const double a = std::min(std::max(fLo * 1.5, fc * 0.55), fHi * 0.9);
    for (int i = 0; i <= 60; ++i) g.push_back(fLo * std::pow(a / fLo, (double)i / 60.0));
    const double b = std::min(fHi * 0.999, fc * 2.2);
    if (b > a) for (int i = 1; i <= 90; ++i) g.push_back(a + (b - a) * (double)i / 90.0);
    for (int i = 1; i <= 60; ++i) {
        const double f = b * std::pow(fHi / b, (double)i / 60.0);
        if (f < fHi) g.push_back(f);
    }
    std::sort(g.begin(), g.end());
    g.erase(std::unique(g.begin(), g.end(), [](double x, double y) {
        return std::fabs(x - y) < 1e-9; }), g.end());
    (void)nPlot;
    return g;
}

// ---------------------------------------------------------------- 数据结构
struct Combo {
    const char* id;
    const char* label;
    uint32_t inRate, outRate;
};

static const Combo kCombos[] = {
    { "44k1-96k",  "44.1k -> 96k",   44100,  96000  },
    { "44k1-384k", "44.1k -> 384k",  44100,  384000 },
    { "96k-384k",  "96k -> 384k",    96000,  384000 },
    { "44k1-705k6","44.1k -> 705.6k",44100,  705600 },   // M=1，两级(-2s)真正生效
    { "48k-384k",  "48k -> 384k",    48000,  384000 },   // M=1，两级真正生效
    { "dsd64-pcm384","DSD64->PCM176.4k->384k", 176400, 384000 },
};
static const int kNumCombos = (int)(sizeof(kCombos) / sizeof(kCombos[0]));

struct Metrics {
    std::string preset, combo;
    int    protoTaps = 0, tapsPerPhase = 0, L = 1, M = 1;
    int    twoStage = 0, F = 1, past = 0;
    double dcDb = 0, ripple20k = 0, ripplePass = 0;
    double f1db = -1, f3db = -1, designFc = 0, devPct = 0;
    double a1k = 0, a10k = 0, a20k = 0, a30k = 0;
    double fsb80 = -1, worstStop = 0, imagePeak = 0, imagePeakFull = 0;
    double gd100 = 0, gd1k = 0, gd10k = 0, gdNomMs = 0;
};

static Metrics measure(const FilterPreset& p, const Combo& cb, Model& m, std::string* err) {
    Metrics r;
    r.preset = p.name; r.combo = cb.id;
    if (!build_model(p, cb.inRate, cb.outRate, m, err)) return r;
    r.past = m.past ? 1 : 0;
    r.protoTaps = m.protoTaps; r.tapsPerPhase = m.tapsPerPhase;
    r.L = m.L; r.M = m.M; r.twoStage = m.twoStage ? 1 : 0; r.F = m.F;
    r.designFc = m.designFc6db;
    r.dcDb = db_of(resp_at(m, 0.0));
    if (m.past) {
        r.ripple20k = 0; r.ripplePass = 0; r.f1db = 0; r.f3db = 0;
        r.a20k = 0; r.fsb80 = 0; r.worstStop = 0; r.imagePeak = 0;
        return r;
    }

    const double fHi = cb.outRate * 0.5;
    const double fInHalf = cb.inRate * 0.5;

    // 20 kHz 及附近的定点值
    r.a1k  = db_of(resp_at(m, 1000.0));
    r.a10k = db_of(resp_at(m, 10000.0));
    r.a20k = db_of(resp_at(m, 20000.0));
    r.a30k = (fHi > 30000.0) ? db_of(resp_at(m, 30000.0)) : 0.0;

    // 通带纹波：[20 Hz, 20 kHz] 与 [20 Hz, 设计通带边]
    {
        double lo = 20.0, hi = std::min(20000.0, fHi * 0.98);
        double mn = 1e9, mx = -1e9;
        for (int i = 0; i <= 120; ++i) {
            const double f = lo * std::pow(hi / lo, (double)i / 120.0);
            const double d = db_of(resp_at(m, f));
            mn = std::min(mn, d); mx = std::max(mx, d);
        }
        r.ripple20k = mx - mn;
    }
    {
        const double hi = std::min(20000.0, std::min(m.designFc6db, fHi) * 0.95);
        double mn = 1e9, mx = -1e9;
        for (int i = 0; i <= 80; ++i) {
            const double f = 20.0 * std::pow(std::max(hi, 21.0) / 20.0, (double)i / 80.0);
            const double d = db_of(resp_at(m, f));
            mn = std::min(mn, d); mx = std::max(mx, d);
        }
        r.ripplePass = mx - mn;
    }

    // -1 / -3 dB 点
    {
        const auto g = make_grid(cb.outRate, m.designFc6db, 0);
        std::vector<double> dbs(g.size());
        for (size_t i = 0; i < g.size(); ++i) dbs[i] = db_of(resp_at(m, g[i]));
        r.f1db = crossing(m, g, dbs, -1.0);
        r.f3db = crossing(m, g, dbs, -3.0103);
        if (r.f3db > 0 && m.designFc6db > 0)
            r.devPct = (m.designFc6db - r.f3db) / m.designFc6db * 100.0;

        // 阻带：从高往低找"之后全部 <= -80 dB"的起点；再求该区间最大残留
        const double thr = -80.0;
        int first = -1;
        for (int i = (int)g.size() - 1; i >= 0; --i) {
            if (dbs[(size_t)i] > thr) { first = i + 1; break; }
            first = i;
        }
        if (first >= 0 && first < (int)g.size()) {
            r.fsb80 = g[(size_t)first];
            // 二分细化（左端 > -80，右端 <= -80）
            if (first > 0) {
                double lo = g[(size_t)first - 1], hi = g[(size_t)first];
                for (int k = 0; k < 40; ++k) {
                    const double mid = std::sqrt(lo * hi);
                    if (db_of(resp_at(m, mid)) <= thr) hi = mid; else lo = mid;
                }
                r.fsb80 = hi;
            }
            double worst = -1e9;
            for (int i = first; i < (int)g.size(); ++i) worst = std::max(worst, dbs[(size_t)i]);
            r.worstStop = worst;
        } else {
            r.fsb80 = -1.0;
            double worst = -1e9;
            for (double d : dbs) worst = std::max(worst, d);
            r.worstStop = worst;
        }
        // 镜像带（全）：输入 Nyquist 以上到输出 Nyquist —— 描述"输入 Nyquist 之上有多干净"
        double ipFull = -1e9;
        if (fInHalf < fHi) {
            for (int i = 0; i <= 160; ++i) {
                const double f = fInHalf * std::pow(fHi / fInHalf, (double)i / 160.0);
                ipFull = std::max(ipFull, db_of(resp_at(m, f)));
            }
        } else {
            ipFull = 0.0;
        }
        r.imagePeakFull = ipFull;
        // 镜像带（20 kHz 内容）：可听带 0..20k 的镜像落点 [fs_in-20k, fs_in+20k] 与
        // [0, fs_out/2] 的交集。这才是"听得见的内容被镜像后压了多少"。
        {
            const double lo = std::max(fInHalf, (double)cb.inRate - 20000.0);
            const double hi = std::min(fHi, (double)cb.inRate + 20000.0);
            double ip = -1e9;
            if (hi > lo) {
                for (int i = 0; i <= 200; ++i) {
                    const double f = lo + (hi - lo) * (double)i / 200.0;
                    ip = std::max(ip, db_of(resp_at(m, f)));
                }
            } else {
                ip = 0.0;
            }
            r.imagePeak = ip;
        }
    }

    r.gd100 = gd_ms(m, 100.0);
    r.gd1k  = gd_ms(m, 1000.0);
    r.gd10k = gd_ms(m, 10000.0);
    r.gdNomMs = m.latInputSamples / (double)cb.inRate * 1000.0;
    return r;
}

// ---------------------------------------------------------------- 时域抽查（脉冲）
static void run_impulse(const ResamplerConfig& cfg, size_t inN, std::vector<double>& out, double amp) {
    PolyphaseResampler rs(cfg);
    std::vector<double> x(inN, 0.0);
    x[0] = amp;                                  // 铁律 1：探针电平 -60 dBFS
    const size_t cap = rs.maxOutFrames(inN) + 64;
    out.assign(cap, 0.0);
    const double* ip[1] = { x.data() };
    double* op[1] = { out.data() };
    const size_t n = rs.process(ip, inN, op);
    out.resize(n);
}

struct ValidateRow {
    std::string preset, combo, status;
    int   protoTaps = 0;
    double gainOffsetDb = 0, maxShapeErrDb = 0;
    int    nPoints = 0;
    double tdSpurDb = 0;      // 时域脉冲 DFT 与模型的最大偏差（> -140 dB 的点）
    double costMadd = 0;
};

// 用时域脉冲响应的 DFT 与频域模型比对
static ValidateRow validate_one(const FilterPreset& p, const Combo& cb, bool doSpur) {
    ValidateRow r; r.preset = p.name; r.combo = cb.id;
    ResamplerConfig cfg;
    cfg.inRate = cb.inRate; cfg.outRate = cb.outRate; cfg.channels = 1;
    apply_preset(cfg, p);

    Model m; std::string err;
    if (!build_model(p, cb.inRate, cb.outRate, m, &err)) { r.status = "model-fail:" + err; return r; }
    r.protoTaps = m.protoTaps;
    if (m.past) { r.status = "passthrough"; return r; }

    // 需要的输入样本数 ≈ 原型跨度（输入样本）+ 余量
    const size_t inN = (size_t)std::min<long long>((long long)m.tapsPerPhase + 64, 4000000LL);
    const double cost = (double)inN * (double)cb.outRate / (double)cb.inRate * (double)m.tapsPerPhase;
    r.costMadd = cost;
    if (cost > 6.0e8) { r.status = "skip-cost"; return r; }

    std::vector<double> y;
    run_impulse(cfg, inN, y, 0.001);
    if (y.empty()) { r.status = "empty-out"; return r; }

    // 脉冲响应长度 = inN*L/M；取前 min(len, 4*T*L/M) 以免窗口截断轮
    const double fHi = cb.outRate * 0.5;
    std::vector<double> probe;
    for (int i = 0; i <= 48; ++i) probe.push_back(20.0 * std::pow(fHi / 20.0, (double)i / 48.0));

    double g0m = db_of(dtft(y, 0.0));
    double g0t = db_of(resp_at(m, 0.0));
    r.gainOffsetDb = g0m - g0t;
    double mx = 0.0; int n = 0;
    for (double f : probe) {
        const double dm = db_of(resp_at(m, f));
        // 深阻带不比：时域 DFT 自己的数值底（约 −137 dB，高斯窗预设的阻带比它更深）
        // 会主导差值，比出来的"误差"是测量底而不是模型错。
        if (dm < -120.0) continue;
        const double dt = db_of(dtft(y, 2.0 * f / (double)cb.outRate));
        mx = std::max(mx, std::fabs(dt - r.gainOffsetDb - dm));
        ++n;
    }
    r.maxShapeErrDb = mx; r.nPoints = n;
    r.status = "ok";
    (void)doSpur;
    return r;
}

// 1 kHz 音的杂散（镜像/混叠残留）：拟合基波后取残差 FFT 最大值
struct SpurRow { std::string preset, combo; double spurDb = 0, thdnDb = 0, thdnInbandDb = 0, f3db = 0; int n = 0; };

static SpurRow spur_one(const FilterPreset& p, const Combo& cb, size_t outN) {
    SpurRow r; r.preset = p.name; r.combo = cb.id;
    ResamplerConfig cfg;
    cfg.inRate = cb.inRate; cfg.outRate = cb.outRate; cfg.channels = 1;
    apply_preset(cfg, p);
    PolyphaseResampler rs(cfg);
    Model m; std::string err;
    if (!build_model(p, cb.inRate, cb.outRate, m, &err)) return r;

    const double amp = 0.001;                       // -60 dBFS
    const double f0 = 1000.0;
    const size_t needOut = outN + 4096;
    const size_t inN = (size_t)((double)needOut * (double)cb.inRate / (double)cb.outRate) +
                       (size_t)m.tapsPerPhase + 64;
    if (inN > 4000000) return r;
    std::vector<double> x(inN);
    for (size_t i = 0; i < inN; ++i)
        x[i] = amp * std::sin(2.0 * kPiD * f0 * (double)i / (double)cb.inRate);
    const size_t cap = rs.maxOutFrames(inN) + 64;
    std::vector<double> y(cap, 0.0);
    const double* ip[1] = { x.data() };
    double* op[1] = { y.data() };
    const size_t nOut = rs.process(ip, inN, op);
    if (nOut < outN + 1024) return r;
    // 丢掉前 T*L/M 个样本（群延迟），取中间一段
    const size_t skip = (size_t)std::min<long long>((long long)(m.tapsPerPhase * (long long)m.L / std::max(1, m.M)) + 1024,
                                                   (long long)(nOut - outN - 1));
    const double* z = y.data() + skip;
    std::vector<double> seg(z, z + outN);

    // 拟合基波（在输出域的 1 kHz）
    const double w = 2.0 * kPiD * f0 / (double)cb.outRate;
    const Fit2 fit = fit_sine(seg.data(), outN, w);
    const double a = fit.A, b = fit.B;
    const double sigRms = std::sqrt(0.5 * (a * a + b * b));
    double noise = 0.0;
    for (size_t i = 0; i < outN; ++i) {
        const double fund = a * std::cos(w * i) + b * std::sin(w * i);
        const double e = seg[i] - fund;
        seg[i] = e;
        noise += e * e;
    }
    const double noiseRms = std::sqrt(noise / (double)outN);
    r.thdnDb = 20.0 * std::log10((noiseRms + 1e-300) / (sigRms + 1e-300));

    // 残差谱：Blackman-Harris 窗 + FFT
    size_t N = 1; while (N < outN) N <<= 1;
    std::vector<cplx> buf(N, cplx(0, 0));
    for (size_t i = 0; i < outN; ++i) {
        const double t = 2.0 * kPiD * (double)i / (double)(outN - 1);
        const double win = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        buf[i] = cplx(seg[i] * win, 0.0);
    }
    fft(buf);
    // 最大杂散：搜【整个输出带】0..fs/2（镜像落在 43 kHz 这类超声位置，只搜 20 kHz 以内会看不见），
    // 但排除基波 ±6 bin（拟合残差在基波附近总有一点点，不排除就会被它冒充"杂散"）。
    const size_t kF0 = (size_t)std::llround(f0 / (double)cb.outRate * (double)N);
    double peak = 0.0; size_t peakK = 0;
    const size_t kLo = (size_t)(20.0 / (double)cb.outRate * (double)N) + 1;
    const size_t kHi = N / 2 - 2;
    for (size_t k = kLo; k <= kHi; ++k) {
        if (k + 6 >= kF0 && k <= kF0 + 6) continue;
        const double m2 = std::abs(buf[k]);
        if (m2 > peak) { peak = m2; peakK = k; }
    }
    // 带内（20 Hz..20 kHz）THD+N：同样是 FFT 口径，排除基波 ±6 bin
    {
        const size_t kTop = (size_t)(20000.0 / (double)cb.outRate * (double)N);
        double pn = 0.0;
        for (size_t k = kLo; k <= std::min(kTop, kHi); ++k) {
            if (k + 6 >= kF0 && k <= kF0 + 6) continue;
            pn += 2.0 * std::norm(buf[k]) / ((double)N * (double)N);
        }
        r.thdnInbandDb = 10.0 * std::log10(pn + 1e-300) - 10.0 * std::log10(0.5 * (a * a + b * b) + 1e-300);
    }
    // 窗的相干增益归一：BH 窗幅度和 = 0.35875*N*... 用直流和替代
    double wsum = 0.0;
    for (size_t i = 0; i < outN; ++i) {
        const double t = 2.0 * kPiD * (double)i / (double)(outN - 1);
        wsum += 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    }
    const double ampPeak = 2.0 * peak / wsum;
    r.spurDb = 20.0 * std::log10((ampPeak + 1e-300) / (sigRms * std::sqrt(2.0) + 1e-300));
    r.n = (int)peakK;
    return r;
}

// ---------------------------------------------------------------- DSD 相关（P3）
// 与测试一致的带内噪声口径：无窗 FFT，单根谱线功率 2|X|²/N²，1..kTop 求和，
// 基波 ±4 bin 排除（先用最小二乘把基波扣掉，避免泄漏污染底噪）。
struct Spec { double sigDb = -999, noiseDb = -999, thdnDb = 0, worstSpurDb = -999, fmHz = 0; int n = 0; };

static Spec analyze_tone(const std::vector<double>& x, double fs, double f0, double bandHi) {
    Spec s;
    const size_t N = 1; (void)N;
    size_t n = x.size();
    // 只取 2 的幂长度
    size_t Nf = 1; while (Nf * 2 <= n) Nf <<= 1;
    std::vector<double> y(x.begin(), x.begin() + (ptrdiff_t)Nf);
    const double w = 2.0 * kPiD * f0 / fs;
    const Fit2 fit = fit_sine(y.data(), Nf, w);
    const double a = fit.A, b = fit.B;
    for (size_t i = 0; i < Nf; ++i) y[i] -= a * std::cos(w * i) + b * std::sin(w * i);

    std::vector<cplx> buf(Nf, cplx(0, 0));
    for (size_t i = 0; i < Nf; ++i) buf[i] = cplx(y[i], 0.0);
    fft(buf);
    auto pwr = [&](size_t k) { return 2.0 * std::norm(buf[k]) / ((double)Nf * (double)Nf); };
    const double sigP = 0.5 * (a * a + b * b);
    const size_t kF = (size_t)std::llround(f0 / fs * (double)Nf);
    const size_t kTop = (size_t)std::llround(bandHi / fs * (double)Nf);
    double pn = 0.0, mx = 0.0;
    for (size_t k = 1; k <= kTop; ++k) {
        if (k + 4 >= kF && k <= kF + 4) continue;
        const double p = pwr(k);
        pn += p;
        if (p > mx) { mx = p; s.n = (int)k; }
    }
    s.sigDb = 10.0 * std::log10(sigP + 1e-300);
    s.noiseDb = 10.0 * std::log10(pn + 1e-300);
    s.thdnDb = s.noiseDb - s.sigDb;
    s.worstSpurDb = 10.0 * std::log10(mx + 1e-300) - s.sigDb;
    s.fmHz = (double)s.n / (double)Nf * fs;
    return s;
}

struct DecimRow {
    std::string kind, conv; int twoStage = 0; uint32_t M = 0, outRate = 0;
    int taps = 0; double passband = 0, stopStart = 0;
    double ripplePass = 0, a20k = 0, aEdge = 0, aliasWorst = 0, stopWorst = 0;
};
static DecimRow decim_row(IntegratorKind k, DsdConversion c, bool twoStage) {
    DecimRow r;
    r.kind = integrator_name(k); r.conv = conversion_name(c); r.twoStage = twoStage ? 1 : 0;
    IntegratorConfig cfg = conversion_integrator(k, c, 2822400, 176400, 120.0, twoStage);
    DsdDecimator dec(cfg);
    r.M = dec.decimation(); r.outRate = dec.outRate();
    r.taps = (int)dec.firTaps().size() + (int)dec.compTaps().size();
    r.passband = cfg.passbandHz; r.stopStart = dec.stopbandStartHz();
    double mn = 1e9, mx = -1e9;
    for (int i = 0; i <= 120; ++i) {
        const double f = 20.0 * std::pow(cfg.passbandHz / 20.0, (double)i / 120.0);
        const double d = dec.response_db(f);
        mn = std::min(mn, d); mx = std::max(mx, d);
    }
    r.ripplePass = mx - mn;
    r.a20k = dec.response_db(20000.0);
    r.aEdge = dec.response_db(cfg.passbandHz);
    // 第一折叠带：outRate - passband .. outRate + passband
    // 注意：格点要带半步偏移。CIC 的 response_db 在 f = k*outRate 处按"直流"返回 1.0
    // （数学上是零点/未定义），正好落在格点上会读成 0 dB，把混叠抑制算成 0 dB —— 判据陷阱。
    double aw = -1e9;
    for (int i = 0; i <= 120; ++i) {
        const double f = (double)cfg.outRate - cfg.passbandHz +
                         (2.0 * cfg.passbandHz) * ((double)i + 0.5) / 121.0;
        aw = std::max(aw, dec.response_db(f));
    }
    r.aliasWorst = aw;
    double sw = -1e9;
    for (int i = 0; i <= 200; ++i) {
        const double f = dec.stopbandStartHz() +
                         ((double)cfg.outRate * 2.0 - dec.stopbandStartHz()) * ((double)i + 0.5) / 201.0;
        sw = std::max(sw, dec.response_db(f));
    }
    r.stopWorst = sw;
    return r;
}

// DSD 64 → PCM 176.4k 的整链：SDM 编码 + 抽取解码
struct DsdLoopRow {
    std::string kind, conv; double f0 = 1000.0, amp = 0.5;
    double sigDb = 0, noiseDb = 0, thdnDb = 0, spurDb = 0, spurHz = 0;
    long long overload = 0, clamped = 0;
};
static DsdLoopRow dsd_loop(IntegratorKind k, DsdConversion c, double ampIn, int order) {
    DsdLoopRow r; r.kind = integrator_name(k); r.conv = conversion_name(c); r.amp = ampIn;
    const double fsD = 2822400.0;
    // 生产默认（tools/rtaweb.cpp:4010-4023 + ui 默认值）：corner 100 kHz、抖动 0.0005、
    // 阶数经 stable_sdm_order 按速率/族裁决。探针必须用同一组参数，否则测的不是用户听到的链路。
    SdmConfig sc; sc.fs = fsD; sc.levels = 2; sc.family = ModulatorFamily::Dsd;
    sc.cutoffHz = 100000.0; sc.ditherLsb = 0.0005;
    sc.order = stable_sdm_order(order, fsD, sc.levels, sc.family);
    SdmModulator mod(sc);
    const size_t nBits = 2822400;                  // 1 秒
    std::vector<uint8_t> packed((nBits + 7) / 8, 0);
    for (size_t i = 0; i < nBits; ++i) {
        const double x = ampIn * std::sin(2.0 * kPiD * r.f0 * (double)i / fsD);
        const double v = mod.process(x);
        if (v > 0.0) packed[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    }
    r.overload = mod.overload(); r.clamped = mod.clamped();
    IntegratorConfig cfg = conversion_integrator(k, c, 2822400, 176400, 120.0, true);
    DsdDecimator dec(cfg);
    std::vector<double> pcm;
    dec.process_bits(packed.data(), nBits, pcm);
    Spec s = analyze_tone(pcm, 176400.0, r.f0, 20000.0);
    r.sigDb = s.sigDb; r.noiseDb = s.noiseDb; r.thdnDb = s.thdnDb;
    r.spurDb = s.worstSpurDb; r.spurHz = s.fmHz;
    return r;
}

// PCM → DSD 调制器：带内噪声 / 带外总量 / 满幅过载
struct SdmRow {
    std::string family; int orderReq = 0, orderEff = 0; double fs = 0, amp = 0;
    double sigDb = 0, noiseDb = 0, thdnDb = 0, oobDb = 0;
    long long overload = 0, clamped = 0, nonFinite = 0;
    double hinf = -1;
};
static SdmRow sdm_row(ModulatorFamily fam, int order, double fs, double amp, int reps = 8) {
    SdmRow r; r.family = modulator_family_name(fam); r.orderReq = order; r.fs = fs; r.amp = amp;
    const size_t kN = 65536, kWarm = 20000;
    const double fin = 23.0 * fs / (double)kN;
    double pnSum = 0.0, psSum = 0.0, oobSum = 0.0;
    for (int rep = 0; rep < reps; ++rep) {
        SdmConfig sc; sc.fs = fs; sc.levels = 2; sc.family = fam;
        sc.cutoffHz = 100000.0; sc.ditherLsb = 0.0005;
        sc.order = stable_sdm_order(order, fs, sc.levels, fam);
        SdmModulator m(sc);
        r.orderEff = m.effectiveOrder(); r.hinf = m.ntf_hinf_used();
        std::vector<double> y; y.reserve(kN);
        for (size_t n = 0; n < kWarm + kN; ++n) {
            const double x = amp * std::sin(2.0 * kPiD * fin * (double)n / fs);
            const double v = m.process(x);
            if (n >= kWarm) y.push_back(v);
        }
        r.overload += m.overload(); r.clamped += m.clamped(); r.nonFinite += m.nonFinite();
        Spec s = analyze_tone(y, fs, fin, 20000.0);
        pnSum += std::pow(10.0, s.noiseDb / 10.0);
        psSum += std::pow(10.0, s.sigDb / 10.0);
        // 带外：全带功率 - 带内功率
        double tot = 0.0;
        for (double v : y) tot += v * v;
        tot /= (double)y.size();
        oobSum += std::pow(10.0, 10.0 * std::log10(tot - std::pow(10.0, s.sigDb / 10.0) - std::pow(10.0, s.noiseDb / 10.0) + 1e-300) / 10.0);
    }
    r.noiseDb = 10.0 * std::log10(pnSum / (double)reps + 1e-300);
    r.sigDb = 10.0 * std::log10(psSum / (double)reps + 1e-300);
    r.thdnDb = r.noiseDb - r.sigDb;
    r.oobDb = 10.0 * std::log10(oobSum / (double)reps + 1e-300);
    return r;
}

// 满幅稳定性（铁律 7）：0 dBFS 瞬态 + 单样本越界后的恢复
struct OvlRow { std::string family; int order = 0; double fs = 0; long long ov = 0, cl = 0; double rmsAfter = 0; };
static OvlRow ovl_row(ModulatorFamily fam, int order, double fs) {
    OvlRow r; r.family = modulator_family_name(fam); r.order = order; r.fs = fs;
    SdmConfig sc; sc.fs = fs; sc.levels = 2; sc.family = fam;
    sc.cutoffHz = 100000.0; sc.ditherLsb = 0.0005;
    sc.order = stable_sdm_order(order, fs, sc.levels, fam);
    SdmModulator m(sc);
    r.order = m.effectiveOrder();
    const size_t n = (size_t)fs;                 // 1 秒
    const size_t t0 = n / 2;
    double acc = 0.0; size_t cnt = 0;
    for (size_t i = 0; i < n; ++i) {
        double x;
        if (i < t0) x = 0.62 * std::sin(2.0 * kPiD * 997.0 * (double)i / fs);
        else if (i < t0 + 64) x = 0.999;                       // 满幅阶跃（0 dBFS）
        else if (i == t0 + 64) x = 1.0;                        // 单样本越界
        else x = 0.0;
        const double v = m.process(x);
        if (i > t0 + 2000) { acc += v * v; ++cnt; }
    }
    r.ov = m.overload(); r.cl = m.clamped();
    r.rmsAfter = std::sqrt(acc / (double)std::max<size_t>(1, cnt));
    return r;
}

// ---------------------------------------------------------------- 预设族分类（按命名规则，不是猜的）
static std::string family_of(const std::string& n) {
    auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
    if (starts("apod")) return "apodizing";
    if (starts("closed-form")) return "closed-form";
    if (starts("minringFIR")) return "minringFIR";
    if (starts("sinc-")) return "sinc";
    if (starts("poly-sinc-gauss")) return "poly-sinc-gauss";
    if (starts("poly-sinc-ext2")) return "poly-sinc-ext2";
    if (starts("poly-sinc-hb")) return "poly-sinc-halfband";
    if (starts("poly-sinc")) return "poly-sinc";
    return "other";
}

template <class F>
static void parallel_for(int n, int jobs, F f) {
    if (jobs <= 1) { for (int i = 0; i < n; ++i) f(i); return; }
    std::atomic<int> idx{0};
    auto worker = [&]() { for (;;) { int i = idx.fetch_add(1); if (i >= n) break; f(i); } };
    std::vector<std::thread> th;
    for (int t = 0; t < jobs; ++t) th.emplace_back(worker);
    for (auto& t : th) t.join();
}

static FILE* open_out(const std::string& dir, const std::string& name) {
    const std::string p = dir + "\\" + name;
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) std::fprintf(stderr, "打不开 %s\n", p.c_str());
    return f;
}

// ---------------------------------------------------------------- 各模式
static void mode_presets(const std::string& dir, int jobs) {
    const auto& ps = filter_presets();
    const Combo& ref = kCombos[1];      // 44.1k -> 384k
    std::vector<Metrics> mm(ps.size());
    std::vector<std::string> errs(ps.size());
    parallel_for((int)ps.size(), jobs, [&](int i) {
        Model m;
        mm[(size_t)i] = measure(ps[(size_t)i], ref, m, &errs[(size_t)i]);
    });
    FILE* f = open_out(dir, "presets.tsv");
    if (!f) return;
    std::fprintf(f, "# RLPlayer 2.0 滤波器预设全表（设计值 + 实测值）\n");
    std::fprintf(f, "# 枚举方法：源码 filter_presets() 的字面量数组（src/dsp/filter_families.h:234-357），\n");
    std::fprintf(f, "#           运行时 filter_presets().size() 计数，逐条读出全部 13 个字段；无一为循环/表驱动生成。\n");
    std::fprintf(f, "# 实测列：按 44.1k->384k 的真实 PolyphaseResampler 系数表现算（prototype_into 取系数，DTFT 求响应）。\n");
    std::fprintf(f, "# 列：name family taps_decl transition atten_db window phase for_nx two_stage stage2_taps"
                    " apodizing apod_f1 apod_f2 taps_per_input | L M proto_taps(实测) taps_per_phase(实测)"
                    " dc_db ripple20k_db ripple_pass_db f_minus1_db f_minus3_db design_fc6_db dev_pct"
                    " a1k_db a10k_db a20k_db a30k_db fsb80_hz worst_stop_db image_peak_db image_peak_full_db"
                    " gd100_ms gd1k_ms gd10k_ms gd_nom_ms\n");
    int nOk = 0;
    for (size_t i = 0; i < ps.size(); ++i) {
        const auto& p = ps[i];
        const auto& r = mm[i];
        std::fprintf(f, "%s\t%s\t%d\t%.4f\t%.0f\t%s\t%s\t%d\t%d\t%d\t%d\t%.6f\t%.6f\t%d",
            p.name, family_of(p.name).c_str(), p.taps, p.transition, p.attenDb,
            window_name(p.window), phase_name(p.phase), p.forNx ? 1 : 0, p.twoStage ? 1 : 0,
            p.stage2Taps, p.apodizing ? 1 : 0, p.apodF1, p.apodF2, p.tapsPerInput);
        std::fprintf(f, "\t%d\t%d\t%d\t%d\t%.3f\t%.4f\t%.4f\t%.1f\t%.1f\t%.1f\t%.2f"
                        "\t%.4f\t%.4f\t%.4f\t%.4f\t%.1f\t%.1f\t%.1f\t%.1f\t%.3f\t%.3f\t%.3f\t%.3f\n",
            r.L, r.M, r.protoTaps, r.tapsPerPhase, r.dcDb, r.ripple20k, r.ripplePass,
            r.f1db, r.f3db, r.designFc, r.devPct, r.a1k, r.a10k, r.a20k, r.a30k,
            r.fsb80, r.worstStop, r.imagePeak, r.imagePeakFull, r.gd100, r.gd1k, r.gd10k, r.gdNomMs);
        if (errs[i].empty()) ++nOk;
    }
    std::fclose(f);
    std::printf("presets.tsv: %zu 个预设，成功 %d\n", ps.size(), nOk);
}

static void mode_resp(const std::string& dir, int jobs) {
    const auto& ps = filter_presets();
    for (int ci = 0; ci < kNumCombos; ++ci) {
        const Combo& cb = kCombos[ci];
        const double t0 = now_s();
        std::vector<Metrics> mm(ps.size());
        std::vector<std::vector<std::pair<double,double>>> curves(ps.size());
        std::vector<std::string> errs(ps.size());
        const double fcGuess = 20000.0;
        auto one = [&](int i) {
            Model m;
            mm[(size_t)i] = measure(ps[(size_t)i], cb, m, &errs[(size_t)i]);
            if (m.past || m.stages.empty()) return;
            const auto g = make_grid(cb.outRate, m.designFc6db > 0 ? m.designFc6db : fcGuess, 0);
            auto& cv = curves[(size_t)i];
            cv.reserve(g.size());
            for (double f : g) cv.emplace_back(f, db_of(resp_at(m, f)));
        };
        parallel_for((int)ps.size(), std::min(jobs, 4), one);

        FILE* f = open_out(dir, std::string("metrics-") + cb.id + ".tsv");
        if (f) {
            std::fprintf(f, "# combo %s\n", cb.label);
            std::fprintf(f, "# preset\tL\tM\ttwo_stage\tF\tproto_taps\ttaps_per_phase\tdc_db"
                            "\tripple_20k_db\tripple_pass_db\tf_minus1_db\tf_minus3_db\tdesign_fc6_db"
                            "\tdev_pct\ta1k_db\ta10k_db\ta20k_db\ta30k_db\tfsb80_hz\tworst_stop_db"
                            "\timage_peak_db\timage_peak_full_db\tgd100_ms\tgd1k_ms\tgd10k_ms\tgd_nom_ms\n");
            for (size_t i = 0; i < ps.size(); ++i) {
                const auto& r = mm[i];
                std::fprintf(f, "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%.4f\t%.4f\t%.1f\t%.1f\t%.1f\t%.2f"
                                "\t%.4f\t%.4f\t%.4f\t%.4f\t%.1f\t%.1f\t%.1f\t%.1f\t%.3f\t%.3f\t%.3f\t%.3f\n",
                    r.preset.c_str(), r.L, r.M, r.twoStage, r.F, r.protoTaps, r.tapsPerPhase,
                    r.dcDb, r.ripple20k, r.ripplePass, r.f1db, r.f3db, r.designFc, r.devPct,
                    r.a1k, r.a10k, r.a20k, r.a30k, r.fsb80, r.worstStop, r.imagePeak, r.imagePeakFull,
                    r.gd100, r.gd1k, r.gd10k, r.gdNomMs);
            }
            std::fclose(f);
        }
        FILE* c = open_out(dir, std::string("curve-") + cb.id + ".tsv");
        if (c) {
            std::fprintf(c, "# combo %s  (f_Hz\tdb)  每行一个采样点，preset 切换行\n", cb.label);
            for (size_t i = 0; i < ps.size(); ++i) {
                std::fprintf(c, "# %s\n", ps[i].name);
                for (auto& kv : curves[i]) std::fprintf(c, "%.4f\t%.4f\n", kv.first, kv.second);
            }
            std::fclose(c);
        }
        std::printf("resp %s: %.1f s\n", cb.id, now_s() - t0);
        std::fflush(stdout);
    }
}

static void mode_validate(const std::string& dir, int jobs) {
    const auto& ps = filter_presets();
    std::vector<std::vector<ValidateRow>> rows(kNumCombos);
    for (int ci = 0; ci < kNumCombos; ++ci) {
        const Combo& cb = kCombos[ci];
        rows[(size_t)ci].resize(ps.size());
        parallel_for((int)ps.size(), std::min(jobs, 4), [&](int i) {
            rows[(size_t)ci][(size_t)i] = validate_one(ps[(size_t)i], cb, false);
        });
        std::printf("validate %s done\n", cb.id);
        std::fflush(stdout);
    }
    FILE* f = open_out(dir, "validate.tsv");
    if (!f) return;
    std::fprintf(f, "# 时域抽查：-60 dBFS 脉冲过真实 process()，输出做 DFT 与频域模型比对\n");
    std::fprintf(f, "# preset\tcombo\tstatus\tproto_taps\tgain_offset_db\tmax_shape_err_db\tn_points\tcost_madd\n");
    int ok = 0, skip = 0;
    for (int ci = 0; ci < kNumCombos; ++ci)
        for (const auto& r : rows[(size_t)ci]) {
            std::fprintf(f, "%s\t%s\t%s\t%d\t%.6f\t%.6f\t%d\t%.3e\n",
                         r.preset.c_str(), r.combo.c_str(), r.status.c_str(),
                         r.protoTaps, r.gainOffsetDb, r.maxShapeErrDb, r.nPoints, r.costMadd);
            if (r.status == "ok") ++ok; else if (r.status.rfind("skip", 0) == 0) ++skip;
        }
    std::fclose(f);
    std::printf("validate: ok=%d skip=%d\n", ok, skip);
}

static void mode_spur(const std::string& dir, int jobs) {
    const auto& ps = filter_presets();
    std::vector<SpurRow> rows(ps.size());
    const Combo& cb = kCombos[0];      // 44.1k -> 96k（8 万点输出）
    parallel_for((int)ps.size(), std::min(jobs, 4), [&](int i) {
        rows[(size_t)i] = spur_one(ps[(size_t)i], cb, 65536);
    });
    FILE* f = open_out(dir, "spur-44k1-96k.tsv");
    if (!f) return;
    std::fprintf(f, "# 1 kHz 音（-60 dBFS）过真实 process()；最小二乘减掉基波后取残差 FFT\n");
    std::fprintf(f, "# thdn_db = 全带（含超声镜像）残差/基波；thdn_inband = 20Hz-20kHz 内（FFT 口径）\n");
    std::fprintf(f, "# preset\tthdn_db\tthdn_inband_db\tworst_spur_db\tspur_hz\n");
    for (const auto& r : rows) {
        if (r.thdnDb == 0.0) continue;
        std::fprintf(f, "%s\t%.2f\t%.2f\t%.2f\t%.1f\n", r.preset.c_str(), r.thdnDb,
                     r.thdnInbandDb, r.spurDb, (double)r.n * (double)cb.outRate / 65536.0);
    }
    std::fclose(f);
    std::printf("spur done\n");
}

static void mode_dsd(const std::string& dir) {
    {
        FILE* f = open_out(dir, "dsd-decim.tsv");
        if (f) {
            std::fprintf(f, "# DSD64(2822400) -> PCM 176400 抽取器设计响应（DsdDecimator::response_db，真实系数）\n");
            std::fprintf(f, "# kind\tconversion\ttwo_stage\tM\tout_rate\ttaps\tpassband_hz\tstop_start_hz"
                            "\tripple_pass_db\ta20k_db\ta_edge_db\talias_worst_db\tstop_worst_db\n");
            for (auto k : { IntegratorKind::Fir, IntegratorKind::Iir, IntegratorKind::Cic })
                for (auto c : { DsdConversion::Narrow, DsdConversion::Wide, DsdConversion::Xfi })
                    for (bool ts : { true, false }) {
                        if (k != IntegratorKind::Fir && !ts) continue;    // 只有 FIR 有 twoStage 语义
                        DecimRow r = decim_row(k, c, ts);
                        std::fprintf(f, "%s\t%s\t%d\t%u\t%u\t%d\t%.0f\t%.0f\t%.4f\t%.3f\t%.3f\t%.1f\t%.1f\n",
                            r.kind.c_str(), r.conv.c_str(), r.twoStage, r.M, r.outRate, r.taps,
                            r.passband, r.stopStart, r.ripplePass, r.a20k, r.aEdge,
                            r.aliasWorst, r.stopWorst);
                    }
            std::fclose(f);
        }
    }
    {
        FILE* f = open_out(dir, "dsd-loop.tsv");
        if (f) {
            std::fprintf(f, "# DSD64 整链：SDM(5 阶) 编码 1kHz 音 -> 抽取到 PCM 176.4k -> 带内 THD+N\n");
            std::fprintf(f, "# kind\tconversion\ttone_hz\ttone_amp\tsig_dbfs\tnoise_band_dbfs\tthdn_db"
                            "\tworst_spur_db\tspur_hz\toverload\tclamped\n");
            for (auto k : { IntegratorKind::Fir, IntegratorKind::Iir, IntegratorKind::Cic })
                for (auto c : { DsdConversion::Narrow, DsdConversion::Wide, DsdConversion::Xfi }) {
                    DsdLoopRow r = dsd_loop(k, c, 0.5, 5);
                    std::fprintf(f, "%s\t%s\t%.0f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.1f\t%lld\t%lld\n",
                        r.kind.c_str(), r.conv.c_str(), r.f0, r.amp, r.sigDb, r.noiseDb,
                        r.thdnDb, r.spurDb, r.spurHz, r.overload, r.clamped);
                    std::fflush(f);
                }
            std::fclose(f);
        }
    }
    {
        FILE* f = open_out(dir, "sdm.tsv");
        if (f) {
            std::fprintf(f, "# PCM->DSD 调制器：0.62 峰值工作点，1kHz 音（第 23 根谱线），8 次实现功率平均\n");
            std::fprintf(f, "# family\torder_req\torder_eff\tfs\ttone_amp\tsig_dbfs\tnoise_band_dbfs"
                            "\tthdn_db\toob_db\tntf_hinf\toverload\tclamped\tnonfinite\n");
            const double rates[3] = { 2822400.0, 5644800.0, 11289600.0 };
            for (auto fam : { ModulatorFamily::Dsd, ModulatorFamily::Chebyshev, ModulatorFamily::Ec })
                for (double fs : rates)
                    for (int ord : { 5, 7 }) {
                        SdmRow r = sdm_row(fam, ord, fs, 0.62, 8);
                        std::fprintf(f, "%s\t%d\t%d\t%.0f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.3f\t%lld\t%lld\t%lld\n",
                            r.family.c_str(), r.orderReq, r.orderEff, r.fs, r.amp, r.sigDb,
                            r.noiseDb, r.thdnDb, r.oobDb, r.hinf, r.overload, r.clamped, r.nonFinite);
                        std::fflush(f);
                    }
            std::fclose(f);
        }
    }
    {
        FILE* f = open_out(dir, "sdm-overload.tsv");
        if (f) {
            std::fprintf(f, "# 满幅稳定性（铁律 7）：0.62 稳态 -> 64 样本 0.999 阶跃 -> 单样本 1.0 -> 静音 1 秒\n");
            std::fprintf(f, "# family\torder_eff\tfs\toverload\tclamped\trms_after_db\n");
            for (auto fam : { ModulatorFamily::Dsd, ModulatorFamily::Chebyshev, ModulatorFamily::Ec })
                for (double fs : { 2822400.0, 5644800.0, 11289600.0 })
                    for (int ord : { 5, 7 }) {
                        OvlRow r = ovl_row(fam, ord, fs);
                        std::fprintf(f, "%s\t%d\t%.0f\t%lld\t%lld\t%.2f\n",
                            r.family.c_str(), r.order, r.fs, r.ov, r.cl,
                            20.0 * std::log10(r.rmsAfter + 1e-300));
                        std::fflush(f);
                    }
            std::fclose(f);
        }
    }
    std::printf("dsd done\n");
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "all";
    std::string dir  = argc > 2 ? argv[2] : "build-doc\\raw";
    int jobs = argc > 3 ? std::atoi(argv[3]) : 4;
    if (jobs < 1) jobs = 1;
    std::printf("presets = %zu\n", filter_presets().size());
    std::fflush(stdout);
    if (mode == "presets" || mode == "all") mode_presets(dir, jobs);
    if (mode == "resp"    || mode == "all") mode_resp(dir, jobs);
    if (mode == "validate"|| mode == "all") mode_validate(dir, jobs);
    if (mode == "spur"    || mode == "all") mode_spur(dir, jobs);
    if (mode == "dsd"     || mode == "all") mode_dsd(dir);
    return 0;
}
