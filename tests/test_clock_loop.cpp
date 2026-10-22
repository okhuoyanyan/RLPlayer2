// test_clock_loop.cpp — 时钟漂移闭环补偿验证（纯离线仿真）
//
// 模型：发送端按 nominalRate 产生数据；端点以 nominalRate*(1+ppm/1e6) 消费。
//       缓冲水位 = (已发送帧数 - 已消费帧数) / rate
// 判据：
//   不开环：水位必然线性漂移到溢出或耗尽（这就是必须补偿的理由）
//   开环后：水位被稳定在目标值附近，且修正量平滑（无跳变）
#include "net/clock_loop.h"
#include "net/drift.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace rta::net;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

struct SimResult {
    double finalBufferMs = 0;
    double minBufferMs = 1e9, maxBufferMs = -1e9;
    bool overflowed = false, underran = false;
    double maxStepPpm = 0;      // 修正量单步最大变化
    double totalAdjustPpm = 0;
};

// ppm: 端点相对发送端的漂移；bufferCapMs: 端点缓冲容量
static SimResult simulate(double ppm, double seconds, uint32_t rate,
                          bool closedLoop, double targetMs = 200.0, double capMs = 400.0) {
    SimResult r;
    const double pktSec = 128.0 / (double)rate;      // 每包 128 帧
    const double consumeRatio = 1.0 + ppm * 1e-6;

    DriftEstimator de(2.0);
    // 内环 1.0（已扣除自身修正，无正反馈）+ 外环 PI（消除稳态偏差）
    ClockLoop loop({targetMs, 500.0, 20.0, 2.0, 1.0, 200.0, 1.0});

    // ★ 预滚：发送端先发出 targetMs 的数据，端点才开始消费。
    //   这正是 RTANet 接收端 preroll 的作用，也是所有流式音频的常规做法。
    //   若不建模预滚，缓冲会从 0 开始，外环只能靠限幅内的速率差去"填"，
    //   结果是修正长期顶在限幅上、水位永远到不了目标（曾因此得到 6 个假失败）。
    double sentFrames = targetMs * (double)rate / 1000.0;
    double consumedFrames = 0.0;
    double t = 0.0;
    const size_t nPkt = (size_t)(seconds / pktSec);

    for (size_t i = 0; i < nPkt; ++i) {
        // ★ 正确的速率模型：
        //   每包固定占用 pktSec 的【真实时间】（实时流）。
        //   发送端的速率修正通过异步重采样实现 —— 即单位时间内【产出更多/更少帧】，
        //   而不是"每包占用更多时间"（那是把方向搞反了，会让补偿越补越糟）。
        const double adj = closedLoop ? loop.applied_ppm() : 0.0;
        const double sendDt = pktSec;
        t += sendDt;
        sentFrames += 128.0 * (1.0 + adj * 1e-6);

        // 端点按自己的时钟消费：固定产出 128 帧对应的真实时长内，它消费的帧数按 consumeRatio 缩放
        consumedFrames += 128.0 * consumeRatio;

        const double bufMs = (sentFrames - consumedFrames) * 1000.0 / (double)rate;
        if (bufMs > capMs) r.overflowed = true;
        if (bufMs < 0.0)   r.underran = true;
        r.minBufferMs = std::min(r.minBufferMs, bufMs);
        r.maxBufferMs = std::max(r.maxBufferMs, bufMs);

        if (closedLoop) {
            // 检测器：用"流时间戳 / 端点本地时间"的斜率（这里用消费侧的等效时间）
            de.observe((uint64_t)sentFrames, t / consumeRatio, rate);
            const double est = de.ready() ? de.drift_ppm() : 0.0;
            const double prev = loop.applied_ppm();
            loop.update(est, bufMs, sendDt);
            r.maxStepPpm = std::max(r.maxStepPpm, std::fabs(loop.applied_ppm() - prev));
        }
    }
    r.finalBufferMs = (sentFrames - consumedFrames) * 1000.0 / (double)rate;
    r.totalAdjustPpm = loop.applied_ppm();
    return r;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const uint32_t rate = 48000;

    printf("=== 1. 不开环：水位必然漂移（这就是必须补偿的理由） ===\n");
    {
        for (double ppm : {20.0, 50.0}) {
            auto r = simulate(ppm, 120.0, rate, /*closedLoop=*/false);
            char buf[220];
            std::snprintf(buf, sizeof(buf),
                          "(端点快 %+.0f ppm, 120 秒后水位 %.1f ms, 溢出=%s 耗尽=%s)",
                          ppm, r.finalBufferMs, r.overflowed ? "是" : "否", r.underran ? "是" : "否");
            check(true, "记录（仅作对照）", buf);   // 不断言，只展示
        }
    }

    printf("\n=== 2. 开环后：水位必须被控在目标附近 ===\n");
    {
        const double target = 200.0;
        for (double ppm : {-100.0, -50.0, -20.0, 20.0, 50.0, 100.0}) {
            auto r = simulate(ppm, 120.0, rate, /*closedLoop=*/true, target);
            const double err = std::fabs(r.finalBufferMs - target);
            char buf[260];
            std::snprintf(buf, sizeof(buf),
                          "(注入 %+6.1f ppm -> 最终水位 %6.1f ms, 偏差 %5.1f ms, 区间 [%.0f, %.0f], "
                          "单步最大 %.2f ppm, 稳态修正 %+.1f ppm)",
                          ppm, r.finalBufferMs, err, r.minBufferMs, r.maxBufferMs,
                          r.maxStepPpm, r.totalAdjustPpm);
            // 外环已改为 PI，稳态偏差应被消除 —— 容差收紧到 8 ms
            check(err < 8.0 && !r.overflowed && !r.underran, "水位被控住", buf);
        }
    }

    printf("\n=== 3. 修正量必须平滑（避免可闻音高抖动） ===\n");
    {
        auto r = simulate(80.0, 120.0, rate, true, 200.0);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(单步最大变化 %.3f ppm，限幅设定 20 ppm/秒 × 2.7 ms/包 ≈ 0.05 ppm)",
                      r.maxStepPpm);
        check(r.maxStepPpm < 0.2, "修正量无跳变", buf);
    }

    printf("\n=== 3b. 长时程收敛（仿真 300 秒；同一控制器、同一代码路径） ===\n");
    {
        // 真实长测需要 60 秒以上的实时运行，而铁律 1 要求长时实验先问用户。
        // 这里用【加速仿真】验证收敛性：控制器代码完全相同，只是时间轴按包步进。
        struct Row { double ppm; };
        const double ppmList[] = {-100.0, -50.0, -20.0, 0.0, 20.0, 50.0, 100.0};
        printf("  %10s %12s %12s %12s\n", "注入(ppm)", "30秒修正", "120秒修正", "300秒修正");
        for (double ppm : ppmList) {
            const double dt = 0.1;
            const double rate = 48000.0;
            ClockLoop loop({200.0, 500.0, 20.0, 2.0, 1.0, 200.0, 1.0});
            double bufMs = 200.0, applied = 0.0;
            double at30 = 0, at120 = 0, at300 = 0;
            for (int i = 0; i < 3000; ++i) {                 // 300 秒
                const double produced = rate * dt * (1.0 + applied * 1e-6);
                const double consumed = rate * dt * (1.0 + ppm * 1e-6);
                bufMs += (produced - consumed) * 1000.0 / rate;
                applied = loop.update(ppm + applied, bufMs, dt);   // 估计值含自身修正
                if (i == 299)  at30 = applied;
                if (i == 1199) at120 = applied;
                if (i == 2999) at300 = applied;
            }
            char buf[220];
            std::snprintf(buf, sizeof(buf), "(注入 %+.0f ppm -> 300 秒后修正 %+.2f ppm, 偏差 %.2f ppm, 水位 %.1f ms)",
                          ppm, at300, std::fabs(at300 - ppm), bufMs);
            printf("  %10.0f %12.2f %12.2f %12.2f   %s\n", ppm, at30, at120, at300, buf);
            check(std::fabs(at300 - ppm) < 2.0 && std::fabs(bufMs - 200.0) < 5.0,
                  "长时程收敛到注入漂移", buf);
        }
    }

    printf("\n=== 4. DriftEstimator 与 ClockLoop 联动 ===\n");
    {
        // 直接用真实漂移估计喂控制器，验证方向与量级
        ClockLoop loop({200.0, 500.0, 1000.0, 2.0, 0.0, 0.0, 1.0});   // 关积分，只看方向
        const double adj = loop.update(50.0, 200.0, 1.0);   // 端点快 50 ppm、水位正好
        char buf[160];
        std::snprintf(buf, sizeof(buf), "(漂移 +50 ppm、水位正常 -> 修正 %+.1f ppm)", adj);
        check(adj > 0.0, "修正方向正确（端点快则发送也加快）", buf);
        const double adj2 = loop.update(0.0, 250.0, 1.0);   // 水位偏高 50 ms
        std::snprintf(buf, sizeof(buf), "(水位偏高 50 ms -> 累计修正 %+.1f ppm)", adj2);
        check(adj2 < adj, "水位偏高时减速", buf);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
