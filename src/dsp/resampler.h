// resampler.h — 多相 sinc 重采样引擎（任意有理比率 L/M）
//
// 设计要点：
//  * 原型 FIR 在"内插域"设计，长度为 L 的整数倍，按相位拆分以获得 O(T) 每输出样本开销
//  * 浮点路径用 double；整数 PCM 可选 int64 系数 / int128 累加（最终转换仍有舍入）
//  * 支持任意 inRate/outRate（自动约分为最简 L/M），支持多声道
//  * inRate == outRate 时按位透传（比特完美）
#pragma once
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（见该头注释）
#include "resampler_simd.h"
#include "parallel.h"
#include <array>
#include "gpu_pref.h"
#include "filter_design.h"
#include "filter_families.h"
#include "linear_quality.h"
#include "fixed_resampler_simd.h"
#include "fixed_conv.h"   // D-11：定点卷积（精确累加）
#include "platform/dynlib.h"   // GPU 升频按需加载 rtagpu.dll —— 本目录不再直接碰平台 API
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <numeric>
#include <algorithm>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>

namespace rta::dsp {

struct ResamplerConfig {
    uint32_t inRate   = 44100;
    uint32_t outRate  = 384000;
    int      channels = 2;

    // 原型总抽头数（内插域），会向上取整到 L 的整数倍。
    // HQPlayer 的 poly-sinc 家族量级：65536（-short）/ 262144（-long）/ 1048576+（long/sinc-M）
    int      totalTaps  = 262144;
    double   attenDb    = 180.0;   // 目标阻带衰减
    // 过渡带占比：截止点相对理想 Nyquist 回退的比例（0 = 砖墙理想，越大地越平缓）
    double   transition = 0.04;

    // ---- 命名滤波器（Round 56）----
    // 原先重采样器只会设计 Kaiser 窗 sinc，选不到手册里的滤波器家族。
    // 这里不传"原型数据"而只传【窗类型 + 相位类型】，让重采样器自己在
    // 【内插域】里按正确截止点与 ×L 直流增益完成设计 —— 从而根本避开
    // "输入域原型 vs 内插域原型"归一化基准不同这个陷阱（见 STATUS.md）。
    WindowKind window       = WindowKind::Kaiser;   // Kaiser = 原有行为，完全等价
    PhaseKind  phase        = PhaseKind::Linear;

    // ---- 切趾滤波（Apodizing）----
    bool   apodizing   = false;
    double apodF1      = 0.0;   // 滚降起始相对输入 Nyquist 比例（例如 18.4k / 22.05k）
    double apodF2      = 0.0;   // 滚降终止相对输入 Nyquist 比例（例如 21.8k / 22.05k）

    // Cascaded integer interpolation: stage one sets the source band;
    // stage two removes its images at a higher rate. Throughput is measured,
    // not inferred from tap counts alone.
    bool twoStage   = false;
    int  stage2Taps = 64;      // 第二级每相位抽头数（过渡带很宽，不需要长）

