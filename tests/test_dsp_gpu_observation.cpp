#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "engine/pipeline.h"
#include <cstdio>
#include <cmath>
#include <stdexcept>
using namespace rta::dsp;
static int checks=0,fails=0;
static void check(bool condition,const char* label){++checks;if(!condition){++fails;std::printf("FAIL %s\n",label);}}
static ResamplerConfig config(bool cascade=false,bool fixed=false){
 ResamplerConfig c;c.inRate=44100;c.outRate=44100*16;c.channels=1;
 c.minTapsPerInput=32;c.totalTaps=512;c.transition=.1;c.attenDb=120;
 c.twoStage=cascade;c.stage2Taps=64;c.useFixed=fixed;return c;
}
static size_t process(PolyphaseResampler&r,size_t frames,std::vector<double>* result=nullptr){
 std::vector<double> input(frames);for(size_t i=0;i<frames;++i)input[i]=(double)((int)i%11-5)/8388608.;
 std::vector<double> output(r.maxOutFrames(frames));const double* ip[]={input.data()};double* op[]={output.data()};
 const size_t n=r.process(ip,frames,op);output.resize(n);if(result)*result=std::move(output);return n;
}
int main(int argc,char**argv){
 setvbuf(stdout,nullptr,_IONBF,0);
 const std::string mode=argc>1?argv[1]:"real";
 const bool actual=mode=="real"||mode=="success";
 const unsigned leaf=(actual||mode=="first")?1u:0u;
 const unsigned cascade=actual?3u:mode=="first"?1u:mode=="second"?2u:0u;
 set_gpu_vendor("amd");
 for(bool two:{false,true}){
  auto c=config(two);PolyphaseResampler cpu(c),gpu(c);gpu.set_use_gpu(true);
  check(gpu.gpu_active(),"loaded backend capability is present even on dispatch failure");
  check(!gpu.gpu_used_last_block()&&!gpu.gpu_all_stages_used_last_block(),"not marked used before first process");
  std::vector<double>a,b;size_t na=process(cpu,128,&a),nb=process(gpu,128,&b);
  check(na==nb&&na>0,"real output frame count");
  double delta=0,peak=0;for(size_t i=0;i<std::min(a.size(),b.size());++i){delta=std::max(delta,std::abs(a[i]-b[i]));peak=std::max(peak,std::abs(a[i]));}
  check(peak>1e-9,"reference is nonzero, so an unexecuted zero-output kernel is rejected");
  check(delta<1e-15,"GPU/mock/fallback samples match unchanged CPU reference");
  const unsigned wanted=two?cascade:leaf;
  check(gpu.gpu_stage_mask_last_block()==wanted,"stage mask reflects actual successful dispatches");
  check(gpu.gpu_used_last_block()==(wanted!=0),"used flag means at least one successful GPU stage");
  check(gpu.gpu_all_stages_used_last_block()==(wanted==(two?3u:1u)),"full GPU flag rejects half fallback");
  std::printf("OBS mode=%s cascade=%d frames=%zu mask=%u all=%d max_abs_delta=%.17g nonzero_peak=%.17g\n",mode.c_str(),two,nb,gpu.gpu_stage_mask_last_block(),gpu.gpu_all_stages_used_last_block(),delta,peak);
  check(gpu.process(nullptr,0,nullptr)==0,"zero frames permit null pointers");
  check(gpu.gpu_stage_mask_last_block()==0&&!gpu.gpu_used_last_block(),"zero frames clear prior success");
  if(two)check(!gpu.first_stage()->gpu_used_last_block()&&!gpu.second_stage()->gpu_used_last_block(),"cascade zero frames clear child observations");
  gpu.set_use_gpu(false);process(gpu,64);check(!gpu.gpu_used_last_block(),"CPU block clears prior GPU success");
  gpu.set_use_gpu(true);process(gpu,32);gpu.reset();check(gpu.gpu_stage_mask_last_block()==0,"reset clears observation");
 }
 {
  auto c=config();c.inRate=96000;c.outRate=1000;
  PolyphaseResampler sparse(c);sparse.set_use_gpu(true);
  check(process(sparse,1)==1,"downsample first block emits one frame");
  check(process(sparse,1)==0&&!sparse.gpu_used_last_block(),"nonempty input with zero produced frames clears observation");
 }
 {
  auto c=config(false,true);PolyphaseResampler fixed(c);fixed.set_use_gpu(true);process(fixed,64);
  check(!fixed.gpu_active(),"fixed leaf is not a GPU capability");
  check(!fixed.gpu_used_last_block()&&fixed.gpu_stage_mask_last_block()==0,"fixed computation never reuses gpuDone as observation");
 }
 {
  auto c=config();c.outRate=c.inRate;PolyphaseResampler pass(c);pass.set_use_gpu(true);process(pass,8);
  check(!pass.gpu_used_last_block(),"passthrough does not dispatch GPU");
  check(pass.process(nullptr,0,nullptr)==0&&!pass.gpu_used_last_block(),"empty passthrough is false");
 }
 // Check the real Pipeline reads telemetry after this process, not capability
 // or the previous block. Integer PCM also exercises its fixed-point gate.
 for(bool two:{false,true})for(bool fixed:{false,true}){
  rta::engine::PipelineConfig cfg;cfg.outRate=44100*16;cfg.taps=512;cfg.minTapsPerInput=32;
  cfg.transition=.1;cfg.attenDb=120;cfg.twoStage=two;cfg.stage2Taps=64;cfg.useFixed=fixed;
  cfg.gpuResample=true;cfg.outBits=24;
  rta::engine::Pipeline pipeline(cfg);rta::audio::AudioBuffer in;
  in.rate=44100;in.channels=1;in.bits=24;in.raw.resize(128);
  for(size_t i=0;i<in.raw.size();++i)in.raw[i]=(int)i%11-5;
  rta::engine::PipelineStats stats;std::vector<std::vector<double>> planes;
  check(!pipeline.gpuResampleActive(),"Pipeline public badge observation is false before process");
  pipeline.process(in,&stats,&planes);
  unsigned wanted=two?cascade:leaf;
  if(fixed)wanted=two?(cascade&2u):0u;
  check(pipeline.gpuResampleActive()==(wanted!=0),"Pipeline public badge observation rejects capability-only and partial/fixed misreporting");
  check(stats.usedGpuResample==(wanted!=0),"Pipeline used flag is post-process observation");
  check(stats.gpuResampleStageMask==wanted,"Pipeline exposes actual stage mask");
  check(stats.gpuAllResampleStagesUsed==(wanted==(two?3u:1u)),"Pipeline full-stage flag rejects partial GPU");
  check(stats.usedFixed==fixed,"Pipeline fixed gate remains unchanged");
  in.raw.clear();pipeline.process(in,&stats,&planes);
  check(stats.gpuResampleStageMask==0&&!stats.usedGpuResample&&!stats.gpuAllResampleStagesUsed,"Pipeline empty block clears telemetry");
  check(!pipeline.gpuResampleActive(),"Pipeline public badge observation clears after empty block");
  in.raw.resize(128);pipeline.process(in,&stats,&planes);
  auto direct=in;direct.rate=cfg.outRate;
  pipeline.process(direct,&stats);
  check(stats.bitPerfect&&!stats.usedGpuResample,"same Pipeline rate-matching bypass performs no resampling");
  check(!pipeline.gpuResampleActive(),"same Pipeline bypass clears prior GPU badge observation");
  pipeline.process(in,&stats,&planes);pipeline.seek_reset();
  check(!pipeline.gpuResampleActive(),"seek reset clears public GPU observation");
 }
 for(bool automatic:{false,true}){
  auto c=config();c.minTapsPerInput=automatic?32:0;c.totalTaps=16384;c.maxPrototypeTaps=5000;
  if(automatic)c.maxPrototypeTaps=100;
  bool rejected=false;try{PolyphaseResampler excessive(c);}catch(const std::runtime_error&e){rejected=std::string(e.what()).find("budget")!=std::string::npos;}
  check(rejected,automatic?"auto budget rejects without reducing T":"manual final T budget is enforced");
 }
 {
  auto c=config();c.minTapsPerInput=0;c.totalTaps=1<<22;c.phase=PhaseKind::Minimum;
  bool rejected=false;try{PolyphaseResampler manualHuge(c);}catch(const std::runtime_error&e){rejected=std::string(e.what()).find("budget")!=std::string::npos;}
  check(rejected,"manual 4M prototype exceeds default 2M budget without allocation");
 }
 {
  auto c=config();c.minTapsPerInput=0;c.totalTaps=16384;c.maxPrototypeTaps=0;
  PolyphaseResampler explicitUnlimited(c);check(explicitUnlimited.tapsPerPhase()==423,"explicit unlimited manual budget preserves final T");
 }
 char path[4096]{};GetModuleFileNameA(GetModuleHandleA("rtagpu.dll"),path,sizeof(path));
 std::printf("BACKEND %s\nRESULT mode=%s checks=%d fails=%d\n",path,mode.c_str(),checks,fails);
 return fails?1:0;
}
