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

namespace rta::net {

namespace fs = std::filesystem;

inline std::string cast_lower_str(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
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
    std::string rateMatch = "unknown"; // match, mismatch, unknown
    std::string warning;
    std::string error;
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
        state_.warning.clear();
        state_.error.clear();

        workerThread_ = std::thread([this, p]() {
            run_worker(p);
        });

        return {true, ""};
    }

    bool stop() {
        stopReq_.store(true);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        std::lock_guard<std::mutex> lk(mtx_);
        state_.running = false;
        state_.phase = "stopped";
        state_.transportState = "STOPPED";
        return true;
    }

private:
    void run_worker(const CastStartParams& p) {
        std::string tempWavPath;
        bool needCleanupTemp = false;
        std::string wavToServe = p.file;
        AudioSourceFormat srcFormat;

        auto fail = [this, &tempWavPath, &needCleanupTemp](const std::string& err) {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = "error";
            state_.error = err;
            if (needCleanupTemp && !tempWavPath.empty()) {
                std::remove(tempWavPath.c_str());
            }
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
            } catch (const std::exception& e) {
                fail(std::string("DSD 读取/打包失败: ") + e.what());
                return;
            }
        } else {
            if (ext == ".wav" && p.rate == 0 && p.bits == 0) {
                try {
                    rta::audio::AudioBuffer ab = rta::audio::read_wav(p.file);
                    srcFormat.sampleRate = ab.rate;
                    srcFormat.bits = ab.bits;
                    srcFormat.channels = ab.channels;
                    srcFormat.isDsd = false;
                    srcFormat.encoding = ab.isFloat ? "IEEE Float PCM" : "Linear PCM";
                    wavToServe = p.file;
                } catch (const std::exception& e) {
                    fail(std::string("读取源 WAV 失败: ") + e.what());
                    return;
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
                    } else if (p.bits > 0 && p.bits != ab.bits) {
                        ab.bits = p.bits;
                    }

                    tempWavPath = make_cast_temp_wav_path("pcm");
                    rta::audio::write_wav(tempWavPath, ab);
                    wavToServe = tempWavPath;
                    needCleanupTemp = true;

                    srcFormat.sampleRate = ab.rate;
                    srcFormat.bits = ab.bits;
                    srcFormat.channels = ab.channels;
                    srcFormat.isDsd = false;
                    srcFormat.encoding = "Linear PCM";
                } catch (const std::exception& e) {
                    fail(std::string("解码/写出临时 WAV 失败: ") + e.what());
                    return;
                }
            }
        }

        if (stopReq_.load()) {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = "stopped";
            if (needCleanupTemp && !tempWavPath.empty()) std::remove(tempWavPath.c_str());
            return;
        }

        // 3) 启动本地 HTTP 供源服务
        uint16_t servePort = p.port ? p.port : 8912;
        UpnpHttpServer server;
        if (!server.start(servePort, "0.0.0.0")) {
            fail("无法在 0.0.0.0:" + std::to_string(servePort) + " 启动 HTTP 供源服务（端口可能被占用）");
            return;
        }

        std::string localIp = upnp_local_ip_for(targetDev.host, (uint16_t)targetDev.port);
        std::string streamPath = server.register_file(wavToServe, "audio/wav");
        std::string streamUrl = "http://" + localIp + ":" + std::to_string(server.port()) + streamPath;

        // 4) UPnP 控制动作 SetAVTransportURI & Play
        UpnpControlClient client(targetDev.av_transport_url(), targetDev.connection_manager_url());

        UpnpDidlItem didl;
        didl.title = p.file;
        size_t lastSlash = didl.title.find_last_of("\\/");
        if (lastSlash != std::string::npos) didl.title = didl.title.substr(lastSlash + 1);
        didl.creator = "RLPlayer Cast";
        didl.uri = streamUrl;
        didl.sampleRate = srcFormat.sampleRate;
        didl.bits = srcFormat.bits;
        didl.channels = srcFormat.channels;
        std::string didlXml = build_didl_lite(didl);

        UpnpActionResult setRes = client.set_av_transport_uri(streamUrl, didlXml);
        if (!setRes.ok) {
            server.stop();
            fail("SetAVTransportURI 失败: " + setRes.error.str());
            return;
        }

        UpnpActionResult playRes = client.play("1");
        if (!playRes.ok) {
            client.stop();
            server.stop();
            fail("Play 失败: " + playRes.error.str());
            return;
        }

        // 5) 轮询与状态更新
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.phase = "playing";
            state_.transportState = "PLAYING";
        }

        UpnpProtocolInfo proto = client.get_protocol_info();
        auto startTime = std::chrono::steady_clock::now();

        while (!stopReq_.load()) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            if (p.seconds > 0.0 && elapsed >= p.seconds) {
                break;
            }

            UpnpTransportInfo transport = client.get_transport_info();
            UpnpPositionInfo pos = client.get_position_info();
            UpnpMediaInfo media = client.get_media_info();

            SamplingRateAnalysis analysis = analyze_sampling_rate(srcFormat, media, pos, proto);

            {
                std::lock_guard<std::mutex> lk(mtx_);
                state_.transportState = !transport.state.empty() ? transport.state : "PLAYING";
                state_.positionSec = parse_time_hh_mm_ss(pos.relTime);
                state_.servedBytes = server.total_bytes_served();
                state_.requestCount = (int)server.total_requests_served();

                if (analysis.verdict == SamplingRateVerdict::Match) {
                    state_.rateMatch = "match";
                } else if (analysis.verdict == SamplingRateVerdict::Mismatch) {
                    state_.rateMatch = "mismatch";
                } else {
                    state_.rateMatch = "unknown";
                }
                state_.warning = analysis.warning;
            }

            if (transport.ok && (transport.state == "STOPPED" || transport.state == "NO_MEDIA_PRESENT") && elapsed > 2.0) {
                break;
            }

            for (int i = 0; i < 5; ++i) {
                if (stopReq_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // 6) 结束收尾
        client.stop();
        server.stop();
        if (needCleanupTemp && !tempWavPath.empty()) {
            std::remove(tempWavPath.c_str());
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = "stopped";
            state_.transportState = "STOPPED";
        }
    }

    std::mutex              mtx_;
    CastState               state_;
    std::atomic<bool>       stopReq_{false};
    std::thread             workerThread_;

    std::mutex              devMutex_;
    std::vector<UpnpDevice> cachedDevices_;
    std::chrono::steady_clock::time_point lastScanTime_;
};

} // namespace rta::net
