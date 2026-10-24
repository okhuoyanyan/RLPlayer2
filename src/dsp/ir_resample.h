// ir_resample.h — 房间校正脉冲响应（IR）的采样率换算
//
// 背景：IR 是【时间函数】。44.1k 量出来的房间响应直接铺到 96k 工程上，时间轴会被
// 拉伸 96/44.1 = 2.177 倍（1 s 的响应当成 0.46 s），校正频点、群延迟、混响尾巴全错。
// 本文件把 IR 从"测量采样率"换算到"工程采样率"，并保证【时间基准】不变。
//
// 设计要点：
//  * 单声道接口：多声道由调用方逐声道调用（各声道 IR 的长度/内容可以不同）
//  * 复用 rta::dsp::PolyphaseResampler（线性相位 Kaiser sinc）
//  * 线性相位 FIR 的群延迟必须剪掉：否则每个 IR 都被偷偷加上 (T*L-1)/(2M) 个输出
//    样本的前置延迟（44.1k->96k 时约 557 样本 ≈ 5.8 ms），房间校正的对齐就白做了
//  * inRate == outRate 时逐位透传（绝不"顺手"过一次滤波器）
//  * 尾部补 T 个零把重采样器的尾巴冲出来（T = 每相位抽头数 = 原型在输入域的长度）
#pragma once
#include "resampler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace rta::dsp {

namespace ir_detail {

// 每相位抽头数：决定过渡带陡度（归一化宽度约 (atten-8)/(2.285*2π*T)）与运算量。
// 512 抽头 + 120 dB 阻带 => 过渡带约占 Nyquist 的 3%，通带到 0.97*Nyquist 都是平的
// （实测 1 kHz 增益误差 < 0.001 dB），对房间校正绰绰有余。
inline constexpr int    kTapsPerPhase = 512;
inline constexpr double kAttenDb      = 120.0;
inline constexpr double kTransition   = 0.03;

// 相位表总抽头上限。常见的 44.1k/48k/88.2k/96k/176.4k/192k/384k 组合里
// L <= 1280（44100->384000），512*1280 = 655360 ≈ 5 MB，没问题；
// 但互质采样率（如 44101->96000）L 可达 96000，512*96000 ≈ 393 MB，会直接 OOM。
inline constexpr int kMaxTotalTaps = 1 << 20;

// 上采样因子上限（防御非音频采样率对：L > 65536 时连相位表都建不起来）。
// 真实音频采样率对经 gcd 约分后 L 都远小于此值（最大 384000/300 = 1280）。
inline constexpr int kMaxUpFactor = 1 << 16;

// 超长 IR 上限：1M 样本 @44.1k = 22.7 s。房间校正 IR 都在秒级以内，超过这个长度
// 更可能是"整首歌被当成 IR"之类的误用；单级多相换算的代价是 O(长度 × 每相位抽头)，
// 此时宁可原样返回并让调用方从长度看出来，也不要让播放线程卡上十几秒。
inline constexpr size_t kMaxSamples = size_t(1) << 20;

} // namespace ir_detail

