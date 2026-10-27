// test_quality_confidence.cpp — 音质 19 项指标的【置信度】专项（DEBUG）
//
// 与 tests/test_quality.cpp 的分工（铁律 3）：
//   test_quality.cpp     ：给定已知答案，证明「测得对」（合成信号 → 理论值 vs 实测）。
//   本文件               ：证明「什么时候会错」 —— 重复性 / 有效动态范围 / 误报边界 / 说谎场景。
//
// 这里输出的不是「通过/失败」，而是**可抄进报告的表格**：
//   [CONF]  该指标的重复性 σ（同一信号重复分析）
//   [RANGE] 该指标从哪个电平/时长/采样率开始失效
//   [FP]    误报边界（什么正常素材会被判成问题）
//   [LIE]   会说谎的场景（指标给了误导性结论）
//   [BUG]   真 bug（复现步骤在源码注释里，当前行为作为回归基线）
//
// 判据纪律：check() 只断言「确实成立的事实」（含「已知缺陷的现状」，标注 BASELINE），
//          真正的不变量（重复性 / 分块一致 / 削波下限 / 单声道损失）必须 PASS。
// 全部现场合成、纯离线（不开任何音频设备）；真实文件交叉验证见 tools/quality_probe.cpp。
#include "analysis/quality.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using namespace rta::analysis;
using rta::dsp::kPi;

static int fails = 0;
static int baselineNotes = 0;
static int bugs = 0;

static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static void baseline(const std::string& what, const std::string& detail = "") {
    std::printf("  [BASE] %s %s\n", what.c_str(), detail.c_str());
    ++baselineNotes;
}
static void bug(const std::string& what, const std::string& detail = "") {
    std::printf("  [BUG ] %s %s\n", what.c_str(), detail.c_str());
    ++bugs;
}
static std::string f2(const char* f, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), f, a, b, c, d);
    return buf;
}

// ---------------- 素材工具 ----------------
struct Rng {
    unsigned long long s;
    explicit Rng(unsigned long long seed) : s(seed ? seed : 88172645463325252ull) {}
    double uni() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0;
    }
    double gauss() {
        const double u1 = std::max(uni(), 1e-12), u2 = uni();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    }
};

static std::vector<double> sine(size_t n, double fs, double hz, double amp, double phase = 0.0) {
    std::vector<double> v(n, 0.0);
    for (size_t i = 0; i < n; ++i) v[i] = amp * std::sin(2.0 * kPi * hz * (double)i / fs + phase);
    return v;
}
static void add_noise(std::vector<double>& v, double rms, unsigned long long seed) {
    Rng r(seed);
    for (auto& x : v) x += rms * r.gauss();
}
// 量化到 bits（与 WAV 16/24 bit 解码同标度：正满幅 = 1 − 2^-(bits−1)）
static void quantize(std::vector<double>& v, int bits) {
    const double q = std::pow(2.0, (double)(bits - 1));
    for (double& x : v) {
        double s = std::floor(x * q + 0.5);
        if (s > q - 1.0) s = q - 1.0;
        if (s < -q) s = -q;
        x = s / q;
    }
}
// 量化噪声理论 rms（相对满幅 1.0）
static double quant_rms(int bits) { return 1.0 / (std::pow(2.0, (double)(bits - 1)) * std::sqrt(12.0)); }

static QualityResult q1(const std::vector<double>& x, double fs) {
    const double* p[1] = { x.data() };
    return measure_quality(p, 1, x.size(), fs);
}
static QualityResult q2(const std::vector<double>& l, const std::vector<double>& r, double fs) {
    const double* p[2] = { l.data(), r.data() };
    return measure_quality(p, 2, l.size(), fs);
}
// 从长信号里取一段（模拟「同一首歌不同片段」）
static std::vector<double> slice(const std::vector<double>& v, size_t start, size_t n) {
    if (start >= v.size()) return std::vector<double>(n, 0.0);
    n = std::min(n, v.size() - start);
    return std::vector<double>(v.begin() + (long long)start, v.begin() + (long long)(start + n));
}
// 分块喂（生产路径），最大块 frames
static QualityResult q_chunked(const std::vector<double>& x, double fs, size_t frames) {
    QualityAccumulator acc;
    acc.begin(x.size(), 1, fs);
    size_t pos = 0;
    while (pos < x.size()) {
        const size_t n = std::min(frames, x.size() - pos);
        acc.add_interleaved(x.data() + pos, n);
        pos += n;
    }
    return acc.finalize();
}

