#include "audio/wav.h"
#include "dsp/pcm_pack.h"
#include "engine/pipeline.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace rta;
static int32_t oracle(double x,int bits) {
    audio::AudioBuffer out;out.channels=1;out.bits=bits;out.raw.resize(1);
    double fs=double(1u<<(bits-1));
    out.setSample(0,0,std::floor(x*fs+.5)/fs);return out.raw[0];
}
int main() {
    size_t checked=0,simd=0,negativeControls=0;
    for(int bits: {8,16,24,32}) {
        const double fs=double(1u<<(bits-1));
        std::vector<double> values={0.,-0.,1.,-1.,1.5,-1.5,
            std::numeric_limits<double>::denorm_min(),-std::numeric_limits<double>::denorm_min(),
            std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),
            std::bit_cast<double>(uint64_t(0x7ff8000000012345)),
            std::numeric_limits<double>::max(),-std::numeric_limits<double>::max()};
        for(int k=-100;k<=100;++k) {
            double half=(k+.5)/fs;
            values.push_back(half);values.push_back(std::nextafter(half,INFINITY));values.push_back(std::nextafter(half,-INFINITY));
        }
        for(double full: {1.,-1.}) {values.push_back(std::nextafter(full,INFINITY));values.push_back(std::nextafter(full,-INFINITY));}
        for(double full: {-fs,fs-1})for(int offset: {-2,-1,0,1,2}) {
            double half=(full+offset+.5)/fs;
            values.push_back(half);values.push_back(std::nextafter(half,INFINITY));values.push_back(std::nextafter(half,-INFINITY));
        }
        for(double sign: {-1.,1.}) {
            double overflow=sign*std::numeric_limits<double>::max()/fs;
            values.push_back(overflow);values.push_back(std::nextafter(overflow,INFINITY));values.push_back(std::nextafter(overflow,-INFINITY));
        }
        uint64_t rng=0x123456789abcdefULL;
        for(int i=0;i<8192;++i){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;values.push_back(std::bit_cast<double>(rng));}
        for(int channels: {1,2,6}) for(size_t frames: std::vector<size_t>{0,1,2,3,4,5,17,621,values.size()+3}) {
            std::vector<std::vector<double>> planes(channels,std::vector<double>(frames));
            std::vector<const double*> pp(channels);
            for(int c=0;c<channels;++c){pp[c]=planes[c].data();for(size_t n=0;n<frames;++n)planes[c][n]=values[(n+c*13)%values.size()];}
            std::vector<int32_t> packed(frames*channels),scalar=packed;
            simd+=dsp::pack_pcm_planes(pp.data(),channels,frames,bits,packed.data());
            if(dsp::pack_pcm_planes(pp.data(),channels,frames,bits,scalar.data(),false)!=0)return 1;
            for(size_t n=0;n<frames;++n)for(int c=0;c<channels;++c) {
                auto expected=oracle(planes[c][n],bits);size_t i=n*channels+c;
                if(packed[i]!=expected || scalar[i]!=expected){std::fprintf(stderr,"mismatch bits%d ch%d frame%zu value%a got%d expected%d\n",bits,c,n,planes[c][n],packed[i],expected);return 2;}++checked;
            }
        }
        // Wrong tie-away and wrong overflow clipping must be rejected by oracle.
        if(oracle(-.5/fs,bits)!=int32_t(std::round(-.5)))++negativeControls;
        if(oracle(std::numeric_limits<double>::max(),bits)!=int32_t(fs-1))++negativeControls;
    }
    size_t pipelineSamples=0;
    for(int channels: {1,2,6})for(int mode=0;mode<4;++mode) {
        engine::PipelineConfig cfg;cfg.outRate=705600;cfg.outBits=16;
        cfg.gpuResample=false;cfg.twoStage=false;cfg.minTapsPerInput=7;
        cfg.shaper=mode==2 ? dsp::Shaper::TPDF : mode==3 ? dsp::Shaper::NS1 : dsp::Shaper::None;
        engine::Pipeline reference(cfg),candidate(cfg);
        std::vector<dsp::Quantizer> quant;
        const int inputBits=mode==1 ? 24 : 16;
        const bool hasQuant=cfg.shaper!=dsp::Shaper::None || cfg.outBits<inputBits;
        for(int c=0;c<channels;++c)quant.emplace_back(cfg.shaper,16,cfg.seed+(uint64_t)c*7919ull,double(cfg.outRate));
        for(size_t frames: {17u,31u,2u}) {
            audio::AudioBuffer input;input.channels=channels;input.bits=inputBits;input.raw.resize(frames*channels);
            for(size_t i=0;i<input.raw.size();++i)input.raw[i]=int32_t((i*7919)%30001)-15000;
            std::vector<std::vector<double>> planes;reference.process(input,nullptr,&planes);
            engine::PipelineStats stats;auto output=candidate.process(input,&stats);
            if(output.frames()!=planes[0].size() || stats.cpuParallelThreads<1)return 4;
            if(dsp::resampler_avx2_available() && stats.cpuSimdOutputs==0)return 5;
            audio::AudioBuffer expected=output;
            for(size_t n=0;n<output.frames();++n)for(int c=0;c<channels;++c) {
                double x=planes[c][n],y=hasQuant ? quant[c].process(x) : std::floor(x*32768+.5)/32768;
                expected.setSample(n,c,y);++pipelineSamples;
            }
            if(output.raw!=expected.raw)return 6;
        }
    }
    if(negativeControls!=8 || (dsp::resampler_avx2_available() && !simd))return 3;
    std::printf("PASS samples=%zu actual_simd_samples=%zu negative_controls=%zu pipeline_samples=%zu\n",checked,simd,negativeControls,pipelineSamples);
}
