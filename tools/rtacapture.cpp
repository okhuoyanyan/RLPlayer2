// rtacapture.cpp — WASAPI 音频采集命令行工具
//
// 命令行选项：
//   rtacapture --list
//   rtacapture [--dev <子串>] [--loopback] [--seconds N] [--out <wav>] [--analyze]
//   rtacapture --src null [--inject <freq>:<amp>] [--seconds N] [--out <wav>] [--analyze] [--fast]
//
// 退出码约定：
//   0 = 成功
//   2 = 参数错误
//   3 = 设备失败
//   4 = 分析失败

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include "audio/wasapi_capture.h"

using namespace rta::audio;

static void print_usage() {
    std::printf("用法:\n");
    std::printf("  rtacapture --list\n");
    std::printf("  rtacapture [--dev <子串>] [--loopback] [--seconds N] [--out <wav>] [--analyze]\n");
    std::printf("  rtacapture --src null [--inject <freq>:<amp>] [--seconds N] [--out <wav>] [--analyze] [--fast]\n");
    std::printf("  rtacapture --in <wav> [--analyze]   分析已有 WAV 文件\n");
    std::printf("\n选项说明:\n");
    std::printf("  --list               列出所有可用的输入设备及回环端点并退出\n");
    std::printf("  --in, --file <wav>   分析已有 WAV 文件的格式与指标\n");
    std::printf("  --dev <子串>         指定目标采集设备的友好名或 ID 子串\n");
    std::printf("  --loopback           启用系统混音回环采集 (AUDCLNT_STREAMFLAGS_LOOPBACK)\n");
    std::printf("  --seconds N          采集时长 (秒, 默认 5.0)\n");
    std::printf("  --out <wav>          将采集的音频写出为 24-bit PCM WAV 文件\n");
    std::printf("  --analyze            在采集结束后进行 RMS / 峰值 / THD+N 分析\n");
    std::printf("  --src null           【自动化门禁专用】不打开真设备, 模拟 48k/2ch 数据流\n");
    std::printf("  --inject <freq:amp>  设置注入的正弦波参数, 例如 1000:0.001 (1kHz, -60dBFS)\n");
    std::printf("  --fast               在 --src null 模式下启用快进模拟 (非实时推进)\n");
    std::printf("  --help, -h           显示本帮助信息\n");
    std::printf("\n退出码:\n");
    std::printf("  0 = 成功, 2 = 参数错误, 3 = 设备失败, 4 = 分析失败\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        print_usage();
        return 2;
    }

    std::string devSubstring;
    bool loopback = false;
    double seconds = 5.0;
    std::string outWav;
    std::string inWav;
    bool analyze = false;
    bool srcNull = false;
    double injectFreq = 1000.0;
    double injectAmp = 0.001;
    bool fastMode = false;
    bool listOnly = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--dev") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: --dev 缺少参数\n");
                return 2;
            }
            devSubstring = argv[++i];
        } else if (arg == "--loopback") {
            loopback = true;
        } else if (arg == "--seconds") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: --seconds 缺少参数\n");
                return 2;
            }
            char* end = nullptr;
            seconds = std::strtod(argv[++i], &end);
            if (!end || *end != '\0' || seconds <= 0.0) {
                std::fprintf(stderr, "错误: --seconds 参数必须为正数\n");
                return 2;
            }
        } else if (arg == "--out") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: --out 缺少文件路径\n");
                return 2;
            }
            outWav = argv[++i];
        } else if (arg == "--in" || arg == "--file") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: %s 缺少文件路径\n", arg.c_str());
                return 2;
            }
            inWav = argv[++i];
        } else if (arg == "--analyze") {
            analyze = true;
        } else if (arg == "--src") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: --src 缺少参数\n");
                return 2;
            }
            std::string srcType = argv[++i];
            if (srcType == "null") {
                srcNull = true;
            } else {
                std::fprintf(stderr, "错误: 不支持的 --src 类型 '%s' (仅支持 'null')\n", srcType.c_str());
                return 2;
            }
        } else if (arg == "--inject") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: --inject 缺少参数\n");
                return 2;
            }
            std::string inj = argv[++i];
            size_t colon = inj.find(':');
            if (colon == std::string::npos) {
                char* end = nullptr;
                injectFreq = std::strtod(inj.c_str(), &end);
                if (!end || *end != '\0' || injectFreq <= 0.0) {
                    std::fprintf(stderr, "错误: --inject 频率格式无效: '%s'\n", inj.c_str());
                    return 2;
                }
                injectAmp = 0.001;
            } else {
                std::string fStr = inj.substr(0, colon);
                std::string aStr = inj.substr(colon + 1);
                char *end1 = nullptr, *end2 = nullptr;
                injectFreq = std::strtod(fStr.c_str(), &end1);
                injectAmp = std::strtod(aStr.c_str(), &end2);
                if (!end1 || *end1 != '\0' || !end2 || *end2 != '\0' || injectFreq <= 0.0 || injectAmp < 0.0) {
                    std::fprintf(stderr, "错误: --inject 格式无效 (需 freq:amp): '%s'\n", inj.c_str());
                    return 2;
                }
            }
        } else if (arg == "--fast") {
            fastMode = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "错误: 未知参数 '%s'\n", arg.c_str());
            return 2;
        }
    }

    if (!inWav.empty()) {
        try {
            AudioBuffer ab = read_wav(inWav);
            std::vector<float> interleaved(ab.raw.size());
            if (ab.isFloat) {
                for (size_t i = 0; i < ab.raw.size(); ++i) {
                    uint32_t u = (uint32_t)ab.raw[i];
                    float f; std::memcpy(&f, &u, 4);
                    interleaved[i] = f;
                }
            } else {
                double scale = (double)(1u << (ab.bits - 1));
                for (size_t i = 0; i < ab.raw.size(); ++i) {
                    interleaved[i] = (float)((double)ab.raw[i] / scale);
                }
            }
            size_t frames = ab.raw.size() / (size_t)ab.channels;
            std::printf("[rtacapture] WAV 文件信息: %s\n", inWav.c_str());
            std::printf("  总帧数  : %zu 帧 (%.3f 秒 @ %u Hz, %d ch, %d-bit%s)\n",
                        frames, (double)frames / (double)ab.rate, ab.rate, ab.channels,
                        ab.bits, ab.isFloat ? " float" : " PCM");
            if (analyze) {
                AnalysisResult ar = analyze_audio(interleaved.data(), frames, ab.channels, ab.rate, injectFreq);
                std::printf("  RMS电平 : %.2f dBFS (总) ", ar.overallRmsDb);
                if (ab.channels == 2) {
                    std::printf("[L: %.2f dBFS, R: %.2f dBFS]\n", ar.rmsDb[0], ar.rmsDb[1]);
                } else {
                    std::printf("\n");
                }
                std::printf("  峰值电平: %.6f (%.2f dBFS)\n", ar.overallPeak, ar.overallPeakDb);
                if (ar.thdnDb > -900.0) {
                    std::printf("  THD+N   : %.2f dB\n", ar.thdnDb);
                }
            }
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "错误: 读取或分析 WAV 失败: %s\n", e.what());
            return 4;
        }
    }

    if (listOnly) {
        auto devs = WasapiCapture::enumerate_capture(true);
        std::printf("WASAPI 采集端点列表 (%zu 个):\n", devs.size());
        for (size_t i = 0; i < devs.size(); ++i) {
            const auto& d = devs[i];
            const char* st = (d.state == DEVICE_STATE_ACTIVE) ? "ACTIVE" :
                             (d.state == DEVICE_STATE_DISABLED) ? "DISABLED" :
                             (d.state == DEVICE_STATE_NOTPRESENT) ? "NOTPRESENT" : "UNPLUGGED";
            std::printf("  [%zu] %-42s %-10s %s %s\n",
                        i, d.name.c_str(), st,
                        d.isDefault ? "<默认>" : "",
                        d.isLoopback ? "[回环]" : "[录音]");
            if (d.mixRate > 0) {
                std::printf("       混音格式: %u Hz, %d ch, %d bits\n",
                            d.mixRate, d.mixChannels, d.mixBits);
            }
        }
        return 0;
    }

    WasapiCapture cap;
    CaptureConfig cfg;
    cfg.deviceId = devSubstring;
    cfg.loopback = loopback;
    cfg.bufferMs = 100.0;
    cfg.eventDriven = true;
    cfg.srcNull = srcNull;
    cfg.injectFreq = injectFreq;
    cfg.injectAmp = injectAmp;
    cfg.paced = !fastMode;

    if (srcNull) {
        size_t targetFrames = (size_t)(seconds * 48000.0);
        cfg.targetFrames = targetFrames;
        std::printf("[rtacapture] 使用模拟源 (--src null): 48000 Hz, 2 ch, 注入 %g Hz @ %g (约 %.1f dBFS)\n",
                    injectFreq, injectAmp, 20.0 * std::log10(injectAmp > 1e-12 ? injectAmp : 1e-12));
    } else {
        std::string chosenName = "(默认端点)";
        auto devs = WasapiCapture::enumerate_capture(loopback);
        bool found = false;
        for (const auto& d : devs) {
            if (d.isLoopback != loopback) continue;
            if (devSubstring.empty()) {
                if (d.isDefault) { chosenName = d.name; found = true; break; }
            } else {
                if (d.id == devSubstring || d.name.find(devSubstring) != std::string::npos) {
                    chosenName = d.name; found = true; break;
                }
            }
        }
        if (!devSubstring.empty() && !found) {
            std::fprintf(stderr, "[rtacapture] 设备失败: 未找到匹配端点 '%s'\n", devSubstring.c_str());
            return 3;
        }

        std::printf("*******************************************************************************\n");
        std::printf("** [rtacapture] 音频采集准备就绪                                             **\n");
        std::printf("**   目标设备: %-60s **\n", chosenName.c_str());
        std::printf("**   采集类型: %-60s **\n", loopback ? "WASAPI 共享系统回环 (Loopback)" : "WASAPI 麦克风/线路输入");
        std::printf("**   计划时长: %-60.2f **\n", seconds);
        std::printf("**   安全声明: 共享模式只读采集，不向设备输出声音，不接管系统独占通道        **\n");
        std::printf("*******************************************************************************\n");
    }

    if (!cap.open(cfg)) {
        std::fprintf(stderr, "[rtacapture] 设备初始化失败: %s\n", cap.last_error().c_str());
        return 3;
    }

    if (!cap.start()) {
        std::fprintf(stderr, "[rtacapture] 启动采集流失败: %s\n", cap.last_error().c_str());
        return 3;
    }

    const uint32_t rate = cap.rate();
    const int channels = cap.channels();
    const size_t totalTargetFrames = (size_t)(seconds * (double)rate);

    std::vector<float> recorded;
    recorded.reserve(totalTargetFrames * (size_t)channels);
    std::vector<float> tempChunk(4096 * (size_t)channels);

    auto tStart = std::chrono::steady_clock::now();
    size_t recordedFrames = 0;

    while (recordedFrames < totalTargetFrames) {
        size_t want = std::min((size_t)4096, totalTargetFrames - recordedFrames);
        size_t n = cap.read_available(tempChunk.data(), want);
        if (n > 0) {
            recorded.insert(recorded.end(), tempChunk.begin(), tempChunk.begin() + n * (size_t)channels);
            recordedFrames += n;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!srcNull) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - tStart).count();
            if (elapsed > seconds + 3.0) {
                // 超时保护
                break;
            }
        }
    }

    cap.stop();

    if (!outWav.empty() && recordedFrames > 0) {
        try {
            write_capture_wav(outWav, recorded.data(), recordedFrames, channels, rate, 24);
            std::printf("[rtacapture] WAV 写入成功: %s (%zu 帧, %.3f 秒, 24-bit PCM)\n",
                        outWav.c_str(), recordedFrames, (double)recordedFrames / (double)rate);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[rtacapture] WAV 写入异常: %s\n", e.what());
            return 4;
        }
    }

    AnalysisResult ar;
    if (analyze) {
        if (recordedFrames == 0) {
            std::fprintf(stderr, "[rtacapture] 分析失败: 未采得任何音频帧\n");
            return 4;
        }
        ar = analyze_audio(recorded.data(), recordedFrames, channels, rate, injectFreq);
        if (ar.overallRms <= 0.0 && ar.overallPeak <= 0.0 && injectAmp > 0.0 && srcNull) {
            std::fprintf(stderr, "[rtacapture] 分析失败: 注入信号未检出\n");
            return 4;
        }
    }

    std::printf("\n[rtacapture] 采集统计:\n");
    std::printf("  总帧数  : %zu 帧 (%.3f 秒 @ %u Hz, %d ch)\n",
                recordedFrames, (double)recordedFrames / (double)rate, rate, channels);
    std::printf("  环形缓冲: 溢出 %llu 帧, 下溢 %llu 帧\n",
                (unsigned long long)cap.overflow_count(), (unsigned long long)cap.underflow_count());
    std::printf("  削波计数: 正峰 %lld, 负峰 %lld\n",
                cap.positive_clips(), cap.negative_clips());

    if (analyze) {
        std::printf("  RMS电平 : %.2f dBFS (总) ", ar.overallRmsDb);
        if (channels == 2) {
            std::printf("[L: %.2f dBFS, R: %.2f dBFS]\n", ar.rmsDb[0], ar.rmsDb[1]);
        } else {
            std::printf("\n");
        }
        std::printf("  峰值电平: %.6f (%.2f dBFS)\n", ar.overallPeak, ar.overallPeakDb);
        if (ar.thdnDb > -900.0) {
            std::printf("  THD+N   : %.2f dB\n", ar.thdnDb);
        } else {
            std::printf("  THD+N   : N/A\n");
        }
    }

    return 0;
}
