"""Actual FLAC/MP3 cover writeback and Win32 rename denial, generated private fixtures only.

argv: compiler fixture_ffmpeg product_ffmpeg product_ffprobe.
The full encoder creates fixtures; writeback resolves the staged product tools.
"""
import pathlib,subprocess,sys,tempfile,os,shutil,wave,zlib,struct,hashlib
root=pathlib.Path(__file__).resolve().parents[1]
code=r'''
#include "meta/writeback.h"
#include <windows.h>
#include <cstdio>
using namespace rta::meta;namespace fs=std::filesystem;
static std::string read(const fs::path&p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static bool noMediaTemp(const fs::path&dir){for(auto&e:fs::directory_iterator(dir))if(e.is_regular_file()&&e.path().extension()==".rtatmp")return false;return true;}
int main(int argc,char**argv){int n=0,f=0;auto ck=[&](bool v,const char*s){++n;f+=!v;printf("%s %s\n",v?"PASS":"FAIL",s);};const fs::path home=argv[1];MetaStore::instance().setBaseDir(meta_string_of(home/"private-meta"));
for(const char*ext:{"flac","mp3"}){std::string file=meta_string_of(home/(std::string("fixture.")+ext));auto original=read(meta_path_of(file));TrackMeta tm;tm.path=file;tm.fields["title"]={"Private cover title","user",123,1};WritebackOpts o;o.dryRun=false;o.cover=true;o.coverPath=meta_string_of(home/"cover.png");
HANDLE lock=CreateFileW(meta_path_of(file).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);ck(lock!=INVALID_HANDLE_VALUE,"private source lock permits reads but denies rename/delete");
auto denied=writeback_file(file,tm,o);printf("denied %s err=%s note=%s\n",ext,denied.err.c_str(),denied.note.c_str());ck(!denied.ok&&denied.err.find("替换失败")!=std::string::npos,"real Win32 rename failure is explicit");ck(!denied.coverWritten,"failed source commit does not report coverWritten");ck(read(meta_path_of(file))==original,"rename denial preserves exact original source bytes");ck(noMediaTemp(home),"rename denial removes every actual unique temporary media name");if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
auto good=writeback_file(file,tm,o);printf("apply %s err=%s note=%s\n",ext,good.err.c_str(),good.note.c_str());ck(good.ok&&good.coverWritten,"same source recovers and commits cover after lock closes");auto pic=writeback_cover_info(file);ck(pic.parsed&&pic.frontCount==1&&pic.bytes==read(home/"cover.png"),"actual front cover bytes equal generated PNG");
tm.fields["title"].value="Second private title";o.coverPath=meta_string_of(home/"other.png");auto repeat=writeback_file(file,tm,o);printf("repeat %s err=%s note=%s\n",ext,repeat.err.c_str(),repeat.note.c_str());auto second=writeback_cover_info(file);ck(repeat.ok&&!repeat.coverWritten&&second.frontCount==1&&second.bytes==read(home/"cover.png"),"existing front cover survives later tag write without replacement");
}
printf("checks=%d failures=%d\n",n,f);return f?1:0;}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
ffmpeg=pathlib.Path(sys.argv[2]);product_ffmpeg=pathlib.Path(sys.argv[3] if len(sys.argv)>4 else sys.argv[2]);ffprobe=pathlib.Path(sys.argv[4] if len(sys.argv)>4 else sys.argv[3])
print('fixture_ffmpeg='+str(ffmpeg)+' product_ffmpeg='+str(product_ffmpeg)+' product_sha256='+hashlib.sha256(product_ffmpeg.read_bytes()).hexdigest(),flush=True)
with tempfile.TemporaryDirectory(prefix='rlplayer-writeback-real-') as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(code,encoding='utf-8')
 def png(name,seed):
  def chunk(t,b):return struct.pack('!I',len(b))+t+b+struct.pack('!I',zlib.crc32(t+b)&0xffffffff)
  raw=b''.join(b'\0'+bytes((x*7+y*3+seed)%256 for x in range(256*3)) for y in range(256))
  (p/name).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',256,256,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
 png('cover.png',1);png('other.png',17)
 with wave.open(str(p/'fixture.wav'),'wb') as w:w.setnchannels(2);w.setsampwidth(2);w.setframerate(44100);w.writeframes(bytes(44100*4))
 for ext,codec in [('flac','flac'),('mp3','libmp3lame')]:subprocess.run([str(ffmpeg),'-v','error','-y','-i',str(p/'fixture.wav'),'-c:a',codec,str(p/('fixture.'+ext))],check=True)
 def pcm(file):return subprocess.run([str(ffmpeg),'-v','error','-i',str(file),'-map','0:a:0','-vn','-f','s32le','-'],check=True,capture_output=True).stdout
 before={ext:pcm(p/('fixture.'+ext)) for ext in ('flac','mp3')}
 shutil.copy2(product_ffmpeg,p/'ffmpeg.exe');shutil.copy2(ffprobe,p/'ffprobe.exe')
 subprocess.run([compiler,'-std=c++20','-O1','-I'+str(root/'src'),str(p/'test.cpp'),'-o',str(p/'test.exe'),'-lwinhttp','-lws2_32'],check=True)
 env=os.environ|{'TEMP':d,'TMP':d,'PATH':str(pathlib.Path(os.environ.get('SystemRoot','C:/Windows'))/'System32')}
 result=subprocess.run([str(p/'test.exe'),d],cwd=d,env=env)
 for ext in ('flac','mp3'):
  after=pcm(p/('fixture.'+ext));same=after==before[ext] and len(after)>0
  print(('PASS' if same else 'FAIL')+' independent decoded PCM byte equality '+ext+' '+str(len(after))+' bytes',flush=True)
  if not same:raise SystemExit(1)
 sys.exit(result.returncode)
