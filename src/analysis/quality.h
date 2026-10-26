// quality.h — 音频体检 B2：音质分析器（失真 / 噪声 / 频响 / 削波 / 立体声 / 动态细化）
//
// 定位：dynamics.h 管「响度与动态」，fake_lossless.h 管「是不是有损转制」，
//       本文件补上用户说的「音质」本身 —— 六类客观指标，全部离线、确定性、不依赖 AI：
//        1) 失真类：THD+N（陷波法，纯音乐素材也能跑）、互调失真（需双音素材，否则显式跳过）
//        2) 噪声类：本底噪声（谱最小统计）、信噪比
//        3) 频响类：频谱重心（brightness）、频谱倾斜（dB/oct）、95% 谱滚降点、10 kHz 以上能量占比
//        4) 削波类：削波样本计数/占比、连续削波段、峰值因子（crest factor）
//        5) 立体声类：声道间相关系数、单声道兼容性（L+R 能量损失）、side/mid 比
//        6) 动态细化：3 秒窗 RMS 分布的 P95−P10、能量加权动态重心（dBFS）
//
// ================== 口径与依据（不许拍脑袋，逐条写清） ==================
//  * 谱分析窗：Blackman-Harris 4 项窗（旁瓣 −92 dB，主瓣 ±4 bin）。为什么不用 Hann：
//    THD+N 靠「挖掉基频再看剩余能量」，Hann 的第一旁瓣只有 −31 dB，1 kHz 基频的泄漏在
//    几十个 bin 外仍有 −80 dB 量级 —— 那会把 THD+N 的地板卡在 −80 dB，测不出更好的设备。
//    BH4 旁瓣 −92 dB，实测地板见 tests/test_quality.cpp（纯正弦 −100 dB 以下）。
//  * THD+N（陷波法）：每窗取最强的 ≤16 个谱峰（间隔 ≥ 2·10 bin），把峰位 ±10 bin 挖掉，
//    THD+N = 10·log10(Σ剩余 |X|² / Σ全部 |X|²)，多窗取中位数。
//    ★ 对纯正弦这是标准 THD+N；对音乐素材它刻画的是「最强 16 个谱峰之外的能量占比」，
//      即噪声 + 失真 + 非谐波成分的【上界】，不能当成单一正弦的 THD+N 去和 dB 数比。
//      之所以仍用它做「版本对比」：同一首曲子的两个版本，谐波结构几乎相同，该量对
//      「噪声地板抬升 / 削波 / 有损转制」都单调变差，是可比的相对量（见 version_compare.h）。
//  * IMD（SMPTE 双音）：需要明确的 f1/f2，故做成独立函数 measure_imd_smpte()，
//    由调用方给频率；体检主流程不猜 —— 素材不是双音时 imdAvailable=false 并写明原因。
//    本底噪声：每 bin 取各窗最小值（谱最小统计 Martin），再乘窗数纠正「取最小值」的
//    指数分布偏置（E[min of W 指数样本] = 真值/W）。平稳素材准确；动态音乐偏低（已知偏置），
//    因此 SNR 【只用于同一首不同版本之间的相对比较】（偏置对双方一致）。
//  * 削波门限 0.999（≈ −0.009 dBFS）。16-bit 解码到 double 的满幅是 32767/32768 = 0.999969，
//    落在门限内；正常节目的峰值极少越过 −0.009 dBFS，误报风险由「连续 ≥3 样本成段」再收一道。
//  * 立体声指标只取前两个声道（多声道素材按 L/R 看前两声道，其余声道不参与）。
//
// ================== 增量（流式）接口 ==================
// 曲长与内存解耦：整轨只保留
//   * 每 100 ms 一条的块能量（8 B/100 ms ≈ 0.29 MB/小时）、每 3 s 一条（8 B/3 s）；
//   * 200 Hz 的对齐包络（4 B / 5 ms ≈ 0.8 MB/小时）；
//   * ≤8 个谱窗的样本副本（8×32768×8 B = 2 MB，与曲长无关）。
// ★ 硬约定：同一组样本，measure_quality（整段）与 QualityAccumulator（任意分块）输出
//   必须【逐位一致（==）】。实现上整段接口就是「建计划 + 整段喂 + finalize」的包装，
//   两者共用同一份算术；所有逐帧累加按样本时间顺序进行，块边界只决定「何时结算窗口」。
#pragma once
#include "dynamics.h"        // 复用 rta::dsp::Biquad/kPi/percentile_sorted/JSON 无关的小工具
#include "../dsp/fft.h"      // rta::dsp::fft（radix-2）/ kPi
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <chrono>

