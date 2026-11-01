#define NOMINMAX
#include <windows.h>
#include <emmintrin.h>
// Production high-rate SDM quality/recovery regression; no guard bypass and no device.
// Primary error_all metric subtracts only the exact input, retaining harmonics
// and gain/phase residual. The notched metric is a secondary legacy comparison.
#include "dsp/fft.h"
#include "dsp/modulator.h"
#include <chrono>
#include <cstdio>
using namespace rta::dsp;
static double db(double p) { return 10 * log10(p + 1e-300); }
static double band(std::vector<cplx> &b, size_t top, size_t fund, bool notch) {
  fft(b);
  double p = 0;
  for (size_t k = 1; k <= top; ++k)
    if (!notch || !(k + 3 >= fund && k <= fund + 3))
      p += 2 * std::norm(b[k]) / (double(b.size()) * b.size() * .375);
  return p;
}
int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  bool full = false, ahm = false;
  for (int a = 1; a < argc; ++a) {
    std::string arg = argv[a];
    if (arg == "--full")
      full = true;
    else if (arg == "--ahm")
      ahm = true;
    else {
      fprintf(stderr,
              "usage: sdm_highrate_quality [--full] [--ahm]\n");
      return 2;
    }
  }
  const size_t N = size_t(1) << (full ? 20 : 18);
  int reps = full ? 3 : 2;
  auto start = std::chrono::steady_clock::now();
  int fails = 0;
  puts("DEFINITION: periodic Hann; 1..floor(20k*N/fs) positive bins; one-sided "
       "sum 2*abs(FFT)^2/(N^2*0.375); 0dBFS=RMS1; output_noise removes "
       "fundamental +/-3bins; error_all removes exact input in time and "
       "excludes no positive bins; signal floor measures ideal input with same "
       "notch; no audio device.");
  puts("kind,fs,requested,effective,guard,bypass,N,amp,tone_hz,rep,output_"
       "notch_dbfs,error_all_dbfs,ideal_floor_dbfs,overloads,clamps,nonfinite");
  for (double fs : {45158400., 49152000.}) {
    double ref = 0, bad = 0, refError = 0, badError = 0;
    SdmConfig guarded;
    guarded.fs = fs;
    guarded.order = 7;
    guarded.cutoffHz = 100000;
    guarded.oneBitOutput = true;
    guarded.family = ahm ? ModulatorFamily::Ahm : ModulatorFamily::Dsd;
    SdmModulator guardedMod(guarded);
    printf("GUARD fs=%.0f family=%s requested=7 effective=%d stepped_down=%d\n",
           fs, modulator_family_name(guarded.family),
           guardedMod.effectiveOrder(), int(guardedMod.steppedDown()));
    for (int ord : {5, 7}) {
      double total = 0, totalError = 0;
      int count = 0;
      for (double req : {101., 997., 5003.}) {
        if (!full && req != 997.)
          continue;
        SdmConfig c;
        c.fs = fs;
        c.order = ord;
        c.family = ahm ? ModulatorFamily::Ahm : ModulatorFamily::Dsd;
        c.cutoffHz = 100000;
        c.ditherLsb = .0005;
        c.oneBitOutput = true;
        c.bypassOrderGuard = false;
        SdmModulator m(c);
        if (m.effectiveOrder() != ord || m.levels() != 2 || (ord == 7 && !m.uses_sections()))
          ++fails;
        size_t kf = llround(req * N / fs), top = floor(20000 * N / fs),
               warm = size_t(fs * .05);
        double fin = kf * fs / N;
        for (size_t i = 0; i < warm; ++i)
          m.process(.62 * sin(2 * kPi * fin * i / fs));
        for (int rep = 0; rep < reps; ++rep) {
          std::vector<cplx> y(N), e(N), ideal(N);
          long long ov = m.overload(), cl = m.clamped();
          for (size_t i = 0; i < N; ++i) {
            double x = .62 * sin(2 * kPi * fin * (warm + rep * N + i) / fs),
                   v = m.process(x), w = .5 - .5 * cos(2 * kPi * i / N);
            y[i] = v * w;
            e[i] = (v - x) * w;
            ideal[i] = x * w;
          }
          double p = band(y, top, kf, true), q = band(e, top, kf, false),
                 floor = band(ideal, top, kf, true);
          printf("spectrum,%.0f,%d,%d,%d,%d,%zu,.62,%.9f,%d,%.9f,%.9f,%.9f,%"
                 "lld,%lld,%lld\n",
                 fs, ord, m.effectiveOrder(), int(m.steppedDown()),
                 int(c.bypassOrderGuard), N, fin, rep, db(p), db(q), db(floor),
                 m.overload() - ov, m.clamped() - cl, m.nonFinite());
          total += p;
          totalError += q;
          ++count;
        }
      }
      double avg = db(total / count);
      double avgError = db(totalError / count);
      printf("MEAN fs=%.0f requested=%d noise_dbfs=%.9f error_all_dbfs=%.9f\n",
             fs, ord, avg, avgError);
      if (ord == 5) {
        ref = avg;
        refError = avgError;
      } else {
        bad = avg;
        badError = avgError;
      }
      SdmConfig c;
      c.fs = fs;
      c.order = ord;
      c.family = ahm ? ModulatorFamily::Ahm : ModulatorFamily::Dsd;
      c.cutoffHz = 100000;
      c.ditherLsb = .0005;
      c.oneBitOutput = true;
      c.bypassOrderGuard = false;
      SdmModulator m(c);
      size_t end = size_t(fs * .16), tail = size_t(fs * .12);
      double sum = 0, sq = 0, last = 0;
      size_t blocks = 0, same = 0;
      long long ovTail = 0, clTail = 0;
      for (size_t n = 0; n < end; ++n) {
        double amp = n >= size_t(fs * .02) && n < size_t(fs * .04) ? 1. : .62;
        double x = amp * sin(2 * kPi * 997 * n / fs);
        if (n == size_t(fs * .07))
          x = 5.;
        if (n == tail) {
          ovTail = m.overload();
          clTail = m.clamped();
        }
        double y = m.process(x);
        if (n >= tail) {
          sum += y;
          if ((n - tail) % 1024 == 1023) {
            sq += (sum / 1024) * (sum / 1024);
            sum = 0;
            ++blocks;
          }
          if (n > tail && y == last)
            ++same;
        }
        last = y;
      }
      double rms = sqrt(sq / blocks),
             sameRate = double(same) / (end - tail - 1);
      printf("RECOVERY fs=%.0f requested=%d effective=%d overload_total=%lld "
             "overload_tail=%lld clamp_tail=%lld rms_1024=%.9f expected=%.9f "
             "same_rate=%.9f nonfinite=%lld\n",
             fs, ord, m.effectiveOrder(), m.overload(), m.overload() - ovTail,
             m.clamped() - clTail, rms, .62 / sqrt(2.), sameRate,
             m.nonFinite());
      if (m.overload() == 0 || m.overload() != ovTail ||
          m.clamped() != clTail || fabs(rms - .62 / sqrt(2.)) > .01 ||
          sameRate > .65 || m.nonFinite() != 0)
        ++fails;
    }
    // Fixed stimulus regression, not an arbitrary-input stability guarantee.
    // Keep the fifth-order implementation as the unchanged measured baseline.
    // Primary exact-input residual includes every harmonic and fundamental error.
    bool good = badError < -155 && badError - refError <= -15;
    printf("QUALITY fs=%.0f meets=%d notch_delta=%.9f error_delta=%.9f\n", fs,
           int(good), bad - ref, badError - refError);
    if (!good)
      ++fails;
  }
  printf("PROBE_RESULT mode=%s accepted_both=%d elapsed_s=%.3f N=%zu reps=%d\n",
         "production_quality_no_bypass", int(fails == 0),
         std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
             .count(),
         N, reps);
  return fails ? 1 : 0;
}
