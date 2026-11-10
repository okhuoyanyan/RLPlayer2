#include "dsp/modulator.h"
#include <cstdio>
#include <limits>
using namespace rta::dsp;
int main(){int checks=0,failures=0;
    auto invalid=[&](auto change,const char* name){SdmConfig c;change(c);bool rejected=false;
        try{SdmModulator m(c);}catch(const std::invalid_argument&){rejected=true;}
        ++checks;failures+=!rejected;std::printf("%s %s\n",rejected?"PASS":"FAIL",name);};
    const double nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
    invalid([](auto& c){c.order=0;},"zero order rejected");
    invalid([](auto& c){c.order=9;},"order above fixed history capacity rejected");
    invalid([](auto& c){c.fs=0;},"zero clock rejected");
    invalid([&](auto& c){c.fs=nan;},"NaN clock rejected");
    invalid([&](auto& c){c.fs=inf;},"infinite clock rejected");
    invalid([&](auto& c){c.cutoffHz=nan;},"NaN corner rejected");
    invalid([](auto& c){c.cutoffHz=0;},"zero corner rejected");
    invalid([](auto& c){c.cutoffHz=c.fs*.5;},"corner at Nyquist rejected");
    invalid([&](auto& c){c.ditherLsb=nan;},"NaN dither rejected");
    invalid([](auto& c){c.ditherLsb=-1;},"unresolved negative dither rejected");
    invalid([&](auto& c){c.ntfHinf=inf;},"nonfinite NTF budget rejected");
    invalid([&](auto& c){c.oobGainDb=nan;},"nonfinite Chebyshev budget rejected");
    invalid([&](auto& c){c.minOsrForHighOrder=nan;},"NaN order guard rejected");
    invalid([](auto& c){c.minOsrForHighOrder=-1;},"negative order guard rejected");
    invalid([](auto& c){c.family=ModulatorFamily(100);},"unknown family rejected");
    for(int order=1;order<=8;++order){SdmConfig c;c.order=order;c.cutoffHz=100000;SdmModulator m(c);bool valid=true;
        for(int n=0;n<256;++n){double y=m.process(0.);valid&=y==1.||y==-1.;}
        ++checks;failures+=!valid;}
    std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
