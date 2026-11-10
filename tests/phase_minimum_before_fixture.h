#pragma once
#include "dsp/filter_families.h"
namespace rta::dsp::phase_before {
inline std::vector<double> to_minimum_phase(const std::vector<double>& h, size_t fftOversample = 8, bool enforceReal = false) {
    const size_t L = h.size();
    // ★ 2026-10-31（独立审核 2.4.5）：这里的倍率是**倒谱采样密度**，不是输出支撑长度。
    //   8 倍会让实倒谱的尾部混叠 ⟹ 通带误差最大 0.21 dB（即使线性相位那级是平的）。
    //   级联分支用 32 倍（高斯 64 倍）；单段保持 8 倍（全局改会造成 12~28 dB 的深阻带退化）。
    size_t N = 1; while (N < L * fftOversample) N <<= 1;

    std::vector<cplx> H(N, cplx(0.0, 0.0));
    for (size_t i = 0; i < L; ++i) H[i] = cplx(h[i], 0.0);
    rta::dsp::fft(H);

    // 1) 对数幅度谱。地板必须取【相对峰值】的值而不是绝对值：
    //    高斯窗没有旁瓣结构，阻带可以深到 -300 dB 以下，若用绝对地板，
    //    深阻带会被整体压平，倒谱里出现大量"假"的 -69 项，反而污染整个最小相位解。
    double peakMag = 0.0;
    for (size_t k = 0; k < N; ++k) peakMag = std::max(peakMag, std::abs(H[k]));
    const double floorMag = std::max(peakMag * 1e-15, 1e-300);
    std::vector<cplx> logH(N);
    for (size_t k = 0; k < N; ++k) {
        const double mag = std::max(std::abs(H[k]), floorMag);
        logH[k] = cplx(std::log(mag), 0.0);
    }

    // 实系数 FIR 要求对数幅度是**偶函数**。深阻带零点附近 FFT 舍入的相对误差很大，
    // 两半独立取对数会凭空造出虚倒谱，破坏重建出的通带（审核实测）。
    if (enforceReal) for (size_t k = 1; k < N / 2; ++k) logH[N - k] = logH[k];

    // 2) 实倒谱
    rta::dsp::fft(logH, /*inverse=*/true);

    // 3) 折叠：n=0 保留；1..N/2-1 加倍；N/2 保留；其余置零
    std::vector<cplx> c(N, cplx(0.0, 0.0));
    c[0] = enforceReal ? cplx(logH[0].real(), 0.0) : logH[0];
    for (size_t n = 1; n < N / 2; ++n) c[n] = enforceReal ? cplx(logH[n].real() * 2.0, 0.0) : logH[n] * 2.0;
    c[N / 2] = enforceReal ? cplx(logH[N / 2].real(), 0.0) : logH[N / 2];

    // 4) 指数还原 -> 最小相位频谱
    rta::dsp::fft(c);
    for (auto& v : c) v = std::exp(v);

    // 5) 回到时域，取前 L 点（因果）
    rta::dsp::fft(c, /*inverse=*/true);
    std::vector<double> out(L);
    for (size_t i = 0; i < L; ++i) out[i] = c[i].real();

    // 归一化到单位直流增益
    double sum = 0.0; for (auto v : out) sum += v;
    if (std::fabs(sum) > 1e-300) for (auto& v : out) v /= sum;
    return out;
}

}
