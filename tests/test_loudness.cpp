// test_loudness.cpp — 等响度补偿验证
#include "dsp/loudness.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// 用正弦测某个频点的增益（dB）
static double gain_at(Loudness& L, double fs, double hz) {
    const size_t N = 48000;
    std::vector<double> x(N);
    for (size_t i = 0; i < N; ++i) x[i] = std::sin(2.0 * kPi * hz * (double)i / fs);
    L.reset();
    L.process(x.data(), N);
    double pk = 0.0;
    for (size_t i = N / 2; i < N; ++i) pk = std::max(pk, std::fabs(x[i]));
    return 20.0 * std::log10(pk + 1e-300);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const double fs = 48000.0;

    printf("=== 1. 边界行为（手册 P7/P8）===\n");
    {
        LoudnessConfig c;   // 下界 -30，上界 0，低音 6 dB，高音 3 dB
        Loudness L(c); L.init(fs);

        L.set_volume_db(0.0);
        check(std::fabs(L.bass_db()) < 1e-9 && std::fabs(L.treble_db()) < 1e-9,
              "音量 0 dB（上界）-> 无提升");

        L.set_volume_db(-30.0);
        check(std::fabs(L.bass_db() - 6.0) < 1e-9 && std::fabs(L.treble_db() - 3.0) < 1e-9,
              "音量 -30 dB（下界）-> 满提升");

        L.set_volume_db(-45.0);
        check(std::fabs(L.bass_db() - 6.0) < 1e-9, "低于下界仍为满提升（截断）");

        L.set_volume_db(-15.0);
        char b[160];
        std::snprintf(b, sizeof(b), "(中点 -15 dB -> 低音 +%.2f dB, 高音 +%.2f dB)",
                      L.bass_db(), L.treble_db());
        check(std::fabs(L.bass_db() - 3.0) < 1e-9, "中点线性插值（低音一半）", b);
    }

    printf("\n=== 2. 实测频响（音量 0 dB 应完全旁路）===\n");
    {
        LoudnessConfig c;
        Loudness L(c); L.init(fs);
        L.set_volume_db(0.0);
        const double g100 = gain_at(L, fs, 100.0);
        const double g6k  = gain_at(L, fs, 6000.0);
        char b[180];
        std::snprintf(b, sizeof(b), "(100 Hz %.4f dB, 6 kHz %.4f dB)", g100, g6k);
        check(std::fabs(g100) < 0.01 && std::fabs(g6k) < 0.01, "0 dB 时频响平坦", b);
    }

    printf("\n=== 3. 实测频响（音量 -30 dB 应有提升）===\n");
    {
        LoudnessConfig c;
        Loudness L(c); L.init(fs);
        L.set_volume_db(-30.0);
        // 低频落在架顶（远低于角频率 100 Hz）
        const double g20  = gain_at(L, fs, 20.0);
        const double g1k  = gain_at(L, fs, 1000.0);
        // 高音测 12 kHz 而不是 20 kHz：48 kHz 采样下 20 kHz 每周期只有 2.4 个点，
        // 「取样本峰值」的估计方法本身就有数 dB 误差，测不出真实增益。
        const double g12k = gain_at(L, fs, 12000.0);
        char b[240];
        std::snprintf(b, sizeof(b), "(20 Hz %+.2f dB, 1 kHz %+.2f dB, 12 kHz %+.2f dB)",
                      g20, g1k, g12k);
        printf("  %s\n", b);
        check(g20 > 5.0 && g20 < 7.0, "20 Hz 提升接近设定 +6 dB", "");
        check(std::fabs(g1k) < 0.5, "中频基本不变");
        check(g12k > 1.5, "12 kHz 有提升（高架生效）");
    }

    printf("\n=== 4. 提升量随音量单调 ===\n");
    {
        LoudnessConfig c;
        Loudness L(c); L.init(fs);
        double prev = -1e9; bool mono = true;   // 提升量随音量下降而【增大】，故从极小值起算
        printf("  音量     低音提升\n");
        for (double v : {0.0, -5.0, -10.0, -15.0, -20.0, -25.0, -30.0, -35.0}) {
            L.set_volume_db(v);
            const double bd = L.bass_db();
            printf("  %6.1f   %+.2f dB\n", v, bd);
            if (bd < prev - 1e-9) mono = false;
            prev = bd;
        }
        check(mono, "音量越低提升越大（单调）");
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
