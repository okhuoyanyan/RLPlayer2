// tools/sweep_max_check.cpp — 验证「在阻带边缘单点测」是否等于「阻带内最大值」（261030-01）
//
// 规格 §2.3 要求：镜像抑制的定义域是 20 kHz 到 fs_out/2 **全程**，取最大值。
// 而 filter_sweep 为了速度只在**边缘单点**测（pass + Δf，即通带边缘那首曲子的镜像落点）。
//
// 本工具对每个预设同时算两个值：
//   edge = 边缘单点；grid = 阻带内 512 点网格的最大值。
// 若 grid 明显低于 edge，说明「边缘就是最坏点」，单点测法成立；
// 若某预设 grid 高于 edge，说明单点测法**低估**了，那个预设的验收数字要改。
#include "dsp/filter_families.h"
#include "dsp/filter_design.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace rta::dsp;

static double at_freq(const std::vector<double>& h, double f) {
    const double w = kPi * f;
    const double wr = std::cos(-w), wi = std::sin(-w);
    double cr = 1, ci = 0;
    long double re = 0, im = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        re += h[i] * cr; im += h[i] * ci;
        const double nr = cr * wr - ci * wi, ni = cr * wi + ci * wr;
        cr = nr; ci = ni;
        if ((i & 0x3FFF) == 0x3FFF) {
            const double m = std::sqrt(cr * cr + ci * ci);
            if (m > 0.0) { cr /= m; ci /= m; }
        }
    }
    const long double mag = std::sqrt(re * re + im * im);
    return 20.0 * (double)std::log10(mag > 1e-300L ? mag : 1e-300L);
}

int main() {
    const double fsIn = 44100.0, L = 16.0, contentHz = 20000.0;
    std::printf("=== 边缘单点 vs 阻带最大值（44.1k -> 705.6k, 16x）===\n");
    std::printf("%-28s %10s %10s %10s\n", "预设", "edge(dB)", "grid(dB)", "差");
    int worse = 0;
    for (const auto& p : filter_presets()) {
        const auto m = preset_metrics(p, fsIn, contentHz);
        if (m.excluded) continue;
        if ((long long)m.T * 16 > 400000LL) continue;   // 跳过超大原型，控制耗时
        const double cutoff = (m.passHz + m.dfHz * 0.5) / (fsIn * L * 0.5);
        FirSpec s; s.taps = m.T * 16; s.cutoff = cutoff;
        s.attenDb = (p.attenDb > m.ceilingDb) ? p.attenDb : m.ceilingDb;
        s.window = p.window; s.apodizing = p.apodizing;
        if (p.apodizing) {
            s.apodF1 = m.passHz / (fsIn * L * 0.5);
            s.apodF2 = (m.passHz + m.dfHz) / (fsIn * L * 0.5);
        }
        auto h = design_fir(s);
        const double fEdge = (fsIn - m.passHz) / (fsIn * L * 0.5);
        const double edge = at_freq(h, fEdge);
        double grid = -1e9;
        const int G = 512;
        for (int g = 0; g <= G; ++g) {
            const double f = fEdge + (0.5 - fEdge) * (double)g / (double)G;
            const double v = at_freq(h, f);
            if (v > grid) grid = v;
        }
        const double diff = grid - edge;
        if (diff > 1.0) ++worse;
        std::printf("%-28s %10.1f %10.1f %10.1f%s\n", p.name, edge, grid, diff,
                    diff > 1.0 ? "  <== grid 更差！" : "");
    }
    std::printf("\n结论：%s\n", worse == 0
                ? "全部预设的「阻带最大值」都不高于「边缘单点」⟹ filter_sweep 的单点测法成立"
                : "有预设的阻带最大值高于边缘单点 ⟹ 单点测法低估了，需改为扫最大值");
    return worse == 0 ? 0 : 1;
}
