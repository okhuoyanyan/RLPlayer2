#include "dsp/resampler.h"
#include <cstdio>
#include <limits>
using namespace rta::dsp;
int main(){
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* label){++checks;failures+=!ok;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
    for(auto pair:{std::pair{44100u,88200u},std::pair{44100u,96000u},std::pair{96000u,44100u}}){
        ResamplerConfig c;c.inRate=pair.first;c.outRate=pair.second;c.channels=1;c.minTapsPerInput=8;c.applyTapFloor=false;
        PolyphaseResampler r(c);
        for(size_t n:{size_t(0),size_t(1),size_t(4096),size_t(1ull<<53)+1}){
            const size_t q=n/size_t(r.downFactor()),rem=n%size_t(r.downFactor());
            const size_t expected=q*size_t(r.upFactor())+rem*size_t(r.upFactor())/size_t(r.downFactor())+r.upFactor()+8;
            check(r.maxOutFrames(n)==expected,"capacity uses exact integer clocks beyond binary64 precision");
        }
        if(pair.second>pair.first){
            bool rejected=false;try{(void)r.maxOutFrames(std::numeric_limits<size_t>::max());}catch(const std::length_error&){rejected=true;}
            check(rejected,"unrepresentable output capacity rejected");
        }
    }
    std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
