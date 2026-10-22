// test_renderfail.cpp — 渲染主循环 RENDERFAIL 归因回归（2026-10-22 VM 事故）
//
// 事故：VM 板载 HD Audio 上独占流 GetCurrentPadding 恒满 → write_available 恒 0 →
// 200 次超时后循环【静默 break】，ps.error 从未被赋值 → END pos=0 error="" 悬案。
//
// 本测试用 mock OutSink（write_available 恒 0 的"假死 sink"）真跑 src/audio/render_loop.h
// 的渲染循环（rtaweb 播放线程跑的是同一份实现），断言：
//   1. renderFail 置位，failMsg 非空且含 RENDERFAIL / 未接受数据 / w0 / last_pad /
//      GetBuffer HRESULT / device 等归因字段（rtaweb 把 failMsg 原样赋 ps.error ——
//      这是本次事故的回归测试：任何非正常退出都必须有非空 error）；
//   2. 正常消费完 → normalEnd（显式「正常结束」语义）；
//   3. 用户停止 → 停止语义（既非 renderFail 也非 normalEnd）；
//   4. DoP（不可回退）与「不可回退且非 DoP」两种文案各归其位。
//
//   5. wasapi_out 的「写不进去」归因分类（ZeroAttrib）与循环侧 zp/zs/zr 输出
//      （2026-10-22 反馈 20261022-02：w0 混着「缓冲满」与「喂不上一个周期」两种成因）。
//
// 说明（如实）：测试对象是渲染循环本体与 failMsg 文本；ps.error = failMsg 的赋值点
// 在 rtaweb.cpp play_one_impl（一行原样传递），不在本测试可达范围。
// ⚠️ 未覆盖（铁律 4）：WasapiOutput 的 GetCurrentPadding→ZeroAttrib 接线必须真开设备，
// 本测试不覆盖（铁律 1：不接管听音设备）；另 rtaweb 的 WasapiSink 尚未转发这 4 个接口，
// 详见 反馈和开发\debug\20261022-02\分析.md「未完成项」。
#include "audio/render_loop.h"
#include "audio/wasapi_out.h"   // ZeroAttrib：「写不进去」的归因分类（真机接线不在本文件）
#include <cstdio>
#include <string>
#include <type_traits>
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

using rta::render::OutSink;
using rta::render::SinkOpenOpts;

// 真机 STAT 的 zp/zs/zr 依赖 WasapiOutput 的这几个接口（由 rtaweb 的 WasapiSink 转发）。
// 这里钉住签名：后续重构若把接口改掉，本测试在【编译期】就红（不发声、不开设备）。
static_assert(std::is_same_v<decltype(&rta::audio::WasapiOutput::zero_pad_full),
                             long long (rta::audio::WasapiOutput::*)() const>,
              "zero_pad_full() 必须保持 const 返回 long long（STAT zp 用）");
static_assert(std::is_same_v<decltype(&rta::audio::WasapiOutput::zero_starved),
                             long long (rta::audio::WasapiOutput::*)() const>,
              "zero_starved() 必须保持 const 返回 long long（STAT zs 用）");
static_assert(std::is_same_v<decltype(&rta::audio::WasapiOutput::last_zero_reason),
                             std::string (rta::audio::WasapiOutput::*)() const>,
              "last_zero_reason() 必须保持 const 返回 std::string（STAT zr 用）");

// ---- mock：write_available 恒 0（VM 独占流假死形态），wait_event 立即返回（测试提速）----
struct FakeDeadSink : rta::render::OutSink {
    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return 9600; }
    size_t write_one_period(const int32_t*, size_t) override { return 0; }
    size_t write_available(const int32_t*, size_t) override { return 0; }
    long long last_pad() const override { return 9600; }          // 恒满（事故形态）
    long long last_gb_hr() const override { return -999; }        // 写入前就在 pad 检查处被拒
    const std::string& last_error() const override { return e_; }
    std::string e_;
};

// ---- mock：来多少吃多少（健康设备形态）----
struct FakeLiveSink : rta::render::OutSink {
    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return 9600; }
    size_t write_one_period(const int32_t*, size_t n) override { return n; }
    size_t write_available(const int32_t*, size_t n) override { return n; }
    long long last_pad() const override { return 0; }
    const std::string& last_error() const override { return e_; }
    std::string e_;
};

