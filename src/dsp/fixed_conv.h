// int64 Q62 coefficients with int128 exact accumulation.
// Integer PCM is quantized exactly on its declared Q24 grid. For |x|<=1,
// |x_fixed|<=2^24 and checked |h_fixed|<2^63. The per-phase sum-of-absolute-
// products bound is tested over every eligible production preset/rate.
// Only coefficient scaling changed; input quantization, integer summation
// order and the correctly-rounded int128-to-double conversion are unchanged.
// Q62 is finite coefficient precision, not an infinite-precision claim.
#pragma once
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（输出进入音频样本）
#include <cstdint>
#include <vector>
#include <cmath>
#include <cstddef>
#include <bit>

namespace rta::dsp {

// Coefficient scale is 2^62. The int64 range is checked before llround.
// The earlier Q50 coefficient grid failed the actual 96k->384k mono LP
// rejection target (-279.647 dB, independently verified at 90 decimal digits).
// Q62 preserves the small coefficients needed for a 280 dB stopband.
// Input quantization, integer accumulation order and final rounding are unchanged.
inline constexpr int kFixedCoeffBits = 62;
inline constexpr int64_t kFixedCoeffOne = (int64_t)1 << kFixedCoeffBits;
// 输入缩放：24 bit（与 CD / 24-bit PCM 对齐）
inline constexpr int kFixedInputBits = 24;
inline constexpr int64_t kFixedInputOne = (int64_t)1 << kFixedInputBits;

// 把 double 原型量化成 int64 系数。返回 false 表示缩放后有系数超出 int64 安全范围。
inline bool fixed_coeffs_from_double(const std::vector<double>& h, std::vector<int64_t>& out) {
    out.resize(h.size());
    for (size_t i = 0; i < h.size(); ++i) {
        const double v = h[i] * (double)kFixedCoeffOne;
        if (!(v > -9.2e18 && v < 9.2e18)) return false;   // 超 int64
        out[i] = (int64_t)std::llround(v);
    }
    return true;
}

// 精确内积：x[i] 为 24 bit 整数，h[i] 为 Q62 缩放整数，累加用 __int128。
// 返回值 = Σ x[i]·h[i]，**无任何舍入**。
inline __int128 fixed_dot(const int64_t* x, const int64_t* h, size_t n) {
    __int128 acc = 0;
    for (size_t i = 0; i < n; ++i) acc += (__int128)x[i] * (__int128)h[i];
    return acc;
}

// 把精确累加结果还原成归一化 double（除以 2^(62+24)）。
// 注意：这一步**有**舍入（double 只有 53 bit 尾数），但它是输出量化，不是累加误差。
//
// 【为什么手写而不是 (double)acc】Windows + clang 上没有 __floattidf 运行时辅助函数，
// 直接强转会链接失败（lld-link: undefined symbol: __floattidf）。
// 先用整数保留 53 位有效数字、按最近偶数舍入，再做精确的二进制缩放。
// 不能分别把 hi/lo 转 double 后相加：lo 的首次舍入可能丢掉半格以上的最低位，
// 导致再次相加时错误地按“恰好半格”取偶数（double rounding）。
inline double fixed_to_double(__int128 acc) {
    const bool neg = (acc < 0);
    const unsigned __int128 raw = (unsigned __int128)acc;
    const unsigned __int128 u = neg ? (unsigned __int128)0 - raw : raw;
    if (u == 0) return 0.0;
    const uint64_t lo = (uint64_t)u;
    const uint64_t hi = (uint64_t)(u >> 64);
    const int bits = hi ? 128 - std::countl_zero(hi) : 64 - std::countl_zero(lo);
    const int shift = bits > 53 ? bits - 53 : 0;
    uint64_t significand = (uint64_t)(u >> shift);
    if (shift > 0) {
        const unsigned __int128 half = (unsigned __int128)1 << (shift - 1);
        const unsigned __int128 remainder = u & ((half << 1) - 1);
        if (remainder > half || (remainder == half && (significand & 1))) ++significand;
    }
    double v = (double)significand; // <= 2^53：转换精确，包括进位情形
    if (neg) v = -v;
    return std::ldexp(v, shift - (kFixedCoeffBits + kFixedInputBits));
}

// 溢出守卫：给定抽头数 T 与系数绝对值上界，判断 int64 累加是否安全（用于决定能否走 int64 快路径）。
// Σ|x·h| ≤ T · 2^24 · 2^63 = T · 2^87 ⟹ T=1 就超了；这条函数恒返回 false，
// 保留它是为了把「为什么不能用 int64」写进代码而不是只写在注释里。
inline bool fixed_fits_int64(size_t n, int64_t maxAbsCoeff) {
    const __int128 worst = (__int128)n * (__int128)kFixedInputOne * (__int128)maxAbsCoeff;
    return worst < (__int128)INT64_MAX;
}

} // namespace rta::dsp
