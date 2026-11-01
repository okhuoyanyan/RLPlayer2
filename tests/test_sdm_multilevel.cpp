// Offline multilevel quantizer contracts and recovery. Never opens a device.
#include "dsp/modulator.h"
#include <cstdio>
#include <limits>
#include <set>
using namespace rta::dsp;
static int failures = 0;
static double testRate = 11289600;
static void check(bool ok, const char *name, double actual = 0,
                  double expected = 0) {
  printf("[%s] %s actual=%.12g expected=%.12g\n", ok ? "PASS" : "FAIL", name,
         actual, expected);
  if (!ok)
    ++failures;
}
static SdmConfig config(int bits, int order = 5) {
  SdmConfig c;
  c.family = ModulatorFamily::Ahm;
  c.ahmBits = bits;
  c.order = order;
  c.fs = testRate;
  c.cutoffHz = 100000;
  return c;
}
int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--rate") testRate=std::stod(argv[2]);
  else if (argc != 1) return 2;
  if (testRate != 11289600 && testRate != 45158400 && testRate != 49152000) return 2;
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("MULTILEVEL_RATE %.0f\n",testRate);
  // Reset before each dither-free sample: causal feedback history is exactly
  // zero, so these are actual production quantizer outputs, independently of
  // loop state.
  for (int bits : {2, 3, 6, 8, 16}) {
    auto c = config(bits);
    SdmModulator m(c);
    int L = 1 << bits;
    std::set<double> levels;
    for (int i = 0; i <= 8 * L; ++i) {
      m.reset();
      levels.insert(m.process(-1. + 2. * i / (8 * L)));
    }
    printf("LEVEL_SET bits=%d declared=%d observed=%zu\n", bits, m.levels(),
           levels.size());
    check(m.levels() == L && levels.size() == size_t(L),
          "exact advertised number of levels", double(levels.size()), L);
    check(*levels.begin() == -1 && *levels.rbegin() == 1,
          "both endpoints are exact +/-1");
    check(!levels.count(0),
          "even-L uniform endpoint grid has no extra zero level");
    double spacingError = 0, symmetryError = 0;
    auto previous = levels.begin();
    for (auto it = std::next(previous); it != levels.end(); ++it, ++previous)
      spacingError =
          std::max(spacingError, fabs((*it - *previous) - 2. / (L - 1)));
    auto lo = levels.begin();
    auto hi = levels.rbegin();
    for (; lo != levels.end(); ++lo, ++hi)
      symmetryError = std::max(symmetryError, fabs(*lo + *hi));
    check(spacingError < 8 * std::numeric_limits<double>::epsilon(),
          "grid spacing is uniform", spacingError, 0);
    check(symmetryError < 8 * std::numeric_limits<double>::epsilon(),
          "level set is symmetric", symmetryError, 0);
  }
  for (int bits : {2, 6, 8, 16})
    for (int order : {5, 7})
      for (double dith : {0., .0005}) {
        auto c = config(bits, order);
        c.ditherLsb = dith;
        const size_t N = 131072;
        std::vector<double> x(N), ref(N), out(N);
        for (size_t i = 0; i < N; ++i) {
          x[i] = .62 * sin(2 * kPi * 997 * i / c.fs);
          if (i > 20000 && i < 40000)
            x[i] = sin(2 * kPi * 997 * i / c.fs);
          if (i == 60000)
            x[i] = 5.;
        }
        SdmModulator a(c), b(c), binary(c);
        for (size_t i = 0; i < N; ++i)
          ref[i] = a.process(x[i]);
        size_t mismatches = 0;
        for (size_t at = 0; at < N; at += 113)
          b.process_block(x.data() + at, std::min<size_t>(113, N - at),
                          [&](size_t j, double y) { out[at + j] = y; });
        for (size_t i = 0; i < N; ++i)
          mismatches += ref[i] != out[i];
        check(mismatches == 0, "scalar versus 113-frame block is bit identical",
              double(mismatches), 0);
        for (size_t at = 0; at < N; at += 257)
          binary.process_binary_block(
              x.data() + at, std::min<size_t>(257, N - at),
              [&](size_t j, double y) { out[at + j] = y; });
        mismatches = 0;
        for (size_t i = 0; i < N; ++i)
          mismatches += ref[i] != out[i];
        check(mismatches == 0,
              "binary block multilevel fallback retains amplitudes",
              double(mismatches), 0);
        check(a.overload() == b.overload() && a.clamped() == b.clamped() &&
                  a.nonFinite() == b.nonFinite(),
              "scalar and block diagnostic counts agree");
        printf("PARITY bits=%d order=%d effective=%d dither=%.4g frames=%zu\n",
               bits, order, a.effectiveOrder(), dith, N);
      }
  for (int bits : {2, 6, 8, 16})
    for (int order : {5, 7}) {
      auto c = config(bits, order);
      c.ditherLsb = .0005;
      SdmModulator zero(c);
      const size_t N = 262144;
      double sum = 0;
      long long ov = 0;
      for (size_t i = 0; i < 2 * N; ++i) {
        if (i == N)
          ov = zero.overload();
        double y = zero.process(0);
        if (i >= N)
          sum += y;
      }
      printf("ZERO bits=%d order=%d mean=%.12g tail_overload=%lld\n", bits,
             order, sum / N, zero.overload() - ov);
      check(fabs(sum / N) < 2e-5, "zero input has zero long-run mean", sum / N,
            0);
      check(zero.overload() == ov && zero.nonFinite() == 0,
            "zero input remains stable");
      SdmModulator rec(c);
      double ys = 0, xs = 0, y2 = 0, x2 = 0, mean = 0;
      long long tailOv = 0, tailCl = 0;
      double fin = 23 * c.fs / N;
      for (size_t i = 0; i < 3 * N; ++i) {
        double amp = i >= N / 2 && i < N ? 1. : .62;
        double x = amp * sin(2 * kPi * fin * i / c.fs);
        if (i == N + 1)
          x = 5.;
        if (i == 2 * N) {
          tailOv = rec.overload();
          tailCl = rec.clamped();
        }
        double y = rec.process(x);
        if (i >= 2 * N) {
          ys += y;
          xs += x;
          mean += y;
          if ((i - 2 * N) % 256 == 255) {
            y2 += (ys / 256) * (ys / 256);
            x2 += (xs / 256) * (xs / 256);
            ys = xs = 0;
          }
        }
      }
      double rms = sqrt(y2 / (N / 256)), expected = sqrt(x2 / (N / 256));
      printf("RECOVERY bits=%d order=%d total_overload=%lld tail_overload=%lld "
             "tail_clamp=%lld rms=%.12g expected=%.12g mean=%.12g\n",
             bits, order, rec.overload(), rec.overload() - tailOv,
             rec.clamped() - tailCl, rms, expected, mean / N);
      check(rec.overload() > 0 && rec.overload() == tailOv &&
                rec.clamped() == tailCl && rec.nonFinite() == 0,
            "full-scale and +5 spike recover without continuing limits");
      check(fabs(rms - expected) < .001 && fabs(mean / N) < 2e-4,
            "recovered audio RMS and mean match coherent input", rms, expected);
    }
  // AHM for packed 1-bit outputs must retain its explicit binary contract.
  for (int bits : {2, 8, 16}) {
    auto c = config(bits);
    c.oneBitOutput = true;
    SdmModulator m(c);
    check(m.levels() == 2 && m.levels_clamped() &&
              !m.levels_clamp_reason().empty(),
          "packed-output AHM remains explicitly two-level");
    for (double x : {-.2, 0., .2}) {
      m.reset();
      const double actual = m.process(x);
      check(actual == (x >= 0 ? 1. : -1.), "binary comparator unchanged",
            actual, x >= 0 ? 1. : -1.);
    }
  }
  printf("sdm_multilevel failures=%d\n", failures);
  return failures ? 1 : 0;
}
