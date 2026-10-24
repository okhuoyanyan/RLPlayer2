// render_loop.h — 流式渲染主循环（2026-10-22 从 rtaweb.cpp play_one 抽出）
//
// 为什么抽成头文件：本轮事故（VM 上独占流 GetCurrentPadding 恒满 → 200 次超时
// 静默 break → END pos=0 error=""）的回归测试必须能用 mock sink 真跑这段循环，
// 而 rtaweb.cpp 是带 main 的可执行目标，测试没法链接。抽到头文件后 rtaweb 与
// tests/test_renderfail.cpp 共用同一份实现（铁律 3：测试要测真东西，不测拷贝）。
//
// 迁移原则：循环体逐行照搬，仅把外部引用（ps.* / RQ / g_dbg* / in.* / play_log / dlog）
// 换成 RenderHost 的指针/钩子；唯一的逻辑增量为超时 break 前的 RENDERFAIL 遥测。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "wav.h"   // rta::audio::AudioBuffer

namespace rta::render {

// ---------------- 输出后端接口（原 rtaweb.cpp OutSink，原样迁出）----------------
// 播放线程只认这一层。WASAPI / 共享 / Null / NAA（RTANet）都是它的实现，
// 这样"取块 → 写一个周期 → 暂停 → 跳转"那套逻辑不必为每个后端复制一遍。
// open 的参数收拢为 SinkOpenOpts：PlayParams 属于 rtaweb，接口层只消费其中
// 与设备相关的三个字段（mock sink 也无须知道完整播放参数）。
struct SinkOpenOpts {
    std::string device;      // WASAPI 端点 id / ASIO 驱动名或 CLSID（空 = 默认）
    std::string naaHost;     // NAA 端点 host:port
    int bufferMs = 100;      // 缓冲毫秒
};
struct OutSink {
    virtual ~OutSink() = default;
    virtual bool   open(const SinkOpenOpts& opts, uint32_t rate, int ch, int bits) = 0;
    virtual bool   start() = 0;
    virtual size_t buffer_frames() const = 0;
    virtual void   set_live_gain_db(double) {}
    // 返回 true = 真的等到了设备事件（超时/共享模式返回 false）。
    // 2026-10-24 C2：返回值只用于 STAT 的 ev/evint 遥测，调用方可以忽略（旧实现是 void）。
    virtual bool   wait_event(int) { return false; }
    virtual long long last_pad() const { return 0; }
    virtual size_t write_one_period(const int32_t* pcm, size_t frames) = 0;
    // WASAPI 用它把设备缓冲补满；其它后端默认退回 write_one_period。
    virtual size_t write_available(const int32_t* pcm, size_t frames) { return write_one_period(pcm, frames); }
    // 后端自己观测到的「实际写出峰值」（float 域，0..1）。默认 0 = 该后端不报。
    // 为什么需要它：管线侧的 peak 是位深域读数，一旦换算与实际写出不一致，
    // 内部所有判据都会显示正常（2026-10-23 共享路径 −48 dB 就是这么被藏住的）。
    virtual double out_peak() const { return 0.0; }
    virtual void   pause() {}
    virtual void   resume() {}
    virtual void   close() {}
    virtual const std::string& last_error() const = 0;
    // RENDERFAIL 遥测：最近一次 GetBuffer 的 HRESULT（-999 = 从未调过）。
    // 各后端按需覆盖；mock sink 可以返回固定值。
    virtual long long last_gb_hr() const { return -999; }
    // 「写不进去」的成因分解（2026-10-22 反馈 20261022-02）：w0 是个混在一起的数，
    // 分不清「设备缓冲恒满」和「我们喂不上」。默认返回 -1 = 后端不支持该统计。
    virtual long long zero_pad_full() const { return -1; }   // ① 设备缓冲满
    virtual long long zero_starved()  const { return -1; }   // ② 供料不足一个周期
    virtual std::string zero_reason() const { return "-"; }  // 最近一次成因

    // ---------------- C2 遥测（2026-10-24 N300 欠载；纯观测，全部带默认值）----------------
    // 为什么默认 -1 / "-" / false：Null / NAA / ASIO 等后端不参与这些统计，UTF-8 打印 "-1" 即
    // 「该后端不报」。渲染循环侧的 dry/dryms/ev/evint/prod 是 STAT 的权威来源（见 §6.3），
    // 这里的同名访问器留给「后端自己在更底层计数」的场景（当前 WASAPI 不覆盖）。
    virtual long long device_period_frames() const { return -1; }        // per：设备周期帧数
    virtual long long dry_events() const { return -1; }                  // dry
    virtual long long dry_ms() const { return -1; }                      // dryms
    virtual long long event_interval_median_ms() const { return -1; }    // evint
    virtual long long prod_stall_max_ms() const { return -1; }           // prod
    // 独占初始化配方（真实毫秒数，不是写死的 "100ms" 文案）与是否走了对齐重试。
    virtual std::string init_recipe() const { return "-"; }
    virtual bool        realigned() const { return false; }

