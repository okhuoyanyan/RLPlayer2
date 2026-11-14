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
#include "coefficient_precision.h"
#include "filter_families.h"
#include "filter_cache.h"
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
#include <climits>
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

    // Cascaded interpolation: stage one sets the source band;
    // stage two removes its images at a higher rate. Throughput is measured,
    // not inferred from tap counts alone.
    bool twoStage   = false;
    // Production autoTaps may choose a rational cascade. Explicit user "off"
    // disables this separately; raw/reference resamplers remain opt-in.
    bool allowAutomaticCascade = false;
    // Resolve source filter support against both rate clocks. Raw/reference
    // configurations keep their explicit tap counts unless this is requested.
    bool autoTaps = false;
    // Selected by the automatic planner when downsampling needs a lower
    // coefficient-rounding floor. Raw gain is rounded only once in this path.
    bool highPrecisionDesign = false;
    // -1 takes one global snapshot at the outer preparation boundary. Child
    // stages, cache keys and design then use that same frozen phase mix.
    double designPhaseMix = -1.0;
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
    // 原型抽头预算（T × L 个 double）。超过即**显式失败**，绝不偷偷降质量。
    // 2026-11-01：旧值 2^21（16 MB）把 21 个预设在 44.1→384k、4 个在 44.1→96k 直接挡死，
    // 而且失败是抛异常 ⟹ CLI/播放线程未捕获 ⟹ std::terminate ⟹ 0xC0000409（真崩溃，已复现）。
    // 实测（本机，--analyze，10 s 素材）：
    //   poly-sinc-xl          @44.1→384k  T=2877 L=1280 → 3.68M 抽头，准备 0.44 s，峰值 167 MB
    //   poly-sinc-gauss-xl    @44.1→96k   T=32767 L=320 → 10.49M 抽头，准备 0.87 s，峰值 301 MB
    //   sinc-MGa              @44.1→384k  T=32767 L=1280 → 41.9M 抽头，准备 1.98 s，峰值 1,044 MB
    // 取 2^24（16.8M 抽头 ≈ 134 MB 系数表）：覆盖前两类，
    // 只把 sinc-MGa@384k 这类 40M 级（1 GB 峰值）继续挡在门外并给出可读错误。
    int maxPrototypeTaps = 1 << 24;
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


