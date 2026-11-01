#include "dsp/resampler.h"
#include "dsp_measure.h"
#include <complex>
#include <cstdio>
using namespace rta::dsp;
static int failures = 0;
static void check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  failures += !ok;
}
static double gd(const std::vector<double> &h, double hz, double fs) {
  std::complex<double> a{}, b{};
  for (size_t i = 0; i < h.size(); ++i) {
    auto z = std::polar(h[i], -2 * kPi * hz / fs * i);
    a += z;
    b += z * double(i);
  }
  return (b / a).real();
}
static void process_check(const FilterPreset &p, unsigned in, unsigned out) {
  auto cfg = review2::production_config(p, in, out);
  cfg.useFixed = false;
  PolyphaseResampler one(cfg), chunk(cfg);
  std::vector<double> expected;
  one.prototype_into(expected);
  // Delta and its full drain exercise actual coefficient layout/order, not only
  // a design helper.
  const size_t frames = size_t(one.tapsPerPhase()) + 16;
  std::vector<double> input(frames, 0.0);
  input[0] = 1e-3;
  std::vector<double> a(one.maxOutFrames(frames) + 16), b;
  const double *ip[] = {input.data()};
  double *op[] = {a.data()};
  a.resize(one.process(ip, frames, op));
  for (size_t offset = 0; offset < frames;) {
    size_t count = std::min<size_t>((offset % 19) + 1, frames - offset);
    std::vector<double> o(chunk.maxOutFrames(count) + 16);
    const double *ii[] = {input.data() + offset};
    double *oo[] = {o.data()};
    size_t n = chunk.process(ii, count, oo);
    b.insert(b.end(), o.begin(), o.begin() + n);
    offset += count;
  }
  check(a == b, "actual process output is bitwise invariant under irregular "
                "input chunks");
  bool exact = a.size() >= expected.size();
  double maxError = 0;
  for (size_t i = 0; i < expected.size() && i < a.size(); ++i)
    maxError = std::max(maxError, std::abs(a[i] - 1e-3 * expected[i]));
  check(exact && maxError < 1e-18, "actual impulse output matches the complete "
                                   "exported prototype including tail");
  printf("PROCESS %u>%u %s frames=%zu error=%.17g chunk_equal=%d\n", in, out,
         p.name, a.size(), maxError, a == b);
}
int main() {
  // Real auto-Taps regression, not the obsolete manual N=262143 path.
  // Deep stop measurements are independent DD direct sums calibrated to
  // mpmath90.
  const unsigned groups[][2] = {{44100, 705600},
                                {44100, 1411200},
                                {48000, 768000},
                                {48000, 1536000},
                                {96000, 384000}};
  for (auto &g : groups) {
    std::vector<std::vector<double>> hs;
    for (auto name :
         {"poly-sinc-long-mp", "poly-sinc-long-ip", "poly-sinc-long-lp"}) {
      dsp_measure::Result r;
      auto h = dsp_measure::prototype(*find_preset(name), g[0], g[1], r);
      check(r.T == 1409, "production auto T remains 1409");
      check(h.size() == size_t(r.T * r.L),
            "output keeps the exact production resource budget");
      check(std::abs(dsp_measure::direct(h, 0, r.fs)) < 1e-9, "DC unity gain");
      double pass = 0;
      for (int k = 0; k <= 128; ++k)
        pass = std::max(
            pass, std::abs(dsp_measure::direct(h, r.passHz * k / 128., r.fs)));
      check(pass <= .01,
            "whole declared passband including endpoint sampled <=.01 dB");
      if (std::string(name).ends_with("lp")) {
        hs.push_back(std::move(h));
        continue;
      }
      double worst = -6000;
      for (int k = 0; k <= 32; ++k)
        worst = std::max(
            worst, dsp_measure::direct(h, g[0] * (.5075 + .00004 * k), r.fs));
      printf("%u>%u %s stop_witness_max=%.12f pass=%.12f centroid=%.8f\n", g[0],
             g[1], name, worst, pass, energy_centroid(h));
      check(worst <= -280,
            "dense transition-side regression witnesses meet 280 dB");
      auto degraded = h;
      degraded[0] += 1e-10;
      check(dsp_measure::direct(degraded, g[0] * .508, r.fs) > -280,
            "negative control: 1e-10 coefficient corruption is rejected");
      double total = 0, tail = 0;
      for (size_t i = 0; i < h.size(); ++i) {
        total += h[i] * h[i];
        if (i >= h.size() * 7 / 8)
          tail += h[i] * h[i];
      }
      printf("tail_energy_ratio=%.17g\n", tail / total);
      check(tail / total < 1e-8, "tail energy stays below 1e-8 of total");
      hs.push_back(std::move(h));
    }
    const double fs = double(g[0]) * g[1] / g[0], edge = g[0] * .4925;
    double dlow = 0, dhigh = 0;
    for (int k = 0; k < 2; ++k) {
      double hz = edge * (k ? .9 : .2), mp = gd(hs[0], hz, fs),
             ip = gd(hs[1], hz, fs), lp = gd(hs[2], hz, fs);
      printf("GD hz=%.3f mp=%.9f ip=%.9f lp=%.9f\n", hz, mp, ip, lp);
      check(mp < ip && ip < lp, "IP group delay lies between MP and LP");
      (k ? dhigh : dlow) = ip - mp;
    }
    check(std::abs(dhigh - dlow) > .01,
          "IP is not an MP response plus a pure integer/fractional delay");
    check(energy_centroid(hs[0]) < energy_centroid(hs[1]) &&
              energy_centroid(hs[1]) < energy_centroid(hs[2]),
          "IP energy centroid lies between MP and LP");
  }
  for (auto &g : groups)
    for (auto name : {"poly-sinc-long-mp", "poly-sinc-long-ip",
                      "poly-sinc-ext2-hires-ip", "poly-sinc-short-mp"})
      process_check(*find_preset(name), g[0], g[1]);
  printf("RESULT phase regressions=%d; finite witness test, dense full-band "
         "gate is dsp_phase_quality.\n",
         failures);
  return failures ? 1 : 0;
}
