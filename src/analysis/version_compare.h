// version_compare.h — 音频体检 B3：同一首曲子的多版本音质对比（N 个文件）
//
// 用户目标：「对比同一首音乐，不同版本之间的音质高低」。注意这是两件事：
//   1) 对齐：不同版本的编码/母带往往前导静音不同、时长不同，不对齐就没法逐项比；
//   2) 排序：必须能说出「为什么 A 比 B 好」，而不是给个黑箱数字。
//
// ================== 对齐（不猜，用互相关） ==================
//  * 包络：每文件按 200 Hz 取单声道 RMS 包络（5 ms 一点），与版本无关、与编码无关；
//  * 粗对齐：FFT 互相关（不是 O(n²) 直接算 —— 5 分钟曲子 6 万个点直接算要 3.6e9 次乘加），
//    取归一化互相关峰的位置做包络级延迟；包络粒度 5 ms。
//  * 相似度门限：对齐后归一化包络相关 < 0.55 ⟹ 判为「不是同一首」，全部对比作废
//    （不同母带同一首歌实测 0.8~0.99；两首不同的曲子 0.1~0.3，余量很大）。
//  * 公共区段：把各版本按其延迟换算成时间轴，取所有版本都覆盖的那一段，
//    然后【只在这一段上重算全部指标】—— 否则一个版本多 10 秒前导静音就会污染
//    本底噪声、静音占比、动态重心等所有统计量。
//
// ================== 不可比项（必须显式标注，不许硬比） ==================
//  * 位深不同（如 16-bit vs 24-bit）⟹ 本底噪声 / SNR 不可比（受容器量化下限影响）；
//  * 采样率不同（|Δ| > 20%）⟹ 谱重心 / 95% 滚降点 / >10 kHz 占比不可比（奈奎斯特不同）；
//  * 声道数不同（单声道 vs 立体声）⟹ 声道相关 / 单声道损失 / side-mid 不可比。
//  不可比的行照常列出数值，但 comparable=false + 中文原因，且不参与综合评分
//  （评分权重在这些行上重新归一化，见 score_versions）。
//
// ================== 综合排序的权重与理由 ==================
//   动态范围 DR        0.25  母带压得狠就不可逆损失微动态，是最核心的判据
//   削波 clipping      0.20  波形被削掉是不可逆破坏，比"听起来差一点"严重
//   信噪比 SNR         0.20  本底噪声抬升（有损转制/不良转录）同样不可逆
//   峰值因子 crest     0.10  与 DR 同向但口径不同（全局 vs 3 s 块），交叉验证
//   LRA                0.10  短时动态分布，同上
//   非谐波残留         0.10  16 峰之外的能量（编码噪声/失真上界），覆盖面比 SNR 宽
//   单声道兼容 mono    0.05  只有相位反转这种病态素材会爆表，平时差异极小，故最低
//  合计 1.00。
//  ★ 打分方式：不用"绝对锚点"，用【相对最好版本的扣分】——
//     score = 100 · (1 − Σ wᵢ·penaltyᵢ / Σ wᵢ)，penaltyᵢ = min(1, |valᵢ − bestᵢ| / 满罚门限ᵢ)
//     满罚门限（"差到这个程度就算全错"）：DR 6 dB、SNR 20 dB、峰值因子 4 dB、LRA 6 LU、
//     非谐波残留 10 dB、单声道损失 3 dB、削波（占比 0.5% 或 2000 段，取大者）。
//   为什么不用绝对锚点：实测 3 个合成版本里 V1/V2 的非谐波残留是 −68.5/−59.0 dB，
//   都低于任何"绝对好"的锚点上限，于是两者同分、排序退化成随机。相对扣分没有饱和区，
//   且解释能直接说"比最好的版本低 9.5 dB，达到满罚门限的 95%"。
//   代价：分数只在【这一批版本内】有意义，跨批次不可比 —— 这一点写进解释里。
#pragma once
#include "quality.h"
#include "dynamics.h"
#include "health_stream.h"
#include "../audio/audio_loader.h"
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>

