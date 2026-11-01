// Full modulation+checked DoP component A/B. No resampler/GPU/audio device.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "dsp/dop_stream.h"
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <bit>
#include <intrin.h>
using namespace rta::dsp;
// Snapshot of the current production stream; the only mutation is replacing
// its scalar process loop with process_block. State/padding/parallelism match.
class DopScalarBenchmarkStream {
public:
    size_t pending_bits() const { return state_.empty() ? 0 : state_[0].count; }

    // A seek discards old audio bits, but the receiver's marker phase continues.
    void discard_pending() {
        for (auto& s : state_) { s.bits = 0; s.count = 0; }
    }

    // drop samples still warm the modulator, as in the existing seek preroll.
    // finish pads a partial carrier with the same 0x69 silence bit phase as
    // the former double-buffer implementation; it does not run the SDM again.
    // Invalid dimensions or a multilevel SDM are rejected before consuming
    // input or changing out/state. An internal exception aborts this stream.
    bool process(const std::vector<std::vector<double>>& planes,
                 const std::vector<std::unique_ptr<SdmModulator>>& mods,
                 size_t drop, bool finish, std::vector<int32_t>& out,
                 std::string* why = nullptr) {
        auto reject = [&](const char* reason) {
            if (why) *why = reason;
            return false;
        };
        const size_t channels = mods.size();
        if (!channels) return reject("DoP modulation needs at least one channel");
        if (!planes.empty() && planes.size() != channels)
            return reject("DoP input channel count differs from the modulators");
        const size_t frames = planes.empty() ? 0 : planes[0].size();
        if (drop > frames) return reject("DoP preroll exceeds the input block");
        for (size_t c = 0; c < channels; ++c) {
            if (!mods[c] || mods[c]->levels() != 2)
                return reject("DoP requires a 1-bit modulator; multilevel output is rejected");
            if (!planes.empty() && planes[c].size() != frames)
                return reject("DoP input channel lengths differ");
        }
        if (!state_.empty() && state_.size() != channels)
            return reject("DoP channel count cannot change within a stream");
        const size_t pending = pending_bits();
        if (frames - drop > std::numeric_limits<size_t>::max() - pending - 15)
            return reject("DoP frame count overflows");
        const size_t total = pending + frames - drop;
        const size_t carrier = (total + (finish ? 15 : 0)) / 16;
        if (carrier > std::numeric_limits<size_t>::max() / channels)
            return reject("DoP output size overflows");
        if (state_.empty()) state_.resize(channels);
        words_.resize(channels);
        for (auto& v : words_) v.resize(carrier);

        // Independent channel buffers avoid two cores repeatedly invalidating
        // the same interleaved cache line. They hold one word per 16 bits.
        ParallelFor::instance().run(channels, [&](size_t c) {
            auto s = state_[c];
            size_t written = 0;
            auto append = [&](bool positive) {
                s.bits = (s.bits << 1) | uint32_t(positive);
                if (++s.count != 16) return;
                uint32_t word = (uint32_t(s.phaseA ? DOP_MARKER_A : DOP_MARKER_B) << 16)
                              | (s.bits & 0xffffu);
                if (word & 0x800000u) word |= 0xff000000u;
                words_[c][written++] = static_cast<int32_t>(word);
                s.phaseA = !s.phaseA;
                s.bits = 0;
                s.count = 0;
            };
            for (size_t i = 0; i < frames; ++i) {
                const double bit = mods[c]->process(planes[c][i]);
                // Keep the binary contract at the actual transport boundary.
                if (bit != 1.0 && bit != -1.0)
                    throw std::runtime_error("SDM violated the 1-bit DoP output contract");
                if (i >= drop) append(bit > 0.0);
            }
            if (finish && s.count) {
                const unsigned left = 16 - s.count;
                const unsigned start = s.count;
                for (unsigned i = 0; i < left; ++i)
                    append(((0x69u >> (7 - ((start + i) & 7))) & 1u) != 0);
            }
            if (written != carrier)
                throw std::runtime_error("DoP streaming frame count invariant failed");
            state_[c] = s;
        });
        out.resize(carrier * channels);
        for (size_t c = 0; c < channels; ++c)
            for (size_t n = 0; n < carrier; ++n)
                out[n * channels + c] = words_[c][n];
        return true;
    }

private:
    struct State { uint32_t bits = 0; unsigned count = 0; bool phaseA = true; };
    std::vector<State> state_;
    std::vector<std::vector<int32_t>> words_;
};

