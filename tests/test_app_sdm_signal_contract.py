"""Complete production signal branch and real state; no server or audio device."""
import hashlib,json,os,pathlib,subprocess,sys,tempfile
root=pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT',pathlib.Path(__file__).resolve().parents[1]));source=(root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
def production_function(marker):
 a=source.index(marker)
 return source[a:source.index('\n}',a)+2]
fields=['j'+line.strip() for line in source.splitlines() if '<< ",\\\"modOrderRequested\\\"' in line]
carrier=next('j'+line.strip() for line in source.splitlines() if '<< ",\\\"carrier\\\":' in line and 'dop ? deviceRate' in line)
if len(fields)!=3:raise SystemExit('Expected direct/native-converted/PCM modulation snapshots')
a=source.index('    if (rq.path.rfind("/api/signal", 0) == 0) {');b=source.index('\n    if (rq.path.rfind("/api/rescan", 0)',a)
handler=source[a:b]
a=source.index('struct PlayParams {');params=source[a:source.index('\n};',a)+3]
a=source.index('struct CleanupAutoSnapshot {');state=source[a:source.index('\nstatic rta::audio::PlaybackSegmentState',a)]
jesc=production_function('static std::string jesc(')
set_dsd_state=production_function('static void set_dsd_out_state(')
a=source.index('// 模块构造前实际阶数尚未确认');a=source.index('set_dsd_out_state(',a);b=source.index(';',a)+1
initial=source[a:b]
end=source.index('mods[0]->effectiveOrder(), modNote);')+len('mods[0]->effectiveOrder(), modNote);')
effective=source[source.rfind('set_dsd_out_state(',0,end):end]
factory=production_function('static std::unique_ptr<OutSink> make_sink(')
status_mode=next('j'+line.strip()+';' for line in source.splitlines() if '<< ",\\\"outMode\\\":' in line and 'castMode' in line)
pcm_fields=[]
for key in ('outBits','dacBitsRequested','shaperRequested','shaper'):
 marker='<< ",\\\"'+key+'\\\":'
 matches=[line.strip() for line in source.splitlines() if marker in line]
 if key=='outBits':matches=[line for line in matches if '<< outBits' in line]
 elif key=='shaper':matches=[line for line in matches if 'dop?"none":""' in line]
 if len(matches)!=1:raise SystemExit('Expected unique PCM snapshot writer for '+key)
 pcm_fields.append('j'+matches[0]+';')
