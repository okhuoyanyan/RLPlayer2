// Actual render_loop -> WasapiOutput::write_available -> pack -> COM buffers.
// No endpoint is opened. The restart fault is a stated driver model, not proof
// that the user's Portacapture has that internal failure. Baseline also checks
// normal COM across repeated/concurrent pause requests and verifies every sample.
#include "audio/render_loop.h"
#include "audio/wasapi_out.h"
#include <thread>
#include <iostream>
#include <cstring>
#include <algorithm>

struct MockClient : IAudioClient, IAudioRenderClient {
    bool running=false, stopped=false, poison=false, fault=false,dop=false;
    int starts=0, stops=0, bytes=3, channels=2;
    UINT32 capacity=128, acquired=0;
    std::vector<BYTE> buffer, captured;
    std::function<void(bool)> onWrite;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override {return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX*,LPCGUID) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* n) override {*n=capacity;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* n) override {*n=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* n) override {*n=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE,const WAVEFORMATEX*,WAVEFORMATEX**) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* a,REFERENCE_TIME* b) override {*a=*b=10000;return S_OK;}
    HRESULT STDMETHODCALLTYPE Start() override {++starts;running=true;if(stopped&&fault)poison=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE Stop() override {++stops;running=false;stopped=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE Reset() override {return S_OK;}
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE GetService(REFIID,void**) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 n,BYTE** out) override {
        if(!running)return AUDCLNT_E_NOT_INITIALIZED;
        if(acquired)return AUDCLNT_E_OUT_OF_ORDER;
        acquired=n;buffer.assign((size_t)n*channels*bytes,0xCD);*out=buffer.data();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 n,DWORD) override {
        if(n!=acquired)return AUDCLNT_E_OUT_OF_ORDER;
        bool silence=std::all_of(buffer.begin(),buffer.end(),[](BYTE x){return x==0;});
        if(dop){silence=true;for(size_t i=0;i<buffer.size();i+=3)silence&=buffer[i]==0x69&&buffer[i+1]==0x69;}
        // A driver that loses DMA packet phase after Stop/Start can return
        // success and keep emitting bad bytes. This capture is at that boundary.
        if(poison)std::fill(buffer.begin(),buffer.end(),0x55);
        captured.insert(captured.end(),buffer.begin(),buffer.end());acquired=0;
        if(onWrite)onWrite(silence);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return S_OK;
    }
};

namespace rta::audio {
struct WasapiOutputTestPeer {
    static void attach(WasapiOutput& out,MockClient& mock,int bits) {
        out.ac_=&mock;out.rc_=&mock;out.rate_=48000;out.channels_=2;out.bits_=bits;
        out.bufferFrames_=mock.capacity;out.period_=mock.dop?0:10000;
        out.fmtc_.bytes=mock.bytes;out.fmtc_.valid=bits;out.fmtc_.isFloat=false;
        // 本测试用假 IAudioClient 在微秒内跑完整段渲染循环，真实时间不前进；
        // 物理节流闸（pace_budget）开着会恒判"设备不可能消费这么快"而饿死。
        // 闸本身由 tests/test_pace_guard.cpp 用纯函数直接验证。
        out.set_pace_guard(false);
    }
};
}

struct CaptureSink : rta::render::OutSink {
    rta::audio::WasapiOutput output;MockClient mock;
    explicit CaptureSink(int bits,bool fault,bool dop=false) {mock.bytes=bits/8;mock.fault=fault;mock.dop=dop;if(dop)mock.capacity=127;rta::audio::WasapiOutputTestPeer::attach(output,mock,bits);output.start();}
    ~CaptureSink(){output.close();}
    bool open(const rta::render::SinkOpenOpts&,uint32_t,int,int) override{return true;}
    bool start() override{return true;}
    size_t buffer_frames() const override{return mock.capacity;}
    size_t write_one_period(const int32_t* p,size_t n) override{return output.write_available(p,n);}
    size_t write_available(const int32_t* p,size_t n) override{return output.write_available(p,n);}
    bool wait_event(int) override {std::this_thread::yield();return false;}
    long long last_pad() const override{return output.last_pad();}
    const std::string& last_error() const override{return output.last_error();}
    void pause() override {output.pause();}
    void resume() override {output.resume();}
    // No override annotation so the pre-fix revision can compile the red test.
    bool keep_clock_on_pause() const {return true;}
};