    // ---- 每输入样本内核长度（tapsPerInput）----
    // 0（默认）= 关闭：抽头预算仍按 totalTaps 解释，行为与历史逐位一致。
    //
    // 为什么需要它：原型是在【内插域】设计的，每相位抽头数 T = totalTaps / L。
    // 同一个 totalTaps，L 越大 T 越小 —— 44.1k->192k 的 L=640、44.1k->384k 的
    // L=1280 时，4095 抽头的预设只剩 6 / 4 个抽头，通带在 10 kHz 处就掉 1 dB，
    // 滤波器退化成缓坡（实测见 docs/PCM回放质量-实测与改进.md）。而 44.1k->705.6k
    // 的 L=16，同样预设却有 255 个抽头。也就是说"抽头数"这个预算单位本身与
    // 输出采样率耦合，导致同一预设在不同输出率下质量天差地别。
    // >0 时：T 直接取该值（内核长度 = 每输入样本的抽头数），原型长度 = T × L，
    // 于是频响与输出采样率解耦。内存与每输出样本的乘加次数均为 O(T)。
    int minTapsPerInput = 0;
    // Empirical floor only for budget-mode construction. An explicit source
    // tap count takes precedence; stage two receives its own stage2Taps value.
    bool applyTapFloor = true;
    // 内核长度 × L 的上限（防御：L 很大时系数表会膨胀）。0 = 不限制。
    // ★ 2026-10-31（独立审核 2.4.5）：1,048,576 点是**工程预算**，不是数学上限。
    //   审核用 T=1600 + 2,097,152 点实测五率（88200/192000/352800/384000/705600）：
    //     通带边缘 −0.000000820 dB、抽样最差阻带 −283.9 ~ −284.7 dB、**T 恒定**。
    //   T=2877 + 4M 也可行（−289 dB），但运算与内存更高。
    int maxPrototypeTaps = 1 << 21;
    // ★ 2026-10-30（261030-01 D-11）：精确定点卷积路径。
    //   仅在「源为整数 PCM ≤24 bit 且无任何增益处理」时由调用方置 true ——
    //   那种情况下输入样本恰好是 k/2^23，转 int24 无损；整数累加零舍入，系数量化及最终 double 转换仍有误差。
    //   输入不是 24 bit 整数时**必须**保持 false（实测定点反而差 194 dB，见 test_fixed_conv §3b）。
    bool useFixed = false;
    ResamplerCpuKernel cpuKernel = ResamplerCpuKernel::Auto;
    bool cpuParallel = true;
};

// 把 FilterPreset 的全部参数应用到 ResamplerConfig
inline void apply_preset(ResamplerConfig& cfg, const FilterPreset& fp) {
    cfg.totalTaps  = fp.taps;
    cfg.transition = fp.transition;
    // ★ 2026-10-31（D-16）：设计用的阻带衰减不低于家族天花板 —— 与
    //   engine::apply_filter_preset 必须**同源**，否则「应用走一条、测量走另一条」，
    //   两个工具会报两套数字（实测：本函数不改时 filter_report 报 −148.5 dB，UI 写 280 dB）。
    cfg.attenDb    = std::max(fp.attenDb, preset_ceiling_db(fp.window));
    if (fp.phase == PhaseKind::Linear && !fp.apodizing &&
        (fp.window == WindowKind::Kaiser || fp.window == WindowKind::BlackmanHarris))
        cfg.attenDb = linear_kaiser_design_attenuation(cfg.attenDb);
    cfg.window     = fp.window;
    cfg.phase      = fp.phase;
    cfg.twoStage   = fp.twoStage;
    cfg.apodizing  = fp.apodizing;
    cfg.apodF1     = fp.apodF1;
    cfg.apodF2     = fp.apodF2;
    cfg.minTapsPerInput = fp.apodizing
        ? apod_quality_taps(fp.apodF1, fp.apodF2, fp.window == WindowKind::Gaussian,
                            cfg.attenDb, fp.tapsPerInput)
        : fp.tapsPerInput;
    if (fp.twoStage && fp.stage2Taps > 0) cfg.stage2Taps = fp.stage2Taps;
}

// Output purpose is explicit; a high PCM sample rate alone is not DSD.
enum class ResamplePurpose { Pcm, Dsd };
inline void apply_resample_purpose(ResamplerConfig& cfg, ResamplePurpose purpose) {
    if (purpose == ResamplePurpose::Dsd) cfg.twoStage = true;
}

class PolyphaseResampler {
public:
    explicit PolyphaseResampler(const ResamplerConfig& cfg) : cfg_(cfg) {
        // HF-03 构造防御：采样率为 0 时 gcd(0,x)=x 会算出 M_=0（或 L_=0）的退化参数，
        // 随后 maxOutFrames 出 inf/巨值，转 size_t 触发裸的 "vector too long"（2.log 症状之一）。
        // 在入口直接拒绝，错误带两率值。
        if (cfg.inRate == 0 || cfg.outRate == 0)
            throw std::runtime_error("重采样器构造失败: 采样率必须>0 (inRate=" + std::to_string(cfg.inRate) +
                                     ", outRate=" + std::to_string(cfg.outRate) + ")");
        uint32_t g = std::gcd(cfg.inRate, cfg.outRate);
        L_ = (int)(cfg.outRate / g);          // 上采样因子（每输入样本产生 L 个相位）
        M_ = (int)(cfg.inRate  / g);          // 下采样因子
        ch_ = cfg.channels > 0 ? cfg.channels : 2;

        // 0 值路径已被上面挡住；这里再防 (int) 截断/溢出导致的非正退化值
        if (L_ <= 0 || M_ <= 0)
            throw std::runtime_error("重采样器构造失败: L/M 计算退化 (inRate=" + std::to_string(cfg.inRate) +
                                     ", outRate=" + std::to_string(cfg.outRate) +
                                     ", L=" + std::to_string(L_) + ", M=" + std::to_string(M_) + ")");

        if (L_ == 1 && M_ == 1) { passthrough_ = true; return; }

        // ---- 两段式：只在"纯升频且倍数够大"时启用 ----
        if (cfg.twoStage && M_ == 1 && L_ >= 8) {
            int F = 1;
            for (int f : { 4, 3, 2 }) if (L_ % f == 0 && L_ / f >= 2) { F = f; break; }
            if (F > 1) {
                // HF-03：两级倍数同样防 0/溢出（中间速率 = inRate*F 必须>0 且未回绕）
                const uint32_t mid = cfg.inRate * (uint32_t)F;
                if (mid == 0 || mid / (uint32_t)F != cfg.inRate)
                    throw std::runtime_error("重采样器构造失败: 两段式倍率计算溢出/退化 (inRate=" +
                                             std::to_string(cfg.inRate) + ", outRate=" +
                                             std::to_string(cfg.outRate) + ", F=" + std::to_string(F) + ")");
                // A source-domain tap count belongs to the first stage only.
                // The wide-band second stage has a separate explicit budget.
                const int T1 = (cfg.minTapsPerInput > 0) ? cfg.minTapsPerInput
                                                        : std::max(4, cfg.totalTaps / L_);
                ResamplerConfig c1 = cfg;
                c1.twoStage   = false;
                // c1 也要吃下限：否则单级（有下限）与两段式（无）会不一致。
                c1.applyTapFloor = true;
                c1.inRate     = cfg.inRate;
                c1.outRate    = cfg.inRate * (uint32_t)F;
                c1.totalTaps  = T1 * F;
                ResamplerConfig c2 = cfg;
                c2.twoStage   = false;
                c2.applyTapFloor = true;
                c2.inRate     = cfg.inRate * (uint32_t)F;
                c2.outRate    = cfg.outRate;
                c2.totalTaps  = cfg.stage2Taps * (L_ / F);
                // Stage two receives the already filtered floating-point output.
                // Reusing the input tap floor ignores stage2Taps, and reusing
                // useFixed quantizes that intermediate signal back to int24.
                c2.minTapsPerInput = cfg.stage2Taps;
                c2.useFixed = false;
                // 第二级的过渡带可以很宽：第一级输出在 22.05 kHz 以上已经是 -180 dB，
                // 所以第二级只要在"图像出现处"之前进入阻带即可。
                c2.transition = 0.30;
                stage1_ = std::make_unique<PolyphaseResampler>(c1);
                stage2_ = std::make_unique<PolyphaseResampler>(c2);
                tmp_.clear();
                tmpP_.assign((size_t)ch_, nullptr);
                return;
            }
        }

        // 内插域中：输入 Nyquist 位于 1/L，输出 Nyquist 位于 1/M，取较小者
        const double idealCutoff = std::min(1.0 / (double)L_, 1.0 / (double)M_);
        // 【261030-01 规格 §4.5 修正】截止点（−6 dB 点）必须放在**过渡带中点**，不是通带边缘。
        //
        // 旧写法 cutoff = idealCutoff × (1 − t) 把 −6 dB 点压在通带边缘上：
        //   通带在 0.95×pass 处就已经掉 −1.9 dB（实测 E-8，T=74 时），
        //   44.1k→384k 那档甚至掉 −4.9 dB —— 那不是滤波器，是一条缓坡。
        // 新写法把 −6 dB 点移到 pass + Δf/2：
        //   通带 0.95×pass 处实测 −0.0000 dB，镜像抑制同时达标（filter_sweep 5 组 × 79 预设验证）。
        //
        // ⚠ 这会让所有预设的频响相对旧版整体上移（通带更平），是**有意的行为变更**，
        //   不是回归。tests/test_filters 的容差基线随之更新。
        const double cutoff = idealCutoff * (1.0 - cfg.transition * 0.5);

        // 原型长度取 L 的整数倍
        int total = cfg.totalTaps;
        T_ = total / L_;                       // 每相位抽头数
        // Historical budget mode; production auto mode supplies an explicit T
        // below. Do not infer an arithmetic precision ceiling from this heuristic.
        if (cfg.transition > 0.0 && cfg.transition < 1.0) {
            int tMin  = (int)(21.13 / cfg.transition + 0.5);      // 下限：过渡带至少要够
            int tMax  = (int)(2.0 * 21.13 / cfg.transition + 0.5); // 上限：留 2 倍余量
            if (tMin < 4) tMin = 4;
            if (tMax < tMin) tMax = tMin;
            if (cfg.phase == PhaseKind::Linear && T_ > tMax) T_ = tMax;
            if (cfg.applyTapFloor && T_ < tMin) T_ = tMin;
        }
        if (cfg.minTapsPerInput > 0) {
            // 内核长度模式：抽头预算与 L 解耦（默认关闭时这一支永不执行 ⟹ 逐位不变）
            T_ = cfg.minTapsPerInput;
        }
        if (T_ < 4) { T_ = 4; }
        if ((long long)T_ * L_ > std::numeric_limits<int>::max())
            throw std::runtime_error("Requested FIR length exceeds the supported integer range");
        if (cfg.maxPrototypeTaps > 0 && (long long)T_ * L_ > cfg.maxPrototypeTaps)
            throw std::runtime_error("Requested FIR exceeds the coefficient budget; quality was not reduced. Use a cascade or explicitly raise maxPrototypeTaps.");
        total = T_ * L_;

        std::vector<double> proto;
        if (cfg.window == WindowKind::Kaiser && cfg.phase == PhaseKind::Linear && !cfg.apodizing) {
            // 原有路径：参数化 Kaiser sinc（保持逐位不变）
            LowpassSpec spec;
            spec.taps    = total;
            spec.cutoff  = cutoff;
            spec.attenDb = cfg.attenDb;
            proto = design_lowpass(spec);
        } else {
            // 命名滤波器路径：在【同一个内插域截止点】上用指定窗设计，
            // 需要最小相位时走倒谱法转换。截止点约定两侧一致（均以 Nyquist 为单位，
            // 0.5 表示 0.25*fs），因此不存在归一化基准不匹配的问题。
            FirSpec fs;
            fs.taps       = total;
            fs.cutoff     = cutoff;
            fs.attenDb    = cfg.attenDb;
            fs.window     = cfg.window;
            fs.apodizing  = cfg.apodizing;
            if (cfg.apodizing) {
                // idealCutoff 为输入 Nyquist 在内插域的位置（1.0 / L）
                fs.apodF1 = cfg.apodF1 * idealCutoff;
                fs.apodF2 = cfg.apodF2 * idealCutoff;
            }
            // ★ 2026-10-31（审核修复）：相位变换改走**级联构造**。
            //   旧写法「整段 design_fir + 一次倒谱变换」实测 mp 只到 −253 dB、ip 只到 −168 dB，
            //   而同一预设的线性版是 −290 dB —— 这就是审核抓到的「mp/ip 质量缺口」。
            //   design_phased_fir 把长滤波器拆成 2（mp）/ 4（ip）段短滤波器，各自做相位变换再卷积，
            //   实测把 mp 从 −253 提到 −307、ip 从 −168 提到 −290，通带偏差 ≤0.004 dB。
            // 通带边缘（内插域归一化）：apply_preset 里 cutoff = idealCutoff*(1-t/2)，
            //   而 passHz = idealCutoff*(1-t) ⟹ passEdge = cutoff*(1-t)/(1-t/2)。
            const double t = (cfg.transition > 0.0) ? cfg.transition : 0.0;
            const double passEdge = (t > 0.0 && t < 1.0)
                                  ? cutoff * (1.0 - t) / (1.0 - t * 0.5) : 0.0;
            proto = design_phased_fir(fs.taps, fs.cutoff, fs.attenDb, fs.window,
                                      fs.apodizing, fs.apodF1, fs.apodF2,
                                      cfg.phase, phase_mix(), passEdge);
            if ((int)proto.size() != total) proto.resize((size_t)total, 0.0);
        }

        // 关键：有理 L/M 转换中，"内插 -> 滤波 -> 抽取"要求原型在内插域的直流增益为 L，
        // 否则输出会被整体衰减 L 倍（L=1280 时正好 -62.1 dB，实测确认过这个陷阱）。
        // Total prototype gain is L; individual phase sums may contain finite interpolation error.
        const double phaseGain = (double)L_;
        for (auto& v : proto) v *= phaseGain;

        // 相位主序布局：phases_[p][u] = h[p + u*L]，提升缓存友好度
        // D-11：定点路径的系数表（kFixedCoeffBits 位量化）。仅 useFixed 时构建。
        if (cfg.useFixed) {
            phasesFixed_.assign((size_t)L_, std::vector<int64_t>((size_t)T_));
            std::vector<int64_t> flat;
            if (!fixed_coeffs_from_double(proto, flat)) {
                throw std::runtime_error("重采样器构造失败: 定点系数量化越界");
            }
            for (int p = 0; p < L_; ++p)
                for (int u = 0; u < T_; ++u)
                    phasesFixed_[(size_t)p][(size_t)u] = flat[(size_t)(p + u * L_)];
        }
        phases_.assign((size_t)L_, std::vector<double>((size_t)T_));
        for (int p = 0; p < L_; ++p)
            for (int u = 0; u < T_; ++u)
                phases_[(size_t)p][(size_t)u] = proto[(size_t)(p + u * L_)];

        // 2026-10-30 试过「反向相位表 + 正序内积」以利向量化 —— A/B 实测无收益
        // （DSD1024 5.75x vs 5.82x，在噪声内），因为 DSD 高倍率的耗时实际由 GPU kernel 决定。
        // 已回退，不留无用复杂度。

        // GPU 需要连续的 [L][T] 相位表
        phases_flat_.resize((size_t)L_ * (size_t)T_);
        for (int pp = 0; pp < L_; ++pp)
            std::copy(phases_[(size_t)pp].begin(), phases_[(size_t)pp].end(),
                      phases_flat_.begin() + (ptrdiff_t)((size_t)pp * (size_t)T_));
        if (gpu_invalidate_) gpu_invalidate_();   // 换了滤波器 -> 显存里的相位表作废
        hist_.assign((size_t)ch_, std::vector<double>());
        for (auto& hh : hist_) hh.reserve((size_t)T_ * 2);
        outIdx_ = 0;
        inAbs_  = 0;                           // 下一个待接收输入样本的绝对序号
    }

