// Independent, device-free regression for high-rate seventh-order section state.
// This test proves API/state equivalence and recovery, not spectral quality.
// Spectral acceptance is intentionally a separate long-window production probe.
#include "dsp/modulator.h"
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <limits>

using namespace rta::dsp;
namespace {
size_t checks=0, failures=0, samples=0, cases=0;
void check(bool ok,const char* label) {
    ++checks;
    if(!ok) { if(failures<24)std::printf("FAIL %s\n",label); ++failures; }
}
void same(double a,double b,const char* label) {
    ++samples; check(std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b),label);
}
void state(const SdmModulator& a,const SdmModulator& b) {
    check(a.effectiveOrder()==b.effectiveOrder() && a.levels()==b.levels()
       && a.clamped()==b.clamped() && a.overload()==b.overload()
       && a.nonFinite()==b.nonFinite() && a.uses_sections()==b.uses_sections()
       && a.steppedDown()==b.steppedDown() && a.stepDownReason()==b.stepDownReason()
       && a.levels_clamped()==b.levels_clamped(),"observable state equality");
}
SdmConfig config(ModulatorFamily f,double fs,double dith) {
    SdmConfig c; c.family=f; c.fs=fs; c.order=7; c.cutoffHz=100000.;
    c.oneBitOutput=true; c.levels=2; c.ditherLsb=dith;
    return c; // Guards are deliberately enabled; this must exercise production.
}
std::vector<double> signal(size_t n,bool right=false) {
    std::vector<double> x(n);
    for(size_t i=0;i<n;++i) {
        const double t=double(i);
        x[i]=right ? .18*std::sin(.0171*t)-.07*std::cos(.009*t)+.025
                   : .31*std::sin(.0113*t)+.09*std::cos(.0051*t)-.04;
        if(i>n/3 && i<n/3+100)x[i]=right?-.95:.99;
        if(i>3*n/4)x[i]=right?-.12:.17;
    }
    if(n>2000) {
        x[511]=5.;x[512]=-5.;
        x[1500]=std::numeric_limits<double>::quiet_NaN();
        x[1501]=std::numeric_limits<double>::infinity();
        x[1502]=-std::numeric_limits<double>::infinity();
    }
    return x;
}
template<class M> void suffix(M& a,M& b,const std::vector<double>& x,size_t n=1024) {
    for(size_t i=0;i<n;++i)same(a.process(x[i]),b.process(x[i]),"suffix exposes hidden state");
}
void block_run(SdmModulator& m,const std::vector<double>& x,
               const std::vector<double>& ref,int mode) {
    static const size_t sizes[]={1,7,31,257,4096,13,8191};
    size_t pos=0,part=0;
    while(pos<x.size()) {
        const size_t n=std::min(sizes[part%7],x.size()-pos);
        const int use=(mode==2)?int(part%3):mode;
        auto sink=[&](size_t i,double y) {
            check(i<n,"sink local index");same(y,ref[pos+i],"chunk bit exact");
        };
        if(use==0)m.process_block(x.data()+pos,n,sink);
        else if(use==1)m.process_binary_block(x.data()+pos,n,sink);
        else for(size_t i=0;i<n;++i)sink(i,m.process(x[pos+i]));
        pos+=n;++part;
    }
}
void mono_case(const SdmConfig& c,const std::vector<double>& x) {
    SdmModulator scalar(c),ordinary(c),binary(c),mixed(c);
    check(scalar.uses_sections(),"sections really selected");
    check(scalar.order()==7 && scalar.effectiveOrder()==7 && !scalar.steppedDown(),"production guard admits seven");
    const double hinf=ntf_hinf_lin(scalar.ntf());
    check(std::isfinite(hinf) && hinf<=1.36,"public NTF Hinf passes");
    check(scalar.levels()==2,"binary output contract");
    std::vector<double> ref(x.size());
    for(size_t i=0;i<x.size();++i)ref[i]=scalar.process(x[i]);
    check(scalar.nonFinite()==3,"all three invalid samples counted");
    size_t zero=0;
    ordinary.process_block(nullptr,0,[&](size_t,double){++zero;});
    binary.process_binary_block(nullptr,0,[&](size_t,double){++zero;});
    check(zero==0,"zero blocks do not call sink");
    block_run(ordinary,x,ref,0);block_run(binary,x,ref,1);block_run(mixed,x,ref,2);
    state(scalar,ordinary);state(scalar,binary);state(scalar,mixed);
    auto a=scalar,b=ordinary,d=binary,e=mixed;
    suffix(a,b,x);a=scalar;suffix(a,d,x);a=scalar;suffix(a,e,x);
    // A copy must preserve section delay state AND RNG, not just coefficients.
    auto copy=mixed;
    suffix(mixed,copy,x,4096);state(mixed,copy);
    // Equal RNG positions but deliberately different histories before reset.
    SdmModulator ra(c),rb(c);
    for(size_t i=0;i<3333;++i) {ra.process(x[i]);rb.process(-.113);}
    ra.reset();rb.reset();
    check(ra.clamped()==0 && ra.overload()==0 && ra.nonFinite()==0,"reset counters");
    suffix(ra,rb,x,4096);state(ra,rb);
    if(c.ditherLsb==0.) {ra.reset();SdmModulator fresh(c);suffix(ra,fresh,x);}
    ++cases;
    std::printf("mono family=%s fs=%.0f dither=%g hinf=%.9g effective=%d invalid=%lld fails=%zu\n",
        modulator_family_name(c.family),c.fs,c.ditherLsb,hinf,scalar.effectiveOrder(),scalar.nonFinite(),failures);
}
struct SinkAbort {};
void throw_case(const SdmConfig& c,const std::vector<double>& x,int mode,size_t stop) {
    SdmModulator ref(c),m(c);
    for(size_t i=0;i<=stop;++i)ref.process(x[i]);
    bool caught=false;
    auto sink=[&](size_t i,double){if(i==stop)throw SinkAbort{};};
    try {
        if(mode==0)m.process_block(x.data(),x.size(),sink);
        else m.process_binary_block(x.data(),x.size(),sink);
    } catch(const SinkAbort&) {caught=true;}
    check(caught,"sink exception propagates");state(ref,m);
    // After failure, the sample on which sink threw has already been consumed.
    for(size_t i=stop+1;i<std::min(x.size(),stop+4097);++i)
        same(ref.process(x[i]),m.process(x[i]),"throw resumes after consumed sample");
    state(ref,m);++cases;
}
void recovery_case(const SdmConfig& c) {
    SdmModulator m(c);
    for(size_t i=0;i<4096;++i)m.process(.19*std::sin(.01*double(i)));
    m.process(std::numeric_limits<double>::quiet_NaN());
    m.process(std::numeric_limits<double>::infinity());
    m.process(-std::numeric_limits<double>::infinity());
    m.process(1.);m.process(-1.);m.process(5.);m.process(-5.);
    for(size_t i=0;i<32768;++i)m.process(.18);
    const auto beforeO=m.overload(),beforeC=m.clamped();
    double sum=0;size_t sameSign=0;double prev=0;
    constexpr size_t count=32768;
    for(size_t i=0;i<count;++i) {
        const double y=m.process(.18);
        check(y==1. || y==-1.,"recovered binary output");
        sum+=y;if(i&&y==prev)++sameSign;prev=y;
    }
    const double mean=sum/count,ratio=double(sameSign)/(count-1);
    check(std::abs(mean-.18)<.02,"DC mean proves recovery not full-scale lock");
    check(ratio<.9,"post-transient same-sign rate not locked");
    check(m.overload()==beforeO && m.clamped()==beforeC,"recovery protection counters stop growing");
    check(m.nonFinite()==3,"nonfinite count preserved");
    std::printf("recovery family=%s fs=%.0f dither=%g mean=%.9g same_sign=%.9g overload_delta=%lld clamp_delta=%lld fails=%zu\n",
      modulator_family_name(c.family),c.fs,c.ditherLsb,mean,ratio,m.overload()-beforeO,m.clamped()-beforeC,failures);
    ++cases;
}
#if !defined(RTA_DISABLE_SDM_STEREO_SIMD) && (defined(__SSE2__) || defined(_M_X64))
void stereo_case(SdmConfig aCfg,SdmConfig bCfg,const std::vector<double>& x,const std::vector<double>& z) {
    SdmModulator a(aCfg),b(bCfg),sa(aCfg),sb(bCfg);
    std::vector<double> l(x.size()),r(x.size());
    for(size_t i=0;i<x.size();++i){l[i]=sa.process(x[i]);r[i]=sb.process(z[i]);}
    size_t callbacks=0;
    check(SdmModulator::try_process_binary_stereo_bits_block(a,b,nullptr,nullptr,0,
       [&](size_t,bool,bool){++callbacks;}),"empty sections stereo supported");
    check(callbacks==0,"empty stereo does not call sink");
    size_t pos=0,part=0;const size_t nlist[]={1,19,4097,7,1024,333};
    while(pos<x.size()) {
        const size_t n=std::min(nlist[part++%6],x.size()-pos);
        bool ok=SdmModulator::try_process_binary_stereo_bits_block(a,b,x.data()+pos,z.data()+pos,n,
          [&](size_t i,bool av,bool bv){
            check(i<n,"stereo index local");same(av?1.:-1.,l[pos+i],"left stereo scalar exact");
            same(bv?1.:-1.,r[pos+i],"right stereo scalar exact");++callbacks;
          });check(ok,"two sections stereo accepted");pos+=n;
    }
    check(callbacks==x.size(),"stereo exact callback count");state(a,sa);state(b,sb);
    suffix(a,sa,x);suffix(b,sb,z);
    // A stereo sink exception commits both current samples, including RNG.
    SdmModulator ta(aCfg),tb(bCfg),ra(aCfg),rb(bCfg);
    constexpr size_t stop=1501;
    for(size_t i=0;i<=stop;++i){ra.process(x[i]);rb.process(z[i]);}
    bool caught=false;
    try {SdmModulator::try_process_binary_stereo_bits_block(ta,tb,x.data(),z.data(),x.size(),
      [&](size_t i,bool,bool){if(i==stop)throw SinkAbort{};});}
    catch(const SinkAbort&){caught=true;}
    check(caught,"stereo exception propagates");state(ta,ra);state(tb,rb);
    for(size_t i=stop+1;i<stop+4097;++i) {
        same(ta.process(x[i]),ra.process(x[i]),"stereo exception left commit");
        same(tb.process(z[i]),rb.process(z[i]),"stereo exception right commit");
    }
    std::printf("stereo left_family=%s right_family=%s left_fs=%.0f right_fs=%.0f dl=%g dr=%g fails=%zu\n",
      modulator_family_name(aCfg.family),modulator_family_name(bCfg.family),aCfg.fs,bCfg.fs,
      aCfg.ditherLsb,bCfg.ditherLsb,failures);++cases;
}
void rejected_pair(SdmConfig aCfg,SdmConfig bCfg,const std::vector<double>& x) {
    SdmModulator a(aCfg),b(bCfg);auto aa=a,bb=b;size_t called=0;
    check(a.uses_sections()!=b.uses_sections(),"rejection fixture is heterogeneous");
    const bool ok=SdmModulator::try_process_binary_stereo_bits_block(a,b,x.data(),x.data(),1024,
       [&](size_t,bool,bool){++called;});
    check(!ok && !called,"heterogeneous old/new rejected before callback");
    state(a,aa);state(b,bb);suffix(a,aa,x);suffix(b,bb,x);++cases;
}
#endif
// Frozen pre-sections scalar recurrence: independent execution oracle for the
// unchanged fifth-order / EC paths. Design functions are shared deliberately;
// this checks execution compatibility, not an independent NTF design proof.
struct LegacyScalar {
    NtfPoly n;double e[16]{},w[16]{};uint64_t rng=0x2545F4914F6CDD1Dull;
    double dither;long long over=0,clamp=0,bad=0;
    LegacyScalar(NtfPoly ntf,double dith):n(std::move(ntf)),dither(dith){}
    double process(double x) {
        const size_t sz=size_t(n.order)+1;double acc=0;
        for(size_t k=1;k<sz;++k)acc+=n.p[k]*e[k-1];
        for(size_t k=1;k<sz;++k)acc-=n.d[k]*w[k-1];
        if(!std::isfinite(acc)){acc=0;w[0]=0;}
        double u=x+acc;if(!std::isfinite(u)){u=0;++bad;}
        if(u>1.5){u=1.5;++over;}if(u< -1.5){u=-1.5;++over;}
        double dith=0;
        if(dither>0){rng^=rng>>12;rng^=rng<<25;rng^=rng>>27;
            const uint64_t next=rng*2685821657736338717ull;
            dith=dither*2.*(double(next>>11)/9007199254740992.-.5);u+=dith;
            if(u>1.5){u=1.5;++over;}if(u< -1.5){u=-1.5;++over;}}
        const double y=u>=0?1.:-1.;double err=y-u;
        if(err>2.5){err=2.5;++clamp;}if(err< -2.5){err=-2.5;++clamp;}err+=dith;
        for(size_t k=sz-1;k>0;--k){e[k]=e[k-1];w[k]=w[k-1];}e[0]=err;w[0]=acc;
        return y;
    }
};
void legacy_case(SdmConfig c,const std::vector<double>& x) {
    SdmModulator m(c);
    check(!m.uses_sections(),"legacy path not redirected to sections");
    check(m.effectiveOrder()==c.order,"legacy effective order unchanged");
    NtfPoly expected=(c.family==ModulatorFamily::Ec||c.ecCorrection)
      ?synthesize_ntf(c.order,c.fs,20000.,c.ntfHinf):design_ntf_poly(c.order,c.cutoffHz,c.fs);
    check(m.ntf().b==expected.b && m.ntf().d==expected.d && m.ntf().p==expected.p,"legacy coefficients unchanged");
    LegacyScalar old(expected,c.ditherLsb);
    for(size_t i=0;i<x.size();++i)same(m.process(x[i]),old.process(x[i]),"frozen legacy scalar execution");
    check(m.overload()==old.over && m.clamped()==old.clamp && m.nonFinite()==old.bad,"frozen legacy counters");
    std::printf("legacy family=%s order=%d fs=%.0f dither=%g fails=%zu\n",
      modulator_family_name(c.family),c.order,c.fs,c.ditherLsb,failures);++cases;
}
}
int main() {
    const auto x=signal(32768),z=signal(32768,true);
    for(auto f:{ModulatorFamily::Dsd,ModulatorFamily::Ahm})
      for(double fs:{45158400.,49152000.})for(double dith:{0.,.0005}) {
        const auto c=config(f,fs,dith);mono_case(c,x);recovery_case(c);
        for(int mode:{0,1})for(size_t stop:{size_t(0),size_t(1501),size_t(1703)})throw_case(c,x,mode,stop);
      }
    for(double dith:{0.,.0005}) {
        auto c=config(ModulatorFamily::Dsd,45158400.,dith);c.order=5;legacy_case(c,x);
        c.family=ModulatorFamily::Ec;c.order=7;c.fs=22579200.;legacy_case(c,x);
        c.family=ModulatorFamily::Dsd;c.ecCorrection=true;legacy_case(c,x);
    }
#if !defined(RTA_DISABLE_SDM_STEREO_SIMD) && (defined(__SSE2__) || defined(_M_X64))
    for(auto f:{ModulatorFamily::Dsd,ModulatorFamily::Ahm})
      for(double fs:{45158400.,49152000.})for(double dl:{0.,.0005})for(double dr:{0.,.0005})
        stereo_case(config(f,fs,dl),config(f,fs,dr),x,z);
    // Different coefficients/rate/family remain independent stereo channels.
    stereo_case(config(ModulatorFamily::Dsd,45158400.,.0005),
                config(ModulatorFamily::Ahm,49152000.,0.),x,z);
    auto fresh=config(ModulatorFamily::Dsd,45158400.,.0005);
    auto old=fresh;old.family=ModulatorFamily::Ec;
    rejected_pair(fresh,old,x);rejected_pair(old,fresh,x);
#else
    check(false,"stereo API unavailable in requested test build");
#endif
    std::printf("RESULT cases=%zu checks=%zu compared_samples=%zu failures=%zu\n",cases,checks,samples,failures);
    return failures?1:0;
}
