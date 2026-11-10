"""1000-image local cache baseline and actual rtaweb cache read boundaries.

Measures disk/read/worker memory only, not Chromium decoded textures or network.
"""
import pathlib,subprocess,sys,tempfile,struct,zlib,random,re
root=pathlib.Path(__file__).resolve().parents[1]
source=(root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
def extract(name):
 a=source.index(name+'(')
 while source.find(';',a)<source.find('{',a):a=source.index(name+'(',a+1)
 a=source.rfind('\n',0,a)+1
 return source[a:source.index('\n}',a)+2]
decl=re.search(r'^static constexpr size_t kArtistImgMaxBytes[^\n]+',source,re.M)
a=source.index('struct ArtistImgPaths {');b=source.index('\n}',source.index('static ArtistImgPaths artistimg_paths(',a))+2
code=r'''
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include "meta/artwork.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
namespace fs=std::filesystem;
static std::string runtime;
static std::string path_in_exe_dir(const std::string& part){return runtime+"/"+part;}
''' +(decl.group(0)+'\n' if decl else '')+extract('static uint64_t artistimg_fnv1a')+'\n'+source[a:b]+'\n'+extract('static bool artistimg_read_file')+'\n'+extract('static std::string artist_avatar_cached')+r'''
static PROCESS_MEMORY_COUNTERS_EX memory(){PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof(m));return m;}
int main(int argc,char**argv){runtime=argv[1];fs::create_directory(fs::path(runtime)/"artistimg");
 int checks=0,failures=0;auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);};
 const auto fixture=fs::path(runtime)/"fixture.png";uint64_t disk=0;
 for(int i=0;i<1000;++i){const auto target=artistimg_paths("Offline Artist "+std::to_string(i)).jpg;fs::copy_file(fixture,target);disk+=fs::file_size(target);}
 std::string expected;artistimg_read_file(fixture.string(),expected,1024*1024);
 DWORD handlesBefore=0;GetProcessHandleCount(GetCurrentProcess(),&handlesBefore);auto before=memory();
 auto t0=std::chrono::steady_clock::now();bool all=true;for(int i=0;i<1000;++i){bool cached=false;auto bytes=artist_avatar_cached("Offline Artist "+std::to_string(i),&cached);all=all&&cached&&bytes==expected;}
 auto seqMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
 check(all,"1000 distinct generated images remain readable through actual cache entry");
 std::atomic<int> wrong{0};std::vector<std::thread> threads;t0=std::chrono::steady_clock::now();
 for(int t=0;t<6;++t)threads.emplace_back([&]{for(int i=0;i<1000;++i){bool cached=false;auto bytes=artist_avatar_cached("Offline Artist "+std::to_string(i),&cached);if(!cached||bytes!=expected)++wrong;}});
 for(auto& t:threads)t.join();auto concurrentMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
 check(wrong==0,"six read workers complete 6000 cache hits without corrupting content");
 auto after=memory();DWORD handlesAfter=0;GetProcessHandleCount(GetCurrentProcess(),&handlesAfter);
 const auto growth=after.PrivateUsage>before.PrivateUsage?after.PrivateUsage-before.PrivateUsage:0;
 check(growth<64ull*1024*1024,"1000-image set is not retained as an unbounded in-memory byte cache");
 check(handlesAfter<=handlesBefore+4,"cache reads do not leak one handle per image");
 printf("RESOURCE images=1000 image_bytes=%llu disk_bytes=%llu sequential_ms=%lld workers=6 reads=6000 concurrent_ms=%lld private_before=%llu private_after=%llu peak_working_set=%llu handles_before=%lu handles_after=%lu\n",(unsigned long long)fs::file_size(fixture),(unsigned long long)disk,(long long)seqMs,(long long)concurrentMs,(unsigned long long)before.PrivateUsage,(unsigned long long)after.PrivateUsage,(unsigned long long)after.PeakWorkingSetSize,handlesBefore,handlesAfter);
 bool cached=true;check(artist_avatar_cached("missing",&cached).empty()&&!cached,"missing cache is a recoverable miss");
 const auto damaged=artistimg_paths("damaged").jpg;std::ofstream(damaged,std::ios::binary)<<"not an image";cached=true;
 check(artist_avatar_cached("damaged",&cached).empty()&&!cached,"corrupt cached bytes are not served as a successful avatar");
 fs::copy_file(fixture,damaged,fs::copy_options::overwrite_existing);check(!artist_avatar_cached("damaged",&cached).empty()&&cached,"replaced corrupt cache recovers immediately");
 const auto large=artistimg_paths("large").jpg;fs::copy_file(fixture,large);fs::resize_file(large,10ull*1024*1024);cached=false;
 check(artist_avatar_cached("large",&cached).size()==10ull*1024*1024&&cached,"10 MiB cache accepted by ArtworkOpts is also servable");
 fs::resize_file(large,13ull*1024*1024);cached=true;
 check(artist_avatar_cached("large",&cached).empty()&&!cached,"oversized cache is rejected before allocation");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-artist-cache-') as d:
 p=pathlib.Path(d);cpp=p/'test.cpp';exe=p/'test.exe';cpp.write_text(code,encoding='utf-8')
 # Standards-compliant 256x256 PNG, deterministic noise avoids tiny compressed files.
 rng=random.Random(731);raw=b''.join(b'\0'+rng.randbytes(256*3) for _ in range(256))
 def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
 png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',256,256,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')
 (p/'fixture.png').write_bytes(png)
 subprocess.run([compiler,'-std=c++20','-O1','-I'+str(root/'src'),str(cpp),'-o',str(exe),'-lwinhttp','-lws2_32','-lpsapi'],check=True)
 sys.exit(subprocess.run([str(exe),str(p)],cwd=d).returncode)
