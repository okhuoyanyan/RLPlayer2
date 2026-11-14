"""Actual manual-IR application boundary using generated WAVs; no audio devices."""
import pathlib,subprocess,sys,tempfile,wave,struct
root=pathlib.Path(__file__).resolve().parents[1];s=(root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
def fn(signature):
 a=s.index(signature);return s[a:s.index('\n}',a)+2]
prefix=r'''
#include "audio/wav.h"
#include "dsp/ir_resample.h"
#include <cstdio>
using namespace rta::audio;
'''
if 'static std::vector<std::vector<double>> roomcorr_manual_prepare(' in s:
 body=fn('static bool roomcorr_ir_valid(')+'\n'+fn('static std::vector<std::vector<double>> roomcorr_manual_prepare(')
else:
 a=s.index('        bool convLoaded = false;',s.index('// ---- 卷积 / 房间校正：IR 是 WAV'))
 b=s.index('\n        // ---------------- 流式渲染',a)
 old=s[a:b];old=old[:old.rfind('\n\n')]
 body=r'''
static std::vector<std::vector<double>> roomcorr_manual_prepare(const std::string&file,int channels,uint32_t rate){
struct {std::string convIr;bool convGpu=false;}p;p.convIr=file;
struct {int channels;uint32_t rate;}in{channels,rate};
struct Channel{std::vector<double>ir;bool useGpu=false;};struct{std::vector<Channel>channel;}cfg;
'''+old+r'''
std::vector<std::vector<double>>out;for(auto&c:cfg.channel)out.push_back(c.ir);return out;}
'''
code=prefix+body+r'''
int main(int argc,char**argv){int n=0,f=0;auto ck=[&](bool v,const char*s){++n;f+=!v;printf("%s %s\n",v?"PASS":"FAIL",s);};std::string home=argv[1];
for(const auto&name:{"mono.wav","stereo.wav"}){auto ir=roomcorr_manual_prepare(home+"/"+name,2,48000);ck(ir.size()==2&&ir[0].size()==4,"valid mono/stereo manual IR prepares full output");ck(ir[0][0]==.5&&ir[1][0]==(std::string(name)=="mono.wav"?.5:.25),"mono broadcast/stereo channel identity preserved");}
for(const auto&name:{"nan.wav","inf.wav","three.wav","empty.wav","missing.wav"}){bool rejected=false;try{auto ir=roomcorr_manual_prepare(home+"/"+name,2,48000);(void)ir;}catch(const std::exception&){rejected=true;}ck(rejected,"invalid manual IR is rejected explicitly before convolution");}
printf("checks=%d failures=%d\n",n,f);return f?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='rlplayer-manual-ir-') as d:
 p=pathlib.Path(d)
 for name,ch,values in [('mono.wav',1,[.5,0,0,0]),('stereo.wav',2,[.5,.25,0,0,0,0,0,0]),('nan.wav',1,[float('nan'),0,0,0]),('inf.wav',1,[float('inf'),0,0,0]),('three.wav',3,[.5,.5,.5]*4),('empty.wav',1,[])]:
  data=struct.pack('<'+'f'*len(values),*values);fmt=struct.pack('<HHIIHH',3,ch,48000,48000*ch*4,ch*4,32)
  (p/name).write_bytes(b'RIFF'+struct.pack('<I',36+len(data))+b'WAVEfmt '+struct.pack('<I',16)+fmt+b'data'+struct.pack('<I',len(data))+data)
 (p/'test.cpp').write_text(code,encoding='utf-8')
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe','-std=c++20','-O1','-I'+str(root/'src'),str(p/'test.cpp'),'-o',str(p/'test.exe'),'-lwinhttp','-lws2_32'],check=True)
 sys.exit(subprocess.run([str(p/'test.exe'),d],cwd=d).returncode)
