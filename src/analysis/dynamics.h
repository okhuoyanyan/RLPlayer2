// dynamics.h — 音频体检 B1a：纯算法动态分析器（TT DR / BS.1770-4 LUFS / EBU R128 LRA / 样本峰值）
//
// 用途：对 PCM（double、标称 [-1,1)、任意声道布局）做一次离线体检，
//       不开音频设备、不解码文件。
//
// 两套接口，算术完全共用（见下文「增量接口的硬约定」）：
//   * measure_dynamics(平面 double, 整段)        —— 一次性接口，老调用方零改动；
//   * DynamicsAccumulator::add_*() + finalize() —— 增量（流式）接口，按块喂样本。
//
// 设计要点（三条指标全部按公开规范实现，不自造口径）：
//  1) TT DR：每声道切 3 秒块（尾部不足一块丢弃），块 RMS 降序取前 20% 的块，把这些块的
//     【样本合并】求二阶 RMS₂，DR_ch = 20·log10(全轨峰值 / RMS₂)，结果取各声道较低者。
//     ★ 合并样本的 RMS ≠ 块 RMS 的平均：块 RMS 平均的平方是均值的平方，合并样本 RMS 的平方
//       是均方本身，正弦信号（各块等幅）下两者差 10·log10(π/2·...) ≈ 3 dB 级别 ——
//       这是该口径最常见的实现错误，会把稳态正弦测成 ~0 dB。
//  2) LUFS：ITU-R BS.1770-4。K 加权 = 高频架（模拟原型约 1681.97 Hz、+4.0 dB）级联
//     RLB 高通（约 38.1 Hz），双二阶实现；系数以 48 kHz 规范表为锚点、按 Tustin
//     （s = 2fs·(1−z⁻¹)/(1+z⁻¹)，预畸变内含）重标定到任意 fs —— fs=48k 时反演→正演
//     是恒等变换，系数与规范表一致（test_dynamics 实测全频点 |Δ响应| < 1e-6 dB）。
//     注意：任务书把两级参数写串了（"一阶高通 1681.97 级联高架 3819.6"）；规范表里
//     1681.97 Hz 属于高频架、38 Hz 级属于高通，本实现按 BS.1770-4 表 1 的规范系数做，
//     官方校准点（997 Hz / −23 dBFS / 立体声）实测 −23.0 LUFS ±0.1（见 tests/test_dynamics.cpp）。
//     分块 400 ms、步进 100 ms（75% 交叠）；块响度 = −0.691 + 10·log10(Σ_ch G_i·z_i)；
//     绝对门 −70 LUFS，相对门 =（门内初算积分 −10 LU）再积分。
//  3) LRA：EBU Tech 3342。3 秒短时窗、步进 100 ms；过绝对门 −70 与相对门（积分 −20 LU）后
//     取 P95 − P10（升序线性插值百分位）。
//
// 已知局限（详见 docs/WIP-B1a.md）：
//   - 声道权重 G_i 只实现了立体声 L/R = 1.0，多声道一律按 1.0（BS.1770 的 Ls/Rs = 1.41 未做）；
//   - 素材 < 3 秒时 DR/LRA 口径不成立（返回 0）；< 400 ms 时整体 valid=false。
//
// ================== 增量（流式）接口 ==================
// 背景（261026-03 遗留项）：整曲体检要把整段样本握在手里 —— DSD64 的 271 MB 文件解码成
//   int32 是 517 MB，再铺成 double 平面又翻一倍，进程峰值实测 ~2.6 GB。增量接口让调用方
//   按块（建议 65536~262144 帧）喂样本，分析器只保留：
//     * 每声道 3 s 的滤波后环形缓冲（fs=176.4k 时 4.2 MB/声道）—— LRA 的 3 s 窗要重算；
//     * 每 100 ms 一条的块功率 / 短时功率（8 B/100 ms ≈ 0.29 MB/小时，与声道数无关）；
//     * 每 3 s 一条的 DR 块均方（8 B / 3 s / 声道）。
//   峰值内存因此只与块大小、采样率、声道数有关；曲长只影响上面那个线性但极小的统计量数组。
//
// ★ 硬约定（必须逐位一致，不是"接近"）：同一组样本，无论
//     measure_dynamics 一次喂完，还是 add_* 分成任意块（含每块 1 帧、块大小跨
//     400 ms / 3 s 边界、块长不是步进的整数倍）喂进去，四个输出必须【完全相同（==）】。
//   实现上 measure_dynamics 就是"整段喂给 DynamicsAccumulator"的包装，两者共用同一份
//   算术；新增指标也必须共用，禁止各写一份。之所以成立：
//     - 峰值是 max，与顺序无关；
//     - DR 的每声道块平方和、K 加权双二阶状态、每个窗口的平方和，一律按
//       【样本时间顺序】累加；块边界只决定"何时把已累加的窗口收走"，不改变加法顺序；
//     - 所有门限/百分位在 finalize() 里按老代码的同一顺序、同一表达式重放。
#pragma once
#include "../dsp/matrix.h"   // Biquad（直接型转置 II）
#include "../dsp/kaiser.h"   // kPi
#include <vector>
#include <algorithm>
#include <functional>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rta::analysis {