    bool passthrough() const { return passthrough_; }
    int  tapsPerPhase() const { return T_; }
    int stage1_taps_per_phase() const { return stage1_ ? stage1_->tapsPerPhase() : T_; }
    int stage2_taps_per_phase() const { return stage2_ ? stage2_->tapsPerPhase() : 0; }
    size_t coefficient_count() const {
        return stage1_ ? stage1_->coefficient_count() + stage2_->coefficient_count()
                       : size_t(T_) * size_t(L_);
    }

    // 量测/回归钩子：把相位表还原成【内插域原型】(含 ×L 直流增益，与 process()
    // 逐位同源的同一批系数)。用于频响/群延迟/振铃与"既有预设逐位不变"回归；
    // 不参与任何信号路径，也不额外占用内存（按需展开到调用方缓冲）。
    void prototype_into(std::vector<double>& out) const {
        if (stage1_) {
            // Measure the real cascade, including its phase transforms. A fresh
            // CPU instance leaves the live stream and GPU cache untouched.
            ResamplerConfig mono = cfg_; mono.channels = 1;
            PolyphaseResampler probe(mono);
            const size_t frames = (size_t)std::ceil(tailInputSamples()) + 1;
            std::vector<double> impulse(frames, 0.0);
            impulse[0] = 1.0;
            out.resize(probe.maxOutFrames(frames));
            const double* in[] = { impulse.data() };
            double* dst[] = { out.data() };
            probe.process(in, frames, dst);
            out.resize((size_t)std::llround(tailInputSamples() * (double)L_) + 1);
            return;
        }
        out.assign((size_t)T_ * (size_t)L_, 0.0);
        for (int p = 0; p < L_; ++p)
            for (int u = 0; u < T_; ++u)
                out[(size_t)(p + u * L_)] = phases_[(size_t)p][(size_t)u];
    }

