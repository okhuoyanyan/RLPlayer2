// Synthetic child process for actual launcher startup/rollback fault injection.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <fstream>
#include <chrono>
#pragma comment(lib,"ws2_32.lib")
int main(int argc,char**argv){
 std::ofstream log("fixture-child.log",std::ios::app);log<<"pid="<<GetCurrentProcessId();
#ifdef RTA_UPDATE_FIXTURE_CRASH
 log<<" mode=crash exit=37\n";return 37;
#else
 log<<" mode=serve\n";log.flush();int port=0;for(int i=1;i+1<argc;++i)if(std::string(argv[i])=="--port")port=atoi(argv[++i]);if(port<19000||port>19999)return 2;
 WSADATA w{};WSAStartup(MAKEWORD(2,2),&w);SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=htons(port);if(bind(s,(sockaddr*)&addr,sizeof(addr)))return 3;listen(s,8);
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);bool done=false;
 while(!done&&std::chrono::steady_clock::now()<end){fd_set rd;FD_ZERO(&rd);FD_SET(s,&rd);timeval timeout{0,250000};if(select(0,&rd,nullptr,nullptr,&timeout)<=0)continue;SOCKET c=accept(s,nullptr,nullptr);DWORD ms=500;setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,(const char*)&ms,sizeof(ms));char bytes[2048];int n=recv(c,bytes,sizeof(bytes),0);
 if(n>0){std::string request(bytes,n);done=request.rfind("POST /api/quit ",0)==0;bool ping=request.find("/api/ping ")!=std::string::npos;std::string body=done?"{\"ok\":true}":ping?"{\"ok\":true,\"app\":\"RLPlayer\",\"version\":\"0.0.1\"}":"<!doctype html><title>Private update rollback fixture</title><p>Private fixture only</p>";std::string response="HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: "+std::string(ping||done?"application/json":"text/html")+"\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;send(c,response.data(),int(response.size()),0);log<<"request="<<request.substr(0,request.find('\r'))<<"\n";log.flush();}closesocket(c);
 }closesocket(s);WSACleanup();log<<"serve_exit=0 cooperative="<<done<<"\n";return done?0:4;
#endif
}
