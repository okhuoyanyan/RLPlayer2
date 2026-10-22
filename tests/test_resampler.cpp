// test_resampler.cpp — 多相 sinc 重采样引擎的客观验证
// 测量方法：Blackman-Harris 窗 + FFT，直接读谱（不用逐点相关法，避免 O(N*F) 爆炸）
#include "dsp/resampler.h"
#include "dsp/filter_design.h"
#include "dsp/fft.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const char* what, const char* detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) ++fails;
}

// 返回幅度谱(dB, 相对满幅)，长度 N/2
static std::vector<double> spectrum_db(const std::vector<double>& x, double fs) {
    const size_t N = x.size();
    std::vector<cplx> b(N);
    double wsum = 0.0;
    // Blackman-Harris 窗（-92 dB 旁瓣）
    for (size_t i = 0; i < N; ++i) {
        double t = 2.0 * kPi * (double)i / (double)N;
        double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        b[i] = cplx(x[i] * w, 0.0);
        wsum += w;
    }
    fft(b);
    std::vector<double> m(N / 2);
    for (size_t k = 0; k < N / 2; ++k) {
        double mag = 2.0 * std::abs(b[k]) / wsum;
        m[k] = 20.0 * std::log10(mag > 1e-300 ? mag : 1e-300);
    }
    (void)fs;
    return m;
}

static double peak_in_range(const std::vector<double>& m, double fs, size_t N,
                            double f0, double f1, int excludeBin = -1, int excl = 0) {
    size_t k0 = (size_t)(f0 / fs * (double)N);
    size_t k1 = (size_t)(f1 / fs * (double)N);
    if (k1 >= m.size()) k1 = m.size() - 1;
    double best = -999.0;
    for (size_t k = k0; k <= k1; ++k) {
        if (excludeBin >= 0 && std::abs((int)k - excludeBin) <= excl) continue;
        best = std::max(best, m[k]);
    }
    return best;
}

