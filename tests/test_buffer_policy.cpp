// test_buffer_policy.cpp — C1 独占缓冲策略回归（2026-10-24 N300 欠载）
//
// 被测对象是生产代码 src/audio/buffer_policy.h —— rtaweb 的 WasapiSink::open 用的就是它，
// 不是测试里另抄一遍（铁律 3：测真东西）。FakeBufferOpener 只替换「真正打开设备」这一步，
// 因此本测试【绝不打开任何音频设备】（铁律 1），也绝不会向听音设备发声。
//
// 覆盖（方案 §8）：
//   · 缓冲请求透传（请求 100ms ⟹ 实际 100ms；旧代码会封顶 20ms）
//   · 下限 20ms / 上限 500ms
//   · L1 放大回退（请求 100ms、驱动给 186ms ⟹ 回退 20ms 并记下原因）
//   · 学习缓存：第一次降级后落盘、二次播放直接命中、设置变更即失效、reset 清空
//   · RTA_BUF_RESPECT=0 ⟹ 旧 20ms 行为（可回滚）
//   · L2 起播看门狗判据（0.7s 不降级 / 0.8s 降级 / 有写入不降级 / 没数据不降级）
//   · 参数矩阵 20/50/100/200ms × 44.1/48/96kHz，以及每种组合的 L1 降级
//
// ⚠️ 未验证（铁律 4）：L1/L2 的真实回退行为需要 MOTU 这类「Initialize 接受但流不推进」的
//    真机才能验证；本测试【仅 mock 验证】，未经真机复测。
#include "audio/buffer_policy.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// 假打开器：按请求毫秒数算出「驱动实际给的帧数」（ratio 模拟驱动对齐放大）。
struct FakeOpener {
    uint32_t rate  = 96000;
    // 实际缓冲 = 请求 × ratio。ratio 只作用于「大缓冲请求」（>25ms）：
    // 真实 MOTU 是「请求 100ms 被对齐成 186ms，但 20ms 请求可用」，把 20ms 也放大
    // 不符合实测形态（方案 §5.2）。ratio 在 20ms 上固定为 1.0。
    double   ratio = 1.0;
    double   failMs = -1.0;    // 该毫秒数打开必失败（-1 = 不失败）
    int      calls = 0;
    bool     isOpen = false;
    size_t   frames = 0;
    std::vector<double> attempts;

    rta::audio::BufferOpener fn() {
        rta::audio::BufferOpener op;
        op.open = [this](double ms, std::string& why) {
            ++calls;
            attempts.push_back(ms);
            if (failMs > 0.0 && std::fabs(ms - failMs) < 0.01) {
                why = "fake: 驱动拒绝该缓冲";
                isOpen = false;
                return false;
            }
            isOpen = true;
            const double r = (ms > 25.0) ? ratio : 1.0;
            frames = (size_t)std::llround((double)rate * ms / 1000.0 * r);
            return true;
        };
        op.frames = [this]() { return frames; };
        op.close  = [this]() { isOpen = false; frames = 0; };
        return op;
    }
    double actual_ms() const { return rate ? (double)frames * 1000.0 / (double)rate : 0.0; }
};