namespace rta::analysis {

// 削波判定门限（见文件头「削波门限」）
inline constexpr double kClipThreshold = 0.999;
// 连续多少个样本越限才算「一段削波」（单样本越限可能只是正常瞬态）
inline constexpr size_t kClipRunMin = 3;

// 耗时分解（诊断用，不影响任何指标）：把「指标算得对不对」与「要花多少时间」分开记账。
// 注意：THD+N / 本底噪声 / SNR / 谱重心 / 倾斜 / 滚降 / 高频占比 共用同一组 FFT，
// 物理上无法再拆 —— 它们合计就是 spectralMs（一次 8×32768 的 FFT + 谱统计）。
struct QualityTimings {
    double timeDomainMs = 0.0;   // 峰值/RMS/削波/立体声/块统计
    double spectralMs = 0.0;     // 8 窗 FFT + 全部谱域指标
};

struct ImdResult {
    bool   ok = false;
    double imdDb = 0.0;          // 边带 RMS / f2 幅度（负 dB）
    std::string note;
};

struct QualityResult {
    bool   valid = false;

    // ---- 1. 失真 ----
    // THD+N 分两种模式（见文件头）：
    //   * 单基频模式（主导峰能量占比 ≥ 50%，即测试音/单一主音素材）：只挖基频，
    //     残差 = 谐波 + 噪声 ⟹ 这是标准意义的 THD+N，thdnAvailable=true；
    //   * 多峰模式（音乐素材）：没有可定义的"基频"，THD+N 无意义 ⟹ thdnAvailable=false，
    //     改用 nonTonalDb（16 个最强谱峰之外的能量占比），这是噪声/失真/有损残留的上界。
    double thdnDb = 0.0;         // 单基频模式：THD+N（多窗中位数）
    double thdnPct = 0.0;        // 10^(thdnDb/20) × 100
    bool   thdnAvailable = false;
    std::string thdnMethod;      // "single_tone" / "multi_peak"
    double nonTonalDb = 0.0;     // 多峰陷波后残留能量占比（对任何素材都有定义）
    bool   imdAvailable = false; // 素材非双音 ⟹ false（不是 0，是「不适用」）
    double imdDb = 0.0;
    std::string imdNote;         // 不可用时的人话原因

    // ---- 2. 噪声 ----
    // 口径（为什么用块电平而不是谱）：谱最小统计在【平稳噪声】上极准（实测 −60.00 ±0.1 dBFS），
    // 但在连续不断播放的密集音乐上，每个 bin 的最小值来自"音乐自己的谱谷"而不是噪声，
    // 实测会报到 −21 dBFS 这种荒谬值。所以：
    //   * noiseFloorDbfs = 最低 10% 的 100 ms 块电平（用户任务书给的口径：静音段/最低 10% RMS）
    //     —— 有静音段就直接测到静音段的噪声；连续音乐测到"最安静的 10% 的音乐"，
    //        是本底噪声的【上界】，同一首不同版本之间仍然可比。
    //   * spectralMinDbfs = 谱最小统计（乘窗数纠正），只作诊断，不参与评分。
    double noiseFloorDbfs = 0.0; // 低电平段本底（100 ms 块 P10）
    double spectralMinDbfs = 0.0;// 谱最小统计（平稳噪声准，密集音乐偏高，仅供参考）
    double snrDb = 0.0;          // 全轨 RMS − 本底（dB）

    // ---- 3. 频响 ----
    double centroidHz = 0.0;     // 能量加权谱重心（brightness）
    double tiltDbPerOct = 0.0;   // [100 Hz, 10 kHz] 最小二乘斜率
    double rolloff95Hz = 0.0;    // 95% 累积能量的频率（音乐本身的高频含量，不是截止点）
    double hfRatio10kDb = 0.0;   // 10 kHz 以上能量 / 全带能量（dB）

    // ---- 4. 削波 ----
    double peakDbfs = 0.0;       // 样本峰值（各声道取最大）
    double rmsDbfs = 0.0;        // 全轨 RMS（各声道平均）
    size_t clippedSamples = 0;   // 各声道合计
    double clippedPct = 0.0;     // 占「帧数×有效声道数」的百分比
    size_t clippedRuns = 0;      // 连续 ≥kClipRunMin 的段数
    size_t longestClipRun = 0;   // 最长一段的样本数
    double crestDb = 0.0;        // 峰值因子 = peakDb − rmsDb

    // ---- 5. 立体声 ----
    bool   stereo = false;       // channels >= 2
    double channelCorr = 0.0;    // Pearson r ∈ [-1,1]
    double monoLossDb = 0.0;     // 10log10(立体声平均功率 / 单声道化功率)，0=完美
    double sideMidDb = 0.0;      // 10log10(side² / mid²)

    // ---- 6. 动态细化 ----
    double dynP95P10Db = 0.0;    // 3 s 块 RMS 的 P95−P10
    double dynCentroidDbfs = 0.0;// 能量加权平均块电平
    double quietP10Dbfs = 0.0;   // 100 ms 块电平的 P10（静音段/低电平检测）
    double silencePct = 0.0;     // 低于 −60 dBFS 的 100 ms 块占比（%）

