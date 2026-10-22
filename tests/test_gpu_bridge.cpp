// test_gpu_bridge.cpp — GPU 卷积桥接的回归测试
//
// 为什么需要它：gpu_bridge.h 的 Windows 分支曾经只 LoadLibrary、不 GetProcAddress，
// 于是 fnAvail_/fnConv_ 恒为空，status_ 永远写"缺少导出符号"，GPU 卷积从来没跑过，
// 而界面上只看到一句"退回 CPU"，谁也不会注意。这个测试把这条路径钉住。
//
// 判据说明（铁律 3）：
//   * GPU 路径内部用单精度 FFT，与 CPU 双精度直接卷积【不可能逐位相同】，
//     所以判据是"相对误差 < 1e-4"，不是逐位比较。
//   * 没有 DLL / 没有可用 GPU 时不算失败，但必须明确打印 SKIP 与原因，
//     不允许把"没测到"写成"通过"。
#include "dsp/gpu_bridge.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <string>

using namespace rta::dsp;

int main() {
    auto& gb = GpuBridge::instance();
    printf("GPU 后端：%s\n", gb.status().c_str());

    // CPU 参考：直接卷积（双精度）
    const int n = 8192, irLen = 4096;
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> d(-0.5, 0.5);
    std::vector<double> in(n), ir(irLen), ref(n, 0.0);
    for (auto& v : in) v = d(rng);
    for (auto& v : ir) v = d(rng);
    for (int i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int k = 0; k < irLen && k <= i; ++k) acc += in[i - k] * ir[k];
        ref[i] = acc;
    }

    if (!gb.available()) {
        printf("[SKIP] GPU 不可用（%s）——本机未验证 GPU 卷积数值正确性\n", gb.status().c_str());
        return 0;
    }

    std::vector<double> out(n, 0.0);
    const int rc = gb.convolve(in.data(), (size_t)n, ir.data(), (size_t)irLen, out.data(), 1024);
    if (rc != 0) { printf("[FAIL] gpu_convolve 返回 %d\n", rc); return 1; }

    // 相对误差：按参考信号的 RMS 归一
    double num = 0.0, den = 0.0;
    for (int i = 0; i < n; ++i) {
        const double e = out[i] - ref[i];
        num += e * e;
        den += ref[i] * ref[i];
    }
    const double rel = std::sqrt(num / (den > 0 ? den : 1.0));
    const double maxAbs = [&] { double m = 0; for (int i = 0; i < n; ++i) m = std::max(m, std::fabs(out[i] - ref[i])); return m; }();

    printf("  样本 %d，IR %d 抽头，分块 1024\n", n, irLen);
    printf("  相对 RMS 误差 %.3e，最大绝对误差 %.3e\n", rel, maxAbs);
    printf("  前 4 个样本：GPU %.9f / CPU %.9f\n", out[0], ref[0]);

    if (!(rel < 1e-4)) { printf("[FAIL] 相对误差超过 1e-4（单精度 FFT 的正常量级约 1e-7）\n"); return 1; }
    printf("[PASS] GPU 卷积与 CPU 直接卷积一致（相对误差 < 1e-4）\n");
    return 0;
}
