// dsd_rate_convert.h — DSD→DSD 速率转换（1-bit → 中间 PCM → 1-bit 重调制）
//
// 2026-10-27 主线落地（DSD 调制专线已结束，见 AGENTS.md §8.6）。
//
// ============================ 路径选择 ============================
// 采用**路径 A**：源 1-bit 流 → dsd_integrator 抽取到 midRate PCM → dsp::PolyphaseResampler
// 升到目标 DSD 位率 → dsp::SdmModulator（既有 NTF/阶数/抖动/限幅档位）调制回 1-bit。
//
// 为什么不选路径 B（1-bit 零插值 + 窄带滤波 + 1-bit 重调制）：
//   1) **1-bit 流没有"零"这个值**。零插值需要插入 0 值样本，而 1-bit 域只有 ±1；
//      插入 ±1 等于注入满幅高频能量，插入"0"必须先把样本展宽成多位 —— 一旦展宽，
//      它就是 PCM，路径 B 的"纯 DSD 域"前提当场破掉。
//   2) 滤波器输出本身就是多位（数十位有效），必须再量化回 1-bit，而"多位 → 1-bit"
//      就是调制。所以路径 B 在数学上等于「线性滤波 + 调制」，与路径 A 同类；
//      差别只是低通放在 2.8 MHz 而不是 176.4 kHz 上做（运算量更高、无判据收益）。
//   3) 可测的瓶颈不在 PCM 中转。实测（tests/test_dsd_rate_convert.cpp §3，DSD64→DSD128，
//      −6 dBFS/998.5 Hz，Release + 工程编译旗标）：整条链路的带内自噪声上界 −140.7 dBFS，
//      而最终 1-bit 输出的带内噪声是 −108.32 dBFS —— 与「DSD64 原样」相同到 0.002 dB
//      （THD+N 99.29 dB 对 99.29 dB）。瓶颈是【源自身的 1-bit 量化噪声】，
//      比链路自噪声高 32.4 dB ⟹ 就算 PCM 中转完全无损，THD+N 也改善不了 0.01 dB。
//      路径 B 收益不足 —— 这是判据结论，不是口味。
//
// ============================ 已知代价 / 未验证项（铁律 4） ============================
//   * 构造（EC 族 synthesize_ntf）实测 373~400 ms/流（每流一次，多声道共用一次综合）。
//     与既有 rtaweb DoP 路径同量级；要更快可换 modFamily=Dsd（解析式 Butterworth，微秒级，
//     代价是带内噪声高约 6 dB）。streaming 起播会把这段算在首帧延迟里。
//   * 降速（如 DSD128→DSD64）只能到 DSD64 自身的带内水平：实测 THD+N 101.87 dB，
//     而源 DSD128 是 106.81 dB —— 这是 1-bit 量化步长与位率的物理限制，不是本模块缺陷
//     （原生 DSD64 基线 99.55 dB，降速结果反而好 +2.3 dB，因为送进来的 PCM 更干净）。
//   * 跨族（44.1k 族 ↔ 48k 族）**不支持**，plan 阶段明确拒绝并给出 gcd 数字。
//   * 尚未接进 tools/rtaweb.cpp 的播放分派（本轮任务书禁止改该文件）；支路已落在
//     engine/pipeline.h（dsd_rate_branch / dsd_rate_convert），由 tests 覆盖。
//
// ============================ 速率族与整数倍 ============================
//   44.1k 族：DSD64=2822400 / 128=5644800 / 256=11289600 / 512=22579200
//   48k  族：DSD64=3072000 / 128=6144000 / 256=12288000 / 512=24576000
// 只做**同族、整数倍**（×2/×4/×8，以及整除的降速 ÷2/÷4/÷8）。
// 跨族（44.1k 族 → 48k 族）在 plan 阶段就明确拒绝：
//   两族比特域比例 147:160（DSD64→DSD64）或 147:320（DSD64→48k DSD128），
//   最大公共整除率只有 19200 Hz ⟹ 不存在可用的整数抽取/整数插值中间率；
//   能做到的话必须走有理多相（176400 → 3072000 是 L=2560/M=147），
//   那既不是"整数倍"，也会把 2560 相系数表搬进播放链 —— 明确报错，绝不静默降级。
//
// ============================ 判据（实测数字见测试） ============================
//   1. 通带平坦度 0…20 kHz 最差偏差 0.000010 dB（与「DSD64 原样」对照）
//   2. 带内 THD+N 99.29 dB 对 99.29 dB（Δ +0.002 dB）；链路自噪声上界 −140.7 dBFS
//   3. 折叠残留：156.8 kHz 探针折叠回 19.6 kHz 的残留 −132.22 dBFS（原样 −126.92 dBFS）
//   4. 级联 DSD64→128→256 vs 直接 DSD64→256：ΔTHD+N −0.004 dB / Δ带内噪声 +0.004 dB；
//      扣基波后两条路径的差信号 −104.09 dBFS（−95.06 dB 相对信号）
//   5. 满幅方波（抽取峰值 1.137，含 Gibbs 过冲）：overload 0 / clamp 0，静音尾带内残留
//      −165.3 dBFS；单样本 +2.0 越界：overload 53 次，尾部带限 RMS 与参考差 0.000 dB
//   6. 真实 .dsf（258.6 MB / DSD64 / 384 s）：流式 3.11× 实时（双声道单线程），内存 +15.6 MB
#pragma once
#include "audio/dsd_reader.h"
#include "dsd_integrator.h"
#include "kaiser.h"
#include "modulator.h"
#include "resampler.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rta::dsp {