    // ---------------- C3 端点消失（N300 96k 掉 USB）----------------
    // 默认 true：Null / NAA / ASIO 与 WASAPI 端点枚举无关，不参与该判据。
    // WASAPI 独占实现按「端点是否仍在 DEVICE_STATE_ACTIVE」回答（内部自带 ≥0.5s 缓存，
    // 因为端点消失时 EnumAudioEndpoints 可能要几毫秒）。
    virtual bool endpoint_alive() const { return true; }

    // ---------------- C2 写失败明细（WFAIL 前 200 条）----------------
    // pad/room/per/avail/toWrite 是 write_available 量化【之后】的现场数字，
    // gbHr 是最近一次 IAudioRenderClient::GetBuffer 的 HRESULT（-999 = 从未调过）。
    struct WriteAttempt {
        long long pad = -1, room = -1, per = -1, avail = -1, toWrite = -1, gbHr = -999;
    };
    // 返回 false = 该后端不报（WFAIL 行会省略这些字段）。
    virtual bool last_write_attempt(WriteAttempt& out) const { (void)out; return false; }
};

// ---------------- 生产者/消费者队列（原 play_one 局部 RenderQ，原样迁出）----------------
struct RenderQ {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::pair<rta::audio::AudioBuffer, uint64_t>> q;
    size_t cap = 8;
    bool done = false;
    bool error = false;
    std::string err;
    std::atomic<bool>     abort{false};
    std::atomic<uint64_t> epoch{1};        // 消费者每次跳转 +1
    std::atomic<long long> seekFrame{-1};  // 生产者看到就重建管线并从这里继续
    std::atomic<bool>     gpu{false};
};

// ---------------- 渲染循环的宿主状态 ----------------
// 指针全部指向 rtaweb 侧的真实状态（PlayerState / g_dbg* / 局部 RQ），
// 测试侧则指向 mock 状态 —— 循环本身两种场景逐字节同一份代码。
struct RenderHost {
    // ---- PlayerState 子集 ----
    std::atomic<bool>*      stop     = nullptr;   // 用户停止/切歌
    std::atomic<bool>*      pauseReq = nullptr;
    std::atomic<bool>*      paused   = nullptr;
    std::atomic<double>*    seekReq  = nullptr;   // 0..1 比例，-1 = 无请求
    std::atomic<size_t>*    played   = nullptr;
    std::atomic<double>*    volDb    = nullptr;
    std::atomic<double>*    startDb  = nullptr;
    std::atomic<long long>* dbgPad   = nullptr;
    bool*                   gpuOut   = nullptr;   // ps.gpu（普通 bool：只在渲染线程写）

    // ---- 队列 ----
    RenderQ* rq = nullptr;

    // ---- 本次播放的形状（原 play_one 局部量）----
    uint32_t deviceRate = 0;
    int      outBits = 24;
    int      outCh   = 2;
    uint32_t inRate  = 0;
    size_t   inFrames = 0;
    size_t   totalOut = 0;

    // ---- 诊断计数（原 g_dbg* 全局）----
    std::atomic<long long>* dbgW0    = nullptr;
    std::atomic<long long>* dbgWr    = nullptr;
    std::atomic<long long>* dbgPulls = nullptr;
    std::atomic<long long>* dbgClip  = nullptr;
    std::atomic<double>*    dbgPeak  = nullptr;

    // ---- 日志钩子 ----
    std::function<void(const std::string&)> log;    // play_log
    std::function<void(const std::string&)> dlogFn; // 运行日志（跳转等）

    // ---- RENDERFAIL 归因材料（由调用方填）----
    std::string deviceLabel;        // 设备友好名
    std::string deviceIdDesc;       // 端点 id（空 = 默认端点）
    bool fallbackEligible = false;  // 独占 RENDERFAIL 时是否允许自动回退共享
    bool dop = false;               // DoP/DSD 输出（不能回退共享）
    bool onShared = false;          // 当前已是共享后端（第二次尝试失败时的文案分支）