// 把单声道 IR 从 inRate 换算到 outRate。
//
// 返回的样本数 = round(ir.size() * outRate / inRate)，且第 n 个输出样本对应
// 输入的第 n*inRate/outRate 个样本 —— 即【时间基准对齐、群延迟已剪掉】。
// 以下情况原样返回（不滤波、不缩放、逐位相同）：
//   * inRate == outRate        —— 同率不需要换算
//   * inRate == 0 || outRate==0 —— 采样率未知（旧 WAV/RAW 没 rate 字段），交给调用方告警
//   * ir 为空                  —— 没有可换算的内容（空 IR 在卷积器里表示"未启用"）
//   * ir.size() > 1M           —— 超长，见 ir_detail::kMaxSamples 的理由
//   * 约分后的上采样因子 > 65536 —— 不是音频采样率对，建表会吃掉上百 MB
inline std::vector<double> ir_resample(const std::vector<double>& ir,
                                       uint32_t inRate, uint32_t outRate) {
    // ---- 1) 直通条件 ----
    if (inRate == 0 || outRate == 0 || inRate == outRate) return ir;
    if (ir.empty()) return ir;
    if (ir.size() > ir_detail::kMaxSamples) return ir;

    const uint32_t g = std::gcd(inRate, outRate);
    const int L = (int)(outRate / g);      // 上采样因子（内插域相位个数）
    const int M = (int)(inRate / g);       // 下采样因子
    if (L <= 0 || M <= 0) return ir;       // 理论不可达（两率均 > 0，gcd 整除两者）
    if (L > ir_detail::kMaxUpFactor) return ir;

    // ---- 2) 建重采样器（单级）----
    // 单级而不是两段式：两段式的总群延迟要跨采样率域相加，容易补偿错；
    // 而 IR 只在加载时算一次，单级的运算量完全可以接受。
    int tpp = ir_detail::kTapsPerPhase;
    if ((long long)tpp * (long long)L > (long long)ir_detail::kMaxTotalTaps)
        tpp = std::max(16, (int)(ir_detail::kMaxTotalTaps / L));   // 大 L：过渡带变宽，时间基准仍正确

    ResamplerConfig cfg;
    cfg.inRate     = inRate;
    cfg.outRate    = outRate;
    cfg.channels   = 1;                    // IR 一律按单声道处理
    cfg.twoStage   = false;
    cfg.window     = WindowKind::Kaiser;
    cfg.phase      = PhaseKind::Linear;
    cfg.attenDb    = ir_detail::kAttenDb;
    cfg.transition = ir_detail::kTransition;
    cfg.totalTaps  = tpp * L;              // 原型长度必须是 L 的整数倍

    PolyphaseResampler rs(cfg);

    // 每相位抽头数 T 同时就是原型在【输入域】的长度（样本数），后面补零按它算。
    const int T = rs.tapsPerPhase();

    // ---- 3) 群延迟补偿 ----
    // 原型是长度 T*L、对称（线性相位）的 FIR，在内插域的中心在 (T*L-1)/2；
    // 输出第 m 个样本对应内插域序号 m*M，故延迟折合【输出样本】= (T*L-1)/(2*M)，
    // 正好等于 latencyInputSamples() * L/M。这一步不是可选的精度优化：
    // 44.1k->96k、T=512 时它是 557 个输出样本（5.8 ms），不剪掉就等于给 IR 加了前置延迟。
    const double delayOutExact = rs.latencyInputSamples()
                               * (double)rs.upFactor() / (double)rs.downFactor();
    const size_t delayOut = (size_t)std::llround(delayOutExact);

    // ---- 4) 输出长度 ----
    // 按"同样的物理时长"取整。至少 1 个样本：极端下采样（如 96k 的 1 样本 IR 到 44.1k）
    // 取整后可能是 0，返回空 IR 会被卷积器当成"未启用"，语义上更糟。
    size_t outLen = (size_t)std::llround((double)ir.size() * (double)L / (double)M);
    if (outLen == 0) outLen = 1;

    // ---- 5) 处理 + 冲尾巴 ----
    // 每个输出 y[m] 只依赖 x[i-T+1 .. i]（i = floor(m*M/L)）。我们要的最后一个输出是
    // delayOut+outLen-1，对应 i ≈ ir.size()-1 + T/2，所以补 T 个零一定够（还多出 T/2
    // 的余量）。补零少于 T/2 会算不出末尾含真实样本的输出（尾巴被截断）；补更多只会
    // 多算一些靠后的输出，而它们本来就被丢弃，对结果没有影响。
    const size_t pad = (size_t)T;

    std::vector<double> raw;
    raw.reserve(rs.maxOutFrames(ir.size()) + rs.maxOutFrames(pad) + 8);
    {
        std::vector<double> buf(rs.maxOutFrames(ir.size()));
        const double* in[1] = { ir.data() };
        double*       out[1] = { buf.data() };
        const size_t n = rs.process(in, ir.size(), out);
        raw.assign(buf.begin(), buf.begin() + (std::ptrdiff_t)n);
    }
    {
        const std::vector<double> zeros(pad, 0.0);
        std::vector<double> buf(rs.maxOutFrames(pad));
        const double* in[1] = { zeros.data() };
        double*       out[1] = { buf.data() };
        const size_t n = rs.process(in, pad, out);
        raw.insert(raw.end(), buf.begin(), buf.begin() + (std::ptrdiff_t)n);
    }

    // 防御：正常情况下 T 个补零已经覆盖 delayOut+outLen，这里只在异常时才补零。
    const size_t need = delayOut + outLen;
    if (raw.size() < need) raw.resize(need, 0.0);

    // ---- 6) 剪掉群延迟，取等长的一段 ----
    std::vector<double> res(outLen);
    std::copy(raw.begin() + (std::ptrdiff_t)delayOut,
              raw.begin() + (std::ptrdiff_t)need, res.begin());
    return res;
}

} // namespace rta::dsp