    // 是否用 GPU 做多相相乘加（需要 rtagpu.dll；不可用则自动回退 CPU）
    void set_use_gpu(bool on) {
        useGpu_ = on;
        if (stage1_) stage1_->set_use_gpu(on);
        if (stage2_) stage2_->set_use_gpu(on);
        if (on) load_gpu();
    }
    bool gpu_active() const {
        if (stage1_ || stage2_)
            return (stage1_ && stage1_->gpu_active()) || (stage2_ && stage2_->gpu_active());
        return !cfg_.useFixed && useGpu_ && gpu_resample_ != nullptr;
    }
    // Capability and observation are separate: a loaded entry point is not a
    // successful dispatch. Leaf mask=1; cascade bits=first/second stage.
    unsigned gpu_compact_stage_mask_last_block() const { return gpuCompactStageMaskLastBlock_; }
    bool gpu_used_last_block() const { return gpuStageMaskLastBlock_ != 0; }
    unsigned gpu_stage_mask_last_block() const { return gpuStageMaskLastBlock_; }
    bool gpu_all_stages_used_last_block() const {
        return gpuStageMaskLastBlock_ == (stage1_ ? 3u : 1u);
    }
    bool two_stage() const { return stage1_ != nullptr; }
    const ResamplerConfig& config() const { return cfg_; }
    const PolyphaseResampler* first_stage() const { return stage1_.get(); }
    const PolyphaseResampler* second_stage() const { return stage2_.get(); }
    int  stage1_factor() const { return stage1_ ? stage1_->upFactor() : L_; }
    int  upFactor() const { return L_; }
    int  downFactor() const { return M_; }

    // 群延迟（输入样本数）。原型长度是 T_*L_（偶数 ⟹ Type II 线性相位），
    // 其在内插域的群延迟为 (T_*L_ - 1)/2，换算回输入样本需再除以 L_。
    // 曾用 (T_-1)/2，在 L=4 时偏差 3.5 个输出样本 ⟹ 997 Hz 上约 7° 相位误差，
    // 会让"与理想正弦比对"的残差虚高到 −26 dBFS。
    // ★ 2026-10-29（两轮审查 F17）：两段式时 T_/L_ 从未被赋值（构造函数在两级分支里
    //   直接 return），旧式会算出 (0*1-1)/2 = −0.5 ⟹ 排空量算成负数、延迟被当成 0。
    //   两级串起来的总延迟 = 第一级（输入域）+ 第二级（中间域，换算回输入域）。
    double latencyInputSamples() const {
        if (stage1_) {
            const double l1 = stage1_->latencyInputSamples();
            const double l2mid = stage2_->latencyInputSamples();       // 中间域样本数
            const double midRate = (double)stage1_->cfg_.inRate * (double)stage1_->upFactor();
            const double inRate  = (double)cfg_.inRate;
            return l1 + (midRate > 0.0 ? l2mid * inRate / midRate : 0.0);
        }
        if (passthrough_ || cfg_.phase != PhaseKind::Linear) return 0.0;
        return ((double)T_ * (double)L_ - 1.0) / (2.0 * (double)L_);
    }

