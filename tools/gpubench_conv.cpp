// gpubench_conv.cpp — 卷积后端性能对标（**走生产路径**，不是独立 demo）
//
// 为什么要这个文件：HIP 那边的 tools/gpu_conv.cpp 是手工编的独立 demo，不跟着源码
// 更新，曾经掩盖过「gpu_bridge.h 没取符号、GPU 从未真正启用」这个 bug（见 verify.ps1
// 第 4 步的注释）。所以 CUDA 侧的性能数字一律从 GpuBridge::instance() 拿——
// 也就是管线真正走的那条路；换后端（HIP/CUDA）只换 DLL，本工具不用改。
//
// 口径（与 docs/GPU卷积纪要.md 的 HIP 记录可比）：
//   IR 262144 抽头 / 信号 48000 帧（1 秒 @48k）/ 分块 1024
//   GPU 端到端 = GpuBridge::convolve 全程（CPU 端 FFT 准备 + 上传 + kernel + 下载 + IFFT + 叠加）
//   CPU 对照   = StreamingConvolver（均匀分块 FFT，公平对手；不是 O(N·M) 直接卷积）
//
// 一致性：StreamingConvolver 预置 B 帧延迟，所以按延迟对齐后比较（out_cpu[i] vs out_gpu[i-B]）。
#include "dsp/gpu_bridge.h"
#include "dsp/convolution.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <string>

using namespace rta::dsp;

static double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

int main(int argc, char** argv) {
    size_t irLen = 262144;
    size_t N     = 48000;
    size_t block = 1024;
    if (argc > 1) irLen = (size_t)strtoull(argv[1], nullptr, 10);
    if (argc > 2) N     = (size_t)strtoull(argv[2], nullptr, 10);
    if (argc > 3) block = (size_t)strtoull(argv[3], nullptr, 10);

    auto& gb = GpuBridge::instance();
    printf("GPU 后端：%s\n", gb.status().c_str());
    printf("参数：IR %zu 抽头 / 信号 %zu 帧 / 分块 %zu\n\n", irLen, N, block);
    if (!gb.available()) {
        printf("[SKIP] GPU 不可用（%s）——本次没有测到 GPU 性能，不要当成通过\n", gb.status().c_str());
        return 0;
    }

    std::mt19937 rng(20261022u);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<double> x(N), ir(irLen);
    for (auto& v : x) v = 0.3 * u(rng);
    for (size_t i = 0; i < irLen; ++i) ir[i] = u(rng) * std::exp(-6.0 * (double)i / (double)irLen);

    // ---- GPU（生产路径）----
    std::vector<double> gpu(N, 0.0);
    (void)gb.convolve(x.data(), N, ir.data(), irLen, gpu.data(), (int)block);   // 预热（首次含上下文初始化）
    auto t0 = std::chrono::steady_clock::now();
    const int rc = gb.convolve(x.data(), N, ir.data(), irLen, gpu.data(), (int)block);
    const double gpuMs = ms_since(t0);

    // ---- CPU：均匀分块 FFT（公平对照）----
    std::vector<double> cpu(N, 0.0);
    StreamingConvolver sc(ir, block);
    auto t1 = std::chrono::steady_clock::now();
    sc.process(x.data(), cpu.data(), N);
    const double cpuMs = ms_since(t1);

    // ---- 一致性（按 StreamingConvolver 的 block 延迟对齐）----
    const size_t latency = sc.latency();
    double maxAbs = 0.0, maxRef = 0.0;
    for (size_t i = latency; i < N; ++i) {
        const double d = std::fabs(cpu[i] - gpu[i - latency]);
        if (d > maxAbs) maxAbs = d;
        if (std::fabs(cpu[i]) > maxRef) maxRef = std::fabs(cpu[i]);
    }
    const double relDb = 20.0 * std::log10(maxAbs / (maxRef > 0 ? maxRef : 1.0) + 1e-300);

    printf("=== 耗时 ===\n");
    printf("  CPU 分块 FFT（StreamingConvolver） : %9.2f ms\n", cpuMs);
    printf("  GPU 端到端（GpuBridge::convolve）  : %9.2f ms   (rc=%d)\n", gpuMs, rc);
    printf("  加速比                             : %9.1fx\n", cpuMs / (gpuMs > 1e-9 ? gpuMs : 1e-9));
    printf("\n=== 一致性（延迟对齐 %zu 帧）===\n", latency);
    printf("  最大绝对偏差 %.3e（参考峰值 %.3e，相对 %.1f dB）\n", maxAbs, maxRef, relDb);

    const bool ok = (rc == 0) && (relDb < -100.0);
    printf("\n[%s] GPU 卷积与 CPU 分块 FFT 一致（判据：相对偏差 < -100 dB）\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
