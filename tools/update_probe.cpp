// tools/update_probe.cpp — signed incremental update selftest and offline drill tool.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "net/update.h"
#include "net/update_apply.h"
#include "version.h"

// Keep the gate and the standalone tests on exactly the same assertions.
#include "../tests/test_update_signature.cpp"
#include "../tests/test_update_protocol.cpp"

using namespace rta::update;

static int g_pass=0,g_fail=0;
static void check(bool condition,const std::string& name){
    if(condition){++g_pass;std::printf("  PASS  %s\n",name.c_str());}
    else{++g_fail;std::printf("  FAIL  %s\n",name.c_str());}
}
static std::string read_all(const std::string& path){return read_text_file(path);}

static int fake_launcher(){
    const std::string dir=install_dir(),cfg=read_text_file(join_path(dir,"fake-launcher.txt"));
    std::printf("[fake-launcher] dir=%s cfg='%s'\n",dir.c_str(),cfg.c_str());std::fflush(stdout);
    if(cfg.rfind("crash",0)==0)return 3;int port=8935;std::string version=current_version();
    {std::istringstream input(cfg);std::string what;input>>what;if(what=="serve")input>>port>>version;}
    WSADATA wsa{};WSAStartup(MAKEWORD(2,2),&wsa);SOCKET server=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(server==INVALID_SOCKET)return 4;sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons((u_short)port);
    inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
    if(bind(server,(sockaddr*)&address,sizeof(address))!=0){closesocket(server);WSACleanup();return 4;}listen(server,8);
    std::printf("[fake-launcher] 127.0.0.1:%d version=%s\n",port,version.c_str());std::fflush(stdout);
    const std::string body="{\"ok\":true,\"app\":\"RLPlayer\",\"version\":\""+version+"\"}";int served=0;
    for(;;){fd_set readSet;FD_ZERO(&readSet);FD_SET(server,&readSet);timeval timeout{6,0};
        if(select(0,&readSet,nullptr,nullptr,&timeout)<=0)break;SOCKET client=accept(server,nullptr,nullptr);if(client==INVALID_SOCKET)break;
        char input[2048];const int got=recv(client,input,sizeof(input)-1,0);if(got>0){input[got]=0;
            std::string response="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+
                std::to_string(body.size())+"\r\n\r\n"+body;send(client,response.c_str(),(int)response.size(),0);}
        closesocket(client);if(++served>=3)break;}
    closesocket(server);WSACleanup();return 0;
}

static std::string sha_of(const std::string& path){std::string hash,error;return sha256_file(path,hash,error)?hash:"ERR";}
static void hash_dir(const std::string& root,const std::string& rel,std::vector<std::pair<std::string,std::string>>& out){
    WIN32_FIND_DATAW data{};const std::string dir=rel.empty()?root:join_path(root,rel);
    HANDLE handle=FindFirstFileW(rta::platform::from_utf8(join_path(dir,"*")).c_str(),&data);if(handle==INVALID_HANDLE_VALUE)return;
    do{const std::wstring wide(data.cFileName);if(wide==L"."||wide==L"..")continue;const std::string name=rta::platform::detail::narrow(wide);
        const std::string child=rel.empty()?name:rel+"\\"+name;if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)hash_dir(root,child,out);
        else out.push_back({child,sha_of(join_path(root,child))});}while(FindNextFileW(handle,&data));FindClose(handle);
}
static void copy_tree(const std::string& from,const std::string& to){
    std::vector<std::string> files;list_files_rel(from,"",files);for(const auto& rel:files){if(rel.rfind("updates\\",0)==0)continue;
        const std::string destination=join_path(to,rel);const size_t slash=destination.find_last_of('\\');
        if(slash!=std::string::npos)ensure_dir(destination.substr(0,slash));
        CopyFileW(rta::platform::from_utf8(join_path(from,rel)).c_str(),rta::platform::from_utf8(destination).c_str(),FALSE);}
}
static void put(const std::string& root,const std::string& rel,const std::string& text){
    const std::string path=join_path(root,rel);const size_t slash=path.find_last_of('\\');if(slash!=std::string::npos)ensure_dir(path.substr(0,slash));
    write_text_file(path,text);
}
static std::string utc_time(std::time_t value){std::tm tm{};gmtime_s(&tm,&value);char text[32];
    std::snprintf(text,sizeof(text),"%04d-%02d-%02dT%02d:%02d:%02dZ",tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec);return text;}

