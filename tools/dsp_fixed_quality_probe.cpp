#include "engine/pipeline.h"
#include "dsp_measure.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdio>
using namespace rta::dsp;
static bool actual_fixed(const FilterPreset &p, int channels, unsigned inRate=96000, unsigned outRate=384000) {
  rta::engine::PipelineConfig cfg;
  rta::engine::apply_filter_preset(cfg, p);
  cfg.autoTaps = true;
  cfg.outRate = outRate;
  cfg.outBits = 24;
  cfg.sourceBits = 24;
  cfg.useFixed = true;
  cfg.gpuResample = false;
  rta::engine::Pipeline pipe(cfg);
  rta::audio::AudioBuffer in;
  in.rate = inRate;
  in.bits = 24;
  in.channels = channels;
  in.raw.assign(size_t(channels) * 4, 1234);
  rta::engine::PipelineStats st;
  pipe.process(in, &st);
  return st.usedFixed;
}

static int matrix(const std::string&dir) {
 std::filesystem::create_directories(dir);
 std::ofstream selection(dir+"/selection790.tsv"),t(dir+"/fixed.tsv");
 selection<<"identity\tchannels\tusedFixed\terror\n";
 t<<std::setprecision(17)<<"identity\tin_rate\tout_rate\tpreset\tT\ttaps\tL\tM\tprototype_rate\tfft_n\tstop_bins\tstop_hz\tstop_db\tpass_hz\tpass_db\ttarget\tkind\tstatus\tdc_db\n";
 unsigned groups[][2]={{44100,705600},{44100,1411200},{48000,768000},{48000,1536000},{96000,384000}};
 int enumerated=0,eligible=0,streams=0,qualityFail=0,excludedFail=0,errors=0,maxBoundBits=0;
 uint64_t globalMaxQ=0;double globalMaxScaled=0;
 for(auto&g:groups)for(const auto&p:filter_presets()) {
  const std::string id=std::to_string(g[0])+"/"+std::to_string(g[1])+"/"+p.name;
  bool used=false;for(int ch:{1,2}) {++enumerated;try{bool on=actual_fixed(p,ch,g[0],g[1]);used|=on;streams+=on;selection<<id<<'\t'<<ch<<'\t'<<on<<"\t\n";}catch(const std::exception&e){++errors;selection<<id<<'\t'<<ch<<"\t0\t"<<e.what()<<'\n';}}selection.flush();if(!used)continue;++eligible;
  try {
   auto cfg=review2::production_config(p,g[0],g[1]);cfg.useFixed=true;PolyphaseResampler rs(cfg);
   const auto* stage=rs.first_stage()?rs.first_stage():&rs;
   std::vector<double>scaled;stage->prototype_into(scaled);std::vector<int64_t>q;
   if(!fixed_coeffs_from_double(scaled,q))throw std::runtime_error("Q62 coefficient overflow");
   for(auto v:scaled)globalMaxScaled=std::max(globalMaxScaled,std::abs(v));
   const int L1=stage->upFactor(),T1=stage->tapsPerPhase();
   for(int phase=0;phase<L1;++phase){unsigned __int128 sum=0;for(int u=0;u<T1;++u){int64_t v=q[size_t(phase+u*L1)];uint64_t a=v<0?uint64_t(-(v+1))+1:uint64_t(v);globalMaxQ=std::max(globalMaxQ,a);sum+=a;}unsigned __int128 bound=sum<<kFixedInputBits;int bits=0;for(auto z=bound;z;z>>=1)++bits;maxBoundBits=std::max(maxBoundBits,bits);if(bits>=128)throw std::runtime_error("int128 accumulator bound overflow");}
   std::vector<double>h;
   if(rs.two_stage()) {rs.prototype_into(h);for(auto&v:h)v/=rs.upFactor();}
   else {h.resize(q.size());for(size_t i=0;i<h.size();++i)h[i]=std::ldexp(double(q[i]),-kFixedCoeffBits)/rs.upFactor();}
   const auto m=preset_metrics(p,g[0],g[0]==96000?40000:20000);
   dsp_measure::Result r;r.T=rs.stage1_taps_per_phase();r.stage2T=rs.stage2_taps_per_phase();r.L=rs.upFactor();r.M=rs.downFactor();r.fs=review2::prototype_rate(rs,g[0],g[1]);r.taps=h.size();r.cascade=rs.two_stage();r.passHz=m.passHz;r.stopHz=g[0]-m.passHz;r.target=m.ceilingDb;r.excluded=m.excluded;r.finite=true;dsp_measure::measure(h,r);
   bool fail=r.stopFail||r.passFail;if(fail){if(r.excluded)++excludedFail;else ++qualityFail;}
   t<<id<<'\t'<<g[0]<<'\t'<<g[1]<<'\t'<<p.name<<'\t'<<r.T<<'\t'<<r.taps<<'\t'<<r.L<<'\t'<<r.M<<'\t'<<r.fs<<'\t'<<r.fftN<<'\t'<<r.stopBins<<'\t'<<r.stopHz<<'\t'<<r.stopDb<<'\t'<<r.passWorstHz<<'\t'<<r.passDb<<'\t'<<r.target<<'\t'<<r.kind<<'\t'<<r.status<<'\t'<<r.dcDb<<'\n';t.flush();
   auto file=std::filesystem::path(dir)/(std::to_string(g[0])+"-"+std::to_string(g[1])+"-"+p.name+".f64");std::filesystem::create_directories(file.parent_path());std::ofstream b(file,std::ios::binary);b.write((char*)h.data(),h.size()*8);if(!b)throw std::runtime_error("coefficient export failed");
   printf("MATRIX %s fixed62 T1=%d T2=%d cascade=%d stop=%.12f pass=%.12f %s\n",id.c_str(),r.T,r.stage2T,r.cascade,r.stopDb,r.passDb,r.status.c_str());fflush(stdout);
  }catch(const std::exception&e){++errors;printf("ERROR %s %s\n",id.c_str(),e.what());fflush(stdout);}
 }
 printf("FIXED_MATRIX enumerated=%d eligible_identities=%d actual_fixed_streams=%d quality_fail=%d excluded_fail=%d errors=%d max_abs_scaled_coefficient=%.17g max_abs_Q62=%llu accumulator_upper_bound_bits=%d\n",enumerated,eligible,streams,qualityFail,excludedFail,errors,globalMaxScaled,(unsigned long long)globalMaxQ,maxBoundBits);
 return errors||qualityFail?1:0;
}
int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "fixed-quality";
  if(argc>2 && std::string(argv[2])=="--matrix")return matrix(dir);
  std::filesystem::create_directories(dir);
  std::ofstream t(dir + "/fixed.tsv");
  t << std::setprecision(17)
    << "identity\tin_rate\tout_rate\tpreset\tT\ttaps\tL\tM\tprototype_"
       "rate\tfft_n\tstop_bins\tstop_hz\tstop_db\tpass_hz\tpass_"
       "db\ttarget\tkind\tstatus\tdc_db\n";
  int failures = 0;
  for (auto name :
       {"poly-sinc-short-lp", "poly-sinc-lp", "poly-sinc-long-lp"}) {
    const auto &p = *find_preset(name);
    dsp_measure::Result r;
    auto h = dsp_measure::prototype(p, 96000, 384000, r);
    auto d = r;
    dsp_measure::measure(h, d);
    std::vector<double> scaled = h;
    for (auto &x : scaled)
      x *= r.L;
    std::vector<int64_t> q;
    if (!fixed_coeffs_from_double(scaled, q))
      return 2;
    std::vector<double> fixed(h.size());
    double maxError = 0;
    for (size_t i = 0; i < h.size(); ++i) {
      fixed[i] = std::ldexp(double(q[i]), -kFixedCoeffBits) / r.L;
      maxError = std::max(maxError, std::abs(fixed[i] - h[i]));
    }
    dsp_measure::measure(fixed, r);
    bool mono = actual_fixed(p, 1), stereo = actual_fixed(p, 2);
    printf("FIXED %s T=%d N=%zu bits=%d usedFixed_mono=%d usedFixed_stereo=%d "
           "binary64_stop=%.12f fixed_stop=%.12f frequency_hz=%.12f "
           "fixed_pass=%.12f max_coefficient_error=%.17g %s\n",
           name, r.T, h.size(), kFixedCoeffBits, mono, stereo, d.stopDb,
           r.stopDb, r.stopHz, r.passDb, maxError, r.status.c_str());
    t << "96000/384000/" << name << "\t96000\t384000\t" << name << '\t' << r.T
      << '\t' << r.taps << '\t' << r.L << '\t' << r.M << '\t' << r.fs << '\t'
      << r.fftN << '\t' << r.stopBins << '\t' << r.stopHz << '\t' << r.stopDb
      << '\t' << r.passWorstHz << '\t' << r.passDb << '\t' << r.target << '\t'
      << r.kind << '\t' << r.status << '\t' << r.dcDb << '\n';
    std::ofstream b(dir + "/96000-384000-" + name + ".f64", std::ios::binary);
    b.write((char *)fixed.data(), fixed.size() * 8);
    failures += r.stopFail || r.passFail;
  }
  printf("FIXED_QUALITY failures=%d; quantized coefficient spectrum only, "
         "before final PCM quantization.\n",
         failures);
  return failures ? 1 : 0;
}
