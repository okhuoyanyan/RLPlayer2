// test_rtanet.cpp — RTANet 网络音频协议验证（全程本机回环，不涉及音频设备）
// 判据：
//   打包/解包必须逐位可逆（16/24/32 bit）
//   UDP 回环传输后必须是【逐位相同】的样本流
//   抖动缓冲：乱序必须还原顺序；丢包必须补静音且时间轴连续；重复包必须被忽略
#include "net/rtanet.h"
#include <cstdio>
#include <vector>
#include <random>
#include <string>
#include <thread>
#include <chrono>

using namespace rta::net;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// 生成"样本号 -> 值"的确定性数据，便于验证连续性
static std::vector<int32_t> make_samples(size_t frames, int ch, int bits, uint64_t seed) {
    std::vector<int32_t> v(frames * (size_t)ch);
    const int32_t hi = (bits >= 32) ? 0x7FFFFFFF : ((1 << (bits - 1)) - 1);
    const int32_t lo = (bits >= 32) ? (int32_t)0x80000000 : -(1 << (bits - 1));
    std::mt19937 rng((uint32_t)seed);
    std::uniform_int_distribution<int32_t> d(lo, hi);
    for (size_t i = 0; i < v.size(); ++i) v[i] = d(rng);
    return v;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== 1. 打包 / 解包逐位可逆 ===\n");
    {
        for (int bits : {16, 24, 32}) {
            const size_t frames = 128; const int ch = 2;
            auto src = make_samples(frames, ch, bits, 42u + (uint32_t)bits);
            std::vector<uint8_t> pkt = build_packet(7, 1234, 48000, ch, bits, src.data(), frames, 1, RTA_FLAG_NONE);

            RtaHeader h{};
            std::memcpy(&h, pkt.data(), RTANET_HEADER);
            std::vector<int32_t> got(frames * (size_t)ch);
            unpack_samples(got.data(), pkt.data() + RTANET_HEADER, got.size(), bits);

            size_t diff = 0;
            for (size_t i = 0; i < src.size(); ++i) if (src[i] != got[i]) ++diff;
            char buf[220];
            std::snprintf(buf, sizeof(buf), "(%d bit: 包长 %zu 字节, 头 %zu, 不符 %zu; seq=%u ts=%llu rate=%u ch=%u)",
                          bits, pkt.size(), RTANET_HEADER, diff,
                          h.seq, (unsigned long long)h.timestamp, h.rate, h.channels);
            check(diff == 0 && h.magic == RTANET_MAGIC && h.seq == 7 && h.timestamp == 1234 &&
                  h.rate == 48000 && h.channels == ch && h.frames == frames,
                  "打包解包可逆且头字段正确", buf);
        }
    }

    printf("\n=== 2. UDP 回环传输：必须逐位相同 ===\n");
    {
        UdpSocket rx, tx;
        if (!rx.open(0) || !tx.open(0)) { check(false, "套接字创建失败"); }
        else {
            rx.set_recv_timeout_ms(300);
            const uint16_t port = rx.local_port();
            const int bits = 24, ch = 2;
            const size_t framesPer = 128, nPackets = 50;

            // 发送
            size_t sentBytes = 0;
            for (size_t p = 0; p < nPackets; ++p) {
                auto s = make_samples(framesPer, ch, bits, 1000u + (uint32_t)p);
                auto pkt = build_packet((uint32_t)p, (uint64_t)p * framesPer, 48000, ch, bits,
                                        s.data(), framesPer, 99, p == 0 ? RTA_FLAG_START : RTA_FLAG_NONE);
                if (!tx.send_to(pkt.data(), pkt.size(), "127.0.0.1", port)) { check(false, "发送失败"); break; }
                sentBytes += pkt.size();
                std::this_thread::sleep_for(std::chrono::microseconds(300));   // 避免接收缓冲溢出
            }

            // 接收
            std::vector<uint8_t> buf(65536);
            size_t got = 0, byteErrors = 0, missing = 0;
            for (size_t p = 0; p < nPackets; ++p) {
                const int r = rx.recv_from(buf.data(), buf.size());
                if (r <= 0) { ++missing; continue; }
                RtaHeader h{};
                std::memcpy(&h, buf.data(), RTANET_HEADER);
                auto expect = make_samples(framesPer, ch, bits, 1000u + (uint32_t)h.seq);
                std::vector<int32_t> gotv(expect.size());
                unpack_samples(gotv.data(), buf.data() + RTANET_HEADER, gotv.size(), bits);
                for (size_t i = 0; i < expect.size(); ++i) if (expect[i] != gotv[i]) { ++byteErrors; break; }
                ++got;
            }
            char buf2[220];
            std::snprintf(buf2, sizeof(buf2), "(发出 %zu 字节 / %zu 包, 收到 %zu 包, 丢失 %zu, 内容不符 %zu)",
                          sentBytes, nPackets, got, missing, byteErrors);
            check(missing == 0 && byteErrors == 0 && got == nPackets, "回环传输逐位相同", buf2);
        }
    }

    printf("\n=== 3. 抖动缓冲：乱序必须还原 ===\n");
    {
        JitterBuffer jb(4);
        const int bits = 24, ch = 2; const size_t F = 128; const size_t NP = 12;
        // 按打乱的顺序投入
        std::vector<size_t> order;
        for (size_t i = 0; i < NP; ++i) order.push_back(i);
        std::mt19937 rng(7); std::shuffle(order.begin(), order.end(), rng);

        for (size_t k = 0; k < order.size(); ++k) {
            const size_t p = order[k];
            auto s = make_samples(F, ch, bits, 2000u + (uint32_t)p);
            jb.push(build_packet((uint32_t)p, (uint64_t)p * F, 48000, ch, bits, s.data(), F, 5));
        }
        RtaHeader h{}; std::vector<uint8_t> pay; size_t cnt = 0; bool ordered = true;
        std::vector<uint32_t> seqs;
        while (jb.pop(h, pay)) {
            seqs.push_back(h.seq);
            auto s = make_samples(F, ch, bits, 2000u + (uint32_t)h.seq);
            std::vector<int32_t> gotv(s.size());
            unpack_samples(gotv.data(), pay.data(), gotv.size(), bits);
            for (size_t i = 0; i < s.size(); ++i) if (s[i] != gotv[i]) { ordered = false; break; }
            ++cnt;
            jb.observe_format(h);
        }
        bool seqOk = true;
        for (size_t i = 0; i < seqs.size(); ++i) if (seqs[i] != i) seqOk = false;
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(投入顺序被完全打乱, 输出 %zu 包, 序号连续=%s, 丢失 %llu)",
                      cnt, seqOk ? "是" : "否", (unsigned long long)jb.stats().lost);
        check(ordered && seqOk && cnt == NP, "乱序输入被还原为正确顺序", buf);
    }

    printf("\n=== 4. 抖动缓冲：丢包必须补静音且时间轴连续 ===\n");
    {
        JitterBuffer jb(2);
        const int bits = 24, ch = 2; const size_t F = 128;
        // 只投 0,1,3,5（缺 2 和 4）
        for (size_t p : {size_t(0), size_t(1), size_t(3), size_t(5)}) {
            auto s = make_samples(F, ch, bits, 3000u + (uint32_t)p);
            jb.push(build_packet((uint32_t)p, (uint64_t)p * F, 48000, ch, bits, s.data(), F, 6));
        }
        RtaHeader h{}; std::vector<uint8_t> pay;
        std::vector<uint32_t> seqs; std::vector<uint64_t> ts;
        size_t silences = 0;
        // 正确用法：能取就取；取不到时由调用方判定超期再 fill_gap
        while (seqs.size() < 6) {
            if (jb.pop(h, pay)) {
                jb.observe_format(h);
                seqs.push_back(h.seq); ts.push_back(h.timestamp);
            } else if (jb.fill_gap(h, pay)) {
                ++silences;
                seqs.push_back(h.seq); ts.push_back(h.timestamp);
                jb.set_next_ts(h.timestamp + h.frames);
            } else break;
        }
        bool seqContinuous = true;
        for (size_t i = 0; i < seqs.size(); ++i) if (seqs[i] != i) seqContinuous = false;
        char buf[220];
        std::snprintf(buf, sizeof(buf), "(输出 %zu 包, 其中静音 %zu, 序号连续=%s, 丢失计数 %llu)",
                      seqs.size(), silences, seqContinuous ? "是" : "否",
                      (unsigned long long)jb.stats().lost);
        check(seqContinuous && silences == 2 && jb.stats().lost == 2, "缺包被补静音且序号连续", buf);
        check(ts.size() >= 2 && ts[1] == F, "时间戳随包递增（128 帧）");
    }

    printf("\n=== 5. 抖动缓冲：重复包必须被忽略 ===\n");
    {
        JitterBuffer jb(2);
        const int bits = 24, ch = 2; const size_t F = 128;
        auto s0 = make_samples(F, ch, bits, 4000u);
        auto p0 = build_packet(0, 0, 48000, ch, bits, s0.data(), F, 7);
        jb.push(p0); jb.push(p0); jb.push(p0);      // 同一个包投三次
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(接收计数 %llu, 重复计数 %llu)",
                      (unsigned long long)jb.stats().received, (unsigned long long)jb.stats().duplicate);
        check(jb.stats().duplicate == 2, "重复包被识别并忽略", buf);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
