#include "dsp/resampler.h"
#include <cstdio>
#include <limits>
#include <vector>
#include <cstring>
using namespace rta::dsp;
static int failures=0,checks=0;
void check(bool ok,const char* label){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",label);}
template<class Change> void invalid(Change change,const char* label){
    ResamplerConfig c;c.inRate=44100;c.outRate=88200;c.channels=1;c.minTapsPerInput=4;
    change(c);bool rejected=false;
    try{PolyphaseResampler r(c);}catch(const std::exception&){rejected=true;}
    check(rejected,label);
}
int main(){
    const double nan=std::numeric_limits<double>::quiet_NaN();
    invalid([&](auto&c){c.transition=nan;},"NaN transition is rejected before design");
    invalid([&](auto&c){c.attenDb=nan;},"NaN attenuation is rejected before design");
    invalid([](auto&c){c.transition=-0.1;},"negative transition is rejected");
    invalid([](auto&c){c.transition=1.1;},"transition beyond Nyquist is rejected");
    invalid([](auto&c){c.channels=0;},"invalid channels cannot silently become stereo");
    invalid([](auto&c){c.totalTaps=-1;},"negative total taps are rejected");
    invalid([](auto&c){c.stage2Taps=-1;},"negative cascade taps are rejected");
    invalid([](auto&c){c.minTapsPerInput=-1;},"negative source tap budget is rejected");
    invalid([](auto&c){c.maxPrototypeTaps=-1;},"negative coefficient budget is rejected");
    invalid([&](auto&c){c.apodizing=true;c.apodF1=nan;c.apodF2=.99;},"nonfinite apodization is rejected");
#ifdef RTA_PLAN_CHECKS
    for(auto rates:{std::pair{44100u,384000u},{44100u,96000u},{48000u,176400u},{96000u,352800u},{44100u,48000u},{96000u,44100u},{44100u,705600u}}){
        ResamplerConfig c;c.inRate=rates.first;c.outRate=rates.second;c.channels=1;c.minTapsPerInput=211;c.allowAutomaticCascade=true;
        const auto plan=plan_resampler(c);PolyphaseResampler r(c);
        check(plan.stageCount==(r.two_stage()?2u:1u),"allocation-free plan matches actual stage count");
        check(plan.coefficientCount==r.coefficient_count(),"planned coefficient count matches real stored object");
        for(size_t k=0;k<plan.stageCount;++k){
            auto* stage=r.two_stage()?(k?r.second_stage():r.first_stage()):&r;
            check(plan.stages[k].T==stage->tapsPerPhase(),"stage tap plan matches construction");
            check(plan.stages[k].L==stage->upFactor()&&plan.stages[k].M==stage->downFactor(),"stage rational clocks match construction");
        }
        check(plan.designPeakBytes>=plan.coefficientCount*sizeof(double),"prewarm bound covers persistent coefficients");
    }
    ResamplerConfig c;c.inRate=44100;c.outRate=384000;c.minTapsPerInput=32767;c.allowAutomaticCascade=true;
    auto p=plan_resampler(c);check(p.coefficientCount<200000,"extreme rational source support stays below 200k coefficients");
    c.allowAutomaticCascade=false;bool rejected=false;try{(void)plan_resampler(c);}catch(const std::exception&){rejected=true;}
    check(rejected,"explicit single-stage exceeds budget instead of silently shortening quality");
    c.inRate=c.outRate=44100;auto bypass=plan_resampler(c);
    check(bypass.stageCount==0&&bypass.coefficientCount==0&&bypass.designPeakBytes==0,"bypass plan needs no coefficient or FFT allocation");
    c.inRate=44100;c.outRate=384000;c.twoStage=true;c.minTapsPerInput=INT_MAX;c.maxPrototypeTaps=0;
    rejected=false;try{(void)plan_resampler(c);}catch(const std::exception&){rejected=true;}
    check(rejected,"cascade tap multiplication cannot overflow signed int");
    c={};c.inRate=44100;c.outRate=88200;c.transition=1e-300;
    rejected=false;try{(void)plan_resampler(c);}catch(const std::exception&){rejected=true;}
    check(rejected,"unrepresentable transition-derived count fails before floating-to-int cast");
    c={};c.inRate=44100;c.outRate=384000;c.channels=1;c.phase=PhaseKind::Intermediate;c.attenDb=284;c.minTapsPerInput=211;c.allowAutomaticCascade=true;
    set_phase_mix(.3);const auto frozen=plan_resampler(c);
    check(frozen.stageCount==2&&frozen.stages[0].config.designPhaseMix==.3&&frozen.stages[1].config.designPhaseMix==.3,"both cascade stages freeze one phase-mix snapshot");
    PolyphaseResampler before(frozen.stages[0].config);std::vector<double> hBefore;before.prototype_into(hBefore);
    set_phase_mix(.8);PolyphaseResampler after(frozen.stages[0].config);std::vector<double> hAfter;after.prototype_into(hAfter);
    check(hBefore.size()==hAfter.size()&&std::memcmp(hBefore.data(),hAfter.data(),hBefore.size()*sizeof(double))==0,"global UI mix changes cannot alter a prepared stage or its cache identity");
    set_phase_mix(.5);
#endif
    std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
