// test_dsd_stream_corrupt.cpp -- DSD 流式解码的「损坏必须明确失败」门禁（两轮审查 F16）
//
// 背景（F16 原文）：同一份截断夹具，整曲读拒绝、流式 open=true 且 read=declared=4096、
//   err 为空 —— 因为 feed_next_block() 在短读时 std::memset 补零，然后照常返回。
//   用户听到的是"曲目提前结束 + 静音"，日志里没有任何异常。
//
// 本测试的判据（全部离线，不打开任何音频设备）：
//   1. 头部声明 > 实际文件：probe_dsd_layout() 必须抛异常（open 前置检查）。
//   2. 播放中文件被截短：后续 read() 必须置 error()，且不再假装读到 declared 帧数。
//   3. 合法末块 padding（DSF 按整块补齐）仍必须正常读完 —— 不能把正常文件判成损坏。
//   4. 整曲路径与流式路径对同一份坏文件给出**同向**结论（都失败）。
#include "audio/audio_loader.h"
#include "audio/dsd_reader.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

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

static uint64_t file_size(const std::string& p) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(std::filesystem::u8path(p), ec);
    return ec ? 0 : (uint64_t)n;
}

// 只改大小，不改内容（模拟"播放到一半文件被截短"）
static bool truncate_to(const std::string& p, uint64_t n) {
    const std::filesystem::path wp = std::filesystem::u8path(p);
    HANDLE h = CreateFileW(wp.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)n;
    const BOOL moved = SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    const BOOL ok = moved && SetEndOfFile(h);
    CloseHandle(h);
    return ok != 0;
}

static DsdStream make_stream(uint32_t rate, int ch, uint64_t bits, uint32_t seed) {
    DsdStream s;
    s.rate = rate;
    s.channels = ch;
    s.bits = bits;
    s.container = "mem";
    s.data.assign((size_t)ch, std::vector<uint8_t>((size_t)((bits + 7) / 8)));
    uint32_t x = seed;
    for (int c = 0; c < ch; ++c)
        for (auto& b : s.data[(size_t)c]) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
    return s;
}

