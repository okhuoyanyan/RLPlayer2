// fake_lossless.h — 音频体检 B1a：假无损（有损源转制）检测 + 24-bit 容器 LSB 检查
//
// 用途：判断「容器/位深是无损，内容却像从有损版本转制」的素材，reason 直接供 UI 展示。
//
// 设计要点（原则：宁可漏报不可误报 —— 老录音天然窄带绝不判假）：
//  1) 频谱估计：全轨均匀取 ≤12 个 65536 样窗（radix-2 FFT，src/dsp/fft.h），加 Hann 窗
//     （压制乐音泛音的泄漏裙边对截止点判定的污染），幅度谱按窗平均；声道先混迭成单声道
//     （有损转制的截止特征各声道强相关，混迭不损失判据）。
//  2) 截止点：从高频向低频扫，找「谱密度连续 ≥8 bin 低于 [1k..5k] 段平均 −40 dB」的
//     最高一段的【低频端】f_cut（8 bin ≈ 5 Hz @44.1k/65536 点，滤掉单点谱陷）。
//  3) 陡沿：f_cut 上下各 1/6 倍频程的能量差 >12 dB 才算 codec 式硬截止。温和滚降
//     （磁带/老录音/动圈话筒）在 −40 dB 点附近的局部斜率只有几十 dB/oct，换算到 1/6
//     倍频程只有几个 dB —— 判不过这条门，正是不误报老录音的关键。
//  4) 置信度 = 陡沿 dB（12→25 dB 线性映射 0.5→1.0）× 窗间一致率（各窗用自己的参考段
//     独立扫截止，落在全局截止 ±1000 Hz 内的比例）；置信度 <0.5 强制不判假。
//  5) fake24bit 与 suspected 相互独立：srcBits ≥ 24 时把样本乘回整数域统计低 8 位，
//     恒零占比 >99.9% ⟹ 16-bit 内容装进 24-bit 容器（真 24-bit 素材低 8 位近似均匀，
//     恒零占比约 1/256 ≈ 0.4%，差 250 倍，判据余量充足）。
#pragma once
#include "../dsp/fft.h"      // rta::dsp::fft（radix-2，n 须为 2 的幂）/ kPi
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstddef>

