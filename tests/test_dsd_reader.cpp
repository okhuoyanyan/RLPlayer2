// test_dsd_reader.cpp — DSF / DFF 原生读取与写出（ROADMAP P0-1）
//
// 判据（照 ROADMAP 写，不靠"看起来对"）：
//   1. 写出的 DSF/DFF 被读回后，**逐位**等于写入前的 1-bit 流（不是"长度对"）。
//   2. 声道数 / 采样率 / 位数 / 位序 / 容器类型全部如实报告。
//   3. 文件布局独立验证：按 DSF 规范手工计算偏移，确认「每声道 blockSize 块交错、
//      LSB-first」——不能用读函数自己验证自己。
//   4. 坏文件必须抛异常并说清原因（截断 / 魔数错 / DST 压缩）。
#include "audio/dsd_reader.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace rta::audio;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static std::string temp_path(const char* name) {
    char dir[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + name;
}

static uint64_t rdl64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
static uint32_t rdl32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
// 独立实现的位翻转（刻意不调用被测头文件里的版本）
static uint8_t rev8(uint8_t b) {
    uint8_t r = 0;
    for (int k = 0; k < 8; ++k) if (b & (1u << k)) r = (uint8_t)(r | (1u << (7 - k)));
    return r;
}

// 确定性素材：随机字节 + 一批"位序敏感"的定值（位翻转后必然不同）
static DsdStream make_stream(uint32_t rate, int channels, uint64_t bits, uint32_t seed) {
    DsdStream s;
    s.rate = rate;
    s.channels = channels;
    s.bits = bits;
    s.container = "mem";
    s.data.assign((size_t)channels, std::vector<uint8_t>((size_t)((bits + 7) / 8)));
    std::mt19937 rng(seed);
    const uint8_t pat[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
                           0x0F, 0xF0, 0x33, 0xCC, 0xA5, 0x5A, 0x00, 0xFF};
    for (int c = 0; c < channels; ++c) {
        auto& b = s.data[(size_t)c];
        for (size_t i = 0; i < b.size(); ++i) {
            b[i] = (i < sizeof(pat)) ? (uint8_t)(pat[i] ^ (uint8_t)(c * 0x37))
                                     : (uint8_t)(rng() & 0xFF);
        }
    }
    return s;
}

// DSF 布局独立验证：手工解析头部，按规范算偏移，抽查字节（含末块与块边界）
static bool verify_dsf_layout(const std::vector<uint8_t>& f, const DsdStream& s, uint32_t blockBytes) {
    if (f.size() < 28 || std::memcmp(f.data(), "DSD ", 4)) return false;
    const size_t fmtOff = (size_t)rdl64(f.data() + 4);
    if (std::memcmp(f.data() + fmtOff, "fmt ", 4)) return false;
    const uint8_t* fmt = f.data() + fmtOff + 12;
    const uint32_t ch = rdl32(fmt + 12);
    const uint32_t rate = rdl32(fmt + 16);
    const uint32_t bps = rdl32(fmt + 20);
    const uint64_t sampleCnt = rdl64(fmt + 24);
    const uint32_t blk = rdl32(fmt + 32);
    if (ch != (uint32_t)s.channels || rate != s.rate || bps != 1 || sampleCnt != s.bits) return false;
    if (blk != blockBytes) return false;

    const size_t dataOff = fmtOff + (size_t)rdl64(f.data() + fmtOff + 4);
    if (std::memcmp(f.data() + dataOff, "data", 4)) return false;
    const size_t payload = dataOff + 12;
    const uint64_t needBytes = (sampleCnt + 7) / 8;

    // 抽查：每个声道、块内首/中/尾字节 + 末块
    const size_t stride = 997;                       // 质数，避免与块长同步
    for (uint32_t c = 0; c < ch; ++c) {
        for (uint64_t i = 0; i < needBytes; i += stride) {
            const uint64_t b = i / blk, o = i % blk;
            const size_t off = payload + (size_t)((b * ch + c) * blk + o);
            if (off >= f.size()) return false;
            if (f[off] != rev8(s.data[(size_t)c][(size_t)i])) {
                printf("    (布局不符: ch=%u i=%llu 文件=%02X 期望=%02X)\n",
                       c, (unsigned long long)i, f[off], rev8(s.data[(size_t)c][(size_t)i]));
                return false;
            }
        }
        // 末块最后一个有效字节
        const uint64_t last = needBytes - 1;
        const uint64_t b = last / blk, o = last % blk;
        const size_t off = payload + (size_t)((b * ch + c) * blk + o);
        if (off >= f.size() || f[off] != rev8(s.data[(size_t)c][(size_t)last])) return false;
    }
    return true;
}

static bool same_bits(const DsdStream& a, const DsdStream& b, uint64_t bits, std::string* why) {
    if (a.channels != b.channels) { *why = "声道数不同"; return false; }
    if (a.rate != b.rate) { *why = "采样率不同"; return false; }
    for (int c = 0; c < a.channels; ++c) {
        for (uint64_t n = 0; n < bits; ++n) {
            if (a.bit((size_t)c, n) != b.bit((size_t)c, n)) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "声道 %d 第 %llu 位不同", c, (unsigned long long)n);
                *why = buf;
                return false;
            }
        }
    }
    return true;
}

