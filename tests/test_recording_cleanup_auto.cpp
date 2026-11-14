// Auto is deliberately conditional: no trustworthy quiet interval means unity.
// Quiet stochastic music remains a preservation counterexample, never a positive
// noise-removal fixture. No audio output APIs are used by this test.
#include "dsp/recording_cleanup.h"
#include "engine/pipeline.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <string>

using namespace rta::dsp;
static int failures=0;
static void check(bool ok,const char* name,double value=0) {
    std::printf("%s %s %.12g\n",ok?"PASS":"FAIL",name,value);
    failures+=!ok;
}
// This detection seam lets the real pre-Auto processor run the red regressions.
// It is not a substitute DSP: the old processor is exercised when fields absent.
template<class T> static bool request_auto(T& cfg) {
    cfg.enabled=true;
    if constexpr(requires(T t){t.noiseMode;}) {
        cfg.noiseMode=static_cast<decltype(cfg.noiseMode)>(1);return true;
    } else return false;
}
template<class T> static bool valid(const T& s) {
    if constexpr(requires(T t){t.autoEstimateValid;})return s.autoEstimateValid;
    else return false;
}
template<class T> static double estimate(const T& s) {
    if constexpr(requires(T t){t.estimatedNoiseFloorDbfs;})return s.estimatedNoiseFloorDbfs;
    else return 0;
}
template<class T> static double budget(const T& s) {
    if constexpr(requires(T t){t.appliedNoiseFloorDbfs;})return s.appliedNoiseFloorDbfs;
    else return 0;
}
template<class T> static size_t learned(const T& s) {
    if constexpr(requires(T t){t.autoLearningFrames;})return s.autoLearningFrames;
    else return 0;
}
template<class T> static bool frozen(const T& s) {
    if constexpr(requires(T t){t.autoFrozenForDrain;})return s.autoFrozenForDrain;
    else return false;
}
template<class T> static bool begin_drain(T& p) {
    if constexpr(requires(T t){t.begin_drain();}){p.begin_drain();return true;}
    else return false;
}
template<class T> static bool pipeline_valid(const T& s) {
    if constexpr(requires(T t){t.cleanupAutoEstimateValid;})return s.cleanupAutoEstimateValid;
    else return false;
}
static double rms(const std::vector<double>& x,size_t first=0,size_t end=0) {
    if(!end)end=x.size();double v=0;
    for(size_t i=first;i<end;++i)v+=x[i]*x[i];
    return std::sqrt(v/(end-first));
}
static double delta(const std::vector<double>& a,const std::vector<double>& b) {
    double e=0;for(size_t i=0;i<a.size();++i)e=std::max(e,std::fabs(a[i]-b[i]));return e;
}
struct Fixture { std::vector<double> clean,raw;double noiseDb; };
static Fixture fixture(int variant,double fs=44100,double noiseDb=-100,bool colored=false,bool quiet=true) {
    size_t n=size_t(6*fs);Fixture f;f.clean.resize(n);f.raw.resize(n);f.noiseDb=noiseDb;
    std::mt19937 rng(0x33207+variant*173);std::normal_distribution<double> normal;
    double sigma=std::pow(10.,noiseDb/20),memory=0;
    for(size_t i=0;i<n;++i) {
        double t=i/fs;bool gap=quiet&&t>=.5&&t<2.;
        double music=.08*std::sin(2*kPi*(997.3+variant*37.1)*t)+
                     .035*std::sin(2*kPi*(13017.7+variant*191.3)*t);
        if(variant&1)music+=.02*std::sin(2*kPi*(173*t+130*t*t));
        f.clean[i]=gap?0:music;
        double z=normal(rng);if(colored){memory=.98*memory+std::sqrt(1-.98*.98)*z;z=memory;}
        f.raw[i]=f.clean[i]+sigma*z;
    }
    return f;
}
static std::vector<double> run(const std::vector<double>& in,RecordingCleanup& c,size_t chunk=137,int ch=1) {
    std::vector<std::vector<double>> p(ch,in);
    for(auto& v:p)v.resize(in.size()+c.latency()+c.tail());
    std::vector<double*> planes(ch);
    for(size_t offset=0;offset<p[0].size();) {
        if(offset==in.size())begin_drain(c);
        size_t end=offset<in.size()?in.size():p[0].size();
        size_t count=std::min(chunk,end-offset);
        for(int k=0;k<ch;++k)planes[k]=p[k].data()+offset;
        c.process(planes.data(),ch,count);offset+=count;
    }
    for(int k=1;k<ch;++k)check(p[k]==p[0],"Auto linked channels stay bit identical");
    return {p[0].begin()+c.latency(),p[0].begin()+c.latency()+in.size()};
}
// Independent direct common-band measurement. Neither alignment search nor
// gain matching is permitted; declared processor latency is removed by run().
static double error(const std::vector<double>& a,const std::vector<double>& ref,double fs=44100) {
    std::vector<double> h(129);double sum=0,total=0;
    double cutoff=std::min(20000.,fs*.45);
    for(int j=-64;j<=64;++j){double s=j?std::sin(2*kPi*cutoff/fs*j)/(kPi*j):2*cutoff/fs;h[j+64]=s*(.5+.5*std::cos(kPi*j/64));sum+=h[j+64];}
    for(auto& v:h)v/=sum;
    for(size_t i=4096;i+4096<a.size();++i){double e=0;for(int j=-64;j<=64;++j)e+=h[j+64]*(a[i+j]-ref[i+j]);total+=e*e;}
    return total/(a.size()-8192);
}
static void learning_and_accuracy(const char* exportPath) {
    double average=0;
    for(int v=0;v<4;++v) {
        double db=-110+v*5.;auto f=fixture(v,44100,db);
        RecordingCleanupConfig cfg;bool supported=request_auto(cfg);cfg.noiseFloorDbfs=-60;
        RecordingCleanup c(cfg,44100,1);auto y=run(f.raw,c);
        check(supported,"Auto configuration exists");
        check(valid(c.stats()),"trusted stationary quiet interval actually learns a profile");
        check(std::fabs(estimate(c.stats())-db)<=2,"Auto estimated floor within2dB of hidden noise reference",estimate(c.stats())-db);
        check(budget(c.stats())<=db,"Auto applied noise budget never exceeds known noise floor",budget(c.stats())-db);
        check(c.stats().maxReductionDb<=1.000001,"Auto per-bin attenuation bounded to1dB",c.stats().maxReductionDb);
        double gain=10*std::log10(error(f.raw,f.clean)/error(y,f.clean));average+=gain;
        check(gain>=.25,"independent qualified same20kHz-band error improves>=.25dB",gain);
        std::printf("AUTO_CASE variant=%d true_db=%.1f estimate_db=%.9f budget_db=%.9f gain_db=%.9f\n",v,db,estimate(c.stats()),budget(c.stats()),gain);
        if(v==1&&exportPath){std::ofstream o(exportPath,std::ios::binary);uint64_t n=f.clean.size();o.write((char*)&n,8);for(auto* a:{&f.clean,&f.raw,&y})o.write((char*)a->data(),a->size()*8);check(bool(o),"Auto independent oracle fixture written");}
    }
    check(average/4>=.5,"frozen qualified Auto holdout mean improvement>=.5dB",average/4);
}
static void preservation_and_lifecycle() {
    RecordingCleanupConfig cfg;request_auto(cfg);
    std::vector<double> music(132300);std::mt19937 rng(7332);std::normal_distribution<double> noise(0,1e-6);
    for(auto& x:music)x=noise(rng);
    RecordingCleanup weak(cfg,44100,1);auto untouched=run(music,weak);
    check(!valid(weak.stats()),"whole-track quiet noise-like MUSIC is not treated as a noise-only calibration");
    check(delta(untouched,music)<1e-18,"no programme reference means exact unity gain for weak stochastic MUSIC",delta(untouched,music));
    auto loud=fixture(5,44100,-100,false,false);RecordingCleanup noPause(cfg,44100,1);auto a=run(loud.raw,noPause);
    check(!valid(noPause.stats())&&delta(a,loud.raw)<1e-13,"continuous music without trusted quiet interval remains unity",delta(a,loud.raw));
    auto colored=fixture(6,44100,-100,true);RecordingCleanup color(cfg,44100,1);auto cy=run(colored.raw,color);
    check(!valid(color.stats())&&delta(cy,colored.raw)<1e-13,"strongly colored noise stays untrusted rather than claiming a white-floor estimate",delta(cy,colored.raw));
    auto f=fixture(2);RecordingCleanup one(cfg,44100,1),irregular(cfg,44100,1),big(cfg,44100,1);
    auto x=run(f.raw,one,1),y=run(f.raw,irregular,137),z=run(f.raw,big,5003);
    check(x==y&&y==z,"Auto sample clock and estimator are exactly independent of caller chunk size");
    check(learned(one.stats())==learned(big.stats()),"Auto learned source-frame counter is block invariant");
    check(frozen(one.stats()),"EOF drain explicitly freezes the estimator");
    one.reset();RecordingCleanup fresh(cfg,44100,1);auto after=run(f.raw,one),brandNew=run(f.raw,fresh);
    check(after==brandNew,"Auto seek/new-track reset is identical to a fresh processor");
    // Same audio, radically different manual fields: Auto must ignore both.
    auto alternate=cfg;alternate.noiseFloorDbfs=-60;RecordingCleanup second(alternate,44100,1);
    check(run(f.raw,second)==brandNew,"Auto does not secretly use the legacy manual noise floor");
    for(int ch:{1,2,6,8}){RecordingCleanup linked(cfg,44100,ch);run(f.raw,linked,5003,ch);check(valid(linked.stats()),"Auto learning supports1/2/6/8 channels");}
    for(double fs:{48000.,96000.,192000.}){auto rate=fixture(0,fs);RecordingCleanup sr(cfg,fs,1);run(rate.raw,sr,5003);check(valid(sr.stats()),"Auto learning seconds are correct across source rates",fs);}
    std::vector<double> impulse(9001);impulse[0]=1;impulse[4000]=2;impulse.back()=-1;
    RecordingCleanup safe(cfg,44100,1);auto si=run(impulse,safe);
    check(delta(si,impulse)<1e-14&&safe.latency()==2048&&safe.tail()==2047,"Auto adds no learning audio delay and preserves full-scale first/mid/final impulses",delta(si,impulse));
    check(!valid(safe.stats()),"zero padding and isolated transients cannot train Auto");
}
static void actual_pipeline() {
    auto f=fixture(1);for(bool doubles:{false,true}){
        rta::engine::PipelineConfig cfg;cfg.outRate=88200;cfg.outBits=32;cfg.taps=1024;cfg.twoStage=false;cfg.gpuResample=false;request_auto(cfg.recordingCleanup);
        rta::engine::Pipeline p(cfg);rta::engine::PipelineStats last;
        for(size_t pos=0;pos<f.raw.size();){size_t n=std::min<size_t>(5003,f.raw.size()-pos);rta::audio::AudioBuffer b;b.rate=44100;b.channels=1;b.bits=32;b.isFloat=true;b.raw.resize(n);for(size_t i=0;i<n;++i)b.setSample(i,0,f.raw[pos+i]);std::vector<std::vector<double>> out;p.process(b,&last,doubles?&out:nullptr);pos+=n;}
        check(pipeline_valid(last),"actual PCM/DSD-doubleOut Pipeline exposes a valid Auto profile");
        auto before=learned(*p.prepared_recording_cleanup_stats());std::vector<std::vector<double>> tail;p.drain_chunk(44100,doubles?&tail:nullptr);
        check(frozen(*p.prepared_recording_cleanup_stats())&&before==learned(*p.prepared_recording_cleanup_stats()),"production Pipeline drain cannot train Auto on injected zeros");
        p.seek_reset();check(!valid(*p.prepared_recording_cleanup_stats())&&!frozen(*p.prepared_recording_cleanup_stats()),"Pipeline seek reset clears Auto profile and drain freeze");
    }
}
static void estimate_boundaries_and_recovery() {
    RecordingCleanupConfig cfg;request_auto(cfg);
    RecordingCleanupConfig manual;manual.enabled=true;manual.noiseFloorDbfs=-80;
    RecordingCleanup observedManual(manual,44100,1);
    check(observedManual.stats().appliedNoiseFloorDbfs==-80&&!observedManual.stats().autoEstimateValid,
          "Manual applied-noise telemetry reports its actual budget without claiming Auto validity");
    observedManual.reset();check(observedManual.stats().appliedNoiseFloorDbfs==-80,"Manual reset preserves its true applied budget telemetry");
    for(bool ultrasonic:{false,true}) {
        double fs=ultrasonic?96000:44100;std::vector<double> weakMusic((size_t)(4*fs));
        std::mt19937 rng(53223);std::normal_distribution<double> weak(0,1e-5);
        for(size_t i=0;i<weakMusic.size();++i){weakMusic[i]=weak(rng);if(i/fs<.5)weakMusic[i]+=ultrasonic?.08*std::sin(2*kPi*30000.*i/fs):.08;}
        RecordingCleanup noProgramme(cfg,fs,1);auto unity=run(weakMusic,noProgramme);
        check(!valid(noProgramme.stats())&&delta(unity,weakMusic)<1e-13,
              "DC/ultrasound alone cannot pretend to be an audible programme reference",delta(unity,weakMusic));
    }
    for(double hz:{5.,10.}) {
        std::vector<double> weakMusic(176400);std::mt19937 rng(53223);std::normal_distribution<double> weak(0,1e-5);
        for(size_t i=0;i<weakMusic.size();++i){weakMusic[i]=weak(rng);if(i<22050)weakMusic[i]+=.08*std::sin(2*kPi*hz*i/44100.);}
        RecordingCleanup noProgramme(cfg,44100,1);auto unity=run(weakMusic,noProgramme);
        check(!valid(noProgramme.stats())&&delta(unity,weakMusic)<1e-13,
              "5/10Hz infrasound alone cannot train a programme reference",delta(unity,weakMusic));
    }
    for(bool ultrasonic:{false,true}) {
        double fs=ultrasonic?48000:44100;auto f=fixture(0,fs,-100);
        for(size_t i=0;i<f.raw.size();++i)if(i/fs>=.5&&i/fs<2.)
            f.raw[i]+=ultrasonic?1e-4*std::sin(2*kPi*23000.*i/fs):1e-4;
        RecordingCleanup c(cfg,fs,1);run(f.raw,c);
        check(valid(c.stats())&&std::fabs(estimate(c.stats())+100)<=2,
              "DC/ultrasonic music cannot inflate the learned broadband noise floor",estimate(c.stats()));
    }
    RecordingCleanup warm(cfg,44100,1);std::vector<double> shortBlock(1024,1e-6);double* pp[]={shortBlock.data()};warm.process(pp,1,shortBlock.size());
    check(learned(warm.stats())==0&&!valid(warm.stats()),"FFT prefill is never trusted learning evidence");
    begin_drain(warm);auto manufactured=fixture(0).raw;double* fake[]={manufactured.data()};warm.process(fake,1,manufactured.size());
    check(learned(warm.stats())==0&&!valid(warm.stats())&&frozen(warm.stats()),"even noise-like drain input cannot manufacture an Auto profile");
    auto f=fixture(0);RecordingCleanup c(cfg,44100,1);auto source=f.raw;double* real[]={source.data()};c.process(real,1,source.size());
    double before=budget(c.stats());check(valid(c.stats()),"overload recovery fixture starts with a genuinely learned profile");
    std::vector<double> burst(32768);for(size_t i=0;i<4096;++i)burst[i]=std::sin(2*kPi*997*i/44100.);burst[6000]=2;burst[9000]=-2;
    auto recovered=run(burst,c);size_t positive=0,negative=0;bool allFinite=true;
    for(size_t i=0;i<recovered.size();++i){allFinite=allFinite&&std::isfinite(recovered[i]);if(i>24000){positive+=recovered[i]>1e-14;negative+=recovered[i]<-1e-14;}}
    check(allFinite&&rms(recovered,24001)<1e-14&&positive==0&&negative==0,
          "learned Auto recovers after0dB and +/-2 overload: RMS and sign counts",rms(recovered,24001));
    check(budget(c.stats())==before,"overload and digital zero cannot contaminate the learned floor");
    RecordingCleanup music(cfg,44100,1);source=f.raw;real[0]=source.data();music.process(real,1,source.size());
    std::mt19937 rng(733211);std::normal_distribution<double> noise(0,1e-6);std::vector<double> quietMusic(132300);for(auto& v:quietMusic)v=noise(rng);
    auto out=run(quietMusic,music);double loss=20*std::log10(rms(quietMusic,4096,quietMusic.size()-4096)/rms(out,4096,out.size()-4096));
    check(loss>=0&&loss<=1.05,"counterexample: quiet stochastic MUSIC after learning has bounded damage, not a noise-removal success",loss);
}
int main(int argc,char** argv){
    // The independent Python oracle provides only raw samples to this path;
    // its clean reference and the hidden noise distribution never enter DSP.
    if(argc==4&&std::string(argv[1])=="--process-f64") {
        std::ifstream in(argv[2],std::ios::binary);uint64_t n=0;in.read((char*)&n,8);
        if(!in||n<8193||n>2000000)return 2;
        std::vector<double> raw((size_t)n);in.read((char*)raw.data(),n*8);if(!in)return 2;
        RecordingCleanupConfig cfg;request_auto(cfg);RecordingCleanup c(cfg,44100,1);auto y=run(raw,c);
        std::ofstream out(argv[3],std::ios::binary);out.write((char*)&n,8);out.write((char*)y.data(),n*8);
        std::printf("ORACLE valid=%d estimate_db=%.12f budget_db=%.12f max_reduction_db=%.12f\n",valid(c.stats()),estimate(c.stats()),budget(c.stats()),c.stats().maxReductionDb);
        return out?0:3;
    }
    learning_and_accuracy(argc==2?argv[1]:nullptr);preservation_and_lifecycle();actual_pipeline();estimate_boundaries_and_recovery();std::printf("FAILURES=%d\n",failures);return failures?1:0;
}
