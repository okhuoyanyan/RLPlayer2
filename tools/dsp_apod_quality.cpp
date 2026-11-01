#include "dsp/resampler.h"
#include "dsp/apod_quality.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
using namespace rta::dsp;
namespace fs=std::filesystem;
int main(int argc,char**argv) {
    if(argc<2) return 2;
    fs::path dir(argv[1]);fs::create_directories(dir);
    std::ofstream out(dir/"coefficients.csv");
    out<<"preset,inRate,outRate,L,T,oldT,N,F1,F2,window,attenDb,mode,file,latencySamples,twoStage,rateIdentityPairs\n";
    const int maxL=argc>2?std::stoi(argv[2]):32;
    const bool cascade=argc>3;
    for(const auto&p:filter_presets())if(p.apodizing) {
      const double atten=std::max(p.attenDb,preset_ceiling_db(p.window));
      int old=p.tapsPerInput>0?p.tapsPerInput:(p.window==WindowKind::Gaussian?
          p.taps/16:static_cast<int>(42.26/(p.apodF2-p.apodF1)+0.5));
      int quality=apod_quality_taps(p.apodF1,p.apodF2,p.window==WindowKind::Gaussian,atten,old);
      for(int L:{4,16,32,512,1024})if(L<=maxL)for(int mode=0;mode<2;++mode) {
        if(cascade && (mode==0 || L<8))continue;
        // At L=4 this is the actual stage-one design of a two-stage path.
        // Higher L is the actual single-stage prototype; no private design substitute.
        ResamplerConfig c;c.inRate=44100;c.outRate=44100*L;c.channels=1;
        apply_preset(c,p);c.twoStage=cascade;c.minTapsPerInput=mode?quality:old;
        PolyphaseResampler rs(c);std::vector<double> h;rs.prototype_into(h);
        int identityPairs=0;
        // Compare the real production tables/process impulse at all three
        // requested source-rate families, not a dimensional assumption alone.
        if(mode)for(unsigned rate:{48000u,96000u}) {
          auto other=c;other.inRate=rate;other.outRate=rate*L;
          PolyphaseResampler test(other);std::vector<double> copy;test.prototype_into(copy);
          if(copy!=h)throw std::runtime_error("source-rate coefficient identity failed");
          ++identityPairs;
        }
        for(auto&v:h)v/=L;
        auto file=std::string(p.name)+"-L"+std::to_string(L)+(mode?"-quality":"-old")+".f64";
        std::ofstream bin(dir/file,std::ios::binary);bin.write((char*)h.data(),h.size()*sizeof(double));
        out<<std::setprecision(17)<<p.name<<",44100,"<<44100*L<<','<<L<<','<<(cascade?c.minTapsPerInput:rs.tapsPerPhase())<<','<<old<<','<<h.size()<<','<<p.apodF1<<','<<p.apodF2<<','<<window_name(p.window)<<','<<atten<<','<<(mode?"quality":"old")<<','<<file<<','<<rs.latencyInputSamples()*L<<','<<rs.two_stage()<<','<<identityPairs<<'\n';
        std::cout<<file<<" T="<<rs.tapsPerPhase()<<" N="<<h.size()<<std::endl;
      }
    }
}
