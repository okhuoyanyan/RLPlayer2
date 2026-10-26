// gpu_stream_verify.cpp —— 独立验证 261026-07 报告（主线自己写，不依赖外部探针）
// 判据：同一输入、同一 IR，
//   参考 = CPU StreamingConvolver 整块喂
//   路径A = CPU 分块喂（4096/块）—— 设计承诺与整块逐位一致
//   路径B = GPU 逐块调用（与 pipeline.h:323 完全相同的调用形态）
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "dsp/convolution.h"
#include "dsp/gpu_bridge.h"

static std::vector<double> make_signal(size_t n, unsigned seed) {
    std::vector<double> v(n);
    unsigned s = seed;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        const double noise = ((double)((s >> 16) & 0x7FFF) / 32768.0 - 0.5) * 0.25;
        v[i] = 0.6 * std::sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 44100.0) + noise;
    }
    return v;
}
static std::vector<double> make_ir(size_t n) {
    std::vector<double> v(n, 0.0);
    unsigned s = 987654321u;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        const double env = std::exp(-(double)i / (double)(n / 6 + 1));
        v[i] = (((double)((s >> 16) & 0x7FFF) / 32768.0) - 0.5) * 0.5 * env;
    }
    v[0] = 1.0;
    return v;
}
int main() {
    const size_t N = 262144, CHUNK = 4096, BLOCK = 1024;
    const auto sig = make_signal(N, 12345u);
    auto& gb = rta::dsp::GpuBridge::instance();
    std::printf("GPU available=%d status=%s\n", (int)gb.available(), gb.status().c_str());

    for (size_t irLen : { size_t(1024), size_t(8192), size_t(65536), size_t(262144) }) {
        const auto ir = make_ir(irLen);
        // 参考：整块喂 CPU
        rta::dsp::StreamingConvolver ref(ir, BLOCK);
        std::vector<double> outRef(N, 0.0);
        ref.process(sig.data(), outRef.data(), N);
        // 路径A：分块喂 CPU
        rta::dsp::StreamingConvolver cpu(ir, BLOCK);
        std::vector<double> outCpu(N, 0.0);
        for (size_t p = 0; p < N; p += CHUNK) {
            const size_t n = (p + CHUNK <= N) ? CHUNK : (N - p);
            cpu.process(sig.data() + p, outCpu.data() + p, n);
        }
        // 路径B：逐块调 GPU（与 pipeline 同形态）
        std::vector<double> outGpu(N, 0.0);
        bool gpuOk = gb.available();
        if (gpuOk) {
            for (size_t p = 0; p < N; p += CHUNK) {
                const size_t n = (p + CHUNK <= N) ? CHUNK : (N - p);
                std::vector<double> tmp(n, 0.0);
                if (gb.convolve(sig.data() + p, n, ir.data(), ir.size(), tmp.data(), (int)BLOCK) != 0) {
                    gpuOk = false; break;
                }
                for (size_t i = 0; i < n; ++i) outGpu[p + i] = tmp[i];
            }
        }
        auto cmp = [&](const std::vector<double>& a, const std::vector<double>& b, const char* tag) {
            double se = 0.0, sr = 0.0, mx = 0.0;
            const size_t skip = BLOCK * 4;   // 跳过启动瞬态
            for (size_t i = skip; i < N; ++i) {
                const double d = a[i] - b[i];
                se += d * d; sr += a[i] * a[i];
                if (std::fabs(d) > mx) mx = std::fabs(d);
            }
            const double rms = (sr > 0.0) ? 10.0 * std::log10((se + 1e-300) / sr) : 0.0;
            std::printf("  %-8s 相对RMS %8.1f dB | max|d| %.3e\n", tag, rms, mx);
        };
        std::printf("IR=%zu:\n", irLen);
        cmp(outRef, outCpu, "CPU流");
        if (gpuOk) cmp(outRef, outGpu, "GPU流");
        else std::printf("  GPU流    跳过（不可用）\n");
    }
    return 0;
}
