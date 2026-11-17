// Exercise the real producer Pipeline and render_loop with a capturing device
// substitute. No audio endpoint is opened. The negative control enables the
// old post-quantization gain and must destroy the requested DAC lattice/gain.
#include "audio/render_loop.h"
#include "engine/pipeline.h"
#include <cstdio>
#include <thread>
#include <stdexcept>
#include <tuple>

using namespace rta;
static int checks=0, failures=0;
static void check(bool good,const char* text){++checks;if(!good){++failures;std::printf("FAIL %s\n",text);}}

struct Capture : render::OutSink {
    uint32_t rate=48000;int bits=32;size_t period=480;
    double clock=0,gain=1;size_t changedAt=0,restoredAt=0;int nonUnityGainCalls=0;
    std::atomic<double>* volume=nullptr;
    std::vector<int32_t> samples;
    bool open(const render::SinkOpenOpts&,uint32_t,int,int)override{return true;}
    bool start()override{return true;}
    size_t buffer_frames()const override{return period;}
    void set_live_gain_db(double db)override{if(db!=0)++nonUnityGainCalls;gain=std::pow(10.,db/20.);}
    size_t write_one_period(const int32_t* pcm,size_t frames)override{
        const auto n=std::min(frames,period);
        for(size_t k=0;k<n*2;++k)samples.push_back(gain==1?pcm[k]:(int32_t)std::llround(pcm[k]*gain));
        const auto written=samples.size()/2;clock=(double)written/rate;
        if(!changedAt&&written>=rate/4){changedAt=written;volume->store(-12.);}
        if(!restoredAt&&written>=rate){restoredAt=written;volume->store(0.);}
        return n;
    }
    const std::string& last_error()const override{static const std::string empty;return empty;}
};

struct Result {std::vector<int32_t> samples;size_t changedAt=0;int nonUnityCalls=0;bool observedDsp=false,observedBypass=false;};
static Result run(uint32_t rate,int containerBits,int validBits,dsp::Shaper shaper,bool automatic,bool finalGain,double initialDb=0) {
    const size_t chunk=std::clamp<size_t>(rate/50,256,4096),total=(size_t)rate*2;
    const int32_t source=(int32_t)std::llround(.0007*std::ldexp(1.,containerBits-1));
    engine::PipelineConfig cfg;cfg.outRate=rate;cfg.outBits=containerBits;cfg.pcmValidBits=validBits;
    cfg.volumeEnabled=true;cfg.volumeDb=initialDb;cfg.volumeRampMs=200;cfg.shaper=shaper;
    cfg.pcmAutoDither=automatic;
    engine::Pipeline pipeline(cfg);
    render::RenderQ queue;queue.cap=3;
    std::atomic<bool> stop{false},pause{false},paused{false};
    std::atomic<double> seek{-1},volume{initialDb},start{initialDb},peak{0};std::atomic<size_t> played{0};
    std::atomic<long long> pad{0},w0{0},wr{0},pulls{0},clips{0};bool gpu=false;
    std::atomic<bool> observedDsp{false},observedBypass{false};
    std::string producerError;
    Capture sink;sink.rate=rate;sink.bits=containerBits;sink.period=rate/100;sink.volume=&volume;
    std::thread producer([&]{
        try {
            for(size_t pos=0;pos<total;pos+=chunk){
                audio::AudioBuffer input;input.rate=rate;input.bits=containerBits;input.channels=2;
                input.raw.assign(std::min(chunk,total-pos)*2,source);
                pipeline.set_volume_db(volume.load());engine::PipelineStats stats;
                auto output=pipeline.process(input,&stats);
                if(stats.bitPerfect||stats.zeroPad)observedBypass.store(true);else observedDsp.store(true);
                std::unique_lock<std::mutex> lock(queue.mtx);
                queue.cv.wait(lock,[&]{return queue.q.size()<queue.cap||queue.abort.load();});
                if(queue.abort.load())break;
                queue.q.emplace_back(std::move(output),queue.epoch.load());lock.unlock();queue.cv.notify_all();
            }
        }catch(const std::exception& e){producerError=e.what();}
        {std::lock_guard<std::mutex> lock(queue.mtx);queue.done=true;}queue.cv.notify_all();
    });
    render::RenderHost host;host.stop=&stop;host.pauseReq=&pause;host.paused=&paused;host.seekReq=&seek;host.played=&played;
    host.volDb=&volume;host.startDb=&start;host.dbgPad=&pad;host.gpuOut=&gpu;host.rq=&queue;
    host.deviceRate=rate;host.inRate=rate;host.inFrames=total;host.totalOut=total;host.outBits=containerBits;host.outCh=2;
    host.dbgW0=&w0;host.dbgWr=&wr;host.dbgPulls=&pulls;host.dbgClip=&clips;host.dbgPeak=&peak;
    host.nowSec=[&]{return sink.clock;};host.starveEnabled=false;host.deviceGuardEnabled=false;host.volumeInPipeline=finalGain;
    render::render_loop(sink,host);queue.abort.store(true);queue.cv.notify_all();producer.join();
    check(producerError.empty(),"producer processed the real pipeline without error");
    check(host.normalEnd&&!host.renderFail&&sink.samples.size()==total*2,"render loop emits all frames exactly once");
    return {std::move(sink.samples),sink.changedAt,sink.nonUnityGainCalls,observedDsp.load(),observedBypass.load()};
}

