"""Run the actual application prewarm scheduler with instrumented DSP mocks."""
import os
import pathlib
import subprocess
import sys
import tempfile
root=pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT',pathlib.Path(__file__).resolve().parents[1]))
source=(root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
def fn(signature):
    start=source.index(signature);return source[start:source.index('\n}',start)+2]
prefix=r'''
#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <numeric>
#include <limits>
static std::map<std::string,std::string> conf;
static std::string conf_get(const std::string& k,const std::string& fallback){auto it=conf.find(k);return it==conf.end()?fallback:it->second;}
static void play_log(const std::string&){}
static std::atomic<bool> g_shutdown{false},g_exitReq{false};
static struct{std::atomic<bool> playing{false};}g_ps;
static std::atomic_bool g_filterPrewarmCancel{false};
static std::thread g_filterPrewarmThread;
static std::mutex g_filterPrewarmMutex;
struct QItem{std::string path;};
static struct{std::mutex mtx;std::vector<QItem> items;size_t index=0;}g_q;
struct LibTrack{std::string path;int sampleRate=0;};
namespace rta::net{using LibTrack=::LibTrack;}
struct Library{std::vector<LibTrack> tracks;static Library& instance(){static Library l;return l;}size_t trackCount()const{return tracks.size();}bool trackAt(size_t i,LibTrack& t){if(i>=tracks.size())return false;t=tracks[i];return true;} };
static std::atomic<int> constructions{0},completed{0};static bool cacheOn=true,blockDesign=false,admitCache=true,phaseDesign=false,lastAllow=false,lastTwoStage=false,lastAuto=false;static int mockTaps=64;
static uint64_t mockPlanPeak=16ull*1024*1024,mockPlanPayload=8ull*1024*1024;
namespace rta::dsp{
enum class PhaseKind{Linear,Minimum};
struct ResamplerConfig{unsigned inRate=0,outRate=0;int channels=2,minTapsPerInput=0,totalTaps=1000,window=0;double transition=0.1,apodF1=0,apodF2=0;bool apodizing=false,twoStage=false,allowAutomaticCascade=false,autoTaps=false;PhaseKind phase=PhaseKind::Linear;};
struct ResamplerPlan{size_t stageCount=2;uint64_t cachePayloadBytes=0,designPeakBytes=0;};
inline ResamplerPlan plan_resampler(const ResamplerConfig& c){lastAuto=c.autoTaps;return {2,mockPlanPayload,mockPlanPeak};}
struct Preset{};inline const Preset* find_preset(const std::string&,bool){static Preset p;return &p;}
inline void apply_preset(ResamplerConfig& c,const Preset&){if(phaseDesign)c.phase=PhaseKind::Minimum;}
inline int solve_taps_per_phase(int,double,bool,double,double,int,int,double){return mockTaps;}
inline thread_local const std::atomic_bool* design_cancel_flag=nullptr;
struct DesignCancelled:std::runtime_error{DesignCancelled():std::runtime_error("cancelled"){}};
struct DesignCancellationScope{const std::atomic_bool* old;DesignCancellationScope(const std::atomic_bool* p):old(design_cancel_flag){design_cancel_flag=p;}~DesignCancellationScope(){design_cancel_flag=old;}};
inline void check_design_cancel(){if(design_cancel_flag&&design_cancel_flag->load())throw DesignCancelled();}
namespace filter_cache{inline bool enabled(){return cacheOn;}struct CacheLimits{};enum class ProbeStatus{Ready,Disabled,SkippedSize,Full,IoError,Busy,Cancelled};inline ProbeStatus can_store(uint64_t,CacheLimits={},const std::atomic_bool* = nullptr){return admitCache?ProbeStatus::Ready:ProbeStatus::IoError;}}
struct PolyphaseResampler{PolyphaseResampler(const ResamplerConfig& c){lastAllow=c.allowAutomaticCascade;lastTwoStage=c.twoStage;++constructions;for(int n=0;blockDesign&&n<2000;++n){check_design_cancel();std::this_thread::sleep_for(std::chrono::milliseconds(1));}check_design_cancel();++completed;}int tapsPerPhase()const{return 64;}};
}
'''
body=''
for name in ['static void filter_prewarm_cancel()', 'static void filter_prewarm_join()', 'static uint32_t filter_prewarm_source_rate()', 'static void filter_prewarm_bg()']:
    if name in source:body+=fn(name)+'\n'
new='static void filter_prewarm_join()' in source
finish='filter_prewarm_join();' if new else 'std::this_thread::sleep_for(std::chrono::milliseconds(50));'
main=r'''
int main(){int failures=0,checks=0;
auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);};
auto reset=[](){conf={{"rate","384000"},{"filter","poly-sinc-short-lp"}};g_q.items={{"track.wav"}};g_q.index=0;Library::instance().tracks={{"track.wav",44100}};constructions=0;completed=0;cacheOn=true;blockDesign=false;admitCache=true;phaseDesign=false;mockTaps=64;mockPlanPeak=16ull*1024*1024;mockPlanPayload=8ull*1024*1024;lastAuto=false;g_filterPrewarmCancel=false;g_shutdown=false;g_exitReq=false;g_ps.playing=false;};
'''
for setup,label,expected in [
    ('cacheOn=false;','disabled cache causes zero designs',0),
    ('conf["rate"]="384000junk";','partial numeric output rate is rejected',0),
    ('conf["rate"]="4295351296";','overflowing output rate is rejected',0),
    ('conf["rate"]="auto";','automatic output does not speculate',0),
    ('g_q.items.clear();','empty restored queue does not speculate',0),
    ('Library::instance().tracks[0].sampleRate=0;','unknown source rate does not speculate',0),
    ('g_ps.playing=true;','active foreground playback takes priority',0),
    ('admitCache=false;','unwritable cache admission causes zero designs',0),
    ('mockPlanPeak=300ull*1024*1024;','planner-reported oversized workspace is skipped before allocation',0),
    ('mockPlanPayload=0;','plans without cacheable payload cause zero designs',0),
    ('mockTaps=2147483647;','feasible two-stage plan is not rejected by obsolete unsplit estimates',1),
    ('conf["taps"]="512";','custom filter overrides do not warm a wrong preset key',0),
    ('','known restored source warms only one matching input family',1),
]:
    main+='reset();'+setup+'filter_prewarm_bg();'+finish+f'check(constructions=={expected},"{label}");\n'
if new:
    main+=r'''
reset();filter_prewarm_bg();filter_prewarm_join();check(lastAllow,"automatic prewarm uses the same automatic cascade policy as playback");
check(lastAuto,"prewarm delegates automatic tap resolution to the production planner");
reset();conf["twostage"]="0";filter_prewarm_bg();filter_prewarm_join();check(!lastAllow&&!lastTwoStage,"explicit off disables automatic and preset cascading in prewarm");
reset();conf["twostage"]="1";filter_prewarm_bg();filter_prewarm_join();check(!lastAllow&&lastTwoStage,"explicit on enables cascading without automatic override");
reset();blockDesign=true;filter_prewarm_bg();for(int n=0;n<1000&&constructions==0;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));
auto before=std::chrono::steady_clock::now();filter_prewarm_cancel();filter_prewarm_join();
check(constructions==1&&completed==0,"cancel reaches an in-progress DSP design before completion");
check(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-before).count()<500,"cancelled worker joins within 500ms in mock design");
'''
main+='printf("checks=%d failures=%d\\n",checks,failures);return failures?1:0;}\n'
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-prewarm-test-') as directory:
    directory=pathlib.Path(directory);cpp=directory/'test.cpp';exe=directory/'test.exe'
    cpp.write_text(prefix+body+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++17','-O0',str(cpp),'-o',str(exe)],check=True)
    sys.exit(subprocess.run([str(exe)]).returncode)
