// Offline regressions: real resampler and production Pipeline, no audio device.
#include "engine/pipeline.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace rta::dsp;
using namespace rta::engine;
static int fails=0;
static void ck(bool ok,const char* label) {std::printf("%s %s\n",ok?"PASS":"FAIL",label);fails+=!ok;}
static std::vector<double> resample(PolyphaseResampler& r,const std::vector<double>& x,size_t block) {
    std::vector<double> result;
    for(size_t pos=0;pos<x.size();) {
        const size_t n=std::min(block,x.size()-pos);
        std::vector<double> o(r.maxOutFrames(n));const double* ip[]={x.data()+pos};double* op[]={o.data()};
        const size_t got=r.process(ip,n,op);result.insert(result.end(),o.begin(),o.begin()+got);pos+=n;
    }return result;
}
static void phase_tail(const char* preset) {
    const auto& p=*find_preset(preset);
    PipelineConfig pc;apply_filter_preset(pc,p);pc.autoTaps=true;pc.outRate=705600;
    pc.outBits=32;pc.gpuResample=false;pc.useFixed=false;
    ResamplerConfig rc;rc.inRate=44100;rc.outRate=pc.outRate;rc.channels=1;apply_preset(rc,p);
    rc.minTapsPerInput=solve_taps_per_phase(p.window,p.transition,p.apodizing,p.apodF1,p.apodF2,p.taps,p.tapsPerInput,44100);
    PolyphaseResampler reference(rc);std::vector<double> h;reference.prototype_into(h);
    const size_t N=512,L=16;
    std::vector<double> expected((N-1)*L+h.size(),0.0);
    for(size_t n=0;n<h.size();++n){expected[n]+=.5*h[n];expected[(N-1)*L+n]+=.5*h[n];}
    // Independent contract: MP/IP are causal filters with no uniform delay to
    // remove. LP has the known analytic (N_h-1)/2 delay (including cascades).
    const size_t trim=p.phase==PhaseKind::Linear?(size_t)std::llround(reference.tailInputSamples()*8.0):0;
    expected.erase(expected.begin(),expected.begin()+trim);
    std::vector<double> whole;
    for(size_t block:{N,(size_t)37}) {
        Pipeline pipe(pc);std::vector<double> got;
        for(size_t pos=0;pos<N;) {
            const size_t n=std::min(block,N-pos);rta::audio::AudioBuffer in;in.rate=44100;in.channels=1;in.bits=32;
            in.raw.assign(n,0);if(pos==0)in.raw[0]=1073741824;
            if(pos+n==N)in.raw[n-1]=1073741824;
            std::vector<std::vector<double>> out;pipe.process(in,nullptr,&out);
            got.insert(got.end(),out[0].begin(),out[0].end());pos+=n;
        }
        const double delay=pipe.algorithmic_latency_out_frames(),tail=pipe.resampler_tail_out_frames();
        ck(p.phase==PhaseKind::Linear?std::fabs(delay-reference.tailInputSamples()*8.0)<1e-9:delay==0,
            "phase-aware compensable delay");
        ck(tail>0,"natural FIR tail is separate from compensable delay");
        const size_t drain=(size_t)std::ceil((delay+tail)/16.0)+1;
        std::vector<std::vector<double>> out;pipe.drain_chunk(drain,&out);
        got.insert(got.end(),out[0].begin(),out[0].end());
        const size_t drop=(size_t)std::llround(delay);
        got.erase(got.begin(),got.begin()+drop);ck(got.size()>=expected.size(),"drain supplies complete first/last impulse support");
        got.resize(expected.size());double mx=0,energy=0,err=0;
        for(size_t i=0;i<got.size();++i){double d=got[i]-expected[i];mx=std::max(mx,std::fabs(d));energy+=expected[i]*expected[i];err+=d*d;}
        std::printf("TAIL %s block=%zu trim=%zu samples=%zu max_diff=%.3e rel_error_db=%.2f\n",
            preset,block,drop,got.size(),mx,err==0?-INFINITY:10*std::log10(err/energy));
        ck(mx<1e-12,"production compensated/drained PCM equals independent full impulse response");
        if(whole.empty())whole=got;else ck(whole==got,"whole/chunked first+last impulse outputs are bit identical");
    }
}
static void gpu_test(const char* path) {
#ifdef _WIN32
    if(path)SetDllDirectoryW(std::filesystem::u8path(path).parent_path().c_str());
    auto lib=path?LoadLibraryW(std::filesystem::u8path(path).c_str()):LoadLibraryW(L"rtagpu.dll");
    if(!lib){if(path)ck(false,"explicit GPU backend must load");else std::printf("GPU SKIP: no backend supplied/found\n");return;}
    auto available=(int(*)())GetProcAddress(lib,"rta_gpu_available");
    ck(available&&available()==1,"GPU trivial write/read kernel succeeds");if(!available||available()!=1)return;
    using Fn=int(*)(const double*,int,long long,const double*,int,int,const int*,const int*,long long,double*);
    auto fn=(Fn)GetProcAddress(lib,"rta_gpu_resample");auto inv=(void(*)())GetProcAddress(lib,"rta_gpu_resample_invalidate");
    ck(fn&&inv,"GPU resample exports present");if(!fn||!inv)return;
    const int N=64;std::vector<int> bases(N,3),phases(N,0);double a[]={1,0,0,0},b[]={2,0,0,0};
    std::vector<double> hist(32,0),out(N*8,0);for(int c=0;c<8;++c)hist[c*4+3]=c+1;
    inv();ck(fn(hist.data(),1,4,a,1,4,bases.data(),phases.data(),N,out.data())==N&&out[0]==1,"GPU coefficient A");
    ck(fn(hist.data(),1,4,b,1,4,bases.data(),phases.data(),N,out.data())==N&&out[0]==2,"same L/T coefficient B replaces cached A");
    for(int channels:{2,8,1,4}){
        const int frames=channels==8?N/2:N;
        const int rc=fn(hist.data(),channels,4,b,1,4,bases.data(),phases.data(),frames,out.data());
        bool ok=rc==frames;for(int n=0;n<frames;++n)for(int c=0;c<channels;++c)ok=ok&&out[n*channels+c]==2*(c+1);
        ck(ok,"GPU mono/multichannel same-or-smaller frame capacity preserves samples");
    }
    set_gpu_vendor("amd");ResamplerConfig rc;rc.channels=1;rc.inRate=44100;rc.outRate=176400;rc.minTapsPerInput=64;
    rc.useFixed=true;PolyphaseResampler fixedCpu(rc),fixedGpuRequested(rc);fixedGpuRequested.set_use_gpu(true);
    ck(!fixedGpuRequested.gpu_active(),"fixed-point path takes precedence over floating GPU backend");
    std::vector<double>x(128);for(size_t i=0;i<x.size();++i)x[i]=std::ldexp((double)((int)i-64),-20);
    ck(resample(fixedCpu,x,19)==resample(fixedGpuRequested,x,19),"GPU request cannot silently replace fixed-point accumulation");
#else
    (void)path;
#endif
}
int main(int argc,char**argv) {
    for(auto p:{"poly-sinc-short-lp","poly-sinc-short-mp","poly-sinc-long-ip","poly-sinc-short-2s"})phase_tail(p);
    gpu_test(argc>1?argv[1]:nullptr);
    std::printf("AUDIT_PCM failures=%d\n",fails);return fails?1:0;
}
