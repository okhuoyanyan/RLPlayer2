// Offline production-component benchmark. No device API, settings file, or network.
// Core DSP is included from the frozen source tree, never copied here.
// Phase sums exclude validation/checksumming; harness time includes those costs.
// This is not GUI first-sound latency, electrical power, or device throughput.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#include <array>
#include "engine/pipeline.h"
#include "audio/audio_loader.h"
#include "dsp/direct_sdm.h"
#include "dsp/dop_stream.h"
#if __has_include("dsp/dop_async.h")
#include "dsp/dop_async.h"
#define RTA_BENCH_ASYNC 1
#else
#define RTA_BENCH_ASYNC 0
#endif
#include <chrono>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <functional>
#include <map>
#include <fstream>

namespace a=rta::audio;
namespace d=rta::dsp;
namespace e=rta::engine;
using Clock=std::chrono::steady_clock;

struct Stamp {
    Clock::time_point wall=Clock::now();
    uint64_t cpu=0;
    Stamp() {
        FILETIME creation,exit,kernel,user;
        if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))
            throw std::runtime_error("GetProcessTimes failed");
        auto n=[](FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
        cpu=n(kernel)+n(user);
    }
};
struct Meter {
    double wall=0,cpu=0;
    void add(const Stamp& s,Meter* diagnostic=nullptr) {
        Stamp end; wall+=std::chrono::duration<double>(end.wall-s.wall).count();
        cpu+=double(end.cpu-s.cpu)*1e-7;
        if(diagnostic){diagnostic->wall+=std::chrono::duration<double>(end.wall-s.wall).count();diagnostic->cpu+=double(end.cpu-s.cpu)*1e-7;}
    }
};
struct Options {
    std::string route="pcm_pcm",preset="poly-sinc-short-lp",family="dsd";
    std::string topology="auto",decode="memory",fixture,inputWav;
    unsigned source=44100,target=705600;
    int order=5,gpu=0,overlap=0;
    size_t block=4096;
    double duration=1;
    bool selftest=false;
};
static Options parse(int argc,char**argv) {
    Options o;
    for(int i=1;i<argc;++i) {
        std::string k=argv[i];
        auto val=[&](){if(++i>=argc)throw std::runtime_error("missing value: "+k);return std::string(argv[i]);};
        if(k=="--route")o.route=val(); else if(k=="--source-rate")o.source=std::stoul(val());
        else if(k=="--target-rate")o.target=std::stoul(val());
        else if(k=="--preset")o.preset=val();else if(k=="--family")o.family=val();
        else if(k=="--order")o.order=std::stoi(val());else if(k=="--gpu")o.gpu=std::stoi(val());
        else if(k=="--overlap")o.overlap=std::stoi(val());
        else if(k=="--topology")o.topology=val();else if(k=="--decode-mode")o.decode=val();
        else if(k=="--block")o.block=std::stoull(val());else if(k=="--seconds")o.duration=std::stod(val());
        else if(k=="--fixture")o.fixture=val();else if(k=="--selftest")o.selftest=true;
        else if(k=="--input-wav")o.inputWav=val();
        else if(k=="--help") {
            std::cout<<"efficiency_benchmark --route pcm_pcm|pcm_dsd|dsd_direct|dsd_native_convert|dsd_decode_remod --source-rate Hz --target-rate Hz --seconds N --preset poly-sinc-short-lp --family dsd|ec|chebyshev|ahm --order N --gpu 0|1 --topology auto|single|cascade --block 4096 --decode-mode memory|stream --fixture absolute.dsf --input-wav absolute.wav\n"
              <<"No device opened. PCM --input-wav requires exact 2ch integer PCM24, matching --source-rate and round(rate*seconds) frames; decoding is outside DSP timing. WAV inputs allow up to 60s; generated sources and DSD fixtures retain the original 10s limit. Without WAV, generates a 2ch -60dBFS 1kHz source outside timing. DSD fixture is generated only if absent, otherwise validated and reused. --selftest validates counters/markers without running a benchmark.\n";
            std::exit(0);
        } else throw std::runtime_error("unknown argument: "+k);
    }
    if(o.selftest)return o;
    if(o.overlap!=0 && o.overlap!=1)throw std::runtime_error("overlap must be 0 or 1");
    if(o.overlap && (!RTA_BENCH_ASYNC || (o.route!="pcm_dsd" && o.route!="dsd_decode_remod")))
        throw std::runtime_error("overlap requires current async pipeline DoP route");
    if(o.route!="pcm_pcm"&&o.route!="pcm_dsd"&&o.route!="dsd_direct"&&o.route!="dsd_native_convert"&&o.route!="dsd_decode_remod")throw std::runtime_error("invalid route");
    const bool pcmRoute=o.route=="pcm_pcm"||o.route=="pcm_dsd";
    if(!o.inputWav.empty()&&!pcmRoute)throw std::runtime_error("--input-wav is only valid for PCM-source routes");
    const double maximumDuration=!o.inputWav.empty()?60.0:10.0;
    if(!o.source||!o.target||!o.block||o.block>262144||!(o.duration>0&&o.duration<=maximumDuration)||o.order<1||o.order>8||(o.gpu!=0&&o.gpu!=1))throw std::runtime_error("invalid numeric argument");
    if(o.topology!="auto"&&o.topology!="single"&&o.topology!="cascade")throw std::runtime_error("invalid topology");
    if(o.decode!="memory"&&o.decode!="stream")throw std::runtime_error("invalid decode mode");
    if(o.family!="dsd"&&o.family!="ec"&&o.family!="chebyshev"&&o.family!="ahm")throw std::runtime_error("invalid family");
    if((o.route=="dsd_direct"||o.route=="dsd_native_convert")&&o.gpu)throw std::runtime_error("these production routes do not use GPU; --gpu must be 0");
    if(o.route.rfind("dsd_",0)==0&&o.fixture.empty())throw std::runtime_error("DSD routes require --fixture absolute.dsf");
    if(o.route=="dsd_direct"&&o.source!=o.target)throw std::runtime_error("DirectSDM requires identical bit rates");
    if(o.route=="dsd_native_convert"&&o.source==o.target)throw std::runtime_error("native convert requires different bit rates");
    return o;
}