struct DynamicsResult {
    double drDb = 0.0;           // TT DR（各声道取较低者）
    double lufsIntegrated = 0.0; // BS.1770-4 K 加权积分响度
    double lra = 0.0;            // EBU R128 短期响度 10~95 百分位差
    double peakDbfs = 0.0;       // 样本峰值
    bool   valid = false;
};

// ---- K 加权锚点：ITU-R BS.1770-4 表 1 的 48 kHz 规范系数 ----
// 所有采样率的系数都从这组数出发重标定，保证 48k 下与规范逐位一致。
inline constexpr double kKWeight48ShelfB[3] = { 1.53512485958697, -2.69169618940638, 1.19839281085285 };
inline constexpr double kKWeight48ShelfA[3] = { 1.0, -1.69065929318241, 0.73248077421585 };
inline constexpr double kKWeight48HpB[3]    = { 1.0, -2.0, 1.0 };
inline constexpr double kKWeight48HpA[3]    = { 1.0, -1.99004745483398, 0.99007225036621 };

struct BiquadCoeffs { double b0, b1, b2, a1, a2; };

// 数字 z⁻¹ 域 → 模拟 s 域（Tustin 反演）：s = 2fs(1−z⁻¹)/(1+z⁻¹) ⟹ z⁻¹ = (K−s)/(K+s)
// 分子/分母各化成 s 的二次式 N2 s² + N1 s + N0（K = 2·fs）
inline void digital_to_analog(const double b[3], const double a[3], double K,
                              double& N2, double& N1, double& N0,
                              double& D2, double& D1, double& D0) {
    N2 = b[0] - b[1] + b[2];
    N1 = 2.0 * K * (b[0] - b[2]);
    N0 = K * K * (b[0] + b[1] + b[2]);
    D2 = 1.0 - a[1] + a[2];
    D1 = 2.0 * K * (1.0 - a[2]);
    D0 = K * K * (1.0 + a[1] + a[2]);
}

// 模拟 s 域 → 数字 z⁻¹ 域（Tustin，s = K(1−z⁻¹)/(1+z⁻¹)，K = 2·fs；预畸变内含）
inline BiquadCoeffs analog_to_digital(double N2, double N1, double N0,
                                      double D2, double D1, double D0, double K) {
    const double kk = K * K;
    const double d0 = D2 * kk + D1 * K + D0;   // a0，归一化基准
    BiquadCoeffs c;
    c.b0 = ( N2 * kk + N1 * K + N0) / d0;
    c.b1 = (-2.0 * N2 * kk + 2.0 * N0) / d0;
    c.b2 = ( N2 * kk - N1 * K + N0) / d0;
    c.a1 = (-2.0 * D2 * kk + 2.0 * D0) / d0;
    c.a2 = ( D2 * kk - D1 * K + D0) / d0;
    return c;
}

