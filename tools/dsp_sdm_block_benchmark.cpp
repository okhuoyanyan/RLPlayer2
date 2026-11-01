#include "dsp/modulator.h"
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <intrin.h>
using namespace rta::dsp;
int main(int argc,char**argv) {
    SdmConfig cfg;cfg.order=argc>1?std::atoi(argv[1]):5;
    cfg.family=argc>2?parse_modulator_family(argv[2]):ModulatorFamily::Dsd;
    const int ratio=argc>3?std::atoi(argv[3]):1024;cfg.fs=44100.*ratio;
    cfg.ditherLsb=argc>4?std::atof(argv[4]):0.;cfg.oneBitOutput=true;
    const size_t frames=argc>5?std::strtoull(argv[5],nullptr,10):1048576;
    const int repeats=argc>6?std::atoi(argv[6]):7;
    if(frames<100000||repeats<3)return 2;
    std::vector<double> input(frames),a(frames),b(frames);
    for(size_t i=0;i<frames;++i)input[i]=.42*std::sin(.0031*(double)i)+.17*std::sin(.00071*(double)i);
    // Warm code/cache; timing excludes construction and output comparison.
    { SdmModulator warm(cfg);warm.process_block(input.data(),frames,[&](size_t i,double v){a[i]=v;}); }
    std::vector<double> scalarNs,batchNs,scalarTicks,batchTicks;size_t mismatches=0;
    auto run=[&](bool batch) {
      SdmModulator mod(cfg);auto&out=batch?b:a;unsigned aux=0;
      const auto start=std::chrono::steady_clock::now();
      _mm_mfence();_mm_lfence();const uint64_t before=__rdtscp(&aux);_mm_lfence();
      if(batch)mod.process_block(input.data(),frames,[&](size_t i,double v){out[i]=v;});
      else for(size_t i=0;i<frames;++i)out[i]=mod.process(input[i]);
      _mm_mfence();_mm_lfence();const uint64_t after=__rdtscp(&aux);_mm_lfence();
      const auto end=std::chrono::steady_clock::now();
      const double ns=std::chrono::duration<double,std::nano>(end-start).count()/frames;
      const double ticks=(double)(after-before)/frames;
      (batch?batchNs:scalarNs).push_back(ns);(batch?batchTicks:scalarTicks).push_back(ticks);
      std::printf("repeat=%zu path=%s requested=%d effective=%d family=%s ratio=%d dither=%g ns_per_sample=%.9f tsc_ticks_per_sample=%.9f clamp=%lld overload=%lld nonfinite=%lld\n",
       scalarNs.size()+batchNs.size(),batch?"batch":"scalar",cfg.order,mod.effectiveOrder(),
       modulator_family_name(cfg.family),ratio,cfg.ditherLsb,ns,ticks,mod.clamped(),mod.overload(),mod.nonFinite());
    };
    for(int r=0;r<repeats;++r) {
      if(r&1){run(true);run(false);}else{run(false);run(true);}
      for(size_t i=0;i<frames;++i)if(std::bit_cast<uint64_t>(a[i])!=std::bit_cast<uint64_t>(b[i]))++mismatches;
    }
    auto median=[](std::vector<double> v){std::sort(v.begin(),v.end());return v[v.size()/2];};
    double nsA=median(scalarNs),nsB=median(batchNs);
    std::printf("RESULT frames=%zu repeats=%d compared=%zu mismatches=%zu scalar_ns=%.9f batch_ns=%.9f speedup=%.9f scalar_Msamples_s=%.9f batch_Msamples_s=%.9f scalar_tsc_ticks=%.9f batch_tsc_ticks=%.9f\n",
      frames,repeats,frames*repeats,mismatches,nsA,nsB,nsA/nsB,1000/nsA,1000/nsB,median(scalarTicks),median(batchTicks));
    return mismatches?1:0;
}
