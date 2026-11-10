"""Read-only risk probes: parse untrusted strings and invoke a no-op loopback handler.

Does not download/install updates, open devices, or load application profiles.
Findings are reported as JSON; this is an audit probe, not a passing gate.
"""
import pathlib,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
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
int main(){using namespace rta::update;
 const std::string hash(64,'a');
 const std::string good="{\"version\":\"99.0.0\",\"download_url\":\"https://github.com/okhuoyanyan/RLPlayer2/releases/download/v99/a.zip\",\"sha256\":\""+hash+"\",\"files\":[{\"path\":\"rtaweb.exe\",\"sha256\":\""+hash+"\",\"size\":1}]}";
 const auto truncated=good.substr(0,good.size()-1),trailing=good+"TRAILING_GARBAGE";
 printf("{\"probe\":\"manifest\",\"valid_fixture\":%s,\"truncated_base_accepted\":%s,\"truncated_extended_accepted\":%s,\"trailing_base_accepted\":%s,\"trailing_extended_accepted\":%s}\n",parse_manifest_ext(good).ok?"true":"false",parse_update_manifest(truncated).valid?"true":"false",parse_manifest_ext(truncated).ok?"true":"false",parse_update_manifest(trailing).valid?"true":"false",parse_manifest_ext(trailing).ok?"true":"false");
 _putenv_s("RLPLAYER_UPDATE_ALLOW_LOCAL","1");
 printf("{\"probe\":\"update_local_allowlist\",\"localhost_suffix_accepted\":%s,\"loopback_userinfo_to_other_host_accepted\":%s}\n",url_allowed("http://localhost.example.invalid/payload.zip")?"true":"false",url_allowed("http://127.0.0.1@other.example.invalid/payload.zip")?"true":"false");
 _putenv_s("RLPLAYER_UPDATE_ALLOW_LOCAL","");
 std::atomic<int> calls{0};rta::net::WebUi ui;
 if(!ui.start(0,[&](const rta::net::HttpRequest&,std::string& ct){++calls;ct="application/json";return std::string("{\"ok\":true,\"noop\":true}");},true))return 2;
 const auto response=exchange(ui.port(),"POST /api/noop HTTP/1.1\r\nHost: rebound.example.invalid\r\nOrigin: http://rebound.example.invalid\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
 printf("{\"probe\":\"loopback_host_origin\",\"untrusted_host_reached_handler\":%s,\"http_200\":%s}\n",calls.load()==1?"true":"false",response.rfind("HTTP/1.1 200",0)==0?"true":"false");
 ui.stop();return 0;}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-risk-probe-') as directory:
 directory=pathlib.Path(directory);cpp=directory/'probe.cpp';exe=directory/'probe.exe';cpp.write_text(code,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O0','-I'+str(root/'src'),str(cpp),'-o',str(exe),'-lwinhttp','-lws2_32'],check=True)
 sys.exit(subprocess.run([str(exe)],cwd=directory).returncode)