// Pure preparation plan: playback, prewarm admission and diagnostics share the
// exact stage/tap decisions. It allocates no coefficient/FFT buffers and never
// weakens a filter to satisfy a resource budget.
struct ResamplerStagePlan {
    ResamplerConfig config;
    int L=1, M=1, T=0;
    uint64_t coefficientCount=0;
};
struct ResamplerPlan {
    std::array<ResamplerStagePlan,2> stages;
    size_t stageCount=0;
    int L=1,M=1;
    uint64_t coefficientCount=0,cachePayloadBytes=0,designPeakBytes=0;
};
inline void validate_resampler_config(const ResamplerConfig& c) {
    if(!c.inRate||!c.outRate) throw std::invalid_argument("Resampler sample rates must be positive: inRate="+
        std::to_string(c.inRate)+" outRate="+std::to_string(c.outRate));
    if(c.channels<1||c.channels>64) throw std::invalid_argument("Resampler supports 1 to 64 channels");
    if(c.totalTaps<1||c.stage2Taps<1||c.minTapsPerInput<0||c.maxPrototypeTaps<0)
        throw std::invalid_argument("Resampler tap counts and budgets are invalid");
    if(!std::isfinite(c.transition)||c.transition<0.0||c.transition>=1.0)
        throw std::invalid_argument("Resampler transition must be finite and in [0,1)");
    if(!std::isfinite(c.attenDb)||c.attenDb<0.0||c.attenDb>1000.0)
        throw std::invalid_argument("Resampler attenuation must be finite and in [0,1000] dB");
    if(int(c.window)<0||int(c.window)>int(WindowKind::Hann)||int(c.phase)<0||int(c.phase)>int(PhaseKind::Intermediate))
        throw std::invalid_argument("Unknown resampler window or phase");
    if(!std::isfinite(c.designPhaseMix)||(c.designPhaseMix!=-1.0&&(c.designPhaseMix<0.0||c.designPhaseMix>1.0)))
        throw std::invalid_argument("Invalid resampler phase mix");
    if(!std::isfinite(c.apodF1)||!std::isfinite(c.apodF2)||
       (c.apodizing&&!(c.apodF1>=0.0&&c.apodF1<c.apodF2&&c.apodF2<=1.0)))
        throw std::invalid_argument("Invalid resampler apodization edges");
    const auto g=std::gcd(c.inRate,c.outRate);
    if(c.inRate/g>uint32_t(INT_MAX)||c.outRate/g>uint32_t(INT_MAX))
        throw std::length_error("Reduced resampler ratio exceeds the supported integer range");
}
inline int resampler_tap_product(int t,int l) {
    if(t<1||l<1||int64_t(t)*l>INT_MAX) throw std::length_error("Resampler coefficient count overflow");
    return t*l;
}
inline ResamplerConfig resolve_auto_taps(const ResamplerConfig& requested) {
    validate_resampler_config(requested);
    ResamplerConfig c=requested;
    if(c.designPhaseMix<0.0)c.designPhaseMix=phase_mix();
    if(!c.autoTaps)return c;
    c.autoTaps=false; // Resolving a returned configuration again is a no-op.
    if(c.inRate==c.outRate)return c;
    if(c.transition==0.0&&!c.apodizing&&c.minTapsPerInput==0)
        throw std::invalid_argument("Automatic tap design requires a positive transition");
    const uint32_t bandRate=std::min(c.inRate,c.outRate);
    int sourceT=c.minTapsPerInput>0?c.minTapsPerInput:
        solve_taps_per_phase(c.window,c.transition,c.apodizing,
            c.apodF1,c.apodF2,c.totalTaps,0,double(bandRate));
    if(c.outRate<c.inRate&&c.phase==PhaseKind::Linear&&!c.apodizing&&
       (c.window==WindowKind::Kaiser||(c.window==WindowKind::BlackmanHarris&&c.attenDb>100.0))) {
        // Every decimation branch can contribute coherently. Reserve the
        // triangle-sum margin at design time; a single-FIR stop band alone
        // does not certify the complete rate-conversion chain.
        const double oldAttenuation=c.attenDb;
        const auto m=c.inRate/std::gcd(c.inRate,c.outRate);
        c.attenDb=std::ceil(oldAttenuation+20.0*std::log10(double(m))+6.0);
        const int nominal=solve_taps_per_phase(c.window,c.transition,false,0,0,c.totalTaps,0,double(bandRate));
        const double required=std::ceil(double(nominal)*(c.attenDb-8.0)/std::max(1.0,oldAttenuation-8.0));
        if(!std::isfinite(required)||required>INT_MAX)
            throw std::length_error("Automatic alias rejection requires too many taps");
        sourceT=std::max(sourceT,int(required));
        c.highPrecisionDesign=true;
    }
    // Prototype length follows max(L,M), not L alone. At downsampling the
    // antialias transition is set by the slower output clock. Rounding upward
    // preserves the budget; integer upsampling remains bit-for-bit unchanged.
    const uint64_t count=(uint64_t(sourceT)*std::max(c.inRate,c.outRate)+c.outRate-1)/c.outRate;
    if(count>uint64_t(INT_MAX))throw std::length_error("Automatic source support exceeds the tap integer range");
    c.minTapsPerInput=int(count);
    return c;
}
inline ResamplerStagePlan plan_resampler_stage(const ResamplerConfig& c) {
    validate_resampler_config(c);
    ResamplerStagePlan p;p.config=c;
    const auto g=std::gcd(c.inRate,c.outRate);p.L=int(c.outRate/g);p.M=int(c.inRate/g);
    int t=c.totalTaps/p.L;
    if(c.minTapsPerInput>0) t=c.minTapsPerInput;
    else if(c.transition>0.0) {
        const double low=21.13/c.transition+0.5,high=2.0*21.13/c.transition+0.5;
        if(!std::isfinite(high)||high>INT_MAX) throw std::length_error("Transition requires an unrepresentable tap count");
        const int tMin=std::max(4,int(low)),tMax=std::max(tMin,int(high));
        if(c.phase==PhaseKind::Linear&&t>tMax)t=tMax;
        if(c.applyTapFloor&&t<tMin)t=tMin;
    }
    p.T=std::max(4,t);p.coefficientCount=uint64_t(resampler_tap_product(p.T,p.L));
    if(c.maxPrototypeTaps>0&&p.coefficientCount>uint64_t(c.maxPrototypeTaps))
        throw std::runtime_error("Requested FIR exceeds the coefficient budget; quality was not reduced. inRate="+
            std::to_string(c.inRate)+" outRate="+std::to_string(c.outRate)+" L="+std::to_string(p.L)+
            " M="+std::to_string(p.M)+" T="+std::to_string(p.T)+" taps="+std::to_string(p.coefficientCount)+
            " budget="+std::to_string(c.maxPrototypeTaps)+". Use a cascade or explicitly change the filter configuration.");
    return p;
}
inline ResamplerPlan plan_resampler(const ResamplerConfig& requested) {
    const auto cfg=resolve_auto_taps(requested);
    ResamplerPlan p;const auto g=std::gcd(cfg.inRate,cfg.outRate);
    p.L=int(cfg.outRate/g);p.M=int(cfg.inRate/g);
    if(p.L==1&&p.M==1)return p;
    const bool rational=p.M!=1&&uint64_t(cfg.outRate)>=uint64_t(cfg.inRate)*2;
    int f=1;
    if((cfg.twoStage||(cfg.allowAutomaticCascade&&rational))&&((p.M==1&&p.L>=8)||rational))
        for(int candidate:{4,3,2})if(p.L%candidate==0&&p.L/candidate>=(rational?1:2)){f=candidate;break;}
    if(f>1) {
        const uint64_t mid=uint64_t(cfg.inRate)*f;
        if(mid>UINT32_MAX)throw std::length_error("Cascade intermediate sample rate overflow");
        const int t1=cfg.minTapsPerInput>0?cfg.minTapsPerInput:std::max(4,cfg.totalTaps/p.L);
        ResamplerConfig a=cfg,b=cfg;
        a.twoStage=b.twoStage=false;a.allowAutomaticCascade=b.allowAutomaticCascade=false;
        a.applyTapFloor=b.applyTapFloor=true;
        a.outRate=uint32_t(mid);a.totalTaps=resampler_tap_product(t1,f);
        b.inRate=uint32_t(mid);b.minTapsPerInput=cfg.stage2Taps;b.useFixed=false;b.transition=.30;
        const int l2=p.L/f;
        if(rational) {
            b.window=WindowKind::Kaiser;b.apodizing=false;b.apodF1=b.apodF2=0.0;
            b.attenDb=std::max(286.0,cfg.attenDb+6.0);
            const double width=.5*b.transition/double(std::max(l2,p.M));
            const int prototype=kaiser_taps_for(b.attenDb,width);
            const double count=std::ceil(1.2*double(prototype)/double(l2));
            if(count>INT_MAX)throw std::length_error("Cascade transition requires too many taps");
            b.minTapsPerInput=std::max(cfg.stage2Taps,int(count));
        }
        b.totalTaps=resampler_tap_product(b.minTapsPerInput,l2);
        p.stages[0]=plan_resampler_stage(a);p.stages[1]=plan_resampler_stage(b);p.stageCount=2;
    }else {p.stages[0]=plan_resampler_stage(cfg);p.stageCount=1;}
    auto pow2=[](uint64_t n){uint64_t v=1;while(v<n){if(v>UINT64_MAX/2)throw std::length_error("FFT size overflow");v*=2;}return v;};
    uint64_t maxScratch=0,history=0;
    for(size_t i=0;i<p.stageCount;++i){
        const auto& stage=p.stages[i];const uint64_t n=stage.coefficientCount;
        p.coefficientCount+=n;
        if(n>=filter_cache::kMinTapsToCache)p.cachePayloadBytes+=n*sizeof(double);
        history+=uint64_t(stage.T)*uint64_t(cfg.channels)*16;
        if(stage.config.phase!=PhaseKind::Linear){
            // Mirror the actual factor supports, not full-N times maximum
            // padding. MP uses two half-support factors; IP allocates LP/MP
            // order using the frozen mix. Sum workspaces conservatively even
            // though cepstrum and DD convolution do not peak simultaneously.
            const bool factored=n>=5&&(n>=2048||stage.config.attenDb>=250.0);
            uint64_t mpSupport=factored?2*((n-1)/4)+1:n;
            if(factored&&stage.config.phase==PhaseKind::Intermediate&&stage.config.designPhaseMix>0.0&&stage.config.designPhaseMix<1.0){
                const int order=2*((int(n)-1)/2);
                const int lpOrder=std::clamp(2*int(std::round(stage.config.designPhaseMix*order*.5)),2,order-2);
                mpSupport=uint64_t(order-lpOrder+1);
            }
            const uint64_t padding=factored?(stage.config.window==WindowKind::Gaussian?64:32):8;
            const uint64_t cepstrum=(stage.config.phase==PhaseKind::Intermediate&&stage.config.designPhaseMix>=1.0)?0:pow2(mpSupport*padding)*32;
            maxScratch=std::max(maxScratch,cepstrum+pow2(n*2)*96);
        }
    }
    p.designPeakBytes=p.coefficientCount*128+history+maxScratch+512*1024;
    return p;
}

