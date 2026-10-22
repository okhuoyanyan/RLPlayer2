// rtacast.cpp — UPnP/DLNA 投放命令行工具
//
// 用法：
//   rtacast --list
//   rtacast --file <音频文件> [--dev <设备名子串>] [--port 8912] [--rate <hz>]
//                             [--bits <位深>] [--seconds <秒数>] [--analyze] [--dop]
//
// 契约要点（ROADMAP P5a）：
//   1) 发现渲染器、选择目标设备并建立 AVTransport 会话；
//   2) 原始 PCM/DoP WAV 不转码本地 HTTP 供源（Range 支撑 seek，token 防遍历）；
//   3) 渲染器实际采样率取证（Match / Mismatch / Unknown 三态）；
//   4) --analyze 打印供源格式、渲染器声明、重采样判定与 position 推进状态；
//   5) 如实标注「未做端到端重采样实测」（铁律 4）。
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "net/upnp.h"
#include "net/upnp_control.h"
#include "audio/wav.h"
#include "audio/audio_loader.h"
#include "audio/dsd_reader.h"
#include "dsp/dop.h"
#include "dsp/resampler.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>

using namespace rta;
using namespace rta::net;
using namespace rta::audio;

static std::atomic<bool> g_stop{false};

static BOOL WINAPI console_ctrl_handler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

static void print_usage() {
    std::printf("RLPlayer UPnP/DLNA 媒体投放工具 (rtacast)\n\n");
    std::printf("用法:\n");
    std::printf("  rtacast --list                               发现局域网内的 UPnP 渲染器\n");
    std::printf("  rtacast --file <音频> [选项]                 投放指定音频文件到渲染器\n\n");
    std::printf("选项:\n");
    std::printf("  --dev <子串>      匹配渲染器名称、型号或 IP 地址（默认自动选择首个设备）\n");
    std::printf("  --port <端口>     本地 HTTP 供源监听端口（默认 8912）\n");
    std::printf("  --rate <Hz>       目标采样率（默认保持源文件采样率）\n");
    std::printf("  --bits <位深>     目标位深 16/24/32（默认保持源文件位深）\n");
    std::printf("  --seconds <N>     播放 N 秒后停止（0 = 播完全曲，默认 0）\n");
    std::printf("  --dop             DSD 素材打包为 DoP v1.1 WAV 供源（未指定时遇 DSD 明确拒绝）\n");
    std::printf("  --analyze         分析模式：打印供源格式、渲染器声明、重采样判定与播放推进\n");
}