    // ---- 元 ----
    int    channels = 0;
    double fs = 0.0;
    size_t frames = 0;
    size_t spectralWindows = 0;
    std::string note;            // 全局说明（例如「素材过短，谱分析不可用」）
};

// ==================== 双音互调失真（SMPTE） ====================
// f1 低频（典型 60 Hz）、f2 高频（典型 7 kHz）。边带取 f2±f1，IMD = 边带 RMS / f2 幅度。
// 用 Hann 加权相关（非整周期也准确）：A(f) = 2·|Σ x·w·e^{−jωn}| / Σw。
// 素材不是双音时不要调用 —— 本函数不猜频率，猜错就是错的数字。
inline ImdResult measure_imd_smpte(const double* x, size_t n, double fs,
                                   double f1, double f2) {
    ImdResult r;
    if (!x || n < 64 || fs <= 0.0 || f1 <= 0.0 || f2 <= 0.0 || f1 >= fs * 0.5 || f2 >= fs * 0.5) {
        r.note = "样本不足或频率非法";
        return r;
    }
    std::vector<double> w(n);
    double wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        w[i] = 0.5 * (1.0 - std::cos(2.0 * rta::dsp::kPi * (double)i / (double)(n - 1)));
        wsum += w[i];
    }
    auto ampAt = [&](double f) {
        const double w0 = 2.0 * rta::dsp::kPi * f / fs;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double a = w0 * (double)i;
            re += x[i] * w[i] * std::cos(a);
            im -= x[i] * w[i] * std::sin(a);
        }
        return 2.0 * std::sqrt(re * re + im * im) / wsum;
    };
    const double ca = ampAt(f2);
    if (ca <= 1e-12) { r.note = "f2 处无能量（素材不是双音）"; return r; }
    const double lo = ampAt(f2 - f1), hi = ampAt(f2 + f1);
    // 口径：两边带幅度 RSS / f2 幅度（SMPTE RP-120 的边带-载波比，幅度对幅度，不含 1/√2）
    const double sb = std::sqrt(lo * lo + hi * hi);
    r.imdDb = 20.0 * std::log10(sb / ca + 1e-300);
    r.ok = true;
    char b[160];
    std::snprintf(b, sizeof(b), "SMPTE 双音 %g Hz + %g Hz：边带 RMS/f2 = %.2f dB", f1, f2, r.imdDb);
    r.note = b;
    return r;
}