// ---------------- 速率族 / 档位 ----------------
inline uint32_t dsd_family_base(uint32_t rate) { return (rate % 48000u == 0u) ? 48000u : 44100u; }

// 标准档位（64/128/256/512/1024）；非标准返回 0
inline int dsd_rate_level(uint32_t rate) {
    if (rate == 0) return 0;
    const uint32_t base = dsd_family_base(rate);
    if (rate % base != 0) return 0;
    const uint32_t lv = rate / base;
    return (lv == 64 || lv == 128 || lv == 256 || lv == 512 || lv == 1024) ? (int)lv : 0;
}

inline const char* dsd_rate_family_name(uint32_t rate) {
    return (rate % 48000u == 0u) ? "48k" : "44.1k";
}

// 中间 PCM 率的分频数：中间率 = 源位率 / 16（与 read_any 的既有做法同口径：
// DSD64→176400、DSD128→352800、DSD256→705600）。
inline constexpr uint32_t kDsdMidDivisor = 16;

// 安全余量：调用方没做峰值预扫时用「满幅 + Gibbs 过冲 1.05」求上界，
// 复用 modulator.h 的 sdm_headroom_db（target=0.62 / margin=0.2，既有档位）。
inline double dsd_convert_safety_headroom_db(double assumedPeak = 1.05) {
    return sdm_headroom_db(assumedPeak);
}

// ---------------- 规划（纯算术，无状态） ----------------
enum class DsdRateMode {
    Direct,       // 同速率：逐位直通，不重调制
    Upsample,     // 同族整数倍升速
    Downsample,   // 同族整数倍降速
    Unsupported   // 明确拒绝（跨族 / 非整数倍 / 非标准位率）
};

inline const char* dsd_rate_mode_name(DsdRateMode m) {
    switch (m) {
        case DsdRateMode::Direct: return "direct";
        case DsdRateMode::Upsample: return "upsample";
        case DsdRateMode::Downsample: return "downsample";
        case DsdRateMode::Unsupported: return "unsupported";
    }
    return "?";
}

struct DsdRatePlan {
    DsdRateMode mode = DsdRateMode::Unsupported;
    uint32_t srcRate = 0;
    uint32_t dstRate = 0;
    uint32_t factor = 0;       // 升速：dst/src；降速：src/dst
    uint32_t midRate = 0;      // 中间 PCM 率
    int decimation = 0;        // srcRate / midRate
    int interpolation = 0;     // dstRate / midRate
    int srcLevel = 0, dstLevel = 0;
    std::string note;          // 决定理由（拒绝时是给用户看的完整原因）