    // ---- 吞吐率判据（2026-10-22 用户真机反馈 20261022-02）----
    // 旧判据「连续 200 次写不进去」有个真实盲区：只要设备【偶尔】接受一次数据，
    // timeouts 就被清零，永远攒不到 200 ⟹ 永不归因、永不回退、也不报错，
    // 而听感是每 ~257ms 一次「啵」（实测 96k/100ms 缓冲：246 次尝试只成功 41 次，
    // w0 恰为 wr 的 5 倍，吞吐仅 0.39 —— 4.1s 音频 / 10.55s 墙钟；41 次成功 × 9600 帧
    // = 393600 帧 = 4.1s@96k，与 STAT 的 pos=4.1 吻合；10.55s / 41 = 257ms ≈ 3.9 次/秒。
    // 注意「83%」是【写入失败率】（205/246），不是吞吐 —— 失败率与吞吐率量纲不同、
    // 不可混用，这正是判据必须用「帧/秒 ÷ deviceRate」而不是「写入次数比」的原因。
    // 现场见 反馈和开发\debug\20261022-02）。
    // 这里补一条与「间歇性成功」无关的判据：把【写入帧数 / 应写帧数】按窗口累计，
    // 连续份额不足达 N 秒即判 STARVING。
    //
    // nowSec 抽成钩子只为让这种形态能用虚拟时钟确定性重放（铁律 3：测真东西；
    // 现实路径 nullptr → steady_clock，行为与抽出前一致）。
    std::function<double()> nowSec;      // nullptr = steady_clock
    double starveWindowSec = 1.0;        // 统计窗口长度（秒）
    double starveRatio     = 0.90;       // 窗口内 写入帧/应写帧 低于此值 ⟹ 本窗口「不足」
    double starveSecs      = 3.0;        // 连续不足累计达此秒数 ⟹ STARVING 归因
    bool   starveEnabled   = true;       // 供测试/特殊后端关闭

    // ---- C1 L2 起播看门狗（2026-10-24 N300 欠载 / 方案 §5.2）----
    // Start 之后 800 ms 内【手里已有数据】却一个字节都没写进去（MOTU 形态：Initialize
    // 接受但流不推进）⟹ 调 l2Degrade 重开 20 ms。返回 true = 降级并已重新 Start，
    // 循环会用 sink.buffer_frames() 重取 period 并重置统计窗口。nullptr = 不启用。
    std::function<bool()> l2Degrade;
    double l2DeadlineMs = 800.0;

    // ---- C2 遥测导出（可空：测试/其它后端可以不设；STAT 仍会打印循环侧计数）----
    std::atomic<long long>* dbgDry    = nullptr;   // dry 次数
    std::atomic<long long>* dbgDryMs  = nullptr;   // dry 墙钟累计 ms
    std::atomic<long long>* dbgEv     = nullptr;   // wait_event 命中次数
    std::atomic<long long>* dbgEvInt  = nullptr;   // 事件间隔中位数（最近 64 次）
    std::atomic<long long>* dbgProd   = nullptr;   // pull_chunk 单次最长阻塞 ms
    // STAT 输出周期（秒）。生产 = 5s；测试用虚拟时钟时可以调小以便断言 STAT 字段。
    double statPeriodSec = 5.0;

    // ---- C3 端点消失（B 缺陷：N300 96k 从 USB 总线掉线）----
    // 写失败期间每隔 deviceAliveCheckMs 询问一次 sink.endpoint_alive()；false ⟹
    // 如实报 DEVICE_LOST 并停止（不回退共享、不从头重播）。
    double deviceAliveCheckMs = 250.0;

