"""Compile actual queue persistence functions; inject Windows replacement denial.

No backend, profile, library or audio device is opened. All files live in a
temporary directory owned by this test. Optional argv[1] overrides clang++.
"""
import pathlib
import os
import subprocess
import sys
import tempfile

root = pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT', pathlib.Path(__file__).resolve().parents[1]))
source = (root / 'tools/rtaweb.cpp').read_text(encoding='utf-8')
def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    # Boundaries are standalone closing braces in these production functions.
    end = source.index('\n}', opening) + 2
    return source[start:end]

prefix = r'''
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstdlib>
namespace fs = std::filesystem;
struct QItem { std::string path; double cs=0,ce=0; QItem(std::string p):path(p){} bool isCue()const{return ce>cs;} };
static QItem item_from_line(const std::string& s){return QItem(s);}
static std::string active, sibling;
static int migrations=0;
static std::string queue_file(){return active;}
static std::string queue_find_sibling(const std::string&){++migrations;return sibling;}
static void play_log(const std::string&){}
static void queue_save(){}
static struct {std::mutex mtx;std::vector<QItem> items,base,sortBase;size_t index=0;int repeat=0;bool shuffle=false;}g_q;
static int failures=0,checks=0;
static void check(bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);}
static std::string bytes(const std::string& p){std::ifstream f(fs::u8path(p),std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
'''
main = r'''
int main(int argc,char**argv){
 active=(fs::path(argv[1])/"current.txt").string();sibling=(fs::path(argv[1])/"sibling.txt").string();
 std::vector<QItem> original={QItem("old.wav")}, replacement={QItem("new.wav")};
 check(queue_write_file(active,original,0,0,false),"seed current queue");const auto old=bytes(active);
 HANDLE held=CreateFileW(fs::u8path(active).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
 check(held!=INVALID_HANDLE_VALUE,"hold target without DELETE sharing");
 check(!queue_write_file(active,replacement,0,0,false),"replacement failure is reported");
 check(bytes(active)==old,"failed replacement preserves every byte of old queue");
 if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
 check(queue_write_file(active,replacement,0,0,false),"replacement succeeds after release");
 check(bytes(active).find("item=new.wav")!=std::string::npos,"successful replacement contains new item");
 check(queue_write_file(sibling,original,0,0,false),"seed old sibling queue");
 check(queue_write_file(active,{},0,2,false),"persist intentionally empty queue");
 migrations=0;queue_load();check(g_q.items.empty()&&g_q.repeat==2&&migrations==0,"empty saved queue stays empty without resurrecting sibling");
 fs::remove(fs::u8path(active));migrations=0;queue_load();check(g_q.items.size()==1&&g_q.items[0].path=="old.wav"&&migrations==1,"missing local queue still migrates old sibling");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
compiler = sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-queue-test-') as folder:
    folder = pathlib.Path(folder)
    cpp = folder/'test.cpp'
    cpp.write_text(prefix+'\n'+ '\n'.join(function(n) for n in [
        'static bool queue_write_file(', 'static bool queue_read_file(', 'static void queue_load() {'])+'\n'+main, encoding='utf-8')
    exe = folder/'test.exe'
    subprocess.run([compiler,'-std=c++17','-O0',str(cpp),'-o',str(exe)],check=True)
    result = subprocess.run([str(exe),str(folder)])
    sys.exit(result.returncode)