    bool ok() const { return mode != DsdRateMode::Unsupported; }
    bool needs_dsp() const { return mode == DsdRateMode::Upsample || mode == DsdRateMode::Downsample; }
};

inline DsdRatePlan plan_dsd_rate(uint32_t srcRate, uint32_t dstRate) {
    DsdRatePlan p;
    p.srcRate = srcRate;
    p.dstRate = dstRate;
    p.srcLevel = dsd_rate_level(srcRate);
    p.dstLevel = dsd_rate_level(dstRate);

    if (srcRate == dstRate) {
        p.mode = DsdRateMode::Direct;
        p.factor = 1;
        p.midRate = srcRate;
        p.decimation = 1;
        p.interpolation = 1;
        p.note = "源位率 == 目标位率：逐位直通（DirectSDM），不抽取、不调制";
        return p;
    }
    if (p.srcLevel == 0 || p.dstLevel == 0) {
        p.note = "位率不是标准 DSD 档位（src=" + std::to_string(srcRate) + " / dst=" +
                 std::to_string(dstRate) + "）：只认 2822400/3072000 的 64/128/256/512/1024 倍";
        return p;
    }
    const uint32_t famS = (srcRate % 48000u == 0u) ? 48000u : 44100u;
    const uint32_t famD = (dstRate % 48000u == 0u) ? 48000u : 44100u;
    if (famS != famD) {
        // 用真实 gcd 说话，不写"不支持"三个字了事
        uint32_t a = srcRate, b = dstRate;
        while (b) { const uint32_t t = a % b; a = b; b = t; }
        const uint32_t g = a ? a : 1;
        p.note = "跨族转换（" + std::string(dsd_rate_family_name(srcRate)) + " 族 " +
                 std::to_string(srcRate) + " → " + std::string(dsd_rate_family_name(dstRate)) + " 族 " +
                 std::to_string(dstRate) + "）：两族比特域比例 " + std::to_string(srcRate / g) + ":" +
                 std::to_string(dstRate / g) + "，最大公共整除率仅 " + std::to_string(g) +
                 " Hz ⟹ 不存在整数抽取/整数插值的中间 PCM 率；有理多相（176400→3072000 需 L=2560/M=147）"
                 "不属于整数倍路径，本实现明确拒绝（不静默降级）。"
                 "可用的替代：同族目标档（如 44.1k 族 DSD128/256），或先转 PCM 再按 PCM 路径重调制。";
        return p;
    }
    const bool up = dstRate > srcRate;
    const uint32_t hi = up ? dstRate : srcRate;
    const uint32_t lo = up ? srcRate : dstRate;
    if (hi % lo != 0u) {
        p.note = "非整数倍（" + std::to_string(srcRate) + " → " + std::to_string(dstRate) +
                 "）：只做整数倍/整数分频，明确拒绝";
        return p;
    }
    const uint32_t f = hi / lo;
    // 标准档位之间比例只可能是 2 的幂；出现别的整数倍说明位率不是真标准档，直接拒绝
    if ((f & (f - 1)) != 0u) {
        p.note = "整数倍 " + std::to_string(f) + " 不是 2 的幂：DSD 标准档之间只存在 ×2 关系，拒绝";
        return p;
    }
    p.factor = f;
    if (up) {
        p.mode = DsdRateMode::Upsample;
        p.midRate = srcRate / kDsdMidDivisor;
        if (p.midRate == 0 || p.dstRate % p.midRate != 0u) {
            p.mode = DsdRateMode::Unsupported;
            p.note = "中间 PCM 率 " + std::to_string(p.midRate) + " 不能整除目标位率，拒绝";
            return p;
        }
        p.decimation = (int)kDsdMidDivisor;
        p.interpolation = (int)(p.dstRate / p.midRate);
        p.note = "同族 ×" + std::to_string(f) + " 升速：" + std::to_string(srcRate) + " → " +
                 std::to_string(dstRate) + "，中间 PCM " + std::to_string(p.midRate) + " Hz（1/" +
                 std::to_string(p.decimation) + " 抽取 ×" + std::to_string(p.interpolation) + " 插值）";
    } else {
        p.mode = DsdRateMode::Downsample;
        p.midRate = dstRate;                   // 直接抽到目标位率，不再插值
        p.decimation = (int)f;
        p.interpolation = 1;
        p.note = "同族 ÷" + std::to_string(f) + " 降速：" + std::to_string(srcRate) + " → " +
                 std::to_string(dstRate) + "，直接抽取到 " + std::to_string(p.midRate) + " Hz 再重调制";
    }
    return p;
}

