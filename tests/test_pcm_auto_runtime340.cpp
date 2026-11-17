// Real Pipeline runtime transitions. Offline only: no audio endpoint/driver.
#include "engine/pipeline.h"
#include <cstdio>
#include <vector>
using namespace rta;
static size_t checks = 0, failures = 0;
static void check(bool ok, const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
static audio::AudioBuffer source(size_t frames, int bits = 16, uint32_t rate = 48000) {
    audio::AudioBuffer in; in.rate = rate; in.channels = 2; in.bits = bits;
    in.raw.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) for (int c = 0; c < 2; ++c)
        in.setSample(i, c, .25 + .1 * std::sin(2 * dsp::kPi * (997 + c * 113) * i / rate));
    return in;
}
static audio::AudioBuffer part(const audio::AudioBuffer& in, size_t at, size_t n) {
    auto p = in; p.raw.assign(in.raw.begin() + at * in.channels, in.raw.begin() + (at+n) * in.channels); return p;
}
struct Run { std::vector<int32_t> samples; std::vector<engine::PipelineStats> phase; };
static Run automate(engine::PipelineConfig cfg, const audio::AudioBuffer& in, bool small) {
    engine::Pipeline p(cfg); Run r;
    const size_t counts[] = {256, 1536, 1536, 512, 1536};
    const double volumes[] = {0, -6, 0, 0, -6}; size_t at = 0;
    for (size_t phase = 0; phase < 5; ++phase) {
        p.set_volume_db(volumes[phase]); engine::PipelineStats stats;
        for (size_t done = 0; done < counts[phase];) {
            const size_t n = std::min(counts[phase] - done, small ? size_t(1 + ((at+done)*19+31)%257) : counts[phase]);
            const auto o = p.process(part(in, at+done, n), &stats);
            r.samples.insert(r.samples.end(), o.raw.begin(), o.raw.end()); done += n;
        }
        r.phase.push_back(stats); at += counts[phase];
    }
    return r;
}
static void runtime_volume() {
    for (int bits : {16, 24, 32}) for (int outBits : {16, 24, 32}) {
        if (outBits < bits) continue;
        engine::PipelineConfig cfg; cfg.outBits = outBits; cfg.gpuResample = false;
        cfg.volumeEnabled = true; cfg.volumeRampMs = 20; cfg.pcmAutoDither = true;
        cfg.shaper = dsp::Shaper::LNS15; // Ignored as a stale preview when auto owns policy.
        auto in = source(5376, bits); const auto whole = automate(cfg, in, false), blocks = automate(cfg, in, true);
        check(whole.samples == blocks.samples, "runtime auto/volume output is identical for arbitrary caller chunks");
        const bool width = outBits > bits;
        check(width ? whole.phase[0].zeroPad : whole.phase[0].bitPerfect, "auto unity begins with exact integer transparency");
        check(whole.phase[0].pcmShaper == dsp::Shaper::None && whole.phase[3].pcmShaper == dsp::Shaper::None,
              "auto reports none on initial and recovered transparent blocks");
        check(whole.phase[1].pcmShaper == dsp::auto_pcm_shaper(outBits, in.rate) && whole.phase[1].usedVolume &&
              whole.phase[1].volumeDbObserved == -6., "runtime volume activates actual-rate dither and observed stats");
        bool raw = true; const int shift = outBits - bits;
        for (size_t n = 0; n < 256; ++n) for (int c = 0; c < 2; ++c)
            raw &= whole.samples[n*2+c] == (int32_t)((uint32_t)in.raw[n*2+c] << shift);
        // The ramp reaches unity at its 960th sample; transparency starts on
        // that same frame even when a larger caller block extends beyond it.
        for (size_t n = 256+1536+959; n < 256+1536+1536+512; ++n) for (int c = 0; c < 2; ++c)
            raw &= whole.samples[n*2+c] == (int32_t)((uint32_t)in.raw[n*2+c] << shift);
        check(raw, "all recovered-unity frames are raw exact including the middle of a block");
        const double step = std::ldexp(1., 1-outBits), target = std::pow(10., -6./20);
        const double first = double(whole.samples[256*2]) / std::ldexp(1., outBits-1);
        const double firstWant = in.sample(256,0) * (1+(target-1)/960);
        check(std::abs(first-firstWant) <= 2*step, "first transparent-to-DSP sample ramps from previous unity, never snaps");
        const size_t settled = 256+1200;
        const double last = double(whole.samples[settled*2]) / std::ldexp(1., outBits-1);
        check(std::abs(last-in.sample(settled,0)*target) <= 2*step, "dynamic ramp reaches requested gain before final PCM quantization");
        std::printf("RUNTIME input_bits=%d output_bits=%d samples=%zu chunk_exact=1 first=%.10f target_first=%.10f shaper=%s\n",
                    bits,outBits,whole.samples.size(),first,firstWant,dsp::shaper_name(whole.phase[1].pcmShaper));
    }
}
static void manual_grid_and_initial_volume() {
    for (int valid : {16,18,20,24}) {
        engine::PipelineConfig cfg; cfg.outBits=32; cfg.pcmValidBits=valid; cfg.gpuResample=false;
        cfg.volumeEnabled=true; cfg.volumeRampMs=20; cfg.pcmAutoDither=true;
        const auto in=source(5376,32); const auto a=automate(cfg,in,false), b=automate(cfg,in,true);
        const uint32_t mask=(uint32_t(1)<<(32-valid))-1;bool grid=true;
        for(auto v:a.samples)grid&=(uint32_t(v)&mask)==0;
        check(grid&&a.samples==b.samples,"dynamic gain preserves manual effective grid and chunk continuity");
        check(!a.phase[0].bitPerfect&&a.phase[0].pcmShaper==dsp::Shaper::TPDF,
              "manual DAC precision remains processed even at unity");
    }
    engine::PipelineConfig cfg;cfg.volumeEnabled=true;cfg.volumeDb=-60;cfg.pcmAutoDither=true;cfg.outBits=24;cfg.gpuResample=false;
    engine::Pipeline p(cfg);engine::PipelineStats st;const auto in=source(64,24);const auto out=p.process(in,&st);
    const double step=std::ldexp(1.,-23);bool safe=true;
    for(size_t n=0;n<in.frames();++n)for(int c=0;c<2;++c)safe&=std::abs(out.sample(n,c)-in.sample(n,c)*.001)<=2*step;
    check(safe&&st.usedVolume&&st.volumeDbObserved==-60,"starting at -60dB is snapped safely on the first sample");
}
static void rates_dsd_and_tail() {
    for(uint32_t rate:{48000u,96000u,176400u,192000u,352800u,384000u}) for(int bits:{16,24,32}) {
        auto in=source(128,bits,rate);engine::PipelineConfig cfg;cfg.outBits=bits;cfg.gpuResample=false;
        cfg.pcmAutoDither=true;cfg.volumeEnabled=true;cfg.volumeDb=-6;
        engine::Pipeline p(cfg);engine::PipelineStats st;p.process(in,&st);
        check(st.pcmShaper==dsp::auto_pcm_shaper(bits,rate),"auto actual shaper follows effective bits and actual rate");
        cfg.pcmValidBits=18;engine::Pipeline doubles(cfg);std::vector<std::vector<double>> planes;
        const auto noPcm=doubles.process(in,&st,&planes);bool exact=noPcm.raw.empty()&&st.pcmShaper==dsp::Shaper::None;
        for(size_t i=0;i<in.frames();++i)for(int c=0;c<2;++c)
            exact&=std::abs(planes[c][i]-in.sample(i,c)*std::pow(10.,-6./20))<1e-15;
        check(exact,"PCM-to-DSD doubles bypass both auto dither and manual effective bits");
    }
    auto in=source(4096,24);engine::PipelineConfig cfg;cfg.outBits=24;cfg.pcmAutoDither=true;cfg.gpuResample=false;
    cfg.crossfeed.enabled=true;cfg.volumeEnabled=true;cfg.volumeDb=-6;
    engine::Pipeline a(cfg),b(cfg);const auto first=a.process(in);std::vector<int32_t> all=first.raw,parts;
    for(size_t at=0;at<in.frames();){const size_t n=std::min(size_t(137),in.frames()-at);const auto o=b.process(part(in,at,n));parts.insert(parts.end(),o.raw.begin(),o.raw.end());at+=n;}
    const size_t tail=(size_t)std::ceil(a.crossfeed_tail_out_frames());const auto drain=a.drain_chunk(tail);all.insert(all.end(),drain.raw.begin(),drain.raw.end());
    for(size_t at=0;at<tail;){const size_t n=std::min(size_t(17),tail-at);const auto o=b.drain_chunk(n);parts.insert(parts.end(),o.raw.begin(),o.raw.end());at+=n;}
    check(tail>0&&all==parts,"crossfeed natural tail and quantizer history survive runtime-auto chunking");
    std::printf("TAIL source_frames=%zu tail_frames=%zu samples=%zu chunk_exact=1\n",in.frames(),tail,all.size());
}
int main(){runtime_volume();manual_grid_and_initial_volume();rates_dsd_and_tail();std::printf("PCM_AUTO_RUNTIME checks=%zu failures=%zu\n",checks,failures);return failures?1:0;}
