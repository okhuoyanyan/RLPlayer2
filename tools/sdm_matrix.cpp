// tools/sdm_matrix.cpp — SDM 稳定性/裕度矩阵（261030-01 开发任务收尾）
//
// 目的：把「哪个族 × 哪档速率 × 哪个电平」会开始触发限幅，一次性摊开。
//
// 起因：把 DSP 切到严格 IEEE 语义后，`test_modulator_families` 暴露出
//   **DSD5 @DSD64 在 0.62 幅度（−4.2 dBFS）稳态就会偶发限幅**（0.5 s 内 8~17 次），
//   而 −6 dBFS 时是 0 次。5 阶 @ OSR=64 本来余量就小，旧构建被 fast-math 的不同舍入掩盖了。
//
// 判据：稳态窗口（最后 0.5 s）内的限幅率。0 = 干净；>1e-4 = 该组合在这个电平下不可用。
#include "dsp/modulator.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace rta::dsp;

struct Case { double fs; ModulatorFamily fam; int order; int levels; int ahmBits; const char* tag; };

static double run_case(const Case& c, double amp, double seconds, long long* ovOut, long long* clOut) {
    SdmConfig cfg;
    cfg.fs = c.fs; cfg.family = c.fam; cfg.order = c.order;
    cfg.levels = c.levels; cfg.ahmBits = c.ahmBits;
    cfg.cutoffHz = 100000.0; cfg.ditherLsb = 0.0005;
    SdmModulator m(cfg);
    const long long total = (long long)(c.fs * seconds);
    const long long steadyFrom = total - (long long)(c.fs * 0.5);
    const double fin = 1981.0546875;
    long long ovAtSteady = 0;
    double acc = 0.0, sq = 0.0; long long nAcc = 0, nSq = 0;
    for (long long i = 0; i < total; ++i) {
        const double x = amp * std::sin(2.0 * kPi * fin * (double)i / c.fs);
        const double y = m.process(x);
        if (i == steadyFrom) ovAtSteady = m.overload();
        if (i >= steadyFrom) {
            acc += y;
            if (i % 16 == 15) { acc /= 16.0; sq += acc * acc; ++nSq; acc = 0.0; }
        }
        ++nAcc;
    }
    *ovOut = m.overload() - ovAtSteady;
    *clOut = m.clamped();
    return (nSq > 0) ? std::sqrt(sq / (double)nSq) : 0.0;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);

    const double rates[] = { 2822400.0, 5644800.0, 11289600.0, 22579200.0 };
    const char* rateNames[] = { "DSD64", "DSD128", "DSD256", "DSD512" };
    const double amps[] = { 0.50, 0.62, 0.80, 1.00 };
    const char* ampNames[] = { "-6.0 dBFS", "-4.2 dBFS", "-1.9 dBFS", "0.0 dBFS" };

    struct Fam { ModulatorFamily f; int order; int levels; int ahm; const char* tag; };
    const Fam fams[] = {
        { ModulatorFamily::Dsd,       5, 2, 0, "DSD5"    },
        { ModulatorFamily::Dsd,       7, 2, 0, "DSD7"    },
        { ModulatorFamily::Ec,        5, 2, 0, "EC5"     },
        { ModulatorFamily::Ec,        7, 2, 0, "EC7"     },
        { ModulatorFamily::Chebyshev, 5, 2, 0, "Cheb5"   },
        { ModulatorFamily::Chebyshev, 7, 2, 0, "Cheb7"   },
        { ModulatorFamily::Ahm,       7, 256, 8, "AHM8"  },
    };

    std::printf("=== SDM 稳态限幅矩阵（每格：最后 0.5 s 的限幅次数 / 解调 RMS）===\n");
    std::printf("    判据：0 次 = 干净；>140 次（≈1e-4 率）= 该组合在此电平不可用\n\n");
    std::printf("%-7s %-10s", "族", "速率");
    for (auto a : ampNames) std::printf(" %14s", a);
    std::printf("\n");

    std::vector<std::string> bad;
    for (const auto& fam : fams) {
        for (int ri = 0; ri < 4; ++ri) {
            Case c{ rates[ri], fam.f, fam.order, fam.levels, fam.ahm, fam.tag };
            std::printf("%-7s %-10s", fam.tag, rateNames[ri]);
            for (int ai = 0; ai < 4; ++ai) {
                long long ov = 0, cl = 0;
                const double rms = run_case(c, amps[ai], 1.0, &ov, &cl);
                std::printf(" %6lld/%8.3f", ov, rms);
                if (ov > 140) bad.push_back(std::string(fam.tag) + "@" + rateNames[ri] + " " + ampNames[ai]);
            }
            std::printf("\n");
        }
    }

    std::printf("\n=== 不可用组合（稳态限幅率 > 1e-4）===\n");
    if (bad.empty()) std::printf("  无\n");
    else for (const auto& s : bad) std::printf("  %s\n", s.c_str());
    return 0;
}
