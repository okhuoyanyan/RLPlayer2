#include "dsp/filter_families.h"
#include "dsp/resampler.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
using namespace rta::dsp;
static double stopMax(const std::vector<double>& h, double first) {
    double w = -6000.0;
    for (int k = 0; k <= 256; ++k)
        w = std::max(w, magnitude_db(h, first + (1.0 - first) * (double)k / 256.0));
    return w;
}
static std::vector<double> build(const FilterPreset& p, uint32_t in, uint32_t out, int minT, int* L, int* T) {
    ResamplerConfig c; c.inRate = in; c.outRate = out; c.channels = 1;
    apply_preset(c, p);
    if (minT >= 0) c.minTapsPerInput = minT;
    PolyphaseResampler rs(c);
    std::vector<double> h; rs.prototype_into(h);
    const double up = (double)rs.upFactor();
    for (auto& v : h) v /= up;
    if (L) *L = rs.upFactor(); if (T) *T = rs.tapsPerPhase();
    return h;
}
int main() {
    struct G { uint32_t in, out; double content; };
    const G gs[] = {{44100,705600,20000},{44100,1411200,20000},{48000,768000,20000},{48000,1536000,20000},{96000,384000,40000}};
    int nProdWorse = 0, nSweepWorse = 0, nSame = 0;
    std::printf("%-28s %-18s %8s %8s %8s %8s %s\n", "预设", "组", "生产N", "生产dB", "sweepN", "sweepdB", "谁更差");
    for (const auto& g : gs) for (const auto& p : filter_presets()) {
        const auto m = preset_metrics(p, (double)g.in, g.content);
        if (m.excluded) continue;
        int La=0, Ta=0, Lb=0, Tb=0;
        auto ha = build(p, g.in, g.out, -1, &La, &Ta);
        auto hb = build(p, g.in, g.out, m.T, &Lb, &Tb);
        if (ha.empty() || hb.empty()) continue;
        const double nyq = (double)g.out * 0.5;
        const double first = ((double)g.in - m.passHz) / nyq;
        const double da = stopMax(ha, first), db = stopMax(hb, first);
        const char* who = (std::fabs(da - db) < 1.0) ? "same" : (da > db ? "sweep" : "prod");
        if (std::fabs(da - db) < 1.0) ++nSame; else if (da > db) ++nSweepWorse; else ++nProdWorse;
        if (std::fabs(da - db) >= 3.0)
            std::printf("%-28s %u->%u %8d %8.1f %8d %8.1f %s\n", p.name, g.in, g.out,
                        La * Ta, da, Lb * Tb, db, who);
    }
    std::printf("\n差异>=3dB 的统计：sweep 更差 %d，生产更差 %d，相同 %d\n", nSweepWorse, nProdWorse, nSame);
    return 0;
}
