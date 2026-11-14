// Production SdmModulator support/recovery matrix. No device or output sink.
#include "dsp/modulator.h"
#include "dsp/fft.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
using namespace rta::dsp;
static std::string quote(const std::string& s){std::string o="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){o+='\\';o+=c;}else if(c=='\n')o+="\\n";else if(c=='\r')o+="\\r";else if(c=='\t')o+="\\t";else if(c>=32)o+=c;}return o+'"';}
struct Metrics{double mean=0,rawRms=0,blockRms=0,sameSign=0,coherentRms=0,bandResidualRms=0,maxSpurRms=0;bool finite=true,binary=true;};
template<class Modulator,class Input> Metrics measure(Modulator& m,size_t count,Input input,double* expected=nullptr,double cycles=0,double sampleRate=0){
    std::vector<cplx> spectrum;if(cycles>0&&sampleRate>0)spectrum.resize(count);
    Metrics r;double sum=0,sq=0,bs=0,bx=0,blocks2=0,xblocks2=0,prior=0,ys=0,yc=0;size_t same=0;
    for(size_t i=0;i<count;++i){double x=input(i),y=m.process(x);r.finite&=std::isfinite(y);r.binary&=(y==-1||y==1);sum+=y;sq+=y*y;bs+=y;bx+=x;if(i)same+=(y>=0)==(prior>=0);prior=y;if(!spectrum.empty())spectrum[i]=y*(.5-.5*std::cos(2*kPi*i/count));
        if(cycles>0){const double phase=2*kPi*cycles*i/count;ys+=y*std::sin(phase);yc+=y*std::cos(phase);}
        if((i+1)%256==0){blocks2+=bs*bs;xblocks2+=bx*bx;bs=bx=0;}}
    r.coherentRms=std::hypot(ys,yc)*std::sqrt(2.)/count;r.mean=sum/count;r.rawRms=std::sqrt(sq/count);r.blockRms=std::sqrt(blocks2/(count/256))/256;r.sameSign=double(same)/(count-1);
    if(!spectrum.empty()){
        fft(spectrum);double residual=std::norm(spectrum[0])/double(count)/count;
        const size_t last=std::min(count/2,size_t(std::floor(20000.*count/sampleRate)));
        for(size_t k=1;k<=last;++k)if(std::abs(double(k)-cycles)>1.){const double power=2.*std::norm(spectrum[k])/double(count)/count;residual+=power;r.maxSpurRms=std::max(r.maxSpurRms,std::sqrt(power)/.5);}
        r.bandResidualRms=std::sqrt(residual/.375);
    }
    if(expected)*expected=std::sqrt(xblocks2/(count/256))/256;return r;
}
static bool silence_shape(const Metrics& m){return m.finite&&std::abs(m.mean)<.05&&m.sameSign<.90&&m.blockRms<.025;}
static bool recovered_audio(const Metrics& m,double expected){return m.finite&&std::abs(m.coherentRms-expected)<.002&&m.bandResidualRms<.002;}
static void metric_json(std::ostream& f,const Metrics& m){f<<"{\"mean\":"<<m.mean<<",\"raw_rms\":"<<m.rawRms<<",\"block256_rms\":"<<m.blockRms<<",\"same_sign_rate\":"<<m.sameSign<<",\"coherent_rms\":"<<m.coherentRms<<",\"audio_20k_residual_rms\":"<<m.bandResidualRms<<",\"audio_20k_max_spur_rms\":"<<m.maxSpurRms<<",\"finite\":"<<(m.finite?"true":"false")<<",\"binary\":"<<(m.binary?"true":"false")<<'}';}
int main(int argc,char**argv){
    std::string output="dsd-production-support.json";for(int i=1;i<argc;++i){if(std::string(argv[i])=="--report"&&i+1<argc)output=argv[++i];else{std::cerr<<"unknown argument\n";return 2;}}
    std::ofstream f(output,std::ios::binary|std::ios::trunc);if(!f)return 2;f<<std::setprecision(17);
    f<<"{\"schema\":1,\"scope\":\"actual production SdmModulator, offline only; cutoff20k and100k are distinct designs\",\"sample_count_per_measurement\":65536,\"box_average\":256,\"audio_residual_window\":\"periodic Hann; 0-20kHz; exclude coherent tone plus adjacent bins; energy normalized by 3/8; not wideband RMS\",\"cells\":[";
    int checks=0,failures=0,cells=0;auto check=[&](bool v,const char* why){++checks;if(!v){++failures;std::cout<<"FAIL cell="<<cells<<' '<<why<<'\n';}};
    Metrics rail;rail.mean=1;rail.rawRms=rail.blockRms=1;rail.sameSign=1;
    Metrics slowSquare;slowSquare.mean=0;slowSquare.rawRms=slowSquare.blockRms=1;slowSquare.sameSign=.999;
    check(!silence_shape(rail)&&!silence_shape(slowSquare),"predicate rejects stuck DC and zero-mean rail square counterexamples");
    constexpr size_t N=65536;const double reference=.62/std::sqrt(2.);
    struct Identity {double process(double x){return x;}} identity;
    const double controlFs=49152000.;auto toneInput=[&](size_t i){return .62*std::sin(2*kPi*13*i/N);};
    auto good=measure(identity,N,toneInput,nullptr,13.,controlFs);
    auto attenuated=measure(identity,N,[&](size_t i){return .5*toneInput(i);},nullptr,13.,controlFs);
    auto spurious=measure(identity,N,[&](size_t i){return toneInput(i)+.02*std::sin(2*kPi*26*i/N);},nullptr,13.,controlFs);
    auto locked=measure(identity,N,[](size_t){return 1.;},nullptr,13.,controlFs);
    auto ultrasonic=measure(identity,N,[&](size_t i){return toneInput(i)+.5*std::sin(2*kPi*1000*i/N);},nullptr,13.,controlFs);
    check(recovered_audio(good,reference),"independent known-good pure tone passes audio recovery predicate");
    check(!recovered_audio(attenuated,reference),"recovery predicate rejects deliberately halved signal");
    check(!recovered_audio(spurious,reference),"recovery predicate rejects an added in-band second harmonic");
    check(!recovered_audio(locked,reference),"recovery predicate rejects locked DC");
    check(recovered_audio(ultrasonic,reference),"audio predicate does not misclassify purely out-of-band energy as audio error");
    bool first=true;
    for(unsigned base:{44100u,48000u})for(unsigned factor:{64u,128u,256u,512u,1024u})for(int order:{5,7})for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Ahm})for(double cutoff:{20000.,100000.}){
        ++cells;const double fs=double(base)*factor;SdmConfig c;c.fs=fs;c.order=order;c.cutoffHz=cutoff;c.family=family;c.ahmBits=8;c.oneBitOutput=true;c.ditherLsb=.0005;
        const int before=failures;SdmModulator m(c);const double frequency=13.*fs/N;
        auto full=measure(m,N,[&](size_t i){return std::sin(2*kPi*frequency*i/fs);});m.process(5.);m.process(-5.);
        const auto burstOv=m.overload(),burstClamp=m.clamped();
        measure(m,N,[](size_t){return .2;});auto positive=measure(m,N,[](size_t){return .2;});
        measure(m,N,[](size_t){return -.2;});auto negative=measure(m,N,[](size_t){return -.2;});
        measure(m,2*N,[](size_t){return 0.;});const auto ov=m.overload(),cl=m.clamped(),nf=m.nonFinite();
        auto silence=measure(m,N,[](size_t){return 0.;});const auto silenceOv=m.overload()-ov,silenceCl=m.clamped()-cl;
        measure(m,N,[&](size_t i){return .62*std::sin(2*kPi*frequency*i/fs);});const auto toneOv=m.overload(),toneCl=m.clamped();double expected=0;
        auto tone=measure(m,N,[&](size_t i){return .62*std::sin(2*kPi*frequency*i/fs);},&expected,13.,fs);
        check(m.levels()==2&&full.binary&&silence.binary&&tone.binary,"one-bit output is genuinely binary");
        check(!m.steppedDown()||(!m.stepDownReason().empty()&&m.effectiveOrder()<order),"actual order guard has explicit protection reason");
        check(family!=ModulatorFamily::Ahm||(m.levels_clamped()&&!m.levels_clamp_reason().empty()),"one-bit AHM downgrade never masquerades as multilevel");
        check(full.finite&&positive.finite&&negative.finite&&tone.finite&&m.nonFinite()==0&&nf==0,"fullscale/impulses/DC/recovery remain finite without nonfinite input repair");
        check(std::abs(positive.mean-.2)<.002&&std::abs(negative.mean+.2)<.002,"both DC polarities retain actual mean instead of sign collapse");
        check(silence_shape(silence)&&silenceOv==0&&silenceCl==0,"post-overload silence recovers without continuing limits or rail locks");
        check(recovered_audio(tone,reference)&&m.overload()==toneOv&&m.clamped()==toneCl,"nonzero coherent I/Q tone returns without ultrasonic-noise RMS bias and no continuing limits");
        if(!first)f<<',';first=false;f<<"{\"base_rate\":"<<base<<",\"dsd_factor\":"<<factor<<",\"rate\":"<<uint64_t(fs)<<",\"family\":"<<quote(modulator_family_name(family))<<",\"cutoff_hz\":"<<cutoff<<",\"one_bit_output\":true,\"requested_order\":"<<order<<",\"effective_order\":"<<m.effectiveOrder()<<",\"sections\":"<<(m.uses_sections()?"true":"false")<<",\"step_down_reason\":"<<quote(m.stepDownReason())<<",\"requested_levels\":"<<(family==ModulatorFamily::Ahm?256:2)<<",\"effective_levels\":"<<m.levels()<<",\"levels_clamp_reason\":"<<quote(m.levels_clamp_reason())<<",\"fullscale\":";metric_json(f,full);
        f<<",\"after_burst_overload\":"<<burstOv<<",\"after_burst_clamped\":"<<burstClamp<<",\"positive_dc\":";metric_json(f,positive);f<<",\"negative_dc\":";metric_json(f,negative);f<<",\"recovered_silence\":";metric_json(f,silence);f<<",\"silence_overload_delta\":"<<silenceOv<<",\"silence_clamp_delta\":"<<silenceCl<<",\"recovered_tone\":";metric_json(f,tone);
        f<<",\"expected_coherent_tone_rms\":"<<.62/std::sqrt(2.)<<",\"expected_filtered_tone_rms\":"<<expected<<",\"tone_overload_delta\":"<<m.overload()-toneOv<<",\"tone_clamp_delta\":"<<m.clamped()-toneCl<<",\"failures\":"<<failures-before<<'}';f.flush();
        std::cout<<"CELL "<<cells<<" base="<<base<<" factor="<<factor<<" family="<<modulator_family_name(family)<<" cutoff="<<cutoff<<" requested="<<order<<" effective="<<m.effectiveOrder()<<" failures="<<failures-before<<'\n';
    }
    f<<"],\"cell_count\":"<<cells<<",\"checks\":"<<checks<<",\"failures\":"<<failures<<"}\n";f.close();if(!f)return 2;
    std::cout<<"checks="<<checks<<" failures="<<failures<<" cells="<<cells<<" audio_device_calls=0\n";return failures?1:0;
}
