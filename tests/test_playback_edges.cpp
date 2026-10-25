// test_playback_edges.cpp — 音频回放链路的边界/数值/并发回归（2026-10-25 深度 DEBUG 路线）
//
// 被测对象是【生产代码】，不是拷贝（铁律 3）：
//   · src/engine/pipeline.h       —— 声道路由 / 延迟线 / 量化
//   · src/audio/wav.h             —— 容器取值范围 / WAV 头校验
//   · src/audio/render_loop.h     —— RENDERFAIL 归因文案里的「秒数」
// mock sink 只替换设备 I/O，测试【绝不打开音频设备】（铁律 1）。
//
// 每个用例都先复现（打印实测数字），再改代码；判据全是统计量/逐样本，不依赖听感。
#include "audio/render_loop.h"
#include "audio/wav.h"
#include "engine/pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

using rta::audio::AudioBuffer;

// ---------------- 1. 单声道 → 多声道：必须复制，不许「单边」----------------
// 规格出处：src/audio/wasapi_capture.h:535-538 的注释（同一项目对同一件事的既定语义）
//   「唯一需要显式展开的是【单声道 → 多声道复制】…不能因为走 DSP 就变单边」
// 但那条注释说的是监听链；播放链 Pipeline 的单位路由只填 c < min(in,out)，
// 于是 outCh > 1 时右声道恒为数字静音。
// 触发路径（生产可达，不需要用户改任何设置）：
//   tools/rtaweb.cpp:2802 共享后端 pEff.outChannels = mixCh（本机 6 个 ACTIVE 端点全是 2ch）
//   tools/rtaweb.cpp:2909 outCh = pEff.outChannels > 0 ? ... : in.channels
//   tools/rtaweb.cpp:2910 outCh != in.channels ⟹ cfg.outChannels = outCh
static AudioBuffer make_mono(int bits, size_t n) {
    AudioBuffer b;
    b.rate = 48000; b.channels = 1; b.bits = bits; b.isFloat = false;
    b.raw.resize(n);
    const double fs = (double)(1u << (bits - 1));
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / 48000.0;
        b.raw[i] = (int32_t)std::lround(0.25 * fs * std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
    }
    return b;
}

static void test_mono_upmix() {
    std::printf("[1] 单声道源 → 输出 2 声道（共享后端强制 mixCh=2 的形态）\n");
    for (int bits : {16, 24}) {
        rta::engine::PipelineConfig cfg;
        cfg.shaper = rta::dsp::Shaper::None;
        cfg.outChannels = 2;                 // ← rtaweb 在共享后端下强制做的事
        rta::engine::Pipeline p(cfg);
        auto in = make_mono(bits, 512);
        auto out = p.process(in);
        ck(out.channels == 2, "输出声道数 = 2", "实际 " + std::to_string(out.channels));

        bool left_ok = true, right_eq_left = true;
        size_t diff = 0, silent = 0;
        for (size_t n = 0; n < in.frames(); ++n) {
            const int32_t l = out.raw[n * 2 + 0], r = out.raw[n * 2 + 1];
            if (l != in.raw[n]) left_ok = false;
            if (r != l) { right_eq_left = false; ++diff; }
            if (r == 0) ++silent;
        }
        std::printf("      bits=%d: 右声道与左声道不同的样本 %zu/%zu，右声道为 0 的样本 %zu/%zu\n",
                    bits, diff, in.frames(), silent, in.frames());
        ck(left_ok, "左声道 = 源（逐样本）");
        ck(right_eq_left, "右声道 = 源（逐样本，单声道必须复制到两边）",
           "不同样本 " + std::to_string(diff) + "/" + std::to_string(in.frames()));
    }

    // 既有语义不能回退：2ch 源 → 6ch 输出仍然是「前两路逐位 + 其余静音」
    //（tests/test_channel_gain.cpp 第 3 节钉住的行为；这里加第二处锚点）
    {
        rta::engine::PipelineConfig cfg;
        cfg.shaper = rta::dsp::Shaper::None;
        cfg.outChannels = 6;
        rta::engine::Pipeline p(cfg);
        AudioBuffer in;
        in.rate = 48000; in.channels = 2; in.bits = 24; in.isFloat = false;
        in.raw.assign(256 * 2, 12345);
        auto out = p.process(in);
        bool front = true, rest_zero = true;
        for (size_t n = 0; n < 256; ++n) {
            for (int c = 0; c < 2; ++c) if (out.raw[n * 6 + (size_t)c] != in.raw[n * 2 + (size_t)c]) front = false;
            for (int c = 2; c < 6; ++c) if (out.raw[n * 6 + (size_t)c] != 0) rest_zero = false;
        }
        ck(front && rest_zero, "2ch → 6ch 仍是「前两路逐位 + 其余静音」（不回退既有语义）");
    }
}