int main(int argc,char** argv){
    SetConsoleOutputCP(CP_UTF8);const std::string mode=argc>1?argv[1]:"";
    if(mode.empty()){
        if(path_exists(join_path(install_dir(),"fake-launcher.txt")))return fake_launcher();
        std::printf("update_probe --selftest | --check | --download | --plan | --apply | --rollback | --confirm\n");
        std::printf("current=%s install=%s\n",current_version().c_str(),install_dir().c_str());return 0;
    }
    if(mode=="--version"){std::printf("%s\n",current_version().c_str());return 0;}
    if(mode=="--selftest"){
        struct{const char*a;const char*b;int want;}versions[]={{"1.5.9","1.5.10",-1},{"1.5.10","1.5.9",1},
            {"1.6.0","1.5.99",1},{"1.5.9","1.5.9",0},{"2.0","1.99.99",1},{"v1.5.9","1.5.9",0},
            {"1.5.9-beta","1.5.9",-1},{"1.5.9","1.5.9-beta",1},{"1.5","1.5.0",0},{"1.10.0","1.9.9",1},{"1.5.9","1.5.9 ",0}};
        for(const auto& c:versions)check(cmp_versions(c.a,c.b)==c.want,std::string("version ")+c.a+" / "+c.b);
        check(sha256_hex(std::string(""))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty FIPS vector: "+sha256_hex(std::string("")));
        check(sha256_hex(std::string("abc"))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc FIPS vector: "+sha256_hex(std::string("abc")));
        check(sha256_hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","SHA256 56-byte FIPS vector: "+sha256_hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")));
        std::string longBytes;longBytes.reserve(200000);for(int i=0;i<200000;++i)longBytes.push_back((char)((i*31+7)&255));
        check(sha256_hex(longBytes)=="8f9d1bf454d63cd9fc6edbe8f3f2331cc1f9b195c7ec90533717bf243ae966c7",
            "SHA256 independent 200000-byte multi-block vector");
        const std::string hashFile=join_path(rta::platform::temp_dir_utf8(),"update-probe-sha-"+std::to_string(GetCurrentProcessId())+".bin");
        write_text_file(hashFile,longBytes);std::string diskHash,diskError;const bool diskOk=sha256_file(hashFile,diskHash,diskError);
        check(diskOk&&diskHash==sha256_hex(longBytes),"streaming file SHA equals memory SHA across 64KiB reads");
        rta::platform::remove_file(rta::platform::from_utf8(hashFile));
        const int suiteFailures=signature_tests::run()+protocol_tests::run();g_fail+=suiteFailures;
        std::printf("update_probe --selftest: base PASS %d; total FAIL %d\n",g_pass,g_fail);return g_fail?1:0;
    }
    if(mode=="--check"){
        const bool ok=check_now(8000);std::printf("%s\n",check_json().c_str());return ok?0:2;
    }
    if(mode=="--download"){
        if(!check_now(8000)){std::printf("check failed: %s\n",check_json().c_str());return 2;}std::string error;
        if(!start_download(error)){std::printf("download not started: %s\n",error.c_str());return 3;}
        for(;;){std::this_thread::sleep_for(std::chrono::milliseconds(250));const std::string progress=progress_json();std::printf("%s\n",progress.c_str());
            if(progress.find("\"phase\":\"ready\"")!=std::string::npos)return 0;
            if(progress.find("\"phase\":\"error\"")!=std::string::npos)return 4;}
    }
    if(mode=="--mkfixture"&&argc>2){
        const std::string dir=argv[2],version=argc>3?argv[3]:"3.2.1";ensure_dir(dir);
        const char* programs[]={"rtaweb.exe","rtagpu.dll","rtagpu_cuda.dll","amdhip64_7.dll","ffmpeg.exe","ffprobe.exe",
            "WebView2Loader.dll","tools\\rtaplay.exe","tools\\rtatag.exe","README.txt"};
        for(const char* path:programs)put(dir,path,"OLD-"+version+"-"+path);
        {wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);CopyFileW(self,rta::platform::from_utf8(join_path(dir,"RLPlayer.exe")).c_str(),FALSE);}
        put(dir,"rlplayer-settings.conf","backend=null\r\n");put(dir,"rlplayer-library.conf","root=D:\\MUSIC\r\n");
        put(dir,"rlplayer-favs.json","[]\r\n");put(dir,"rlplayer-queue.txt","D:\\MUSIC\\b.flac\r\n");
        put(dir,"rlplayer-savedqueues.conf","night=D:\\MUSIC\\c.flac\r\n");put(dir,"rlplayer-llm.conf","key=test-only\r\n");
        put(dir,"rlplayer-peq.txt","0,0,1.0\r\n");put(dir,"data\\opra_index.json","{\"old\":true}\r\n");
        put(dir,"data\\lib.idx","LIBIDX-OLD\r\n");put(dir,"covercache\\abc.jpg","JPEG-OLD\r\n");
        put(dir,"WebView2Runtime\\msedgewebview2.exe","WVR-OLD\r\n");put(dir,"fake-launcher.txt","serve 8935 "+version+"\r\n");
        std::printf("fixture=%s version=%s\n",dir.c_str(),version.c_str());return 0;
    }
    if(mode=="--mkpkg"&&argc>4){
        const std::string oldDir=argv[2],newDir=argv[3],version=argv[4];const bool crash=argc>5&&std::string(argv[5])=="crash";
        rmtree(newDir);ensure_dir(newDir);copy_tree(oldDir,newDir);
        {wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);const std::string destination=join_path(newDir,"RLPlayer.exe");
            CopyFileW(self,rta::platform::from_utf8(destination).c_str(),FALSE);std::ofstream append(rta::platform::from_utf8(destination),std::ios::binary|std::ios::app);
            append<<"RLPLAYER-DRILL-NEW "<<version<<"\n";}
        put(newDir,"rtaweb.exe","NEW-"+version+"-rtaweb.exe");put(newDir,"rtagpu.dll","NEW-"+version+"-rtagpu.dll");
        put(newDir,"tools\\rtaplay.exe","NEW-"+version+"-tools\\rtaplay.exe");put(newDir,"data\\opra_index.json","{\"new\":true}\r\n");
        put(newDir,"data\\lib.idx","EVIL-LIBIDX\r\n");put(newDir,"rlplayer-settings.conf","EVIL-SETTINGS\r\n");
        put(newDir,"WebView2Runtime\\msedgewebview2.exe","EVIL-WVR\r\n");put(newDir,"covercache\\abc.jpg","EVIL-JPEG\r\n");
        put(newDir,"fake-launcher.txt",std::string(crash?"crash":"serve 8935 ")+version+"\r\n");
        std::printf("package=%s version=%s\n",newDir.c_str(),version.c_str());return 0;
    }
    if(mode=="--mkmanifest"&&argc>8){
        const std::string newDir=argv[2],oldDir=argv[3],version=argv[4],out=argv[5];
        const std::string manualSha=argc>11&&is_hex64(argv[11])?lower_ascii(argv[11]):std::string(64,'0');
        const long long manualSize=argc>10&&std::atoll(argv[10])>0?std::atoll(argv[10]):1;
        std::vector<std::pair<std::string,std::string>> newFiles,oldFiles;hash_dir(newDir,"",newFiles);hash_dir(oldDir,"",oldFiles);
        std::map<std::string,std::string> oldMap;for(const auto& item:oldFiles)oldMap[item.first]=item.second;
        const size_t outSlash=out.find_last_of("\\/");const std::string outParent=outSlash==std::string::npos?std::string():out.substr(0,outSlash);
        const std::string objects=join_path(outParent,"objects");ensure_dir(objects);
        std::string json="{\"schema\":2,\"platform\":\"windows\",\"channel\":\"stable\",\"metadata_version\":1,\"version\":\""+
            json_escape(version)+"\",\"published_at\":\""+utc_time(std::time(nullptr))+"\",\"expires_at\":\""+
            utc_time(std::time(nullptr)+7*86400)+"\",\"install_mode\":\"incremental\",\"reason\":\"offline drill\",\"files\":[";
        std::set<std::string> current;for(size_t i=0;i<newFiles.size();++i){const auto& item=newFiles[i];current.insert(norm_rel2(item.first));
            std::string rel=item.first;for(char& c:rel)if(c=='\\')c='/';const std::string object=item.second+".zip",objectPath=join_path(objects,object);
            const std::string command="\""+tar_exe()+"\" -a -cf \""+objectPath+"\" -C \""+newDir+"\" \""+rel+"\"";
            auto packed=rta::platform::run_hidden(command,true);if(!packed.started||packed.exit_code!=0){std::printf("object pack failed: %s\n",rel.c_str());return 5;}
            if(i)json+=",";json+="{\"path\":\""+json_escape(rel)+"\",\"sha256\":\""+item.second+"\",\"size\":"+
                std::to_string(file_size(join_path(newDir,item.first)))+",\"object\":\""+object+"\",\"object_sha256\":\""+
                sha_of(objectPath)+"\",\"object_size\":"+std::to_string(file_size(objectPath))+"}";}
        json+="],\"delete\":[";int deletes=0;for(const auto& item:oldFiles)if(!current.count(norm_rel2(item.first))){
            std::string rel=item.first;for(char& c:rel)if(c=='\\')c='/';if(deletes++)json+=",";json+="\""+json_escape(rel)+"\"";}
        json+="],\"manual\":{\"cn_url\":\"https://pan.baidu.com/s/offline-drill\","
            "\"global_url\":\"https://github.com/okhuoyanyan/RLPlayer2/releases\",\"extraction_code\":\"\","
            "\"sha256\":\""+manualSha+"\",\"size\":"+std::to_string(manualSize)+"},\"notes\":\"unsigned drill payload; publisher must sign\"}";
        write_text_file(out,json);std::printf("schema-2 payload=%s files=%zu objects=%s\n",out.c_str(),newFiles.size(),objects.c_str());return 0;
    }
    if(mode=="--plan"&&argc>3){
        ManifestExt manifest=parse_manifest_payload(read_all(argv[2]),current_version(),(long long)std::time(nullptr),kWindowsUpdateChannel);
        if(!manifest.ok){std::printf("invalid payload: %s\n",manifest.error.c_str());return 5;}
        Plan plan=plan_incremental(manifest.files,argv[3],manifest.deletes);
        std::printf("version=%s manual=%d download=%d bytes=%lld unchanged=%d deletes=%zu\n",manifest.version.c_str(),
            (int)manifest.manual,plan.toDownload,plan.downloadBytes,plan.unchanged,plan.deletes.size());return 0;
    }
    if(mode=="--apply"){
        restore_staging_state();auto& shared=state();std::string version,staging;bool staged=false;
        {std::lock_guard<std::mutex> lock(shared.mtx);staged=shared.staged;version=shared.dlVersion;staging=shared.stagingDir;}
        if(!staged){std::printf("no staged update\n");return 6;}std::string error;
        if(read_pending().phase!="ready"&&!write_ready_plan(version,current_version(),staging,error)){std::printf("ledger failed: %s\n",error.c_str());return 6;}
        std::printf("%s\n",apply_pending_on_startup().c_str());return read_pending().phase=="applied"?0:7;
    }
    if(mode=="--rollback"){
        std::printf("%s\n",rollback_pending(argc>2?argv[2]:"offline drill rollback").c_str());return read_pending().phase=="rolled_back"?0:9;
    }
    if(mode=="--confirm"){std::printf("%s\n",confirm_on_startup().c_str());return 0;}
    if(mode=="--hashdir"&&argc>2){std::vector<std::pair<std::string,std::string>> files;hash_dir(argv[2],"",files);
        std::sort(files.begin(),files.end());for(const auto& item:files)std::printf("%s  %s\n",item.second.c_str(),item.first.c_str());return 0;}
    std::printf("unknown argument: %s\n",mode.c_str());return 1;
}
