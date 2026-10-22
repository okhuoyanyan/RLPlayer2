// dopinfo.cpp — 校验一个 WAV 是否为合法 DoP，并还原其 1-bit 流做统计
// 用途：验证 DSD 输出链路的端到端正确性（纯离线，不打开音频设备）
#include "audio/wav.h"
#include "dsp/dop.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>
#include <string>

using namespace rta;
using namespace rta::audio;
using rta::dsp::DOP_MARKER_A;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { printf("用法: dopinfo <file.wav>\n"); return 2; }

    audio::AudioBuffer a;
    try { a = read_wav(argv[1]); }
    catch (const std::exception& e) { printf("读取失败: %s\n", e.what()); return 2; }
    printf("文件: %s\n  %u Hz %d ch %d bit %zu 帧\n", argv[1], a.rate, a.channels, a.bits, a.frames());
    if (a.bits != 24) { printf("!! DoP 要求 24-bit 容器\n"); return 1; }

    const size_t carrier = a.frames();
    std::vector<std::vector<double>> bits;
    const bool ok = rta::dsp::dop_unpack(a.raw.data(), a.channels, carrier, bits);
    char b[200];
    std::snprintf(b, sizeof(b), "(%zu 个 24-bit 样本 -> %zu 个 DSD 位/声道)",
                  carrier, carrier * 16);
    printf("  %s\n", b);
    printf("  [%s] DoP 标记序列合法（逐样本交替 0x05/0xFA）\n", ok ? "PASS" : "FAIL");
    if (!ok) return 1;

    // 每个声道的统计：1 的比例应接近 0.5（DSD 的直流平衡）
    for (int c = 0; c < a.channels; ++c) {
        size_t ones = 0;
        for (double v : bits[(size_t)c]) if (v > 0.0) ++ones;
        const double ratio = (double)ones / (double)bits[(size_t)c].size();
        // 16 位滑窗平均 -> 等效 PCM，看峰值（DSD 满幅时约 ±1）
        double pk = 0.0;
        const size_t step = 64;
        for (size_t i = 0; i + step <= bits[(size_t)c].size(); i += step) {
            double s = 0.0;
            for (size_t k = 0; k < step; ++k) s += bits[(size_t)c][i + k];
            pk = std::max(pk, std::fabs(s / (double)step));
        }
        std::snprintf(b, sizeof(b), "(声道 %d: 1 的比例 %.4f, 滑窗峰值 %.4f)", c, ratio, pk);
        printf("  %s\n", b);
    }
    // ---- 抽取回 PCM 并测基波：证明 1-bit 流真的承载了信号 ----
    // 简单平均抽取（一阶 CIC），抽取倍数选 64 -> DSD64 得到 44100 Hz
    {
        const size_t dec = 64;
        const double outRate = (double)a.rate * 16.0 / (double)dec;   // 承载率*16 = DSD 率
        printf("\n抽取: %zu 倍 -> %.0f Hz PCM\n", dec, outRate);
        for (int c = 0; c < a.channels; ++c) {
            const auto& b = bits[(size_t)c];
            const size_t n = b.size() / dec;
            std::vector<double> pcm(n);
            for (size_t i = 0; i < n; ++i) {
                double s = 0.0;
                for (size_t k = 0; k < dec; ++k) s += b[i * dec + k];
                pcm[i] = s / (double)dec;
            }
            // 测 1 kHz 单频点幅度（源文件是 1 kHz 正弦）
            const double w = 2.0 * 3.14159265358979 * 1000.0 / outRate;
            double re = 0.0, im = 0.0;
            const size_t skip = 2000;
            for (size_t i = skip; i < n; ++i) { re += pcm[i] * std::cos(w * (double)i); im -= pcm[i] * std::sin(w * (double)i); }
            const double amp = 2.0 * std::sqrt(re * re + im * im) / (double)(n - skip);
            const double db = 20.0 * std::log10(amp + 1e-300);
            char lb[200];
            std::snprintf(lb, sizeof(lb), "(声道 %d: 1 kHz 分量 %.2f dBFS，源为 -60 dBFS)", c, db);
            printf("  %s\n", lb);
        }
        printf("  说明：若 1 kHz 分量在 -60 dBFS 附近，说明 DSD 流确实承载了原始信号。\n");
    }

    printf("\n结论: %s\n", ok ? "合法的 DoP 流" : "非法");
    return ok ? 0 : 1;
}
