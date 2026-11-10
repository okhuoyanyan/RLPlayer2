"""Actual app -> canonical free cover providers, isolated loopback transport.

No playback, external network, commercial images or real LLM credentials.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import wave
from test_track_cover_resolution import png


def auto_decision_controls(source,compiler,evidence):
    """Execute the actual automatic coordinator with IO/scheduler boundaries.

    This checks its decision and batch sizes; canonical provider transport is
    exercised separately below. No implementation of a provider is replaced.
    """
    text=source.read_text(encoding='utf-8-sig');begin=text.index('static void api_auto_fill_after_script(std::vector<ApiMissItem> items, bool artist) {')
    end=text.index('\n}\n',begin)+3;function=text[begin:end]
    code=r'''
#include <string>
#include <vector>
#include <set>
#include <atomic>
#include <iostream>
struct ApiMissItem{bool isArtist=false;std::string key,name,artist;};
struct ApiJob{bool isArtist=false;std::string key,name,artist;std::vector<std::string> items;};
struct MetaSettings{bool on=true,llm=true;};
static MetaSettings settings;
static MetaSettings meta_settings_load(){return settings;}
namespace rta::meta{struct LlmConfig{std::string apiKey,model="fixture";};static std::string llm_mask_key(const std::string&){return "masked";}}
static int keyReads=0,queued=0;static size_t total=0;static bool sourcePhase=false,extensions=false,paid=false;
static rta::meta::LlmConfig apifill_llm_cfg(){++keyReads;return {"private-key","fixture"};}
static bool api_bg_start(std::vector<ApiJob> jobs,bool,const rta::meta::LlmConfig& cfg,bool sources=true,bool primaryDone=false){++queued;total=jobs.size();sourcePhase=sources;extensions=primaryDone;paid=!cfg.apiKey.empty();return true;}
static std::atomic<bool> g_shutdown{false},g_api_auto_cover_once{false},g_api_auto_artist_once{false};
static constexpr size_t kApiAutoMax=40;
static void play_log(const std::string&){}
'''+function+r'''
int main(){int fail=0;auto check=[&](const char*name,bool ok){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";fail+=!ok;};
 auto reset=[&]{keyReads=queued=0;total=0;sourcePhase=extensions=paid=false;g_api_auto_cover_once=false;g_api_auto_artist_once=false;};
 std::vector<ApiMissItem> items;for(int i=0;i<43;++i)items.push_back({false,std::to_string(i),"Artist"+std::to_string(i),"Owner"});
 settings={false,true};api_auto_fill_after_script(items,false);check("auto-master-off-zero-key-read-zero-jobs",keyReads==0&&queued==0);
 reset();
 settings={true,false};api_auto_fill_after_script(items,false);check("auto-album-paid-off-zero-key-read-zero-duplicate-free",keyReads==0&&queued==0);
 api_auto_fill_after_script(items,true);check("auto-artist-paid-off-still-runs-free-all43",queued==1&&total==43&&sourcePhase&&!paid);
 check("auto-artist-free-phase-does-not-read-key",keyReads==0);
 settings={true,true};api_auto_fill_after_script(items,false);check("auto-album-paid-cap40-no-repeated-free",total==40&&!sourcePhase&&paid&&keyReads==1);
 int before=queued;api_auto_fill_after_script(items,false);check("auto-album-paid-once",queued==before&&keyReads==1);
 api_auto_fill_after_script(items,true);check("auto-artist-enabled-preserves-free-and-paid",total==43&&sourcePhase&&paid&&keyReads==2);
 api_auto_fill_after_script(items,true);check("auto-artist-free-repeat-does-not-repeat-paid-or-cap43",total==43&&sourcePhase&&!paid&&keyReads==2);
 return fail?1:0;}
'''
    cpp=evidence/'auto-decision.cpp';exe=evidence/'auto-decision.exe';cpp.write_text(code,encoding='utf-8')
    build=subprocess.run([str(compiler),'-std=c++20','-O1',str(cpp),'-o',str(exe)],capture_output=True)
    (evidence/'auto-compile.log').write_bytes(build.stdout+build.stderr)
    if build.returncode:raise RuntimeError('actual automatic coordinator seam did not compile')
    run=subprocess.run([str(exe)],capture_output=True,text=True,encoding='utf-8');(evidence/'auto-decisions.log').write_text(run.stdout+run.stderr,encoding='utf-8');print(run.stdout,end='')
    return [dict(case=line[5:],passed=line.startswith('PASS ')) for line in run.stdout.splitlines() if line.startswith(('PASS ','FAIL '))]


def late_paid_controls(source,compiler,evidence):
    """Real batch coordinator; user changes settings during free workers.

    Scheduler/settings/provider/paid boundaries are offline counters. The
    production coordinator is extracted unchanged, including its real threads.
    """
    text=source.read_text(encoding='utf-8-sig');begin=text.index('static bool api_bg_start(std::vector<ApiJob> jobs, bool artist, const rta::meta::LlmConfig& cfg, bool runSources=true')
    end=text.index('\n}\n',begin)+3;function=text[begin:end]
    code=r'''
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>
struct MetaSettings{bool on=true,llm=true;};
static std::atomic<bool> master{true},llm{true};static int mode=0;
static MetaSettings meta_settings_load(){return {master.load(),llm.load()};}
namespace rta::meta {
struct LlmConfig{std::string apiKey="FAKE_OFFLINE_ONLY",model="fixture";};
struct Entry{std::string outcome;};struct ArtLedger{std::map<std::string,Entry>m;};
static bool llm_check_cap(const LlmConfig&,const std::string&,std::string*){return true;}
static long long art_now_sec(){return 0;}
static std::string art_ledger_key(bool,const std::string&s){return s;}
static bool art_ledger_need(const ArtLedger&,bool,const std::string&,long long){return true;}
static int art_batch_llm_requests(size_t){return 1;}
static std::string llm_mask_key(const std::string&){return "MASKED";}
}
struct ApiJob{bool isArtist=false;std::string key,name,artist;std::vector<std::string>items;};
struct State{bool sourceTransient=false;};struct Stats{std::string line()const{return "";}};
struct ApiFillBg{
 std::vector<ApiJob>jobs;rta::meta::LlmConfig cfg;MetaSettings settings;
 bool artist=false,runSources=true,primaryArtistDone=false,allowPaid=false;
 rta::meta::ArtLedger ledger;std::string ledgerPath;
 std::vector<State>st;std::vector<unsigned char>resolved;
 std::atomic<size_t>next{0};std::atomic<int>resumeSkip{0},hit{0},done{0},rej{0},fail{0},llmReq{0};
 std::atomic<bool>fatal{false},running{true};std::mutex statMtx,ledgerMtx;Stats stats;
};
static std::atomic<size_t>g_api_artist_active{0},g_api_cover_active{0};
static std::mutex g_api_bg_mtx;static std::shared_ptr<ApiFillBg>g_api_artist_bg,g_api_cover_bg;
static std::atomic<bool>g_shutdown{false};static constexpr int kApiFillConcurrency=2;
static std::atomic<int>paidCalls{0},freeCalls{0};
static rta::meta::ArtLedger apifill_ledger_load(std::string*){return {};}
static void apifill_ledger_save_locked(const rta::meta::ArtLedger&){}
static bool artistimg_has_cache(const std::string&){return false;}
static bool cover_has_disk(const std::string&){return false;}
static void play_log(const std::string&){}
static void apibatch_mark(std::shared_ptr<ApiFillBg>,const ApiJob&,const char*,const char*){}
static void apibatch_phase_sources(std::shared_ptr<ApiFillBg>){++freeCalls;if(mode==0)llm=false;else master=false;}
static void apibatch_phase_llm(std::shared_ptr<ApiFillBg>){++paidCalls;}
static void apibatch_phase_pages(std::shared_ptr<ApiFillBg>){}
'''+function+r'''
int main(){int failures=0;
 for(mode=0;mode<2;++mode){
  master=true;llm=true;paidCalls=0;freeCalls=0;
  api_bg_start({ApiJob{false,"key","Private","Owner",{}}},false,{});
  while(g_api_cover_active.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
  bool ok=freeCalls.load()==kApiFillConcurrency&&paidCalls.load()==0;
  std::cout<<(ok?"PASS ":"FAIL ")<<"disable-"<<(mode==0?"metallm":"metaon")<<"-during-free-phase-stops-new-paid-stage\n";failures+=!ok;
 }return failures?1:0;
}
'''
    cpp=evidence/'late-paid.cpp';exe=evidence/'late-paid.exe';cpp.write_text(code,encoding='utf-8')
    built=subprocess.run([str(compiler),'-std=c++23','-O1',str(cpp),'-o',str(exe)],capture_output=True)
    (evidence/'late-paid-compile.log').write_bytes(built.stdout+built.stderr)
    if built.returncode:raise RuntimeError('late paid coordinator seam failed to compile')
    run=subprocess.run([str(exe)],capture_output=True,text=True,encoding='utf-8',timeout=10)
    (evidence/'late-paid.log').write_text(run.stdout+run.stderr,encoding='utf-8');print(run.stdout,end='')
    return [dict(case=line[5:],passed=line.startswith('PASS ')) for line in run.stdout.splitlines() if line.startswith(('PASS ','FAIL '))]


def paid_request_controls(source,compiler,evidence):
    """Execute each production chat/tool transition with counted offline IO."""
    text=source.read_text(encoding='utf-8-sig');begin=text.index('static void apibatch_phase_llm(std::shared_ptr<ApiFillBg> bg) {')
    end=text.index('\n}\n',begin)+3;function=text[begin:end]
    code=r'''
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <functional>
#include <iostream>
#include <chrono>
#include <algorithm>
struct MetaSettings{bool on=true,llm=true;};
static bool master=true,llm=true,cap=true;static int mode=0,chats=0,tools=0;
static MetaSettings meta_settings_load(){return {master,llm};}
namespace rta::net {struct Response{};static Response http_post(const std::string&,const std::string&,const std::vector<std::string>&,const std::string&,int){return {};}}
namespace rta::meta {
struct LlmConfig{std::string apiKey="FAKE_OFFLINE_ONLY",model="fixture";std::function<rta::net::Response(const std::string&,const std::string&,const std::vector<std::string>&)>transport;};
static bool llm_check_cap(const LlmConfig&,const std::string&,std::string*){return cap;}
struct ToolCall{std::string id="id",name="web_search",arguments="{}";};
struct LlmMessage{std::string role,content,toolCallsJson;};
struct LlmReply{bool ok=true;std::string content,error;std::vector<ToolCall>toolCalls;};
static LlmReply llm_chat(const LlmConfig&,const std::vector<LlmMessage>&,int,float,bool,const std::string& ts={}){
 ++chats;LlmReply reply;reply.content="{}";
 if(mode==1){llm=false;reply.content="";reply.toolCalls.resize(1);}
 if(mode==2||mode==3){if(chats==1){reply.content="";reply.toolCalls.resize(2);}}
 if(mode==4){master=false;reply.content="";}
 if(mode==5||mode==9){if(chats==1){reply.content="";reply.toolCalls.resize(1);}else{llm=false;reply.content="";if(mode==5)reply.toolCalls.resize(1);}}
 if(mode==7&&chats==1)llm=false;
 if(mode==8&&chats==1){reply.content="";reply.toolCalls.resize(1);}
 return reply;
}
static std::string llm_execute_web_tool(const LlmConfig&,const std::string&,const std::string&,int,bool*){++tools;if(mode==2)master=false;if(mode==3)cap=false;if(mode==8)llm=false;return "{}";}
static std::string llm_web_tools_json(){return "[]";}
static std::string llm_tool_calls_to_json(const std::vector<ToolCall>&){return "[]";}
static LlmMessage llm_tool_result_message(const std::string&,const std::string&){return {};}
struct ArtBatchSpan{size_t from=0,to=0;};static constexpr int kArtBatchSize=20;
static std::vector<ArtBatchSpan> art_batch_spans(size_t n,int batch){std::vector<ArtBatchSpan>out;for(size_t i=0;i<n;i+=batch)out.push_back({i,std::min(n,i+batch)});return out;}
struct ArtBatchItem{int id=0;bool isArtist=false;std::string key,name,artist;std::vector<std::string>tracks;};
struct AnswerItem{std::vector<std::string>pages,images;std::string why;};
struct ArtBatchReply{bool ok=true,fence=false,salvaged=false;int withPages=0;std::string why,echo;std::vector<AnswerItem>items;};
static std::string art_batch_system_prompt(){return "fixture";}
static std::string art_batch_user_prompt(const std::vector<ArtBatchItem>&){return "fixture";}
static ArtBatchReply art_batch_parse(const std::string&,const std::vector<ArtBatchItem>&){return {};}
}
struct ApiJob{bool isArtist=false;std::string key,name,artist;std::vector<std::string>items;};
struct State{std::vector<std::string>pages,images;std::string why;};
struct ApiFillBg{std::vector<ApiJob>jobs;rta::meta::LlmConfig cfg;bool artist=false,primaryArtistDone=false,allowPaid=true;std::vector<unsigned char>resolved;std::vector<State>st;std::atomic<bool>fatal{false};std::atomic<int>llmReq{0},batches{0};std::mutex lastMtx;std::string lastErr;};
static std::atomic<bool>g_shutdown{false};static constexpr int kApiBatchMaxTokens=1600,kApiBatchLlmMs=20000,kApiFillToolRounds=1,kApiFillToolHopMs=5000,kApiAutoMax=40;
static void play_log(const std::string&){}
static std::string apifill_log_safe(const std::string&s,const std::string&){return s;}
static bool apifill_fatal_error(const std::string&){return false;}
'''+function+r'''
int main(){int failures=0;
 const char* names[]={"paid-start-disabled","first-chat-then-disabled-tool","first-tool-then-master-off","first-tool-exhausts-cap","empty-chat-then-disabled-closing","second-chat-then-disabled-closing-tool","enabled-normal-next-batch","disabled-before-next-batch","first-tool-then-disabled-next-chat","second-chat-then-disabled-final-chat"};
 const int expectedChats[]={0,1,1,1,1,2,2,1,1,2},expectedTools[]={0,0,1,1,0,1,0,0,1,1};
 for(mode=0;mode<10;++mode){master=llm=cap=true;chats=tools=0;if(mode==0)master=false;
  auto bg=std::make_shared<ApiFillBg>();size_t count=(mode==6||mode==7)?21:1;bg->jobs.resize(count);bg->resolved.resize(count);bg->st.resize(count);apibatch_phase_llm(bg);
  bool ok=chats==expectedChats[mode]&&tools==expectedTools[mode];std::cout<<(ok?"PASS ":"FAIL ")<<names[mode]<<" chats="<<chats<<" tools="<<tools<<"\n";failures+=!ok;
 }return failures?1:0;
}
'''
    cpp=evidence/'paid-requests.cpp';exe=evidence/'paid-requests.exe';cpp.write_text(code,encoding='utf-8')
    built=subprocess.run([str(compiler),'-std=c++23','-O1',str(cpp),'-o',str(exe)],capture_output=True)
    (evidence/'paid-requests-compile.log').write_bytes(built.stdout+built.stderr)
    if built.returncode:raise RuntimeError('actual paid request orchestration seam failed to compile')
    run=subprocess.run([str(exe)],capture_output=True,text=True,encoding='utf-8',timeout=10)
    (evidence/'paid-requests.log').write_text(run.stdout+run.stderr,encoding='utf-8');print(run.stdout,end='')
    return [dict(case=line[5:],passed=line.startswith('PASS ')) for line in run.stdout.splitlines() if line.startswith(('PASS ','FAIL '))]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ['exe','encoder','decoder','probe','evidence-dir']:p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--red-only',action='store_true')
    p.add_argument('--auto-only',action='store_true')
    p.add_argument('--source-file',type=Path,default=Path(__file__).resolve().parents[1]/'tools/rtaweb.cpp')
    p.add_argument('--compiler',type=Path,default=Path('C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'))
    args=p.parse_args();evidence=args.evidence_dir.resolve();evidence.mkdir(parents=True,exist_ok=False)
    rows=auto_decision_controls(args.source_file,args.compiler,evidence)
    rows+=late_paid_controls(args.source_file,args.compiler,evidence)
    rows+=paid_request_controls(args.source_file,args.compiler,evidence)
    if args.auto_only:
        (evidence/'results.json').write_text(json.dumps(dict(checks=rows,total=len(rows),passed=sum(x['passed'] for x in rows),failed=sum(not x['passed'] for x in rows)),indent=2),encoding='utf-8')
        return int(any(not x['passed'] for x in rows))
    # Version-profile paths contain SHA filenames; short unique runtime also
    # works when CTest's evidence/TEMP paths already approach MAX_PATH.
    root=Path(tempfile.mkdtemp(prefix='rlpcs-',dir=Path(tempfile.gettempdir()).anchor))
    (evidence/'runtime.json').write_text(json.dumps({'root':str(root)}),encoding='utf-8')
    home=root/'app';media=root/'music';home.mkdir();media.mkdir()
    for source,name in [(args.exe,'rtaweb.exe'),(args.decoder,'ffmpeg.exe'),(args.probe,'ffprobe.exe')]:shutil.copy2(source.resolve(),home/name)
    wav=root/'silence.wav'
    with wave.open(str(wav),'wb') as f:f.setparams((2,2,44100,0,'NONE','not compressed'));f.writeframes(bytes(4410*4))
    # Every album has one real tagged FLAC. 43 exceeds the legacy paid cap40.
    labels=['Free '+str(i).zfill(2) for i in range(43)]+['Mismatch','Transient','Master','Disabled','Ledger','Api']
    for label in labels:
        folder=media/label;folder.mkdir()
        subprocess.run([str(args.encoder),'-v','error','-i',str(wav),'-metadata','album='+label,'-metadata','artist=Fixture Artist','-metadata','title=Song',str(folder/'song.flac')],check=True,capture_output=True)
    image=png(161,256);calls=[];lock=threading.Lock()
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*a):pass
        def do_POST(self):self.body=self.rfile.read(int(self.headers.get('Content-Length','0'))).decode();self.do_GET()
        def do_GET(self):
            body=getattr(self,'body','');query=urllib.parse.parse_qs(body);term=query.get('s',[''])[0]
            with lock:calls.append(dict(path=self.path,body=body))
            if self.path.startswith('/picture.png'):out=image;mime='image/png';status=200
            elif self.path.startswith('/api/search/get'):
                label=next((x for x in sorted(labels,key=len,reverse=True) if x.lower() in term.lower()),'Unknown')
                status=503 if label=='Transient' else 200
                artist='Different Artist' if label=='Mismatch' else 'Fixture Artist'
                out=json.dumps({'result':{'albums':[{'name':label,'artists':[{'name':artist}],'picUrl':f'http://127.0.0.1:{self.server.server_port}/picture.png'}],'songs':[{'name':'Song','artists':[{'name':artist}],'album':{'id':123,'name':label,'picUrl':f'http://127.0.0.1:{self.server.server_port}/picture.png'}}]}}).encode();mime='application/json'
            else:out=b'{"error":"unexpected transport (including paid)"}';mime='application/json';status=500
            self.send_response(status);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(out)));self.end_headers();self.wfile.write(out)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
    provider=f'http://127.0.0.1:{server.server_port}'
    settings=home/'rlplayer-settings.conf'
    def configure(master='true',source='on'):
        settings.write_text('backend=null\nmetaon='+master+'\nmetallm=off\nmetasummary=0\nanlmaster=0\nisp=0\ngpuvendor=off\n'+''.join('metasrc_'+s+'='+(''+source if s=='netease' else 'false')+'\n' for s in ['netease','qq','dizzylab','itunes','discogs','musicbrainz','bgm','anilist','apple']) ,encoding='utf-8')
    configure('off')
    (home/'rlplayer-library.conf').write_text(str(media)+'\n',encoding='utf-8')
    # A private fake key deliberately exists: metallm=off must still cause zero
    # requests to its loopback endpoint, even after a provider miss.
    (home/'rlplayer-llm.conf').write_text('api_key=private-test-never-bill\nbase_url='+provider+'/paid\nmodel=fixture\nmonthly_cap=1\n',encoding='utf-8')
    env={**os.environ,'PATH':str(Path(os.environ['SystemRoot'])/'System32'),'RLPLAYER_LIBRARY_DIR':str(home),'RLPLAYER_UPDATE_DIR':str(home),'USERPROFILE':str(home),'APPDATA':str(home),'LOCALAPPDATA':str(home),'TEMP':str(root),'TMP':str(root),'RTA_NO_DEVICE':'1','RTA_NET_TESTS':'0','RLPLAYER_COVER_HOST':provider,'RLPLAYER_COVER_HOST_QQ':provider,'RLPLAYER_COVER_HOST_DIZZY':provider,'RLPLAYER_COVER_IMG_QQ':provider,'RLPLAYER_COVER_IMG_DIZZY':provider,'RLPLAYER_UPDATE_ALLOW_LOCAL':'1','RLPLAYER_UPDATE_MANIFEST':'http://127.0.0.1:1/api/v1/update/windows/latest'}
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}));base=f'http://127.0.0.1:{port}'
    def req(route,data=None):
        try:
            with opener.open(urllib.request.Request(base+route,data=data),timeout=20) as r:return r.status,r.read()
        except urllib.error.HTTPError as e:return e.code,e.read()
    def api(route,data=None):return json.loads(req(route,data)[1])
    def wait(pred,seconds=90):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if pred():return
            time.sleep(.03)
        raise RuntimeError('private source-controller wait timed out')
    def check(name,ok,**detail):rows.append(dict(case=name,passed=bool(ok),**detail));print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    log=(evidence/'app.log').open('wb');child=subprocess.Popen([str(home/'rtaweb.exe'),'--port',str(port)],cwd=home,env=env,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    try:
        def ready():
            try:return req('/api/ping')[0]==200
            except OSError:return False
        wait(ready);api('/api/lib/scan',b'');wait(lambda:api('/api/lib/status').get('tracks')==len(labels) and not api('/api/lib/status').get('scanning'))
        albums=api('/api/lib/albums')['albums'];keys={a['name']:a['k'] for a in albums};cache=home/'covercache';cache.mkdir(exist_ok=True)
        def prefetch(label):
            out=api('/api/lib/cover/prefetch?k='+keys[label],b'');wait(lambda:not api('/api/lib/cover/progress').get('running'));return out
        before=len(calls);prefetch('Master');check('master-off-makes-zero-provider-requests',len(calls)==before)
        path=urllib.parse.quote(str(media/'Master/song.flac'),safe='')
        before=len(calls);enriched=api('/api/enrich?file='+path+'&force=1',b'')
        check('explicit-enrich-also-respects-master-off',enriched.get('skipped') and len(calls)==before)
        configure('on','false');before=len(calls);prefetch('Disabled');check('all-source-switches-false-make-zero-requests',len(calls)==before)
        settings.write_text(settings.read_text().replace('metasrc_netease=false','metanetease=off'),encoding='utf-8')
        before=len(calls);prefetch('Ledger');check('legacy-provider-switch-off-retains-same-semantics',len(calls)==before)
        if args.red_only:return int(any(not x['passed'] for x in rows))
        configure();before=len(calls)
        # Empty old .none files used to suppress new providers for seven days.
        for label in labels[:43]:(cache/(keys[label]+'.none')).write_bytes(b'')
        batch=api('/api/lib/cover/prefetch',b'');wait(lambda:not api('/api/lib/cover/progress').get('running'),120)
        pictures=[req('/api/lib/cover?k='+keys[label]) for label in labels[:43]]
        check('all43-free-albums-bypass-old-negative-and-paid40-cap',all(result==(200,image) for result in pictures),queued=batch.get('queued'),hits=sum(result==(200,image) for result in pictures))
        check('canonical-provider-real-protocol-and-only-enabled-source',len(calls)>before and all(c['path'].startswith(('/api/search/get','/picture.png')) for c in calls[before:]))
        proofs=[json.loads((cache/(keys[label]+'.jpg.source.json')).read_text()) for label,result in zip(labels[:43],pictures) if result[0]==200]
        check('free-hits-have-exact-v2-release-provenance',len(proofs)==43 and all(x.get('version')==2 and x.get('matchProvider')=='netease' and x.get('albumArtist')=='Fixture Artist' for x in proofs))
        check('wrong-full-artist-never-becomes-album-art',req('/api/lib/cover?k='+keys['Mismatch'])[0]==404)
        check('transient-provider-failure-never-writes-negative',not (cache/(keys['Transient']+'.none')).exists())
        negative=json.loads((cache/(keys['Mismatch']+'.none')).read_text())
        check('new-provider-negative-binds-actual-chain-revision',negative.get('coverChainRevision')==2)
        # Same chain revision miss may be skipped; legacy ledger miss must not
        # permanently keep the public API behind an absent paid credential.
        (cache/'apifill-ledger.json').write_text(json.dumps({'v':1,'items':[{'k':'c|'+keys['Api'],'o':'miss','s':'','t':int(time.time())}]}),encoding='utf-8')
        # API has already been visited by bulk prefetch; use a fresh revision
        # proof by removing only this test-owned positive record and blob.
        for path in cache.glob(keys['Api']+'*'):path.unlink()
        (home/'rlplayer-llm.conf').write_text('api_key=\nbase_url='+provider+'/paid\n',encoding='utf-8')
        out=api('/api/lib/cover/apiFill?k='+keys['Api'],b'');wait(lambda:not api('/api/lib/cover/progress').get('apiRunning'))
        check('public-api-free-without-key-and-old-ledger-miss-retried',out.get('ok') and out.get('queued')==1 and req('/api/lib/cover?k='+keys['Api'])==(200,image),response=out)
        check('disabled-paid-stage-never-contacts-loopback-llm',not any('/paid' in c['path'] for c in calls))
        manual=png(163,256);out=api('/api/lib/cover/set?k='+keys['Mismatch'],manual)
        record=cache/(keys['Mismatch']+'.jpg.source.json');before_record=record.read_bytes();before=len(calls)
        prefetch('Mismatch')
        check('current-manual-never-overwritten-or-requeried-by-provider',out.get('ok') and req('/api/lib/cover?k='+keys['Mismatch'])==(200,manual) and record.read_bytes()==before_record and len(calls)==before)
        file=urllib.parse.quote(str(media/'Mismatch/song.flac'),safe='')
        api('/api/cover/set?file='+file,manual);api('/api/cover/reset?file='+file,b'')
        reset_records=list((home/'trackcovercache').glob('*.source.json'));reset_before={str(p):p.read_bytes() for p in reset_records}
        prefetch('Transient')
        check('provider-failure-leaves-explicit-track-reset-tombstone-intact',bool(reset_before) and all(Path(p).read_bytes()==b and json.loads(b).get('source')=='reset' for p,b in reset_before.items()))
    finally:
        if child.poll() is None:req('/api/stop',b'');req('/api/quit',b'');child.wait(timeout=30)
        log.close();server.shutdown();server.server_close()
        (evidence/'transport.json').write_text(json.dumps(calls,indent=2),encoding='utf-8')
        (evidence/'results.json').write_text(json.dumps(dict(checks=rows,total=len(rows),passed=sum(x['passed'] for x in rows),failed=sum(not x['passed'] for x in rows),runtime=str(root)),indent=2),encoding='utf-8')
    return int(any(not x['passed'] for x in rows))


if __name__=='__main__':raise SystemExit(main())
