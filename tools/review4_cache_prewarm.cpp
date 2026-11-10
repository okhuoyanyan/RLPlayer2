#include "dsp/resampler.h"

#include <chrono>
#include <cstdio>
#include <psapi.h>
#include <thread>
static double run(const char* name, uint32_t inRate,uint32_t outRate) {
 rta::dsp::ResamplerConfig rc; rc.inRate=inRate;rc.outRate=outRate;rc.channels=2;
 rta::dsp::apply_preset(rc,*rta::dsp::find_preset(name,false));
 if(rc.minTapsPerInput==0)rc.minTapsPerInput=rta::dsp::solve_taps_per_phase(rc.window,rc.transition,rc.apodizing,rc.apodF1,rc.apodF2,rc.totalTaps,0,double(inRate));
 auto start=std::chrono::steady_clock::now();rta::dsp::PolyphaseResampler r(rc);
 auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 PROCESS_MEMORY_COUNTERS pm{};pm.cb=sizeof pm;GetProcessMemoryInfo(GetCurrentProcess(),&pm,sizeof pm);
 std::printf("preset=%s in=%u out=%u T=%d coeff=%zu ms=%.3f peak_MiB=%.3f cache=%d\n",name,inRate,outRate,r.tapsPerPhase(),r.coefficient_count(),ms,double(pm.PeakWorkingSetSize)/1048576,rta::dsp::filter_cache::enabled());
 return ms;
}
int main() {
 std::printf("No audio device. No settings file. Isolated cache=%s\n",rta::dsp::filter_cache::directory().c_str());
 run("poly-sinc-long-mp",44100,384000);run("poly-sinc-long-mp",44100,384000);run("poly-sinc-long-mp",48000,384000);
 _putenv_s("RLPLAYER_FILTER_CACHE","0");
 run("poly-sinc-long-mp",44100,384000);run("poly-sinc-long-mp",44100,384000);
}