// ---------------- 2. 非有限值（NaN/±Inf）：不许变成满幅直流 ----------------
// 容器不变式：bits=16 的 raw 必须落在 [-32768, 32767]，bits=24 必须落在 [-8388608, 8388607]。
// 现有行为：floor(NaN+0.5)=NaN，两处钳位比较对 NaN 全为 false ⟹ (int32_t)NaN（UB，
// x64 上得 INT_MIN）直接落进 raw ⟹ 16-bit 容器里出现 -2147483648（越界 65536 倍）。
// 下游 wasapi_out.h:191-199 / wasapi_shared.h:243-254 都有「非有限 → 0」的守卫，
// 但那已经救不回来了：它们拿到的是 -1.0（满幅负直流）而不是 NaN。
static void test_non_finite() {
    std::printf("[2] NaN / ±Inf / 越界有限值 → 容器取值范围\n");
    const double nan = std::nan("");
    for (int bits : {16, 24, 32}) {
        const double lo = -(double)(1u << (bits - 1));
        const double hi = (double)((1u << (bits - 1)) - 1u);
        for (int k = 0; k < 4; ++k) {
            const double x = (k == 0) ? nan : (k == 1 ? HUGE_VAL : (k == 2 ? -HUGE_VAL : -1.5));
            AudioBuffer b;
            b.rate = 48000; b.channels = 1; b.bits = bits; b.isFloat = false;
            b.raw.assign(1, 0);
            b.setSample(0, 0, x);
            const double v = (double)b.raw[0];
            char what[128];
            std::snprintf(what, sizeof(what), "bits=%d %s → 落在容器域内", bits,
                          k == 0 ? "NaN" : (k == 1 ? "+Inf" : (k == 2 ? "-Inf" : "-1.5")));
            ck(v >= lo && v <= hi, what,
               "实际 raw=" + std::to_string(b.raw[0]) + "（域 [" + std::to_string((long long)lo) +
               "," + std::to_string((long long)hi) + "]）");
            if (k <= 2) {
                char what0[128];
                std::snprintf(what0, sizeof(what0), "bits=%d %s → 0（非有限值按静音处理，与两个后端一致）",
                              bits, k == 0 ? "NaN" : (k == 1 ? "+Inf" : "-Inf"));
                ck(b.raw[0] == 0, what0, "实际 raw=" + std::to_string(b.raw[0]));
            } else {
                char what1[128];
                std::snprintf(what1, sizeof(what1), "bits=%d -1.5 → 负满幅（有限越界只钳位，不是静音）", bits);
                ck(b.raw[0] == (int32_t)lo, what1, "实际 raw=" + std::to_string(b.raw[0]));
            }
        }
    }
    // float 容器：非有限值同样不能留在缓冲里（下游会当 ±1.0 铺满）
    {
        AudioBuffer b;
        b.rate = 48000; b.channels = 1; b.bits = 32; b.isFloat = true;
        b.raw.assign(1, 0);
        b.setSample(0, 0, nan);
        ck(std::isfinite(b.sample(0, 0)), "float 容器 NaN → 有限值",
           "实际 " + std::to_string(b.sample(0, 0)));
    }
    // 满幅钳位不许被顺手改坏
    {
        AudioBuffer b;
        b.rate = 48000; b.channels = 1; b.bits = 16; b.isFloat = false;
        b.raw.assign(3, 0);
        b.setSample(0, 0, 1.0); b.setSample(1, 0, -1.0); b.setSample(2, 0, 0.5);
        ck(b.raw[0] == 32767 && b.raw[1] == -32768 && b.raw[2] == 16384, "16-bit 满幅/半幅仍逐码值精确",
           std::to_string(b.raw[0]) + "," + std::to_string(b.raw[1]) + "," + std::to_string(b.raw[2]));
    }
}