namespace rta::analysis {

// ---------------- PCM 源：优先分块流式（DSD/WAV），否则整段解码 ----------------
class PcmFile {
public:
    bool open(const std::string& path, std::string* why = nullptr) {
        close();
        std::string streamWhy;
        if (src_.open(path, &streamWhy)) { streamed_ = true; return true; }
        // 回退：整段解码（flac/mp3/…），仍然只读
        try {
            rta::audio::LoadNote note;
            buf_ = rta::audio::read_any(path, &note);
            whole_ = true;
            if (buf_.frames() == 0 || buf_.channels <= 0 || buf_.rate == 0) {
                if (why) *why = "解码后无可用样本";
                close();
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            if (why) *why = std::string("解码失败: ") + e.what();
        } catch (...) {
            if (why) *why = "解码失败（未知异常）";
        }
        return false;
    }
    void close() { src_.close(); buf_ = rta::audio::AudioBuffer(); streamed_ = whole_ = false; }
    bool   streamed() const { return streamed_; }
    uint32_t rate() const { return streamed_ ? src_.rate() : buf_.rate; }
    int    channels() const { return streamed_ ? src_.channels() : buf_.channels; }
    int    bits() const { return streamed_ ? src_.bits() : buf_.bits; }
    bool   isFloat() const { return streamed_ ? src_.isFloat() : buf_.isFloat; }
    size_t totalFrames() const { return streamed_ ? src_.totalFrames() : buf_.frames(); }
    size_t read(size_t pos, size_t n, rta::audio::AudioBuffer& out) {
        if (streamed_) return src_.read(pos, n, out);
        const size_t total = buf_.frames();
        if (pos >= total || n == 0) return 0;
        if (n > total - pos) n = total - pos;
        const size_t chn = (size_t)buf_.channels;
        out.rate = buf_.rate; out.channels = buf_.channels; out.bits = buf_.bits; out.isFloat = buf_.isFloat;
        out.raw.assign(buf_.raw.begin() + (long long)(pos * chn),
                       buf_.raw.begin() + (long long)((pos + n) * chn));
        return n;
    }
private:
    detail::HealthSource src_;
    rta::audio::AudioBuffer buf_;
    bool streamed_ = false, whole_ = false;
};

// ---------------- 结果结构 ----------------
struct CompareOptions {
    size_t blockFrames = 65536;
    // 判同曲门限。实测（本机曲库，200 Hz 包络、重叠段 Pearson）：
    //   同一首不同母带 0.53 / 0.56（真实曲库两对）；不同曲子 0.04 / 0.25 / 0.29。
    // 取 0.45 落在两簇之间；0.45~0.7 属"像同一首但母带/剪辑不同"，解释里会额外提示。
    double minEnvCorr = 0.45;
    bool   verbose = false;       // true 时把对齐/耗时打到 stderr
};

struct MetricRow {
    std::string key, label, unit;
    bool higherBetter = true;
    bool informational = false;   // 无方向性（只展示，不评分）
    bool comparable = true;
    std::string why;              // 不可比时的中文原因
    std::vector<double> values;
    size_t bestIndex = 0;
};

struct VersionData {
    std::string path, label;
    std::string error;
    uint32_t fs = 0;
    int    channels = 0, bits = 0;
    double durationSec = 0.0;
    size_t totalFrames = 0;
    // 对齐
    long long lagSec200 = 0;      // 相对参考版本的内容偏移（1/200 秒，正=该文件内容更早）
    double lagSec = 0.0;
    double envCorr = 1.0;
    long long windowStartFrames = 0;
    size_t windowFrames = 0;
    std::vector<float> env;       // 200 Hz 单声道 RMS 包络（对齐用；不参与评分）
    // 公共区段上的指标
    QualityResult quality;
    DynamicsResult dynamicsTimes;
    bool analyzed = false;
};

struct VersionCompare {
    bool ok = false;
    std::string error;
    bool sameTrack = false;
    double sameTrackCorr = 1.0;
    size_t windowFrames = 0;
    double windowSec = 0.0;
    std::vector<VersionData> versions;
    std::vector<MetricRow> rows;
    std::vector<double> scores;
    std::vector<size_t> ranking;      // 好→差
    std::string explanation;          // 确定性人话（不依赖 AI）
    double enrollMs = 0.0, alignMs = 0.0, analyzeMs = 0.0;
};

namespace vcdetail {

inline size_t next_pow2(size_t v) { size_t p = 1; while (p < v) p <<= 1; return p; }

// 200 Hz 单声道 RMS 包络（只做一遍幂运算，分析器不在这一步跑）
template <class Src>
inline std::vector<float> envelope_of(Src& src, size_t blockFrames, double* envHzOut) {
    const uint32_t fs = src.rate();
    const size_t hop = (size_t)std::max(1.0, std::floor((double)fs / 200.0));
    *envHzOut = (double)fs / (double)hop;
    std::vector<float> env;
    const size_t total = src.totalFrames();
    env.reserve(total / hop + 2);
    rta::audio::AudioBuffer chunk;
    double acc = 0.0;
    size_t cnt = 0;
    size_t pos = 0;
    while (pos < total) {
        const size_t n = std::min(blockFrames ? blockFrames : size_t(65536), total - pos);
        const size_t got = src.read(pos, n, chunk);
        if (got == 0) break;
        const int chn = src.channels();
        const double inv = 1.0 / (double)std::max(1, chn);
        for (size_t i = 0; i < got; ++i) {
            double s = 0.0;
            for (int c = 0; c < chn; ++c) s += chunk.sample(i, c);
            const double m = s * inv;
            acc += m * m;
            if (++cnt == hop) { env.push_back((float)std::sqrt(acc / (double)hop)); acc = 0.0; cnt = 0; }
        }
        pos += got;
        if (got < n) break;
    }
    return env;
}

// 归一化互相关（FFT）：返回最佳延迟 L（1/200 秒），a[n] ≈ b[n−L] 时的 L 与相关系数。
// 实现：r[k] = Σ_n a[n]·b[n−k]（k 可正可负），由 IFFT(A·conj(B)) 一次性给出。
inline void best_lag(const std::vector<float>& a, const std::vector<float>& b,
                     long long* lagOut, double* corrOut) {
    *lagOut = 0; *corrOut = 0.0;
    const size_t na = a.size(), nb = b.size();
    if (na < 8 || nb < 8) return;
    const size_t N = next_pow2(na + nb);
    std::vector<rta::dsp::cplx> A(N, rta::dsp::cplx(0.0, 0.0)), B(N, rta::dsp::cplx(0.0, 0.0));
    double meanA = 0.0, meanB = 0.0;
    for (float v : a) meanA += v;
    for (float v : b) meanB += v;
    meanA /= (double)na; meanB /= (double)nb;
    double ea = 0.0, eb = 0.0;
    for (size_t i = 0; i < na; ++i) { const double v = (double)a[i] - meanA; A[i] = rta::dsp::cplx(v, 0.0); ea += v * v; }
    for (size_t i = 0; i < nb; ++i) { const double v = (double)b[i] - meanB; B[i] = rta::dsp::cplx(v, 0.0); eb += v * v; }
    if (ea <= 0.0 || eb <= 0.0) return;
    rta::dsp::fft(A, false);
    rta::dsp::fft(B, false);
    for (size_t k = 0; k < N; ++k) A[k] *= std::conj(B[k]);
    rta::dsp::fft(A, true);
    const double norm = 1.0 / std::sqrt(ea * eb);
    double best = -2.0; long long bestLag = 0;
    for (size_t k = 0; k < nb; ++k) {          // 正延迟
        const double r = A[k].real() * norm;
        if (r > best) { best = r; bestLag = (long long)k; }
    }
    for (size_t k = 1; k < na; ++k) {          // 负延迟
        const double r = A[N - k].real() * norm;
        if (r > best) { best = r; bestLag = -(long long)k; }
    }
    *lagOut = bestLag;
    *corrOut = best;
}

// 重叠区段上的 Pearson 相关系数（均值/能量都只按重叠部分算）。
// ★ 为什么必须只用重叠段：两版前导静音不同（0.5 s vs 2.0 s），整段均值差 0.08，
//   减掉各自整段均值后 b 残留一个直流偏置，实测把同内容的相关系数从 0.99 压到 0.73。
//   对齐（求 lag）可以容忍这一点，但"是否同一首"的判据必须用无偏的重叠段 Pearson。
inline double overlap_corr(const std::vector<float>& a, const std::vector<float>& b, long long lag) {
    const long long na = (long long)a.size(), nb = (long long)b.size();
    const long long n0 = std::max<long long>(0, lag);
    const long long n1 = std::min<long long>(na, nb + lag);
    if (n1 - n0 < 8) return 0.0;
    double sa = 0.0, sb = 0.0;
    const long long cnt = n1 - n0;
    for (long long n = n0; n < n1; ++n) { sa += a[(size_t)n]; sb += b[(size_t)(n - lag)]; }
    const double ma = sa / (double)cnt, mb = sb / (double)cnt;
    double cov = 0.0, va = 0.0, vb = 0.0;
    for (long long n = n0; n < n1; ++n) {
        const double x = a[(size_t)n] - ma, y = b[(size_t)(n - lag)] - mb;
        cov += x * y; va += x * x; vb += y * y;
    }
    if (va <= 0.0 || vb <= 0.0) return 0.0;
    return cov / std::sqrt(va * vb);
}

// 线性锚点映射到 0..100（低于 lo 记 0，高于 hi 记 100）
inline double map_range(double v, double lo, double hi) {
    if (!(hi > lo)) return 0.0;
    return std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * 100.0;
}

} // namespace vcdetail

// 公共区段分析：把 src 的 [start, start+len) 帧喂给两个增量分析器
template <class Src>
inline bool analyze_window(Src& src, size_t start, size_t len, size_t blockFrames,
                           QualityResult& q, DynamicsResult& d, std::string* why = nullptr) {
    const int chn = src.channels();
    if (chn <= 0 || len == 0) { if (why) *why = "公共区段为空"; return false; }
    QualityAccumulator qacc;
    DynamicsAccumulator dacc;
    qacc.begin(len, chn, (double)src.rate());
    dacc.begin(chn, (double)src.rate());
    rta::audio::AudioBuffer chunk;
    std::vector<double> tmp;
    size_t pos = 0;
    while (pos < len) {
        const size_t n = std::min(blockFrames ? blockFrames : size_t(65536), len - pos);
        const size_t got = src.read(start + pos, n, chunk);
        if (got == 0) break;
        tmp.resize(got * (size_t)chn);
        for (size_t i = 0; i < got; ++i)
            for (int c = 0; c < chn; ++c) tmp[i * (size_t)chn + (size_t)c] = chunk.sample(i, c);
        qacc.add_interleaved(tmp.data(), got);
        dacc.add_interleaved(tmp.data(), got);
        pos += got;
        if (got < n) break;
    }
    q = qacc.finalize();
    d = dacc.finalize();
    if (pos != len) {
        if (why) *why = "公共区段读取不完整（" + std::to_string(pos) + "/" + std::to_string(len) + " 帧）";
        return false;
    }
    return true;
}

// ==================== 主入口：比较 N 个文件 ====================
inline VersionCompare compare_versions(const std::vector<std::string>& paths, const CompareOptions& opt = {}) {
    VersionCompare out;
    auto t0 = std::chrono::steady_clock::now();
    if (paths.size() < 2) { out.error = "至少需要 2 个文件才能对比"; return out; }

    out.versions.resize(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        out.versions[i].path = paths[i];
        out.versions[i].label = "V" + std::to_string(i + 1);
    }

    // ---- 第一遍：包络 + 元信息（内存只留包络，与曲长弱相关：4 B / 5 ms ≈ 0.8 MB/小时）----
    std::vector<std::unique_ptr<PcmFile>> srcs;
    for (size_t i = 0; i < paths.size(); ++i) {
        auto pf = std::make_unique<PcmFile>();
        std::string why;
        if (!pf->open(paths[i], &why)) {
            out.error = "版本 " + out.versions[i].label + " 无法打开：" + why;
            return out;
        }
        VersionData& v = out.versions[i];
        v.fs = pf->rate(); v.channels = pf->channels(); v.bits = pf->bits();
        v.totalFrames = pf->totalFrames();
        v.durationSec = (v.fs > 0) ? (double)v.totalFrames / (double)v.fs : 0.0;
        double envHz = 200.0;
        std::vector<float> env = vcdetail::envelope_of(*pf, opt.blockFrames, &envHz);
        if (env.empty()) { out.error = "版本 " + v.label + " 无可用样本"; return out; }
        // 统一到「以秒为单位」的包络：按本文件 envHz 重采样到公共 200 Hz 网格（避免不同 fs 的偏差）
        const double targetHz = 200.0;
        std::vector<float> renorm;
        renorm.reserve((size_t)((double)env.size() * targetHz / envHz) + 2);
        for (double t = 0.0; t < (double)env.size() / envHz; t += 1.0 / targetHz) {
            const double fpos = t * envHz;
            const size_t i0 = (size_t)fpos;
            const size_t i1 = std::min(env.size() - 1, i0 + 1);
            const double a = fpos - (double)i0;
            renorm.push_back((float)((1.0 - a) * env[i0] + a * env[i1]));
        }
        srcs.push_back(std::move(pf));
        v.env.swap(renorm);
    }
    out.enrollMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto t1 = std::chrono::steady_clock::now();

    // ---- 对齐：全部对参考版本（V1）做互相关 ----
    const std::vector<float>& refEnv = out.versions[0].env;
    double worstCorr = 2.0;
    for (size_t i = 1; i < out.versions.size(); ++i) {
        long long lag = 0; double corr = 0.0;
        vcdetail::best_lag(refEnv, out.versions[i].env, &lag, &corr);
        // r[k] = Σ a[n]·b[n−k] 在 k 处最大 ⟹ 版本 i 的内容比参考【早】k 点，
        // 即「参考时间 0 在该文件里的位置」= −k。符号写反会让公共区段整段错位（实测踩过）。
        out.versions[i].lagSec200 = -lag;
        out.versions[i].lagSec = -(double)lag / 200.0;
        // 判据用重叠段 Pearson（见 vcdetail::overlap_corr 的说明）；FFT 的全局值只用于定位
        out.versions[i].envCorr = vcdetail::overlap_corr(refEnv, out.versions[i].env, lag);
        worstCorr = std::min(worstCorr, out.versions[i].envCorr);
        if (opt.verbose)
            std::fprintf(stderr, "  对齐 %s: 延迟 %+.3f s, 包络相关 %.4f\n",
                         out.versions[i].label.c_str(), out.versions[i].lagSec, corr);
    }
    out.sameTrackCorr = (worstCorr > 1.0) ? 1.0 : worstCorr;
    out.sameTrack = (out.sameTrackCorr >= opt.minEnvCorr);
    if (!out.sameTrack) {
        char cb[64], tb[32];
        std::snprintf(cb, sizeof(cb), "%.3f", out.sameTrackCorr);
        std::snprintf(tb, sizeof(tb), "%.2f", opt.minEnvCorr);
        out.error = std::string("对齐后包络相关只有 ") + cb + "（门限 " + tb +
                    "）：这些文件不像同一首曲子，对比结果没有意义";
        out.ok = false;
        return out;
    }
    out.alignMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    auto t2 = std::chrono::steady_clock::now();

    // ---- 公共区段（时间轴）----
    // 定义：文件 i 的时间 = 参考文件时间 + lag_i  ⟹  参考文件时间 = 文件时间 − lag_i。
    // 于是文件 i 覆盖的参考时间区间 = [−lag_i, dur_i − lag_i]；
    // 文件 i 里对应参考时间 contentStart 的位置 = contentStart + lag_i。
    // ★ 这两处符号写反，窗口就会落进前导静音里（实测 V2 因此测出 −200 dBFS 的假本底）。
    double contentStart = -1e18, contentEnd = 1e18;
    for (size_t i = 0; i < out.versions.size(); ++i) {
        const VersionData& v = out.versions[i];
        contentStart = std::max(contentStart, -v.lagSec);
        contentEnd = std::min(contentEnd, v.durationSec - v.lagSec);
    }
    const double commonSec = contentEnd - contentStart;
    if (!(commonSec > 0.5)) {
        out.error = "各版本没有足够的公共区段（重叠 " + std::to_string(commonSec) + " s）";
        return out;
    }
    // 每文件窗口：帧起点 = (contentStart − lag_i) × fs
    for (size_t i = 0; i < out.versions.size(); ++i) {
        VersionData& v = out.versions[i];
        const double t0s = contentStart + v.lagSec;
        long long startF = (long long)std::llround(t0s * (double)v.fs);
        startF = std::max<long long>(0, startF);
        size_t len = (size_t)std::llround(commonSec * (double)v.fs);
        if ((size_t)startF + len > v.totalFrames) len = v.totalFrames - (size_t)startF;
        v.windowStartFrames = startF;
        v.windowFrames = len;
    }
    // 统一到最短（不同 fs 时四舍五入可能差几帧）：公共时长取各版本窗口时长最小值
    double minWinSec = 1e18;
    for (const VersionData& v : out.versions)
        minWinSec = std::min(minWinSec, (v.fs > 0) ? (double)v.windowFrames / (double)v.fs : 0.0);
    out.windowSec = minWinSec;
    out.windowFrames = (size_t)std::llround(minWinSec * (double)out.versions[0].fs);

    // ---- 第二遍：只在公共区段上重算全部指标 ----
    for (size_t i = 0; i < out.versions.size(); ++i) {
        VersionData& v = out.versions[i];
        const size_t len = (size_t)std::llround(minWinSec * (double)v.fs);
        std::string why;
        if (!analyze_window(*srcs[i], (size_t)v.windowStartFrames, len, opt.blockFrames,
                            v.quality, v.dynamicsTimes, &why)) {
            v.error = why;
        } else {
            v.analyzed = true;
        }
    }
    out.analyzeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
    out.ok = true;
    return out;
}

// ==================== 指标表 + 评分 + 排序 + 解释 ====================
namespace vcdetail {

inline void push_row(VersionCompare& r, const char* key, const char* label, const char* unit,
                     bool higherBetter, bool informational,
                     std::function<double(const VersionData&)> get,
                     const std::string& why = std::string()) {
    MetricRow row;
    row.key = key; row.label = label; row.unit = unit;
    row.higherBetter = higherBetter; row.informational = informational;
    row.comparable = why.empty();
    row.why = why;
    for (const VersionData& v : r.versions) row.values.push_back(get(v));
    // 最佳：方向性指标取极值；信息性指标不标最佳
    row.bestIndex = 0;
    if (!informational && row.comparable) {
        for (size_t i = 1; i < row.values.size(); ++i) {
            const bool better = higherBetter ? (row.values[i] > row.values[row.bestIndex])
                                             : (row.values[i] < row.values[row.bestIndex]);
            if (better) row.bestIndex = i;
        }
    }
    r.rows.push_back(std::move(row));
}

// 单个维度的 0..100 分（锚点写在这里，解释时可直接引用）
inline double dyn_score(const VersionData& v) { return map_range(v.dynamicsTimes.drDb, 3.0, 20.0); }
inline double snr_score(const VersionData& v) { return map_range(v.quality.snrDb, 20.0, 80.0); }
inline double crest_score(const VersionData& v) { return map_range(v.quality.crestDb, 6.0, 20.0); }
inline double lra_score(const VersionData& v) { return map_range(v.dynamicsTimes.lra, 3.0, 15.0); }
inline double nonTonal_score(const VersionData& v) { return map_range(-v.quality.nonTonalDb, 0.0, 30.0); }
inline double mono_score(const VersionData& v) {
    if (!v.quality.stereo) return 100.0;          // 单声道素材没有兼容性问题
    return std::clamp(100.0 - v.quality.monoLossDb * 20.0, 0.0, 100.0);
}
// 削波：占比与连续段都罚，但【连续段罚得更重】—— 单个越限样本多为限幅器/抖动残留，
// 几乎不可闻；连续 ≥3 样本的平台才是真削波（听感是"糊/破"）。
//   占比项 20·log10(1+pct/0.01)：0.02% → 0.2 dB，1% → 6 dB，10% → 21 dB
//   段数项 12·log10(1+runs)：10 段 → 12.5 dB，1000 段 → 36 dB，10000 段 → 48 dB
inline double clip_score(const VersionData& v) {
    const double pct = v.quality.clippedPct;
    const double runs = (double)v.quality.clippedRuns;
    if (pct <= 0.0 && runs <= 0.0) return 100.0;
    const double penalty = 20.0 * std::log10(1.0 + pct / 0.01) + 12.0 * std::log10(1.0 + runs);
    return std::clamp(100.0 - penalty, 0.0, 100.0);
}

} // namespace vcdetail

// 填 rows / scores / ranking / explanation。可在 compare_versions 之后单独调用（便于测试）。
inline void score_versions(VersionCompare& r) {
    if (!r.ok) return;
    const size_t n = r.versions.size();

    // ---- 可比性判定（先算，再建行）----
    bool bitsSame = true, fsSame = true, chSame = true;
    for (size_t i = 1; i < n; ++i) {
        if (r.versions[i].bits != r.versions[0].bits) bitsSame = false;
        if (r.versions[i].channels != r.versions[0].channels) chSame = false;
        const double a = (double)r.versions[0].fs, b = (double)r.versions[i].fs;
        if (a > 0 && b > 0 && std::fabs(a - b) / std::max(a, b) > 0.2) fsSame = false;
    }
    const std::string whyBits = bitsSame ? std::string()
        : "版本间位深不同（" + std::to_string(r.versions[0].bits) + "-bit vs 其它）：本底噪声/SNR 受容器量化下限影响，不可直接比";
    const std::string whyFs = fsSame ? std::string()
        : "版本间采样率不同：奈奎斯特不同，频响类指标不在同一带宽内，不可直接比";
    std::string whyCh = chSame ? std::string()
        : "版本间声道数不同（单声道 vs 立体声）：重合度/单声道兼容性不可比";
    if (whyCh.empty()) {
        for (const VersionData& v : r.versions)
            if (!v.quality.stereo) { whyCh = "存在非立体声（单声道或多声道）版本：立体声指标不适用"; break; }
    }

    r.rows.clear();
    using VD = const VersionData&;
    vcdetail::push_row(r, "dr_db", "动态范围 TT DR", "dB", true, false,
                       [](VD v) { return v.dynamicsTimes.drDb; });
    vcdetail::push_row(r, "lra", "短时动态 LRA", "LU", true, false,
                       [](VD v) { return v.dynamicsTimes.lra; });
    vcdetail::push_row(r, "crest_db", "峰值因子", "dB", true, false,
                       [](VD v) { return v.quality.crestDb; });
    vcdetail::push_row(r, "snr_db", "信噪比（全轨RMS−本底）", "dB", true, false,
                       [](VD v) { return v.quality.snrDb; }, whyBits);
    vcdetail::push_row(r, "noise_floor_dbfs", "本底噪声（最低 10% 块电平）", "dBFS", false, false,
                       [](VD v) { return v.quality.noiseFloorDbfs; }, whyBits);
    vcdetail::push_row(r, "non_tonal_db", "非谐波残留（噪声+失真上界）", "dB", false, false,
                       [](VD v) { return v.quality.nonTonalDb; });
    vcdetail::push_row(r, "thdn_db", "THD+N（单基频素材才有）", "dB", false, false,
                       [](VD v) { return v.quality.thdnAvailable ? v.quality.thdnDb : -300.0; });
    vcdetail::push_row(r, "clipped_pct", "削波样本占比", "%", false, false,
                       [](VD v) { return v.quality.clippedPct; });
    vcdetail::push_row(r, "clipped_runs", "连续削波段数（≥3 样本）", "段", false, false,
                       [](VD v) { return (double)v.quality.clippedRuns; });
    vcdetail::push_row(r, "centroid_hz", "谱重心（brightness）", "Hz", true, true,
                       [](VD v) { return v.quality.centroidHz; }, whyFs);
    vcdetail::push_row(r, "tilt_db_oct", "频谱倾斜", "dB/oct", true, true,
                       [](VD v) { return v.quality.tiltDbPerOct; }, whyFs);
    vcdetail::push_row(r, "rolloff95_hz", "95% 谱滚降点", "Hz", true, true,
                       [](VD v) { return v.quality.rolloff95Hz; }, whyFs);
    vcdetail::push_row(r, "hf_ratio_10k_db", ">10 kHz 能量占比", "dB", true, false,
                       [](VD v) { return v.quality.hfRatio10kDb; }, whyFs);
    vcdetail::push_row(r, "channel_corr", "声道间相关", "", true, true,
                       [](VD v) { return v.quality.stereo ? v.quality.channelCorr : 0.0; }, whyCh);
    vcdetail::push_row(r, "mono_loss_db", "单声道化能量损失", "dB", false, false,
                       [](VD v) { return v.quality.stereo ? v.quality.monoLossDb : 0.0; }, whyCh);
    vcdetail::push_row(r, "dyn_p95_p10_db", "3 s 块 RMS 的 P95−P10", "dB", true, false,
                       [](VD v) { return v.quality.dynP95P10Db; });
    vcdetail::push_row(r, "dyn_centroid_dbfs", "动态重心（能量加权）", "dBFS", true, true,
                       [](VD v) { return v.quality.dynCentroidDbfs; });
    vcdetail::push_row(r, "peak_dbfs", "样本峰值", "dBFS", true, true,
                       [](VD v) { return v.quality.peakDbfs; });
    vcdetail::push_row(r, "lufs", "积分响度", "LUFS", true, true,
                       [](VD v) { return v.dynamicsTimes.lufsIntegrated; });

    // ---- 评分（相对最好版本的扣分，见文件头）----
    struct Dim { const char* key; double w; double fullDelta; bool higherBetter; };
    static const Dim dims[] = {
        { "dr_db",        0.25,  6.0, true  },
        { "snr_db",       0.20, 20.0, true  },
        { "clipped_pct",  0.20,  0.0, false },   // 特殊：penalty 由 clip_penalty 给
        { "crest_db",     0.10,  4.0, true  },
        { "lra",          0.10,  6.0, true  },
        { "non_tonal_db", 0.10, 10.0, false },
        { "mono_loss_db", 0.05,  3.0, false },
    };
    auto row_by_key = [&](const std::string& k) -> const MetricRow* {
        for (const MetricRow& row : r.rows) if (row.key == k) return &row;
        return nullptr;
    };
    // 削波扣分：占比 0.5% 或 2000 段即满罚（取大者）—— 单样本越限几乎不可闻，连续削波才是硬伤
    auto clip_penalty = [&](size_t i) {
        const VersionData& v = r.versions[i];
        if (v.quality.clippedPct <= 0.0 && v.quality.clippedRuns == 0) return 0.0;
        const double pctP = std::min(1.0, v.quality.clippedPct / 0.5);
        const double runP = std::min(1.0, (double)v.quality.clippedRuns / 2000.0);
        return std::max(pctP, runP);
    };
    r.scores.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        double pen = 0.0, wsum = 0.0;
        for (const Dim& d : dims) {
            if (std::string(d.key) == "clipped_pct") {
                const MetricRow* row = row_by_key(d.key);
                if (!row || !row->comparable) continue;
                const double bestP = [&] {
                    double b = 1e18;
                    for (size_t k = 0; k < n; ++k) b = std::min(b, clip_penalty(k));
                    return b;
                }();
                pen += d.w * std::min(1.0, clip_penalty(i) - bestP);
                wsum += d.w;
                continue;
            }
            const MetricRow* row = row_by_key(d.key);
            if (!row || !row->comparable) continue;   // 不可比 ⟹ 不参与，权重重新归一化
            const double best = row->values[row->bestIndex];
            const double diff = d.higherBetter ? (best - row->values[i]) : (row->values[i] - best);
            const double penalty = (d.fullDelta > 0.0) ? std::min(1.0, std::max(0.0, diff) / d.fullDelta) : 0.0;
            pen += d.w * penalty;
            wsum += d.w;
        }
        r.scores[i] = (wsum > 0.0) ? 100.0 * (1.0 - pen / wsum) : 0.0;
        if (r.scores[i] < 0.0) r.scores[i] = 0.0;
    }