namespace qdetail {

// BH4 窗系数（旁瓣 −92 dB）
inline constexpr double kB0 = 0.35875, kB1 = 0.48829, kB2 = 0.14128, kB3 = 0.01168;

// 窗样本（N 点），i 与 N 为 0 基/长度
inline double bh4(size_t i, size_t N) {
    const double t = 2.0 * rta::dsp::kPi * (double)i / (double)(N > 1 ? N - 1 : 1);
    return kB0 - kB1 * std::cos(t) + kB2 * std::cos(2.0 * t) - kB3 * std::cos(3.0 * t);
}

// 实信号【单边】功率和（Parseval）：P0 + P_Nyq + 2·Σ中间
inline double full_power_sum(const std::vector<double>& P, size_t lo, size_t hi) {
    double s = 0.0;
    for (size_t k = lo; k <= hi; ++k) s += P[k];
    s *= 2.0;
    if (lo == 0) s -= P[0];                       // 直流只算一次
    if (hi == P.size() - 1) s -= P[hi];           // 奈奎斯特只算一次
    return s;
}

// 谱窗计划：窗位置的唯一来源（整段与增量共用）
struct QualityWindowPlan {
    size_t kW = 32768;
    size_t half = 16384;
    size_t nWin = 1;
    double binHz = 1.0;
    std::vector<size_t> start;
};

inline QualityWindowPlan make_quality_plan(size_t n, double fs) {
    QualityWindowPlan p;
    size_t kW = 32768;
    while (kW > 2048 && kW > n) kW >>= 1;
    p.kW = kW;
    p.half = kW / 2;
    p.binHz = fs / (double)kW;
    p.nWin = std::max<size_t>(1, std::min<size_t>(8, n / kW));
    p.start.assign(p.nWin, 0);
    for (size_t w = 1; w < p.nWin; ++w)
        p.start[w] = (n > kW) ? w * (n - kW) / (p.nWin - 1) : 0;
    return p;
}

// 谱域指标（THD+N / 本底 / 频响）。winMix[w] = 第 w 窗的已混迭单声道原始样本（未加窗）。
inline void spectrum_metrics(const QualityWindowPlan& plan,
                             const std::vector<std::vector<double>>& winMix,
                             QualityResult& out) {
    const size_t kW = plan.kW, half = plan.half, nWin = plan.nWin;
    if (kW < 1024 || nWin == 0 || winMix.empty()) return;

    double w2 = 0.0;
    for (size_t i = 0; i < kW; ++i) w2 += qdetail::bh4(i, kW) * qdetail::bh4(i, kW);
    const double meanW2 = w2 / (double)kW;

    std::vector<std::vector<double>> wins;
    wins.reserve(nWin);
    std::vector<double> avgPower(half + 1, 0.0);
    std::vector<rta::dsp::cplx> buf(kW);
    for (size_t w = 0; w < nWin; ++w) {
        const std::vector<double>& src = winMix[w];
        for (size_t i = 0; i < kW; ++i) buf[i] = rta::dsp::cplx(src[i] * qdetail::bh4(i, kW), 0.0);
        rta::dsp::fft(buf, false);
        std::vector<double> P(half + 1, 0.0);
        for (size_t k = 0; k <= half; ++k) { const double m = std::abs(buf[k]); P[k] = m * m; }
        for (size_t k = 0; k <= half; ++k) avgPower[k] += P[k];
        wins.push_back(std::move(P));
    }
    for (size_t k = 0; k <= half; ++k) avgPower[k] /= (double)nWin;

    const double totalAll = qdetail::full_power_sum(avgPower, 0, half);
    if (!(totalAll > 0.0)) return;

    // ---- 谱峰检测：一次给出「多峰掩码（≤16 峰）」与「单基频掩码（只挖最强峰）」----
    // 峰位从【平均谱】上取一次（逐窗各取会让不同窗的挖孔位置漂移，指标就不可复现）。
    const size_t kNotch = 10;                     // 挖孔半宽（BH4 主瓣 ±4 bin，留足裙边）
    std::vector<char> notchAll(half + 1, 0), notchFund(half + 1, 0);
    size_t kFund = 0, nPeak = 0;
    double fundRatio = 0.0;
    {
        double pmax = 0.0;
        for (size_t k = 1; k < half; ++k) pmax = std::max(pmax, avgPower[k]);
        struct Peak { size_t k; double p; };
        std::vector<Peak> cand;
        if (pmax > 0.0) {
            for (size_t k = 1; k < half; ++k) {
                if (avgPower[k] < avgPower[k - 1] || avgPower[k] < avgPower[k + 1]) continue;
                if (avgPower[k] < pmax * 1e-6) continue;               // 低于 −60 dB 不看
                cand.push_back(Peak{ k, avgPower[k] });
            }
            std::sort(cand.begin(), cand.end(), [](const Peak& a, const Peak& b) { return a.p > b.p; });
            for (const Peak& pk : cand) {
                if (nPeak >= 16) break;
                bool clash = false;
                const size_t lo = pk.k > kNotch ? pk.k - kNotch : 0;
                const size_t hi = std::min(half, pk.k + kNotch);
                for (size_t k = lo; k <= hi; ++k) if (notchAll[k]) { clash = true; break; }
                if (clash) continue;
                for (size_t k = lo; k <= hi; ++k) notchAll[k] = 1;
                nPeak = nPeak + 1;
            }
            if (!cand.empty()) {
                kFund = cand[0].k;
                const size_t lo = kFund > kNotch ? kFund - kNotch : 0;
                const size_t hi = std::min(half, kFund + kNotch);
                for (size_t k = lo; k <= hi; ++k) notchFund[k] = 1;
                // ★ 必须与 totalAll 同口径（单边功率和，中间 bin ×2）：直接对 21 个 bin 求和
                //   而不折半，得到的比值恰好少一半（实测 fundRatio=0.5000 的坑）。
                std::vector<double> fundBand(half + 1, 0.0);
                for (size_t k = lo; k <= hi; ++k) fundBand[k] = avgPower[k];
                const double fe = qdetail::full_power_sum(fundBand, 0, half);
                fundRatio = (totalAll > 0.0) ? fe / totalAll : 0.0;
            }
        }
    }
    // 单基频判据 0.90：测试音/单一主音素材的基频占全带能量 >0.99；真实音乐里"最响的那个音"
    // 通常只占 0.5~0.7（其余是别的乐音/噪声），用 0.5 会把音乐误判成"有基频"，
    // 于是给出一堆毫无意义的 THD+N（实测某电子曲报 −2.43 dB）。
    const bool singleTone = (fundRatio >= 0.90);


    // ---- THD+N（单基频）/ 非谐波残留（多峰），均取窗间 dB 中位数 ----
    std::vector<double> thdnList, nonTonalList;
    for (const auto& P : wins) {
        // ★ 分子分母必须同口径：这里都用「单边 bin 直和」（不折半）。用 full_power_sum（中间
        //   bin ×2）当分母、残差用直和，会恰好差 3.01 dB —— 实测踩过（−40 → −43.01）。
        double tot = 0.0;
        for (size_t k = 0; k <= half; ++k) tot += P[k];
        if (!(tot > 0.0)) continue;
        double rf = 0.0, rm = 0.0;
        for (size_t k = 0; k <= half; ++k) {
            if (!notchFund[k]) rf += P[k];      // 只扣基频 ⟹ 谐波+噪声全留在残差里
            if (!notchAll[k])  rm += P[k];
        }
        thdnList.push_back(10.0 * std::log10(std::max(rf, 1e-300) / tot));
        nonTonalList.push_back(10.0 * std::log10(std::max(rm, 1e-300) / tot));
    }
    auto median_of = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    out.thdnAvailable = singleTone && !thdnList.empty();
    out.thdnMethod = singleTone ? "single_tone" : "multi_peak";
    out.thdnDb = singleTone ? median_of(thdnList) : 0.0;
    out.thdnPct = singleTone ? std::pow(10.0, out.thdnDb / 20.0) * 100.0 : 0.0;
    out.nonTonalDb = median_of(nonTonalList);

    // ---- 本底噪声：逐 bin 取各窗最小值 × 窗数（指数分布最小值的偏置纠正）----
    // 两个必须排除的东西（都是实测踩出来的）：
    //  ① 谱峰（基频/主音）所在的 bin：那里「各窗最小值」等于基频本身，不排掉 SNR 会被基频支配
    //     （实测正弦+噪声时 SNR 被压到 0 dB）；
    //  ② 静音窗：有 8% 静音段的曲子里只要有一个窗落进静音，所有 bin 的最小值都变成 0，
    //     本底噪声立刻变成 −200 dBFS、SNR 变成 140 dB（实测 3 首 FLAC 就是这样被污染的）。
    //     判据：窗电平低于「最响窗 −60 dB」或低于 −70 dBFS 视为静音窗，不参与最小值统计。
    const double winScale = 1.0 / ((double)kW * (double)kW * meanW2);
    std::vector<double> winPow(wins.size(), 0.0);
    double maxWin = 0.0;
    for (size_t w = 0; w < wins.size(); ++w) {
        winPow[w] = qdetail::full_power_sum(wins[w], 0, half) * winScale;
        maxWin = std::max(maxWin, winPow[w]);
    }
    const double gateFloor = std::max(1e-7, maxWin * 1e-6);   // −70 dBFS 与「最响窗 −60 dB」取高者
    std::vector<char> activeWin(wins.size(), 0);
    size_t nActive = 0;
    for (size_t w = 0; w < wins.size(); ++w)
        if (winPow[w] > gateFloor) { activeWin[w] = 1; ++nActive; }
    if (nActive == 0) { for (size_t w = 0; w < wins.size(); ++w) { activeWin[w] = 1; ++nActive; } }

    std::vector<double> noiseP(half + 1, 0.0);
    for (size_t k = 0; k <= half; ++k) {
        double mn = 0.0; bool first = true;
        for (size_t w = 0; w < wins.size(); ++w) {
            if (!activeWin[w]) continue;
            if (first) { mn = wins[w][k]; first = false; }
            else mn = std::min(mn, wins[w][k]);
        }
        noiseP[k] = notchAll[k] ? 0.0 : mn;
    }
    const double nScale = (double)nActive * winScale;
    const double noisePow = qdetail::full_power_sum(noiseP, 0, half) * nScale;
    double sigPow = 0.0;
    for (size_t w = 0; w < wins.size(); ++w)
        if (activeWin[w]) sigPow += qdetail::full_power_sum(wins[w], 0, half);
    sigPow = sigPow / (double)nActive * winScale;
    out.spectralMinDbfs = 10.0 * std::log10(std::max(noisePow, 1e-30));
    if (out.spectralMinDbfs < -200.0) out.spectralMinDbfs = -200.0;
    (void)sigPow;   // 全轨 RMS 由时域统计给出（块电平口径），谱域不再另算一套

    // ---- 频响：重心 / 倾斜 / 95% 滚降 / 10 kHz 以上占比 ----
    const double binHz = plan.binHz;
    double num = 0.0, den = 0.0;
    for (size_t k = 1; k <= half; ++k) {
        const double f = (double)k * binHz;
        num += f * avgPower[k];
        den += avgPower[k];
    }
    out.centroidHz = (den > 0.0) ? num / den : 0.0;

    // 最小二乘斜率：y = 10log10(P)，x = log2(f)，限 [100 Hz, 10 kHz]
    {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, m = 0.0;
        const size_t kLo = std::max<size_t>(1, (size_t)std::ceil(100.0 / binHz));
        const size_t kHi = std::min(half, (size_t)std::floor(10000.0 / binHz));
        for (size_t k = kLo; k <= kHi; ++k) {
            const double f = (double)k * binHz;
            if (avgPower[k] <= 0.0) continue;
            const double x = std::log2(f), y = 10.0 * std::log10(avgPower[k]);
            sx += x; sy += y; sxx += x * x; sxy += x * y; m += 1.0;
        }
        const double den2 = m * sxx - sx * sx;
        out.tiltDbPerOct = (m >= 8.0 && std::fabs(den2) > 1e-12) ? (m * sxy - sx * sy) / den2 : 0.0;
    }

    // 95% 谱滚降点（累积能量达到 95% 的频率）
    {
        double cum = 0.0;
        double target = 0.95 * den;
        out.rolloff95Hz = 0.0;
        for (size_t k = 1; k <= half; ++k) {
            cum += avgPower[k];
            if (cum >= target) { out.rolloff95Hz = (double)k * binHz; break; }
        }
    }

    // 10 kHz 以上能量占比
    {
        double hf = 0.0;
        const size_t k10 = (size_t)std::ceil(10000.0 / binHz);
        for (size_t k = std::min(k10, half); k <= half; ++k) hf += avgPower[k];
        out.hfRatio10kDb = 10.0 * std::log10(std::max(hf, 1e-300) / den);
        if (out.hfRatio10kDb < -120.0) out.hfRatio10kDb = -120.0;
    }
}

} // namespace qdetail

