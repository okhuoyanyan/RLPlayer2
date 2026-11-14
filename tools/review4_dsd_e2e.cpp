// Fourth review: CPU production Pipeline -> SDM -> checked DoP. Offline only.
// Decodes an actual PCM24 WAV fixture using audio::read_wav. No device APIs.
// Fixture generation, output persistence and digest are outside timed region.
// Entire output is retained, then hashed: no dead-loop benchmark.
#include "engine/pipeline.h"
#include "dsp/dop_stream.h"
#include <chrono>
#include <cstdio>
#include <fstream>
using Clock=std::chrono::steady_clock;
static double elapsed(Clock::time_point t) { return std::chrono::duration<double>(Clock::now()-t).count(); }
int main(int argc,char**argv) try {
    if(argc!=9) { std::fprintf(stderr,"usage: review4_dsd_e2e sourceRate sourceFrames blockFrames order cutoffHz preset single|cascade fixture.wav\n"); return 2; }
    const unsigned fs=std::stoul(argv[1]); const size_t frames=std::stoull(argv[2]), block=std::stoull(argv[3]);
    const int order=std::stoi(argv[4]); const double cutoff=std::stod(argv[5]); const auto* preset=rta::dsp::find_preset(argv[6]);
    const std::string topology=argv[7];
    if((fs!=44100&&fs!=48000)||!frames||frames>fs*4ull||!block||order<1||order>7||!preset||!(cutoff>0)||(topology!="single"&&topology!="cascade")) return 2;
    rta::audio::AudioBuffer fixture; fixture.rate=fs; fixture.bits=24; fixture.channels=2; fixture.raw.resize(frames*2);
    for(size_t i=0;i<frames;++i) for(size_t c=0;c<2;++c) fixture.raw[i*2+c]=int32_t(std::llround(.001*8388608*std::sin(2*rta::dsp::kPi*(997+211*c)*i/fs)));
    // Last-frame impulse ensures a dropped EOF cannot be mistaken for complete silence.
    fixture.raw[(frames-1)*2]=8388;
    rta::audio::write_wav(argv[8],fixture);
    _putenv_s("RLPLAYER_FILTER_CACHE","0"); // process-local; no user config/cache touched.
    auto start=Clock::now(); auto t=Clock::now(); auto input=rta::audio::read_wav(argv[8]); const double decode=elapsed(t);
    rta::engine::PipelineConfig cfg; rta::engine::apply_filter_preset(cfg,*preset);
    cfg.autoTaps=true; cfg.outRate=fs*1024u; cfg.outBits=24; cfg.useFixed=true; cfg.gpuResample=false;
    rta::engine::apply_resample_purpose(cfg,rta::dsp::ResamplePurpose::Dsd); cfg.twoStage=(topology=="cascade");
    rta::engine::Pipeline pipe(cfg); rta::engine::PipelineStats stats;
    rta::dsp::SdmConfig mc; mc.fs=cfg.outRate; mc.order=order; mc.oneBitOutput=true; mc.ditherLsb=.0005; mc.cutoffHz=cutoff;
    std::vector<std::unique_ptr<rta::dsp::SdmModulator>> mods;
    for(int c=0;c<2;++c) mods.push_back(std::make_unique<rta::dsp::SdmModulator>(mc));
    rta::dsp::DopModulationStream stream; std::vector<int32_t> words;
    std::vector<double> blockTimes; double pipeline=0, modpack=0, first=0, tailSec=0, steady=0;
    size_t skip=0, delay=0, natural=0, drainTotal=0, produced=0, emitted=0, firstFrames=0, missedSteady=0;
    auto emit=[&](std::vector<std::vector<double>>& planes,bool finish) {
        const size_t n=planes.empty()?0:planes[0].size(), drop=std::min(skip,n); skip-=drop; emitted+=n-drop;
        auto mt=Clock::now(); std::vector<int32_t> packed; std::string why;
        if(!stream.process(planes,mods,drop,finish,packed,&why)) throw std::runtime_error(why);
        words.insert(words.end(),packed.begin(),packed.end()); modpack+=elapsed(mt); if(first==0&&!words.empty()) first=elapsed(start);
    };
    for(size_t pos=0;pos<frames;pos+=block) {
        auto bt=Clock::now(); const size_t n=std::min(block,frames-pos); rta::audio::AudioBuffer chunk; chunk.rate=input.rate; chunk.bits=input.bits; chunk.channels=input.channels; chunk.isFloat=input.isFloat;
        chunk.raw.assign(input.raw.begin()+pos*2,input.raw.begin()+(pos+n)*2);
        t=Clock::now(); std::vector<std::vector<double>> planes; pipe.process(chunk,&stats,&planes); pipeline+=elapsed(t);
        if(stats.usedGpuResample) throw std::runtime_error("unexpected GPU dispatch");
        produced+=planes[0].size();
        if(pos==0) { delay=size_t(pipe.algorithmic_latency_out_frames()+.5); natural=size_t(std::ceil(pipe.resampler_tail_out_frames())); skip=delay; drainTotal=delay+natural; firstFrames=n; }
        emit(planes,false); const double btSec=elapsed(bt); blockTimes.push_back(btSec);
        if(pos!=0) {steady+=btSec; if(btSec>double(n)/fs) ++missedSteady;}
    }
    t=Clock::now(); size_t drained=0;
    while(drained<drainTotal) {
        // Request only the source frames needed; trim to the exact reported tail.
        const size_t sourceNeed=std::min(block,(drainTotal-drained+1023)/1024);
        auto pt=Clock::now(); std::vector<std::vector<double>> planes; pipe.drain_chunk(sourceNeed,&planes); pipeline+=elapsed(pt);
        if(planes.empty()||planes[0].empty()) throw std::runtime_error("drain made no progress");
        const size_t take=std::min(planes[0].size(),drainTotal-drained);
        for(auto& plane:planes)plane.resize(take); produced+=take; drained+=take; emit(planes,false);
    }
    const size_t pendingBeforeFinish=stream.pending_bits(); std::vector<std::vector<double>> empty; emit(empty,true); tailSec=elapsed(t);
    const double wall=elapsed(start); const size_t expected=frames*1024+natural, padded=expected%16?16-expected%16:0;
    uint64_t hash=1469598103934665603ull; size_t badMarkers=0;
    for(size_t i=0;i<words.size();++i) { hash=(hash^uint32_t(words[i]))*1099511628211ull; const unsigned want=(i/2)%2?0xfa:0x05; badMarkers+=((uint32_t(words[i])>>16)&255)!=want; }
    long long overload=0,clamp=0,nonfinite=0; for(auto& m:mods) {overload+=m->overload();clamp+=m->clamped();nonfinite+=m->nonFinite();}
    const bool ok=skip==0&&emitted==expected&&words.size()==((expected+15)/16)*2&&stream.pending_bits()==0&&!badMarkers&&!nonfinite;
    std::sort(blockTimes.begin(),blockTimes.end());
    std::printf("{\"scope\":\"PCM24_WAV_read_wav_CPU_Pipeline_SDM_DoP_memory_sink_sync\",\"cache\":\"disabled\",\"source_rate\":%u,\"dsd_rate\":%u,\"source_frames\":%zu,\"block_source_frames\":%zu,\"stage1_taps_per_phase\":%d,\"stage2_taps_per_phase\":%d,\"fixed\":%s,\"actual_cascade\":%s,\"steady_blocks_over_media_deadline\":%zu,\"channels\":2,\"source_seconds\":%.12g,\"preset\":\"%s\",\"topology\":\"%s\",\"requested_order\":%d,\"effective_order\":%d,\"cutoff_hz\":%.12g,\"sections\":%s,\"levels\":%d,\"stepped_down\":%s,\"wall_seconds\":%.12g,\"decode_seconds\":%.12g,\"prepare_seconds\":%.12g,\"first_output_seconds\":%.12g,\"source_steady_seconds\":%.12g,\"source_steady_media_seconds\":%.12g,\"source_steady_realtime\":%.12g,\"tail_seconds\":%.12g,\"pipeline_seconds\":%.12g,\"sdm_pack_sink_seconds\":%.12g,\"cold_e2e_realtime\":%.12g,\"algorithmic_delay_bits_per_channel\":%zu,\"natural_tail_bits_per_channel\":%zu,\"produced_bits_per_channel\":%zu,\"emitted_bits_per_channel\":%zu,\"expected_bits_per_channel\":%zu,\"pending_before_finish\":%zu,\"padding_bits_per_channel\":%zu,\"output_bytes\":%zu,\"bad_markers\":%zu,\"overload\":%lld,\"clamp\":%lld,\"nonfinite\":%lld,\"hash\":\"%016llx\",\"pass\":%s}\n",fs,cfg.outRate,frames,block,stats.tapsPerPhase,stats.stage2TapsPerPhase,stats.usedFixed?"true":"false",pipe.twoStageActive()?"true":"false",missedSteady,double(frames)/fs,preset->name,topology.c_str(),order,mods[0]->effectiveOrder(),cutoff,mods[0]->uses_sections()?"true":"false",mods[0]->levels(),mods[0]->steppedDown()?"true":"false",wall,decode,stats.prepareMs/1000,first,steady,double(frames-firstFrames)/fs,steady?double(frames-firstFrames)/fs/steady:0,tailSec,pipeline,modpack,double(frames)/fs/wall,delay,natural,produced,emitted,expected,pendingBeforeFinish,padded,words.size()*4,badMarkers,overload,clamp,nonfinite,(unsigned long long)hash,ok?"true":"false");
    return ok?0:1;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}


