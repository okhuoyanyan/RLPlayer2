// rtaweb.cpp — RLPlayer 网页播放器
//
// 一个常驻的本地服务：
//   * 浏览磁盘上的 WAV 文件
//   * 点一下就用当前参数播放
//   * 参数（滤波器/抖动/采样率/音量/GPU）在页面上直接选
//   * 停止是【协作式】的：置停止标志让播放线程自己收尾，不杀进程（铁律 2）
//
// 只监听 127.0.0.1，不对外网开放。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "net/webui.h"
#include "net/ui_page.h"
#include "net/ui_script.h"
#include "net/source_mgr.h"
#include "net/library.h"
#include "net/upnp.h"
#include "net/cast_service.h"
#include "net/app_icon.h"
#include "net/zipstore.h"          // 最小 ZIP(STORE)打包：导出诊断 = 一个压缩包(2026-10-22 功能3)
using rta::net::Library;
using rta::net::SourceManager;
using rta::net::SourceConfig;
using rta::net::SubEntry;
// 不直接 include <mmsystem.h>：它会和 MSVC 的 <complex> 冲突（本轮实测）。
// 只声明需要的两个函数，并链接 winmm。
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
#include "engine/pipeline.h"
#include "audio/wav.h"
#include "audio/audio_loader.h"
#include "audio/rt_tune.h"
#include "audio/media_info.h"
#include "audio/wasapi_out.h"
#include "audio/wasapi_shared.h"   // WASAPI 共享后端(VM/独占不可用兜底,2026-10-22 事故热修)
#include "audio/wasapi_capture.h"  // WASAPI 采集核心 (ROADMAP P4b)
#include "audio/render_loop.h"     // 渲染主循环(含 RENDERFAIL 遥测;测试与 rtaweb 共用同一份)
#include "audio/rate_pick.h"       // 输出采样率选择的纯逻辑(可离线单测;2026-10-22 第二首按 44100 开流事故)
#include "audio/asio_out.h"

// 渲染主循环迁到 rta::render 后，本文件的各后端实现继续用短名（历史上就是全局名）
using rta::render::OutSink;
using rta::render::SinkOpenOpts;
#include "meta/enrich.h"            // 单曲元数据补全编排(播放触发 + /api/enrich + /api/meta 兜底)
#include "meta/writeback.h"         // P2 标签写回(remux + PCM 逐位门禁;/api/writeback 三端点)
#include "meta/knowledge.h"         // P3 资料层直连与缓存(/api/entity/knowledge)
#include "meta/retrieval.h"         // P3 资料层③ AI 综合检索(/api/entity/summary)
#include "meta/discogs.h"           // P3 资料层④b Discogs 版本推荐(/api/entity/versions)
#include "net/rtanet_stream.h"
#include "dsp/filter_families.h"
#include "dsp/oversample_peak.h"
#include "dsp/eq_text.h"
#include "dsp/opra.h"
#include "dsp/dop.h"
#include "dsp/modulator.h"
#include "dsp/direct_sdm.h"
#include "version.h"
#include "crash_dump.h"            // 崩溃转储 + OS/CPU/tester 诊断（1.0.5 可观测性）
#include <cstdio>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <random>
#include <deque>
#include <condition_variable>
#include <memory>
#include <set>
#include <map>

namespace fs = std::filesystem;
using namespace rta;
using namespace rta::audio;
namespace eng = rta::engine;

// 路径 <-> UTF-8 字符串（C++20 的 u8string 返回 char8_t，不能直接当 std::string 用）
static std::string pathToUtf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

static rta::dsp::Shaper parse_shaper(const std::string& s) {
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    if (u == "NONE")   return rta::dsp::Shaper::None;
    if (u == "NS1")    return rta::dsp::Shaper::NS1;
    if (u == "NS4")    return rta::dsp::Shaper::NS4;
    if (u == "NS5")    return rta::dsp::Shaper::NS5;
    if (u == "NS9")    return rta::dsp::Shaper::NS9;
    if (u == "LNS15")  return rta::dsp::Shaper::LNS15;
    if (u == "RPDF")   return rta::dsp::Shaper::RPDF;
    if (u == "TPDF")   return rta::dsp::Shaper::TPDF;
    if (u == "GAUSS1") return rta::dsp::Shaper::Gauss1;
    if (u == "SHAPED") return rta::dsp::Shaper::Shaped;
    return rta::dsp::Shaper::None;
}

// ---------------- 参数 ----------------
struct PlayParams {
    std::string file;
    std::string filter;      // 1x（44.1/48k）用；空 = 默认 Kaiser
    std::string filterNx;    // Nx（>=88.2k）用；空则沿用 filter
    std::string shaper = "none";
    std::string rate;        // "src" | "max" | 数字
    double volume = 0.0;
    bool   gpu = false;
    bool   loudness = false;
    bool   isp = true;         // 过采样峰值保护
    bool   twoStage = true;    // 两段式升频
    int    dsd = 0;            // 0 = PCM；64/128/256 = DoP 封装的 DSD
    int    modOrder = 5;       // SDM 调制器阶数
    std::string modFamily = "dsd";     // SDM 调制器族：dsd | chebyshev | ec | ahm
    double oobGain = 5.0;              // Chebyshev 带外增益 dB
    int    ahmBits = 8;                // AHM 多电平位深 (6~8 bit)
    std::string gpuVendor = "auto";   // auto | amd | nvidia | off
    bool   dsdNative = false;         // true = 走 ASIO 原生 DSD（当前后端未实现，会明确报错）
    bool   directSdm = false;         // true = DirectSDM：源 DSD 与目标同速率时逐位直通（要求 0 dB、无 PCM 处理）
    int    bufferMs = 100;

    // ---- 输出后端 / 多声道 / 每声道微调 ----
    std::string sdmFilter;            // SDM（DSD 输出）用的 1x 滤波器；空 = 沿用 filter
    std::string sdmFilterNx;          // SDM 用的 Nx 滤波器；空 = 沿用 filterNx
    std::string backend = "wasapi";   // wasapi | asio | naa | null
    std::string naaHost;              // NAA 端点 host:port
    int    outChannels = 0;           // 0 = 跟随源
    double trim[8] = {0,0,0,0,0,0,0,0};   // 每声道增益 dB

    // ---- 输入 ----
    std::string inBackend = "none";   // none | wasapi
    std::string inDevice;
    int    inOffset = 0;
    int    inBufferMs = 100;
    bool   inMonitor = false;

    // ---- 完整设置（设置页用）----
    std::string device;          // 输出设备 id，空 = 默认设备
    int    bits = 0;             // 0 = 24（或 DSD 时固定 24）
    int    taps = 0;             // 0 = 用滤波器预设的抽头数
    double atten = 0.0;          // 0 = 用预设的阻带衰减
    double transition = 0.0;     // 0 = 用预设的过渡带
    std::string phase;           // "" = 预设 | "linear" | "minimum"
    double modCutoff = 0.0;      // kHz，0 = 100 kHz
    double modDither = -1.0;     // <0 = 默认 0.01
    std::string convIr;          // 卷积 IR 的 WAV 路径（空 = 不做卷积）
    std::string eqFile;          // EQ 文本（REW / AutoEq 格式）
    bool   convGpu = false;
    double volRamp = 0.0;        // 音量渐变毫秒，0 = 200

    std::string dsdKind = "fir";     // DSD 源抽取器：fir | iir | cic
    std::string dsdConv = "narrow";  // DSD 源 Conversion 预设：narrow | wide | xfi
    bool        dsd2Stage = true;    // DSD 源两级抽取（CIC 4x + 短 FIR），默认开启

    std::string remoteId;        // 远程曲目 id（Subsonic）
    std::string remoteSuffix;    // 远程文件后缀，决定缓存文件名
    std::string remoteTitle;
};

// ---------------- 播放状态 ----------------
struct PlayerState {
    std::mutex  mtx;
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<double> seekReq{-1.0};   // 跳转目标（0..1 的比例），-1 表示无请求
    std::atomic<bool> pauseReq{false};   // HTTP 线程置位，播放线程响应
    std::atomic<bool> paused{false};     // 实际是否处于暂停
    std::atomic<bool> playing{false};
    std::atomic<size_t> played{0};
    std::atomic<size_t> total{0};
    std::string current;
    std::string error;
    std::string info;
    std::string signal;       // 本次播放的信号链快照（JSON），供界面画"每一环"
    double outRate = 0;
    int    outBits = 0;
    bool   gpu = false;        // 本次播放是否由 GPU 升频
    std::atomic<long long> dbgPad{0}, dbgSkips{0};   // 诊断：设备侧水位与欠载跳过次数
    std::atomic<double> volDb{0.0};    // 用户当前请求的音量（拖动滑块即时生效）
    std::atomic<double> startDb{0.0};  // 本次播放起点音量（DSP 里已按它做过抖动量化）

    // 耗时统计（ROADMAP P6a 可观测性）
    std::atomic<long long> decodeMs{0};     // 解码总耗时 (ms)
    std::atomic<long long> ffmpegMs{0};     // 临时 WAV 转码写耗时 (ms)
    std::atomic<long long> wavReadMs{0};    // 临时 WAV / WAV 读取耗时 (ms)
    std::atomic<long long> dspChunkUs{0};   // 最近一块 DSP 处理耗时 (微秒)
    std::atomic<long long> dspTotalUs{0};   // 累计 DSP 处理耗时 (微秒)
    std::atomic<long long> dspChunks{0};    // 处理的 DSP 块数

    void joinIfDone() {
        if (th.joinable()) th.join();
    }
};
static PlayerState g_ps;
static std::atomic<bool> g_exitReq{false};
// 播放线程诊断计数（出问题时第一手证据，比猜快得多）
static std::atomic<long long> g_dbgW0{0}, g_dbgPulls{0}, g_dbgWf{0}, g_dbgWr{0}, g_dbgPer{0};
static std::atomic<double> g_dbgPeak{0.0};    // 最近一个写入周期的峰值（0..1）
static std::atomic<long long> g_dbgClip{0};   // 本次播放的满幅样本数（诊断削波/噪音）
static uint16_t g_uiPort = 8899;
static bool     g_lanOn  = false;
static std::mutex g_logMx;
static std::deque<std::string> g_log;
static void dlog(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_log.size() > 400) g_log.pop_front();
    g_log.push_back(s);
}
static PlayParams  g_params;      // 最近一次播放参数，跳曲时沿用

// ---------------- 可观测性（1.0.5）：运行时长 / 构建时间 ----------------
static const std::chrono::steady_clock::time_point g_startTp = std::chrono::steady_clock::now();
static const char* const g_buildTime = __DATE__ " " __TIME__;

// DSD 源抽取设置（P0-4/P0-5）：播放与预解码两条线程都要用，所以放在一个函数里
static void apply_dsd_src_settings(const PlayParams& p) {
    auto& st = dsd_pcm_settings();      // 必须是引用：auto 会拷贝，设置就丢了（本轮踩到）
    st.kind = (p.dsdKind == "iir") ? rta::dsp::IntegratorKind::Iir
            : (p.dsdKind == "cic") ? rta::dsp::IntegratorKind::Cic
                                   : rta::dsp::IntegratorKind::Fir;
    st.conversion = rta::dsp::parse_conversion(p.dsdConv);
    st.twoStage = p.dsd2Stage;
}

// ---------------- 工具 ----------------
static std::string jesc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

static std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int a = hex(s[i + 1]), b = hex(s[i + 2]);
            if (a >= 0 && b >= 0) { o += (char)(a * 16 + b); i += 2; continue; }
        }
        if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

// ---------------- F1：严格 UTF-8 校验（逐字节状态机）----------------
// fs::u8path 对非法 UTF-8 字节会抛异常 —— 而本服务对请求异常没有整体兜底，一次
// 编码不对的 file 参数就能把整个进程打崩（2026-10-22 两次复现）。所有把 file/dir
// 交给 fs 路径的端点必须先过这一关。拒绝：续字节错配、截断、过短编码、代理区、>U+10FFFF。
static bool valid_utf8(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        int cont; uint32_t cp;
        if (c < 0x80) { ++i; continue; }                                  // 0xxxxxxx
        else if (c >= 0xC2 && c <= 0xDF) { cont = 1; cp = c & 0x1Fu; }    // 110xxxxx
        else if (c >= 0xE0 && c <= 0xEF) { cont = 2; cp = c & 0x0Fu; }    // 1110xxxx
        else if (c >= 0xF0 && c <= 0xF4) { cont = 3; cp = c & 0x07u; }    // 11110xxx
        else return false;                                                // C0/C1、F5..FF、续字节错位
        if (i + (size_t)cont >= n) return false;                          // 截断
        for (int k = 1; k <= cont; ++k) {
            const unsigned char cc = p[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) return false;                        // 续字节必须 10xxxxxx
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }
        if ((cont == 1 && cp < 0x80) || (cont == 2 && cp < 0x800) ||
            (cont == 3 && cp < 0x10000)) return false;                    // 过短编码
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false; // 越界/代理区
        i += (size_t)cont + 1;
    }
    return true;
}

// F6：非法 UTF-8 字节替换为 U+FFFD（替换符）。playback.log 里可能出现旧版本写入或
// 外部工具追加的非 UTF-8 字节，直接塞进 JSON 会被浏览器当乱码。
static std::string utf8_sanitize(const std::string& s) {
    static const char kRepl[] = "\xEF\xBF\xBD";                            // U+FFFD
    std::string o; o.reserve(s.size() + 16);
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { o += (char)c; ++i; continue; }
        int cont; uint32_t cp;
        if      (c >= 0xC2 && c <= 0xDF) { cont = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { cont = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { cont = 3; cp = c & 0x07u; }
        else { o += kRepl; ++i; continue; }
        bool ok = (i + (size_t)cont < n);
        if (ok) {
            for (int k = 1; k <= cont; ++k) {
                const unsigned char cc = (unsigned char)s[i + (size_t)k];
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
            }
        }
        if (ok && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF) &&
            !((cont == 1 && cp < 0x80) || (cont == 2 && cp < 0x800) || (cont == 3 && cp < 0x10000))) {
            o.append(s, i, (size_t)cont + 1);                              // 合法序列原样保留
            i += (size_t)cont + 1;
        } else {
            o += kRepl; ++i;                                               // 坏起点换 U+FFFD，只前进 1 字节
        }
    }
    return o;
}

// 解析 "k=v&k=v"
static PlayParams parseForm(const std::string& body) {
    PlayParams p;
    std::istringstream is(body);
    std::string tok;
    while (std::getline(is, tok, '&')) {
        const size_t e = tok.find('=');
        if (e == std::string::npos) continue;
        const std::string k = urlDecode(tok.substr(0, e));
        const std::string v = urlDecode(tok.substr(e + 1));
        if      (k == "file")     p.file = v;
        else if (k == "filter")   p.filter = v;
        else if (k == "filterNx") p.filterNx = v;
        else if (k == "shaper")   p.shaper = v;
        else if (k == "rate")     p.rate = v;
        else if (k == "volume")   p.volume = atof(v.c_str());
        else if (k == "gpu")      p.gpu = (v == "1");
        else if (k == "isp")      p.isp = (v == "1");
        else if (k == "twostage") p.twoStage = (v == "1");
        else if (k == "dsd")      p.dsd = atoi(v.c_str());
        else if (k == "modorder") p.modOrder = atoi(v.c_str());
        else if (k == "modfamily") p.modFamily = v;
        else if (k == "oobgain")   p.oobGain = atof(v.c_str());
        else if (k == "ahmbits")   p.ahmBits = atoi(v.c_str());
        else if (k == "gpuvendor") p.gpuVendor = v;
        else if (k == "dsdnative") p.dsdNative = (v == "1");
        else if (k == "directsdm") p.directSdm = (v == "1");
        else if (k == "dsdkind")  p.dsdKind = v;
        else if (k == "dsdconv")  p.dsdConv = v;
        else if (k == "dsd2stage") p.dsd2Stage = (v == "1" || v == "true");
        else if (k == "loudness") p.loudness = (v == "1");
        else if (k == "buffer")   p.bufferMs = std::max(10, atoi(v.c_str()));
        else if (k == "device")   p.device = v;
        else if (k == "bits")     p.bits = atoi(v.c_str());
        else if (k == "taps")     p.taps = atoi(v.c_str());
        else if (k == "atten")    p.atten = atof(v.c_str());
        else if (k == "transition") p.transition = atof(v.c_str());
        else if (k == "phase")    p.phase = v;
        else if (k == "modcut")   p.modCutoff = atof(v.c_str());
        else if (k == "moddith")  p.modDither = atof(v.c_str());
        else if (k == "ir")       p.convIr = v;
        else if (k == "eq")       p.eqFile = v;
        else if (k == "irgpu")    p.convGpu = (v == "1");
        else if (k == "ramp")     p.volRamp = atof(v.c_str());
        else if (k == "sdmfilt")   p.sdmFilter = v;
        else if (k == "sdmfiltnx") p.sdmFilterNx = v;
        else if (k == "backend") p.backend = v;
        else if (k == "naahost") p.naaHost = v;
        else if (k == "channels") p.outChannels = atoi(v.c_str());
        else if (k == "trim") {
            std::istringstream ts(v);
            std::string tv;
            for (int i = 0; i < 8 && std::getline(ts, tv, ','); ++i) p.trim[i] = atof(tv.c_str());
        }
        else if (k == "inbackend") p.inBackend = v;
        else if (k == "indev")    p.inDevice = v;
        else if (k == "inoffset") p.inOffset = atoi(v.c_str());
        else if (k == "inbuf")    p.inBufferMs = std::max(10, atoi(v.c_str()));
        else if (k == "inmon")    p.inMonitor = (v == "1");
        else if (k == "rid")     p.remoteId = v;
        else if (k == "rsuffix") p.remoteSuffix = v;
        else if (k == "rtitle")  p.remoteTitle = v;
    }
    return p;
}

static void recent_add(const std::string& file, const std::string& title,
                       const std::string& artist, const std::string& album);

// ---------------- 输出后端 ----------------
// OutSink 接口与渲染主循环已迁至 src/audio/render_loop.h（测试与 rtaweb 共用同一份，
// RENDERFAIL 回归测试用 mock sink 真跑同一循环）。这里只放各后端的实现。

static SinkOpenOpts sink_opts(const PlayParams& p) {
    SinkOpenOpts o;
    o.device = p.device;
    o.naaHost = p.naaHost;
    o.bufferMs = p.bufferMs;
    return o;
}

class WasapiSink : public OutSink {
public:
    bool open(const SinkOpenOpts& o, uint32_t rate, int ch, int bits) override {
        fail_ = OpenFail{};   // 每次打开重置：失败原因只属于最近一次尝试
        // ★ 2026-10-23：独占缓冲封顶 20ms。设置里的 100ms 在 MOTU 上会被对齐成
        // **8192 帧（186ms）**，Initialize 虽接受，但 Start 后设备流不推进（现场：
        // RENDERFAIL「4.02 秒未接受数据」，zp=201 pad=full、wr 只有 76、GetBuffer 从未被调用）。
        // 20ms（约 882 帧）是这类 USB 声卡独占的常见可用区间。
        const double ms = (o.bufferMs > 0 && o.bufferMs < 20) ? (double)o.bufferMs : 20.0;
        ok_ = ao_.open(o.device, rate, ch, bits, ms, &fail_);
        return ok_;
    }
    // HF-01/02：open 失败时的结构化原因（请求格式 / 支持列表 / HRESULT / 端点 id）。
    // 成功时内容无意义 —— 调用方只在 open 返回 false 后读它。
    const OpenFail& fail() const { return fail_; }
    // 2026-10-23：成功时驱动实际接受的独占格式（"独占格式=EXT24/24 紧凑(3字节)"）。
    // 以前这个信息只有失败时才带出，成功就丢 —— 用户看到 note 写"32-bit"却不知道驱动到底认了什么。
    const std::string& fmt_note() const { return ao_.fmt_note(); }
    bool start() override {
        if (!ok_) return false;
        if (!ao_.start()) { err_ = ao_.last_error(); return false; }
        return true;
    }
    size_t buffer_frames() const override { return ao_.buffer_frames() ? (size_t)ao_.buffer_frames() : 9600; }
    void set_live_gain_db(double db) override { ao_.set_live_gain_db(db); }
    void wait_event(int ms) override { ao_.wait_event(ms); }
    long long last_pad() const override { return ao_.last_pad(); }
    size_t write_one_period(const int32_t* pcm, size_t frames) override { return ao_.write_one_period(pcm, frames); }
    size_t write_available(const int32_t* pcm, size_t frames) override { return ao_.write_available(pcm, frames); }
    void pause() override { ao_.pause(); }
    void resume() override { ao_.resume(); }
    void close() override { ao_.close(); }
    const std::string& last_error() const override { return err_.empty() ? ao_.last_error() : err_; }
    long long last_gb_hr() const override { return ao_.last_gb_hr(); }
    // 2026-10-22 反馈 20261022-02：把 WasapiOutput 的「写不进去」成因计数转发给 OutSink。
    // 不转发 ⟹ render_loop.h:296 的 if (zp >= 0 || zs >= 0) 闸门永假 ⟹ STAT 永不打印 zp/zs/zr。
    // 命名映射：wasapi_out.h 叫 last_zero_reason()，OutSink 叫 zero_reason()。
    long long zero_pad_full() const override { return ao_.zero_pad_full(); }
    long long zero_starved()  const override { return ao_.zero_starved(); }
    std::string zero_reason() const override { return ao_.last_zero_reason(); }
private:
    WasapiOutput ao_;
    bool ok_ = false;
    OpenFail fail_;
    std::string err_;
};

// WASAPI 共享模式：经系统混音引擎，独占不可用（VM 板载声卡 / 远程桌面 / 设备被占）时
// 的兜底后端。开流格式 = GetMixFormat（通常 float32/48k），调用方必须按 mix 采样率与
// 声道数排管线 —— open 里做一致性校验，不匹配即拒绝并说明（防止静默二次重采样/声道错乱）。
class WasapiSharedSink : public OutSink {
public:
    bool open(const SinkOpenOpts& o, uint32_t rate, int ch, int bits) override {
        // ★ 2026-10-23：这里原来把位深硬编码成 32（`int /*bits*/` 直接丢弃），理由是
        // "共享模式位深由混音引擎决定"。混音引擎确实是 float32，但 WasapiShared 的 reqBits_
        // 不是描述引擎，而是描述**管线交来的 int32 是按几个 bit 定标的**——pack() 要用它把
        // ±2^23（24-bit）归一化到 float。传 32 → 相当于除以 2^31 → 输出 −48.16 dB。
        // 现场实测：peak=0.9886 而 fpeak=0.0038619，比值 1/256，正好是这一处。
        ok_ = so_.open(o.device, rate, ch, bits > 0 ? bits : 32, 200.0);
        if (!ok_) return false;
        if (so_.mix_rate() != rate || so_.mix_channels() != ch) {
            char b[160];
            std::snprintf(b, sizeof(b),
                          "共享模式管线/设备格式不一致：管线 %u Hz/%d ch，系统混音 %u Hz/%d ch"
                          "（调用方应先 query_mix_format 对齐）",
                          (unsigned)rate, ch, (unsigned)so_.mix_rate(), so_.mix_channels());
            err_ = b;
            so_.close();
            ok_ = false;
            return false;
        }
        return true;
    }
    bool start() override {
        if (!ok_) return false;
        if (!so_.start()) { err_ = so_.last_error(); return false; }
        return true;
    }
    size_t buffer_frames() const override { return so_.buffer_frames() ? so_.buffer_frames() : 9600; }
    void set_live_gain_db(double db) override { so_.set_live_gain_db(db); }
    void wait_event(int ms) override { so_.wait_event(ms); }
    long long last_pad() const override { return so_.last_pad(); }
    size_t write_one_period(const int32_t* pcm, size_t frames) override { return so_.write_available(pcm, frames); }
    size_t write_available(const int32_t* pcm, size_t frames) override { return so_.write_available(pcm, frames); }
    double out_peak() const override { return so_.out_peak(); }
    void pause() override { so_.pause(); }
    void resume() override { so_.resume(); }
    void close() override { so_.close(); }
    const std::string& last_error() const override { return err_.empty() ? so_.last_error() : err_; }
    long long last_gb_hr() const override { return so_.last_gb_hr(); }
    uint32_t mix_rate() const { return so_.mix_rate(); }
    int mix_channels() const { return so_.mix_channels(); }
    bool is_float() const { return so_.is_float(); }
private:
    WasapiSharedOutput so_;
    bool ok_ = false;
    std::string err_;
};

// Null output：完整跑 DSP，但样本直接丢掉。按实时节奏"消耗"，所以进度条与真播放一致。
// 用途：验证管线、量 CPU 占用、给频谱分析抓数据，都不必打开任何设备。
class NullSink : public OutSink {
public:
    bool open(const SinkOpenOpts& o, uint32_t rate, int ch, int bits) override {
        rate_ = rate ? rate : 44100; ch_ = ch;
        period_ = (size_t)((double)rate_ * (double)o.bufferMs / 1000.0);
        if (period_ < 256) period_ = 256;
        t0_ = now();
        return true;
    }
    bool start() override { return true; }
    size_t buffer_frames() const override { return period_; }
    size_t write_one_period(const int32_t* /*pcm*/, size_t frames) override {
        written_ += frames;
        pace(written_ / (double)rate_);
        return frames;
    }
    const std::string& last_error() const override { return err_; }
private:
    static double now() {
        LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
        return (double)t.QuadPart / (double)f.QuadPart;
    }
    void pace(double audioSec) const {
        for (;;) {
            const double el = now() - t0_;
            if (el >= audioSec) return;
            double ms = (audioSec - el) * 1000.0;
            if (ms > 20.0) ms = 20.0;
            Sleep((DWORD)(ms > 0 ? ms : 1));
        }
    }
    uint32_t rate_ = 44100;
    int ch_ = 2;
    size_t period_ = 4410;
    size_t written_ = 0;
    double t0_ = 0.0;
    std::string err_;
};

// NAA：把 PCM 经 RTANet 推到端点机（rtanet_rx）。数值上与本地输出同源，不经过任何转码。
class NaaSink : public OutSink {
public:
    bool open(const SinkOpenOpts& o, uint32_t rate, int ch, int bits) override {
        rate_ = rate ? rate : 44100; ch_ = ch; bits_ = bits;
        std::string host = o.naaHost.empty() ? std::string("127.0.0.1") : o.naaHost;
        uint16_t port = 43210;
        const size_t c = host.rfind(':');
        if (c != std::string::npos) {
            port = (uint16_t)atoi(host.c_str() + c + 1);
            host = host.substr(0, c);
        }
        rta::net::RtaFormat want;
        want.rate = rate_; want.channels = ch_; want.bits = bits_;
        uint16_t udp = 0;
        if (!cli_.negotiate(host, port, want, udp, 1, err_)) {
            err_ = "NAA 协商失败（" + host + ":" + std::to_string(port) + "）：" + err_;
            return false;
        }
        snd_.configure(host, udp, want, 1, 128);
        period_ = 1024;
        t0_ = now();
        return true;
    }
    bool start() override { return true; }
    size_t buffer_frames() const override { return period_; }
    size_t write_one_period(const int32_t* pcm, size_t frames) override {
        const size_t w = snd_.send(pcm, frames);
        if (w == 0) { err_ = "发送失败（端点已断开？）"; return 0; }
        sent_ += w;
        // 按实时节奏推送，否则一瞬间就把端点的缓冲灌满
        for (;;) {
            const double el = now() - t0_;
            const double want = (double)sent_ / (double)rate_;
            if (el >= want) break;
            double ms = (want - el) * 1000.0;
            if (ms > 20.0) ms = 20.0;
            Sleep((DWORD)(ms > 0 ? ms : 1));
        }
        return w;
    }
    void close() override { cli_.close(); }
    const std::string& last_error() const override { return err_; }
private:
    static double now() {
        LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
        return (double)t.QuadPart / (double)f.QuadPart;
    }
    rta::net::RtaControlClient cli_;
    rta::net::RtaSender snd_;
    uint32_t rate_ = 44100;
    int ch_ = 2, bits_ = 24;
    size_t period_ = 1024;
    size_t sent_ = 0;
    double t0_ = 0.0;
    std::string err_;
};

static std::string path_in_exe_dir(const char* name);   // 定义在后面
static void play_log(const std::string& s);              // 定义在后面
static void play_log_rotate();

// E-11: 记录上一次播放以 END pos=0 error="" 结束的状态，供下一次 START 显式归因 PLAY_ABORT
struct LastPlayEnd {
    bool isPosZeroNoErr = false;
    std::string file;
    std::chrono::steady_clock::time_point tp{};
};
static std::mutex g_lastEndMx;
static LastPlayEnd g_lastEnd;

// ---------------- 设备独占支持率探测（带重试 + 上次成功结果缓存）----------------
// 为什么需要这一层（2026-10-22 现场事故，日志 playback (4).log 第 9~23 行）：
//   第一首 44.1k FLAC 按 rate=max 探到 96000 并正常播完；紧接着播第二首，同一端点、
//   同一设置的 exclusive_rates() 却【瞬时返回空列表】（上一首独占流 sink->close() 之后
//   约 165ms，端点正在重配置/占用切换），几毫秒后同一端点又能读到 48000/96000
//   （紧接着的错误路径里 probe_formats 就证明过）。旧代码 want = rr.empty() ? 0 : rr.back()
//   把「探测为空」静默当成「跟随源率」→ 按 44100 开独占流 → 端点只支持 48k/96k 族 →
//   在写 PLAY 之前就报错退出（日志里连 PLAY 都没有）。
// 三道防线，任一条生效都不会再犯：
//   ① 探测最多 3 次、间隔 40ms，第一次非空即返回（3×40ms=120ms 的起播延迟可接受，
//      比一次误判导致整首不出声便宜得多）；
//   ② 进程内按端点键缓存【上一次成功的列表】——「上一首刚用 96k 播完整首歌」本身就是
//      设备支持 96k 的铁证，不能让一次瞬时探测为空把它抹掉（铁律 3：判据要测对东西）；
//   ③ 探测为空且无缓存时，pick_output_rate 明确标 RATEPROBE_EMPTY（decided=false），
//      绝不静默当成「探测结果是源率」。
// 缓存键含位深与声道数：16-bit 与 24-bit 的独占支持列表本来就不同，混用等于造假数据。
static std::mutex g_rateCacheMtx;
static std::map<std::string, std::vector<uint32_t>> g_rateCache;

static std::string rate_cache_key(const std::string& devId, int bits, int channels) {
    // devId 为空 = "WASAPI 默认端点"：必须解析成真实端点 id 再当键，否则用户中途换了默认设备时
    // 会把上一个设备的支持列表当成新设备的（那就是造假数据）。
    std::string id = devId;
    if (id.empty()) {
        for (const auto& d : WasapiOutput::enumerate())
            if (d.isDefault) { id = d.id; break; }
        if (id.empty()) id = "(默认端点)";
    }
    return id + "|" + std::to_string(bits) + "bit|" + std::to_string(channels) + "ch";
}
// 容量的有界性：键 = 端点 id | 位深 | 声道数，端点数最多几十个、位深 3 档、声道数也只是个位数，
// 因此总量是「端点数 × 3 × 声道数」这个量级（几十~几百条），进程生命期内无需淘汰 —— 不是内存泄漏。
//
// ⚠️ key 必须在【锁外】算好：rate_cache_key 在 devId 为空时要 enumerate()（COM + 每个端点的
//    PropertyStore，本机 39 个端点）。把 COM 枚举放进全局锁里，一旦 COM 慢/卡，会同时冻住
//    播放线程和 web 线程（这把锁被 /api/rates 用，而 webui 是单线程 accept）。
static void rate_cache_put(const std::string& devId, int bits, int channels,
                           const std::vector<uint32_t>& rates) {
    if (rates.empty()) return;                 // 空列表绝不入缓存：否则一次瞬时故障把「什么都没有」永久固化
    const std::string key = rate_cache_key(devId, bits, channels);   // 锁外（可能 enumerate）
    std::lock_guard<std::mutex> lk(g_rateCacheMtx);
    g_rateCache[key] = rates;
}
static std::vector<uint32_t> rate_cache_get(const std::string& devId, int bits, int channels) {
    const std::string key = rate_cache_key(devId, bits, channels);   // 锁外（可能 enumerate）
    std::lock_guard<std::mutex> lk(g_rateCacheMtx);
    const auto it = g_rateCache.find(key);
    return it == g_rateCache.end() ? std::vector<uint32_t>() : it->second;
}

// 独占支持率探测：最多 tries 次、每次间隔 40ms，第一次非空即返回并刷新缓存。
// 不修改 wasapi_out.h 的行为 —— 重试策略只属于「调用方知不知道自己被瞬时态坑过」这一层。
static std::vector<uint32_t> probe_exclusive_rates(const std::string& deviceId, int bits,
                                                   int channels, int tries = 3) {
    std::vector<uint32_t> rr;
    for (int i = 0; i < tries; ++i) {
        if (i > 0) Sleep(40);      // 等端点从「刚关闭独占流」的重配置里恢复
        rr = WasapiOutput::exclusive_rates(deviceId, bits, channels);
        if (!rr.empty()) { rate_cache_put(deviceId, bits, channels, rr); return rr; }
    }
    return rr;                     // 全空：调用方按「探测为空」处理（不是结论）
}

