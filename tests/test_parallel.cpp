// test_parallel.cpp — 多核 DSP 的正确性与收益
//
// 判据：
//   1) 并行结果必须与串行【逐位一致】（每声道状态独立，没有共享可变量）
//   2) 多核必须真的更快（否则就是白白增加复杂度）
#include "dsp/parallel.h"
#include "dsp/modulator.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

using namespace rta::dsp;
static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static const double PI = 3.14159265358979323846;

int main() {
    printf("=== 多核 DSP 验证 ===\n");
    printf("  可用硬件线程: %zu\n", hardware_threads());

    // 工作量要足够大，否则线程创建开销会把收益吃掉
    // （48k 样本只要 1.8 ms，量级不对；这里用 200 万样本 ≈ 0.18 秒 DSD256 音频）
    const size_t CH = 2, N = 2000000;
    const double fs = 11289600.0;            // DSD256，最重的调制场景

    auto make_input = [&](size_t ch) {
        std::vector<double> x(N);
        for (size_t n = 0; n < N; ++n)
            x[n] = 0.4 * std::sin(2.0 * PI * (997.0 + 100.0 * (double)ch) * (double)n / (double)fs);
        return x;
    };

    // 串行 / 并行各跑 3 轮取**最小值**再比加速比。
    // 为什么：判据原本是单次计时，verify.ps1 在构建/杀毒/其它测试并发时会偶发 < 1.3x
    //（实测算过一次 96% 通过，唯一失败项就是这里），属于测量噪声而不是并行失效。
    // 取最小值能滤掉调度抖动，同时仍然要求「多核确实更快」这条判据成立。
    std::vector<std::vector<double>> serial(CH), par(CH);
    double ms_s = 1e30, ms_p = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t c = 0; c < CH; ++c) {
            SdmConfig cfg; cfg.fs = fs; cfg.order = 5; cfg.levels = 2; cfg.cutoffHz = 100000.0;
            SdmModulator m(cfg);
            auto x = make_input(c);
            serial[c].resize(N);
            for (size_t n = 0; n < N; ++n) serial[c][n] = m.process(x[n] * 0.9);
        }
        const auto t1 = std::chrono::steady_clock::now();
        ms_s = std::min(ms_s, std::chrono::duration<double, std::milli>(t1 - t0).count());

        const auto t2 = std::chrono::steady_clock::now();
        ParallelFor::instance().run(CH, [&](size_t c) {
            SdmConfig cfg; cfg.fs = fs; cfg.order = 5; cfg.levels = 2; cfg.cutoffHz = 100000.0;
            SdmModulator m(cfg);
            auto x = make_input(c);
            par[c].resize(N);
            for (size_t n = 0; n < N; ++n) par[c][n] = m.process(x[n] * 0.9);
        });
        const auto t3 = std::chrono::steady_clock::now();
        ms_p = std::min(ms_p, std::chrono::duration<double, std::milli>(t3 - t2).count());
    }

    size_t diff = 0;
    for (size_t c = 0; c < CH; ++c)
        for (size_t n = 0; n < N; ++n) if (serial[c][n] != par[c][n]) ++diff;

    printf("  串行(最好) %.1f ms   并行(最好) %.1f ms   加速 %.2fx\n", ms_s, ms_p, ms_s / std::max(1e-9, ms_p));
    check(diff == 0, "并行结果与串行逐位一致", std::to_string(diff) + " 个不同点");
    check(ms_s / std::max(1e-9, ms_p) > 1.3, "多核确实更快（3 轮取最好）",
          std::to_string(ms_s / std::max(1e-9, ms_p)) + "x");

    // 边界：n=0 / n=1 不应该崩
    int hit = 0;
    ParallelFor::instance().run(0, [&](size_t) { ++hit; });
    ParallelFor::instance().run(1, [&](size_t i) { if (i == 0) ++hit; });
    check(hit == 1, "n=0 不执行、n=1 执行一次", std::to_string(hit));

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
