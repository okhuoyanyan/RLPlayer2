// cast_service.h — UPnP 媒体投放后台会话编排与状态模型 (ROADMAP P5b)
#pragma once
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include "net/upnp.h"
#include "net/upnp_control.h"
#include "audio/wav.h"
#include "audio/audio_loader.h"
#include "audio/dsd_reader.h"
#include "dsp/dop.h"
#include "dsp/resampler.h"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <functional>
#include <cstdint>

namespace rta::net {

namespace fs = std::filesystem;

inline std::string cast_lower_str(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ---------------- 供源格式表（2026-10-29）----------------
// 先说清一件容易误判的事：投放链路**永远**把曲目解码成 WAV 再供源，所以
//   · HTTP 的 Content-Type = audio/wav
//   · DIDL 的 res@protocolInfo = http-get:*:audio/wav:*
// 是"与线上字节一致"的，不是把 FLAC/APE 谎称成 WAV。表里这些 MIME 只用来说明"源是什么"，
// 以及将来要做"原样直通（不转码）"时该声明什么 —— 现在不参与线上声明。
inline std::string cast_source_mime(const std::string& ext) {
    if (ext == ".flac") return "audio/flac";
    if (ext == ".wav" || ext == ".wave") return "audio/wav";
    if (ext == ".dsf") return "audio/dsf";
    if (ext == ".dff") return "audio/dff";
    if (ext == ".ape") return "audio/ape";
    if (ext == ".alac" || ext == ".m4a") return "audio/mp4";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".aif" || ext == ".aiff") return "audio/aiff";
    if (ext == ".wv") return "audio/x-wavpack";
    if (ext == ".ogg") return "audio/ogg";
    if (ext == ".opus") return "audio/opus";
    if (ext == ".wma") return "audio/x-ms-wma";
    if (ext == ".tta") return "audio/tta";
    if (ext == ".tak") return "audio/tak";
    if (ext == ".iso") return "application/x-sacd-iso";
    return "application/octet-stream";
}
// 人话：给 UI/日志用
inline std::string cast_source_kind(const std::string& ext) {
    if (ext == ".flac") return "FLAC 无损";
    if (ext == ".wav" || ext == ".wave") return "WAV";
    if (ext == ".dsf" || ext == ".dff") return "DSD";
    if (ext == ".ape") return "APE 无损";
    if (ext == ".alac" || ext == ".m4a") return "ALAC/AAC";
    if (ext == ".mp3") return "MP3 有损";
    if (ext == ".aif" || ext == ".aiff") return "AIFF";
    if (ext == ".wv") return "WavPack";
    if (ext == ".ogg" || ext == ".opus") return "Ogg/Opus";
    if (ext == ".wma") return "WMA";
    return ext.empty() ? "未知" : ext;
}

// 只读前 1MB 判断"这个 WAV 能不能原样交给渲染器"（不做整曲解码，几毫秒）。
// 判据 = 渲染器实际能吃的那一档：整型 PCM、16/24-bit、≤2 声道、≤192k。
// 返回 false 时 why 里写清是哪一条不合格（会进日志与 UI 的 warning）。
inline bool cast_wav_can_serve_raw(const std::string& path, rta::audio::WavHeaderInfo& hi, std::string& why) {
    std::FILE* f = upnp_detail::open_file_utf8(path, L"rb", "rb");
    if (!f) { why = "文件打不开"; return false; }
    std::vector<unsigned char> buf(1u << 20);
    const size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n < 12) { why = "文件过短"; return false; }
    std::string err;
    const int rc = rta::audio::parse_wav_header(buf.data(), n, hi, &err);
    if (rc <= 0) {
        why = err.empty() ? std::string("WAV 头在 1MB 内没解析出来（有超大附属块？）") : err;
        return false;
    }
    if (hi.fmtTag != 1) { why = "不是整型 PCM（fmtTag=" + std::to_string((int)hi.fmtTag) + "）"; return false; }
    if (hi.bits != 16 && hi.bits != 24) {
        why = std::to_string((int)hi.bits) + "-bit（渲染器普遍只认 16/24-bit）";
        return false;
    }
    if (hi.channels < 1 || hi.channels > 2) {
        why = std::to_string((int)hi.channels) + " 声道（多数 DLNA 渲染器只支持 ≤2）";
        return false;
    }
    if (hi.rate > 192000u) { why = std::to_string(hi.rate) + "Hz（超过 192k）"; return false; }
    return true;
}

inline std::string make_cast_temp_wav_path(const char* tag) {
    std::error_code ec;
    auto td = fs::temp_directory_path(ec);
    static std::atomic<uint64_t> s_seq{0};
    auto name = "rtacast_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + "_" + std::to_string(++s_seq) + "_" + (tag ? tag : "") + ".wav";
    return (td / name).string();
}

