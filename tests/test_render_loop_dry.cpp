// test_render_loop_dry.cpp — C2/C3 遥测与端点消失回归（2026-10-24 N300 欠载）
//
// 被测对象是生产代码 src/audio/render_loop.h（rtaweb 播放线程跑的就是同一份循环，
// 铁律 3：不测拷贝）。mock sink 只替换设备 I/O，测试【绝不打开音频设备】（铁律 1）。
//
// 覆盖（方案 §8）：
//   · dry 计数：起播 500ms 后，pad==0 + 手里有数据 + 写不进去 ⟹ 计一次；500ms 内不计数
//   · pad>0 的写失败不算 dry（设备缓冲满 ≠ 欠载）
//   · dryms 与注入的 dry 时长一致（±50ms）
//   · STAT 新字段 per/bf/dry/dryms/ev/evint/prod 真的打印出来
//   · WFAIL 明细只打前 200 条，且带 room/per/avail/toWrite/zr
//   · C3：端点消失 ⟹ DEVICE_LOST + 「输出设备已断开」，且不提示回退共享
//   · C1 L2：起播 0.8s 有数据零写入 ⟹ 调 l2Degrade 重开 20ms，循环改用新缓冲
//
// ⚠️ 未验证（铁律 4）：真机（N300 / DC-Elite / MOTU）上的 dry 与 DEVICE_LOST 行为未复测；
//    本测试【仅 mock 验证】。STARVING 判据本身不在这里重测（test_renderfail 的 8 号用例
//    已钉住「健康设备不误报」），本文件的用例显式关闭 starveEnabled，避免两套判据互相干扰。
#include "audio/render_loop.h"
#include <algorithm>
#include <atomic>
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
// 从 STAT 行里取某个整数字段（" dry=12" → 12）；找不到返回 -1。
static long long field(const std::string& line, const char* name) {
    const std::string key = std::string(" ") + name + "=";
    const size_t p = line.find(key);
    if (p == std::string::npos) return -1;
    return std::atoll(line.c_str() + p + key.size());
}
static std::string last_stat(const std::vector<std::string>& logs) {
    for (auto it = logs.rbegin(); it != logs.rend(); ++it)
        if (it->rfind("STAT ", 0) == 0) return *it;
    return {};
}
static int count_prefix(const std::vector<std::string>& logs, const char* prefix) {
    int n = 0;
    for (const auto& s : logs) if (s.rfind(prefix, 0) == 0) ++n;
    return n;
}

using rta::render::OutSink;
using rta::render::SinkOpenOpts;

// ---- mock：可编排的「写不进去」形态，虚拟时钟每 tick +20ms ----
struct ScriptSink : OutSink {
    ScriptSink(double* clk, long long bf, long long per, long long writePer)
        : clk_(clk), bf_(bf), per_(per), writePer_(writePer) {}

    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return (size_t)bf_; }
    long long device_period_frames() const override { return per_; }
    bool wait_event(int) override { *clk_ += 0.020; return true; }   // 每次都有事件
    size_t write_one_period(const int32_t* p, size_t n) override { return write_available(p, n); }
    size_t write_available(const int32_t*, size_t n) override {
        if (n == 0) return 0;          // 没数据可写：不计入脚本拍子（否则与 dry 的 pend>0 错位）
        ++calls_;
        bool wantFail = false;
        if (failFirstCalls_ > 0 && calls_ <= failFirstCalls_) wantFail = true;
        else if (failEvery_ > 0 && (calls_ % failEvery_) == 0) wantFail = true;
        if (dead_) wantFail = true;
        pad_ = wantFail ? failPad_ : padOnSuccess_;
        // dry 的判定口径与 render_loop 完全一致：起播 500ms 后、本次写入前 pad==0、有数据。
        // 注意「写成功」也算：设备缓冲已经触底，补上都来不及消除那段空窗（方案 §6.3）。
        if (pad_ == 0 && *clk_ >= 0.5 && n > 0) ++eligible_;
        if (wantFail) return 0;
        return std::min(n, (size_t)writePer_);
    }
    long long last_pad() const override { return pad_; }
    const std::string& last_error() const override { return e_; }
    // 归因/WFAIL 明细：与 WasapiOutput::last_write_detail 同字段
    bool last_write_attempt(WriteAttempt& out) const override {
        out.pad = pad_; out.room = pad_ >= bf_ ? 0 : bf_ - pad_;
        out.per = per_; out.avail = 9600; out.toWrite = 0; out.gbHr = 0;
        return true;
    }
    long long zero_pad_full() const override { return padFull_; }
    long long zero_starved() const override { return starved_; }
    std::string zero_reason() const override { return pad_ >= bf_ ? "pad=full" : "other"; }

    double* clk_;
    long long bf_, per_, writePer_;
    long long failEvery_ = 0;       // 每 N 次调用失败一次
    long long failFirstCalls_ = 0;  // 前 N 次调用必失败
    long long failPad_ = 0;         // 失败时报告的设备水位（0 = 真·欠载；bf = 缓冲满）
    long long padOnSuccess_ = 4800; // 写成功时报告的设备水位（4800 = 健康，0 = 2 周期缓冲形态）
    bool dead_ = false;             // 永远写不进去
    long long eligible_ = 0;        // mock 自己数「应计 dry」的次数（时钟 ≥0.5s 且 pad==0）
    long long calls_ = 0;
    long long pad_ = 0;
    long long padFull_ = 0, starved_ = 0;
    std::string e_;
};

