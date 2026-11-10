// Independent review assertions and falsification tests. Never opens devices.
#include "dsp/dop_stream.h"
#include <cstdio>
#include <cmath>
using namespace rta::dsp;
static int fail=0,checks=0;
static void ck(bool v,const char* name) {++checks; if(!v){++fail;std::printf("FAIL %s\n",name);}}
static bool transportShape(const std::vector<double>& x) {for(auto y:x)if(y!=1&&y!=-1)return false;return true;}
static bool recoveryShape(const std::vector<double>& x) {
    if(!transportShape(x)||x.size()<2)return false;double mean=0;size_t same=0;
    for(size_t i=0;i<x.size();++i){mean+=x[i];if(i)same+=x[i]==x[i-1];}
    return std::fabs(mean/x.size())<.03&&double(same)/(x.size()-1)<.9;
}
int main() {
    // These calls never dereference an invalid address, even in the old code.
    const double* nullChannel[]={nullptr};
    const double validSample=1.; const double* validChannels[]={&validSample};
    const bool nullTop=dop_check_binary(nullptr,1,1), nullLane=dop_check_binary(nullChannel,1,1);
    const bool zeroChannels=dop_check_binary(validChannels,0,1), negativeChannels=dop_check_binary(validChannels,-1,1);
    std::printf("INVALID_INPUT null_top=%d null_lane=%d zero_channels=%d negative_channels=%d (all must reject)\n",nullTop,nullLane,zeroChannels,negativeChannels);
    ck(!nullTop,"binary validator rejects null top-level pointer");
    ck(!nullLane,"binary validator rejects null channel pointer");
    ck(!zeroChannels&&!negativeChannels,"binary validator rejects non-positive channel count");
    for(int shape=0;shape<4;++shape) {
        const double* const* ptr=shape==0?nullptr:shape==1?nullChannel:validChannels;
        const int channels=shape==2?0:shape==3?-1:1;
        DopPackState packState;packState.phaseA={0,1};
        std::vector<int32_t> preserved={12345,-67890};std::string reason;
        ck(!dop_pack_checked(ptr,channels,1,preserved,packState,&reason)&&!reason.empty()
            &&preserved==std::vector<int32_t>({12345,-67890})&&packState.phaseA==std::vector<char>({0,1}),
            "invalid dimensions rejected before output or marker-state mutation");
    }
    ck(dop_check_binary(nullptr,1,0),"valid zero-frame block may omit buffer");
    DopPackState emptyState;std::vector<int32_t> emptyOut;
    ck(dop_pack_checked(nullptr,1,0,emptyOut,emptyState)&&emptyOut.empty(),"checked pack preserves valid zero-frame behavior");
    // +/-1 alone accepts fullscale DC. Mean alone even accepts a slow rail-to-rail
    // square wave. The recovery predicate must reject BOTH deliberate counterexamples.
    std::vector<double> stuck(65536,1),square(65536),alternating(65536);
    for(size_t i=0;i<square.size();++i){square[i]=i<32768?1:-1;alternating[i]=(i&1)?1:-1;}
    ck(transportShape(stuck),"counterexample: binary transport accepts stuck DC");
    ck(!recoveryShape(stuck)&&!recoveryShape(square),"recovery rejects stuck DC and zero-mean slow square");
    ck(recoveryShape(alternating),"recovery predicate has a known-good positive control");
    for(double rate:{45158400.,49152000.}) {
        for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Ahm}) {
            SdmConfig c;c.fs=rate;c.order=7;c.family=family;c.oneBitOutput=true;c.cutoffHz=100000;c.ditherLsb=.0005;
            SdmModulator m(c);ck(m.effectiveOrder()==7&&!m.steppedDown()&&m.uses_sections(),"production defaults really run sectioned seventh order");
            if(family==ModulatorFamily::Ahm)ck(m.levels()==2&&m.levels_clamped()&&!m.levels_clamp_reason().empty(),"one-bit AHM reports its multilevel downgrade");
            c.cutoffHz=20000; SdmModulator low(c);
            std::printf("ORDER rate=%.0f family=%s cutoff100k_effective=%d cutoff20k_effective=%d high_sections=%d low_sections=%d\n",rate,modulator_family_name(family),m.effectiveOrder(),low.effectiveOrder(),m.uses_sections(),low.uses_sections());
            ck(low.effectiveOrder()==5&&low.steppedDown()&&!low.stepDownReason().empty(),"20k design fallback remains explicit");
        }
        SdmConfig c;c.fs=rate;c.order=7;c.oneBitOutput=true;c.cutoffHz=100000;c.ditherLsb=.0005;
        SdmModulator m(c); const size_t N=262144;
        for(size_t i=0;i<N;++i)m.process(std::sin(2*kPi*997*i/rate));
        m.process(5.);m.process(-5.); // both polarities of single-sample overrange
        for(size_t i=0;i<N;++i)m.process(0.);
        const auto ov=m.overload(),cl=m.clamped();std::vector<double> recovered(N);
        for(auto& y:recovered)y=m.process(0.);
        double mean=0;size_t same=0;for(size_t i=0;i<N;++i){mean+=recovered[i];if(i)same+=recovered[i]==recovered[i-1];}
        ck(recoveryShape(recovered)&&m.overload()==ov&&m.clamped()==cl&&m.nonFinite()==0,"full-scale +/-5 burst recovers without rail locks or continuing limits");
        // Restore a real tone, not just silence. Block-average independently removes
        // most shaped ultrasonic energy; compare coherent filtered input/output RMS.
        const double fin=23*rate/N; double xsum=0,ysum=0,x2=0,y2=0;
        for(size_t i=0;i<2*N;++i){double x=.62*std::sin(2*kPi*fin*i/rate),y=m.process(x);if(i>=N){xsum+=x;ysum+=y;if((i+1)%256==0){x2+=xsum*xsum;y2+=ysum*ysum;xsum=ysum=0;}}}
        double xr=std::sqrt(x2/(N/256))/256,yr=std::sqrt(y2/(N/256))/256;
        ck(std::fabs(xr-yr)<.001&&m.overload()==ov&&m.clamped()==cl,"recovered coherent audio RMS matches input");
        std::printf("RECOVERY rate=%.0f order=%d total_overload=%lld overload_delta=%lld clamp_delta=%lld silence_mean=%.12g same_sign=%.12g tone_rms=%.12g expected_rms=%.12g\n",rate,m.effectiveOrder(),ov,m.overload()-ov,m.clamped()-cl,mean/N,double(same)/(N-1),yr,xr);
    }
    // Real multilevel AHM cannot pass a 1-bit transport. Reject before mutation.
    SdmConfig c;c.family=ModulatorFamily::Ahm;c.fs=45158400;c.cutoffHz=100000;c.ahmBits=8;
    std::vector<std::unique_ptr<SdmModulator>> mods;mods.push_back(std::make_unique<SdmModulator>(c));
    std::vector<std::vector<double>> in(1,std::vector<double>(4097,.2));
    DopModulationStream stream;std::vector<int32_t> out{12345};std::string why;
    bool accepted=stream.process(in,mods,0,true,out,&why);
    ck(!accepted&&!why.empty()&&out==std::vector<int32_t>{12345}&&stream.pending_bits()==0,"multilevel AHM rejected without mutating transport");
    double sum=0,signsum=0;SdmModulator real(c);for(size_t i=0;i<65536;++i){double y=real.process(.2);sum+=y;signsum+=y>0?1:-1;}
    std::printf("AHM_SIGN_LOSS levels=%d real_mean=%.12g sign_only_mean=%.12g rejected=%d\n",real.levels(),sum/65536,signsum/65536,!accepted);
    ck(std::fabs(sum/65536-.2)<.0001&&signsum/65536>.99,"known-bad sign conversion proves multilevel transport matters");
    std::printf("review4_dsd_contract checks=%d failures=%d\n",checks,fail);return fail?1:0;
}



