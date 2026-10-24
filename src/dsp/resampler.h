// resampler.h — 多相 sinc 重采样引擎（任意有理比率 L/M）
//
// 设计要点：
//  * 原型 FIR 在"内插域"设计，长度为 L 的整数倍，按相位拆分以获得 O(T) 每输出样本开销
//  * 内部全程 double；关键路径可再升级为 double-double（128bit 精度）以杜绝累积误差
//  * 支持任意 inRate/outRate（自动约分为最简 L/M），支持多声道
//  * inRate == outRate 时按位透传（比特完美）
#pragma once
#include "gpu_pref.h"
#include "filter_design.h"
#include "filter_families.h"
#include "platform/dynlib.h"   // GPU 升频按需加载 rtagpu.dll —— 本目录不再直接碰平台 API
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <numeric>
#include <algorithm>
#include <memory>
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

    // ---- 两段式升频 ----
    // 大比率（如 44.1k -> 705.6k 是 16 倍）单级做时，每输出样本要做 totalTaps/L 次乘加，
    // 而输出样本数是 L 倍 ⟹ 总运算量 ∝ totalTaps × 输出率，非常贵。
    // 拆成两级后：第一级只跑到中间速率（4 倍），承担陡峭的过渡带；
    // 第二级从中间速率到最终速率，图像离得远，几十抽头就够。
    // 实测同参数下 CPU 运算量降到约 1/4，声音在可听带内一致。
    bool twoStage   = false;
    int  stage2Taps = 64;      // 第二级每相位抽头数（过渡带很宽，不需要长）
};

