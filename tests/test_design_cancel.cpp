#include "dsp/resampler.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <exception>
using namespace rta::dsp;
using Clock=std::chrono::steady_clock;
int main(){int checks=0,failures=0;
    auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
    std::atomic_bool cancel{true};bool rejected=false;
    try{DesignCancellationScope scope(&cancel);ResamplerConfig c;PolyphaseResampler rs(c);}catch(const DesignCancelled&){rejected=true;}
    check(rejected,"pre-cancelled filter preparation never designs a kernel");
    check(design_cancel_flag==nullptr,"cancellation scope restores caller thread state");
    for(PhaseKind phase:{PhaseKind::Linear,PhaseKind::Minimum,PhaseKind::Intermediate}){
        cancel=false;std::atomic_bool entered{false};bool cancelled=false,completed=false,workerScopeRestored=false;std::exception_ptr error;
        std::thread worker([&]{
            try{
                DesignCancellationScope scope(&cancel);
                ResamplerConfig c;c.channels=1;c.inRate=44100;c.outRate=176400;
                c.minTapsPerInput=phase==PhaseKind::Linear?2000000:32767;
                c.phase=phase;c.window=WindowKind::Gaussian;c.attenDb=215;c.transition=.015;
                entered=true;PolyphaseResampler rs(c);completed=true;
            }catch(const DesignCancelled&){cancelled=true;}catch(...){error=std::current_exception();}
            workerScopeRestored=design_cancel_flag==nullptr;
        });
        while(!entered.load())std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto begin=Clock::now();cancel=true;worker.join();
        const double latency=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        std::printf("phase=%s actual_cancel_join_ms=%.9g\n",phase_name(phase),latency);
        check(cancelled&&!completed&&!error,"cancellation exits real design with its explicit reason");
        check(workerScopeRestored,"cancelled worker restores its thread-local cancellation scope");
        check(latency<250.,"real cancelled preparation releases worker within 250ms");
    }
    // A separate positive control checks nontrivial FFT phase values. This does
    // not claim that a timed cancellation landed inside the root-table loop.
    cancel=false;
    for(size_t n:{8192u,16384u,32768u}){
        std::vector<cplx> impulse(n);impulse[1]=1;
        fft(impulse);double error=0;for(size_t k=0;k<n;++k)error=std::max(error,std::abs(impulse[k]-std::polar(1.0,-2*kPi*double(k)/double(n))));
        check(error<1e-12,"FFT matches the independent shifted-impulse phase response");
    }
    std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
