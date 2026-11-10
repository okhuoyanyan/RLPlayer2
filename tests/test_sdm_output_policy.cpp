// Offline product admission: raw research modulators remain available so the
// historical low-corner failure can still be reproduced, never relabelled green.
#include "dsp/sdm_output_policy.h"
#include "dsp/dsd_rate_convert.h"
#include <iostream>
#include <limits>
#include <string>

using namespace rta::dsp;
int main() {
    int checks=0, failures=0;
    auto check=[&](bool pass,const char* name){++checks;if(!pass){++failures;std::cout<<"FAIL "<<name<<'\n';}};
    auto rejected=[](ModulatorFamily family,bool ec,double hz){
        try{validate_one_bit_sdm_corner(family,ec,hz);return false;}
        catch(const std::invalid_argument& e){return std::string(e.what()).find("SDM_CORNER_QUALITY:")==0;}
    };
    for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Chebyshev,ModulatorFamily::Ec,ModulatorFamily::Ahm})
    for(bool correction:{false,true})for(double corner:{20000.,50000.,99999.,100000.,150000.,200000.}){
        const bool ec = family==ModulatorFamily::Ec || (family!=ModulatorFamily::Chebyshev&&correction);
        check(rejected(family,correction,corner)==(!ec&&corner<100000.),"exact family/corner admission, including EC precedence");
    }
    for(double bad:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        bool threw=false;try{validate_one_bit_sdm_corner(ModulatorFamily::Dsd,false,bad);}catch(const std::invalid_argument&){threw=true;}
        check(threw,"nonfinite/nonpositive input is not silently accepted");
    }
    // The offline constructor intentionally retains the failed historical NTF.
    SdmConfig raw;raw.fs=45158400.;raw.cutoffHz=20000.;raw.oneBitOutput=true;
    SdmModulator research(raw);
    check(research.ntf().order==5&&research.levels()==2,"research path remains reproducible, not silently substituted");
    for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Chebyshev,ModulatorFamily::Ahm})
    for(unsigned rate:{2822400u,3072000u})for(unsigned factor:{2u,16u})for(double corner:{20000.,50000.}) {
        DsdRateConvertConfig cfg;cfg.modFamily=family;cfg.modCutoffHz=corner;
        bool blocked=false;
        try{DsdRateConverter converter(plan_dsd_rate(rate,rate*factor),cfg,2);}
        catch(const std::invalid_argument& e){blocked=std::string(e.what()).find("SDM_CORNER_QUALITY:")==0;}
        check(blocked,"actual DSD-to-DSD output rejects low corner before constructing DSP");
    }
    DsdRateConvertConfig passthrough;passthrough.modFamily=ModulatorFamily::Dsd;passthrough.modCutoffHz=20000.;
    bool directAllowed=true;
    try{DsdRateConverter converter(plan_dsd_rate(2822400u,2822400u),passthrough,2);}
    catch(...){directAllowed=false;}
    check(directAllowed,"direct bit-preserving DSD path has no modulator and remains available");
    DsdRateConvertConfig ecLow;ecLow.modFamily=ModulatorFamily::Ec;ecLow.modCutoffHz=20000.;
    bool ecAllowed=true;
    try{DsdRateConverter converter(plan_dsd_rate(2822400u,5644800u),ecLow,2);}
    catch(...){ecAllowed=false;}
    check(ecAllowed,"actual EC conversion ignores low corner and remains available");
    std::cout<<"checks="<<checks<<" failures="<<failures<<" device_calls=0\n";
    return failures?1:0;
}