// K 加权两级双二阶：以 48 kHz 规范系数为锚点重标定到 fs（fs=48k 时 roundtrip 恒等）
inline void k_weight_biquads(double fs, rta::dsp::Biquad& shelf, rta::dsp::Biquad& hp) {
    const double K48 = 2.0 * 48000.0, K = 2.0 * fs;
    double N2, N1, N0, D2, D1, D0;
    digital_to_analog(kKWeight48ShelfB, kKWeight48ShelfA, K48, N2, N1, N0, D2, D1, D0);
    const BiquadCoeffs cs = analog_to_digital(N2, N1, N0, D2, D1, D0, K);
    shelf.set(cs.b0, cs.b1, cs.b2, 1.0, cs.a1, cs.a2);
    digital_to_analog(kKWeight48HpB, kKWeight48HpA, K48, N2, N1, N0, D2, D1, D0);
    const BiquadCoeffs ch = analog_to_digital(N2, N1, N0, D2, D1, D0, K);
    hp.set(ch.b0, ch.b1, ch.b2, 1.0, ch.a1, ch.a2);
}

// 升序线性插值百分位（EBU Tech 3342 的 P10/P95 口径）
inline double percentile_sorted(const std::vector<double>& asc, double q) {
    if (asc.empty()) return 0.0;
    const double pos = q * (double)(asc.size() - 1);
    const size_t i = (size_t)pos;
    const size_t j = (i + 1 < asc.size()) ? i + 1 : i;
    const double t = pos - (double)i;
    return asc[i] * (1.0 - t) + asc[j] * t;
}

// ==================== 增量（流式）动态分析器 ====================
//
// 用法：
//     DynamicsAccumulator acc;
//     acc.begin(channels, fs);                        // active 掩码可省略（默认全声道有效）
//     while (还有数据) acc.add_interleaved(buf, frames);   // 也可 add_planar / add_interleaved_f32
//     const DynamicsResult r = acc.finalize();        // 可重复调用（不改内部状态）
//
// 不变量：add_* 的块边界对结果没有影响；块内样本必须按时间顺序喂，块与块之间必须连续
//         （中间丢帧等于换了素材，结果自然不同）。
class DynamicsAccumulator {
public:
    // active: 每声道 1/0（nullptr = 全部有效）。0 表示该声道没有数据 —— 与一次性接口
    //         遇到空指针时「整声道跳过」的语义一致（DR 取较低者、响度求和都跳过它）。
    void begin(int channels, double fs, const char* active = nullptr) {
        channels_ = channels > 0 ? channels : 0;
        fs_ = fs;
        active_.assign((size_t)channels_, 0);
        for (int c = 0; c < channels_; ++c)
            active_[(size_t)c] = (active ? (active[c] ? 1 : 0) : 1);

        pos_ = 0;
        peakCh_.assign((size_t)channels_, 0.0);

        drLen_ = (fs > 0.0) ? (size_t)std::llround(3.0 * fs) : 0;
        drAcc_.assign((size_t)channels_, 0.0);
        drCnt_.assign((size_t)channels_, 0);
        drMs_.assign((size_t)channels_, std::vector<double>());

        blk_ = (fs > 0.0) ? (size_t)std::llround(0.4 * fs) : 0;
        stp_ = (fs > 0.0) ? (size_t)std::llround(0.1 * fs) : 0;
        w3_  = (fs > 0.0) ? (size_t)std::llround(3.0 * fs) : 0;
        lufsEnabled_ = (blk_ > 0 && stp_ > 0);
        lraEnabled_  = lufsEnabled_ && (w3_ > 0);
        // 环形缓冲：LRA 要 3 s 的滤波后样本，LUFS 只要 0.4 s —— 取大者
        ringLen_ = lufsEnabled_ ? (lraEnabled_ ? w3_ : blk_) : 0;
        ringPos_ = 0;
        ring_.assign((size_t)channels_, std::vector<double>());
        if (ringLen_ > 0)
            for (auto& v : ring_) v.assign(ringLen_, 0.0);

        shelf_.assign((size_t)channels_, rta::dsp::Biquad());
        hp_.assign((size_t)channels_, rta::dsp::Biquad());
        if (fs > 0.0) {
            rta::dsp::Biquad sp, hpp;
            k_weight_biquads(fs, sp, hpp);
            for (int c = 0; c < channels_; ++c) { shelf_[(size_t)c] = sp; hp_[(size_t)c] = hpp; }
        }

        nextBlkEnd_ = blk_;
        nextStEnd_  = w3_;
        blkPow_.clear();
        stPow_.clear();
    }

