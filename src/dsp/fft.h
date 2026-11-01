// fft.h — 紧凑 radix-2 复数 FFT（用于客观测量与后续分块卷积的基础）
#pragma once
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（输出进入音频样本）
#include "kaiser.h"          // 提供 kPi
#include <vector>
#include <complex>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace rta::dsp {

using cplx = std::complex<double>;

// 原地基-2 FFT，n 必须是 2 的幂
inline void fft(std::vector<cplx>& a, bool inverse = false) {
    const size_t n = a.size();
    if (n < 2) return;
    if (n & (n - 1)) throw std::invalid_argument("FFT length must be a power of two");
    // One last-size table per thread, bounded to 512 KiB of root values.
    // Larger transforms use a local table without evicting the small cache.
    // Release the old size before allocating another: no retained spare table.
    static thread_local std::vector<cplx> cachedRoots;
    std::vector<cplx> localRoots;
    auto& roots = n <= 65536 ? cachedRoots : localRoots;
    if (roots.size() != n / 2) {
        std::vector<cplx>().swap(roots);
        roots = std::vector<cplx>(n / 2);
        for (size_t k = 0; k < roots.size(); ++k) {
            const double angle = -2.0 * kPi * ((double)k / (double)n);
            roots[k] = cplx(std::cos(angle), std::sin(angle));
        }
        roots[0] = cplx(1.0, 0.0);
        if (n >= 4) roots[n / 4] = cplx(0.0, -1.0);
    }
    // Obtain roots before touching input, including on allocation failure.
    // 位反转置换
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    // Every stage shares accurate direct roots, never the drifting recurrence.
    for (size_t len = 2;; len <<= 1) {
        const size_t stride = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < len / 2; ++k) {
                const cplx w = inverse ? std::conj(roots[k * stride]) : roots[k * stride];
                const cplx u = a[i + k];
                const cplx v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
        if (len == n) break; // Do not overflow size_t after the final stage.
    }
    if (inverse) for (auto& x : a) x /= (double)n;
}

// 计算实信号在给定频点上的幅度（dB，相对满幅 1.0）。直接相关法；非整周期会有窗泄漏，DC/Nyquist 不适用双边幅度乘 2。
inline double tone_magnitude_db(const std::vector<double>& x, double freqHz, double sampleRate) {
    const double w = 2.0 * kPi * freqHz / sampleRate;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        re += x[i] * std::cos(w * (double)i);
        im -= x[i] * std::sin(w * (double)i);
    }
    double mag = 2.0 * std::sqrt(re * re + im * im) / (double)x.size();
    return 20.0 * std::log10(mag > 1e-300 ? mag : 1e-300);
}

} // namespace rta::dsp
