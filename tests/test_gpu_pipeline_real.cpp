// Hardware acceptance: actual production Pipeline, CPU versus real GPU.
// Always fails on fallback/unavailable GPU. Never opens an audio endpoint.
#define NOMINMAX
#include "engine/pipeline.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace rta;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
 _putenv_s("RLPLAYER_FILTER_CACHE","0");dsp::set_gpu_vendor("amd");
 HMODULE dll=LoadLibraryW(L"rtagpu.dll");require(dll!=nullptr,"hardware test requires a staged rtagpu.dll");
 wchar_t dllPath[32768]{};require(GetModuleFileNameW(dll,dllPath,32768)>0,"cannot identify GPU artifact");
 std::printf("HARDWARE_GPU audio_device_calls=0 DLL=%ls\n",dllPath);
 size_t cases=0,samples=0,dispatches=0;
 for(auto rates:{std::pair{44100u,384000u},{48000u,176400u},{96000u,44100u},{44100u,48000u},{44100u,705600u}})
 for(const char* name:{"poly-sinc-short-lp","poly-sinc-short-mp","poly-sinc-ext2-hires-ip"})
 for(int channels:{1,2,6}){
  engine::PipelineConfig cfg;engine::apply_filter_preset(cfg,*dsp::find_preset(name));cfg.outRate=rates.second;
  cfg.autoTaps=true;cfg.useFixed=false;cfg.gpuResample=false;cfg.shaper=dsp::Shaper::None;
  engine::Pipeline cpu(cfg);cfg.gpuResample=true;engine::Pipeline gpu(cfg);
  size_t offset=0,different=0,nonzero=0,calls=0;double error=0;
  auto compare=[&](const auto& a,const auto& b){require(a.size()==b.size(),"channel count mismatch");
   for(size_t c=0;c<a.size();++c){require(a[c].size()==b[c].size(),"frame count mismatch");
    for(size_t i=0;i<a[c].size();++i){require(std::isfinite(b[c][i]),"nonfinite GPU output");++samples;
     different+=std::memcmp(&a[c][i],&b[c][i],sizeof(double))!=0;error=std::max(error,std::abs(a[c][i]-b[c][i]));nonzero+=b[c][i]!=0.;}}
   const auto* actual=gpu.prepared_resampler();require(actual!=nullptr,"no prepared GPU resampler");
   if(!b.empty()&&!b[0].empty()){require(actual->gpu_all_stages_used_last_block(),"actual Pipeline silently fell back to CPU");++dispatches;}
  };
  for(size_t n:{1u,7u,31u,257u,3u,511u,1025u,2u,61u}){
   audio::AudioBuffer input;input.rate=rates.first;input.channels=channels;input.bits=24;input.isFloat=false;input.raw.resize(n*channels);
   for(size_t i=0;i<n;++i)for(int c=0;c<channels;++c){uint64_t v=(offset+i+1)*6364136223846793005ull+1442695040888963407ull*(c+1);input.raw[i*channels+c]=int32_t(v>>42)-2097152;}
   std::vector<std::vector<double>> a,b;cpu.process(input,nullptr,&a);gpu.process(input,nullptr,&b);compare(a,b);offset+=n;++calls;
  }
  // Drain the complete declared causal support, crossing multiple block sizes.
  size_t remaining=size_t(std::ceil(cpu.prepared_resampler()->tailInputSamples()))+1;
  while(remaining){size_t n=std::min(remaining,size_t(333));std::vector<std::vector<double>> a,b;cpu.drain_chunk(n,&a);gpu.drain_chunk(n,&b);compare(a,b);remaining-=n;++calls;}
  require(different==0&&nonzero>0,"GPU samples differ or all-zero kernel false success");
  ++cases;std::printf("PASS rate=%u/%u preset=%s channels=%d stages=%d calls=%zu different=%zu max_abs=%.17g\n",rates.first,rates.second,name,channels,gpu.prepared_resampler()->two_stage()?2:1,calls,different,error);
 }
 std::printf("cases=%zu compared_samples=%zu real_GPU_blocks=%zu failures=0\n",cases,samples,dispatches);return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