// ==================== 增量（流式）音质分析器 ====================
class QualityAccumulator {
public:
    // active: 每声道 1/0（nullptr = 全有效）。totalFrames 必须已知（谱窗位置取决于它）。
    void begin(size_t totalFrames, int channels, double fs, const char* active = nullptr) {
        totalFrames_ = totalFrames;
        channels_ = channels > 0 ? channels : 0;
        fs_ = fs;
        pos_ = 0;
        bad_ = (totalFrames == 0 || channels_ <= 0 || fs <= 0.0);
        active_.assign((size_t)channels_, 0);
        nActive_ = 0;
        for (int c = 0; c < channels_; ++c) {
            active_[(size_t)c] = (active ? (active[c] ? 1 : 0) : 1);
            if (active_[(size_t)c]) ++nActive_;
        }
        peak_.assign((size_t)channels_, 0.0);
        ssum_.assign((size_t)channels_, 0.0);
        clipCnt_.assign((size_t)channels_, 0);
        run_.assign((size_t)channels_, 0);
        runCount_ = 0;
        longestRun_ = 0;
        sLR_ = sL_ = sR_ = sM_ = sS_ = 0.0;

        blkLen_ = (fs > 0.0) ? (size_t)std::llround(0.1 * fs) : 0;
        dynLen_ = (fs > 0.0) ? (size_t)std::llround(3.0 * fs) : 0;
        blkAcc_ = dynAcc_ = 0.0;
        blkCnt_ = dynCnt_ = 0;
        blkP_.clear(); dynP_.clear();

        envHop_ = (fs > 0.0) ? (size_t)std::max(1.0, std::floor(fs / 200.0)) : 1;
        envHz_ = fs / (double)envHop_;
        envAcc_ = 0.0; envCnt_ = 0;
        env_.clear();

        plan_ = qdetail::QualityWindowPlan();
        plan_.start.clear();
        winMix_.clear();
        mixBuf_.clear();
        if (bad_) return;
        plan_ = qdetail::make_quality_plan(totalFrames, fs_);
        winMix_.resize(plan_.nWin);
        for (size_t w = 0; w < plan_.nWin; ++w) winMix_[w].assign(plan_.kW, 0.0);
        if (envHop_ > 0 && totalFrames > 0) env_.reserve(totalFrames / envHop_ + 1);
    }

