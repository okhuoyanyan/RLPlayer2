// room_transport_wasapi.h — 房间测量的真实传输实现（2026-10-26，用户要求"房间EQ上架"）
//
// 为什么单独一个头：room_measurement.h 定义的是**接口**（MeasurementTransport），只依赖它就能
// 用虚拟房间端到端测算法（C2 的 45 项判据）。真实实现要碰 WASAPI 播放与采集，放进 dsp/ 会污染
// 算法层，所以放这里，由 rtaweb 接线。
//
// ★ 铁律 1：扫频本质可闻。调用方（/api/roomcorr/measure）必须先在 UI 把
//   「设备名 / 时长 / 电平 / 独占会静音系统声音」四项摊开、用户点确认后，才允许带 confirm=1 调用。
//   本类**不做任何电平放大**，用 MeasurementSession 给的电平（缺省 −20 dBFS）。
//
// 时序（反卷积靠这段）：
//   开采集 → 预录 preRollMs（吃本底噪声）→ 边播边录 → 播完再录 tailMs（混响尾巴）→ 双双关闭。
//   采集比播放早开晚关，保证直达声与尾音都在录音里。
//
// 已知限制（如实）：
//   · 共享模式播放 ⟹ 系统其它声音会混进录音。正式测量时请静音其它程序；UI 会提示。
//   · 每声道单独跑一遍（单声道扫频铺到所有声道），不做多声道同时扫频（那需要每声道独立扫频序列）。
#pragma once
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../audio/wasapi_capture.h"
#include "../audio/wasapi_shared.h"
#include "room_measurement.h"

namespace rta::dsp {

struct RoomTxConfig {
    std::string playDeviceId;      // 空 = 默认输出端点
    std::string captureDeviceId;   // 空 = 默认采集端点
    int         channels = 2;      // 参与测量的声道数
    double      tailMs = 800.0;    // 播放结束后继续录多久（房间混响）
    double      preRollMs = 300.0; // 播放前先录多久（本底噪声，质检用）
};

class WasapiRoomTransport : public MeasurementTransport {
public:
    explicit WasapiRoomTransport(RoomTxConfig cfg) : cfg_(std::move(cfg)) {}
    ~WasapiRoomTransport() override = default;

    bool lastOk() const { return ok_; }
    const std::string& lastError() const { return err_; }
    size_t capturedSamples() const { return captured_; }
    double captureRate() const { return rate_; }
    const std::string& playName() const { return playName_; }
    const std::string& capName() const { return capName_; }

    std::vector<double> play_and_capture(const std::vector<double>& out, size_t samplesToCapture) override {
        ok_ = false; err_.clear(); captured_ = 0;
        if (out.empty()) { err_ = "扫频信号为空"; return {}; }

        // 1) 播放端点（共享模式：不独占、不静音系统声音，测量时可听）
        auto spk = std::make_shared<rta::audio::WasapiSharedOutput>();
        const uint32_t wantRate = 48000;
        if (!spk->open(cfg_.playDeviceId, wantRate, 2, 32, 200.0)) {
            err_ = "打开播放端点失败：" + spk->last_error();
            return {};
        }
        const uint32_t playRate = spk->mix_rate() ? spk->mix_rate() : wantRate;
        const int playCh = spk->mix_channels() > 0 ? spk->mix_channels() : 2;

        // 2) 采集端点
        rta::audio::CaptureConfig cc;
        cc.deviceId = cfg_.captureDeviceId;
        cc.bufferMs = 200.0;
        cc.eventDriven = true;
        auto cap = std::make_shared<rta::audio::WasapiCapture>();
        if (!cap->open(cc)) {
            err_ = "打开采集端点失败：" + cap->last_error();
            spk->close();
            return {};
        }
        rate_ = (double)(cap->rate() ? cap->rate() : playRate);
        const int capCh = cap->channels() > 0 ? cap->channels() : 2;

        if (!spk->start()) { err_ = "启动播放失败：" + spk->last_error(); spk->close(); cap->close(); return {}; }
        if (!cap->start()) { err_ = "启动采集失败：" + cap->last_error(); spk->close(); cap->close(); return {}; }

        const size_t preN  = (size_t)(cfg_.preRollMs * 0.001 * rate_);
        const size_t tailN = (size_t)(cfg_.tailMs   * 0.001 * rate_);
        std::vector<float> rec;
        rec.reserve((preN + out.size() + tailN + 8192) * (size_t)capCh);

        auto drain = [&]() {
            std::vector<float> tmp((size_t)capCh * 2048);
            for (;;) {
                const size_t got = cap->read_available(tmp.data(), 2048);
                if (got == 0) break;
                rec.insert(rec.end(), tmp.begin(), tmp.begin() + (std::ptrdiff_t)(got * (size_t)capCh));
            }
        };
        auto frames = [&]() { return rec.size() / (size_t)capCh; };
        auto waitUntil = [&](size_t want) {
            const int spins = 4000;
            for (int i = 0; i < spins && frames() < want; ++i) {
                drain();
                if (frames() >= want) break;
                if (cancelled && cancelled()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };

        // 3) 预录
        waitUntil(preN);

        // 4) 边播边录（单声道扫频铺到所有声道）
        std::vector<float> blk((size_t)playCh * 480);
        size_t written = 0;
        while (written < out.size()) {
            if (cancelled && cancelled()) break;
            const size_t n = std::min<size_t>(480, out.size() - written);
            for (size_t i = 0; i < n; ++i) {
                const float v = (float)out[written + i];
                for (int c = 0; c < playCh; ++c) blk[i * (size_t)playCh + (size_t)c] = v;
            }
            spk->write_available_f32(blk.data(), n);
            written += n;
            drain();
            waitUntil(preN + written / 2);   // 大致跟上实时节奏即可，不追求精确
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // 5) 尾音
        waitUntil(preN + out.size() + tailN);
        drain();

        spk->close();
        cap->close();

        captured_ = frames();
        std::vector<double> mono(captured_, 0.0);
        for (size_t i = 0; i < captured_; ++i) mono[i] = (double)rec[i * (size_t)capCh];
        ok_ = (captured_ > 1000);
        if (!ok_) err_ = "采到的样本太少（" + std::to_string(captured_) + "），检查采集设备/静音设置";
        (void)samplesToCapture;
        return mono;
    }

private:
    RoomTxConfig cfg_;
    bool ok_ = false;
    std::string err_;
    size_t captured_ = 0;
    double rate_ = 0.0;
    std::string playName_, capName_;
};

}  // namespace rta::dsp