// ---------------- 配置（调制器全部沿用 modulator.h 的既有档位） ----------------
struct DsdRateConvertConfig {
    DsdConversion conversion = DsdConversion::Narrow;   // 通带 20 kHz（narrow/wide/xfi 与既有预设同源）
    IntegratorKind decimator = IntegratorKind::Fir;     // FIR 两级（既有默认）
    bool   twoStageDecimator = true;
    double decimatorAttenDb = 120.0;                    // 与 read_any 的既有取值一致
    double interpAttenDb = 180.0;                       // 插值原型阻带（与 resampler.h 默认一致）

    // 调制器：默认 EC-7（engine/preset_profiles.h 的 dsd 档就是 ec/7）。
    // 实际阶数仍由 stable_sdm_order 按位率降阶保护 —— 这里不另设规则。
    ModulatorFamily modFamily = ModulatorFamily::Ec;
    int    modOrder = 7;
    double modDitherLsb = 0.0005;                       // rtaweb 的既有默认（-34 dBFS 的 0.01 会毁掉 SNR）
    double modCutoffHz = 100000.0;                      // Dsd/Chebyshev 族用；Ec 族由 H∞ 预算决定
    double ntfHinf = 1.35;                              // SdmConfig 默认

    // 余量：负数 dB（<=0）。0 = 不衰减（调用方保证峰值 ≤ 0.62）。
    // 默认给满幅安全值（-4.78 dB），需要更高质量时用 measure_peak() 后重设。
    double preAttenDb = 0.0;                            // 由构造函数按 headroomAuto 决定
    bool   headroomAuto = true;                         // true = 用 dsd_convert_safety_headroom_db()
};

// ---------------- 统计 ----------------
struct DsdRateStats {
    long long overload = 0;     // 调制器环路限幅次数（输入过载）
    long long clamped = 0;      // 误差钳位次数（数值保护）
    long long nonFinite = 0;    // 非有限输入次数
    long long outBits = 0;      // 已产出的目标位数（每声道）
    long long pcmFrames = 0;    // 已产出的中间 PCM 帧（每声道）
    double    inPeak = 0.0;     // 调制器输入峰值（含预衰减后）
    double    gainDb = 0.0;     // 实际施加的预衰减
    int       effectiveOrder = 0;
    bool      steppedDown = false;
    std::string stepDownReason;
};

