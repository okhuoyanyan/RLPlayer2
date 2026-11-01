// Offline production Pipeline + SDM + DoP benchmark. Never opens an audio device.
#include "engine/pipeline.h"
#include "dsp/dop_stream.h"
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <fstream>
using Clock=std::chrono::steady_clock;
static double seconds(Clock::time_point t) { return std::chrono::duration<double>(Clock::now()-t).count(); }
int main(int argc,char**argv) {
    if(argc!=9) { std::fprintf(stderr,"Usage: dsp_pipeline_benchmark legacy|fused single|cascade preset sourceRate DSDlevel sourceFrames blockFrames output.bin\nRequires AMD HIP rtagpu.dll; offline only, validates actual GPU dispatch.\n");return 2; }
    const std::string mode=argv[1], topology=argv[2];
    if((mode!="legacy"&&mode!="fused")||(topology!="single"&&topology!="cascade"))return 2;
    const auto* preset=rta::dsp::find_preset(argv[3]); if(!preset)return 2;
    const unsigned fs=std::stoul(argv[4]), level=std::stoul(argv[5]);
    const size_t frames=std::stoull(argv[6]),block=std::stoull(argv[7]);
    if((fs!=44100&&fs!=48000)||(level!=512&&level!=1024)||!frames||!block||frames>fs*10ull)return 2;
    const unsigned rate=fs*level;
    std::vector<rta::audio::AudioBuffer> inputs;
    for(size_t pos=0;pos<frames;pos+=block) {
        const size_t n=std::min(block,frames-pos);rta::audio::AudioBuffer in;
        in.rate=fs;in.bits=24;in.channels=2;in.raw.resize(n*2);
        for(size_t i=0;i<n;++i) {
            const int v=int(std::llround(.001*8388608.*std::sin(2*rta::dsp::kPi*1000*(pos+i)/fs)));
            in.raw[2*i]=in.raw[2*i+1]=v;
        }
        inputs.push_back(std::move(in));
    }
    auto dll=LoadLibraryW(L"rtagpu.dll"); if(!dll)return 20;
    auto available=(int(*)())GetProcAddress(dll,"rta_gpu_available");
    if(!available||available()!=1)return 21; // actual constant write/read kernel
    const bool compactAvailable=GetProcAddress(dll,"rta_gpu_resample_compact_planar")!=nullptr;
    auto start=Clock::now();
    rta::engine::PipelineConfig cfg;rta::engine::apply_filter_preset(cfg,*preset);
    cfg.autoTaps=true;cfg.outRate=rate;cfg.outBits=24;cfg.useFixed=true;cfg.gpuResample=true;
    rta::engine::apply_resample_purpose(cfg,rta::dsp::ResamplePurpose::Dsd);
    cfg.twoStage=(topology=="cascade"); // explicit benchmark topology, always reported
    rta::engine::Pipeline pipe(cfg);rta::engine::PipelineStats stats;
    rta::dsp::SdmConfig mc;mc.fs=rate;mc.order=5;mc.oneBitOutput=true;mc.ditherLsb=.0005;mc.cutoffHz=100000;
    std::vector<std::unique_ptr<rta::dsp::SdmModulator>> mods;
    for(int c=0;c<2;++c)mods.push_back(std::make_unique<rta::dsp::SdmModulator>(mc));
    rta::dsp::DopModulationStream stream;rta::dsp::DopPackState state;
    std::vector<std::vector<double>> carry(2);
    std::vector<std::vector<int32_t>> output;output.reserve(inputs.size());
    double pipelineSec=0,modPackSec=0,firstSec=0;size_t count=0;
    for(const auto& input:inputs) {
        auto t=Clock::now();std::vector<std::vector<double>> planes;pipe.process(input,&stats,&planes);
        pipelineSec+=seconds(t);if(!stats.gpuAllResampleStagesUsed||stats.gpuResampleStageMask!=(topology=="cascade"?3u:1u)||stats.usedFixed)return 22;
        if(stats.gpuCompactResampleStageMask!=(compactAvailable?(topology=="cascade"?3u:1u):0u))return 26;
        count+=planes[0].size();t=Clock::now();std::vector<int32_t> packed;std::string why;
        if(mode=="fused") {
            if(!stream.process(planes,mods,0,false,packed,&why))return 23;
        } else {
            std::vector<std::vector<double>> bits(2);
            rta::dsp::ParallelFor::instance().run(2,[&](size_t c) {
                auto& v=bits[c];v=carry[c];v.reserve(v.size()+planes[c].size());
                for(double x:planes[c])v.push_back(mods[c]->process(x));
            });
            const size_t use=bits[0].size()/16*16;
            const double* ptr[]={bits[0].data(),bits[1].data()};
            if(!rta::dsp::dop_pack_checked(ptr,2,use,packed,state,&why))return 23;
            for(int c=0;c<2;++c)carry[c].assign(bits[c].begin()+use,bits[c].end());
        }
        modPackSec+=seconds(t);output.push_back(std::move(packed));
        if(output.size()==1)firstSec=seconds(start);
    }
    const double wall=seconds(start);
    // Hash/persistence excluded. EOF/seek equivalence is independently tested.
    std::ofstream f(argv[8],std::ios::binary);if(!f)return 25;
    uint64_t hash=1469598103934665603ull;size_t bytes=0;
    for(const auto& v:output) {
        f.write(reinterpret_cast<const char*>(v.data()),v.size()*sizeof(int32_t));
        for(auto x:v)hash=(hash^uint32_t(x))*1099511628211ull;
        bytes+=v.size()*sizeof(int32_t);
    }
    f.close();if(!f)return 25;
    PROCESS_MEMORY_COUNTERS memory{};memory.cb=sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory));
    std::printf("{\"mode\":\"%s\",\"topology\":\"%s\",\"preset\":\"%s\",\"source_rate\":%u,\"dsd_rate\":%u,\"level\":%u,\"source_frames\":%zu,\"block\":%zu,\"wall\":%.9f,\"realtime\":%.9f,\"pipeline\":%.9f,\"mod_pack\":%.9f,\"first\":%.9f,\"samples_per_channel\":%zu,\"bytes\":%zu,\"hash\":\"%016llx\",\"cascade_active\":%s,\"gpu\":%s,\"gpu_stage_mask\":%u,\"gpu_all_stages\":%s,\"gpu_compact_stage_mask\":%u,\"peak_working_set\":%zu}\n",
        mode.c_str(),topology.c_str(),preset->name,fs,rate,level,frames,block,wall,double(frames)/fs/wall,
        pipelineSec,modPackSec,firstSec,count,bytes,(unsigned long long)hash,
        pipe.twoStageActive()?"true":"false",stats.usedGpuResample?"true":"false",stats.gpuResampleStageMask,stats.gpuAllResampleStagesUsed?"true":"false",stats.gpuCompactResampleStageMask,size_t(memory.PeakWorkingSetSize));
    return count==frames*level&&bytes==(frames*level/16)*8?0:24;
}