static std::string lower_str(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string make_temp_wav_path(const char* tag) {
    char tmpDir[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tmpDir);
    char path[MAX_PATH] = {0};
    std::snprintf(path, sizeof(path), "%srtacast_%lu_%s.wav",
                  tmpDir, (unsigned long)GetCurrentProcessId(), tag);
    return std::string(path);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::fprintf(stderr, "WSAStartup 失败\n");
        return 1;
    }

    bool optList = false;
    bool optAnalyze = false;
    bool optDop = false;
    std::string optFile;
    std::string optDev;
    uint16_t optPort = 8912;
    uint32_t optRate = 0;
    int optBits = 0;
    double optSeconds = 0.0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_val = [&](const char* def) -> std::string {
            if (i + 1 < argc) return argv[++i];
            return def;
        };
        if (arg == "--list" || arg == "-l") {
            optList = true;
        } else if (arg == "--file" || arg == "-f") {
            optFile = next_val("");
        } else if (arg == "--dev" || arg == "-d") {
            optDev = next_val("");
        } else if (arg == "--port" || arg == "-p") {
            optPort = (uint16_t)std::atoi(next_val("8912").c_str());
        } else if (arg == "--rate" || arg == "-r") {
            optRate = (uint32_t)std::strtoul(next_val("0").c_str(), nullptr, 10);
        } else if (arg == "--bits" || arg == "-b") {
            optBits = std::atoi(next_val("0").c_str());
        } else if (arg == "--seconds" || arg == "-s") {
            optSeconds = std::atof(next_val("0").c_str());
        } else if (arg == "--analyze" || arg == "-a") {
            optAnalyze = true;
        } else if (arg == "--dop") {
            optDop = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] != '-' && optFile.empty()) {
            optFile = arg;
        }
    }

    if (!optList && optFile.empty()) {
        print_usage();
        return 1;
    }

    // ---------------- 1) 设备发现与列表 ----------------
    if (optList) {
        std::printf("UPnP/DLNA 媒体渲染器发现中（SSDP M-SEARCH，超时 2000 ms）...\n");
        auto devs = upnp_discover(2000);
        if (devs.empty()) {
            std::printf("未发现 UPnP/DLNA 渲染器（可能原因：接收设备未开机、不在同一子网、或组播被防火墙拦截）。\n");
            return 0;
        }
        std::printf("共发现 %zu 个 UPnP/DLNA 渲染器：\n", devs.size());
        for (size_t i = 0; i < devs.size(); ++i) {
            std::printf("  [%zu] %s\n", i, devs[i].name.c_str());
            std::printf("       型号: %s\n", devs[i].model.empty() ? "(未声明)" : devs[i].model.c_str());
            std::printf("       地址: http://%s:%d\n", devs[i].host.c_str(), devs[i].port);
            std::printf("       AVTransport: %s\n", devs[i].av_transport_url().c_str());
            if (!devs[i].controlConnection.empty()) {
                std::printf("       ConnectionManager: %s\n", devs[i].connection_manager_url().c_str());
            }
        }
        WSACleanup();
        return 0;
    }

    // ---------------- 2) 准备供源音频 ----------------
    AudioSourceFormat srcFormat;
    std::string tempWavPath;
    std::string wavToServe = optFile;
    bool needCleanupTemp = false;

    std::string ext = lower_str(lowerExt(optFile));
    bool isDsd = (ext == ".dsf" || ext == ".dff");

    if (isDsd) {
        if (!optDop) {
            std::fprintf(stderr,
                         "错误：源文件为 DSD 格式 (%s)。未开启 DoP 模式 (--dop)，"
                         "为保证音质拒绝偷偷转码为 PCM 降质输出（铁律 3）。\n"
                         "如需通过 DoP 投放，请添加 --dop 参数。\n",
                         optFile.c_str());
            return 1;
        }
        // 原生 DSD 读取并打包为 DoP 24-bit WAV
        try {
            std::printf("读取原生 DSD 流: %s ...\n", optFile.c_str());
            DsdStream dsd = read_dsd(optFile);
            if (dsd.rate == 0 || dsd.channels == 0 || dsd.data.empty()) {
                std::fprintf(stderr, "错误：DSD 文件读取失败或格式无效\n");
                return 1;
            }
            const uint32_t carrierRate = dsd.rate / 16u;
            const size_t carrierFrames = (size_t)(dsd.bits / 16u);

            AudioBuffer dopBuf;
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

            tempWavPath = make_temp_wav_path("dop");
            write_wav(tempWavPath, dopBuf);
            wavToServe = tempWavPath;
            needCleanupTemp = true;

            srcFormat.sampleRate = carrierRate;
            srcFormat.bits = 24;
            srcFormat.channels = dsd.channels;
            srcFormat.isDsd = true;
            srcFormat.encoding = "DSD over PCM (DoP v1.1)";
            std::printf("DSD -> DoP 载波 WAV 就绪: %u Hz / 24 bit / %d ch (时长 %.2f 秒)\n",
                        carrierRate, dsd.channels, dsd.seconds());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "DSD 读取/打包失败: %s\n", e.what());
            return 1;
        }
    } else {
        // PCM 格式（WAV / FLAC / ALAC ...）
        if (ext == ".wav" && optRate == 0 && optBits == 0) {
            // 原生已有 WAV 且不重采样：直接供源，保持逐位无损
            try {
                AudioBuffer ab = read_wav(optFile);
                srcFormat.sampleRate = ab.rate;
                srcFormat.bits = ab.bits;
                srcFormat.channels = ab.channels;
                srcFormat.isDsd = false;
                srcFormat.encoding = ab.isFloat ? "IEEE Float PCM" : "Linear PCM";
                wavToServe = optFile;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "读取源 WAV 失败: %s\n", e.what());
                return 1;
            }
        } else {
            // 解码为中间 PCM 并按目标要求输出临时 WAV
            try {
                std::printf("解码源音频: %s ...\n", optFile.c_str());
                LoadNote note;
                AudioBuffer ab = read_any(optFile, &note);

                // 若用户指定重采样
                if (optRate > 0 && optRate != ab.rate) {
                    std::printf("按请求重采样: %u Hz -> %u Hz\n", ab.rate, optRate);
                    rta::dsp::ResamplerConfig rcfg;
                    rcfg.inRate = ab.rate;
                    rcfg.outRate = optRate;
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
                    AudioBuffer resBuf;
                    resBuf.rate = optRate;
                    resBuf.channels = ab.channels;
                    resBuf.bits = (optBits > 0) ? optBits : ab.bits;
                    resBuf.isFloat = false;
                    resBuf.raw.resize(outFrames * (size_t)ab.channels);
                    for (size_t n = 0; n < outFrames; ++n) {
                        for (int c = 0; c < ab.channels; ++c) {
                            resBuf.setSample(n, c, outCh[c][n]);
                        }
                    }
                    ab = std::move(resBuf);
                } else if (optBits > 0 && optBits != ab.bits) {
                    ab.bits = optBits;
                }

                tempWavPath = make_temp_wav_path("pcm");
                write_wav(tempWavPath, ab);
                wavToServe = tempWavPath;
                needCleanupTemp = true;

                srcFormat.sampleRate = ab.rate;
                srcFormat.bits = ab.bits;
                srcFormat.channels = ab.channels;
                srcFormat.isDsd = false;
                srcFormat.encoding = "Linear PCM";
            } catch (const std::exception& e) {
                std::fprintf(stderr, "解码/写出临时 WAV 失败: %s\n", e.what());
                return 1;
            }
        }
    }

    // ---------------- 3) 发现并匹配目标渲染器 ----------------
    UpnpDevice targetDev;
    bool foundDev = false;

    if (!optDev.empty() && (optDev.rfind("http://", 0) == 0 || optDev.rfind("https://", 0) == 0)) {
        // 直接指定了设备描述 URL（支持跨子网/组播受限环境）
        std::printf("通过直连地址获取设备描述: %s\n", optDev.c_str());
        targetDev.location = optDev;
        const size_t sp = optDev.find("//");
        if (sp != std::string::npos) {
            const size_t slash = optDev.find('/', sp + 2);
            std::string hostport = optDev.substr(sp + 2, (slash == std::string::npos ? optDev.size() : slash) - (sp + 2));
            const std::string path = (slash == std::string::npos) ? "/" : optDev.substr(slash);
            const size_t colon = hostport.find(':');
            targetDev.host = (colon == std::string::npos) ? hostport : hostport.substr(0, colon);
            targetDev.port = (colon == std::string::npos) ? 80 : std::atoi(hostport.substr(colon + 1).c_str());

            std::string xml;
            if (upnp_detail::http_get(targetDev.host, targetDev.port, path, xml)) {
                targetDev.name    = upnp_detail::xml_tag(xml, "friendlyName");
                targetDev.model   = upnp_detail::xml_tag(xml, "modelName");
                targetDev.udn     = upnp_detail::xml_tag(xml, "UDN");
                targetDev.control = upnp_detail::extract_service_control_url(xml, "AVTransport");
                if (targetDev.control.empty()) targetDev.control = upnp_detail::xml_tag(xml, "controlURL");
                targetDev.controlConnection = upnp_detail::extract_service_control_url(xml, "ConnectionManager");
                foundDev = true;
            }
        }
        if (!foundDev) {
            std::fprintf(stderr, "错误：无法从指定 URL (%s) 获取设备描述。\n", optDev.c_str());
            if (needCleanupTemp) std::remove(tempWavPath.c_str());
            return 1;
        }
    } else {
        std::printf("搜索目标 UPnP 渲染器...\n");
        auto devs = upnp_discover(2000);
        if (devs.empty()) {
            std::fprintf(stderr, "错误：局域网内未发现任何 UPnP 渲染器，请检查设备是否开机与网络配置。\n");
            if (needCleanupTemp) std::remove(tempWavPath.c_str());
            return 1;
        }

        if (!optDev.empty()) {
            std::string query = lower_str(optDev);
            for (const auto& d : devs) {
                if (lower_str(d.name).find(query) != std::string::npos ||
                    lower_str(d.model).find(query) != std::string::npos ||
                    d.host.find(query) != std::string::npos) {
                    targetDev = d;
                    foundDev = true;
                    break;
                }
            }
            if (!foundDev) {
                std::fprintf(stderr, "错误：未找到名称/型号包含 \"%s\" 的渲染器。\n", optDev.c_str());
                if (needCleanupTemp) std::remove(tempWavPath.c_str());
                return 1;
            }
        } else {
            targetDev = devs[0];
            foundDev = true;
        }
    }

    std::printf("======================================================================\n");
    std::printf("目标渲染器: %s (%s)\n", targetDev.name.c_str(),
                targetDev.model.empty() ? "UPnP MediaRenderer" : targetDev.model.c_str());
    std::printf("设备控制地址: %s\n", targetDev.av_transport_url().c_str());
    std::printf("======================================================================\n");

    // ---------------- 4) 启动本地 HTTP 供源 ----------------
    std::string localIp = upnp_local_ip_for(targetDev.host, targetDev.port);
    UpnpHttpServer server;
    if (!server.start(optPort, "0.0.0.0")) {
        std::fprintf(stderr, "错误：无法在 0.0.0.0:%u 启动 HTTP 供源服务（端口可能被占用）\n", (unsigned)optPort);
        if (needCleanupTemp) std::remove(tempWavPath.c_str());
        return 1;
    }

    std::string streamPath = server.register_file(wavToServe, "audio/wav");
    std::string streamUrl = "http://" + localIp + ":" + std::to_string(server.port()) + streamPath;
    std::printf("本地 HTTP 供源就绪: %s\n", streamUrl.c_str());

    // ---------------- 5) UPnP 会话建立与起播 ----------------
    UpnpControlClient client(targetDev.av_transport_url(), targetDev.connection_manager_url());

    UpnpDidlItem didl;
    didl.title = optFile;
    size_t lastSlash = didl.title.find_last_of("\\/");
    if (lastSlash != std::string::npos) didl.title = didl.title.substr(lastSlash + 1);
    didl.creator = "RLPlayer Cast";
    didl.uri = streamUrl;
    didl.sampleRate = srcFormat.sampleRate;
    didl.bits = srcFormat.bits;
    didl.channels = srcFormat.channels;
    std::string didlXml = build_didl_lite(didl);

    std::printf("正在设置 AVTransport URI...\n");
    UpnpActionResult setRes = client.set_av_transport_uri(streamUrl, didlXml);
    if (!setRes.ok) {
        std::fprintf(stderr, "SetAVTransportURI 失败: %s\n", setRes.error.str().c_str());
        server.stop();
        if (needCleanupTemp) std::remove(tempWavPath.c_str());
        return 2;
    }

    std::printf("指令渲染器播放 (Play)...\n");
    UpnpActionResult playRes = client.play("1");
    if (!playRes.ok) {
        std::fprintf(stderr, "Play 失败: %s\n", playRes.error.str().c_str());
        client.stop();
        server.stop();
        if (needCleanupTemp) std::remove(tempWavPath.c_str());
        return 2;
    }

    // ---------------- 6) 轮询监控与取证 ----------------
    std::printf("起播成功！正在监控播放推进与格式取证（按 Ctrl+C 停止）...\n");

    std::string initialRelTime;
    std::string finalRelTime;
    UpnpMediaInfo lastMedia;
    UpnpPositionInfo lastPos;
    UpnpProtocolInfo lastProto;

    // 初始抓取 ProtocolInfo
    lastProto = client.get_protocol_info();

    auto startTime = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - startTime).count();
        if (optSeconds > 0.0 && elapsed >= optSeconds) {
            std::printf("\n达到指定播放时间 %.1f 秒，准备停止。\n", optSeconds);
            break;
        }

        UpnpTransportInfo transport = client.get_transport_info();
        lastPos = client.get_position_info();
        lastMedia = client.get_media_info();

        if (initialRelTime.empty() && !lastPos.relTime.empty() && lastPos.relTime != "00:00:00") {
            initialRelTime = lastPos.relTime;
        }
        if (!lastPos.relTime.empty()) {
            finalRelTime = lastPos.relTime;
        }

        std::printf("\r[播放中] 状态: %-12s | 进度: %s / %-8s | 已传输: %lld 字节   ",
                    transport.state.empty() ? "(未知)" : transport.state.c_str(),
                    lastPos.relTime.empty() ? "00:00:00" : lastPos.relTime.c_str(),
                    lastPos.trackDuration.empty() ? "--:--:--" : lastPos.trackDuration.c_str(),
                    (long long)server.total_bytes_served());

        if (transport.ok && (transport.state == "STOPPED" || transport.state == "NO_MEDIA_PRESENT") && elapsed > 2.0) {
            std::printf("\n渲染器已结束播放。\n");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::printf("\n停止渲染器...\n");
    client.stop();
    server.stop();
    if (needCleanupTemp) std::remove(tempWavPath.c_str());

    // ---------------- 7) 格式对照与分析报告 ----------------
    SamplingRateAnalysis analysis = analyze_sampling_rate(srcFormat, lastMedia, lastPos, lastProto);

    double t0 = parse_time_hh_mm_ss(initialRelTime);
    double t1 = parse_time_hh_mm_ss(finalRelTime);
    bool positionProgressed = (t1 > t0) || (t1 > 0.0);

    const char* vStr = (analysis.verdict == SamplingRateVerdict::Match) ? "一致 (Match)" :
                       (analysis.verdict == SamplingRateVerdict::Mismatch) ? "不一致 (Mismatch)" : "未知 (Unknown)";

    if (optAnalyze) {
        std::printf("\n======================================================================\n");
        std::printf("                    UPnP 投放分析报告 (--analyze)                     \n");
        std::printf("======================================================================\n");
        std::printf("供源音频格式 : %s\n", analysis.sourceDesc.c_str());
        std::printf("渲染器声明   : %s\n", analysis.rendererDesc.c_str());
        std::printf("采样率取证   : [%s]\n", vStr);

        if (!analysis.warning.empty()) {
            std::printf("告警提示     : %s\n", analysis.warning.c_str());
        }
        std::printf("详细判定说明 : %s\n", analysis.detail.c_str());
        std::printf("Position 推进: %s -> %s (%s)\n",
                    initialRelTime.empty() ? "00:00:00" : initialRelTime.c_str(),
                    finalRelTime.empty() ? "00:00:00" : finalRelTime.c_str(),
                    positionProgressed ? "推进正常" : "未见推进（可能未起播或渲染器未回报）");
        std::printf("HTTP 供源统计: 共发送 %lld 字节，服务请求 %zu 次\n",
                    (long long)server.total_bytes_served(), server.total_requests_served());
        std::printf("端到端实测   : 未做端到端重采样实测（P4 采集核心未合入，当前以渲染器协议声明作为判据）\n");
        std::printf("======================================================================\n");
    } else {
        std::printf("\n投放结束。采样率取证: [%s]（使用 --analyze 可查看详细分析报告）\n", vStr);
        if (!analysis.warning.empty()) {
            std::printf("告警: %s\n", analysis.warning.c_str());
        }
    }

    WSACleanup();
    return 0;
}