struct CastState {
    bool        running = false;
    std::string device;
    std::string sourceFile;
    uint16_t    servePort = 8912;
    std::string phase = "stopped"; // discovering, serving, playing, stopped, error
    double      positionSec = 0.0;
    std::string transportState = "STOPPED";
    int64_t     servedBytes = 0;
    int         requestCount = 0;
    // 261024-01 #9：UPnP 作为输出后端时，播放器（底栏进度条 / 时长 / 格式 / 暂停）
    // 的状态统一取这次投放会话 —— 一条会话、一个状态源，避免两套控制互相打架。
    double      durationSec = 0.0;
    uint32_t    sampleRate = 0;
    int         bits = 0;
    bool        paused = false;
    std::string rateMatch = "unknown"; // match, mismatch, unknown（唯一依据 = GetProtocolInfo 的 Sink）
    std::string warning;
    std::string error;
    // ---- 2026-10-29：bit-perfect 真实探测（取代 rtaweb 里硬编码的 "no"）----
    // bitPerfect: yes / no / unknown —— 只有"软件侧逐位 + 渲染器 Sink 显式支持该格式 + 已取流在播"
    // 三条同时成立才给 yes；能证明不成立给 no；其余 unknown（前端应显示"无法判定"而不是"非 bit-perfect"）。
    std::string bitPerfect = "unknown";
    std::string bitPerfectWhy;
    bool        softwareBitPerfect = false;  // 软件侧（解码→供源）是否逐位，不含渲染器
    std::string serveFormat;                 // 实际供源格式，如 "WAV 44100Hz/16bit/2ch（源 .flac）"
    std::string sinkSummary;                 // 渲染器 Sink 能力摘要
    std::string servedUrl;                   // 渲染器要取的 URL（排障用）
};

struct CastStartParams {
    std::string dev;
    std::string file;
    uint16_t    port = 8912;
    uint32_t    rate = 0;
    int         bits = 0;
    double      seconds = 0.0;
    bool        dop = false;
};

struct CastStartResult {
    bool ok = false;
    std::string error;
};

class CastManager {
public:
    static CastManager& instance() {
        static CastManager s_inst;
        return s_inst;
    }

    CastManager() = default;
    ~CastManager() {
        stop();
    }

    CastManager(const CastManager&) = delete;
    CastManager& operator=(const CastManager&) = delete;

    std::vector<UpnpDevice> get_devices(bool forceRescan = false) {
        std::lock_guard<std::mutex> lk(devMutex_);
        auto now = std::chrono::steady_clock::now();
        double ageSec = std::chrono::duration<double>(now - lastScanTime_).count();
        if (forceRescan || cachedDevices_.empty() || ageSec > 15.0) {
            cachedDevices_ = upnp_discover(1200);
            lastScanTime_ = std::chrono::steady_clock::now();
        }
        return cachedDevices_;
    }

    CastState get_state() {
        std::lock_guard<std::mutex> lk(mtx_);
        return state_;
    }

    // 2026-10-29：投放链路原先一行日志都没有 —— 用户报"绝大多数失败"时，
    // rlplayer-playback.log 里查不到任何 CAST 记录，只能靠猜。
    // 主控在 rtaweb 启动处接一行即可把 CAST 全流程写进同一份播放日志：
    //   rta::net::CastManager::instance().set_logger([](const std::string& s) { play_log(s); });
    // 没接也不崩（退回 stderr）。
    void set_logger(std::function<void(const std::string&)> fn) {
        std::lock_guard<std::mutex> lk(logMx_);
        logger_ = std::move(fn);
    }
    void log(const std::string& s) {
        std::function<void(const std::string&)> fn;
        {
            std::lock_guard<std::mutex> lk(logMx_);
            fn = logger_;
        }
        if (fn) fn("CAST " + s);
        else std::fprintf(stderr, "[CAST] %s\n", s.c_str());
    }

    CastStartResult start(const CastStartParams& p) {
        if (p.file.empty()) {
            return {false, "未指定音频文件路径"};
        }

        std::string ext = cast_lower_str(rta::audio::lowerExt(p.file));
        bool isDsd = (ext == ".dsf" || ext == ".dff");
        if (isDsd && !p.dop) {
            return {false, "源文件为 DSD 格式 (" + p.file + ")。未开启 DoP 模式，为保证音质拒绝转码为 PCM 降质输出（请勾选 DoP）"};
        }

        std::error_code ec;
        if (!fs::exists(fs::u8path(p.file), ec) || ec) {
            return {false, "音频文件不存在: " + p.file};
        }

        std::lock_guard<std::mutex> lk(mtx_);
        if (state_.running) {
            return {false, "已有投放会话正在进行中，请先停止"};
        }

        if (workerThread_.joinable()) {
            workerThread_.join();
        }

        stopReq_.store(false);
        pauseReq_.store(false);
        seekReq_.store(-1.0);
        state_.running = true;
        state_.device = p.dev;
        state_.sourceFile = p.file;
        state_.servePort = p.port ? p.port : 8912;
        state_.phase = "discovering";
        state_.positionSec = 0.0;
        state_.transportState = "STOPPED";
        state_.servedBytes = 0;
        state_.requestCount = 0;
        state_.rateMatch = "unknown";
        state_.durationSec = 0.0;
        state_.sampleRate = 0;
        state_.bits = 0;
        state_.paused = false;
        state_.warning.clear();
        state_.error.clear();
        state_.bitPerfect = "unknown";
        state_.bitPerfectWhy.clear();
        state_.softwareBitPerfect = false;
        state_.serveFormat.clear();
        state_.sinkSummary.clear();
        state_.servedUrl.clear();

        workerThread_ = std::thread([this, p]() {
            run_worker(p);
        });

        return {true, ""};
    }

    // 播放器控制映射（后端 = UPnP 时 /api/pause、/api/seek 转交给投放会话）：
    // HTTP 线程只写原子量，真正的 SOAP 动作在 worker 的轮询循环里做（不在请求线程里阻塞）。
    void set_paused(bool on) { pauseReq_.store(on); }
    void seek_seconds(double sec) { seekReq_.store(sec < 0.0 ? -1.0 : sec); }