// ---- mock：共享设备的真实形态——每次调用最多消化 ~20ms（缓冲的 1/10）----
// 2026-10-22 e2e 事故回归：eos 后「不足一个周期补静音」若无 once 限制，
// pendFrames 每次落回 period 以下都会被重新补齐 → 0.5 秒文件无限播。
struct FakeSharedSink : rta::render::OutSink {
    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return 8820; }            // 200ms @44100
    size_t write_one_period(const int32_t*, size_t n) override { return eat(n); }
    size_t write_available(const int32_t*, size_t n) override { return eat(n); }
    long long last_pad() const override { return 7938; }
    const std::string& last_error() const override { return e_; }
    std::string e_;
private:
    static size_t eat(size_t want) { return want < 882u ? want : 882u; }   // 每 tick ≤20ms
};

// ---- mock：间歇性成功（2026-10-22 用户真机反馈 20261022-02 的真实形态）----
// 现场数字：96k/100ms 缓冲（period=9600 帧），STAT 里 w0=205 wr=41 —— 246 次尝试
// 只成功 41 次，失败恰好是成功的 5 倍。也就是说【每 6 次轮询才写进去 1 次】，
// 而每次成功正好写走一个整缓冲（100ms 音频）。听感 = 每 120ms 断 20ms ⟹ 持续「啵」。
//
// 这个 mock 用【虚拟时钟】而不是真 Sleep：dt 就是「一次轮询 = 20ms」。
// 判据（写入帧/秒 ÷ deviceRate）在真实 WASAPI 上要 3 秒以上才显形，
// 用虚拟时钟能确定性地重放同一形态，且让测试瞬间跑完。
struct FakeIntermittentSink : rta::render::OutSink {
    explicit FakeIntermittentSink(double* clock) : clock_(clock) {}
    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return 9600; }        // 100ms @96k（现场值）
    size_t write_one_period(const int32_t*, size_t) override { return step(); }
    size_t write_available(const int32_t*, size_t n) override {
        // 现场：成功一次正好写满一个整缓冲（41×9600=393600=4.1s@96k）
        const size_t r = step();
        return r && n >= 9600u ? 9600u : 0u;
    }
    long long last_pad() const override { return pad_; }
    long long last_gb_hr() const override { return 0; }
    const std::string& last_error() const override { return e_; }
    std::string e_;
private:
    // 每 6 次尝试成功 1 次（= 现场 w0/wr = 5.0 的整数形态）；每次尝试推进虚拟时钟 20ms
    size_t step() {
        *clock_ += 0.020;
        pad_ = (++n_ % 6u == 0u) ? 0 : 9600;   // 只有该写的那一拍 pad 才腾空
        return (n_ % 6u == 0u) ? 1u : 0u;
    }
    double* clock_;
    unsigned n_ = 0;
    long long pad_ = 9600;
};

// ---- mock：健康独占设备（用于证明新判据不会误报）----
// 关键：健康设备一次写满整个缓冲，所以每 5 个 tick 才写一次 —— 若拿
// 「写入次数/轮询次数」（duty≈20%）当判据，健康设备 100% 会被误判成饥饿。
// 吞吐率判据下它是 1.0，必须【不】触发 STARVING。
struct FakeHealthySink : rta::render::OutSink {
    explicit FakeHealthySink(double* clock) : clock_(clock) {}
    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return 9600; }
    size_t write_one_period(const int32_t*, size_t) override { return tick(); }
    size_t write_available(const int32_t*, size_t n) override {
        return tick() && n >= 9600u ? 9600u : 0u;
    }
    long long last_pad() const override { return pad_; }
    const std::string& last_error() const override { return e_; }
    std::string e_;
private:
    // 每 tick 设备消耗 1920 帧（96k×20ms），攒满 5 个 tick = 9600 帧才接受一次写入
    size_t tick() {
        *clock_ += 0.020;
        if (++n_ % 5u == 0u) { pad_ = 0; return 1u; }
        pad_ = 9600;
        return 0u;
    }
    double* clock_;
    unsigned n_ = 0;
    long long pad_ = 9600;
};

// ---- mock：把「写不进去」的成因计数摆到 OutSink 接口上 ----
// 真机上这些数由 WasapiOutput::write_available（内部 ZeroAttrib）统计，再由 rtaweb 的
// WasapiSink 转发（⚠️ 截至 2026-10-22 尚未转发，见 分析.md「未完成项」）。
// 本 mock 用固定值验证【渲染循环侧】确实把 zp/zs/zr 带进了 RENDERFAIL 文案。
struct FakeCountedDeadSink : rta::render::OutSink {
    bool open(const SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return 9600; }
    size_t write_one_period(const int32_t*, size_t) override { return 0; }
    size_t write_available(const int32_t*, size_t) override { return 0; }
    long long last_pad() const override { return 9600; }
    long long zero_pad_full() const override { return 7; }
    long long zero_starved()  const override { return 3; }
    std::string zero_reason() const override { return "pad=full"; }
    const std::string& last_error() const override { return e_; }
    std::string e_;
};

