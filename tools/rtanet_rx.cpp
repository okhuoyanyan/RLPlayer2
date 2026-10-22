// rtanet_rx.cpp — RTANet 端点程序（跑在接 DAC 的那台机器上）
//
// 架构（Round 25 重构）：接收线程与播放线程【解耦】
//   接收线程：只管收包入抖动缓冲 + 累加 framesIn
//   播放线程：按墙钟/DAC 时钟取数据交付 sink + 回传状态
// 为什么必须解耦：单线程时 deliver 的实时等待会把接收也一起阻塞，
//   framesIn 停在原地，水位指标测出来恒为 0（Round 24 的教训）。
//
// ★ 安全约定（AGENTS.md 铁律 1）：
//   --sink file 是【默认】——写 WAV，完全不打开音频设备。
//   --sink wasapi 才真正出声，且启动前打印醒目横幅。
#include "net/rtanet_stream.h"
#include "net/rtanet_status.h"
#include "audio/wav.h"
#include "audio/wasapi_out.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

using namespace rta;
using namespace rta::net;
namespace audio = rta::audio;

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    uint16_t ctrlPort = 43210;
    std::string sink = "file", outPath = "rtanet_out.wav", deviceId;
    double bufferMs = 200.0, limitSec = 0.0;
    size_t preroll = 8;
    bool requireFormat = false, realtimeSink = false;
    RtaFormat want{48000, 24, 2};

    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&](const char* d) -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(d); };
        if (k == "--list") {
            auto devs = audio::WasapiOutput::enumerate();
            printf("渲染端点 %zu 个：\n", devs.size());
            for (size_t j = 0; j < devs.size(); ++j)
                printf("  [%zu] %-44s %s\n", j, devs[j].name.c_str(), devs[j].isDefault ? "<== 默认" : "");
            return 0;
        }
        else if (k == "--ctrl-port") ctrlPort = (uint16_t)atoi(next("43210").c_str());
        else if (k == "--sink")      sink = next("file");
        else if (k == "--out")       outPath = next("rtanet_out.wav");
        else if (k == "--device")    deviceId = next("");
        else if (k == "--buffer")    bufferMs = atof(next("200").c_str());
        else if (k == "--seconds")   limitSec = atof(next("0").c_str());
        else if (k == "--preroll")   preroll = (size_t)atoi(next("8").c_str());
        else if (k == "--realtime")  realtimeSink = true;
        else if (k == "--format") {
            const std::string fs = next("");
            unsigned r = 0; int b = 0, c = 0;
            if (std::sscanf(fs.c_str(), "%u/%d/%d", &r, &b, &c) == 3) { want = {r, b, c}; requireFormat = true; }
        }
    }

    printf("RTANet 端点启动\n  控制端口: %u (TCP)\n  输出方式: %s\n",
           (unsigned)ctrlPort, sink.c_str());
    if (requireFormat) printf("  要求格式: %s\n", want.str().c_str());

    TcpSocket listener;
    if (!listener.listen_any(ctrlPort)) { printf("!! 无法监听控制端口\n"); return 1; }
    UdpSocket udp;
    if (!udp.open(0)) { printf("!! 无法创建 UDP 套接字\n"); return 1; }
    udp.set_recv_buffer(8 * 1024 * 1024);
    udp.set_recv_timeout_ms(200);
    printf("  等待 DSP 机连接…\n");

    TcpSocket conn;
    if (!listener.accept_from(conn, 60000)) { printf("!! 等待连接超时\n"); return 2; }

    struct AnyServer : RtaControlServer {
        bool serve_any(TcpSocket& c, RtaFormat& f, uint32_t& sid, std::string& e, uint16_t udpPort) {
            std::string line;
            if (!c.recv_line(line) || line.rfind("HELLO", 0) != 0) { e = "缺少 HELLO"; return false; }
            if (!c.send_line("OK RTANET/1 rta-endpoint")) { e = "发送失败"; return false; }
            if (!c.recv_line(line) || line.rfind("FORMAT", 0) != 0) { e = "缺少 FORMAT"; return false; }
            f = parse_format(line);
            if (f.rate == 0 || (f.bits != 16 && f.bits != 24 && f.bits != 32) || f.channels < 1 || f.channels > 32) {
                e = "非法格式 " + f.str(); c.send_line("ERR FORMAT " + e); return false;
            }
            if (!c.send_line("OK FORMAT")) { e = "发送失败"; return false; }
            if (!c.recv_line(line) || line.rfind("START", 0) != 0) { e = "缺少 START"; return false; }
            sid = parse_kv_u32(line, "stream", 1);
            char b[96]; std::snprintf(b, sizeof(b), "OK START port=%u", (unsigned)udpPort);
            if (!c.send_line(b)) { e = "发送失败"; return false; }
            return true;
        }
    } anySrv;
    RtaControlServer srv;
    RtaFormat fmt{}; uint32_t streamId = 0; std::string err;
    const bool hs = requireFormat ? srv.serve(conn, fmt, streamId, err, want, udp.local_port())
                                  : anySrv.serve_any(conn, fmt, streamId, err, udp.local_port());
    if (!hs) { printf("!! 协商失败: %s\n", err.c_str()); return 3; }
    printf("  已连接, 协商完成: %s, stream=%u, UDP 端口 %u\n",
           fmt.str().c_str(), streamId, (unsigned)udp.local_port());

    audio::AudioBuffer out;
    out.rate = fmt.rate; out.channels = fmt.channels; out.bits = fmt.bits; out.isFloat = false;
    audio::WasapiOutput ao;
    bool usingWasapi = false;
    if (sink == "wasapi") {
        printf("\n╔══════════════════════════════════════════════════════════════╗\n");
        printf("║ ⚠️  --sink wasapi：独占模式会接管设备并静音系统其他声音      ║\n");
        printf("║ 格式 %-10s 缓冲 %.0f ms                                 ║\n", fmt.str().c_str(), bufferMs);
        printf("╚══════════════════════════════════════════════════════════════╝\n");
        if (!ao.open(deviceId, fmt.rate, fmt.channels, fmt.bits, bufferMs)) {
            printf("!! 打开输出失败: %s\n", ao.last_error().c_str()); return 4;
        }
        if (!ao.start()) { printf("!! 启动失败: %s\n", ao.last_error().c_str()); return 5; }
        usingWasapi = true;
    }

    // ================= 共享状态（接收线程 <-> 播放线程） =================
    std::mutex mtx;
    JitterBuffer jb(preroll);
    uint64_t framesIn = 0;
    std::atomic<bool> rxDone{false};

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    // ---- 接收线程：只管收包，绝不做任何等待 ----
    std::thread rxThread([&] {
        std::vector<uint8_t> buf(65536);
        size_t idle = 0;
        while (!rxDone.load()) {
            const int r = udp.recv_from(buf.data(), buf.size());
            if (r > 0) {
                idle = 0;
                RtaHeader ph{};
                if (r >= (int)RTANET_HEADER) std::memcpy(&ph, buf.data(), RTANET_HEADER);
                std::lock_guard<std::mutex> lk(mtx);
                if (ph.magic == RTANET_MAGIC && ph.version == RTANET_VERSION) framesIn += ph.frames;
                jb.push(std::vector<uint8_t>(buf.begin(), buf.begin() + r));
            } else if (++idle >= 25) {          // 5 秒无包
                break;
            }
        }
        rxDone.store(true);
    });

    // ---- 播放线程（主线程）：按墙钟节奏交付 + 回传状态 ----
    std::vector<int32_t> tmp;
    size_t framesOut = 0, packets = 0, silentPackets = 0;
    double rtStartSec = -1.0;
    double deliveredBufferMs = 0.0;
    std::deque<std::pair<double, double>> bufHist;
    auto lastReport = std::chrono::steady_clock::now();

    auto deliver = [&](const RtaHeader& hh, const std::vector<uint8_t>& pp) {
        const size_t n = hh.frames * (size_t)hh.channels;
        tmp.resize(n);
        if (hh.flags & RTA_FLAG_SILENCE) std::fill(tmp.begin(), tmp.end(), 0);
        else unpack_samples(tmp.data(), pp.data(), n, hh.bits);
        if (usingWasapi) ao.write(tmp.data(), hh.frames);
        else {
            const size_t need = (size_t)(hh.timestamp + hh.frames);
            if (out.raw.size() < need * (size_t)out.channels) out.raw.resize(need * (size_t)out.channels, 0);
            std::memcpy(out.raw.data() + (size_t)hh.timestamp * (size_t)out.channels,
                        tmp.data(), n * sizeof(int32_t));
        }
        framesOut += hh.frames;
    };

    for (;;) {
        const double el = elapsed();
        if (limitSec > 0.0 && el >= limitSec) break;
        if (rxDone.load()) {
            std::lock_guard<std::mutex> lk(mtx);
            if (jb.pending() == 0 || limitSec <= 0.0) { /* 收尾 */ }
        }

        // 实时节奏门：只有时钟放行才交付（不阻塞接收线程）
        if (realtimeSink) {
            if (rtStartSec < 0.0) rtStartSec = el;
            const double prefillSec = 0.20;
            const double allowed = (el - rtStartSec - prefillSec) * (double)fmt.rate;
            int guard = 0;
            while (guard++ < 4096) {
                RtaHeader h{}; std::vector<uint8_t> pay;
                bool got = false;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    got = jb.pop(h, pay);
                }
                if (!got) break;
                if ((double)(framesOut + h.frames) > allowed) {
                    // 时钟未放行：放回并退出本轮（不能丢！）
                    std::lock_guard<std::mutex> lk(mtx);
                    jb.unpop(h, pay);
                    break;
                }
                deliver(h, pay);
                ++packets;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            // 非实时模式：尽快把已到达的数据全部交付（用于逐位传输验证）
            int guard = 0;
            while (guard++ < 100000) {
                RtaHeader h{}; std::vector<uint8_t> pay;
                bool got = false;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    got = jb.pop(h, pay);
                }
                if (!got) break;
                deliver(h, pay);
                ++packets;
            }
            if (rxDone.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // ---- 每 100 ms 回传状态 ----
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastReport).count() >= 0.1) {
            lastReport = now;
            uint64_t fin = 0;
            { std::lock_guard<std::mutex> lk(mtx); fin = framesIn; }
            const double bufMs = ((double)fin - (double)framesOut) * 1000.0 / (double)fmt.rate;
            deliveredBufferMs = bufMs;
            bufHist.emplace_back(el, (double)fin - (double)framesOut);
            while (bufHist.size() > 2 && (el - bufHist.front().first) > 5.0) bufHist.pop_front();
            double drift = 0.0;
            bool haveDrift = bufHist.size() >= 8;
            if (haveDrift) {
                const size_t n = bufHist.size();
                double sx = 0, sy = 0, sxx = 0, sxy = 0;
                for (const auto& pt : bufHist) { sx += pt.first; sy += pt.second; sxx += pt.first * pt.first; sxy += pt.first * pt.second; }
                const double den = (double)n * sxx - sx * sx;
                if (std::fabs(den) > 1e-12)
                    drift = ((double)n * sxy - sx * sy) / den / (double)fmt.rate * 1e6;
            }
            conn.send_line(rta::net::format_status(framesOut, bufMs, drift, haveDrift));
        }
        if (rxDone.load()) break;
    }

    rxDone.store(true);
    if (rxThread.joinable()) rxThread.join();

    printf("\n接收统计:\n  包 %zu | 输出帧 %zu (%.3f 秒) | 补静音 %zu 包 | 末水位 %.1f ms\n",
           packets, framesOut, (double)framesOut / (double)fmt.rate, silentPackets, deliveredBufferMs);
    printf("  抖动缓冲: 收到 %llu, 迟到 %llu, 重复 %llu, 乱序 %llu, 判丢 %llu\n",
           (unsigned long long)jb.stats().received, (unsigned long long)jb.stats().late,
           (unsigned long long)jb.stats().duplicate, (unsigned long long)jb.stats().outOfOrder,
           (unsigned long long)jb.stats().lost);

    if (usingWasapi) { unsigned long long pos = 0; if (ao.device_position(pos))
        printf("  设备实际播放 %llu 帧 (%.3f 秒)\n", pos, (double)pos / (double)fmt.rate); ao.close(); }
    else {
        size_t nz = out.raw.size();
        while (nz > 0 && out.raw[nz - 1] == 0) --nz;
        out.raw.resize(nz - (nz % (size_t)out.channels));
        audio::write_wav(outPath, out);
        printf("  已写入 %s (%.3f 秒)\n", outPath.c_str(), (double)out.frames() / (double)out.rate);
    }
    return 0;
}
