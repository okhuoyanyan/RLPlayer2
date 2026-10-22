// gpu_resample_test.cpp — GPU 升频核的正确性与速度验证
//
// 判据：
//   1. 与 CPU 参考实现的点积结果最大偏差应 < 1e-14（同为 double，只是并行）
//   2. 端到端与 CPU 完整重采样器比对（在 resampler 接入 GPU 后由 test_resampler 覆盖）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include <random>
#include <chrono>
#include "dsp/gpu_pref.h"   // 与 GpuBridge 共用同一份候选 DLL 顺序（单一真相源）

typedef int (*fn_resample)(const double*, int, long long,
                           const double*, int, int,
                           const int*, const int*, long long, double*);

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 候选 DLL 顺序与管线完全一致（gpu_pref.h）；也可显式指定：
    //   gpu_resample_test.exe rtagpu_cuda.dll
    std::vector<std::string> cands;
    if (argc > 1) cands.push_back(argv[1]);
    else for (const char* nm : rta::dsp::gpu_dll_candidates()) cands.push_back(nm);

    HMODULE h = nullptr;
    fn_resample fn = nullptr;
    std::string used;
    std::string tried;
    for (const auto& nm : cands) {
        h = LoadLibraryA(nm.c_str());
        if (!h) { tried += nm + "(缺失) "; continue; }
        fn = (fn_resample)GetProcAddress(h, "rta_gpu_resample");
        if (!fn) { FreeLibrary(h); h = nullptr; tried += nm + "(无导出) "; continue; }
        used = nm;
        break;
    }
    if (!h || !fn) {
        printf("没有可加载的 GPU 后端（尝试过：%s）\n", tried.c_str());
        return 1;
    }
    printf("已加载 %s 的 rta_gpu_resample\n\n", used.c_str());

    const int ch = 2, L = 16, T = 16384;
    const long long nOut = 4096;
    const long long histLen = T;

    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<double> hist((size_t)ch * histLen);
    for (auto& v : hist) v = dist(rng);
    std::vector<double> phases((size_t)L * T);
    for (auto& v : phases) v = dist(rng) * 1e-4;

    std::vector<int> bases(nOut), phIdx(nOut);
    for (long long n = 0; n < nOut; ++n) {
        bases[(size_t)n] = (int)(T - 1);          // 总是有完整历史
        phIdx[(size_t)n] = (int)(n % L);
    }

    // --- CPU 参考 ---
    std::vector<double> cpu((size_t)nOut * ch, 0.0);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (long long n = 0; n < nOut; ++n) {
        const double* ph = phases.data() + (size_t)phIdx[(size_t)n] * T;
        for (int c = 0; c < ch; ++c) {
            const double* hh = hist.data() + (size_t)c * histLen;
            double acc = 0.0;
            for (int u = 0; u < T; ++u) acc += ph[u] * hh[bases[(size_t)n] - u];
            cpu[(size_t)(n * ch + c)] = acc;
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    const double cpuMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // --- GPU ---
    std::vector<double> gpu((size_t)nOut * ch, 0.0);
    fn(hist.data(), ch, histLen, phases.data(), L, T, bases.data(), phIdx.data(), nOut, gpu.data());  // 预热
    auto t2 = std::chrono::high_resolution_clock::now();
    const int rc = fn(hist.data(), ch, histLen, phases.data(), L, T, bases.data(), phIdx.data(), nOut, gpu.data());
    auto t3 = std::chrono::high_resolution_clock::now();
    const double gpuMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

    printf("CPU: %.2f ms\n", cpuMs);
    printf("GPU: %.2f ms  (rc=%d)\n", gpuMs, rc);

    double maxAbs = 0.0, maxRel = 0.0, sumSq = 0.0;
    for (size_t i = 0; i < cpu.size(); ++i) {
        const double d = std::fabs(cpu[i] - gpu[i]);
        maxAbs = std::max(maxAbs, d);
        sumSq += cpu[i] * cpu[i];
        if (std::fabs(cpu[i]) > 1e-30) maxRel = std::max(maxRel, d / std::fabs(cpu[i]));
    }
    const double rms = std::sqrt(sumSq / (double)cpu.size());
    printf("\n最大绝对偏差: %.3e   （信号 RMS %.3e）\n", maxAbs, rms);
    printf("最大相对偏差: %.3e\n", maxRel);
    printf("加速比: %.1fx\n", cpuMs / (gpuMs > 1e-9 ? gpuMs : 1e-9));

    const bool ok = (maxAbs / (rms > 1e-30 ? rms : 1.0) < 1e-12) && rc == (int)nOut;
    printf("\n[%s] GPU 结果与 CPU 参考一致\n", ok ? "PASS" : "FAIL");
    FreeLibrary(h);
    return ok ? 0 : 1;
}