int main(int argc,char** argv) {
    const bool modelFault=argc<2||std::string(argv[1])!="--normal";
    int failures=0;size_t compared=0;
    for(int kind:{16,24,32,0}) {
        const bool dop=kind==0;const int bits=dop?24:kind;
        constexpr size_t frames=128*48;
        std::vector<int32_t> input(frames*2);
        for(size_t i=0;i<frames;++i){input[2*i]=int32_t(1+i%53);input[2*i+1]=-int32_t(1+i%47);}
        if(dop){for(size_t i=0;i<frames;++i)for(int c=0;c<2;++c){uint32_t v=((i&1)?0xFAu:0x05u)<<16;v|=(1+i%53+c*71);if(v&0x800000)v|=0xff000000;input[2*i+c]=(int32_t)v;}}
        std::vector<BYTE> expected(frames*2*(bits/8));
        rta::audio::ExContainer fmt;fmt.bytes=bits/8;fmt.valid=bits;fmt.isFloat=false;
        rta::audio::ex_pack_int32(fmt,2,bits,1,expected.data(),input.data(),frames);
        if(dop){expected.clear();for(auto v:input){expected.push_back(BYTE(v&255));expected.push_back(BYTE((v>>8)&255));}}
        CaptureSink sink(bits,modelFault,dop);
        std::atomic<bool> stop{false},pause{false},paused{false};
        std::atomic<double> seek{-1},volume{-60},startDb{-60},peak{0};
        std::atomic<size_t> played{0};std::atomic<long long> pad{0},w0{0},wr{0},pulls{0},clips{0};bool gpu=false;
        rta::render::RenderQ queue;
        std::thread producer([&]{
            for(size_t offset=0;offset<frames;offset+=257){
                rta::audio::AudioBuffer b;b.rate=48000;b.channels=2;b.bits=bits;
                const auto n=std::min(size_t(257),frames-offset);
                b.raw.assign(input.begin()+offset*2,input.begin()+(offset+n)*2);
                {std::lock_guard<std::mutex> lock(queue.mtx);queue.q.push_back({std::move(b),1});}queue.cv.notify_all();
            }
            {std::lock_guard<std::mutex> lock(queue.mtx);queue.done=true;}queue.cv.notify_all();
        });
        size_t writes=0;std::atomic<int> cycles{0};
        sink.mock.onWrite=[&](bool silent){if(!silent&&++writes%4==0&&cycles.load()<10)pause.store(true);};
        std::thread controller([&]{
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!stop.load()&&std::chrono::steady_clock::now()<deadline){
                if(paused.load()){
                    const auto before=played.load();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    if(played.load()!=before)++failures;
                    ++cycles;pause.store(false);
                    while(paused.load()&&!stop.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
                }else std::this_thread::yield();
            }
            if(std::chrono::steady_clock::now()>=deadline){++failures;stop.store(true);}
        });
        rta::render::RenderHost h;h.stop=&stop;h.pauseReq=&pause;h.paused=&paused;h.seekReq=&seek;
        h.played=&played;h.volDb=&volume;h.startDb=&startDb;h.dbgPad=&pad;h.gpuOut=&gpu;
        h.rq=&queue;h.deviceRate=48000;h.outBits=bits;h.outCh=2;h.inRate=48000;h.inFrames=frames;h.totalOut=frames;
        h.dbgW0=&w0;h.dbgWr=&wr;h.dbgPulls=&pulls;h.dbgClip=&clips;h.dbgPeak=&peak;
        h.starveEnabled=false;h.deviceGuardEnabled=false;
        h.dop=dop;
        rta::render::render_loop(sink,h);stop.store(true);controller.join();producer.join();
        std::vector<BYTE> content;
        const size_t frameBytes=2*(bits/8);
        size_t badMarkers=0;
        for(size_t i=0;i+frameBytes<=sink.mock.captured.size();i+=frameBytes){
            const auto a=sink.mock.captured.begin()+i,b=a+frameBytes;
            if(dop){
                const BYTE marker=((i/frameBytes)&1)?0xFA:0x05;
                if(a[2]!=marker||a[5]!=marker)++badMarkers;
                const bool silent=a[0]==0x69&&a[1]==0x69&&a[3]==0x69&&a[4]==0x69;
                if(!silent){content.insert(content.end(),a,a+2);content.insert(content.end(),a+3,a+5);}
            }else if(!std::all_of(a,b,[](BYTE x){return x==0;}))content.insert(content.end(),a,b);
        }
        size_t mismatches=0;
        for(size_t i=0;i<std::min(expected.size(),content.size());++i)if(expected[i]!=content[i])++mismatches;
        compared+=expected.size();
        const bool ok=content==expected&&cycles>=10&&h.normalEnd&&!h.renderFail&&!badMarkers;
        if(!ok)++failures;
        std::cout<<(ok?"PASS":"FAIL")<<" bits="<<bits<<" dop="<<dop<<" badMarkers="<<badMarkers<<" cycles="<<cycles<<" expectedBytes="<<expected.size()
                 <<" capturedContentBytes="<<content.size()<<" badBytes="<<mismatches
                 <<" COM_Start="<<sink.mock.starts<<" COM_Stop="<<sink.mock.stops<<"\n";
    }
    std::cout<<"PAUSE_DATA_RESULT failures="<<failures<<" comparedBytes="<<compared<<" driverRestartFault="<<modelFault<<"\n";
    return failures?1:0;
}