namespace rta::analysis {

struct LosslessCheck {
    bool   suspected = false;   // 疑似有损源转制（假无损）
    double cutoffHz = 0.0;      // 检出的频谱截止（0=未见）
    double confidence = 0.0;    // 0..1
    std::string reason;         // 中文人话，供 UI 直接显示
    bool   fake24bit = false;   // LSB 恒零（16bit 内容搬进 24bit 容器）
};

namespace detail {

// 从高频向低频扫：找第一段「连续 ≥minRun bin 低于阈值」的 run 的低频端 bin（无则 0）。
// kTop 为扫描起点（含），kLow 为终点（含）。扫到底仍是同一段（整带皆阈下，近纯音/超窄带）
// 时不报 —— 宁可漏报。
inline size_t scan_cutoff_bin(const std::vector<double>& magDb, double thrDb,
                              size_t kTop, size_t kLow, size_t minRun) {
    size_t run = 0;
    for (size_t k = kTop + 1; k-- > kLow;) {
        if (magDb[k] < thrDb) { ++run; continue; }
        if (run >= minRun) return k + 1;   // 刚结束的这段 run 的低频端
        run = 0;
    }
    return 0;
}

} // namespace detail

inline LosslessCheck check_lossless(const double* const* ch, int channels, size_t n, double fs, int srcBits) {
    LosslessCheck out;
    if (!ch || channels <= 0 || n == 0 || fs <= 0.0) {
        out.reason = "样本不足，无法分析";
        return out;
    }

    constexpr size_t kW = 65536;                    // radix-2，且窗越长谱密度分辨率越高
    const size_t nWin = std::max<size_t>(1, std::min<size_t>(12, n / kW));
    const size_t half = kW / 2;
    const double binHz = fs / (double)kW;

    // 窗位置：均匀铺满全轨（首窗贴头、末窗贴尾）
    std::vector<size_t> start(nWin, 0);
    for (size_t w = 1; w < nWin; ++w)
        start[w] = (n > kW) ? w * (n - kW) / (nWin - 1) : 0;

    // 各窗加 Hann → FFT → 幅度谱；同时保留各窗自己的 dB 谱（窗间一致率用）
    std::vector<double> avgMag(half + 1, 0.0);
    std::vector<std::vector<double>> winDb;
    winDb.reserve(nWin);
    std::vector<rta::dsp::cplx> buf(kW);
    for (size_t w = 0; w < nWin; ++w) {
        for (size_t i = 0; i < kW; ++i) {
            double mix = 0.0;
            for (int c = 0; c < channels; ++c) {
                const size_t idx = start[w] + i;
                if (idx < n && ch[c]) mix += ch[c][idx];
            }
            mix /= (double)channels;
            const double hann = 0.5 * (1.0 - std::cos(2.0 * rta::dsp::kPi * (double)i / (double)(kW - 1)));
            buf[i] = rta::dsp::cplx(mix * hann, 0.0);
        }
        rta::dsp::fft(buf, false);
        std::vector<double> mdb(half + 1, -300.0);
        for (size_t k = 0; k <= half; ++k) {
            const double m = std::abs(buf[k]);
            avgMag[k] += m;
            mdb[k] = 20.0 * std::log10(m + 1e-300);
        }
        winDb.push_back(std::move(mdb));
    }
    for (size_t k = 0; k <= half; ++k) avgMag[k] /= (double)nWin;

    // 参考段 [1k..5k] 平均谱密度（线性均值，混迭与窗函数的常数尺度在相对比较中抵消）
    const size_t kRefLo = (size_t)std::max(1.0, std::floor(1000.0 / binHz));
    const size_t kRefHi = std::min(half - 1, (size_t)std::ceil(5000.0 / binHz));
    if (kRefHi <= kRefLo) { out.reason = "采样率异常，无法分析"; return out; }
    double refSum = 0.0;
    for (size_t k = kRefLo; k <= kRefHi; ++k) refSum += avgMag[k];
    const double refDb = 20.0 * std::log10(refSum / (double)(kRefHi - kRefLo + 1) + 1e-300);

    std::vector<double> avgDb(half + 1, -300.0);
    for (size_t k = 0; k <= half; ++k) avgDb[k] = 20.0 * std::log10(avgMag[k] + 1e-300);

    // 截止点：从奈奎斯特向低扫到 1 kHz 为止
    const size_t kCut = detail::scan_cutoff_bin(avgDb, refDb - 40.0, half, kRefLo, 8);

    double cutoffHz = 0.0, edgeDb = 0.0;
    bool steep = false;
    if (kCut > 0) {
        cutoffHz = (double)kCut * binHz;
        // 陡沿：f_cut 上下各 1/6 倍频程的能量差（上限截到奈奎斯特）
        const double oct6 = std::pow(2.0, 1.0 / 6.0);
        const size_t kLo = (size_t)std::floor(cutoffHz / oct6 / binHz);
        const size_t kHi = std::min(half, (size_t)std::ceil(cutoffHz * oct6 / binHz));
        double eLo = 0.0, eHi = 0.0;
        for (size_t k = kLo; k < kCut; ++k) eLo += avgMag[k] * avgMag[k];
        for (size_t k = kCut; k <= kHi; ++k) eHi += avgMag[k] * avgMag[k];
        edgeDb = 10.0 * std::log10(eLo / std::max(eHi, 1e-300) + 1e-300);
        steep = edgeDb > 12.0;
    }

    // 窗间一致率：各窗用自己的 [1k..5k] 均值−40 dB 独立扫，截止点落在全局 ±1000 Hz 内算一致
    double consist = 0.0;
    if (kCut > 0) {
        size_t matched = 0;
        for (const auto& mdb : winDb) {
            double rs = 0.0;
            for (size_t k = kRefLo; k <= kRefHi; ++k) {
                rs += std::pow(10.0, mdb[k] / 20.0);
            }
            const double lthr = 20.0 * std::log10(rs / (double)(kRefHi - kRefLo + 1) + 1e-300) - 40.0;
            const size_t kc = detail::scan_cutoff_bin(mdb, lthr, half, kRefLo, 8);
            if (kc > 0 && std::fabs((double)kc - (double)kCut) * binHz <= 1000.0) ++matched;
        }
        consist = (double)matched / (double)nWin;
    }

    out.cutoffHz = cutoffHz;
    out.confidence = (kCut > 0)
        ? std::clamp((edgeDb - 12.0) / (25.0 - 12.0), 0.0, 1.0) * consist
        : 0.0;
    // 判假三道门：截止可见、足够低（接近奈奎斯特的不算）、边沿足够陡、置信度达标
    out.suspected = (kCut > 0) && (cutoffHz < 20500.0) && steep && (out.confidence >= 0.5);

    char msg[256];
    if (out.suspected) {
        std::snprintf(msg, sizeof(msg), "%.1f kHz 处存在陡峭截止（下降 %.0f dB），典型的有损源转制特征",
                      cutoffHz / 1000.0, edgeDb);
    } else if (kCut > 0 && cutoffHz < 20500.0) {
        // 截止可见但被陡沿门拦下 —— 正是「老录音温和滚降」要放过的情况
        std::snprintf(msg, sizeof(msg), "%.1f kHz 处频谱走低但下降沿缓和（1/6 倍频程约 %.0f dB），更像老录音自然滚降，不判假",
                      cutoffHz / 1000.0, edgeDb);
    } else if (kCut > 0) {
        std::snprintf(msg, sizeof(msg), "检出 %.1f kHz 附近的频谱下降（已接近奈奎斯特，不判假）",
                      cutoffHz / 1000.0);
    } else {
        std::snprintf(msg, sizeof(msg), "未检出明显频谱截止");
    }
    out.reason = msg;

    // fake24bit：srcBits ≥ 24 才有意义。乘回整数域看低 8 位 —— 16-bit 内容左移装填时恒零，
    // 真 24-bit 素材低 8 位近似均匀（≈0.4% 恒零），阈值 99.9% 两侧余量都很大。
    if (srcBits >= 24) {
        const double scale = std::pow(2.0, (double)(srcBits - 1));
        size_t tot = 0, low0 = 0;
        for (int c = 0; c < channels; ++c) {
            if (!ch[c]) continue;
            for (size_t i = 0; i < n; ++i) {
                const long long v = (long long)std::llround(ch[c][i] * scale);
                ++tot;
                if ((v & 0xFFll) == 0) ++low0;
            }
        }
        if (tot > 0 && (double)low0 / (double)tot > 0.999) {
            out.fake24bit = true;
            out.reason += "；低 8 位恒零，疑似 16-bit 内容装入 24-bit 容器";
        }
    }
    return out;
}

} // namespace rta::analysis
