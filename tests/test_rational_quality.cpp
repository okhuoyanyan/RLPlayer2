#include "engine/pipeline.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <limits>
using namespace rta::dsp;

double response(const PolyphaseResampler& rs, double hz) {
    if (rs.two_stage()) return response(*rs.first_stage(), hz) + response(*rs.second_stage(), hz);
    std::vector<double> h; rs.prototype_into(h);
    const double domain=double(rs.config().inRate)*rs.upFactor();
    return magnitude_db_exact(h,2*std::remainder(hz,domain)/domain)-20*std::log10(double(rs.upFactor()));
}
int run(int argc,char**argv) {
    const char* name=argc>1?argv[1]:"sinc-MGa";
    const unsigned out=argc>2?unsigned(std::stoul(argv[2])):384000;
    const unsigned in=argc>3?unsigned(std::stoul(argv[3])):44100;
    const auto* preset=find_preset(name);
    if(!preset)return 2;
    rta::engine::PipelineConfig cfg;
    cfg.outRate=out; cfg.autoTaps=true; cfg.useFixed=false; cfg.gpuResample=false;
    rta::engine::apply_filter_preset(cfg,*preset);
    rta::engine::Pipeline pipeline(cfg);
    rta::audio::AudioBuffer input;
    input.rate=in; input.bits=24; input.channels=1; input.isFloat=false; input.raw={0};
    std::vector<std::vector<double>> actual;
    pipeline.process(input,nullptr,&actual);
    const auto* rs=pipeline.prepared_resampler();
    if(!rs)return 3;
    const unsigned limitingRate=std::min(in,out);
    const auto metric=preset_metrics(*preset,limitingRate,content_hz_for_rate(limitingRate));
    std::printf("preset=%s rate=%u/%u cascade=%d L=%d M=%d T1=%d T2=%d coeff=%zu pass=%.9g stop=%.9g target=%.9g\n",
        name,in,out,rs->two_stage(),rs->upFactor(),rs->downFactor(),rs->stage1_taps_per_phase(),rs->stage2_taps_per_phase(),rs->coefficient_count(),metric.passHz,limitingRate-metric.passHz,metric.ceilingDb);
    const auto first=rs->first_stage(); const auto second=rs->second_stage();
    if(first) std::printf("stage1=%u/%u L=%d M=%d; stage2=%u/%u L=%d M=%d\n",first->config().inRate,first->config().outRate,first->upFactor(),first->downFactor(),second->config().inRate,second->config().outRate,second->upFactor(),second->downFactor());
    bool finite=true;
    for(double hz : {0.0,1000.0,10000.0,20000.0,metric.passHz,limitingRate-metric.passHz,30000.0,44100.0,60000.0,100000.0}) {
        const double db=response(*rs,hz);
        std::printf("witness_hz=%.17g response_db=%.17g\n",hz,db);
        finite=finite&&std::isfinite(db);
    }
    // This executable exports finite-point witnesses, not a continuous-band
    // certificate. Absolute acceptance is performed by an independent oracle.
    return finite?0:1;
}
int main(int argc,char**argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try { return run(argc,argv); }
    catch(const std::exception& e) { std::fprintf(stderr,"ERROR %s\n",e.what()); return 2; }
}