class DopBlockBenchmarkStream {
public:
    size_t pending_bits() const { return state_.empty() ? 0 : state_[0].count; }

    // A seek discards old audio bits, but the receiver's marker phase continues.
    void discard_pending() {
        for (auto& s : state_) { s.bits = 0; s.count = 0; }
    }

    // drop samples still warm the modulator, as in the existing seek preroll.
    // finish pads a partial carrier with the same 0x69 silence bit phase as
    // the former double-buffer implementation; it does not run the SDM again.
    // Invalid dimensions or a multilevel SDM are rejected before consuming
    // input or changing out/state. An internal exception aborts this stream.
    bool process(const std::vector<std::vector<double>>& planes,
                 const std::vector<std::unique_ptr<SdmModulator>>& mods,
                 size_t drop, bool finish, std::vector<int32_t>& out,
                 std::string* why = nullptr) {
        auto reject = [&](const char* reason) {
            if (why) *why = reason;
            return false;
        };
        const size_t channels = mods.size();
        if (!channels) return reject("DoP modulation needs at least one channel");
        if (!planes.empty() && planes.size() != channels)
            return reject("DoP input channel count differs from the modulators");
        const size_t frames = planes.empty() ? 0 : planes[0].size();
        if (drop > frames) return reject("DoP preroll exceeds the input block");
        for (size_t c = 0; c < channels; ++c) {
            if (!mods[c] || mods[c]->levels() != 2)
                return reject("DoP requires a 1-bit modulator; multilevel output is rejected");
            if (!planes.empty() && planes[c].size() != frames)
                return reject("DoP input channel lengths differ");
        }
        if (!state_.empty() && state_.size() != channels)
            return reject("DoP channel count cannot change within a stream");
        const size_t pending = pending_bits();
        if (frames - drop > std::numeric_limits<size_t>::max() - pending - 15)
            return reject("DoP frame count overflows");
        const size_t total = pending + frames - drop;
        const size_t carrier = (total + (finish ? 15 : 0)) / 16;
        if (carrier > std::numeric_limits<size_t>::max() / channels)
            return reject("DoP output size overflows");
        if (state_.empty()) state_.resize(channels);
        words_.resize(channels);
        for (auto& v : words_) v.resize(carrier);

        // Independent channel buffers avoid two cores repeatedly invalidating
        // the same interleaved cache line. They hold one word per 16 bits.
        ParallelFor::instance().run(channels, [&](size_t c) {
            auto s = state_[c];
            size_t written = 0;
            auto append = [&](bool positive) {
                s.bits = (s.bits << 1) | uint32_t(positive);
                if (++s.count != 16) return;
                uint32_t word = (uint32_t(s.phaseA ? DOP_MARKER_A : DOP_MARKER_B) << 16)
                              | (s.bits & 0xffffu);
                if (word & 0x800000u) word |= 0xff000000u;
                words_[c][written++] = static_cast<int32_t>(word);
                s.phaseA = !s.phaseA;
                s.bits = 0;
                s.count = 0;
            };
            mods[c]->process_block(frames ? planes[c].data() : nullptr, frames, [&](size_t i, double bit) {
                // Keep the binary contract at the actual transport boundary.
                if (bit != 1.0 && bit != -1.0)
                    throw std::runtime_error("SDM violated the 1-bit DoP output contract");
                if (i >= drop) append(bit > 0.0);
            });
            if (finish && s.count) {
                const unsigned left = 16 - s.count;
                const unsigned start = s.count;
                for (unsigned i = 0; i < left; ++i)
                    append(((0x69u >> (7 - ((start + i) & 7))) & 1u) != 0);
            }
            if (written != carrier)
                throw std::runtime_error("DoP streaming frame count invariant failed");
            state_[c] = s;
        });
        out.resize(carrier * channels);
        for (size_t c = 0; c < channels; ++c)
            for (size_t n = 0; n < carrier; ++n)
                out[n * channels + c] = words_[c][n];
        return true;
    }

private:
    struct State { uint32_t bits = 0; unsigned count = 0; bool phaseA = true; };
    std::vector<State> state_;
    std::vector<std::vector<int32_t>> words_;
};

