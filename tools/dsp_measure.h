#pragma once
// Shared production measurement entry point. DD FFT >=16*N, exact endpoints,
// all local-max candidates ranked globally, strongest 8 refined at 17 points.
// This remains a finite-grid estimate, never a continuous-band certificate.
#include "review2_spectrum_engine.h"
#include <limits>
namespace dsp_measure {
struct Result {
  size_t taps=0, fftN=0, stopBins=0;
  int T=0, stage2T=0, L=0, M=0;
  double fs=0, passHz=0, stopHz=0, passWorstHz=0;
  double stopDb=-6000, passDb=0, dcDb=0, target=0;
  bool excluded=false, finite=false, stopFail=false, passFail=false, uncertain=false;
  bool cascade=false;
  std::string status="STRUCTURE_FAIL", kind="STRUCTURE";
};
template<class R> inline int first_stage_taps(const R& r) {
  if constexpr(requires {r.stage1_taps_per_phase();}) return r.stage1_taps_per_phase();
  else return r.tapsPerPhase(); // 0 means unavailable on the read-only historical cascade.
}
template<class R> inline int second_stage_taps(const R& r) {
  if constexpr(requires {r.stage2_taps_per_phase();}) return r.stage2_taps_per_phase();
  else return 0;
}
inline void classify(Result& r) {
  r.stopFail=r.stopDb>-r.target+1e-8; r.passFail=std::abs(r.passDb)>.01+1e-10;
  r.uncertain=!(r.stopFail||r.passFail) &&
    (std::abs(r.stopDb+r.target)<1e-8 || std::abs(std::abs(r.passDb)-.01)<1e-10);
  r.kind=r.stopFail?(r.passFail?"STOP_AND_PASS":"STOP"):(r.passFail?"PASS":(r.uncertain?"PRECISION":"NONE"));
  r.status=r.excluded?"EXCLUDED":(r.stopFail||r.passFail?"CONFIRMED_SAMPLED_FAIL":(r.uncertain?"PRECISION_UNRESOLVED":"DENSE_SAMPLED_TARGET_MET"));
}
inline std::vector<double> prototype(const rta::dsp::FilterPreset& p,
 unsigned in, unsigned out, Result& r, bool manual=false, int twoStage=-1) {
  auto cfg=review2::production_config(p,in,out,manual);
  if(twoStage>=0)cfg.twoStage=twoStage!=0;
  rta::dsp::PolyphaseResampler rs(cfg);
  std::vector<double> h; rs.prototype_into(h);
  for(auto& x:h) x/=rs.upFactor();
  r.T=first_stage_taps(rs); r.stage2T=second_stage_taps(rs); r.L=rs.upFactor(); r.M=rs.downFactor();
  r.cascade=rs.two_stage();
  r.fs=review2::prototype_rate(rs,in,out); r.taps=h.size();
  const auto m=rta::dsp::preset_metrics(p,in,in==96000?40000:20000);
  r.passHz=m.passHz; r.stopHz=in-m.passHz; r.target=m.ceilingDb;
  r.excluded=m.excluded;
  r.finite=!h.empty() && std::all_of(h.begin(),h.end(),[](double v){return std::isfinite(v);});
  return h;
}
inline void measure(const std::vector<double>& h, Result& r, unsigned factor=16,
                    bool structureOnly=false) {
  if(!r.finite) return;
  r.dcDb=direct(h,0,r.fs);
  if(!std::isfinite(r.dcDb)||std::abs(r.dcDb)>1e-4) {r.finite=false;return;}
  if(structureOnly) {r.status="STRUCTURE_ONLY";r.kind="NOT_MEASURED";return;}
  if(factor<16||factor>64) throw std::runtime_error("FFT factor must be 16..64");
  size_t n=1;
  while(n<h.size()*factor) {
    n*=2;
    if(n>(size_t(1)<<25)) throw std::runtime_error("FFT >2^25; use --points for segmented direct measurements");
  }
  r.fftN=n;
  std::vector<DC> a(n);
  for(size_t i=0;i<h.size();++i) a[i].r.h=h[i];
  fft(a);
  const size_t first=(size_t)std::ceil(r.stopHz/r.fs*n), last=n/2;
  const double edge=r.stopHz;
  if(first>last||r.passHz>r.fs/2) throw std::runtime_error("invalid measurement frequency domain");
  r.stopBins=last-first+1;
  std::vector<std::pair<double,size_t>> candidates;
  for(size_t k=first;k<=last;++k) {
    const double v=mag(a[k]);
    if((k==first||v>=mag(a[k-1]))&&(k==last||v>=mag(a[k+1]))) candidates.push_back({v,k});
    if(db(v)>r.stopDb) {r.stopDb=db(v);r.stopHz=r.fs*k/n;}
  }
  auto stopAt=[&](double hz) {double v=direct(h,hz,r.fs);if(v>r.stopDb){r.stopDb=v;r.stopHz=hz;}};
  stopAt(edge);stopAt(r.fs/2);
  std::sort(candidates.begin(),candidates.end(),[](auto x,auto y){return x.first>y.first;});
  for(size_t i=0;i<std::min<size_t>(8,candidates.size());++i) {
    const double center=r.fs*candidates[i].second/n;
    for(int q=-8;q<=8;++q) {double hz=center+r.fs/n*q/8.;if(hz>=edge&&hz<=r.fs/2)stopAt(hz);}
  }
  for(size_t k=0;k<=last && r.fs*k/n<=r.passHz;++k) {
    double v=db(mag(a[k]));if(std::abs(v)>std::abs(r.passDb)){r.passDb=v;r.passWorstHz=r.fs*k/n;}
  }
  double endpoint=direct(h,r.passHz,r.fs);
  if(std::abs(endpoint)>std::abs(r.passDb)){r.passDb=endpoint;r.passWorstHz=r.passHz;}
  // Values inside this small numerical guard band need a 90-digit witness.
  // The guard never relaxes the stated target into a pass.
  classify(r);
}
}