    // ---- 排序 ----
    r.ranking.resize(n);
    for (size_t i = 0; i < n; ++i) r.ranking[i] = i;
    std::stable_sort(r.ranking.begin(), r.ranking.end(), [&](size_t a, size_t b) {
        if (r.scores[a] != r.scores[b]) return r.scores[a] > r.scores[b];
        return a < b;
    });

    // ---- 确定性人话（不依赖 AI；AI 层只在此基础上做自然语言润色）----
    std::ostringstream o;
    o << "共 " << n << " 个版本；已按 200 Hz 包络互相关对齐，"
      << "在公共区段 " << std::fixed << std::setprecision(1) << r.windowSec << " s 上重算全部指标"
      << "（最小包络相关 " << std::setprecision(3) << r.sameTrackCorr << "）。\n";
    if (r.sameTrackCorr < 0.70)
        o << "⚠ 包络相关 " << std::setprecision(2) << r.sameTrackCorr
          << " 偏低（同一首不同母带实测 0.53~0.56）：可能是母带/剪辑不同，也可能只是同一录音的不同转制；"
             "请自行确认这些文件确实是同一首。\n";
    o << "（分数是【本批次内】相对最好版本的扣分，跨批次不可比）\n综合排序：";
    for (size_t k = 0; k < n; ++k) {
        if (k) o << " > ";
        o << r.versions[r.ranking[k]].label << "(" << std::setprecision(1) << r.scores[r.ranking[k]] << ")";
    }
    o << "\n";
    const size_t best = r.ranking.front();
    for (size_t k = 1; k < n; ++k) {
        const size_t other = r.ranking[k];
        o << "为什么 " << r.versions[best].label << " 优于 " << r.versions[other].label << "：";
        bool any = false;
        for (const MetricRow& row : r.rows) {
            if (row.informational || !row.comparable) continue;
            const double a = row.values[best], b2 = row.values[other];
            const double d = a - b2;
            // 只报"够显著"的差异，避免噪声级差值堆成一大段
            double thr = 0.0;
            if (row.key == "dr_db" || row.key == "crest_db" || row.key == "lra") thr = 0.3;
            else if (row.key == "snr_db" || row.key == "noise_floor_dbfs" || row.key == "non_tonal_db") thr = 0.5;
            else if (row.key == "clipped_pct") thr = 1e-9;
            else if (row.key == "clipped_runs") thr = 0.5;
            else if (row.key == "mono_loss_db") thr = 0.2;
            else continue;                 // 信息性/无方向的维度不进解释
            if (std::fabs(d) < thr) continue;
            const bool better = row.higherBetter ? (d > 0) : (d < 0);
            if (!better) continue;         // 只列"赢"的维度，输的维度另有汇总
            any = true;
            o << " " << row.label << " " << std::setprecision(2) << a << row.unit
              << " vs " << b2 << row.unit << "（" << (d > 0 ? "+" : "") << d << "）;";
        }
        if (!any) o << " 各评分维度差异都在噪声级以内（差别主要来自信息性指标，见对比表）。";
        o << "\n";
    }
    // 不可比项
    std::vector<std::string> notComp;
    for (const MetricRow& row : r.rows) if (!row.comparable) notComp.push_back(row.label + "（" + row.why + "）");
    if (!notComp.empty()) {
        o << "不可比项（未参与评分）：";
        for (size_t i = 0; i < notComp.size(); ++i) { if (i) o << "；"; o << notComp[i]; }
        o << "\n";
    }
    r.explanation = o.str();
}

} // namespace rta::analysis