int main() {
    printf("=== 1. 原型滤波器响应 ===\n");
    {
        ResamplerConfig cfg;
        cfg.inRate = 44100; cfg.outRate = 384000;
        cfg.totalTaps = 262144; cfg.attenDb = 180.0; cfg.transition = 0.04;
        PolyphaseResampler rs(cfg);
        printf("  44100->384000: L=%d M=%d  每相位抽头=%d  原型总抽头=%d  群延迟=%.1f 输入样本\n",
               rs.upFactor(), rs.downFactor(), rs.tapsPerPhase(),
               rs.tapsPerPhase() * rs.upFactor(), rs.latencyInputSamples());

        const double idealCutoff = std::min(1.0 / (double)rs.upFactor(), 1.0 / (double)rs.downFactor());
        LowpassSpec spec;
        spec.taps = rs.tapsPerPhase() * rs.upFactor();
        spec.cutoff = idealCutoff * (1.0 - cfg.transition);
        spec.attenDb = cfg.attenDb;
        auto proto = design_lowpass(spec);

        const double fsInterp = 44100.0 * rs.upFactor();
        auto dbAt = [&](double hz) { return magnitude_db(proto, hz / (fsInterp / 2.0)); };

        double pmin = 1e9, pmax = -1e9;
        for (double f = 100.0; f <= 20000.0; f += 500.0) { double d = dbAt(f); pmin = std::min(pmin, d); pmax = std::max(pmax, d); }
        printf("  通带 100Hz-20kHz: [%.4f, %.4f] dB  纹波 %.4f dB\n", pmin, pmax, pmax - pmin);
        check(pmax - pmin < 0.1, "通带纹波 < 0.1 dB");

        double worst = -1e9;
        for (double f = 25000.0; f <= fsInterp / 2.0; f += 500.0) worst = std::max(worst, dbAt(f));
        printf("  阻带 25kHz-%.0fHz 最大泄漏: %.2f dB\n", fsInterp / 2.0, worst);
        check(worst < -170.0, "阻带衰减 > 170 dB");
    }

    printf("\n=== 2. 44.1k -> 384k 重采样 1 kHz 正弦 ===\n");
    {
        ResamplerConfig cfg;
        cfg.inRate = 44100; cfg.outRate = 384000; cfg.channels = 2;
        cfg.totalTaps = 262144; cfg.attenDb = 180.0; cfg.transition = 0.04;
        PolyphaseResampler rs(cfg);

        const size_t inFrames = 44100;
        std::vector<double> ix(inFrames), iy(inFrames);
        for (size_t n = 0; n < inFrames; ++n) {
            double v = 0.5 * std::sin(2.0 * kPi * 1000.0 * (double)n / 44100.0);
            ix[n] = v; iy[n] = v;
        }
        const double* in[2] = { ix.data(), iy.data() };
        std::vector<double> ox(rs.maxOutFrames(inFrames)), oy(rs.maxOutFrames(inFrames));
        double* out[2] = { ox.data(), oy.data() };
        size_t produced = rs.process(in, inFrames, out);
        printf("  输入 %zu 帧 -> 输出 %zu 帧 (理论 %.0f)\n",
               inFrames, produced, (double)inFrames * 384000.0 / 44100.0);

        const double fs = 384000.0;
        const size_t N = 131072;
        const size_t skip = 40000;                 // 跳过启动瞬态与群延迟
        std::vector<double> seg(ox.begin() + skip, ox.begin() + skip + N);
        auto m = spectrum_db(seg, fs);

        const size_t kFund = (size_t)std::llround(1000.0 / fs * (double)N);
        double fundDb = -999.0;
        for (size_t k = kFund - 2; k <= kFund + 2; ++k) fundDb = std::max(fundDb, m[k]);
        printf("  基波峰值: %.3f dB (输入 -6.02 dB)\n", fundDb);

        double harmDb = -999.0;
        for (int h = 2; h <= 10; ++h) {
            size_t k = (size_t)std::llround((double)h * 1000.0 / fs * (double)N);
            for (size_t j = (k > 3 ? k - 3 : 0); j <= k + 3; ++j) harmDb = std::max(harmDb, m[j]);
        }
        // 带外：21 kHz 以上（含重采样镜像）到 180 kHz
        double imgDb = peak_in_range(m, fs, N, 21500.0, 180000.0, (int)kFund, 4);
        printf("  最大谐波(2-10次): %.2f dB  -> 相对基波 %.2f dBc\n", harmDb, harmDb - fundDb);
        printf("  带外峰值 21.5k-180kHz: %.2f dB  -> 相对基波 %.2f dBc\n", imgDb, imgDb - fundDb);
        check(harmDb - fundDb < -100.0, "谐波 < -100 dBc");
        check(imgDb - fundDb < -100.0, "带外镜像 < -100 dBc");
    }

    printf("\n=== 3. 直流增益与相位一致性（内插增益必须是 L） ===\n");
    {
        ResamplerConfig cfg;
        cfg.inRate = 44100; cfg.outRate = 384000; cfg.channels = 1;
        cfg.totalTaps = 262144; cfg.attenDb = 180.0; cfg.transition = 0.04;
        PolyphaseResampler rs(cfg);
        const size_t inFrames = 44100;
        std::vector<double> ix(inFrames, 0.5);          // 恒定直流 0.5
        const double* in[1] = { ix.data() };
        std::vector<double> ox(rs.maxOutFrames(inFrames));
        double* out[1] = { ox.data() };
        size_t p = rs.process(in, inFrames, out);
        double mn = 1e9, mx = -1e9;
        for (size_t n = 20000; n < p; ++n) { mn = std::min(mn, ox[n]); mx = std::max(mx, ox[n]); }
        printf("  直流 0.5 输入 -> 输出范围 [%.9f, %.9f]  纹波 %.3e\n", mn, mx, mx - mn);
        check(std::fabs(mx - 0.5) < 1e-6 && std::fabs(mn - 0.5) < 1e-6, "直流增益为 1（无 L 倍衰减）");
        check((mx - mn) < 1e-6, "相位间无直流纹波");
    }

    printf("\n=== 4. 比特完美透传 (44100 -> 44100) ===\n");
    {
        ResamplerConfig cfg;
        cfg.inRate = 44100; cfg.outRate = 44100; cfg.channels = 2;
        PolyphaseResampler rs(cfg);
        check(rs.passthrough(), "识别为透传模式");

        std::vector<double> ix(1000), iy(1000);
        for (size_t n = 0; n < 1000; ++n) { ix[n] = std::sin(0.01 * (double)n); iy[n] = ix[n]; }
        const double* in[2] = { ix.data(), iy.data() };
        std::vector<double> ox(2000), oy(2000);
        double* out[2] = { ox.data(), oy.data() };
        size_t p = rs.process(in, 1000, out);
        bool identical = (p == 1000);
        for (size_t n = 0; n < p && identical; ++n)
            if (ox[n] != ix[n] || oy[n] != iy[n]) identical = false;
        check(identical, "样本逐位相同");
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
