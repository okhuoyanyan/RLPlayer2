#include "dsp/resampler.h"
#include <bit>
#include <cfenv>
#include <cstdio>
static size_t diff(const std::vector<double>& a,const std::vector<double>& b) {size_t n=0;for(size_t i=0;i<a.size();++i)if(std::bit_cast<uint64_t>(a[i])!=std::bit_cast<uint64_t>(b[i]))++n;return n;}
int main() {
 rta::dsp::ResamplerConfig cfg;cfg.inRate=44100;cfg.outRate=705600;cfg.minTapsPerInput=12500;cfg.transition=.125;cfg.attenDb=180;cfg.phase=rta::dsp::PhaseKind::Linear;
 std::vector<double> nearest,cachedDown,freshDown;
 std::fesetround(FE_TONEAREST);{rta::dsp::PolyphaseResampler r(cfg);r.prototype_into(nearest);}
 std::fesetround(FE_DOWNWARD);{rta::dsp::PolyphaseResampler r(cfg);r.prototype_into(cachedDown);}
 _putenv_s("RLPLAYER_FILTER_CACHE","0");{rta::dsp::PolyphaseResampler r(cfg);r.prototype_into(freshDown);}
 std::fesetround(FE_TONEAREST);
 auto wrong=diff(cachedDown,freshDown);
 std::printf("taps=%zu cachedDown_vs_freshDown=%zu cachedDown_vs_nearest=%zu\n",nearest.size(),wrong,diff(cachedDown,nearest));
 return wrong?1:0;
}
