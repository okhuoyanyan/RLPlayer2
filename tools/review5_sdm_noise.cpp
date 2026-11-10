// Actual production one-bit SDM output. The separate Python reader performs
// coherent, periodic-Hann FFT analysis. Offline only; no device API is called.
#include "dsp/modulator.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace rta::dsp;
int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"review5_sdm_noise <new-output-directory>\n";return 2;}
    const auto root=std::filesystem::u8path(argv[1]);
    if(std::filesystem::exists(root)){std::cerr<<"Refuse to replace prior evidence\n";return 2;}
    std::filesystem::create_directories(root);
    std::ofstream index(root/"index.csv");
    index<<"file,family,rate,requested,effective,amplitude,bin,frames,settle,overloads,clamps,nonfinite\n";
    int id=0;
    for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Ec})
    for(unsigned rate:{2822400u,45158400u})for(int order:{5,7})
    for(double amp:{.01,.62})for(double hz:{1000.,10000.,19000.}){
        const size_t count=size_t(1u<<18)*(rate/2822400u),settle=count/2;
        const size_t bin=size_t(std::llround(hz*double(count)/rate));
        SdmConfig c;c.fs=rate;c.cutoffHz=100000;c.order=order;c.family=family;c.oneBitOutput=true;c.ditherLsb=.0005;
        SdmModulator m(c);std::vector<int8_t> y(count);
        for(size_t i=0;i<count+settle;++i){
            const double x=amp*std::sin(2*kPi*double(bin)*double(i)/double(count));
            const double v=m.process(x);
            if(v!=-1.0&&v!=1.0){std::cerr<<"Not one-bit output\n";return 1;}
            if(i>=settle)y[i-settle]=v>0?1:-1;
        }
        const auto name="cell-"+std::to_string(++id)+".i8";
        std::ofstream out(root/name,std::ios::binary);out.write(reinterpret_cast<const char*>(y.data()),y.size());
        if(!out){std::cerr<<"Short evidence write\n";return 1;}
        index<<name<<','<<(family==ModulatorFamily::Ec?"ec":"dsd")<<','<<rate<<','<<order<<','<<m.effectiveOrder()<<','
             <<std::setprecision(17)<<amp<<','<<bin<<','<<count<<','<<settle<<','<<m.overload()<<','<<m.clamped()<<','<<m.nonFinite()<<'\n';
        index.flush();std::cout<<"written "<<id<<" frames="<<count<<std::endl;
    }
    return index?0:1;
}
