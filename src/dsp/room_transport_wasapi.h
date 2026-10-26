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
// ★ 实测教训（2026-10-27，261026-08 #8/#9）：
//   测量电平太低会造成**假阴性**。同一套 5.1 系统：
//     -30 dBFS：L/R/C/LFE/Rs 峰位正常（533~594 ms），但 **Ls 峰位 10498 ms**（噪声主导，反卷积找错峰）
//     -20 dBFS：六声道全部正常，SNR 12.5~34.5 dB，峰位 535~586 ms
//   所以「峰位异常」先查电平与 SNR，不要急着判"音箱没接"。
//   质检（room_measurement）的 SNR 告警就是干这个用的：SNR 低 ⟹ 结论不可信，提高电平或降噪后重测。
//
// 已知限制（如实）：
//   · 共享模式播放 ⟹ 系统其它声音会混进录音。正式测量时请静音其它程序；UI 会提示。
//   · 每声道单独跑一遍（单声道扫频铺到所有声道），不做多声道同时扫频（那需要每声道独立扫频序列）。
#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
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
    // 停滞上限（2026-10-27 实机教训：设备被占用时不能无限等，否则整条测量链挂死）
    double      writeStallMs = 3000.0;    // 播放端点连续写不进超过这么多久 -> 中止
    double      captureStallMs = 5000.0;  // 采集端点连续无新数据超过这么多久 -> 中止
};

class WasapiRoomTransport : public MeasurementTransport {
public:
    explicit WasapiRoomTransport(RoomTxConfig cfg) : cfg_(std::move(cfg)) {}
    ~WasapiRoomTransport() override = default;

    bool lastOk() const { return ok_; }
    const std::string& lastError() const { return err_; }
    // 日志注入（dsp 层不依赖 rtaweb 的 play_log；由调用方接 rlplayer-playback.log）
    std::function<void(const std::string&)> log;
    size_t capturedSamples() const { return captured_; }
    double captureRate() const { return rate_; }
    const std::string& playName() const { return playName_; }
    const std::string& capName() const { return capName_; }

    std::vector<double> play_and_capture(const std::vector<double>& out, size_t samplesToCapture) override {
        ok_ = false; err_.clear(); captured_ = 0;
        if (out.empty()) { err_ = "扫频信号为空"; return {}; }

        // 1) 播放端点（共享模式：不独占、不静音系统声音，测量时可听）
        // 2026-10-26（261026-08 #8，用户接入 5.1 虚拟端点 VBMatrix In 2 = 6ch）：
        // 输出端点要按**布局声道数**打开，不能硬编码 2 —— 否则 5.1/7.1 布局根本打不开 6 声道流。
        auto spk = std::make_shared<rta::audio::WasapiSharedOutput>();
        const uint32_t wantRate = 48000;
        const int reqCh = (cfg_.channels > 0) ? cfg_.channels : 2;
        if (!spk->open(cfg_.playDeviceId, wantRate, reqCh, 32, 200.0)) {
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
        // 返回 false = 超时（采集端没有推进），调用方必须中止而不是继续等
        auto waitUntil = [&](size_t want, double limitMs) -> bool {
            const int spins = (int)(limitMs / 2.0);
            for (int i = 0; i < spins && frames() < want; ++i) {
                drain();
                if (frames() >= want) return true;
                if (cancelled && cancelled()) return true;   // 取消由调用方判断
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return frames() >= want;
        };

        // 3) 预录
        if (!waitUntil(preN, cfg_.captureStallMs)) {
            err_ = "采集端点无数据（预录阶段），检查采集设备是否可用";
            if (log) log("ROOMTX " + err_);
            spk->close(); cap->close(); ok_ = false; captured_ = 0;
            return {};
        }

        // 4) 边播边录：扫频只铺到**当前被测声道**。
        //    原先把同一段扫频铺到所有声道 —— 那样 5.1 测出来是"全场合奏"，分不清是哪一路，
        //    多声道校正就失去意义。MeasurementSession 是**逐声道顺序调用**本函数，
        //    所以用调用计数轮转声道即可（不依赖会话层改接口）。
        const int activeCh = (playCh > 0) ? (curCh_ % playCh) : 0;
        if (log) log("ROOMTX play dev=" + (cfg_.playDeviceId.empty() ? std::string("<default>") : cfg_.playDeviceId) +
                     " ch=" + std::to_string(playCh) + " active=" + std::to_string(activeCh));
        ++curCh_;
        // ★ 2026-10-26（261026-08 实机发现）：节奏必须严格 1:1。
        //   旧代码写的是 waitUntil(preN + written / 2) —— 每写 1 帧只等 0.5 帧，
        //   于是播放远快于实时、录音却按实时走，两者错位。实测后果：3 秒扫频却在
        //   peakMs=10049 ms 处出峰（多了 7 秒），LFE 对齐算出 subDelay=-41020。
        //   正确做法：写完多少帧，就等采集也到这么多帧（1:1），并处理 write 的部分写入。
        std::vector<float> blk((size_t)playCh * 480);
        size_t written = 0;
        while (written < out.size()) {
            if (cancelled && cancelled()) break;
            const size_t n = std::min<size_t>(480, out.size() - written);
            for (size_t i = 0; i < n; ++i) {
                const float v = (float)out[written + i];
                for (int c = 0; c < playCh; ++c)
                    blk[i * (size_t)playCh + (size_t)c] = (c == activeCh) ? v : 0.0f;
            }
            // 共享输出是非阻塞写，可能只吃下一部分；必须循环写满，否则实际播放序列短于参考
            // ★ 2026-10-27 实机抓到：设备被其它程序占用时 write_available_f32 恒返回 0，
            //   这个 while 会无限阻塞（实测 270 秒只走完 1/6 声道，running 永远 true）。
            //   必须给「写不进」和「采集没动静」都设上限，宁可报错也不能把测量线程挂死。
            size_t wrote = 0;
            int stallMs = 0;
            while (wrote < n) {
                if (cancelled && cancelled()) break;
                const size_t got = spk->write_available_f32(blk.data() + wrote * (size_t)playCh, n - wrote);
                if (got > 0) { wrote += got; stallMs = 0; continue; }
                drain();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (++stallMs > (int)cfg_.writeStallMs) {
                    err_ = "播放端点写不进数据超过 " + std::to_string((int)cfg_.writeStallMs) +
                           " ms（设备可能被独占或被其它程序占用），已中止本次测量";
                    if (log) log("ROOMTX " + err_);
                    spk->close(); cap->close();
                    ok_ = false; captured_ = 0;
                    return {};
                }
            }
            if (cancelled && cancelled()) break;
            written += n;
            drain();
            // 采集也必须设上限：否则采集端点不出数据时会在这里挂死
            if (!waitUntil(preN + written, cfg_.captureStallMs)) {
                err_ = "采集端点超过 " + std::to_string((int)cfg_.captureStallMs) +
                       " ms 没有新数据，已中止本次测量";
                if (log) log("ROOMTX " + err_);
                spk->close(); cap->close();
                ok_ = false; captured_ = 0;
                return {};
            }
        }

        // 5) 尾音（超时不算失败：尾巴录不全只是混响短一点，不该判整轮作废）
        if (!waitUntil(preN + out.size() + tailN, cfg_.captureStallMs)) {
            if (log) log("ROOMTX 尾音阶段采集停滞，按已录样本继续（房间混响可能录不全）");
        }
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
    int curCh_ = 0;                  // 逐声道轮转（每次 play_and_capture = 测下一个声道）
    std::string playName_, capName_;
};

}  // namespace rta::dsp
