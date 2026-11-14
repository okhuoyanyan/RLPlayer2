// 临时探针：打印每个预设在若干输出率下的「每相抽头 T × L」与预算占用。
// 目的：定位 44.1k->384k 超 maxPrototypeTaps(=2^21) 的具体条目（崩溃根因调查）。
#include "dsp/filter_families.h"
#include "dsp/resampler.h"
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

int main() {
    struct Combo { unsigned in, out; const char* name; };
    const Combo combos[] = {
        {44100,96000,"44.1->96k"}, {44100,384000,"44.1->384k"}, {44100,705600,"44.1->705.6k"},
        {44100,1411200,"44.1->1411.2k"}, {48000,1536000,"48->1536k"}, {96000,384000,"96->384k"},
    };
    const int budget = 1 << 21;
    for (const auto& c : combos) {
        const unsigned g = std::gcd(c.in, c.out);
        const int L = (int)(c.out / g);
        int over = 0; long long worst = 0; std::string worstName;
        std::printf("\n== %s (L=%d, budget=%d) ==\n", c.name, L, budget);
        for (const auto& p : rta::dsp::filter_presets()) {
            rta::dsp::ResamplerConfig rc;
            rc.inRate = c.in; rc.outRate = c.out; rc.channels = 2;
            rta::dsp::apply_preset(rc, p);
            int T = rc.minTapsPerInput;
            if (T == 0)
                T = rta::dsp::solve_taps_per_phase(rc.window, rc.transition, rc.apodizing,
                                                   rc.apodF1, rc.apodF2, rc.totalTaps, rc.minTapsPerInput,
                                                   (double)c.in);
            const long long n = (long long)T * L;
            if (n > budget) {
                ++over;
                if (n > worst) { worst = n; worstName = p.name; }
                if (over <= 12) std::printf("  OVER %-26s T=%6d  proto=%lld (%.1f MB)\n", p.name, T, n, n * 8.0 / 1048576.0);
            }
        }
        std::printf("  合计超预算 %d 条；最大 %s proto=%lld (%.0f MB)\n", over, worstName.c_str(), worst, worst * 8.0 / 1048576.0);
    }
    return 0;
}
