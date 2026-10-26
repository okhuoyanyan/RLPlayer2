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
//
// ================== 增量（流式）接口 ==================
// 背景（261026-03 遗留项）：整曲体检要把整段样本握在手里才能取 12 个谱窗，DSD 大文件
//   因此峰值 ~2.6 GB。增量接口把「取哪 12 个窗」预先算成计划，边流边把窗内样本抄进
//   12×65536 的小缓冲（立体声 6.3 MB，与曲长无关），其余样本随到随丢。
//
// ★ 前提与硬约定：
//   - 窗位置 start[w] 依赖【总帧数】（全轨均匀铺满），所以 begin() 必须先拿到总帧数
//     （DSD/WAV 从文件头就知道；拿不到就别用增量接口）。
//   - 同一组样本，check_lossless(整段) 与 LosslessAccumulator（任意分块）的输出必须
//     【逐位一致（==）】：cutoffHz / confidence / suspected / fake24bit / reason 全同。
//     实现上 check_lossless 就是「按总帧数建计划 + 整段喂 + finalize」的包装。
//     分块一致性靠两点：① 每块样本先按【同一顺序】混迭成单声道（Σ_c ch[c][i]/channels，
//     声道升序、i 升序），窗重叠也只重放同一个已算好的值；② 统计量是整数计数。
//   - 若实际喂进来的帧数 != begin() 声明的总帧数（截断/读失败），计划就不成立，
//     planMatched() 返回 false，调用方必须回退整段路径 —— 不要拿它的结果当数。
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

// 块内取样本的三个取值器（平面 / 交错 double / 交错 float）。i 一律是【本块内的帧偏移】。
struct PlanarGet {
    const double* const* ch;
    double operator()(size_t c, size_t i) const { return ch[c][i]; }
};
struct InterleavedGet {
    const double* p;
    size_t nch;
    double operator()(size_t c, size_t i) const { return p[i * nch + c]; }
};
struct InterleavedF32Get {
    const float* p;
    size_t nch;
    double operator()(size_t c, size_t i) const { return (double)p[i * nch + c]; }
};

// ---- 谱窗计划：整段接口与增量接口共用（窗位置的唯一来源）----
struct LosslessWindowPlan {
    size_t kW = 65536;                  // radix-2，且窗越长谱密度分辨率越高
    size_t half = 32768;
    size_t nWin = 1;
    double binHz = 1.0;
    std::vector<size_t> start;          // 各窗起始帧（全轨均匀铺满：首窗贴头、末窗贴尾）
    size_t kRefLo = 1, kRefHi = 0;      // 参考段 [1k..5k]
    bool   refOk = false;
};

inline LosslessWindowPlan make_window_plan(size_t n, double fs) {
    LosslessWindowPlan p;
    p.kW = 65536;
    p.half = p.kW / 2;
    p.binHz = fs / (double)p.kW;
    p.nWin = std::max<size_t>(1, std::min<size_t>(12, n / p.kW));
    p.start.assign(p.nWin, 0);
    for (size_t w = 1; w < p.nWin; ++w)
        p.start[w] = (n > p.kW) ? w * (n - p.kW) / (p.nWin - 1) : 0;
    p.kRefLo = (size_t)std::max(1.0, std::floor(1000.0 / p.binHz));
    p.kRefHi = std::min(p.half - 1, (size_t)std::ceil(5000.0 / p.binHz));
    p.refOk = (p.kRefHi > p.kRefLo);
    return p;
}