    // 平面 double。★ 寻址约定（与 add_interleaved 一致）：ch[c] 指向【本块第 0 帧】，
    // 块内相对寻址 ch[c][0..frames-1]；块在整轨里的绝对位置由内部游标 framesSeen() 负责。
    // 调用方传「ch[c] + pos」这类已偏移的指针是正确用法，千万不要再叠加 pos。
    // ch[c] 为空 ⟹ 该声道按"无数据"处理（整声道跳过，与一次性接口遇到空指针一致）。
    void add_planar(const double* const* ch, size_t frames) {
        if (!ch || channels_ <= 0) return;
        for (int c = 0; c < channels_; ++c) if (!ch[c]) active_[(size_t)c] = 0;
        for (size_t i = 0; i < frames; ++i) {
            auto getVal = [&](int c) { return ch[c][i]; };
            push_frame(getVal);
        }
    }
    // 交错 double（frames 帧 × channels 声道）
    void add_interleaved(const double* interleaved, size_t frames) {
        if (!interleaved || channels_ <= 0) return;
        const size_t chn = (size_t)channels_;
        for (size_t i = 0; i < frames; ++i) {
            const double* p = interleaved + i * chn;
            auto getVal = [&](int c) { return p[(size_t)c]; };
            push_frame(getVal);
        }
    }
    // 交错 float（便利重载）。注意 float 只有 24 bit 尾数，与 double 整段接口【不保证】逐位
    // 一致 —— 只在源本身就是 float 时使用。
    void add_interleaved_f32(const float* interleaved, size_t frames) {
        if (!interleaved || channels_ <= 0) return;
        const size_t chn = (size_t)channels_;
        for (size_t i = 0; i < frames; ++i) {
            const float* p = interleaved + i * chn;
            auto getVal = [&](int c) { return (double)p[(size_t)c]; };
            push_frame(getVal);
        }
    }

