"""Fault-inject the production settings handler; no server or audio device.

The profile-bank collaborator is isolated here. Its call order, injected failure
and the real HTTP error function are checked; the real bank has integration tests.
"""
import pathlib
import hashlib
import json
import os
import subprocess
import sys
import tempfile

root = pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT', pathlib.Path(__file__).resolve().parents[1]))
source = (root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
start = source.index('    if (rq.path.rfind("/api/settings", 0) == 0) {')
end = source.index('    // ASIO 驱动列表', start)
handler = source[start:end]
reader_start = source.index('struct MetaSettings {')
reader = source[reader_start:source.index('\nstatic ArtistImgOutcome artist_avatar_fetch(',reader_start)]
store = (root/'src/meta/store.h').read_text(encoding='utf-8')
atomic = store[store.index('inline std::recursive_mutex& meta_file_mutex('):store.index('\nclass MetaStore')]
workbench = (root/'src/net/workbench_config.h').read_text(encoding='utf-8')
error_start = workbench.index('inline std::string error(')
workbench_error = workbench[error_start:workbench.index('\n}', error_start)+2]
def production_function(text, marker):
    start = text.index(marker)
    return text[start:text.index('\n}', start)+2]

form = production_function(workbench, 'inline std::string form(')
params_start = source.index('struct PlayParams {')
params = source[params_start:source.index('\n};', params_start)+3]
helpers = '\n'.join(production_function(source, marker) for marker in (
    'static std::string jesc(', 'static std::string urlDecode(',
    'static std::string artistimg_pct(', 'static rta::dsp::Shaper parse_shaper(',
    'static std::string saved_backend_setting()', 'static double saved_volume_setting()',
    'static bool saved_roomcorr_auto()', 'static std::string saved_rate_family_policy()',
    'static PlayParams parseForm(', 'static std::string pcm_shaper_request_error('))
prefix = r'''
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>
#include <string>
#include <map>
#include <set>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <charconv>
#include <algorithm>
#include <cctype>
#include "audio/wav.h"
#include "dsp/dither.h"
#include "net/json.h"
namespace fs=std::filesystem;
static std::string base;
static void filter_prewarm_cancel(){}
static std::string path_in_exe_dir(const std::string& s){return (fs::path(base)/s).string();}
struct Library{static Library& instance(){static Library x;return x;}void setScanThreads(size_t){}};
namespace rta{namespace audio{inline bool valid_rate_family_policy(const std::string&){return true;}inline void set_stream_decode_enabled(bool){}}
namespace net{static thread_local int lastStatus=200;inline void webui_set_status(int s){lastStatus=s;}inline std::string i18n_resolve(const std::string& s){return s;}}
namespace analysis{inline void analysis_set_lang(const std::string&) {}}
namespace meta{namespace fsx=std::filesystem;struct ProviderDesc{std::string id;};inline std::vector<ProviderDesc> provider_registry(){return {};}
inline void pp_set_output_lang(const std::string&){}
'''
wrapper = r'''
}}
// PRODUCTION_PLAY_PARAMS
// PRODUCTION_SETTINGS_HELPERS
// The extracted handler uses the same shared session lock as playback. Keep
// one mutex for all fixture requests, including the concurrent partial saves.
struct TestPlayerState { std::mutex sessionMx; };
static TestPlayerState ps;
namespace workbench {
using Values = std::map<std::string,std::string>;
// PRODUCTION_WORKBENCH_FORM
static std::atomic<int> syncCalls{0};
static std::atomic<bool> syncSuccess{true};
static std::string lastSavedBytes;
inline bool sync_saved_locked(){
 ++syncCalls;
 std::ifstream in(path_in_exe_dir("rlplayer-settings.conf"),std::ios::binary);
 const bool opened=bool(in);
 lastSavedBytes.assign(std::istreambuf_iterator<char>(in),{});
 return opened&&syncSuccess.load();
}
// PRODUCTION_WORKBENCH_ERROR
}
// PRODUCTION_SETTINGS_READER
struct Request{std::string path="/api/settings",method="POST",body;};
static std::string settings(const Request& rq){std::string ct;rta::net::lastStatus=200;
'''
main = r'''
return "unhandled";}
int main(int argc,char**argv){base=argv[1];int failures=0,checks=0;
 auto check=[&](bool b,const char* what){++checks;if(!b)++failures;printf("%s %s\n",b?"PASS":"FAIL",what);};
 auto read=[](const std::string& p){std::ifstream f(p);return std::string(std::istreambuf_iterator<char>(f),{});};
 const auto file=path_in_exe_dir("rlplayer-settings.conf");
 check(settings({"/api/settings","POST","backend=null&volume=-60"}).find("\"ok\":true")!=std::string::npos,"seed isolated safe settings");
 const auto seedSyncCalls=workbench::syncCalls.load();const auto seedSyncBytes=workbench::lastSavedBytes;
 const auto old=read(file);HANDLE held=CreateFileW(fs::u8path(file).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
 check(held!=INVALID_HANDLE_VALUE,"hold settings target against atomic replacement");
 const auto response=settings({"/api/settings","POST","theme=light"});
 check(response.find("\"ok\":false")!=std::string::npos,"replacement denial reports save failure");check(read(file)==old,"denied replacement preserves old settings bytes");
 int deniedTrials=0;for(int i=0;i<100;++i){const auto denied=settings({"/api/settings","POST","theme=light"});if(denied.find("\"ok\":false")!=std::string::npos&&read(file)==old)++deniedTrials;}
 const auto deniedSyncCalls=workbench::syncCalls.load();
 if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
 check(settings({"/api/settings","POST","theme=dark"}).find("\"ok\":true")!=std::string::npos,"retry after lock release succeeds");
 const auto retriedSyncCalls=workbench::syncCalls.load();
 check(read(file).find("backend=null")!=std::string::npos&&read(file).find("volume=-60")!=std::string::npos,"partial POST retains existing safe backend and volume");
 int lost=0;for(int round=0;round<30;++round){std::ofstream(file)<<"backend=null\nvolume=-60\n";std::atomic<int> ready=0;std::atomic<bool> go=false;
 std::thread a([&]{++ready;while(!go)std::this_thread::yield();settings({"/api/settings","POST","theme=dark"});});
 std::thread b([&]{++ready;while(!go)std::this_thread::yield();settings({"/api/settings","POST","uilang=es"});});
 while(ready<2)std::this_thread::yield();go=true;a.join();b.join();const auto content=read(file);
 if(content.find("theme=dark")==std::string::npos||content.find("uilang=es")==std::string::npos||content.find("backend=null")==std::string::npos)++lost;}
 printf("concurrent_partial_POST_lost_rounds=%d/30\n",lost);check(lost==0,"concurrent partial POST retains both updates and original fields");
 const int originalChecks=checks;
 check(seedSyncCalls==1&&seedSyncBytes==old,"profile collaborator observes the successfully persisted settings");
 check(deniedSyncCalls==seedSyncCalls,"failed atomic replacement never calls profile synchronization");
 check(retriedSyncCalls==seedSyncCalls+1,"successful replacement retry calls profile synchronization exactly once");
 check(workbench::syncCalls==retriedSyncCalls+60&&workbench::lastSavedBytes==read(file),"all sixty concurrent updates synchronize their committed settings");
 const auto beforeSyncFailure=workbench::syncCalls.load();workbench::syncSuccess=false;
 const auto failedSync=settings({"/api/settings","POST","theme=light"});const int failedStatus=rta::net::lastStatus;
 check(workbench::syncCalls==beforeSyncFailure+1,"injected profile failure is actually called once");
 check(failedStatus==500&&failedSync.find("\"ok\":false")!=std::string::npos,"production settings branch reports HTTP 500 when profile synchronization fails");
 check(read(file).find("theme=light")!=std::string::npos&&read(file).find("backend=null")!=std::string::npos,"profile failure truthfully retains the already committed settings and safe backend");
 workbench::syncSuccess=true;const auto recovered=settings({"/api/settings","POST","theme=dark"});
 check(rta::net::lastStatus==200&&recovered.find("\"ok\":true")!=std::string::npos&&workbench::syncCalls==beforeSyncFailure+2,"profile synchronization recovers on the next successful request");
 const auto beforeGet=workbench::syncCalls.load();const auto get=settings({"/api/settings","GET",""});
 check(rta::net::lastStatus==200&&get.find("\"ok\":true")!=std::string::npos&&workbench::syncCalls==beforeGet,"read-only settings GET never calls the profile writer");
 check(deniedTrials==100,"one hundred blocked atomic replacements preserve original settings bytes");
 int readerEarly=0,readerFresh=0;
 for(int round=0;round<20;++round){
  rta::meta::meta_write_atomic(fs::u8path(file),"backend=null\nvolume=-60\nmetaon=0\n");
  std::unique_lock<std::recursive_mutex> writer(rta::meta::meta_file_mutex(fs::u8path(file)));
  std::atomic<bool> started=false,finished=false;bool observedOn=false;
  std::thread worker([&]{started=true;const auto observed=meta_settings_load();observedOn=observed.on;finished=true;});
  while(!started)std::this_thread::yield();Sleep(20);if(finished)++readerEarly;
  const bool committed=rta::meta::meta_write_atomic(fs::u8path(file),"backend=null\nvolume=-60\nmetaon=1\n");
  writer.unlock();worker.join();if(committed&&finished&&observedOn)++readerFresh;
 }
 check(readerEarly==0,"extracted production metadata reader waits for the real settings file mutex");
 check(readerFresh==20,"production reader observes committed bytes only after writer unlocks");
  auto ok=[&](const std::string& body){const auto response=settings({"/api/settings","POST",body});rta::net::Json j;return rta::net::lastStatus==200&&rta::net::json_parse_strict(response,j)&&j["ok"].b;};
  auto precisionDenied=[&](const std::string& body,const char* what){
    const auto bytes=read(file);const auto calls=workbench::syncCalls.load();
    const auto response=settings({"/api/settings","POST",body});rta::net::Json j;
    check(rta::net::lastStatus==400&&rta::net::json_parse_strict(response,j)&&!j["ok"].b&&j["error"].s().find("16 bit")!=std::string::npos&&j["error"].s().find("TPDF")!=std::string::npos,what);
    check(read(file)==bytes&&workbench::syncCalls==calls,"precision rejection neither replaces saved bytes nor synchronizes the profile");
  };
  check(ok("bits=32&dacbits=18&shaper=NS9&dsd=0"),"valid 18-bit NS9 settings commit in a 32-bit container");
  check(read(file).find("bits=32\n")!=std::string::npos&&read(file).find("dacbits=18\n")!=std::string::npos,"container and effective precision persist as distinct keys");
  for(const char* shaper:{"NS9","LNS15"})for(int bits=8;bits<16;++bits)
    precisionDenied(std::string("shaper=")+shaper+"&dacbits="+std::to_string(bits),"partial settings merge rejects unsupported low-bit manual shaping");
  precisionDenied("bits=8&dacbits=32&shaper=NS9","precision restriction uses the smaller container even when DAC request is 32 bit");
  check(ok("bits=16&dacbits=32&shaper=NS9"),"a 16-bit container with a wider DAC request remains supported");
  check(ok("bits=32&dacbits=8&shaper=auto"),"automatic dither remains valid at 8-bit effective precision");
  check(ok("shaper=TPDF"),"partial TPDF change preserves valid low-bit settings");
  check(ok("shaper=Shaped"),"legacy Shaped input remains accepted");
  const workbench::Values roundtrip{{"shaper","Shaped"},{"file","C:/A & B/100%+music.wav"},{"bits","32"},{"dacbits","18"}};
  const auto parsed=parseForm(workbench::form(roundtrip));
  check(parsed.shaper=="shaped"&&parsed.bits==32&&parsed.dacBits==18&&parsed.file==roundtrip.at("file"),"production form encoder and parser preserve escaped values and normalize Shaped");
  check(ok("dsd=64&bits=24&dacbits=8&shaper=NS9"),"DSD double-precision route is exempt from PCM shaper precision rejection");
  precisionDenied("dsd=0","partial return to PCM checks retained low-bit manual shaper before persistence");
  check(ok("dsd=0&shaper=TPDF"),"a corrected PCM request can recover after precision rejection");
 printf("production_reader_early_returns=%d/20 production_reader_fresh_reads=%d/20 blocked_atomic_preserved=%d/100\n",readerEarly,readerFresh,deniedTrials);
 printf("original_checks=%d added_workbench_checks=%d sync_calls=%d sync_failure_http_status=%d\n",originalChecks,checks-originalChecks,workbench::syncCalls.load(),failedStatus);
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
compiler = sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-settings-test-') as folder:
    folder=pathlib.Path(folder);cpp=folder/'test.cpp';exe=folder/'test.exe'
    fixture=prefix+atomic+wrapper.replace('// PRODUCTION_PLAY_PARAMS',params).replace('// PRODUCTION_SETTINGS_HELPERS',helpers).replace('// PRODUCTION_WORKBENCH_FORM',form).replace('// PRODUCTION_WORKBENCH_ERROR',workbench_error).replace('// PRODUCTION_SETTINGS_READER',reader)+handler+main
    cpp.write_text(fixture,encoding='utf-8')
    subprocess.run([compiler,'-std=c++17','-O0','-I'+str(root/'src'),str(cpp),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe),str(folder)],capture_output=True,text=True,encoding='utf-8',errors='replace')
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    report={'exit':result.returncode,'stdout':result.stdout,'stderr':result.stderr,'audioOpened':False,'serverStarted':False,
            'productionHandlerExtracted':True,'productionErrorExtracted':True,'profileCollaborator':'isolated fault-injectable spy',
            'productionSourceSHA256':hashlib.sha256((root/'tools/rtaweb.cpp').read_bytes()).hexdigest(),
            'productionHandlerSHA256':hashlib.sha256(handler.encode()).hexdigest(),
            'productionReaderExtracted':True,'productionReaderSHA256':hashlib.sha256(reader.encode()).hexdigest(),
            'productionFormExtracted':True,'productionFormSHA256':hashlib.sha256(form.encode()).hexdigest(),
            'productionParsingAndPrecisionHelpersSHA256':hashlib.sha256(helpers.encode()).hexdigest(),
            'productionErrorSHA256':hashlib.sha256(workbench_error.encode()).hexdigest(),
            'fixtureSHA256':hashlib.sha256(fixture.encode()).hexdigest(),'executedEXESHA256':hashlib.sha256(exe.read_bytes()).hexdigest()}
    if os.environ.get('RLPLAYER_TEST_EVIDENCE_DIR'):
        evidence=pathlib.Path(os.environ['RLPLAYER_TEST_EVIDENCE_DIR']);evidence.mkdir(parents=True,exist_ok=True)
        (evidence/'fixture.cpp').write_text(fixture,encoding='utf-8')
        (evidence/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('production_handler_sha256='+report['productionHandlerSHA256'])
    sys.exit(result.returncode)
