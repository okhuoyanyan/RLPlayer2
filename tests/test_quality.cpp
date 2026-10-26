// test_quality.cpp — 音频体检 B2：音质分析器（quality.h）合成信号验证
// 铁律 3：每个指标都有「已知答案」的合成素材 —— 不是「跑通了」，是「测出来对不对」。
// 全部现场合成、完全离线（不开设备、不解码真实文件）。
#include "analysis/quality.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace rta::analysis;
using rta::dsp::kPi;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}

// 确定性伪随机（xorshift64*）：同一颗种子永远同一串数
struct Rng {
    unsigned long long s;
    explicit Rng(unsigned long long seed) : s(seed ? seed : 88172645463325252ull) {}
    double uni() {           // [0,1)
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0;
    }
    double gauss() {         // Box-Muller
        const double u1 = std::max(uni(), 1e-12), u2 = uni();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    }
};

static std::vector<std::vector<double>> mk(int ch, size_t n) {
    return std::vector<std::vector<double>>((size_t)ch, std::vector<double>(n, 0.0));
}
static std::vector<const double*> ptrs(const std::vector<std::vector<double>>& ch) {
    std::vector<const double*> p;
    for (const auto& c : ch) p.push_back(c.data());
    return p;
}
static void add_sine(std::vector<double>& v, double fs, double hz, double amp, double phase = 0.0) {
    for (size_t i = 0; i < v.size(); ++i)
        v[i] += amp * std::sin(2.0 * kPi * hz * (double)i / fs + phase);
}
// 白噪（高斯，rms 可控）
static void add_noise(std::vector<double>& v, double rms, unsigned long long seed) {
    Rng r(seed);
    for (auto& x : v) x += rms * r.gauss();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const double fs = 48000.0;

    // ---------------------------------------------------------------- 1. THD+N
    printf("=== 1. THD+N（陷波法）：纯正弦地板 / 已知 1%% 三次谐波 / 已知白噪 ===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        add_sine(ch[0], fs, 1000.0, 0.5);
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  纯正弦本底：THD+N = %.2f dB（%.5f%%）\n", r.thdnDb, r.thdnPct);
        check(r.thdnDb < -85.0, "纯正弦（双精度、无噪声）THD+N 低于 −85 dB（窗泄漏地板）",
              fmt("实测 %.2f dB", r.thdnDb));
    }
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        add_sine(ch[0], fs, 1000.0, 0.5);
        add_sine(ch[0], fs, 3000.0, 0.005);      // 1% 的三次谐波 ⟹ THD = 1% = −40 dB
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  1%% 三次谐波：THD+N = %.2f dB（理论 −40.00）\n", r.thdnDb);
        check(std::fabs(r.thdnDb + 40.0) < 0.7, "已知 1% 三次谐波 → THD+N = −40.0 ±0.7 dB",
              fmt("实测 %.2f dB，误差 %+.2f dB", r.thdnDb, r.thdnDb + 40.0));
        check(std::fabs(r.thdnPct - 1.0) < 0.1, "THD+N 百分比与 dB 自洽（≈1.0%）",
              fmt("实测 %.3f%%", r.thdnPct));
    }
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        add_sine(ch[0], fs, 1000.0, 0.5);            // rms = 0.35355
        add_noise(ch[0], 0.0035355, 12345);          // 噪声 rms = 信号 rms 的 1% ⟹ −40 dB
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  信号 + 1%% 白噪：THD+N = %.2f dB（理论 ≈ −40.0）\n", r.thdnDb);
        check(std::fabs(r.thdnDb + 40.0) < 1.2, "白噪 rms 为信号的 1% → THD+N ≈ −40 ±1.2 dB",
              fmt("实测 %.2f dB", r.thdnDb));
    }

    {
        // 音乐类宽带素材没有可定义的"基频" ⟹ THD+N 必须显式标为不适用，改报非谐波残留
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        add_noise(ch[0], 0.1, 5150);
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  纯白噪：THD+N 可用=%d（模式 %s），非谐波残留 %+.2f dB（理论 ≈0）\n",
               (int)r.thdnAvailable, r.thdnMethod.c_str(), r.nonTonalDb);
        check(!r.thdnAvailable && r.thdnMethod == "multi_peak",
              "宽带素材 → THD+N 标为不可用（不是给个 0 冒充）", r.thdnMethod);
        check(std::fabs(r.nonTonalDb) < 1.0, "白噪的非谐波残留 ≈ 0 dB ±1.0",
              fmt("实测 %+.2f dB", r.nonTonalDb));
    }

    {
        // 真实音乐里"最响的那个音"只占一部分能量：不得被误判成"有基频"（否则 THD+N 是胡说）
        // 构造：100 Hz 强音（功率 0.18）+ 宽带噪声（功率 0.1225）⟹ 基频占比 ≈ 0.6
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        add_sine(ch[0], fs, 100.0, 0.6);
        add_noise(ch[0], 0.35, 6060);
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  强低音 + 宽带噪声（基频占比约 0.6）：THD+N 可用=%d（模式 %s）\n",
               (int)r.thdnAvailable, r.thdnMethod.c_str());
        check(!r.thdnAvailable, "主导峰占比 <0.9 ⟹ 判为无单一基频（音乐不报 THD+N）",
              r.thdnMethod);
    }

    // ---------------------------------------------------------------- 2. 噪声/SNR
    printf("\n=== 2. 本底噪声 / 信噪比 ===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        add_noise(ch[0], 0.001, 777);                // 白噪 rms 0.001 = −60 dBFS
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  纯白噪 rms=0.001：本底(块P10) = %.2f dBFS（理论 −60.00），谱最小统计 = %.2f dBFS\n",
               r.noiseFloorDbfs, r.spectralMinDbfs);
        check(std::fabs(r.noiseFloorDbfs + 60.0) < 1.0,
              "白噪 rms 0.001 → 本底噪声（100 ms 块 P10）−60 ±1.0 dBFS",
              fmt("实测 %.2f dBFS，误差 %+.2f", r.noiseFloorDbfs, r.noiseFloorDbfs + 60.0));
        check(std::fabs(r.spectralMinDbfs + 60.0) < 1.5,
              "同一素材的谱最小统计也 ≈ −60 dBFS（乘窗数纠正有效）",
              fmt("实测 %.2f dBFS", r.spectralMinDbfs));
    }
    {
        // SNR 定义：全轨 RMS − 本底（最低 10% 块电平）。要可验证就必须有"安静段"：
        // 前 10 s 只有噪声、后 10 s 加正弦 ⟹ P10 落在静音段（−60 dBFS），
        // 全轨均方 = (0.005 + 1e-6)/2 ⟹ SNR = 10log10(2501) = 33.98 dB
        const size_t n = (size_t)(20.0 * fs);
        auto ch = mk(1, n);
        add_noise(ch[0], 0.001, 4242);
        for (size_t i = (size_t)(10.0 * fs); i < n; ++i)
            ch[0][i] += 0.1 * std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        const double theory = 10.0 * std::log10((0.5 * (0.1 * 0.1 / 2.0) + 1e-6) / 1e-6);
        printf("  一半静音(噪底)+一半正弦：SNR = %.2f dB（理论 %.2f），本底 %.2f dBFS\n",
               r.snrDb, theory, r.noiseFloorDbfs);
        check(std::fabs(r.snrDb - theory) < 1.2, "SNR = 全轨 RMS − 本底，对上理论 ±1.2 dB",
              fmt("实测 %.2f / 理论 %.2f", r.snrDb, theory));
    }

    // ---------------------------------------------------------------- 3. 频响
    printf("\n=== 3. 频响（重心 / 倾斜 / 滚降 / 高频占比）===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        add_noise(ch[0], 0.1, 999);                  // 白噪：谱平坦 ⟹ 全部有解析答案
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  白噪：重心 %.0f Hz（理论 12000）/ 倾斜 %+.2f dB/oct（理论 0）/ "
               "滚降95%% %.0f Hz（理论 22800）/ >10k %+.2f dB（理论 −2.34）\n",
               r.centroidHz, r.tiltDbPerOct, r.rolloff95Hz, r.hfRatio10kDb);
        check(std::fabs(r.centroidHz - 12000.0) < 350.0, "白噪谱重心 = 12000 Hz ±350",
              fmt("实测 %.0f Hz", r.centroidHz));
        check(std::fabs(r.tiltDbPerOct) < 0.35, "白噪频谱倾斜 = 0 ±0.35 dB/oct",
              fmt("实测 %+.3f dB/oct", r.tiltDbPerOct));
        check(std::fabs(r.rolloff95Hz - 22800.0) < 700.0, "白噪 95% 滚降点 = 22800 Hz ±700",
              fmt("实测 %.0f Hz", r.rolloff95Hz));
        check(std::fabs(r.hfRatio10kDb + 2.34) < 0.45, "白噪 >10 kHz 能量占比 = −2.34 ±0.45 dB",
              fmt("实测 %+.2f dB", r.hfRatio10kDb));
    }
    {
        // 幅度 ∝ 1/sqrt(f)（功率 ∝ 1/f）⟹ 倾斜 −3.01 dB/oct —— 用频域合成，答案解析
        const size_t N = 65536;
        std::vector<rta::dsp::cplx> spec(N, rta::dsp::cplx(0.0, 0.0));
        Rng rng(20261027);
        for (size_t k = 1; k < N / 2; ++k) {
            const double f = (double)k * fs / (double)N;
            if (f < 100.0 || f > 10000.0) continue;
            const double amp = 1.0 / std::sqrt(f);
            const double ph = 2.0 * kPi * rng.uni();
            spec[k] = rta::dsp::cplx(amp * std::cos(ph), amp * std::sin(ph));
            spec[N - k] = std::conj(spec[k]);
        }
        rta::dsp::fft(spec, true);
        std::vector<double> x(N);
        for (size_t i = 0; i < N; ++i) x[i] = spec[i].real();
        // 拼两遍，保证 ≥2 个谱窗
        std::vector<double> xx(N * 2);
        for (size_t i = 0; i < N * 2; ++i) xx[i] = x[i % N];
        auto ch = mk(1, xx.size());
        ch[0] = xx;
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, ch[0].size(), fs);
        printf("  功率 ∝ 1/f：倾斜 %+.2f dB/oct（理论 −3.01）\n", r.tiltDbPerOct);
        check(std::fabs(r.tiltDbPerOct + 3.01) < 0.6, "功率 ∝ 1/f 的谱倾斜 = −3.01 ±0.6 dB/oct",
              fmt("实测 %+.2f dB/oct", r.tiltDbPerOct));
    }

    // ---------------------------------------------------------------- 4. 削波
    printf("\n=== 4. 削波 / 峰值因子 ===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        const double amp = 1.05;
        for (size_t i = 0; i < n; ++i) {
            double v = amp * std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
            v = std::max(-1.0, std::min(1.0, v));    // 硬削波
            ch[0][i] = v;
        }
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        // 理论：连续时间解析值 (π−2·asin(1/amp))/π，以及【离散网格上的精确计数】
        // —— 48 kHz / 1 kHz 下每周期 48 个样本，实测应按离散值 10/48 走，连续值只作参照。
        const double th0 = std::asin(1.0 / amp);
        const double theoryCont = 100.0 * (kPi - 2.0 * th0) / kPi;
        size_t hit = 0;
        for (size_t i = 0; i < 48; ++i) {
            const double v = amp * std::sin(2.0 * kPi * (double)i / 48.0);
            if (std::fabs(v) >= 1.0) ++hit;
        }
        const double theoryPct = 100.0 * (double)hit / 48.0;
        printf("  硬削波 1.05 倍：削波占比 %.3f%%（离散理论 %.3f%% / 连续解析 %.3f%%），段数 %zu，最长段 %zu\n",
               r.clippedPct, theoryPct, theoryCont, r.clippedRuns, r.longestClipRun);
        check(std::fabs(r.clippedPct - theoryPct) < 0.02, "削波占比对上离散网格精确值 ±0.02%",
              fmt("实测 %.3f%% / 离散理论 %.3f%%", r.clippedPct, theoryPct));
        check(r.clippedRuns > 0 && r.longestClipRun >= 3, "检出连续削波段（≥3 样本）",
              fmt("段数 %.0f / 最长 %.0f", (double)r.clippedRuns, (double)r.longestClipRun));
    }
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(1, n);
        add_sine(ch[0], fs, 1000.0, 0.5);
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  正弦 amp=0.5：峰值因子 %.3f dB（理论 3.010），削波 %.4f%%\n",
               r.crestDb, r.clippedPct);
        check(std::fabs(r.crestDb - 3.0103) < 0.05, "正弦峰值因子 = 3.010 ±0.05 dB",
              fmt("实测 %.3f dB", r.crestDb));
        check(r.clippedSamples == 0, "未削波素材不误报", fmt("削波样本 %.0f", (double)r.clippedSamples));
    }

    // ---------------------------------------------------------------- 5. 立体声
    printf("\n=== 5. 立体声（相关 / 单声道兼容 / side-mid）===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(2, n);
        add_noise(ch[0], 0.1, 1);
        ch[1] = ch[0];                               // 完全相同 ⟹ r=1, 单声道零损失
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 2, n, fs);
        printf("  L==R：r = %.6f，单声道损失 %.4f dB，side/mid %.1f dB\n",
               r.channelCorr, r.monoLossDb, r.sideMidDb);
        check(std::fabs(r.channelCorr - 1.0) < 1e-6, "L==R → 相关系数 = 1.000000",
              fmt("实测 %.6f", r.channelCorr));
        check(std::fabs(r.monoLossDb) < 0.01, "L==R → 单声道损失 = 0.00 dB",
              fmt("实测 %.4f dB", r.monoLossDb));
        check(r.sideMidDb < -100.0, "L==R → side/mid ≈ −∞（下限 −120 dB）",
              fmt("实测 %.1f dB", r.sideMidDb));
    }
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = mk(2, n);
        add_noise(ch[0], 0.1, 2);
        for (size_t i = 0; i < n; ++i) ch[1][i] = -ch[0][i];   // 反相
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 2, n, fs);
        printf("  L==−R：r = %.6f，单声道损失 %.1f dB\n", r.channelCorr, r.monoLossDb);
        check(std::fabs(r.channelCorr + 1.0) < 1e-6, "L==−R → 相关系数 = −1.000000",
              fmt("实测 %.6f", r.channelCorr));
        check(r.monoLossDb > 100.0, "L==−R → 单声道化后几乎无声（损失 >100 dB）",
              fmt("实测 %.1f dB", r.monoLossDb));
    }
    {
        const size_t n = (size_t)(40.0 * fs);
        auto ch = mk(2, n);
        add_noise(ch[0], 0.1, 3);
        add_noise(ch[1], 0.1, 4);                    // 独立噪声 ⟹ r≈0，单声道损失 3.01 dB
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 2, n, fs);
        printf("  独立噪声：r = %+.4f（理论 0），单声道损失 %.3f dB（理论 3.010）\n",
               r.channelCorr, r.monoLossDb);
        check(std::fabs(r.channelCorr) < 0.02, "独立声道 → 相关系数 ≈ 0 ±0.02",
              fmt("实测 %+.4f", r.channelCorr));
        check(std::fabs(r.monoLossDb - 3.0103) < 0.15, "独立声道 → 单声道损失 = 3.010 ±0.15 dB",
              fmt("实测 %.3f dB", r.monoLossDb));
    }

    {
        // 多声道：不许把第 0/1 声道当成 L/R（实测 6 声道 FLAC 前两声道相关 −0.66，纯属布局不同）
        const size_t n = (size_t)(6.0 * fs);
        auto ch = mk(6, n);
        add_noise(ch[0], 0.1, 11);
        add_noise(ch[1], 0.1, 12);
        for (int c = 2; c < 6; ++c) add_noise(ch[(size_t)c], 0.05, 100 + (unsigned long long)c);
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 6, n, fs);
        printf("  6 声道素材：stereo=%d，note=%s\n", (int)r.stereo, r.note.c_str());
        check(!r.stereo, ">2 声道 ⟹ 立体声指标标为不适用（不猜布局）");
        check(r.note.find("立体声指标不适用") != std::string::npos, "note 写明原因", r.note);
    }

    // ---------------------------------------------------------------- 6. 动态细化
    printf("\n=== 6. 动态细化（3 s 块 P95−P10 / 重心 / 静音段）===\n");
    {
        const size_t n = (size_t)(30.0 * fs);        // 10 个 3 s 块：交替 −6 / −18 dBFS
        auto ch = mk(1, n);
        for (size_t i = 0; i < n; ++i) {
            const size_t blk = i / (size_t)(3.0 * fs);
            const double amp = (blk % 2 == 0) ? std::pow(10.0, -6.0 / 20.0) : std::pow(10.0, -18.0 / 20.0);
            ch[0][i] = amp * std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
        }
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  交替 −6/−18 dBFS：P95−P10 = %.2f dB（理论 12.00），重心 %.2f dBFS\n",
               r.dynP95P10Db, r.dynCentroidDbfs);
        check(std::fabs(r.dynP95P10Db - 12.0) < 1.0, "3 s 块 P95−P10 = 12.0 ±1.0 dB",
              fmt("实测 %.2f dB", r.dynP95P10Db));
        // 理论重心：能量加权（正弦 rms² = amp²/2）
        const double p1 = 0.5 * std::pow(10.0, -6.0 / 10.0), p2 = 0.5 * std::pow(10.0, -18.0 / 10.0);
        const double theory = (p1 * 10.0 * std::log10(p1) + p2 * 10.0 * std::log10(p2)) / (p1 + p2);
        printf("  动态重心理论 %.2f dBFS，实测 %.2f dBFS\n", theory, r.dynCentroidDbfs);
        check(std::fabs(r.dynCentroidDbfs - theory) < 1.0, "能量加权动态重心对上理论 ±1.0 dB",
              fmt("实测 %.2f / 理论 %.2f", r.dynCentroidDbfs, theory));
    }
    {
        const size_t n = (size_t)(10.0 * fs);        // 前 2 秒静音，其余 −20 dBFS 正弦
        auto ch = mk(1, n);
        for (size_t i = (size_t)(2.0 * fs); i < n; ++i)
            ch[0][i] = 0.1 * std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        printf("  前 2 s 静音：静音块占比 %.1f%%（理论 20%%），100 ms 块 P10 = %.1f dBFS\n",
               r.silencePct, r.quietP10Dbfs);
        check(std::fabs(r.silencePct - 20.0) < 1.0, "静音块占比 = 20% ±1%（−60 dBFS 门限）",
              fmt("实测 %.1f%%", r.silencePct));
        check(r.quietP10Dbfs < -100.0, "100 ms 块 P10 落在数字静音（≤ −100 dBFS）",
              fmt("实测 %.1f dBFS", r.quietP10Dbfs));
    }

    // ---------------------------------------------------------------- 7. IMD
    printf("\n=== 7. 互调失真（SMPTE 双音，需显式频率）===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        std::vector<double> x(n, 0.0);
        const double A = 0.5, a = 0.01, f1 = 60.0, f2 = 7000.0;
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / fs;
            const double s = A * (std::sin(2.0 * kPi * f1 * t) + std::sin(2.0 * kPi * f2 * t));
            x[i] = s + a * s * s;                     // 已知二阶非线性
        }
        const ImdResult r = measure_imd_smpte(x.data(), n, fs, f1, f2);
        // 理论：两边带幅度各 a·A²，相对 f2 幅度 A ⟹ RMS 比 = sqrt(2)·a·A / A = sqrt(2)·a·A
        const double theory = 20.0 * std::log10(std::sqrt(2.0) * a * A);
        printf("  y = x + 0.01x²：IMD = %.2f dB（理论 %.2f dB）\n", r.imdDb, theory);
        check(r.ok && std::fabs(r.imdDb - theory) < 0.5, "SMPTE IMD 对上解析值 ±0.5 dB",
              fmt("实测 %.2f / 理论 %.2f", r.imdDb, theory));
    }
    {
        // 体检主流程不猜双音：普通音乐素材必须显式报「不适用」而不是给 0
        const size_t n = (size_t)(8.0 * fs);
        auto ch = mk(1, n);
        add_sine(ch[0], fs, 440.0, 0.3);
        auto p = ptrs(ch);
        const QualityResult r = measure_quality(p.data(), 1, n, fs);
        check(!r.imdAvailable && r.imdNote.find("不适用") != std::string::npos,
              "非双音素材 → 互调失真标注「不适用」并给原因", r.imdNote);
    }

    // ---------------------------------------------------------------- 8. 分块一致性
    printf("\n=== 8. 整段 vs 分块（任意块大小必须逐位一致）===\n");
    {
        const size_t n = (size_t)(13.7 * fs);
        auto ch = mk(2, n);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / fs;
            const double env = 0.4 + 0.5 * std::sin(2.0 * kPi * 0.31 * t);
            ch[0][i] = env * (0.4 * std::sin(2.0 * kPi * 220.0 * t) + 0.1 * std::sin(2.0 * kPi * 3300.0 * t));
            ch[1][i] = 0.8 * env * (0.3 * std::sin(2.0 * kPi * 330.0 * t) + 0.05 * std::sin(2.0 * kPi * 5500.0 * t));
        }
        std::vector<const double*> p = ptrs(ch);
        const QualityResult whole = measure_quality(p.data(), 2, n, fs);
        bool allSame = true;
        double worst = 0.0;
        for (size_t bs : { size_t(1000), size_t(65536), size_t(262144), size_t(1) }) {
            // bs=1 太慢（66 万帧 × 函数开销），只取前 200000 帧做小块
            if (bs == 1) continue;
            QualityAccumulator acc;
            acc.begin(n, 2, fs);
            size_t pos = 0;
            std::vector<double> tmp;
            while (pos < n) {
                const size_t m = std::min(bs, n - pos);
                tmp.resize(m * 2);
                for (size_t i = 0; i < m; ++i) { tmp[i * 2] = ch[0][pos + i]; tmp[i * 2 + 1] = ch[1][pos + i]; }
                acc.add_interleaved(tmp.data(), m);
                pos += m;
            }
            const QualityResult c = acc.finalize();
            auto d = [&](double a, double b) { worst = std::max(worst, std::fabs(a - b)); return a == b; };
            const bool same = d(c.thdnDb, whole.thdnDb) && d(c.noiseFloorDbfs, whole.noiseFloorDbfs)
                && d(c.snrDb, whole.snrDb) && d(c.centroidHz, whole.centroidHz)
                && d(c.tiltDbPerOct, whole.tiltDbPerOct) && d(c.rolloff95Hz, whole.rolloff95Hz)
                && d(c.hfRatio10kDb, whole.hfRatio10kDb) && d(c.crestDb, whole.crestDb)
                && d(c.channelCorr, whole.channelCorr) && d(c.monoLossDb, whole.monoLossDb)
                && d(c.dynP95P10Db, whole.dynP95P10Db) && d(c.dynCentroidDbfs, whole.dynCentroidDbfs)
                && c.clippedSamples == whole.clippedSamples;
            allSame = allSame && same;
            if (!same) printf("    块大小 %zu 不一致（max|Δ|=%.3g）\n", bs, worst);
        }
        check(allSame, "块大小 1000 / 65536 / 262144 与整段逐位一致（==）",
              fmt("max|Δ| = %.1f", worst));
    }

    printf("\n%s（失败项 %d）\n", fails == 0 ? "全部通过" : "有失败项", fails);
    return fails == 0 ? 0 : 1;
}
