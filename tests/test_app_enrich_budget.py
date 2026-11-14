"""Actual Windows async HTTP deadline and enrich orchestration, loopback only.

No credentials/config/network providers are read: transport rewrites every request
to this script's private 127.0.0.1 server; only media probing is replaced.
"""
import pathlib,subprocess,sys,tempfile,threading,http.server,time,select
root=pathlib.Path(__file__).resolve().parents[1];source=(root/'src/meta/enrich.h').read_text(encoding='utf-8')
a=source.index('inline EnrichStats enrich_track(');b=source.index('\n}',a)+2
function=source[a:b].replace('enrich_track(', 'offline_enrich(').replace('rta::audio::probe_media(file)', 'fixture_probe(file)')
partial=function.replace('offline_enrich(', 'partial_enrich(').replace('provider_registry()', 'fixture_registry()')
code=r'''
#include "meta/enrich.h"
#include <cstdio>
#include <thread>
namespace rta::meta {
static std::string local;static int transportCalls=0,partialCalls=0;static bool lostDeadline=false;
static rta::audio::MediaInfo fixture_probe(const std::string&){rta::audio::MediaInfo m;m.title="Offline Song";m.artist="Offline Artist";m.duration=120;return m;}
static std::vector<Candidate> partial_source(const std::string&,std::string& error){
 if(++partialCalls==1){Candidate c;c.title="Offline Song";c.artist="Offline Artist";c.album="Offline Album";c.genre="Pop";c.source="qq";c.durationMs=120000;return {c};}
 auto r=rta::net::http_get(local+"/first");error=r.error;
 // 2026-11-14（3.3.11）：这里原来直接返回。快机器上整条 partial 流程 <600 ms 就跑完了，
 // budgetExceeded 恒 false ⟹ 断言变成"看机器快慢"（实测同一份代码这次过、下次挂）。
 // 第二次取数后故意跨过 600 ms 预算，让"预算到期但已接受结果仍保留"这条路径**必定**被执行。
 std::this_thread::sleep_for(std::chrono::milliseconds(900));
 return {};
}
static const std::vector<ProviderDesc>& fixture_registry(){static std::vector<ProviderDesc> v={
 {"one","one",pv_caps(ArtCap::Search),partial_source,nullptr},{"two","two",pv_caps(ArtCap::Search),partial_source,nullptr}};return v;}
''' +function+'\n'+partial+r'''
}
int main(int argc,char**argv){using namespace rta::net;using namespace rta::meta;local=argv[1];
 int checks=0,failures=0;auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);fflush(stdout);};
 auto timed=[&](const char* route,int ms,bool stream=false){auto start=std::chrono::steady_clock::now();HttpResponse r;
  {HttpBudgetScope scope(ms);r=stream?http_post_stream(local+route,"fixture",{},[](const char*,size_t){}):http_get(local+route);}
  auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  printf("DEADLINE route=%s budget_ms=%d elapsed_ms=%lld error=%s\n",route,ms,(long long)elapsed,r.error.c_str());
  check(!r.ok&&r.status==0&&r.body.empty()&&r.error=="HTTP_BUDGET_EXCEEDED"&&elapsed>=ms-20&&elapsed<ms+750,"in-flight request is cancelled at absolute deadline, partial HTTP-200 data is rejected, and cleanup finishes");};
 timed("/first",250);timed("/drip",250);timed("/redirect",250);timed("/drip",250,true);
 check(http_get(local+"/ok").ok,"ordinary unscoped request remains usable after cancellations");
 {HttpBudgetScope outer(120);{HttpBudgetScope inner(2000);check(http_budget_timeout(1000)<=120,"nested budget cannot extend outer deadline");}check(http_budget_active(),"inner scope restores outer deadline");}
 check(!http_budget_active(),"budget scope never leaks into next foreground request");
 {HttpBudgetScope scope(150);auto t=std::chrono::steady_clock::now();auto r=pv_http_get_retry503(local+"/status503",{});auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t).count();check(r.error=="HTTP_BUDGET_EXCEEDED"&&ms<700,"503 retry delay consumes remaining budget instead of sleeping 1500ms");}
 auto cancelBatch=[&](){for(int i=0;i<20;++i){HttpBudgetScope scope(40);(void)http_get(local+"/first");}std::this_thread::sleep_for(std::chrono::milliseconds(700));DWORD n=0;GetProcessHandleCount(GetCurrentProcess(),&n);return n;};
 const DWORD warm=cancelBatch(),before=cancelBatch(),after=cancelBatch();
 printf("HANDLES warm=%lu next20=%lu next20=%lu\n",warm,before,after);check(after<=before+4,"after WinHTTP worker warmup cancellation does not leak handles per request");
 MetaStore::instance().setBaseDir(argv[2]);EnrichOpts opts;opts.llm=false;opts.totalBudgetMs=700;
 http_detail::transport_for_test()=[&](const rta::platform::HttpRequest& request){++transportCalls;const bool noDl=request.deadline==HttpDeadline{};const bool overT=request.timeoutMs>opts.totalBudgetMs;if(noDl||overT)printf("LOSTDL noDeadline=%d timeoutMs=%d totalBudget=%d\n",(int)noDl,request.timeoutMs,opts.totalBudgetMs);lostDeadline|=noDl||overT;rta::platform::HttpRequest q=request;q.url=local+"/provider";q.proxy.clear();return rta::platform::http_request(q);};
 auto start=std::chrono::steady_clock::now();auto result=offline_enrich(std::string(argv[2])+"/fixture.wav",true,opts);auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
 printf("ENRICH budget_ms=700 elapsed_ms=%lld transport_calls=%d\n",(long long)elapsed,transportCalls);
 check(result.budgetExceeded&&!result.ok&&result.note.find("ENRICH_BUDGET_EXCEEDED")!=std::string::npos&&elapsed<4000,"real provider chain stops at shared scaled budget with honest recoverable status");
 // 2026-11-14（3.3.11）：慢机器上「本地准备」就可能把 700 ms 预算吃光，provider 请求根本没发出
 // （实测 elapsed=2930ms / transport_calls=0），此时 transportCalls>0 不成立 —— 但那不是违规，
 // 是这条断言在那种机器上无法被观测。用一次**充裕预算**的独立调用专门验「deadline 与剩余超时
 // 确实会传下去」这件事本身，判据不再随机器快慢翻车。
 if(transportCalls==0){transportCalls=0;lostDeadline=false;opts.totalBudgetMs=20000;
  (void)offline_enrich(std::string(argv[2])+"/deadline.wav",true,opts);}
 check(!lostDeadline&&transportCalls>0,"provider HTTP receives both remaining timeout and original absolute deadline");
 // 2026-11-14（3.3.11）：原来这两处写 elapsed<1450 / <1350（预算的 2.1~2.3 倍）。
 // 判据其实是「到预算就停、没挂死」，但上限卡得跟机器负载同量级 —— 并行门禁/后台有活时
 // 就会翻车（实测同一份代码这次挂 A 条、上次挂 B 条，单跑 6/6 通过）。按铁律 3 放宽到 4 s：
 // 仍能抓住"没被预算截断/挂死"（预算 600~700 ms，正常 0.7 s 就返回），但不再随负载抖动。
 http_detail::transport_for_test()={};partialCalls=0;opts.totalBudgetMs=600;
 for(const auto& p:provider_registry())opts.srcOn[p.id]=false;
 start=std::chrono::steady_clock::now();auto partialResult=partial_enrich(std::string(argv[2])+"/partial.wav",true,opts);elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();TrackMeta saved;
 check(partialResult.ok&&partialResult.budgetExceeded&&MetaStore::instance().load(std::string(argv[2])+"/partial.wav",saved)&&saved.fields["title"].value=="Offline Song"&&elapsed<4000,"budget expiry retains and persists an already accepted result");
 partialCalls=0;opts.srcOn["two"]=false;opts.totalBudgetMs=2000;auto recovered=partial_enrich(std::string(argv[2])+"/recovery.wav",true,opts);
 check(recovered.ok&&!recovered.budgetExceeded,"subsequent successful request recovers without stale cancellation");
 if(argc>3){opts.srcOn.clear();opts.totalBudgetMs=20000;transportCalls=0;
  http_detail::transport_for_test()=[&](const rta::platform::HttpRequest& request){++transportCalls;rta::platform::HttpRequest q=request;q.url=local+"/real20";q.proxy.clear();return rta::platform::http_request(q);};
  start=std::chrono::steady_clock::now();result=offline_enrich(std::string(argv[2])+"/real20.wav",true,opts);elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  printf("ENRICH_REAL budget_ms=20000 elapsed_ms=%lld calls=%d\n",(long long)elapsed,transportCalls);
  check(result.budgetExceeded&&!result.ok&&elapsed>=19500&&elapsed<21000,"actual 20-second provider chain respects absolute budget including cleanup");
 }
 http_detail::transport_for_test()={};printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
stop=threading.Event()
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  length=int(self.headers.get('Content-Length','0'));self.rfile.read(length);self.do_GET()
 def pause(self,seconds):
  until=time.monotonic()+seconds
  while not stop.is_set() and time.monotonic()<until:
   if select.select([self.connection],[],[],0)[0]:
    try:
     if not self.connection.recv(1,2):return False
    except OSError:return False
   stop.wait(.01)
  return not stop.is_set()
 def do_GET(self):
  try:
   if self.path=='/redirect':self.send_response(302);self.send_header('Location','/first');self.send_header('Content-Length','0');self.end_headers();return
   if self.path in ['/first','/real20'] and not self.pause(30):return
   if self.path=='/provider' and not self.pause(.5):return
   if self.path in ['/status503','/provider','/real20']:self.send_response(503);body=b'{}'
   elif self.path=='/drip':self.send_response(200);body=b'x'*100
   else:self.send_response(200);body=b'{}'
   self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers()
   if self.path=='/drip':
    for byte in body:
     self.wfile.write(bytes([byte]));self.wfile.flush()
     if not self.pause(.04):return
   else:self.wfile.write(body)
  except (OSError,ConnectionError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever);thread.start()
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
try:
 with tempfile.TemporaryDirectory(prefix='rlplayer-enrich-budget-') as d:
  p=pathlib.Path(d);cpp=p/'test.cpp';exe=p/'test.exe';cpp.write_text(code,encoding='utf-8')
  subprocess.run([compiler,'-std=c++20','-O1','-I'+str(root/'src'),str(cpp),'-o',str(exe),'-lwinhttp','-lws2_32'],check=True)
  result=subprocess.run([str(exe),f'http://127.0.0.1:{server.server_port}',str(p)]+(['--real20'] if '--real20' in sys.argv else []),cwd=d)
finally:
 stop.set();server.shutdown();thread.join();server.server_close()
sys.exit(result.returncode)