// ---------------- 2b. float WAV 里的 NaN/Inf 走完整管线 ----------------
static void test_float_wav_non_finite() {
    std::printf("[2b] float32 WAV（含 NaN/±Inf）→ read_wav → Pipeline\n");
    const char* path = "test_playback_edges_nan.wav";
    {
        AudioBuffer src;
        src.rate = 48000; src.channels = 2; src.bits = 32; src.isFloat = true;
        src.raw.assign(64 * 2, 0);
        for (size_t n = 0; n < 64; ++n) {
            src.setSample(n, 0, 0.1);
            src.setSample(n, 1, 0.1);
        }
        src.setSample(10, 0, std::nan(""));
        src.setSample(20, 0, HUGE_VAL);
        src.setSample(30, 0, -HUGE_VAL);
        src.setSample(40, 0, -1.5);
        rta::audio::write_wav(path, src);
    }
    AudioBuffer in = rta::audio::read_wav(path);
    ck(in.isFloat && in.bits == 32, "float32 WAV 读回仍是 32f",
       std::to_string(in.bits) + "f=" + std::to_string((int)in.isFloat));
    rta::engine::PipelineConfig cfg;
    cfg.shaper = rta::dsp::Shaper::None;      // 未开抖动（默认 path）
    rta::engine::Pipeline p(cfg);
    auto out = p.process(in);
    const int32_t v10 = out.raw[10 * 2 + 0], v20 = out.raw[20 * 2 + 0];
    const int32_t v30 = out.raw[30 * 2 + 0], v40 = out.raw[40 * 2 + 0];
    std::printf("      输出码值: NaN→%d  +Inf→%d  -Inf→%d  -1.5→%d\n", v10, v20, v30, v40);
    ck(v10 == 0, "NaN 样本 → 0（不是满幅直流）", "实际 " + std::to_string(v10));
    ck(v20 == 0 && v30 == 0, "±Inf 样本 → 0", std::to_string(v20) + "," + std::to_string(v30));
    ck(v40 == INT32_MIN, "-1.5 仍钳到 int32 负满幅（有限越界不受影响）", std::to_string(v40));
    std::remove(path);
}

// ---------------- 3. 每声道延迟：状态必须跨块保持 ----------------
// pipeline.h 每个 process() 都新建一条 dsp::DelayLine 并 set_samples，
// 于是每一块都「先补 N 个零、再丢掉本块最后 N 个样本」。判据：out1++out2 必须等于
// 把 in1++in2 延迟 N 个样本（逐样本），既不许丢样，也不许块边界跳变。
// ⚠️ 现状：tools/rtaweb.cpp 从不设置 delaySamples（grep 只有 pipeline.h 自己命中），
//    所以这是【潜伏缺陷】：一旦把 delay 接到设置页，每次分块都会丢 64 个样本 + 一个断点。
static void test_delay_persistence() {
    std::printf("[3] 每声道延迟跨块连续性（delaySamples=64，两块 256）\n");
    rta::engine::PipelineConfig cfg;
    cfg.shaper = rta::dsp::Shaper::None;
    cfg.channel.resize(1);
    cfg.channel[0].delaySamples = 64;
    rta::engine::Pipeline p(cfg);

    auto make = [](size_t off, size_t n) {
        AudioBuffer b;
        b.rate = 48000; b.channels = 1; b.bits = 24; b.isFloat = false;
        b.raw.resize(n);
        for (size_t i = 0; i < n; ++i) b.raw[i] = (int32_t)(1000 + off + i);   // 单调斜坡（唯一，损失可数）
        return b;
    };
    const size_t N = 256, D = 64;
    auto in1 = make(0, N), in2 = make(N, N);
    auto out1 = p.process(in1);
    auto out2 = p.process(in2);

    std::vector<int32_t> cat(out1.raw.begin(), out1.raw.end());
    cat.insert(cat.end(), out2.raw.begin(), out2.raw.end());
    std::vector<int32_t> src(in1.raw.begin(), in1.raw.end());
    src.insert(src.end(), in2.raw.begin(), in2.raw.end());

    size_t bad = 0, lost = 0;
    for (size_t i = 0; i < cat.size(); ++i) {
        const int32_t want = (i < D) ? 0 : src[i - D];
        if (cat[i] != want) { ++bad; if (cat[i] == 0 && want != 0) ++lost; }
    }
    std::printf("      与「纯 64 样本延迟」不一致的样本 %zu/%zu（其中应为非零却输出 0 的 %zu 个）\n",
                bad, cat.size(), lost);
    std::printf("      块边界：cat[255]=%d(期望 %d) cat[256]=%d(期望 %d)\n",
                cat[N - 1], (int)src[N - 1 - D], cat[N], (int)src[N - D]);
    ck(bad == 0, "两块拼接后逐样本等于纯 64 样本延迟（无丢样、无断点）",
       "不一致 " + std::to_string(bad) + "/" + std::to_string(cat.size()));
}

