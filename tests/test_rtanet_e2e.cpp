// test_rtanet_e2e.cpp — RTANet 端到端验证：控制面握手 + UDP 流 + 抖动缓冲还原
// 这是"两机方案"的完整链路演练（全部在本机回环上跑，不涉及音频设备）
// 判据：接收端重建出的样本流必须与发送端【逐位相同】
#include "net/rtanet_stream.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <random>
#include <string>

using namespace rta::net;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static std::vector<int32_t> make(uint64_t n, int ch, int bits, uint64_t seed) {
    std::vector<int32_t> v(n * (size_t)ch);
    const int32_t hi = (1 << (bits - 1)) - 1, lo = -(1 << (bits - 1));
    std::mt19937 rng((uint32_t)seed);
    std::uniform_int_distribution<int32_t> d(lo, hi);
    for (auto& x : v) x = d(rng);
    return v;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    const RtaFormat want{48000, 24, 2};
    const size_t FRAMES = 48000;          // 1 秒
    const size_t PPP = 128;               // 每包帧数
    const size_t NPKT = (FRAMES + PPP - 1) / PPP;

    printf("=== 端到端：控制面握手 -> UDP 流 -> 抖动缓冲还原 ===\n");
    printf("  格式 %s, %zu 帧, 每包 %zu 帧, 共 %zu 包\n", want.str().c_str(), FRAMES, PPP, NPKT);

    TcpSocket listener;
    if (!listener.listen(0)) { check(false, "控制端口监听失败"); return 1; }
    const uint16_t ctrlPort = listener.local_port();

    UdpSocket udp;
    if (!udp.open(0)) { check(false, "UDP 套接字创建失败"); return 1; }
    udp.set_recv_buffer(4 * 1024 * 1024);
    udp.set_recv_timeout_ms(200);   // 必须设：否则收不到包时会永久阻塞
    const uint16_t udpPort = udp.local_port();

    bool serverOk = false; std::string serverErr;
    RtaFormat negotiated; uint32_t gotStream = 0;
    std::thread srv([&] {
        TcpSocket conn;
        if (!listener.accept_from(conn, 3000)) { serverErr = "accept 超时"; return; }
        RtaControlServer s;
        serverOk = s.serve(conn, negotiated, gotStream, serverErr, want, udpPort);
    });

    // ---- 客户端协商 ----
    RtaControlClient cli;
    uint16_t epPort = 0; std::string cliErr;
    const bool cliOk = cli.negotiate("127.0.0.1", ctrlPort, want, epPort, 7, cliErr);
    char buf[240];
    std::snprintf(buf, sizeof(buf), "(控制端口 %u, 端点返回 UDP 端口 %u, stream=%u)",
                  ctrlPort, epPort, gotStream);
    check(cliOk, "控制面握手成功", cliOk ? buf : cliErr);

    // ---- 发送 + 接收（交错进行，避免接收缓冲溢出）----
    auto src = make(FRAMES, want.channels, want.bits, 20261019u);
    RtaSender snd;
    snd.configure("127.0.0.1", epPort ? epPort : udpPort, want, 7, PPP);

    std::vector<uint8_t> rbuf(65536);
    std::vector<std::vector<uint8_t>> pkts;
    size_t sentFrames = 0;
    const size_t batchFrames = PPP * 16;

    while (sentFrames < FRAMES) {
        const size_t n = std::min(batchFrames, FRAMES - sentFrames);
        sentFrames += snd.send(src.data() + sentFrames * (size_t)want.channels, n);
        // 排空这一批
        for (size_t i = 0; i < n / PPP + 1; ++i) {
            const int r = udp.recv_from(rbuf.data(), rbuf.size());
            if (r <= 0) break;
            pkts.emplace_back(rbuf.begin(), rbuf.begin() + r);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    // 收尾：把剩下的包收干净
    for (int i = 0; i < 64; ++i) {
        const int r = udp.recv_from(rbuf.data(), rbuf.size());
        if (r <= 0) break;
        pkts.emplace_back(rbuf.begin(), rbuf.begin() + r);
    }

    std::snprintf(buf, sizeof(buf), "(发送 %zu 帧, 收到 %zu 包, 期望 %zu)",
                  snd.frames_sent(), pkts.size(), NPKT);
    check(snd.frames_sent() == FRAMES, "发送帧数正确", buf);
    check(pkts.size() == NPKT, "收到全部包", buf);

    // ---- 抖动缓冲还原 ----
    JitterBuffer jb(4);
    for (auto& p : pkts) jb.push(p);

    std::vector<int32_t> got(FRAMES * (size_t)want.channels, 0);
    std::vector<bool> written(FRAMES, false);
    RtaHeader h{}; std::vector<uint8_t> pay;
    size_t placed = 0;
    while (jb.pop(h, pay)) {
        if (h.flags & RTA_FLAG_SILENCE) continue;
        if (h.timestamp + h.frames > FRAMES) continue;
        std::vector<int32_t> tmp(h.frames * (size_t)h.channels);
        unpack_samples(tmp.data(), pay.data(), tmp.size(), h.bits);
        std::memcpy(got.data() + h.timestamp * (size_t)h.channels,
                    tmp.data(), tmp.size() * sizeof(int32_t));
        for (size_t f = 0; f < h.frames; ++f) written[h.timestamp + f] = true;
        placed += h.frames;
    }

    size_t missing = 0;
    for (bool b : written) if (!b) ++missing;
    size_t diff = 0;
    for (size_t i = 0; i < src.size(); ++i) if (src[i] != got[i]) ++diff;

    std::snprintf(buf, sizeof(buf), "(按时间戳落位 %zu 帧, 缺帧 %zu, 样本不符 %zu / %zu)",
                  placed, missing, diff, src.size());
    check(missing == 0, "无缺帧（时间戳连续）", buf);
    check(diff == 0, "**接收端样本流与发送端逐位相同**", buf);
    check(jb.stats().lost == 0 && jb.stats().duplicate == 0, "无丢包、无重复");

    // ---- 收尾 ----
    cli.stop();
    cli.close();
    if (srv.joinable()) srv.join();
    std::snprintf(buf, sizeof(buf), "(服务端握手 %s%s)", serverOk ? "成功" : "失败",
                  serverOk ? "" : (": " + serverErr).c_str());
    check(serverOk, "服务端协商成功", buf);
    check(negotiated == want, "协商出的格式与请求一致");

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