    // 平面 double。ch[c] 指向【本块第 0 帧】，块内相对寻址 ch[c][0..frames-1]。
    void add_planar(const double* const* ch, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!ch) { pos_ += frames; return; }
        for (int c = 0; c < channels_; ++c) if (!ch[c] && active_[(size_t)c]) { active_[(size_t)c] = 0; --nActive_; }
        struct G { const double* const* p; double operator()(size_t c, size_t i) const { return p[c][i]; } };
        const G g{ ch };
        push_block(g, frames);
    }
    void add_interleaved(const double* p, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!p) { pos_ += frames; return; }
        struct G { const double* p; size_t nch; double operator()(size_t c, size_t i) const { return p[i * nch + c]; } };
        const G g{ p, (size_t)channels_ };
        push_block(g, frames);
    }
    void add_interleaved_f32(const float* p, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!p) { pos_ += frames; return; }
        struct G { const float* p; size_t nch; double operator()(size_t c, size_t i) const { return (double)p[i * nch + c]; } };
        const G g{ p, (size_t)channels_ };
        push_block(g, frames);
    }

    QualityResult finalize(QualityTimings* tm = nullptr) const {
        const auto tStart = std::chrono::steady_clock::now();
        QualityResult r;
        if (bad_ || pos_ == 0) { r.note = "样本不足，音质分析不可用"; return r; }
        r.channels = channels_;
        r.fs = fs_;
        r.frames = pos_;

        // ---- 峰值 / RMS / 峰值因子 ----
        double peak = 0.0, sumsq = 0.0;
        for (int c = 0; c < channels_; ++c) {
            if (!active_[(size_t)c]) continue;
            peak = std::max(peak, peak_[(size_t)c]);
            sumsq += ssum_[(size_t)c];
        }
        const double rms = (nActive_ > 0) ? std::sqrt(sumsq / ((double)nActive_ * (double)pos_)) : 0.0;
        r.crestDb = (peak > 0.0 && rms > 0.0) ? 20.0 * std::log10(peak / rms) : 0.0;
        r.peakDbfs = (peak > 0.0) ? 20.0 * std::log10(peak) : -300.0;
        r.rmsDbfs = (rms > 0.0) ? 20.0 * std::log10(rms) : -300.0;

        // ---- 削波 ----
        size_t cnt = 0, runs = 0, longest = 0;
        for (int c = 0; c < channels_; ++c) {
            cnt += clipCnt_[(size_t)c];
            // 文件结束时仍挂着的连续段也要结算
            const size_t openRun = run_[(size_t)c];
            longest = std::max(longest, openRun);
        }
        runs = runCount_;
        longest = std::max(longest, longestRun_);
        const double denom = (double)pos_ * (double)std::max(1, nActive_);
        r.clippedSamples = cnt;
        r.clippedPct = (denom > 0.0) ? 100.0 * (double)cnt / denom : 0.0;
        r.clippedRuns = runs;
        r.longestClipRun = longest;

        // ---- 立体声 ----
        // ★ 只对【2 声道】素材计算。>2 声道时第 0/1 声道不一定是 L/R（实测某 6 声道 FLAC
        //   前两声道相关 −0.664，看起来"反相"，其实只是声道布局不同）⟹ 不猜，标注不适用。
        if (channels_ > 2) r.note = (r.note.empty() ? "" : r.note + "；") +
            std::string("多声道（") + std::to_string(channels_) + " ch）素材：立体声指标不适用（不猜声道布局）";
        if (channels_ == 2 && active_[0] && active_[1]) {
            r.stereo = true;
            const double denr = std::sqrt(sL_ * sR_);
            r.channelCorr = (denr > 0.0) ? sLR_ / denr : 0.0;
            r.channelCorr = std::clamp(r.channelCorr, -1.0, 1.0);
            const double stereoPow = 0.5 * (sL_ + sR_) / (double)pos_;
            const double monoPow = sM_ / (double)pos_;
            r.monoLossDb = (monoPow > 0.0) ? 10.0 * std::log10(std::max(stereoPow, 1e-30) / monoPow) : 120.0;
            r.monoLossDb = std::clamp(r.monoLossDb, 0.0, 120.0);
            r.sideMidDb = (monoPow > 0.0)
                ? 10.0 * std::log10(std::max(sS_ / (double)pos_, 1e-30) / monoPow) : -120.0;
            r.sideMidDb = std::max(r.sideMidDb, -120.0);
        }

        // ---- 100 ms 块电平 ----
        if (!blkP_.empty()) {
            std::vector<double> lv;
            lv.reserve(blkP_.size());
            size_t silent = 0;
            for (double p : blkP_) {
                const double db = 10.0 * std::log10(std::max(p, 1e-30));
                lv.push_back(db);
                if (db < -60.0) ++silent;
            }
            std::vector<double> asc = lv;
            std::sort(asc.begin(), asc.end());
            r.quietP10Dbfs = percentile_sorted(asc, 0.10);
            r.silencePct = 100.0 * (double)silent / (double)blkP_.size();
            // 本底噪声 = 最低 10% 的 100 ms 块电平；SNR = 全轨 RMS − 本底
            r.noiseFloorDbfs = r.quietP10Dbfs;
            r.snrDb = std::clamp(r.rmsDbfs - r.noiseFloorDbfs, 0.0, 140.0);
        }

        // ---- 3 s 块动态 ----
        if (!dynP_.empty()) {
            std::vector<double> lv;
            lv.reserve(dynP_.size());
            for (double p : dynP_) lv.push_back(10.0 * std::log10(std::max(p, 1e-30)));
            std::vector<double> asc = lv;
            std::sort(asc.begin(), asc.end());
            if (asc.size() >= 2) r.dynP95P10Db = percentile_sorted(asc, 0.95) - percentile_sorted(asc, 0.10);
            double we = 0.0, wl = 0.0;
            for (size_t i = 0; i < dynP_.size(); ++i) { we += dynP_[i]; wl += dynP_[i] * lv[i]; }
            r.dynCentroidDbfs = (we > 0.0) ? wl / we : 0.0;
        }

        if (tm) tm->timeDomainMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();

        // ---- 谱域 ----
        const auto tSpec = std::chrono::steady_clock::now();
        r.spectralWindows = plan_.nWin;
        if (!winMix_.empty() && plan_.kW >= 1024) qdetail::spectrum_metrics(plan_, winMix_, r);
        r.imdAvailable = false;
        r.imdNote = "素材非双音，互调失真不适用（需 60 Hz + 7 kHz 之类的双音测试信号；"
                    "对双音素材请调用 measure_imd_smpte）";
        if (blkP_.empty()) {   // 短于 100 ms：块统计不存在，回退用谱最小统计
            r.noiseFloorDbfs = r.spectralMinDbfs;
            r.snrDb = std::clamp(r.rmsDbfs - r.noiseFloorDbfs, 0.0, 140.0);
        }
        r.valid = (r.frames > 0);
        if (r.frames < plan_.kW) r.note = "素材短于一个谱窗，谱类指标按单窗估计";
        // 口径提醒（不是错误）：谱最小统计对动态音乐是【下界】，绝对值偏低；同一首的相对比较不受影响
        if (!r.note.empty()) r.note += "；";
        r.note += "本底噪声为谱最小统计（动态音乐为下界，同一首相对比较不受影响）";
        if (tm) tm->spectralMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tSpec).count();
        return r;
    }

    bool   planMatched() const { return !bad_ && pos_ == totalFrames_; }
    bool   bad() const { return bad_; }
    size_t framesSeen() const { return pos_; }
    size_t windowCount() const { return plan_.nWin; }
    size_t plannedFrames() const { return totalFrames_; }
    // 对齐包络（200 Hz 单声道 RMS；version_compare.h 用它做粗对齐）
    const std::vector<float>& envelope() const { return env_; }
    double envelopeHz() const { return envHz_; }
    // 谱窗缓冲字节数（内存报告用）
    size_t windowBytes() const { return plan_.nWin * plan_.kW * sizeof(double); }

