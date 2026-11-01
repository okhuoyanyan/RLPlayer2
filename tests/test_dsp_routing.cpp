// Explicit DSD routing, production configuration parity, and resource limits.
#include "engine/pipeline.h"
#include <iostream>
using namespace rta::dsp;
using namespace rta::engine;
#define REQUIRE(x) do {if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";return 1;}}while(false)
int main() {
    size_t cases=0;
    for(const auto& p:filter_presets()) {
        ResamplerConfig rc;rc.inRate=44100;rc.outRate=45158400;rc.channels=1;
        apply_preset(rc,p);PipelineConfig pc;apply_filter_preset(pc,p);
        REQUIRE(pc.taps==rc.totalTaps&&pc.attenDb==rc.attenDb&&pc.transition==rc.transition);
        REQUIRE(pc.filterWindow==rc.window&&pc.filterPhase==rc.phase);
        REQUIRE(pc.apodizing==rc.apodizing&&pc.apodF1==rc.apodF1&&pc.apodF2==rc.apodF2);
        REQUIRE(pc.minTapsPerInput==rc.minTapsPerInput&&pc.twoStage==rc.twoStage);
        apply_resample_purpose(rc,ResamplePurpose::Pcm);
        REQUIRE(rc.twoStage==p.twoStage); // named -2s behavior is retained for PCM
        apply_resample_purpose(rc,ResamplePurpose::Dsd);
        apply_resample_purpose(pc,ResamplePurpose::Dsd);
        REQUIRE(rc.twoStage&&pc.twoStage);
        if(!rc.minTapsPerInput)rc.minTapsPerInput=solve_taps_per_phase(p.window,p.transition,
            p.apodizing,p.apodF1,p.apodF2,p.taps,0,44100);
        PolyphaseResampler r(rc);
        REQUIRE(r.two_stage()&&r.stage1_taps_per_phase()==rc.minTapsPerInput);
        REQUIRE(r.stage2_taps_per_phase()==rc.stage2Taps);
        REQUIRE(r.first_stage()->config().outRate*r.second_stage()->upFactor()==rc.outRate);
        if(p.apodizing) {
            REQUIRE(rc.minTapsPerInput>=p.tapsPerInput);
            REQUIRE(rc.minTapsPerInput==solve_taps_per_phase(p.window,p.transition,true,
                p.apodF1,p.apodF2,p.taps,p.tapsPerInput,44100));
        }
        ++cases;
    }
    ResamplerConfig limit;limit.inRate=44100;limit.outRate=45158400;
    limit.channels=1;limit.minTapsPerInput=3000;limit.maxPrototypeTaps=1<<21;
    bool rejected=false;
    try { PolyphaseResampler tooLarge(limit); } catch(const std::runtime_error&) { rejected=true; }
    REQUIRE(rejected); // budget may never silently lower the requested quality
    apply_resample_purpose(limit,ResamplePurpose::Dsd);PolyphaseResampler bounded(limit);
    REQUIRE(bounded.stage1_taps_per_phase()==3000&&bounded.two_stage());
    for(const char* name:{"poly-sinc-short-lp","apod-20k-tight","poly-sinc-long-ip"}) {
        PipelineConfig cfg;apply_filter_preset(cfg,*find_preset(name));
        apply_resample_purpose(cfg,ResamplePurpose::Dsd);
        cfg.autoTaps=true;cfg.outRate=45158400;cfg.outBits=32;cfg.gpuResample=false;
        Pipeline pipe(cfg);rta::audio::AudioBuffer in;
        in.rate=44100;in.bits=24;in.channels=1;in.raw={0,1,0,-1,0,0,0};
        std::vector<std::vector<double>> planes;PipelineStats stats;
        pipe.process(in,&stats,&planes);
        REQUIRE(pipe.twoStageActive()&&planes.size()==1&&planes[0].size()==7*1024);
        REQUIRE(stats.usedResample && stats.tapsPerPhase>0 && stats.stage2TapsPerPhase==64);
        REQUIRE(stats.coeffBytes>0);
        for(double v:planes[0])REQUIRE(std::isfinite(v));
    }
    std::cout<<"PASS "<<cases<<" preset mappings and DSD1024 constructors; budget rejection and actual Pipeline cascade\n";
}