class PolyphaseResampler {
public:
    explicit PolyphaseResampler(const ResamplerConfig& requested) : cfg_(resolve_auto_taps(requested)) {
        check_design_cancel();
        const ResamplerConfig& cfg=cfg_;
        const auto plan=plan_resampler(cfg);
        L_=plan.L;M_=plan.M;ch_=cfg.channels;
        if(plan.stageCount==0){passthrough_=true;return;}
        if(plan.stageCount==2){
            stage1_=std::make_unique<PolyphaseResampler>(plan.stages[0].config);
            stage2_=std::make_unique<PolyphaseResampler>(plan.stages[1].config);
            tmpP_.assign(size_t(ch_),nullptr);
            return;
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

        T_=plan.stages[0].T;
        const int total=int(plan.stages[0].coefficientCount);
        const bool rawPrecise=cfg.highPrecisionDesign&&cfg.phase==PhaseKind::Linear&&!cfg.apodizing&&
            (cfg.window==WindowKind::Kaiser||(cfg.window==WindowKind::BlackmanHarris&&cfg.attenDb>100.0));

        std::vector<double> proto;
        // ★ 2026-11-01：大原型走磁盘缓存（见 filter_cache.h）。
        //   mp/ip 的相位变换代价随原型长度线性增长：44.1→384k 的 long-mp / long-ip 冷启动
        //   实测 8.54 / 8.56 s（线性相位同尺寸只要 0.2~0.5 s）。缓存只是把**同一批系数**
        //   存下来，命中与未命中必须逐位相同（tests/test_filter_cache.cpp 断言）。
        bool protoCached = false;
        std::string cacheKey;
        filter_cache::LoadStatus cacheLoad = filter_cache::LoadStatus::Disabled;
        filter_cache::StoreStatus cacheStore = filter_cache::StoreStatus::SkippedSize;
        bool designLockUnavailable = false;
        const double designPhaseMix = cfg.designPhaseMix; // same snapshot for both stages, key AND design
        if (filter_cache::enabled() && (long long)total >= (long long)filter_cache::kMinTapsToCache) {
            const std::string sig = filter_cache::signature(
                {(double)total, (double)L_, (double)T_, cutoff, cfg.transition,
                 (double)cfg.attenDb, (double)(int)cfg.window, (double)(int)cfg.phase,
                 cfg.apodizing ? 1.0 : 0.0, cfg.apodF1, cfg.apodF2, designPhaseMix},
                (cfg.phase == PhaseKind::Linear) ? (rawPrecise?"lp-dd-raw":"lp") : "phased");
            cacheKey = filter_cache::key(sig);
            cacheLoad = filter_cache::load_checked(cacheKey, proto, (size_t)total);
            if (cacheLoad == filter_cache::LoadStatus::Hit) protoCached = true;
            else proto.clear();
        }
        if (!protoCached) {
        // 同一原型可能被两个线程同时需要（启动预热 + 用户点播放）：只让一个真正设计，
        // 另一个在锁上等它把缓存写好再读，避免 9 秒的设计跑两遍。
        std::unique_ptr<filter_cache::DesignLock> processDesignLock;
        if (!cacheKey.empty()) {
            // A cancellable OS lock also coordinates other application
            // processes; a plain blocking std::mutex cannot cancel prewarm.
            processDesignLock = std::make_unique<filter_cache::DesignLock>(
                cacheKey, 30000, design_cancel_flag);
            check_design_cancel();
            designLockUnavailable = !bool(*processDesignLock);
            const auto secondLoad = filter_cache::load_checked(cacheKey, proto, (size_t)total);
            // Retain the first failure for diagnosis; a later hit still takes
            // precedence because it explains the coefficients actually used.
            if (secondLoad == filter_cache::LoadStatus::Hit || cacheLoad == filter_cache::LoadStatus::NotFound)
                cacheLoad = secondLoad;
            if (secondLoad == filter_cache::LoadStatus::Hit) protoCached = true;
            else proto.clear();
        }
        if (!protoCached) {
        if(rawPrecise) {
            proto=coefficient_precision::lowpass(total,cutoff,
                kaiser_beta_for_attenuation(cfg.attenDb),double(L_));
        } else if (cfg.window == WindowKind::Kaiser && cfg.phase == PhaseKind::Linear && !cfg.apodizing) {
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
                                      cfg.phase, designPhaseMix, passEdge);
            if ((int)proto.size() != total) proto.resize((size_t)total, 0.0);
        }
        check_design_cancel();
        if (!cacheKey.empty()) cacheStore = filter_cache::store_checked(cacheKey, proto);
        }   // if (!protoCached) 内层：真正设计
        }   // if (!protoCached) 外层：允许持锁等待者命中缓存
        if (!cacheKey.empty()) {
            const auto loadName = [](filter_cache::LoadStatus s) {
                using S = filter_cache::LoadStatus;
                switch(s) {
                    case S::Hit:return "hit";case S::Disabled:return "disabled";
                    case S::NotFound:return "not_found";case S::Invalid:return "invalid";
                    case S::IoError:return "io_error";case S::Busy:return "busy";
                }
                return "unknown";
            };
            const auto storeName = [](filter_cache::StoreStatus s) {
                using S = filter_cache::StoreStatus;
                switch(s) {
                    case S::Stored:return "stored";case S::Disabled:return "disabled";
                    case S::SkippedSize:return "skipped_size";case S::Invalid:return "invalid";
                    case S::Full:return "full";case S::IoError:return "io_error";
                    case S::Busy:return "busy";case S::Cancelled:return "cancelled";
                }
                return "unknown";
            };
            std::fprintf(stderr,"FILTER_CACHE key=%s taps=%d load=%s store=%s design_lock=%s\n",
                cacheKey.c_str(),total,loadName(cacheLoad),protoCached?"not_attempted":storeName(cacheStore),
                protoCached?"not_needed":(designLockUnavailable?"unavailable":"acquired"));
        }

        // 关键：有理 L/M 转换中，"内插 -> 滤波 -> 抽取"要求原型在内插域的直流增益为 L，
        // 否则输出会被整体衰减 L 倍（L=1280 时正好 -62.1 dB，实测确认过这个陷阱）。
        // Total prototype gain is L; individual phase sums may contain finite interpolation error.
        const double phaseGain = (double)L_;
        if(!rawPrecise)for (auto& v : proto) v *= phaseGain;

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
        // ★ 2026-11-01（独立复核 ③）：只保留**一份**连续相位表。
        //   旧实现 phases_[L][T] 与 phases_flat_[L*T] 内容相同、只是容器不同，
        //   41.9M 抽头时两份各 320 MiB：构造期 proto + 两份表 ≈ 960 MiB，
        //   构造完成后仍常驻 640 MiB（复核实测 968.8 / 648.1 MiB）。
        //   现在直接按相位主序写进唯一一份表，CPU 用指针访问、GPU 直接用同一块，
        //   随后把原型释放掉。**系数与累加顺序完全不变**（tests/test_resampler* 逐位基线）。
        phases_flat_.assign((size_t)L_ * (size_t)T_, 0.0);
        for (int p = 0; p < L_; ++p)
            for (int u = 0; u < T_; ++u)
                phases_flat_[(size_t)p * (size_t)T_ + (size_t)u] = proto[(size_t)(p + u * L_)];
        std::vector<double>().swap(proto);   // 原型已经重排进相位表，尽早释放

        // 2026-10-30 试过「反向相位表 + 正序内积」以利向量化 —— A/B 实测无收益
        // （DSD1024 5.75x vs 5.82x，在噪声内），因为 DSD 高倍率的耗时实际由 GPU kernel 决定。
        // 已回退，不留无用复杂度。

        // GPU 与 CPU 共用同一份连续相位表（不再另建副本）
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
            // Return the equivalent prototype at inRate*L, before decimation.
            // Changing M after construction preserves the actual coefficients;
            // redesigning at another rate would measure a different filter.
            probe.M_ = 1;
            probe.stage2_->M_ = 1;
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
                out[(size_t)(p + u * L_)] = phases_flat_[(size_t)p * (size_t)T_ + (size_t)u];
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
            if (cap > tmp_.max_size() / size_t(ch_))
                throw std::length_error("Cascade intermediate block exceeds addressable storage");
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
                    phases_tap_major_[(size_t)u*L_+p]=phases_flat_[(size_t)p*(size_t)T_+(size_t)u];
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
                    const double* ph = phases_flat_.data() + (size_t)p * (size_t)T_;
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
                                const double* ph = phases_flat_.data() + (size_t)p * (size_t)T_;
                                if(width==16)resampler_temporal_group<4>(ph,hist_[c].data(),base,T_,size_t(M_),out[c]+n,size_t(L_));
                                else if(width==8)resampler_temporal_group<2>(ph,hist_[c].data(),base,T_,size_t(M_),out[c]+n,size_t(L_));
                                else resampler_temporal_group<1>(ph,hist_[c].data(),base,T_,size_t(M_),out[c]+n,size_t(L_));
                            }
                            count+=width*size_t(ch_);n+=width*size_t(L_);continue;
                        }
