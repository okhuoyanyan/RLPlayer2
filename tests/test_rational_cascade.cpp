#include "dsp/resampler.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
using namespace rta::dsp;
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
std::vector<double> stream(ResamplerConfig cfg, const std::vector<double>& x, size_t block) {
    PolyphaseResampler rs(cfg);
    std::vector<double> result;
    for (size_t pos=0; pos<x.size(); pos+=block) {
        const size_t n=std::min(block,x.size()-pos);
        std::vector<double> out(rs.maxOutFrames(n));
        const double* input[]={x.data()+pos}; double* output[]={out.data()};
        const auto got=rs.process(input,n,output);
        result.insert(result.end(),out.begin(),out.begin()+got);
    }
    return result;
}
int main() {
    for (auto rate : {std::pair{44100u,96000u}, {44100u,384000u}, {48000u,176400u}, {96000u,352800u}}) {
        ResamplerConfig cfg;
        cfg.inRate=rate.first; cfg.outRate=rate.second; cfg.channels=1;
        cfg.minTapsPerInput=256; cfg.attenDb=180; cfg.twoStage=true;
        PolyphaseResampler rs(cfg);
        std::printf("rate=%u/%u stages=%d coeff=%zu\n",cfg.inRate,cfg.outRate,rs.two_stage()?2:1,rs.coefficient_count());
        check(rs.two_stage(),"rational ratio uses the requested cascade");
        if (!rs.two_stage()) continue;
        const auto* a=rs.first_stage(); const auto* b=rs.second_stage();
        check(uint64_t(a->upFactor())*b->upFactor()*rs.downFactor()==
              uint64_t(rs.upFactor())*a->downFactor()*b->downFactor(),"exact rational factor product");
        check(!b->config().useFixed,"intermediate samples are never quantized to int24");
        check(rs.coefficient_count()<size_t(cfg.minTapsPerInput)*rs.upFactor(),"actual coefficient storage decreases");
        const size_t n=3071+size_t(std::ceil(rs.tailInputSamples()))+2;
        std::vector<double> x(n);
        for (size_t i=0;i<3071;++i) x[i]=0.25*std::sin(double(i)*0.371)+0.1*std::cos(double(i)*0.0331);
        const auto ref=stream(cfg,x,x.size());
        check(ref.size()==(uint64_t(n)*rs.upFactor()+rs.downFactor()-1)/rs.downFactor(),"complete ceil-rational output frame ledger including flushed tail");
        for (size_t chunk : {1u,17u,333u,777u}) {
            const auto actual=stream(cfg,x,chunk);
            check(actual.size()==ref.size(),"chunk boundaries preserve all output frames");
            check(actual.size()==ref.size() && std::memcmp(actual.data(),ref.data(),ref.size()*sizeof(double))==0,"whole output and tail are bit-identical across chunking");
        }
        rs.reset();
        const double inputValue=0.5; const double* ip[]={&inputValue};
        std::vector<double> resetOut(rs.maxOutFrames(1)); double* op[]={resetOut.data()};
        const auto got=rs.process(ip,1,op);
        const auto clean=stream(cfg,std::vector<double>{inputValue},1);
        check(got==clean.size() && std::memcmp(clean.data(),resetOut.data(),got*sizeof(double))==0,"reset restores both stage phases/history");
    }
    std::printf("failures=%d\n",failures);
    return failures ? 1 : 0;
}