static double mean(const Result& r,uint32_t rate,int bits,double first,double last){
    double sum=0;size_t count=0;
    for(size_t n=(size_t)(rate*first);n<(size_t)(rate*last);++n){sum+=r.samples[n*2];++count;}
    return sum/count/std::ldexp(1.,bits-1);
}

int main(){
    for(const auto [rate,container,valid,shaper,automatic]:std::vector<std::tuple<uint32_t,int,int,dsp::Shaper,bool>>{
        {48000,32,18,dsp::Shaper::None,false},{48000,32,20,dsp::Shaper::TPDF,false},
        {192000,32,24,dsp::Shaper::NS9,false},{48000,24,0,dsp::Shaper::None,true},
        {192000,24,0,dsp::Shaper::None,true},{48000,32,0,dsp::Shaper::None,false}}){
        const auto r=run(rate,container,valid,shaper,automatic,true);
        const double initial=mean(r,rate,container,.05,.2),attenuated=mean(r,rate,container,.7,.9),restored=mean(r,rate,container,1.5,1.8);
        const int effective=valid?std::min(valid,container):container;
        const double lsb=std::ldexp(1.,1-effective);
        check(std::fabs(attenuated-initial*std::pow(10.,-.6))<2*lsb+1e-8,"live gain is applied exactly once before quantization");
        check(std::fabs(restored-initial)<2*lsb+1e-8,"restored unity returns to the initial level");
        check(r.nonUnityCalls==0,"render loop never requests a second nonunity sink gain");
        const int64_t lattice=int64_t{1}<<(container-effective);
        check(std::all_of(r.samples.begin(),r.samples.end(),[&](int32_t x){return x%lattice==0;}),"all dynamic-volume samples remain on the requested DAC bit grid");
        if(automatic)check(r.observedBypass&&r.observedDsp,"automatic dither enters DSP after genuine initial integer bypass");
        size_t response=r.samples.size()/2;
        const double threshold=std::max(2*lsb,.02*std::fabs(initial));
        for(size_t pos=r.changedAt;pos+128<r.samples.size()/2;++pos){
            double avg=0;for(size_t n=0;n<128;++n)avg+=r.samples[(pos+n)*2];avg/=128*std::ldexp(1.,container-1);
            if(std::fabs(avg-initial)>threshold){response=pos;break;}
        }
        const size_t responseBudget=5*std::clamp<size_t>(rate/50,256,4096)+rate/100;
        check(response>=r.changedAt&&response-r.changedAt<=responseBudget,"bounded PCM queue responds within five DSP blocks and one device period");
        std::printf("rate=%u container=%d valid=%d auto=%d initial=%.9g attenuated=%.9g restored=%.9g responseMs=%.3f\n",
            rate,container,valid,automatic?1:0,initial,attenuated,restored,1000.*(response-r.changedAt)/rate);
    }
    const auto initialGain=run(48000,32,18,dsp::Shaper::None,true,true,-6.);
    const double initialLsb=std::ldexp(1.,-17);
    check(std::fabs(mean(initialGain,48000,32,.05,.2)-.0007*std::pow(10.,-.3))<2*initialLsb,
          "nonunity initial gain is snapped safely once before automatic dither");
    check(std::fabs(mean(initialGain,48000,32,.7,.9)-.0007*std::pow(10.,-.6))<2*initialLsb,
          "changing an already attenuated initial gain uses the absolute new target");
    check(std::fabs(mean(initialGain,48000,32,1.5,1.8)-.0007)<2*initialLsb,
          "restoring unity after attenuated startup does not retain the startup gain");
    check(initialGain.nonUnityCalls==0&&std::all_of(initialGain.samples.begin(),initialGain.samples.end(),[](int32_t x){return x%(1<<14)==0;}),
          "initial and dynamic gains preserve the 18-bit grid without a second sink gain");
    const auto mutant=run(48000,32,18,dsp::Shaper::None,false,false);
    const double ratio=mean(mutant,48000,32,.7,.9)/mean(mutant,48000,32,.05,.2);
    const bool gridBroken=std::any_of(mutant.samples.begin(),mutant.samples.end(),[](int32_t x){return x%(1<<14)!=0;});
    check(mutant.nonUnityCalls>0&&gridBroken&&std::fabs(ratio-std::pow(10.,-.6))>.1,
          "negative control reproduces old sink gain double attenuation and broken 18-bit grid");
    std::printf("PCM_LIVE_VOLUME checks=%d failures=%d audioOpened=0\n",checks,failures);
    return failures?1:0;
}
