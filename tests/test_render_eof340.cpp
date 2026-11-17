// Actual RenderQ EOF handshake and render_loop, with a capturing device only.
// Never loads an ASIO driver or opens an audio endpoint.
#include "audio/render_loop.h"
#include "audio/asio_out.h"
#include <cstdio>
#include <thread>
#include <algorithm>

using namespace rta;
static int checks=0,failures=0;
static void check(bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL %s\n",why);}}

namespace rta::audio {
struct AsioDriverTestAccess {
    static void prepare(AsioDriver& d,size_t period,std::vector<std::vector<int32_t>>& out){
        d.ch_=2;d.srcBits_=32;d.bufSize_=(long)period;d.dsdMode_=false;
        d.types_.assign(2,ASIOSTInt32LSB);d.bufs_.resize(2);
        d.ringFrames_=std::max<size_t>(AsioDriver::kRingFrames,period*4);
        d.ring_.assign(d.ringFrames_*2,0);d.ringR_=d.ringW_=d.ringCount_=0;
        d.reset_completion_state();
        for(size_t c=0;c<2;++c){d.bufs_[c].buffers[0]=out[c].data();d.bufs_[c].buffers[1]=out[c].data();}
    }
    static void callback(AsioDriver& d,int index=0){d.on_buffer(index);}
    static void native(AsioDriver& d,IASIO* stub,size_t carrierPeriod){
        d.drv_=stub;d.dsdMode_=true;d.dsdBufBytes_=(long)(carrierPeriod*2);d.bufSize_=(long)(carrierPeriod*16);
        d.types_.assign(2,ASIOSTDSDInt8MSB1);d.packing_=AsioDriver::DsdPack::Msb1;
    }
};
struct AsioBufferedStreamTestAccess {
    static void hooks(AsioBufferedStream& s,std::function<bool(std::string&)> start,
                      std::function<void(int)> wait,std::function<double()> clock){
        s.startHook_=std::move(start);s.waitHook_=std::move(wait);s.clockHook_=std::move(clock);
    }
};
}


struct CaptureEof : render::OutSink {
    size_t period=65536,frames=0,emptyWrites=0;double clock=0;uint32_t rate=1536000;
    std::vector<int32_t> samples;
    bool open(const render::SinkOpenOpts&,uint32_t,int,int)override{return true;}
    bool start()override{return true;}
    size_t buffer_frames()const override{return period;}
    bool wait_event(int ms)override{clock+=ms/1000.;return false;}
    size_t write_one_period(const int32_t* pcm,size_t n)override{
        if(!n){++emptyWrites;clock+=.25;return 0;}
        samples.insert(samples.end(),pcm,pcm+n*2);frames+=n;clock+=(double)n/rate;return n;
    }
    const std::string& last_error()const override{static const std::string s;return s;}
    void finish(){}
};

struct AsioCaptureEof : CaptureEof {
    audio::AsioDriver driver;audio::AsioBufferedStream stream{driver};std::vector<std::vector<int32_t>> out;
    bool hardwareStarted=false,callbacksEnabled=true;int nextIndex=0;double nextCallback=0;std::atomic<bool> cancelled{false};
    std::atomic<bool>* pauseControl=nullptr;std::atomic<bool>* pausedState=nullptr;
    bool pauseSent=false,resumeSent=false;double pauseAt=0;size_t silenceFrames=0;
    AsioCaptureEof():out(2,std::vector<int32_t>(period)) {
        audio::AsioDriverTestAccess::prepare(driver,period,out);stream.configure(false,rate,2,&cancelled);
        audio::AsioBufferedStreamTestAccess::hooks(stream,[&](std::string&){hardwareStarted=true;nextCallback=clock;advance(0);return true;},
            [&](int ms){advance(ms);},[&]{return clock*1000.;});
    }
    void callback(){
        audio::AsioDriverTestAccess::callback(driver,nextIndex);nextIndex^=1;
        for(size_t n=0;n<period;++n){samples.push_back(out[0][n]);samples.push_back(out[1][n]);}
    }
    void advance(int ms){clock+=ms/1000.;while(hardwareStarted&&callbacksEnabled&&clock+1e-12>=nextCallback){callback();nextCallback+=(double)period/rate;}}
    bool start()override{return stream.start();}
    bool keep_clock_on_pause()const override{return true;}
    bool wait_event(int ms)override{advance(ms);return true;}
    size_t write_one_period(const int32_t* pcm,size_t n)override{
        if(!n){++emptyWrites;clock+=.25;return 0;}
        const bool pauseSilence=pausedState&&pausedState->load();
        const auto accepted=stream.write(pcm,n);
        if(pauseSilence)silenceFrames+=accepted;else frames+=accepted;
        if(pauseControl&&!pauseSent&&frames>=period*4){pauseSent=true;pauseAt=clock;pauseControl->store(true);}
        if(pauseControl&&pauseSilence&&!resumeSent&&clock-pauseAt>=.6){resumeSent=true;pauseControl->store(false);}
        return accepted;
    }
    bool fatal_error()const override{return stream.failed();}
    const std::string& last_error()const override{return stream.last_error();}
    void finish(){check(stream.drain(),"real ASIO stream drains successfully");}
};

template<class Sink=CaptureEof> static void run(size_t total,size_t chunk,bool lateEof=false,bool pauseResume=false){
    render::RenderQ rq;rq.cap=3;
    std::atomic<bool> stop{false},pause{false},paused{false};
    std::atomic<double> seek{-1},volume{0},start{0},peak{0};std::atomic<size_t> played{0};
    std::atomic<long long> pad{0},w0{0},wr{0},pulls{0},clip{0};bool gpu=false;
    std::atomic<bool> releaseLog{false};
    std::thread producer([&]{
        for(size_t pos=0;pos<total;pos+=chunk){
            audio::AudioBuffer b;b.rate=1536000;b.bits=32;b.channels=2;
            const auto n=std::min(chunk,total-pos);b.raw.resize(n*2);
            for(size_t i=0;i<n;++i)b.raw[2*i]=b.raw[2*i+1]=(int32_t)(pos+i+1);
            std::unique_lock<std::mutex> lock(rq.mtx);
            rq.cv.wait(lock,[&]{return rq.q.size()<rq.cap||rq.abort.load();});
            if(rq.abort.load())return;
            rq.q.emplace_back(std::move(b),rq.epoch.load());lock.unlock();rq.cv.notify_all();
        }
        // Model the real producer's synchronous DSP_DRAIN log holding a slow
        // file handle, released while the renderer's next STAT log waits.
        if(lateEof)while(!releaseLog.load()&&!rq.abort.load())std::this_thread::yield();
        rq.wait_after_eof(rq.epoch.load());
    });
    Sink sink;render::RenderHost h;h.stop=&stop;h.pauseReq=&pause;h.paused=&paused;h.seekReq=&seek;
    if constexpr(std::is_same_v<Sink,AsioCaptureEof>)if(pauseResume){sink.pauseControl=&pause;sink.pausedState=&paused;}
    h.played=&played;h.volDb=&volume;h.startDb=&start;h.dbgPad=&pad;h.gpuOut=&gpu;h.rq=&rq;
    h.deviceRate=sink.rate;h.inRate=sink.rate;h.inFrames=total;h.totalOut=total;h.outBits=32;h.outCh=2;
    h.dbgW0=&w0;h.dbgWr=&wr;h.dbgPulls=&pulls;h.dbgClip=&clip;h.dbgPeak=&peak;
    h.nowSec=[&]{return sink.clock;};h.volumeInPipeline=true;
    h.log=[&](const std::string& text){
        if(lateEof&&text.starts_with("STAT ")){
            releaseLog.store(true);
            while(rq.eofEpoch.load()==0)std::this_thread::yield();
            sink.clock+=3.1; // Slow log completion is wall time, not more audio.
        }
    };
    sink.start();render::render_loop(sink,h);rq.abort.store(true);releaseLog.store(true);rq.cv.notify_all();producer.join();sink.finish();
    check(h.normalEnd&&!h.renderFail,"EOF completes normally without STARVING");
    check(sink.frames>=total&&sink.frames<total+sink.period,"all content and at most one partial device period are written");
    std::vector<int32_t> content=sink.samples;
    if(pauseResume){content.clear();for(size_t n=0;n<sink.samples.size()/2;++n)if(sink.samples[n*2]){content.push_back(sink.samples[n*2]);content.push_back(sink.samples[n*2+1]);}}
    bool equal=content.size()>=total*2;
    if(equal)for(size_t n=0;n<total;++n)if(content[2*n]!=(int32_t)(n+1)||content[2*n+1]!=(int32_t)(n+1)){equal=false;break;}
    check(equal,"every captured content frame arrives in order exactly once");
    if(!lateEof)check(sink.emptyWrites<=1,"EOF does not keep writing zero frames until starvation timeout");
    if constexpr(std::is_same_v<Sink,AsioCaptureEof>){
        check(sink.driver.buffered()==0,"ASIO hardware ring is completely drained before close");
        check(sink.driver.underruns()==0,"ASIO real callbacks never underrun at startup or EOF");
        check(sink.driver.drain_complete()&&sink.driver.played_units()==sink.stream.accepted_frames(),"both driver buffers completed every accepted frame before close");
        check(sink.driver.eof_silence_callbacks()>0,"post-EOF clock silence is reported separately");
        if(pauseResume)check(sink.pauseSent&&sink.resumeSent&&sink.silenceFrames>sink.period*4,
            "actual render pause exceeds the entire ASIO ring then resumes without starving or losing content");
        std::printf("ASIO_CALLBACKS callbacks=%llu underruns=%llu buffered=%zu\n",sink.driver.callbacks(),sink.driver.underruns(),sink.driver.buffered());
    }
    std::printf("total=%zu chunk=%zu captured=%zu empty=%zu virtualSeconds=%.6f normal=%d fail=%s\n",total,chunk,sink.frames,sink.emptyWrites,sink.clock,h.normalEnd,h.failMsg.c_str());
}

static void completion_edges(){
    {
        audio::AsioDriver driver;std::vector<std::vector<int32_t>> out(2,std::vector<int32_t>(16));
        audio::AsioDriverTestAccess::prepare(driver,16,out);std::vector<int32_t> data(32,321);
        check(driver.push(data.data(),16,true)==16,"final content and EOF publish in one driver lock");
        audio::AsioDriverTestAccess::callback(driver,0);
        check(driver.buffered()==0&&!driver.drain_complete(),"empty software ring still has one real driver buffer in flight");
        audio::AsioDriverTestAccess::callback(driver,1);
        check(!driver.drain_complete(),"the other buffer callback cannot retire the last content buffer");
        audio::AsioDriverTestAccess::callback(driver,0);
        check(driver.drain_complete()&&driver.played_units()==16,"return of the last content buffer proves its completion");
        check(driver.push(data.data(),16)==0,"content arriving after explicit EOF is rejected");
    }
    for(int iteration=0;iteration<200;++iteration){
        audio::AsioDriver driver;std::vector<std::vector<int32_t>> out(2,std::vector<int32_t>(16));
        audio::AsioDriverTestAccess::prepare(driver,16,out);std::vector<int32_t> data(64,123),tail(14,456);
        driver.push(data.data(),32);
        std::atomic<bool> go{false};size_t accepted=0;
        std::thread callback([&]{while(!go.load())std::this_thread::yield();audio::AsioDriverTestAccess::callback(driver,0);});
        std::thread writer([&]{while(!go.load())std::this_thread::yield();accepted=driver.push(tail.data(),7,true);});
        go.store(true);callback.join();writer.join();
        for(int n=0;n<5;++n)audio::AsioDriverTestAccess::callback(driver,(n+1)%2);
        check(accepted==7&&driver.drain_complete()&&driver.played_units()==39&&driver.underruns()==0,
              "partial final push racing a real callback never loses data or misclassifies EOF as underrun");
    }
    {
        AsioCaptureEof sink;sink.start();std::vector<int32_t> data(sink.period*4,123);
        sink.write_one_period(data.data(),sink.period*2);sink.advance(300);
        const auto starvation=sink.driver.underruns();check(starvation>0,"negative control produces actual midstream starvation");
        check(sink.stream.drain()&&sink.driver.underruns()==starvation,"EOF never resets or hides a pre-existing underrun");
    }
    {
        AsioCaptureEof sink;sink.start();std::vector<int32_t> data(sink.period*4,123);
        sink.write_one_period(data.data(),sink.period*2);sink.callbacksEnabled=false;
        const double start=sink.clock;check(!sink.stream.drain()&&sink.stream.drain_state()=="timeout"&&sink.clock-start<2,
            "a stalled device fails drain within a finite budget without claiming completion");
    }
    {
        AsioCaptureEof sink;sink.start();std::vector<int32_t> data(sink.period*4,123);
        sink.write_one_period(data.data(),sink.period*2);sink.callbacksEnabled=false;sink.cancelled.store(true);
        const double start=sink.clock;check(sink.stream.drain()&&sink.stream.drain_state()=="cancelled"&&sink.clock==start,
            "user stop cancels a pending drain immediately without device callbacks");
    }
}

// A local ABI object only: no registry lookup, CoCreateInstance, or endpoint.
struct LocalAsio : audio::IASIO {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**)override{return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return 1;} ULONG STDMETHODCALLTYPE Release()override{return 1;}
    audio::ASIOBool init(void*)override{return 1;}void getDriverName(char*)override{}long getDriverVersion()override{return 1;}
    void getErrorMessage(char*)override{}audio::ASIOError start()override{return audio::ASE_OK;}
    audio::ASIOError stop()override{return audio::ASE_OK;}
    audio::ASIOError getChannels(long*,long*)override{return audio::ASE_OK;}
    audio::ASIOError getLatencies(long*,long*)override{return audio::ASE_OK;}
    audio::ASIOError getBufferSize(long*,long*,long*,long*)override{return audio::ASE_OK;}
    audio::ASIOError canSampleRate(audio::ASIOSampleRate)override{return audio::ASE_OK;}
    audio::ASIOError getSampleRate(audio::ASIOSampleRate*)override{return audio::ASE_OK;}
    audio::ASIOError setSampleRate(audio::ASIOSampleRate)override{return audio::ASE_OK;}
    audio::ASIOError getClockSources(audio::ASIOClockSource*,long*)override{return audio::ASE_OK;}
    audio::ASIOError setClockSource(long)override{return audio::ASE_OK;}
    audio::ASIOError getSamplePosition(audio::ASIOSamples*,audio::ASIOTimeStamp*)override{return audio::ASE_OK;}
    audio::ASIOError getChannelInfo(audio::ASIOChannelInfo*)override{return audio::ASE_OK;}
    audio::ASIOError createBuffers(audio::ASIOBufferInfo*,long,long,audio::ASIOCallbacks*)override{return audio::ASE_OK;}
    audio::ASIOError disposeBuffers()override{return audio::ASE_OK;}audio::ASIOError controlPanel()override{return audio::ASE_OK;}
    void* future(long,void*)override{return nullptr;}audio::ASIOError outputReady()override{return audio::ASE_OK;}
};

