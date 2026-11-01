#include "dsp/filter_families.h"
#include "dsp_measure.h"
#include <cstdio>
using namespace rta::dsp;
static int fails = 0;
static void check(bool b, const char *s) {
  if (!b)
    ++fails;
  printf("%s %s\n", b ? "PASS" : "FAIL", s);
}
static double gd(const std::vector<double> &h, double f) {
  std::complex<double> a{}, b{};
  for (size_t i = 0; i < h.size(); ++i) {
    auto z = std::polar(h[i], -kPi * f * i);
    a += z;
    b += double(i) * z;
  }
  return (b / a).real();
}
int main() {
  for (int n : {3, 4, 300, 704, 1056})
    for (auto window :
         {WindowKind::Kaiser, WindowKind::Gaussian, WindowKind::BlackmanHarris})
      for (bool apod : {false, true}) {
        FirSpec s;
        s.taps = n;
        s.cutoff = .45;
        s.attenDb = 120;
        s.window = window;
        s.apodizing = apod;
        s.apodF1 = .4;
        s.apodF2 = .5;
        auto h = design_fir(s);
        printf("FIR taps_requested=%d observed=%zu window=%d apod=%d\n", n,
               h.size(), int(window), apod);
        check(h.size() == size_t(n), "exact requested odd/even support");
        check(abs(dsp_measure::direct(h, 0, 2)) < 1e-9,
              "unit DC without post-crop repair");
        double asym = 0;
        for (size_t i = 0; i < h.size(); ++i)
          asym = std::max(asym, abs(h[i] - h[h.size() - 1 - i]));
        check(asym < 1e-14, "coefficient pair symmetry");
        bool analytic = true;
        for (double f : {.05, .12, .2})
          analytic = analytic && abs(gd(h, f) - .5 * (h.size() - 1)) < 1e-8;
        check(
            analytic,
            "analytic LP group delay equals (N-1)/2 at three passband points");
        if (!(n & 1))
          check(dsp_measure::direct(h, 1, 2) < -240,
                "even Type-II has exact Nyquist pair cancellation");
      }
  for (auto name : {"poly-sinc-gauss-short", "poly-sinc-gauss-medium"}) {
    auto cfg =
        review2::production_config(*find_preset(name), 44100, 88200, true);
    PolyphaseResampler rs(cfg);
    std::vector<double> h;
    rs.prototype_into(h);
    for (auto &x : h)
      x /= rs.upFactor();
    double dc = dsp_measure::direct(h, 0, 44100. * rs.upFactor()), asym = 0;
    for (size_t i = 0; i < h.size(); ++i)
      asym = std::max(asym, abs(h[i] - h[h.size() - 1 - i]));
    printf("OLD_RED %s N=%zu dc=%.15g asym=%.15g gd=%.15g expected=%.15g\n",
           name, h.size(), dc, asym, gd(h, .1), .5 * (h.size() - 1));
    check(abs(dc) < 1e-9 && asym < 1e-14 &&
              abs(gd(h, .1) - .5 * (h.size() - 1)) < 1e-8,
          "actual polyphase export preserves DC/symmetry/analytic delay");
  }
  for (int n : {300, 704}) {
    auto h = design_phased_fir(n, .45, 120, WindowKind::Kaiser, false, 0, 0,
                               PhaseKind::Minimum, .5, .35);
    check(
        h.size() == size_t(n) && abs(dsp_measure::direct(h, 0, 2)) < 1e-9,
        "single-stage low-attenuation MP keeps requested support and unit DC");
  }
  for (unsigned out : {705600u, 1411200u})
    for (auto name : {"poly-sinc-short-lp", "poly-sinc-long-lp"}) {
      auto cfg = review2::production_config(*find_preset(name), 44100, out);
      PolyphaseResampler rs(cfg);
      std::vector<double> h;
      rs.prototype_into(h);
      const double expected = .5 * (h.size() - 1),
                   reported = rs.latencyInputSamples() * rs.upFactor();
      const double measured = gd(h, .2 * (1 - cfg.transition) / rs.upFactor());
      printf("KAISER_DELAY %s %u N=%zu measured=%.12g reported=%.12g "
             "expected=%.12g\n",
             name, out, h.size(), measured, reported, expected);
      check(abs(measured - expected) < 1e-7 && reported == expected,
            "actual Kaiser analytic phase delay matches reported latency, no "
            "half-sample error");
      std::vector<double> in(size_t(rs.tapsPerPhase()) + 8, 0);
      in[0] = 1e-3;
      std::vector<double> output(rs.maxOutFrames(in.size()) + 8);
      const double *ii[] = {in.data()};
      double *oo[] = {output.data()};
      size_t count = rs.process(ii, in.size(), oo);
      double err = 0;
      for (size_t i = 0; i < h.size() && i < count; ++i)
        err = std::max(err, abs(output[i] - 1e-3 * h[i]));
      check(count >= h.size() && err < 1e-18,
            "actual Kaiser process impulse retains the complete exact-length "
            "coefficient sequence");
    }
  printf("EVEN_FIR failures=%d\n", fails);
  return fails ? 1 : 0;
}