// 频谱部分：winMix[w][i] = 第 w 个窗第 i 点的「已混迭成单声道」原值（未加窗）。
// 整段接口与增量接口都把样本收进这个形态，再走同一段算术 ⟹ 逐位一致。
inline void spectrum_from_windows(const LosslessWindowPlan& plan,
                                  const std::vector<std::vector<double>>& winMix,
                                  LosslessCheck& out) {
    const size_t kW = plan.kW, half = plan.half, nWin = plan.nWin;
    const double binHz = plan.binHz;

    // 各窗加 Hann → FFT → 幅度谱；同时保留各窗自己的 dB 谱（窗间一致率用）
    std::vector<double> avgMag(half + 1, 0.0);
    std::vector<std::vector<double>> winDb;
    winDb.reserve(nWin);
    std::vector<rta::dsp::cplx> buf(kW);
    for (size_t w = 0; w < nWin; ++w) {
        const std::vector<double>& src = winMix[w];
        for (size_t i = 0; i < kW; ++i) {
            const double mix = src[i];
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
    const size_t kRefLo = plan.kRefLo;
    const size_t kRefHi = plan.kRefHi;
    if (!plan.refOk) { out.reason = "采样率异常，无法分析"; return; }
    double refSum = 0.0;
    for (size_t k = kRefLo; k <= kRefHi; ++k) refSum += avgMag[k];
    const double refDb = 20.0 * std::log10(refSum / (double)(kRefHi - kRefLo + 1) + 1e-300);

    std::vector<double> avgDb(half + 1, -300.0);
    for (size_t k = 0; k <= half; ++k) avgDb[k] = 20.0 * std::log10(avgMag[k] + 1e-300);

    // 截止点：从奈奎斯特向低扫到 1 kHz 为止
    const size_t kCut = scan_cutoff_bin(avgDb, refDb - 40.0, half, kRefLo, 8);

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
            const size_t kc = scan_cutoff_bin(mdb, lthr, half, kRefLo, 8);
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
}

} // namespace detail

// ==================== 增量（流式）假无损分析器 ====================
//
// 用法：
//     LosslessAccumulator acc;
//     acc.begin(totalFrames, channels, fs, srcBits);      // 总帧数必须已知（窗位置取决于它）
//     while (还有数据) acc.add_interleaved(buf, frames);   // 或 add_planar / add_interleaved_f32
//     const LosslessCheck c = acc.finalize();             // 先看 planMatched()
class LosslessAccumulator {
public:
    // active: 每声道 1/0（nullptr = 全有效）
    void begin(size_t totalFrames, int channels, double fs, int srcBits, const char* active = nullptr) {
        totalFrames_ = totalFrames;
        channels_ = channels > 0 ? channels : 0;
        fs_ = fs;
        srcBits_ = srcBits;
        pos_ = 0;
        tot_ = 0;
        low0_ = 0;
        scale_ = std::pow(2.0, (double)(srcBits - 1));
        active_.assign((size_t)channels_, 0);
        for (int c = 0; c < channels_; ++c)
            active_[(size_t)c] = (active ? (active[c] ? 1 : 0) : 1);
        bad_ = (totalFrames_ == 0 || channels_ <= 0 || fs_ <= 0.0);
        if (bad_) {
            plan_ = detail::LosslessWindowPlan();
            plan_.start.clear();
            winMix_.clear();
            mixBuf_.clear();
            return;
        }
        plan_ = detail::make_window_plan(totalFrames_, fs_);
        winMix_.clear();
        winMix_.resize(plan_.nWin);
        for (size_t w = 0; w < plan_.nWin; ++w) winMix_[w].assign(plan_.kW, 0.0);
        mixBuf_.clear();
    }

    // 平面 double。★ 寻址约定（与 add_interleaved 一致）：ch[c] 指向【本块第 0 帧】，
    // 即块内相对寻址 ch[c][0..frames-1]；块在整轨里的绝对位置由内部游标 pos_ 负责。
    // —— 2026-10-27 实测踩过的坑：早先这里写成 ch[c][pos_ + i]，而调用方传的是已偏移的
    //    指针（ch[c] + pos），偏移被算了两次，分块跑到 441000 帧时读越界崩溃（ASan 抓到）。
    // ch[c] 为空 ⟹ 该声道按"无数据"处理（混迭时贡献 0，与整段接口一致）
    void add_planar(const double* const* ch, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!ch) { pos_ += frames; return; }
        for (int c = 0; c < channels_; ++c) if (!ch[c]) active_[(size_t)c] = 0;
        const detail::PlanarGet get{ ch };
        capture(get, frames);
        if (srcBits_ >= 24) fake24_planar(ch, frames);
        pos_ += frames;
    }

    // 交错 double（interleaved，块内相对寻址 interleaved[i*channels + c]）
    void add_interleaved(const double* interleaved, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!interleaved) { pos_ += frames; return; }
        const detail::InterleavedGet get{ interleaved, (size_t)channels_ };
        capture(get, frames);
        if (srcBits_ >= 24) fake24_interleaved(interleaved, (size_t)channels_, frames);
        pos_ += frames;
    }

    // 交错 float（便利重载；float 只有 24 bit 尾数，与 double 整段接口不保证逐位一致）
    void add_interleaved_f32(const float* interleaved, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!interleaved) { pos_ += frames; return; }
        const detail::InterleavedF32Get get{ interleaved, (size_t)channels_ };
        capture(get, frames);
        if (srcBits_ >= 24) {
            const size_t nch = (size_t)channels_;
            for (size_t c = 0; c < nch; ++c) {
                if (!active_[c]) continue;
                for (size_t i = 0; i < frames; ++i) {
                    const long long v = (long long)std::llround((double)interleaved[i * nch + c] * scale_);
                    ++tot_;
                    if ((v & 0xFFll) == 0) ++low0_;
                }
            }
        }
        pos_ += frames;
    }

    // 结果。planMatched() == false 时【不可信】（窗位置与实际帧数不符）—— 调用方须回退整段路径。
    LosslessCheck finalize() const {
        LosslessCheck out;
        if (bad_) { out.reason = "样本不足，无法分析"; return out; }
        detail::spectrum_from_windows(plan_, winMix_, out);
        // fake24bit：srcBits ≥ 24 才有意义。乘回整数域看低 8 位 —— 16-bit 内容左移装填时恒零，
        // 真 24-bit 素材低 8 位近似均匀（≈0.4% 恒零），阈值 99.9% 两侧余量都很大。
        if (srcBits_ >= 24 && tot_ > 0 && (double)low0_ / (double)tot_ > 0.999) {
            out.fake24bit = true;
            out.reason += "；低 8 位恒零，疑似 16-bit 内容装入 24-bit 容器";
        }
        return out;
    }

    bool   planMatched() const { return !bad_ && pos_ == totalFrames_; }
    bool   bad() const { return bad_; }
    size_t framesSeen() const { return pos_; }
    size_t plannedFrames() const { return totalFrames_; }
    size_t windowCount() const { return plan_.nWin; }
    // 计划占用的窗缓冲字节数（诊断/内存报告用）
    size_t windowBytes() const { return plan_.nWin * plan_.kW * sizeof(double); }

