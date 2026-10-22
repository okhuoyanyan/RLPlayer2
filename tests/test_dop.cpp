// test_dop.cpp — DoP v1.1 打包/解包验证（纯离线）
#include "dsp/dop.h"
#include <cstdio>
#include <vector>
#include <random>
#include <string>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== 1. 速率换算 ===\n");
    {
        char b[160];
        for (int m : {64, 128, 256, 512}) {
            std::snprintf(b, sizeof(b), "(DSD%d: %.4f MHz -> 承载 PCM %u Hz)",
                          m, dsd_rate(m) / 1e6, dop_carrier_rate(m));
            printf("  %s\n", b);
        }
        check(dop_carrier_rate(64) == 176400 && dop_carrier_rate(128) == 352800 &&
              dop_carrier_rate(256) == 705600, "DSD64/128/256 承载率正确");
    }

    printf("\n=== 2. 打包 -> 解包 逐位往返（多声道）===\n");
    {
        std::mt19937 rng(20261019u);
        for (int ch : {1, 2, 6}) {
            const size_t frames = 2822400 / 100;      // 10 ms @ DSD64
            std::vector<std::vector<double>> in((size_t)ch, std::vector<double>(frames));
            for (int c = 0; c < ch; ++c)
                for (size_t i = 0; i < frames; ++i)
                    in[(size_t)c][i] = (rng() & 1) ? 1.0 : -1.0;

            std::vector<const double*> ip;
            for (auto& v : in) ip.push_back(v.data());

            std::vector<int32_t> packed;
            dop_pack(ip.data(), ch, frames, packed);

            const size_t carrier = frames / 16;
            std::vector<std::vector<double>> back;
            const bool okFlag = dop_unpack(packed.data(), ch, carrier, back);

            size_t diff = 0;
            for (int c = 0; c < ch; ++c)
                for (size_t i = 0; i < carrier * 16; ++i)
                    if (back[(size_t)c][i] != in[(size_t)c][i]) ++diff;

            char b[200];
            std::snprintf(b, sizeof(b), "(%d 声道, %zu 位 -> %zu 个 24-bit 样本, 不符 %zu)",
                          ch, frames, packed.size(), diff);
            check(okFlag && diff == 0, "逐位往返一致", b);
        }
    }

    printf("\n=== 3. 标记序列必须交替 0x05 / 0xFA ===\n");
    {
        const size_t frames = 1600;
        std::vector<double> mono(frames, 1.0);
        const double* ip[1] = { mono.data() };
        std::vector<int32_t> packed;
        dop_pack(ip, 1, frames, packed);
        bool alt = true;
        for (size_t n = 0; n < packed.size(); ++n) {
            const uint32_t mk = ((uint32_t)packed[n] >> 16) & 0xFFu;
            const uint32_t want = (n % 2 == 0) ? DOP_MARKER_A : DOP_MARKER_B;
            if (mk != want) { alt = false; break; }
        }
        check(alt, "标记逐样本交替", "(0x05 / 0xFA)");

        // 故意破坏一个标记，解包必须报错
        auto bad = packed;
        bad[3] = (int32_t)((0x11u << 16) | ((uint32_t)bad[3] & 0xFFFFu));
        std::vector<std::vector<double>> tmp;
        check(!dop_unpack(bad.data(), 1, bad.size(), tmp), "非法标记被拒绝");
    }

    printf("\n=== 4. 全 1 与全 0 的位图案 ===\n");
    {
        const size_t frames = 32;
        std::vector<double> ones(frames, 1.0), neg(frames, -1.0);
        const double* p1[1] = { ones.data() };
        const double* p0[1] = { neg.data() };
        std::vector<int32_t> a, b;
        dop_pack(p1, 1, frames, a);
        dop_pack(p0, 1, frames, b);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(全 +1 -> 0x%06X, 全 -1 -> 0x%06X)",
                      (unsigned)((uint32_t)a[0] & 0xFFFFFFu), (unsigned)((uint32_t)b[0] & 0xFFFFFFu));
        check(((uint32_t)a[0] & 0xFFFFu) == 0xFFFFu && ((uint32_t)b[0] & 0xFFFFu) == 0x0000u,
              "位图案正确（+1 -> 全 1，-1 -> 全 0）", buf);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
