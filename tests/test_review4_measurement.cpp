#include "engine/pipeline.h"
#include "../tools/dsp_measure.h"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdio>

int main(int argc, char** argv) {
  int failed = 0, checks = 0;
  auto check = [&](bool ok, const char* label) {
    ++checks; failed += !ok; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
  };
  for (auto [name, in, out] : {
      std::tuple{"poly-sinc-short-lp", 44100u, 705600u},
      std::tuple{"poly-sinc-short-lp", 44100u, 96000u},
      std::tuple{"poly-sinc-long-mp", 44100u, 705600u},
      std::tuple{"poly-sinc-long-ip", 44100u, 705600u},
      std::tuple{"apod-20k", 44100u, 705600u},
      std::tuple{"poly-sinc-gauss-2s", 44100u, 705600u}}) {
    const auto* preset = rta::dsp::find_preset(name);
    auto rc = review2::production_config(*preset, in, out);
    rta::dsp::PolyphaseResampler oracle(rc);
    rta::engine::PipelineConfig pc;
    rta::engine::apply_filter_preset(pc, *preset);
    pc.autoTaps = true; pc.gpuResample = false; pc.useFixed = false; pc.outRate = out;
    rta::engine::Pipeline product(pc);
    rta::audio::AudioBuffer input;
    input.rate = in; input.channels = 1; input.bits = 24;
    input.raw.resize(67);
    for (size_t i=0; i<input.raw.size(); ++i) input.raw[i] = int((i*7919)%1000000)-500000;
    std::vector<double> plane(input.frames());
    for (size_t i=0; i<plane.size(); ++i) plane[i]=input.sample(i,0);
    std::vector<double> expected(oracle.maxOutFrames(plane.size()));
    const double* ip=plane.data(); double* op=expected.data();
    auto n=oracle.process(&ip,plane.size(),&op); expected.resize(n);
    rta::engine::PipelineStats stats;
    std::vector<std::vector<double>> actual;
    product.process(input,&stats,&actual);
    bool identity = stats.tapsPerPhase==oracle.stage1_taps_per_phase() &&
        stats.stage2TapsPerPhase==oracle.stage2_taps_per_phase() &&
        stats.coeffBytes==oracle.coefficient_count()*sizeof(double) &&
        product.twoStageActive()==oracle.two_stage();
    check(identity, name);
    bool same=actual.size()==1 && actual[0].size()==expected.size();
    if(same) for(size_t i=0;i<n;++i) same &= std::bit_cast<uint64_t>(actual[0][i])==std::bit_cast<uint64_t>(expected[i]);
    check(same,"actual-Pipeline-output-equals-measurement-construction");
    std::printf("IDENTITY %s in=%u out=%u T=%d stage2=%d N=%zu samples=%zu\n",name,in,out,
        stats.tapsPerPhase,stats.stage2TapsPerPhase,stats.coeffBytes/8,n);
  }
  auto rejects=[&](const std::vector<double>& h,double f,double fs) {
    try { (void)dsp_measure::direct(h,f,fs); return false; }
    catch(const std::invalid_argument&) { return true; }
  };
  check(rejects({},0,2),"empty-is-not-a-deep-stopband");
  check(rejects({NAN},0,2),"NaN-is-not-a-measurement");
  check(rejects({1},0,0),"zero-rate-rejected");
  check(rejects({1},NAN,2),"NaN-frequency-rejected");
  check(dsp_measure::direct({.5,.5},-.25,2)==dsp_measure::direct({.5,.5},.25,2),
        "cascade-signed-alias-is-valid");
  check(dsp_measure::direct({.5,.5},2.25,2)==dsp_measure::direct({.5,.5},.25,2),
        "physical-response-periodicity");
  check(dsp_measure::direct({.5,.5},1,2)==-6000,"analytic-Nyquist-zero-not-roundoff");
  auto wrong=review2::production_config(*rta::dsp::find_preset("poly-sinc-short-lp"),44100,705600,true);
  rta::dsp::PolyphaseResampler different(wrong);
  check(different.tapsPerPhase()!=211,"manual-production-identity-negative-control");
  if(argc>1) {
    const std::filesystem::path directory=argv[1]; std::filesystem::create_directories(directory);
    std::ofstream table(directory/"calibration.tsv"); table<<std::setprecision(17);
    table<<"identity\tin_rate\tout_rate\tpreset\tprototype_rate\tfrequency_hz\tdd_db\texact_db\tcoefficient_file\n";
    struct Case { const char* name; std::vector<double> h; std::vector<double> f; };
    const std::vector<Case> cases = {
      {"unity",{1},{0,.123456789,1}}, {"delay",{0,0,1},{.2123456789,1}},
      {"average",{.5,.5},{0,.5,.9999999999999993,1}},
      {"deep300",{1,-1,1e-15},{0,.5}},
      {"deep280",{1e-8,1,1e-8,1+1e-14},{.5}},
      {"symmetric",{.2,.3,.3,.2},{.123456789,1}}
    };
    for(const auto& c:cases) {
      const std::string file=std::string(c.name)+".f64";
      std::ofstream blob(directory/file,std::ios::binary);
      blob.write(reinterpret_cast<const char*>(c.h.data()),c.h.size()*8);
      for(double f:c.f) table<<"cal/"<<c.name<<'/'<<f<<"\t1\t2\t"<<c.name<<"\t2\t"<<f<<'\t'
          <<dsp_measure::direct(c.h,f,2)<<'\t'<<rta::dsp::magnitude_db_exact(c.h,f)<<'\t'<<file<<'\n';
    }
    check(bool(table),"calibration-file-written");
  }
  std::printf("review4_measurement: %d checks, %d failures\n",checks,failed);
  return failed?1:0;
}