// ---- mock：端点已从系统枚举中消失（C3）----
struct LostSink : OutSink {
    explicit LostSink(double* clk) : clk_(clk) {}
    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return 9600; }
    long long device_period_frames() const override { return 960; }
    bool wait_event(int) override { *clk_ += 0.020; return true; }
    size_t write_one_period(const int32_t*, size_t) override { return 0; }
    size_t write_available(const int32_t*, size_t) override { pad_ = 9600; return 0; }
    long long last_pad() const override { return pad_; }
    bool endpoint_alive() const override { return false; }     // ← 端点消失
    const std::string& last_error() const override { return e_; }
    double* clk_;
    long long pad_ = 9600;
    std::string e_;
};

// ---- mock：L2 形态（Start 后流不推进；被要求降级后正常）----
struct L2Sink : OutSink {
    explicit L2Sink(double* clk) : clk_(clk) {}
    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return degraded_ ? 1920u : 9600u; }
    long long device_period_frames() const override { return 960; }
    bool wait_event(int) override { *clk_ += 0.020; return true; }
    size_t write_one_period(const int32_t* p, size_t n) override { return write_available(p, n); }
    size_t write_available(const int32_t*, size_t n) override {
        if (!degraded_) { pad_ = 9600; return 0; }      // 流不推进
        pad_ = 0;
        return std::min(n, (size_t)1920);
    }
    long long last_pad() const override { return pad_; }
    bool endpoint_alive() const override { return true; }
    const std::string& last_error() const override { return e_; }
    void degrade() { degraded_ = true; }
    bool degraded_ = false;
    double* clk_;
    long long pad_ = 9600;
    std::string e_;
};

struct Bench {
    std::atomic<bool> stop{false}, pauseReq{false}, paused{false};
    std::atomic<double> seekReq{-1.0};
    std::atomic<size_t> played{0};
    std::atomic<double> volDb{0.0}, startDb{0.0};
    std::atomic<long long> dbgPad{0};
    bool gpu = false;
    rta::render::RenderQ rq;
    std::atomic<long long> w0{0}, wr{0}, pulls{0}, clip{0};
    std::atomic<double> peak{0.0};
    std::atomic<long long> dry{0}, dryMs{0}, ev{0}, evInt{0}, prod{0};
    std::vector<std::string> logs;
    double clock = 0.0;