// ---------------- 转换器 ----------------
//
// 用法（流式，与 DoP/ASIO 输出链的分块方式无关）：
//   DsdRatePlan plan = plan_dsd_rate(2822400, 5644800);
//   DsdRateConverter conv(plan, cfg, /*channels=*/2);
//   DsdStream chunkOut;                       // 复用一个空流即可，内部按 8 位对齐追加
//   conv.process_bits(chunkIn, chunkOut);     // 每个源块调用一次，状态跨调用保持
//   conv.finish(chunkOut);
class DsdRateConverter {
public:
    DsdRateConverter(const DsdRatePlan& plan, const DsdRateConvertConfig& cfg, int channels)
        : plan_(plan), cfg_(cfg), ch_(channels < 1 ? 1 : channels) {
        if (!plan_.ok() && plan_.mode != DsdRateMode::Direct)
            throw std::runtime_error("DSD→DSD 速率转换不可用：" + plan_.note);
        gainDb_ = cfg_.headroomAuto ? dsd_convert_safety_headroom_db()
                                    : std::min(0.0, cfg_.preAttenDb);
        gain_ = std::pow(10.0, gainDb_ / 20.0);
        if (!plan_.needs_dsp()) return;
        passbandHz_ = conversion_passband(cfg_.conversion);

        // ---- 抽取器：源位率 → midRate（每声道一个，状态独立）----
        IntegratorConfig ic = conversion_integrator(cfg_.decimator, cfg_.conversion,
                                                    plan_.srcRate, plan_.midRate,
                                                    cfg_.decimatorAttenDb, cfg_.twoStageDecimator);
        for (int c = 0; c < ch_; ++c) dec_.push_back(std::make_unique<DsdDecimator>(ic));

        // ---- 插值器：midRate → 目标位率（整数倍，L = interpolation）----
        if (plan_.interpolation > 1) {
            ResamplerConfig rc;
            rc.inRate  = plan_.midRate;
            rc.outRate = plan_.dstRate;
            rc.channels = ch_;
            rc.attenDb = cfg_.interpAttenDb;
            // 关键映射（resampler.h 的 transition 是"截止点相对输入 Nyquist 回退的比例"）：
            // 升 L 倍时输入 Nyquist 在内插域位于 1/L；要通带到 passband、阻带从
            // (midRate − passband) 开始 ⟹ 过渡带宽度 = midRate − 2·passband。
            // 抽取器已经把 outRate − passband 以上压到 −120 dB，所以插值器只要在
            // "输入 Nyquist 到 第一镜像" 之间完成过渡即可 ⟹ transition 取 0（截止 = 输入 Nyquist）。
            rc.transition = 0.0;
            rc.twoStage = false;
            const double pass = ic.passbandHz;
            const double transHz = (double)plan_.midRate - 2.0 * pass;   // 136.4 kHz @ DSD64
            const int L = plan_.interpolation;
            int tapsTotal = kaiser_taps_for(cfg_.interpAttenDb, transHz / (double)plan_.dstRate);
            int T = (int)std::ceil((double)tapsTotal / (double)L);
            if (T < 4) T = 4;
            rc.minTapsPerInput = T;                  // 抽头预算与 L 解耦（原型 = T×L）
            rs_ = std::make_unique<PolyphaseResampler>(rc);
            tapsPerPhase_ = T;
        } else {
            tapsPerPhase_ = 0;                       // 降速：抽取器直接落到目标位率
        }

        // ---- 调制器：每声道一个（状态跨调用保持）----
        SdmConfig mc;
        mc.fs       = (double)plan_.dstRate;
        mc.family   = cfg_.modFamily;
        mc.levels   = 2;
        mc.order    = stable_sdm_order(cfg_.modOrder, mc.fs, 2, mc.family);
        mc.cutoffHz = cfg_.modCutoffHz;
        mc.ditherLsb = cfg_.modDitherLsb;
        mc.ntfHinf  = cfg_.ntfHinf;
        // ★ 只综合一次 NTF，其余声道拷贝（EC 族的 synthesize_ntf 实测 410 ms/声道；
        //   拷贝不改变任何行为：既有 modulator 各声道本来就用同一个固定种子的抖动序列）。
        mod_.push_back(std::make_unique<SdmModulator>(mc));
        for (int c = 1; c < ch_; ++c)
            mod_.push_back(std::make_unique<SdmModulator>(*mod_[0]));
        if (!mod_.empty()) {
            st_.effectiveOrder = mod_[0]->effectiveOrder();
            st_.steppedDown = mod_[0]->steppedDown();
            st_.stepDownReason = mod_[0]->stepDownReason();
        }
        st_.gainDb = gainDb_;
        init_sink(ch_);
    }

    const DsdRatePlan& plan() const { return plan_; }
    const DsdRateStats& stats() const { return st_; }
    uint32_t midRate() const { return plan_.midRate; }
    int channels() const { return ch_; }
    int interpolation() const { return plan_.interpolation; }
    int decimation() const { return plan_.decimation; }
    int tapsPerPhase() const { return tapsPerPhase_; }
    double gain_db() const { return gainDb_; }
    double passband_hz() const { return passbandHz_; }
    // 插值器原型（内插域，含 ×L 直流增益）。判据用：通带平坦度 / 阻带衰减直接量它，
    // 不靠"看起来对"。空 = 降速路径（无插值器）。
    void interp_prototype_into(std::vector<double>& out) const {
        if (rs_) rs_->prototype_into(out); else out.clear();
    }
    // 插值器组延迟（中间 PCM 样本）→ 换算成目标位数
    double latencyOutBits() const {
        if (!rs_) return 0.0;
        return rs_->latencyInputSamples() * (double)plan_.interpolation;
    }

