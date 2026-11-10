"""Actual writeback transaction with explicit transport/process seams and real private file IO.

No audio/profile/network. Remux/decode are controlled because these tests target
commit interruption, not codec correctness (the existing independent PCM gate does that).
"""
import pathlib,subprocess,sys,tempfile,os
root=pathlib.Path(__file__).resolve().parents[1]
s=pathlib.Path(os.environ.get('RLPLAYER_TEST_WRITEBACK_HEADER',str(root/'src/meta/writeback.h'))).read_text(encoding='utf-8')
a=s.index('inline WritebackResult writeback_file(');f=s[a:s.index('\n}',a)+2]
for old,new in [('writeback_file(','tested_writeback_file('),('rta::platform::move_file(','fault_move('),('rta::platform::copy_file(','fault_copy('),('rta::platform::find_executable(','fake_find('),('rta::platform::run_hidden(','fake_run('),('writeback_remux(','fake_remux('),('writeback_decode_pcm(','fake_decode('),('writeback_cover_info(','fake_cover(')]:f=f.replace(old,new)
code=r'''
#include "meta/writeback.h"
#include <cstdio>
#include <stdexcept>
#include <mutex>
#include <condition_variable>
#include <thread>
using namespace rta::meta;namespace fs=std::filesystem;
static std::string mode,source,original="private original container bytes",image;
static bool faultReached=false;
static std::mutex syncMx;static std::condition_variable syncCv;static bool bReady=false,aDone=false;
static std::string read(const fs::path&p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const fs::path&p,const std::string&s){std::ofstream f(p,std::ios::binary);f<<s;}
static bool noMediaTemp(const fs::path&dir){for(auto&e:fs::directory_iterator(dir))if(e.is_regular_file()&&e.path().extension()==".rtatmp")return false;return true;}
static bool backupTarget(const fs::path&p){return p.parent_path().filename()=="backups";}
static bool fault_move(const fs::path&a,const fs::path&b,bool replace){
 if(backupTarget(b)&&mode=="backup-fail"){faultReached=true;return false;}
 if(a.extension()==".rtatmp"&&(mode=="commit-fail"||mode=="restore-fail")){faultReached=true;return false;}
 if(a.parent_path().filename()=="backups"&&mode=="restore-fail")return false;
 bool ok=rta::platform::move_file(a,b,replace);
 if(ok&&backupTarget(b)&&mode=="interrupt"){faultReached=true;throw std::runtime_error("injected interruption after backup");}return ok;
}
static bool fault_copy(const fs::path&a,const fs::path&b,bool overwrite){
 if(backupTarget(b)&&mode=="backup-fail"){faultReached=true;return false;}
 bool ok=rta::platform::copy_file(a,b,overwrite);
 if(ok&&backupTarget(b)&&mode=="interrupt"){faultReached=true;throw std::runtime_error("injected interruption after backup");}return ok;
}
static std::string fake_find(const std::string&){return "private-fake-tool";}
static rta::platform::ProcessResult fake_run(const std::string&,bool){return {};}
static bool fake_remux(const std::string&,const std::string&,const std::string&dst,const std::string&,bool,const std::vector<WbField>&fields,std::string&err,const std::string& ={},int=0){
 if(mode=="concurrent"){
  const auto title=fields[0].value;write(meta_path_of(dst),"prepared "+title);
  std::unique_lock<std::mutex>lk(syncMx);
  if(title=="A")syncCv.wait(lk,[]{return bReady;});else{bReady=true;syncCv.notify_all();syncCv.wait(lk,[]{return aDone;});}
  return true;
 }
 write(meta_path_of(dst),"new private container bytes");if(mode=="remux-fail"){err="injected remux interruption";faultReached=true;return false;}return true;}
static bool fake_decode(const std::string&,const std::string&in,const std::string&dst,std::string&err){if(mode=="decode-fail"||(mode=="concurrent"&&!fs::exists(meta_path_of(in)))){faultReached=true;err="injected decode failure or absent input";return false;}write(meta_path_of(dst),std::string(48,'P'));return true;}
static WbCoverInfo fake_cover(const std::string&file,const std::string& ={}){WbCoverInfo r;r.parsed=true;if(fs::path(file).extension()==".rtatmp"){r.count=1;r.frontCount=1;r.firstType=3;r.bytes=mode=="cover-fail"?"wrong cover":image;r.mime="image/png";if(mode=="cover-fail")faultReached=true;}return r;}
'''+f+r'''
int main(int argc,char**argv){int checks=0,failures=0;auto ck=[&](bool v,const char*s){++checks;failures+=!v;printf("%s %s\n",v?"PASS":"FAIL",s);};const fs::path home=argv[1];image=std::string(300,'I');image.replace(0,8,"\x89PNG\r\n\x1a\n",8);write(home/"cover.png",image);
for(const auto& scenario:std::vector<std::string>{"remux-fail","decode-fail","cover-fail","backup-fail","commit-fail","restore-fail","interrupt","success"}){
 mode=scenario;faultReached=false;const fs::path dir=home/scenario;fs::create_directories(dir);source=meta_string_of(dir/"fixture.flac");write(meta_path_of(source),original);MetaStore::instance().setBaseDir(meta_string_of(dir/"meta"));
 TrackMeta tm;tm.path=source;tm.fields["title"]={"new title","user",123,1};WritebackOpts opts;opts.dryRun=false;opts.cover=true;opts.coverPath=meta_string_of(home/"cover.png");WritebackResult r;bool interrupted=false;
 try{r=tested_writeback_file(source,tm,opts);}catch(const std::runtime_error&){interrupted=true;}
 printf("scenario=%s ok=%d interrupted=%d faultReached=%d\n",mode.c_str(),r.ok,interrupted,faultReached);
 if(mode=="success"){ck(r.ok&&r.coverWritten,"valid transaction succeeds with requested cover");ck(read(meta_path_of(source))=="new private container bytes","successful commit publishes prepared file");}
 else {ck(faultReached,"requested fault branch was reached");ck(!r.ok,"failed/interrupted transaction never reports success");ck(!r.coverWritten,"failed transaction never claims committed cover");ck(read(meta_path_of(source))==original,"failure/interruption preserves original at original path");}
 const fs::path backups=dir/"meta/backups";bool hasOriginal=false;if(fs::exists(backups))for(auto&e:fs::directory_iterator(backups))if(read(e.path())==original)hasOriginal=true;
 if(mode=="interrupt"||mode=="commit-fail"||mode=="restore-fail")ck(hasOriginal||read(meta_path_of(source))==original,"at least one intact original remains after commit failure");
 if(mode!="interrupt")ck(noMediaTemp(dir),"handled completion/failure leaves no actual transaction media temp names");
}
mode="concurrent";const fs::path concurrent=home/"concurrent";fs::create_directories(concurrent);source=meta_string_of(concurrent/"shared.flac");write(meta_path_of(source),original);MetaStore::instance().setBaseDir(meta_string_of(concurrent/"meta"));
WritebackOpts concurrentOpts;concurrentOpts.dryRun=false;concurrentOpts.cover=false;TrackMeta ta;ta.path=source;ta.fields["title"]={"A","user",123,1};TrackMeta tb=ta;tb.fields["title"].value="B";WritebackResult ra,rb;std::string committedA;
std::thread first([&]{ra=tested_writeback_file(source,ta,concurrentOpts);committedA=read(meta_path_of(source));{std::lock_guard<std::mutex>lk(syncMx);aDone=true;}syncCv.notify_all();});
// Wait until A has created its remux product before B overwrites the old fixed name.
while(!fs::exists(meta_path_of(source+".rtatmp"))){bool found=false;for(auto&e:fs::directory_iterator(concurrent))if(e.path().extension()==".rtatmp")found=true;if(found)break;std::this_thread::yield();}
std::thread second([&]{rb=tested_writeback_file(source,tb,concurrentOpts);});first.join();second.join();
printf("concurrent A_ok=%d A_observed_bytes=%s B_ok=%d final_bytes=%s\n",ra.ok,committedA.c_str(),rb.ok,read(meta_path_of(source)).c_str());
ck(ra.ok&&committedA=="prepared A","concurrent request A commits its own validated metadata, never B temporary bytes");
ck(rb.ok&&read(meta_path_of(source))=="prepared B","concurrent request B can commit its own isolated temporary artifact");
printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='rlplayer-writeback-transaction-') as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(code,encoding='utf-8')
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe','-std=c++20','-O1','-I'+str(root/'src'),str(p/'test.cpp'),'-o',str(p/'test.exe'),'-lwinhttp','-lws2_32'],check=True)
 sys.exit(subprocess.run([str(p/'test.exe'),d],cwd=d,env=__import__('os').environ|{'TEMP':d,'TMP':d}).returncode)
