"""Real application room correction preparation, private files; no device/process/network."""
import pathlib,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
s=(root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
def function(signature):
 a=s.index(signature);a=s.index(signature,a+1) if s.find(';',a,s.find('{',a))>=0 else a
 if '}' in s[a:s.index('\n',a)]:return s[a:s.index('\n',a)]
 return s[a:s.index('\n}',a)+2]
struct=s[s.index('struct RoomCorrAuto {'):s.index('\n};',s.index('struct RoomCorrAuto {'))+3]
a=s.index('namespace roomcorr_auto_detail {');namespace=s[a:s.index('}   // namespace roomcorr_auto_detail',a)+len('}   // namespace roomcorr_auto_detail')]
code=r'''
#include "dsp/room_measurement.h"
#include "dsp/ir_resample.h"
#include "audio/wav.h"
#include "net/json.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdio>
namespace fs=std::filesystem;
static std::string home;
static std::string path_in_exe_dir(const std::string&){return home;}
static std::string pathToUtf8(const fs::path&p){auto s=p.u8string();return std::string((const char*)s.data(),s.size());}
'''+struct+'\n'+ (function('static bool roomcorr_ir_valid(')+'\n' if 'static bool roomcorr_ir_valid(' in s else '')+ '\n'.join(function(x) for x in ['static std::string roomcorr_dir()', 'static std::string roomcorr_file(', 'static const std::vector<rta::dsp::RoomLayout>& roomcorr_all_layouts()', 'static const rta::dsp::RoomLayout* roomcorr_find_layout(', 'static bool roomcorr_read_file('])+'\n'+namespace+'\n'+function('static bool roomcorr_file_exists(')+'\n'+function('static RoomCorrAuto roomcorr_auto_prepare(')+r'''
int main(int argc,char**argv){home=argv[1];int checks=0,failures=0;auto check=[&](bool ok,const char*label){++checks;failures+=!ok;printf("%s %s\n",ok?"PASS":"FAIL",label);};
auto put=[&](const std::string&text){std::ofstream(fs::u8path(roomcorr_file("2.0")))<<text;};
const std::string good=R"({"layout":"2.0","fs":48000,"taps":3,"subDelaySamples":0,"subGainDb":0,"channels":[{"name":"L","ir":[1,0,0]},{"name":"R","ir":[0.5,0,0]}]})";
check(!roomcorr_auto_prepare(2,48000,true,true,false).applied,"no correction does not apply");put(good);
check(!roomcorr_auto_prepare(2,48000,false,true,false).applied,"off setting disables automatic correction");
auto manual=roomcorr_auto_prepare(2,48000,true,false,false);check(!manual.applied&&manual.manualIr,"manual IR has priority");
auto applied=roomcorr_auto_prepare(2,48000,true,true,false);check(applied.applied&&applied.irs.size()==2&&applied.irs[0][0]==1&&applied.irs[1][0]==.5,"valid two-channel correction applies with exact coefficients");
auto mismatch=roomcorr_auto_prepare(6,48000,true,true,false);check(!mismatch.applied&&mismatch.mismatch,"layout mismatch does not apply partial correction");
for(const auto&bad:std::vector<std::pair<std::string,const char*>>{
 {good+" trailing","trailing garbage is rejected"},
 {good.substr(0,good.size()-2),"truncated JSON is rejected"},
 {R"({"layout":"2.0","fs":48000,"taps":3,"channels":[{"name":"L","ir":[1,0,0]}]})","missing output channel is rejected"},
 {R"({"layout":"2.0","fs":48000,"taps":3,"channels":[{"name":"L","ir":[1,0,0]},{"name":"R","ir":[1]}]})","unequal FIR lengths are rejected"},
 {R"({"layout":"2.1","fs":48000,"taps":3,"channels":[{"name":"L","ir":[1,0,0]},{"name":"R","ir":[1,0,0]}]})","declared layout mismatch is rejected"},
 {R"({"layout":"2.0","fs":48000,"taps":3,"channels":[{"name":"R","ir":[1,0,0]},{"name":"L","ir":[1,0,0]}]})","swapped channel names are rejected"},
 {R"({"layout":"2.0","fs":0,"taps":3,"channels":[{"name":"L","ir":[1,0,0]},{"name":"R","ir":[1,0,0]}]})","zero sample rate is rejected"},
 {R"({"layout":"2.0","fs":48000,"taps":3,"channels":[{"name":"L","ir":[NaN,0,0]},{"name":"R","ir":[1,0,0]}]})","NaN coefficient is rejected before DSP"},
 {R"({"layout":"2.0","fs":48000,"taps":3,"channels":[{"name":"L","ir":[1e309,0,0]},{"name":"R","ir":[1,0,0]}]})","overflow coefficient is rejected before DSP"},
 {R"({"layout":"2.0","fs":"48000","taps":3,"channels":[{"name":"L","ir":[1,0,0]},{"name":"R","ir":[1,0,0]}]})","sample rate string type is rejected"}}){put(bad.first);auto r=roomcorr_auto_prepare(2,48000,true,true,false);check(!r.applied&&!r.why.empty(),bad.second);}
put(good);check(roomcorr_auto_prepare(2,48000,true,true,false).applied,"valid correction recovers after all malformed candidates");
printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='rlplayer-roomcorr-auto-') as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(code,encoding='utf-8')
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe','-std=c++20','-O1','-I'+str(root/'src'),str(p/'test.cpp'),'-o',str(p/'test.exe'),'-lwinhttp','-lws2_32'],check=True)
 sys.exit(subprocess.run([str(p/'test.exe'),d],cwd=d).returncode)