static void native_completion(){
    const size_t period=4096;const double rate=2822400.;
    for(size_t total:std::vector<size_t>{1,period-1,period,period+1,period*3+17}){
        LocalAsio stub;audio::AsioDriver driver;audio::AsioBufferedStream stream(driver);
        std::vector<std::vector<int32_t>> out(2,std::vector<int32_t>(period*2));
        audio::AsioDriverTestAccess::prepare(driver,period*2,out);audio::AsioDriverTestAccess::native(driver,&stub,period);
        stream.configure(true,(uint32_t)rate,2);
        double clock=0,next=0;int idx=0;bool started=false;std::vector<uint8_t> captured[2];
        auto advance=[&](int ms){
            clock+=ms/1000.;
            while(started&&clock+1e-12>=next){
                audio::AsioDriverTestAccess::callback(driver,idx);idx^=1;
                for(int c=0;c<2;++c){const auto* bytes=(const uint8_t*)out[c].data();captured[c].insert(captured[c].end(),bytes,bytes+period*2);}
                next+=(double)period/rate;
            }
        };
        audio::AsioBufferedStreamTestAccess::hooks(stream,[&](std::string&){started=true;next=clock;advance(0);return true;},advance,[&]{return clock*1000.;});
        stream.start();std::vector<int32_t> words(total*2);
        for(size_t n=0;n<total;++n)for(size_t c=0;c<2;++c)words[n*2+c]=(int32_t)(((n&1)?0xFA0000:0x050000)|((n*37+c*11)&0xFFFF));
        for(size_t n=0;n<total;n+=137){const auto take=std::min<size_t>(137,total-n);check(stream.write(words.data()+n*2,take)==take,"native stream accepts exact carrier chunks");}
        check(stream.drain()&&driver.drain_complete()&&driver.buffered()==0,"native DSD drains both actual callback buffers");
        bool same=true;
        for(size_t c=0;c<2;++c)for(size_t n=0;n<total;++n){const uint32_t w=(uint32_t)words[n*2+c];if(captured[c][n*2]!=(uint8_t)(w>>8)||captured[c][n*2+1]!=(uint8_t)w)same=false;}
        check(same&&driver.played_units()==total*2,"native DSD preserves every payload byte through a partial final period");
        check(driver.underruns()==0&&driver.eof_silence_callbacks()>0,"native DSD has zero underruns with separate EOF silence");
        bool balanced=true;for(size_t c=0;c<2;++c)for(size_t n=total*2;n<captured[c].size();++n)if(captured[c][n]!=0x69)balanced=false;
        check(balanced,"native EOF silence remains the valid balanced 0x69 bit pattern");
    }
}
int main(){run(6157292,30720);run(1,1);run(65536,30720);run(65537,30720);run(6157292,30720,true);run<AsioCaptureEof>(6157292,30720);run<AsioCaptureEof>(1,1);run<AsioCaptureEof>(65537,30720);run<AsioCaptureEof>(6157292,30720,false,true);completion_edges();native_completion();std::printf("RENDER_EOF checks=%d failures=%d audioOpened=0\n",checks,failures);return failures?1:0;}