    // Complete FIR support after the final source sample, in input samples.
    // This is separate from compensable delay. MP/IP have frequency-dependent
    // delay, so trimming a linear group delay destroys their leading content;
    // their natural tail must still be drained at EOF.
    double tailInputSamples() const {
        if (stage1_)
            return stage1_->tailInputSamples() +
                   stage2_->tailInputSamples() / (double)stage1_->upFactor();
        if (passthrough_) return 0.0;
        return ((double)T_ * (double)L_ - 1.0) / (double)L_;
    }

    // 处理一块输入。out 必须至少有 maxOutFrames(inFrames) 容量。
    // in/out 为每声道指针数组。
    size_t process(const double* const* in, size_t inFrames,
                   double* const* out) {
        cpuSimdOutputsLastBlock_ = 0;
        cpuParallelThreadsLastBlock_ = 0;
        clear_gpu_observation();
        if (inFrames == 0) return 0;
        if (inFrames > (unsigned long long)(std::numeric_limits<int64_t>::max() - inAbs_))
            throw std::length_error("Resampler absolute input counter overflow");
        // ---- 两段式：第一级 -> 中间缓冲 -> 第二级 ----
        if (stage1_) {
            const size_t cap = stage1_->maxOutFrames(inFrames);
            if (tmp_.size() < cap * (size_t)ch_) {
                tmp_.assign(cap * (size_t)ch_, 0.0);
                for (int c = 0; c < ch_; ++c) tmpP_[(size_t)c] = tmp_.data() + (size_t)c * cap;
            }
            const size_t n1 = stage1_->process(in, inFrames, tmpP_.data());
            const size_t n2 = stage2_->process(tmpP_.data(), n1, out);
            gpuStageMaskLastBlock_ = (stage1_->gpu_used_last_block() ? 1u : 0u)
                                  | (stage2_->gpu_used_last_block() ? 2u : 0u);
            gpuCompactStageMaskLastBlock_ = (stage1_->gpu_compact_stage_mask_last_block() ? 1u : 0u)
                                         | (stage2_->gpu_compact_stage_mask_last_block() ? 2u : 0u);
            return n2;
        }
        if (passthrough_) {
            for (int c = 0; c < ch_; ++c)
                std::copy(in[c], in[c] + inFrames, out[c]);
            return inFrames;
        }

        // Reject impossible compact counts before touching input/history.
        const bool compactReady = !cfg_.useFixed && useGpu_ && gpu_compact_ &&
                                  M_ == 1 && (L_ & (L_ - 1)) == 0;
        int64_t compactExclusive = 0;
        if (compactReady) {
            const int64_t nextInput = inAbs_ + (int64_t)inFrames;
            if (nextInput > (std::numeric_limits<int64_t>::max() - (M_ - 1)) / L_)
                throw std::length_error("Compact resampler output counter overflow");
            compactExclusive = (nextInput * (int64_t)L_ + M_ - 1) / M_;
            if (compactExclusive - outIdx_ > std::numeric_limits<int>::max())
                throw std::length_error("Compact resampler block exceeds ABI frame range");
        }
        // 历史缓冲中第一个样本的绝对序号（追加新输入之前计算）
        const int64_t firstAbs = inAbs_ - (int64_t)hist_[0].size();

        for (int c = 0; c < ch_; ++c) {
            auto& hh = hist_[(size_t)c];
            hh.insert(hh.end(), in[c], in[c] + inFrames);
        }
        const int64_t lastAbs = inAbs_ + (int64_t)inFrames - 1;
        inAbs_ += (int64_t)inFrames;

        // ---- 1) 先收集本块所有输出样本的 (base, phase) ----
        bases_.clear(); phs_.clear();
        const int64_t firstOut = outIdx_;
        auto makeIndices = [&](size_t count) {
            if (bases_.size() == count) return;
            bases_.clear(); phs_.clear();
            for (size_t n = 0; n < count; ++n) {
                const int64_t num = (firstOut + (int64_t)n) * M_;
                bases_.push_back((int)(num / L_ - firstAbs));
                phs_.push_back((int)(num % L_));
            }
        };
        size_t produced;
        if (compactReady) {
            produced = (size_t)std::max<int64_t>(0, compactExclusive - firstOut);
            outIdx_ += (int64_t)produced;
        } else {
            for (;;) {
                const int64_t num = outIdx_ * (int64_t)M_;
                const int64_t i = num / L_;
                if (i > lastAbs) break;
                bases_.push_back((int)(i - firstAbs));
                phs_.push_back((int)(num % L_));
                ++outIdx_;
            }
            produced = bases_.size();
        }
        if (produced == 0) return 0;

        // ---- 2) 计算 ----
        bool gpuDone = false;
        if (!cfg_.useFixed && useGpu_ && gpu_resample_) {
            const size_t hl = hist_[0].size();
            if (hl > 0) {
                flat_.resize((size_t)ch_ * hl);
                for (int c = 0; c < ch_; ++c)
                    std::copy(hist_[(size_t)c].begin(), hist_[(size_t)c].end(),
                              flat_.begin() + (ptrdiff_t)((size_t)c * hl));
                if (compactReady) {
                    const size_t capacity = maxOutFrames(inFrames);
                    if (produced > capacity || capacity > (size_t)std::numeric_limits<long long>::max())
                        throw std::length_error("Compact resampler output capacity overflow");
                    const int rc = gpu_compact_(flat_.data(), ch_, (long long)hl,
                        phases_flat_.data(), L_, M_, T_, firstOut, firstAbs,
                        (long long)produced, (long long)capacity, out);
                    if (rc == (int)produced) {
                        gpuDone = true; gpuStageMaskLastBlock_ = 1u; gpuCompactStageMaskLastBlock_ = 1u;
                    }
                }
                if (!gpuDone) {
                    makeIndices(produced);
                    inter_.resize(produced * (size_t)ch_);
                    const int rc = gpu_resample_(flat_.data(), ch_, (long long)hl,
                        phases_flat_.data(), L_, T_, bases_.data(), phs_.data(),
                        (long long)produced, inter_.data());
                    if (rc == (int)produced) {
                        for (size_t n = 0; n < produced; ++n)
                            for (int c = 0; c < ch_; ++c)
                                out[c][n] = inter_[n * (size_t)ch_ + (size_t)c];
                        gpuDone = true; gpuStageMaskLastBlock_ = 1u;
                    }
                }
            }
        }

        if (!gpuDone) makeIndices(produced);
        // ---- D-11 定点路径（精确累加）----
        // 前提由调用方保证：输入样本恰好是 k/2^23（整数 PCM ≤24 bit 且无增益处理）。
        // 那种情况下 double→int24 无损，整条卷积零舍入；否则**必须**走下面的 double 路径。
        if (!gpuDone && cfg_.useFixed && !phasesFixed_.empty()) {
            bool fixedInputSafe = true;
            histFixed_.resize((size_t)ch_);
            for (int c = 0; c < ch_; ++c) {
                const auto& hh = hist_[(size_t)c];
                auto& hf = histFixed_[(size_t)c];
                hf.resize(hh.size());
                for (size_t i = 0; i < hh.size(); ++i) {
                    hf[i] = (int64_t)std::llround(hh[i] * (double)kFixedInputOne);
                    // Legacy callers may set useFixed for out-of-range inputs.
                    // Preserve their int128 scalar behavior; never truncate to int32.
                    if (!std::isfinite(hh[i]) || hh[i] < -1.0 || hh[i] > 1.0 ||
                        hf[i] < -kFixedInputOne || hf[i] > kFixedInputOne)
                        fixedInputSafe = false;
                }
            }
            const bool fixedAvx = fixedInputSafe &&
                cfg_.cpuKernel != ResamplerCpuKernel::Scalar && resampler_avx2_available();
            const bool fixedPhase = fixedAvx && M_ == 1 && L_ >= 4;
            const bool fixedTemporal = fixedAvx && M_ > 1;
            if (fixedPhase && phases_fixed_tap_major_.empty()) {
                phases_fixed_tap_major_.resize((size_t)T_ * (size_t)L_);
                for (int u = 0; u < T_; ++u)
                    for (int p = 0; p < L_; ++p)
                        phases_fixed_tap_major_[(size_t)u * L_ + p] = phasesFixed_[p][u];
            }
            auto fixedScalarOutput = [&](size_t n) {
                const size_t base = (size_t)bases_[n];
                const int p = phs_[n];
                const int uEnd = (base + 1 < (size_t)T_) ? (int)(base + 1) : T_;
                for (int c = 0; c < ch_; ++c) {
                    const auto& hf = histFixed_[(size_t)c];
                    const auto& ph = phasesFixed_[(size_t)p];
                    __int128 acc = 0;
                    for (int u = 0; u < uEnd; ++u)
                        acc += (__int128)ph[(size_t)u] * (__int128)hf[base - (size_t)u];
                    out[c][n] = fixed_to_double(acc);
                }
            };
            auto fixedRange = [&](size_t begin, size_t end) {
                size_t count = 0;
                for (size_t n = begin; n < end; ++n) {
                    const size_t base = (size_t)bases_[n];
                    const int p = phs_[n];
                    if (fixedPhase && p + 4 <= L_ && end - n >= 4) {
                        const size_t taps = std::min(base + 1, (size_t)T_);
                        for (int c = 0; c < ch_; ++c) {
                            __int128 acc[4];
                            const bool used = fixed_resampler_simd::phase4(
                                phases_fixed_tap_major_.data() + p, (size_t)L_,
                                histFixed_[c].data(), base, taps, acc);
                            for (size_t lane = 0; lane < 4; ++lane)
                                out[c][n + lane] = fixed_to_double(acc[lane]);
                            if (used) count += 4;
                        }
                        n += 3;
                    } else fixedScalarOutput(n);
                }
                return count;
            };
            auto fixedPhaseRange = [&](size_t begin, size_t end) {
                size_t count = 0;
                for (size_t q = begin; q < end; ++q) {
                    size_t n = q;
                    while (n < produced) {
                        const size_t base = (size_t)bases_[n];
                        const int p = phs_[n];
                        const size_t remaining = 1 + (produced - 1 - n) / (size_t)L_;
                        if (base >= (size_t)(T_ - 1) && remaining >= 4) {
                            for (int c = 0; c < ch_; ++c) {
                                __int128 acc[4];
                                const bool used = fixed_resampler_simd::temporal4(
                                    phasesFixed_[p].data(), histFixed_[c].data(),
                                    base, (size_t)T_, (size_t)M_, acc);
                                for (size_t lane = 0; lane < 4; ++lane)
                                    out[c][n + lane * (size_t)L_] = fixed_to_double(acc[lane]);
                                if (used) count += 4;
                            }
                            n += 4 * (size_t)L_;
                        } else {
                            fixedScalarOutput(n);
                            n += (size_t)L_;
                        }
                    }
                }
                return count;
            };
            if ((fixedPhase || fixedTemporal) && cfg_.cpuParallel && produced >= 512 &&
                (long double)produced * T_ * ch_ >= 2000000.0L) {
                const size_t work = fixedTemporal ? std::min(produced, (size_t)L_) : produced;
                const size_t tasks = std::min({size_t(8), hardware_threads(), work});
                std::array<size_t, 8> counts{};
                std::array<std::thread::id, 8> ids{};
                ParallelFor::instance().run(tasks, [&](size_t task) {
                    const size_t begin = (work / tasks) * task + std::min(task, work % tasks);
                    const size_t end = begin + work / tasks + (task < work % tasks);
                    ids[task] = std::this_thread::get_id();
                    counts[task] = fixedTemporal ? fixedPhaseRange(begin, end) : fixedRange(begin, end);
                });
                for (size_t task = 0; task < tasks; ++task) {
                    cpuSimdOutputsLastBlock_ += counts[task];
                    bool first = true;
                    for (size_t prior = 0; prior < task; ++prior)
                        if (ids[prior] == ids[task]) first = false;
                    if (first) ++cpuParallelThreadsLastBlock_;
                }
            } else {
                cpuSimdOutputsLastBlock_ = fixedTemporal
                    ? fixedPhaseRange(0, std::min(produced, (size_t)L_)) : fixedRange(0, produced);
                cpuParallelThreadsLastBlock_ = produced ? 1 : 0;
            }
            gpuDone = true;   // 复用标志：跳过下面的 double 路径
        }

        // CPU 路径（也是 GPU 不可用时的回退）
        if (!gpuDone) {
            const bool avx = cfg_.cpuKernel != ResamplerCpuKernel::Scalar && resampler_avx2_available();
            const bool simd = M_ == 1 && L_ >= 4 && avx;
            const bool temporal = M_ > 1 && avx;
            if (simd && phases_tap_major_.empty()) {
                phases_tap_major_.resize((size_t)T_ * (size_t)L_);
                for(int u=0;u<T_;++u) for(int p=0;p<L_;++p)
                    phases_tap_major_[(size_t)u*L_+p]=phases_[(size_t)p][(size_t)u];
            }
            auto range = [&](size_t begin,size_t end) {
            size_t simdOutputs=0;
            for (size_t n = begin; n < end; ++n) {
                const size_t base = (size_t)bases_[n];
                const int p = phs_[n];
                const int uEnd = (base + 1 < (size_t)T_) ? (int)(base + 1) : T_;
#if RTA_RESAMPLER_AVX2
                if(simd && p+4<=L_ && n+4<=end) {
                    const size_t width=(p+16<=L_ && n+16<=end) ? 16 :
                                       (p+8<=L_ && n+8<=end) ? 8 : 4;
                    for(int c=0;c<ch_;++c)
                        if(width==16)resampler_phase_group<4>(phases_tap_major_.data()+p,(size_t)L_,hist_[(size_t)c].data(),base,uEnd,out[c]+n);
                        else if(width==8)resampler_phase_group<2>(phases_tap_major_.data()+p,(size_t)L_,hist_[(size_t)c].data(),base,uEnd,out[c]+n);
                        else resampler_phase_group<1>(phases_tap_major_.data()+p,(size_t)L_,hist_[(size_t)c].data(),base,uEnd,out[c]+n);
                    simdOutputs += width*(size_t)ch_;
                    n+=width-1;
                    continue;
                }
#endif
                for (int c = 0; c < ch_; ++c) {
                    const auto& hh = hist_[(size_t)c];
                    const auto& ph = phases_[(size_t)p];
                    double acc = 0.0;
                    for (int u = 0; u < uEnd; ++u) acc += ph[(size_t)u] * hh[base - (size_t)u];
                    out[c][n] = acc;
                }
            }
            return simdOutputs;
            };
            // Rational ratios recur at the same phase every L outputs. Group
            // those independent outputs, keeping each lane's accumulation order.
            auto phaseRange = [&](size_t begin,size_t end) {
                size_t count=0;
                for(size_t q=begin;q<end;++q) {
                    size_t n=q;
                    while(n<produced) {
                        const size_t base=size_t(bases_[n]);
                        const int p=phs_[n];
#if RTA_RESAMPLER_AVX2
                        const size_t remaining=1+(produced-1-n)/size_t(L_);
                        if(base>=size_t(T_-1) && remaining>=4) {
                            const size_t width=remaining>=16 ? 16 : remaining>=8 ? 8 : 4;
                            for(int c=0;c<ch_;++c) {
                                if(width==16)resampler_temporal_group<4>(phases_[p].data(),hist_[c].data(),base,T_,size_t(M_),out[c]+n,size_t(L_));
                                else if(width==8)resampler_temporal_group<2>(phases_[p].data(),hist_[c].data(),base,T_,size_t(M_),out[c]+n,size_t(L_));
                                else resampler_temporal_group<1>(phases_[p].data(),hist_[c].data(),base,T_,size_t(M_),out[c]+n,size_t(L_));
                            }
                            count+=width*size_t(ch_);n+=width*size_t(L_);continue;
                        }
#endif
                        const int endTap=std::min<size_t>(base+1,size_t(T_));
                        for(int c=0;c<ch_;++c) {
                            double acc=0;
                            for(int u=0;u<endTap;++u)acc+=phases_[p][u]*hist_[c][base-size_t(u)];
                            out[c][n]=acc;
                        }
                        n+=size_t(L_);
                    }
                }
                return count;
            };
            // Independent output partitions share read-only indices, history
            // and coefficients. All writes and counters are task-local until
            // run() completes, before any history mutation below.
            if((simd || temporal) && cfg_.cpuParallel && produced>=(temporal ? 512u : 8192u) &&
               (long double)produced*T_*ch_>=2000000.0L) {
                const size_t work=temporal ? std::min(produced,size_t(L_)) : produced;
                const size_t tasks=std::min({size_t(8),hardware_threads(),work});
                std::array<size_t,8> counts{};
                std::array<std::thread::id,8> ids{};
                ParallelFor::instance().run(tasks,[&](size_t task) {
                    const size_t begin=(work/tasks)*task+std::min(task,work%tasks);
                    const size_t end=begin+work/tasks+(task<work%tasks);
                    ids[task]=std::this_thread::get_id();
                    counts[task]=temporal ? phaseRange(begin,end) : range(begin,end);
                });
                for(size_t task=0;task<tasks;++task) {
                    cpuSimdOutputsLastBlock_+=counts[task];
                    bool first=true;
                    for(size_t prior=0;prior<task;++prior)if(ids[prior]==ids[task])first=false;
                    if(first)++cpuParallelThreadsLastBlock_;
                }
            } else {
                cpuSimdOutputsLastBlock_=temporal ? phaseRange(0,std::min(produced,size_t(L_))) : range(0,produced);
                cpuParallelThreadsLastBlock_=1;
            }
        }

        // ---- 3) 裁剪历史 ----
        for (int c = 0; c < ch_; ++c) {
            auto& hh = hist_[(size_t)c];
            if (hh.size() > (size_t)T_)
                hh.erase(hh.begin(), hh.end() - (ptrdiff_t)T_);
        }
        return produced;
    }