// ---------------- 4. WAV 头：采样率为 0 必须拒绝 ----------------
// read_wav / parse_wav_header 都不校验 rate。rate=0 的文件会被当成合法音频：
// 时长 = frames/0 = inf、render_loop 的 STAT pos = played/deviceRate = inf，
// 而错误信息只剩一句「打开输出后端失败」。判据：read_wav 抛异常且文案提到采样率。
static void test_wav_rate_zero() {
    std::printf("[4] WAV 声明采样率 0\n");
    const char* path = "test_playback_edges_rate0.wav";
    std::vector<uint8_t> o;
    auto w32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((uint8_t)((v >> (8 * i)) & 0xFF)); };
    auto w16 = [&](uint16_t v) { o.push_back((uint8_t)(v & 0xFF)); o.push_back((uint8_t)((v >> 8) & 0xFF)); };
    o.insert(o.end(), {'R','I','F','F'}); w32(36 + 16 * 2);
    o.insert(o.end(), {'W','A','V','E'});
    o.insert(o.end(), {'f','m','t',' '}); w32(16); w16(1); w16(1);
    w32(0);                 // ← nSamplesPerSec = 0
    w32(0); w16(2); w16(16);
    o.insert(o.end(), {'d','a','t','a'}); w32(16 * 2);
    for (int i = 0; i < 16; ++i) w16((uint16_t)(i * 1000));
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fwrite(o.data(), 1, o.size(), f); std::fclose(f); }

    // parse_wav_header 的契约：>0 成功 / 0 需要更多数据 / <0 格式非法
    rta::audio::WavHeaderInfo hdr{};
    std::string err;
    const int r = rta::audio::parse_wav_header(o.data(), o.size(), hdr, &err);
    std::printf("      parse_wav_header(rate=0) → %d（rate 回报 %u）%s\n", r, hdr.rate,
                err.empty() ? "" : ("  err=" + err).c_str());
    ck(r < 0, "parse_wav_header 把 rate=0 判成格式非法（当前返回 >0）", "返回 " + std::to_string(r));

    bool threw = false;
    std::string msg;
    try {
        AudioBuffer ab = rta::audio::read_wav(path);
        msg = "未抛异常，读到 rate=" + std::to_string(ab.rate) + " frames=" +
              std::to_string(ab.frames()) + "，时长=" + std::to_string((double)ab.frames() / (double)ab.rate);
    } catch (const std::exception& e) { threw = true; msg = e.what(); }
    std::printf("      read_wav → %s\n", msg.c_str());
    ck(threw, "read_wav 拒绝 rate=0（当前静默接受 → 时长/进度 inf）", msg);
    ck(msg.find("采样率") != std::string::npos, "报错文案点名采样率", msg);
    std::remove(path);
}

