#include "dsp/filter_families.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace rta::dsp;
static int checks=0,failures=0;
template<class F> void rejects(F f,const char* label){
    bool rejected=false;
    try{f();}catch(const std::invalid_argument&){rejected=true;}catch(const std::length_error&){rejected=true;}
    ++checks;if(!rejected)++failures;
    std::printf("%s %s\n",rejected?"PASS":"FAIL",label);
}
int main(){
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double inf=std::numeric_limits<double>::infinity();
    FilterPreset p{};p.transition=.1;p.taps=4095;p.window=WindowKind::Kaiser;
    for(double rate:{0.,-1.,nan,inf,1e300})
        rejects([&]{(void)preset_metrics(p,rate,20000.);},"invalid source rate rejected before arithmetic/casts");
    for(double transition:{-.1,1.,nan,inf}){
        auto bad=p;bad.transition=transition;
        rejects([&]{(void)preset_metrics(bad,44100.,20000.);},"invalid transition rejected");
    }
    rejects([&]{(void)preset_metrics(p,44100.,nan);},"nonfinite content boundary rejected");
    rejects([&]{(void)preset_metrics(p,44100.,20000.,-1.);},"negative boundary tolerance rejected");
    auto narrow=p;narrow.transition=0.;
    rejects([&]{(void)preset_metrics(narrow,4294967295.,20000.);},"unrepresentable tap count rejected before signed cast");
    rejects([&]{(void)solve_taps_per_phase(WindowKind::Kaiser,.1,false,0,0,4095,1,nan);},"explicit taps do not bypass rate validation");
    rejects([&]{(void)to_minimum_phase({1.0},0);},"zero FFT padding rejected");
    rejects([&]{(void)make_window(static_cast<WindowKind>(99),4,280);},"unknown window cannot silently become rectangular");
    rejects([&]{(void)make_window(WindowKind::Gaussian,4,nan);},"nonfinite attenuation rejected");
#ifdef RTA_VALIDATION_OVERFLOW_CHECK
    rejects([&]{(void)to_minimum_phase({.5,.5},std::numeric_limits<size_t>::max()/2+1);},"FFT target multiplication overflow rejected");
    rejects([&]{(void)to_minimum_phase({1.0},std::numeric_limits<size_t>::max());},"FFT power-of-two overflow rejected");
#endif
    ++checks;if(!to_minimum_phase({}).empty())++failures;
    ++checks;if(preset_metrics(p,44100.,20000.).T!=211)++failures;
    std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