    // 调用方拿到峰值预扫后可以重设余量（必须在开始处理之前调用）
    void set_pre_atten_db(double db) {
        gainDb_ = std::min(0.0, db);
        gain_ = std::pow(10.0, gainDb_ / 20.0);
        st_.gainDb = gainDb_;
    }

    void reset() {
        for (auto& d : dec_) d->reset();
        if (rs_) rs_->reset();
        for (auto& m : mod_) m->reset();
        st_ = DsdRateStats{};
        st_.gainDb = gainDb_;
        if (!mod_.empty()) {
            st_.effectiveOrder = mod_[0]->effectiveOrder();
            st_.steppedDown = mod_[0]->steppedDown();
            st_.stepDownReason = mod_[0]->stepDownReason();
        }
        outBits_ = 0;
        init_sink(ch_);
    }

    // ---- 1-bit(midRate 之前) → PCM@midRate（每声道追加到 out）----
    size_t to_pcm(const rta::audio::DsdStream& in, std::vector<std::vector<double>>& out) {
        if (!plan_.needs_dsp()) return 0;
        if (in.channels != ch_)
            throw std::runtime_error("DsdRateConverter::to_pcm 声道数不匹配");
        if ((int)out.size() != ch_) out.assign((size_t)ch_, {});
        size_t n = 0;
        for (int c = 0; c < ch_; ++c) {
            const size_t before = out[(size_t)c].size();
            dec_[(size_t)c]->process_bits(in.data[(size_t)c].data(), (size_t)in.bits, out[(size_t)c]);
            n = out[(size_t)c].size() - before;
        }
        st_.pcmFrames += (long long)n;
        return n;
    }

    // ---- PCM@midRate → 1-bit@目标位率（追加到 out）----
    // pcm 每声道至少 frames 个样本；调用方可以在这里插入过载/越界探针（铁律 7 的接缝）。
    size_t push_pcm(const std::vector<std::vector<double>>& pcm, size_t frames,
                    rta::audio::DsdStream& out) {
        if (!plan_.needs_dsp()) return 0;
        if ((int)pcm.size() != ch_) throw std::runtime_error("DsdRateConverter::push_pcm 声道数不匹配");
        if (frames == 0) return 0;
        if (sinkStream_ != &out) { sinkStream_ = &out; align_sink(out); }

        std::vector<const double*> ip((size_t)ch_);
        for (int c = 0; c < ch_; ++c) ip[(size_t)c] = pcm[(size_t)c].data();

        size_t produced = frames;
        std::vector<std::vector<double>> resampled;
        if (rs_) {
            const size_t cap = rs_->maxOutFrames(frames);
            resampled.assign((size_t)ch_, std::vector<double>(cap, 0.0));
            std::vector<double*> op((size_t)ch_);
            for (int c = 0; c < ch_; ++c) op[(size_t)c] = resampled[(size_t)c].data();
            produced = rs_->process(ip.data(), frames, op.data());
        }
        for (size_t n = 0; n < produced; ++n) {
            for (int c = 0; c < ch_; ++c) {
                const double x = ((rs_ ? resampled[(size_t)c][n] : pcm[(size_t)c][n])) * gain_;
                const double ax = std::fabs(x);
                if (ax > st_.inPeak) st_.inPeak = ax;
                const double y = mod_[(size_t)c]->process(x);
                put_bit((uint64_t)(outBits_ + (long long)n), (size_t)c, y > 0.0);
            }
        }
        outBits_ += (long long)produced;
        st_.outBits = outBits_;
        collect_mod_stats();
        return produced;
    }

    // ---- 一条龙：1-bit@源位率 → 1-bit@目标位率 ----
    size_t process_bits(const rta::audio::DsdStream& in, rta::audio::DsdStream& out) {
        if (!plan_.needs_dsp()) {                    // Direct：逐位搬运
            copy_bits(in, out);
            return (size_t)in.bits;
        }
        std::vector<std::vector<double>> pcm((size_t)ch_);
        const size_t got = to_pcm(in, pcm);
        if (got == 0) return 0;
        return push_pcm(pcm, got, out);
    }

