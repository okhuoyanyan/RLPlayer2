#include "dsp/linear_quality.h"
#include "dsp_measure.h"
#include <cstdio>
using namespace rta::dsp;
int main() {
  struct Red {
    const char *name;
    unsigned in, out;
    double oldHz;
  };
  const Red reds[] = {{"poly-sinc-mqa/mp3-lp", 48000, 768000, 27435.05859375},
                      {"poly-sinc-hb-m-2s", 48000, 768000, 27432.12890625},
                      {"poly-sinc-hb-m-2s", 48000, 1536000, 27432.12890625},
                      {"poly-sinc-mqa/mp3-lp", 96000, 384000, 54864.2578125},
                      {"poly-sinc-hb-m", 96000, 384000, 55918.9453125},
                      {"poly-sinc-hb-m-2s", 96000, 384000, 54864.2578125}};
  int failed = 0;
  for (auto &x : reds) {
    auto cfg = review2::production_config(*find_preset(x.name), x.in, x.out);
    const bool productionMarginWired =
        cfg.attenDb == linear_kaiser_design_attenuation(280.);
    PolyphaseResampler rs(cfg);
    std::vector<double> h;
    rs.prototype_into(h);
    for (auto &v : h)
      v /= rs.upFactor();
    double fs = review2::prototype_rate(rs, x.in, x.out);
    double v = dsp_measure::direct(h, x.oldHz, fs),
           dc = dsp_measure::direct(h, 0, fs);
    auto corrupt = h;
    corrupt[0] += 1e-10;
    double bad = dsp_measure::direct(corrupt, x.oldHz, fs);
    bool ok = productionMarginWired && v <= -280 && std::abs(dc) < 1e-9 && bad > -280;
    failed += !ok;
    printf("%s %u>%u f=%.12f witness=%.12f dc=%.3g corrupted=%.12f %s\n",
           x.name, x.in, x.out, x.oldHz, v, dc, bad, ok ? "PASS" : "FAIL");
  }
  printf("LINEAR_REGRESSION failures=%d (finite old-red witnesses, dense "
         "full-band gate remains required)\n",
         failed);
  return failed ? 1 : 0;
}
