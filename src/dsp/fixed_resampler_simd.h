#pragma once
// Exact Q62-coefficient / Q24-input SIMD for four independent output lanes.
#include "fixed_conv.h"
#include "resampler_simd.h"
#include <algorithm>
#include <climits>
#include <limits>
#include <stdexcept>

namespace rta::dsp::fixed_resampler_simd {
// Four adjacent phases sharing the same history and valid tap count.
// Contract: |history[base-u]| <= 2^24; coefficients may use all int64 values.
// For coefficient h, let lo=signed(low32(h)), hi=signed(high32(h)).
// h = lo + (hi + (lo<0))*2^32. Do not form hi+carry in int32: it can be 2^31.
// Instead add carry*x to the int64 high product after signed 32x32 multiplication.
// Each low/high product is bounded by 2^55; 64-tap blocks by 2^61.
// With taps<=INT_MAX, the final exact sum is bounded by 2^118 < 2^127.
template<bool Temporal>
inline void scalar_group(const int64_t* coeff, size_t stride,
                          const int64_t* history, size_t base, size_t taps, size_t step,
                          __int128* output) {
    for (size_t lane = 0; lane < 4; ++lane) {
        __int128 sum = 0;
        for (size_t u = 0; u < taps; ++u)
            sum += (__int128)(Temporal ? coeff[u] : coeff[u * stride + lane]) *
                   history[base + (Temporal ? lane * step : 0) - u];
        output[lane] = sum;
    }
}

#if RTA_RESAMPLER_AVX2
template<bool Temporal>
__attribute__((target("avx2,no-fma"), noinline))
inline void avx2_group(const int64_t* coeff, size_t stride,
                        const int64_t* history, size_t base, size_t taps, size_t step,
                        __int128* output) {
    for (int lane = 0; lane < 4; ++lane) output[lane] = 0;
    const __m256i zero = _mm256_setzero_si256();
    for (size_t first = 0; first < taps; first += 64) {
        const size_t end = std::min(taps, first + 64);
        __m256i low = zero, high = zero;
        for (size_t u = first; u < end; ++u) {
            __m256i h, x;
            if constexpr (Temporal) {
                h = _mm256_set1_epi64x(coeff[u]);
                const int64_t* sample = history + base - u;
                x = _mm256_setr_epi64x(sample[0], sample[step], sample[2 * step], sample[3 * step]);
            } else {
                h = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(coeff + u * stride));
                x = _mm256_set1_epi64x(history[base - u]);
            }
            const __m256i low_negative = _mm256_cmpgt_epi64(zero, _mm256_slli_epi64(h, 32));
            low = _mm256_add_epi64(low, _mm256_mul_epi32(h, x));
            const __m256i high_product = _mm256_add_epi64(
                _mm256_mul_epi32(_mm256_srli_epi64(h, 32), x),
                _mm256_and_si256(low_negative, x));
            high = _mm256_add_epi64(high, high_product);
        }
        int64_t low_lane[4], high_lane[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(low_lane), low);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(high_lane), high);
        for (size_t lane = 0; lane < 4; ++lane) {
            // Multiplication is defined for negative high parts; signed left shift is not.
            output[lane] += (__int128)high_lane[lane] * ((__int128)1 << 32) + low_lane[lane];
        }
    }
}
#endif

// Returns whether SIMD actually ran. Unsupported CPUs/disabled builds use scalar.
template<bool Temporal>
inline bool exact_group(const int64_t* coeff, size_t stride,
                        const int64_t* history, size_t base, size_t taps,
                        size_t step, __int128* output, bool allow_simd) {
    if (taps > INT_MAX || (taps && base < taps - 1))
        throw std::invalid_argument("Invalid fixed SIMD group tap dimensions");
    if constexpr (Temporal) {
        if (step == 0 || step > (std::numeric_limits<size_t>::max() - base) / 3)
            throw std::invalid_argument("Invalid fixed SIMD temporal span");
    } else {
        if (stride < 4 || (taps && stride > std::numeric_limits<size_t>::max() / taps))
            throw std::invalid_argument("Invalid fixed SIMD phase stride");
    }
#if RTA_RESAMPLER_AVX2
    if (allow_simd && resampler_avx2_available()) {
        avx2_group<Temporal>(coeff, stride, history, base, taps, step, output);
        return true;
    }
#endif
    (void)allow_simd;
    scalar_group<Temporal>(coeff, stride, history, base, taps, step, output);
    return false;
}

inline bool phase4(const int64_t* coeff, size_t stride,
                   const int64_t* history, size_t base, size_t taps,
                   __int128* output, bool allow_simd = true) {
    return exact_group<false>(coeff, stride, history, base, taps, 0, output, allow_simd);
}

// Four occurrences of the same phase, at base + lane*step. A rational
// resampler uses step=M and scatters these four results at output stride L.
// No coefficient transpose is needed. The earliest base must have all taps.
inline bool temporal4(const int64_t* coeff, const int64_t* history,
                      size_t base, size_t taps, size_t step,
                      __int128* output, bool allow_simd = true) {
    return exact_group<true>(coeff, 1, history, base, taps, step, output, allow_simd);
}
} // namespace rta::dsp::fixed_resampler_simd
