"""Run the actual production position-method bodies with a fake WASAPI clock.
The oracle uses documented p/f seconds; driver units are deliberately varied.
No endpoint is enumerated or opened.
"""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
def method(file):
 s=file.read_text(encoding='utf-8');a=s.index('    bool device_position(unsigned long long& pos) const {');b=s.index('\n    }',a)+6
 return s[a:b]
code=r'''
#include <windows.h>
#include <audioclient.h>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <limits>
#if __has_include("audio/audio_clock.h")
#include "audio/audio_clock.h"
#endif
struct Clock final:IAudioClock {
 UINT64 frequency=384000,position=192000;HRESULT frequencyResult=S_OK,positionResult=S_OK;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**)override{return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return 1;}ULONG STDMETHODCALLTYPE Release()override{return 1;}
 HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* v)override{*v=frequency;return frequencyResult;}
 HRESULT STDMETHODCALLTYPE GetPosition(UINT64* v,UINT64* q)override{*v=position;if(q)*q=0;return positionResult;}
 HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD*)override{return E_NOTIMPL;}
};
namespace rta::audio {
class SharedProbe { public: Clock* clock_=nullptr;uint32_t mixRate_=48000;
SHARED_METHOD
};
class ExclusiveProbe { public: Clock* clock_=nullptr;uint32_t rate_=48000;
EXCLUSIVE_METHOD
};
}
int main(){int count=0,bad=0;Clock clock;
auto check=[&](bool ok,const char* name){++count;bad+=!ok;std::printf("%s %s\n",ok?"PASS":"FAIL",name);};
auto exercise=[&](auto& p){p.clock_=&clock;
 for(UINT64 scale:{1ull,4ull,8ull,100ull}){clock.frequency=48000*scale;clock.position=24000*scale;unsigned long long out=777;check(p.device_position(out)&&out==24000,"driver unit scale is converted to sample frames");}
 clock.frequency=10000000;clock.position=5000123;unsigned long long out=777;check(p.device_position(out)&&out==24000,"nonintegral frame ratio rounds down");
 clock.frequency=0;out=777;check(!p.device_position(out)&&out==777,"zero frequency rejected without false zero position");
 clock.frequency=48000;clock.frequencyResult=E_FAIL;check(!p.device_position(out),"frequency query failure preserved");clock.frequencyResult=S_OK;
 clock.positionResult=E_FAIL;check(!p.device_position(out),"position query failure preserved");clock.positionResult=S_OK;
 clock.frequency=1;clock.position=~0ull;check(!p.device_position(out),"unrepresentable frame position rejected");
 clock.frequency=~0ull;clock.position=~0ull-1;check(p.device_position(out)&&out==47999,"multiply overflow avoided at large clock units");
 clock.frequency=48000;clock.position=24000;
};rta::audio::SharedProbe s;rta::audio::ExclusiveProbe e;exercise(s);exercise(e);
std::printf("checks=%d failures=%d\n",count,bad);return bad?1:0;}
'''.replace('SHARED_METHOD',method(root/'src/audio/wasapi_shared.h')).replace('EXCLUSIVE_METHOD',method(root/'src/audio/wasapi_out.h'))
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-clock-') as name:
 folder=Path(name);cpp=folder/'test.cpp';exe=folder/'test.exe';cpp.write_text(code,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O2','-I'+str(root/'src'),str(cpp),'-o',str(exe)],check=True)
 sys.exit(subprocess.run([str(exe)],cwd=folder).returncode)
