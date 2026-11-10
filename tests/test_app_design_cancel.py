"""Actual offline DSP cooperative cancellation used by the application worker."""
import pathlib,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
code=r'''
#include "dsp/resampler.h"
#include "dsp/design_control.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
int main(){using namespace rta::dsp;_putenv_s("RLPLAYER_FILTER_CACHE","0");int failures=0,checks=0;
auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);};
ResamplerConfig cfg;cfg.inRate=44100;cfg.outRate=176400;cfg.channels=2;cfg.minTapsPerInput=4095;cfg.phase=PhaseKind::Minimum;cfg.attenDb=280;cfg.transition=.05;cfg.twoStage=false;cfg.allowAutomaticCascade=false;
std::atomic_bool cancel{true};bool rejected=false;
try{DesignCancellationScope scope(&cancel);PolyphaseResampler r(cfg);}catch(const DesignCancelled&){rejected=true;}
check(rejected,"already-cancelled real constructor rejects before allocation");
cancel=false;std::atomic_bool started{false},cancelled{false},completed{false};
std::thread worker([&]{DesignCancellationScope scope(&cancel);started=true;try{PolyphaseResampler r(cfg);completed=true;}catch(const DesignCancelled&){cancelled=true;}});
while(!started)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(10));const auto t0=std::chrono::steady_clock::now();cancel=true;worker.join();
const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
printf("real_cancel_join_ms=%lld completed=%d cancelled=%d\n",(long long)elapsed,int(completed.load()),int(cancelled.load()));
check(cancelled&&!completed,"real in-progress phase design observes cancellation");check(elapsed<1000,"real cancellation joins within 1 second without a device");
check(design_cancel_flag==nullptr,"background cancellation scope does not leak into foreground thread");
for(const auto& name:{"poly-sinc-long-mp","poly-sinc-short-lp"}){const auto* preset=find_preset(name,false);if(!preset)continue;ResamplerConfig c;c.inRate=44100;c.outRate=384000;c.channels=2;apply_preset(c,*preset);c.autoTaps=true;c.allowAutomaticCascade=true;auto p=plan_resampler(c);printf("prewarm_plan preset=%s stages=%zu coefficients=%llu cache_payload=%llu peak_bound=%llu admitted_256MiB=%d\n",name,p.stageCount,(unsigned long long)p.coefficientCount,(unsigned long long)p.cachePayloadBytes,(unsigned long long)p.designPeakBytes,int(p.stageCount&&p.cachePayloadBytes&&p.designPeakBytes<=256ull*1024*1024));}
printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-design-cancel-') as directory:
 directory=pathlib.Path(directory);cpp=directory/'test.cpp';exe=directory/'test.exe';cpp.write_text(code,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O2','-I'+str(root/'src'),str(cpp),'-o',str(exe)],check=True)
 sys.exit(subprocess.run([str(exe)],cwd=directory).returncode)