    // 输出缓冲上界
    size_t cpu_simd_outputs_last_block() const {
        if(stage1_) return stage1_->cpu_simd_outputs_last_block()+stage2_->cpu_simd_outputs_last_block();
        return cpuSimdOutputsLastBlock_;
    }
    size_t cpu_parallel_threads_last_block() const {
        if(stage1_)return std::max(stage1_->cpu_parallel_threads_last_block(),stage2_->cpu_parallel_threads_last_block());
        return cpuParallelThreadsLastBlock_;
    }
    size_t maxOutFrames(size_t inFrames) const {
        if (stage1_) return stage2_->maxOutFrames(stage1_->maxOutFrames(inFrames));
        if (passthrough_) return inFrames;
        return (size_t)(((double)inFrames * (double)L_) / (double)M_) + (size_t)L_ + 8;
    }

    void reset() {
        cpuSimdOutputsLastBlock_ = 0;
        cpuParallelThreadsLastBlock_ = 0;
        clear_gpu_observation();
        if (stage1_) { stage1_->reset(); stage2_->reset(); return; }
        for (auto& hh : hist_) hh.clear();
        outIdx_ = 0; inAbs_ = 0; histHeld_ = 0;
        if (gpu_invalidate_) gpu_invalidate_();   // 设备侧也要清，否则跳转后拖尾
    }

private:
    void clear_gpu_observation() {
        gpuStageMaskLastBlock_ = 0;
        gpuCompactStageMaskLastBlock_ = 0;
        if (stage1_) stage1_->clear_gpu_observation();
        if (stage2_) stage2_->clear_gpu_observation();
    }
    ResamplerConfig cfg_;
    size_t cpuSimdOutputsLastBlock_ = 0;
    size_t cpuParallelThreadsLastBlock_ = 0;
    std::vector<double> phases_tap_major_;
    int L_ = 1, M_ = 1, T_ = 0, ch_ = 2;
    bool passthrough_ = false;
    // ---- GPU 做多相乘加的桥接 ----
    using GpuResampleFn = int (*)(const double*, int, long long, const double*, int, int,
                                  const int*, const int*, long long, double*);
    using GpuCompactFn = int (*)(const double*, int, long long, const double*, int, int, int,
                                 long long, long long, long long, long long, double* const*);
    GpuCompactFn gpu_compact_ = nullptr;
    using GpuVoidFn = void (*)();
    bool            useGpu_ = false;
    unsigned        gpuStageMaskLastBlock_ = 0;
    unsigned        gpuCompactStageMaskLastBlock_ = 0;
    GpuResampleFn   gpu_resample_ = nullptr;
    GpuVoidFn       gpu_invalidate_ = nullptr;
    bool            gpuTried_ = false;
    std::vector<double> phases_flat_, flat_, inter_;
    std::vector<int>    bases_, phs_;

