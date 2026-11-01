#include "engine/pipeline.h"
#include "dsp_measure.h"
#include <cstdio>
using namespace rta::dsp;
int main() {
  int failures = 0;
  for (auto name :
       {"poly-sinc-short-lp", "poly-sinc-lp", "poly-sinc-long-lp"}) {
    const auto &p = *find_preset(name);
    dsp_measure::Result r;
    auto h = dsp_measure::prototype(p, 96000, 384000, r);
    std::vector<double> scaled = h;
    for (auto &x : scaled)
      x *= r.L;
    std::vector<int64_t> q;
    bool fits = fixed_coeffs_from_double(scaled, q);
    if (!fits) {
      ++failures;
      continue;
    }
    std::vector<double> fixed(h.size());
    for (size_t i = 0; i < h.size(); ++i)
      fixed[i] = std::ldexp(double(q[i]), -kFixedCoeffBits) / r.L;
    dsp_measure::measure(fixed, r);
    bool pass =
        !r.stopFail && !r.passFail && !r.uncertain && kFixedCoeffBits == 62;
    failures += !pass;
    printf("Q62 %s stop=%.12f pass=%.12f %s\n", name, r.stopDb, r.passDb,
           pass ? "PASS" : "FAIL");
    if (std::string(name) == "poly-sinc-lp") {
      auto low = h;
      for (size_t i = 0; i < h.size(); ++i)
        low[i] =
            std::ldexp(double(std::llround(std::ldexp(scaled[i], 50))), -50) /
            r.L;
      double old = dsp_measure::direct(low, 130856.689453125, r.fs);
      bool red = old > -280;
      failures += !red;
      printf("NEGATIVE_Q50 known_violation=%.12f %s\n", old,
             red ? "REJECTED" : "MISSED");
      rta::engine::PipelineConfig c;
      rta::engine::apply_filter_preset(c, p);
      c.outRate = 384000;
      c.outBits = 24;
      c.sourceBits = 24;
      c.autoTaps = true;
      c.useFixed = true;
      rta::engine::Pipeline pipeline(c);
      rta::audio::AudioBuffer input;
      input.rate = 96000;
      input.bits = 24;
      input.channels = 1;
      input.raw.assign(4, 1234);
      rta::engine::PipelineStats stats;
      pipeline.process(input, &stats);
      failures += !stats.usedFixed;
      printf("REAL_PIPELINE_MONO usedFixed=%d\n", stats.usedFixed);
    }
  }
  printf("FIXED_QUALITY_REGRESSION failures=%d; coefficient-domain target "
         "before final PCM quantization\n",
         failures);
  return failures ? 1 : 0;
}