// 21 个数值（19 项里「削波 5 项」展开了 5 个字段、动态细化 4 项展开 4 个）
enum MK {
    K_PEAK, K_RMS, K_CREST, K_CLIP_PCT, K_CLIP_RUNS, K_CLIP_LONG, K_THDN, K_NONTONAL,
    K_NOISE, K_SNR, K_CENTROID, K_TILT, K_ROLLOFF, K_HF10K, K_CORR, K_MONOLOSS,
    K_SIDEMID, K_DYN_P95P10, K_DYN_CENTROID, K_QUIET_P10, K_SILENCE, K_COUNT
};
static const char* kName[K_COUNT] = {
    "峰值dBFS", "RMS dBFS", "峰值因子dB", "削波占比%", "削波段数", "最长削波段", "THD+N dB",
    "非谐波残留dB", "本底噪声dBFS", "SNR dB", "谱重心Hz", "谱倾斜dB/oct", "95%滚降Hz",
    ">10k占比dB", "声道相关", "单声道损失dB", "side/mid dB", "动态P95-P10dB",
    "动态重心dBFS", "100msP10dBFS", "静音占比%"
};
static const bool kIntMetric[K_COUNT] = {
    false, false, false, false, true, true, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false
};
static double mv(const QualityResult& r, int k) {
    switch (k) {
        case K_PEAK: return r.peakDbfs;
        case K_RMS: return r.rmsDbfs;
        case K_CREST: return r.crestDb;
        case K_CLIP_PCT: return r.clippedPct;
        case K_CLIP_RUNS: return (double)r.clippedRuns;
        case K_CLIP_LONG: return (double)r.longestClipRun;
        case K_THDN: return r.thdnAvailable ? r.thdnDb : 0.0;
        case K_NONTONAL: return r.nonTonalDb;
        case K_NOISE: return r.noiseFloorDbfs;
        case K_SNR: return r.snrDb;
        case K_CENTROID: return r.centroidHz;
        case K_TILT: return r.tiltDbPerOct;
        case K_ROLLOFF: return r.rolloff95Hz;
        case K_HF10K: return r.hfRatio10kDb;
        case K_CORR: return r.channelCorr;
        case K_MONOLOSS: return r.monoLossDb;
        case K_SIDEMID: return r.sideMidDb;
        case K_DYN_P95P10: return r.dynP95P10Db;
        case K_DYN_CENTROID: return r.dynCentroidDbfs;
        case K_QUIET_P10: return r.quietP10Dbfs;
        case K_SILENCE: return r.silencePct;
        default: return 0.0;
    }
}

