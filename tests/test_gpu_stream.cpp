// tests/test_gpu_stream.cpp —— GPU 卷积流式正确性门禁（261026-07/08）
//
// 背景：261026-07 报告指控「GPU 卷积逐块调用无跨调用状态 ⟹ IR 拖尾丢失」。
// 主线用自写探针独立复测确认指控成立（误差 +1.7~+3.4 dB，超过信号能量，且 IR=1024 就错），
// 生产管线随即停用 GPU 卷积兜底。
//
// 261026-08：rtagpu.dll 增加**带跨调用状态的流式入口**
// （rta_gpu_convolve_create / _stream / _destroy，latency()==block，
//   与 CPU StreamingConvolver 是同一条 FIFO 实现），本测试升级为**硬门禁**：
//
//   判据：GpuStreamConvolver（走 GPU 时）与 CPU StreamingConvolver
//         逐样本 max|Δ| ≤ 1e-6 且相对 RMS ≤ −120 dB，三档 IR 全过。
//
// 无卡 / 无 DLL / 老 DLL（无流式符号）时 GPU 段自动 SKIP，CPU 判据照跑。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "dsp/convolution.h"
#include "dsp/gpu_bridge.h"

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : "  ", detail.c_str());
}
std::vector<double> make_signal(size_t n, unsigned seed) {
    std::vector<double> v(n);
    unsigned s = seed;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        const double noise = ((double)((s >> 16) & 0x7FFF) / 32768.0 - 0.5) * 0.25;
        v[i] = 0.6 * std::sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 44100.0) + noise;
    }
    return v;
}
std::vector<double> make_ir(size_t n) {
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
struct Diff { double relDb = 0.0, maxAbs = 0.0; };
Diff diffOf(const std::vector<double>& a, const std::vector<double>& b, size_t skip) {
    Diff d;
    double se = 0.0, sr = 0.0;
    for (size_t i = skip; i < a.size() && i < b.size(); ++i) {
        const double x = a[i] - b[i];
        se += x * x; sr += a[i] * a[i];
        if (std::fabs(x) > d.maxAbs) d.maxAbs = std::fabs(x);
    }
    d.relDb = (sr > 0.0) ? 10.0 * std::log10((se + 1e-300) / sr) : 0.0;
    return d;
}
}  // namespace

int main() {
    std::printf("=== GPU 卷积流式正确性（261026-07 缺陷 / 261026-08 修复）===\n");
    const size_t N = 262144, CHUNK = 4096, BLOCK = 1024;
    const auto sig = make_signal(N, 12345u);

    auto& gb = rta::dsp::GpuBridge::instance();
    const bool gpuAvail = gb.available();
    const bool streamOk = gb.stream_supported();
    std::printf("  rtagpu.dll: %s\n", gpuAvail ? "可用" : "不可用");
    std::printf("  流式符号 : %s\n", streamOk ? "齐全" : "缺失（老 DLL 或非本机卡 ⟹ GPU 段 SKIP）");
    std::printf("  status   : %s\n", gb.status().c_str());

    for (size_t irLen : { size_t(1024), size_t(8192), size_t(65536) }) {
        const auto ir = make_ir(irLen);

        // 参考 1：CPU 整块喂
        rta::dsp::StreamingConvolver whole(ir, BLOCK);
        std::vector<double> outWhole(N, 0.0);
        whole.process(sig.data(), outWhole.data(), N);

        // 参考 2：CPU 分块喂（与生产同形态）
        rta::dsp::StreamingConvolver cpu(ir, BLOCK);
        std::vector<double> outCpu(N, 0.0);
        for (size_t p = 0; p < N; p += CHUNK) {
            const size_t n = (p + CHUNK <= N) ? CHUNK : (N - p);
            cpu.process(sig.data() + p, outCpu.data() + p, n);
        }
        std::printf("\nIR=%zu:\n", irLen);
        const Diff dCpu = diffOf(outWhole, outCpu, BLOCK * 4);
        check(dCpu.maxAbs == 0.0, "CPU 流式与整块逐位一致",
              "max|d|=" + std::to_string(dCpu.maxAbs) + " relRMS=" + std::to_string(dCpu.relDb) + " dB");

        if (!streamOk) { std::printf("  [SKIP] GPU 流式（无流式符号）\n"); continue; }

        // 被测：GpuStreamConvolver（内部自动回退 CPU；on_gpu() 告诉实情）
        rta::dsp::GpuStreamConvolver gsc(ir, BLOCK);
        std::vector<double> outGpu(N, 0.0);
        for (size_t p = 0; p < N; p += CHUNK) {
            const size_t n = (p + CHUNK <= N) ? CHUNK : (N - p);
            gsc.process(sig.data() + p, outGpu.data() + p, n);
        }
        if (!gsc.on_gpu()) {
            std::printf("  [SKIP] GPU 流式（会话建不起来，已回退 CPU）\n");
            continue;
        }
        const Diff dGpu = diffOf(outCpu, outGpu, BLOCK * 4);
        check(dGpu.relDb <= -120.0 && dGpu.maxAbs <= 1e-6,
              "GPU 流式与 CPU 流式等价（判据 relRMS<=-120dB 且 max|d|<=1e-6）",
              "relRMS=" + std::to_string(dGpu.relDb) + " dB max|d|=" + std::to_string(dGpu.maxAbs));
    }

    std::printf("\n%s (失败项 %d)\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
