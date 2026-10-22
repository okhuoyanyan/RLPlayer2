// matrix.h — Matrix 处理：双二阶 EQ（对应手册 "iir" 插件）、延迟（"delay" 插件）、通道路由
//
// 手册 7.3 的 iir 插件类型与本文件一一对应：
//   lp / lp1 / hp / hp1 / bp / ap / notch / peak / lshelf / hshelf / biquad
// 手册 7.2 的 delay 插件参数：s=样本  t=秒  d=米（v 默认 343.956 m/s）
//
// 双二阶采用 RBJ Audio EQ Cookbook 公式，直接型转置 II（数值稳定性好）。
#pragma once
#include "kaiser.h"
#include <vector>
#include <cmath>
#include <complex>
#include <string>
#include <cstddef>
#include <algorithm>

namespace rta::dsp {

// ---------------- 双二阶 ----------------
class Biquad {
public:
    Biquad() = default;
    void set(double b0, double b1, double b2, double a0, double a1, double a2) {
        const double inv = (a0 != 0.0) ? 1.0 / a0 : 0.0;
        b0_ = b0 * inv; b1_ = b1 * inv; b2_ = b2 * inv;
        a1_ = a1 * inv; a2_ = a2 * inv;
        reset();
    }
    void reset() { z1_ = z2_ = 0.0; }

    double process(double x) {
        const double y = b0_ * x + z1_;            // 直接型转置 II
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }
    // 解析幅频响应（dB），freqNorm: 1.0 = Nyquist
    double response_db(double freqNorm) const {
        const double w = kPi * freqNorm;
        const std::complex<double> z1 = std::polar(1.0, -w);
        const std::complex<double> z2 = z1 * z1;
        const auto num = b0_ + b1_ * z1 + b2_ * z2;
        const auto den = 1.0 + a1_ * z1 + a2_ * z2;
        return 20.0 * std::log10(std::abs(num) / (std::abs(den) + 1e-300) + 1e-300);
    }
    // 用脉冲响应实测幅频（交叉验证解析式）
    double measured_response_db(double freqNorm, size_t n = 8192) const {
        Biquad tmp = *this;
        tmp.reset();
        const double w = kPi * freqNorm;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double y = tmp.process(i == 0 ? 1.0 : 0.0);
            re += y * std::cos(w * (double)i);
            im -= y * std::sin(w * (double)i);
        }
        return 20.0 * std::log10(std::sqrt(re * re + im * im) + 1e-300);
    }
private:
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
};