    rta::render::RenderHost make(uint32_t rate) {
        rta::render::RenderHost h;
        h.stop = &stop; h.pauseReq = &pauseReq; h.paused = &paused;
        h.seekReq = &seekReq; h.played = &played;
        h.volDb = &volDb; h.startDb = &startDb; h.dbgPad = &dbgPad; h.gpuOut = &gpu;
        h.rq = &rq;
        h.nowSec = [this]() { return clock; };
        h.deviceRate = rate; h.outBits = 24; h.outCh = 2;
        h.inRate = rate; h.inFrames = 2000000; h.totalOut = 2000000;
        h.dbgW0 = &w0; h.dbgWr = &wr; h.dbgPulls = &pulls; h.dbgClip = &clip; h.dbgPeak = &peak;
        h.dbgDry = &dry; h.dbgDryMs = &dryMs; h.dbgEv = &ev; h.dbgEvInt = &evInt; h.dbgProd = &prod;
        h.log = [this](const std::string& s) { logs.push_back(s); };
        h.deviceLabel = "扬声器 (mock)";
        h.deviceIdDesc = "{0.0.0.00000000}.{mock-endpoint}";
        h.fallbackEligible = true;
        h.dop = false;
        h.onShared = false;
        h.starveEnabled = false;     // 本文件只测 C2/C3；STARVING 由 test_renderfail 覆盖
        h.statPeriodSec = 1.0;       // 虚拟时钟下 1 秒一条 STAT，便于断言字段
        return h;
    }
    void feed_frames(size_t frames, size_t chunk = 4096) {
        size_t left = frames;
        while (left > 0) {
            const size_t n = left < chunk ? left : chunk;
            rta::audio::AudioBuffer b;
            b.rate = 96000; b.channels = 2; b.bits = 24;
            b.raw.assign(n * 2, 0);
            rq.q.emplace_back(std::move(b), rq.epoch.load());
            left -= n;
        }
        rq.done = true;
    }
};

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== render_loop_dry：dry/STAT/WFAIL/DEVICE_LOST/L2（mock）==\n");

    // ---- 1. dry 计数 + dryms：每 10 拍失败 1 次，失败时 pad==0 ----
    {
        std::printf("[dry/dryms]\n");
        Bench b;
        b.feed_frames(96000u * 8u);
        auto h = b.make(96000);
        ScriptSink sink(&b.clock, /*bf=*/9600, /*per=*/960, /*writePer=*/1920);
        sink.failEvery_ = 10;         // 每 10 次调用（200ms）失败一次
        rta::render::render_loop(sink, h);
        ck(!h.renderFail, "间歇欠载不触发 RENDERFAIL（吞吐正常）", h.failMsg);
        ck(sink.eligible_ > 0, "mock 确实注入了 pad==0 的写失败", std::to_string(sink.eligible_));
        ck(b.dry.load() == sink.eligible_,
           "dry 次数 == 注入的「pad==0 且写不进去」次数",
           "dry=" + std::to_string(b.dry.load()) + " 注入=" + std::to_string(sink.eligible_));
        const long long expectMs = sink.eligible_ * 20;   // 每拍 20ms，单拍故障
        ck(std::llabs((long long)b.dryMs.load() - expectMs) <= 50,
           "dryms ≈ 注入时长（±50ms）",
           "dryms=" + std::to_string(b.dryMs.load()) + " 期望≈" + std::to_string(expectMs));
        const std::string st = last_stat(b.logs);
        ck(!st.empty(), "虚拟时钟下 STAT 有输出", st);
        ck(field(st, "per") == 960, "STAT per = 设备周期帧数", st);
        ck(field(st, "bf") == 9600, "STAT bf = 缓冲帧数", st);
        ck(field(st, "dry") > 0, "STAT dry 反映了欠载", st);
        ck(field(st, "dryms") >= 0, "STAT dryms 存在", st);
        ck(field(st, "ev") > 0, "STAT ev = wait_event 命中次数", st);
        ck(field(st, "evint") == 20, "STAT evint = 20ms（mock 事件间隔）", st);
        ck(field(st, "prod") >= 0, "STAT prod 存在（pull_chunk 最长阻塞）", st);
    }

    // ---- 1b. A 组形态：2 个周期的 20ms 缓冲，每次轮询 pad 都已触底（但都补上了）----
    // 这是方案 §2 的现场形态（pad==0 占 85.7%、吞吐仍 1.0）。旧代码的 STARVING 看吞吐率
    // 永远看不见它；新 dry 必须在【写成功】时也能计数，否则判据又失明。
    {
        std::printf("[A 组形态：2 周期缓冲 pad 触底]\n");
        Bench b;
        b.feed_frames(96000u * 8u);
        auto h = b.make(96000);
        ScriptSink sink(&b.clock, /*bf=*/1920, /*per=*/960, /*writePer=*/1920);
        sink.padOnSuccess_ = 0;      // 设备在每次轮询时都是空的
        rta::render::render_loop(sink, h);
        ck(!h.renderFail, "吞吐正常：每次都补上了，无 RENDERFAIL", h.failMsg);
        ck(b.dry.load() > 0, "pad 触底的轮询被 dry 计数（即使这一拍写成功）",
           std::to_string(b.dry.load()));
        ck(b.dry.load() == sink.eligible_, "dry 次数 == mock 统计的触底轮询数",
           "dry=" + std::to_string(b.dry.load()) + " 注入=" + std::to_string(sink.eligible_));
        ck(b.dryMs.load() > 7000, "dryms 累计处于触底状态的墙钟（≈7.5s）",
           std::to_string(b.dryMs.load()));
    }

    // ---- 1c. B 组形态：10 个周期的 100ms 缓冲，pad 不触底 ⟹ dry=0（M2 判据形态）----
    {
        std::printf("[B 组形态：10 周期缓冲 pad 不触底]\n");
        Bench b;
        b.feed_frames(96000u * 8u);
        auto h = b.make(96000);
        ScriptSink sink(&b.clock, /*bf=*/9600, /*per=*/960, /*writePer=*/1920);
        sink.padOnSuccess_ = 4800;   // 设备缓冲里始终有货
        rta::render::render_loop(sink, h);
        ck(b.dry.load() == 0, "pad 不触底 ⟹ dry=0", std::to_string(b.dry.load()));
        ck(b.dryMs.load() == 0, "dryms=0", std::to_string(b.dryMs.load()));
        ck(!h.renderFail, "B 组形态无 RENDERFAIL");
    }

    // ---- 2. 起播 500ms 内不计数 ----
    {
        std::printf("[500ms 门槛]\n");
        Bench b;
        b.feed_frames(96000u * 4u);
        auto h = b.make(96000);
        ScriptSink sink(&b.clock, 9600, 960, 1920);
        sink.failFirstCalls_ = 20;    // 前 20 拍（= 0.4s）全失败，之后正常
        rta::render::render_loop(sink, h);
        ck(b.w0.load() >= 20, "确实发生了 20 次写失败", std::to_string(b.w0.load()));
        ck(b.dry.load() == 0, "起播 500ms 内的写失败不计 dry", "dry=" + std::to_string(b.dry.load()));
        ck(b.dryMs.load() == 0, "起播 500ms 内 dryms 仍为 0", std::to_string(b.dryMs.load()));
    }

    // ---- 3. pad>0（设备缓冲满）的写失败不算 dry ----
    {
        std::printf("[pad 满 ≠ 欠载]\n");
        Bench b;
        b.feed_frames(96000u * 4u);
        auto h = b.make(96000);
        ScriptSink sink(&b.clock, 9600, 960, 1920);
        sink.dead_ = true;            // 永远写不进去
        sink.failPad_ = 9600;         // 但设备缓冲是满的（不是我们欠载）
        rta::render::render_loop(sink, h);
        ck(b.w0.load() > 0, "确实有写失败", std::to_string(b.w0.load()));
        ck(b.dry.load() == 0, "pad 满导致的写失败不计 dry", "dry=" + std::to_string(b.dry.load()));
        ck(h.renderFail, "持续 4 秒不接受数据仍按 RENDERFAIL 归因");
    }

    // ---- 4. WFAIL 明细：前 200 条 + 字段齐全 ----
    {
        std::printf("[WFAIL 前 200 条]\n");
        Bench b;
        b.feed_frames(96000u * 4u);
        auto h = b.make(96000);
        ScriptSink sink(&b.clock, 9600, 960, 1920);
        sink.dead_ = true;
        sink.failPad_ = 9600;
        rta::render::render_loop(sink, h);
        const int n = count_prefix(b.logs, "WFAIL ");
        ck(n > 0, "写失败有 WFAIL 明细", std::to_string(n));
        ck(n <= 200, "WFAIL 最多 200 条（不刷爆日志）", std::to_string(n));
        std::string first;
        for (const auto& s : b.logs) if (s.rfind("WFAIL ", 0) == 0) { first = s; break; }
        ck(contains(first, "t=") && contains(first, "pad=9600"), "WFAIL 带 t 与 pad", first);
        ck(contains(first, "room=0") && contains(first, "per=960"), "WFAIL 带 room/per", first);
        ck(contains(first, "avail=") && contains(first, "toWrite=0"),
           "WFAIL 带 avail 与量化后的 toWrite", first);
        ck(contains(first, "gb_hr="), "WFAIL 带 GetBuffer HRESULT", first);
        ck(contains(first, "zr="), "WFAIL 带归因 zr", first);
        std::printf("  样例 WFAIL: %s\n", first.c_str());
    }

    // ---- 5. C3：端点消失 ⟹ DEVICE_LOST，如实报「输出设备已断开」----
    {
        std::printf("[DEVICE_LOST]\n");
        Bench b;
        b.feed_frames(96000u * 4u);
        auto h = b.make(96000);
        LostSink sink(&b.clock);
        rta::render::render_loop(sink, h);
        ck(h.renderFail, "设备消失 ⟹ renderFail");
        ck(h.deviceLost, "deviceLost 置位（调用方据此不回退共享）");
        ck(contains(h.failMsg, "输出设备已断开"), "ps.error 文案 = 输出设备已断开", h.failMsg);
        ck(contains(h.failMsg, "USB"), "文案说明是 USB 掉线这类物理断开", h.failMsg);
        ck(contains(h.failMsg, "不自动回退") && contains(h.failMsg, "不从头重播"),
           "文案明确不降级、不重播", h.failMsg);
        ck(!contains(h.failMsg, "已自动回退"), "没有「已自动回退共享」这种误导", h.failMsg);
        ck(count_prefix(b.logs, "DEVICE_LOST ") == 1, "play_log 里有一条 DEVICE_LOST",
           std::to_string(count_prefix(b.logs, "DEVICE_LOST ")));
        for (const auto& s : b.logs)
            if (s.rfind("DEVICE_LOST ", 0) == 0) { std::printf("  样例 DEVICE_LOST: %s\n", s.c_str()); break; }
        std::printf("  样例 ps.error: %s\n", h.failMsg.c_str());
    }

    // ---- 6. C1 L2：起播 0.8s 有数据零写入 ⟹ 降级 20ms 后继续播 ----
    {
        std::printf("[L2 起播看门狗]\n");
        Bench b;
        b.feed_frames(96000u * 8u);
        auto h = b.make(96000);
        L2Sink sink(&b.clock);
        int hookCalls = 0;
        h.l2Degrade = [&]() { ++hookCalls; sink.degrade(); return true; };
        rta::render::render_loop(sink, h);
        ck(hookCalls == 1, "L2 只尝试一次", std::to_string(hookCalls));
        ck(sink.degraded_, "L2 确实重开了 20ms");
        ck(!h.renderFail, "降级后正常播放，不触发 RENDERFAIL", h.failMsg);
        ck(h.normalEnd, "降级后播完（normalEnd）");
        bool sawNewBf = false;
        for (const auto& s : b.logs)
            if (s.rfind("STAT ", 0) == 0 && field(s, "bf") == 1920) sawNewBf = true;
        ck(sawNewBf, "降级后 STAT bf 变成 1920（循环重取了 period）");
        bool sawNote = false;
        for (const auto& s : b.logs)
            if (s.rfind("BUFPOLICY l2_degraded", 0) == 0) sawNote = true;
        ck(sawNote, "日志留下 BUFPOLICY l2_degraded 归因");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "render_loop_dry 全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
