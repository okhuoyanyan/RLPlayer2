// Exercise the production render loop with a partial-write device and seek queue loss.
// No audio backend is opened. The oracle is the actual accepted carrier sequence,
// not the producer's marker counter or the historical packer.
#include "audio/render_loop.h"
#include <cstdio>

using namespace rta::render;

static int32_t carrier(unsigned payload, bool phaseA) {
    unsigned v = (phaseA ? 0x05u : 0xfau) << 16 | (payload & 0xffffu);
    if (v & 0x800000u) v |= 0xff000000u;
    return static_cast<int32_t>(v);
}

struct Fixture {
    RenderQ rq;
    std::atomic<bool> stop{false}, pause{false}, paused{false};
    std::atomic<double> seek{-1}, volume{0}, startVolume{0}, peak{0};
    std::atomic<size_t> played{0};
    std::atomic<long long> pad{0}, w0{0}, wr{0}, pulls{0}, clip{0};
    bool gpu = false, serviced = false;
    size_t generated = 0, queuedDiscarded = 0;
    int channels;
    double clock = 0;
    explicit Fixture(int ch) : channels(ch) {}
    rta::audio::AudioBuffer block(size_t frames, unsigned base) {
        rta::audio::AudioBuffer b;
        b.rate = 176400; b.bits = 24; b.channels = channels;
        for (size_t i = 0; i < frames; ++i, ++generated)
            for (int c = 0; c < channels; ++c)
                b.raw.push_back(carrier(base + unsigned(i) * 73 + unsigned(c) * 31,
                                        (generated & 1) == 0));
        return b;
    }
    RenderHost host(bool dop, size_t newFrames) {
        RenderHost h;
        h.stop=&stop; h.pauseReq=&pause; h.paused=&paused; h.seekReq=&seek;
        h.played=&played; h.volDb=&volume; h.startDb=&startVolume; h.dbgPad=&pad;
        h.gpuOut=&gpu; h.rq=&rq; h.deviceRate=176400; h.inRate=44100;
        h.outBits=24; h.outCh=channels; h.inFrames=100; h.totalOut=400;
        h.dbgW0=&w0; h.dbgWr=&wr; h.dbgPulls=&pulls; h.dbgClip=&clip; h.dbgPeak=&peak;
        h.dop=dop; h.starveEnabled=false; h.deviceGuardEnabled=false;
        // Deterministic producer scheduling: seek_reset/discard, then replace the
        // old epoch queue. The real loop has already cleared its partial pend.
        h.nowSec=[this,newFrames] {
            if (rq.seekFrame.exchange(-1) >= 0) {
                for (const auto& item : rq.q) queuedDiscarded += item.first.frames();
                rq.q.clear();
                rq.q.emplace_back(block(newFrames,0x9140),rq.epoch.load());
                rq.done=true; serviced=true;
            }
            return clock += .00001;
        };
        return h;
    }
};

struct PartialSink : OutSink {
    Fixture& f;
    size_t first, firstAvailable=0, calls=0;
    bool doSeek, retry;
    std::vector<int32_t> accepted;
    std::string error;
    PartialSink(Fixture& v,size_t n,bool sk,bool rt) : f(v),first(n),doSeek(sk),retry(rt) {}
    bool open(const SinkOpenOpts&,uint32_t,int,int) override { return true; }
    bool start() override { return true; }
    size_t buffer_frames() const override { return 4; }
    const std::string& last_error() const override { return error; }
    size_t write_one_period(const int32_t* p,size_t n) override { return write_available(p,n); }
    size_t write_available(const int32_t* p,size_t n) override {
        if (++calls > 100) { f.stop=true; return 0; }
        if (calls==1) firstAvailable=n;
        if (retry && (calls==2 || calls==3)) return 0;
        const size_t got=std::min(n,calls==1?first:size_t(2));
        accepted.insert(accepted.end(),p,p+got*f.channels);
        if (calls==1 && doSeek) f.seek=.5;
        return got;
    }
};

static bool run_case(int channels,size_t first,bool doSeek,bool retry,bool dop,size_t newFrames) {
    Fixture f(channels);
    std::vector<int32_t> old;
    for (unsigned i=0;i<4;++i) {
        auto b=f.block(4,0x8040+i*1024);
        old.insert(old.end(),b.raw.begin(),b.raw.end());
        f.rq.q.emplace_back(std::move(b),1);
    }
    if (!doSeek) f.rq.done=true;
    auto h=f.host(dop,newFrames);
    PartialSink sink(f,first,doSeek,retry);
    render_loop(sink,h);
    bool ok=h.normalEnd && !h.renderFail && sink.calls<=100;
    size_t markerErrors=0,payloadErrors=0;
    std::vector<int32_t> expected;
    if (doSeek) {
        expected.assign(old.begin(),old.begin()+first*channels);
        for(size_t i=0;i<newFrames;++i) for(int c=0;c<channels;++c)
            expected.push_back(carrier(0x9140+unsigned(i)*73+unsigned(c)*31,true));
        // With this sink, EOF padding happens after the first two new frames.
        const size_t padding = newFrames>2 && newFrames<6 ? (6-newFrames)%4 : 0;
        for(size_t i=0;i<padding;++i) for(int c=0;c<channels;++c)
            expected.push_back(dop?carrier(0x6969,true):0);
        ok=ok && f.serviced && f.queuedDiscarded==4 && sink.firstAvailable==12;
    } else {
        expected=old;
        // One odd first write, then pairs: three final frames are padded to four.
        for(int c=0;c<channels;++c) expected.push_back(dop?carrier(0x6969,true):0);
    }
    ok=ok && expected.size()==sink.accepted.size();
    for(size_t i=0;i<sink.accepted.size();++i) {
        const unsigned word=unsigned(sink.accepted[i]);
        if (dop && ((word>>16)&255u)!=((i/channels)&1?0xfau:0x05u)) ++markerErrors;
        if (i<expected.size() && (dop ? ((word&65535u)!=(unsigned(expected[i])&65535u))
                                     : (sink.accepted[i]!=expected[i]))) ++payloadErrors;
    }
    ok=ok && markerErrors==0 && payloadErrors==0;
    const bool hasBoundary=sink.accepted.size()>first*channels;
    ok=ok && hasBoundary;
    const unsigned lastBefore = hasBoundary?(unsigned(sink.accepted[(first-1)*channels])>>16)&255u:0;
    const unsigned firstAfter = hasBoundary?(unsigned(sink.accepted[first*channels])>>16)&255u:0;
    std::printf("%s ch=%d first=%zu seek=%d retry=%d dop=%d new=%zu queueDiscard=%zu pendingDiscard=%zu frames=%zu markerErrors=%zu payloadErrors=%zu boundary=%02x,%02x normal=%d\n",
        ok?"PASS":"FAIL",channels,first,doSeek,retry,dop,newFrames,f.queuedDiscarded,
        doSeek?sink.firstAvailable-first:0,sink.accepted.size()/channels,markerErrors,payloadErrors,lastBefore,firstAfter,h.normalEnd);
    return ok;
}

int main() {
    unsigned fails=0,cases=0;
    for(int ch:{1,2,6}) for(size_t first:{1u,2u}) for(bool retry:{false,true}) {
        ++cases; fails+=!run_case(ch,first,true,retry,true,5);
    }
    for(int ch:{1,2,6}) {
        ++cases; fails+=!run_case(ch,1,false,true,true,0);
        ++cases; fails+=!run_case(ch,1,false,true,false,0);
    }
    std::printf("%u cases, %u failures; real render_loop, no audio device\n",cases,fails);
    return fails?1:0;
}