static std::string attempts_str(const FakeOpener& f) {
    std::string s;
    for (double v : f.attempts) {
        if (!s.empty()) s += ",";
        char b[32];
        std::snprintf(b, sizeof(b), "%.0f", v);
        s += b;
    }
    return s;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    using rta::audio::BufferLearnStore;
    using rta::audio::BufferPolicy;
    using rta::audio::BufferRequest;
    using rta::audio::buffer_apply;

    std::printf("== buffer_policy：独占缓冲策略（mock，不打开设备）==\n");

    // ---- 1. clamp：下限 20 / 上限 500 ----
    {
        std::printf("[clamp]\n");
        ck(rta::audio::buffer_clamp_ms(10.0) == 20.0, "请求 10ms ⟹ 20ms（下限保护）");
        ck(rta::audio::buffer_clamp_ms(20.0) == 20.0, "请求 20ms ⟹ 20ms");
        ck(rta::audio::buffer_clamp_ms(100.0) == 100.0, "请求 100ms ⟹ 100ms（不再封顶）");
        ck(rta::audio::buffer_clamp_ms(900.0) == 500.0, "请求 900ms ⟹ 500ms（上限）");
        ck(rta::audio::buffer_clamp_ms(0.0) == 20.0, "请求 0ms ⟹ 20ms");
        ck(rta::audio::buffer_clamp_ms(-5.0) == 20.0, "请求负数 ⟹ 20ms");
        ck(rta::audio::buffer_clamp_ms(std::nan("")) == 20.0, "请求 NaN ⟹ 20ms（不把 NaN 传给驱动）");
    }

    // ---- 2. 旧行为对照（逐字复刻被删掉的那一行，证明要修的是什么）----
    {
        std::printf("[旧行为对照]\n");
        auto old_cap = [](double bufferMs) {
            return (bufferMs > 0 && bufferMs < 20) ? bufferMs : 20.0;
        };
        ck(old_cap(100.0) == 20.0, "旧代码：请求 100ms 只会开 20ms（N300 欠载的直接原因）");
        ck(old_cap(10.0) == 10.0, "旧代码：请求 10ms 反而开 10ms（连 20ms 下限都没有）");
        ck(rta::audio::buffer_clamp_ms(100.0) == 100.0 && old_cap(100.0) == 20.0,
           "新策略与旧代码在 100ms 上给出相反结果（回归对照）");
    }

    // ---- 3. 缓冲请求透传 ----
    {
        std::printf("[请求透传]\n");
        FakeOpener f; f.rate = 96000;
        BufferLearnStore none;             // 空路径 = 不落盘
        bool ok = false;
        BufferRequest req; req.requestedMs = 100.0; req.rate = 96000; req.respect = true; req.useLearn = false;
        const BufferPolicy pol = buffer_apply(req, f.fn(), &none, "k", &ok);
        ck(ok, "按请求 100ms 打开成功");
        ck(pol.effectiveMs == 100.0 && !pol.degraded && pol.policy == "requested",
           "请求 100ms ⟹ 实际 100ms，未降级", pol.policy);
        ck(f.attempts.size() == 1 && f.attempts[0] == 100.0,
           "只按 100ms 尝试一次", attempts_str(f));
        ck(f.frames == 9600, "96k × 100ms = 9600 帧（= 10 个 960 帧周期）",
           std::to_string(f.frames));

        FakeOpener f2; f2.rate = 96000;
        bool ok2 = false;
        req.requestedMs = 10.0;
        const BufferPolicy pol2 = buffer_apply(req, f2.fn(), &none, "k", &ok2);
        ck(ok2 && pol2.effectiveMs == 20.0 && f2.frames == 1920,
           "请求 10ms ⟹ 实际 20ms（1920 帧）", attempts_str(f2));

        FakeOpener f3; f3.rate = 96000;
        bool ok3 = false;
        req.requestedMs = 900.0;
        const BufferPolicy pol3 = buffer_apply(req, f3.fn(), &none, "k", &ok3);
        ck(ok3 && pol3.effectiveMs == 500.0 && f3.frames == 48000,
           "请求 900ms ⟹ 实际 500ms（上限）", attempts_str(f3));

        // ---- PLAYBUF 行样例（生产 rtaweb 的 play_one_impl 调同一个 formatter）----
        const std::string line = "PLAYBUF " + rta::audio::buffer_playbuf_json(
            pol, f.frames, 960, "buffer=100.0ms(9600帧),period=10.0ms,event", false);
        ck(contains(line, "\"bufferReqMs\":100") && contains(line, "\"bufferMs\":100.0") &&
           contains(line, "\"bf\":9600") && contains(line, "\"per\":960") &&
           contains(line, "\"bufPolicy\":\"requested\"") &&
           contains(line, "\"initRecipe\":\"buffer=100.0ms(9600帧),period=10.0ms,event\"") &&
           contains(line, "\"realigned\":false"),
           "PLAYBUF 行字段齐全（方案 §5.4）", line);
        std::printf("  样例1: %s\n", line.c_str());
    }

    // ---- 4. L1 放大预检 ----
    {
        std::printf("[L1 放大回退]\n");
        FakeOpener f; f.rate = 96000; f.ratio = 1.86;   // 请求 100ms → 186ms（MOTU 形态）
        BufferLearnStore none;
        bool ok = false;
        BufferRequest req; req.requestedMs = 100.0; req.rate = 96000; req.useLearn = false;
        const BufferPolicy pol = buffer_apply(req, f.fn(), &none, "k", &ok);
        ck(ok, "降级后仍成功打开");
        ck(pol.degraded && pol.effectiveMs == 20.0 && pol.policy == "degraded",
           "实际 186ms > 请求 1.5 倍 ⟹ 降级 20ms", pol.policy);
        ck(f.attempts.size() == 2 && f.attempts[0] == 100.0 && f.attempts[1] == 20.0,
           "先试 100ms、再重开 20ms", attempts_str(f));
        ck(pol.why.find("放大") != std::string::npos && pol.why.find("186") != std::string::npos,
           "bufWhy 写明实际毫秒数", pol.why);
        ck(f.frames == 1920, "降级后实际缓冲 = 1920 帧", std::to_string(f.frames));
        // 降级形态的 PLAYBUF 样例（生产同一个 formatter）
        const std::string dline = "PLAYBUF " + rta::audio::buffer_playbuf_json(
            pol, f.frames, 960, "buffer=20.0ms(1920帧),period=10.0ms,event", false);
        ck(contains(dline, "\"bufPolicy\":\"degraded\"") && contains(dline, "\"bufferMs\":20.0") &&
           contains(dline, "\"bf\":1920"),
           "降级形态 PLAYBUF 记录 bufPolicy=degraded", dline);
        std::printf("  样例2: %s\n", dline.c_str());

        // 阈值边界：1.4 倍不降级（避免把正常的驱动取整当成故障）
        FakeOpener f2; f2.rate = 96000; f2.ratio = 1.4;
        bool ok2 = false;
        req.requestedMs = 100.0;
        const BufferPolicy pol2 = buffer_apply(req, f2.fn(), &none, "k2", &ok2);
        ck(ok2 && !pol2.degraded && pol2.effectiveMs == 100.0,
           "实际 140ms < 150ms（1.5 倍）⟹ 不降级");

        // 请求本来就是 20ms：不重复重开
        FakeOpener f3; f3.rate = 96000; f3.ratio = 3.0;
        bool ok3 = false;
        req.requestedMs = 20.0;
        const BufferPolicy pol3 = buffer_apply(req, f3.fn(), &none, "k3", &ok3);
        ck(ok3 && !pol3.degraded && f3.attempts.size() == 1,
           "请求已是 20ms ⟹ 不做无意义的重开", attempts_str(f3));
    }

    // ---- 5. 请求值打不开 ⟹ 降级 20ms ----
    {
        std::printf("[打开失败回退]\n");
        FakeOpener f; f.rate = 96000; f.failMs = 100.0;
        BufferLearnStore none;
        bool ok = false;
        BufferRequest req; req.requestedMs = 100.0; req.rate = 96000; req.useLearn = false;
        const BufferPolicy pol = buffer_apply(req, f.fn(), &none, "k", &ok);
        ck(ok && pol.degraded && pol.effectiveMs == 20.0, "100ms 打不开 ⟹ 20ms 打开成功");
        ck(f.attempts.size() == 2 && f.attempts[1] == 20.0, "尝试序列 100,20", attempts_str(f));
        ck(pol.why.find("打开失败") != std::string::npos, "原因写明打开失败", pol.why);
    }

    // ---- 6. 学习缓存 ----
    {
        std::printf("[学习缓存]\n");
        const std::string path = "test-buffer-learned.tmp.json";
        std::remove(path.c_str());
        const std::string key = rta::audio::buffer_key("dev-A", 96000, 24, 100.0);
        ck(key != rta::audio::buffer_key("dev-A", 96000, 24, 200.0),
           "键里包含请求毫秒数（改设置 ⟹ 自然失效）", key);
        {
            FakeOpener f; f.rate = 96000; f.ratio = 1.86;
            BufferLearnStore store(path);
            bool ok = false;
            BufferRequest req; req.requestedMs = 100.0; req.rate = 96000;
            const BufferPolicy pol = buffer_apply(req, f.fn(), &store, key, &ok);
            ck(ok && pol.degraded, "第一次：驱动放大 ⟹ 降级");
            ck(store.size() == 1, "降级结果写入学习缓存", std::to_string(store.size()));
            rta::audio::BufferLearnEntry e;
            ck(store.get(key, e) && std::fabs(e.ms - 20.0) < 1e-9 && !e.why.empty(),
               "缓存内容 ms=20 且带原因", e.why);
        }
        {
            FakeOpener f; f.rate = 96000; f.ratio = 1.0;
            BufferLearnStore store(path);
            bool ok = false;
            BufferRequest req; req.requestedMs = 100.0; req.rate = 96000;
            const BufferPolicy pol = buffer_apply(req, f.fn(), &store, key, &ok);
            ck(ok && pol.learned && pol.policy == "learned" && pol.effectiveMs == 20.0,
               "第二次：命中缓存，直接用学习值 20ms", pol.policy);
            ck(f.attempts.size() == 1 && f.attempts[0] == 20.0,
               "命中缓存时不再先按 100ms 失败一次", attempts_str(f));
        }
        {
            FakeOpener f; f.rate = 96000; f.ratio = 1.0;
            BufferLearnStore store(path);
            bool ok = false;
            BufferRequest req; req.requestedMs = 200.0; req.rate = 96000;
            const std::string key200 = rta::audio::buffer_key("dev-A", 96000, 24, 200.0);
            const BufferPolicy pol = buffer_apply(req, f.fn(), &store, key200, &ok);
            ck(ok && !pol.learned && f.attempts.size() == 1 && f.attempts[0] == 200.0,
               "用户改成 200ms ⟹ 旧学习值失效，按 200ms 重试", attempts_str(f));
        }
        {
            BufferLearnStore store(path);
            ck(store.reset() && store.size() == 0, "reset 清空学习缓存（/api/buffer/reset）");
            bool ok = false;
            BufferRequest req; req.requestedMs = 100.0; req.rate = 96000;
            FakeOpener f; f.rate = 96000; f.ratio = 1.0;
            const BufferPolicy pol = buffer_apply(req, f.fn(), &store, key, &ok);
            ck(ok && !pol.learned && f.attempts[0] == 100.0,
               "reset 后重新按用户设置 100ms 打开");
            store.reset();
        }
    }

    // ---- 7. RTA_BUF_RESPECT=0 ⟹ 旧 20ms 行为 ----
    {
        std::printf("[回滚开关]\n");
        FakeOpener f; f.rate = 96000; f.ratio = 1.86;
        BufferLearnStore none;
        bool ok = false;
        BufferRequest req; req.requestedMs = 100.0; req.rate = 96000; req.respect = false;
        const BufferPolicy pol = buffer_apply(req, f.fn(), &none, "k", &ok);
        ck(ok && pol.policy == "forced20" && pol.effectiveMs == 20.0,
           "RTA_BUF_RESPECT=0 ⟹ 强制 20ms（与旧代码一致）", pol.policy);
        ck(f.attempts.size() == 1 && f.attempts[0] == 20.0,
           "回滚时不做任何 L1 尝试", attempts_str(f));

#ifdef _WIN32
        _putenv_s("RTA_BUF_RESPECT", "0");
        ck(!rta::audio::buffer_respect_env(), "环境变量 RTA_BUF_RESPECT=0 被识别");
        _putenv_s("RTA_BUF_RESPECT", "1");
        ck(rta::audio::buffer_respect_env(), "RTA_BUF_RESPECT=1 尊重设置");
        _putenv_s("RTA_BUF_RESPECT", "");
        ck(rta::audio::buffer_respect_env(), "未设置 RTA_BUF_RESPECT ⟹ 默认尊重设置");
#endif
    }

    // ---- 8. L2 起播看门狗判据 ----
    {
        std::printf("[L2 判据]\n");
        rta::audio::StartWatchdog w;
        ck(!w.armed(), "未 arm 时不判");
        w.arm(1000.0);
        ck(!w.should_degrade(1700.0, 0, true), "0.7s 零写入 ⟹ 还不降级（留出起播斜坡）");
        ck(w.should_degrade(1800.0, 0, true), "0.8s 零写入且有数据 ⟹ 判死");
        ck(!w.should_degrade(3000.0, 1, true), "已有写入 ⟹ 不降级");
        ck(!w.should_degrade(3000.0, 0, false), "手里没数据 ⟹ 不降级（不是设备的锅）");
    }

    // ---- 9. 参数矩阵 20/50/100/200ms × 44.1/48/96k ----
    {
        std::printf("[参数矩阵]\n");
        const uint32_t rates[] = {44100u, 48000u, 96000u};
        const double   bufs[]  = {20.0, 50.0, 100.0, 200.0};
        int pass = 0, total = 0;
        for (uint32_t rate : rates) {
            for (double ms : bufs) {
                ++total;
                FakeOpener f; f.rate = rate;
                BufferLearnStore none;
                bool ok = false;
                BufferRequest req; req.requestedMs = ms; req.rate = rate; req.useLearn = false;
                const BufferPolicy pol = buffer_apply(req, f.fn(), &none, "k", &ok);
                const size_t want = (size_t)std::llround((double)rate * ms / 1000.0);
                if (ok && pol.effectiveMs == ms && !pol.degraded && pol.policy == "requested" &&
                    f.frames == want && f.attempts.size() == 1) {
                    ++pass;
                } else {
                    std::printf("    [x] rate=%u ms=%.0f effective=%.0f frames=%zu want=%zu policy=%s\n",
                                (unsigned)rate, ms, pol.effectiveMs, f.frames, want, pol.policy.c_str());
                }
            }
        }
        ck(pass == total, "12 种组合全部按请求值打开（帧数 = rate × ms）",
           std::to_string(pass) + "/" + std::to_string(total));

        // 同一矩阵下的 L1 降级：驱动放大 1.9 倍 ⟹ 全部回退 20ms
        pass = 0; total = 0;
        for (uint32_t rate : rates) {
            for (double ms : bufs) {
                if (ms <= 20.0) continue;
                ++total;
                FakeOpener f; f.rate = rate; f.ratio = 1.9;
                BufferLearnStore none;
                bool ok = false;
                BufferRequest req; req.requestedMs = ms; req.rate = rate; req.useLearn = false;
                const BufferPolicy pol = buffer_apply(req, f.fn(), &none, "k", &ok);
                const size_t want = (size_t)std::llround((double)rate * 20.0 / 1000.0);
                if (ok && pol.degraded && pol.effectiveMs == 20.0 && f.frames == want) ++pass;
                else std::printf("    [x] L1 rate=%u ms=%.0f effective=%.0f frames=%zu want=%zu\n",
                                 (unsigned)rate, ms, pol.effectiveMs, f.frames, want);
            }
        }
        ck(pass == total, "9 种组合在驱动放大时全部降级 20ms", std::to_string(pass) + "/" + std::to_string(total));
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "buffer_policy 全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
