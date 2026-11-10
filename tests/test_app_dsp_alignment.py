"""Production application DSP wiring; pipeline outputs, no audio endpoint."""
import pathlib,sys,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[1];s=(root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
def function(signature):
 a=s.index(signature);return s[a:s.index('\n}',a)+2]
a=s.index('struct RoomCorrAuto {');struct=s[a:s.index('\n};',a)+3]
code='''#include "engine/pipeline.h"
#include "dsp/room_correction.h"
#include <cstdio>
'''+struct+'\n'+function('static void apply_channel_trims(')+'\n'+function('static void roomcorr_apply_alignment(')+r'''
int main(){int failures=0,checks=0;auto check=[&](bool ok,const char* label){++checks;failures+=!ok;printf("%s %s\n",ok?"PASS":"FAIL",label);};
rta::engine::PipelineConfig cfg;cfg.outBits=24;cfg.channel.resize(2);cfg.channel[0].gainDb=-6;cfg.channel[1].gainDb=-6;double trims[8]={1,1};apply_channel_trims(cfg,2,trims);
rta::audio::AudioBuffer in;in.rate=48000;in.bits=24;in.channels=2;in.raw.resize(128*2);for(size_t i=0;i<128;++i)for(int c=0;c<2;++c)in.setSample(i,c,.9);auto out=rta::engine::Pipeline(cfg).process(in);printf("combined_sample=%.9f\n",out.sample(0,0));check(std::abs(out.sample(0,0)-.506107192)<2e-7,"0.9 input + EQ -6dB + trim +1dB yields 0.506107, preserving EQ preamp");
for(const auto layout:std::vector<std::pair<const char*,int>>{{"2.1",3},{"5.1",6},{"7.1",8}})for(int direction:{-1,1}){
 const int sub=layout.second==3?2:3;rta::engine::PipelineConfig c;c.outBits=24;c.channel.resize(layout.second);for(auto& ch:c.channel)ch.ir={1.0};RoomCorrAuto r;r.layout=layout.first;r.subDelaySamples=direction*7;r.subGainDb=-6.020599913279624;roomcorr_apply_alignment(c,r);
 rta::audio::AudioBuffer a;a.rate=48000;a.bits=24;a.channels=layout.second;a.raw.assign(4096*a.channels,0);for(int ch=0;ch<a.channels;++ch)a.setSample(100,ch,.5);rta::engine::Pipeline pipe(c);auto y=pipe.process(a);size_t latency=(size_t)std::llround(pipe.algorithmic_latency_out_frames());bool ok=true;
 for(int ch=0;ch<a.channels;++ch){size_t at=100+latency+(((direction>0&&ch!=sub)||(direction<0&&ch==sub))?7:0);double expected=ch==sub?.25:.5;ok=ok&&at<y.frames()&&std::abs(y.sample(at,ch)-expected)<2e-7;}check(ok,(std::string(layout.first)+(direction>0?" positive sub lag delays all mains and applies LFE-only gain":" negative sub lag delays LFE and applies LFE-only gain")).c_str());}
printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='rlplayer-upnp-dsp-') as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(code,encoding='utf-8');subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe','-std=c++23','-O1','-Wno-deprecated-declarations','-I'+str(root/'src'),str(p/'test.cpp'),'-o',str(p/'test.exe'),'-lwinhttp','-lws2_32'],check=True);sys.exit(subprocess.run([str(p/'test.exe')],cwd=d).returncode)
