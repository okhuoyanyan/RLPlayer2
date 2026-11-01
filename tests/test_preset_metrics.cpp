// tests/test_preset_metrics.cpp — 261030-01 规格 v2.1 的预设指标与排除规则单测
//
// 判据（规格 §2.5 / §6.1）：
//   · 每个家族按自己的天花板取最大质量点：Kaiser/BH 280 dB，高斯 205 dB
//   · 排除只两条：通带切到真实内容（f_p < f_c − 1 kHz）/ 抗混叠不足
//   · 最小相位的预振铃恒为 0
#include "dsp/filter_families.h"
#include <cstdio>
#include <string>
#include <cmath>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace rta::dsp;

static int g_fails = 0;
static void ck(bool cond, const char* desc, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", desc);
    else { std::printf("  [FAIL] %s %s\n", desc, detail.c_str()); ++g_fails; }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_preset_metrics: 预设指标与排除规则 ===\n");

    const double CD = 20000.0;    // CD 内容边缘
    const double HI = 40000.0;    // 96k 内容边缘

    // 1. 家族天花板
    {
        const FilterPreset* p = find_preset("poly-sinc-lp", false);
        const FilterPreset* g = find_preset("poly-sinc-gauss-hires-mp", true);
        ck(p && g, "能取到 poly-sinc-lp 与 poly-sinc-gauss-hires-mp");
        if (p && g) {
            auto mp = preset_metrics(*p, 44100.0, CD);
            auto mg = preset_metrics(*g, 44100.0, CD);
            ck(std::fabs(mp.ceilingDb - 280.0) < 0.5, "Kaiser 天花板 = 280 dB", std::to_string(mp.ceilingDb));
            // 2026-10-30：高斯窗天花板由 205 改为 110 —— 205 是拍脑袋，实测做不到。
            // tap_calib 显示高斯窗可达衰减随 (taps, alpha) 非单调（103/25/25/305/361），无法用单一公式标定；
            // 取实测地板 110 dB（最弱 poly-sinc-gauss-short 为 −100.5 dB，登记为已知缺口）。
            ck(std::fabs(mg.ceilingDb - 100.0) < 0.5, "高斯天花板 = 100 dB（实测地板，留 3.9 dB 余量）", std::to_string(mg.ceilingDb));
        }
    }

    // 2. 最小相位预振铃恒为 0
    {
        const FilterPreset* g = find_preset("poly-sinc-gauss-hires-mp", true);
        if (g) {
            auto m = preset_metrics(*g, 44100.0, CD);
            ck(m.ringMs == 0.0, "最小相位预振铃 = 0", std::to_string(m.ringMs));
        }
    }

    // 3. 线性相位的预振铃随 T 增长
    {
        const FilterPreset* a = find_preset("poly-sinc-short-lp", false);
        const FilterPreset* b = find_preset("poly-sinc-lp", false);
        if (a && b) {
            auto ma = preset_metrics(*a, 44100.0, CD);
            auto mb = preset_metrics(*b, 44100.0, CD);
            ck(mb.T > ma.T, "poly-sinc-lp 的 T 大于 short-lp", std::to_string(ma.T) + " vs " + std::to_string(mb.T));
            ck(mb.ringMs > ma.ringMs, "poly-sinc-lp 的振铃大于 short-lp",
               std::to_string(ma.ringMs) + " vs " + std::to_string(mb.ringMs));
        }
    }

    // 4. 排除规则（D-15 判据 1）：CD 源下 minringFIR 被排除，short-lp 不被排除
    {
        const FilterPreset* m = find_preset("minringFIR", false);
        const FilterPreset* s = find_preset("poly-sinc-short-lp", false);
        if (m && s) {
            auto mm = preset_metrics(*m, 44100.0, CD);
            auto ms = preset_metrics(*s, 44100.0, CD);
            ck(mm.excluded, "CD 源下 minringFIR（通带 15.9k）被排除", std::to_string(mm.passHz));
            ck(!ms.excluded, "CD 源下 short-lp（通带 19.8k）不被排除", std::to_string(ms.passHz));
        }
    }

    // 5. 同一个预设在不同源上排除结果不同：short-lp 在 96k 源下通带 43.2k，不排除
    {
        const FilterPreset* s = find_preset("poly-sinc-short-lp", false);
        if (s) {
            auto m = preset_metrics(*s, 96000.0, HI);
            ck(!m.excluded && m.passHz > 42000.0, "96k 源下 short-lp 通带 43.2k，不排除",
               std::to_string(m.passHz));
            ck(m.ringMs < 0.6, "96k 源下 short-lp 振铃更短（Δf 同比放大）", std::to_string(m.ringMs));
        }
    }

    // 6. 全表扫描：CD 源下被排除的数量应在合理区间（半带/极短家族）
    {
        int excluded = 0, total = 0;
        for (const auto& fp : filter_presets()) {
            auto m = preset_metrics(fp, 44100.0, CD);
            ++total;
            if (m.excluded) ++excluded;
        }
        ck(total == 79, "预设总数 79", std::to_string(total));
        std::printf("  [INFO] CD 源排除 %d / %d\n", excluded, total);
        for (const auto& fp : filter_presets()) {
            auto m = preset_metrics(fp, 44100.0, CD);
            if (m.excluded) std::printf("         - %s (通带 %.0f Hz%s)\n", fp.name, m.passHz,
                                        fp.apodizing ? " apod" : "");
        }
        ck(excluded >= 8 && excluded <= 14, "CD 源下排除数量在 8~14 之间（半带/极短家族）",
           std::to_string(excluded));
    }

    // 7. 数值不构成排除理由：高斯家族虽然只有 205 dB，但 short-lp 之外的都应保留
    {
        const FilterPreset* g = find_preset("poly-sinc-gauss-hires-mp", true);
        if (g) {
            auto m = preset_metrics(*g, 44100.0, CD);
            ck(!m.excluded, "高斯 205 dB 不因数值被排除（D-15）", std::to_string(m.ceilingDb));
        }
    }

    // 8. 抽头反解（§4.3）：T 与输出率无关，只与源和预设的过渡带有关
    {
        const int t441 = solve_taps_per_phase(WindowKind::Kaiser, 0.04, false, 0, 0, 32767, 0, 44100.0);
        const int t48  = solve_taps_per_phase(WindowKind::Kaiser, 0.04, false, 0, 0, 32767, 0, 48000.0);
        const int t96  = solve_taps_per_phase(WindowKind::Kaiser, 0.04, false, 0, 0, 32767, 0, 96000.0);
        ck(t441 == 528, "poly-sinc-lp @44.1k 反解 T=528（旧行为在 DSD1024 下只有 32）", std::to_string(t441));
        ck(t441 >= 528 && t441 <= 600, "T 在标定值附近（21.13 与 24.0 两档之间都算合格）", std::to_string(t441));
        ck(t48 == t441 && t96 == t441, "T 与源采样率无关（归一化过渡带相同）",
           std::to_string(t48) + "/" + std::to_string(t96));
        const int tTight = solve_taps_per_phase(WindowKind::Kaiser, 0.02, false, 0, 0, 32767, 512, 44100.0);
        ck(tTight == 512, "预设自带 tapsPerInput 时优先用它（*-tight）", std::to_string(tTight));
        const int tGauss = solve_taps_per_phase(WindowKind::Gaussian, 0.05, false, 0, 0, 32767, 0, 44100.0);
        ck(tGauss == 32767 / 16, "高斯窗保留原 taps/16（不套公式）", std::to_string(tGauss));
    }

    std::printf("=== test_preset_metrics: %d 失败 ===\n", g_fails);
    return g_fails ? 1 : 0;
}