// SHA256 includes every int32 carrier byte in little-endian order. Native
// streams use channel-major bytes, MSB-first bits, zeroed unused low tail bits.
// All updates happen in validation, outside measured production DSP phases.
class Sha256 {
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    std::vector<UCHAR> object;
    static void check(NTSTATUS s) {if(s<0)throw std::runtime_error("BCrypt SHA256 failed");}
public:
    Sha256() {
        check(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0));
        DWORD n=0,got=0;check(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&n),sizeof(n),&got,0));
        object.resize(n);check(BCryptCreateHash(alg,&hash,object.data(),n,nullptr,0,0));
    }
    ~Sha256(){if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);}
    Sha256(const Sha256&)=delete;Sha256& operator=(const Sha256&)=delete;
    void update(const void* p,size_t n) {
        auto data=static_cast<const UCHAR*>(p);
        while(n){const ULONG take=ULONG(std::min<size_t>(n,0x40000000));check(BCryptHashData(hash,const_cast<PUCHAR>(data),take,0));data+=take;n-=take;}
    }
    std::string digest()const {
        // Duplicate rather than finalize: reading a digest cannot prevent more updates.
        BCRYPT_HASH_HANDLE copy=nullptr;std::vector<UCHAR> obj(object.size());
        check(BCryptDuplicateHash(hash,&copy,obj.data(),ULONG(obj.size()),0));
        std::array<UCHAR,32> bytes{};auto status=BCryptFinishHash(copy,bytes.data(),ULONG(bytes.size()),0);BCryptDestroyHash(copy);check(status);
        static constexpr char hex[]="0123456789abcdef";std::string s;
        for(auto x:bytes){s+=hex[x>>4];s+=hex[x&15];}return s;
    }
};
static void hash_native(Sha256& hash,const a::DsdStream& stream) {
    const size_t full=size_t(stream.bits/8);const unsigned rem=unsigned(stream.bits%8);
    for(const auto& channel:stream.data) {
        if(channel.size()<full+(rem?1:0))throw std::runtime_error("short native output channel");
        hash.update(channel.data(),full);
        if(rem){const uint8_t tail=channel[full]&uint8_t(0xffu<<(8-rem));hash.update(&tail,1);}
    }
}
struct Capture {
    uint64_t frames=0,words=0,markerErrors=0,finiteErrors=0,rangeErrors=0;
    std::vector<int32_t> prefix,suffix;
    Sha256 fullHash;
    static constexpr size_t cap=8192;
    bool dop=false;
    explicit Capture(bool v=false):dop(v) {prefix.reserve(cap);suffix.reserve(cap);}
    void planes(const std::vector<std::vector<double>>& p) {
        for(const auto& ch:p)for(double v:ch)if(!std::isfinite(v))++finiteErrors;
    }
    void consume(const std::vector<int32_t>& v,int channels=2) {
        if(channels<=0||v.size()%size_t(channels))throw std::runtime_error("invalid output dimensions");
        for(size_t i=0;i<v.size();++i) {
            if(v[i]<-8388608||v[i]>8388607)++rangeErrors;
            const unsigned want=((frames+i/size_t(channels))&1)?0xfau:0x05u;
            if(dop&&((uint32_t(v[i])>>16)&255u)!=want)++markerErrors;
        }
        const size_t n=std::min(v.size(),cap-prefix.size());prefix.insert(prefix.end(),v.begin(),v.begin()+n);
        if(v.size()>=cap)suffix.assign(v.end()-cap,v.end());
        else {
            const size_t excess=suffix.size()+v.size()>cap?suffix.size()+v.size()-cap:0;
            suffix.erase(suffix.begin(),suffix.begin()+excess);suffix.insert(suffix.end(),v.begin(),v.end());
        }
        fullHash.update(v.data(),v.size()*sizeof(int32_t));
        frames+=v.size()/size_t(channels);words+=v.size();
    }
    uint64_t checksum()const {
        uint64_t h=1469598103934665603ull;
        for(const auto* v:{&prefix,&suffix})for(int32_t x:*v)h=(h^uint32_t(x))*1099511628211ull;
        return h;
    }
};
static int selftest() {
    Capture c(true);std::vector<int32_t> v;
    for(unsigned i=0;i<5;++i)for(unsigned ch=0;ch<2;++ch) {
        uint32_t x=((i&1)?0xfa0000u:0x050000u)|0x6969u;
        if(x&0x800000)x|=0xff000000u;v.push_back(int32_t(x));
    }
    c.consume({v.begin(),v.begin()+6});c.consume({v.begin()+6,v.end()});
    if(c.frames!=5||c.words!=10||c.markerErrors)return 1;
    v[0]^=1<<16;Capture bad(true);bad.consume(v);
    if(bad.markerErrors!=1)return 1;
    c.planes({{0.0,1.0,std::numeric_limits<double>::quiet_NaN()}});
    if(c.finiteErrors!=1)return 1;
    Sha256 known;known.update("abc",3);
    if(known.digest()!="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")return 1;
    std::vector<int32_t> middle(Capture::cap*3,42);Capture before,after;
    before.consume(middle);middle[Capture::cap+17]^=1;after.consume(middle);
    if(before.prefix!=after.prefix||before.suffix!=after.suffix||before.checksum()!=after.checksum()||before.fullHash.digest()==after.fullHash.digest())return 1;
    Capture segmented;segmented.consume({middle.begin(),middle.begin()+100});segmented.consume({middle.begin()+100,middle.end()});
    if(segmented.fullHash.digest()!=after.fullHash.digest())return 1;
    a::DsdStream native;native.channels=2;native.bits=9;native.data={{0x12,0x80,0xff},{0x34,0x7f,0xff}};
    Sha256 n1,n2,n3;hash_native(n1,native);native.data[0][1]^=0x7f;native.data[1][2]^=0xff;hash_native(n2,native);
    if(n1.digest()!=n2.digest())return 1;
    native.data[0][1]^=0x80;hash_native(n3,native);if(n1.digest()==n3.digest())return 1;
    std::cout<<"SELFTEST full SHA256 known-vector, middle-word negative control (unchanged prefix/suffix), chunk invariance, native tail normalization PASS; ";
    std::cout<<"SELFTEST counters, cross-block DoP phase, bad-marker and NaN negative controls PASS; no performance measurement\n";
    return 0;
}
static a::AudioBuffer pcm_source(unsigned fs,double duration) {
    a::AudioBuffer b;b.rate=fs;b.channels=2;b.bits=24;b.isFloat=false;
    const size_t n=size_t(std::llround(fs*duration));b.raw.resize(n*2);
    for(size_t i=0;i<n;++i) {
        const auto x=int32_t(std::llround(.001*8388608.*std::sin(2*d::kPi*1000.*i/fs)));
        b.raw[2*i]=b.raw[2*i+1]=x;
    }
    return b;
}
static a::AudioBuffer pcm_wav_source(const Options&o) {
    // Same native PCM reader used by read_any(WAV). Loading/validation occurs
    // before GPU probe, Stamp all, and every measured DSP phase.
    auto b=a::read_wav(o.inputWav);
    const size_t expected=size_t(std::llround(o.source*o.duration));
    if(b.channels!=2)throw std::runtime_error("input WAV must have exactly two channels");
    if(b.isFloat||b.bits!=24)throw std::runtime_error("input WAV must be integer PCM24, not float or a different bit depth");
    if(b.rate!=o.source)throw std::runtime_error("input WAV rate differs from --source-rate");
    if(b.frames()!=expected||b.raw.size()!=expected*2)
        throw std::runtime_error("input WAV frame count differs from round(--source-rate * --seconds); no trimming or padding allowed");
    return b;
}
static void ensure_fixture(const Options&o) {
    const uint64_t bits=uint64_t(std::llround(o.source*o.duration));
    if(bits%16)throw std::runtime_error("DSD source duration must yield an integral 16 bits");
    if(std::filesystem::exists(std::filesystem::u8path(o.fixture))) {
        const auto i=a::probe_dsd(o.fixture);
        if(i.rate!=o.source||i.channels!=2||i.bits!=bits)throw std::runtime_error("existing fixture shape differs; use a different fixture path");
        std::ifstream meta(std::filesystem::u8path(o.fixture+".fixturemeta"));
        unsigned rate=0;uint64_t savedBits=0,savedHash=0;
        if(!(meta>>rate>>savedBits>>savedHash)||rate!=o.source||savedBits!=bits)
            throw std::runtime_error("existing fixture lacks matching generated -60dBFS provenance; use a new fixture path");
        const auto s=a::read_dsd(o.fixture);uint64_t hash=1469598103934665603ull;
        for(const auto& ch:s.data)for(uint8_t v:ch)hash=(hash^v)*1099511628211ull;
        if(hash!=savedHash)throw std::runtime_error("fixture content changed since generation");
        return;
    }
    // Same frozen scalar SDM as production. Fixture generation is never timed.
    d::SdmConfig sc;sc.fs=o.source;sc.order=5;sc.family=d::ModulatorFamily::Dsd;
    sc.oneBitOutput=true;sc.ditherLsb=.0005;sc.cutoffHz=100000;
    d::SdmModulator m(sc);a::DsdStream s;s.rate=o.source;s.channels=2;s.bits=bits;
    s.data.assign(2,std::vector<uint8_t>(size_t(bits/8),0));
    for(uint64_t i=0;i<bits;++i)if(m.process(.001*std::sin(2*d::kPi*1000.*double(i)/o.source))>0)
        s.data[0][size_t(i/8)]|=uint8_t(1u<<(7-(i&7)));
    s.data[1]=s.data[0];
    a::write_dsf(o.fixture,s);
    uint64_t hash=1469598103934665603ull;
    for(const auto& ch:s.data)for(uint8_t v:ch)hash=(hash^v)*1099511628211ull;
    std::ofstream meta(std::filesystem::u8path(o.fixture+".fixturemeta"));
    meta<<o.source<<' '<<bits<<' '<<hash<<'\n';
    if(!meta)throw std::runtime_error("fixture provenance write failed");
}
static void gpu_probe(bool enabled) {
    if(!enabled)return;
    // FULL packages put CLI tools one level below the shared backend/runtime.
    // Resolve against our executable, not the user's current directory. Altered
    // search path also resolves the backend's HIP runtime beside that backend.
    const auto exeDir=std::filesystem::path(a::exe_dir_w());
    auto backend=exeDir/L"rtagpu.dll";
    if(!std::filesystem::is_regular_file(backend) &&
       _wcsicmp(exeDir.filename().c_str(),L"tools")==0 &&
       std::filesystem::is_regular_file(exeDir.parent_path()/L"RLPlayer.exe"))
        backend=exeDir.parent_path()/L"rtagpu.dll";
    HMODULE h=std::filesystem::is_regular_file(backend)
        ? LoadLibraryExW(backend.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH)
        : LoadLibraryW(L"rtagpu.dll");
    if(!h)throw std::runtime_error("GPU requested but rtagpu.dll cannot load");
    auto f=reinterpret_cast<int(*)()>(GetProcAddress(h,"rta_gpu_available"));
    if(!f||f()!=1)throw std::runtime_error("GPU constant write/read probe failed");
    // Retain module so subsequent production lookup uses this probed instance.
}
struct Telemetry {
    bool fixedAny=false,fixedAll=true,gpuAny=false,gpuAll=true,cascade=false;
    uint32_t gpuMaskOr=0,gpuMaskAnd=~0u,compactOr=0,compactAnd=~0u;
    int firstT=0,secondT=0;size_t coeffBytes=0,blocks=0;
    double prepareMs=0;
    size_t cpuSimdOutputs=0,cpuParallelThreads=0;
    template<class Stats> void observe(const Stats&s,const e::Pipeline&p) {
        ++blocks;fixedAny|=s.usedFixed;fixedAll&=s.usedFixed;
        gpuAny|=s.usedGpuResample;gpuAll&=s.gpuAllResampleStagesUsed;
        gpuMaskOr|=s.gpuResampleStageMask;gpuMaskAnd&=s.gpuResampleStageMask;
        compactOr|=s.gpuCompactResampleStageMask;compactAnd&=s.gpuCompactResampleStageMask;
        cascade=p.twoStageActive();firstT=s.tapsPerPhase;secondT=s.stage2TapsPerPhase;
        coeffBytes=s.coeffBytes;if(blocks==1)prepareMs=s.prepareMs;
        if constexpr(requires {s.cpuSimdOutputs;s.cpuParallelThreads;}) {
            cpuSimdOutputs+=s.cpuSimdOutputs;
            cpuParallelThreads=std::max(cpuParallelThreads,s.cpuParallelThreads);
        }
    }
};
// Same harness source supports old headers with no diagnostic API.
template<class Modulator>
static std::array<uint64_t, 2> sdm_stereo_diagnostics() {
    if constexpr (requires { Modulator::stereo_simd_frames_for_diagnostics();
                             Modulator::stereo_simd_dither_frames_for_diagnostics(); }) {
        return {Modulator::stereo_simd_frames_for_diagnostics(),
                Modulator::stereo_simd_dither_frames_for_diagnostics()};
    }
    return {0, 0};
}
struct Result {
    Meter init,scan,first,steady,eof,validation,harness;
    Meter diagPipeline,diagModpack;
    Capture output;
    Telemetry telemetry;
    uint64_t inputFrames=0,inputBits=0,rawFrames=0,expectedFrames=0,drainRequested=0,drained=0,dropped=0,dsdBits=0;
    int effective=0;double gainDb=0,decodeRate=0;
    std::string stepDownReason;
    long long overload=0,clamped=0,nonfinite=0;
    uint64_t nativeChecksum=0;
    Sha256 nativeHash;
    uint64_t nativeMinimumBits=0,nativeMaximumBits=0;
    bool nativeTargetRetained=false,countOk=false;
    double asyncWorkerWall=0;
    explicit Result(bool dop):output(dop){}
};

