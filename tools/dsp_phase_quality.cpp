#include "dsp_measure.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace rta::dsp;
int main(int argc, char **argv) {
  const std::string dir = argc > 1 ? argv[1] : "phase-quality";
  std::filesystem::create_directories(dir);
  std::ofstream t(dir + "/phase.tsv");
  t.precision(17);
  t << "identity\tin_rate\tout_rate\tpreset\tT\ttaps\tL\tM\tprototype_"
       "rate\tfft_n\tstop_bins\tstop_hz\tstop_db\tpass_hz\tpass_"
       "db\ttarget\tkind\tstatus\tdc_db\tcentroid\tpeak_position\tpre_"
       "post\ttail_energy_ratio\tdesign_ms\tgroup_delay_low\tgroup_delay_"
       "high\n";
  unsigned groups[][2] = {{44100, 705600},
                          {44100, 1411200},
                          {48000, 768000},
                          {48000, 1536000},
                          {96000, 384000}};
  int count = 0, fails = 0;
  for (auto &g : groups)
    for (auto &p : filter_presets()) {
      if (p.phase == PhaseKind::Linear)
        continue;
      dsp_measure::Result r;
      auto begin = std::chrono::steady_clock::now();
      auto h = dsp_measure::prototype(p, g[0], g[1], r);
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - begin)
                      .count();
      dsp_measure::measure(h, r);
      double et = 0, e = 0;
      for (size_t i = 0; i < h.size(); ++i) {
        e += h[i] * h[i];
        if (i >= h.size() * 7 / 8)
          et += h[i] * h[i];
      }
      auto gd = [&](double hz) {
        std::complex<double> a{}, b{};
        double w = 2 * kPi * hz / r.fs;
        for (size_t i = 0; i < h.size(); ++i) {
          auto z = std::polar(h[i], -w * i);
          a += z;
          b += z * double(i);
        }
        return (b / a).real();
      };
      const std::string id =
          std::to_string(g[0]) + "/" + std::to_string(g[1]) + "/" + p.name;
      t << id << '\t' << g[0] << '\t' << g[1] << '\t' << p.name << '\t' << r.T
        << '\t' << r.taps << '\t' << r.L << '\t' << r.M << '\t' << r.fs << '\t'
        << r.fftN << '\t' << r.stopBins << '\t' << r.stopHz << '\t' << r.stopDb
        << '\t' << r.passWorstHz << '\t' << r.passDb << '\t' << r.target << '\t'
        << r.kind << '\t' << r.status << '\t' << r.dcDb << '\t'
        << energy_centroid(h) << '\t' << peak_position(h) << '\t'
        << pre_post_ratio(h) << '\t' << et / e << '\t' << ms << '\t'
        << gd(.2 * r.passHz) << '\t' << gd(.9 * r.passHz) << '\n';
      t.flush();
      auto bpath = std::filesystem::u8path(dir) /
                   (std::to_string(g[0]) + "-" + std::to_string(g[1]) + "-" +
                    p.name + ".f64");
      std::filesystem::create_directories(bpath.parent_path());
      std::ofstream b(bpath, std::ios::binary);
      b.write((char *)h.data(), h.size() * 8);
      if (!b)
        throw std::runtime_error("coefficient export failed");
      ++count;
      if (!r.excluded && (r.stopFail || r.passFail))
        ++fails;
      printf("%s T=%d N=%zu stop=%.9f @%.9f pass=%.9f dc=%.3g tail=%.3g "
             "design_ms=%.3f %s\n",
             id.c_str(), r.T, h.size(), r.stopDb, r.stopHz, r.passDb, r.dcDb,
             et / e, ms, r.status.c_str());
      fflush(stdout);
    }
  printf("PHASE identities=%d failures=%d; finite grid only, no "
         "continuous-band proof.\n",
         count, fails);
  return fails ? 1 : 0;
}