struct Trial {
    std::vector<int32_t> words;
    double seconds=0,tscPerSample=0;
    long long clamp=0,overload=0,nonfinite=0;
    int effective=0;
};
using Block=std::vector<std::vector<double>>;
static std::string sha256(const std::vector<int32_t>& words) {
    BCRYPT_ALG_HANDLE alg=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)
        throw std::runtime_error("BCrypt SHA256 open failed");
    std::array<unsigned char,32> hash{};
    const size_t bytes=words.size()*sizeof(int32_t);
    if(bytes>0xffffffffu)throw std::runtime_error("SHA256 input exceeds this benchmark's 4GiB limit");
    NTSTATUS status=BCryptHash(alg,nullptr,0,(PUCHAR)words.data(),(ULONG)bytes,hash.data(),32);
    BCryptCloseAlgorithmProvider(alg,0);
    if(status<0)throw std::runtime_error("BCrypt SHA256 failed");
    std::ostringstream text;text<<std::hex<<std::setfill('0');
    for(auto byte:hash)text<<std::setw(2)<<(unsigned)byte;
    return text.str();
}
static Trial run(int mode,const std::vector<Block>& blocks,size_t frames,const SdmConfig& cfg) {
    constexpr size_t channels=2;
    std::vector<std::unique_ptr<SdmModulator>> mods;
    for(size_t c=0;c<channels;++c)mods.push_back(std::make_unique<SdmModulator>(cfg));
    DopScalarBenchmarkStream scalarStream;DopBlockBenchmarkStream blockStream;
    DopPackState legacyState;
    std::vector<std::vector<double>> legacyBits(channels),pending(channels);
    std::vector<int32_t> chunk;
    Trial result;result.words.reserve(((frames+15)/16)*channels);
    unsigned aux=0;
    const auto start=std::chrono::steady_clock::now();
    _mm_mfence();_mm_lfence();uint64_t before=__rdtscp(&aux);_mm_lfence();
    for(size_t n=0;n<blocks.size();++n) {
        const auto& block=blocks[n];const size_t count=block[0].size();
        const bool finish=n+1==blocks.size();std::string why;
        if(mode==0) {
            // Former representation: one double per DSD bit, separate binary
            // validation, then checked pack. Parallelism matches both streams.
            ParallelFor::instance().run(channels,[&](size_t c){
                const size_t prefix=pending[c].size();
                legacyBits[c].resize(prefix+count);
                std::copy(pending[c].begin(),pending[c].end(),legacyBits[c].begin());
                for(size_t i=0;i<count;++i)legacyBits[c][prefix+i]=mods[c]->process(block[c][i]);
            });
            size_t total=legacyBits[0].size();
            if(finish&&total%16) {
                const unsigned remainder=(unsigned)(total%16);
                for(size_t c=0;c<channels;++c)
                    for(unsigned i=0;i<16-remainder;++i)
                        legacyBits[c].push_back(((0x69u>>(7-((remainder+i)&7)))&1u)?1.:-1.);
                total=legacyBits[0].size();
            }
            const size_t packed=total/16*16;
            const double* ptrs[]={legacyBits[0].data(),legacyBits[1].data()};
            if(!dop_pack_checked(ptrs,2,packed,chunk,legacyState,&why))throw std::runtime_error(why);
            for(size_t c=0;c<channels;++c)
                pending[c].assign(legacyBits[c].begin()+packed,legacyBits[c].end());
        } else if(mode==1) {
            if(!scalarStream.process(block,mods,0,finish,chunk,&why))throw std::runtime_error(why);
        } else {
            if(!blockStream.process(block,mods,0,finish,chunk,&why))throw std::runtime_error(why);
        }
        result.words.insert(result.words.end(),chunk.begin(),chunk.end());
    }
    _mm_mfence();_mm_lfence();uint64_t after=__rdtscp(&aux);_mm_lfence();
    result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    result.tscPerSample=(double)(after-before)/(frames*channels);
    for(auto&mod:mods) {
        result.clamp+=mod->clamped();result.overload+=mod->overload();result.nonfinite+=mod->nonFinite();
        result.effective=mod->effectiveOrder();
    }
    return result;
}
int main(int argc,char**argv)try {
    double seconds=.25,dither=0.;int pairs=5,order=5;std::string family="dsd";bool verify=false;
    for(int i=1;i<argc;++i) {
        std::string arg=argv[i];
        if(arg=="--verify-only")verify=true;
        else if(arg=="--seconds"&&i+1<argc)seconds=std::stod(argv[++i]);
        else if(arg=="--pairs"&&i+1<argc)pairs=std::stoi(argv[++i]);
        else if(arg=="--order"&&i+1<argc)order=std::stoi(argv[++i]);
        else if(arg=="--family"&&i+1<argc)family=argv[++i];
        else if(arg=="--dither"&&i+1<argc)dither=std::stod(argv[++i]);
        else throw std::runtime_error("invalid argument");
    }
    if(!(seconds>0&&seconds<=4)||pairs<5||!(pairs&1)||order<1||order>8||!(dither>=0&&dither<=1))
        throw std::runtime_error("seconds (0,4], odd pairs>=5, order 1..8, dither [0,1] required");
    int cpu[4];__cpuid(cpu,(int)0x80000000u);
    if((unsigned)cpu[0]<0x80000001u)throw std::runtime_error("RDTSCP is not supported");
    __cpuid(cpu,(int)0x80000001u);
    if(!(cpu[3]&(1<<27)))throw std::runtime_error("RDTSCP is not supported");
    const char* names[]={"legacy_double_checked","fused_scalar","fused_block"};
    for(int ratio:{512,1024}) {
        SdmConfig cfg;cfg.fs=44100.*ratio;cfg.order=order;cfg.family=parse_modulator_family(family);
        cfg.ditherLsb=dither;cfg.oneBitOutput=true;
        const size_t frames=verify?65537:(size_t)(cfg.fs*seconds)+7;
        const size_t blockFrames=verify?8191:4096*(size_t)ratio;
        std::vector<Block> blocks;
        for(size_t pos=0;pos<frames;pos+=blockFrames) {
            size_t count=std::min(blockFrames,frames-pos);Block block(2,std::vector<double>(count));
            for(size_t c=0;c<2;++c)for(size_t i=0;i<count;++i) {
                const double t=(double)(pos+i)/cfg.fs;
                block[c][i]=.42*std::sin(2*kPi*997.*t+.17*c)+.12*std::sin(2*kPi*3211.*t+.39*c);
            }
            blocks.push_back(std::move(block));
        }
        auto reference=run(0,blocks,frames,cfg);const auto hash=sha256(reference.words);
        std::cout<<"CONFIG ratio="<<ratio<<" channels=2 frames_per_channel="<<frames
                 <<" blocks="<<blocks.size()<<" block_frames="<<blockFrames<<" family="<<family
                 <<" requested="<<order<<" effective="<<reference.effective<<" dither="<<dither
                 <<" seed=0x2545F4914F6CDD1D tsc_unit=ticks_not_cpu_cycles"
                 <<" bytes="<<reference.words.size()*4<<" sha256="<<hash<<std::endl;
        auto equal=[&](const Trial&t){return t.words==reference.words&&t.clamp==reference.clamp
            &&t.overload==reference.overload&&t.nonfinite==reference.nonfinite&&t.effective==reference.effective;};
        for(int mode=1;mode<3;++mode) {
            auto warm=run(mode,blocks,frames,cfg);
            if(!equal(warm))throw std::runtime_error("warmup output/state mismatch");
            std::cout<<"WARMUP mode="<<names[mode]<<" sha256="<<sha256(warm.words)<<" byte_equal=1 state_equal=1\n";
        }
        if(verify){std::cout<<"VERIFY_ONLY no_performance_claim=1\n";continue;}
        for(auto pair:std::vector<std::array<int,2>>{{0,1},{1,2}}) {
            std::vector<double> time[2],ticks[2];
            for(int rep=0;rep<pairs;++rep) {
                for(int index:{rep&1,1-(rep&1)}) {
                    int mode=pair[index];auto trial=run(mode,blocks,frames,cfg);
                    if(!equal(trial))throw std::runtime_error("timed output/state mismatch");
                    time[index].push_back(trial.seconds);ticks[index].push_back(trial.tscPerSample);
                    std::cout<<std::setprecision(12)<<"TRIAL pair="<<names[pair[0]]<<"/"<<names[pair[1]]
                      <<" rep="<<rep<<" mode="<<names[mode]<<" seconds="<<trial.seconds
                      <<" tsc_ticks_per_sample="<<trial.tscPerSample<<" sha256="<<sha256(trial.words)
                      <<" byte_equal=1 state_equal=1"<<std::endl;
                }
            }
            auto median=[](auto v){std::sort(v.begin(),v.end());return v[v.size()/2];};
            double a=median(time[0]),b=median(time[1]);
            std::cout<<"RESULT ratio="<<ratio<<" pair="<<names[pair[0]]<<"/"<<names[pair[1]]
               <<" pairs="<<pairs<<" first_seconds="<<a<<" second_seconds="<<b<<" speedup="<<a/b
               <<" first_Msamples_s="<<frames*2/a/1e6<<" second_Msamples_s="<<frames*2/b/1e6
               <<" first_tsc="<<median(ticks[0])<<" second_tsc="<<median(ticks[1])<<" mismatches=0\n";
        }
    }
    return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
