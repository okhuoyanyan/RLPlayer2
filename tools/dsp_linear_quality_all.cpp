#include "dsp_measure.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace rta::dsp;
int main(int argc, char **argv) {
  double bonus = argc > 1 ? std::stod(argv[1]) : 4.;
  std::string dir = argc > 2 ? argv[2] : "linear-quality";
  std::filesystem::create_directories(dir);
  std::ofstream t(dir + "/linear.tsv");
  t.precision(17);
  t << "identity\tin_rate\tout_rate\tpreset\tT\ttaps\tL\tM\tprototype_"
       "rate\tfft_n\tstop_bins\tstop_hz\tstop_db\tpass_hz\tpass_"
       "db\ttarget\tkind\tstatus\tdc_db\n";
  unsigned groups[][2] = {{44100, 705600},
                          {44100, 1411200},
                          {48000, 768000},
                          {48000, 1536000},
                          {96000, 384000}};
  int count = 0, fail = 0;
  for (auto &g : groups)
    for (auto &p : filter_presets()) {
      if (p.phase != PhaseKind::Linear || p.apodizing ||
          p.window == WindowKind::Gaussian)
        continue;
      if (argc > 3 && std::string(p.name) != argv[3])
        continue;
      auto cfg = review2::production_config(p, g[0], g[1]);
      cfg.attenDb = std::max(p.attenDb, preset_ceiling_db(p.window)) + bonus;
      PolyphaseResampler rs(cfg);
      std::vector<double> h;
      rs.prototype_into(h);
      for (auto &x : h)
        x /= rs.upFactor();
      auto m = preset_metrics(p, g[0], g[0] == 96000 ? 40000 : 20000);
      dsp_measure::Result r;
      r.T = rs.tapsPerPhase();
      r.L = rs.upFactor();
      r.M = rs.downFactor();
      r.fs = review2::prototype_rate(rs, g[0], g[1]);
      r.taps = h.size();
      r.passHz = m.passHz;
      r.stopHz = g[0] - m.passHz;
      r.target = m.ceilingDb;
      r.excluded = m.excluded;
      r.finite = true;
      dsp_measure::measure(h, r);
      std::string id =
          std::to_string(g[0]) + "/" + std::to_string(g[1]) + "/" + p.name;
      t << id << '\t' << g[0] << '\t' << g[1] << '\t' << p.name << '\t' << r.T
        << '\t' << r.taps << '\t' << r.L << '\t' << r.M << '\t' << r.fs << '\t'
        << r.fftN << '\t' << r.stopBins << '\t' << r.stopHz << '\t' << r.stopDb
        << '\t' << r.passWorstHz << '\t' << r.passDb << '\t' << r.target << '\t'
        << r.kind << '\t' << r.status << '\t' << r.dcDb << '\n';
      t.flush();
      auto bpath = std::filesystem::u8path(dir) /
                   (std::to_string(g[0]) + "-" + std::to_string(g[1]) + "-" +
                    p.name + ".f64");
      std::filesystem::create_directories(bpath.parent_path());
      std::ofstream b(bpath, std::ios::binary);
      b.write((char *)h.data(), h.size() * 8);
      if (!b)
        throw std::runtime_error("coefficient export failed");
      bool bad = !r.excluded && (r.stopFail || r.passFail);
      ++count;
      fail += bad;
      printf("%s bonus=%.0f stop=%.9f pass=%.9f %s\n", id.c_str(), bonus,
             r.stopDb, r.passDb, r.status.c_str());
      fflush(stdout);
    }
  printf("LINEAR bonus=%.0f cases=%d fails=%d\n", bonus, count, fail);
  return fail ? 1 : 0;
}
