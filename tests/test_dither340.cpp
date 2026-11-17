// 3.4.0 regression: real quantizer samples, not option names or NTF formula alone.
// The shipped 3.3.18 shaped mode produces all-zero output for a 0.25-LSB input.
// These checks cover its audible consequence (lost low-level signal), noise color,
// independent channel seeds, block boundaries, and recovery after actual overload.
#include "dsp/dither.h"
#include "dsp/fft.h"
#include "engine/pipeline.h"
#include <array>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string>
#include <vector>
using namespace rta::dsp;
static int failures=0,checks=0;
static void check(bool ok,const std::string& text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text.c_str());}}
static constexpr std::array<Shaper,10> modes{Shaper::None,Shaper::NS1,Shaper::NS4,Shaper::NS5,Shaper::NS9,Shaper::LNS15,Shaper::RPDF,Shaper::TPDF,Shaper::Gauss1,Shaper::Shaped};
static void low_level(){
 const size_t n=262144;const double lsb=1./32768.;
 for(auto mode:{Shaper::RPDF,Shaper::TPDF,Shaper::Gauss1,Shaper::Shaped}){
  for(double input:{-.4,-.25,0.,.25,.4}){
   Quantizer a(mode,16,12345,96000.),b(Shaper::None,16,12345,96000.);
   double sum=0,sumE=0,sumE2=0;size_t differs=0;
   for(size_t i=0;i<n;++i){const double x=input*lsb,y=a.process(x),e=y/lsb-input;sum+=y/lsb;sumE+=e;sumE2+=e*e;differs+=y!=b.process(x);}
   const double mean=sum/n,var=sumE2/n-(sumE/n)*(sumE/n);
   std::printf("LOW mode=%s input=%.2f mean_lsb=%.8f variance_lsb2=%.8f differs_none=%zu/%zu\n",shaper_name(mode),input,mean,var,differs,n);
   check(std::abs(mean-input)<.008,std::string(shaper_name(mode))+" preserves sub-LSB mean");
   if(input!=0)check(differs>n/10,std::string(shaper_name(mode))+" is not rounding-only");
   if(mode==Shaper::TPDF||mode==Shaper::Shaped)check(std::abs(var-.25)<.008,std::string(shaper_name(mode))+" input-independent TPDF error variance");
  }
 }
}
static std::pair<double,double> spectral(Shaper mode){
 const size_t n=131072;const double lsb=1./32768.;Quantizer q(mode,16,417,96000.);
 std::vector<cplx> v(n);double mean=0;
 for(size_t i=0;i<n;++i){v[i]=q.process(.25*lsb)/lsb-.25;mean+=v[i].real();}
 mean/=n;for(auto& x:v)x-=mean;fft(v);
 double low=0,high=0;for(size_t i=1;i<n/10;++i)low+=std::norm(v[i]);
 for(size_t i=2*n/5+1;i<n/2;++i)high+=std::norm(v[i]);
 return {low,high};
}
static void spectrum_and_channels(){
 auto tp=spectral(Shaper::TPDF),sh=spectral(Shaper::Shaped);
 const double tilt=10*std::log10(sh.second/sh.first),gain=10*std::log10(tp.first/sh.first);
 std::printf("SPECTRUM shaped_high_over_low_db=%.5f lowband_gain_vs_TPDF_db=%.5f\n",tilt,gain);
 check(tilt>5.,"shaped output really moves noise to higher frequencies");
 check(gain>2.,"shaped low-band noise is lower than flat TPDF");
 for(auto mode:{Shaper::RPDF,Shaper::TPDF,Shaper::Gauss1,Shaper::Shaped}){
  Quantizer l(mode,24,12345,192000.),r(mode,24,12345+7919,192000.),same(mode,24,12345,192000.);
  const double step=l.step();const size_t n=131072;double sl=0,sr=0,sll=0,srr=0,slr=0;size_t bad=0;
  for(size_t i=0;i<n;++i){const double a=l.process(.25*step)/step-.25,b=r.process(.25*step)/step-.25;bad+=a!=(same.process(.25*step)/step-.25);sl+=a;sr+=b;sll+=a*a;srr+=b*b;slr+=a*b;}
  const double corr=(slr-sl*sr/n)/std::sqrt((sll-sl*sl/n)*(srr-sr*sr/n));
  std::printf("CHANNEL mode=%s correlation=%.8f same_seed_mismatches=%zu\n",shaper_name(mode),corr,bad);
  check(bad==0,"same seed is reproducible");check(std::abs(corr)<.02,std::string(shaper_name(mode))+" channels do not share correlated noise");
 }
}
static void blocks_and_recovery(){
 const size_t n=32768;
 for(auto mode:modes)for(double fs:{44100.,96000.,192000.,768000.})for(int bits:{16,24,32}){
  Quantizer whole(mode,bits,81,fs),blocked(mode,bits,81,fs);std::vector<double> x(n),reference(n);
  for(size_t i=0;i<n;++i){x[i]=.31*std::sin(2*kPi*997*i/fs);reference[i]=whole.process(x[i]);}
  size_t at=0,mismatch=0;for(size_t chunk:{1u,7u,256u,1003u,17u,2048u,16384u,65536u}){const size_t end=std::min(n,at+chunk);for(;at<end;++at)mismatch+=blocked.process(x[at])!=reference[at];}
  check(at==n&&mismatch==0,std::string(shaper_name(mode))+" arbitrary blocks preserve samples and histories");
  Quantizer q(mode,bits,23,fs);for(size_t i=0;i<2048;++i)q.process(std::sin(2*kPi*997*i/fs));
  const auto before=q.overload();q.process(2.);q.process(-2.);check(q.overload()>before,"actual out-of-range samples trigger overload");
  q.process(std::numeric_limits<double>::quiet_NaN());q.process(std::numeric_limits<double>::infinity());
  check(q.nonFinite()>=2,"NaN/Inf detected");
  Quantizer clean(mode,bits,23,fs);
  for(size_t i=0;i<4096;++i){const double a=.1*std::sin(2*kPi*997*i/fs);q.process(a);clean.process(a);}
  const auto settledOver=q.overload(),settledClamp=q.clamped();
  double xy=0,xx=0,yy=0,mean=0,cleanYY=0;size_t invalid=0,rails=0;
  for(size_t i=0;i<n;++i){const double a=.1*std::sin(2*kPi*997*(i+4096)/fs),y=q.process(a),cleanY=clean.process(a);xy+=a*y;xx+=a*a;yy+=y*y;mean+=y;cleanYY+=cleanY*cleanY;
   invalid+=!finite_bits(y)||y< -1.||y>1.-q.step()||y/q.step()!=std::round(y/q.step());rails+=std::abs(y)>=1.-q.step();}
  const double gain=xy/xx,rms=std::sqrt(yy/n),cleanRms=std::sqrt(cleanYY/n);
  // Existing independent binomial LNS15 at 16bit has substantial ultrasonic
  // noise (about 0.11 FS RMS at 192k+). Compare to a fresh unstressed instance,
  // not the input's total RMS: otherwise valid noise shaping looks like lockup.
  // Auto deliberately never selects LNS15 at 16bit; this is a known limitation.
  if(mode==Shaper::LNS15&&bits==16)std::printf("RECOVERY_LNS15_16BIT fs=%.0f gain=%.8f rms=%.8f clean_rms=%.8f mean=%.8f overload_delta=%lld clamp_delta=%lld\n",fs,gain,rms,cleanRms,mean/n,q.overload()-settledOver,q.clamped()-settledClamp);
  check(invalid==0,"recovered samples are finite bounded target-grid values");check(rails==0,"no recovered sample stuck at full scale");
  check(std::abs(gain-1)<.02,"recovered coherent signal amplitude within 2 percent");
  check(rms>.05&&rms<.25&&std::abs(rms-cleanRms)<cleanRms*.08+.001,"recovered RMS matches un-overloaded reference, not full-scale lock");
  check(q.overload()==settledOver&&q.clamped()==settledClamp,"recovered overload/clamp counters stop growing");
 }
 std::printf("BLOCK_RECOVERY cases=%zu samples_per_case=%zu all_offline=1\n",modes.size()*4*3,n);
}
static void policy(){
 for(auto mode:modes)for(int bits:{8,12,15,16,24,32})check(pcm_shaper_precision_supported(mode,bits)==(bits>=16||(mode!=Shaper::NS9&&mode!=Shaper::LNS15)),"manual low-precision guard rejects only NS9/LNS15 below the conservative 16-bit boundary");
 for(int bits=8;bits<16;++bits)for(double fs:{176400.,192000.,352800.,384000.,705600.,768000.,1411200.,1536000.})check(auto_pcm_shaper(bits,fs)==Shaper::TPDF,"auto 8..15-bit avoids high-order shaper overload at high rates");
 for(int bits:{8,16,24})for(double fs:{0.,44100.,48000.,88200.,96000.,128000.})check(auto_pcm_shaper(bits,fs)==Shaper::TPDF,"auto low rate uses TPDF");
 for(int bits:{16,24})for(double fs:{176400.,192000.})check(auto_pcm_shaper(bits,fs)==Shaper::NS9,"auto 176/192k uses NS9");
 for(double fs:{352800.,384000.,705600.,768000.,1411200.,1536000.}){for(int bits:{16,18,20,23,24})check(auto_pcm_shaper(bits,fs)==Shaper::NS9,"auto high-rate avoids LNS15 ultrasonic cost; manual LNS15 remains available");check(auto_pcm_shaper(32,fs)==Shaper::None,"auto 32-bit retains no-shaping policy");}
 check(auto_pcm_shaper(24,std::numeric_limits<double>::quiet_NaN())==Shaper::TPDF,"unknown rate falls back to TPDF");
 check(shaper_recommended_min_rate(Shaper::Shaped)==88200.,"shared shaped recommendation metadata");
}
static void pipeline_path(){
 for(int bits:{16,24,32}){
  rta::audio::AudioBuffer input;input.rate=96000;input.channels=2;input.bits=32;input.isFloat=true;input.raw.resize(65536*2);
  const double lsb=std::ldexp(1.,1-bits);for(size_t i=0;i<input.frames();++i)for(int c=0;c<2;++c)input.setSample(i,c,.25*lsb);
  rta::engine::PipelineConfig cfg;cfg.outBits=bits;cfg.shaper=Shaper::Shaped;cfg.gpuResample=false;
  rta::engine::Pipeline whole(cfg),blocked(cfg),floating(cfg);rta::engine::PipelineStats stats;
  auto all=whole.process(input,&stats);std::vector<int32_t> parts;size_t at=0;
  while(at<input.frames()){const size_t count=std::min(input.frames()-at,1+(at*43+137)%2003);auto block=input;block.raw.assign(input.raw.begin()+at*2,input.raw.begin()+(at+count)*2);const auto output=blocked.process(block);parts.insert(parts.end(),output.raw.begin(),output.raw.end());at+=count;}
  check(all.raw==parts,"real PCM pipeline preserves shaped history across arbitrary blocks");
  check(!stats.bitPerfect&&all.bits==bits&&all.frames()==input.frames(),"real shaped pipeline reports processing and target format");
  double left=0,right=0;size_t unequal=0;for(size_t i=0;i<all.frames();++i){left+=all.raw[i*2];right+=all.raw[i*2+1];unequal+=all.raw[i*2]!=all.raw[i*2+1];}
  check(std::abs(left/all.frames()-.25)<.015&&std::abs(right/all.frames()-.25)<.015,"real shaped pipeline preserves sub-LSB stereo content");
  check(unequal>all.frames()/8,"real shaped pipeline uses independent channel seeds");
  std::vector<std::vector<double>> planes;const auto noPcm=floating.process(input,nullptr,&planes);bool exact=noPcm.raw.empty()&&planes.size()==2&&planes[0].size()==input.frames();
  for(const auto& channel:planes)for(double v:channel)exact&=v==.25*lsb;
  check(exact,"DSD pre-quantizer double path does not inherit PCM dither");
  std::printf("PIPELINE bits=%d frames=%zu L_mean_lsb=%.8f R_mean_lsb=%.8f unequal_channels=%zu\n",bits,all.frames(),left/all.frames(),right/all.frames(),unequal);
 }
}
static void history_reference(){
 // Frozen circular-history recurrence checks the optimization independently:
 // the production path now shifts a linear history without any modulo divide.
 size_t compared=0,mismatches=0;
 for(auto mode:{Shaper::NS1,Shaper::NS4,Shaper::NS5,Shaper::NS9,Shaper::LNS15})for(int bits:{16,24,32})for(double fs:{44100.,96000.,192000.,768000.}){
  Quantizer q(mode,bits,12345,fs);const auto h=q.ntf();std::vector<double> errors(h.size(),0.);size_t at=0;long long over=0;
  for(size_t n=0;n<32768;++n){double x=.73*std::sin(2*kPi*997*n/fs);if(n==16384)x=2.;double u=x;for(size_t k=1;k<h.size();++k)u+=h[k]*errors[(at+h.size()-k)%h.size()];
   if(u>1.-q.step()){u=1.-q.step();++over;}if(u< -1.){u=-1.;++over;}const double y=q.step()*std::round(u/q.step());errors[at]=y-u;at=(at+1)%h.size();mismatches+=q.process(x)!=y;++compared;}
  check(q.overload()==over,"linear history optimization preserves overload counter");
 }
 check(mismatches==0,"linear history optimization is sample-exact against old circular recurrence");
 std::printf("HISTORY_REFERENCE samples=%zu mismatches=%zu\n",compared,mismatches);
}
int main(){low_level();spectrum_and_channels();blocks_and_recovery();policy();pipeline_path();history_reference();std::printf("DITHER340 checks=%d failures=%d\n",checks,failures);return failures?1:0;}
