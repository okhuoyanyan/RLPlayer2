"""Test actual token generation with a frozen clock and a Windows RNG boundary.

Unique random samples are a regression signal for the old clock collision,
not a proof of unpredictability. The security source is BCryptGenRandom.
"""
import os,pathlib,subprocess,sys,tempfile
root=pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT',pathlib.Path(__file__).resolve().parents[1]))
source=(root/'src/net/upnp_control.h').read_text(encoding='utf-8')
start=source.index('inline std::string generate_random_token(')
function=source[start:source.index('\n}',start)+2]
# Pin the old clock dependency without changing any token-generation steps.
function=function.replace('std::chrono::high_resolution_clock','FrozenClock')
def registration_block(file):
 text=file.read_text(encoding='utf-8');start=text.index('        std::string streamPath;') if 'cast_service' in file.name else text.index('    std::string streamPath;')
 return text[start:text.index('std::string streamUrl',start)]
prefix=r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <stdexcept>
#include <limits>
#include <cstdio>
#include <fstream>
#include <filesystem>
struct FrozenClock{static std::chrono::system_clock::time_point now(){return std::chrono::system_clock::time_point(std::chrono::system_clock::duration(123456789));} };
static std::atomic<bool> forceFailure{false};
static std::atomic<int> rngCalls{0},badFlags{0};
static NTSTATUS wrapped_rng(BCRYPT_ALG_HANDLE algorithm,PUCHAR bytes,ULONG length,ULONG flags){
 ++rngCalls;if(algorithm!=nullptr||flags!=BCRYPT_USE_SYSTEM_PREFERRED_RNG)++badFlags;
 if(forceFailure)return (NTSTATUS)0xc0000001;
 return ::BCryptGenRandom(algorithm,bytes,length,flags);
}
#define BCryptGenRandom wrapped_rng
'''
boundaries=''
if 'std::string streamPath;' in (root/'src/net/cast_service.h').read_text(encoding='utf-8'):
 boundaries=r'''
struct FakeServer{bool stopped=false;std::string register_file(const std::string&,const std::string&){return generate_random_token();}void stop(){stopped=true;}};
static void gui_boundary(FakeServer& server,std::string& error,bool& published){std::string wavToServe;auto fail=[&](const std::string& e){error=e;};
'''+registration_block(root/'src/net/cast_service.h')+r'''
published=true;}
static int cli_boundary(FakeServer& server,const std::string& tempWavPath){std::string wavToServe;bool needCleanupTemp=true;
'''+registration_block(root/'tools/rtacast.cpp')+r'''
return 0;}
'''
main=r'''
int main(int argc,char**argv){int checks=0,failures=0;auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);};
for(size_t length:{size_t(0),size_t(1),size_t(15),size_t(16),size_t(17),size_t(512)}){
 const auto token=generate_random_token(length);check(token.size()==length,"requested token length is preserved");
 check(token.find_first_not_of("0123456789abcdef")==std::string::npos,"token contains lowercase hex only");}
std::unordered_set<std::string> serial;for(int n=0;n<10000;++n)serial.insert(generate_random_token());
printf("frozen_clock_unique=%zu/10000\n",serial.size());check(serial.size()==10000,"same-clock calls do not repeat the old LCG sequence");
std::unordered_set<std::string> parallel;std::mutex mu;std::vector<std::thread> workers;
for(int n=0;n<8;++n)workers.emplace_back([&]{std::vector<std::string> tokens;for(int i=0;i<5000;++i)tokens.push_back(generate_random_token());std::lock_guard<std::mutex> lock(mu);parallel.insert(tokens.begin(),tokens.end());});
for(auto& t:workers)t.join();printf("concurrent_unique=%zu/40000 threads=8\n",parallel.size());check(parallel.size()==40000,"concurrent callers do not reseed from a shared clock tick");
check(rngCalls>0&&badFlags==0,"production generator requests the Windows system preferred RNG");
forceFailure=true;bool rejected=false;std::string error;
try{(void)generate_random_token();}catch(const std::exception& e){rejected=true;error=e.what();}
check(rejected&&!error.empty(),"system RNG failure propagates an explicit error without a weak fallback");
'''
if boundaries:
 main+=r'''
FakeServer gui;bool published=false;std::string guiError;gui_boundary(gui,guiError,published);
check(gui.stopped&&!published&&!guiError.empty(),"GUI registration failure stops server and reports failure before publishing");
FakeServer cli;const auto temp=(std::filesystem::path(argv[1])/"token-test.wav").string();std::ofstream(temp)<<"fixture";
check(cli_boundary(cli,temp)==1&&cli.stopped&&!std::filesystem::exists(temp),"CLI registration failure stops server, removes its fixture and returns error");
'''
main+='printf("checks=%d failures=%d\\n",checks,failures);return failures?1:0;}\n'
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-token-test-') as directory:
 directory=pathlib.Path(directory);cpp=directory/'test.cpp';exe=directory/'test.exe';cpp.write_text(prefix+function+boundaries+main,encoding='utf-8')
 subprocess.run([compiler,'-std=c++17','-O2',str(cpp),'-o',str(exe),'-lbcrypt'],check=True)
 sys.exit(subprocess.run([str(exe),str(directory)]).returncode)