static std::vector<uint8_t> slurp(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b((size_t)n);
    const size_t got = std::fread(b.data(), 1, b.size(), f);
    std::fclose(f);
    b.resize(got);
    return b;
}

static bool throws(const std::string& path, const char* kind, std::string* msg) {
    try {
        if (std::strcmp(kind, "dsf") == 0) read_dsf(path);
        else read_dff(path);
    } catch (const std::exception& e) {
        *msg = e.what();
        return true;
    }
    return false;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    const std::string pDsf = temp_path("rta_dsd_reader_test.dsf");
    const std::string pDff = temp_path("rta_dsd_reader_test.dff");

    printf("=== 1. DSF：3 声道 / 块长 4096 / 末块不满（9192 字节 = 2 块 + 1000）===\n");
    {
        const uint64_t bits = 8ull * (4096ull * 2 + 1000);
        DsdStream a = make_stream(2822400u, 3, bits, 12345);
        write_dsf(pDsf, a, 4096);
        DsdStream b = read_dsf(pDsf);
        std::string why;
        check(b.container == "dsf", "容器识别为 dsf", b.container);
        check(b.rate == 2822400u, "采样率 2822400", std::to_string(b.rate));
        check(b.channels == 3, "声道数 3", std::to_string(b.channels));
        check(b.bits == bits, "样本位数一致", std::to_string(b.bits));
        check(b.fileOrder == DsdBitOrder::LsbFirst, "文件位序报告 LSB-first", dsd_bit_order_name(b.fileOrder));
        check(b.channelIds == a.channelIds || b.channelIds.size() == 3, "声道 ID 数量 3");
        check(same_bits(a, b, bits, &why), "读回的 1-bit 流逐位一致", why);
        const auto raw = slurp(pDsf);
        check(!raw.empty() && verify_dsf_layout(raw, a, 4096),
              "文件字节按「块交错 + LSB-first」独立校验通过",
              "文件 " + std::to_string(raw.size()) + " 字节");
        // 只读文件头：DirectSDM 判定用，不载入数据也必须给出同样的元数据
        const DsdInfo pi = probe_dsd(pDsf);
        check(pi.rate == 2822400u && pi.channels == 3 && pi.bits == bits && pi.container == "dsf",
              "probe_dsd 只读头结果与完整读取一致",
              std::to_string(pi.rate) + " Hz / " + std::to_string(pi.channels) + " ch / " +
              std::to_string(pi.bits) + " 位");
    }

    printf("\n=== 2. DSF：2 声道 / 位数不是 8 的整数倍（末尾 3 位）===\n");
    {
        const uint64_t bits = 8ull * 5000 + 3;
        DsdStream a = make_stream(5644800u, 2, bits, 777);
        write_dsf(pDsf, a, 4096);
        DsdStream b = read_dsf(pDsf);
        std::string why;
        check(b.rate == 5644800u, "采样率 5644800", std::to_string(b.rate));
        check(b.bits == bits, "位数保留（含非整字节尾巴）", std::to_string(b.bits));
        check(same_bits(a, b, bits - 3, &why), "有效位逐位一致（末 3 位不参与）", why);
        bool tailOk = true;
        for (uint64_t n = bits - 3; n < bits; ++n)
            if (a.bit(0, n) != b.bit(0, n)) tailOk = false;
        check(tailOk, "末 3 位也一致（打包时未丢位）");
    }

    printf("\n=== 3. DFF：2 声道 / 大端 / 声道独立存放 / CHNL 声道 ID ===\n");
    {
        const uint64_t bits = 8ull * 8192;
        DsdStream a = make_stream(2822400u, 2, bits, 424242);
        a.channelIds = { "SLFT", "SRGT" };
        write_dff(pDff, a);
        DsdStream b = read_dff(pDff);
        std::string why;
        check(b.container == "dff", "容器识别为 dff", b.container);
        check(b.rate == 2822400u, "采样率 2822400", std::to_string(b.rate));
        check(b.channels == 2, "声道数 2", std::to_string(b.channels));
        check(b.bits == bits, "位数一致", std::to_string(b.bits));
        check(b.fileOrder == DsdBitOrder::MsbFirst, "文件位序报告 MSB-first", dsd_bit_order_name(b.fileOrder));
        check(b.channelIds.size() == 2 && b.channelIds[0] == "SLFT" && b.channelIds[1] == "SRGT",
              "CHNL 声道 ID 读回", b.channelIds.size() == 2 ? b.channelIds[0] + "/" + b.channelIds[1] : "?");
        check(same_bits(a, b, bits, &why), "读回的 1-bit 流逐位一致", why);

        // 独立核对：按 DSDIFF 块链表走到 "DSD " 块（不能直接找 "DSD "，
        // 因为 FRM8 的 form type 与 CMPR 的压缩类型也都是 "DSD "），按声道连续排列检查
        const auto raw = slurp(pDff);
        bool layoutOk = false;
        size_t off = 16;
        while (off + 12 <= raw.size()) {
            uint64_t sz = 0;
            for (int i = 0; i < 8; ++i) sz = (sz << 8) | raw[off + 4 + (size_t)i];   // 大端
            if (off + 12 + (size_t)sz > raw.size()) break;
            if (std::memcmp(raw.data() + off, "DSD ", 4) == 0) {
                layoutOk = (sz == bits / 8 * 2);
                const uint8_t* body = raw.data() + off + 12;
                for (size_t i = 0; layoutOk && i < bits / 8; i += 331)
                    if (body[i] != a.data[0][i] || body[bits / 8 + i] != a.data[1][i]) layoutOk = false;
                break;
            }
            off += 12 + (size_t)sz + (size_t)(sz & 1);
        }
        check(layoutOk, "文件内 DSD 块「声道 0 全部 → 声道 1 全部」独立校验");
        const DsdInfo pi = probe_dsd(pDff);
        check(pi.rate == 2822400u && pi.channels == 2 && pi.bits == bits && pi.container == "dff",
              "probe_dsd 只读 DFF 头结果与完整读取一致",
              std::to_string(pi.rate) + " Hz / " + std::to_string(pi.channels) + " ch / " +
              std::to_string(pi.bits) + " 位");
    }

    printf("\n=== 4. 坏文件必须明确报错 ===\n");
    {
        const auto raw = slurp(pDsf);
        // 截断到一半
        const std::string trunc = temp_path("rta_dsd_trunc.dsf");
        { FILE* f = std::fopen(trunc.c_str(), "wb"); const size_t half = raw.size() / 2;
          if (f) { std::fwrite(raw.data(), 1, half, f); std::fclose(f); } }
        std::string msg;
        check(throws(trunc, "dsf", &msg), "截断的 DSF 抛异常", msg);

        // 魔数错
        const std::string bad = temp_path("rta_dsd_badmagic.dsf");
        { auto m = raw; m[0] = 'X'; FILE* f = std::fopen(bad.c_str(), "wb");
          if (f) { std::fwrite(m.data(), 1, m.size(), f); std::fclose(f); } }
        check(throws(bad, "dsf", &msg), "魔数错误的 DSF 抛异常", msg);

        // DST 压缩的 DFF：最小骨架 FRM8 / DSD  / DST 块
        const std::string dst = temp_path("rta_dsd_dst.dff");
        {
            std::vector<uint8_t> o = { 'F', 'R', 'M', '8', 0,0,0,0,0,0,0,32, 'D', 'S', 'D', ' ' };
            auto chunk = [&](const char* id, const std::vector<uint8_t>& body) {
                for (int i = 0; i < 4; ++i) o.push_back((uint8_t)id[i]);
                for (int i = 7; i >= 0; --i) o.push_back((uint8_t)((body.size() >> (8 * i)) & 0xFF));
                o.insert(o.end(), body.begin(), body.end());
                if (body.size() & 1) o.push_back(0);
            };
            chunk("DST ", std::vector<uint8_t>(16, 0));
            FILE* f = std::fopen(dst.c_str(), "wb");
            if (f) { std::fwrite(o.data(), 1, o.size(), f); std::fclose(f); }
        }
        check(throws(dst, "dff", &msg), "DST 压缩的 DFF 抛异常", msg);
        check(msg.find("DST") != std::string::npos, "错误信息点明 DST", msg);

        // 缺 DSD 块的 DFF
        const std::string nodata = temp_path("rta_dsd_nodata.dff");
        {
            std::vector<uint8_t> o = { 'F', 'R', 'M', '8', 0,0,0,0,0,0,0,4, 'D', 'S', 'D', ' ' };
            FILE* f = std::fopen(nodata.c_str(), "wb");
            if (f) { std::fwrite(o.data(), 1, o.size(), f); std::fclose(f); }
        }
        check(throws(nodata, "dff", &msg), "缺数据块的 DFF 抛异常", msg);

        DeleteFileA(trunc.c_str());
        DeleteFileA(bad.c_str());
        DeleteFileA(dst.c_str());
        DeleteFileA(nodata.c_str());
    }

    printf("\n=== 5. 按扩展名分派 read_dsd ===\n");
    {
        DsdStream s = read_dsd(pDsf);
        check(s.container == "dsf" && s.channels == 2, "read_dsd(.dsf) 走 DSF", s.container);
        DsdStream d = read_dsd(pDff);
        check(d.container == "dff" && d.channels == 2, "read_dsd(.dff) 走 DFF", d.container);
        bool threw = false;
        try { read_dsd("no_such_file.wav"); } catch (const std::exception&) { threw = true; }
        check(threw, "非 DSD 扩展名拒绝处理（调用方回退 ffmpeg）");
    }

    DeleteFileA(pDsf.c_str());
    DeleteFileA(pDff.c_str());
    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
