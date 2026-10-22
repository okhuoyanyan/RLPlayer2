// test_capture.cpp — ROADMAP P4a: WASAPI 采集核心 + 环形缓冲 + 格式转换 + THD+N 离线门禁
//
// 验证范围：
//   §1 无锁 SPSC 环形缓冲：读写顺序、环绕、溢出/下溢计数、水位
//   §2 SPSC 两线程压测：模拟 10 分钟 48k 数据（28800000 帧），快进循环，断言 0 溢出且帧数精确
//   §3 格式转换与削波计数：16/24/32-bit 整型与 float → float 数值正确性（含 24-bit 符号扩展坑）及削波检测
//   §4 WAV 写出回读：采样数/位深/声道/时长精确校验
//   §5 --src null --inject 1000:0.001 端到端：帧数精确、RMS 误差 <0.5%、THD+N >= 60 dB
//   §6 CLI 参数解析与退出码门禁（0=成功、2=参数错误、3=设备失败、4=分析失败）
//   §7 真实设备回环 smoke（铁律 1：只读回环采集，绝不出声，时限 ≤1 秒）

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <filesystem>
#include "platform/path.h"
#include "platform/process.h"
#include "audio/wasapi_capture.h"
#include "audio/wav.h"

using namespace rta::audio;

#ifndef RTA_CAPTURE_EXE
#define RTA_CAPTURE_EXE "build/rtacapture.exe"
#endif

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s  %s\n", what, detail.c_str());
        ++g_fail;
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("=================================================================\n");
    std::printf("== WASAPI 采集核心离线门禁测试 (ROADMAP P4a)                   ==\n");
    std::printf("=================================================================\n");

    // ---------------- §1 无锁 SPSC 环形缓冲基础与边界 ----------------
    std::printf("\n-- §1 无锁 SPSC 环形缓冲基础与边界 --\n");
    {
        SpscRingBuffer<float> rb(128, 2);
        ck(rb.capacity() == 128, "容量向上对齐为 2 的幂 (128)");
        ck(rb.available_read() == 0, "初始可读为 0");
        ck(rb.available_write() == 128, "初始可写为 128");
        ck(rb.water_level() == 0.0, "初始水位为 0.0");
        ck(rb.overflow_count() == 0, "初始溢出为 0");
        ck(rb.underflow_count() == 0, "初始下溢为 0");

        // 写入 32 帧
        std::vector<float> writeData(32 * 2);
        for (size_t i = 0; i < 32; ++i) {
            writeData[i * 2 + 0] = (float)i;
            writeData[i * 2 + 1] = (float)(-1.0 * (double)i);
        }
        size_t w = rb.write(writeData.data(), 32);
        ck(w == 32, "写入 32 帧");
        ck(rb.available_read() == 32, "可读帧数为 32");
        ck(rb.available_write() == 96, "剩余可写帧数为 96");
        ck(std::fabs(rb.water_level() - 0.25) < 1e-6, "水位为 25%% (0.25)");

        // 读回 32 帧并校验数据
        std::vector<float> readData(32 * 2, 0.0f);
        size_t r = rb.read(readData.data(), 32);
        ck(r == 32, "读出 32 帧");
        bool match = true;
        for (size_t i = 0; i < 32 * 2; ++i) {
            if (readData[i] != writeData[i]) { match = false; break; }
        }
        ck(match, "读出数据与写入数据逐点完全一致");
        ck(rb.available_read() == 0, "读完后可读为 0");
        ck(rb.available_write() == 128, "读完后可写恢复为 128");

        // 环绕测试：先推进指针到缓冲区中间，再跨越边界写读
        std::vector<float> dummy(80 * 2, 1.23f);
        rb.write(dummy.data(), 80);
        rb.read(dummy.data(), 80); // write_pos = 112, read_pos = 112

        std::vector<float> wrapWrite(64 * 2);
        for (size_t i = 0; i < 64; ++i) {
            wrapWrite[i * 2 + 0] = (float)(100 + i);
            wrapWrite[i * 2 + 1] = (float)(200 + i);
        }
        // 112 + 64 = 176 > 128，必然发生环绕回 index 0..47
        size_t wWrap = rb.write(wrapWrite.data(), 64);
        ck(wWrap == 64, "跨越边界环绕写入 64 帧成功");
        std::vector<float> wrapRead(64 * 2, 0.0f);
        size_t rWrap = rb.read(wrapRead.data(), 64);
        ck(rWrap == 64, "环绕读出 64 帧成功");
        bool wrapMatch = true;
        for (size_t i = 0; i < 64 * 2; ++i) {
            if (wrapRead[i] != wrapWrite[i]) { wrapMatch = false; break; }
        }
        ck(wrapMatch, "跨越边界环绕读出数据完全一致");

        // 溢出与下溢计数校验
        rb.reset();
        std::vector<float> fullData(128 * 2, 5.0f);
        rb.write(fullData.data(), 128);
        ck(rb.overflow_count() == 0, "写满 128 帧无溢出");
        std::vector<float> overData(20 * 2, 6.0f);
        size_t wOver = rb.write(overData.data(), 20);
        ck(wOver == 0, "缓冲已满时写入返回 0 帧");
        ck(rb.overflow_count() == 20, "超出 20 帧正确计入溢出计数 (overflow_count == 20)");

        rb.reset();
        std::vector<float> underBuf(15 * 2, 0.0f);
        size_t rUnder = rb.read(underBuf.data(), 15);
        ck(rUnder == 0, "缓冲为空时 read 返回 0 帧");
        ck(rb.underflow_count() == 15, "未满足的 15 帧正确计入下溢计数 (underflow_count == 15)");
    }

    // ---------------- §2 SPSC 两线程 10 分钟 48k 数据压测 ----------------
    std::printf("\n-- §2 SPSC 两线程 10 分钟 48k 数据压测 (28800000 帧) --\n");
    {
        const size_t totalFrames = 10 * 60 * 48000; // 28,800,000 帧
        const size_t chunkSize = 480;               // 模拟 10ms 回调包
        SpscRingBuffer<float> rb(65536, 2);

        auto t0 = std::chrono::steady_clock::now();
        std::atomic<bool> producerDone{false};
        size_t seqErrors = 0;
        size_t totalReadFrames = 0;

        // 生产者线程
        std::thread producer([&]() {
            std::vector<float> chunk(chunkSize * 2);
            size_t produced = 0;
            while (produced < totalFrames) {
                size_t toWrite = std::min(chunkSize, totalFrames - produced);
                for (size_t i = 0; i < toWrite; ++i) {
                    float v = (float)((produced + i) & 0x7FFFFF);
                    chunk[i * 2 + 0] = v;
                    chunk[i * 2 + 1] = -v;
                }
                while (rb.available_write() < toWrite) {
                    std::this_thread::yield();
                }
                size_t w = rb.write(chunk.data(), toWrite);
                produced += w;
            }
            producerDone.store(true, std::memory_order_release);
        });

        // 消费者线程（主线程）
        std::vector<float> consumerBuf(1024 * 2);
        while (totalReadFrames < totalFrames) {
            size_t want = std::min((size_t)1024, totalFrames - totalReadFrames);
            size_t got = rb.read_available(consumerBuf.data(), want);
            if (got > 0) {
                for (size_t i = 0; i < got; ++i) {
                    float expL = (float)((totalReadFrames + i) & 0x7FFFFF);
                    float expR = -expL;
                    if (consumerBuf[i * 2 + 0] != expL || consumerBuf[i * 2 + 1] != expR) {
                        ++seqErrors;
                    }
                }
                totalReadFrames += got;
            } else {
                std::this_thread::yield();
            }
        }

        producer.join();
        auto t1 = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(t1 - t0).count();
        double speedup = 600.0 / elapsedSec;

        std::printf("    10 分钟音频模拟完成: 耗时 %.3f 秒, 加速比 %.1f 倍实时\n", elapsedSec, speedup);
        ck(totalReadFrames == totalFrames, "10 分钟数据 (28800000 帧) 全部读出",
           std::to_string(totalReadFrames) + " / " + std::to_string(totalFrames));
        ck(rb.overflow_count() == 0, "10 分钟快进压测 0 溢出 (overflow == 0)",
           std::to_string(rb.overflow_count()));
        ck(rb.underflow_count() == 0, "10 分钟快进压测 0 下溢 (underflow == 0)",
           std::to_string(rb.underflow_count()));
        ck(seqErrors == 0, "所有 28800000 帧样本序列编号逐位完全正确 (0 错误)",
           std::to_string(seqErrors));
    }

    // ---------------- §3 格式转换与削波计数 ----------------
    std::printf("\n-- §3 格式转换与削波计数 (16/24/32-bit PCM 与 Float32) --\n");
    {
        ClipStats cs;

        // 16-bit PCM
        int16_t pcm16Data[5] = { 0, 16384, -16384, 32767, -32768 };
        float out16[5] = { 0 };
        convert_to_float(pcm16Data, out16, 5, 1, CaptureFormat::Pcm16, cs);
        ck(std::fabs(out16[0] - 0.0f) < 1e-6, "PCM16 0 -> 0.0f");
        ck(std::fabs(out16[1] - 0.5f) < 1e-6, "PCM16 16384 -> +0.5f");
        ck(std::fabs(out16[2] - (-0.5f)) < 1e-6, "PCM16 -16384 -> -0.5f");
        ck(std::fabs(out16[4] - (-1.0f)) < 1e-6, "PCM16 -32768 -> -1.0f (负满幅)");
        ck(cs.posClips == 1, "PCM16 32767 命中正向削波", std::to_string(cs.posClips));
        ck(cs.negClips == 1, "PCM16 -32768 命中负向削波", std::to_string(cs.negClips));

        // 24-bit 紧凑 3 字节 PCM（核心：符号扩展坑）
        cs = ClipStats{};
        uint8_t pcm24Data[6 * 3] = {
            0x00, 0x00, 0x00,        // 0 -> 0.0f
            0x00, 0x00, 0x40,        // +4194304 -> +0.5f
            0x00, 0x00, 0xC0,        // -4194304 -> -0.5f (最高字节0xC0符号位为1)
            0xFF, 0xFF, 0xFF,        // -1 -> -1/8388608 (符号扩展至 0xFFFFFFFF)
            0xFF, 0xFF, 0x7F,        // +8388607 -> 正向满幅削波
            0x00, 0x00, 0x80         // -8388608 -> -1.0f 负向满幅削波 (0x800000 扩展至 0xFF800000)
        };
        float out24[6] = { 0 };
        convert_to_float(pcm24Data, out24, 6, 1, CaptureFormat::Pcm24_3Byte, cs);
        ck(std::fabs(out24[0] - 0.0f) < 1e-6, "PCM24 3-byte 0 -> 0.0f");
        ck(std::fabs(out24[1] - 0.5f) < 1e-6, "PCM24 3-byte +4194304 -> +0.5f");
        ck(std::fabs(out24[2] - (-0.5f)) < 1e-6, "PCM24 3-byte -4194304 -> -0.5f (负数符号扩展正确)");
        ck(std::fabs(out24[3] - (-1.0f / 8388608.0f)) < 1e-7, "PCM24 3-byte 0xFFFFFF -> -1 (扩展至 0xFFFFFFFF)");
        ck(std::fabs(out24[5] - (-1.0f)) < 1e-6, "PCM24 3-byte -8388608 -> -1.0f (负满幅 0x800000 扩展正确)");
        ck(cs.posClips == 1, "PCM24 +8388607 命中正向削波", std::to_string(cs.posClips));
        ck(cs.negClips == 1, "PCM24 -8388608 命中负向削波", std::to_string(cs.negClips));

        // 32-bit PCM
        cs = ClipStats{};
        int32_t pcm32Data[5] = { 0, 1073741824, -1073741824, 2147483647, (int32_t)-2147483648LL };
        float out32[5] = { 0 };
        convert_to_float(pcm32Data, out32, 5, 1, CaptureFormat::Pcm32, cs);
        ck(std::fabs(out32[0] - 0.0f) < 1e-6, "PCM32 0 -> 0.0f");
        ck(std::fabs(out32[1] - 0.5f) < 1e-6, "PCM32 +2^30 -> +0.5f");
        ck(std::fabs(out32[2] - (-0.5f)) < 1e-6, "PCM32 -2^30 -> -0.5f");
        ck(std::fabs(out32[4] - (-1.0f)) < 1e-6, "PCM32 INT32_MIN -> -1.0f (负满幅)");
        ck(cs.posClips == 1, "PCM32 INT32_MAX 命中正向削波", std::to_string(cs.posClips));
        ck(cs.negClips == 1, "PCM32 INT32_MIN 命中负向削波", std::to_string(cs.negClips));

        // Float32
        cs = ClipStats{};
        float fltData[6] = { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.25f };
        float outFlt[6] = { 0 };
        convert_to_float(fltData, outFlt, 6, 1, CaptureFormat::Float32, cs);
        ck(outFlt[1] == 0.5f && outFlt[2] == -0.5f, "Float32 原样直通");
        ck(cs.posClips == 2, "Float32 1.0f 与 1.25f 命中 2 次正向削波", std::to_string(cs.posClips));
        ck(cs.negClips == 1, "Float32 -1.0f 命中 1 次负向削波", std::to_string(cs.negClips));
    }

    // ---------------- §4 WAV 写出与读回精确校验 ----------------
    std::printf("\n-- §4 WAV 写出与读回精确校验 (使用 src/audio/wav.h) --\n");
    {
        char tempPath[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPath);
        std::string wav24 = std::string(tempPath) + "test_capture_24bit.wav";
        std::string wav16 = std::string(tempPath) + "test_capture_16bit.wav";

        // 构造 1.5 秒 48k 双声道音频 (72000 帧)
        const size_t frames24 = 72000;
        std::vector<float> audio24(frames24 * 2);
        for (size_t i = 0; i < frames24; ++i) {
            double t = (double)i / 48000.0;
            audio24[i * 2 + 0] = (float)(0.001 * std::sin(2.0 * 3.141592653589793 * 1000.0 * t));
            audio24[i * 2 + 1] = (float)(0.001 * std::cos(2.0 * 3.141592653589793 * 1000.0 * t));
        }

        write_capture_wav(wav24, audio24.data(), frames24, 2, 48000, 24);
        AudioBuffer ab24 = read_wav(wav24);
        ck(ab24.rate == 48000, "WAV24 采样率精确为 48000 Hz");
        ck(ab24.channels == 2, "WAV24 声道数精确为 2");
        ck(ab24.bits == 24, "WAV24 位深精确为 24 bits");
        ck(ab24.frames() == frames24, "WAV24 采样帧数精确为 72000 帧", std::to_string(ab24.frames()));
        ck(std::fabs((double)ab24.frames() / (double)ab24.rate - 1.5) < 1e-6, "WAV24 时长精确为 1.500 秒");

        // 构造 0.5 秒 48k 双声道音频 (24000 帧) 16-bit
        const size_t frames16 = 24000;
        write_capture_wav(wav16, audio24.data(), frames16, 2, 48000, 16);
        AudioBuffer ab16 = read_wav(wav16);
        ck(ab16.rate == 48000, "WAV16 采样率精确为 48000 Hz");
        ck(ab16.channels == 2, "WAV16 声道数精确为 2");
        ck(ab16.bits == 16, "WAV16 位深精确为 16 bits");
        ck(ab16.frames() == frames16, "WAV16 采样帧数精确为 24000 帧", std::to_string(ab16.frames()));
        ck(std::fabs((double)ab16.frames() / (double)ab16.rate - 0.5) < 1e-6, "WAV16 时长精确为 0.500 秒");

        std::remove(wav24.c_str());
        std::remove(wav16.c_str());
    }

    // ---------------- §5 --src null --inject 1000:0.001 端到端 ----------------
    std::printf("\n-- §5 --src null --inject 1000:0.001 端到端与 THD+N 分析 --\n");
    {
        WasapiCapture cap;
        CaptureConfig cfg;
        cfg.srcNull = true;
        cfg.injectFreq = 1000.0;
        cfg.injectAmp = 0.001;
        const size_t targetFrames = 48000; // 1 秒
        cfg.targetFrames = targetFrames;
        cfg.paced = false; // 快进模式运行

        ck(cap.open(cfg), "打开模拟源 (--src null)");
        ck(cap.start(), "启动模拟流推流");

        std::vector<float> recorded;
        recorded.reserve(targetFrames * 2);
        std::vector<float> buf(2048 * 2);

        while (recorded.size() < targetFrames * 2) {
            size_t want = std::min((size_t)2048, targetFrames - recorded.size() / 2);
            size_t n = cap.read_available(buf.data(), want);
            if (n > 0) {
                recorded.insert(recorded.end(), buf.begin(), buf.begin() + n * 2);
            } else {
                std::this_thread::yield();
            }
        }
        cap.stop();

        ck(recorded.size() / 2 == targetFrames, "端到端采集帧数精确为 48000 帧",
           std::to_string(recorded.size() / 2));
        ck(cap.overflow_count() == 0, "端到端采集 0 溢出");

        AnalysisResult ar = analyze_audio(recorded.data(), targetFrames, 2, 48000, 1000.0);
        const double theoRms = 0.001 / std::sqrt(2.0); // 约 0.00070710678
        double rmsRelErr = std::fabs(ar.overallRms - theoRms) / theoRms;
        std::printf("    实测 RMS: %.8f (%.2f dBFS), 理论 RMS: %.8f, 相对误差: %.4f%%\n",
                    ar.overallRms, ar.overallRmsDb, theoRms, rmsRelErr * 100.0);
        ck(rmsRelErr < 0.005, "RMS 与理论值相对误差 < 0.5%", std::to_string(rmsRelErr * 100.0) + "%");
        ck(std::fabs(ar.overallPeak - 0.001) < 1e-5, "峰值精确为 0.001 (-60.0 dBFS)",
           std::to_string(ar.overallPeak));
        std::printf("    纯数字注入正弦实测 THD+N: %.2f dB\n", ar.thdnDb);
        ck(ar.thdnDb >= 60.0, "THD+N 有数值且 >= 60 dB", std::to_string(ar.thdnDb) + " dB");
    }

    // ---------------- §6 CLI 参数解析与退出码门禁 ----------------
    std::printf("\n-- §6 CLI 参数解析与退出码门禁 (tools/rtacapture) --\n");
    {
        std::string exe = RTA_CAPTURE_EXE;
        // 用双引号包围路径
        std::string qExe = "\"" + exe + "\"";

        // 1. 成功路径：--src null --seconds 0.2 --inject 1000:0.001 --analyze --fast
        {
            std::string cmd = qExe + " --src null --seconds 0.2 --inject 1000:0.001 --analyze --fast";
            auto pr = rta::platform::run_hidden(cmd, true);
            ck(pr.started && pr.exit_code == 0, "CLI --src null 正常执行退出码为 0",
               "exit=" + std::to_string(pr.exit_code));
            ck(pr.output.find("采集统计") != std::string::npos, "CLI 输出包含采集统计横幅");
            ck(pr.output.find("THD+N") != std::string::npos, "CLI 输出包含 THD+N 数值");
        }

        // 2. 参数错误路径：--seconds -5
        {
            std::string cmd = qExe + " --src null --seconds -5";
            auto pr = rta::platform::run_hidden(cmd, true);
            ck(pr.started && pr.exit_code == 2, "CLI 非法负数 --seconds 退出码为 2 (参数错误)",
               "exit=" + std::to_string(pr.exit_code));
        }

        // 3. 参数错误路径：--inject 格式错误
        {
            std::string cmd = qExe + " --src null --inject invalid_str";
            auto pr = rta::platform::run_hidden(cmd, true);
            ck(pr.started && pr.exit_code == 2, "CLI 错误 --inject 格式退出码为 2 (参数错误)",
               "exit=" + std::to_string(pr.exit_code));
        }

        // 4. 设备失败路径：指定不存在的端点
        {
            std::string cmd = qExe + " --dev NON_EXISTENT_CAPTURE_DEV_ZZZ_9999 --seconds 0.1";
            auto pr = rta::platform::run_hidden(cmd, true);
            ck(pr.started && pr.exit_code == 3, "CLI 不存在设备退出码为 3 (设备失败)",
               "exit=" + std::to_string(pr.exit_code));
        }
    }

    // ---------------- §7 真实设备回环采集 smoke（只读，铁律 1 约束） ----------------
    std::printf("\n-- §7 真实设备回环采集 smoke (只读回环监听，铁律 1 约束) --\n");
    {
        auto devs = WasapiCapture::enumerate_capture(true);
        std::string lbDevName, lbDevId;
        for (const auto& d : devs) {
            if (d.isLoopback && d.state == DEVICE_STATE_ACTIVE) {
                lbDevName = d.name;
                lbDevId = d.id;
                if (d.isDefault) break;
            }
        }

        if (lbDevId.empty()) {
            std::printf("  [NOTE] 未检测到活动状态的渲染端点，跳过真实设备回环 smoke\n");
        } else {
            std::printf("*******************************************************************************\n");
            std::printf("** 真实设备回环 smoke (只读监听):                                            **\n");
            std::printf("**   目标设备: %-60s **\n", lbDevName.c_str());
            std::printf("**   采样时长: 0.5 秒 (只读 WASAPI 回环，绝不出声，不影响系统其他声音)       **\n");
            std::printf("*******************************************************************************\n");

            WasapiCapture cap;
            CaptureConfig cfg;
            cfg.deviceId = lbDevId;
            cfg.loopback = true;
            cfg.eventDriven = true;
            cfg.bufferMs = 100.0;

            bool okOpen = cap.open(cfg);
            if (!okOpen) {
                std::printf("  [NOTE] 回环 open 报告: %s (真实环境如实记录)\n", cap.last_error().c_str());
            } else {
                ck(true, "真实设备回环 open 成功");
                bool okStart = cap.start();
                if (okStart) {
                    ck(true, "真实设备回环 start 成功");
                    std::vector<float> buf(2048 * cap.channels());
                    size_t gotTotal = 0;
                    auto tStart = std::chrono::steady_clock::now();
                    while (gotTotal < (size_t)(0.5 * (double)cap.rate())) {
                        size_t n = cap.read_available(buf.data(), 2048);
                        gotTotal += n;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration<double>(now - tStart).count() > 1.2) break;
                    }
                    cap.stop();
                    std::printf("    回环采集实测获得: %zu 帧 (%.3f 秒 @ %u Hz, %d 声道)\n",
                                gotTotal, (double)gotTotal / (double)cap.rate(), cap.rate(), cap.channels());
                    ck(cap.overflow_count() == 0, "回环采集 0 溢出");
                } else {
                    std::printf("  [NOTE] 回环 start 报告: %s\n", cap.last_error().c_str());
                }
                cap.close();
            }
        }
    }

    std::printf("\n=================================================================\n");
    if (g_fail == 0) {
        std::printf("== [PASS] 全部 capture 测试门禁通过 (0 fail)                    ==\n");
    } else {
        std::printf("== [FAIL] 存在 %d 项 capture 测试失败                         ==\n", g_fail);
    }
    std::printf("=================================================================\n");
    return g_fail > 0 ? 1 : 0;
}
