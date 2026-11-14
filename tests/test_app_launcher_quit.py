"""Exercise only launcher's HTTP quit sender against loopback mocks, never stop a process."""
import os,pathlib,subprocess,sys,tempfile
root=pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT',pathlib.Path(__file__).resolve().parents[1]))
s=(root/'tools/launcher.cpp').read_text(encoding='utf-8');a=s.index('static bool http_get_quit(');function=s[a:s.index('\n}',a)+2]
prefix=r'''
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <thread>
#include <string>
#include <cstdio>
#include <cstring>
#include <chrono>
'''
main=r'''
int main(int argc,char**argv){WSADATA wd{};WSAStartup(MAKEWORD(2,2),&wd);
if(argc>1){int port=atoi(argv[1]);bool ok=http_get_quit(port);printf("live_quit_acknowledged=%d port=%d\n",ok?1:0,port);WSACleanup();return ok?0:1;}
int checks=0,failures=0;
auto check=[&](bool b,const char* label){++checks;if(!b)++failures;printf("%s %s\n",b?"PASS":"FAIL",label);};
auto exchange=[&](const std::string& reply,bool fragmented){SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in sa{};sa.sin_family=AF_INET;sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);bind(listener,(sockaddr*)&sa,sizeof(sa));listen(listener,1);int n=sizeof(sa);getsockname(listener,(sockaddr*)&sa,&n);std::string request;
std::thread server([&]{SOCKET client=accept(listener,nullptr,nullptr);char buffer[2048];int count=recv(client,buffer,sizeof(buffer),0);if(count>0)request.assign(buffer,count);
if(fragmented){send(client,reply.data(),4,0);std::this_thread::sleep_for(std::chrono::milliseconds(20));send(client,reply.data()+4,(int)reply.size()-4,0);}else if(!reply.empty())send(client,reply.data(),(int)reply.size(),0);shutdown(client,SD_BOTH);closesocket(client);});
bool ok=http_get_quit(ntohs(sa.sin_port));server.join();closesocket(listener);return std::pair<bool,std::string>{ok,request};};
auto accepted=exchange("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}",false);
check(accepted.second.rfind("POST /api/quit HTTP/1.1\r\n",0)==0,"quit uses POST required by backend method guard");
check(accepted.second.find("Content-Length: 0\r\n")!=std::string::npos,"empty POST explicitly declares zero body length");
check(accepted.first,"HTTP 200 is acknowledged");
check(!exchange("HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n",false).first,"HTTP 405 is not falsely reported delivered");
check(!exchange("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n",false).first,"HTTP 500 is not falsely reported delivered");
check(exchange("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}",true).first,"fragmented HTTP status line is handled");
check(!exchange("",false).first,"empty connection close is not acknowledged");
check(!http_get_quit(0),"invalid port is rejected without connection");WSACleanup();printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-quit-test-') as directory:
 directory=pathlib.Path(directory);cpp=directory/'test.cpp';exe=directory/'test.exe';cpp.write_text(prefix+function+main,encoding='utf-8')
 subprocess.run([compiler,'-std=c++17',str(cpp),'-o',str(exe),'-lws2_32'],check=True)
 args=[str(exe)]
 if len(sys.argv)>2:
  if not sys.argv[2].startswith('--live-port='):raise SystemExit('Expected explicit --live-port=N')
  port=int(sys.argv[2].split('=',1)[1])
  if not 19000<=port<=19999:raise SystemExit('Live mode restricted to isolated QA port range 19000..19999')
  args.append(str(port))
 sys.exit(subprocess.run(args).returncode)
