#pragma once
#include "dsp/resampler.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>
namespace review2 {
inline rta::dsp::ResamplerConfig
production_config(const rta::dsp::FilterPreset &p, unsigned in, unsigned out,
                  bool manual = false) {
  using namespace rta::dsp;
  ResamplerConfig c;
  c.inRate = in;
  c.outRate = out;
  c.channels = 1;
  apply_preset(c, p);
  if (!manual && c.minTapsPerInput == 0)
    c.minTapsPerInput =
        solve_taps_per_phase(p.window, p.transition, p.apodizing, p.apodF1,
                             p.apodF2, p.taps, 0, (double)in);
  return c;
}

// Single-stage prototype is in interpolation domain; cascade hook returns
// process() output impulse.
inline double prototype_rate(const rta::dsp::PolyphaseResampler &r, unsigned in,
                             unsigned out) {
  return r.two_stage() ? (double)out : (double)in * r.upFactor();
}
inline double diagnostic_stop_db(const std::vector<double> &h, double first) {
  // Coarse sampling plus local search can miss other narrow peaks.
  auto at = [&](double f) { return rta::dsp::magnitude_db(h, f); };
  const double step0 = (1.0 - first) / 256.0;
  double bestF = first, bestV = at(first);
  for (int k = 1; k <= 256; ++k) {
    const double f = first + step0 * (double)k;
    const double v = at(f);
    if (v > bestV) {
      bestV = v;
      bestF = f;
    }
  }
  double step = step0;
  for (int round = 0; round < 3; ++round) {
    const double lo = std::max(first, bestF - 2.0 * step);
    const double hi = std::min(1.0, bestF + 2.0 * step);
    for (int k = 0; k <= 64; ++k) {
      const double f = lo + (hi - lo) * (double)k / 64.0;
      const double v = at(f);
      if (v > bestV) {
        bestV = v;
        bestF = f;
      }
    }
    step /= 32.0;
  }
  return bestV;
}

inline double normalized_nyquist(double hz, unsigned in, unsigned L) {
  return 2 * hz / ((double)in * L);
}
inline bool identity_gate(const std::set<std::string> &expected,
                          const std::set<std::string> &actual) {
  return expected == actual;
}
inline int quality_exit(int failures, int unresolved, int allowed = -1) {
  (void)allowed; // Legacy argument is never permission to certify known failures.
  if (failures > 0)
    return 1;
  return unresolved ? 3 : 0;
}
inline double sample_stop(const std::vector<double> &h, double first) {
  auto at = [&](double f) {
    double re = 0, im = 0;
    for (size_t j = 0; j < h.size(); ++j) {
      re += h[j] * cos(3.14159265358979323846 * f * j);
      im += h[j] * sin(3.14159265358979323846 * f * j);
    }
    return hypot(re, im);
  };
  double best = 0;
  for (int k = 0; k <= 256; ++k)
    best = std::max(best, at(first + (1 - first) * k / 256));
  return best;
}
} // namespace review2