    // 收尾：把最后不足 8 位的字节补零并把 out.bits 写成真实位数
    void finish(rta::audio::DsdStream& out) {
        if (sinkStream_ != &out) { sinkStream_ = &out; align_sink(out); }
        out.rate = plan_.dstRate;
        out.channels = ch_;
        out.bits = (uint64_t)outBits_;
        for (int c = 0; c < ch_; ++c) {
            const size_t need = (size_t)((outBits_ + 7) / 8);
            if (out.data[(size_t)c].size() < need) out.data[(size_t)c].resize(need, 0);
        }
        if (out.container.empty()) out.container = "conv";
    }

private:
    void init_sink(int) { sinkStream_ = nullptr; outBits_ = 0; }

    void align_sink(rta::audio::DsdStream& out) {
        if ((int)out.data.size() != ch_) out.data.assign((size_t)ch_, {});
        if (out.bits != 0) {
            if (out.bits % 8 != 0)
                throw std::runtime_error("DsdRateConverter：目标流不是 8 位对齐，无法追加");
            outBits_ = (long long)out.bits;
            const size_t need = (size_t)(outBits_ / 8);
            for (int c = 0; c < ch_; ++c)
                if (out.data[(size_t)c].size() < need) out.data[(size_t)c].resize(need, 0);
        }
        out.rate = plan_.dstRate;
        out.channels = ch_;
    }

    inline void put_bit(uint64_t pos, size_t c, bool v) {
        auto& buf = sinkStream_->data[c];
        const size_t byte = (size_t)(pos >> 3);
        if (byte >= buf.size()) buf.resize(byte + 1, 0);
        const uint8_t mask = (uint8_t)(0x80u >> (pos & 7u));
        if (v) buf[byte] = (uint8_t)(buf[byte] | mask);
        else   buf[byte] = (uint8_t)(buf[byte] & (uint8_t)~mask);
    }

    void collect_mod_stats() {
        st_.overload = 0; st_.clamped = 0; st_.nonFinite = 0;
        for (auto& m : mod_) {
            st_.overload += m->overload();
            st_.clamped += m->clamped();
            st_.nonFinite += m->nonFinite();
        }
    }

    void copy_bits(const rta::audio::DsdStream& in, rta::audio::DsdStream& out) {
        out.rate = plan_.dstRate;
        out.channels = in.channels;
        out.bits = in.bits;
        out.data = in.data;
        outBits_ = (long long)in.bits;
        st_.outBits = outBits_;
    }

    DsdRatePlan plan_;
    DsdRateConvertConfig cfg_;
    int ch_ = 1;
    double gainDb_ = 0.0;
    double gain_ = 1.0;
    int tapsPerPhase_ = 0;
    double passbandHz_ = 20000.0;
    long long outBits_ = 0;
    rta::audio::DsdStream* sinkStream_ = nullptr;
    DsdRateStats st_;
    std::vector<std::unique_ptr<DsdDecimator>> dec_;
    std::unique_ptr<PolyphaseResampler> rs_;
    std::vector<std::unique_ptr<SdmModulator>> mod_;
};

