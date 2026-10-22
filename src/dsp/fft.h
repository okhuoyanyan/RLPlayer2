// fft.h — 紧凑 radix-2 复数 FFT（用于客观测量与后续分块卷积的基础）
#pragma once
#include "kaiser.h"          // 提供 kPi
#include <vector>
#include <complex>
#include <cmath>
#include <cstddef>

namespace rta::dsp {

using cplx = std::complex<double>;

// 原地基-2 FFT，n 必须是 2 的幂
inline void fft(std::vector<cplx>& a, bool inverse = false) {
    const size_t n = a.size();
    if (n < 2) return;
    // 位反转置换
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / (double)len * (inverse ? 1.0 : -1.0);
        const cplx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cplx w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                cplx u = a[i + k];
                cplx v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= (double)n;
}

// 计算实信号在给定频点上的幅度（dB，相对满幅 1.0）。非整周期也准确（直接相关法）。
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