// DSF 头部把「样本数」改大（伪造长度），文件本体不变
static bool spoof_dsf_sample_count(const std::string& p, uint64_t newCount) {
    FILE* f = std::fopen(p.c_str(), "r+b");
    if (!f) return false;
    // 头 28 + fmt 块头 12 = 40；fmt 体内 sampleCount 在 +24 ⟹ 文件偏移 40+24 = 64
    const long off = 64;
    if (std::fseek(f, off, SEEK_SET) != 0) { std::fclose(f); return false; }
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)((newCount >> (8 * i)) & 0xFF);
    const size_t w = std::fwrite(b, 1, 8, f);
    std::fclose(f);
    return w == 8;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 夹具：2 声道 / DSD64 / 每声道 8 块 x 4096 字节（合法的整块结尾）
    const uint64_t kBlock = 4096;
    const uint64_t kBlocks = 8;
    const uint64_t bits = kBlock * kBlocks * 8ull;
    const std::string pDsf = temp_path("rta_f16_ok.dsf");
    const std::string pDff = temp_path("rta_f16_ok.dff");
    {
        DsdStream s = make_stream(2822400u, 2, bits, 0x1234u);
        write_dsf(pDsf, s, (uint32_t)kBlock);
        write_dff(pDff, s);
    }

    printf("=== 1. 合法文件：流式必须完整读完，且 error() 为假 ===\n");
    {
        DsdPcmStream st;
        std::string err;
        const bool ok = st.open(pDsf, dsd_pcm_settings(), &err);
        check(ok, "合法 DSF 能流式打开", err);
        size_t got = 0;
        while (got < st.totalFrames()) {
            AudioBuffer c;
            const size_t n = st.read(got, 4096, c);
            if (n == 0) break;
            got += n;
        }
        check(got == st.totalFrames(), "读到全部帧", std::to_string(got) + "/" + std::to_string(st.totalFrames()));
        std::string why;
        check(!st.error(&why), "合法文件 error() 为假", why);
    }

    printf("=== 2. 头部伪造长度（声明 4 倍数据，文件不变）必须被拒 ===\n");
    {
        const std::string pBad = temp_path("rta_f16_spoof.dsf");
        std::error_code ec;
        std::filesystem::copy_file(std::filesystem::u8path(pDsf), std::filesystem::u8path(pBad),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        const bool wrote = spoof_dsf_sample_count(pBad, bits * 4);
        check(wrote, "夹具：样本数字段改写成功");
        std::string msg;
        bool threw = false;
        try { (void)probe_dsd_layout(pBad); } catch (const std::exception& e) { threw = true; msg = e.what(); }
        check(threw, "probe_dsd_layout 拒绝「声明 > 文件」", msg);
        // 整曲路径也必须拒绝（两条路同向）
        bool threw2 = false;
        std::string msg2;
        try { (void)read_dsf(pBad); } catch (const std::exception& e) { threw2 = true; msg2 = e.what(); }
        check(threw2, "整曲 read_dsf 同样拒绝", msg2);
        // 流式 open 必须失败（而不是 open=true 然后补零）
        DsdPcmStream st;
        std::string err;
        const bool opened = st.open(pBad, dsd_pcm_settings(), &err);
        check(!opened, "流式 open 明确失败（不静默补零）", err);
        DeleteFileA(pBad.c_str());
    }

    printf("=== 3. 播放中文件被截短：后续 read() 必须置 error() ===\n");
    {
        const std::string pCut = temp_path("rta_f16_cut.dsf");
        std::error_code ec;
        std::filesystem::copy_file(std::filesystem::u8path(pDsf), std::filesystem::u8path(pCut),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        DsdPcmStream st;
        std::string err;
        const bool opened = st.open(pCut, dsd_pcm_settings(), &err);
        check(opened, "截短前能正常打开", err);
        AudioBuffer c0;
        const size_t first = st.read(0, 2048, c0);
        check(first == 2048, "截短前能正常读", std::to_string(first));
        const uint64_t sz = file_size(pCut);
        const bool cut = truncate_to(pCut, sz / 3);
        check(cut, "夹具：文件被截短到 1/3", std::to_string(sz) + " -> " + std::to_string(sz / 3));
        // 从被截掉的位置往后读：必须失败，不能补零后假装成功
        size_t got = 0, zeroFilled = 0;
        for (int k = 0; k < 64 && got < st.totalFrames(); ++k) {
            AudioBuffer c;
            const size_t n = st.read(got, 4096, c);
            if (n == 0) break;
            got += n;
        }
        (void)zeroFilled;
        std::string why;
        const bool bad = st.error(&why);
        check(bad, "截短后 read() 置 error()", why);
        check(!why.empty(), "错误信息非空（能说明原因）", why);
        check(got < st.totalFrames(), "截短后不再声称读到全部帧",
              std::to_string(got) + "/" + std::to_string(st.totalFrames()));
        DeleteFileA(pCut.c_str());
    }

    printf("=== 4. DFF 同一条判据（声道交错 + 截断） ===\n");
    {
        DsdPcmStream st;
        std::string err;
        check(st.open(pDff, dsd_pcm_settings(), &err), "合法 DFF 能流式打开", err);
        size_t got = 0;
        while (got < st.totalFrames()) {
            AudioBuffer c;
            const size_t n = st.read(got, 4096, c);
            if (n == 0) break;
            got += n;
        }
        check(got == st.totalFrames() && !st.error(), "合法 DFF 读完且无错",
              std::to_string(got) + "/" + std::to_string(st.totalFrames()));
        const std::string pCut = temp_path("rta_f16_cut.dff");
        std::error_code ec;
        std::filesystem::copy_file(std::filesystem::u8path(pDff), std::filesystem::u8path(pCut),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        // 直接把文件截到只剩头部 + 一小段数据（头部声明的长度不变）
        truncate_to(pCut, file_size(pCut) / 4);
        std::string msg;
        bool threw = false;
        try { (void)probe_dsd_layout(pCut); } catch (const std::exception& e) { threw = true; msg = e.what(); }
        check(threw, "截短的 DFF 在 probe 阶段就被拒", msg);
        DsdPcmStream st2;
        std::string err2;
        check(!st2.open(pCut, dsd_pcm_settings(), &err2), "截短的 DFF 流式 open 失败", err2);
        DeleteFileA(pCut.c_str());
    }

    DeleteFileA(pDsf.c_str());
    DeleteFileA(pDff.c_str());
    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}