struct Bench {
    // PlayerState 子集
    std::atomic<bool> stop{false}, pauseReq{false}, paused{false};
    std::atomic<double> seekReq{-1.0};
    std::atomic<size_t> played{0};
    std::atomic<double> volDb{0.0}, startDb{0.0};
    std::atomic<long long> dbgPad{0};
    bool gpu = false;
    // 队列 + 计数
    rta::render::RenderQ rq;
    std::atomic<long long> w0{0}, wr{0}, pulls{0}, clip{0};
    std::atomic<double> peak{0.0};
    std::vector<std::string> logs;
    double clock = 0.0;   // 虚拟时钟（只给吞吐率判据的用例用）

    rta::render::RenderHost make(bool fallbackOk, bool dop, bool onShared = false) {
        rta::render::RenderHost h;
        h.stop = &stop; h.pauseReq = &pauseReq; h.paused = &paused;
        h.seekReq = &seekReq; h.played = &played;
        h.volDb = &volDb; h.startDb = &startDb; h.dbgPad = &dbgPad; h.gpuOut = &gpu;
        h.rq = &rq;
        // 吞吐率判据默认关掉：既有 6 个用例用真实时钟 + 瞬时 sink，
        // 虚拟时间线对他们没有意义（铁律 3：旧断言不许红，此处也不许被新判据干扰）。
        h.starveEnabled = false;
        h.deviceRate = 44100; h.outBits = 24; h.outCh = 2;
        h.inRate = 44100; h.inFrames = 200000; h.totalOut = 200000;
        h.dbgW0 = &w0; h.dbgWr = &wr; h.dbgPulls = &pulls; h.dbgClip = &clip; h.dbgPeak = &peak;
        h.log = [this](const std::string& s) { logs.push_back(s); };
        h.deviceLabel = "扬声器 (High Definition Audio Device)";
        h.deviceIdDesc = "{0.0.0.00000000}.{a18ee6ff-b151-4a80-9590-ec694ae7a798}";
        h.fallbackEligible = fallbackOk;
        h.dop = dop;
        h.onShared = onShared;
        return h;
    }
    // 吞吐率用例专用：96k + 100ms 缓冲（= 现场 20261022-02 的形状），虚拟时钟接管
    rta::render::RenderHost make_starve(uint32_t rate) {
        rta::render::RenderHost h = make(/*fallbackOk=*/true, /*dop=*/false);
        h.deviceRate = rate;
        h.nowSec = [this]() { return clock; };
        h.starveEnabled = true;
        h.starveWindowSec = 1.0;
        h.starveRatio = 0.90;
        h.starveSecs = 3.0;
        return h;
    }
    // 灌足够多的真实数据：判据要跑 >4 秒×96k ≈ 38.4 万帧，4096 帧/块
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