#endif
                        const int endTap=std::min<size_t>(base+1,size_t(T_));
                        for(int c=0;c<ch_;++c) {
                            double acc=0;
                            const double* ph = phases_flat_.data() + (size_t)p * (size_t)T_;
                            for(int u=0;u<endTap;++u)acc+=ph[u]*hist_[c][base-size_t(u)];
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
        // A buffer bound must not lose integer bits through binary64 or wrap
        // on huge caller-supplied counts. The remainder product fits uint64:
        // both rational factors were checked against INT_MAX by the planner.
        const size_t limit = (std::numeric_limits<size_t>::max)();
        const size_t quotient = inFrames / size_t(M_);
        const size_t remainder = inFrames % size_t(M_);
        if (quotient > limit / size_t(L_))
            throw std::length_error("Resampler output capacity overflow");
        const size_t whole = quotient * size_t(L_);
        const size_t extra = size_t(uint64_t(remainder) * uint64_t(L_) / uint64_t(M_)) + size_t(L_) + 8;
        if (whole > limit - extra)
            throw std::length_error("Resampler output capacity overflow");
        return whole + extra;
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

    // 相位表只有一份（连续 [L][T]，CPU 用指针、GPU 直接用同一块）：
    //   2026-11-01 起删掉了旧的 vector<vector> 副本，见构造函数里的说明。
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