static void pipeline_route(const Options&o,a::AudioBuffer input,Result&r) {
    const bool remod=o.route=="dsd_decode_remod",dop=o.route!="pcm_pcm";
    a::DsdPcmStream decoder;
    unsigned fs=input.rate;size_t total=input.frames();
    Stamp start;
    if(remod) {
        if(o.decode=="stream") {
            std::string why;
            if(!decoder.open(o.fixture,a::DsdPcmSettings{},&why))throw std::runtime_error(why);
            fs=decoder.rate();total=decoder.totalFrames();
        } else {input=a::read_dsd_native(o.fixture,nullptr);fs=input.rate;total=input.frames();}
        r.decodeRate=fs;
    }
    const auto* preset=d::find_preset(o.preset);if(!preset)throw std::runtime_error("unknown preset");
    e::PipelineConfig cfg;e::apply_filter_preset(cfg,*preset);
    cfg.autoTaps=true;cfg.outRate=o.target;cfg.outBits=24;cfg.useFixed=true;cfg.gpuResample=o.gpu!=0;
    cfg.sourceBits=remod?32:24;
    e::apply_resample_purpose(cfg,dop?d::ResamplePurpose::Dsd:d::ResamplePurpose::Pcm);
    if(o.topology!="auto")cfg.twoStage=o.topology=="cascade";
    if(dop)cfg.shaper=d::Shaper::None;
    // GUI stream mode uses conservative full-scale headroom even for quiet input.
    // Memory PCM/source is -60dBFS; validate its decoded peak below before use.
    if(remod&&o.decode=="stream")r.gainDb=d::sdm_headroom_db_for_fs(1.0,o.target);
    cfg.preAttenDb=r.gainDb;
    e::Pipeline pipe(cfg);d::DopModulationStream stream;
    std::vector<std::unique_ptr<d::SdmModulator>> mods;
#if RTA_BENCH_ASYNC
    d::DopAsyncStream async(mods);
#endif
    if(dop) {
        d::SdmConfig mc;mc.fs=o.target;mc.family=d::parse_modulator_family(o.family);
        mc.order=o.order;mc.levels=2;mc.oneBitOutput=true;mc.cutoffHz=100000;
        mc.ditherLsb=.0005;mc.oobGainDb=4.5;mc.ahmBits=8;
        for(int c=0;c<2;++c)mods.push_back(std::make_unique<d::SdmModulator>(mc));
        r.effective=mods[0]->effectiveOrder();
        r.stepDownReason=mods[0]->stepDownReason();
    }
    r.init.add(start);
    // GUI does a separate ISP/source-peak scan before playback. That scanner is
    // private to rtaweb; this component benchmark excludes it explicitly rather
    // than substitute a different scan and call that a GUI startup measurement.
    if(!(remod&&o.decode=="stream")) {
        Stamp valid;
        double peak=0;for(size_t i=0;i<input.frames();++i)for(int c=0;c<2;++c)peak=std::max(peak,std::fabs(input.sample(i,c)));
        if(dop&&d::sdm_headroom_db_for_fs(peak,o.target)<0)throw std::runtime_error("fixture needs headroom: memory route benchmark expects quiet source");
        r.validation.add(valid);
    }
    r.inputFrames=total;size_t skip=0,delay=0,natural=0,ir=0;
    auto capture=[&](const std::vector<int32_t>& words){Stamp v;r.output.consume(words);r.validation.add(v);};
    auto flushAsync=[&](Meter& meter) {
#if RTA_BENCH_ASYNC
        if(o.overlap && async.pending()) {
            Stamp wait;auto result=async.take();meter.add(wait);
            r.asyncWorkerWall+=result.wallSeconds;capture(result.words);
        }
#endif
    };
    auto emit=[&](a::AudioBuffer& out,std::vector<std::vector<double>>& planes,bool finish,Meter& meter) {
        if(dop) {
            {Stamp v;r.output.planes(planes);r.validation.add(v);}
            Stamp t;std::vector<int32_t> packed;std::string why;
            const size_t drop=std::min(skip,planes.empty()?size_t(0):planes[0].size());skip-=drop;r.dropped+=drop;
#if RTA_BENCH_ASYNC
            if(o.overlap) {
                // End this segment before waiting/capturing the previous job;
                // validation may overlap, so only harness time is primary here.
                meter.add(t);
                flushAsync(meter);
                Stamp submit;async.submit(std::move(planes),drop,finish);meter.add(submit);
                return;
            }
#endif
            if(!stream.process(planes,mods,drop,finish,packed,&why))throw std::runtime_error(why);
            meter.add(t,&r.diagModpack);capture(packed);
        } else {
            Stamp t;const size_t drop=std::min(skip,out.frames());skip-=drop;r.dropped+=drop;
            out.raw.erase(out.raw.begin(),out.raw.begin()+drop*2);meter.add(t,&r.diagModpack);capture(out.raw);
        }
    };
    bool first=true;
    for(size_t pos=0;pos<total;) {
        Meter& meter=first?r.first:r.steady;Stamp t;
        a::AudioBuffer b;
        const size_t n=std::min(o.block,total-pos);
        if(remod&&o.decode=="stream") {
            if(decoder.read(pos,n,b)!=n)throw std::runtime_error("stream decoder short read");
        } else {
            b.rate=fs;b.bits=input.bits;b.channels=2;b.isFloat=input.isFloat;
            b.raw.assign(input.raw.begin()+pos*2,input.raw.begin()+(pos+n)*2);
        }
        std::vector<std::vector<double>> planes;e::PipelineStats st;
        Stamp diag;
        auto out=pipe.process(b,&st,dop?&planes:nullptr);
        r.diagPipeline.add(diag);
        meter.add(t);r.telemetry.observe(st,pipe);
        if(first) {
            delay=size_t(std::llround(pipe.algorithmic_latency_out_frames()));
            natural=size_t(std::ceil(pipe.resampler_tail_out_frames()));
            ir=size_t(std::llround(pipe.convolution_tail_out_frames()));skip=delay;
        }
        r.rawFrames+=dop?planes[0].size():out.frames();
        emit(out,planes,false,meter);pos+=n;first=false;
    }
    r.drainRequested=delay+natural+ir;
    while(r.drained<r.drainRequested) {
        Stamp t;std::vector<std::vector<double>> planes;
        Stamp diag;
        auto out=pipe.drain_chunk(o.block,dop?&planes:nullptr);
        r.diagPipeline.add(diag);
        const size_t got=dop?(planes.empty()?0:planes[0].size()):out.frames();
        if(!got)throw std::runtime_error("drain ended before declared production tail");
        const size_t take=std::min<uint64_t>(got,r.drainRequested-r.drained);
        if(dop)for(auto& v:planes)v.resize(take);else out.raw.resize(take*2);
        r.eof.add(t);emit(out,planes,false,r.eof);r.drained+=take;
    }
    if(dop){a::AudioBuffer empty;std::vector<std::vector<double>> planes;emit(empty,planes,true,r.eof);}
    flushAsync(r.eof);
    const uint64_t logical=r.rawFrames+r.drained-r.dropped;
    r.expectedFrames=dop?(logical+15)/16:logical;
    const uint64_t nominalNumerator=uint64_t(total)*o.target;
    const bool rawCountOk=nominalNumerator%fs==0&&r.rawFrames==nominalNumerator/fs;
    r.countOk=rawCountOk&&r.output.frames==r.expectedFrames&&r.drained==r.drainRequested&&skip==0;
    for(const auto&m:mods){r.overload+=m->overload();r.clamped+=m->clamped();r.nonfinite+=m->nonFinite();}
}