// ---------------- 5. RENDERFAIL 文案里的「秒数」必须等于实测墙钟 ----------------
// render_loop.h 用 timeouts*20/1000 拼这句文案。20ms 是【旧时序】的轮询上限；
// 现默认 fastFeed=true，等待上限改成 per/2（96k/per=960 ⟹ 5ms），于是同一句文案
// 把 1 秒说成 4 秒 —— 归因证据本身就错了（铁律 3 的反面：证据与事实脱钩）。
struct DeadFullSink : rta::render::OutSink {
    explicit DeadFullSink(double* c) : clk(c) {}
    bool open(const rta::render::SinkOpenOpts&, uint32_t, int, int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return (size_t)bf; }
    long long device_period_frames() const override { return per; }
    bool wait_event(int ms) override { *clk += (double)ms / 1000.0; lastWaitMs = ms; return true; }
    size_t write_one_period(const int32_t*, size_t) override { return 0; }
    size_t write_available(const int32_t*, size_t) override { return 0; }
    long long last_pad() const override { return bf; }        // 缓冲恒满 ⟹ 写不进去
    const std::string& last_error() const override { return e; }
    double* clk;
    long long bf = 9600, per = 960;
    int lastWaitMs = 0;
    std::string e;
};

static void run_renderfail(bool fastFeed, double* elapsed, std::string* msg, int* waitMs) {
    std::atomic<bool> stop{false}, pauseReq{false}, paused{false};
    std::atomic<double> seekReq{-1.0};
    std::atomic<size_t> played{0};
    std::atomic<double> volDb{0.0}, startDb{0.0};
    std::atomic<long long> dbgPad{0}, w0{0}, wr{0}, pulls{0}, clip{0};
    std::atomic<double> peak{0.0};
    bool gpu = false;
    rta::render::RenderQ rq;
    std::vector<std::string> logs;
    double clock = 0.0;

    for (int i = 0; i < 8; ++i) {           // 预填队列（容量 8 块，块 4096 帧）
        AudioBuffer b;
        b.rate = 96000; b.channels = 2; b.bits = 24;
        b.raw.assign(4096 * 2, 0);
        rq.q.emplace_back(std::move(b), rq.epoch.load());
    }
    rq.done = true;

    rta::render::RenderHost h;
    h.stop = &stop; h.pauseReq = &pauseReq; h.paused = &paused;
    h.seekReq = &seekReq; h.played = &played;
    h.volDb = &volDb; h.startDb = &startDb; h.dbgPad = &dbgPad; h.gpuOut = &gpu;
    h.rq = &rq;
    h.nowSec = [&]() { return clock; };
    h.deviceRate = 96000; h.outBits = 24; h.outCh = 2;
    h.inRate = 96000; h.inFrames = 1000000; h.totalOut = 1000000;
    h.dbgW0 = &w0; h.dbgWr = &wr; h.dbgPulls = &pulls; h.dbgClip = &clip; h.dbgPeak = &peak;
    h.log = [&](const std::string& s) { logs.push_back(s); };
    h.deviceLabel = "扬声器 (mock)";
    h.fallbackEligible = false; h.dop = false; h.onShared = false;
    h.starveEnabled = false;         // 本用例只走「连续 200 次」那条归因
    h.deviceGuardEnabled = false;    // 关掉开播自检/熔断，避免先被 hang 分支截走
    h.fastFeed = fastFeed;
    h.statPeriodSec = 1e9;           // 不打 STAT

    DeadFullSink sink(&clock);
    rta::render::render_loop(sink, h);
    *elapsed = clock;
    *waitMs = sink.lastWaitMs;
    *msg = h.failMsg;
}