// ---------------- RBJ 设计公式 ----------------
namespace biquad_design {

inline void raw(Biquad& q, double b0, double b1, double b2, double a0, double a1, double a2) {
    q.set(b0, b1, b2, a0, a1, a2);
}
inline void peak(Biquad& q, double fs, double f0, double Q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * Q);
    q.set(1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
}
inline void lowShelf(Biquad& q, double fs, double f0, double S, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
    const double tsa = 2.0 * std::sqrt(A) * alpha;
    q.set(A * ((A + 1) - (A - 1) * cw + tsa),
          2 * A * ((A - 1) - (A + 1) * cw),
          A * ((A + 1) - (A - 1) * cw - tsa),
          (A + 1) + (A - 1) * cw + tsa,
          -2 * ((A - 1) + (A + 1) * cw),
          (A + 1) + (A - 1) * cw - tsa);
}
inline void highShelf(Biquad& q, double fs, double f0, double S, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
    const double tsa = 2.0 * std::sqrt(A) * alpha;
    q.set(A * ((A + 1) + (A - 1) * cw + tsa),
          -2 * A * ((A - 1) + (A + 1) * cw),
          A * ((A + 1) + (A - 1) * cw - tsa),
          (A + 1) - (A - 1) * cw + tsa,
          2 * ((A - 1) - (A + 1) * cw),
          (A + 1) - (A - 1) * cw - tsa);
}
// 标准 RBJ 架式滤波器设计（指定品质因数 Q，alpha = sin(w0)/(2Q)）
inline void lowShelfQ(Biquad& q, double fs, double f0, double Q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * (Q > 1e-6 ? Q : 0.7071));
    const double tsa = 2.0 * std::sqrt(A) * alpha;
    q.set(A * ((A + 1) - (A - 1) * cw + tsa),
          2 * A * ((A - 1) - (A + 1) * cw),
          A * ((A + 1) - (A - 1) * cw - tsa),
          (A + 1) + (A - 1) * cw + tsa,
          -2 * ((A - 1) + (A + 1) * cw),
          (A + 1) + (A - 1) * cw - tsa);
}
inline void highShelfQ(Biquad& q, double fs, double f0, double Q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * (Q > 1e-6 ? Q : 0.7071));
    const double tsa = 2.0 * std::sqrt(A) * alpha;
    q.set(A * ((A + 1) + (A - 1) * cw + tsa),
          -2 * A * ((A - 1) + (A + 1) * cw),
          A * ((A + 1) + (A - 1) * cw - tsa),
          (A + 1) - (A - 1) * cw + tsa,
          2 * ((A - 1) - (A + 1) * cw),
          (A + 1) - (A - 1) * cw - tsa);
}
// 便捷重载：lowShelfQ(f, gain, q, sr) 直接返回 Biquad
inline Biquad lowShelfQ(double f0, double gainDb, double Q, double fs) {
    Biquad q;
    lowShelfQ(q, fs, f0, Q, gainDb);
    return q;
}
inline Biquad highShelfQ(double f0, double gainDb, double Q, double fs) {
    Biquad q;
    highShelfQ(q, fs, f0, Q, gainDb);
    return q;
}
inline void lowpass(Biquad& q, double fs, double f0, double Q) {
    const double w0 = 2.0 * kPi * f0 / fs, cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * Q);
    q.set((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}
inline void highpass(Biquad& q, double fs, double f0, double Q) {
    const double w0 = 2.0 * kPi * f0 / fs, cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * Q);
    q.set((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}
inline void bandpass(Biquad& q, double fs, double f0, double Q) {
    const double w0 = 2.0 * kPi * f0 / fs, cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * Q);
    q.set(alpha, 0.0, -alpha, 1 + alpha, -2 * cw, 1 - alpha);
}
inline void notch(Biquad& q, double fs, double f0, double Q) {
    const double w0 = 2.0 * kPi * f0 / fs, cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * Q);
    q.set(1.0, -2 * cw, 1.0, 1 + alpha, -2 * cw, 1 - alpha);
}
inline void allpass(Biquad& q, double fs, double f0, double Q) {
    const double w0 = 2.0 * kPi * f0 / fs, cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * Q);
    q.set(1 - alpha, -2 * cw, 1 + alpha, 1 + alpha, -2 * cw, 1 - alpha);
}
// 一阶（手册 lp1 / hp1）：用 K = tan(w0/2) 的双线性设计
inline void lowpass1(Biquad& q, double fs, double f0) {
    const double K = std::tan(kPi * f0 / fs);
    q.set(K, K, 0.0, K + 1.0, K - 1.0, 0.0);
}
inline void highpass1(Biquad& q, double fs, double f0) {
    const double K = std::tan(kPi * f0 / fs);
    q.set(1.0, -1.0, 0.0, K + 1.0, K - 1.0, 0.0);
}

} // namespace biquad_design

// ---------------- 延迟线（手册 "delay" 插件）----------------
class DelayLine {
public:
    void set_samples(size_t n) { buf_.assign(n > 0 ? n : 1, 0.0); pos_ = 0; n_ = n; }
    void set_seconds(double sec, double fs) { set_samples((size_t)std::llround(sec * fs)); }
    void set_meters(double m, double fs, double v = 343.956) { set_seconds(m / v, fs); }
    size_t samples() const { return n_; }
    void reset() { std::fill(buf_.begin(), buf_.end(), 0.0); pos_ = 0; }

    double process(double x) {
        if (n_ == 0) return x;
        const double y = buf_[pos_];
        buf_[pos_] = x;
        pos_ = (pos_ + 1) % buf_.size();
        return y;
    }
private:
    std::vector<double> buf_{std::vector<double>(1, 0.0)};
    size_t pos_ = 0, n_ = 0;
};

// ---------------- Matrix 路由 ----------------
struct MatrixRoute {
    int    src  = 0;
    int    dst  = 0;
    double gain = 1.0;      // 线性增益（可负 = 反相，用于 M/S 处理）
};

inline double db_to_linear(double db) { return std::pow(10.0, db / 20.0); }

// 把一个源声道按多条路由混合到若干目标声道
class MatrixMixer {
public:
    void configure(int inCh, int outCh, std::vector<MatrixRoute> routes) {
        inCh_ = inCh; outCh_ = outCh; routes_ = std::move(routes);
        mix_.assign((size_t)outCh_, {});
    }
    const std::vector<MatrixRoute>& routes() const { return routes_; }

    void process(const double* const* in, double* const* out, size_t frames) {
        for (int c = 0; c < outCh_; ++c) std::fill(out[c], out[c] + frames, 0.0);
        for (const auto& r : routes_) {
            if (r.src < 0 || r.src >= inCh_ || r.dst < 0 || r.dst >= outCh_) continue;
            const double* s = in[r.src];
            double* d = out[r.dst];
            const double g = r.gain;
            for (size_t i = 0; i < frames; ++i) d[i] += g * s[i];
        }
    }
private:
    int inCh_ = 2, outCh_ = 2;
    std::vector<MatrixRoute> routes_;
    std::vector<std::vector<double>> mix_;
};

} // namespace rta::dsp