    void load_gpu() {
        if (gpuTried_) return;
        gpuTried_ = true;
        // 按 GPU 后端偏好找 DLL：AMD=rtagpu.dll，NVIDIA=rtagpu_cuda.dll，自动两个都试
        // 逐个候选尝试：HIP → D3D12 → CUDA。某个 DLL 存在但 kernel 不可执行时
        // 必须继续试下一个（NVIDIA 机器上可能残留 HIP 版 rtagpu.dll）。
        for (const char* nm : gpu_dll_candidates()) {
            rta::platform::SharedLib lib;
            if (!lib.open(nm)) continue;
            auto avail = (int (*)())lib.symbol("rta_gpu_available");
            if (avail && avail() != 1) { lib.close(); continue; }
            auto res = (GpuResampleFn)lib.symbol("rta_gpu_resample");
            auto inv = (GpuVoidFn)lib.symbol("rta_gpu_resample_invalidate");
            if (!res) { lib.close(); continue; }
            gpu_resample_ = res; gpu_invalidate_ = inv;
            gpu_compact_ = (GpuCompactFn)lib.symbol("rta_gpu_resample_compact_planar");
            // 与迁移前一致：模块永久保持加载。多个重采样器实例各自持有取到的函数指针，
            // 一旦卸载（FreeLibrary）就会变成野指针 —— 所以这里放弃所有权而不是留着析构。
            lib.detach();
            return;
        }
    }
    // 两段式下的两级（本级不再自己滤波）
    std::unique_ptr<PolyphaseResampler> stage1_, stage2_;
    std::vector<double>  tmp_;
    std::vector<double*> tmpP_;

    std::vector<std::vector<double>> phases_;
    std::vector<std::vector<double>> hist_;
    // D-11 定点路径：kFixedCoeffBits 位量化系数（phasesFixed_[p][u]）与本块的 int24 历史副本
    std::vector<std::vector<int64_t>> phasesFixed_;
    std::vector<int64_t> phases_fixed_tap_major_;
    std::vector<std::vector<int64_t>> histFixed_;
    int64_t outIdx_ = 0;
    int64_t inAbs_  = 0;
    int     histHeld_ = 0;
};

} // namespace rta::dsp
