#include "dsp/modulator.h"
#include <bit>
#include <cstdio>
#include <vector>
#include <limits>
using namespace rta::dsp;
int main() {
    constexpr size_t frames=131072;
    std::vector<double> input(frames),reference(frames);
    for(size_t i=0;i<frames;++i) {
        input[i]=.42*std::sin(0.013*(double)i)+.13*std::sin(.0071*(double)i);
        if(i>=frames/4&&i<frames/3)input[i]=std::sin(.023*(double)i);
        if(i>=3*frames/4)input[i]=0.;
    }
    input[frames/3]=5.;input[frames/3+2]=-5.;
    input[frames/2]=std::numeric_limits<double>::quiet_NaN();
    input[frames/2+1]=std::numeric_limits<double>::infinity();
    input[frames/2+2]=-std::numeric_limits<double>::infinity();
    size_t compared=0,cases=0,fails=0,zeroCallbacks=0;unsigned effectiveMask=0;
    auto counters_equal=[&](const SdmModulator&a,const SdmModulator&b) {
        return a.clamped()==b.clamped()&&a.overload()==b.overload()&&a.nonFinite()==b.nonFinite()
            &&a.effectiveOrder()==b.effectiveOrder()&&a.levels()==b.levels()
            &&a.steppedDown()==b.steppedDown()&&a.stepDownReason()==b.stepDownReason()
            &&a.levels_clamped()==b.levels_clamped();
    };
    for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Chebyshev,ModulatorFamily::Ec,ModulatorFamily::Ahm})
      for(int order:{3,5,7})for(int ratio:{64,128,256,512,1024})
       for(bool binary:{true,false})for(double dither:{0.,.0005}) {
        SdmConfig cfg;cfg.family=family;cfg.order=order;cfg.fs=44100.*ratio;
        cfg.levels=binary?2:16;cfg.oneBitOutput=binary;cfg.ahmBits=8;
        cfg.ditherLsb=dither;cfg.bypassOrderGuard=true;
        SdmModulator scalar(cfg),batch(cfg),chunked(cfg);
        effectiveMask|=1u<<batch.effectiveOrder();
        for(size_t i=0;i<frames;++i)reference[i]=scalar.process(input[i]);
        batch.process_block(nullptr,0,[&](size_t,double){++zeroCallbacks;});
        batch.process_block(input.data(),frames,[&](size_t i,double value){
            if(std::bit_cast<uint64_t>(value)!=std::bit_cast<uint64_t>(reference[i]))++fails;
            ++compared;
        });
        // Tiny/uneven blocks exercise both partial chunks and sink-local indices.
        size_t pos=0,block=0;const size_t lengths[]={1,7,16,31,257,4096,8191,65537};
        while(pos<frames) {
            size_t count=std::min(lengths[block++%8],frames-pos);
            chunked.process_block(input.data()+pos,count,[&](size_t i,double value){
                if(std::bit_cast<uint64_t>(value)!=std::bit_cast<uint64_t>(reference[pos+i]))++fails;
                ++compared;
            });pos+=count;
        }
        if(!counters_equal(scalar,batch)||!counters_equal(scalar,chunked)||scalar.nonFinite()!=3)++fails;
        // reset intentionally preserves the existing RNG position.
        scalar.reset();batch.reset();chunked.reset();
        if(!counters_equal(scalar,batch)||scalar.overload()||scalar.nonFinite()||scalar.clamped())++fails;
        for(size_t i=0;i<4096;++i)reference[i]=scalar.process(input[i]);
        batch.process_block(input.data(),4096,[&](size_t i,double value){
            if(std::bit_cast<uint64_t>(value)!=std::bit_cast<uint64_t>(reference[i]))++fails;
            ++compared;
        });
        // Continue through both APIs on the same state, then switch again.
        for(size_t i=0;i<1024;++i) {
            double a=scalar.process(input[i]),b=batch.process(input[i]);
            if(std::bit_cast<uint64_t>(a)!=std::bit_cast<uint64_t>(b))++fails;
        }
        if(!counters_equal(scalar,batch))++fails;
        ++cases;
        std::printf("case=%zu family=%s order=%d ratio=%d levels=%d dither=%g compared=%zu fails=%zu\n",
          cases,modulator_family_name(family),order,ratio,batch.levels(),dither,compared,fails);
       }
    // The less common 1/2/4/6/8 dispatch arms and normal order guards.
    for(int order=1;order<=8;++order) {
        SdmConfig cfg;cfg.order=order;cfg.levels=16;cfg.ditherLsb=.0005;
        SdmModulator a(cfg),b(cfg);effectiveMask|=1u<<b.effectiveOrder();
        for(size_t i=0;i<frames;++i)reference[i]=a.process(input[i]);
        b.process_block(input.data(),frames,[&](size_t i,double v){
          if(std::bit_cast<uint64_t>(v)!=std::bit_cast<uint64_t>(reference[i]))++fails;++compared;
        });if(!counters_equal(a,b))++fails;
    }
    // A throwing sink consumes its current sample, exactly like an external
    // scalar process()+sink() loop. Also instantiate a non-copyable sink.
    {
        SdmConfig cfg;cfg.ditherLsb=.0005;
        SdmModulator a(cfg),b(cfg);
        for(size_t i=0;i<17;++i)(void)a.process(input[i]);
        try { b.process_block(input.data(),100,[&](size_t i,double){if(i==16)throw 42;}); }
        catch(int value){if(value!=42)++fails;}
        if(!counters_equal(a,b))++fails;
        struct Sink {
            Sink()=default;Sink(const Sink&)=delete;size_t count=0;
            void operator()(size_t,double){++count;}
        }sink;
        a.process_block(nullptr,0,sink);if(sink.count)++fails;
        for(size_t i=0;i<4096;++i)reference[i]=a.process(input[i]);
        b.process_block(input.data(),4096,[&](size_t i,double v){
          if(std::bit_cast<uint64_t>(v)!=std::bit_cast<uint64_t>(reference[i]))++fails;++compared;
        });
    }
    for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Chebyshev,ModulatorFamily::Ec,ModulatorFamily::Ahm})
      for(int ratio:{64,128,256,512,1024}) {
        SdmConfig cfg;cfg.family=family;cfg.order=7;cfg.oneBitOutput=true;cfg.fs=44100.*ratio;
        SdmModulator a(cfg),b(cfg);
        for(size_t i=0;i<4096;++i)reference[i]=a.process(input[i]);
        b.process_block(input.data(),4096,[&](size_t i,double v){
          if(std::bit_cast<uint64_t>(v)!=std::bit_cast<uint64_t>(reference[i]))++fails;++compared;
        });if(!counters_equal(a,b))++fails;
    }
    if(zeroCallbacks||effectiveMask!=0x1fe)++fails;
    std::printf("RESULT matrix_cases=%zu guard_cases=20 effective_order_mask=%x compared=%zu fails=%zu\n",
      cases,effectiveMask,compared,fails);
    return fails?1:0;
}
