// tests/test_sdm_headroom.cpp — 1-bit 调制器的「前级余量目标」安全性验收（261030-01）
//
// 判据（铁律 3）：对每一个 (族, 速率) 组合，用 sdm_target_peak(fs) 这个**产品实际使用的目标峰值**
//   去驱动调制器 1 秒，最后 0.5 秒的限幅次数必须为 0。
//   证明的是该单音、该时长与这些配置下的经验余量，不能外推为所有输入波形均稳定。
//   审计扩展全带白噪已发现同峰值仍可能限幅，见 modulator.h 的工作点边界说明。
//
// 负向对照：把电平抬到目标值 ×1.5，至少要有组合开始限幅 —— 否则说明判据测不出过载。
#include "dsp/modulator.h"
#include <cstdio>
#include <cmath>
#include <string>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace rta::dsp;

static int g_fails = 0;
static void ck(bool cond, const std::string& desc, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", desc.c_str());
    else { std::printf("  [FAIL] %s %s\n", desc.c_str(), detail.c_str()); ++g_fails; }
}

static long long steady_overloads(double fs, ModulatorFamily fam, int order, int levels, int ahmBits,
                                  double amp, double seconds = 1.0) {
    SdmConfig cfg;
    cfg.fs = fs; cfg.family = fam; cfg.order = order;
    cfg.levels = levels; cfg.ahmBits = ahmBits;
    cfg.cutoffHz = 100000.0; cfg.ditherLsb = 0.0005;
    SdmModulator m(cfg);
    const long long total = (long long)(fs * seconds);
    const long long steadyFrom = total - (long long)(fs * 0.5);
    const double fin = 1981.0546875;
    long long ovAt = 0;
    for (long long i = 0; i < total; ++i) {
        const double x = amp * std::sin(2.0 * kPi * fin * (double)i / fs);
        m.process(x);
        if (i == steadyFrom) ovAt = m.overload();
    }
    return m.overload() - ovAt;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_sdm_headroom: 前级余量目标安全性 ===\n");

    struct Fam { ModulatorFamily f; int order; int levels; int ahm; const char* tag; };
    const Fam fams[] = {
        { ModulatorFamily::Dsd,       5, 2,   0, "DSD5"  },
        { ModulatorFamily::Ec,        5, 2,   0, "EC5"   },
        { ModulatorFamily::Chebyshev, 5, 2,   0, "Cheb5" },
        { ModulatorFamily::Ahm,       7, 256, 8, "AHM8"  },
    };
    const double rates[] = { 2822400.0, 5644800.0, 11289600.0, 22579200.0 };
    const char* rateNames[] = { "DSD64", "DSD128", "DSD256", "DSD512" };

    // 1) 目标峰值必须安全
    for (const auto& fam : fams) {
        for (int r = 0; r < 4; ++r) {
            const double target = sdm_target_peak(rates[r]);
            const long long ov = steady_overloads(rates[r], fam.f, fam.order, fam.levels, fam.ahm, target);
            ck(ov == 0, std::string(fam.tag) + "@" + rateNames[r] + " 在目标峰值 " +
                        std::to_string(target).substr(0, 4) + " 下稳态零限幅",
               std::to_string(ov) + " 次");
        }
    }

    // 2) 目标值本身也要合理：DSD64 必须比 DSD128+ 更保守（实测边界差 1.9 dB）
    ck(sdm_target_peak(2822400.0) < sdm_target_peak(5644800.0),
       "DSD64 的目标峰值低于 DSD128+（实测 DSD64 稳定边界更低）",
       std::to_string(sdm_target_peak(2822400.0)) + " vs " + std::to_string(sdm_target_peak(5644800.0)));

    // 3) 负向对照：抬高 1.5 倍必须出现限幅，否则判据是死的
    {
        const double target = sdm_target_peak(2822400.0);
        const long long ov = steady_overloads(2822400.0, ModulatorFamily::Dsd, 5, 2, 0, target * 1.5);
        ck(ov > 0, "负向对照：DSD5@DSD64 抬到目标×1.5 会限幅（判据有效）", std::to_string(ov) + " 次");
    }

    // 4) 余量换算自洽：sdm_headroom_db_for_fs 必须把满幅压到目标峰值
    {
        const double hd = sdm_headroom_db_for_fs(1.0, 2822400.0);
        const double after = std::pow(10.0, hd / 20.0);
        ck(after <= sdm_target_peak(2822400.0) + 1e-9,
           "满幅输入经余量衰减后 <= 目标峰值（DSD64）", std::to_string(after));
        const double hd2 = sdm_headroom_db_for_fs(1.0, 5644800.0);
        ck(hd2 > hd, "DSD128 的衰减量小于 DSD64（目标更高）",
           std::to_string(hd2) + " vs " + std::to_string(hd));
    }

    std::printf("=== test_sdm_headroom: %d 失败 ===\n", g_fails);
    return g_fails ? 1 : 0;
}
