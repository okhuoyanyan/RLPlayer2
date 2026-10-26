// cap_probe.cpp —— 只开采集端点读 1 秒，看有没有数据（不出声）
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include "audio/wasapi_capture.h"

int main(int argc, char** argv) {
    rta::audio::CaptureConfig cc;
    if (argc > 1) cc.deviceId = argv[1];
    cc.bufferMs = 200.0;
    cc.eventDriven = true;
    auto cap = std::make_shared<rta::audio::WasapiCapture>();
    if (!cap->open(cc)) {
        std::printf("open 失败: %s\n", cap->last_error().c_str());
        return 1;
    }
    std::printf("采集已开: rate=%u ch=%d\n", (unsigned)cap->rate(), cap->channels());
    if (!cap->start()) { std::printf("start 失败: %s\n", cap->last_error().c_str()); return 2; }

    long long frames = 0;
    double peak = 0.0, sumsq = 0.0;
    std::vector<float> buf(4096 * 8);
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < 1500) {
        const size_t got = cap->read_available(buf.data(), 4096);
        if (got > 0) {
            const int ch = cap->channels() > 0 ? cap->channels() : 1;
            for (size_t i = 0; i < got * (size_t)ch; ++i) {
                const double a = (double)buf[i];
                if (std::fabs(a) > peak) peak = std::fabs(a);
                sumsq += a * a;
            }
            frames += (long long)got;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cap->close();
    const double rms = frames > 0 ? std::sqrt(sumsq / (double)(frames * (cap->channels() > 0 ? cap->channels() : 1))) : 0.0;
    std::printf("1.5 秒内读到 %lld 帧，峰值 %.6f (%.1f dBFS)，RMS %.6f (%.1f dBFS)\n",
                frames, peak, peak > 0 ? 20.0 * std::log10(peak) : -999.0,
                rms, rms > 0 ? 20.0 * std::log10(rms) : -999.0);
    return frames > 0 ? 0 : 3;
}
