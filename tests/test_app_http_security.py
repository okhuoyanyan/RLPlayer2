"""Real loopback HTTP and pure update-parser rejection controls, no profiles/devices."""
import os,pathlib,subprocess,sys,tempfile
root=pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT',pathlib.Path(__file__).resolve().parents[1]))
code=r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "net/update.h"
#include "net/webui.h"
#include <atomic>
#include <cstdio>
#include <cstring>
static std::string exchange(unsigned short port,const std::string& request){
 SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in sa{};sa.sin_family=AF_INET;sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);sa.sin_port=htons(port);
 if(connect(s,(sockaddr*)&sa,sizeof(sa)))return "connect-failed";send(s,request.data(),(int)request.size(),0);std::string response;char buf[4096];int n;
 while((n=recv(s,buf,sizeof(buf),0))>0)response.append(buf,n);closesocket(s);return response;}
int main(){using namespace rta::update;int checks=0,failures=0;
auto check=[&](bool ok,const char* what){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",what);};
 const std::string hash(64,'a');
 const std::string base="{\"schema\":2,\"platform\":\"windows\",\"channel\":\"stable\",\"metadata_version\":1,\"version\":\"99.0.0\",\"published_at\":\"2026-11-01T00:00:00Z\",\"expires_at\":\"2027-01-01T00:00:00Z\",\"install_mode\":\"incremental\",\"reason\":\"\",\"files\":[{\"path\":\"rtaweb.exe\",\"sha256\":\""+hash+"\",\"size\":1,\"object\":\""+hash+".zip\",\"object_sha256\":\""+hash+"\",\"object_size\":1}],\"delete\":[],\"manual\":{\"cn_url\":\"https://pan.baidu.com/s/test\",\"global_url\":\"https://github.com/okhuoyanyan/RLPlayer2/releases\",\"extraction_code\":\"\",\"sha256\":\""+hash+"\",\"size\":1},\"notes\":\"\"}";
 const auto good=base;
 const long long now=parse_rfc3339_epoch("2026-11-03T00:00:00Z");
 check(parse_manifest_payload(good,"3.2.1",now,"stable").ok,"valid schema-2 incremental manifest remains accepted");
 for(const auto& bad:{good.substr(0,good.size()-1),good+"GARBAGE"})check(!parse_manifest_payload(bad,"3.2.1",now,"stable").ok,"manifest parser rejects incomplete or trailing JSON");
 for(const std::string size:{"\"1\"","true","-1","1.5","1e309","9223372036854775808","01","1e","1."}){
  auto bad=good;const auto at=bad.find("\"size\":1");bad.replace(at,8,"\"size\":"+size);
  check(!parse_manifest_payload(bad,"3.2.1",now,"stable").ok,"manifest size must be a legal, finite, nonnegative representable integer");}
 for(const std::string version:{"99","null","true"}){auto bad=good;const auto at=bad.find("\"99.0.0\"");bad.replace(at,8,version);check(!parse_manifest_payload(bad,"3.2.1",now,"stable").ok,"version is a string, never coerced from another JSON type");}
 auto missingVersion=good;const auto at=missingVersion.find("\"version\":\"99.0.0\",");missingVersion.erase(at,19);check(!parse_manifest_payload(missingVersion,"3.2.1",now,"stable").ok,"manifest requires a valid version before staging paths");
 auto wrongFile=base;const auto fileAt=wrongFile.find("\"path\":\"rtaweb.exe\"");wrongFile.replace(fileAt,19,"\"path\":123");check(!parse_manifest_payload(wrongFile,"3.2.1",now,"stable").ok,"file path must be a JSON string");
 const auto entry="{\"path\":\"rtaweb.exe\",\"sha256\":\""+hash+"\",\"size\":1,\"object\":\""+hash+".zip\",\"object_sha256\":\""+hash+"\",\"object_size\":1}]";
 auto duplicate=base;const auto fileEnd=duplicate.find("}],\"delete\"");duplicate.replace(fileEnd,2,"},"+entry);check(!parse_manifest_payload(duplicate,"3.2.1",now,"stable").ok,"duplicate destination paths are rejected");
 for(const std::string path:{"tools/a.exe:stream","tools/NUL.txt","tools/file.exe.","tools/file.exe ","tools//a.exe","tools/"})check(!safe_install_rel(path),"Windows ambiguous/device/ADS target paths are rejected");
 const auto entry2="{\"path\":\"RTAWEB.EXE\",\"sha256\":\""+hash+"\",\"size\":1,\"object\":\""+hash+".zip\",\"object_sha256\":\""+hash+"\",\"object_size\":1}]";
 auto mixed=base;const auto mixedEnd=mixed.find("}],\"delete\"");mixed.replace(mixedEnd,2,"},"+entry2);check(!parse_manifest_payload(mixed,"3.2.1",now,"stable").ok,"case-folded duplicate Windows destination is rejected");
 check(norm_rel(".notice")==".notice","safe leading-dot filenames are preserved rather than silently renamed");
 for(const auto& malformed:{std::string("{\"a\":1,\"a\":2}"),std::string("{\"a\":\"")+char(0xc0)+char(0xaf)+"\"}"}){rta::net::Json value;check(!rta::net::json_parse_strict(malformed,value),"strict JSON rejects ambiguous keys and invalid UTF-8");}
 for(const auto& valid:{"{\"n\":0}","{\"n\":-0}","{\"n\":0.25}","{\"n\":1e-9}"}){rta::net::Json value;check(rta::net::json_parse_strict(valid,value),"legal JSON number grammar remains accepted");}
 _putenv_s("RLPLAYER_UPDATE_ALLOW_LOCAL","1");
 check(url_allowed("http://127.0.0.1:19263/api/v1/update/windows/latest")&&url_allowed("http://localhost:19263/api/v1/update/windows/latest"),"explicit local test flag still accepts exact loopback hosts");
 for(const auto& url:{"http://localhost.example.invalid/a.zip","http://127.0.0.1@other.example.invalid/a.zip","http://localhost:19263@other.example.invalid/a.zip","http://127.0.0.10:19263/a.zip","http://localhost:0/a.zip","http://localhost:99999/a.zip","http://localhost:19263/a.zip\r\nx: y"})check(!url_allowed(url),"local test flag cannot expand to another authority or inject a request");
 _putenv_s("RLPLAYER_UPDATE_ALLOW_LOCAL","");check(!url_allowed("http://127.0.0.1:19263/a.zip"),"local URLs remain disabled by default");
 for(bool loopbackOnly:{true,false}){
  std::atomic<int> calls{0};rta::net::WebUi ui;check(ui.start(0,[&](const rta::net::HttpRequest&,std::string& ct){++calls;ct="application/json";return std::string("{\"ok\":true,\"noop\":true}");},loopbackOnly),"isolated no-op HTTP server starts");
  const auto authority="127.0.0.1:"+std::to_string(ui.port());
  auto http=[&](std::string method,std::string host,std::string origin){const int before=calls;auto reply=exchange(ui.port(),method+" /api/noop HTTP/1.1\r\n"+host+"\r\n"+origin+"Content-Length: 0\r\nConnection: close\r\n\r\n");return std::pair{reply.rfind("HTTP/1.1 200",0)==0,calls.load()-before};};
  auto result=http("POST","Host: "+authority,"");check(result.first&&result.second==1,"native/script POST without Origin remains allowed");
  result=http("POST","host: "+authority,"Origin: http://"+authority+"\r\n");check(result.first&&result.second==1,"same-origin browser POST and lowercase Host are allowed");
  result=http("POST","Host: 127.0.0.1","");check(result.first&&result.second==1,"existing native quit sender bare loopback Host remains allowed");
  result=http("POST","Host: rebound.example.invalid","Origin: http://rebound.example.invalid\r\n");check(!result.first&&result.second==0,"untrusted Host never reaches handler in either bind mode");
  result=http("POST","Host: "+authority,"Origin: http://attacker.example.invalid\r\n");check(!result.first&&result.second==0,"cross-site browser POST is rejected before handler");
  result=http("POST","Host: "+authority,"Origin: null\r\n");check(!result.first&&result.second==0,"opaque browser Origin is rejected for state-changing requests");
  result=http("POST","Host: "+authority,"Origin: http://127.0.0.1:1\r\n");check(!result.first&&result.second==0,"same hostname with a different Origin port is rejected");
  result=http("POST","Host: "+authority,"Origin: http://127.0.0.1@attacker.example.invalid\r\n");check(!result.first&&result.second==0,"Origin userinfo does not impersonate loopback");
  result=http("POST","Host: "+authority+"\r\nHost: attacker.example.invalid","");check(!result.first&&result.second==0,"duplicate Host headers are rejected rather than first-value trusted");
  result=http("POST","Host: "+authority,"Origin: http://"+authority+"\r\nOrigin: http://attacker.example.invalid\r\n");check(!result.first&&result.second==0,"duplicate Origin cannot hide a cross-origin writer");
  result=http("POST","Host: 127.0.0.1:1","");check(!result.first&&result.second==0,"wrong target port is rejected even without browser Origin");
  result=http("POST","host: localhost:"+std::to_string(ui.port()),"origin: http://localhost:"+std::to_string(ui.port())+"\r\n");check(result.first&&result.second==1,"localhost same-origin and lowercase Origin header remain supported");
  result=http("GET","Host: "+authority,"Origin: http://attacker.example.invalid\r\n");check(result.first&&result.second==1,"read-only GET keeps existing HTTP behavior; endpoint method guard owns side effects");
  ui.stop();
 }
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-http-security-') as directory:
 directory=pathlib.Path(directory);cpp=directory/'test.cpp';exe=directory/'test.exe';cpp.write_text(code,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O0','-I'+str(root/'src'),str(cpp),'-o',str(exe),'-lwinhttp','-lws2_32'],check=True)
 sys.exit(subprocess.run([str(exe)],cwd=directory).returncode)
