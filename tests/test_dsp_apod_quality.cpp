#include "dsp/resampler.h"
#include "dsp/apod_quality.h"
#include <cstdio>
#include <cmath>
#include <exception>
using namespace rta::dsp;
int main() {
    int fails=0,checked=0,rejected=0;
    for(const auto&p:filter_presets())if(p.apodizing)for(int L:{4,16,32}) {
        auto evaluate=[&](int T){
            ResamplerConfig c;c.inRate=44100;c.outRate=c.inRate*L;c.channels=1;
            apply_preset(c,p);c.twoStage=false;c.minTapsPerInput=T;
            PolyphaseResampler r(c);std::vector<double> h;r.prototype_into(h);
            for(auto&v:h)v/=L;
            return magnitude_db_exact(h,p.apodF1/L);
        };
        // Frozen 3.0.6 automatic rule. Do not ask the now-fixed solver to
        // reconstruct the known-bad regression input.
        int old=p.tapsPerInput>0?p.tapsPerInput:(p.window==WindowKind::Gaussian?
            p.taps/16:static_cast<int>(42.26/(p.apodF2-p.apodF1)+0.5));
        double atten=std::max(p.attenDb,preset_ceiling_db(p.window));
        int quality=apod_quality_taps(p.apodF1,p.apodF2,p.window==WindowKind::Gaussian,atten,old);
        double before=evaluate(old),after=evaluate(quality);
        if(!(std::isfinite(after)&&std::abs(after)<0.006))++fails;
        if(std::abs(before)>0.01)++rejected;
        ++checked;
        std::vector<double> reference;
        for(unsigned rate:{44100u,48000u,96000u}) {
            ResamplerConfig c;c.inRate=rate;c.outRate=rate*L;c.channels=1;
            apply_preset(c,p);c.twoStage=false;c.minTapsPerInput=quality;
            PolyphaseResampler rs(c);std::vector<double> h;rs.prototype_into(h);
            if(reference.empty())reference=h;else if(reference!=h)++fails;
        }
        std::printf("%s L%d T%d->%d F1 %.12g->%.12g\n",p.name,L,old,quality,before,after);
    }
    // Known old production baseline: seven of eight fail at each of three Ls.
    if(rejected!=21)++fails;
    for(double bad:{0.0,-1.0,std::nan("")}) {
      try{(void)apod_quality_taps(bad,0.9,false,280);++fails;}catch(const std::invalid_argument&){}
    }
    std::printf("checked=%d old_rejected=%d rate_invariance_pairs=48 fails=%d\n",checked,rejected,fails);
    return fails?1:0;
}