    void feed(size_t chunks) {   // 预先灌 3 块 × 4096 帧 > 一个周期 9600 帧
        for (size_t i = 0; i < chunks; ++i) {
            rta::audio::AudioBuffer b;
            b.rate = 44100; b.channels = 2; b.bits = 24;
            b.raw.assign(4096u * 2u, 0);
            rq.q.emplace_back(std::move(b), rq.epoch.load());
        }
        rq.done = true;
    }
};

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== renderfail：mock 假死 sink 真跑渲染主循环 ==\n");

    // ---- 1. 事故回归：write_available 恒 0 → 必须 RENDERFAIL + 非空 failMsg ----
    {
        Bench b; b.feed(3);
        auto h = b.make(/*fallbackOk=*/true, /*dop=*/false);
        FakeDeadSink sink;
        rta::render::render_loop(sink, h);
        ck(h.renderFail, "假死设备触发 renderFail");
        ck(!h.normalEnd, "假死设备不误判为正常结束");
        ck(!h.failMsg.empty(), "failMsg 非空（rtaweb 将其原样赋 ps.error）");
        const std::string& m = h.failMsg;
        ck(contains(m, "RENDERFAIL"), "failMsg 含 RENDERFAIL 标记", m);
        ck(contains(m, "未接受数据"), "failMsg 含「未接受数据」人话归因", m);
        ck(contains(m, "秒未接受数据"), "failMsg 含超时时长（200×20ms≈4 秒）", m);
        ck(contains(m, "w0=") && contains(m, "wr="), "failMsg 含 w0/wr 计数", m);
        ck(contains(m, "last_pad=9600"), "failMsg 含 last_pad（恒满 = 事故特征）", m);
        ck(contains(m, "GetBuffer HRESULT=-999"), "failMsg 含 GetBuffer HRESULT", m);
        ck(contains(m, "High Definition Audio Device") && contains(m, "a18ee6ff"), "failMsg 含设备名与端点 id", m);
        ck(contains(m, "已自动回退"), "可回退时 failMsg 说明已回退共享", m);
        bool logged = false;
        for (const auto& s : b.logs) if (s.find("RENDERFAIL") != std::string::npos) logged = true;
        ck(logged, "RENDERFAIL 同步进了播放日志（play_log 钩子）");
        ck(b.w0.load() > 0, "w0 计数被循环递增", std::to_string(b.w0.load()));
    }

    // ---- 2. DoP：不可回退文案 ----
    {
        Bench b; b.feed(3);
        auto h = b.make(/*fallbackOk=*/false, /*dop=*/true);
        FakeDeadSink sink;
        rta::render::render_loop(sink, h);
        ck(h.renderFail && contains(h.failMsg, "DoP/DSD 不能回退共享模式"),
           "DoP 场景 failMsg 指向独占/ASIO 而非共享", h.failMsg);
    }

    // ---- 3. 不可回退的非 DoP：建议改共享 ----
    {
        Bench b; b.feed(3);
        auto h = b.make(/*fallbackOk=*/false, /*dop=*/false);
        FakeDeadSink sink;
        rta::render::render_loop(sink, h);
        ck(h.renderFail && contains(h.failMsg, "WASAPI 共享"),
           "不可回退场景 failMsg 建议切换共享", h.failMsg);
    }

    // ---- 3b. 已在共享后端仍 RENDERFAIL（回退后第二次失败）：文案不再建议切共享 ----
    {
        Bench b; b.feed(3);
        auto h = b.make(/*fallbackOk=*/false, /*dop=*/false, /*onShared=*/true);
        FakeDeadSink sink;
        rta::render::render_loop(sink, h);
        ck(h.renderFail && contains(h.failMsg, "共享模式也未接受数据"),
           "共享后端 failMsg 指向设备/混音异常", h.failMsg);
    }

    // ---- 4. 健康 sink：消费完 → normalEnd（显式正常结束语义）----
    {
        Bench b; b.feed(3);
        auto h = b.make(true, false);
        FakeLiveSink sink;
        rta::render::render_loop(sink, h);
        ck(h.normalEnd && !h.renderFail, "数据被正常消费完 → normalEnd");
        ck(h.failMsg.empty(), "正常结束没有 failMsg");
        ck(b.wr.load() > 0, "wr 计数被循环递增", std::to_string(b.wr.load()));
        ck(h.played->load() == 3u * 4096u, "played 累计等于全部帧数",
           std::to_string(h.played->load()));
    }

    // ---- 5. 用户停止：停止语义 ----
    {
        Bench b; b.feed(3);
        auto h = b.make(true, false);
        b.stop.store(true);
        FakeLiveSink sink;
        rta::render::render_loop(sink, h);
        ck(!h.renderFail && !h.normalEnd, "stop 置位 → 既非 renderFail 也非 normalEnd");
    }

    // ---- 6. 共享形态（每 tick 只消化 ~20ms）+ eos：必须终止且 normalEnd（无限尾音回归）----
    // 2026-10-22 e2e 实测事故：0.5s 文件在共享后端 pos 冲到 207 秒仍在写 ——
    // eos 后「补静音到周期」每 tick 重触发。修复 = 只补一次。判据：played 有界
    //（= 已拉数据 + 最多一个周期尾音），且循环以 normalEnd 结束。
    {
        Bench b; b.feed(3);                     // 3 × 4096 = 12288 帧真实数据
        auto h = b.make(true, false);
        FakeSharedSink sink;
        rta::render::render_loop(sink, h);
        ck(h.normalEnd && !h.renderFail, "共享形态消费完 → normalEnd（循环会终止）");
        const size_t expectMax = 12288u + 8820u;   // 真实数据 + 最多一个周期尾音
        ck(h.played->load() <= expectMax, "played 有界（≤ 数据 + 一个周期尾音）",
           "played=" + std::to_string(h.played->load()) + " 上限=" + std::to_string(expectMax));
        ck(h.played->load() >= 12288u, "真实数据全部被消费",
           "played=" + std::to_string(h.played->load()));
    }

    // ---- 7. ★ 间歇性成功导致的持续性饥饿（用户真机反馈 20261022-02）----
    // 现场：96k/24bit/100ms 缓冲，STAT w0=205 wr=41（失败恰为成功的 5 倍），
    // 41×9600=393600 帧=4.1s@96k —— 每次成功正好写满一整个缓冲。
    // 旧判据（连续 200 次写不进）在这形态下【永不触发】：写入偶有成功就把
    // timeouts 清零。结果 END 既无 RENDERFAIL 也无 error，而用户在听「啵啵啵」。
    // 本用例钉住新判据：吞吐率持续 < 90% 达 3 秒 ⟹ STARVING 归因。
    {
        Bench b;
        b.feed_frames(96000u * 12u);     // 12 秒音频，够判据跑完 3 秒
        auto h = b.make_starve(96000);
        FakeIntermittentSink sink(&b.clock);
        rta::render::render_loop(sink, h);
        ck(h.renderFail, "间歇性成功（每 6 拍成功 1 次）也被判为设备异常",
           "renderFail=" + std::to_string(h.renderFail));
        ck(!h.normalEnd, "饥饿未被误判为正常结束");
        ck(contains(h.failMsg, "STARVING"), "failMsg 含新标签 STARVING", h.failMsg);
        ck(contains(h.failMsg, "吞吐持续不足"), "failMsg 用「吞吐」而非「未接受数据」定性", h.failMsg);
        ck(contains(h.failMsg, "破音") || contains(h.failMsg, "啵"),
           "failMsg 把现象说成人话（周期性断音/破音）", h.failMsg);
        ck(h.failThroughput > 0.0 && h.failThroughput < 0.95,
           "实测吞吐份额被记录且确实不足",
           "throughput=" + std::to_string(h.failThroughput));
        // mock 造的是 0.833 形态：每 6 拍（120ms）写满 9600 帧（100ms 音频）⟹ 0.833，
        // 用于稳定复现「间歇性成功」这一类。
        // 但现场实测吞吐是 0.39，不是 0.83：现场的 w0:wr = 5:1 是【计数比】而非时间比
        // （246 次尝试 × 20ms ≈ 4.92s，墙钟 10.55s —— 大部分时间阻塞在 pull_chunk/wait_event），
        // 计数比根本映射不到时间吞吐。
        // 两者都 < 0.90 阈值，所以同一判据都能抓到。
        ck(h.failThroughput > 0.75 && h.failThroughput < 0.90,
           "mock 吞吐份额 = 0.833 形态（现场 0.39；两者都 < 0.90 阈值）",
           "throughput=" + std::to_string(h.failThroughput));
        bool logged = false;
        for (const auto& s : b.logs) if (s.find("STARVING") != std::string::npos) logged = true;
        ck(logged, "STARVING 同步进播放日志（play_log 钩子）");
        ck(contains(h.failMsg, "w0=") && contains(h.failMsg, "缓冲=9600"),
           "STARVING 归因含 w0/wr 与缓冲帧数（可与现场 STAT 对照）", h.failMsg);
        ck(contains(h.failMsg, "缓冲调大"), "STARVING 给出针对性处置（调大缓冲）", h.failMsg);
    }

    // ---- 8. ★ 反向用例（铁律 3）：健康独占设备绝不能被新判据误报 ----
    // 健康设备一次写满整个缓冲 ⟹ 每 5 个 tick 才写一次 ⟹「写入次数/轮询次数」
    // （duty）只有 20%。若判据误用 duty，此用例必红。吞吐率判据下恒为 1.0。
    {
        Bench b;
        b.feed_frames(96000u * 12u);
        auto h = b.make_starve(96000);
        FakeHealthySink sink(&b.clock);
        rta::render::render_loop(sink, h);
        ck(!h.renderFail, "健康设备（duty 仅 20% 但吞吐 100%）不被误判为饥饿", h.failMsg);
        ck(h.failMsg.empty(), "健康设备没有 failMsg");
        ck(b.clock > 4.0, "健康用例确实跑满了判据所需的时长", "clock=" + std::to_string(b.clock));
    }

    // ---- 9. 判据可关闭（starveEnabled=false 时行为与旧版一致）----
    {
        Bench b;
        b.feed_frames(96000u * 12u);
        auto h = b.make_starve(96000);
        h.starveEnabled = false;
        FakeIntermittentSink sink(&b.clock);
        rta::render::render_loop(sink, h);
        ck(!h.renderFail && h.failMsg.empty(),
           "starveEnabled=false ⟹ 新判据完全不介入（旧行为可复现）");
    }

    // ---- 10. 「写不进去」的三类归因（2026-10-22 反馈 20261022-02）----
    // 用户现场：96k/24bit/100ms 缓冲，pad=9600（采样瞬间缓冲满）、w0=205 wr=41。
    // 旧日志只有一个 w0，分不清「设备缓冲满」和「我们喂不上一个周期」—— 后者要调
    // 轮询/供料，前者要调设备，处置完全不同。ZeroAttrib 就是区分二者的判据本体。
    //
    // 为什么直接驱动 ZeroAttrib，而不是「用 mock IAudioClient 打开 WasapiOutput」：
    //   · 铁律 1：真开 WASAPI 独占会接管听音设备，必须避免；
    //   · WasapiOutput 没有注入 IAudioClient/IAudioRenderClient 的接缝，mock 驱动不了它。
    // 这里驱动的是 write_available 内部使用的同一个结构、同一份 classify/reason（不是拷贝）；
    // 未被覆盖的只剩 GetCurrentPadding→(room, per) 那段 COM 接线，需真机（如实标注）。
    {
        rta::audio::ZeroAttrib z;
        // ① 缓冲恒满：96k/100ms，pad==bufferFrames_==9600 ⟹ room==0
        z.classify(/*toWrite=*/0, /*room=*/0, /*avail=*/9600, /*per=*/9600);
        ck(z.padFull == 1 && z.starved == 0 && z.other == 0,
           "缓冲恒满 ⟹ zero_pad_full 递增（其余不涨）");
        ck(z.reason() == "pad=full", "缓冲恒满 ⟹ last_zero_reason()=pad=full", z.reason());

        // ② 供料不足：设备腾空了整缓冲（room=9600），但手里只有 4096 帧 < 一个周期
        z.classify(0, 9600, 4096, 9600);
        ck(z.starved == 1 && z.padFull == 1 && z.other == 0,
           "供料不足一个周期 ⟹ zero_starved 递增（不冒充 pad=full）");
        ck(z.reason() == "avail<period", "供料不足 ⟹ last_zero_reason()=avail<period", z.reason());

        // ③ 成功写入：成因复位，计数一个都不许涨
        const long long pf = z.padFull, zs = z.starved, zo = z.other;
        z.classify(/*toWrite=*/9600, 9600, 9600, 9600);
        ck(z.reason() == "-", "成功写入 ⟹ last_zero_reason()=\"-\"", z.reason());
        ck(z.padFull == pf && z.starved == zs && z.other == zo,
           "成功写入不改变任何计数");

        // ④ 第三类：room>0 但不足一个周期（96k 独占 period==buffer 时会落这里）
        z.classify(0, 4800, 9600, 9600);
        ck(z.other == 1, "可用空间不足一个周期 ⟹ zero_other（不冒充 pad=full）",
           "other=" + std::to_string(z.other));
        ck(z.reason() == "other", "第三类 ⟹ last_zero_reason()=other", z.reason());

        // ⑤ 累计：同一成因连续 5 次必须真的累加 5，而不是只置个标志
        for (int i = 0; i < 5; ++i) z.classify(0, 0, 9600, 9600);
        ck(z.padFull == pf + 5, "同一成因连续发生 ⟹ 计数累加（不是布尔标志）",
           "padFull=" + std::to_string(z.padFull));
    }

    // ---- 11. 循环侧确实把 zp/zs/zr 写进 RENDERFAIL 归因 ----
    {
        Bench b; b.feed(3);
        auto h = b.make(/*fallbackOk=*/true, /*dop=*/false);
        FakeCountedDeadSink sink;
        rta::render::render_loop(sink, h);
        ck(h.renderFail, "带计数的假死 sink 触发 renderFail");
        ck(contains(h.failMsg, "zp=7") && contains(h.failMsg, "zs=3"),
           "failMsg 带出 zp/zs（可与 STAT 对照）", h.failMsg);
        ck(contains(h.failMsg, "zr=pad=full"), "failMsg 带出最近一次成因 zr", h.failMsg);
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "renderfail 全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