    bool stop() {
        stopReq_.store(true);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        std::lock_guard<std::mutex> lk(mtx_);
        state_.running = false;
        state_.phase = "stopped";
        state_.transportState = "STOPPED";
        state_.paused = false;
        return true;
    }

private:
    void run_worker(const CastStartParams& p) {
        std::string tempWavPath;
        bool needCleanupTemp = false;
        std::string wavToServe = p.file;
        AudioSourceFormat srcFormat;
        double durationSec = 0.0;   // 供播放器进度条用（源文件时长，秒）
        int64_t serveBytes = 0;     // 供源文件字节数（res@size 与"是否播完"的判据）
        // 软件侧判定：解码结果是否**原封不动**交给渲染器（无重采样/无位深转换/无音量/无 EQ/无抖动）
        bool softwareBitExact = true;
        std::string softwareWhy = "逐位直通（解码后按源采样率/位深/声道写 WAV，未做重采样/音量/EQ/抖动）";
        std::string serveFormat;
        std::string preWarn;        // 供源前的已知风险（会写进 state_.warning，让用户先看到）

        auto fail = [this, &tempWavPath, &needCleanupTemp](const std::string& err) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                state_.running = false;
                state_.phase = "error";
                state_.error = err;
                // 会话失败：bit-perfect 结论必须退回 unknown（"设备没开机"不等于"链路非 bit-perfect"），
                // 但把失败原因写进 why，前端 tooltip 能直接看到。
                if (state_.bitPerfect != "no") {
                    state_.bitPerfect = "unknown";
                    state_.bitPerfectWhy = err;
                }
                if (needCleanupTemp && !tempWavPath.empty()) {
                    std::remove(tempWavPath.c_str());
                }
            }
            log("FAIL " + err);
        };

        // 1) 发现渲染器
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.phase = "discovering";
        }

        UpnpDevice targetDev;
        bool foundDev = false;

        if (!p.dev.empty() && (p.dev.rfind("http://", 0) == 0 || p.dev.rfind("https://", 0) == 0)) {
            targetDev.location = p.dev;
            const size_t sp = p.dev.find("//");
            if (sp != std::string::npos) {
                const size_t slash = p.dev.find('/', sp + 2);
                std::string hostport = p.dev.substr(sp + 2, (slash == std::string::npos ? p.dev.size() : slash) - (sp + 2));
                const std::string path = (slash == std::string::npos) ? "/" : p.dev.substr(slash);
                const size_t colon = hostport.find(':');
                targetDev.host = (colon == std::string::npos) ? hostport : hostport.substr(0, colon);
                targetDev.port = (colon == std::string::npos) ? 80 : std::atoi(hostport.substr(colon + 1).c_str());

                std::string xml;
                if (upnp_detail::http_get(targetDev.host, targetDev.port, path, xml)) {
                    targetDev.name = upnp_detail::xml_tag(xml, "friendlyName");
                    targetDev.model = upnp_detail::xml_tag(xml, "modelName");
                    targetDev.udn = upnp_detail::xml_tag(xml, "UDN");
                    targetDev.control = upnp_detail::extract_service_control_url(xml, "AVTransport");
                    if (targetDev.control.empty()) targetDev.control = upnp_detail::xml_tag(xml, "controlURL");
                    targetDev.controlConnection = upnp_detail::extract_service_control_url(xml, "ConnectionManager");
                    foundDev = true;
                }
            }
            if (!foundDev) {
                fail("无法从指定设备描述 URL 获取描述: " + p.dev);
                return;
            }
        } else {
            auto devs = upnp_discover(2000);
            if (devs.empty()) {
                fail("局域网内未发现任何 UPnP 渲染器（请检查设备是否开机与网络配置）");
                return;
            }
            if (!p.dev.empty()) {
                std::string query = cast_lower_str(p.dev);
                for (const auto& d : devs) {
                    if (cast_lower_str(d.name).find(query) != std::string::npos ||
                        cast_lower_str(d.model).find(query) != std::string::npos ||
                        d.host.find(query) != std::string::npos) {
                        targetDev = d;
                        foundDev = true;
                        break;
                    }
                }
                if (!foundDev) {
                    fail("未找到名称/型号包含 "" + p.dev + "" 的 UPnP 渲染器");
                    return;
                }
            } else {
                targetDev = devs[0];
                foundDev = true;
            }
        }

        if (stopReq_.load()) {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = "stopped";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.device = targetDev.name.empty() ? targetDev.host : targetDev.name;
            state_.phase = "serving";
        }

        // 2) 准备供源音频
        std::string ext = cast_lower_str(rta::audio::lowerExt(p.file));
        bool isDsd = (ext == ".dsf" || ext == ".dff");
        if (isDsd) {
            try {
                rta::audio::DsdStream dsd = rta::audio::read_dsd(p.file);
                if (dsd.rate == 0 || dsd.channels == 0 || dsd.data.empty()) {
                    fail("DSD 文件读取失败或格式无效: " + p.file);
                    return;
                }
                const uint32_t carrierRate = dsd.rate / 16u;
                const size_t carrierFrames = (size_t)(dsd.bits / 16u);

                rta::audio::AudioBuffer dopBuf;
                dopBuf.rate = carrierRate;
                dopBuf.channels = dsd.channels;
                dopBuf.bits = 24;
                dopBuf.isFloat = false;
                dopBuf.raw.resize(carrierFrames * (size_t)dsd.channels);

                for (int c = 0; c < dsd.channels; ++c) {
                    bool phaseA = true;
                    for (size_t n = 0; n < carrierFrames; ++n) {
                        uint8_t b0 = dsd.data[(size_t)c][n * 2 + 0];
                        uint8_t b1 = dsd.data[(size_t)c][n * 2 + 1];
                        uint32_t bits16 = ((uint32_t)b0 << 8) | (uint32_t)b1;
                        uint32_t marker = (phaseA ? rta::dsp::DOP_MARKER_A : rta::dsp::DOP_MARKER_B) << 16;
                        int32_t v = (int32_t)(marker | bits16);
                        if (v & 0x800000) v |= (int32_t)0xFF000000;
                        dopBuf.raw[n * (size_t)dsd.channels + (size_t)c] = v;
                        phaseA = !phaseA;
                    }
                }

                tempWavPath = make_cast_temp_wav_path("dop");
                rta::audio::write_wav(tempWavPath, dopBuf);
                wavToServe = tempWavPath;
                needCleanupTemp = true;

                srcFormat.sampleRate = carrierRate;
                srcFormat.bits = 24;
                srcFormat.channels = dsd.channels;
                srcFormat.isDsd = true;
                srcFormat.encoding = "DSD over PCM (DoP v1.1)";
                durationSec = (double)dsd.bits / (double)dsd.rate;
                serveFormat = "WAV 24bit/" + std::to_string(carrierRate) + "Hz/" +
                              std::to_string(dsd.channels) + "ch（源 DSD 以 DoP v1.1 打包）";
                softwareWhy = "DoP v1.1 打包：载波逐位无损（未做 PCM 转换）";
            } catch (const std::exception& e) {
                fail(std::string("DSD 读取/打包失败: ") + e.what());
                return;
            }
        } else {
            if ((ext == ".wav" || ext == ".wave") && p.rate == 0 && p.bits == 0) {
                // 2026-10-29：.wav 分两类处理。
                //   A) 渲染器普遍能吃的整型 PCM（16/24-bit、≤2 声道、≤192k）⟹ 源文件**原样**供源：
                //      字节级一致、零转码、不把整首曲子读进内存（1 小时 24/192 的 WAV 有 4GB，
                //      旧实现一律整读，会顶到 read_wav 的 4GiB 防御上限直接失败）。
                //   B) 其余（float32 / 32-bit int / 多声道 / >192k / 头里有超大附属块）⟹ 走
                //      read_wav → write_wav 规范化成标准 RIFF/WAVE（WAVE_FORMAT_EXTENSIBLE 与
                //      超大 LIST/id3 附属块会让一部分渲染器直接拒收）。PCM 样本逐位不变，只重写头。
                //      注意：float32 源规范化后仍是 float32 —— 不偷偷量化成 24-bit，只在 warning 里说清楚。
                rta::audio::WavHeaderInfo hi;
                std::string whyNot;
                if (cast_wav_can_serve_raw(p.file, hi, whyNot)) {
                    srcFormat.sampleRate = hi.rate;
                    srcFormat.bits = hi.bits;
                    srcFormat.channels = hi.channels;
                    srcFormat.isDsd = false;
                    srcFormat.encoding = "Linear PCM";
                    durationSec = (hi.byteRate > 0 && hi.dataDeclaredBytes > 0 && hi.dataDeclaredBytes != 0xFFFFFFFFu)
                                  ? (double)hi.dataDeclaredBytes / (double)hi.byteRate : 0.0;
                    wavToServe = p.file;
                    softwareWhy = "源 WAV 原样供源（未解码、未改写，字节级一致）";
                } else {
                    bool normalized = false;
                    try {
                        rta::audio::AudioBuffer ab = rta::audio::read_wav(p.file);
                        srcFormat.sampleRate = ab.rate;
                        srcFormat.bits = ab.bits;
                        srcFormat.channels = ab.channels;
                        srcFormat.isDsd = false;
                        srcFormat.encoding = ab.isFloat ? "IEEE Float PCM" : "Linear PCM";
                        durationSec = ab.rate ? (double)ab.frames() / (double)ab.rate : 0.0;
                        tempWavPath = make_cast_temp_wav_path("wav");
                        rta::audio::write_wav(tempWavPath, ab);
                        wavToServe = tempWavPath;
                        needCleanupTemp = true;
                        normalized = true;
                        softwareWhy = "源 WAV 已规范化（" + whyNot + "）：PCM 样本逐位未变，只重写 RIFF 头";
                        if (ab.isFloat) softwareWhy += "；float32 源仍是 32-bit float（不悄悄量化成 24-bit）";
                        if (ab.isFloat || (!ab.isFloat && ab.bits != 16 && ab.bits != 24) || ab.channels > 2) {
                            preWarn = "这个 WAV 的格式不是常见的 16/24-bit PCM 双声道（" + whyNot +
                                      "）：部分渲染器会拒收，若无声请改用 16/24-bit 的源。";
                        }
                    } catch (const std::exception& e) {
                        // 规范化失败（>4GiB 的整型 WAV / 特殊容器）：退回原样供源 ——
                        // 供源至少还有机会成功，同时把"渲染器可能不认这个头"讲清楚。
                        wavToServe = p.file;
                        softwareWhy = std::string("源 WAV 规范化失败（") + e.what() + "），已原样供源";
                        preWarn = std::string("这个 WAV 无法规范化（") + e.what() +
                                  "）：渲染器若无声，多半是不认它的 RIFF 头。";
                    }
                    if (!normalized && srcFormat.sampleRate == 0) {
                        // 连读都读不了：明确失败，不要拿一个空格式去投
                        fail("源 WAV 无法读取: " + p.file + "（" + whyNot + "）");
                        return;
                    }
                }
            } else {
                try {
                    rta::audio::LoadNote note;
                    rta::audio::AudioBuffer ab = rta::audio::read_any(p.file, &note);

                    if (p.rate > 0 && p.rate != ab.rate) {
                        rta::dsp::ResamplerConfig rcfg;
                        rcfg.inRate = ab.rate;
                        rcfg.outRate = p.rate;
                        rcfg.channels = ab.channels;
                        rta::dsp::PolyphaseResampler resampler(rcfg);
                        const size_t inFrames = ab.frames();
                        const size_t maxOut = resampler.maxOutFrames(inFrames);
                        std::vector<std::vector<double>> inCh(ab.channels, std::vector<double>(inFrames));
                        std::vector<const double*> inPtrs(ab.channels);
                        for (int c = 0; c < ab.channels; ++c) {
                            for (size_t n = 0; n < inFrames; ++n) inCh[c][n] = ab.sample(n, c);
                            inPtrs[c] = inCh[c].data();
                        }
                        std::vector<std::vector<double>> outCh(ab.channels, std::vector<double>(maxOut));
                        std::vector<double*> outPtrs(ab.channels);
                        for (int c = 0; c < ab.channels; ++c) outPtrs[c] = outCh[c].data();

                        const size_t outFrames = resampler.process(inPtrs.data(), inFrames, outPtrs.data());
                        rta::audio::AudioBuffer resBuf;
                        resBuf.rate = p.rate;
                        resBuf.channels = ab.channels;
                        resBuf.bits = (p.bits > 0) ? p.bits : ab.bits;
                        resBuf.isFloat = false;
                        resBuf.raw.resize(outFrames * (size_t)ab.channels);
                        for (size_t n = 0; n < outFrames; ++n) {
                            for (int c = 0; c < ab.channels; ++c) {
                                resBuf.setSample(n, c, outCh[c][n]);
                            }
                        }
                        ab = std::move(resBuf);
                        softwareBitExact = false;
                        softwareWhy = "已按请求重采样 " + std::to_string(rcfg.inRate) + "Hz → " +
                                      std::to_string(rcfg.outRate) + "Hz（重采样即不可逆，逐位直通不成立）";
                    } else if (p.bits > 0 && p.bits != ab.bits) {
                        // ★ 2026-10-29 真 BUG：旧代码只改 ab.bits 字段、不换算样本值 ⟹
                        //   16→24bit 之后每个样本被当成 24bit 解读，电平直接掉 48dB（用户听到的是"没有声音"）。
                        //   这里按满幅重新量化（会引入量化误差，所以 bit-perfect 记 no 并写明原因）。
                        const int oldBits = ab.bits;
                        rta::audio::AudioBuffer conv;
                        conv.rate = ab.rate;
                        conv.channels = ab.channels;
                        conv.bits = p.bits;
                        conv.isFloat = false;
                        conv.raw.assign(ab.raw.size(), 0);
                        const size_t convFrames = ab.frames();
                        for (size_t n = 0; n < convFrames; ++n)
                            for (int c = 0; c < ab.channels; ++c) conv.setSample(n, c, ab.sample(n, c));
                        ab = std::move(conv);
                        softwareBitExact = false;
                        softwareWhy = "已按请求把位深 " + std::to_string(oldBits) + "bit → " +
                                      std::to_string(p.bits) + "bit（重新量化）";
                    }

                    tempWavPath = make_cast_temp_wav_path("pcm");
                    rta::audio::write_wav(tempWavPath, ab);
                    wavToServe = tempWavPath;
                    needCleanupTemp = true;

                    srcFormat.sampleRate = ab.rate;
                    srcFormat.bits = ab.bits;
                    srcFormat.channels = ab.channels;
                    srcFormat.isDsd = false;
                    srcFormat.encoding = ab.isFloat ? "IEEE Float PCM" : "Linear PCM";
                    durationSec = ab.rate ? (double)ab.frames() / (double)ab.rate : 0.0;
                    serveFormat = std::string("WAV ") + std::to_string(ab.bits) + "bit/" +
                                  std::to_string(ab.rate) + "Hz/" + std::to_string(ab.channels) +
                                  "ch（源 " + ext + " = " + cast_source_kind(ext) +
                                  "，MIME " + cast_source_mime(ext) + "，解码后写入 WAV 供源）";
                    if (ab.isFloat) {
                        preWarn = "源解码成 32-bit float PCM：不少 DLNA 渲染器只接受 16/24-bit 整型 WAV，"
                                  "若无声请改用 16/24-bit 的源（本机不做偷偷量化）。";
                    } else if (ab.channels > 2) {
                        preWarn = "源是 " + std::to_string(ab.channels) +
                                  " 声道：多数 DLNA 渲染器只支持 2 声道，可能直接不播。";
                    }
                } catch (const std::exception& e) {
                    fail(std::string("解码/写出临时 WAV 失败: ") + e.what());
                    return;
                }
            }
        }

        if (serveFormat.empty()) {
            // 走到这里说明上面三个分支都没有填 serveFormat（.wav 直通分支）
            serveFormat = "WAV " + std::to_string(srcFormat.bits) + "bit/" +
                          std::to_string(srcFormat.sampleRate) + "Hz/" +
                          std::to_string(srcFormat.channels) + "ch（源 .wav 直通，audio/wav）";
        }

        // 供源文件字节数：res@size 用它；"是否播完"也用它
        {
            std::error_code ec2;
            const auto sz = fs::file_size(fs::u8path(wavToServe), ec2);
            if (!ec2) serveBytes = (int64_t)sz;
        }

        {
            // 供源格式与时长：播放器底栏（/api/status）在 UPnP 后端下直接取这份数据
            std::lock_guard<std::mutex> lk(mtx_);
            state_.sampleRate = srcFormat.sampleRate;
            state_.bits = srcFormat.bits;
            state_.durationSec = durationSec > 0.0 ? durationSec : 0.0;
            state_.serveFormat = serveFormat;
            state_.softwareBitPerfect = softwareBitExact;
            if (!preWarn.empty()) state_.warning = preWarn;
            // 软件侧已经确定不是逐位直通 ⟹ 这里就能给 no（不必等渲染器）
            if (!softwareBitExact) {
                state_.bitPerfect = "no";
                state_.bitPerfectWhy = "软件侧已改动：" + softwareWhy;
            } else {
                state_.bitPerfect = "unknown";
                state_.bitPerfectWhy = "软件侧：" + softwareWhy + "；渲染器侧：等待 GetProtocolInfo 与取流证据";
            }
        }
        log("PREP " + serveFormat + " | bytes=" + std::to_string(serveBytes) +
            " | " + softwareWhy);

        if (stopReq_.load()) {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = "stopped";
            state_.paused = false;
            if (needCleanupTemp && !tempWavPath.empty()) std::remove(tempWavPath.c_str());
            return;
        }

        // 3) 启动本地 HTTP 供源服务
        // 2026-10-24（261024-02 #5 实测）：8912 可能被别的程序占用（本机 Clash/verge-mihomo 就长期监听
        // 0.0.0.0:8912）⟹ 投放直接失败。改为在 8912..8921 里挑第一个能绑上的端口，
        // 实际端口写回 state_.servePort（UI 与取流 URL 都以 state 为准，不假设 8912）。
        const uint16_t wantPort = p.port ? p.port : 8912;
        UpnpHttpServer server;
        uint16_t servePort = wantPort;
        bool bound = false;
        for (uint16_t i = 0; i < 10; ++i) {
            servePort = (uint16_t)(wantPort + i);
            if (server.start(servePort, "0.0.0.0")) { bound = true; break; }
            if (stopReq_.load()) break;
        }
        if (!bound) {
            fail("无法在 0.0.0.0:" + std::to_string(wantPort) + "–" + std::to_string((int)wantPort + 9) +
                 " 启动 HTTP 供源服务（端口全被占用）");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.servePort = servePort;
        }

        // ★ 2026-10-29：供源前先自检"这个文件我们真的打得开吗"。
        // 旧实现要等渲染器发来 GET 才发现打不开（返回 404），用户看到的是"播两秒自动停"。
        {
            const std::string rdErr = UpnpHttpServer::check_file_readable(wavToServe);
            if (!rdErr.empty()) {
                server.stop();
                fail("供源文件不可读：" + rdErr +
                     "（WAV 直通路径曾因窄 fopen 打开 UTF-8 中文路径失败 —— 已改为宽字符打开，若仍报此错请看权限/占用）");
                return;
            }
        }

        std::string localIp = upnp_local_ip_for(targetDev.host, (uint16_t)targetDev.port);
        std::string streamPath = server.register_file(wavToServe, "audio/wav");
        std::string streamUrl = "http://" + localIp + ":" + std::to_string(server.port()) + streamPath;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.servedUrl = streamUrl;
        }
        log("SERVE url=" + streamUrl + " file=" + wavToServe + " port=" + std::to_string(server.port()));

        // 4) UPnP 控制动作 SetAVTransportURI & Play
        UpnpControlClient client(targetDev.av_transport_url(), targetDev.connection_manager_url());
        log("DEVICE " + (targetDev.name.empty() ? targetDev.host : targetDev.name) +
            " model=" + targetDev.model + " av=" + client.av_transport_url() +
            " cm=" + client.connection_manager_url());

        // 4a) 先问渲染器"你到底收什么"（ConnectionManager.GetProtocolInfo）。
        //     放在 SetAVTransportURI 之前：① 能在投之前就告诉用户"它不支持这个格式"；
        //     ② bit-perfect 判定只认这份 Sink，不再拿我们自己发过去的 DIDL 回显当证据。
        UpnpProtocolInfo proto = client.get_protocol_info();
        UpnpSinkCaps caps = parse_sink_caps(proto.ok ? proto.sink : std::string());
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.sinkSummary = caps.ok ? caps.summary() : std::string("(GetProtocolInfo 无响应)");
        }
        log(std::string("SINK ") + (proto.ok ? "ok" : "FAIL") + " " +
            (caps.ok ? caps.summary() : std::string("(空)")) +
            (proto.ok ? "" : (" err=" + proto.error.str())));

        if (caps.ok) {
            const auto ans = caps.match_stream(srcFormat.sampleRate, srcFormat.bits, srcFormat.channels);
            if (ans == UpnpSinkCaps::Answer::No) {
                const std::string w = "渲染器 Sink 未声明支持 " + serveFormat + "（Sink: " + caps.summary() +
                                      "）⟹ 它大概率会拒收或自行转换；若无声请换设备或降低源格式。";
                preWarn = preWarn.empty() ? w : (preWarn + " " + w);
                std::lock_guard<std::mutex> lk(mtx_);
                state_.bitPerfect = "no";
                state_.bitPerfectWhy = w;
                log("PREFLIGHT_MISMATCH " + w);
            }
        }

        UpnpDidlItem didl;
        didl.title = p.file;
        size_t lastSlash = didl.title.find_last_of("\\/");
        if (lastSlash != std::string::npos) didl.title = didl.title.substr(lastSlash + 1);
        didl.creator = "RLPlayer Cast";
        didl.uri = streamUrl;
        // 线上字节 = WAV ⟹ protocolInfo 必须与之一致（不是把 FLAC 谎称成 WAV，而是我们**已经**解码成 WAV）。
        didl.protocolInfo = "http-get:*:audio/wav:*";
        didl.sampleRate = srcFormat.sampleRate;
        didl.bits = srcFormat.bits;
        didl.channels = srcFormat.channels;
        didl.durationSec = durationSec;      // 旧实现从没填过 ⟹ 部分渲染器无法拖动/无法预算缓冲
        didl.sizeBytes = serveBytes;
        std::string didlXml = build_didl_lite(didl);
        log("DIDL " + didlXml.substr(0, 400));

        UpnpActionResult setRes = client.set_av_transport_uri(streamUrl, didlXml);
        if (!setRes.ok) {
            server.stop();
            // ★ 绝不吞 SOAP Fault：UPnPError 的 code 与 description 原样进 state_.error（前端可读）
            fail("SetAVTransportURI 失败: " + setRes.error.str() +
                 "（HTTP " + std::to_string(setRes.httpStatus) + "；URI=" + streamUrl + "）");
            return;
        }

        UpnpActionResult playRes = client.play("1");
        if (!playRes.ok) {
            client.stop();
            server.stop();
            fail("Play 失败: " + playRes.error.str() + "（HTTP " + std::to_string(playRes.httpStatus) + "）");
            return;
        }
        log("PLAY ok");

        // 5) 轮询与状态更新
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.phase = "playing";
            state_.transportState = "PLAYING";
        }

        auto startTime = std::chrono::steady_clock::now();
        bool pausedApplied = false;
        std::string lastUpnpErr;    // 最近一次 SOAP 失败的 UPnPError 原文（收尾时拼进错误里）
        std::string stopReason;     // 退出轮询的原因；空 = 正常结束
        int transportFailStreak = 0;

        while (!stopReq_.load()) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            if (p.seconds > 0.0 && elapsed >= p.seconds) {
                break;
            }

            // 261024-01 #9：暂停 / 跳转请求（来自播放器底栏）。HTTP 线程只写原子量，
            // SOAP 动作在这里做 —— 请求线程不会因为渲染器慢响应而卡住整个 Web UI。
            const bool wantPause = pauseReq_.load();
            if (wantPause != pausedApplied) {
                const UpnpActionResult pr = wantPause ? client.pause() : client.play("1");
                if (pr.ok) {
                    pausedApplied = wantPause;
                    std::lock_guard<std::mutex> lk(mtx_);
                    state_.paused = wantPause;
                }
            }
            {
                const double seekSec = seekReq_.exchange(-1.0);
                if (seekSec >= 0.0) {
                    char hms[32]{};
                    const int total = (int)(seekSec + 0.5);
                    std::snprintf(hms, sizeof(hms), "%02d:%02d:%02d",
                                  total / 3600, (total % 3600) / 60, total % 60);
                    const UpnpActionResult sr = client.seek_rel_time(hms);
                    if (sr.ok) {
                        std::lock_guard<std::mutex> lk(mtx_);
                        state_.positionSec = seekSec;
                    }
                }
            }

            UpnpTransportInfo transport = client.get_transport_info();
            UpnpPositionInfo pos = client.get_position_info();
            UpnpMediaInfo media = client.get_media_info();
            (void)media;   // 回显的 DIDL 是我们自己发过去的，拿它当"渲染器能力"是自证（见 upnp_control.h 注释）

            if (!transport.ok && transport.error.has_error()) lastUpnpErr = transport.error.str();
            if (!pos.ok && pos.error.has_error()) lastUpnpErr = pos.error.str();
            transportFailStreak = transport.ok ? 0 : (transportFailStreak + 1);

            const bool fetched = server.total_requests_served() > 0;
            const bool playingNow = (transport.state == "PLAYING" || transport.state == "PAUSED_PLAYBACK");

            // ★ 2026-10-29：bit-perfect 真实探测（只认 Sink + 软件侧逐位 + 取流/在播证据）。
            //   旧实现是 rtaweb 里硬编码 ps.bpState="no"，与链路无关 —— 用户看到的"非 bit-perfect"是假的。
            BitPerfectInput bpIn;
            bpIn.src = srcFormat;
            bpIn.caps = caps;
            bpIn.softwareBitExact = softwareBitExact;
            bpIn.softwareWhy = softwareWhy;
            bpIn.dop = (srcFormat.isDsd && isDsd);
            bpIn.fetched = fetched;
            bpIn.playing = playingNow;
            const BitPerfectVerdict bp = judge_upnp_bit_perfect(bpIn);

            const int64_t served = server.total_bytes_served();
            const int reqs = (int)server.total_requests_served();
            const bool complete = (serveBytes > 0 && served + 65536 >= serveBytes);

            {
                std::lock_guard<std::mutex> lk(mtx_);
                // 旧实现在 SOAP 失败时仍显示 "PLAYING"（撒谎）。现在只在真有答案时更新。
                if (transport.ok && !transport.state.empty()) state_.transportState = transport.state;
                state_.positionSec = parse_time_hh_mm_ss(pos.relTime);
                state_.servedBytes = served;
                state_.requestCount = reqs;
                state_.rateMatch = (bp.state == BitPerfectState::Yes) ? "match"
                                  : (bp.state == BitPerfectState::No) ? "mismatch" : "unknown";
                state_.bitPerfect = bp.str();
                state_.bitPerfectWhy = bp.why;
                // 渲染器自己"暂停"了（用户没按暂停）—— 这正是现场说的"播放会自动暂停"。
                // 旧实现 paused 只看用户请求，渲染器进 PAUSED_PLAYBACK 时界面还显示在播。
                if (transport.ok && transport.state == "PAUSED_PLAYBACK") {
                    state_.paused = true;
                } else if (transport.ok && transport.state == "PLAYING") {
                    state_.paused = false;
                }
                // warning 不再被每一轮覆盖（旧实现会把"防火墙/格式"提示冲掉）
                std::string warn = preWarn;
                if (transport.ok && transport.state == "PAUSED_PLAYBACK" && !pauseReq_.load()) {
                    warn += (warn.empty() ? "" : " ");
                    warn += "渲染器自行进入 PAUSED_PLAYBACK（不是本机发起的暂停）：常见于取流中断、"
                            "缓冲不足或被设备判定为不支持的格式。";
                }
                if (!caps.ok && elapsed > 3.0) {
                    warn += (warn.empty() ? "" : " ");
                    warn += "渲染器没有返回 GetProtocolInfo 能力（部分老设备如此），无法判断它是否原样接收。";
                }
                if (elapsed > 8.0 && reqs == 0) {
                    warn += (warn.empty() ? "" : " ");
                    warn += "渲染器没有来取流：请检查 Windows 防火墙是否允许 rtaweb.exe 入站 TCP " +
                            std::to_string(server.port()) + "，或设备是否支持该格式";
                }
                if (server.not_found_count() > 0) {
                    warn += (warn.empty() ? "" : " ");
                    warn += "渲染器取流收到了 " + std::to_string(server.not_found_count()) +
                            " 次 404：" + server.last_error();
                }
                state_.warning = warn;
            }

            // 结束判定：
            //   · 供源字节基本发完 ⟹ 正常播完，不算失败；
            //   · 设备回 STOPPED/NO_MEDIA_PRESENT 但流没发完 ⟹ **必须**给出可读原因，
            //     绝不静默"自动暂停"（旧实现就是直接 break，界面只剩一个播放键变回"播放"）。
            if (transport.ok && (transport.state == "STOPPED" || transport.state == "NO_MEDIA_PRESENT")) {
                const double grace = fetched ? 2.0 : 8.0;   // 没取过流的多给几秒（老设备起播慢）
                if (elapsed > grace) {
                    if (!complete) {
                        std::string why = "渲染器在 " + std::to_string((int)elapsed) + " 秒后报告 " +
                                          transport.state + "，但只取走 " + std::to_string(served) + "/" +
                                          std::to_string(serveBytes) + " 字节";
                        if (reqs == 0) {
                            why += "（一次都没来取流：检查 Windows 防火墙是否放行入站 TCP " +
                                   std::to_string(server.port()) + "，或设备不支持该格式）";
                        } else if (server.not_found_count() > 0) {
                            why += "（取流被 404 拒绝：" + server.last_error() + "）";
                        } else {
                            why += "（流发到一半被设备中断：常见原因是格式/采样率/声道数不被支持、"
                                   "DoP 不被识别、或设备侧解码出错）";
                        }
                        if (!lastUpnpErr.empty()) why += "；最近 UPnP 错误: " + lastUpnpErr;
                        stopReason = why;
                        log("STOPPED_EARLY " + why);
                    }
                    break;
                }
            }
            // 与渲染器彻底失联（连续 20 次 = 约 10 秒拿不到任何状态）：不要假装还在播
            if (transportFailStreak >= 20) {
                stopReason = "与渲染器失联（连续 20 次 GetTransportInfo 失败）" +
                             (lastUpnpErr.empty() ? std::string() : "；最近 UPnP 错误: " + lastUpnpErr);
                log("LOST " + stopReason);
                break;
            }

            for (int i = 0; i < 5; ++i) {
                if (stopReq_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // 6) 结束收尾
        log(std::string("TEARDOWN ") + (stopReason.empty() ? "ok" : ("error=" + stopReason)) +
            " served=" + std::to_string(server.total_bytes_served()) +
            " reqs=" + std::to_string(server.total_requests_served()) +
            " 404s=" + std::to_string(server.not_found_count()));
        client.stop();
        server.stop();
        if (needCleanupTemp && !tempWavPath.empty()) {
            std::remove(tempWavPath.c_str());
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = stopReason.empty() ? "stopped" : "error";
            state_.transportState = "STOPPED";
            state_.paused = false;
            if (!stopReason.empty()) {
                state_.error = stopReason;
                // 会话失败时不能留着 "yes"（那会变成"bit-perfect 但它没响"）
                if (state_.bitPerfect == "yes") state_.bitPerfect = "unknown";
                state_.bitPerfectWhy = stopReason;
            }
        }
    }

    std::mutex              mtx_;
    CastState               state_;
    std::atomic<bool>       stopReq_{false};
    std::atomic<bool>       pauseReq_{false};
    std::atomic<double>     seekReq_{-1.0};
    std::thread             workerThread_;

    std::mutex              logMx_;
    std::function<void(const std::string&)> logger_;

    std::mutex              devMutex_;
    std::vector<UpnpDevice> cachedDevices_;
    std::chrono::steady_clock::time_point lastScanTime_;
};

} // namespace rta::net