// 「音乐型」立体声素材：低音 + 和弦 + 噪声背景 + 每 0.5 s 的瞬态
static void make_music(std::vector<double>& l, std::vector<double>& r, double fs, double sec,
                       double peakScale, bool limited) {
    const size_t n = (size_t)(sec * fs);
    l.assign(n, 0.0);
    r.assign(n, 0.0);
    Rng ng(20261027);
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / fs;
        const double env = 0.55 + 0.35 * std::sin(2.0 * kPi * 0.37 * t);
        double s = 0.28 * std::sin(2.0 * kPi * 55.0 * t)
                 + 0.18 * std::sin(2.0 * kPi * 220.0 * t + 0.3)
                 + 0.14 * std::sin(2.0 * kPi * 277.18 * t + 1.1)
                 + 0.10 * std::sin(2.0 * kPi * 329.63 * t + 2.0)
                 + 0.07 * std::sin(2.0 * kPi * 1760.0 * t + 0.7);
        s *= env;
        // 瞬态：每 0.5 s 一个噪声簇（衰减 40 ms）
        const double ph = std::fmod(t, 0.5);
        if (ph < 0.04) s += 0.35 * std::exp(-ph / 0.012) * ng.gauss();
        l[i] = s + 0.0003 * ng.gauss();          // −70 dBFS 噪声底
        r[i] = s * 0.82 + 0.05 * std::sin(2.0 * kPi * 440.0 * t) + 0.0003 * ng.gauss();
    }
    if (limited) {                               // 模拟「响度战争」母带：硬限幅在 ±peakScale
        for (size_t i = 0; i < n; ++i) {
            l[i] = std::max(-peakScale, std::min(peakScale, l[i]));
            r[i] = std::max(-peakScale, std::min(peakScale, r[i]));
        }
    } else {
        double pk = 0.0;
        for (size_t i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        const double g = (pk > 0.0) ? peakScale / pk : 0.0;
        for (size_t i = 0; i < n; ++i) { l[i] *= g; r[i] *= g; }
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const double fs = 48000.0;

    // ================================================================ A. 重复性
    std::printf("=== A. 重复性：同一信号重复分析 N 次的结果抖动 ===\n");
    {
        std::vector<double> l, r;
        make_music(l, r, fs, 20.0, 0.9, false);
        const QualityResult base = q2(l, r, fs);
        double worst = 0.0;
        for (int rep = 0; rep < 5; ++rep) {
            const QualityResult x = q2(l, r, fs);
            for (int k = 0; k < K_COUNT; ++k) worst = std::max(worst, std::fabs(mv(x, k) - mv(base, k)));
        }
        std::printf("  [CONF] 同进程重复 5 次：max|Δ| = %.17g（21 个数值字段）\n", worst);
        check(worst == 0.0, "重复分析逐位一致（同一进程内，σ 恒等于 0）", f2("max|Δ|=%.17g", worst));
    }
    {
        // 跨「解码块大小」的一致性：生产链路按 65536 帧分块，测试按 4096/262144
        std::vector<double> l, r;
        make_music(l, r, fs, 20.0, 0.9, false);
        const QualityResult whole = q2(l, r, fs);
        double worst = 0.0;
        for (size_t bs : { size_t(4096), size_t(65536), size_t(262144) }) {
            // 交错后按块喂
            std::vector<double> il(l.size() * 2);
            for (size_t i = 0; i < l.size(); ++i) { il[i * 2] = l[i]; il[i * 2 + 1] = r[i]; }
            QualityAccumulator acc;
            acc.begin(l.size(), 2, fs);
            size_t pos = 0;
            while (pos < l.size()) {
                const size_t n = std::min(bs, l.size() - pos);
                acc.add_interleaved(il.data() + pos * 2, n);
                pos += n;
            }
            const QualityResult c = acc.finalize();
            for (int k = 0; k < K_COUNT; ++k) worst = std::max(worst, std::fabs(mv(c, k) - mv(whole, k)));
        }
        std::printf("  [CONF] 块大小 4096/65536/262144 vs 整段：max|Δ| = %.17g\n", worst);
        check(worst == 0.0, "21 个字段对块大小不变（流式 == 整段，逐位）", f2("max|Δ|=%.17g", worst));
    }
    {
        // 谱估计的固有抖动：平稳素材（粉噪）改成起点/长度不同，看同一统计量的漂移
        const size_t n = (size_t)(60.0 * fs);
        std::vector<double> pink(n, 0.0);
        {
            Rng rng(4242);
            // 一阶低通白噪 → 近似粉噪（谱形固定，与起点无关）
            double lp = 0.0;
            for (size_t i = 0; i < n; ++i) { lp = 0.995 * lp + 0.05 * rng.gauss(); pink[i] = lp; }
            double s = 0.0;
            for (double v : pink) s += v * v;
            const double g = 0.1 / std::sqrt(s / (double)n);
            for (double& v : pink) v *= g;
        }
        const QualityResult ref = q1(pink, fs);
        double jit[K_COUNT] = { 0 };
        for (int rep = 0; rep < 6; ++rep) {
            const std::vector<double> seg = slice(pink, (size_t)(rep * 0.37 * fs), (size_t)(20.0 * fs));
            const QualityResult x = q1(seg, fs);
            for (int k = 0; k < K_COUNT; ++k) {
                if (kIntMetric[k]) continue;
                const double a = mv(ref, k), b = mv(x, k);
                const double d = std::fabs(a - b) / std::max(1e-12, std::fabs(a));
                jit[k] = std::max(jit[k], d);
            }
        }
        std::printf("  [CONF] 平稳素材（20 s 段 × 6 个起点，相对参考 60 s 全曲）最大相对偏差：\n");
        for (int k = 0; k < K_COUNT; ++k) {
            if (kIntMetric[k]) continue;
            std::printf("         %-14s %6.2f%%\n", kName[k], 100.0 * jit[k]);
        }
        check(jit[K_CENTROID] < 0.15, "平稳素材的谱重心对「片段起点」不敏感（<15%）",
              f2("实测最大偏差 %.2f%%（这就是谱重心的固有抖动）", 100.0 * jit[K_CENTROID]));
        check(jit[K_TILT] < 0.10, "平稳素材的谱倾斜对片段起点不敏感（<10%）",
              f2("实测最大偏差 %.2f%%", 100.0 * jit[K_TILT]));
    }
    {
        // 真实音乐（非平稳）：同一首歌不同片段的指标漂移 —— 这是「用户看到的抖动」的真正来源
        std::vector<double> l, r;
        make_music(l, r, fs, 120.0, 0.9, false);
        const QualityResult whole = q2(l, r, fs);
        double mn[K_COUNT], mx[K_COUNT];
        for (int k = 0; k < K_COUNT; ++k) { mn[k] = 1e300; mx[k] = -1e300; }
        for (int rep = 0; rep < 5; ++rep) {
            const size_t off = (size_t)(rep * 20.0 * fs);
            const QualityResult x = q2(slice(l, off, (size_t)(20.0 * fs)), slice(r, off, (size_t)(20.0 * fs)), fs);
            for (int k = 0; k < K_COUNT; ++k) {
                mn[k] = std::min(mn[k], mv(x, k));
                mx[k] = std::max(mx[k], mv(x, k));
            }
        }
        std::printf("  [CONF] 非平稳素材（5 段 × 20 s，同一素材不同段落）极差 max−min：\n");
        for (int k = 0; k < K_COUNT; ++k)
            std::printf("         %-14s %10.3f   全曲值 %.3f\n", kName[k], mx[k] - mn[k], mv(whole, k));
        const double spread = (mx[K_CENTROID] - mn[K_CENTROID]) / std::max(1.0, mv(whole, K_CENTROID));
        std::printf("  [CONF] 谱重心在同一素材的不同段落之间可差 %.2f 倍（%.0f Hz 极差）；"
                    ">10k 占比极差 %.2f dB\n",
                    spread, mx[K_CENTROID] - mn[K_CENTROID], mx[K_HF10K] - mx[K_HF10K] + (mx[K_HF10K] - mn[K_HF10K]));
        check(spread < 5.0, "同一素材不同段落的谱重心极差 < 5 倍（超过这个量级说明素材本来就不均匀）",
              f2("极差/全曲 = %.2f 倍", spread));
    }

    // ================================================================ B. 有效动态范围
    std::printf("\n=== B. 有效动态范围：从哪个电平开始失效 ===\n");
    {
        std::printf("  [RANGE] 1 kHz 正弦 + 16 bit 量化（5 s）：THD+N 只能测到量化噪声地板\n");
        std::printf("          %-10s %-12s %-14s %-12s\n", "峰值dBFS", "实测THD+N", "量化地板理论", "差值");
        const double levels[] = { 0.0, -6.0, -20.0, -40.0, -60.0, -80.0, -90.0 };
        double thdnAt[7] = { 0 };
        int idx = 0;
        for (double lv : levels) {
            const double amp = std::pow(10.0, lv / 20.0);
            std::vector<double> x = sine((size_t)(5.0 * fs), fs, 1000.0, amp);
            quantize(x, 16);
            const QualityResult r = q1(x, fs);
            const double floorDb = 20.0 * std::log10(quant_rms(16) / (amp / std::sqrt(2.0)));
            thdnAt[idx] = r.thdnDb;
            std::printf("          %-10.1f %-12.2f %-14.2f %+-12.2f\n", lv, r.thdnDb, floorDb, r.thdnDb - floorDb);
            ++idx;
        }
        check(thdnAt[0] < -85.0, "满幅（0 dBFS）16 bit 正弦的 THD+N 地板 < −85 dB（量化极限 ≈ −98 dB）",
              f2("实测 %.2f dB", thdnAt[0]));
        check(thdnAt[4] > -50.0, "★ −60 dBFS 测试音测不出 −100 dB 的 THD+N（16 bit 地板 ≈ −43 dB）",
              f2("实测 %.2f dB（满幅 %.2f dB，差 %.1f dB）", thdnAt[4], thdnAt[0], thdnAt[4] - thdnAt[0]));
        check(thdnAt[4] - thdnAt[0] > 30.0,
              "THD+N 的可测下限随信号电平恶化 >30 dB（−60 dBFS vs 0 dBFS）",
              f2("差 %.1f dB", thdnAt[4] - thdnAt[0]));
        std::printf("          ⟹ 结论：THD+N 的绝对 dB 数【只能在满幅附近】读；−60 dBFS 以上的"
                    "数字至少被量化地板抬高 %.1f dB\n", thdnAt[4] - thdnAt[0]);
    }
    {
        // 同一电平下 24 bit：地板应好 48 dB
        std::printf("  [RANGE] 同上但 24 bit（同一 −60 dBFS）：\n");
        std::vector<double> x16 = sine((size_t)(5.0 * fs), fs, 1000.0, 0.001);
        std::vector<double> x24 = x16;
        quantize(x16, 16); quantize(x24, 24);
        const QualityResult r16 = q1(x16, fs), r24 = q1(x24, fs);
        std::printf("          16 bit THD+N %+.2f dB / 24 bit %+.2f dB（理论地板 −43.1 / −91.1）\n",
                    r16.thdnDb, r24.thdnDb);
        check(r24.thdnDb < r16.thdnDb - 30.0, "同电平下 24 bit 比 16 bit 多出 >30 dB 可测动态",
              f2("%.1f dB", r16.thdnDb - r24.thdnDb));
    }
    {
        // 削波门限扫描：正常满幅素材会不会被算成削波
        std::printf("  [RANGE] 削波判定对「峰值接近满幅」的敏感度（1 kHz，48 kHz，16 bit，5 s）：\n");
        std::printf("          %-12s %-12s %-10s %-10s %-10s\n", "幅度", "峰值dBFS", "削波占比%", "段数", "最长段");
        const double amps[] = { 1.0, 0.9997, 0.999, 0.995, 0.99, 0.98, 0.95 };
        double pctAt1 = 0.0, pctAt098 = 0.0;
        for (double a : amps) {
            std::vector<double> v = sine((size_t)(5.0 * fs), fs, 1000.0, a);
            quantize(v, 16);
            const QualityResult r = q1(v, fs);
            std::printf("          %-12.4f %-12.4f %-10.4f %-10zu %-10zu\n",
                        a, r.peakDbfs, r.clippedPct, r.clippedRuns, r.longestClipRun);
            if (a == 1.0) pctAt1 = r.clippedPct;
            if (a == 0.98) pctAt098 = r.clippedPct;
        }
        check(pctAt098 == 0.0, "峰值 0.98（−0.175 dBFS）的干净正弦：削波占比严格为 0",
              f2("%.4f%%", pctAt098));
        baseline("★ 误报边界：任何峰值 ≥ −0.0087 dBFS（0.999）的【未削波】素材都会被计入削波",
                 f2("满幅正弦实测 %.3f%%（这是完全合法的满幅正弦，不是削波）", pctAt1));
    }
    {
        // 低频满幅：驻留时间更长 → 误报更严重；同时暴露「单样本 vs 成段」判据不一致
        std::printf("  [RANGE] 满幅正弦的频率决定误报量（16 bit，峰值 0 dBFS）：\n");
        const double hzs[] = { 40.0, 100.0, 1000.0, 10000.0 };
        for (double hz : hzs) {
            std::vector<double> v = sine((size_t)(5.0 * fs), fs, hz, 1.0);
            quantize(v, 16);
            const QualityResult r = q1(v, fs);
            std::printf("          %6.0f Hz：占比 %7.4f%%  样本 %7zu  段数 %5zu  最长 %5zu\n",
                        hz, r.clippedPct, r.clippedSamples, r.clippedRuns, r.longestClipRun);
            if (hz == 40.0)
                baseline(f2("40 Hz 满幅正弦（合法素材）被判 %.4f%% 削波", r.clippedPct));
        }
    }
    {
        // 本底噪声 / SNR 的有效范围
        std::printf("  [RANGE] 白噪（16 bit，10 s）本底噪声：块 P10 的准确度 vs 电平\n");
        std::printf("          %-12s %-14s %-12s %-10s\n", "设定dBFS", "实测本底", "谱最小", "SNR");
        for (double lv : { -40.0, -60.0, -80.0, -90.0, -100.0, -120.0 }) {
            std::vector<double> v((size_t)(10.0 * fs), 0.0);
            add_noise(v, std::pow(10.0, lv / 20.0), 9001);
            quantize(v, 16);
            const QualityResult r = q1(v, fs);
            std::printf("          %-12.1f %-14.2f %-12.2f %-10.2f\n", lv, r.noiseFloorDbfs, r.spectralMinDbfs, r.snrDb);
            if (lv == -90.0)
                check(std::fabs(r.noiseFloorDbfs + 90.0) < 1.5,
                      "本底噪声在 −90 dBFS 仍准（16 bit 最低有效位 ≈ −96 dBFS，尚有 6 dB 余量）",
                      f2("实测 %.2f dBFS", r.noiseFloorDbfs));
        }
        std::printf("          ⟹ 结论：本底噪声/SNR 在 −96 dBFS 以下被 16 bit 量化地板钳住，测的已经不是素材\n");
    }
    {
        // 频响四项的有效范围：低电平下的量化噪声地板怎么污染 >10k 占比
        std::printf("  [RANGE] 1 kHz 正弦（16 bit）：>10 kHz 占比被量化噪声撑出的假地板\n");
        std::printf("          %-12s %-12s %-14s %-12s\n", "峰值dBFS", ">10k占比", "谱重心Hz", "95%滚降Hz");
        for (double lv : { -20.0, -40.0, -60.0, -80.0 }) {
            std::vector<double> v = sine((size_t)(5.0 * fs), fs, 1000.0, std::pow(10.0, lv / 20.0));
            quantize(v, 16);
            const QualityResult r = q1(v, fs);
            std::printf("          %-12.1f %-12.2f %-14.0f %-12.0f\n", lv, r.hfRatio10kDb, r.centroidHz, r.rolloff95Hz);
        }
        std::printf("          ⟹ 结论：>10k 占比的下限 ≈ 量化噪声里 10 kHz 以上那部分；纯正弦报 −70 dB 不代表"
                    "素材有高频\n");
    }

    // ================================================================ C. 说谎场景
    std::printf("\n=== C. 会说谎的场景（指标输出 + 会不会误导）===\n");
    {
        std::printf("  [LIE] 全静音 10 s（2 ch，数字 0）：\n");
        std::vector<double> z((size_t)(10.0 * fs), 0.0);
        const QualityResult r = q2(z, z, fs);
        std::printf("          peak %.1f  rms %.1f  crest %.1f  centroid %.0f  tilt %+.2f  rolloff %.0f  hf10k %+.1f\n",
                    r.peakDbfs, r.rmsDbfs, r.crestDb, r.centroidHz, r.tiltDbPerOct, r.rolloff95Hz, r.hfRatio10kDb);
        std::printf("          corr %.3f  monoLoss %.1f  sideMid %.1f  noise %.1f  snr %.1f  silence %.1f%%  valid=%d\n",
                    r.channelCorr, r.monoLossDb, r.sideMidDb, r.noiseFloorDbfs, r.snrDb, r.silencePct, (int)r.valid);
        baseline("静音文件仍报 valid=true 且给出一整套「看起来有效」的 0 值（重心 0 Hz / 倾斜 0 dB/oct / SNR 0 dB）",
                 f2("peak %.0f dBFS, SNR %.0f dB", r.peakDbfs, r.snrDb));
    }
    {
        std::printf("  [LIE] 直流偏移 0.5（无任何交流成分）：\n");
        std::vector<double> dc((size_t)(10.0 * fs), 0.5);
        const QualityResult r = q1(dc, fs);
        std::printf("          peak %.2f  rms %.2f  crest %.2f  centroid %.0f  tilt %+.2f  rolloff %.0f  hf10k %+.1f  snr %.1f\n",
                    r.peakDbfs, r.rmsDbfs, r.crestDb, r.centroidHz, r.tiltDbPerOct, r.rolloff95Hz, r.hfRatio10kDb, r.snrDb);
        baseline("纯直流被报成「峰值因子 0 dB + 重心 0 Hz + 无高频」——看起来像「满幅方波/极低重心素材」",
                 f2("crest %.2f dB, centroid %.0f Hz", r.crestDb, r.centroidHz));
    }
    {
        std::printf("  [LIE] 单个非零采样点（0.2 s 内一个 1.0，其余 0）：\n");
        std::vector<double> imp((size_t)(0.2 * fs), 0.0);
        imp[imp.size() / 2] = 1.0;
        const QualityResult r = q1(imp, fs);
        std::printf("          peak %.1f  rms %.1f  crest %.1f  centroid %.0f  rolloff %.0f  hf10k %+.1f  clipPct %.4f  runs %zu\n",
                    r.peakDbfs, r.rmsDbfs, r.crestDb, r.centroidHz, r.rolloff95Hz, r.hfRatio10kDb,
                    r.clippedPct, r.clippedRuns);
        baseline("单点冲激被报成「宽带亮声素材」（重心 ≈ 奈奎斯特 / >10k 占比 ≈ −2 dB）",
                 f2("centroid %.0f Hz, hf10k %+.1f dB", r.centroidHz, r.hfRatio10kDb));
    }
    {
        std::printf("  [LIE] 单声道内容塞进 2 声道（L == R，44.1 kHz）：\n");
        std::vector<double> l, r;
        make_music(l, r, fs, 15.0, 0.9, false);
        const QualityResult q = q2(l, l, fs);
        std::printf("          stereo=%d  corr %.6f  monoLoss %.4f  sideMid %.1f  （note: %s）\n",
                    (int)q.stereo, q.channelCorr, q.monoLossDb, q.sideMidDb, q.note.c_str());
        check(q.stereo && std::fabs(q.channelCorr - 1.0) < 1e-9, "L==R → stereo=true 且相关 = 1（指标本身没错）");
        baseline("但 19 项里【没有任何一项】说「这其实是单声道」；UI 会把 stereo=true 当「立体声」展示",
                 f2("corr=%.3f, side/mid=%.1f dB", q.channelCorr, q.sideMidDb));
    }
    {
        std::printf("  [LIE] 相位反转立体声（R = −L）：\n");
        std::vector<double> l, r;
        make_music(l, r, fs, 15.0, 0.9, false);
        for (size_t i = 0; i < r.size(); ++i) r[i] = -l[i];   // 严格 R = −L
        const QualityResult q = q2(l, r, fs);
        std::printf("          corr %.6f  monoLoss %.1f  sideMid %.1f\n", q.channelCorr, q.monoLossDb, q.sideMidDb);
        check(q.channelCorr < -0.99 && q.monoLossDb > 100.0, "R=−L → 相关 −1、单声道化后损失 >100 dB（判据正确）");
        baseline("但「相关 −1」也可能是【老唱片的真实声道极性】或【单声道素材被塞进反相两声道】——"
                 "必须加限定词，不能直接说「录音接反了」", "");
    }
    {
        std::printf("  [LIE] 窄带素材（300–3400 Hz，模拟电话音质/老唱片）：\n");
        // 严格砖墙带限：FFT 上把 [300, 3400] Hz 之外的 bin 全部置零再 IFFT（定义清楚，无滤波器过渡带问题）
        const size_t N = (size_t)1 << 20;                  // 21.8 s @ 48 kHz
        std::vector<rta::dsp::cplx> spec(N, rta::dsp::cplx(0.0, 0.0));
        {
            Rng rng(7777);
            for (size_t k = 1; k < N / 2; ++k) {
                const double f = (double)k * fs / (double)N;
                if (f < 300.0 || f > 3400.0) continue;
                const double ph = 2.0 * kPi * rng.uni();
                spec[k] = rta::dsp::cplx(std::cos(ph), std::sin(ph));
                spec[N - k] = std::conj(spec[k]);
            }
        }
        rta::dsp::fft(spec, true);
        std::vector<double> v(N);
        for (size_t i = 0; i < N; ++i) v[i] = spec[i].real();
        const QualityResult r = q1(v, fs);
        std::printf("          centroid %.0f Hz  rolloff95 %.0f Hz  hf10k %+.1f dB  tilt %+.2f dB/oct\n",
                    r.centroidHz, r.rolloff95Hz, r.hfRatio10kDb, r.tiltDbPerOct);
        baseline("窄带素材只表现为「重心低/滚降低」，19 项里没有任何一项能自证「带宽只有 3.4 kHz」；"
                 "「假无损」判定属于 fake_lossless.h，不在本 19 项里", 
                 f2("rolloff95 %.0f Hz", r.rolloff95Hz));
    }
    {
        std::printf("  [LIE] 极短样本：0.2 / 0.5 / 1.0 s 白噪 vs 10 s 参考（谱四项）：\n");
        std::vector<double> ref((size_t)(10.0 * fs), 0.0);
        add_noise(ref, 0.1, 31337);
        const QualityResult R = q1(ref, fs);
        std::printf("          参考 10 s：centroid %.0f  rolloff %.0f  hf10k %+.2f  tilt %+.2f（窗数 %zu）\n",
                    R.centroidHz, R.rolloff95Hz, R.hfRatio10kDb, R.tiltDbPerOct, R.spectralWindows);
        double worstC = 0.0, worstR = 0.0;
        for (double sec : { 0.2, 0.5, 1.0 }) {
            double cMin = 1e300, cMax = -1e300, rMin = 1e300, rMax = -1e300;
            for (int rep = 0; rep < 5; ++rep) {
                std::vector<double> s = slice(ref, (size_t)(rep * 0.11 * fs), (size_t)(sec * fs));
                const QualityResult x = q1(s, fs);
                cMin = std::min(cMin, x.centroidHz); cMax = std::max(cMax, x.centroidHz);
                rMin = std::min(rMin, x.rolloff95Hz); rMax = std::max(rMax, x.rolloff95Hz);
            }
            const double dc = (cMax - cMin) / std::max(1.0, R.centroidHz);
            const double dr = (rMax - rMin) / std::max(1.0, R.rolloff95Hz);
            std::printf("          %.2f s（5 个起点）：centroid %.0f–%.0f Hz（极差 %.1f%%）  rolloff %.0f–%.0f Hz（极差 %.1f%%）\n",
                        sec, cMin, cMax, 100.0 * dc, rMin, rMax, 100.0 * dr);
            if (sec == 0.2) { worstC = dc; worstR = dr; }
        }
        check(worstC < 1.0, "0.2 s 白噪的谱重心极差 < 100%（即：短样本仍「有点用」但不足以定量）",
              f2("极差 %.1f%%", 100.0 * worstC));
        baseline("0.2 s 素材只有 1 个谱窗（nWin=min(8, n/kW)），谱四项的误差量级见上；"
                 "报告里必须注明「短样本」", f2("极差 centroid %.1f%% / rolloff %.1f%%", 100.0 * worstC, 100.0 * worstR));
    }
    {
        std::printf("  [LIE] 8 kHz 采样率（奈奎斯特 4 kHz）白噪：\n");
        std::vector<double> v((size_t)(10.0 * 8000.0), 0.0);
        add_noise(v, 0.1, 555);
        const QualityResult r = q1(v, 8000.0);
        std::printf("          centroid %.0f Hz  rolloff95 %.0f Hz  hf10k %+.2f dB  tilt %+.2f dB/oct\n",
                    r.centroidHz, r.rolloff95Hz, r.hfRatio10kDb, r.tiltDbPerOct);
        check(r.hfRatio10kDb <= -100.0,
              "fs=8 kHz 时 >10k 占比判为「无此频段」（≤ −100 dB），不再拿奈奎斯特 bin 充数",
              f2("实测 %.2f dB", r.hfRatio10kDb));
        baseline("同时谱倾斜的最小二乘区间被悄悄改成 [100 Hz, 4 kHz]（代码里 min(half, 10k/bin)），"
                 "与 44.1/48 kHz 的 [100 Hz, 10 kHz] 不可比", f2("tilt %+.2f", r.tiltDbPerOct));
    }
    {
        std::printf("  [LIE] 192 kHz 采样率 + 16 bit 量化噪声：频响四项是否还成立\n");
        std::vector<double> v = sine((size_t)(3.0 * 192000.0), 192000.0, 1000.0, 0.0001);  // −80 dBFS
        quantize(v, 16);
        const QualityResult r = q1(v, 192000.0);
        std::printf("          centroid %.0f Hz  rolloff95 %.0f Hz  hf10k %+.2f dB  tilt %+.2f  THD+N %+.2f 窗数 %zu\n",
                    r.centroidHz, r.rolloff95Hz, r.hfRatio10kDb, r.tiltDbPerOct, r.thdnDb, r.spectralWindows);
        baseline("192 kHz 下量化噪声铺到 96 kHz：−80 dBFS 的 1 kHz 纯音报出 >10k 占比 −20 dB 量级、"
                 "THD+N −19 dB —— 这些数字反映的是【量化噪声】而不是素材",
                 f2("hf10k %+.2f dB, THD+N %+.2f dB", r.hfRatio10kDb, r.thdnDb));
    }
    {
        std::printf("  [LIE] DSD 式噪声整形（20 kHz 以上 +30 dB 抬升，352.8 kHz）：\n");
        const double fsd = 352800.0;
        std::vector<double> v((size_t)(2.0 * fsd), 0.0);
        add_noise(v, 0.03, 24680);
        // 把 20 kHz 以上整体抬 30 dB：用一阶高通 + 混入
        {
            double hp = 0.0, prev = 0.0;
            const double a = std::exp(-2.0 * kPi * 20000.0 / fsd);
            for (size_t i = 0; i < v.size(); ++i) {
                hp = a * (hp + v[i] - prev);
                prev = v[i];
                v[i] += 30.0 * hp;
            }
        }
        const QualityResult r = q1(v, fsd);
        std::printf("          centroid %.0f Hz  rolloff95 %.0f Hz  hf10k %+.2f dB  tilt %+.2f dB/oct\n",
                    r.centroidHz, r.rolloff95Hz, r.hfRatio10kDb, r.tiltDbPerOct);
        check(r.centroidHz > 50000.0, "噪声整形素材的重心被推到 >50 kHz（频响四项在 DSD 源上失去意义）",
              f2("实测 %.0f Hz", r.centroidHz));
        baseline("DSD→PCM 转换后的 >10 kHz 占比恒为正（噪声整形），不能用它判断「高频丰富/刺耳」", "");
    }
    {
        std::printf("  [LIE] 单边静音立体声（R 全 0，L 正常）：\n");
        std::vector<double> l, r;
        make_music(l, r, fs, 10.0, 0.9, false);
        for (double& v : r) v = 0.0;
        const QualityResult q = q2(l, r, fs);
        std::printf("          corr %.3f  monoLoss %.3f  sideMid %.3f  rms %.2f\n",
                    q.channelCorr, q.monoLossDb, q.sideMidDb, q.rmsDbfs);
        baseline("右声道全 0 时报「side/mid = 0 dB」，看起来像「完全中置的立体声」，"
                 "其实是单边无声（corr=0 才是线索）", f2("corr %.3f / sideMid %.1f", q.channelCorr, q.sideMidDb));
    }

    // ================================================================ D. 真 bug
    std::printf("\n=== D. 真 bug 复现（当前行为作为回归基线）===\n");
    {
        // D1：整曲削波时，末尾「仍挂着」的连续段不计入 clippedRuns
        std::printf("  [BUG ] D1 整曲满幅方波（5 s，全部样本 = 1.0）：\n");
        std::vector<double> sq((size_t)(5.0 * fs), 1.0);
        const QualityResult r = q1(sq, fs);
        std::printf("          clippedPct %.2f%%  样本 %zu  段数 %zu  最长段 %zu\n",
                    r.clippedPct, r.clippedSamples, r.clippedRuns, r.longestClipRun);
        if (r.clippedRuns == 0 && r.longestClipRun > 0)
            bug("clippedRuns 漏计【结尾仍开放】的连续削波段：段数 0 但最长段 = 样本数（自相矛盾）",
                f2("runs=%.0f, longest=%.0f", (double)r.clippedRuns, (double)r.longestClipRun));
        else
            check(r.clippedRuns >= 1, "整曲削波至少计 1 段（结尾开放段也计入）",
                  f2("runs=%.0f, longest=%.0f", (double)r.clippedRuns, (double)r.longestClipRun));
    }
    {
        // D2：单样本越限计入 clippedPct，但成段判据(kClipRunMin=3)不认 —— 两个字段口径不一致
        std::printf("  [BUG ] D2 稀疏单点满幅（每 100 ms 一个孤立 1.0，其余 0.5 正弦）：\n");
        std::vector<double> v = sine((size_t)(5.0 * fs), fs, 1000.0, 0.5);
        for (size_t i = 0; i < v.size(); i += (size_t)(0.1 * fs)) v[i] = 1.0;
        const QualityResult r = q1(v, fs);
        std::printf("          样本 %zu  占比 %.4f%%  段数 %zu  最长段 %zu\n",
                    r.clippedSamples, r.clippedPct, r.clippedRuns, r.longestClipRun);
        if (r.clippedSamples > 0 && r.clippedRuns == 0)
            bug("clippedPct 计入全部越限样本，而「≥3 样本成段」只作用于段数/最长段 "
                "⟹ 会出现「有削波占比、0 个削波段」，两个字段互相打脸",
                f2("%.4f%% 但 runs=0", r.clippedPct));
        else
            check(true, "D2 口径一致性现状", f2("pct=%.4f%% runs=%zu", r.clippedPct, r.clippedRuns));
    }
    {
        // D3：THD+N 的中位数被【静音窗】污染。
        //     修前实测：THD+N = −0.01 dB（99.93%）  ⟹ 修后：−80.94 dB（0.01%）
        //     （用严格 0 的静音段测不出来 —— tot==0 会被 continue 掉；真实文件的"静音"是
        //      −90 dBFS 量级的抖动噪声，正是这种窗口把中位数拉到 0 dB。）
        std::printf("  [BUG ] D3 前 5 s 1 kHz 满幅正弦 + 后 15 s −90 dBFS 抖动静音（共 20 s）：\n");
        // "静音"段用 −90 dBFS 的抖动噪声（真实文件里几乎不存在数字全零的静音段；
        //  用严格 0 时 tot==0 会被 continue 掉，反而看不出污染）
        std::vector<double> v((size_t)(20.0 * fs), 0.0);
        {
            std::vector<double> s = sine((size_t)(5.0 * fs), fs, 1000.0, 0.5);
            for (size_t i = 0; i < s.size(); ++i) v[i] = s[i];
            add_noise(v, 3.0e-5, 555);
            quantize(v, 16);
        }
        const QualityResult r = q1(v, fs);
        std::printf("          thdnAvail %d  THD+N %.2f dB（%.2f%%）  method %s  窗数 %zu\n",
                    (int)r.thdnAvailable, r.thdnDb, r.thdnPct, r.thdnMethod.c_str(), r.spectralWindows);
        if (r.thdnAvailable && r.thdnDb > -3.0)
            bug("THD+N 的「窗间中位数」把【纯静音窗】也算了进去（静音窗 rf/tot ≈ 1 ⟹ 0 dB），"
                "一半静音就能把 −95 dB 抬到 0 dB（100% 失真）",
                f2("%.2f dB / %.2f%%", r.thdnDb, r.thdnPct));
        else
            check(true, "D3 静音窗未污染 THD+N 中位数", f2("实测 %.2f dB", r.thdnDb));
    }
    {
        // D4：>2 声道 + 短素材时 note 被覆盖（丢掉「立体声指标不适用」）
        std::printf("  [BUG ] D4 6 声道 + 1500 采样（0.031 s，短于一个谱窗）：\n");
        const size_t n = 1500;
        std::vector<std::vector<double>> ch(6);
        for (int c = 0; c < 6; ++c) { ch[(size_t)c].assign(n, 0.0); add_noise(ch[(size_t)c], 0.05, 100 + (unsigned long long)c); }
        std::vector<const double*> p;
        for (auto& c : ch) p.push_back(c.data());
        const QualityResult r = measure_quality(p.data(), 6, n, fs);
        std::printf("          note = 「%s」\n", r.note.c_str());
        if (r.note.find("立体声指标不适用") == std::string::npos)
            bug("finalize() 用 r.note = ...（赋值而非追加）写「短于一个谱窗」，"
                "把前面 >2 声道的「立体声指标不适用」整条覆盖掉", "note 丢失多声道提示");
        else
            check(true, "D4 note 未被覆盖", r.note);
    }
    {
        // D5：note 文案必须与实现同口径（noiseFloorDbfs 是 100 ms 块 P10，不是谱最小统计）
        std::vector<double> v((size_t)(10.0 * fs), 0.0);
        add_noise(v, 0.01, 123);
        const QualityResult r = q1(v, fs);
        const bool claimsSpectral = r.note.find("本底噪声为谱最小统计") != std::string::npos;
        const bool saysBlockP10 = r.note.find("块 P10") != std::string::npos;
        const bool actuallyBlock = std::fabs(r.noiseFloorDbfs - r.quietP10Dbfs) < 1e-12;
        std::printf("  [NOTE] D5 note 文案与实现：含「谱最小统计」=%d，含「块 P10」=%d，"
                    "noiseFloorDbfs==quietP10Dbfs=%d（值 %.2f vs spectralMin %.2f）\n",
                    (int)claimsSpectral, (int)saysBlockP10, (int)actuallyBlock,
                    r.noiseFloorDbfs, r.spectralMinDbfs);
        std::printf("          当前 note = 「%s」\n", r.note.c_str());
        check(saysBlockP10 && actuallyBlock && !claimsSpectral,
              "note 文案与实现一致（都说 100 ms 块 P10，不再声称「谱最小统计」）",
              r.note);
    }
    std::printf("\n小结：失败项 %d，已知缺陷/基线 %d，真 bug %d\n", fails, baselineNotes, bugs);
    std::printf("%s\n", fails == 0 ? "全部不变量成立（已知缺陷见 [BASE]/[BUG] 行）" : "有不变量失败");
    return fails == 0 ? 0 : 1;
}