static void direct_route(const Options&o,Result&r) {
    Stamp t;auto source=a::read_dsd(o.fixture);r.init.add(t);r.inputBits=source.bits;
    d::DopPackState state;
    const size_t chunk=1u<<18;
    for(uint64_t pos=0;pos<source.bits;pos+=chunk) {
        Meter& meter=pos?r.steady:r.first;Stamp block;
        a::DsdStream s;s.rate=source.rate;s.channels=2;s.bits=std::min<uint64_t>(chunk,source.bits-pos);s.data.resize(2);
        for(int c=0;c<2;++c)s.data[c].assign(source.data[c].begin()+pos/8,source.data[c].begin()+(pos+s.bits)/8);
        std::vector<int32_t> words;Stamp diag;d::dsd_to_dop(s,words,state);r.diagModpack.add(diag);meter.add(block);
        Stamp v;r.output.consume(words);
        // Full independent payload witness for this route, outside timed phases.
        for(size_t i=0;i<words.size()/2;++i)for(int c=0;c<2;++c) {
            const unsigned payload=unsigned(source.data[c][size_t(pos/8)+i*2])<<8|source.data[c][size_t(pos/8)+i*2+1];
            if((uint32_t(words[i*2+c])&65535u)!=payload)throw std::runtime_error("DirectSDM payload changed");
        }
        r.validation.add(v);
    }
    r.expectedFrames=source.bits/16;r.countOk=r.output.frames==r.expectedFrames;
}
static void native_route(const Options&o,Result&r) {
    Stamp t;auto source=a::read_dsd(o.fixture);r.inputBits=source.bits;
    const auto plan=d::plan_dsd_rate(o.source,o.target);
    if(!plan.needs_dsp())throw std::runtime_error("native convert plan not supported");
    d::DsdRateConvertConfig cfg;cfg.modFamily=d::parse_modulator_family(o.family);cfg.modOrder=o.order;
    d::DsdRateConverter conv(plan,cfg,2);r.init.add(t);
    {Stamp scan;const double peak=d::dsd_convert_measure_peak(source,plan,cfg);
     conv.set_pre_atten_db(d::sdm_headroom_db_for_fs(peak,o.target));r.gainDb=conv.gain_db();r.scan.add(scan);}
    a::DsdStream out;out.rate=o.target;out.channels=2;out.bits=0;out.data.resize(2);
    const size_t chunk=1u<<18;
    for(uint64_t pos=0;pos<source.bits;pos+=chunk) {
        Meter& meter=pos?r.steady:r.first;Stamp block;
        a::DsdStream s;s.rate=source.rate;s.channels=2;s.bits=std::min<uint64_t>(chunk,source.bits-pos);s.data.resize(2);
        for(int c=0;c<2;++c)s.data[c].assign(source.data[c].begin()+pos/8,source.data[c].begin()+(pos+s.bits+7)/8);
        conv.process_bits(s,out);meter.add(block);
    }
    {Stamp end;conv.finish(out);r.eof.add(end);}
    const auto st=conv.stats();r.effective=st.effectiveOrder;r.overload=st.overload;r.clamped=st.clamped;r.nonfinite=st.nonFinite;
    r.stepDownReason=st.stepDownReason;
    r.decodeRate=plan.midRate;r.dsdBits=out.bits;r.nativeTargetRetained=true;
    Stamp valid;
    const uint64_t nominalBits=source.bits*uint64_t(o.target)/o.source;
    // Independent rate/support bound: do not merely trust the converter's own
    // statistics. No second audio conversion is run. Obtain finite support from
    // a separately designed public decimator and the actual interpolation FIR.
    const auto ic=d::conversion_integrator(cfg.decimator,cfg.conversion,o.source,plan.midRate,cfg.decimatorAttenDb,cfg.twoStageDecimator);
    d::DsdDecimator countReference(ic);
    std::vector<double> interp;conv.interp_prototype_into(interp);
    const uint64_t L=uint64_t(plan.interpolation);
    const uint64_t settle=uint64_t(std::ceil(.002*o.target));
    const uint64_t decTailOut=uint64_t(std::ceil(double(countReference.tailInputBits())*o.target/o.source));
    const uint64_t interpDrain=interp.empty()?0:((interp.size()-1+L-1)/L)*L;
    r.nativeMinimumBits=nominalBits;
    // Two interpolation strides cover phase/ceil rounding, not an arbitrary
    // tail allowance. Head-delay trimming can only reduce this upper bound.
    r.nativeMaximumBits=nominalBits+decTailOut+interpDrain+settle+2*L;
    r.countOk=out.rate==o.target&&out.channels==2&&out.bits==uint64_t(st.outBits)
              &&out.bits>=r.nativeMinimumBits&&out.bits<=r.nativeMaximumBits;
    for(const auto& v:out.data)r.countOk&=v.size()>=(out.bits+7)/8;
    // Native output is packed 1-bit, not DoP; marker checks are inapplicable.
    uint64_t h=1469598103934665603ull;
    for(const auto& v:out.data) {
        const size_t n=std::min<size_t>(v.size(),8192);
        for(size_t i=0;i<n;++i)h=(h^v[i])*1099511628211ull;
        for(size_t i=v.size()-n;i<v.size();++i)h=(h^v[i])*1099511628211ull;
    }
    r.nativeChecksum=h;hash_native(r.nativeHash,out);r.validation.add(valid);
}
static std::string json_string(const std::string& s) {
    std::string out;
    for(unsigned char c:s) {
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";
        else if(c<32)out+=' ';else out+=char(c);
    }
    return out;
}
int main(int argc,char**argv)try {
    const auto o=parse(argc,argv);if(o.selftest)return selftest();
    const bool dsdSource=o.route.rfind("dsd_",0)==0;
    if(dsdSource)ensure_fixture(o);
    auto input=dsdSource?a::AudioBuffer{}:(o.inputWav.empty()?pcm_source(o.source,o.duration):pcm_wav_source(o));
    // GPU probe, generated source, and optional native PCM WAV decoding excluded.
    gpu_probe(o.gpu!=0);
    Result r(o.route=="pcm_dsd"||o.route=="dsd_direct"||o.route=="dsd_decode_remod");
    Stamp all;
    if(o.route=="dsd_direct")direct_route(o,r);
    else if(o.route=="dsd_native_convert")native_route(o,r);
    else pipeline_route(o,std::move(input),r);
    r.harness.add(all);
    PROCESS_MEMORY_COUNTERS pm{};pm.cb=sizeof(pm);
    if(!GetProcessMemoryInfo(GetCurrentProcess(),&pm,sizeof(pm)))throw std::runtime_error("GetProcessMemoryInfo failed");
    const double wall=r.init.wall+r.scan.wall+r.first.wall+r.steady.wall+r.eof.wall;
    const double cpu=r.init.cpu+r.scan.cpu+r.first.cpu+r.steady.cpu+r.eof.cpu;
    const bool ok=r.countOk&&!r.output.markerErrors&&!r.output.finiteErrors&&!r.output.rangeErrors&&!r.nonfinite;
    const auto sdmStereo = sdm_stereo_diagnostics<d::SdmModulator>();
    std::cout<<std::setprecision(12)<<"{\"schema\":2,\"route\":\""<<o.route<<"\",\"source_rate\":"<<o.source<<",\"target_rate\":"<<o.target
      <<",\"channels\":2,\"generated_baseband_level_dbfs\":"<<(o.inputWav.empty()?"-60":"null")<<",\"source_seconds\":"<<o.duration<<",\"block_frames\":"<<o.block
      <<",\"input_wav\":\""<<json_string(o.inputWav)<<"\",\"input_wav_decode_in_dsp_timing\":false"
      <<",\"preset_requested\":\""<<o.preset<<"\",\"preset_applies\":"<<((o.route=="dsd_direct"||o.route=="dsd_native_convert")?"false":"true")
      <<",\"family\":\""<<(r.effective?o.family:"none")<<"\",\"requested_order\":"<<(r.effective?o.order:0)<<",\"effective_order\":"<<r.effective
      <<",\"effective_levels\":"<<(r.effective?2:0)<<",\"step_down_reason\":\""<<json_string(r.stepDownReason)<<"\""
      <<",\"sdm_osr_20k\":"<<(r.effective?double(o.target)/40000:0)<<",\"sdm_dither_lsb\":0.0005,\"sdm_cutoff_hz\":100000,\"sdm_oob_gain_db\":4.5,\"sdm_ntf_hinf\":1.35"
      <<",\"decode_mode\":\""<<o.decode<<"\",\"decode_rate\":"<<r.decodeRate<<",\"headroom_db\":"<<r.gainDb
      <<",\"gui_isp_prescan_included\":false,\"device_and_decoder_framework_excluded\":true,\"input_file_io_included_for_dsd\":"<<(dsdSource?"true":"false")
      <<",\"finite_check_scope\":\"all_returned_double_SDM_planes_and_SDM_nonfinite_counters;all_final_int24_words_range_checked\""
      <<",\"native_decimator_atten_db\":"<<(o.route=="dsd_native_convert"?120:0)<<",\"native_interpolator_atten_db\":"<<(o.route=="dsd_native_convert"?180:0)
      <<",\"topology_requested\":\""<<o.topology<<"\",\"cascade_active\":"<<(r.telemetry.cascade?"true":"false")
      <<",\"gpu_requested\":"<<o.gpu<<",\"actual_fixed_any\":"<<(r.telemetry.fixedAny?"true":"false")
      <<",\"actual_fixed_all\":"<<(r.telemetry.blocks&&r.telemetry.fixedAll?"true":"false")<<",\"actual_gpu_any\":"<<(r.telemetry.gpuAny?"true":"false")
      <<",\"actual_gpu_all_blocks_all_stages\":"<<(r.telemetry.blocks&&r.telemetry.gpuAll?"true":"false")
      <<",\"cpu_simd_outputs\":"<<r.telemetry.cpuSimdOutputs<<",\"cpu_parallel_threads_max\":"<<r.telemetry.cpuParallelThreads
      <<",\"sdm_stereo_simd_frames\":"<<sdmStereo[0]
      <<",\"sdm_stereo_dither_simd_frames\":"<<sdmStereo[1]
      <<",\"gpu_mask_or\":"<<r.telemetry.gpuMaskOr<<",\"gpu_mask_and\":"<<(r.telemetry.blocks?r.telemetry.gpuMaskAnd:0)
      <<",\"compact_mask_or\":"<<r.telemetry.compactOr<<",\"compact_mask_and\":"<<(r.telemetry.blocks?r.telemetry.compactAnd:0)
      <<",\"T1\":"<<r.telemetry.firstT<<",\"T2\":"<<r.telemetry.secondT<<",\"logical_coeff_bytes\":"<<r.telemetry.coeffBytes
      <<",\"first_pipeline_prepare_ms\":"<<r.telemetry.prepareMs<<",\"pipeline_blocks\":"<<r.telemetry.blocks;
    auto meter=[&](const char* name,const Meter&m){std::cout<<",\""<<name<<"_wall_s\":"<<m.wall<<",\""<<name<<"_cpu_s\":"<<m.cpu;};
    meter("init",r.init);meter("native_peak_scan",r.scan);meter("first_block",r.first);meter("steady",r.steady);meter("eof",r.eof);meter("validation",r.validation);meter("harness",r.harness);meter("diag_pipeline",r.diagPipeline);meter("diag_modpack",r.diagModpack);
    std::cout<<",\"overlap\":"<<o.overlap<<",\"async_worker_wall_s\":"<<r.asyncWorkerWall
      <<",\"end_to_end_realtime_factor\":"<<o.duration/r.harness.wall
      <<",\"harness_average_cpu_cores\":"<<r.harness.cpu/r.harness.wall
      <<",\"harness_cpu_seconds_per_audio_second\":"<<r.harness.cpu/o.duration
      <<",\"primary_elapsed_scope\":\"harness_including_validation_and_all_async_completion;do_not_subtract_validation_when_overlapped\"";
    std::cout<<",\"diag_scope\":\"pipeline.process/drain_only;modpack=stream.process_or_PCM_head_trim_or_direct_DoP;synchronous_subsets_only_async_worker_wall_overlaps_pipeline_do_not_sum;native_converter_unsplit\"";
    std::cout<<",\"measured_phase_wall_s\":"<<wall<<",\"measured_phase_cpu_s\":"<<cpu<<",\"phase_realtime_factor\":"<<o.duration/wall
      <<",\"average_cpu_cores\":"<<(wall>0?cpu/wall:0)<<",\"logical_processors\":"<<GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
      <<",\"input_frames\":"<<r.inputFrames<<",\"input_bits\":"<<r.inputBits<<",\"raw_pipeline_frames\":"<<r.rawFrames
      <<",\"drain_requested\":"<<r.drainRequested<<",\"drained\":"<<r.drained<<",\"head_trimmed\":"<<r.dropped
      <<",\"expected_output_frames\":"<<r.expectedFrames<<",\"output_frames\":"<<r.output.frames<<",\"output_native_bits\":"<<r.dsdBits
      <<",\"native_independent_min_bits\":"<<r.nativeMinimumBits<<",\"native_independent_support_max_bits\":"<<r.nativeMaximumBits
      <<",\"marker_errors\":"<<r.output.markerErrors<<",\"finite_errors\":"<<r.output.finiteErrors<<",\"integer_24bit_range_errors\":"<<r.output.rangeErrors<<",\"sdm_nonfinite\":"<<r.nonfinite
      <<",\"sdm_overload\":"<<r.overload<<",\"sdm_clamped\":"<<r.clamped<<",\"bounded_prefix_suffix_checksum\":\""<<std::hex<<(r.nativeTargetRetained?r.nativeChecksum:r.output.checksum())<<std::dec
      <<"\",\"full_output_checksum\":true,\"full_output_sha256\":\""<<(r.nativeTargetRetained?r.nativeHash.digest():r.output.fullHash.digest())<<"\",\"hash_encoding\":\"int32_le_interleaved_or_native_channel_major_msb_first_unused_low_bits_zero\",\"native_whole_target_retained\":"<<(r.nativeTargetRetained?"true":"false")
      <<",\"peak_working_set_bytes\":"<<pm.PeakWorkingSetSize<<",\"memory_scope\":\"tool_process_including_fixture_generation_not_player_RAM\",\"count_ok\":"<<(r.countOk?"true":"false")
      <<",\"spectral_quality_test\":false,\"cpu_gpu_checksum_equality_not_assumed\":true,\"structural_ok\":"<<(ok?"true":"false")<<"}\n";
    return ok?0:1;
}catch(const std::exception& ex){std::cerr<<"FAIL "<<ex.what()<<"\n";return 2;}