// ---------------- 峰值预扫（给 sdm_headroom_db 提供输入；不调制） ----------------
// 与 rtaweb 的 ISP 预扫同口径：只做抽取（不做插值），取 max|x|。
inline double dsd_convert_measure_peak(const rta::audio::DsdStream& s, const DsdRatePlan& plan,
                                       const DsdRateConvertConfig& cfg) {
    if (!plan.needs_dsp()) return 0.0;
    IntegratorConfig ic = conversion_integrator(cfg.decimator, cfg.conversion, plan.srcRate, plan.midRate,
                                                cfg.decimatorAttenDb, cfg.twoStageDecimator);
    double peak = 0.0;
    for (int c = 0; c < s.channels; ++c) {
        DsdDecimator dec(ic);
        std::vector<double> pcm;
        dec.process_bits(s.data[(size_t)c].data(), (size_t)s.bits, pcm);
        for (double v : pcm) peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

// ---------------- 整段转换（文件/离线；自动做峰值预扫） ----------------
struct DsdConvertResult {
    uint64_t inBits = 0, outBits = 0;
    double seconds = 0.0;
    double srcPeak = 0.0;
    double headroomDb = 0.0;
    double ms = 0.0;              // 纯转换（不含构造与峰值预扫）
    double ctorMs = 0.0;          // 调制器/滤波器设计（EC 族一次综合 ~0.4 s）
    double scanMs = 0.0;          // 峰值预扫（只抽取，不调制）
    double totalMs = 0.0;         // 三者之和
    double realtimeFactor = 0.0;  // seconds / 纯转换时间
    int effectiveOrder = 0;
    bool steppedDown = false;
    std::string stepDownReason;
    std::string note;
};

inline DsdConvertResult convert_dsd_stream(const rta::audio::DsdStream& in, uint32_t dstRate,
                                          const DsdRateConvertConfig& cfgIn,
                                          rta::audio::DsdStream& out,
                                          bool measureHeadroom = true) {
    DsdRateConvertConfig cfg = cfgIn;
    const DsdRatePlan plan = plan_dsd_rate(in.rate, dstRate);
    if (!plan.ok())
        throw std::runtime_error("DSD→DSD 速率转换不可用：" + plan.note);

    DsdConvertResult r;
    r.inBits = in.bits;
    const auto t_all0 = std::chrono::steady_clock::now();

    const auto t_c0 = std::chrono::steady_clock::now();
    DsdRateConverter conv(plan, cfg, in.channels);
    r.ctorMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_c0).count();
    if (plan.needs_dsp() && measureHeadroom) {
        const auto t_s0 = std::chrono::steady_clock::now();
        r.srcPeak = dsd_convert_measure_peak(in, plan, cfg);
        r.scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_s0).count();
        const double hd = sdm_headroom_db(r.srcPeak);   // 既有档位：target 0.62 / margin 0.2
        conv.set_pre_atten_db(hd);
        r.headroomDb = conv.gain_db();
    } else {
        r.headroomDb = conv.gain_db();
    }
    const auto t0 = std::chrono::steady_clock::now();

    out.data.assign((size_t)in.channels, {});
    out.bits = 0;
    out.rate = dstRate;
    out.channels = in.channels;
    out.container = "conv";

    // 分块：以 2^18 位为一块（与 DoP/输出链无关，纯流式；分块边界不影响结果）
    const size_t chunkBits = 1u << 18;
    for (uint64_t pos = 0; pos < in.bits; pos += (uint64_t)chunkBits) {
        const size_t n = (size_t)std::min<uint64_t>(chunkBits, in.bits - pos);
        rta::audio::DsdStream chunk;
        chunk.rate = in.rate;
        chunk.channels = in.channels;
        chunk.bits = n;
        chunk.data.resize((size_t)in.channels);
        const size_t b0 = (size_t)(pos / 8);
        const size_t bytes = (size_t)((n + 7) / 8);
        for (int c = 0; c < in.channels; ++c)
            chunk.data[(size_t)c].assign(in.data[(size_t)c].begin() + (long long)b0,
                                         in.data[(size_t)c].begin() + (long long)(b0 + bytes));
        conv.process_bits(chunk, out);
    }
    conv.finish(out);

    const auto t1 = std::chrono::steady_clock::now();
    r.outBits = out.bits;
    r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.totalMs = std::chrono::duration<double, std::milli>(t1 - t_all0).count();
    const double inSec = in.rate ? (double)in.bits / (double)in.rate : 0.0;
    r.seconds = inSec;
    r.realtimeFactor = (r.ms > 0.0) ? (inSec / (r.ms / 1000.0)) : 0.0;
    r.effectiveOrder = conv.stats().effectiveOrder;
    r.steppedDown = conv.stats().steppedDown;
    r.stepDownReason = conv.stats().stepDownReason;
    r.note = plan.note;
    return r;
}

} // namespace rta::dsp