static void test_renderfail_duration() {
    std::printf("[5] RENDERFAIL 文案里的「N 秒未接受数据」\n");
    for (int k = 0; k < 2; ++k) {
        const bool fast = (k == 0);
        double elapsed = 0.0; std::string msg; int waitMs = 0;
        run_renderfail(fast, &elapsed, &msg, &waitMs);
        double claimed = -1.0;
        const std::string key = "设备流 ";
        const size_t p = msg.find(key);
        if (p != std::string::npos) claimed = std::atof(msg.c_str() + p + key.size());
        std::printf("      fastFeed=%d 等待上限=%dms 实测墙钟=%.3fs 文案声称=%.3fs 误差=%+.1f%%\n",
                    (int)fast, waitMs, elapsed, claimed,
                    claimed > 0 ? 100.0 * (claimed - elapsed) / elapsed : -1.0);
        ck(claimed > 0, "文案里有秒数", msg);
        const double tol = std::max(0.05, 0.10 * elapsed);
        ck(std::fabs(claimed - elapsed) <= tol,
           fast ? "fastFeed 时序下声称时长 == 实测墙钟（±10%）"
                : "旧时序下声称时长 == 实测墙钟（±10%）",
           "声称 " + std::to_string(claimed) + "s vs 实测 " + std::to_string(elapsed) + "s");
    }
}

// ---------------- 6. 数字静音必须真的全零（10 分钟长跑，重采样 + 音量）----------------
// 判据：0 dB 音量、无抖动（Shaper::None）、44.1k->48k 升频下，10 分钟【全零】输入
// 的输出必须逐样本 == 0、peak == 0。任何一个非零样本都说明链路里存在直流累积
// （重采样器相位偏移、音量渐变残留、量化四舍五入偏离零），长跑会把它放大成可闻直流。
static void test_silence_all_zero() {
    std::printf("[6] 10 分钟数字静音 → 输出逐样本全零（44.1k→48k + 音量 0 dB）\n");
    rta::engine::PipelineConfig cfg;
    cfg.outRate = 48000;
    cfg.taps = 8192;                 // 缩短设计时间，判据不变（0*x 恒 0）
    cfg.twoStage = true;
    cfg.shaper = rta::dsp::Shaper::None;
    cfg.volumeEnabled = true;
    cfg.volumeDb = 0.0;
    rta::engine::Pipeline p(cfg);

    const size_t kChunk = 4096;
    const size_t total = (size_t)(44100.0 * 600.0);      // 10 分钟 @44.1k
    size_t done = 0, nonZero = 0, framesOut = 0;
    int32_t maxAbs = 0;
    while (done < total) {
        const size_t n = std::min(kChunk, total - done);
        AudioBuffer in;
        in.rate = 44100; in.channels = 2; in.bits = 24; in.isFloat = false;
        in.raw.assign(n * 2, 0);
        auto out = p.process(in);
        framesOut += out.frames();
        for (int32_t v : out.raw) {
            if (v != 0) ++nonZero;
            if (std::abs(v) > maxAbs) maxAbs = std::abs(v);
        }
        done += n;
    }
    std::printf("      输入 %zu 帧 → 输出 %zu 帧；非零样本 %zu 个，最大绝对值 %d\n",
                done, framesOut, nonZero, maxAbs);
    ck(nonZero == 0, "静音块逐样本全零（无直流累积/无不收敛）",
       "非零 " + std::to_string(nonZero) + "，maxAbs=" + std::to_string(maxAbs));
    ck(framesOut > (size_t)(48000.0 * 590.0), "输出帧数与 600s@48k 同量级（比例正确）",
       std::to_string(framesOut));
}

// ---------------- 7. 单声道 → 6 声道（多声道混音端点的形态）----------------
static void test_mono_to_six() {
    std::printf("[7] 单声道源 → 6 声道输出（本机 VB-Matrix VAIO 6ch 混音端点形态）\n");
    rta::engine::PipelineConfig cfg;
    cfg.shaper = rta::dsp::Shaper::None;
    cfg.outChannels = 6;
    rta::engine::Pipeline p(cfg);
    auto in = make_mono(24, 128);
    auto out = p.process(in);
    size_t bad = 0;
    for (size_t n = 0; n < in.frames(); ++n)
        for (int c = 0; c < 6; ++c)
            if (out.raw[n * 6 + (size_t)c] != in.raw[n]) ++bad;
    std::printf("      6 路里与源不同的样本 %zu/%zu\n", bad, in.frames() * 6);
    ck(bad == 0, "单声道复制到全部 6 路", "不同 " + std::to_string(bad));
}

