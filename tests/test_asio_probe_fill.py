"""Extract the actual ASIO probe callback; test byte buffers only, never ASIO/COM."""
import argparse,os,pathlib,subprocess,sys,tempfile
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
p=argparse.ArgumentParser();p.add_argument("--source-root",type=pathlib.Path,default=pathlib.Path(os.environ.get("RLPLAYER_TEST_SOURCE_ROOT",pathlib.Path(__file__).resolve().parents[1])));p.add_argument("--compiler",default=r"C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe");a=p.parse_args()
s=(a.source_root/"tools/asio_probe.cpp").read_text(encoding="utf-8-sig")
start=s.index("static std::atomic<long> g_cbCount")
block=s[start:s.index("static void probe_switch(",start)]
prefix=r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
using ASIOSampleType=long;
constexpr long ASIOSTDSDInt8LSB1=32,ASIOSTDSDInt8MSB1=33;
struct ASIOBufferInfo{bool isInput;long channelNum;void* buffers[2];};
unsigned long long GetTickCount64(){return 1;}
'''
if "g_probeTypes" not in block:prefix+="static ASIOSampleType g_probeTypes[32]{};\n"
main=r'''
static int checks=0,failures=0;
void ck(bool value,const char* label){++checks;if(!value){++failures;printf("FAIL %s\n",label);}}
int main(){
 const size_t guard=16;
 for(long type:{18L,32L,33L})for(size_t size:{1u,3u,7u,8u,33u,257u,4096u}){
  std::vector<unsigned char> memory[2][2];ASIOBufferInfo buffers[2]{};
  for(int c=0;c<2;++c){g_probeTypes[c]=type;for(int b=0;b<2;++b){memory[c][b].assign(size+2*guard+8,0xa5);buffers[c].buffers[b]=memory[c][b].data()+guard;}}
  g_bufs=buffers;g_ch=2;g_bufUnits=long(size);g_unitBytes=1;g_fillBytes=0;g_cbLog=99;g_cbCount=0;
  const unsigned char expected=type==32?0x69:type==33?0x96:0;
  for(long index:{0L,1L,0L}){
   probe_fill(index);
   for(int c=0;c<2;++c){auto& bytes=memory[c][index];ck(std::all_of(bytes.begin()+guard,bytes.begin()+guard+size,[&](auto x){return x==expected;}),"actual callback writes exact PCM/DSD silence byte");ck(std::all_of(bytes.begin(),bytes.begin()+guard,[](auto x){return x==0xa5;})&&std::all_of(bytes.begin()+guard+size,bytes.end(),[](auto x){return x==0xa5;}),"prefix/suffix sentinels survive");}
   if(index==0 && g_cbCount.load()%3==1)for(int c=0;c<2;++c)ck(std::all_of(memory[c][1].begin(),memory[c][1].end(),[](auto x){return x==0xa5;}),"callback cannot modify the other double buffer");
   if(type!=18){for(int bit=0;bit<8;++bit){int actual=type==32?(memory[0][index][guard]>>bit)&1:(memory[0][index][guard]>>(7-bit))&1;ck(actual==((0x96>>(7-bit))&1),"LSB/MSB have identical chronological balanced bit pattern");}}
  }
  // A diagnostic override cannot write beyond the actual buffer capacity.
  for(int c=0;c<2;++c)std::fill(memory[c][0].begin(),memory[c][0].end(),0xa5);
  g_fillBytes=long(size+4);probe_fill(0);
  for(int c=0;c<2;++c)ck(std::all_of(memory[c][0].begin()+guard+size,memory[c][0].end(),[](auto x){return x==0xa5;}),"oversized fill override is bounded by capacity");
 }
 g_bufs=nullptr;probe_fill(0);
 printf("checks=%d failures=%d actual_audio_device_calls=0\n",checks,failures);return failures?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix="rlplayer-asio-silence-") as tmp:
 d=pathlib.Path(tmp);cpp=d/"test.cpp";exe=d/"test.exe";cpp.write_text(prefix+block+main,encoding="utf-8")
 r=subprocess.run([a.compiler,"-std=c++17","-O0",str(cpp),"-o",str(exe)],capture_output=True,text=True,encoding="utf-8",errors="replace")
 if r.returncode:print(r.stdout,r.stderr);raise SystemExit(r.returncode)
 r=subprocess.run([str(exe)],capture_output=True,text=True,encoding="utf-8",errors="replace");print(r.stdout,r.stderr);raise SystemExit(r.returncode)