// 把 FilterPreset 的全部参数应用到 ResamplerConfig
inline void apply_preset(ResamplerConfig& cfg, const FilterPreset& fp) {
    cfg.totalTaps  = fp.taps;
    cfg.transition = fp.transition;
    cfg.attenDb    = fp.attenDb;
    cfg.window     = fp.window;
    cfg.phase      = fp.phase;
    cfg.twoStage   = fp.twoStage;
    cfg.apodizing  = fp.apodizing;
    cfg.apodF1     = fp.apodF1;
    cfg.apodF2     = fp.apodF2;
    if (fp.twoStage && fp.stage2Taps > 0) cfg.stage2Taps = fp.stage2Taps;
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
                const int T1 = std::max(4, cfg.totalTaps / L_);      // 每相位抽头数（决定过渡带陡度）
                ResamplerConfig c1 = cfg;
                c1.twoStage   = false;
                c1.inRate     = cfg.inRate;
                c1.outRate    = cfg.inRate * (uint32_t)F;
                c1.totalTaps  = T1 * F;
                ResamplerConfig c2 = cfg;
                c2.twoStage   = false;
                c2.inRate     = cfg.inRate * (uint32_t)F;
                c2.outRate    = cfg.outRate;
                c2.totalTaps  = cfg.stage2Taps * (L_ / F);
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
        const double cutoff = idealCutoff * (1.0 - cfg.transition);

        // 原型长度取 L 的整数倍
        int total = cfg.totalTaps;
        T_ = total / L_;                       // 每相位抽头数
        if (T_ < 4) { T_ = 4; }
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
            proto = design_fir(fs);
            // 最小相位是独立一步（倒谱法），不是 design_fir 的参数
            if (cfg.phase == PhaseKind::Minimum) {
                proto = to_minimum_phase(proto);
                if ((int)proto.size() != total) proto.resize((size_t)total, 0.0);
            } else if (cfg.phase == PhaseKind::Intermediate) {
                proto = to_intermediate_phase(proto);
                if ((int)proto.size() != total) proto.resize((size_t)total, 0.0);
            }
        }

        // 关键：有理 L/M 转换中，"内插 -> 滤波 -> 抽取"要求原型在内插域的直流增益为 L，
        // 否则输出会被整体衰减 L 倍（L=1280 时正好 -62.1 dB，实测确认过这个陷阱）。
        // 等价说法：每一相位的抽头之和必须为 1。
        const double phaseGain = (double)L_;
        for (auto& v : proto) v *= phaseGain;

        // 相位主序布局：phases_[p][u] = h[p + u*L]，提升缓存友好度
        phases_.assign((size_t)L_, std::vector<double>((size_t)T_));
        for (int p = 0; p < L_; ++p)
            for (int u = 0; u < T_; ++u)
                phases_[(size_t)p][(size_t)u] = proto[(size_t)(p + u * L_)];

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
        return useGpu_ && gpu_resample_ != nullptr;
    }
    bool two_stage() const { return stage1_ != nullptr; }
    int  stage1_factor() const { return stage1_ ? stage1_->upFactor() : L_; }
    int  upFactor() const { return L_; }
    int  downFactor() const { return M_; }

    // 群延迟（输入样本数）。原型长度是 T_*L_（偶数 ⟹ Type II 线性相位），
    // 其在内插域的群延迟为 (T_*L_ - 1)/2，换算回输入样本需再除以 L_。
    // 曾用 (T_-1)/2，在 L=4 时偏差 3.5 个输出样本 ⟹ 997 Hz 上约 7° 相位误差，
    // 会让"与理想正弦比对"的残差虚高到 −26 dBFS。
    double latencyInputSamples() const { return ((double)T_ * (double)L_ - 1.0) / (2.0 * (double)L_); }

    // 处理一块输入。out 必须至少有 maxOutFrames(inFrames) 容量。
    // in/out 为每声道指针数组。
    size_t process(const double* const* in, size_t inFrames,
                   double* const* out) {
        // ---- 两段式：第一级 -> 中间缓冲 -> 第二级 ----
        if (stage1_) {
            if (inFrames == 0) return 0;
            const size_t cap = stage1_->maxOutFrames(inFrames);
            if (tmp_.size() < cap * (size_t)ch_) {
                tmp_.assign(cap * (size_t)ch_, 0.0);
                for (int c = 0; c < ch_; ++c) tmpP_[(size_t)c] = tmp_.data() + (size_t)c * cap;
            }
            const size_t n1 = stage1_->process(in, inFrames, tmpP_.data());
            return stage2_->process(tmpP_.data(), n1, out);
        }
        if (passthrough_) {
            for (int c = 0; c < ch_; ++c)
                std::copy(in[c], in[c] + inFrames, out[c]);
            return inFrames;
        }
        if (inFrames == 0) return 0;

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
        for (;;) {
            const int64_t num = outIdx_ * (int64_t)M_;
            const int64_t i   = num / L_;
            if (i > lastAbs) break;
            bases_.push_back((int)(i - firstAbs));
            phs_.push_back((int)(num % L_));
            ++outIdx_;
        }
        const size_t produced = bases_.size();
        if (produced == 0) return 0;

        // ---- 2) 计算 ----
        bool gpuDone = false;
        if (useGpu_ && gpu_resample_) {
            const size_t hl = hist_[0].size();
            if (hl > 0) {
                flat_.resize((size_t)ch_ * hl);
                for (int c = 0; c < ch_; ++c)
                    std::copy(hist_[(size_t)c].begin(), hist_[(size_t)c].end(),
                              flat_.begin() + (ptrdiff_t)((size_t)c * hl));
                inter_.resize(produced * (size_t)ch_);
                const int rc = gpu_resample_(flat_.data(), ch_, (long long)hl,
                                             phases_flat_.data(), L_, T_,
                                             bases_.data(), phs_.data(),
                                             (long long)produced, inter_.data());
                if (rc == (int)produced) {
                    for (size_t n = 0; n < produced; ++n)
                        for (int c = 0; c < ch_; ++c)
                            out[c][n] = inter_[n * (size_t)ch_ + (size_t)c];
                    gpuDone = true;
                }
            }
        }

        // CPU 路径（也是 GPU 不可用时的回退）
        if (!gpuDone) {
            for (size_t n = 0; n < produced; ++n) {
                const size_t base = (size_t)bases_[n];
                const int p = phs_[n];
                const int uEnd = (base + 1 < (size_t)T_) ? (int)(base + 1) : T_;
                for (int c = 0; c < ch_; ++c) {
                    const auto& hh = hist_[(size_t)c];
                    const auto& ph = phases_[(size_t)p];
                    double acc = 0.0;
                    for (int u = 0; u < uEnd; ++u)
                        acc += ph[(size_t)u] * hh[base - (size_t)u];
                    out[c][n] = acc;
                }
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
    size_t maxOutFrames(size_t inFrames) const {
        if (stage1_) return stage2_->maxOutFrames(stage1_->maxOutFrames(inFrames));
        if (passthrough_) return inFrames;
        return (size_t)(((double)inFrames * (double)L_) / (double)M_) + (size_t)L_ + 8;
    }

    void reset() {
        if (stage1_) { stage1_->reset(); stage2_->reset(); return; }
        for (auto& hh : hist_) hh.clear();
        outIdx_ = 0; inAbs_ = 0; histHeld_ = 0;
        if (gpu_invalidate_) gpu_invalidate_();   // 设备侧也要清，否则跳转后拖尾
    }

private:
    ResamplerConfig cfg_;
    int L_ = 1, M_ = 1, T_ = 0, ch_ = 2;
    bool passthrough_ = false;
    // ---- GPU 做多相乘加的桥接 ----
    using GpuResampleFn = int (*)(const double*, int, long long, const double*, int, int,
                                  const int*, const int*, long long, double*);
    using GpuVoidFn = void (*)();
    bool            useGpu_ = false;
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
    int64_t outIdx_ = 0;
    int64_t inAbs_  = 0;
    int     histHeld_ = 0;
};

} // namespace rta::dsp
