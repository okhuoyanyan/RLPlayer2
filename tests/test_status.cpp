// test_status.cpp — 端点状态行编解码 + 与闭环控制器的联动验证（纯离线）
#include "net/rtanet_status.h"
#include "net/clock_loop.h"
#include <cstdio>
#include <cmath>
#include <string>

using namespace rta::net;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== 1. 状态行编解码往返 ===\n");
    {
        const uint64_t ts = 123456;
        const std::string line = format_status(ts, 198.4, 12.7, true);
        printf("  生成: %s\n", line.c_str());
        auto s = parse_status(line);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(ts=%llu buf=%.1f drift=%+.1f)",
                      (unsigned long long)s.streamTs, s.bufferMs, s.driftPpm);
        check(s.valid && s.streamTs == ts && std::fabs(s.bufferMs - 198.4) < 1e-9 &&
              std::fabs(s.driftPpm - 12.7) < 1e-9 && s.hasDrift, "正漂移往返", buf);

        auto s2 = parse_status(format_status(999, 250.0, -47.3, true));
        std::snprintf(buf, sizeof(buf), "(ts=%llu drift=%+.1f)", (unsigned long long)s2.streamTs, s2.driftPpm);
        check(s2.streamTs == 999 && std::fabs(s2.driftPpm + 47.3) < 1e-9, "负漂移往返", buf);

        auto s3 = parse_status(format_status(7, 100.0, 0.0, false));
        check(s3.valid && !s3.hasDrift && std::fabs(s3.bufferMs - 100.0) < 1e-9, "无漂移字段时正常解析");

        auto bad = parse_status("OK START port=1234");
        check(!bad.valid, "非 STATUS 行被拒绝");
    }

    printf("\n=== 2. 真实回传序列驱动闭环 ===\n");
    {
        // 模拟：端点快 75 ppm（缓冲在掉），每 100 ms 回传一次状态
        ClockLoop loop({200.0, 500.0, 20.0, 2.0, 1.0, 200.0, 1.0});
        const double rate = 48000.0;
        const double ppm = 75.0;
        double bufMs = 200.0;
        double applied = 0.0;
        uint64_t ts = 0;
        const double dt = 0.1;                       // 100 ms 一次回传
        for (int i = 0; i < 600; ++i) {              // 60 秒
            // 端点按自己的时钟消费；发送端按修正后的速率产出
            const double produced = rate * dt * (1.0 + applied * 1e-6);
            const double consumed = rate * dt * (1.0 + ppm * 1e-6);
            bufMs += (produced - consumed) * 1000.0 / rate;
            ts += (uint64_t)produced;

            // 端点回传；注意它测到的是【相对】漂移（含发送端已施加的修正）
            const std::string line = format_status(ts, bufMs, ppm + applied, true);
            auto st = parse_status(line);
            applied = loop.update(st.driftPpm, st.bufferMs, dt);
        }
        char buf[220];
        std::snprintf(buf, sizeof(buf), "(60 秒后水位 %.2f ms, 目标 200, 稳态修正 %+.1f ppm, 积分 %+.1f ppm)",
                      bufMs, applied, loop.integral_ppm());
        check(std::fabs(bufMs - 200.0) < 8.0, "经真实状态行驱动后水位收敛", buf);
        check(applied > 60.0 && applied < 90.0, "稳态修正量接近注入漂移 75 ppm");
    }

    printf("\n=== 3. 端点不提供漂移时的退化行为 ===\n");
    {
        ClockLoop loop({200.0, 500.0, 20.0, 2.0, 1.0, 200.0, 1.0});
        double applied = 0.0;
        double bufMs = 250.0;                        // 水位偏高 50 ms
        for (int i = 0; i < 200; ++i) {
            const std::string line = format_status(0, bufMs, 0.0, false);
            auto st = parse_status(line);
            applied = loop.update(st.hasDrift ? st.driftPpm : 0.0, st.bufferMs, 0.1);
            bufMs -= applied * 1e-6 * 48000.0 * 0.1 * 1000.0 / 48000.0;
        }
        char buf[180];
        std::snprintf(buf, sizeof(buf), "(仅水位反馈: 修正 %+.1f ppm, 末水位 %.1f ms)", applied, bufMs);
        check(applied < 0.0, "无漂移信息时仍能凭水位纠偏", buf);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