private:
    template <class Get>
    void push_block(const Get& get, size_t frames) {
        // 谱窗重叠区间（只有落入谱窗的帧才做混迭）
        for (size_t w = 0; w < plan_.nWin; ++w) {
            const size_t ws = plan_.start[w], we = ws + plan_.kW;
            const size_t begin = pos_, end = begin + frames;
            if (we <= begin || ws >= end) continue;
            const size_t lo = std::max(begin, ws), hi = std::min(end, we);
            double* dst = winMix_[w].data();
            for (size_t g = lo; g < hi; ++g) {
                double s = 0.0;
                for (int c = 0; c < channels_; ++c) if (active_[(size_t)c]) s += get((size_t)c, g - begin);
                dst[g - ws] = (nActive_ > 0) ? s / (double)nActive_ : 0.0;
            }
        }
        for (size_t i = 0; i < frames; ++i) {
            double sum = 0.0;
            for (int c = 0; c < channels_; ++c) {
                if (!active_[(size_t)c]) continue;
                const double x = get((size_t)c, i);
                sum += x;
                const double a = std::fabs(x);
                if (a > peak_[(size_t)c]) peak_[(size_t)c] = a;
                ssum_[(size_t)c] += x * x;
                if (a >= kClipThreshold) {
                    ++clipCnt_[(size_t)c];
                    if (++run_[(size_t)c] > longestRun_) longestRun_ = run_[(size_t)c];
                } else {
                    if (run_[(size_t)c] >= kClipRunMin) ++runCount_;
                    run_[(size_t)c] = 0;
                }
            }
            if (channels_ >= 2 && active_[0] && active_[1]) {
                const double l = get(0, i), rr = get(1, i);
                const double m = 0.5 * (l + rr), s = 0.5 * (l - rr);
                sL_ += l * l; sR_ += rr * rr; sLR_ += l * rr;
                sM_ += m * m; sS_ += s * s;
            }
            const double np = (double)std::max(1, nActive_);
            const double mean = sum / np;
            const double p = mean * mean;   // 单声道化后的瞬时功率
            blkAcc_ += p; dynAcc_ += p; envAcc_ += p;
            if (blkLen_ > 0 && ++blkCnt_ == blkLen_) {
                blkP_.push_back(blkAcc_ / (double)blkLen_);
                blkAcc_ = 0.0; blkCnt_ = 0;
            }
            if (dynLen_ > 0 && ++dynCnt_ == dynLen_) {
                dynP_.push_back(dynAcc_ / (double)dynLen_);
                dynAcc_ = 0.0; dynCnt_ = 0;
            }
            if (envHop_ > 0 && ++envCnt_ == envHop_) {
                env_.push_back((float)std::sqrt(envAcc_ / (double)envHop_));
                envAcc_ = 0.0; envCnt_ = 0;
            }
        }
        pos_ += frames;
    }

    size_t totalFrames_ = 0;
    int    channels_ = 0, nActive_ = 0;
    double fs_ = 0.0;
    bool   bad_ = true;
    size_t pos_ = 0;
    std::vector<char> active_;

    std::vector<double> peak_, ssum_;
    std::vector<size_t> clipCnt_, run_;
    size_t runCount_ = 0, longestRun_ = 0;
    double sLR_ = 0.0, sL_ = 0.0, sR_ = 0.0, sM_ = 0.0, sS_ = 0.0;

    size_t blkLen_ = 0, dynLen_ = 0;
    double blkAcc_ = 0.0, dynAcc_ = 0.0;
    size_t blkCnt_ = 0, dynCnt_ = 0;
    std::vector<double> blkP_, dynP_;

    size_t envHop_ = 1;
    double envHz_ = 200.0;
    double envAcc_ = 0.0;
    size_t envCnt_ = 0;
    std::vector<float> env_;

    qdetail::QualityWindowPlan plan_;
    std::vector<std::vector<double>> winMix_;
    std::vector<double> mixBuf_;
};

// 一次性接口：整段平面 double（实现上就是「建计划 + 整段喂 + finalize」，与增量逐位一致）
inline QualityResult measure_quality(const double* const* ch, int channels, size_t n, double fs) {
    QualityResult r;
    if (!ch || channels <= 0 || n == 0 || fs <= 0.0) { r.note = "样本不足，音质分析不可用"; return r; }
    std::vector<char> active((size_t)channels, 0);
    for (int c = 0; c < channels; ++c) active[(size_t)c] = ch[c] ? 1 : 0;
    QualityAccumulator acc;
    acc.begin(n, channels, fs, active.data());
    acc.add_planar(ch, n);
    return acc.finalize();
}

} // namespace rta::analysis
