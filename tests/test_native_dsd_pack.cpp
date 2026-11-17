// test_native_dsd_pack.cpp — 2026-11-15
// 验证 PCM→原生 DSD 的最后一跳：DoP 24-bit 帧的 16 位载荷 → ASIO MSB1 的 2 个字节。
// 判据不是"看起来像"，而是穷举 65536 个载荷：高/低字节必须能无损还原，
// 且标记位（0x05/0xFA）绝不允许漏进音频字节。
#include "dsp/dop.h"
#include <cstdint>
#include <cstdio>

static int fails = 0;
static void ck(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++fails; }
}
int main() {
    using rta::dsp::dop_word_to_dsd_bytes;
    // 1) 标记位隔离：同样载荷，两个合法标记必须给出同样字节。
    for (uint32_t payload = 0; payload <= 0xFFFF; ++payload) {
        uint8_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;
        dop_word_to_dsd_bytes(0x05u << 16 | payload, a0, a1);
        dop_word_to_dsd_bytes(0xFAu << 16 | payload, b0, b1);
        if (a0 != b0 || a1 != b1 || a0 != ((payload >> 8) & 0xFF) || a1 != (payload & 0xFF)) {
            std::printf("FAIL: payload=%04X marker leak or byte order\n", payload);
            ++fails;
            break;
        }
    }
    // 2) 关键位型（MSB-first 的语义）。
    struct Case { uint32_t word; uint8_t hi, lo; const char* why; };
    const Case cases[] = {
        {0x05FFFFu, 0xFF, 0xFF, "16 个 1"},
        {0xFA0000u, 0x00, 0x00, "16 个 0"},
        {0x05AAAAu, 0xAA, 0xAA, "1010…"},
        {0xFA8000u, 0x80, 0x00, "只有最早一位"},
        {0x05FF00u, 0xFF, 0x00, "前 8 位"},
        {0xFA00FFu, 0x00, 0xFF, "后 8 位"},
        {0x05A55Au, 0xA5, 0x5A, "交叉位型"},
    };
    for (const auto& c : cases) {
        uint8_t hi = 0, lo = 0;
        dop_word_to_dsd_bytes(c.word, hi, lo);
        ck(hi == c.hi && lo == c.lo, c.why);
    }
    // 3) 与生产 dop_pack 对接：16 个 DSD 样本打包后必须还原成同一字节序列。
    const double bits[16] = {1,0,1,0, 1,0,1,0, 0,1,0,1, 0,1,0,1};
    const double* plane[1] = {bits};
    std::vector<int32_t> packed;
    rta::dsp::dop_pack(plane, 1, 16, packed);
    ck(packed.size() == 1, "dop_pack 产生 1 帧");
    if (!packed.empty()) {
        uint8_t hi = 0, lo = 0;
        dop_word_to_dsd_bytes((uint32_t)packed[0], hi, lo);
        ck(hi == 0xAA && lo == 0x55, "dop_pack → 原生字节逐位一致");
    }
    if (fails == 0) std::printf("native_dsd_pack PASS (65536 payloads + 7 bit patterns + pack bridge)\n");
    return fails ? 1 : 0;
}