// ---------------- 8. 跳转必须清空延迟线历史 ----------------
static void test_delay_seek_reset() {
    std::printf("[8] seek_reset 后延迟线历史必须清空（不许把上一位置的尾巴带过去）\n");
    rta::engine::PipelineConfig cfg;
    cfg.shaper = rta::dsp::Shaper::None;
    cfg.channel.resize(1);
    cfg.channel[0].delaySamples = 32;
    rta::engine::Pipeline p(cfg);
    auto make = [](int32_t v, size_t n) {
        AudioBuffer b;
        b.rate = 48000; b.channels = 1; b.bits = 24; b.isFloat = false;
        b.raw.assign(n, v);
        return b;
    };
    auto a = p.process(make(5000, 128));      // 把 5000 灌满延迟线
    p.seek_reset();                           // 跳转（生产者侧就是这一步）
    auto b = p.process(make(700, 128));
    size_t leaked = 0, head_zero = 0;
    for (size_t i = 0; i < 32 && i < b.raw.size(); ++i) { if (b.raw[i] != 0) ++leaked; else ++head_zero; }
    std::printf("      跳转后前 32 个样本：非零 %zu 个 / 零 %zu 个（a 首样本=%d）\n",
                leaked, head_zero, (int)a.raw[0]);
    ck(leaked == 0, "跳转后先输出 32 个零（新位置的前 32 帧还没进来），不泄漏旧位置",
       "非零 " + std::to_string(leaked));
    ck(b.raw[40] == 700, "之后恢复为新位置的内容", std::to_string(b.raw[40]));
}

// ---------------- 9. 非 44.1/48 家族的采样率 + 单声道组合 ----------------
// 判据（与听感无关）：1 秒输入经管线升/降频后，输出帧数必须落在 期望值 ±0.1% 内，
// 且输出不得出现非有限值留下的容器越界码值。覆盖 9.6k / 11.025k / 192k / 352.8k。
static void test_odd_rates() {
    std::printf("[9] 非 44.1/48 家族采样率 → 48k（帧数比例 + 容器域）\n");
    struct Case { uint32_t inRate; int inCh; };
    for (Case c : { Case{9600, 1}, Case{11025, 2}, Case{192000, 2}, Case{352800, 2} }) {
        rta::engine::PipelineConfig cfg;
        cfg.outRate = 48000;
        cfg.outChannels = 2;
        cfg.taps = 8192;
        cfg.shaper = rta::dsp::Shaper::None;
        rta::engine::Pipeline p(cfg);
        const size_t n = (size_t)c.inRate;              // 1 秒
        AudioBuffer in;
        in.rate = c.inRate; in.channels = c.inCh; in.bits = 24; in.isFloat = false;
        in.raw.assign(n * (size_t)c.inCh, 0);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / (double)c.inRate;
            in.raw[i * (size_t)c.inCh] = (int32_t)std::lround(0.5 * 8388608.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
        }
        auto out = p.process(in);
        const double expect = 48000.0;
        const double got = (double)out.frames();
        const double errPct = 100.0 * (got - expect) / expect;
        bool inDomain = true; size_t bad = 0;
        for (int32_t v : out.raw) if (v < -8388608 || v > 8388607) { inDomain = false; ++bad; }
        std::printf("      %6u Hz/%dch → 48000 Hz：输出 %zu 帧（期望 48000，偏差 %+.3f%%）\n",
                    c.inRate, c.inCh, out.frames(), errPct);
        ck(std::fabs(errPct) <= 0.1, "输出帧数与 1 秒等时（±0.1%）",
           std::to_string(got) + " 帧，偏差 " + std::to_string(errPct) + "%");
        ck(inDomain, "24-bit 容器域内（无越界码值）", "越界 " + std::to_string(bad));
        ck(out.channels == 2 && out.rate == 48000, "输出格式 = 48k/2ch");
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== playback_edges：单声道上混 / 非有限值 / 延迟线 / WAV 头 / 归因时长 ==\n");
    test_mono_upmix();
    test_non_finite();
    test_float_wav_non_finite();
    test_delay_persistence();
    test_wav_rate_zero();
    test_renderfail_duration();
    test_silence_all_zero();
    test_mono_to_six();
    test_delay_seek_reset();
    test_odd_rates();
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "playback_edges 全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