    DynamicsResult finalize() const {
        DynamicsResult r;
        // 一帧都没喂过：与一次性接口 n==0 的早退分支逐位一致（默认值，peakDbfs 保持 0.0
        // 而不是 −300，别"顺手修"——那会让两个接口在空输入上不再相等）。
        if (pos_ == 0) return r;
        double peak = 0.0;
        for (int c = 0; c < channels_; ++c)
            if (active_[(size_t)c] && peakCh_[(size_t)c] > peak) peak = peakCh_[(size_t)c];
        r.peakDbfs = (peak > 0.0) ? 20.0 * std::log10(peak) : -300.0;

        // ---- TT DR：3 s 块，前 20% 块合并样本求 RMS₂ ----
        if (drLen_ > 0) {
            bool drInit = false;
            for (int c = 0; c < channels_; ++c) {
                if (!active_[(size_t)c] || drMs_[(size_t)c].empty()) continue;
                std::vector<double> ms = drMs_[(size_t)c];      // 拷贝后排序：finalize 可重复调用
                std::sort(ms.begin(), ms.end(), std::greater<double>());
                const size_t nSel = std::max<size_t>(1, ms.size() / 5);   // 前 20%（至少 1 块）
                double meanSq2 = 0.0;
                for (size_t b = 0; b < nSel; ++b) meanSq2 += ms[b];
                meanSq2 /= (double)nSel;
                const double rms2 = std::sqrt(meanSq2);
                // 全静音声道（峰值 0）对"取较低者"没有信息量，不参与，避免 −inf 污染
                if (rms2 > 0.0 && peakCh_[(size_t)c] > 0.0) {
                    const double drCh = 20.0 * std::log10(peakCh_[(size_t)c] / rms2);
                    r.drDb = drInit ? std::min(r.drDb, drCh) : drCh;
                    drInit = true;
                }
            }
        }

        if (blk_ == 0 || stp_ == 0) return r;
        if (blkPow_.empty()) return r;
        r.valid = true;

        // ---- 积分响度：绝对门 −70 + 相对门（积分−10）----
        // 块响度 −0.691 + 10log10(p) > −70 ⟺ p > 10^((−70+0.691)/10)
        const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
        double ungated = 0.0; size_t nU = 0;
        for (double p : blkPow_) if (p > absGate) { ungated += p; ++nU; }
        if (nU == 0) { r.lufsIntegrated = -300.0; return r; }   // 全素材低于绝对门：测不出
        const double relGate = (ungated / (double)nU) / 10.0;   // （门内初算积分 −10 LU）⟹ 功率 ÷10
        double gated = 0.0; size_t nG = 0;
        for (double p : blkPow_) if (p > absGate && p > relGate) { gated += p; ++nG; }
        r.lufsIntegrated = (nG > 0) ? (-0.691 + 10.0 * std::log10(gated / (double)nG)) : -300.0;

        // ---- LRA：3 s 短时窗 / 100 ms 步进，绝对门 −70 + 相对门（积分−20），P95 − P10 ----
        if (w3_ > 0 && !stPow_.empty()) {
            const double relGateL = std::pow(10.0, (r.lufsIntegrated - 20.0 + 0.691) / 10.0);
            std::vector<double> stL;
            stL.reserve(stPow_.size());
            for (double s : stPow_) {
                const double l = -0.691 + 10.0 * std::log10(s + 1e-300);
                if (l > -70.0 && s > relGateL) stL.push_back(l);
            }
            if (stL.size() >= 2) {
                std::sort(stL.begin(), stL.end());
                r.lra = percentile_sorted(stL, 0.95) - percentile_sorted(stL, 0.10);
            }
        }
        return r;
    }

    bool   ready() const { return channels_ > 0 && fs_ > 0.0; }
    size_t framesSeen() const { return pos_; }
    int    channels() const { return channels_; }
    double fs() const { return fs_; }
    // 已收下的 400 ms 块 / 3 s 窗数量（诊断用）
    size_t blockCount() const { return blkPow_.size(); }
    size_t shortTermCount() const { return stPow_.size(); }

private:
    // 逐帧处理。所有累加都按样本时间顺序进行（见文件头硬约定）：
    // 块边界只决定"何时把已累加的窗口收走"，不改变加法顺序。
    template <class GetVal>
    inline void push_frame(GetVal&& getVal) {
        if (drLen_ > 0) {
            for (int c = 0; c < channels_; ++c) {
                if (!active_[(size_t)c]) continue;
                const double x = getVal(c);
                const double a = std::fabs(x);
                if (a > peakCh_[(size_t)c]) peakCh_[(size_t)c] = a;
                double s = drAcc_[(size_t)c] + x * x;
                if (++drCnt_[(size_t)c] == drLen_) {
                    drMs_[(size_t)c].push_back(s / (double)drLen_);   // 块内平方和 ÷ 块长
                    s = 0.0;
                    drCnt_[(size_t)c] = 0;
                }
                drAcc_[(size_t)c] = s;
            }
        } else {
            for (int c = 0; c < channels_; ++c) {
                if (!active_[(size_t)c]) continue;
                const double a = std::fabs(getVal(c));
                if (a > peakCh_[(size_t)c]) peakCh_[(size_t)c] = a;
            }
        }
        if (lufsEnabled_) {
            // 每声道独立滤波状态 + 环形缓冲（LRA 的 3 s 窗要重算，必须留样本）
            for (int c = 0; c < channels_; ++c) {
                if (!active_[(size_t)c]) continue;
                ring_[(size_t)c][ringPos_] =
                    hp_[(size_t)c].process(shelf_[(size_t)c].process(getVal(c)));
            }
            if (++ringPos_ == ringLen_) ringPos_ = 0;
        }
        ++pos_;
        if (lufsEnabled_) {
            if (pos_ == nextBlkEnd_) { blkPow_.push_back(window_power(blk_)); nextBlkEnd_ += stp_; }
            if (lraEnabled_ && pos_ == nextStEnd_) { stPow_.push_back(window_power(w3_)); nextStEnd_ += stp_; }
        }
    }