    // ---- 结果 ----
    bool        renderFail = false;  // true = 设备持续不接受数据（RENDERFAIL break）
    bool        normalEnd  = false;  // true = eos 且队列耗尽，正常播完
    bool        deviceLost = false;  // true = C3 端点已消失（调用方不得回退共享/重播）
    std::string failMsg;             // renderFail 时为完整归因文本（调用方原样赋 ps.error）
    double      failThroughput = -1; // STARVING 时的实测吞吐份额（-1 = 未测/非此路径）
};

// 单调秒表。抽成函数是为了让「间歇性成功」这种形态能用虚拟时钟确定性重放
//（真实 WASAPI 上它要 3 秒以上才显形，测试里跑 3 秒纯属浪费且不确定）。
inline double render_now_sec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------- 渲染主循环 ----------------
// 从队列取块 → 20ms 轮询设备水位 → 补满设备缓冲。行为与抽出前逐行一致。
inline void render_loop(OutSink& sink, RenderHost& h) {
    auto& RQ = *h.rq;
    // period 从 const 改成可变：C1 的 L2 起播看门狗降级后重开 20ms，缓冲帧数会变，
    // 必须重取（旧实现只在起播时取一次）。
    size_t period = sink.buffer_frames() ? sink.buffer_frames() : 9600;
    std::vector<int32_t> pend;
    size_t pendFrames = 0;
    size_t played = 0;
    int timeouts = 0;
    // ---- 吞吐率判据（STARVING）状态 ----
    // 判据只看「帧/秒」与 deviceRate 的比值 —— 这个量纲与缓冲深度、周期量化、
    // 轮询节奏全都无关。反面教材（已算过，见测试注释）：用「写入次数/轮询次数」
    // 即 duty 做判据，健康独占设备因为一次写满整个缓冲，duty 本来就只有 ~20%，
    // 拿 90% 当门限会把【所有健康设备】判成饥饿 —— 判据必须选无量纲的那个。
    auto nowSec = [&]() { return h.nowSec ? h.nowSec() : render_now_sec(); };

    // ---------------- C2 遥测状态（2026-10-24 N300 欠载；纯观测）----------------
    // 全部是本地计数：不加锁、不分配（evRing 固定 64 槽），不改变写入/调度/DSP。
    const double loopStartT = nowSec();   // 起播时刻（dry 的 500ms 门槛、WFAIL 的 t= 都相对它）
    long long dryN      = 0;      // dry：设备缓冲空 + 手里有数据 + 我们一个字节也写不进去
    double    dryAccMs  = 0.0;    // 已结算的 dry 墙钟毫秒
    double    dryEnterT = 0.0;    // 当前 dry 片段起点
    bool      inDry     = false;
    long long evN       = 0;      // wait_event 命中次数
    double    evLastT   = -1.0;   // 上一次事件时刻（算事件间隔）
    double    evRing[64] = {0};
    int       evRingN = 0, evRingPos = 0;
    long long prodMaxMs = 0;      // pull_chunk 单次最长阻塞
    size_t    writtenTotal = 0;   // 已写入帧数（L2 判「零写入」用）
    long long wfailN    = 0;      // 写失败次数（WFAIL 明细只打前 200 条）
    double    lastAliveProbeT = loopStartT - 1.0;  // 首次写失败立即问一次端点存活
    bool      l2Tried   = false;  // L2 看门狗只尝试一次（避免无限重开）
    double    lastLogT  = loopStartT;
    double winStart   = nowSec();       // 当前统计窗口起点
    double lastTick   = winStart;       // 上一次累计的时刻（应写帧数按时段累加，不重复计）
    size_t winWritten = 0;              // 窗口内成功写入帧数
    double winShould  = 0.0;           // 窗口内「按 deviceRate 应写」帧数（累计，避免取整误差）
    double starveAcc  = 0.0;            // 连续「吞吐不足」累计秒数
    bool   winFirst   = true;           // 第一个窗口是起播斜坡，只作基线不判罚
    bool pausedNow = false;
    bool eos = false;
    bool padded = false;   // 末尾不足一个周期时只补一次静音（防共享模式无限尾音）
    uint64_t myEpoch = RQ.epoch.load();

    // ---- C2 小工具（只读/只累加本地计数）----
    auto flushDry = [&](double t) {          // 结算当前 dry 片段
        if (!inDry) return;
        dryAccMs += (t - dryEnterT) * 1000.0;
        inDry = false;
    };
    auto dryMsNow = [&](double t) -> long long {   // 累计 dry 毫秒（含未结算的当前片段）
        const double v = dryAccMs + (inDry ? (t - dryEnterT) * 1000.0 : 0.0);
        return (long long)(v + 0.5);
    };
    auto evIntMedian = [&]() -> long long {  // 事件间隔中位数（最近 64 次，固定槽位排序）
        if (evRingN <= 0) return -1;
        double tmp[64];
        for (int i = 0; i < evRingN; ++i) tmp[i] = evRing[i];
        for (int i = 1; i < evRingN; ++i) {
            const double v = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
            tmp[j + 1] = v;
        }
        return (long long)(tmp[evRingN / 2] + 0.5);
    };
    auto note_pull = [&](double sec) {       // pull_chunk 单次阻塞
        const long long ms = (long long)(sec * 1000.0 + 0.5);
        if (ms > prodMaxMs) prodMaxMs = ms;
    };
    auto publish = [&](double t) {           // 导出到主机原子量（可空）
        if (h.dbgDry)   h.dbgDry->store(dryN);
        if (h.dbgDryMs) h.dbgDryMs->store(dryMsNow(t));
        if (h.dbgEv)    h.dbgEv->store(evN);
        if (h.dbgEvInt) h.dbgEvInt->store(evIntMedian());
        if (h.dbgProd)  h.dbgProd->store(prodMaxMs);
    };

    // 从队列取一块并入待写缓冲；返回 false 表示暂时没有数据
    auto pull_chunk = [&](bool block) -> bool {
        const double pullT0 = nowSec();
        std::unique_lock<std::mutex> lk(RQ.mtx);
        if (block)
            RQ.cv.wait_for(lk, std::chrono::milliseconds(200),
                           [&] { return !RQ.q.empty() || RQ.done || RQ.abort.load(); });

        while (!RQ.q.empty()) {
            auto& f = RQ.q.front();
            if (f.second != myEpoch) { RQ.q.pop_front(); continue; }   // 跳转前的旧块
            rta::audio::AudioBuffer b = std::move(f.first);
            RQ.q.pop_front();
            lk.unlock();
            if (!b.raw.empty()) {
                pend.insert(pend.end(), b.raw.begin(), b.raw.end());
                pendFrames += b.frames();
                if (h.dbgPulls) h.dbgPulls->fetch_add(1);
            }
            RQ.cv.notify_all();
            if (block) note_pull(nowSec() - pullT0);
            return true;
        }
        if (RQ.done) eos = true;
        // ★ 丢弃旧块后必须通知生产者：否则它一直卡在"队列满"的 cv.wait 上，
        //   跳转就永远停在原地（本轮实测的真凶）。
        lk.unlock();
        RQ.cv.notify_all();
        if (block) note_pull(nowSec() - pullT0);
        return false;
    };

    while (!h.stop->load()) {
        // 响应暂停：Stop 设备（合法路径），恢复时 Start
        if (h.pauseReq->load() != pausedNow) {
            pausedNow = h.pauseReq->load();
            if (pausedNow) { sink.pause(); h.paused->store(true); }
            else {
                sink.resume(); h.paused->store(false); timeouts = 0;
                // 暂停期间设备不消耗数据，必须把吞吐窗口清零重来，否则恢复瞬间
                // 会被判成「设备不接受数据」（假的 STARVING）。
                winStart = nowSec(); lastTick = winStart;
                winWritten = 0; winShould = 0.0; starveAcc = 0.0; winFirst = true;
            }
        }
        if (pausedNow) { Sleep(30); continue; }

        // 响应跳转：Reset 清空设备缓冲，让生产者从新位置重建管线
        const double sk = h.seekReq->exchange(-1.0);
        if (sk >= 0.0) {
            if (h.dlogFn) h.dlogFn("seek " + std::to_string((int)(sk * 100)) + "%");
            // 不要调用 IAudioClient::Reset()：实测 Reset+Start 之后 GetCurrentPadding
            // 会一直停在满值，GetBuffer 永远拿不到空间，表现为"跳转后进度卡住"。
            // 代价是设备缓冲里最多 1 个周期（≈100 ms）的旧音频会先播完，可接受。
            myEpoch = RQ.epoch.fetch_add(1) + 1;
            RQ.seekFrame.store((long long)(sk * (double)h.inFrames));
            pend.clear(); pendFrames = 0; eos = false; padded = false;
            played = (size_t)(sk * (double)h.totalOut);
            h.played->store(played);
            timeouts = 0;
            // 跳转会清空待写缓冲，窗口必须重开（否则 0 写入被算成设备饥饿）
            winStart = nowSec(); lastTick = winStart;
            winWritten = 0; winShould = 0.0; starveAcc = 0.0; winFirst = true;
            continue;
        }
        if (RQ.gpu.load() && !*h.gpuOut) *h.gpuOut = true;

        // 实时音量：只把「当前请求 - 起播时已应用的」这个差值交给设备层，
        // 差值为 0 时完全走原路径（逐位不变）
        sink.set_live_gain_db(h.volDb->load() - h.startDb->load());

        // 先把待写缓冲填到至少一个周期（最多预攒 3 个周期，避免占内存）
        while (pendFrames < period * 3 && !eos) {
            if (!pull_chunk(pendFrames < period)) break;
        }
        // 全部取完且不足一个周期：补静音凑满最后一块，然后把剩下的播完就退出。
        // ★ 只补一次（2026-10-22 共享后端 e2e 抓到的无限尾音）：独占模式 pad==0 时一次
        //   就能把整个周期写走，pad 后 pendFrames 立即归零退出；共享模式每 20ms 只腾出
        //   ≈20ms 空间，pendFrames 落回 period 以下若再次补齐，就会每 tick 凭空多出一段
        //   静音、0.5 秒的文件永远播不完（实测 pos 冲到 200 秒还在写）。
        if (eos && pendFrames > 0 && pendFrames < period && !padded) {
            pend.resize(period * (size_t)h.outCh, 0);
            pendFrames = period;
            padded = true;
        }
        if (pendFrames == 0 && eos) { h.normalEnd = true; break; }

        // 事件只当"节奏提示"，不作为写入的前提。
        // 独占流一旦因为短暂没有数据而停发事件，纯事件驱动的写入循环就会永久卡死
        //（本轮实测：跳转后事件不再到来，进度永远停在原处）。
        // 改成"小睡 + 轮询 padding"：pad==0 就写一个周期，最坏多 20 ms 延迟，CPU 开销可忽略。
        {
            const double full = (double)((int64_t)1 << (h.outBits - 1));
            double pk = 0.0; long long cl = 0;
            for (size_t i = 0; i < pendFrames * (size_t)h.outCh; ++i) {
                const double a = std::fabs((double)pend[i]);
                if (a > pk) pk = a;
                if (a >= full - 0.5) ++cl;
            }
            if (h.dbgPeak) h.dbgPeak->store(pk / full);
            if (cl && h.dbgClip) h.dbgClip->fetch_add(cl);
            // STAT 的节奏用 nowSec()（生产 = steady_clock，与旧实现同一时钟；测试注入虚拟
            // 时钟时才能确定性地断言 STAT 里的 dry/evint 等字段）。
            const double nowT = nowSec();
            if (h.log && nowT - lastLogT >= h.statPeriodSec) {
                lastLogT = nowT;
                std::ostringstream ls;
                ls << "STAT pos=" << (double)played / (double)h.deviceRate
                   << " rate=" << h.deviceRate << " bits=" << h.outBits
                   << " pad=" << h.dbgPad->load()
                   << " peak=" << h.dbgPeak->load() << " clip=" << h.dbgClip->load()
                   // fpeak = 后端实际写出的峰值（float 域）。与 peak（位深域）并列，
                   // 两者不一致就说明换算/位深基准有问题（2026-10-23 共享电平定论用）。
                   << " fpeak=" << sink.out_peak()
                   << " w0=" << h.dbgW0->load() << " wr=" << h.dbgWr->load();
                // ★ 2026-10-22 反馈 20261022-02：w0 混着两种互斥成因，必须分开记。
                //   zp=设备缓冲满（pad 满）  zs=供料不足一个周期（我们喂不上）
                //   实测只要 zs>0 就说明瓶颈在【轮询粒度/供料】，不在设备。
                const long long zp = sink.zero_pad_full(), zs = sink.zero_starved();
                if (zp >= 0 || zs >= 0)
                    ls << " zp=" << zp << " zs=" << zs << " zr=" << sink.zero_reason();
                // ---- C2（2026-10-24 N300 欠载）：把「设备触底」数字化 ----
                // per = 设备周期帧数（-1 = 该后端不报）；bf = 本循环用的缓冲帧数；
                // dry/dryms = 轮询时设备缓冲已触底（pad==0）的次数与处于该状态的累计毫秒；
                // ev/evint = 事件次数 / 最近 64 次事件间隔中位数；prod = pull_chunk 最长阻塞。
                ls << " per=" << sink.device_period_frames()
                   << " bf=" << (long long)period
                   << " dry=" << dryN
                   << " dryms=" << dryMsNow(nowT)
                   << " ev=" << evN
                   << " evint=" << evIntMedian()
                   << " prod=" << prodMaxMs;
                h.log(ls.str());
                publish(nowT);
            }
        }
        const bool waited = sink.wait_event(20);
        const double evT = nowSec();
        if (waited) {
            ++evN;
            if (evLastT >= 0.0) {
                evRing[evRingPos] = (evT - evLastT) * 1000.0;
                evRingPos = (evRingPos + 1) % 64;
                if (evRingN < 64) ++evRingN;
            }
            evLastT = evT;
        }
        h.dbgPad->store(sink.last_pad());
        const size_t w = sink.write_available(pend.data(), pendFrames);
        const double tAfter = nowSec();
        if (w > 0) h.dbgWr->fetch_add(1); else h.dbgW0->fetch_add(1);

        // ---- C2 dry 计数（纯观测，方案 §6.3）----
        // 定义（与方案一致）：起播 500 ms 后，设备缓冲在本次写入前已经触底（pad==0）且
        // 手里有数据 ⟹ 计一次 dry。**不看这一拍是否写成功**：
        //   · 2 个周期的 20ms 缓冲（A 组现场）几乎每次轮询 pad 都是 0 —— 就算紧接着补上，
        //     设备也已经有了一段空窗，这正是听感「啵」的温床；
        //   · 10 个周期的 100ms 缓冲（B 组现场）只有真喂不上时才会触底（现场采样 0%）。
        // 所以 M2 才能用「10 分钟 dry=0」做判据。若改成「只在写失败时计数」，A 组形态
        // 反而会漏掉（pad 触底但补上了）—— 那是把判据改成「看不见」。
        // 只对能报告真实设备周期的后端计数（per<0 的 Null/NAA/共享后端没有 padding 概念）。
        const bool padTouched = (sink.device_period_frames() >= 0 &&
                                 sink.last_pad() == 0 && pendFrames > 0);
        if (padTouched) {
            if (tAfter - loopStartT >= 0.5) {
                ++dryN;
                if (!inDry) { inDry = true; dryEnterT = tAfter; }
            }
        } else if (inDry) {
            flushDry(tAfter);
        }

        // ---- C2 WFAIL 明细（只前 200 条；方案 §6.5）----
        // 只记「手里有数据却写不进去」；队列暂时空导致的 0 写入不是写失败。
        if (w == 0 && pendFrames > 0) {
            ++wfailN;
            if (wfailN <= 200 && h.log) {
                std::ostringstream ws;
                ws.setf(std::ios::fixed); ws.precision(3);
                ws << "WFAIL t=" << (tAfter - loopStartT) << " pad=" << sink.last_pad();
                OutSink::WriteAttempt wa;
                if (sink.last_write_attempt(wa))
                    ws << " room=" << wa.room << " per=" << wa.per << " avail=" << wa.avail
                       << " toWrite=" << wa.toWrite << " gb_hr=" << wa.gbHr;
                ws << " zr=" << sink.zero_reason();
                h.log(ws.str());
            }
        }

        if (w > 0) {
            pend.erase(pend.begin(), pend.begin() + (long long)(w * (size_t)h.outCh));
            pendFrames -= w;
            played += w;
            writtenTotal += w;
            h.played->store(played);
            timeouts = 0;
            winWritten += w;
        } else if (pendFrames >= period) {
            // ---- C3 端点消失（N300 96k 掉 USB）：如实报错，不静默欠载/回退 ----
            // 触发：写不进去且系统枚举里该端点已不再 ACTIVE。每 250ms 才问一次
            //（端点消失时 EnumAudioEndpoints 可能要几毫秒，不能每 tick 都问）。
            if (tAfter - lastAliveProbeT >= h.deviceAliveCheckMs / 1000.0) {
                lastAliveProbeT = tAfter;
                if (!sink.endpoint_alive()) {
                    std::ostringstream ds;
                    ds.setf(std::ios::fixed); ds.precision(3);
                    ds << "DEVICE_LOST device=\"" << h.deviceLabel << "\""
                       << " id=" << (h.deviceIdDesc.empty() ? "(默认)" : h.deviceIdDesc)
                       << " t=" << (tAfter - loopStartT) << "s"
                       << " w0=" << h.dbgW0->load() << " wr=" << h.dbgWr->load()
                       << " last_pad=" << sink.last_pad()
                       << " 缓冲=" << period << "帧";
                    if (h.log) h.log(ds.str());
                    std::ostringstream fs;
                    fs << "输出设备已断开（USB 掉线/端点已从系统枚举中消失）—— "
                          "设备不再是 ACTIVE 端点，请重新连接后在系统里确认设备恢复再重播。"
                          "已停止播放：不自动回退 WASAPI 共享，也不从头重播"
                          "（用户要 bit-perfect，静默降级比报错更有害）。"
                       << " device=\"" << h.deviceLabel << "\""
                       << " id=" << (h.deviceIdDesc.empty() ? "(默认)" : h.deviceIdDesc)
                       << " w0=" << h.dbgW0->load() << " wr=" << h.dbgWr->load()
                       << " last_pad=" << sink.last_pad()
                       << " 缓冲=" << period << "帧";
                    h.failMsg = fs.str();
                    h.deviceLost = true;
                    h.renderFail = true;
                    if (h.log) h.log(h.failMsg);
                    flushDry(tAfter);
                    publish(tAfter);
                    break;
                }
            }
            // 手里有数据却 4 秒写不进去 ⟹ 设备异常。2026-10-22 VM 事故根因：
            // 独占模式在 VM 板载 HD Audio 上 GetCurrentPadding 恒满 → write_available 恒 0。
            // 旧版在这里静默 break，END pos=0 error="" 成悬案；现在必须留下完整归因。
            if (++timeouts > 200) {
                std::ostringstream fs;
                fs << "RENDERFAIL 设备流 " << (timeouts * 20 / 1000.0) << " 秒未接受数据"
                   << "（疑似虚拟声卡/独占模式不可用）"
                   << " w0=" << h.dbgW0->load() << " wr=" << h.dbgWr->load();
                // 与 STAT 同一口径：把「写不进去」的成因一并带出（不支持计数的后端省略）。
                // w0 本身混着「缓冲满」与「喂不上一个周期」两种互斥成因，只看 w0 无法定案。
                const long long zpf = sink.zero_pad_full(), zst = sink.zero_starved();
                if (zpf >= 0 || zst >= 0)
                    fs << " zp=" << zpf << " zs=" << zst << " zr=" << sink.zero_reason();
                fs << " last_pad=" << sink.last_pad()
                   << " 缓冲=" << period << "帧"
                   << " GetBuffer HRESULT=" << sink.last_gb_hr()
                   << " device=\"" << h.deviceLabel << "\""
                   << " id=" << (h.deviceIdDesc.empty() ? "(默认)" : h.deviceIdDesc);
                if (h.fallbackEligible)
                    fs << "。设备独占流持续不接受数据,已自动回退 WASAPI 共享模式(非 bit-perfect)。";
                else if (h.dop)
                    fs << "。DoP/DSD 不能回退共享模式(共享会重采样破坏 DoP 载波),请改用独占可用的设备或 ASIO。";
                else if (h.onShared)
                    fs << "。共享模式也未接受数据(设备/系统混音异常),请检查音频设备状态,或在设置中改用 ASIO/独占。";
                else
                    fs << "。建议在设置中切换「WASAPI 共享」(兼容虚拟机/独占不可用,非 bit-perfect)或改用 ASIO。";
                h.failMsg = fs.str();
                h.renderFail = true;
                if (h.log) h.log(h.failMsg);
                break;
            }
        }

        // ---- C1 L2 起播看门狗（方案 §5.2）----
        // Start 后 800 ms 内手里已有数据却一个字节也没写进去（MOTU 形态：
        // Initialize 接受、流不推进）⟹ 重开 20 ms 再 Start。只尝试一次。
        // 这比等 4 秒 RENDERFAIL 快得多，而且不会触发「回退共享 + 从头重播」。
        if (h.l2Degrade && !l2Tried && writtenTotal == 0 && pendFrames >= period &&
            (tAfter - loopStartT) * 1000.0 >= h.l2DeadlineMs) {
            l2Tried = true;
            const size_t bfBefore = period;
            if (h.l2Degrade()) {
                const size_t np = sink.buffer_frames();
                if (np > 0) period = np;
                timeouts = 0;
                winStart = tAfter; lastTick = tAfter;
                winWritten = 0; winShould = 0.0; starveAcc = 0.0; winFirst = true;
                flushDry(tAfter);
                if (h.log) {
                    std::ostringstream ls;
                    ls << "BUFPOLICY l2_degraded bf_before=" << bfBefore
                       << " bf_now=" << period
                       << " 原因=Start 后 " << (long long)h.l2DeadlineMs << "ms 有数据但零写入";
                    h.log(ls.str());
                }
                continue;
            }
        }

        // ---- 吞吐率判据：抓「间歇性成功」导致的持续性饥饿 ----
        // 旧判据（上面那段 timeouts > 200）在设备【偶尔】接受一次数据时被清零，
        // 永远攒不到 200。现场 20261022-02 正是这个形态：96k/100ms 缓冲下
        // 246 次尝试只成功 41 次（w0 恰为 wr 的 5 倍），设备其实在放，但每 ~120ms
        // 就断 20ms，用户听感「边播边啵啵啵」，而日志里既没有 RENDERFAIL 也没有 error。
        //
        // 判据用「帧/秒 ÷ deviceRate」而不是「写入次数/轮询次数」：
        // 健康独占设备一次写满整个缓冲，duty 本来就只有 ~20%（20ms 轮询 / 100ms 缓冲），
        // 拿 duty 当门限会把所有正常设备判死。吞吐率则恒等于 1.0，与缓冲深度无关。
        if (h.starveEnabled && h.deviceRate > 0) {
            const double t = nowSec();
            // 应写帧数按【增量】累加：lastTick→t 这一段设备理应消耗 deviceRate×(t-lastTick) 帧。
            // 不能用 (t - winStart) 累加 —— 那会把窗口内每一段重复计一遍。
            const double dt = t - lastTick;
            if (dt > 0.0) { winShould += (double)h.deviceRate * dt; lastTick = t; }
            const double span = t - winStart;          // 窗口已经过去多久（只用于判定窗口是否已满）
            if (span >= h.starveWindowSec) {
                const double got = winShould > 0.0 ? (double)winWritten / winShould : 1.0;
                // 第一个窗口是起播斜坡（设备缓冲从空到满，写入本来就会少于 deviceRate），
                // 只用来建立基线，不判罚 —— 否则每次起播都可能误报。
                if (winFirst) {
                    winFirst = false;
                    starveAcc = 0.0;
                } else if (got < h.starveRatio) {
                    starveAcc += span;
                    if (starveAcc >= h.starveSecs) {
                        std::ostringstream ss;
                        ss.setf(std::ios::fixed); ss.precision(3);
                        ss << "STARVING 设备吞吐持续不足 " << starveAcc << " 秒"
                           << "（实测 " << got << " × 设备率，判据 < " << h.starveRatio
                           << " 达 " << h.starveSecs << " 秒）"
                           << " —— 设备在放但每个周期都断，听感为周期性「啵」声/破音"
                           << " w0=" << h.dbgW0->load() << " wr=" << h.dbgWr->load()
                           << " zp=" << sink.zero_pad_full() << " zs=" << sink.zero_starved()
                           << " last_pad=" << sink.last_pad()
                           << " 缓冲=" << period << "帧"
                           << " GetBuffer HRESULT=" << sink.last_gb_hr()
                           << " device=\"" << h.deviceLabel << "\""
                           << " id=" << (h.deviceIdDesc.empty() ? "(默认)" : h.deviceIdDesc);
                        // 与 RENDERFAIL 的区别写清楚：这条不是「流死了」，而是「流活着但喂不饱」，
                        // 所以处置建议不同 —— 优先怀疑缓冲/周期配置与实时优先级，而非设备不可用。
                        ss << "。设备流仍在工作但供给不足："
                              "请把设置里的缓冲调大（如 100ms→200ms），关闭其它独占该设备的程序，"
                              "或在设置中切换 WASAPI 共享 / ASIO 以绕开独占周期约束。";
                        h.failMsg = ss.str();
                        h.failThroughput = got;
                        h.renderFail = true;
                        if (h.log) h.log(h.failMsg);
                        break;
                    }
                } else {
                    starveAcc = 0.0;   // 恢复了：连续计数清零（但已判罚的窗口不会回滚）
                }
                winStart = t; lastTick = t; winWritten = 0; winShould = 0.0;
            }
        }
    }

    // 退出时结算最后一个 dry 片段并把最终遥测导出给主机（/api/status 用）。
    {
        const double tEnd = nowSec();
        flushDry(tEnd);
        publish(tEnd);
    }
}

} // namespace rta::render
