#if defined(RTA_TEST_EARLY_INTRINSICS) && defined(__clang__) && defined(__x86_64__)
#include <immintrin.h>
#endif
#include "dsp/resampler.h"
#include <bit>
#include <cstdio>
#include <limits>
using namespace rta::dsp;
int main() {
    size_t checks=0,vectorOutputs=0;
    for(auto rates: {std::pair{44100u,48000u},{44100u,192000u},{48000u,44100u},
                     {96000u,44100u},{48000u,24000u},{48000u,16000u}})
    for(int channels: {1,2,6}) for(int taps: {7,32,211}) {
        ResamplerConfig cfg;cfg.inRate=rates.first;cfg.outRate=rates.second;
        cfg.channels=channels;cfg.minTapsPerInput=taps;cfg.applyTapFloor=false;
        cfg.cpuKernel=ResamplerCpuKernel::Scalar;
        PolyphaseResampler scalar(cfg);
        cfg.cpuKernel=ResamplerCpuKernel::Auto;cfg.cpuParallel=false;
        PolyphaseResampler serial(cfg);
        cfg.cpuParallel=true;PolyphaseResampler pooled(cfg);
        for(int pass=0;pass<2;++pass) {
            if(pass){scalar.reset();serial.reset();pooled.reset();}
            for(size_t frames: {1u,3u,17u,129u,4096u,2053u,257u}) {
                size_t cap=scalar.maxOutFrames(frames);
                std::vector<std::vector<double>> in(channels,std::vector<double>(frames));
                std::vector<std::vector<double>> a(channels,std::vector<double>(cap+4,123)),b=a,c=a;
                std::vector<const double*> ip(channels);
                std::vector<double*> ap(channels),bp(channels),cp(channels);
                for(int ch=0;ch<channels;++ch){
                    ip[ch]=in[ch].data();ap[ch]=a[ch].data();bp[ch]=b[ch].data();cp[ch]=c[ch].data();
                    for(size_t i=0;i<frames;++i)in[ch][i]=frames==257 ? 0 : std::sin((i+1)*.113+ch)*.8;
                    if(pass && frames==4096){in[ch][70]=-0.0;in[ch][240]=1e-310;in[ch][4020]=std::numeric_limits<double>::infinity();}
                }
                const size_t n=scalar.process(ip.data(),frames,ap.data());
                if(serial.process(ip.data(),frames,bp.data())!=n || pooled.process(ip.data(),frames,cp.data())!=n)return 1;
                vectorOutputs+=pooled.cpu_simd_outputs_last_block();
                for(int ch=0;ch<channels;++ch)for(size_t i=0;i<cap+4;++i){
                    auto expected=std::bit_cast<uint64_t>(a[ch][i]);
                    if(expected!=std::bit_cast<uint64_t>(b[ch][i]) || expected!=std::bit_cast<uint64_t>(c[ch][i])) {
                        std::printf("FAIL %u->%u ch%d taps%d frames%zu i%zu\n",rates.first,rates.second,channels,taps,frames,i);return 2;
                    }
                    checks+=2;
                }
                if(scalar.cpu_simd_outputs_last_block()!=0 || serial.cpu_parallel_threads_last_block()>1)return 3;
            }
        }
    }
    if(resampler_avx2_available() && !vectorOutputs)return 4;
    std::printf("PASS rational bit checks=%zu vector_outputs=%zu avx2=%d\n",checks,vectorOutputs,resampler_avx2_available());
}