code=r'''
#include "net/json.h"
#include "dsp/dither.h"
#include "dsp/recording_cleanup.h"
#include "dsp/modulator.h"
#include <sstream>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
''' +params+state+jesc+set_dsd_state+r'''
static PlayParams p;
// The production factory selects these inert sink types. No driver is opened.
struct OutSink{virtual ~OutSink()=default;};
struct NativeDsdSink:OutSink{};struct AsioSink:OutSink{};struct NullSink:OutSink{};
struct WasapiSharedSink:OutSink{};struct WasapiSink:OutSink{};
static std::atomic<bool> g_outShared{false};
''' +factory+r'''
// Only this unrelated scan-time collaborator is a fixture.
struct Library{static Library& instance(){static Library v;return v;}long long lastScanMs(){return 17;}};
struct Request{std::string path="/api/signal";};
static std::string signal(){const Request rq;std::string ct;auto& ps=g_ps;
''' +handler+r'''
 return "unhandled";}
int main(){int checks=0,failures=0;auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);};
 p.modOrder=7;p.dsd=64;p.modFamily="dsd";
 bool dop=true;int convertedModOrder=5;unsigned dsdRate=2822400,deviceRate=176400;std::string directNote;
''' + initial+r'''
 check(g_ps.outModOrder==0,"before modulator construction effective order is unconfirmed, not the request");
 for(bool native:{false,true}){
   p.backend="asio";p.dsdNative=native;const std::string want=native?"native":"dop";
   const auto sink=make_sink(p,true);
   check(native?dynamic_cast<NativeDsdSink*>(sink.get())!=nullptr:dynamic_cast<AsioSink*>(sink.get())!=nullptr,"production factory selects the requested Native or DoP transport");
''' +initial+r'''
   check(g_ps.outMode==want&&g_ps.outModOrder==0,"initial transport state matches the selected sink before effective SDM order is known");
   rta::dsp::SdmConfig mcfg;mcfg.order=p.modOrder;mcfg.fs=dsdRate;mcfg.oneBitOutput=true;
   std::vector<std::unique_ptr<rta::dsp::SdmModulator>> mods;mods.push_back(std::make_unique<rta::dsp::SdmModulator>(mcfg));
   const std::string modNote="guarded effective order";
''' +effective+r'''
   check(g_ps.outMode==want&&g_ps.outModOrder==mods[0]->effectiveOrder()&&g_ps.outModOrder>0,"post-construction transport and actual SDM order remain truthful for both branches");
   auto& ps=g_ps;const bool castMode=false;std::ostringstream j;j<<"{\"ok\":true";
''' +status_mode+r'''
   j<<"}";rta::net::Json stateJson;
   check(rta::net::json_parse_strict(j.str(),stateJson)&&stateJson["outMode"].s()==want,"production status JSON reports Native as native and DoP as dop");
 }
 p.dsdNative=false;
 auto verify=[&](std::string text,int actual,int wantRequested){g_ps.signal=text;g_ps.outModOrder=actual;rta::net::Json j;check(rta::net::json_parse_strict(signal(),j)&&j["ok"].b&&j["path"]["modOrder"].n()==actual&&j["path"]["modOrderRequested"].n()==wantRequested,"actual JSON retains requested order separately from normalized effective order");if(j["path"].has("carrier"))check(j["path"]["carrier"].n()==176400,"ordinary DoP snapshot records actual device carrier separately from DSD bit rate");};
 {std::ostringstream j;j<<"{\"modOrder\":7";
''' +fields[0]+r''';j<<"}";verify(j.str(),0,0);}
 {std::ostringstream j;j<<"{\"modOrder\":5";
''' +fields[1]+r''';j<<"}";verify(j.str(),5,7);}
 convertedModOrder=0;{std::ostringstream j;j<<"{\"modOrder\":0";
''' +fields[1]+r''';j<<"}";verify(j.str(),0,0);}
 {std::ostringstream j;j<<"{\"modOrder\":7";
''' +fields[2]+';'+carrier+r''';j<<"}";verify(j.str(),5,7);verify(j.str(),0,7);}
 auto snapshot=[&](int outBits){std::ostringstream j;j<<"{\"modOrder\":0,\"bitPerfect\":\"old\",\"bitPerfectWhy\":\"old \\\"reason\\\"\"";
''' +'\n'.join(pcm_fields)+r'''
 j<<"}";g_ps.signal=j.str();};
 auto readPath=[&](){rta::net::Json j;check(rta::net::json_parse_strict(signal(),j)&&j["ok"].b,"complete production signal branch emits strict JSON");return j["path"];};
 dop=false;p.shaper="auto";p.dacBits=18;g_ps.quantizationBits=0;g_ps.pcmObserved=false;g_ps.pcmShaper=(int)rta::dsp::Shaper::NS9;
 g_ps.bpState="no";g_ps.bpWhy="not observed";snapshot(32);auto j=readPath();
 check(j["outBits"].n()==32&&j["dacBitsRequested"].n()==18&&j["quantizationBits"].n()==0,"unobserved PCM distinguishes container, requested DAC bits, and unknown actual precision");
 check(j["shaperRequested"].s()=="auto"&&j["shaper"].s().empty()&&!j["shaperObserved"].b,"unobserved auto selection is never reported as applied");
 const rta::dsp::Shaper observed[]={rta::dsp::Shaper::None,rta::dsp::Shaper::TPDF,rta::dsp::Shaper::NS9,rta::dsp::Shaper::Shaped};
 for(int bits:{18,20,24})for(auto shaper:observed){
   p.dacBits=bits;g_ps.quantizationBits=bits;g_ps.pcmObserved=true;g_ps.pcmShaper=(int)shaper;g_ps.volumeDbObserved=-6.;
   g_ps.bpState="no";g_ps.bpWhy="音量 \"test\" + path\\name\nnext";snapshot(32);j=readPath();
   check(j["outBits"].n()==32&&j["dacBitsRequested"].n()==bits&&j["quantizationBits"].n()==bits,"observed effective precision remains distinct from 32-bit container");
   check(j["shaper"].s()==rta::dsp::shaper_name(shaper)&&j["shaperRequested"].s()=="auto"&&j["shaperObserved"].b,"actual runtime shaper replaces preview without overwriting requested auto");
   check(j["bitPerfect"].s()==g_ps.bpState&&j["bitPerfectWhy"].s()==g_ps.bpWhy&&j["volumeDbObserved"].n()==-6.,"runtime bit-perfect reasons are escaped correctly and observed volume is truthful");
 }
 p.dacBits=32;g_ps.quantizationBits=16;g_ps.pcmShaper=(int)rta::dsp::Shaper::NS9;snapshot(16);j=readPath();
 check(j["dacBitsRequested"].n()==32&&j["outBits"].n()==16&&j["quantizationBits"].n()==16,"container-limited actual precision is separate from wider request");
 p.dacBits=0;g_ps.quantizationBits=24;g_ps.pcmShaper=(int)rta::dsp::Shaper::None;g_ps.volumeDbObserved=0;g_ps.bpState="yes";g_ps.bpWhy="";snapshot(24);j=readPath();
 check(j["shaper"].s()=="none"&&j["bitPerfect"].s()=="yes"&&j["bitPerfectWhy"].s().empty(),"return to transparent PCM replaces earlier DSP state and explanation");
 dop=true;g_ps.pcmObserved=false;g_ps.quantizationBits=0;p.dacBits=18;p.shaper="NS9";snapshot(24);j=readPath();
 check(j["shaper"].s()=="none"&&!j["shaperObserved"].b&&j["quantizationBits"].n()==0&&j["dacBitsRequested"].n()==18,"DSD output reports no applied PCM quantizer despite retained PCM request");
 g_ps.signal.clear();rta::net::Json empty;check(rta::net::json_parse_strict(signal(),empty)&&!empty["ok"].b,"no signal snapshot remains explicitly unavailable");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-sdm-signal-') as d:
 p=pathlib.Path(d);cpp=p/'test.cpp';exe=p/'test.exe';cpp.write_text(code,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O1','-I'+str(root/'src'),str(cpp),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],cwd=d,capture_output=True,text=True,encoding='utf-8',errors='replace')
 print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
 if os.environ.get('RLPLAYER_TEST_EVIDENCE_DIR'):
  evidence=pathlib.Path(os.environ['RLPLAYER_TEST_EVIDENCE_DIR']);evidence.mkdir(parents=True,exist_ok=True)
  (evidence/'fixture.cpp').write_text(code,encoding='utf-8')
  report={'exit':result.returncode,'stdout':result.stdout,'stderr':result.stderr,'audioOpened':False,'serverStarted':False,
          'completeProductionSignalBranchExtracted':True,'productionSourceSHA256':hashlib.sha256(source.encode()).hexdigest(),
          'productionHandlerSHA256':hashlib.sha256(handler.encode()).hexdigest(),'productionStateSHA256':hashlib.sha256(state.encode()).hexdigest(),
          'productionFactorySHA256':hashlib.sha256(factory.encode()).hexdigest(),'productionTransportSettersSHA256':hashlib.sha256((initial+effective).encode()).hexdigest(),
          'fixtureSHA256':hashlib.sha256(code.encode()).hexdigest(),'executedEXESHA256':hashlib.sha256(exe.read_bytes()).hexdigest()}
  (evidence/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 sys.exit(result.returncode)
