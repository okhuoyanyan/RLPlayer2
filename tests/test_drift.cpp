// test_drift.cpp — 时钟漂移检测验证（纯离线数值实验）
// 判据：注入已知 ppm 漂移，估计值必须准确复现；无漂移时必须报 0
#include "net/drift.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <random>

using namespace rta::net;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// 模拟：发送端按流速率产生包，端点以 (1 + ppm/1e6) 倍速消费
//   每包 128 帧，包间隔 = 128/rate 秒（发送侧）
//   接收侧观测到的 (localSec, streamTs) 斜率 = 1 / (1 + ppm/1e6)
static void run_case(double injectedPpm, double seconds, uint32_t rate, uint64_t framesPerPkt,
                     double jitterUs, uint64_t seed) {
    DriftEstimator de(10.0);
    std::mt19937 rng((uint32_t)seed);
    std::uniform_real_distribution<double> jit(-jitterUs, jitterUs);

    const double pktInterval = (double)framesPerPkt / (double)rate;   // 发送侧标称间隔
    const double consumeRatio = 1.0 + injectedPpm * 1e-6;             // 端点消费倍率

    uint64_t ts = 0;
    double local = 0.0;
    const size_t nPkt = (size_t)(seconds / pktInterval);
    for (size_t i = 0; i < nPkt; ++i) {
        // 发送端固定节奏
        local = (double)i * pktInterval;
        ts = (uint64_t)i * framesPerPkt;
        // 端点观测到的是"本地时间"，而本地时间由【端点自己的时钟】计量：
        // 端点时钟走得快 ⟹ 同样的流内容在更短的本机时间里到达
        const double localAtEndpoint = local / consumeRatio + jit(rng) * 1e-6;
        de.observe(ts, localAtEndpoint, rate);
    }
    const double est = de.drift_ppm();
    const double err = std::fabs(est - injectedPpm);
    char buf[240];
    std::snprintf(buf, sizeof(buf),
                  "(注入 %+8.2f ppm -> 估计 %+8.2f ppm, 误差 %.3f ppm, 用了 %zu 个样本)",
                  injectedPpm, est, err, de.samples());
    // 抖动会让估计有噪声，容差取 1.5 ppm
    check(err < 1.5, "漂移估计准确", buf);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const uint32_t rate = 48000;
    const uint64_t FPP = 128;

    printf("=== 1. 无漂移必须报 0 ===\n");
    run_case(0.0, 10.0, rate, FPP, 0.0, 1);

    printf("\n=== 2. 各种已知漂移（无抖动）===\n");
    for (double ppm : {-100.0, -50.0, -10.0, 10.0, 50.0, 100.0, 200.0})
        run_case(ppm, 10.0, rate, FPP, 0.0, 2);

    printf("\n=== 3. 加入网络抖动（±500 微秒）后仍必须准确 ===\n");
    for (double ppm : {-50.0, 0.0, 50.0})
        run_case(ppm, 20.0, rate, FPP, 500.0, 3);

    printf("\n=== 4. 窗口长度的影响：短窗口更快但更抖 ===\n");
    {
        // 手工构造一个窗口只有 2 秒的估计器
        DriftEstimator fast(2.0), slow(20.0);
        const double ppm = 75.0;
        const double pktInterval = (double)FPP / (double)rate;
        uint64_t ts = 0;
        for (size_t i = 0; i < (size_t)(30.0 / pktInterval); ++i) {
            const double local = (double)i * pktInterval;
            const double le = local / (1.0 + ppm * 1e-6);
            ts = (uint64_t)i * FPP;
            fast.observe(ts, le, rate);
            slow.observe(ts, le, rate);
        }
        char buf[220];
        std::snprintf(buf, sizeof(buf), "(2 秒窗口 %.2f ppm / %zu 点, 20 秒窗口 %.2f ppm / %zu 点)",
                      fast.drift_ppm(), fast.samples(), slow.drift_ppm(), slow.samples());
        check(std::fabs(fast.drift_ppm() - ppm) < 1.5 && std::fabs(slow.drift_ppm() - ppm) < 1.5,
              "不同窗口长度都准确", buf);
        check(fast.samples() < slow.samples(), "长窗口保留更多样本");
    }

    printf("\n=== 5. 速率补偿建议的方向必须正确 ===\n");
    {
        DriftEstimator de(10.0);
        const double pktInterval = (double)FPP / (double)rate;
        // 端点消费更慢（ppm>0）⟹ 缓冲在涨 ⟹ 发送应减慢 ⟹ 建议值为负
        for (size_t i = 0; i < (size_t)(10.0 / pktInterval); ++i) {
            const double local = (double)i * pktInterval;
            de.observe((uint64_t)i * FPP, local / (1.0 + 80.0 * 1e-6), rate);
        }
        const double sug = de.suggested_rate_adjust_ppm();
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(端点消费慢 80 ppm -> 建议速率补偿 %+.2f ppm，应为负)", sug);
        check(sug < -1.0, "补偿方向正确", buf);
    }

    printf("\n=== 6. 缓冲水位换算 ===\n");
    {
        DriftEstimator de;
        de.observe_played(48000, 43200, 48000);   // 已发 1.0 秒，已播 0.9 秒
        char buf[160];
        std::snprintf(buf, sizeof(buf), "(水位 %.1f ms, 期望 100.0 ms)", de.buffer_ms());
        check(std::fabs(de.buffer_ms() - 100.0) < 1e-6, "缓冲水位换算正确", buf);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