private:
    // fake24：把样本乘回整数域统计低 8 位（整数计数，顺序无关；整块全扫，不只谱窗）
    void fake24_planar(const double* const* ch, size_t frames) {
        const size_t nch = (size_t)channels_;
        for (size_t c = 0; c < nch; ++c) {
            if (!active_[c]) continue;
            const double* p = ch[c];                  // 块内相对寻址
            for (size_t i = 0; i < frames; ++i) {
                const long long v = (long long)std::llround(p[i] * scale_);
                ++tot_;
                if ((v & 0xFFll) == 0) ++low0_;
            }
        }
    }
    void fake24_interleaved(const double* interleaved, size_t nch, size_t frames) {
        for (size_t c = 0; c < nch; ++c) {
            if (!active_[c]) continue;
            for (size_t i = 0; i < frames; ++i) {
                const long long v = (long long)std::llround(interleaved[i * nch + c] * scale_);
                ++tot_;
                if ((v & 0xFFll) == 0) ++low0_;
            }
        }
    }

    // 把与 [pos_, pos_+frames) 相交的谱窗区间抄进 winMix_。
    // 只有落入谱窗的帧才做混迭（12 个窗 × 65536 = 786k 帧，与曲长无关）；
    // 全轨逐帧混迭会白烧 67.7M 次加法（实测 +1.3 s / 271 MB DSD）。
    template <class Get>
    void capture(const Get& get, size_t frames) {
        const size_t begin = pos_, end = begin + frames;
        for (size_t w = 0; w < plan_.nWin; ++w) {
            const size_t ws = plan_.start[w];
            const size_t we = ws + plan_.kW;
            if (we <= begin || ws >= end) continue;
            const size_t lo = (begin > ws) ? begin : ws;
            const size_t hi = (end < we) ? end : we;
            const size_t cnt = hi - lo;
            mixBuf_.resize(cnt);
            for (size_t i = 0; i < cnt; ++i) {
                double mix = 0.0;
                const size_t rel = lo - begin + i;      // 本块内的帧偏移
                for (size_t c = 0; c < (size_t)channels_; ++c) {
                    if (!active_[c]) continue;
                    mix += get(c, rel);
                }
                mixBuf_[i] = mix / (double)channels_;
            }
            double* dst = winMix_[w].data();
            for (size_t g = lo; g < hi; ++g) dst[g - ws] = mixBuf_[g - lo];
        }
    }

    size_t totalFrames_ = 0;
    int    channels_ = 0;
    double fs_ = 0.0;
    int    srcBits_ = 0;
    double scale_ = 1.0;
    bool   bad_ = false;
    size_t pos_ = 0;
    size_t tot_ = 0, low0_ = 0;
    std::vector<char> active_;
    detail::LosslessWindowPlan plan_;
    std::vector<std::vector<double>> winMix_;
    std::vector<double> mixBuf_;      // 当前块的单声道混迭值（复用，容量随块大小）
};

// 一次性接口：整段平面 double。实现上是「建计划 + 整段喂 + finalize」，与增量接口逐位一致。
inline LosslessCheck check_lossless(const double* const* ch, int channels, size_t n, double fs, int srcBits) {
    LosslessCheck out;
    if (!ch || channels <= 0 || n == 0 || fs <= 0.0) {
        out.reason = "样本不足，无法分析";
        return out;
    }
    std::vector<char> active((size_t)channels, 0);
    for (int c = 0; c < channels; ++c) active[(size_t)c] = ch[c] ? 1 : 0;
    LosslessAccumulator acc;
    acc.begin(n, channels, fs, srcBits, active.data());
    acc.add_planar(ch, n);
    return acc.finalize();
}

} // namespace rta::analysis
