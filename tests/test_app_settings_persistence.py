"""Fault-inject the production /api/settings handler without starting a server."""
import pathlib
import os
import subprocess
import sys
import tempfile

root = pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT', pathlib.Path(__file__).resolve().parents[1]))
source = (root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
start = source.index('    if (rq.path.rfind("/api/settings", 0) == 0) {')
end = source.index('    // ASIO 驱动列表', start)
handler = source[start:end]
store = (root/'src/meta/store.h').read_text(encoding='utf-8')
atomic = store[store.index('inline std::recursive_mutex& meta_file_mutex('):store.index('\nclass MetaStore')]
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
namespace fs=std::filesystem;
static std::string base;
static void filter_prewarm_cancel(){}
static std::string path_in_exe_dir(const std::string& s){return (fs::path(base)/s).string();}
static std::string urlDecode(const std::string& s){return s;}
static std::string jesc(const std::string& s){return s;}
struct Library{static Library& instance(){static Library x;return x;}void setScanThreads(size_t){}};
namespace rta{namespace audio{inline bool valid_rate_family_policy(const std::string&){return true;}inline void set_stream_decode_enabled(bool){}}
namespace net{inline void webui_set_status(int){}inline std::string i18n_resolve(const std::string& s){return s;}}
namespace analysis{inline void analysis_set_lang(const std::string&) {}}
namespace meta{namespace fsx=std::filesystem;struct ProviderDesc{std::string id;};inline std::vector<ProviderDesc> provider_registry(){return {};}
inline void pp_set_output_lang(const std::string&){}
'''
wrapper = r'''
}}
struct Request{std::string path="/api/settings",method="POST",body;};
static std::string settings(const Request& rq){std::string ct;
'''
main = r'''
return "unhandled";}
int main(int argc,char**argv){base=argv[1];int failures=0,checks=0;
 auto check=[&](bool b,const char* what){++checks;if(!b)++failures;printf("%s %s\n",b?"PASS":"FAIL",what);};
 auto read=[](const std::string& p){std::ifstream f(p);return std::string(std::istreambuf_iterator<char>(f),{});};
 const auto file=path_in_exe_dir("rlplayer-settings.conf");
 check(settings({"/api/settings","POST","backend=null&volume=-60"}).find("\"ok\":true")!=std::string::npos,"seed isolated safe settings");
 const auto old=read(file);HANDLE held=CreateFileW(fs::u8path(file).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
 check(held!=INVALID_HANDLE_VALUE,"hold settings target against atomic replacement");
 const auto response=settings({"/api/settings","POST","theme=light"});
 check(response.find("\"ok\":false")!=std::string::npos,"replacement denial reports save failure");check(read(file)==old,"denied replacement preserves old settings bytes");
 if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
 check(settings({"/api/settings","POST","theme=dark"}).find("\"ok\":true")!=std::string::npos,"retry after lock release succeeds");
 check(read(file).find("backend=null")!=std::string::npos&&read(file).find("volume=-60")!=std::string::npos,"partial POST retains existing safe backend and volume");
 int lost=0;for(int round=0;round<30;++round){std::ofstream(file)<<"backend=null\nvolume=-60\n";std::atomic<int> ready=0;std::atomic<bool> go=false;
 std::thread a([&]{++ready;while(!go)std::this_thread::yield();settings({"/api/settings","POST","theme=dark"});});
 std::thread b([&]{++ready;while(!go)std::this_thread::yield();settings({"/api/settings","POST","uilang=es"});});
 while(ready<2)std::this_thread::yield();go=true;a.join();b.join();const auto content=read(file);
 if(content.find("theme=dark")==std::string::npos||content.find("uilang=es")==std::string::npos||content.find("backend=null")==std::string::npos)++lost;}
 printf("concurrent_partial_POST_lost_rounds=%d/30\n",lost);check(lost==0,"concurrent partial POST retains both updates and original fields");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
compiler = sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-settings-test-') as folder:
    folder=pathlib.Path(folder);cpp=folder/'test.cpp';exe=folder/'test.exe'
    cpp.write_text(prefix+atomic+wrapper+handler+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++17','-O0',str(cpp),'-o',str(exe)],check=True)
    sys.exit(subprocess.run([str(exe),str(folder)]).returncode)