    // 最近 len 帧的 Σ_ch (Σ y²/len)。加法顺序与一次性接口逐位相同：
    // 每声道从 0.0 起按时间顺序累加平方，再 s += e/len（声道升序）。
    double window_power(size_t len) const {
        double s = 0.0;
        if (len == 0 || ringLen_ < len) return s;
        for (int c = 0; c < channels_; ++c) {
            if (!active_[(size_t)c]) continue;
            double e = 0.0;
            size_t k = (ringPos_ + ringLen_ - len) % ringLen_;
            const std::vector<double>& ring = ring_[(size_t)c];
            for (size_t i = 0; i < len; ++i) {
                const double v = ring[k];
                e += v * v;
                if (++k == ringLen_) k = 0;
            }
            s += e / (double)len;
        }
        return s;
    }

    int    channels_ = 0;
    double fs_ = 0.0;
    std::vector<char> active_;
    size_t pos_ = 0;

    std::vector<double> peakCh_;
    size_t drLen_ = 0;
    std::vector<double> drAcc_;
    std::vector<size_t> drCnt_;
    std::vector<std::vector<double>> drMs_;   // 每声道每 3 s 块的均方

    size_t blk_ = 0, stp_ = 0, w3_ = 0;
    bool   lufsEnabled_ = false, lraEnabled_ = false;
    size_t ringLen_ = 0, ringPos_ = 0;
    std::vector<std::vector<double>> ring_;
    std::vector<rta::dsp::Biquad> shelf_, hp_;
    size_t nextBlkEnd_ = 0, nextStEnd_ = 0;
    std::vector<double> blkPow_;   // 每 100 ms 一条 400 ms 块功率
    std::vector<double> stPow_;    // 每 100 ms 一条 3 s 短时块功率
};

// 一次性接口：整段平面 double。实现上就是「整段喂给增量接口」，因此两者逐位一致
// （见文件头的硬约定；tests/test_dynamics.cpp 逐样本比对 max|Δ| == 0）。
inline DynamicsResult measure_dynamics(const double* const* ch, int channels, size_t n, double fs) {
    DynamicsResult r;
    if (!ch || channels <= 0 || n == 0 || fs <= 0.0) return r;
    bool any = false;
    for (int c = 0; c < channels; ++c) if (ch[c]) any = true;
    if (!any) { r.peakDbfs = -300.0; return r; }   // 全空指针：旧实现同样只留默认值
    std::vector<char> active((size_t)channels, 0);
    for (int c = 0; c < channels; ++c) active[(size_t)c] = ch[c] ? 1 : 0;
    DynamicsAccumulator acc;
    acc.begin(channels, fs, active.data());
    acc.add_planar(ch, n);
    return acc.finalize();
}

} // namespace rta::analysis
