// asio_smoke.cpp —— ASIO 通路冒烟（−60 dBFS 不可听电平，短时）
// 目的：证明 enumerate -> open -> createBuffers -> start -> push -> stop 全链路可用。
// 电平默认 −60 dBFS（铁律 1：不需要听得见，统计量足够）。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <windows.h>
#include <objbase.h>
#include "audio/asio_out.h"

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 默认选虚拟驱动，避免向用户的听音设备出声
    std::string want = (argc > 1) ? argv[1] : "VASIO-8";
    const double levelDb = (argc > 2) ? atof(argv[2]) : -60.0;

    std::string err;
    const auto list = rta::audio::asio_enum_drivers();
    std::printf("驱动数=%zu  err=%s\n", list.size(), err.c_str());

    const rta::audio::AsioDriverEntry* pick = nullptr;
    for (const auto& e : list) if (e.name.find(want) != std::string::npos) { pick = &e; break; }
    if (!pick) {
        std::printf("没找到名字含 \"%s\" 的驱动，列出全部：\n", want.c_str());
        for (const auto& e : list) std::printf("  %s  %s\n", e.name.c_str(), e.clsid.c_str());
        return 2;
    }

    std::printf("\n★ 目标驱动 = %s（CLSID %s）\n", pick->name.c_str(), pick->clsid.c_str());
    std::printf("★ 电平 = %.1f dBFS（−60 不可听，仅验证通路）\n", levelDb);
    std::printf("★ 时长 = 2 秒  采样率 = 48000  声道 = 2  位深 = 24\n\n");

    rta::audio::AsioDriver drv;
    bool okOpen = false;
    // setSampleRate 有些驱动不接受（虚拟端点/被占用），先把它们真实支持的档位与当前值试出来
    for (double rate : { 48000.0, 44100.0, 96000.0 }) {
        err.clear();
        if (drv.open(pick->clsid, rate, 2, 24, err)) { okOpen = true; std::printf("open OK @ %.0f Hz\n", rate); break; }
        std::printf("  尝试 %.0f Hz 失败: %s\n", rate, err.c_str());
        drv.close();
    }
    if (!okOpen) { std::printf("open 全部失败\n"); return 3; }
    std::printf("open OK: ch=%d bufSize=%d\n", drv.channels(), drv.buffer_size());
    if (!drv.start(err)) { std::printf("start 失败: %s\n", err.c_str()); drv.close(); return 4; }
    std::printf("start OK，开始推 2 秒正弦\n");

    // 24-bit 满幅 = 2^23；−60 dBFS 幅度
    const double amp = std::pow(10.0, levelDb / 20.0) * 8388608.0;
    const int ch = drv.channels();
    std::vector<int32_t> buf(1024 * (size_t)ch);
    long long pushed = 0;
    const long long want2 = 48000LL * 2;
    double phase = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    while (pushed < want2) {
        const size_t n = 1024;
        for (size_t i = 0; i < n; ++i) {
            const double v = amp * std::sin(phase);
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
            if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
            for (int c = 0; c < ch; ++c) buf[i * (size_t)ch + (size_t)c] = (int32_t)v;
        }
        const size_t got = drv.push(buf.data(), n);
        pushed += (long long)got;
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count() > 5000) {
                std::printf("推送超时（5 秒内只推了 %lld 帧）\n", pushed);
                break;
            }
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    // ★ 判据修正（铁律 3）：只推「2 秒数据」再等 3 秒，最后 1 秒必然没数据 ⟹ 欠载是必然的，
    //   那不能证明驱动有问题。正确做法 = **按实时节奏持续供给 3 秒**，看这期间是否欠载。
    std::printf("\n按实时节奏持续供给 3 秒（模拟真实播放），观察欠载\n");
    const auto rt0 = std::chrono::steady_clock::now();
    long long supplied = 0;
    const long long rtWant = 48000LL * 3;
    while (supplied < rtWant) {
        const double elapsedSec = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - rt0).count() / 1000.0;
        const long long shouldHave = (long long)(elapsedSec * 48000.0);
        if (supplied >= shouldHave) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        const size_t n = 1024;
        for (size_t i = 0; i < n; ++i) {
            const double v = amp * std::sin(phase);
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
            if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
            for (int c = 0; c < ch; ++c) buf[i * (size_t)ch + (size_t)c] = (int32_t)v;
        }
        supplied += (long long)drv.push(buf.data(), n);
    }
    const auto rtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - rt0).count();
    std::printf("  实时供给 %lld 帧 / 应 %lld，耗时 %lld ms（实时应为 3000 ms）\n",
                supplied, rtWant, (long long)rtMs);
    std::printf("  回调 %llu 次，欠载 %llu 次\n",
                (unsigned long long)drv.callbacks(), (unsigned long long)drv.underruns());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    drv.stop();
    std::printf("\n推送 %lld 帧 / 期望 %lld  耗时 %lld ms（实时应为 2000 ms）\n", pushed, want2, (long long)ms);
    std::printf("callbacks=%llu  underruns=%llu\n",
                (unsigned long long)drv.callbacks(), (unsigned long long)drv.underruns());
    drv.close();
    CoUninitialize();
    std::printf("close OK（已恢复 PCM 格式）\n");
    return 0;
}