// 探测 + 缓存兜底：探测为空时退回该端点/位深/声道上一次成功的列表，并记一行 RATEPROBE_STALE。
// 为什么能力检查也要走这一层：rate=max 那一步即使靠缓存拿到了 96000，如果紧接着的能力检查
// 又探测为空，位深循环会认为「该位深什么都不支持」→ 仍旧硬失败 —— 缓存只救第一步等于没救。
// ⚠️ 如实说明：重试与缓存这两层依赖真实设备的瞬时态，没有自动化测试覆盖（离线造不出
//    端点重配置窗口），只有代码审查 + 逻辑推演 + 现场日志形态推演。
static std::vector<uint32_t> probe_rates_cached(const std::string& deviceId, int bits, int channels,
                                                const char* why, int tries = 3) {
    std::vector<uint32_t> rr = probe_exclusive_rates(deviceId, bits, channels, tries);
    if (!rr.empty()) return rr;
    rr = rate_cache_get(deviceId, bits, channels);
    if (!rr.empty()) {
        play_log(std::string("RATEPROBE_STALE ") + why + " 探测为空，沿用上次结果（最高 " +
                 std::to_string(rr.back()) + " Hz） bits=" + std::to_string(bits) +
                 " device=" + (deviceId.empty() ? std::string("默认端点") : deviceId));
    }
    return rr;
}
static void meta_on_play(const std::string& file);       // 定义在后面:播放触发的元数据补全(后台线程)

// F1：非法 UTF-8 的 file/dir 参数统一在此挡下 —— 记一行 play_log WARN（原始字节
// 十六进制前 64 字节，不含解释），返回统一错误 JSON。绝不把非法字节交给 fs::u8path。
static std::string utf8_path_reject(const std::string& path) {
    std::string hex;
    char b[4];
    const size_t nb = path.size() < 64 ? path.size() : 64;
    for (size_t i = 0; i < nb; ++i) { std::snprintf(b, sizeof(b), "%02X", (unsigned char)path[i]); hex += b; }
    play_log("WARN 拒绝非UTF-8路径 原始字节[" + std::to_string(path.size()) + "B]=" + hex);
    return "{\"ok\":false,\"error\":\"路径编码非 UTF-8(请用浏览器或 UTF-8 客户端)\"}";
}

// F2：enriched 语义 —— 「真的被在线补过」：任一字段 source ∈ 在线源集合，或缓存过封面。
// 只有本地 tag/user 写入的基本标签（如 studio 曲目自带 title/album）不再误显示「已补全」。
static bool meta_enriched(const rta::meta::TrackMeta& tm) {
    if (!tm.coverRelPath.empty()) return true;
    for (const auto& kv : tm.fields) {
        const std::string& s = kv.second.source;
        if (s == "qq" || s == "netease" || s == "itunes" || s == "lrclib" ||
            s == "musicbrainz" || s == "wiki" || s == "llm") return true;
    }
    return false;
}

// F1 兜底：播放线程里 recent_add 要的文件名主干。队列项理论上都来自已校验端点，
// 但 rlplayer-queue.txt 可被旧版本/手工编辑写坏 —— 非法字节绝不能进 fs::u8path
//（播放线程里抛异常 = 整进程终止），此时直接用原串当标题兜底。
static std::string stem_or_self(const std::string& file) {
    if (!valid_utf8(file)) return file;
    return pathToUtf8(fs::u8path(file).stem());
}

// ASIO 驱动预检：第三方驱动的 start() 之后可能在它自己的流线程里访问冲突，
// 那种崩溃在进程内拦不住（SEH 只覆盖当前线程）。所以首次使用某个驱动时，
// 先让 asio_probe.exe 在【子进程】里跑 0.7 秒零样本；子进程崩了就把结果写进缓存，
// 此后这个驱动一律拒绝，播放器本体不受影响。
static bool asio_preflight(const std::string& clsid, uint32_t rate, int ch, std::string& why) {
    const std::string cache = path_in_exe_dir("rlplayer-asio-cache.txt");
    const std::string key = clsid + " " + std::to_string(rate) + " " + std::to_string(ch);
    {
        std::ifstream f(cache);
        std::string line;
        while (std::getline(f, line)) {
            if (line == key + " OK") return true;
            if (line == key + " CRASH") {
                why = "该 ASIO 驱动在本机启动后会崩溃（子进程已复现），已拒绝使用以免拖垮播放器";
                return false;
            }
        }
    }
    const std::string probe = path_in_exe_dir("asio_probe.exe");
    if (GetFileAttributesA(probe.c_str()) == INVALID_FILE_ATTRIBUTES) return true;  // 没有探针就不拦

    // 用序号而不是 CLSID：探针的两个参数解析器不共享状态，序号最不容易错
    int idx = -1;
    {
        const auto list = rta::audio::asio_enum_drivers();
        for (size_t i = 0; i < list.size(); ++i) if (list[i].clsid == clsid) { idx = (int)i; break; }
    }
    if (idx < 0) return true;   // 列不出来的驱动交给后面 open 去报错
    std::string cmd = "\"" + probe + "\" --stream " + std::to_string(idx) +
                      " --rate " + std::to_string(rate) + " --ch " + std::to_string(ch) + " --ms 700";
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return true;   // 起不来就当没预检
    }
    const DWORD w = WaitForSingleObject(pi.hProcess, 8000);
    DWORD code = 1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else { TerminateProcess(pi.hProcess, 1); code = 1; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

    std::ofstream cf(cache, std::ios::app);
    if (cf) cf << key << (code == 0 ? " OK" : " CRASH") << "\n";
    if (code != 0) {
        why = "该 ASIO 驱动启动后崩溃（子进程码 " + std::to_string((long long)code) + "），已拒绝使用";
        return false;
    }
    return true;
}

// 设备 id 是否为合法 CLSID。不能只看首字符 '{'：WASAPI 端点 id 形如
// {0.0.0.00000000}.{GUID}，也会被误当成 ASIO CLSID（实测报 CLSID 解析失败）。
static bool is_asio_clsid(const std::string& s) {
    if (s.empty()) return false;
    std::wstring w(s.begin(), s.end());
    CLSID c{};
    return SUCCEEDED(CLSIDFromString(w.c_str(), &c));
}

// ASIO：设备 id 就是驱动的 CLSID（由 /api/asiodevices 提供）；空则用第一个能开的驱动。
class AsioSink : public OutSink {
public:
    bool open(const SinkOpenOpts& o, uint32_t rate, int ch, int bits) override {
        com_ = (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) == S_OK || com_ == S_FALSE);
        auto list = rta::audio::asio_enum_drivers();
        if (list.empty()) { err_ = "本机没有 ASIO 驱动"; return false; }
        std::string clsid = o.device;
        if (!is_asio_clsid(clsid)) {
            clsid.clear();
            for (const auto& e : list) if (e.name == o.device) { clsid = e.clsid; break; }
        }
        if (!is_asio_clsid(clsid)) {
            // 设备没指定（或名字对不上）：逐个 preflight，第一个通过的就用。
            // 不能只看 list[0] —— 本机 [0] 是未连接的 Audient，会把真正可用的 MOTU 挡在后面，
            // 表现就是"选了 ASIO 但没声音/直接失败"（2026-10-23 现场）。
            std::string tried;
            for (const auto& e : list) {
                std::string why;
                if (asio_preflight(e.clsid, rate, ch, why)) { clsid = e.clsid; break; }
                tried += e.name + "[" + why + "] ";
            }
            if (!is_asio_clsid(clsid)) {
                err_ = "没有可用的 ASIO 驱动：" + tried;
                play_log("ASIO_NODRIVER " + err_);
                return false;
            }
            play_log("ASIO_AUTOPICK 未指定驱动，自动选中 " + clsid);
        }
        if (!asio_preflight(clsid, rate, ch, err_)) {
            play_log("ASIO_PREFLIGHT_FAIL " + err_);
            return false;
        }
        if (!drv_.open(clsid, (double)rate, ch, bits > 0 ? bits : 24, err_)) return false;
        period_ = (size_t)drv_.buffer_size();
        return true;
    }
    bool start() override { std::string e; if (!drv_.start(e)) { err_ = e; return false; } return true; }
    size_t buffer_frames() const override { return period_ ? period_ : 1024; }
    // ASIO 没有独立暂停。暂停时上层不再写，环被回调抽空后自然输出静音，
    // 这样不必 stop/start 驱动（重启驱动会有咔哒声，还可能丢时钟）。
    void pause() override {}
    void resume() override {}
    size_t write_one_period(const int32_t* pcm, size_t frames) override {
        // 等环腾出空间再推（这就是节奏来源：由驱动回调的消费速度决定）
        for (int i = 0; i < 200; ++i) {
            if (drv_.free_frames() >= frames) {
                const size_t w = drv_.push(pcm, frames);
                if (w) written_ += w;
                return w;
            }
            Sleep(1);
        }
        return 0;
    }
    void close() override {
        drv_.close();
        if (com_) { CoUninitialize(); com_ = false; }
    }
    const std::string& last_error() const override { return err_; }
    unsigned long long underruns() const { return drv_.underruns(); }
private:
    rta::audio::AsioDriver drv_;
    size_t period_ = 1024;
    size_t written_ = 0;
    bool com_ = false;
    std::string err_;
};

static std::unique_ptr<OutSink> make_sink(const PlayParams& p) {
    if (p.backend == "null")           return std::make_unique<NullSink>();
    if (p.backend == "naa")            return std::make_unique<NaaSink>();
    if (p.backend == "asio")           return std::make_unique<AsioSink>();   // 见 audio/asio_out.h
    if (p.backend == "wasapi-shared")  return std::make_unique<WasapiSharedSink>();
    return std::make_unique<WasapiSink>();
}

// ---------------- DirectSDM：源 DSD 与目标 DSD 同速率时的逐位直通 ----------------
// 判定必须在解码之前：DSD256 整曲解成 PCM 要几百 MB，先判再决定路径。
static bool direct_sdm_eligible(const PlayParams& p, const DsdInfo& info, std::string& why) {
    if (p.dsdNative) { why = "DSD Native 还没实现（它走 ASIO 原生 DSD，不是 DoP）"; return false; }
    if (p.dsd < 64) { why = "输出不是 DSD：把 SDM 页的比特率选成 DSD64/128/256（DoP）"; return false; }
    const uint32_t base = (info.rate % 48000u == 0) ? 48000u : 44100u;
    const uint32_t target = rta::dsp::dsd_rate(p.dsd, base);
    bool anyProc = !p.eqFile.empty() || !p.convIr.empty() || p.loudness;
    for (int i = 0; i < 8; ++i) if (p.trim[i] != 0.0) anyProc = true;
    if (p.outChannels > 0 && p.outChannels != info.channels) anyProc = true;   // 声道数变化也是处理
    const auto d = rta::dsp::direct_sdm_decision(info.rate, target, anyProc, p.volume != 0.0);
    why = d.reason;
    return d.enabled;
}

// 直接播放 DSD 源：1-bit 流按 16 位整数倍切片 → DoP 24-bit 载波 → 输出后端。
// 不经过 PCM、不调制、不做音量/抖动 —— 这正是"逐位一致"的前提。
static bool play_one_direct(const std::string& file, const PlayParams& p) {
    auto& ps = g_ps;
    play_log_rotate();
    g_dbgPeak.store(0.0);
    g_dbgClip.store(0);
    RtTune rt = rta::audio::apply_playback_tuning();
    struct RtGuard { RtTune* t; ~RtGuard() { rta::audio::release_playback_tuning(*t); } } rtg{ &rt };
    try {
        ps.decodeMs.store(0);
        ps.ffmpegMs.store(0);
        ps.wavReadMs.store(0);
        ps.dspChunkUs.store(0);
        ps.dspTotalUs.store(0);
        ps.dspChunks.store(0);
        const auto t_dsd_0 = std::chrono::steady_clock::now();
        const DsdStream src = read_dsd(file);
        const auto t_dsd_1 = std::chrono::steady_clock::now();
        const double dsdMs = std::chrono::duration<double, std::milli>(t_dsd_1 - t_dsd_0).count();
        ps.decodeMs.store((long long)(dsdMs + 0.5));
        if (src.rate == 0 || src.bits == 0) {
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "DSD 解码结果为空：" + file;
            return false;
        }
        // 共享模式不支持 DoP/DSD：共享会经系统混音引擎重采样，DoP 载波（0x05/0xFA 标记）
        // 一旦被重采样就不再是合法 DoP，接收端判非法并静音 —— 必须明确报错而不是降质播放。
        if (p.backend == "wasapi-shared") {
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "共享模式不支持 DoP/DSD（共享会重采样破坏 DoP 载波），请切换 WASAPI 独占或 ASIO。";
            return false;
        }
        const uint32_t srcRate = src.rate;
        const uint32_t carrier = srcRate / 16u;
        const int      ch = src.channels;
        const size_t   totalFrames = (size_t)(src.bits / 16ull);   // DoP 帧数（每声道）
        if (totalFrames == 0) {
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "DSD 数据不足一个 DoP 帧（16 位）";
            return false;
        }

        // 设备能力：DoP 载波率是源 DSD/16，本机 DAC 不一定支持（DSD256 要 705.6 kHz）。
        // HF-01 接线（2026-10-22）：空支持列表一律当「不支持/端点异常」，绝不静默放行（交接单 §6）。
        // DoP 固定 24-bit 容器，不参与位深回退（维持现状）。
        if (p.backend.empty() || p.backend == "wasapi") {
            const auto rates = WasapiOutput::exclusive_rates(p.device, 24, ch);
            bool ok = false;
            for (auto r : rates) if (r == carrier) ok = true;
            if (!ok) {
                std::string list;
                for (size_t i = 0; i < rates.size(); ++i) { if (i) list += " / "; list += std::to_string(rates[i]); }
                std::lock_guard<std::mutex> lk(ps.mtx);
                ps.error = rates.empty()
                    ? ("设备独占支持列表为空（端点不可用或 24-bit 容器不被接受），无法确认 DirectSDM 所需的 DoP 载波 " +
                       std::to_string(carrier) + " Hz。")
                    : ("设备不支持 DirectSDM 所需的 DoP 载波 " + std::to_string(carrier) +
                       " Hz。设备支持：" + list);
                return false;
            }
        }

        ps.outRate = carrier;
        ps.outBits = 24;
        ps.total.store(totalFrames);
        ps.played.store(0);
        ps.startDb.store(0.0);
        ps.volDb.store(0.0);
        ps.gpu = false;

        // 信号链快照：DirectSDM 只有"源 → DoP → 设备"三环，如实写出来
        {
            std::string dev = "默认音频设备";
            if (p.backend == "naa") dev = p.naaHost.empty() ? "NAA 127.0.0.1:43210" : p.naaHost;
            else if (p.backend == "null") dev = "Null 输出（不出声）";
            else for (const auto& d : WasapiOutput::enumerate()) if (d.isDefault) dev = d.name;
            std::ostringstream sig;
            sig << "{\"src\":\"" << jesc(file) << "\""
                << ",\"codec\":\"dsd\""
                << ",\"inRate\":" << srcRate
                << ",\"inBits\":1"
                << ",\"inCh\":" << ch
                << ",\"dur\":" << (double)src.bits / (double)srcRate
                << ",\"note\":\"DirectSDM：1-bit 直通，跳过 PCM 与调制\""
                << ",\"outRate\":" << srcRate
                << ",\"outBits\":24"
                << ",\"outCh\":" << ch
                << ",\"carrier\":" << carrier
                << ",\"device\":\"" << jesc(dev) << "\""
                << ",\"resample\":false,\"twoStage\":false,\"taps\":0"
                << ",\"filter\":\"DirectSDM\""
                << ",\"phaseCode\":\"direct\",\"phase\":\"逐位直通\""
                << ",\"gpuAvailable\":false,\"shaper\":\"none\",\"loudness\":false"
                << ",\"volume\":0,\"buffer\":" << p.bufferMs
                << ",\"dsd\":" << p.dsd
                << ",\"dsdRate\":" << srcRate
                << ",\"modOrder\":0"
                << ",\"ispPeak\":0,\"ispOver\":0,\"preAtten\":0"
                << ",\"atten\":0,\"conv\":false,\"irGpu\":false"
                << ",\"gpuVendor\":\"\",\"gpuStatus\":\"\""
                << ",\"volRamp\":0,\"bits\":24,\"eq\":false"
                << ",\"directSdm\":true"
                << ",\"decodeMs\":" << dsdMs
                << ",\"ffmpegMs\":0"
                << ",\"wavReadMs\":0"
                << ",\"scanMs\":" << Library::instance().lastScanMs()
                << "}";
            recent_add(file, stem_or_self(file), "", "");
            play_log(std::string("PLAY v") + RLPLAYER_VERSION + " " + sig.str());
            meta_on_play(file);   // 播放触发元数据补全(设置可关;inFlight 去重,后台线程)
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.signal = sig.str();
            ps.info = "DirectSDM：DSD 源 " + std::to_string(srcRate) + " Hz → DoP " +
                      std::to_string(carrier) + " Hz（跳过调制，逐位直通）";
        }

        auto sink = make_sink(p);
        if (!sink->open(sink_opts(p), carrier, ch, 24)) {
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "打开输出后端失败: " + sink->last_error();
            return false;
        }
        if (!sink->start()) {
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "启动输出失败: " + sink->last_error();
            sink->close();
            return false;
        }

        const size_t period = sink->buffer_frames() ? sink->buffer_frames() : 4096;
        std::vector<int32_t> pend;
        size_t pendFrames = 0, posFrames = 0, played = 0;
        bool pausedNow = false, eos = false;
        int  timeouts = 0;
        auto lastLog = std::chrono::steady_clock::now();
        rta::dsp::DopPackState packSt;      // 标记相位跨块保持；跳转时也不重置（避免出现 A,A）

        // 从 posFrames 处切 n 个 DoP 帧（n 个 16 位组）打包
        auto append = [&](size_t want) -> bool {
            if (posFrames >= totalFrames) return false;
            const size_t n = std::min(want, totalFrames - posFrames);
            DsdStream chunk;
            chunk.rate = srcRate;
            chunk.channels = ch;
            chunk.bits = (uint64_t)n * 16ull;
            chunk.data.resize((size_t)ch);
            for (int c = 0; c < ch; ++c)
                chunk.data[(size_t)c].assign(src.data[(size_t)c].begin() + (long long)(posFrames * 2),
                                             src.data[(size_t)c].begin() + (long long)((posFrames + n) * 2));
            std::vector<int32_t> packed;
            rta::dsp::dsd_to_dop(chunk, packed, packSt);
            pend.insert(pend.end(), packed.begin(), packed.end());
            pendFrames += n;
            posFrames += n;
            return true;
        };
        // 末尾补"合法的 DoP 静音帧"（0x55 交替）而不是全 0：
        // 全 0 样本的标记位是 0x00，接收端会判定"非法 DoP"并可能静音/报错。
        auto append_silence = [&](size_t n) {
            DsdStream chunk;
            chunk.rate = srcRate;
            chunk.channels = ch;
            chunk.bits = (uint64_t)n * 16ull;
            chunk.data.assign((size_t)ch, std::vector<uint8_t>(n * 2, 0x55));
            std::vector<int32_t> packed;
            rta::dsp::dsd_to_dop(chunk, packed, packSt);
            pend.insert(pend.end(), packed.begin(), packed.end());
            pendFrames += n;
        };

        while (!ps.stop.load()) {
            if (ps.pauseReq.load() != pausedNow) {
                pausedNow = ps.pauseReq.load();
                if (pausedNow) { sink->pause(); ps.paused.store(true); }
                else { sink->resume(); ps.paused.store(false); timeouts = 0; }
            }
            if (pausedNow) { Sleep(30); continue; }

            const double sk = ps.seekReq.exchange(-1.0);
            if (sk >= 0.0) {
                posFrames = std::min(totalFrames, (size_t)(sk * (double)totalFrames + 0.5));
                pend.clear(); pendFrames = 0; eos = false; timeouts = 0;
                played = posFrames;
                ps.played.store(played);
                continue;
            }

            while (!eos && pendFrames < period * 3) {
                if (!append(period)) { eos = true; break; }
                if (posFrames >= totalFrames) { eos = true; break; }
            }
            if (eos && pendFrames > 0 && pendFrames < period)
                append_silence(period - pendFrames);
            if (pendFrames == 0) {
                if (eos) break;
                sink->wait_event(20);
                continue;
            }

            ps.dbgPad.store(sink->last_pad());
            {
                const double full = (double)((int64_t)1 << 23);
                double pk = 0.0; long long cl = 0;
                for (size_t i = 0; i < pendFrames * (size_t)ch; ++i) {
                    const double a = std::fabs((double)pend[i]);
                    if (a > pk) pk = a;
                    if (a >= full - 0.5) ++cl;
                }
                g_dbgPeak.store(pk / full);
                if (cl) g_dbgClip.fetch_add(cl);
                const auto nowT = std::chrono::steady_clock::now();
                if (nowT - lastLog > std::chrono::seconds(5)) {
                    lastLog = nowT;
                    std::ostringstream ls;
                    ls << "STAT pos=" << (double)played / (double)carrier
                       << " rate=" << carrier << " pad=" << ps.dbgPad.load()
                       << " peak=" << g_dbgPeak.load() << " clip=" << g_dbgClip.load()
                       << " fpeak=" << sink->out_peak()   // 后端实际写出峰值（float 域）
                       << " w0=" << g_dbgW0.load() << " wr=" << g_dbgWr.load();
                    play_log(ls.str());
                }
            }
            sink->wait_event(20);
            const size_t w = sink->write_available(pend.data(), pendFrames);
            if (w > 0) {
                pend.erase(pend.begin(), pend.begin() + (long long)(w * (size_t)ch));
                pendFrames -= w;
                played += w;
                ps.played.store(played);
                timeouts = 0;
            } else if (pendFrames >= period) {
                // 手里有数据却 4 秒写不进去 ⟹ 设备异常。DoP 路径不回退共享（重采样会
                // 破坏 DoP 载波），但必须留下完整归因（2026-10-22 事故：静默 break
                // 导致 END pos=0 error=""）。
                if (++timeouts > 200) {
                    std::string devL = "默认音频设备", devId;
                    for (const auto& d : WasapiOutput::enumerate()) {
                        const bool hit = p.device.empty() ? d.isDefault : (d.id == p.device);
                        if (hit) { devL = d.name; devId = d.id; break; }
                    }
                    std::ostringstream fs;
                    fs << "RENDERFAIL 设备流 " << (timeouts * 20 / 1000.0) << " 秒未接受数据（DirectSDM/DoP）"
                       << "（疑似虚拟声卡/独占模式不可用）"
                       << " last_pad=" << sink->last_pad()
                       << " 缓冲=" << period << "帧"
                       << " GetBuffer HRESULT=" << sink->last_gb_hr()
                       << " device=\"" << devL << "\""
                       << " id=" << (devId.empty() ? "(默认)" : devId)
                       << "。DoP/DSD 不能回退共享模式(共享会重采样破坏 DoP 载波),请改用独占可用的设备或 ASIO。";
                    play_log(fs.str());
                    { std::lock_guard<std::mutex> lk(ps.mtx); ps.error = fs.str(); }
                    break;
                }
            }
        }

        ps.paused.store(false);
        Sleep((DWORD)std::max(10, p.bufferMs));   // 让设备把缓冲里最后一点放完
        sink->close();
        // error 非空 = 上面的 RENDERFAIL/渲染失败路径，绝不能当"正常播完"返回 true
        //（否则队列线程会自动续播下一曲，把设备故障掩盖成正常切歌）。
        return !ps.stop.load() && ps.error.empty();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(ps.mtx);
        ps.error = std::string("DirectSDM 异常: ") + e.what();
    } catch (...) {
        std::lock_guard<std::mutex> lk(ps.mtx);
        ps.error = "DirectSDM 未知异常";
    }
    return false;
}

