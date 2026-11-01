#pragma once
#include "dsp_measure.h"
#include <functional>
#include <queue>
#include <cstring>
#include <future>
namespace dsp_measure {
struct StageData {
  Result result;
  rta::dsp::ResamplerConfig config;
  std::vector<double> first,second;
  unsigned f1=0,f2=0,in=0,out=0;
};
inline StageData production_stages(const rta::dsp::FilterPreset& p,unsigned in,unsigned out) {
  StageData s;s.in=in;s.out=out;
  auto cfg=review2::production_config(p,in,out);
  rta::dsp::apply_resample_purpose(cfg,rta::dsp::ResamplePurpose::Dsd);
  rta::dsp::PolyphaseResampler rs(cfg);
  s.config=rs.config();
  const auto* a=rs.first_stage();const auto*b=rs.second_stage();
  if(!a||!b||rs.downFactor()!=1)throw std::runtime_error("DSD staged measurement requires actual integer-ratio cascade");
  s.f1=a->upFactor();s.f2=b->upFactor();
  a->prototype_into(s.first);b->prototype_into(s.second);
  for(auto&x:s.first)x/=s.f1;
  for(auto&x:s.second)x/=s.f2;
  auto&m=s.result;m.T=a->tapsPerPhase();m.stage2T=b->tapsPerPhase();m.L=rs.upFactor();m.M=rs.downFactor();
  m.fs=out;m.taps=(s.first.size()-1)*s.f2+s.second.size();
  auto spec=rta::dsp::preset_metrics(p,in,in==96000?40000:20000);
  m.passHz=spec.passHz;m.stopHz=in-spec.passHz;m.target=spec.ceilingDb;m.excluded=spec.excluded;
  m.finite=!s.first.empty()&&!s.second.empty();
  for(auto&h:{&s.first,&s.second})for(double x:*h)m.finite=m.finite&&std::isfinite(x);
  return s;
}
struct ProcessEnvelope {
  std::vector<double> impulse;
  double deltaL1=0;
};
inline std::vector<double> actual_process_impulse(const StageData&s) {
  rta::dsp::PolyphaseResampler rs(s.config);std::vector<double> h;rs.prototype_into(h);
  for(auto&x:h)x/=rs.upFactor();return h;
}
inline ProcessEnvelope process_envelope(const StageData&s,std::vector<double> actual) {
  ProcessEnvelope e;e.impulse=std::move(actual);
  const size_t effective=(s.first.size()-1)*s.f2+s.second.size();
  DD sum;
  // The upsampled first kernel is sparse: at most ceil(N2/F2) products per
  // effective coefficient. No FFT and no large complex buffer are needed.
  for(size_t i=0;i<std::max(effective,e.impulse.size());++i) {
    DD ideal;
    size_t low=i>=s.second.size()?(i-s.second.size())/s.f2+1:0;
    size_t high=std::min(i/s.f2,s.first.size()-1);
    if(low<=high)for(size_t j=low;j<=high;++j)
      ideal=add(ideal,mul(DD{s.first[j],0},DD{s.second[i-j*s.f2],0}));
    const double real=i<e.impulse.size()?e.impulse[i]:0;
    DD error=add(ideal,{-real,0});
    sum=add(sum,{std::abs(error.h+error.l),0});
  }
  // A conservative numerical cushion (> DD accumulation error on these
  // bounded production kernels) is kept separate from the measured error.
  // This bounds the process-impulse response on the sampled grid, not arbitrary
  // signal-dependent floating arithmetic and not continuous extrema.
  e.deltaL1=std::nextafter(sum.h+sum.l+1e-25,std::numeric_limits<double>::infinity());
  return e;
}
inline double stage_direct(const StageData&s,double hz) {
  return direct(s.first,std::remainder(hz,double(s.in)*s.f1),double(s.in)*s.f1)+direct(s.second,hz,s.out);
}
inline bool equal_stage_coefficients(const StageData&a,const StageData&b) {
  return a.f1==b.f1&&a.f2==b.f2&&a.first.size()==b.first.size()&&a.second.size()==b.second.size()
    &&std::memcmp(a.first.data(),b.first.data(),a.first.size()*sizeof(double))==0
    &&std::memcmp(a.second.data(),b.second.data(),a.second.size()*sizeof(double))==0;
}
// Exact same frequency grid as the full effective FIR FFT, with O(N1) memory.
// Every bin j=k*F2+r is evaluated once. No interpolation or discarded bands.
inline size_t measure_stages(StageData&s,unsigned factor=16,
 std::function<void(size_t,DC)> observe={},bool progress=false) {
  auto&r=s.result;if(!r.finite)throw std::runtime_error("invalid stage coefficients");
  size_t n=1;while(n<factor*((s.first.size()-1)+(s.second.size()+s.f2-1)/s.f2))n*=2;
  if(n>(size_t(1)<<25)||factor<16||factor>64)throw std::runtime_error("unsupported streamed FFT size/factor");
  const size_t total=n*s.f2,last=total/2;
  r.fftN=total;r.stopBins=last-(size_t)std::ceil(r.stopHz/s.out*total)+1;
  const double edge=r.stopHz,passEdge=r.passHz;
  const size_t first=(size_t)std::ceil(edge/s.out*total),passLast=(size_t)std::floor(passEdge/s.out*total);
  std::vector<DC> a(n);
  for(size_t i=0;i<s.first.size();++i)a[i].r.h=s.first[i];
  fft(a);
  std::vector<double> prevprev(n),prev(n),cur(n),firstStripe,secondStripe;
  using Candidate=std::pair<double,size_t>;
  std::priority_queue<Candidate,std::vector<Candidate>,std::greater<Candidate>> peaks;
  auto candidate=[&](double amplitude,size_t j) {
    if(j<first||j>last)return;
    if(peaks.size()<8)peaks.push({amplitude,j});
    else if(amplitude>peaks.top().first){peaks.pop();peaks.push({amplitude,j});}
  };
  size_t bestJ=first;
  auto computeStripe=[&](unsigned rem) {
    std::vector<DC>b(n);
    DC rot=rotation(rem,double(total)),z{{1,0},{0,0}};
    for(size_t i=0;i<s.second.size();++i) {
      b[i%n]=add(b[i%n],{mul({s.second[i],0},z.r),mul({s.second[i],0},z.i)});
      z=mul(z,rot);
    }
    fft(b);
    return b;
  };
  const unsigned workers=observe?1:std::min(3u,s.f2);
  std::vector<std::future<std::vector<DC>>> pending(workers);
  if(workers>1)for(unsigned w=0;w<workers;++w)pending[w]=std::async(std::launch::async,computeStripe,w);
  for(unsigned rem=0;rem<s.f2;++rem) {
    auto b=workers==1?computeStripe(rem):pending[rem%workers].get();
    if(workers>1&&rem+workers<s.f2)pending[rem%workers]=std::async(std::launch::async,computeStripe,rem+workers);
    std::fill(cur.begin(),cur.end(),0);
    for(size_t k=0;k<n;++k) {
      const size_t j=k*s.f2+rem;if(j>last)break;
      DC value=mul(a[j%n],b[k]);cur[k]=mag(value);
      if(observe)observe(j,value);
      const double v=db(cur[k]);
      if(j>=first&&v>r.stopDb){r.stopDb=v;r.stopHz=double(s.out)*j/total;bestJ=j;}
      if(j<=passLast&&std::abs(v)>std::abs(r.passDb)){r.passDb=v;r.passWorstHz=double(s.out)*j/total;}
    }
    if(rem==0)firstStripe=cur;
    if(rem==1)secondStripe=cur;
    if(rem>=2)for(size_t k=0;k<n;++k) {
      const size_t j=k*s.f2+rem-1;if(j>last)break;
      if(prev[k]>=prevprev[k]&&prev[k]>=cur[k])candidate(prev[k],j);
    }
    prevprev.swap(prev);prev.swap(cur);
    if(progress&&(rem%s.f2==0||(rem+1)%std::max(1u,s.f2/8)==0))
      printf("STRIPES %u/%u fft_per_stripe=%zu full_grid=%zu\n",rem+1,s.f2,n,total);
  }
  for(size_t k=0;k<n;++k) {
    const size_t j=k*s.f2;if(j>last)break;
    if((k==0||firstStripe[k]>=prev[k-1])&&(j==last||firstStripe[k]>=secondStripe[k]))candidate(firstStripe[k],j);
    const size_t q=j+s.f2-1;if(q>last)continue;
    if(prev[k]>=prevprev[k]&&(q==last||prev[k]>=firstStripe[k+1]))candidate(prev[k],q);
  }
  candidate(std::pow(10.,r.stopDb/20),bestJ);
  auto stopAt=[&](double hz){double v=stage_direct(s,hz);if(v>r.stopDb){r.stopDb=v;r.stopHz=hz;}};
  stopAt(edge);stopAt(s.out*.5);
  while(!peaks.empty()) {
    const double center=double(s.out)*peaks.top().second/total;peaks.pop();
    for(int q=-8;q<=8;++q){double hz=center+double(s.out)/total*q/8.;if(hz>=edge&&hz<=s.out*.5)stopAt(hz);}
  }
  const double pv=stage_direct(s,passEdge);
  if(std::abs(pv)>std::abs(r.passDb)){r.passDb=pv;r.passWorstHz=passEdge;}
  r.dcDb=stage_direct(s,0);r.finite=std::isfinite(r.dcDb)&&std::abs(r.dcDb)<1e-4;
  classify(r);return n;
}
}
