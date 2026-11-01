#include "dsp_measure.h"
#include <cstdio>
#include <fstream>
using namespace rta::dsp;
std::vector<double> candidate(int n, double cutoff, double atten, bool even,
                              bool endpoints) {
  if (!even && (n % 2 == 0))
    ++n;
  double beta = kaiser_beta_for_attenuation(atten), den = bessel_i0(beta),
         mid = .5 * (n - 1), sum = 0;
  std::vector<double> h(n);
  for (int i = 0; i < n; ++i) {
    double x = i - mid, r = 2 * x / (n - 1),
           w = (endpoints || std::abs(r) < 1)
                   ? bessel_i0(beta * sqrt(std::max(0., 1 - r * r))) / den
                   : 0;
    double v = x == 0 ? cutoff : sin(kPi * cutoff * x) / (kPi * x);
    h[i] = v * w;
    sum += h[i];
  }
  for (auto &x : h)
    x /= sum;
  return h;
}
int main() {
  for (int L : {4, 16, 32})
    for (double trans : {.14, .15})
      for (int mode = 0; mode < 4; ++mode)
        for (double extra : {0., 1., 2., 3., 4.}) {
          int T = int(21.13 / trans + .5), n = T * L;
          auto h = candidate(n, (1 - .5 * trans) / L, 280 + extra, mode & 1,
                             mode & 2);
          h.resize(n);
          dsp_measure::Result r;
          r.fs = 48000. * L;
          r.passHz = (1 - trans) * 24000;
          r.stopHz = 48000 - r.passHz;
          r.target = 280;
          r.finite = true;
          dsp_measure::measure(h, r);
          printf("L=%d t=%.2f T=%d mode=%d extra=%.0f stop=%.9f pass=%.9f "
                 "f=%.5f\n",
                 L, trans, T, mode, extra, r.stopDb, r.passDb, r.stopHz);
          fflush(stdout);
        }
}