// ---------------- 播放单曲 ----------------
// buf 若已含数据则直接使用（预解码命中），否则现场解码。
// 返回 true = 正常播完；false = 被打断或出错。
//
// sharedFallback（可选出参）：独占流 RENDERFAIL（设备持续不接受数据）且允许回退时置 true，
// 由 play_one 包装层决定是否以 wasapi-shared 重开整条播放；extraNote：回退重入时追加到
// note/ps.info/信号链的说明（"独占流无响应,已回退 WASAPI 共享"）。
static bool play_one_impl(const std::string& file, const PlayParams& p, AudioBuffer& buf,
                          bool* sharedFallback, const std::string& extraNote) {
    auto& ps = g_ps;
    play_log_rotate();
    g_dbgPeak.store(0.0);
    g_dbgClip.store(0);
    apply_dsd_src_settings(p);
    // ---- DirectSDM 判定：必须在解码之前（DSD256 整曲解成 PCM 要几百 MB）----
    // 不满足条件时把原因带到信号链里（不能只写 ps.info —— 紧接着 read_any 会覆盖它）
    std::string directNote;
    std::string bitNote;   // HF-01：位深回退说明（进信号链 JSON，PLAY 日志随之带出）
    if (p.directSdm) {
        const std::string ext = lowerExt(file);
        if (ext == ".dsf" || ext == ".dff") {
            try {
                const DsdInfo info = probe_dsd(file);
                std::string why;
                if (direct_sdm_eligible(p, info, why)) return play_one_direct(file, p);
                directNote = "DirectSDM 未启用：" + why;
            } catch (const std::exception& e) {
                directNote = std::string("DirectSDM 判定失败（回退普通路径）：") + e.what();
            }
        } else {
            directNote = "DirectSDM 只对 DSD 源（.dsf / .dff）有效，本文件按普通路径播放";
        }
    }
    // 系统级时序调优：MMCSS Pro Audio + CPU 亲和 + 定时器 1ms
    RtTune rt = rta::audio::apply_playback_tuning();
    struct RtGuard { RtTune* t; ~RtGuard() { rta::audio::release_playback_tuning(*t); } } rtg{ &rt };
    try {
        // 共享模式不支持 DoP/DSD 输出：共享经系统混音引擎重采样，DoP 载波（0x05/0xFA
        // 标记）被重采样后即非法，接收端判非法并静音 —— 直接明确报错，不降质播放。
        // （DirectSDM 路径在 play_one_direct 里另有一个相同的门。）
        if (p.backend == "wasapi-shared" && p.dsd >= 64) {
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "共享模式不支持 DoP/DSD（共享会重采样破坏 DoP 载波），请切换 WASAPI 独占或 ASIO。";
            return false;
        }
        LoadNote note;
        // HF-03 可观测性：read_any 之前先写 START —— 2.log 那种"END 有 error、前面没有 PLAY"
        // 的失败，靠 START 与 DECODE 的有无就能把故障段定位到解码/临时 WAV。
        ps.decodeMs.store(0);
        ps.ffmpegMs.store(0);
        ps.wavReadMs.store(0);
        ps.dspChunkUs.store(0);
        ps.dspTotalUs.store(0);
        ps.dspChunks.store(0);
        {
            std::lock_guard<std::mutex> lk(g_lastEndMx);
            if (g_lastEnd.isPosZeroNoErr) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - g_lastEnd.tp).count();
                if (elapsedMs < 30000) {
                    play_log("PLAY_ABORT prev=" + g_lastEnd.file + " reason=superseded_by_new_play");
                }
                g_lastEnd.isPosZeroNoErr = false;
            }
        }
        play_log("START file=" + file);
        const auto t_dec_0 = std::chrono::steady_clock::now();
        if (buf.raw.empty()) buf = read_any(file, &note);
        else note.text = "预解码命中（整曲已在内存）";
        const auto t_dec_1 = std::chrono::steady_clock::now();
        if (note.totalMs <= 0.0) {
            note.totalMs = std::chrono::duration<double, std::milli>(t_dec_1 - t_dec_0).count();
        }
        ps.decodeMs.store((long long)(note.totalMs + 0.5));
        ps.ffmpegMs.store((long long)(note.ffmpegMs + 0.5));
        ps.wavReadMs.store((long long)(note.wavReadMs + 0.5));
        AudioBuffer& in = buf;
        if (!note.text.empty()) { std::lock_guard<std::mutex> lk(ps.mtx); ps.info = note.text; }
        if (!directNote.empty()) { std::lock_guard<std::mutex> lk(ps.mtx); ps.info += " ｜ " + directNote; }
        if (ps.stop.load()) return false;
        // 空解码结果不要再往下走到"设备不支持 0 Hz"——那是误导性的错误。
        if (in.rate == 0 || in.frames() == 0) {
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "解码结果为空（文件损坏、格式不支持，或 ffmpeg 解码失败）：" + file;
            return false;
        }
        // 解码成功即记 DECODE（含临时 WAV 之类 note；START 有、DECODE 无 ⟹ 死在解码段）
        {
            std::ostringstream ds;
            ds << "DECODE file=" << file
               << " rate=" << in.rate << " ch=" << in.channels << " bits=" << in.bits
               << " frames=" << in.frames();
            if (!note.text.empty()) ds << " note=\"" << note.text << "\"";
            play_log(ds.str());
        }
        // 位深回退 / 端点回退等"不能悄悄变"的说明都汇进 note（进 ps.info 与信号链 JSON 的 note）
        auto appendNote = [&](const std::string& s) {
            if (s.empty()) return;
            if (!note.text.empty()) note.text += " ｜ ";
            note.text += s;
            std::lock_guard<std::mutex> lk(ps.mtx);
            if (!ps.info.empty()) ps.info += " ｜ ";
            ps.info += s;
        };
        // 独占 RENDERFAIL 回退共享后的重入：把回退原因带进 note/ps.info/信号链
        if (!extraNote.empty()) appendNote(extraNote);
        // ---- HF-02：open 前先核对端点状态。找不到/禁用/拔出 → 日志写明，并回退默认端点 ----
        // 必须在设备能力检查之前做：失效端点会让 exclusive_rates 对任何位深都返回空列表，
        // 否则永远走不到"回退默认端点"这一步（交接单 §2.1 的并行嫌疑）。
        PlayParams pEff = p;
        if (p.backend.empty() || p.backend == "wasapi") {
            if (p.device.empty()) {
                // 空 device = "系统默认端点"。但 probe_formats("") 解析不出任何设备
                //（它只在 id 或名字精确匹配时命中），于是后续独占能力检查拿到空列表，
                // 报出的却是"设备独占模式不支持 44100 Hz…端点不存在或已禁用: 默认端点"——
                // 归因误导（2026-10-23 现场：用户明明有一块 ACTIVE 的 UltraLite-mk5）。
                // 这里先把空 id 解析成真实的 ACTIVE 端点（优先系统默认），后续全线用真实 id。
                const auto devs = WasapiOutput::enumerate();
                const DeviceInfo* pick = nullptr;
                for (const auto& d : devs) {
                    if (d.state != DEVICE_STATE_ACTIVE) continue;
                    if (!pick) pick = &d;
                    if (d.isDefault) { pick = &d; break; }
                }
                if (pick) {
                    pEff.device = pick->id;
                    appendNote("设备未指定，解析为 ACTIVE 端点(" + pick->name + ")");
                }
            }
            if (!p.device.empty()) {
                const FormatProbe ep = WasapiOutput::probe_formats(p.device);
                if (!ep.endpointAlive) {
                    const std::string shown = ep.friendlyName.empty()
                        ? p.device : (ep.friendlyName + " / " + p.device);
                    std::string defName = "默认端点";
                    for (const auto& d : WasapiOutput::enumerate())
                        if (d.isDefault) { defName = d.name + " / " + d.id; break; }
                    play_log("ENDPOINT_FALLBACK 端点不存在或已禁用: " + shown +
                             " → 回退默认端点(" + defName + ")");
                    appendNote("配置端点不存在或已禁用(" + shown + ")，已回退默认端点");
                    pEff.device.clear();     // 空 = WASAPI 默认端点
                }
            }
        }

        // GPU 后端偏好要在建管线之前生效（DLL 是懒加载的）
        rta::dsp::set_gpu_vendor(p.gpuVendor.empty() ? "auto" : p.gpuVendor);
        rta::dsp::GpuBridge::instance().reload();
        dlog("stage gpu-ok");

        eng::PipelineConfig cfg;
        cfg.outBits = (p.bits > 0) ? p.bits : 24;
        if (!p.shaper.empty()) cfg.shaper = parse_shaper(p.shaper);
        cfg.volumeEnabled = true;
        cfg.volumeDb = p.volume;
        cfg.loudnessEnabled = p.loudness;
        if (p.volRamp > 0.0) cfg.volumeRampMs = p.volRamp;
        // GPU：升频与卷积都走显卡（rtagpu.dll 缺失时自动退回 CPU，结果逐位相同）
        cfg.gpuResample = true;

        // ---- 采样率（选择逻辑在 src/audio/rate_pick.h，纯函数、可离线单测）----
        // 只有 WASAPI 独占有"设备支持哪些采样率"可查。NAA / Null 没有本地设备，共享模式由
        // 系统混音率决定，这三者都把 "max" 当"跟随源"（想升频就直接选具体数值）。
        const bool wasapiExcl = (p.backend.empty() || p.backend == "wasapi");
        // 非 WASAPI 独占（NAA / Null / 共享）没有"设备采样率"可查，follow 与 max 一样
        // 归一成"跟随源"；否则 pick_output_rate("follow") 会因为 systemRate==0 退回源率
        // 并打一条 SYSRATE_EMPTY，日志噪音且语义含糊。
        const std::string reqRate = ((p.rate == "max" || p.rate == "follow") && !wasapiExcl)
                                        ? std::string("src") : p.rate;
        // rate=max 的探测带重试（3 次 × 40ms）并刷新"上次成功列表"缓存 —— 上一首独占流刚
        // sink->close() 时 IsFormatSupported 会瞬时返回空，一次结果不足以下结论。
        std::vector<uint32_t> probeRates;
        bool rateStale = false;        // true = 本次探测为空，沿用了缓存里的上次结果
        if (reqRate == "max") {
            probeRates = probe_exclusive_rates(pEff.device, 24, in.channels);
            if (probeRates.empty()) {
                // 三次都空：先看缓存。⚠️ 这里刻意"退回上次结果"而不是退回源率 ——
                // 上一首刚用 96k 播完整首歌，本身就是这个端点支持 96k 的铁证；
                // 一次瞬时探测为空不能把它抹掉（铁律 3：判据要能证明它要证明的事）。
                probeRates = rate_cache_get(pEff.device, 24, in.channels);
                rateStale = !probeRates.empty();
            }
        }
        // ---- rate=follow：跟随【声卡当前配置的采样率】（用户 2026-10-22 功能1）----
        // 首次安装默认就是这一档（见 settings_defaults / loadSettings 的迁移注释）。
        // 取法：GetMixFormat（wasapi_shared.h 已有能力，这里复用它，不开流不出声）。
        // bit-perfect 说明：跟随系统 ≠ 跟随源，44.1k 源在 48k 混音的机器上必然重采样一次，
        // 所以这一档不承诺 bit-perfect；要 bit-perfect 仍选「保持源采样率」。
        // 独占模式下真正的开流率由我们决定，系统率只当"当前配置"的参考值 ——
        // 这正是用户要的"锁定声卡当前设置的采样率"。
        uint32_t sysRate = 0;
        if (reqRate == "follow") {
            uint32_t mr = 0; int mc = 0; bool mf = false; std::string me;
            if (WasapiSharedOutput::query_mix_format(pEff.device, mr, mc, mf, me)) sysRate = mr;
            else play_log("SYSRATE_EMPTY GetMixFormat 失败: " + me);
        }
        const rta::audio::RatePick pick =
            rta::audio::pick_output_rate(reqRate, in.rate, probeRates, sysRate);
        cfg.outRate = pick.rate;
        {
            const std::string devShown = pEff.device.empty() ? std::string("默认端点") : pEff.device;
            if (rateStale) {
                play_log("RATEPROBE_STALE 自动采样率沿用上次结果 " + std::to_string(pick.rate) +
                         " Hz（本次探测为空） device=" + devShown);
                appendNote("自动采样率沿用上次探测结果 " + std::to_string(pick.rate) +
                           " Hz（本次探测为空，端点可能正在重配置）");
            } else if (!pick.logTag.empty()) {
                // 探测为空且无缓存：绝不能当成"探测结果就是源率"（那正是第二首按 44100
                // 开流、在 PLAY 之前就报错退出的事故根因）。logTag 让日志能自证。
                play_log(pick.logTag + " " + pick.note + " device=" + devShown);
                appendNote(pick.note);
            } else if (!pick.note.empty()) {
                appendNote(pick.note);        // 例如采样率参数无法解析
            }
        }
        // 用户是否选了「自动采样率」（max / 跟随源 / 未指定）。
        // ★ 判据必须用 p.rate（用户原始输入），不能用上面的 reqRate —— 非 WASAPI 后端时
        //   reqRate 被归一成 "src"，那是实现细节，不是用户意图。
        // 用途：能力不命中时的行为要分档 ——
        //   自动档的职责是「让声音出来」⟹ 挑一个设备能用的率继续播（否则 44.1k 源在只支持
        //     48k 族的端点上永远没声音，而这类端点很常见）；
        //   明确选择的职责是「如实告诉用户做不到」⟹ 保持硬失败，绝不悄悄改成别的率。
        //   保真播放器不能替用户改主意：错要错得看得见（用户选了 768k 就该知道设备做不了）。
        // follow 也属于「自动档」：它的职责是"让声音出来"，所以设备做不到系统率时
        // 允许按 pick_supported_rate 挑一个能用的（与 max 同族逻辑）。
        const bool rateAuto = (p.rate == "max" || p.rate == "src" || p.rate.empty() ||
                               p.rate == "follow");

        // 自动抖动：按输出位深选型（16 bit 用 NS9，24 bit 用 LNS15，更高位深不整形）
        if (p.shaper == "auto") {
            const int ob = cfg.outBits ? cfg.outBits : in.bits;
            cfg.shaper = (ob <= 16) ? rta::dsp::Shaper::NS9
                       : (ob <= 24) ? rta::dsp::Shaper::LNS15
                                    : rta::dsp::Shaper::None;
        }

        // 过采样峰值（ISP）检测：一次性扫全曲，插值 4 倍取峰值。
        // ★ 必须用 in.sample() 而不是直接读 in.raw：float WAV 的 raw 是 float 的
        //   位模式，按整数解释会得到天文数字的"峰值"（旧代码会因此永远预衰减 3 dB）。
        double preAtten = 0.0;
        double ispPeak = 0.0;
        double srcPeak = 0.0;
        long long ispOver = 0;
        std::vector<std::vector<double>> planes;
        if (p.isp || p.dsd >= 64) {
            planes.resize((size_t)in.channels, std::vector<double>(in.frames()));
            for (size_t n = 0; n < in.frames(); ++n) {
                for (int c = 0; c < in.channels; ++c) {
                    const double v = in.sample(n, c);
                    planes[(size_t)c][n] = v;
                    if (std::fabs(v) > srcPeak) srcPeak = std::fabs(v);
                }
            }
        }
        if (p.isp && !planes.empty()) {
            const auto rep = rta::dsp::detect_isp(planes);
            ispPeak = rep.peak; ispOver = rep.overCount;
            if (rep.peak > 1.0) {
                preAtten = std::max(-3.0, -std::ceil(rep.peakDb * 10.0) / 10.0 - 0.1);
            }
        }
        dlog("stage isp-ok");

        // DSD 调制器余量（2026-10-21）：1-bit 5 阶在 DSD64 上实测最大稳定输入约 0.65
        // （0.70 起环路锁死，输出变成满幅噪音）。这里按本曲真峰值给一个【恒定】预衰减，
        // 只削峰值、不动动态；安静曲目不衰减。不做这一步，任何接近 0 dBFS 的曲目
        // 只要选了 DSD 输出，播到第一个大动态就会"突然变噪音"。
        double modHeadroomDb = 0.0;
        if (p.dsd >= 64) {
            const double peak = std::max(std::max(srcPeak, ispPeak), 1e-9);
            const double afterIsp = peak * std::pow(10.0, preAtten / 20.0);
            modHeadroomDb = rta::dsp::sdm_headroom_db(afterIsp);
            if (modHeadroomDb < 0.0) preAtten += modHeadroomDb;
            if (preAtten < -12.0) preAtten = -12.0;   // 极端峰值也不至于把曲目变哑
        }
        cfg.preAttenDb = preAtten;
        cfg.twoStage = p.twoStage;

        // ---- DSD Native（ASIO）：当前没有 ASIO 后端，如实告知而不是假装在放 ----
        if (p.dsdNative) {
            std::string drivers;
            HKEY hk = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
                for (DWORD i = 0;; ++i) {
                    wchar_t name[256];
                    DWORD len = 256;
                    if (RegEnumKeyExW(hk, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    char buf[512] = {0};
                    WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
                    if (!drivers.empty()) drivers += "、";
                    drivers += buf;
                }
                RegCloseKey(hk);
            }
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "DSD Native 还没实现：要把 1-bit 码流按 ASIO 的 DSD 采样类型（ASIOSTDSDInt8*）直接送出，"
                       "而当前管线输出的是 int32 PCM（DoP 封装）。" +
                       (drivers.empty() ? std::string("本机也没有 ASIO 驱动。")
                                        : ("本机 ASIO 驱动：" + drivers + "。")) +
                       "DoP 走 PCM 通道，ASIO / WASAPI 都能承载。";
            return false;
        }

        // ---- DSD（DoP）：管线先升到 DSD 速率，再调制 + 打包成 24-bit 载波 ----
        const bool dop = (p.dsd >= 64);
        uint32_t deviceRate = cfg.outRate;            // 设备真正打开的采样率
        uint32_t dsdRate = 0;
        if (dop) {
            const uint32_t base = (in.rate % 48000u == 0) ? 48000u : 44100u;
            dsdRate    = rta::dsp::dsd_rate(p.dsd, base);
            deviceRate = dsdRate / 16u;               // DoP 载波
            cfg.outRate = dsdRate;
            cfg.outBits = 24;                         // DoP 固定 24-bit 容器
            cfg.shaper  = rta::dsp::Shaper::None;     // 抖动交给调制器之后的 1-bit 量化
        }

        // 设备能力检查（仅 WASAPI）。HF-01/02/04 接线（2026-10-22）：
        //   * rates.empty() 一律当「不支持/端点异常」，绝不静默放行（交接单 §6）；
        //   * 位深候选序列：请求位深 → 源位深(8/16/24/32) → 16 → 32，去重后依序检查，
        //     首个「独占支持列表含目标率」的位深生效（记 effectiveBits → cfg.outBits）；
        //   * 全不命中 → 用 probe_formats 拿三档列表，错误含 deviceId、请求 rate/bits、
        //     16/24/32 各自支持列表（不再只丢 UNSUPPORTED_FORMAT）；
        //   * 降位深必须在信号链 note 与 PLAY 日志明示，不能悄悄变（交接单 §3 风险注）；
        //   * deviceRate==0（解析不出设备率）防御：错误带 inRate/frames/rate/dsd（HF-04）。
        if (p.backend.empty() || p.backend == "wasapi") {
            // deviceRate 为 0 表示"跟随源"——设备真正会打开的率是 in.rate（与下方 outRate 解析一致）。
            // DoP 时 deviceRate 已是 DoP 载波，必非 0。
            const uint32_t wantDevRate = dop ? deviceRate : (deviceRate ? deviceRate : in.rate);
            if (wantDevRate == 0) {
                std::lock_guard<std::mutex> lk(ps.mtx);
                char eb[256];
                std::snprintf(eb, sizeof(eb),
                              "输出采样率解析为 0（防御，拒绝开流）：inRate=%u frames=%zu rate=%s dsd=%d",
                              (unsigned)in.rate, in.frames(), p.rate.c_str(), p.dsd);
                ps.error = eb;
                return false;
            }
            // 错误文案带上源采样率与后端：日志要能自证（哪首源、哪个后端、请求的什么率）
            const std::string srcDesc = "（源采样率 " + std::to_string(in.rate) + " Hz，后端 " +
                                        (p.backend.empty() ? std::string("wasapi") : p.backend) + "）";
            if (dop) {
                // DoP 路径：24-bit 容器固定，不参与位深回退（维持现状）；空列表不再当成功。
                // 载波率不可变（DoP 的 0x05/0xFA 标记依赖精确的 DSD/16），所以这里保持硬失败。
                // 探测带重试 + 缓存兜底：DoP 的载波率不可变（不换率），但"探测为空"绝不等于
                // "设备不支持" —— 一次瞬时为空不能让 DSD 播放被误判成硬失败。
                const auto rates = probe_rates_cached(pEff.device, 24, in.channels, "DoP 载波率检查");
                bool ok = false;
                for (auto r : rates) if (r == deviceRate) ok = true;
                if (!ok) {
                    std::string list;
                    for (size_t i = 0; i < rates.size(); ++i) { if (i) list += " / "; list += std::to_string(rates[i]); }
                    std::lock_guard<std::mutex> lk(ps.mtx);
                    ps.error = (rates.empty()
                        ? ("设备独占支持列表为空（端点不可用或 24-bit 容器不被接受），无法确认 DoP 所需的 " +
                           std::to_string(deviceRate) + " Hz（DSD" + std::to_string(p.dsd) + "）。")
                        : ("设备不支持 DoP 所需的 " + std::to_string(deviceRate) + " Hz（DSD" +
                           std::to_string(p.dsd) + "）。设备支持：" + list)) +
                        "；端点 id=" + (pEff.device.empty() ? std::string("(默认端点)") : pEff.device) + srcDesc;
                    return false;
                }
            } else {
                const int reqBits = (p.bits > 0) ? p.bits : 24;
                int bitsCand[4];
                int nBits = 0;
                auto pushBit = [&](int b) {
                    if (b != 8 && b != 16 && b != 24 && b != 32) return;   // 候选只认合法位深
                    for (int i = 0; i < nBits; ++i) if (bitsCand[i] == b) return;   // 去重
                    if (nBits < 4) bitsCand[nBits++] = b;
                };
                pushBit(reqBits);                                          // ① 请求位深（默认 24）
                if (in.bits == 8 || in.bits == 16 || in.bits == 24 || in.bits == 32)
                    pushBit(in.bits);                                      // ② 源位深
                pushBit(16);                                               // ③ 16
                pushBit(32);                                               // ④ 32
                int effBits = 0;
                // 位深循环里每个候选位深的支持列表都留一份：目标率不命中时，自适应换率
                // 要在这三档位深里挑，不能只试最后一个（否则可能白丢一个本来能用的位深）
                std::vector<std::vector<uint32_t>> ratesByBits((size_t)std::max(nBits, 1));
                for (int i = 0; i < nBits && effBits == 0; ++i) {
                    // 探测带重试 + 缓存兜底：独占流刚 close 时 IsFormatSupported 会瞬时返回空列表，
                    // 空列表若被当成"该位深什么都不支持"，即使 rate=max 靠缓存拿到了率也照样硬失败
                    const auto rates = probe_rates_cached(pEff.device, bitsCand[i], in.channels, "位深能力检查");
                    if (i < (int)ratesByBits.size()) ratesByBits[(size_t)i] = rates;
                    if (rates.empty()) continue;     // 该位深独占全不支持/端点异常 → 试下一位深
                    for (auto r : rates) if (r == wantDevRate) { effBits = bitsCand[i]; break; }
                }
                if (effBits == 0 && rateAuto) {
                    // ---- 自适应换率：只用于「自动/跟随源」档 ----
                    // 为什么自动档要换：44.1k 源 + 只支持 48k/96k 族的端点是很常见的组合
                    //（本机就是这样），硬失败等于这类端点永远放不出声音。降一点总比不出声好。
                    // 为什么明确选择档【不换】：用户选了 96000 / 768000 就是要在那个率上听，
                    // 设备做不到时必须如实报错（见下方 effBits==0 的硬失败分支），
                    // 替用户悄悄改成别的率等于骗他 —— 这是保真播放器，不是"能响就行"的播放器。
                    // 判据用纯函数 pick_supported_rate（同族优先，避免 44.1k 换到 48k 族引入
                    // 非整数比重采样），并把它钉在 test_ratepick.cpp 里。
                    // 取【第一个非空】位深列表（位深优先于换率：能保 24-bit 就不降到 16-bit；
                    // 而且若该列表恰好含源率，就等于完全不用重采样 —— 最优解）
                    int      effBitsFB = 0;
                    uint32_t pickedRate = 0;
                    std::string pickedWhy;
                    for (int i = 0; i < nBits && effBitsFB == 0; ++i) {
                        const std::vector<uint32_t>& rates = (i < (int)ratesByBits.size())
                            ? ratesByBits[(size_t)i] : ratesByBits[0];
                        uint32_t outR = wantDevRate;
                        const auto dec = rta::audio::decide_capability_failure(rateAuto, wantDevRate, rates, outR, in.rate);
                        if (dec.action == rta::audio::CapAction::FallbackRate && outR > 0) {
                            effBitsFB = bitsCand[i];
                            pickedRate = outR;
                            pickedWhy = dec.userMessage;
                        }
                    }
                    if (effBitsFB != 0 && pickedRate != 0) {
                        // 设备真正打开的是 pickedRate：cfg.outRate 与 deviceRate 必须同时改。
                        // deviceRate 在 cfg.outRate 之后初始化、又在 !dop 时按 outRate 重算，
                        // 两边都改才能保证 sink->open / STAT / totalOut 三者一致（见报告里的核对链）。
                        // 位深与抖动选型交给下方既有逻辑（effBits 赋值后统一处理），不在这里重复。
                        cfg.outRate = pickedRate;
                        deviceRate  = pickedRate;   // 本分支只在 !dop 时进入（DoP 走上面的硬失败）
                        // 不能悄悄换率：note / ps.info / 信号链 JSON 都要写明（HF-01 同理）
                        std::string why = pickedWhy.empty()
                                          ? ("目标 " + std::to_string(wantDevRate) + " Hz 独占不支持，已用 " +
                                             std::to_string(pickedRate) + " Hz（源 " +
                                             std::to_string(in.rate) + " Hz 重采样，非 bit-perfect）")
                                          : pickedWhy;
                        appendNote(why);
                        bitNote = bitNote.empty() ? why : (bitNote + "；" + why);
                        play_log("RATEFALLBACK " + why + " bits=" + std::to_string(effBitsFB) +
                                 " device=" + (pEff.device.empty() ? std::string("默认端点") : pEff.device));
                        // 走到这里说明已选定位深与采样率，跳过下面的硬失败
                        effBits = effBitsFB;
                    }
                }
                if (effBits == 0) {
                    // 全不命中：probe_formats 拿三档列表，错误带端点 id 与请求值
                    const FormatProbe pr = WasapiOutput::probe_formats(pEff.device);
                    auto join = [](const std::vector<uint32_t>& v) {
                        std::string s;
                        for (size_t k = 0; k < v.size(); ++k) { if (k) s += "/"; s += std::to_string(v[k]); }
                        return s.empty() ? std::string("无") : s;
                    };
                    std::ostringstream es;
                    es << "设备独占模式不支持 " << wantDevRate << " Hz（请求位深 " << reqBits
                       << "-bit，候选 16/24/32 全不命中）。";
                    if (!pr.endpointAlive)
                        es << "端点不存在或已禁用: "
                           << ((pr.friendlyName.empty() && pr.deviceId.empty())
                                   ? (pEff.device.empty() ? std::string("默认端点") : pEff.device)
                                   : pr.friendlyName + " / " + pr.deviceId)
                           << "。";
                    es << "端点 id=" << (pr.deviceId.empty() ? "(未找到)" : pr.deviceId)
                       << "；独占支持 16-bit: " << join(pr.rates16)
                       << "；24-bit: " << join(pr.rates24)
                       << "；32-bit: " << join(pr.rates32)
                       << srcDesc;    // 源采样率 + 后端：日志要能自证是哪首源、哪个后端
                    if (!rateAuto) {
                        // 明确选择档：绝不自动改率，但必须让用户清楚「做不到的是你选的那个率」
                        const auto dec = rta::audio::decide_capability_failure(false, wantDevRate, {});
                        es << "。" << dec.userMessage;
                    }
                    std::lock_guard<std::mutex> lk(ps.mtx);
                    ps.error = es.str();
                    return false;
                }
                if (effBits != reqBits) {
                    // 不能悄悄降位深：note 进 ps.info 与信号链 JSON（PLAY 日志随之带出）
                    const std::string bn = "实际输出 " + std::to_string(effBits) + "-bit（设备不支持 " +
                                           std::to_string(reqBits) + "-bit 独占）";
                    appendNote(bn);
                    // 追加而不是覆盖：上面可能已经写过换采样的 RATEFALLBACK 说明，不能丢掉
                    bitNote = bitNote.empty() ? bn : (bitNote + "；" + bn);
                }
                cfg.outBits = effBits;
                if (p.shaper == "auto") {
                    // 位深变了，自动抖动选型跟着变（16→NS9，24→LNS15，更高不整形）
                    cfg.shaper = (cfg.outBits <= 16) ? rta::dsp::Shaper::NS9
                               : (cfg.outBits <= 24) ? rta::dsp::Shaper::LNS15
                                                     : rta::dsp::Shaper::None;
                }
            }
        }

        // ---- WASAPI 共享后端：格式由系统混音引擎（GetMixFormat）决定 ----
        // 不走独占能力检查；输出采样率与声道一律对齐混音格式：
        //   * 管线只做一次重采样到 mix_rate，系统引擎不再二次重采样（避免双重降质）；
        //   * 声道对齐 mix_channels（管线负责 downmix/upmix），否则共享开流直接错乱。
        uint32_t mixRate = 0;
        int      mixCh   = 0;
        bool     mixFloat = false;
        if (p.backend == "wasapi-shared") {
            std::string mixErr;
            if (!WasapiSharedOutput::query_mix_format(pEff.device, mixRate, mixCh, mixFloat, mixErr)) {
                std::lock_guard<std::mutex> lk(ps.mtx);
                ps.error = "读取系统混音格式失败（共享模式）：" + mixErr +
                           "；端点=" + (pEff.device.empty() ? "(默认)" : pEff.device);
                return false;
            }
            if (!mixFloat) {
                std::lock_guard<std::mutex> lk(ps.mtx);
                ps.error = "系统混音格式不是 float32，共享模式暂不支持；请使用 WASAPI 独占。";
                return false;
            }
            const uint32_t wantRate = cfg.outRate ? cfg.outRate : (uint32_t)in.rate;
            if (wantRate != mixRate)
                appendNote("共享模式按系统混音率 " + std::to_string(mixRate) + " Hz 输出（请求 " +
                           std::to_string(wantRate) + " Hz，非 bit-perfect）");
            cfg.outRate = mixRate;                     // 下方 outRate/deviceRate 随之取 mixRate
            if (p.outChannels > 0 && p.outChannels != mixCh)
                appendNote("共享模式输出声道按系统混音 " + std::to_string(mixCh) + " ch（请求 " +
                           std::to_string(p.outChannels) + " ch 已忽略）");
            pEff.outChannels = mixCh;                  // 下方 outCh 取 pEff —— 管线负责声道变换
        }

        // 滤波器：1x 与 Nx 可以分开选（HQPlayer 的标准做法：
        // 44.1/48k 用陡一点的，>=88.2k 用短一点的，避免过度处理）
        // DSD 输出时优先用 SDM 页那一组滤波器（与 PCM 独立）；没选就沿用 PCM 的
        std::string fname;
        if (p.dsd >= 64) {
            fname = (in.rate >= 88200)
                ? (!p.sdmFilterNx.empty() ? p.sdmFilterNx : (!p.filterNx.empty() ? p.filterNx : p.sdmFilter))
                : (!p.sdmFilter.empty() ? p.sdmFilter : p.filter);
        } else {
            fname = (in.rate >= 88200 && !p.filterNx.empty()) ? p.filterNx : p.filter;
        }
        if (!fname.empty()) {
            const auto* fp = rta::dsp::find_preset(fname, in.rate >= 88200);
            if (fp) {
                cfg.taps = fp->taps; cfg.transition = fp->transition; cfg.attenDb = fp->attenDb;
                cfg.filterWindow = fp->window; cfg.filterPhase = fp->phase;
                cfg.twoStage = fp->twoStage;
                if (fp->twoStage && fp->stage2Taps > 0) cfg.stage2Taps = fp->stage2Taps;
            }
        }

        // ---- 手动覆盖（设置页里可以逐项调，不再受预设限制）----
        if (p.taps > 0)       cfg.taps = std::max(1024, p.taps);
        if (p.atten > 0.0)    cfg.attenDb = std::min(300.0, std::max(60.0, p.atten));
        if (p.transition > 0.0) cfg.transition = std::min(0.45, std::max(0.005, p.transition));
        if (p.phase == "minimum") cfg.filterPhase = rta::dsp::PhaseKind::Minimum;
        else if (p.phase == "linear") cfg.filterPhase = rta::dsp::PhaseKind::Linear;
        else if (p.phase == "intermediate") cfg.filterPhase = rta::dsp::PhaseKind::Intermediate;

        // ---- EQ（REW / AutoEq 文本）：在【输入采样率】上工作（EQ 位于重采样之前）----
        bool eqLoaded = false;
        if (!p.eqFile.empty()) {
            std::ifstream ef(p.eqFile);
            if (!ef) {
                ef.open(path_in_exe_dir(p.eqFile.c_str()));
            }
            if (ef) {
                std::string text((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
                // 去掉 UTF-8 BOM
                if (text.size() > 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB) text.erase(0, 3);
                try {
                    auto spec = rta::dsp::parse_eq_text(text);
                    auto chain = rta::dsp::build_eq_filters(spec, (double)in.rate);
                    if (!chain.empty() || spec.preampDb != 0.0) {
                        const size_t maxCh = (size_t)std::max(in.channels, 2);
                        if (cfg.channel.size() < maxCh) cfg.channel.resize(maxCh);
                        for (size_t c = 0; c < maxCh; ++c) {
                            cfg.channel[c].eq = chain;
                            if (spec.preampDb != 0.0) cfg.channel[c].gainDb += spec.preampDb;
                        }
                        eqLoaded = true;
                    }
                } catch (...) { /* 解析失败就当没配 */ }
            }
        }

        // ---- 卷积 / 房间校正：IR 是 WAV，按输出声道数铺开 ----
        bool convLoaded = false;
        if (!p.convIr.empty()) {
            try {
                AudioBuffer irb = read_wav(p.convIr);
                if (!irb.raw.empty()) {
                    const size_t maxCh = (size_t)std::max(in.channels, 2);
                    cfg.channel.resize(maxCh);
                    for (size_t c = 0; c < maxCh; ++c) {
                        auto& dst = cfg.channel[c];
                        dst.ir.assign(irb.frames(), 0.0);
                        const int srcCh = (irb.channels > 1) ? (int)(c % (size_t)irb.channels) : 0;
                        for (size_t n = 0; n < irb.frames(); ++n) dst.ir[n] = irb.sample(n, srcCh);
                        dst.useGpu = p.convGpu;
                    }
                    convLoaded = true;
                }
            } catch (...) { /* IR 读失败就当没配 */ }
        }

        // ---------------- 流式渲染 ----------------
        // 生产者线程：分块【解码 -> DSP】-> 有界队列；本线程：按设备事件把队列写进设备。
        // 为什么必须流式：整曲一次性处理时，705.6 kHz 下 5 分钟的输出缓冲就是 1.7 GB，
        // 既会 OOM，VirtualLock 整块更会把系统拖死。分块后内存恒定在几秒量级，
        // 起播延迟也从「整曲处理时间」降到「一块的处理时间」。
        rta::render::RenderQ RQ;

        // outRate = 管线输出（DSD 时是 DSD 速率）；deviceRate = 设备真正打开的采样率
        // （DSD 时是 DoP 载波 = DSD/16）
        const uint32_t outRate  = cfg.outRate ? cfg.outRate : in.rate;
        if (!dop) deviceRate = outRate;
        // 输出声道数：0 = 跟随源。5.1/7.1 需要设备与文件都支持。
        // 共享后端：pEff.outChannels 已被强制为系统混音声道数（见上方共享归一块）。
        const int      outCh    = (pEff.outChannels > 0) ? pEff.outChannels : in.channels;
        if (outCh != in.channels) cfg.outChannels = outCh;
        {
            // 每声道增益：左右平衡 / 多声道校正
            int need = 0;
            for (int c = 0; c < outCh && c < 8; ++c) if (p.trim[c] != 0.0) need = c + 1;
            if (need > 0) {
                if ((int)cfg.channel.size() < need) cfg.channel.resize((size_t)need);
                for (int c = 0; c < need; ++c) cfg.channel[(size_t)c].gainDb = p.trim[c];
            }
        }
        const int      outBits  = cfg.outBits ? cfg.outBits : 24;   // 跟随设置里的输出位深
        const size_t   totalOut = (size_t)((double)in.frames() * (double)deviceRate / (double)in.rate + 0.5);
        ps.outRate = deviceRate; ps.outBits = outBits;
        ps.total.store(totalOut);

        // 实际端点名称/id：信号链 JSON 与 RENDERFAIL 遥测共用同一来源（HF-02 + 本轮事故归因）
        std::string dev, devId;
        for (const auto& d : WasapiOutput::enumerate()) {
            const bool hit = pEff.device.empty() ? d.isDefault : (d.id == pEff.device);
            if (hit) { dev = d.name; devId = d.id; break; }
        }
        if (dev.empty()) dev = pEff.device.empty() ? "默认音频设备" : pEff.device;

        // 信号链快照：源文件 -> 解码 -> 重采样 -> 抖动 -> 音量 -> 设备
        {
            const MediaInfo mi = probe_media(file);
            // HF-02：device/deviceId 反映【实际使用的端点】（含回退默认端点后的真实身份），
            // 不再无条件下写成默认设备 —— 1.log 那种"只有友好名没有 id"的日志无法区分端点失效。
            // DSD 源抽取设置（P0-5）：信号链里如实写出来，便于核对"选的是不是真生效"
            const rta::dsp::DsdConversion dsdConv = rta::dsp::parse_conversion(p.dsdConv);
            const std::string dsdConvName = rta::dsp::conversion_name(dsdConv);
            const double dsdPassbandHz = rta::dsp::conversion_passband(dsdConv);
            std::ostringstream sig;
            sig << "{\"src\":\"" << jesc(file) << "\""
                << ",\"codec\":\"" << jesc(mi.codec.empty() ? "wav" : mi.codec) << "\""
                << ",\"inRate\":" << in.rate
                << ",\"inBits\":" << in.bits
                << ",\"inCh\":" << in.channels
                << ",\"dur\":" << (double)in.frames() / (double)in.rate
                << ",\"note\":\"" << jesc(note.text) << "\""
            << ",\"outRate\":" << outRate
            << ",\"outBits\":" << outBits
            << ",\"outCh\":" << outCh
            << ",\"device\":\"" << jesc(dev) << "\""
            << ",\"deviceId\":\"" << jesc(devId) << "\""
            << ",\"bitsFallback\":\"" << jesc(bitNote) << "\""
                << ",\"resample\":" << (outRate != in.rate ? "true" : "false")
                << ",\"twoStage\":" << ((cfg.twoStage && outRate >= in.rate * 8) ? "true" : "false")
                << ",\"taps\":" << cfg.taps
                << ",\"filter\":\"" << jesc(fname.empty() ? std::string("Kaiser sinc") : fname) << "\""
                << ",\"phaseCode\":\"" << (cfg.filterPhase == rta::dsp::PhaseKind::Minimum ? "minimum"
                                            : (cfg.filterPhase == rta::dsp::PhaseKind::Intermediate ? "intermediate" : "linear")) << "\""
                << ",\"gpuAvailable\":" << (rta::dsp::GpuBridge::instance().available() ? "true" : "false")
                << ",\"phase\":\"" << (cfg.filterPhase == rta::dsp::PhaseKind::Minimum ? "最小相位"
                                  : (cfg.filterPhase == rta::dsp::PhaseKind::Intermediate ? "混合相位" : "线性相位")) << "\""
                << ",\"shaper\":\"" << jesc(p.shaper == "auto" ? std::string(rta::dsp::shaper_name(cfg.shaper))
                                                          : (p.shaper.empty() ? std::string("none") : p.shaper)) << "\""
                << ",\"loudness\":" << (p.loudness ? "true" : "false")
                << ",\"volume\":" << p.volume
                << ",\"buffer\":" << p.bufferMs
                << ",\"dsdKind\":\"" << jesc(p.dsdKind.empty() ? std::string("fir") : p.dsdKind) << "\""
                << ",\"dsdConv\":\"" << jesc(dsdConvName) << "\""
                << ",\"dsdPassband\":" << dsdPassbandHz
                << ",\"dsd\":" << (dop ? p.dsd : 0)
                << ",\"dsdRate\":" << (dop ? dsdRate : 0)
                << ",\"modFamily\":\"" << jesc(p.modFamily.empty() ? std::string("dsd") : p.modFamily) << "\""
                 << ",\"modOrder\":" << (dop ? (p.modOrder > 0 ? p.modOrder : 5) : 0)
                << ",\"ispPeak\":" << ispPeak
                << ",\"ispOver\":" << ispOver
                << ",\"srcPeak\":" << srcPeak
                << ",\"modHeadroom\":" << modHeadroomDb
                << ",\"preAtten\":" << preAtten
                << ",\"atten\":" << cfg.attenDb
                << ",\"conv\":" << (convLoaded ? "true" : "false")
                << ",\"irGpu\":" << (p.convGpu ? "true" : "false")
                << ",\"gpuVendor\":\"" << jesc(p.gpuVendor) << "\""
                << ",\"gpuStatus\":\"" << jesc(rta::dsp::GpuBridge::instance().status()) << "\""
                << ",\"volRamp\":" << cfg.volumeRampMs
                << ",\"bits\":" << outBits
                << ",\"eq\":" << (eqLoaded ? "true" : "false")
                << ",\"directSdm\":false"
                << ",\"directNote\":\"" << jesc(directNote) << "\""
                << ",\"decodeMs\":" << note.totalMs
                << ",\"ffmpegMs\":" << note.ffmpegMs
                << ",\"wavReadMs\":" << note.wavReadMs
                << ",\"scanMs\":" << Library::instance().lastScanMs()
                << "}";
            recent_add(file, mi.title.empty() ? stem_or_self(file) : mi.title,
                       mi.artist, mi.album);
            dlog("stage recent-ok");
            play_log(std::string("PLAY v") + RLPLAYER_VERSION + " " + sig.str());
            dlog("stage playlog-ok");
            meta_on_play(file);   // 播放触发元数据补全(设置可关;inFlight 去重,后台线程)
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.signal = sig.str();
        }
        ps.startDb.store(p.volume);
        dlog("stage sig-ok");
        // ★ 必须无条件重置：旧代码只在 p.volume != 0 时写 volDb，导致上一曲
        //   拖过音量后，下一曲的实时增益 = 旧 volDb - 新 startDb（凭空多减几十 dB）。
        ps.volDb.store(p.volume);

        std::thread prod([&]() {
            const size_t kChunk = 4096;
            uint64_t myEpoch = RQ.epoch.load();
            auto pl = std::make_unique<eng::Pipeline>(cfg);
            size_t pos = 0, skipOut = 0;

            // ---- DSD：每声道一个调制器（状态必须跨块保持）+ DoP 打包残样 ----
            std::vector<std::unique_ptr<rta::dsp::SdmModulator>> mods;
            rta::dsp::DopPackState dopSt;
            std::vector<std::vector<double>> carry;
            if (dop) {
                rta::dsp::SdmConfig mcfg;
                mcfg.fs = (double)dsdRate;
                mcfg.family = rta::dsp::parse_modulator_family(p.modFamily);
                mcfg.levels = 2;                 // 1-bit (DoP)
                mcfg.order = rta::dsp::stable_sdm_order(p.modOrder > 0 ? p.modOrder : 5, mcfg.fs, mcfg.levels, mcfg.family);
                mcfg.cutoffHz = (p.modCutoff > 0.0) ? (p.modCutoff * 1000.0) : 100000.0;
                // 默认 0.0005：1-bit 的量化步长是 2.0，旧默认 0.01 等于注入 -34 dBFS 白噪，
                // 实测把 DSD64 的带内 SNR 从 89 dB 砍到 54 dB（听感：持续嘶声）。
                mcfg.ditherLsb = (p.modDither >= 0.0) ? p.modDither : 0.0005;
                for (int c = 0; c < outCh; ++c) mods.push_back(std::make_unique<rta::dsp::SdmModulator>(mcfg));
                carry.assign((size_t)outCh, {});
            }
            try {
                for (;;) {
                    if (RQ.abort.load()) break;
                    const long long sf = RQ.seekFrame.exchange(-1);
                    if (sf >= 0) {
                        const size_t target  = (size_t)sf;
                        const size_t preroll = std::min<size_t>(target, (size_t)(0.5 * (double)in.rate));
                        pos = target - preroll;
                        skipOut = (size_t)((double)preroll * (double)outRate / (double)in.rate + 0.5);
                        // 清空滤波历史（不是重建管线：重建要重新设计 26 万抽头滤波器，秒级卡顿）
                        pl->seek_reset();

                        { std::lock_guard<std::mutex> lk(RQ.mtx); RQ.q.clear(); }
                        myEpoch = RQ.epoch.load();
                        RQ.cv.notify_all();
                    }
                    if (pos >= in.frames()) break;
                    const size_t n = std::min(kChunk, in.frames() - pos);
                    AudioBuffer c;
                    c.rate = in.rate; c.channels = in.channels; c.bits = in.bits; c.isFloat = in.isFloat;
                    c.raw.assign(in.raw.begin() + (long long)(pos * (size_t)in.channels),
                                 in.raw.begin() + (long long)((pos + n) * (size_t)in.channels));
                    pos += n;
                    const auto t_dsp_0 = std::chrono::steady_clock::now();
                    AudioBuffer o = pl->process(c, nullptr);
                    if (!RQ.gpu.load()) RQ.gpu.store(pl->gpuResampleActive());
                    if (skipOut > 0 && !o.raw.empty()) {
                        const size_t d = std::min(skipOut, o.frames());
                        o.raw.erase(o.raw.begin(), o.raw.begin() + (long long)(d * (size_t)o.channels));
                        skipOut -= d;
                    }
                    // ---- DSD：PCM(DSD 速率) -> 1-bit -> DoP 24-bit 载波 ----
                    if (dop && !o.raw.empty()) {
                        const size_t n = o.frames();
                        std::vector<std::vector<double>> sd((size_t)outCh);
                        for (int c = 0; c < outCh; ++c) {
                            auto& v = sd[(size_t)c];
                            v = carry[(size_t)c];                    // 接着上一块的残余
                            v.reserve(v.size() + n);
                            for (size_t i = 0; i < n; ++i) v.push_back(mods[(size_t)c]->process(o.sample(i, c)));
                        }
                        const size_t total = sd[0].size();
                        const size_t use = (total / 16) * 16;        // 只打包整数个 DoP 帧
                        std::vector<const double*> sp;
                        for (auto& v : sd) sp.push_back(v.data());
                        std::vector<int32_t> packed;
                        rta::dsp::dop_pack(sp.data(), outCh, use, packed, dopSt);
                        for (int c = 0; c < outCh; ++c)
                            carry[(size_t)c].assign(sd[(size_t)c].begin() + (long long)use, sd[(size_t)c].end());
                        AudioBuffer nb;
                        nb.rate = deviceRate; nb.channels = outCh; nb.bits = 24; nb.isFloat = false;
                        nb.raw = std::move(packed);
                        o = std::move(nb);
                    }
                    const auto t_dsp_1 = std::chrono::steady_clock::now();
                    const long long dsp_us = std::chrono::duration_cast<std::chrono::microseconds>(t_dsp_1 - t_dsp_0).count();
                    ps.dspChunkUs.store(dsp_us);
                    ps.dspTotalUs.fetch_add(dsp_us, std::memory_order_relaxed);
                    ps.dspChunks.fetch_add(1, std::memory_order_relaxed);
                    if (o.raw.empty()) continue;
                    {
                        std::unique_lock<std::mutex> lk(RQ.mtx);
                        RQ.cv.wait(lk, [&] { return RQ.q.size() < RQ.cap || RQ.abort.load(); });
                        if (RQ.abort.load()) break;
                        RQ.q.emplace_back(std::move(o), myEpoch);
                    }
                    RQ.cv.notify_all();
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(RQ.mtx); RQ.err = e.what(); RQ.error = true;
            } catch (...) {
                std::lock_guard<std::mutex> lk(RQ.mtx); RQ.err = "未知异常"; RQ.error = true;
            }
            { std::lock_guard<std::mutex> lk(RQ.mtx); RQ.done = true; }
            RQ.cv.notify_all();
        });

        // 必须等第一块数据就绪再开流。实测踩到的坑：WASAPI 独占事件驱动在 Start 之后
        // 迟迟拿不到数据，驱动会判定欠载、此后【停止发事件】，GetBuffer 一直失败，
        // 表现为「播放中但一直停在 0 秒」。旧版之所以没这个问题，是因为它先把整曲处理完
        // 才 open/start —— 那份缓冲本身就是"预填充"。
        {
            std::unique_lock<std::mutex> lk(RQ.mtx);
            RQ.cv.wait_for(lk, std::chrono::seconds(15),
                           [&] { return !RQ.q.empty() || RQ.done || RQ.abort.load(); });
            if (RQ.q.empty()) {
                RQ.abort.store(true); RQ.cv.notify_all(); prod.join();
                // 归因（本轮补）：首块 15 秒未产出发生在【输出设备打开之前】，
                // 与音频设备无关 —— 解码、滤波器设计（大抽头数）、CPU 过载都会导致。
                const std::string msg = RQ.err.empty()
                    ? "渲染线程未产出数据（解码/DSP 管线 15 秒无首块输出；尚未到输出设备环节，"
                      "与音频设备无关。文件过大、滤波器抽头过多或 CPU 过载都会导致）"
                    : ("渲染失败: " + RQ.err);
                play_log("NORENDER kind=producer-first-block " + msg + " file=" + file);
                std::lock_guard<std::mutex> lk2(ps.mtx);
                ps.error = msg;
                return false;
            }
        }

        // 输出后端：WASAPI 独占 / 共享 / ASIO / NAA / Null，由 make_sink 决定（pEff：可能已回退默认端点）
        auto sink = make_sink(pEff);
        if (!sink->open(sink_opts(pEff), deviceRate, outCh, outBits)) {
            RQ.abort.store(true); RQ.cv.notify_all(); prod.join();
            std::lock_guard<std::mutex> lk(ps.mtx);
            std::string msg = "打开输出后端失败: " + sink->last_error();
            // HF-01/02：open 失败把第一刀的 OpenFail（请求格式/支持列表/HRESULT/端点 id）拼进错误，
            // 不再只剩一句 UNSUPPORTED_FORMAT。
            if (auto* w = dynamic_cast<WasapiSink*>(sink.get())) {
                const OpenFail& f = w->fail();
                msg += "；请求[" + (f.request.empty() ? std::string("(未记录)") : f.request) + "]";
                msg += "；该位深独占支持[" + (f.supportedList.empty()
                            ? std::string("空（端点不可用或不支持该位深）") : f.supportedList) + "]";
                msg += "；HRESULT " + (f.hresultText.empty() ? std::string("(未记录)") : f.hresultText);
                msg += "；端点 id=" + (f.deviceId.empty() ? std::string("(默认)") : f.deviceId);
                if (f.hresultText.find("DEVICE_INVALIDATED") != std::string::npos)
                    msg = "音频设备已失效（AUDCLNT_E_DEVICE_INVALIDATED），请在设置里重新选择输出设备。" + msg;
            }
            if (!note.text.empty()) msg += "；note=" + note.text;
            ps.error = msg;
            return false;
        }
        // 2026-10-23 真机 DEBUG：打开成功也要如实带出「驱动实际接受的是哪种独占格式」。
        // 之前这句只有失败时才有，成功时用户只看到"32-bit"，根本不知道内部是紧凑24还是32容器，
        // 1.3.x「能播/不能播」的原因因此全靠猜（本机是紧凑24、UltraLite 是32容器/24有效）。
        if (auto* w = dynamic_cast<WasapiSink*>(sink.get())) {
            if (!w->fmt_note().empty()) appendNote(w->fmt_note());
        }
        if (!sink->start()) {
            RQ.abort.store(true); RQ.cv.notify_all(); prod.join();
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error = "启动输出失败: " + sink->last_error();
            return false;
        }

        // ---------------- 渲染主循环（实现在 src/audio/render_loop.h，与回归测试共用）----------------
        // 2026-10-22 事故（VM 上独占流 GetCurrentPadding 恒满 → write_available 恒 0 →
        // 200 次超时静默 break → END pos=0 error=""）之后，循环内的超时 break 带 RENDERFAIL
        // 遥测（w0/wr、last_pad、缓冲帧数、GetBuffer HRESULT、设备名/id、处置建议）。
        rta::render::RenderHost rl;
        rl.stop = &ps.stop; rl.pauseReq = &ps.pauseReq; rl.paused = &ps.paused;
        rl.seekReq = &ps.seekReq; rl.played = &ps.played;
        rl.volDb = &ps.volDb; rl.startDb = &ps.startDb; rl.dbgPad = &ps.dbgPad; rl.gpuOut = &ps.gpu;
        rl.rq = &RQ;
        rl.deviceRate = deviceRate; rl.outBits = outBits; rl.outCh = outCh;
        rl.inRate = in.rate; rl.inFrames = in.frames(); rl.totalOut = totalOut;
        rl.dbgW0 = &g_dbgW0; rl.dbgWr = &g_dbgWr; rl.dbgPulls = &g_dbgPulls;
        rl.dbgClip = &g_dbgClip; rl.dbgPeak = &g_dbgPeak;
        rl.log = [](const std::string& s) { play_log(s); };
        rl.dlogFn = [](const std::string& s) { dlog(s); };
        rl.deviceLabel = dev;
        rl.deviceIdDesc = devId;
        rl.onShared = (p.backend == "wasapi-shared");
        // 只有「默认走 WASAPI 独占」的普通 PCM 播放才允许自动回退共享；
        // DoP/DSD 绝不回退（共享会重采样破坏 DoP 载波），ASIO/NAA/Null 与共享本身无关。
        rl.fallbackEligible = (sharedFallback != nullptr) && !dop &&
                              (p.backend.empty() || p.backend == "wasapi");
        rl.dop = dop;
        rta::render::render_loop(*sink, rl);

        RQ.abort.store(true); RQ.cv.notify_all();
        if (prod.joinable()) prod.join();
        if (rl.renderFail) {
            // 设备持续不接受数据：完整归因原样进 ps.error —— END 不再是 error="" 悬案
            if (sharedFallback && rl.fallbackEligible) *sharedFallback = true;
            sink->close();
            { std::lock_guard<std::mutex> lk(ps.mtx); ps.error = rl.failMsg; }
            return false;
        }
        ps.paused.store(false);
        Sleep((DWORD)std::max(10, p.bufferMs));      // 让设备把缓冲里的尾音放完（RENDERFAIL 路径已提前 return，不睡）
        sink->close();
        if (rl.normalEnd) {
            // 显式「正常结束」语义：此后 END 的 error="" 只可能是正常播完（有 RENDER_DONE 为证）
            play_log("RENDER_DONE 正常播完 pos=" +
                     std::to_string((double)ps.played.load() / (double)deviceRate) + "s");
        } else if (ps.stop.load()) {
            play_log("RENDER_STOP 停止/切歌 pos=" +
                     std::to_string((double)ps.played.load() / (double)deviceRate) + "s");
        }
        if (!RQ.err.empty()) { std::lock_guard<std::mutex> lk(ps.mtx); ps.error = "渲染失败: " + RQ.err; }
        // error 非空 = 非正常退出（生产者异常等），绝不能当"正常播完"返回 true
        //（否则队列线程会静默续播下一曲，把故障掩盖成正常切歌）。
        return !ps.stop.load() && ps.error.empty();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(ps.mtx);
        ps.error = std::string("异常: ") + e.what();
    } catch (...) {
        std::lock_guard<std::mutex> lk(ps.mtx);
        ps.error = "未知异常";
    }
    return false;
}

// ---------------- 播放入口（独占 RENDERFAIL 自动回退共享）----------------
// 2026-10-22 VM 事故的落地修复：独占流开得出来但设备 4 秒不接受数据时，
// 以 wasapi-shared 重开整条 play_one（设备率取系统混音率、shaper=None、note 明示
// 已回退非 bit-perfect）；共享也失败才最终报错。DoP/DSD 不回退（impl 已把关）。
static bool play_one(const std::string& file, const PlayParams& p, AudioBuffer& buf) {
    auto& ps = g_ps;
    bool sharedFallback = false;
    const bool ok = play_one_impl(file, p, buf, &sharedFallback, std::string());
    if (ok || !sharedFallback) return ok;
    if (ps.stop.load()) return false;   // 用户已停止：不要再重开一遍

    PlayParams p2 = p;
    p2.backend = "wasapi-shared";
    p2.shaper = "none";                 // 共享模式不追求 bit-perfect，关掉抖动整形
    play_log("FALLBACK_SHARED 独占流无响应,回退 WASAPI 共享(非 bit-perfect) file=" + file);
    { std::lock_guard<std::mutex> lk(ps.mtx); ps.error.clear(); }
    ps.played.store(0);
    return play_one_impl(file, p2, buf, nullptr,
                         "独占流无响应,已回退 WASAPI 共享(非 bit-perfect)");
}

// ---------------- 播放队列 ----------------

struct QueueState {
    std::mutex mtx;
    std::vector<std::string> items;
    size_t index = 0;
    int    repeat = 0;      // 0=关 1=列表循环 2=单曲循环
    bool   shuffle = false; // 随机播放
    std::vector<std::string> base;   // 随机前的原始顺序（关掉随机时还原）
};
static QueueState g_q;
// ---- 队列持久化：重启后仍保留播放列表 ----
static std::string queue_file() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring d(buf);
    const size_t s = d.find_last_of(L"\\/");
    if (s != std::wstring::npos) d = d.substr(0, s);
    d += L"\\rlplayer-playlist.txt";
    const int n = WideCharToMultiByte(CP_UTF8, 0, d.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string o((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, d.c_str(), -1, o.data(), n, nullptr, nullptr);
    if (!o.empty() && o.back() == 0) o.pop_back();
    return o;
}

// ---- 最近播放（落盘，最多 200 条，写的时候就把标题/艺术家记下来，
//      这样列表视图不用为了显示去翻音乐库）----
static std::string path_in_exe_dir(const char* name) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring d(buf);
    const size_t s = d.find_last_of(L"\\/");
    if (s != std::wstring::npos) d = d.substr(0, s);
    std::wstring w = (name && *name) ? (d + L"\\" + std::wstring(name, name + std::strlen(name))) : d;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string o((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, o.data(), n, nullptr, nullptr);
    if (!o.empty() && o.back() == 0) o.pop_back();
    return o;
}

// ---------------- 采集 / 直通监听状态与后台线程 (ROADMAP P4b) ----------------
struct InputSessionState {
    std::mutex mtx;
    std::mutex joinMtx;
    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<bool> stopFlag{false};

    std::string mode = "null"; // "record" | "monitor" | "null"
    std::string device;
    uint32_t rate = 0;
    int channels = 0;
    double seconds = 0.0;
    uint64_t frames = 0;
    double water = 0.0;
    uint64_t overflow = 0;
    uint64_t underflow = 0;
    long long posClips = 0;
    long long negClips = 0;
    std::string outFile;
    double rmsDb = -999.0;
    double peakDb = -999.0;
    std::string error;

    void reap() {
        std::lock_guard<std::mutex> lk(joinMtx);
        if (!running.load() && th.joinable()) {
            th.join();
        }
    }

    void stop() {
        stopFlag.store(true);
        std::lock_guard<std::mutex> lk(joinMtx);
        if (th.joinable()) {
            th.join();
        }
        running.store(false);
    }
};
static InputSessionState g_inputSession;

static std::string default_recording_path() {
    std::string recDir = path_in_exe_dir("recordings");
    std::error_code ec;
    fs::create_directories(fs::u8path(recDir), ec);
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "cap-%Y%m%d-%H%M%S.wav", &tm);
    return recDir + "/" + buf;
}

static void input_worker_func(std::shared_ptr<rta::audio::WasapiCapture> cap,
                              std::shared_ptr<rta::audio::WasapiSharedOutput> sharedOut,
                              std::string mode,
                              std::string outFile,
                              double maxSeconds) {
    HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownCom = SUCCEEDED(ci);

    const uint32_t rate = cap->rate();
    const int channels = cap->channels();
    const size_t totalTargetFrames = (maxSeconds > 0.0) ? (size_t)(maxSeconds * (double)rate) : 0;

    std::vector<float> recorded;
    if (mode == "record" && totalTargetFrames > 0) {
        recorded.reserve(totalTargetFrames * (size_t)channels);
    }

    std::vector<float> chunk(2048 * (size_t)channels);
    size_t recordedFrames = 0;
    auto tStart = std::chrono::steady_clock::now();

    while (!g_inputSession.stopFlag.load()) {
        if (totalTargetFrames > 0 && recordedFrames >= totalTargetFrames) {
            break;
        }
        size_t want = 2048;
        if (totalTargetFrames > 0 && recordedFrames + want > totalTargetFrames) {
            want = totalTargetFrames - recordedFrames;
        }

        size_t n = cap->read_available(chunk.data(), want);
        if (n > 0) {
            if (mode == "record") {
                recorded.insert(recorded.end(), chunk.begin(), chunk.begin() + n * (size_t)channels);
            }
            recordedFrames += n;

            // RMS 与峰值计算
            double sumSq = 0.0;
            double maxPeak = 0.0;
            const size_t totalSamples = n * (size_t)channels;
            for (size_t i = 0; i < totalSamples; ++i) {
                double absVal = std::fabs((double)chunk[i]);
                if (absVal > maxPeak) maxPeak = absVal;
                sumSq += (double)chunk[i] * (double)chunk[i];
            }
            double rms = (totalSamples > 0) ? std::sqrt(sumSq / (double)totalSamples) : 0.0;
            double rmsDb = (rms > 1e-12) ? 20.0 * std::log10(rms) : -999.0;
            double peakDb = (maxPeak > 1e-12) ? 20.0 * std::log10(maxPeak) : -999.0;

            // 直通监听（如果配置了共享输出）
            if (sharedOut && sharedOut->is_float()) {
                if (cap->rate() == sharedOut->mix_rate() && cap->channels() == sharedOut->mix_channels()) {
                    sharedOut->write_available_f32(chunk.data(), n);
                } else if (cap->rate() == sharedOut->mix_rate() && cap->channels() == 1 && sharedOut->mix_channels() == 2) {
                    std::vector<float> stereo(n * 2);
                    for (size_t i = 0; i < n; ++i) {
                        stereo[i * 2 + 0] = chunk[i];
                        stereo[i * 2 + 1] = chunk[i];
                    }
                    sharedOut->write_available_f32(stereo.data(), n);
                } else {
                    double ratio = (double)cap->rate() / (double)sharedOut->mix_rate();
                    size_t outFrames = (size_t)((double)n / ratio);
                    int outCh = sharedOut->mix_channels();
                    if (outFrames > 0 && outCh > 0) {
                        std::vector<float> resampled(outFrames * (size_t)outCh);
                        for (size_t i = 0; i < outFrames; ++i) {
                            double srcIdx = (double)i * ratio;
                            size_t idx0 = (size_t)srcIdx;
                            size_t idx1 = (idx0 + 1 < n) ? idx0 + 1 : idx0;
                            double frac = srcIdx - (double)idx0;
                            for (int ch = 0; ch < outCh; ++ch) {
                                int srcCh = (ch < channels) ? ch : (channels - 1);
                                float s0 = chunk[idx0 * (size_t)channels + srcCh];
                                float s1 = chunk[idx1 * (size_t)channels + srcCh];
                                resampled[i * (size_t)outCh + ch] = (float)((1.0 - frac) * s0 + frac * s1);
                            }
                        }
                        sharedOut->write_available_f32(resampled.data(), outFrames);
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lk(g_inputSession.mtx);
                g_inputSession.frames = recordedFrames;
                g_inputSession.seconds = (double)recordedFrames / (double)rate;
                g_inputSession.water = cap->water_level();
                g_inputSession.overflow = cap->overflow_count();
                g_inputSession.underflow = cap->underflow_count();
                g_inputSession.posClips = cap->positive_clips();
                g_inputSession.negClips = cap->negative_clips();
                g_inputSession.rmsDb = rmsDb;
                g_inputSession.peakDb = peakDb;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (maxSeconds > 0.0) {
            auto now = std::chrono::steady_clock::now();
            double el = std::chrono::duration<double>(now - tStart).count();
            if (el > maxSeconds + 3.0) {
                break;
            }
        }
    }

    cap->stop();
    if (sharedOut) sharedOut->close();

    // 录制落盘
    if (mode == "record" && recordedFrames > 0 && !outFile.empty()) {
        try {
            std::error_code ec;
            auto p = fs::u8path(outFile);
            if (p.has_parent_path()) {
                fs::create_directories(p.parent_path(), ec);
            }
            rta::audio::write_capture_wav(outFile, recorded.data(), recordedFrames, channels, rate, 24);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(g_inputSession.mtx);
            g_inputSession.error = "WAV写入失败: " + std::string(e.what());
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_inputSession.mtx);
        g_inputSession.running.store(false);
        g_inputSession.frames = recordedFrames;
        g_inputSession.seconds = (rate > 0) ? (double)recordedFrames / (double)rate : 0.0;
        g_inputSession.water = cap->water_level();
        g_inputSession.overflow = cap->overflow_count();
        g_inputSession.underflow = cap->underflow_count();
        g_inputSession.posClips = cap->positive_clips();
        g_inputSession.negClips = cap->negative_clips();
    }

    if (ownCom) CoUninitialize();
}

// rlplayer-llm.conf 的可选 proxy= 行(P1.5 后备代理)。llm.h 的 LlmConfig 不带该字段
// (llm.h 不在本轮可改清单),rtaweb 侧自行解析同一文件:键名 proxy,值形如
// http://127.0.0.1:7897;# 注释/空行/trim 规则与 llm_config_load 一致。没有该行 → 空。
static std::string llm_conf_proxy_line() {
    std::ifstream f(path_in_exe_dir("rlplayer-llm.conf"), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        line = line.substr(b);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == "proxy") return line.substr(eq + 1);
    }
    return "";
}

// ---- 播放日志（落盘，排查"播几秒变噪音"这类现场问题）----
// 启动器日志只有启动信息；播放格式、峰值、欠载必须由播放线程自己记，
// 否则用户发来的 launcher.log 里根本看不到"放到第几秒、什么采样率、峰值多少"。
// 位置：exe 同目录 rlplayer-playback.log；超过 4 MB 轮转。
// 轮转保留 3 份（1.0.5 改）：playback.log → .1 → .2，再轮转时最旧的 .2 被删 ——
// 旧版只有单份 .1，一盖就丢，现场排查要"上上次启动"的日志时无从找起。
static std::mutex g_playLogMx;
static void play_log(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_playLogMx);
    const std::string p = path_in_exe_dir("rlplayer-playback.log");
    std::error_code ec;
    const bool needBom = !fs::exists(p, ec) || fs::file_size(p, ec) == 0;
    std::ofstream f(p, std::ios::app | std::ios::binary);
    if (!f) return;
    if (needBom) f.write("\xEF\xBB\xBF", 3);   // Notepad 才能正确识别中文
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[64];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    f << ts << s << "\n";
}
static void play_log_rotate() {
    const std::string p = path_in_exe_dir("rlplayer-playback.log");
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz <= 4u * 1024u * 1024u) return;
    const std::string p1 = p + ".1", p2 = p + ".2";
    std::error_code eDel, eR1, eR2;
    fs::remove(p2, eDel);          // 最旧的一份出队（保留 3 份：当前 + .1 + .2）
    fs::rename(p1, p2, eR1);
    fs::rename(p, p1, eR2);
}

// ---------------- 元数据补全(播放触发 + /api/enrich 共用) ----------------
// P1.5 注册表化:数据源开关按注册表逐源存 metasrc_<id>(metasrc_qq/...)。
// 读侧:旧键 metaqq/metanetease/metaitunes/metalrclib 若存在则迁移读取(只读不写,
// 写侧只写新键);metaon 总开关、metallm 不变。每次现读文件 —— 设置改完立即生效。
// POST 白名单见 /api/settings。
struct MetaSettings {
    bool on = true;                          // metaon 总开关
    bool llm = true;                         // metallm
    bool summary = false;                    // metasummary 默认 0（成本策略：显式开启）
    std::map<std::string, bool> srcOn;       // 注册表 id → 开关(缺省 true)
};
static bool meta_bool_val(const std::string& v) {
    return v == "1" || v == "true" || v == "on";
}
static MetaSettings meta_settings_load() {
    MetaSettings ms;
    std::map<std::string, std::string> legacy;      // 旧键 metaqq/... 的值(迁移读取用)
    std::ifstream f(path_in_exe_dir("rlplayer-settings.conf"));
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t e = line.find('=');
        if (e == std::string::npos) continue;
        const std::string k = line.substr(0, e);
        const std::string v = line.substr(e + 1);
        if      (k == "metaon")      ms.on      = meta_bool_val(v);
        else if (k == "metallm")     ms.llm     = meta_bool_val(v);
        else if (k == "metasummary") ms.summary = meta_bool_val(v);
        else if (k.rfind("metasrc_", 0) == 0) ms.srcOn[k.substr(8)] = meta_bool_val(v);
        else if (k == "metaqq")      legacy["qq"]      = v;
        else if (k == "metanetease") legacy["netease"] = v;
        else if (k == "metaitunes")  legacy["itunes"]  = v;
        else if (k == "metalrclib")  legacy["lrclib"]  = v;
    }
    // 补全:注册表逐源,新键未写且旧键存在 → 迁移;两者都没有 → 缺省开
    for (const rta::meta::ProviderDesc& pd : rta::meta::provider_registry()) {
        if (ms.srcOn.count(pd.id) != 0) continue;
        const auto it = legacy.find(pd.id);
        ms.srcOn[pd.id] = (it == legacy.end()) ? true : meta_bool_val(it->second);
    }
    return ms;
}
static rta::meta::EnrichOpts meta_opts_from_settings() {
    const MetaSettings ms = meta_settings_load();
    rta::meta::EnrichOpts o;
    o.srcOn = ms.srcOn;
    o.llm = ms.llm;
    // 功能4：llm 配置固定读 exe 同目录（界面 /api/llm 写的就是这一份）
    o.llmConfPath = path_in_exe_dir("rlplayer-llm.conf");
    return o;
}

// 播放触发的后台补全:metaon 关 → 直接回;同一文件在途(inFlight)不重复起线程。
// enrich_track 自带全链路降级,线程内任何失败只影响该文件的 note,不影响播放。
static void meta_on_play(const std::string& file) {
    const MetaSettings ms = meta_settings_load();
    // F1：后台线程入口同样校验 —— 端点层已挡，这里防的是旧队列/外部写入的坏路径
    //（enrich_track 链路 meta_path_of 用 u8string→fs::path，后台线程抛异常=整进程终止）。
    if (!ms.on || file.empty()) return;
    if (!valid_utf8(file)) { utf8_path_reject(file); return; }
    static std::mutex s_metaMx;
    static std::set<std::string> s_inFlight;
    {
        std::lock_guard<std::mutex> lk(s_metaMx);
        if (s_inFlight.count(file) != 0) return;    // 在途:本曲已在补全,不重复
        s_inFlight.insert(file);
    }
    std::thread([file, ms] {
        rta::meta::EnrichOpts o;
        o.srcOn = ms.srcOn; o.llm = ms.llm;
        o.llmConfPath = path_in_exe_dir("rlplayer-llm.conf");
        rta::meta::enrich_track(file, false, o);
        { std::lock_guard<std::mutex> lk(s_metaMx); s_inFlight.erase(file); }
    }).detach();
}

// ---------------- 标签写回(P2;/api/writeback 三端点共用状态) ----------------
// POST /api/writeback {"apply":true} 起后台线程跑 writeback_library,立即返回;
// GET /api/writeback/status 轮询进度。互斥量保护全部字段;进度回调在批任务线程上
// 同步到来,只做赋值。dry-run 批(POST 不带 apply:true)同样支持 —— 全流程校验但不落盘。
struct WbJob {
    std::mutex mtx;
    std::thread th;              // 当前/上一轮批任务线程(start 时收割上一轮的 join)
    bool running = false;
    bool dryRun = true;
    int done = 0, total = 0, written = 0, skipped = 0, failed = 0;
    std::string err;             // 整批级错误(如 MetaStore 目录不存在)
    std::vector<std::pair<std::string, std::string>> lastFailures;   // 截到 10 条
};
static WbJob g_wb;

// 起批;已在进行返回 false(调用端回「已在进行中」,不覆盖在跑的一轮)。
static bool writeback_start(const bool apply) {
    std::thread prev;
    {
        std::lock_guard<std::mutex> lk(g_wb.mtx);
        if (g_wb.running) return false;
        if (g_wb.th.joinable()) prev = std::move(g_wb.th);   // 收割已结束的上一轮,锁外 join
        g_wb.running = true;
        g_wb.dryRun = !apply;
        g_wb.done = g_wb.total = g_wb.written = g_wb.skipped = g_wb.failed = 0;
        g_wb.err.clear();
        g_wb.lastFailures.clear();
        g_wb.th = std::thread([apply] {
            rta::meta::WritebackOpts o;
            o.dryRun = !apply;
            o.backup = true;                 // Web 批写回固定备份(安全红线①)
            std::string err;
            const rta::meta::BatchResult br = rta::meta::writeback_library(
                o, &err,
                [](const rta::meta::BatchResult& b, const int done, const int total) {
                    std::lock_guard<std::mutex> lk(g_wb.mtx);
                    g_wb.done = done; g_wb.total = total;
                    g_wb.written = b.written; g_wb.skipped = b.skipped; g_wb.failed = b.failed;
                });
            std::lock_guard<std::mutex> lk(g_wb.mtx);
            g_wb.done = br.total; g_wb.total = br.total;
            g_wb.written = br.written; g_wb.skipped = br.skipped; g_wb.failed = br.failed;
            g_wb.err = err;
            g_wb.lastFailures = br.failureList;
            if (g_wb.lastFailures.size() > 10) g_wb.lastFailures.resize(10);
            g_wb.running = false;
        });
    }
    if (prev.joinable()) prev.join();
    return true;
}

// GET /api/writeback 预览:纯只读扫描 MetaStore 记录,复刻 writeback_file 的
// 扩展名门与非空字段门来数「可写/跳过」,不跑 ffmpeg、不做 remux、不写任何文件
// (remux + PCM 逐位校验只发生在真正起批的 writeback_file 里)。
struct WbPreviewItem {
    std::string file;        // 音频路径(记录损坏时是 json 文件名)
    bool writable = false;
    std::string fields;      // 将写入的字段名(逗号分隔;可写时)
    std::string reason;      // 跳过原因(不可写时)
};
struct WbPreview {
    int total = 0, writable = 0, skipped = 0;
    std::string err;
    std::vector<WbPreviewItem> items;
};
static WbPreview writeback_preview_scan() {
    WbPreview w;
    const std::string baseDir = rta::meta::MetaStore::instance().baseDir();
    const fs::path tracksDir = fs::u8path(baseDir) / "tracks";
    std::error_code ec;
    if (!fs::exists(tracksDir, ec) || ec) {
        // 从未 enrich 的空库不是错误:预览如实给 0/0/0(UI toast「0 首可写,0 首跳过」)。
        // 真正起批时 writeback_library 仍会对缺失目录报错(那里"无事可做"必须说清)。
        return w;
    }
    std::vector<fs::path> jsons;
    for (fs::directory_iterator it(tracksDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        const std::string name = pathToUtf8(it->path().filename());
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        jsons.push_back(it->path());
    }
    if (ec) { w.err = "扫描 MetaStore 记录目录失败:" + baseDir + "/tracks"; return w; }
    std::sort(jsons.begin(), jsons.end());
    for (const fs::path& jp : jsons) {
        ++w.total;
        WbPreviewItem it;
        rta::meta::TrackMeta tm;
        if (!rta::meta::writeback_load_store_record(pathToUtf8(jp), tm)) {
            it.file = pathToUtf8(jp.filename());
            it.reason = "记录解析失败/缺 path";
            ++w.skipped;
            w.items.push_back(std::move(it));
            continue;
        }
        it.file = tm.path;
        // 与 writeback_file 相同的门,只判断不落盘(DSD/ape/未知扩展/空字段/源缺失)
        const std::string ext = rta::audio::lowerExt(tm.path);
        std::string reason;
        if (ext == ".dsf" || ext == ".dff") reason = "DSD(" + ext + ")按安全红线绝不写回";
        else if (ext == ".ape") reason = "ffmpeg 无 APE muxer,无法无损 remux";
        else if (!rta::meta::writeback_muxer(ext))
            reason = "不支持的扩展名 \"" + ext + "\"(支持 flac/mp3/ogg/opus/m4a/wma/wav/wv)";
        else if (rta::meta::writeback_collect(tm, rta::meta::writeback_vorbis_family(ext)).empty())
            reason = "无可写字段(12 个常规字段全空)";
        else {
            std::error_code e3;
            if (!fs::exists(fs::u8path(tm.path), e3) || e3) reason = "源文件不存在";
        }
        if (reason.empty()) {
            it.writable = true;
            ++w.writable;
            const std::vector<rta::meta::WbField> kvs =
                rta::meta::writeback_collect(tm, rta::meta::writeback_vorbis_family(ext));
            for (size_t i = 0; i < kvs.size(); ++i) {
                if (i) it.fields += ",";
                it.fields += kvs[i].field;
            }
        } else {
            it.reason = reason;
            ++w.skipped;
        }
        w.items.push_back(std::move(it));
    }
    return w;
}

static void recent_add(const std::string& file, const std::string& title,
                       const std::string& artist, const std::string& album) {
    const std::string p = path_in_exe_dir("rlplayer-recent.txt");
    std::vector<std::string> lines;
    { std::ifstream f(p); std::string l;
      while (std::getline(f, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); if (!l.empty()) lines.push_back(l); } }
    // 去重（同一文件只留最新一条）
    std::vector<std::string> out;
    out.push_back(file + "\t" + title + "\t" + artist + "\t" + album);
    for (auto& l : lines) {
        if (l.rfind(file + "\t", 0) == 0) continue;
        out.push_back(l);
        if (out.size() >= 200) break;
    }
    std::ofstream f(p, std::ios::trunc);
    for (auto& l : out) f << l << "\n";
}

static void queue_save() {
    std::lock_guard<std::mutex> lk(g_q.mtx);
    std::ofstream f(queue_file(), std::ios::trunc);
    if (!f) return;
    f << "# RLPlayer playlist v1\n";
    f << "index=" << g_q.index << "\n";
    f << "repeat=" << g_q.repeat << "\n";
    f << "shuffle=" << (g_q.shuffle ? 1 : 0) << "\n";
    for (const auto& it : g_q.items) f << "item=" << it << "\n";
}

static void queue_load() {
    std::ifstream f(queue_file());
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_q.mtx);
    std::string line;
    g_q.items.clear();
    while (std::getline(f, line)) {
        if (line.rfind("index=", 0) == 0) g_q.index = (size_t)atoi(line.c_str() + 6);
        else if (line.rfind("repeat=", 0) == 0) g_q.repeat = atoi(line.c_str() + 7);
        else if (line.rfind("shuffle=", 0) == 0) g_q.shuffle = (atoi(line.c_str() + 8) != 0);
        else if (line.rfind("item=", 0) == 0) g_q.items.push_back(line.substr(5));
    }
    if (g_q.index >= g_q.items.size()) g_q.index = 0;
}

// ---- 预解码槽：当前曲播放的同时，后台把下一曲解码进内存 ----
struct PreSlot {
    std::mutex mtx;
    std::thread th;
    std::atomic<bool> busy{false};
    std::string path;
    AudioBuffer buf;
};
static PreSlot g_pre;

static void prefetch_start(const std::string& path) {
    if (path.empty() || g_pre.busy.load()) return;
    if (g_pre.th.joinable()) g_pre.th.join();
    {
        std::lock_guard<std::mutex> lk(g_pre.mtx);
        if (g_pre.path == path && !g_pre.buf.raw.empty()) return;   // 已有现成的
    }
    g_pre.busy.store(true);
    g_pre.path = path;
    { std::lock_guard<std::mutex> lk(g_pre.mtx); g_pre.buf = AudioBuffer{}; }
    g_pre.th = std::thread([path]() {
        try {
            apply_dsd_src_settings(g_params);      // 预解码也按当前设置（否则会解码出另一套滤波器）
            AudioBuffer b = read_any(path, nullptr);
            std::lock_guard<std::mutex> lk(g_pre.mtx);
            if (g_pre.path == path) g_pre.buf = std::move(b);
        } catch (...) { /* 预解码失败不影响正常解码 */ }
        g_pre.busy.store(false);
    });
}

static AudioBuffer prefetch_take(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_pre.mtx);
    if (g_pre.path == path && !g_pre.buf.raw.empty()) {
        AudioBuffer b = std::move(g_pre.buf);
        g_pre.buf = AudioBuffer{};
        return b;
    }
    return AudioBuffer{};
}

// ---- 队列播放线程：自动播下一首，并在播当前曲时预解码下一曲 ----
static void queue_thread(PlayParams p, size_t start) {
    auto& ps = g_ps;
    size_t idx = start;
    for (;;) {
        if (ps.stop.load()) break;

        std::string cur, nextPath;
        {
            std::lock_guard<std::mutex> lk(g_q.mtx);
            if (g_q.items.empty()) break;
            if (idx >= g_q.items.size()) {
                if (g_q.repeat == 1) idx = 0;
                else break;
            }
            cur = g_q.items[idx];
            g_q.index = idx;
            if (idx + 1 < g_q.items.size()) nextPath = g_q.items[idx + 1];
        }

        { std::lock_guard<std::mutex> lk(ps.mtx); ps.current = cur; ps.error.clear(); }
        ps.played.store(0);
        ps.total.store(0);

        if (!nextPath.empty()) prefetch_start(nextPath);   // 后台预解码下一曲

        AudioBuffer buf = prefetch_take(cur);
        const bool done = play_one(cur, p, buf);
        const double endPos = (ps.outRate > 0 ? (double)ps.played.load() / (double)ps.outRate : 0.0);
        const size_t endPlayed = ps.played.load();
        const std::string endErr = ps.error;
        const std::string endCur = cur;
        {
            std::ostringstream es;
            es << "END pos=" << endPos
               << " played=" << endPlayed << " total=" << ps.total.load()
               << " clip=" << g_dbgClip.load()
               << " error=\"" << endErr << "\""
               << " file=" << endCur
               << " v" << RLPLAYER_VERSION;
            play_log(es.str());
        }
        {
            std::lock_guard<std::mutex> lk(g_lastEndMx);
            g_lastEnd.isPosZeroNoErr = (endPos == 0.0 && endPlayed == 0 && endErr.empty());
            g_lastEnd.file = endCur;
            g_lastEnd.tp = std::chrono::steady_clock::now();
        }
        if (ps.stop.load() || !done) break;

        std::lock_guard<std::mutex> lk(g_q.mtx);
        if (g_q.repeat == 2) continue;                     // 单曲循环
        if (idx + 1 < g_q.items.size()) { idx += 1; continue; }
        if (g_q.repeat == 1) { idx = 0; continue; }
        break;
    }
    ps.playing = false;
}

// ---------------- HTTP 处理 ----------------
static std::string page();

// F3：端点探活缓存。probe_formats 对 16/24/32 各跑一遍独占支持率（每端点最多约 1s），
// 而 webui 是单线程 accept —— /api/diag 每次实时探活会让期间的全部 API 排队。
// 这里按端点 id 缓存探活结果，TTL 60s；?refresh=1 强制重探。
struct DiagProbeEntry {
    FormatProbe pr;
    std::chrono::steady_clock::time_point tp;
};
static std::mutex g_diagProbeMx;
static std::map<std::string, DiagProbeEntry> g_diagProbeCache;

// ---------------- DeepSeek API Key（2026-10-22 功能4）----------------
// 用户原话：「元数据没有提供 ds 的 api 输入接口（就默认 deepseek）」。
//
// 红线（本端点存在的唯一理由）：
//   * key 绝不进源码、绝不进安装包 —— 只落在 exe 同目录 rlplayer-llm.conf；
//   * 界面【只显示掩码】，GET 不返回明文；POST 只写不读；
//   * 写 conf 时保留既有行（proxy= / 注释）—— 走 llm_config_save_merge，
//     不是整文件重写的 llm_config_save；
//   * 测试连接的错误一律过 llm_redact，服务端把 key 回显进错误文本也不会漏。
//
// 存哪里：rlplayer-llm.conf 与 enrich 的 llm_config_load 是同一份文件，
// 所以「界面填的 key」就是「补全时用的 key」，不存在第二个真相源。
static std::string llm_conf_path() { return path_in_exe_dir("rlplayer-llm.conf"); }

// 只读状态：是否已配置 + 掩码 + 模型/端点 + 代理行是否在。绝不返回 api_key 明文。
static std::string llm_status_json() {
    const rta::meta::LlmConfig c = rta::meta::llm_config_load(llm_conf_path());
    std::ostringstream j;
    j << "{\"ok\":true"
      << ",\"configured\":" << (c.apiKey.empty() ? "false" : "true")
      << ",\"masked\":\"" << jesc(rta::meta::llm_mask_key(c.apiKey)) << "\""
      << ",\"model\":\"" << jesc(c.model) << "\""
      << ",\"baseUrl\":\"" << jesc(c.baseUrl) << "\""
      << ",\"path\":\"" << jesc(llm_conf_path()) << "\""
      << ",\"proxy\":" << (llm_conf_proxy_line().empty() ? "false" : "true")
      << ",\"monthlyCap\":" << c.monthlyCap
      << ",\"currentUsage\":" << rta::meta::llm_get_monthly_count(rta::meta::llm_current_month(), c.usagePath)
      << "}";
    return j.str();
}

// key/值合法性：长度上限 + 不允许控制字符与空格（粘进来的 key 常带换行/空格，
// 写进 conf 会把文件结构弄坏，读的时候又被 trim 掉，症状极难查）。
static bool llm_value_ok(const std::string& v, size_t maxLen) {
    if (v.size() > maxLen) return false;
    for (unsigned char c : v) {
        if (c < 0x20 || c == 0x7f) return false;
        if (c == ' ') return false;
    }
    return true;
}

// 「测试连接」：最小一次 chat 请求（max_tokens=8），只判「key 有效」。
// 单独用 15s 超时而不是 llm_chat 的 60s —— 这是交互按钮，不能让设置页转一分钟。
// 返回体只带模型/耗时/token 数：reply 内容对用户没用，反而多一个可能夹带信息的面。
static std::string llm_test_json() {
    const rta::meta::LlmConfig c = rta::meta::llm_config_load(llm_conf_path());
    if (c.apiKey.empty())
        return "{\"ok\":false,\"error\":\"尚未配置 API Key\"}";
    const std::string body = rta::meta::llm_build_chat_body(
        c.model, { rta::meta::LlmMessage{"user", "ping"} }, 8, 0.0f, false);
    const auto t0 = std::chrono::steady_clock::now();
    const rta::net::HttpResponse resp = rta::net::http_post(
        rta::meta::llm_base_url(c.baseUrl) + "/chat/completions", body,
        { "Authorization: Bearer " + c.apiKey }, "application/json", 15000);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    const std::string text(resp.body.begin(), resp.body.end());
    if (resp.status == 0) {
        const std::string e = rta::meta::llm_redact(c.apiKey, "网络请求失败: " + resp.error);
        return "{\"ok\":false,\"error\":\"" + jesc(e) + "\",\"latencyMs\":" + std::to_string(ms) + "}";
    }
    const rta::meta::LlmReply p = rta::meta::llm_parse_chat_response(text);
    if (resp.status < 200 || resp.status >= 300) {
        const std::string e = rta::meta::llm_redact(c.apiKey,
            "HTTP " + std::to_string(resp.status) + (p.error.empty() ? "" : ": " + p.error));
        return "{\"ok\":false,\"error\":\"" + jesc(e) + "\",\"latencyMs\":" + std::to_string(ms) + "}";
    }
    if (!p.ok) {
        const std::string e = rta::meta::llm_redact(c.apiKey, p.error);
        return "{\"ok\":false,\"error\":\"" + jesc(e) + "\",\"latencyMs\":" + std::to_string(ms) + "}";
    }
    return "{\"ok\":true,\"model\":\"" + jesc(c.model) + "\",\"latencyMs\":" +
           std::to_string(ms) + ",\"tokens\":" + std::to_string(p.totalTokens) + "}";
}

static std::string handle(const rta::net::HttpRequest& rq, std::string& ct) {
    auto& ps = g_ps;

    if (rq.path.rfind("/favicon", 0) == 0) {
        ct = "image/png";
        return std::string(reinterpret_cast<const char*>(rta::net::kAppIconPng), rta::net::kAppIconPngSize);
    }
    if (rq.path.rfind("/app.js", 0) == 0) {
        ct = "application/javascript; charset=utf-8";
        return std::string(rta::net::rta_ui_script());
    }
    if (rq.path == "/" || rq.path == "/index.html") {
        ct = "text/html; charset=utf-8";
        return page();
    }

    if (rq.path.rfind("/api/files", 0) == 0) {
        ct = "application/json; charset=utf-8";
        // 默认位置：音乐库的第一个根目录（而不是磁盘根 —— 那样打开就是空的）
        std::string dir;
        const size_t q = rq.path.find("?dir=");
        if (q != std::string::npos) dir = urlDecode(rq.path.substr(q + 5));
        if (dir.empty()) {
            const auto roots = Library::instance().roots();
            if (!roots.empty()) dir = roots.front();
        }
        if (dir.empty()) dir = "C:\\Users";
        if (!valid_utf8(dir)) return utf8_path_reject(dir);   // F1：目录名进 fs 前先校验

        std::error_code ec;
        if (!fs::exists(dir, ec)) {
            return "{\"ok\":false,\"error\":\"目录不存在\"}";
        }
        std::vector<std::string> dirs, files;
        for (auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            const std::string name = pathToUtf8(e.path().filename());
            if (!name.empty() && name[0] == '.') continue;
            std::string ext = pathToUtf8(e.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (e.is_directory(ec)) dirs.push_back(name);
            else {
                static const char* kAudioExt[] = {
                    ".wav", ".wave", ".flac", ".alac", ".m4a", ".mp3", ".aac", ".ogg", ".opus",
                    ".wma", ".ape", ".wv", ".dsf", ".dff", ".aif", ".aiff", ".aifc", ".caf", ".mka"
                };
                for (const char* x : kAudioExt) if (ext == x) { files.push_back(name); break; }
            }
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());

        std::ostringstream j;
        j << "{\"ok\":true,\"dir\":\"" << jesc(dir) << "\",\"dirs\":[";
        for (size_t i = 0; i < dirs.size(); ++i) { if (i) j << ","; j << "\"" << jesc(dirs[i]) << "\""; }
        j << "],\"files\":[";
        for (size_t i = 0; i < files.size(); ++i) { if (i) j << ","; j << "\"" << jesc(files[i]) << "\""; }
        j << "]}";
        return j.str();
    }

    // ---------------- 本地音乐库（专辑 / 艺术家 / 搜索）----------------
    // 设计取向参考 Roon：以「专辑」为一等公民，而不是让用户翻目录树。
    if (rq.path.rfind("/api/lib", 0) == 0) {
        ct = "application/json; charset=utf-8";
        auto& lib = Library::instance();

        if (rq.path.rfind("/api/lib/stats", 0) == 0) {
            std::ostringstream j;
            const auto roots = lib.roots();
            j << "{\"ok\":true,\"tracks\":" << lib.trackCount()
              << ",\"albums\":" << lib.albumCount()
              << ",\"scanning\":" << (lib.scanning() ? "true" : "false")
              << ",\"scanned\":" << lib.scanned()
              << ",\"total\":" << lib.scanTotal()
              << ",\"scanThreads\":" << lib.scanThreads()
              << ",\"lastScanMs\":" << lib.lastScanMs()
              << ",\"probed\":" << lib.lastScanProbed()
              << ",\"cached\":" << lib.lastScanCached()
              << ",\"msg\":\"" << jesc(lib.scanMsg()) << "\",\"roots\":[";
            for (size_t i = 0; i < roots.size(); ++i) { if (i) j << ","; j << "\"" << jesc(roots[i]) << "\""; }
            j << "]}";
            return j.str();
        }
        if (rq.path.rfind("/api/lib/scan", 0) == 0 || rq.path.rfind("/api/rescan", 0) == 0) {
            lib.startScan(rq.body.find("full=1") != std::string::npos || rq.path.rfind("/api/rescan", 0) == 0);
            return "{\"ok\":true}";
        }
        if (rq.path.rfind("/api/lib/roots", 0) == 0) {
            std::string add, rem;
            std::istringstream is(rq.body);
            std::string tok;
            while (std::getline(is, tok, (char)38)) {
                const size_t e = tok.find((char)61);
                if (e == std::string::npos) continue;
                const std::string kk = urlDecode(tok.substr(0, e));
                const std::string vv = urlDecode(tok.substr(e + 1));
                if (kk == "add") add = vv;
                else if (kk == "remove") rem = vv;
            }
            if (!add.empty() && !lib.addRoot(add))
                return "{\"ok\":false,\"error\":\"目录不存在: " + jesc(add) + "\"}";
            if (!rem.empty()) lib.removeRoot(rem);
            return "{\"ok\":true}";
        }
        // 注意：/api/lib/albums 必须在 /api/lib/album 之前判断（后者是前者的前缀）
        if (rq.path.rfind("/api/lib/albums", 0) == 0) {
            std::string q;
            const size_t p = rq.path.find("?q=");
            if (p != std::string::npos) q = urlDecode(rq.path.substr(p + 3));
            std::string ql = q;
            for (auto& c : ql) c = (char)std::tolower((unsigned char)c);
            const auto albums = lib.albums();
            std::ostringstream j;
            j << "{\"ok\":true,\"q\":\"" << jesc(q) << "\",\"albums\":[";
            bool first = true;
            for (const auto& a : albums) {
                if (!ql.empty()) {
                    std::string hay = a.name + " " + a.artist;
                    for (auto& c : hay) c = (char)std::tolower((unsigned char)c);
                    if (hay.find(ql) == std::string::npos) continue;
                }
                if (!first) j << ",";
                first = false;
                rta::net::LibTrack t0;
                int sr = 0, bits = 0;
                if (!a.idx.empty() && lib.trackAt(a.idx[0], t0)) { sr = t0.sampleRate; bits = t0.bits; }
                j << "{\"k\":\"" << a.key << "\",\"name\":\"" << jesc(a.name)
                  << "\",\"artist\":\"" << jesc(a.artist) << "\",\"year\":" << a.year
                  << ",\"n\":" << a.nTracks << ",\"dur\":" << a.duration
                  << ",\"sr\":" << sr << ",\"bits\":" << bits
                  << ",\"cv\":" << (a.hasCover ? "true" : "false") << "}";
            }
            j << "]}";
            return j.str();
        }
        if (rq.path.rfind("/api/lib/album", 0) == 0) {
            std::string key;
            const size_t p = rq.path.find("?k=");
            if (p != std::string::npos) key = urlDecode(rq.path.substr(p + 3));
            rta::net::LibAlbum a;
            if (!lib.album(key, a)) return "{\"ok\":false,\"error\":\"专辑不存在\"}";
            std::ostringstream j;
            j << "{\"ok\":true,\"k\":\"" << a.key << "\",\"name\":\"" << jesc(a.name)
              << "\",\"artist\":\"" << jesc(a.artist) << "\",\"year\":" << a.year
              << ",\"dur\":" << a.duration << ",\"cv\":" << (a.hasCover ? "true" : "false")
              << ",\"tracks\":[";
            for (size_t i = 0; i < a.idx.size(); ++i) {
                rta::net::LibTrack t;
                if (!lib.trackAt(a.idx[i], t)) continue;
                if (i) j << ",";
                j << "{\"i\":" << i << ",\"n\":" << t.trackNo << ",\"d\":" << t.discNo
                  << ",\"title\":\"" << jesc(t.title) << "\",\"artist\":\"" << jesc(t.artist)
                  << "\",\"dur\":" << t.duration << ",\"sr\":" << t.sampleRate
                  << ",\"bits\":" << t.bits << ",\"ch\":" << t.channels
                  << ",\"codec\":\"" << jesc(t.codec) << "\",\"path\":\"" << jesc(t.path) << "\"}";
            }
            j << "]}";
            return j.str();
        }
        if (rq.path.rfind("/api/lib/search", 0) == 0) {
            std::string q;
            const size_t p = rq.path.find("?q=");
            if (p != std::string::npos) q = urlDecode(rq.path.substr(p + 3));
            const auto hits = lib.search(q, 300);
            std::ostringstream j;
            j << "{\"ok\":true,\"q\":\"" << jesc(q) << "\",\"n\":" << hits.size() << ",\"tracks\":[";
            for (size_t i = 0; i < hits.size(); ++i) {
                rta::net::LibTrack t;
                if (!lib.trackAt(hits[i], t)) continue;
                if (i) j << ",";
                j << "{\"title\":\"" << jesc(t.title) << "\",\"artist\":\"" << jesc(t.artist)
                  << "\",\"album\":\"" << jesc(t.album) << "\",\"dur\":" << t.duration
                  << ",\"sr\":" << t.sampleRate << ",\"bits\":" << t.bits
                  << ",\"path\":\"" << jesc(t.path) << "\"}";
            }
            j << "]}";
            return j.str();
        }
        if (rq.path.rfind("/api/lib/cover", 0) == 0) {
            std::string key;
            const size_t p = rq.path.find("?k=");
            if (p != std::string::npos) key = urlDecode(rq.path.substr(p + 3));
            std::vector<unsigned char> bytes;
            std::string mime = "image/jpeg";
            lib.albumCover(key, bytes, mime);
            ct = mime;
            return std::string(bytes.begin(), bytes.end());
        }
        // 播放整张专辑（?k=专辑键&i=起始曲序），队列被换成这张专辑
        if (rq.path.rfind("/api/lib/play", 0) == 0) {
            PlayParams p = parseForm(rq.body);
            std::string key;
            size_t start = 0;
            {
                std::istringstream is(rq.body);
                std::string tok;
                while (std::getline(is, tok, (char)38)) {
                    const size_t e = tok.find((char)61);
                    if (e == std::string::npos) continue;
                    const std::string kk = urlDecode(tok.substr(0, e));
                    const std::string vv = urlDecode(tok.substr(e + 1));
                    if (kk == "k") key = vv;
                    else if (kk == "i") start = (size_t)atoi(vv.c_str());
                }
            }
            rta::net::LibAlbum a;
            if (!lib.album(key, a)) return "{\"ok\":false,\"error\":\"专辑不存在\"}";
            std::vector<std::string> paths;
            for (size_t ix : a.idx) {
                rta::net::LibTrack t;
                if (lib.trackAt(ix, t)) paths.push_back(t.path);
            }
            if (paths.empty()) return "{\"ok\":false,\"error\":\"专辑为空\"}";
            if (start >= paths.size()) start = 0;
            {
                std::lock_guard<std::mutex> lk(g_q.mtx);
                g_q.items = paths;
                g_q.index = start;
            }
            queue_save();
            g_params = p;
            ps.stop.store(true);
            ps.joinIfDone();
            ps.stop.store(false);
            { std::lock_guard<std::mutex> lk(ps.mtx); ps.error.clear(); ps.current = paths[start]; }
            ps.played.store(0);
            ps.total.store(0);
            ps.playing.store(true);
            ps.th = std::thread(queue_thread, p, start);
            return "{\"ok\":true,\"count\":" + std::to_string(paths.size()) + "}";
        }
        // 整张专辑加入队列（不打断当前播放）
        if (rq.path.rfind("/api/lib/enqueue", 0) == 0) {
            std::string key;
            const size_t p = rq.path.find("?k=");
            if (p != std::string::npos) key = urlDecode(rq.path.substr(p + 3));
            rta::net::LibAlbum a;
            if (!lib.album(key, a)) return "{\"ok\":false,\"error\":\"专辑不存在\"}";
            size_t cnt = 0;
            {
                std::lock_guard<std::mutex> lk(g_q.mtx);
                for (size_t ix : a.idx) {
                    rta::net::LibTrack t;
                    if (lib.trackAt(ix, t)) { g_q.items.push_back(t.path); ++cnt; }
                }
                cnt = g_q.items.size();
            }
            queue_save();
            return "{\"ok\":true,\"count\":" + std::to_string(cnt) + "}";
        }
        return "{\"ok\":false,\"error\":\"unknown lib api\"}";
    }

    // ---------------- 歌词 ----------------
    if (rq.path.rfind("/api/lyrics", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::string file;
        const size_t q = rq.path.find("?file=");
        if (q != std::string::npos) file = urlDecode(rq.path.substr(q + 6));
        if (file.empty()) return "{\"ok\":false,\"error\":\"未指定文件\"}";
        if (!valid_utf8(file)) return utf8_path_reject(file);   // F1：本端点下方就是 fs::u8path
        std::string text, src;
        // 1) 同名 .lrc（或同目录唯一的 .lrc）
        {
            std::error_code ec;
            fs::path p = fs::u8path(file);
            std::vector<fs::path> cands;
            fs::path lrc = p; lrc.replace_extension(".lrc");
            if (fs::exists(lrc, ec)) cands.push_back(lrc);
            fs::path LRC = p; LRC.replace_extension(".LRC");
            if (fs::exists(LRC, ec)) cands.push_back(LRC);
            if (cands.empty()) {
                int cnt = 0;
                for (auto& e : fs::directory_iterator(p.parent_path(), ec)) {
                    std::string ext = pathToUtf8(e.path().extension());
                    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
                    if (ext == ".lrc") { cands.push_back(e.path()); ++cnt; }
                }
                if (cnt != 1) cands.clear();          // 有多个就不知道配哪个了
            }
            for (auto& c : cands) {
                std::ifstream f(c, std::ios::binary);
                if (!f) continue;
                std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (s.size() > 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
                // UTF-8 校验；失败则按 GBK 转（中文 LRC 大量是 GBK）
                bool utf8 = true;
                for (size_t i = 0; i < s.size(); ) {
                    const unsigned char c0 = (unsigned char)s[i];
                    int n = c0 < 0x80 ? 0 : ((c0 >> 5) == 6 ? 1 : ((c0 >> 4) == 14 ? 2 : ((c0 >> 3) == 30 ? 3 : -1)));
                    if (n < 0) { utf8 = false; break; }
                    for (int k = 1; k <= n; ++k) if (i + k >= s.size() || ((unsigned char)s[i + k] >> 6) != 2) { utf8 = false; break; }
                    if (!utf8) break;
                    i += (size_t)n + 1;
                }
                if (!utf8) {
                    const int wl = MultiByteToWideChar(936, 0, s.c_str(), (int)s.size(), nullptr, 0);
                    std::wstring w((size_t)wl, L'\0');
                    MultiByteToWideChar(936, 0, s.c_str(), (int)s.size(), w.data(), wl);
                    const int ml = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wl, nullptr, 0, nullptr, nullptr);
                    std::string o((size_t)ml, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wl, o.data(), ml, nullptr, nullptr);
                    s.swap(o);
                }
                text.swap(s); src = "sidecar"; break;
            }
        }
        // 2) 内嵌歌词标签：FLAC/MP3/M4A 在 format_tags，OGG/Opus 在 stream_tags；
        //    标签名有 LYRICS / UNSYNCEDLYRICS / USLT / SYLT / LRC 等多种写法。
        if (text.empty()) {
            const std::string ffprobe = find_tool("ffprobe");
            if (!ffprobe.empty()) {
                std::string out;
                const std::wstring cmd = L"\"" + widen(ffprobe) +
                    L"\" -v error -show_entries format_tags:stream_tags -of json \"" + widen(file) + L"\"";
                if (run_hidden(cmd, &out) == 0 && !out.empty()) {
                    const rta::net::Json j = rta::net::JsonParser(out).parse();
                    std::string best; bool bestTimed = false;
                    auto consider = [&](const rta::net::Json& tags) {
                        for (const auto& kv : tags.obj) {
                            std::string k = kv.first;
                            for (auto& c : k) c = (char)std::tolower((unsigned char)c);
                            if (k.find("lyric") == std::string::npos && k.find("uslt") == std::string::npos &&
                                k.find("sylt") == std::string::npos && k.find("unsync") == std::string::npos &&
                                k.find("lrc") == std::string::npos) continue;
                            if (kv.second.type != rta::net::Json::Str || kv.second.str.empty()) continue;
                            const std::string& v = kv.second.str;
                            const bool timed = v.find('[') != std::string::npos && v.find(':') != std::string::npos;
                            // 优先"带时间轴"的歌词；否则取最长的一段（有些文件同时有纯文本+时间轴）
                            if (best.empty() || (timed && !bestTimed) ||
                                (timed == bestTimed && v.size() > best.size())) {
                                best = v; bestTimed = timed;
                            }
                        }
                    };
                    consider(j["format"]["tags"]);
                    for (size_t i = 0; i < j["streams"].size(); ++i)
                        consider(j["streams"].at(i)["tags"]);
                    if (!best.empty()) { text = best; src = "embedded"; }
                }
            }
        }
        // 3) MetaStore 补全歌词兜底(P1):前两路都空才查;原文样输出(src="meta:<来源>")
        if (text.empty()) {
            rta::meta::TrackMeta tm;
            if (rta::meta::MetaStore::instance().load(file, tm)) {
                const auto it = tm.fields.find("lyrics");
                if (it != tm.fields.end() && !it->second.value.empty()) {
                    text = it->second.value;
                    src = "meta:" + (it->second.source.empty() ? std::string("unknown")
                                                               : it->second.source);
                }
            }
        }
        std::ostringstream j;
        j << "{\"ok\":" << (text.empty() ? "false" : "true")
          << ",\"src\":\"" << src << "\",\"lrc\":\"" << jesc(text) << "\"}";
        return j.str();
    }

    // ---------------- 播放列表 ----------------
    // 注意：不能写成 rfind("/api/pl",0)，否则 /api/play 会被这里截走（本轮真踩到）
    if (rq.path.rfind("/api/pl/", 0) == 0 || rq.path == "/api/pl") {
        ct = "application/json; charset=utf-8";
        auto plDir = [] {
            wchar_t buf[MAX_PATH];
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            std::wstring d(buf);
            const size_t s = d.find_last_of(L"\\/");
            if (s != std::wstring::npos) d = d.substr(0, s);
            d += L"\\playlists";
            return fs::path(d);
        };
        auto toUtf8 = [](const std::wstring& w) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string o((size_t)n, 0);
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, o.data(), n, nullptr, nullptr);
            if (!o.empty() && o.back() == 0) o.pop_back();
            return o;
        };
        std::error_code ec;
        fs::create_directories(plDir(), ec);

        if (rq.path.rfind("/api/pl/save", 0) == 0) {
            std::string name;
            std::istringstream is(rq.body);
            std::string tok;
            while (std::getline(is, tok, (char)38)) {
                const size_t e = tok.find((char)61);
                if (e == std::string::npos) continue;
                if (urlDecode(tok.substr(0, e)) == "name") name = urlDecode(tok.substr(e + 1));
            }
            if (name.empty()) name = "未命名播放列表";
            for (auto& c : name) if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
            std::vector<std::string> items;
            { std::lock_guard<std::mutex> lk(g_q.mtx); items = g_q.items; }
            std::ofstream f(plDir() / (name + ".m3u8"), std::ios::binary | std::ios::trunc);
            if (!f) return "{\"ok\":false,\"error\":\"写文件失败\"}";
            f << "#EXTM3U\n";
            for (auto& it : items) f << it << "\n";
            return "{\"ok\":true,\"count\":" + std::to_string(items.size()) + "}";
        }
        if (rq.path.rfind("/api/pl/load", 0) == 0) {
            std::string name;
            const size_t q = rq.path.find("?name=");
            if (q != std::string::npos) name = urlDecode(rq.path.substr(q + 6));
            std::ifstream f(plDir() / (name + ".m3u8"));
            if (!f) return "{\"ok\":false,\"error\":\"播放列表不存在\"}";
            std::vector<std::string> items;
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                items.push_back(line);
            }
            {
                std::lock_guard<std::mutex> lk(g_q.mtx);
                g_q.items = items;
                g_q.index = 0;
            }
            queue_save();
            return "{\"ok\":true,\"count\":" + std::to_string(items.size()) + "}";
        }
        if (rq.path.rfind("/api/pl/delete", 0) == 0) {
            std::string name;
            const size_t q = rq.path.find("?name=");
            if (q != std::string::npos) name = urlDecode(rq.path.substr(q + 6));
            fs::remove(plDir() / (name + ".m3u8"), ec);
            return std::string("{\"ok\":") + (ec ? "false" : "true") + "}";
        }
        // 列表
        std::ostringstream j;
        j << "{\"ok\":true,\"lists\":[";
        bool first = true;
        for (auto& e : fs::directory_iterator(plDir(), ec)) {
            std::string ext = toUtf8(e.path().extension().wstring());
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext != ".m3u8") continue;
            std::string name = toUtf8(e.path().stem().wstring());
            int n = 0;
            { std::ifstream f(e.path()); std::string l; while (std::getline(f, l)) if (!l.empty() && l[0] != '#') ++n; }
            if (!first) j << ",";
            first = false;
            j << "{\"name\":\"" << jesc(name) << "\",\"n\":" << n << "}";
        }
        j << "]}";
        return j.str();
    }

    if (rq.path.rfind("/api/play", 0) == 0) {
        ct = "application/json; charset=utf-8";
        PlayParams p = parseForm(rq.body);   // 非 const：远程曲目会把 p.file 换成缓存路径
        // 远程曲目：先把原始文件完整下载到缓存，再走与本地完全相同的解码与 DSP 路径
        if (!p.remoteId.empty()) {
            std::string ferr;
            const std::string local = SourceManager::instance().fetch(p.remoteId, p.remoteSuffix, ferr);
            if (local.empty())
                return std::string("{\"ok\":false,\"error\":\"获取远程曲目失败: ") + jesc(ferr) + "\"}";
            p.file = local;
        }
        if (p.file.empty()) return "{\"ok\":false,\"error\":\"未指定文件\"}";
        // F1：file 会进队列 → 播放线程 recent_add/meta_on_play/fs::u8path 全链消费，
        // 非法字节在这里挡下（播放线程里抛异常没有兜底，等于整进程退出）。
        if (!valid_utf8(p.file)) return utf8_path_reject(p.file);
        g_params = p;

        // 先停掉上一次
        ps.stop.store(true);
        ps.joinIfDone();
        ps.stop.store(false);
        {
            std::lock_guard<std::mutex> lk(ps.mtx);
            ps.error.clear();
            ps.current = p.file;
        }
        {
            std::lock_guard<std::mutex> lk(g_q.mtx);
            // 如果点的文件已在队列里，就从它开始播；否则把队列换成它
            bool found = false;
            for (size_t i = 0; i < g_q.items.size(); ++i)
                if (g_q.items[i] == p.file) { g_q.index = i; found = true; break; }
            if (!found) { g_q.items.clear(); g_q.items.push_back(p.file); g_q.index = 0; }
        }
        ps.played.store(0);
        ps.total.store(0);
        ps.playing.store(true);
        const size_t st0 = g_q.index;
        ps.th = std::thread(queue_thread, p, st0);
        return "{\"ok\":true}";
    }

    if (rq.path.rfind("/api/qdel", 0) == 0) {
        ct = "application/json; charset=utf-8";
        size_t idx = 0;
        const size_t q = rq.path.find("?i=");
        if (q != std::string::npos) idx = (size_t)atoi(rq.path.c_str() + q + 3);
        size_t cnt = 0;
        {
            std::lock_guard<std::mutex> lk(g_q.mtx);
            if (idx < g_q.items.size()) {
                g_q.items.erase(g_q.items.begin() + (long long)idx);
                if (g_q.index > idx && g_q.index > 0) --g_q.index;
                if (g_q.index >= g_q.items.size()) g_q.index = g_q.items.empty() ? 0 : g_q.items.size() - 1;
            }
            cnt = g_q.items.size();
        }
        queue_save();
        return "{\"ok\":true,\"count\":" + std::to_string(cnt) + "}";
    }
    if (rq.path.rfind("/api/qmove", 0) == 0) {
        ct = "application/json; charset=utf-8";
        size_t from = 0, to = 0;
        { const size_t q = rq.path.find("?from="); if (q != std::string::npos) from = (size_t)atoi(rq.path.c_str() + q + 6); }
        { const size_t q = rq.path.find("&to=");   if (q != std::string::npos) to   = (size_t)atoi(rq.path.c_str() + q + 4); }
        size_t cnt = 0;
        {
            std::lock_guard<std::mutex> lk(g_q.mtx);
            if (from < g_q.items.size() && to < g_q.items.size() && from != to) {
                std::string v = g_q.items[from];
                g_q.items.erase(g_q.items.begin() + (long long)from);
                g_q.items.insert(g_q.items.begin() + (long long)to, v);
            }
            cnt = g_q.items.size();
        }
        queue_save();
        return "{\"ok\":true,\"count\":" + std::to_string(cnt) + "}";
    }

    if (rq.path.rfind("/api/add", 0) == 0) {
        ct = "application/json; charset=utf-8";
        PlayParams p = parseForm(rq.body);   // 非 const：远程曲目会把 p.file 换成缓存路径
        // 远程曲目：先把原始文件完整下载到缓存，再走与本地完全相同的解码与 DSP 路径
        if (!p.remoteId.empty()) {
            std::string ferr;
            const std::string local = SourceManager::instance().fetch(p.remoteId, p.remoteSuffix, ferr);
            if (local.empty())
                return std::string("{\"ok\":false,\"error\":\"获取远程曲目失败: ") + jesc(ferr) + "\"}";
            p.file = local;
        }
        // 拖进来的是目录时，把里面的音频文件全部展开（最多 5000 个，递归）
        if (!p.file.empty() && !valid_utf8(p.file)) return utf8_path_reject(p.file);   // F1
        std::vector<std::string> addList;
        {
            std::error_code ec;
            if (fs::is_directory(fs::u8path(p.file), ec) && !ec) {
                static const char* kExt[] = { ".wav", ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".opus",
                                              ".ape", ".wv", ".aiff", ".aif", ".dsf", ".dff", ".wma" };
                int guard = 0;
                for (auto it = fs::recursive_directory_iterator(fs::u8path(p.file), fs::directory_options::skip_permission_denied, ec);
                     it != fs::recursive_directory_iterator() && !ec && guard < 5000; it.increment(ec)) {
                    if (!it->is_regular_file(ec)) continue;
                    std::string ext = pathToUtf8(it->path().extension());
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    bool ok = false;
                    for (const char* e : kExt) if (ext == e) { ok = true; break; }
                    if (ok) { addList.push_back(pathToUtf8(it->path())); ++guard; }
                }
            } else {
                addList.push_back(p.file);
            }
        }
        size_t cnt = 0;
        {
            // 注意：必须用独立作用域释放锁 —— queue_save() 内部会再次加锁，
            // 若在持锁状态下调用就会死锁（本轮踩到：服务端直接挂住，连接被关闭）。
            std::lock_guard<std::mutex> lk(g_q.mtx);
            for (const auto& f : addList) g_q.items.push_back(f);
            cnt = g_q.items.size();
        }
        queue_save();
        return "{\"ok\":true,\"count\":" + std::to_string(cnt) + "}";
    }

    if (rq.path.rfind("/api/src", 0) == 0) {
        ct = "application/json; charset=utf-8";
        auto& mgr = SourceManager::instance();
        if (rq.path.rfind("/api/src/config", 0) == 0) {
            if (rq.method == "POST") {
                SourceConfig c = mgr.config();
                std::istringstream is(rq.body);
                std::string tok;
                while (std::getline(is, tok, (char)38)) {
                    const size_t e = tok.find((char)61);
                    if (e == std::string::npos) continue;
                    const std::string kk = urlDecode(tok.substr(0, e));
                    const std::string vv = urlDecode(tok.substr(e + 1));
                    if (kk == "kind") c.kind = vv;
                    else if (kk == "base") c.base = vv;
                    else if (kk == "user") c.user = vv;
                    else if (kk == "pass") c.pass = vv;
                }
                mgr.set_config(c);
                return "{\"ok\":true}";
            }
            const SourceConfig c = mgr.config();
            std::ostringstream j;
            j << "{\"ok\":true,\"configured\":" << (c.enabled() ? "true" : "false")
              << ",\"kind\":\"" << jesc(c.kind) << "\""
              << ",\"base\":\"" << jesc(c.base) << "\""
              << ",\"user\":\"" << jesc(c.user) << "\""
              << ",\"cache\":" << mgr.cache_count() << "}";
            return j.str();
        }
        if (rq.path.rfind("/api/src/ping", 0) == 0) {
            std::string err;
            const bool ok = mgr.ping(err);
            return std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" + jesc(err) + "\"}";
        }
        if (rq.path.rfind("/api/src/browse", 0) == 0 || rq.path.rfind("/api/src/search", 0) == 0) {
            const bool isSearch = rq.path.rfind("/api/src/search", 0) == 0;
            std::string key;
            const size_t q = rq.path.find(isSearch ? "?q=" : "?id=");
            if (q != std::string::npos) key = urlDecode(rq.path.substr(q + (isSearch ? 3 : 4)));
            std::string err;
            std::vector<SubEntry> v;
            if (isSearch) v = mgr.search(key, err);
            else if (key.empty()) v = mgr.indexes(err);
            else v = mgr.browse(key, err);
            std::ostringstream j;
            j << "{\"ok\":true,\"key\":\"" << jesc(key) << "\",\"err\":\"" << jesc(err) << "\",\"items\":[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) j << ",";
                j << "{\"id\":\"" << jesc(v[i].id) << "\""
                  << ",\"t\":\"" << jesc(v[i].title) << "\""
                  << ",\"ar\":\"" << jesc(v[i].artist) << "\""
                  << ",\"al\":\"" << jesc(v[i].album) << "\""
                  << ",\"sf\":\"" << jesc(v[i].suffix) << "\""
                  << ",\"dur\":" << v[i].duration
                  << ",\"dir\":" << (v[i].isDir ? "true" : "false")
                  << ",\"cv\":\"" << jesc(v[i].coverId) << "\"}";
            }
            j << "]}";
            return j.str();
        }
        if (rq.path.rfind("/api/src/cover", 0) == 0) {
            std::string id;
            const size_t q = rq.path.find("?id=");
            if (q != std::string::npos) id = urlDecode(rq.path.substr(q + 4));
            std::vector<unsigned char> bytes;
            if (!id.empty()) { auto rr = mgr.cover(id); if (rr.ok) bytes = rr.body; }
            ct = "image/jpeg";
            return std::string(bytes.begin(), bytes.end());
        }
        return "{\"ok\":false,\"error\":\"unknown src api\"}";
    }
    // ---------------- 数据源注册表(P1.5;设置页动态渲染依据) ----------------
    // GET /api/providers → 注册表内容 + 当前开关状态(rlplayer-settings.conf,旧键迁移读取)。
    // UI 启动时拉一次,按 items 逐源渲染 metasrc_<id> 开关 —— 新增数据源前端零改动。
    if (rq.path.rfind("/api/providers", 0) == 0) {
        ct = "application/json; charset=utf-8";
        const MetaSettings ms = meta_settings_load();
        std::ostringstream j;
        j << "{\"ok\":true,\"items\":[";
        bool first = true;
        for (const rta::meta::ProviderDesc& pd : rta::meta::provider_registry()) {
            if (!first) j << ",";
            first = false;
            const auto it = ms.srcOn.find(pd.id);
            const bool on = (it == ms.srcOn.end()) ? true : it->second;
            j << "{\"id\":\"" << jesc(pd.id) << "\",\"name\":\"" << jesc(pd.name)
              << "\",\"on\":" << (on ? "true" : "false")
              << ",\"canSearch\":" << (pd.canSearch ? "true" : "false")
              << ",\"canLyric\":" << (pd.canLyric ? "true" : "false") << "}";
        }
        j << "],\"llm\":" << (ms.llm ? "true" : "false")
          << ",\"on\":" << (ms.on ? "true" : "false") << "}";
        return j.str();
    }
    // ---------------- 单曲元数据补全(同步;按设置组装 EnrichOpts) ----------------
    // /api/enrich?file=...&force=1 → enrich_track 结果 JSON。正常命中数秒返回;
    // 全源超时的最坏情形可能超 20s(enrich.h 头注「未解决项」如实记录)。
    if (rq.path.rfind("/api/enrich", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::string file;
        bool force = false;
        {
            const size_t q = rq.path.find("?file=");
            if (q != std::string::npos) {
                const std::string rest = rq.path.substr(q + 6);
                const size_t amp = rest.find("&force=");
                if (amp != std::string::npos) {
                    file = urlDecode(rest.substr(0, amp));
                    force = rest.compare(amp + 7, 1, "1") == 0;
                } else {
                    file = urlDecode(rest);
                }
            }
        }
        if (file.empty()) return "{\"ok\":false,\"error\":\"未指定文件\"}";
        if (!valid_utf8(file)) return utf8_path_reject(file);   // F1：enrich_track 内部走 u8string→fs::path
        const rta::meta::EnrichStats st = rta::meta::enrich_track(file, force, meta_opts_from_settings());
        std::ostringstream j;
        j << "{\"ok\":" << (st.ok ? "true" : "false")
          << ",\"skipped\":" << (st.skipped ? "true" : "false")
          << ",\"nCand\":" << st.nCand
          << ",\"bestScore\":" << st.bestScore
          << ",\"bestSource\":\"" << jesc(st.bestSource) << "\""
          << ",\"confident\":" << (st.confident ? "true" : "false")
          << ",\"fields\":[";
        for (size_t i = 0; i < st.fieldsFilled.size(); ++i) {
            if (i) j << ",";
            j << "\"" << jesc(st.fieldsFilled[i]) << "\"";
        }
        j << "],\"note\":\"" << jesc(st.note) << "\"}";
        return j.str();
    }

    if (rq.path.rfind("/api/meta", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::string file;
        const size_t q = rq.path.find("?file=");
        if (q != std::string::npos) file = urlDecode(rq.path.substr(q + 6));
        if (file.empty()) return "{\"ok\":false,\"error\":\"未指定文件\"}";
        if (!valid_utf8(file)) return utf8_path_reject(file);   // F1：probe_media/get_cover 内部有 u8path
        const MediaInfo mi = probe_media(file);
        std::string mime;
        // 只探测有没有封面（不把图片塞进 JSON）
        auto c = get_cover(file, mime);
        const bool hasCover = c.size() > 128;
        // MetaStore 补全结果覆盖输出(P1):记录里值非空才覆盖;albumArtist/year/lyrics 是新增键
        rta::meta::TrackMeta tm;
        const bool haveMeta = rta::meta::MetaStore::instance().load(file, tm);
        std::string fTitle = mi.title, fArtist = mi.artist, fAlbum = mi.album, fGenre = mi.genre;
        std::string fAlbumArtist, fYear, fLyrics;
        std::map<std::string, std::string> metaSrc;     // 字段 → MetaValue.source(溯源表)
        auto metaOverride = [&](const char* key, std::string& dst) {
            if (!haveMeta) return;
            const auto it = tm.fields.find(key);
            if (it != tm.fields.end() && !it->second.value.empty()) {
                dst = it->second.value;
                metaSrc[key] = it->second.source;
            }
        };
        metaOverride("title", fTitle);
        metaOverride("artist", fArtist);
        metaOverride("album", fAlbum);
        metaOverride("albumArtist", fAlbumArtist);
        metaOverride("year", fYear);
        metaOverride("genre", fGenre);
        metaOverride("lyrics", fLyrics);
        std::ostringstream j;
        j << "{\"ok\":true"
          << ",\"title\":\"" << jesc(fTitle) << "\""
          << ",\"artist\":\"" << jesc(fArtist) << "\""
          << ",\"album\":\"" << jesc(fAlbum) << "\""
          << ",\"albumArtist\":\"" << jesc(fAlbumArtist) << "\""
          << ",\"year\":\"" << jesc(fYear) << "\""
          << ",\"genre\":\"" << jesc(fGenre) << "\""
          << ",\"lyrics\":\"" << jesc(fLyrics) << "\""
          << ",\"date\":\"" << jesc(mi.date) << "\""
          << ",\"track\":\"" << jesc(mi.track) << "\""
          << ",\"codec\":\"" << jesc(mi.codec) << "\""
          << ",\"sampleRate\":" << mi.sampleRate
          << ",\"channels\":" << mi.channels
          << ",\"bits\":" << mi.bits
          << ",\"hasCover\":" << (hasCover ? "true" : "false")
          << ",\"meta\":{\"enriched\":" << (meta_enriched(tm) ? "true" : "false")   // F2：真被在线补过才算
          << ",\"sources\":{";
        {
            bool firstS = true;
            for (const auto& kv : metaSrc) {
                if (!firstS) j << ",";
                firstS = false;
                j << "\"" << jesc(kv.first) << "\":\"" << jesc(kv.second) << "\"";
            }
        }
        j << "}}"
          << "}";
        return j.str();
    }

    // ---------------- 标签写回(P2):预览 / 起批 / 状态轮询 ----------------
    // GET  /api/writeback(?dry=1)   纯只读扫描:记录数/可写数(有非空字段且非 DSD)/
    //                               逐条预览 JSON;不跑 ffmpeg、不写任何文件
    // POST /api/writeback {"apply":true}  起后台线程执行 writeback_library,立即返回;
    //                               已有批次在跑时 {"ok":false,"running":true}
    // GET  /api/writeback/status    {"running","done","total","written","skipped",
    //                               "failed","lastFailures"[≤10],"error"}
    if (rq.path.rfind("/api/writeback", 0) == 0) {
        ct = "application/json; charset=utf-8";
        if (rq.path.rfind("/api/writeback/status", 0) == 0) {
            std::lock_guard<std::mutex> lk(g_wb.mtx);
            std::ostringstream j;
            j << "{\"ok\":true,\"running\":" << (g_wb.running ? "true" : "false")
              << ",\"dryRun\":" << (g_wb.dryRun ? "true" : "false")
              << ",\"done\":" << g_wb.done << ",\"total\":" << g_wb.total
              << ",\"written\":" << g_wb.written << ",\"skipped\":" << g_wb.skipped
              << ",\"failed\":" << g_wb.failed
              << ",\"error\":\"" << jesc(g_wb.err) << "\",\"lastFailures\":[";
            for (size_t i = 0; i < g_wb.lastFailures.size(); ++i) {
                if (i) j << ",";
                j << "{\"file\":\"" << jesc(g_wb.lastFailures[i].first)
                  << "\",\"err\":\"" << jesc(g_wb.lastFailures[i].second) << "\"}";
            }
            j << "]}";
            return j.str();
        }
        if (rq.method == "POST") {
            bool apply = false;
            const rta::net::Json jb = rta::net::json_parse(rq.body);
            if (jb.type == rta::net::Json::Obj && jb.has("apply"))
                apply = (jb["apply"].type == rta::net::Json::Bool) ? jb["apply"].b
                                                                   : (jb["apply"].n() != 0.0);
            if (!writeback_start(apply))
                return "{\"ok\":false,\"started\":false,\"running\":true,"
                       "\"error\":\"已有写回批次在进行中(见 /api/writeback/status)\"}";
            return std::string("{\"ok\":true,\"started\":true,\"apply\":") +
                   (apply ? "true" : "false") + "}";
        }
        // GET:预览扫描(纯只读,无 IO 写)
        const WbPreview w = writeback_preview_scan();
        std::ostringstream j;
        j << "{\"ok\":" << (w.err.empty() ? "true" : "false")
          << ",\"total\":" << w.total << ",\"writable\":" << w.writable
          << ",\"skipped\":" << w.skipped;
        if (!w.err.empty()) j << ",\"error\":\"" << jesc(w.err) << "\"";
        j << ",\"items\":[";
        for (size_t i = 0; i < w.items.size(); ++i) {
            if (i) j << ",";
            j << "{\"file\":\"" << jesc(w.items[i].file) << "\",\"writable\":"
              << (w.items[i].writable ? "true" : "false")
              << ",\"fields\":\"" << jesc(w.items[i].fields)
              << "\",\"reason\":\"" << jesc(w.items[i].reason) << "\"}";
        }
        j << "]}";
        return j.str();
    }

    // ---------------- 资料层(P3④a 实体权威文本与来源 materials): GET /api/entity/knowledge ----------------
    if (rq.path.rfind("/api/entity/knowledge", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::string kindStr = "person";
        std::string name;
        std::string lang = "zh";
        bool refresh = false;

        const size_t qpos = rq.path.find('?');
        if (qpos != std::string::npos) {
            std::istringstream is(rq.path.substr(qpos + 1));
            std::string tok;
            while (std::getline(is, tok, '&')) {
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = urlDecode(tok.substr(0, eq));
                const std::string v = urlDecode(tok.substr(eq + 1));
                if (k == "kind") kindStr = v;
                else if (k == "name") name = v;
                else if (k == "refresh") refresh = (v == "1" || v == "true");
                else if (k == "lang") lang = v;
            }
        }
        if (name.empty()) {
            return "{\"ok\":false,\"entity\":{\"id\":\"\",\"kind\":\"" + jesc(kindStr) +
                   "\",\"name\":\"\"},\"materials\":[],\"errors\":[{\"source\":\"params\",\"error\":\"未指定实体名称\"}],\"from_cache\":false,\"offline\":false}";
        }
        rta::meta::EntityKind ek = rta::meta::EntityKind::Person;
        if (!rta::meta::entity_kind_from_tag(kindStr, ek)) {
            return "{\"ok\":false,\"entity\":{\"id\":\"\",\"kind\":\"" + jesc(kindStr) +
                   "\",\"name\":\"" + jesc(name) + "\"},\"materials\":[],\"errors\":[{\"source\":\"params\",\"error\":\"未知实体类型: " + jesc(kindStr) + "\"}],\"from_cache\":false,\"offline\":false}";
        }

        rta::meta::KnowledgeOpts opts;
        opts.metabase = path_in_exe_dir("rlplayer-meta");
        opts.refresh = refresh;
        if (!lang.empty()) opts.lang = lang;

        const rta::meta::KnowledgeResult kr = rta::meta::knowledge_fetch(ek, name, opts);
        const bool ok = !kr.materials.empty();
        const std::string entId = kr.entityId.empty() ? rta::meta::entity_id(ek, name) : kr.entityId;
        const std::string kindTag = rta::meta::entity_kind_tag(ek);

        std::ostringstream j;
        j << "{\"ok\":" << (ok ? "true" : "false")
          << ",\"entity\":{"
          << "\"id\":\"" << jesc(entId) << "\","
          << "\"kind\":\"" << jesc(kindTag) << "\","
          << "\"name\":\"" << jesc(name) << "\""
          << "},"
          << "\"materials\":[";
        for (size_t i = 0; i < kr.materials.size(); ++i) {
            if (i) j << ",";
            const auto& m = kr.materials[i];
            j << "{"
              << "\"source\":\"" << jesc(m.source) << "\","
              << "\"kind\":\"" << jesc(m.kind) << "\","
              << "\"subject\":\"" << jesc(m.subject) << "\","
              << "\"title\":\"" << jesc(m.title) << "\","
              << "\"text\":\"" << jesc(m.text) << "\","
              << "\"url\":\"" << jesc(m.url) << "\","
              << "\"lang\":\"" << jesc(m.lang) << "\","
              << "\"fetched_at\":" << m.fetched_at << ","
              << "\"expires_at\":" << m.expires_at << ","
              << "\"authority\":" << m.authority
              << "}";
        }
        j << "],\"errors\":[";
        bool firstErr = true;
        for (const auto& ep : kr.errors) {
            if (!firstErr) j << ",";
            firstErr = false;
            j << "{\"source\":\"" << jesc(ep.first) << "\",\"error\":\"" << jesc(ep.second) << "\"}";
        }
        if (!ok && kr.errors.empty()) {
            const std::string errText = !kr.error.empty() ? kr.error : "未找到任何相关权威材料";
            j << "{\"source\":\"general\",\"error\":\"" << jesc(errText) << "\"}";
        }
        j << "],"
          << "\"from_cache\":" << (kr.from_cache ? "true" : "false") << ","
          << "\"offline\":" << (kr.offline ? "true" : "false")
          << "}";
        return j.str();
    }

    // ---------------- 资料层(P3③ AI 检索综合): GET /api/entity/summary ----------------
    if (rq.path.rfind("/api/entity/summary", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::string kindStr = "person";
        std::string name;
        std::string lang = "zh";
        bool refresh = false;

        const size_t qpos = rq.path.find('?');
        if (qpos != std::string::npos) {
            std::istringstream is(rq.path.substr(qpos + 1));
            std::string tok;
            while (std::getline(is, tok, '&')) {
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = urlDecode(tok.substr(0, eq));
                const std::string v = urlDecode(tok.substr(eq + 1));
                if (k == "kind") kindStr = v;
                else if (k == "name") name = v;
                else if (k == "refresh") refresh = (v == "1" || v == "true");
                else if (k == "lang") lang = v;
            }
        }
        if (name.empty()) {
            return "{\"ok\":false,\"error\":\"未指定实体名称\"}";
        }
        rta::meta::EntityKind ek = rta::meta::EntityKind::Person;
        if (!rta::meta::entity_kind_from_tag(kindStr, ek)) {
            return "{\"ok\":false,\"error\":\"未知实体类型: " + jesc(kindStr) + "\"}";
        }

        const MetaSettings ms = meta_settings_load();
        const std::string metabaseDir = path_in_exe_dir("rlplayer-meta");
        const std::string eid = rta::meta::entity_id(ek, name);

        // 成本策略：默认不自动生成
        // 1. 若未请求 refresh：优先检查未过期/离线缓存；若无缓存则如实返回 ok:false，绝不静默调 LLM
        if (!refresh) {
            rta::meta::SummaryResult cached;
            std::string cache_err;
            if (rta::meta::summary_load_cache(metabaseDir, eid, lang, cached, &cache_err) && cached.ok) {
                cached.from_cache = true;
                return rta::meta::summary_to_json(cached);
            }
            return "{\"ok\":false,\"error\":\"未生成 AI 简介（请点击「生成 AI 简介」）\",\"from_cache\":false,\"offline\":false}";
        }

        // 2. 显式请求 refresh=1：必须检查设置开关 metasummary
        if (!ms.summary) {
            return "{\"ok\":false,\"error\":\"未开启 AI 资料综合（请在设置-元数据页开启）\",\"from_cache\":false,\"offline\":false}";
        }

        rta::meta::RetrievalOptions ropts;
        ropts.metabase = metabaseDir;
        ropts.refresh = true;
        ropts.timeout_ms = 25000;

        const rta::meta::SummaryResult sr = rta::meta::retrieval_fetch(ek, name, lang, ropts);
        if (!sr.ok) {
            return "{\"ok\":false,\"error\":\"" + jesc(sr.error.empty() ? "AI 检索综合失败" : sr.error) + "\"}";
        }
        return rta::meta::summary_to_json(sr);
    }

    // ---------------- 资料层(P3④b Discogs L4 古典作品版本推荐): GET /api/entity/versions ----------------
    if (rq.path.rfind("/api/entity/versions", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::string kindStr = "work";
        std::string name;
        std::string composer;
        bool refresh = false;

        const size_t qpos = rq.path.find('?');
        if (qpos != std::string::npos) {
            std::istringstream is(rq.path.substr(qpos + 1));
            std::string tok;
            while (std::getline(is, tok, '&')) {
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = urlDecode(tok.substr(0, eq));
                const std::string v = urlDecode(tok.substr(eq + 1));
                if (k == "name" || k == "title") name = v;
                else if (k == "composer") composer = v;
                else if (k == "refresh") refresh = (v == "1" || v == "true");
                else if (k == "kind") kindStr = v;
            }
        }
        if (name.empty()) {
            return "{\"ok\":false,\"error\":\"未指定作品名称\"}";
        }

        rta::meta::DiscogsOptions dopts;
        dopts.metabase = path_in_exe_dir("rlplayer-meta");
        dopts.refresh = refresh;
        dopts.timeout_ms = 15000;

        const rta::meta::WorkVersionsResult wr = rta::meta::discogs_versions(name, composer, dopts);
        return rta::meta::work_versions_to_json(wr);
    }

    if (rq.path.rfind("/api/cover", 0) == 0) {
        std::string file;
        const size_t q = rq.path.find("?file=");
        if (q != std::string::npos) file = urlDecode(rq.path.substr(q + 6));
        if (!file.empty() && !valid_utf8(file)) {   // F1：get_cover/MetaStore 内部消费路径
            ct = "application/json; charset=utf-8";
            return utf8_path_reject(file);
        }
        std::string mime = "image/png";
        std::vector<unsigned char> bytes;
        if (!file.empty()) bytes = rta::audio::get_cover(file, mime);
        // 内嵌/本地图片读取失败(≤128B = get_cover 的占位 PNG)→ MetaStore 缓存封面兜底
        if (file.empty() || bytes.size() <= 128) {
            std::vector<unsigned char> mb;
            std::string mm;
            if (!file.empty() && rta::meta::MetaStore::instance().cover(file, mb, mm) && !mb.empty()) {
                bytes = std::move(mb);
                mime = mm;
            }
        }
        ct = mime;
        return std::string(bytes.begin(), bytes.end());
    }

    if (rq.path.rfind("/api/shuffle", 0) == 0) {
        ct = "application/json; charset=utf-8";
        int on = 1;
        const size_t q = rq.path.find("?on=");
        if (q != std::string::npos) on = atoi(rq.path.c_str() + q + 4);
        {
            std::lock_guard<std::mutex> lk(g_q.mtx);
            // 记住"正在播的那首"，重排后要让 index 仍然指向它，
            // 否则一按随机就会跳歌（这是最容易被用户当成 bug 的细节）
            const std::string cur = (g_q.index < g_q.items.size()) ? g_q.items[g_q.index] : std::string();
            const bool want = (on != 0);
            if (want && !g_q.shuffle) {
                g_q.base = g_q.items;                 // 保存原顺序，关掉时能还原
                if (g_q.items.size() > 1) {
                    std::mt19937 rng((unsigned)GetTickCount64());
                    std::shuffle(g_q.items.begin(), g_q.items.end(), rng);
                }
            } else if (!want && g_q.shuffle) {
                if (!g_q.base.empty()) g_q.items = g_q.base;   // 还原原始顺序
            }
            g_q.shuffle = want;
            if (!cur.empty()) {
                for (size_t i = 0; i < g_q.items.size(); ++i)
                    if (g_q.items[i] == cur) { g_q.index = i; break; }
            }
        }
        queue_save();
        return std::string("{\"ok\":true,\"shuffle\":") + (on ? "true" : "false") + "}";
    }
    if (rq.path.rfind("/api/queue", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::lock_guard<std::mutex> lk(g_q.mtx);
        std::ostringstream j;
        j << "{\"ok\":true,\"index\":" << g_q.index
          << ",\"repeat\":" << g_q.repeat
          << ",\"shuffle\":" << (g_q.shuffle ? "true" : "false") << ",\"items\":[";
        for (size_t i = 0; i < g_q.items.size(); ++i) {
            if (i) j << ",";
            j << "\"" << jesc(g_q.items[i]) << "\"";
        }
        j << "]}";
        return j.str();
    }

    if (rq.path.rfind("/api/clear", 0) == 0) {
        ct = "application/json; charset=utf-8";
        ps.stop.store(true);
        ps.joinIfDone();
        ps.stop.store(false);
        ps.playing.store(false);
        {
            std::lock_guard<std::mutex> lk(g_lastEndMx);
            g_lastEnd.isPosZeroNoErr = false;
        }
        { std::lock_guard<std::mutex> lk(g_q.mtx); g_q.items.clear(); g_q.index = 0; }
        queue_save();
        return "{\"ok\":true}";
    }

    if (rq.path.rfind("/api/repeat", 0) == 0) {
        ct = "application/json; charset=utf-8";
        PlayParams p = parseForm(rq.body);
        int mode = atoi(p.rate.c_str());
        if (mode < 0 || mode > 2) mode = 0;
        {
            std::lock_guard<std::mutex> lk(g_q.mtx);
            g_q.repeat = mode;
        }
        queue_save();
        return std::string("{\"ok\":true,\"repeat\":") + std::to_string(mode) + "}";
    }

    if (rq.path.rfind("/api/skip", 0) == 0) {
        // ?dir=1 下一首，?dir=-1 上一首
        ct = "application/json; charset=utf-8";
        int dir = 1;
        const size_t q = rq.path.find("?dir=");
        if (q != std::string::npos) dir = atoi(rq.path.c_str() + q + 5);

        ps.stop.store(true);
        ps.joinIfDone();
        ps.stop.store(false);

        PlayParams p = g_params;
        size_t target = 0;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(g_q.mtx);
            if (g_q.items.empty()) return "{\"ok\":false,\"error\":\"队列为空\"}";
            if (dir > 0) {
                if (g_q.index + 1 < g_q.items.size()) { target = g_q.index + 1; ok = true; }
                else if (g_q.repeat != 0) { target = 0; ok = true; }
            } else {
                if (g_q.index > 0) { target = g_q.index - 1; ok = true; }
                else if (g_q.repeat != 0) { target = g_q.items.size() - 1; ok = true; }
            }
        }
        if (!ok) return "{\"ok\":false,\"error\":\"已到队列边界\"}";
        ps.played.store(0);
        ps.total.store(0);
        ps.playing.store(true);
        ps.th = std::thread(queue_thread, p, target);
        return "{\"ok\":true,\"index\":" + std::to_string(target) + "}";
    }

    // ---------------- UPnP/DLNA 媒体投放（ROADMAP P5b）----------------
    if (rq.path.rfind("/api/cast", 0) == 0) {
        ct = "application/json; charset=utf-8";
        if (rq.path.rfind("/api/cast/devices", 0) == 0) {
            bool forceRescan = (rq.path.find("scan=1") != std::string::npos || rq.path.find("rescan=1") != std::string::npos);
            auto devs = rta::net::CastManager::instance().get_devices(forceRescan);
            std::ostringstream j;
            j << "{\"ok\":true,\"n\":" << devs.size() << ",\"devices\":[";
            for (size_t i = 0; i < devs.size(); ++i) {
                if (i) j << ",";
                j << "{\"name\":\"" << jesc(devs[i].name) << "\""
                  << ",\"friendlyName\":\"" << jesc(devs[i].name) << "\""
                  << ",\"model\":\"" << jesc(devs[i].model) << "\""
                  << ",\"host\":\"" << jesc(devs[i].host) << "\""
                  << ",\"port\":" << devs[i].port
                  << ",\"location\":\"" << jesc(devs[i].location) << "\""
                  << ",\"controlUrl\":\"" << jesc(devs[i].av_transport_url()) << "\""
                  << ",\"avTransportUrl\":\"" << jesc(devs[i].av_transport_url()) << "\""
                  << ",\"connectionManagerUrl\":\"" << jesc(devs[i].connection_manager_url()) << "\""
                  << ",\"hasAVTransport\":" << (!devs[i].control.empty() ? "true" : "false")
                  << "}";
            }
            j << "]}";
            return j.str();
        }
        if (rq.path.rfind("/api/cast/state", 0) == 0) {
            auto st = rta::net::CastManager::instance().get_state();
            std::ostringstream j;
            j << "{\"ok\":true"
              << ",\"running\":" << (st.running ? "true" : "false")
              << ",\"device\":\"" << jesc(st.device) << "\""
              << ",\"sourceFile\":\"" << jesc(st.sourceFile) << "\""
              << ",\"servePort\":" << st.servePort
              << ",\"phase\":\"" << jesc(st.phase) << "\""
              << ",\"positionSec\":" << st.positionSec
              << ",\"transportState\":\"" << jesc(st.transportState) << "\""
              << ",\"servedBytes\":" << st.servedBytes
              << ",\"requestCount\":" << st.requestCount
              << ",\"rateMatch\":\"" << jesc(st.rateMatch) << "\""
              << ",\"warning\":\"" << jesc(st.warning) << "\""
              << ",\"error\":\"" << jesc(st.error) << "\""
              << "}";
            return j.str();
        }
        if (rq.path.rfind("/api/cast/start", 0) == 0) {
            if (rq.method != "POST") return "{\"ok\":false,\"error\":\"Method Not Allowed\"}";
            rta::net::CastStartParams p;
            if (!rq.body.empty() && rq.body.front() == '{') {
                auto j = rta::net::json_parse(rq.body);
                if (j.has("dev")) p.dev = j["dev"].s();
                if (j.has("file")) p.file = j["file"].s();
                if (j.has("port")) p.port = (uint16_t)j["port"].n();
                if (j.has("rate")) p.rate = (uint32_t)j["rate"].n();
                if (j.has("bits")) p.bits = (int)j["bits"].n();
                if (j.has("seconds")) p.seconds = j["seconds"].n();
                if (j.has("dop")) p.dop = (j["dop"].type == rta::net::Json::Bool) ? j["dop"].b : (j["dop"].n() != 0.0);
            } else {
                std::istringstream is(rq.body);
                std::string tok;
                while (std::getline(is, tok, '&')) {
                    size_t eq = tok.find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = urlDecode(tok.substr(0, eq));
                    std::string v = urlDecode(tok.substr(eq + 1));
                    if (k == "dev") p.dev = v;
                    else if (k == "file") p.file = v;
                    else if (k == "port") p.port = (uint16_t)std::atoi(v.c_str());
                    else if (k == "rate") p.rate = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
                    else if (k == "bits") p.bits = std::atoi(v.c_str());
                    else if (k == "seconds") p.seconds = std::atof(v.c_str());
                    else if (k == "dop") p.dop = (v == "1" || v == "true");
                }
            }
            if (p.port == 0) p.port = 8912;
            auto res = rta::net::CastManager::instance().start(p);
            if (!res.ok) {
                return "{\"ok\":false,\"error\":\"" + jesc(res.error) + "\"}";
            }
            return "{\"ok\":true,\"started\":true,\"phase\":\"discovering\"}";
        }
        if (rq.path.rfind("/api/cast/stop", 0) == 0) {
            rta::net::CastManager::instance().stop();
            return "{\"ok\":true,\"stopped\":true}";
        }
        return "{\"ok\":false,\"error\":\"未知的投放 API 路径\"}";
    }

    if (rq.path.rfind("/api/upnp", 0) == 0) {
        ct = "application/json; charset=utf-8";
        const auto devs = rta::net::upnp_discover(1800);
        std::ostringstream j;
        j << "{\"ok\":true,\"n\":" << devs.size() << ",\"devices\":[";
        for (size_t i = 0; i < devs.size(); ++i) {
            if (i) j << ",";
            j << "{\"name\":\"" << jesc(devs[i].name) << "\""
              << ",\"model\":\"" << jesc(devs[i].model) << "\""
              << ",\"host\":\"" << jesc(devs[i].host) << "\":\"" << devs[i].port << "\""
              << ",\"loc\":\"" << jesc(devs[i].location) << "\"}";
        }
        j << "]}";
        return j.str();
    }

    // ---------------- DeepSeek API Key（2026-10-22 功能4）----------------
    // ★ 不并入 /api/settings：那个端点写 rlplayer-settings.conf（整文件重写），
    //   key 混进去会被诊断包 / 导出 / 同步顺手带走，而且 POST 白名单里也不该有它。
    //   本端点单独写 rlplayer-llm.conf，且 GET 只回掩码。
    if (rq.path.rfind("/api/llm", 0) == 0) {
        ct = "application/json; charset=utf-8";
        if (rq.path.rfind("/api/llm/test", 0) == 0) return llm_test_json();
        if (rq.method != "POST") return llm_status_json();

        std::map<std::string, std::string> f;
        {
            std::istringstream is(rq.body);
            std::string tok;
            while (std::getline(is, tok, (char)38)) {
                const size_t e = tok.find((char)61);
                if (e == std::string::npos) continue;
                f[urlDecode(tok.substr(0, e))] = urlDecode(tok.substr(e + 1));
            }
        }
        // 空 body 绝不落盘：GET/POST 撞车或前端 bug 时，整份配置不能被"空写"清掉。
        if (f.empty()) return "{\"ok\":false,\"error\":\"没有参数（拒绝空写入，避免误清空配置）\"}";
        const bool clear    = (f.count("clear") != 0) && (f["clear"] == "1" || f["clear"] == "true");
        const bool hasKey   = (f.count("key") != 0) && !f["key"].empty();
        const bool hasModel = (f.count("model") != 0) && !f["model"].empty();
        const bool hasBase  = (f.count("base_url") != 0) && !f["base_url"].empty();
        const bool hasCap   = (f.count("llmmonthlycap") != 0) || (f.count("monthly_cap") != 0);
        if (!clear && !hasKey && !hasModel && !hasBase && !hasCap)
            return "{\"ok\":false,\"error\":\"没有可保存的字段\"}";
        if (hasKey && !llm_value_ok(f["key"], 512))
            return "{\"ok\":false,\"error\":\"API Key 格式不对（不允许空格/控制字符，最长 512）\"}";
        if (hasModel && !llm_value_ok(f["model"], 128))
            return "{\"ok\":false,\"error\":\"模型名格式不对\"}";
        if (hasBase && !llm_value_ok(f["base_url"], 512))
            return "{\"ok\":false,\"error\":\"base_url 格式不对\"}";

        rta::meta::LlmConfig c = rta::meta::llm_config_load(llm_conf_path());
        std::string changed;
        auto note = [&changed](const char* s) { if (!changed.empty()) changed += ","; changed += s; };
        if (clear)         { c.apiKey.clear(); note("key"); }
        else if (hasKey)   { c.apiKey = f["key"]; note("key"); }
        if (hasModel)      { c.model = f["model"]; note("model"); }
        if (hasBase)       { c.baseUrl = rta::meta::llm_base_url(f["base_url"]); note("baseUrl"); }
        if (hasCap) {
            std::string capStr = f.count("llmmonthlycap") ? f["llmmonthlycap"] : f["monthly_cap"];
            c.monthlyCap = atoi(capStr.c_str());
            note("monthlyCap");
        }
        if (!rta::meta::llm_config_save_merge(c, llm_conf_path()))
            return "{\"ok\":false,\"error\":\"写入 rlplayer-llm.conf 失败（文件被占用？）\"}";
        // 日志只记「改了哪些字段 + 掩码」，绝不记值：launcher/playback 日志会进诊断包。
        play_log("LLM_CONF 更新 fields=" + changed +
                 " key=" + (c.apiKey.empty() ? std::string("(未设置)") : rta::meta::llm_mask_key(c.apiKey)) +
                 " model=" + c.model + " (proxy 行等既有内容保留)");
        std::string out = llm_status_json();
        if (!out.empty() && out.back() == '}') {
            out.pop_back();
            out += ",\"changed\":\"" + jesc(changed) + "\"}";
        }
        return out;
    }

    // ---------------- 耳机 EQ（OPRA 预设管理）----------------
    if (rq.path.rfind("/api/opra/index", 0) == 0) {
        ct = "application/json; charset=utf-8";
        rta::dsp::OpraService::instance().init(path_in_exe_dir("data\\opra_index.json"), path_in_exe_dir(""));
        const auto& index = rta::dsp::OpraService::instance().index();
        std::string q;
        const size_t qpos = rq.path.find("?q=");
        if (qpos != std::string::npos) {
            q = urlDecode(rq.path.substr(qpos + 3));
        }
        std::ostringstream j;
        if (!q.empty()) {
            auto items = index.filter(q, 100);
            j << "{\"ok\":true,\"count\":" << items.size() << ",\"items\":[";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) j << ",";
                j << "[\"" << jesc(items[i].id) << "\",\"" << jesc(items[i].vendor) << "\",\"" << jesc(items[i].name) << "\"]";
            }
            j << "]}";
        } else {
            j << "{\"ok\":true,\"count\":" << index.size() << ",\"items\":[";
            bool first = true;
            for (const auto& it : index.items()) {
                if (!first) j << ",";
                first = false;
                j << "[\"" << jesc(it.id) << "\",\"" << jesc(it.vendor) << "\",\"" << jesc(it.name) << "\"]";
            }
            j << "]}";
        }
        return j.str();
    }
    if (rq.path.rfind("/api/opra/presets", 0) == 0) {
        ct = "application/json; charset=utf-8";
        rta::dsp::OpraService::instance().init(path_in_exe_dir("data\\opra_index.json"), path_in_exe_dir(""));
        std::string productId;
        bool forceRefresh = false;
        const size_t qpos = rq.path.find('?');
        if (qpos != std::string::npos) {
            std::istringstream qs(rq.path.substr(qpos + 1));
            std::string tok;
            while (std::getline(qs, tok, '&')) {
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = urlDecode(tok.substr(0, eq));
                const std::string v = urlDecode(tok.substr(eq + 1));
                if (k == "productId" || k == "id") productId = v;
                else if (k == "refresh") forceRefresh = (v == "1" || v == "true");
            }
        }
        if (productId.empty()) {
            return "{\"ok\":false,\"error\":\"缺少 productId 参数\"}";
        }
        std::vector<rta::dsp::OpraPreset> presets;
        bool fromCache = false;
        std::string err;
        bool ok = rta::dsp::OpraService::instance().get_presets(productId, presets, fromCache, err, forceRefresh);
        if (!ok && presets.empty()) {
            std::ostringstream j;
            j << "{\"ok\":false,\"error\":\"" << jesc(err.empty() ? "获取预设失败" : err) << "\"}";
            return j.str();
        }
        std::ostringstream j;
        j << "{\"ok\":true,\"fromCache\":" << (fromCache ? "true" : "false");
        if (!err.empty()) j << ",\"msg\":\"" << jesc(err) << "\"";
        j << ",\"items\":[";
        for (size_t i = 0; i < presets.size(); ++i) {
            const auto& p = presets[i];
            if (i) j << ",";
            j << "{\"id\":\"" << jesc(p.id) << "\""
              << ",\"author\":\"" << jesc(p.author) << "\""
              << ",\"details\":\"" << jesc(p.details) << "\""
              << ",\"link\":\"" << jesc(p.link) << "\""
              << ",\"gainDb\":" << p.gainDb
              << ",\"bandsCount\":" << p.bands.size()
              << "}";
        }
        j << "]}";
        return j.str();
    }
    if (rq.path.rfind("/api/opra/apply", 0) == 0) {
        ct = "application/json; charset=utf-8";
        rta::dsp::OpraService::instance().init(path_in_exe_dir("data\\opra_index.json"), path_in_exe_dir(""));
        std::string productId, presetId;
        std::istringstream is(rq.body);
        std::string tok;
        while (std::getline(is, tok, '&')) {
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = urlDecode(tok.substr(0, eq));
            const std::string v = urlDecode(tok.substr(eq + 1));
            if (k == "productId") productId = v;
            else if (k == "presetId") presetId = v;
        }
        if (productId.empty() || presetId.empty()) {
            return "{\"ok\":false,\"error\":\"缺少 productId 或 presetId 参数\"}";
        }
        std::string outRelPath, err;
        bool ok = rta::dsp::OpraService::instance().apply_preset(productId, presetId, path_in_exe_dir(""), outRelPath, err);
        if (!ok) {
            std::ostringstream j;
            j << "{\"ok\":false,\"error\":\"" << jesc(err) << "\"}";
            return j.str();
        }
        std::ostringstream j;
        j << "{\"ok\":true,\"path\":\"" << jesc(outRelPath) << "\""
          << ",\"productId\":\"" << jesc(productId) << "\""
          << ",\"presetId\":\"" << jesc(presetId) << "\"}";
        return j.str();
    }

    // 局域网监听状态（控制面默认只对本机开放）
    // ---------------- 播放设置持久化 ----------------
    // 没有这个，用户每次重启都要重新选滤波器/抖动/缓冲 —— 日常用起来很烦。
    if (rq.path.rfind("/api/settings", 0) == 0) {
        ct = "application/json; charset=utf-8";
        const std::string sp = path_in_exe_dir("rlplayer-settings.conf");
        if (rq.method == "POST") {
            std::ofstream f(sp, std::ios::trunc);
            if (!f) return "{\"ok\":false,\"error\":\"写入失败\"}";
            std::istringstream is(rq.body);
            std::string tok;
            while (std::getline(is, tok, (char)38)) {
                const size_t e = tok.find((char)61);
                if (e == std::string::npos) continue;
                const std::string k = urlDecode(tok.substr(0, e));
                const std::string v = urlDecode(tok.substr(e + 1));
                if (k.empty() || v.size() > 512) continue;
                bool okKey = false;
                for (const char* allow : { "rate", "filter", "filterNx", "shaper", "dsd", "modorder",
                                           "buffer", "volume", "loudness", "isp", "twostage",
                                           "device", "bits", "taps", "atten", "transition", "phase",
                                           "modfamily", "oobgain", "ahmbits", "modcut", "moddith", "ir", "irgpu", "ramp", "theme", "eq",
                                           "gpuvendor", "dsdnative", "directsdm", "dsdkind", "dsdconv", "dsd2stage",
                                           "backend", "naahost", "channels", "trim",
                                           "inbackend", "indev", "inoffset", "inbuf", "inmon",
                                           "inseconds", "inout",
                                           "sdmfilt", "sdmfiltnx", "streamdecode",
                                           // 耳机 EQ (OPRA)
                                           "opraproduct", "oprapreset", "opravendor",
                                           // 元数据补全(P1.5):总开关与 LLM 固定两键;
                                           // 源开关 metasrc_<id> 按注册表动态放行(见下);
                                           // P3④b AI 综合开关 metasummary (默认0)
                                           "metaon", "metallm", "metasummary", "llmmonthlycap", "scanthreads", "scanThreads" })
                    if (k == allow) okKey = true;
                if (!okKey && k.rfind("metasrc_", 0) == 0) {
                    const std::string id = k.substr(8);
                    for (const rta::meta::ProviderDesc& pd : rta::meta::provider_registry())
                        if (id == pd.id) { okKey = true; break; }
                }
                if (!okKey) continue;
                std::string clean;
                for (char ch : v) if (ch != 10 && ch != 13) clean += ch;
                if (k == "scanThreads" || k == "scanthreads") {
                    const int st = atoi(clean.c_str());
                    if (st > 0) Library::instance().setScanThreads((size_t)st);
                }
                if (k == "streamdecode") {
                    rta::audio::set_stream_decode_enabled(clean != "0" && clean != "false");
                }
                f << k << "=" << clean << (char)10;
            }
            return "{\"ok\":true}";
        }
        std::ifstream f(sp);
        std::ostringstream j;
        j << "{\"ok\":true,\"kv\":{";
        std::string line; bool first = true;
        std::set<std::string> seen;                 // 元数据键缺省带出(缺省全开),见下
        std::map<std::string, std::string> kvs;     // 全量 k→v(旧键迁移带值用)
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == 13) line.pop_back();
            const size_t e = line.find('=');
            if (e == std::string::npos) continue;
            const std::string k = line.substr(0, e);
            seen.insert(k);
            kvs[k] = line.substr(e + 1);
            if (!first) j << ",";
            first = false;
            j << "\"" << jesc(k) << "\":\"" << jesc(line.substr(e + 1)) << "\"";
        }
        // 元数据开关缺省值(P1.5):没写进 conf 也要让 UI 拿到「开」。源开关输出新键
        // metasrc_<id>:新键未写而旧键(metaqq 等)存在 → 迁移带出旧值;都没有 → "1"。
        // 只读迁移 —— 写侧(POST 白名单)只认新键,下次保存后旧键自然消失。
        auto injectKv = [&](const std::string& k, const std::string& v) {
            if (seen.count(k) != 0) return;
            if (!first) j << ",";
            first = false;
            j << "\"" << jesc(k) << "\":\"" << jesc(v) << "\"";
        };
        injectKv("metaon", "1");
        injectKv("metallm", "1");
        injectKv("metasummary", "0");
        injectKv("llmmonthlycap", "0");
        injectKv("streamdecode", "1");
        for (const rta::meta::ProviderDesc& pd : rta::meta::provider_registry()) {
            const std::string nk = std::string("metasrc_") + pd.id;
            if (seen.count(nk) != 0) continue;
            const auto it = kvs.find("meta" + std::string(pd.id));   // metaqq/metanetease/metaitunes/metalrclib
            injectKv(nk, it == kvs.end() ? std::string("1") : it->second);
        }
        j << "}}";
        return j.str();
    }

    // ASIO 驱动列表（设置页里显示，让用户知道本机有什么 ASIO 后端）
    if (rq.path.rfind("/api/asio", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::ostringstream j;
        j << "{\"ok\":true,\"drivers\":[";
        HKEY hk = nullptr;
        bool first = true;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            for (DWORD i = 0;; ++i) {
                wchar_t name[256];
                DWORD len = 256;
                if (RegEnumKeyExW(hk, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                char buf[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
                if (!first) j << ",";
                first = false;
                j << "\"" << jesc(buf) << "\"";
            }
            RegCloseKey(hk);
        }
        j << "]}";
        return j.str();
    }

    // 输出设备列表（设置页里可以选，不再写死"默认设备"）
    if (rq.path.rfind("/api/devices", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::ostringstream j;
        j << "{\"ok\":true,\"devices\":[";
        bool first = true;
        for (const auto& d : WasapiOutput::enumerate()) {
            if (d.state != DEVICE_STATE_ACTIVE) continue;
            if (!first) j << ",";
            first = false;
            j << "{\"id\":\"" << jesc(d.id) << "\",\"name\":\"" << jesc(d.name) << "\""
              << ",\"default\":" << (d.isDefault ? "true" : "false") << "}";
        }
        j << "]}";
        return j.str();
    }

    // 滤波器预设表（含每个预设【自带的相位】）：
    // 用户 2026-10-22 功能2 的诉求是「'跟随预设'到底跟的是什么」说不清。
    // 让后端把真相源交出来，前端只负责显示 —— 避免前端再抄一份相位表（那必然漂移）。
    if (rq.path.rfind("/api/filters", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::ostringstream j;
        j << "{\"ok\":true,\"items\":[";
        bool firstF = true;
        for (const auto& fp : rta::dsp::filter_presets()) {
            if (!firstF) j << ",";
            firstF = false;
            const char* ph = (fp.phase == rta::dsp::PhaseKind::Minimum) ? "minimum"
                           : (fp.phase == rta::dsp::PhaseKind::Intermediate) ? "intermediate"
                           : "linear";
            const char* phCn = (fp.phase == rta::dsp::PhaseKind::Minimum) ? "最小相位"
                             : (fp.phase == rta::dsp::PhaseKind::Intermediate) ? "混合相位"
                             : "线性相位";
            j << "{\"name\":\"" << jesc(fp.name) << "\""
              << ",\"phase\":\"" << ph << "\""
              << ",\"phaseName\":\"" << phCn << "\""
              << ",\"taps\":" << fp.taps
              << ",\"twoStage\":" << (fp.twoStage ? "true" : "false")
              << ",\"forNx\":" << (fp.forNx ? "true" : "false") << "}";
        }
        j << "]}";
        return j.str();
    }

    // 设备能力：只让用户选设备真正支持的采样率（选了不支持的一播放就报错，体验很差）
    if (rq.path.rfind("/api/rates", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::string devId;
        { const size_t q = rq.path.find("?device="); if (q != std::string::npos) devId = urlDecode(rq.path.substr(q + 8)); }
        // 同一套带重试/缓存的探测：页面刚播完一首就拉列表时，同样会撞上端点重配置的瞬时空表。
        // 探测成功会刷新"上次成功列表"缓存，rate=max 在探测为空时可以据此沿用（RATEPROBE_STALE）。
        const auto rates = probe_exclusive_rates(devId, 24, 2);
        std::string dev = "默认音频设备";
        for (const auto& d : WasapiOutput::enumerate()) if (d.isDefault) dev = d.name;
        // 顺带带出"系统当前配置的采样率"（GetMixFormat，只读、不开流、不出声）：
        // 设置页拿它把「跟随系统 (48000 Hz)」写清楚，用户就不用猜这一档到底是多少。
        uint32_t sysRate = 0; int sysCh = 0; bool sysFloat = false; std::string sysErr;
        if (!WasapiSharedOutput::query_mix_format(devId, sysRate, sysCh, sysFloat, sysErr)) sysRate = 0;
        std::ostringstream j;
        j << "{\"ok\":true,\"device\":\"" << jesc(dev) << "\",\"sysRate\":" << sysRate
          << ",\"rates\":[";
        for (size_t i = 0; i < rates.size(); ++i) { if (i) j << ","; j << rates[i]; }
        j << "]}";
        return j.str();
    }

    // 拖拽兜底：浏览器拿不到真实路径时，把文件内容传上来，落到 exe 目录下的 uploads
    if (rq.path.rfind("/api/upload", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::string name = "upload.bin";
        const size_t qp = rq.path.find("?name=");
        if (qp != std::string::npos) name = urlDecode(rq.path.substr(qp + 6));
        std::string safe;
        for (char ch : name) {
            const unsigned char c = (unsigned char)ch;
            if (c < 0x20 || ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
                ch == '"' || ch == '<' || ch == '>' || ch == '|') safe += '_';
            else safe += ch;
        }
        if (safe.empty()) safe = "upload.bin";
        if (!valid_utf8(safe)) return utf8_path_reject(name);   // F1：下方 fs::u8path(full) 消费
        std::error_code ec;
        const std::string dir = path_in_exe_dir("uploads");
        fs::create_directories(fs::u8path(dir), ec);
        const std::string full = dir + "\\" + safe;
        std::ofstream of(fs::u8path(full), std::ios::binary | std::ios::trunc);
        if (!of) return "{\"ok\":false,\"error\":\"无法写入上传目录\"}";
        of.write(rq.body.data(), (std::streamsize)rq.body.size());
        of.close();
        return std::string("{\"ok\":true,\"path\":\"") + jesc(full) + "\",\"size\":" +
               std::to_string((long long)rq.body.size()) + "}";
    }

    // ASIO 驱动列表（后端下拉里选 ASIO 时用它填设备）
    if (rq.path.rfind("/api/asiodevices", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::ostringstream j;
        j << "{\"ok\":true,\"drivers\":[";
        bool first = true;
        for (const auto& e : rta::audio::asio_enum_drivers()) {
            if (!first) j << ",";
            first = false;
            j << "{\"name\":\"" << jesc(e.name) << "\",\"id\":\"" << jesc(e.clsid) << "\"}";
        }
        j << "]}";
        return j.str();
    }

    // ---------------- ROADMAP P4b：输入采集与直通监听 API ----------------
    // 设备列表：含 loopback 端点标记、默认、混音率/声道/位深
    if (rq.path.rfind("/api/input/devices", 0) == 0 || rq.path.rfind("/api/indevices", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::ostringstream j;
        j << R"({"ok":true,"devices":[)";
        bool first = true;
        for (const auto& d : rta::audio::WasapiCapture::enumerate_capture(true)) {
            if (d.state != DEVICE_STATE_ACTIVE) continue;
            if (!first) j << ",";
            first = false;
            j << R"({"id":")" << jesc(d.id) << R"(",)"
              << R"("name":")" << jesc(d.name) << R"(",)"
              << R"("state":)" << (unsigned long)d.state << ","
              << R"("isDefault":)" << (d.isDefault ? "true" : "false") << ","
              << R"("isLoopback":)" << (d.isLoopback ? "true" : "false") << ","
              << R"("mixRate":)" << d.mixRate << ","
              << R"("mixChannels":)" << d.mixChannels << ","
              << R"("mixBits":)" << d.mixBits
              << "}";
        }
        j << "]}";
        return j.str();
    }

    // 实时状态：{running, mode(record|monitor|null), device, rate, channels, seconds, frames, water, overflow, underflow, posClips, negClips, outFile, rmsDb, peakDb, error}
    if (rq.path.rfind("/api/input/state", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::lock_guard<std::mutex> lk(g_inputSession.mtx);
        std::ostringstream j;
        j << R"({"ok":true)";
        j << R"(,"running":)" << (g_inputSession.running.load() ? "true" : "false");
        j << R"(,"mode":")" << jesc(g_inputSession.mode) << R"(")";
        j << R"(,"device":")" << jesc(g_inputSession.device) << R"(")";
        j << R"(,"rate":)" << g_inputSession.rate;
        j << R"(,"channels":)" << g_inputSession.channels;
        j << R"(,"seconds":)" << g_inputSession.seconds;
        j << R"(,"frames":)" << g_inputSession.frames;
        j << R"(,"water":)" << g_inputSession.water;
        j << R"(,"overflow":)" << g_inputSession.overflow;
        j << R"(,"underflow":)" << g_inputSession.underflow;
        j << R"(,"posClips":)" << g_inputSession.posClips;
        j << R"(,"negClips":)" << g_inputSession.negClips;
        j << R"(,"outFile":")" << jesc(g_inputSession.outFile) << R"(")";
        j << R"(,"rmsDb":)" << g_inputSession.rmsDb;
        j << R"(,"peakDb":)" << g_inputSession.peakDb;
        j << R"(,"error":")" << jesc(g_inputSession.error) << R"(")";
        j << "}";
        return j.str();
    }

    // 启动采集：POST /api/input/start
    // 参数：mode=record|monitor、dev=<子串或 id>、loopback=1、out=<wav>、seconds=<上限>；
    // 测试用：src=null&inject=1000:0.001 走模拟源（不开设备、不发声），用于自动化门禁
    if (rq.path.rfind("/api/input/start", 0) == 0) {
        ct = "application/json; charset=utf-8";
        auto parseParams = [](const rta::net::HttpRequest& req) {
            std::map<std::string, std::string> m;
            auto trim = [](std::string s) {
                while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
                size_t p = 0;
                while (p < s.size() && (s[p] == '\r' || s[p] == '\n' || s[p] == ' ' || s[p] == '\t')) p++;
                return s.substr(p);
            };
            auto parseStr = [&](const std::string& str) {
                std::istringstream is(str);
                std::string tok;
                while (std::getline(is, tok, '&')) {
                    tok = trim(tok);
                    if (tok.empty()) continue;
                    const size_t e = tok.find('=');
                    if (e == std::string::npos) {
                        m[trim(urlDecode(tok))] = "1";
                    } else {
                        m[trim(urlDecode(tok.substr(0, e)))] = trim(urlDecode(tok.substr(e + 1)));
                    }
                }
            };
            const size_t q = req.path.find('?');
            if (q != std::string::npos) parseStr(req.path.substr(q + 1));
            if (!req.body.empty()) parseStr(req.body);
            return m;
        };

        auto params = parseParams(rq);
        if (g_inputSession.running.load()) {
            return R"({"ok":false,"error":"设备忙：采集已在运行中"})";
        }
        g_inputSession.reap();

        std::string mode = "record";
        if (params.count("mode")) mode = params["mode"];
        std::string src = params.count("src") ? params["src"] : "";
        if (src == "null" && (!params.count("mode") || params["mode"] == "null")) {
            mode = "null";
        }
        if (mode != "record" && mode != "monitor" && mode != "null") {
            return R"({"ok":false,"error":"不支持的采集模式: )" + jesc(mode) + R"EOF( (仅支持 record/monitor/null)"})EOF";
        }

        std::string dev = params.count("dev") ? params["dev"] : "";
        bool loopback = (params.count("loopback") && (params["loopback"] == "1" || params["loopback"] == "true"));
        double seconds = params.count("seconds") ? std::strtod(params["seconds"].c_str(), nullptr) : 0.0;
        if (seconds < 0.0 || std::isnan(seconds)) seconds = 0.0;
        if (seconds > 86400.0) seconds = 86400.0;
        std::string outFile = params.count("out") ? params["out"] : "";
        if (!outFile.empty() && !valid_utf8(outFile)) {
            return utf8_path_reject(outFile);
        }

        double injectFreq = 1000.0;
        double injectAmp = 0.001;
        if (params.count("inject")) {
            std::string inj = params["inject"];
            size_t colon = inj.find(':');
            if (colon != std::string::npos) {
                injectFreq = std::strtod(inj.substr(0, colon).c_str(), nullptr);
                injectAmp = std::strtod(inj.substr(colon + 1).c_str(), nullptr);
            } else {
                injectFreq = std::strtod(inj.c_str(), nullptr);
            }
        }

        // 若选择回环设备且未显式指定 loopback 标志，自动识别
        if (!loopback && !dev.empty()) {
            auto allDevs = rta::audio::WasapiCapture::enumerate_capture(true);
            for (const auto& d : allDevs) {
                if (d.id == dev || d.name == dev || d.name.find(dev) != std::string::npos) {
                    if (d.isLoopback) loopback = true;
                    break;
                }
            }
        }

        if (src != "null" && mode != "null" && !dev.empty() && dev[0] != '{') {
            auto allDevs = rta::audio::WasapiCapture::enumerate_capture(loopback);
            bool found = false;
            for (const auto& d : allDevs) {
                if (d.id == dev || d.name == dev || d.name.find(dev) != std::string::npos) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return R"({"ok":false,"error":"打开采集端点失败: 未找到指定的设备 [)" + jesc(dev) + R"(]"})";
            }
        }

        rta::audio::CaptureConfig cfg;
        cfg.deviceId = dev;
        cfg.loopback = loopback;
        cfg.bufferMs = 100.0;
        cfg.eventDriven = true;
        cfg.srcNull = (src == "null" || mode == "null");
        cfg.injectFreq = injectFreq;
        cfg.injectAmp = injectAmp;
        cfg.paced = true;
        if (cfg.srcNull && seconds > 0.0) {
            cfg.targetFrames = (size_t)(seconds * 48000.0);
        }

        auto cap = std::make_shared<rta::audio::WasapiCapture>();
        if (!cap->open(cfg)) {
            std::string err = cap->last_error();
            if (err.empty()) err = "打开采集端点失败: 无法打开目标采集设备或端点不存在";
            return R"({"ok":false,"error":")" + jesc(err) + R"("})";
        }

        std::shared_ptr<rta::audio::WasapiSharedOutput> sharedOut;
        if (mode == "monitor" && !cfg.srcNull) {
            // 直通监听（非 bit-perfect）：送往默认共享输出端点
            sharedOut = std::make_shared<rta::audio::WasapiSharedOutput>();
            if (!sharedOut->open("", cap->rate(), cap->channels(), 32)) {
                cap->close();
                return R"({"ok":false,"error":"直通监听打开输出端点失败: )" + jesc(sharedOut->last_error()) + R"("})";
            }
            if (!sharedOut->start()) {
                cap->close();
                return R"({"ok":false,"error":"直通监听启动输出失败: )" + jesc(sharedOut->last_error()) + R"("})";
            }
        }

        if (!cap->start()) {
            if (sharedOut) sharedOut->close();
            cap->close();
            std::string err = cap->last_error();
            if (err.empty()) err = "启动采集流失败";
            return R"({"ok":false,"error":"启动采集流失败: )" + jesc(err) + R"("})";
        }

        if (mode == "record" && outFile.empty()) {
            outFile = default_recording_path();
        }

        std::string devName = dev.empty() ? "(默认端点)" : dev;
        if (cfg.srcNull) {
            devName = "模拟源 (48k/2ch)";
        } else {
            for (const auto& d : rta::audio::WasapiCapture::enumerate_capture(loopback)) {
                if (d.id == dev || d.name.find(dev) != std::string::npos) {
                    devName = d.name;
                    break;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_inputSession.mtx);
            g_inputSession.mode = mode;
            g_inputSession.device = devName;
            g_inputSession.rate = cap->rate();
            g_inputSession.channels = cap->channels();
            g_inputSession.seconds = 0.0;
            g_inputSession.frames = 0;
            g_inputSession.water = 0.0;
            g_inputSession.overflow = 0;
            g_inputSession.underflow = 0;
            g_inputSession.posClips = 0;
            g_inputSession.negClips = 0;
            g_inputSession.outFile = (mode == "record") ? outFile : "";
            g_inputSession.rmsDb = -999.0;
            g_inputSession.peakDb = -999.0;
            g_inputSession.error.clear();
            g_inputSession.stopFlag.store(false);
            g_inputSession.running.store(true);
        }

        {
            std::lock_guard<std::mutex> lk(g_inputSession.joinMtx);
            g_inputSession.th = std::thread(input_worker_func, cap, sharedOut, mode, outFile, seconds);
        }
        return R"({"ok":true})";
    }

    // 停止采集：POST /api/input/stop
    if (rq.path.rfind("/api/input/stop", 0) == 0) {
        ct = "application/json; charset=utf-8";
        g_inputSession.stop();
        return R"({"ok":true})";
    }

    // ---------------- 导出诊断：一个压缩包（2026-10-22 功能3）----------------
    // ★ 顺序很重要：本块必须排在 /api/diag 之前。两个端点都用 rfind(prefix,0)==0
    //   前缀匹配，"diagzip" 也满足 "/api/diag" 的前缀 —— 放在后面就永远进不来
    //   （2026-10-22 实测：下载回来的是 JSON 不是 ZIP，Expand-Archive 报
    //   "找不到中央目录结尾记录"，第一 4 字节是 7B 22 6F 6B = {"ok）。
    // 用户原话：「其实就是个 log，最好直接导出一个压缩包或一个整体的文件，不要用户
    // 单独找 rlplayer-diag.json / rlplayer-playback.log / launcher.log 三份文件」。
    //
    // 打包方式：本进程内存里生成 STORE(不压缩) ZIP（src/net/zipstore.h）。
    //   ✗ 不调 PowerShell Compress-Archive —— 这是分发给用户的程序，用户机器上不保证有。
    //   ✗ 不引第三方库 —— 项目零压缩依赖，且要过分发兼容门禁。
    //   ✓ 自己写几十行：本地头 + 数据 + 中央目录 + EOCD，每文件 CRC32。
    //
    // ★ 红线：包内【绝不能有 api key】。两道保险：
    //   ① 逐条内容都过 redact_secrets()（复用 logTail 那段 sk- 掩码，另加
    //      "api_key=..." 整行掩码 —— 诊断 JSON 里没有 key，但 log 理论上可能有）；
    //   ② 只挑【已知文件名】，绝不整目录打包（避免把 rlplayer-llm.conf 之类带 key
    //      的配置文件顺手塞进去）。白名单在下面 kFiles 里写死。
    if (rq.path.rfind("/api/diagzip", 0) == 0) {
        ct = "application/zip";
        // 把 secret 抹掉：整行 api_key=/token=/password= 替换，再把 sk-xxx 抹成 sk-***
        auto redact_secrets = [](std::string s) {
            // (a) 整行键值：api_key=..., token=..., password=...
            for (const char* key : {"api_key=", "apikey=", "token=", "password=", "secret="}) {
                size_t from = 0;
                for (;;) {
                    const size_t hit = s.find(key, from);
                    if (hit == std::string::npos) break;
                    size_t e = s.find_first_of(" \t\r\n\"',;", hit + std::strlen(key));
                    if (e == std::string::npos) e = s.size();
                    s.replace(hit + std::strlen(key), e - (hit + std::strlen(key)), "***");
                    from = hit + std::strlen(key) + 3;
                }
            }
            // (b) 散落的 sk-xxxx（DeepSeek / OpenAI 形态）
            size_t scan = 0;
            for (;;) {
                const size_t p2 = s.find("sk-", scan);
                if (p2 == std::string::npos) break;
                const bool boundary = (p2 == 0) || !std::isalnum((unsigned char)s[p2 - 1]);
                size_t e2 = p2 + 3;
                while (e2 < s.size() && std::isalnum((unsigned char)s[e2])) ++e2;
                if (boundary && e2 > p2 + 3) { s.replace(p2 + 3, e2 - (p2 + 3), "***"); scan = p2 + 6; }
                else scan = p2 + 3;
            }
            return s;
        };
        auto read_all = [](const std::string& path, size_t maxBytes) {
            std::ifstream f(fs::u8path(path), std::ios::binary);
            if (!f) return std::string();
            f.seekg(0, std::ios::end);
            const std::streamoff len = f.tellg();
            if (len <= 0) return std::string();
            const std::streamoff take = (len > (std::streamoff)maxBytes) ? (std::streamoff)maxBytes : len;
            f.seekg(len - take);      // 只取尾部：诊断看的是最近发生了什么
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return s;
        };

        std::vector<rta::zip::Entry> items;
        // ① diag.json：直接把 /api/diag 同一份内容拿进来（现算，不递归 HTTP）
        //    这里不重跑端点探活（可能耗时 3 秒），只取"设置摘要 + 版本/OS/CPU/GPU"这类
        //    静态信息；探活结果本来就属于"现场状态"，用户要时再点一次 /api/diag 即可。
        {
            std::ostringstream dj;
            dj << "{\"ok\":true"
               << ",\"version\":\"" << RLPLAYER_VERSION << "\""
               << ",\"build\":\"" << jesc(g_buildTime) << "\""
               << ",\"pid\":" << (unsigned long)GetCurrentProcessId()
               << ",\"uptime\":" << (long long)std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - g_startTp).count()
               << ",\"os\":\"" << jesc(rta::diag::os_version_string()) << "\""
               << ",\"cpu\":\"" << jesc(rta::diag::cpu_brand()) << "\""
               << ",\"tester\":\"" << jesc(rta::diag::read_tester()) << "\""
               << ",\"gpu\":\"" << jesc(std::string(rta::dsp::GpuBridge::instance().available() ? "可用" : "不可用") +
                                        std::string("(") + rta::dsp::GpuBridge::instance().status() + ")") << "\""
               << ",\"port\":" << g_uiPort
               << ",\"lan\":" << (g_lanOn ? "true" : "false") << "}";
            rta::zip::Entry e;
            e.name = "rlplayer-diag.json";
            e.data = redact_secrets(utf8_sanitize(dj.str()));
            items.push_back(std::move(e));
        }
        // ② 播放日志：当前 + 轮转的 .1/.2（现场排查常常需要"上上次启动"）
        for (const char* nm : {"rlplayer-playback.log", "rlplayer-playback.log.1",
                               "rlplayer-playback.log.2"}) {
            std::string body = read_all(path_in_exe_dir(nm), 2u * 1024u * 1024u);
            if (body.empty()) continue;
            rta::zip::Entry e;
            e.name = nm;
            e.data = redact_secrets(utf8_sanitize(body));
            items.push_back(std::move(e));
        }
        // ③ 启动器日志
        {
            std::string body = read_all(path_in_exe_dir("launcher.log"), 2u * 1024u * 1024u);
            if (!body.empty()) {
                rta::zip::Entry e;
                e.name = "launcher.log";
                e.data = redact_secrets(utf8_sanitize(body));
                items.push_back(std::move(e));
            }
        }
        // ④ 崩溃转储目录：只带文件名清单（dump 本身几十 MB，塞进诊断包没有意义，
        //    但"有没有 dump、叫什么名字"对排查非常有用）
        {
            std::ostringstream ls;
            std::error_code ec;
            const std::string dir = path_in_exe_dir("dumps");
            int n = 0;
            if (fs::exists(fs::u8path(dir), ec) && fs::is_directory(fs::u8path(dir), ec)) {
                for (auto& de : fs::directory_iterator(fs::u8path(dir),
                                                       fs::directory_options::skip_permission_denied, ec)) {
                    ls << pathToUtf8(de.path().filename()) << "\t"
                       << (long long)fs::file_size(de.path(), ec) << " bytes\n";
                    if (++n > 200) break;
                }
            }
            ls << "\n" << (n == 0 ? "(dumps 目录为空或不存在：本机没有发生崩溃转储)\n"
                                  : ("共 " + std::to_string(n) + " 个文件；完整 dump 未打包（体积过大）\n"));
            rta::zip::Entry e;
            e.name = "dumps-manifest.txt";
            e.data = redact_secrets(ls.str());
            items.push_back(std::move(e));
        }
        // ⑤ 设置摘要（只放白名单键，绝不整份 conf —— 里面可能躺着别的东西）
        {
            std::ostringstream st2;
            st2 << "# RLPlayer 设置摘要（仅白名单键，不含 api key / 路径以外的敏感项）\n";
            std::ifstream f(fs::u8path(path_in_exe_dir("rlplayer-settings.conf")));
            std::string line;
            static const char* kAllow[] = {"rate", "filter", "filterNx", "shaper", "dsd", "modorder",
                                           "modfamily", "buffer", "isp", "twostage", "backend", "bits", "device",
                                           "theme", "taps", "atten", "transition", "phase", "directsdm",
                                           "dsdkind", "dsdconv", "dsd2stage", "channels", "metaon", "metallm"};
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t e2 = line.find('=');
                if (e2 == std::string::npos) continue;
                const std::string k = line.substr(0, e2);
                for (const char* a : kAllow) if (k == a) { st2 << line << "\n"; break; }
            }
            rta::zip::Entry e;
            e.name = "settings-summary.txt";
            e.data = redact_secrets(st2.str());
            items.push_back(std::move(e));
        }
        // ⑥ 说明文件：让用户/我们打开包就知道怎么用、以及包内容边界
        {
            std::ostringstream rd;
            rd << "RLPlayer 诊断包\n"
               << "=================================================\n"
               << "版本: " << RLPLAYER_VERSION << "\n"
               << "生成: " << g_buildTime << "（构建时间）\n"
               << "条目:\n"
               << "  rlplayer-diag.json      版本/OS/CPU/GPU/端口等静态信息\n"
               << "  rlplayer-playback.log   播放格式/峰值/欠载（当前，最多 2MB 尾部）\n"
               << "  rlplayer-playback.log.1 上一次轮转的播放日志（存在才带）\n"
               << "  rlplayer-playback.log.2 上上次轮转（存在才带）\n"
               << "  launcher.log            启动器日志（启动横幅/主题/子进程）\n"
               << "  dumps-manifest.txt      崩溃转储文件清单（dump 本体不打包）\n"
               << "  settings-summary.txt    设置摘要（仅白名单键）\n"
               << "\n安全: 本包含内容已做脱敏处理 —— api_key / token / password 整行值被替换为 ***，\n"
               << "      散落的 sk-xxxxx 也被掩码。配置文件本体（rlplayer-llm.conf）【不在包内】。\n";
            rta::zip::Entry e;
            e.name = "README.txt";
            e.data = rd.str();
            items.push_back(std::move(e));
        }
        return rta::zip::build(items);
    }

    // ---------------- 诊断（1.0.5 可观测性）----------------
    // 版本/构建/PID/运行时长/OS/CPU/GPU/内测标识 + 端点探活摘要 + 设置六键 + 日志尾 + 最近错误。
    // 端点探活（probe_formats 对 16/24/32 各跑一遍独占支持率）可能耗时：总预算 3 秒，
    // 超时未探的字段如实写"不可用"，绝不卡住 HTTP 线程。
    if (rq.path.rfind("/api/diag", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::ostringstream j;
        const bool forceProbe = rq.path.find("refresh=1") != std::string::npos;   // F3：强制重探
        const auto upSec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - g_startTp).count();
        j << "{\"ok\":true"
          << ",\"version\":\"" << RLPLAYER_VERSION << "\""
          << ",\"build\":\"" << jesc(g_buildTime) << "\""
          << ",\"pid\":" << (unsigned long)GetCurrentProcessId()
          << ",\"uptime\":" << (long long)upSec
          << ",\"os\":\"" << jesc(rta::diag::os_version_string()) << "\""
          << ",\"cpu\":\"" << jesc(rta::diag::cpu_brand()) << "\""
          << ",\"gpu\":\"" << jesc(std::string(rta::dsp::GpuBridge::instance().available() ? "可用" : "不可用") +
                                   std::string("(") + rta::dsp::GpuBridge::instance().status() + ")") << "\""
          << ",\"tester\":\"" << jesc(rta::diag::read_tester()) << "\"";
        // 端点：每端点给友好名/ID/活状态 + 16/24/32 各自的支持率数。
        // F3：probe 结果走 60s TTL 缓存（probeAge = 本次响应所用探活的最大年龄秒数，
        // 刚探的为 0）；refresh=1 强制全部重探。只有真要探时才受 3 秒共享预算约束。
        const auto probeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        long long probeAgeMax = -1;
        j << ",\"endpoints\":[";
        {
            const auto devs = WasapiOutput::enumerate();
            auto joinRates = [](const std::vector<uint32_t>& v) {
                std::string s;
                for (size_t k = 0; k < v.size(); ++k) { if (k) s += "/"; s += std::to_string(v[k]); }
                return s;
            };
            // 取一个端点的探活结果（缓存命中 → 用缓存；否则在预算内现探并回填缓存）。
            // 返回的 age 为该结果年龄秒数（刚探 = 0）；预算耗尽时 ok=false。
            auto probeOf = [&](const std::string& id, FormatProbe& pr, long long& age) {
                std::lock_guard<std::mutex> lk(g_diagProbeMx);
                auto it = g_diagProbeCache.find(id);
                if (it != g_diagProbeCache.end()) {
                    const long long a = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - it->second.tp).count();
                    if (!forceProbe && a < 60) { pr = it->second.pr; age = a; return true; }
                }
                if (std::chrono::steady_clock::now() >= probeDeadline) return false;
                pr = WasapiOutput::probe_formats(id);
                age = 0;
                g_diagProbeCache[id] = DiagProbeEntry{ pr, std::chrono::steady_clock::now() };
                return true;
            };
            bool firstE = true;
            for (const auto& d : devs) {
                if (!firstE) j << ",";
                firstE = false;
                j << "{\"name\":\"" << jesc(d.name) << "\",\"id\":\"" << jesc(d.id) << "\""
                  << ",\"default\":" << (d.isDefault ? "true" : "false");
                if (d.state != DEVICE_STATE_ACTIVE) {
                    j << ",\"alive\":false,\"r16\":0,\"r24\":0,\"r32\":0}";
                    continue;
                }
                FormatProbe pr;
                long long age = -1;
                if (!probeOf(d.id, pr, age)) {
                    j << ",\"alive\":true,\"probe\":\"不可用（探活预算超时）\"}";
                    continue;
                }
                if (age > probeAgeMax) probeAgeMax = age;
                j << ",\"alive\":" << (pr.endpointAlive ? "true" : "false")
                  << ",\"r16\":" << pr.rates16.size()
                  << ",\"r24\":" << pr.rates24.size()
                  << ",\"r32\":" << pr.rates32.size()
                  << ",\"rates16\":\"" << joinRates(pr.rates16) << "\""
                  << ",\"rates24\":\"" << joinRates(pr.rates24) << "\""
                  << ",\"rates32\":\"" << joinRates(pr.rates32) << "\"}";
            }
        }
        j << "],\"probeAge\":" << probeAgeMax;
        // 设置摘要：只取六个白名单键（不回显 ir/eq 等文件路径内容）
        {
            std::ifstream f(path_in_exe_dir("rlplayer-settings.conf"));
            std::string line, backend = "wasapi", rate, bits, filter, shaper, metaon = "1";
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t e = line.find('=');
                if (e == std::string::npos) continue;
                const std::string k = line.substr(0, e), v = line.substr(e + 1);
                if      (k == "backend") backend = v;
                else if (k == "rate")    rate    = v;
                else if (k == "bits")    bits    = v;
                else if (k == "filter")  filter  = v;
                else if (k == "shaper")  shaper  = v;
                else if (k == "metaon")  metaon  = v;
            }
            j << ",\"settings\":{\"backend\":\"" << jesc(backend)
              << "\",\"rate\":\"" << jesc(rate)
              << "\",\"bits\":\"" << jesc(bits)
              << "\",\"filter\":\"" << jesc(filter)
              << "\",\"shaper\":\"" << jesc(shaper)
              << "\",\"metaon\":\"" << jesc(metaon) << "\"}";
        }
        // 日志尾 ≤8KB。当前日志不写 key 等敏感串；仍加一层 sk-[A-Za-z0-9]+ 掩码作保险
        {
            std::string tail;
            {
                std::ifstream f(path_in_exe_dir("rlplayer-playback.log"), std::ios::binary);
                if (f) {
                    f.seekg(0, std::ios::end);
                    const std::streamoff len = f.tellg();
                    if (len > 0) {
                        f.seekg(len > 8192 ? len - 8192 : (std::streamoff)0);
                        tail.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                    }
                }
            }
            size_t scan = 0;
            for (;;) {
                const size_t p2 = tail.find("sk-", scan);
                if (p2 == std::string::npos) break;
                const bool boundary = (p2 == 0) || !std::isalnum((unsigned char)tail[p2 - 1]);
                size_t e2 = p2 + 3;
                while (e2 < tail.size() && std::isalnum((unsigned char)tail[e2])) ++e2;
                if (boundary && e2 > p2 + 3) { tail.replace(p2 + 3, e2 - (p2 + 3), "***"); scan = p2 + 6; }
                else scan = p2 + 3;
            }
            tail = utf8_sanitize(tail);   // F6：日志里的非 UTF-8 字节换成 U+FFFD，浏览器不再乱码
            j << ",\"logTail\":\"" << jesc(tail) << "\"";
        }
        {
            std::lock_guard<std::mutex> lk(ps.mtx);
            j << ",\"lastError\":\"" << jesc(ps.error) << "\"";
        }
        j << "}";
        return j.str();
    }

    // 应用标识：启动器用它区分"本程序的实例"和"别的程序占了这个端口"
    if (rq.path.rfind("/api/ping", 0) == 0) {
        ct = "application/json; charset=utf-8";
        return std::string("{\"ok\":true,\"app\":\"RLPlayer\",\"version\":\"") + RLPLAYER_VERSION + "\"}";
    }

    // GPU 后端实际情况：后端 DLL 与 HIP 运行库是否在本目录、当前能不能用
    // （设置页要用它把"AMD"标成可用或未构建，而不是让用户选了才发现没后端）
    if (rq.path.rfind("/api/gpu", 0) == 0) {
        ct = "application/json; charset=utf-8";
        auto dll_here = [](const char* n) {
            const DWORD a = GetFileAttributesA(path_in_exe_dir(n).c_str());
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        };
        const bool amd  = dll_here("rtagpu.dll");
        const bool hip  = dll_here("amdhip64_7.dll");
        const bool cuda = dll_here("rtagpu_cuda.dll");   // 本版本不提供 CUDA 后端，仅用于如实报告
        std::ostringstream j;
        j << "{\"ok\":true"
          << ",\"vendor\":\"" << jesc(rta::dsp::gpu_vendor()) << "\""
          << ",\"amd\":" << (amd ? "true" : "false")
          << ",\"cuda\":" << (cuda ? "true" : "false")
          << ",\"hip\":" << (hip ? "true" : "false")
          << ",\"available\":" << (rta::dsp::GpuBridge::instance().available() ? "true" : "false")
          << ",\"status\":\"" << jesc(rta::dsp::GpuBridge::instance().status()) << "\"}";
        return j.str();
    }

    if (rq.path.rfind("/api/net", 0) == 0) {
        ct = "application/json; charset=utf-8";
        char hn[256] = {0};
        gethostname(hn, sizeof(hn) - 1);
        std::string ip;
        hostent* he = gethostbyname(hn);
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            in_addr a; std::memcpy(&a, he->h_addr_list[0], 4);
            ip = inet_ntoa(a);
        }
        std::ostringstream j;
        j << "{\"ok\":true,\"port\":" << g_uiPort << ",\"ip\":\"" << jesc(ip) << "\""
          << ",\"lan\":" << (g_lanOn ? "true" : "false") << "}";
        return j.str();
    }

    if (rq.path.rfind("/api/signal", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::lock_guard<std::mutex> lk(ps.mtx);
        if (ps.signal.empty()) return "{\"ok\":false}";
        const long long dspCnt = ps.dspChunks.load();
        const long long dspTotUs = ps.dspTotalUs.load();
        const double avgDspBlockMs = dspCnt > 0 ? (double)dspTotUs / (dspCnt * 1000.0) : 0.0;
        const double totalDspMs = (double)dspTotUs / 1000.0;
        std::string s = ps.signal;
        if (!s.empty() && s.back() == '}') {
            s.pop_back();
            std::ostringstream dyn;
            dyn << ",\"dspBlockMs\":" << avgDspBlockMs
                << ",\"dspTotalMs\":" << totalDspMs
                << ",\"dspChunkUs\":" << ps.dspChunkUs.load()
                << ",\"scanMs\":" << Library::instance().lastScanMs()
                << "}";
            s += dyn.str();
        }
        return "{\"ok\":true,\"path\":" + s + "}";
    }

    if (rq.path.rfind("/api/rescan", 0) == 0) {
        Library::instance().startScan(true);
        return "{\"ok\":true}";
    }

    if (rq.path.rfind("/api/quit", 0) == 0 || rq.path.rfind("/api/shutdown", 0) == 0) {
        g_exitReq.store(true);
        return "{\"ok\":true}";
    }

    if (rq.path.rfind("/api/dbg", 0) == 0) {
        ct = "text/plain; charset=utf-8";
        std::lock_guard<std::mutex> lk(g_logMx);
        std::string s;
        for (auto& l : g_log) { s += l; s += "\n"; }
        return s;
    }

    if (rq.path.rfind("/api/recent", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::ifstream f(path_in_exe_dir("rlplayer-recent.txt"));
        std::ostringstream j;
        j << "{\"ok\":true,\"items\":[";
        std::string line;
        bool first = true;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> f4;
            size_t s = 0;
            for (size_t k = 0; k <= line.size(); ++k)
                if (k == line.size() || line[k] == '\t') { f4.push_back(line.substr(s, k - s)); s = k + 1; }
            if (f4.size() < 4) continue;
            if (!first) j << ",";
            first = false;
            j << "{\"path\":\"" << jesc(f4[0]) << "\",\"title\":\"" << jesc(f4[1])
              << "\",\"artist\":\"" << jesc(f4[2]) << "\",\"album\":\"" << jesc(f4[3]) << "\"}";
        }
        j << "]}";
        return j.str();
    }

    if (rq.path.rfind("/api/volume", 0) == 0) {
        ct = "application/json; charset=utf-8";
        const size_t q = rq.path.find("?db=");
        double db = 0.0;
        if (q != std::string::npos) db = atof(urlDecode(rq.path.substr(q + 4)).c_str());
        if (db > 0.0) db = 0.0;
        if (db < -120.0) db = -120.0;
        ps.volDb.store(db);
        return "{\"ok\":true,\"db\":" + std::to_string(db) + "}";
    }

    if (rq.path.rfind("/api/seek", 0) == 0) {
        ct = "application/json; charset=utf-8";
        double frac = 0.0;
        const size_t q = rq.path.find("?pos=");
        if (q != std::string::npos) frac = atof(rq.path.c_str() + q + 5);
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        ps.seekReq.store(frac);
        return "{\"ok\":true}";
    }

    if (rq.path.rfind("/api/pause", 0) == 0) {
        ct = "application/json; charset=utf-8";
        int on = 1;
        const size_t q = rq.path.find("?on=");
        if (q != std::string::npos) on = atoi(rq.path.c_str() + q + 4);
        ps.pauseReq.store(on != 0);
        return std::string("{\"ok\":true,\"paused\":") + ((on != 0) ? "true" : "false") + "}";
    }

    if (rq.path.rfind("/api/stop", 0) == 0) {
        ct = "application/json; charset=utf-8";
        ps.stop.store(true);
        ps.pauseReq.store(false);
        ps.seekReq.store(-1.0);
        ps.joinIfDone();
        ps.stop.store(false);
        ps.paused.store(false);
        ps.playing.store(false);
        {
            std::lock_guard<std::mutex> lk(g_lastEndMx);
            g_lastEnd.isPosZeroNoErr = false;
        }
        return "{\"ok\":true}";
    }

    if (rq.path.rfind("/api/status", 0) == 0) {
        ct = "application/json; charset=utf-8";
        std::lock_guard<std::mutex> lk(ps.mtx);
        std::ostringstream j;
        const size_t pl2 = ps.played.load(), tt = ps.total.load();
        const double pos = ps.outRate > 0 ? (double)pl2 / ps.outRate : 0.0;
        const double dur = ps.outRate > 0 ? (double)tt / ps.outRate : 0.0;
        j << "{\"ok\":true"
          << ",\"playing\":" << (ps.playing.load() ? "true" : "false")
          << ",\"current\":\"" << jesc(ps.current) << "\""
          << ",\"error\":\"" << jesc(ps.error) << "\""
          << ",\"info\":\"" << jesc(ps.info) << "\""
          << ",\"paused\":" << (ps.paused.load() ? "true" : "false")
          << ",\"pos\":" << pos
          << ",\"dur\":" << dur
          << ",\"outRate\":" << ps.outRate
          << ",\"outBits\":" << ps.outBits
          << ",\"gpu\":" << (ps.gpu ? "true" : "false")
          << ",\"volDb\":" << ps.volDb.load()
          << ",\"startDb\":" << ps.startDb.load()
          << ",\"pad\":" << ps.dbgPad.load() << ",\"skips\":" << ps.dbgSkips.load()
          << ",\"w0\":" << g_dbgW0.load() << ",\"wr\":" << g_dbgWr.load()
          << ",\"peak\":" << g_dbgPeak.load() << ",\"clip\":" << g_dbgClip.load()
          << ",\"wf\":" << g_dbgWf.load() << ",\"pulls\":" << g_dbgPulls.load()
          << ",\"per\":" << g_dbgPer.load()
          << ",\"decodeMs\":" << ps.decodeMs.load()
          << ",\"ffmpegMs\":" << ps.ffmpegMs.load()
          << ",\"wavReadMs\":" << ps.wavReadMs.load()
          << ",\"dspChunkUs\":" << ps.dspChunkUs.load()
          << ",\"dspBlockMs\":" << (ps.dspChunks.load() > 0 ? (double)ps.dspTotalUs.load() / (ps.dspChunks.load() * 1000.0) : 0.0)
          << ",\"dspTotalMs\":" << ((double)ps.dspTotalUs.load() / 1000.0)
          << ",\"scanMs\":" << Library::instance().lastScanMs()
          << "}";
        return j.str();
    }

    ct = "application/json; charset=utf-8";
    return "{\"ok\":false,\"error\":\"not found\"}";
}

// ---------------- 页面 ----------------
static std::string page() {
    std::string h = rta::net::rta_ui_page();
    std::string fo, so;
    // 滤波器下拉：文案里标出预设自带相位（用户 2026-10-22 功能2 要把"预设"讲明白）。
    // 设置页的 #sp_phasemsg 也从这份数据取真实值（/api/filters），不靠前端硬编码。
    for (const auto& fp : rta::dsp::filter_presets()) {
        fo += "<option value=\"" + std::string(fp.name) + "\">" + fp.name
            + (fp.twoStage ? "  [两段式]" : "")
            + (fp.phase == rta::dsp::PhaseKind::Minimum ? "  [最小相位]"
             : fp.phase == rta::dsp::PhaseKind::Intermediate ? "  [混合相位]"
             : "  [线性相位]")
            + "</option>";
    }
    for (const char* s : {"auto", "none", "NS1", "NS4", "NS5", "NS9", "LNS15", "RPDF", "TPDF", "Gauss1", "Shaped"}) {
        so += std::string("<option value=\"") + s + "\">" + s + "</option>";
    }
    // 占位符要【全部】替换：FILTER_OPTS 同时被 1x 和 Nx 两个下拉使用，
    // 之前只 replace 第一处，Nx 那个下拉于是留着字面量、一个选项都没有，
    // 也就是"Nx 滤波器"根本选不了（本轮修）。
    auto fill_all = [&h](const std::string& key, const std::string& val) {
        if (val.empty()) return;
        for (size_t p = h.find(key); p != std::string::npos; p = h.find(key, p + val.size()))
            h.replace(p, key.size(), val);
    };
    fill_all("FILTER_OPTS", fo);
    fill_all("SHAPER_OPTS", so);
    return h;
}

int main(int argc, char** argv) {
    rta::crash::install();    // 崩溃转储（1.0.5）：dump 落 exe 目录 dumps\，filter 先写日志
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    uint16_t port = 8899;
    bool lan = false;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--port" && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (k == "--lan") lan = true;
    }
    g_uiPort = port;
    g_lanOn  = lan;
    // 系统级调优：提高进程优先级 + 把系统定时器精度提到 1ms。
    // 默认 15.6ms 的定时器粒度会让喂数节奏变粗（Round 6 实测过这个问题）。
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);

    queue_load();                             // 恢复上次的播放列表
    Library::instance().load();               // 载入音乐库索引
    // 可选后备代理(P1.5):rlplayer-llm.conf 里 proxy=http://127.0.0.1:7897(可省)。
    // 非空则本进程全部 http_get/http_post 走它(HTTPS 由 WinHTTP 走 CONNECT 隧道);
    // 没有该行 = 直连,现状行为不变(本机 MusicBrainz 已可直连,代理只是后备)。
    {
        const std::string proxy = llm_conf_proxy_line();
        if (!proxy.empty()) {
            rta::net::set_http_proxy(proxy);
            printf("HTTP 代理: %s(来自 rlplayer-llm.conf 的 proxy= 行)\n", proxy.c_str());
        }
    }
    // MetaBase 存放 exe 同目录 rlplayer-meta/(与播放日志/设置同位置约定);
    // 播放触发补全与 /api/enrich、/api/meta、/api/cover、/api/lyrics 都走这个单例
    rta::meta::MetaStore::instance().setBaseDir(path_in_exe_dir("rlplayer-meta"));
    rta::meta::EntityStore::instance().setBaseDir(path_in_exe_dir("rlplayer-meta"));
    rta::dsp::OpraService::instance().init(path_in_exe_dir("data\\opra_index.json"), path_in_exe_dir(""));
    {
        std::ifstream f(path_in_exe_dir("rlplayer-settings.conf"));
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t e = line.find('=');
            if (e == std::string::npos) continue;
            const std::string k = line.substr(0, e);
            const std::string v = line.substr(e + 1);
            if (k == "scanThreads" || k == "scanthreads") {
                const int st = atoi(v.c_str());
                if (st > 0) Library::instance().setScanThreads((size_t)st);
            }
            if (k == "streamdecode") {
                rta::audio::set_stream_decode_enabled(v != "0" && v != "false");
            }
        }
    }
    if (Library::instance().trackCount() == 0) {
        printf("音乐库为空，开始首次扫描（根目录见 rlplayer-library.conf）…\n");
        Library::instance().startScan();
    }
    rta::net::WebUi ui;
    if (!ui.start(port, handle, !lan)) {
        printf("!! 端口 %u 启动失败（被占用？）\n", (unsigned)port);
        return 1;
    }
    // 启动横幅（1.0.5 可观测性）：版本/构建时间/PID/端口/OS/CPU/GPU/端点数/内测标识。
    // 现场排查第一眼就有的信息：日志里的 v 字样只能说明版本，说明不了"跑在谁的哪台机器上"。
    {
        std::ostringstream b;
        b << "STARTUP v" << RLPLAYER_VERSION
          << " build=" << g_buildTime
          << " pid=" << (unsigned long)GetCurrentProcessId()
          << " port=" << (unsigned)ui.port()
          << " os=" << rta::diag::os_version_string()
          << " cpu=" << rta::diag::cpu_brand()
          << " gpu=" << (rta::dsp::GpuBridge::instance().available() ? "available" : "unavailable")
          << "(" << rta::dsp::GpuBridge::instance().status() << ")"
          << " endpoints=" << WasapiOutput::enumerate().size();
        const std::string tester = rta::diag::read_tester();
        if (!tester.empty()) b << " tester=" << tester;
        play_log(b.str());
        printf("%s\n", b.str().c_str());
    }
    printf("RLPlayer 网页播放器\n");
    printf("  打开浏览器访问: http://127.0.0.1:%u/\n", (unsigned)ui.port());
    printf("  （只监听本机；播放会以独占模式接管音频设备）\n");
    printf("  按 Ctrl+C 退出\n");
    while (!g_exitReq.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_ps.stop.store(true);
    g_ps.joinIfDone();
    ui.stop();
    return 0;
}
