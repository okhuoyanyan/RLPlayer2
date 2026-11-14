// Offline tone-gain witnesses through the production modulator. No device.
// Coherent Hann I/Q includes a separate ideal-input reference with the SAME
// weighting. A point screen cannot certify broadband SDM quality.
#include "dsp/modulator.h"
#include <cstdio>
#include <cmath>
#include <chrono>
using namespace rta::dsp;
int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const size_t n=1u<<19;
    const auto family=argc>1?parse_modulator_family(argv[1]):ModulatorFamily::Dsd;
    std::puts("rate,corner,requested,effective,amplitude,tone_hz,gain_db,phase_rad,overloads,clamps,nonfinite");
    for(double rate:{2822400.,11289600.,45158400.,49152000.})
    for(double corner:{20000.,50000.,100000.})
    for(int order:{5,7}) {
        if(family==ModulatorFamily::Ec&&corner!=100000.)continue;
        SdmConfig c;c.fs=rate;c.cutoffHz=corner;c.order=order;c.oneBitOutput=true;c.ditherLsb=.0005;c.family=family;
        for(double amp:{.01,.62})for(double hz:{1000.,10000.,19000.}) {
            SdmModulator m(c);
            const size_t k=size_t(std::llround(hz*n/rate));const double f=double(k)*rate/n;
            double yr=0,yi=0,xr=0,xi=0;
            for(size_t i=0;i<2*n;++i){
                const double phase=2*kPi*double(k)*double(i)/double(n),sn=std::sin(phase),cs=std::cos(phase),x=amp*sn;
                const double y=m.process(x);
                if(i>=n){const double w=.5-.5*std::cos(2*kPi*double(i-n)/double(n));
                    yr+=y*w*sn;yi+=y*w*cs;xr+=x*w*sn;xi+=x*w*cs;}
            }
            std::printf("%.0f,%.0f,%d,%d,%.8g,%.12g,%.12g,%.12g,%lld,%lld,%lld\n",rate,corner,order,m.effectiveOrder(),amp,f,
                20*std::log10(std::hypot(yr,yi)/std::hypot(xr,xi)),std::atan2(yi,yr)-std::atan2(xi,xr),m.overload(),m.clamped(),m.nonFinite());
        }
    }
}
