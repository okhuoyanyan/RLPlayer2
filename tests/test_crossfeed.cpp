// Public, independent transfer-function checks plus actual Pipeline paths.
// No audio device is opened. Enabled crossfeed is intentionally NOT transparent;
// only Off is bit-exact. Mono DC and mono AC have different documented gains.
#include "dsp/crossfeed.h"
#include "engine/pipeline.h"
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
using namespace rta::dsp;
static int checks=0,failures=0;
static void ck(bool ok,const char* s){++checks;if(!ok){++failures;std::printf("FAIL %s\n",s);}}
template<class F>void rejects(F action,const char* s){bool failed=false;try{action();}catch(const std::invalid_argument&){failed=true;}ck(failed,s);}
static void bypass_validation(){
 std::vector<double> l{0.,-0.,1.,-1.,std::numeric_limits<double>::quiet_NaN()},r=l,ol=l,orr=r;
 Crossfeed off;off.process(l.data(),r.data(),l.size());ck(std::memcmp(l.data(),ol.data(),l.size()*8)==0&&std::memcmp(r.data(),orr.data(),r.size()*8)==0,"Off preserves sample bits including negative zero/NaN");
 ck(off.tail_frames()==0&&off.cross_delay_seconds()==0,"Off has no history tail or delay");
 for(double cutoff:{299.,1201.,std::numeric_limits<double>::quiet_NaN()})rejects([&]{Crossfeed x({true,cutoff,12},44100);},"invalid cutoff rejected");
 for(double feed:{2.99,15.01,std::numeric_limits<double>::infinity()})rejects([&]{Crossfeed x({true,700,feed},44100);},"invalid feed rejected");
 rejects([]{Crossfeed x({true,700,12},0);},"invalid sample rate rejected");
}
static void response(){
 constexpr double pi=3.14159265358979323846;
 for(double fs:{44100.,48000.,96000.,192000.,768000.,1536000.})for(double feed:{6.,12.}){
  Crossfeed f({true,700,feed},fs);const size_t n=static_cast<size_t>(fs*.03);std::vector<double> l(n,0.),r(n,0.);l[0]=1.;f.process(l.data(),r.data(),n);
  const double pole=std::exp(-2*pi*700/fs),alpha=1-pole,cross=1/(1+std::pow(10.,feed/20)),direct=1-cross;
  double mass=0,moment=0;bool impulse=true;for(size_t i=0;i<n;++i){impulse&=std::abs(r[i]-cross*alpha*std::pow(pole,double(i)))<2e-15;impulse&=l[i]==(i==0?direct:0.);mass+=r[i];moment+=i*r[i];}
  ck(impulse,"one-sided impulse matches independent exponential oracle");ck(std::abs(mass-cross)<1e-12,"cross path DC feed level correct");ck(std::abs(moment/mass/fs-f.cross_delay_seconds())<1e-12,"cross path natural low-frequency delay correct");
  double mono20=0,cross100=0,cross10000=0;
  for(double hz:{100.,700.,10000.,20000.}){
   std::complex<double> measured{0,0};for(size_t i=0;i<n;++i)measured+=r[i]*std::polar(1.,-2*pi*hz*i/fs);
   const auto expected=cross*alpha/(1.-pole*std::polar(1.,-2*pi*hz/fs));
   ck(std::abs(measured-expected)<1e-12,"measured cross transfer equals independent z-transform");
   if(hz==100)cross100=std::abs(measured);if(hz==10000)cross10000=std::abs(measured);if(hz==20000)mono20=std::abs(direct+measured);
  }
  ck(20*std::log10(cross100/cross10000)>20.,"opposite ear receives mostly low frequencies");
  ck(mono20<1&&mono20>direct*.99,"enabled mono AC gain honestly differs from DC unity");
  std::printf("RESPONSE fs=%.0f feed_db=%.0f cross_dc=%.9f delay_us=%.5f mono20k_db=%.5f tail_frames=%zu\n",fs,feed,cross,f.cross_delay_seconds()*1e6,20*std::log10(mono20),f.tail_frames());
 }
}
static void bounds_blocks_reset(){
 for(double fs:{44100.,192000.,1536000.})for(double cutoff:{300.,700.,1200.})for(double feed:{3.,6.,12.,15.}){
  CrossfeedConfig cfg{true,cutoff,feed};const size_t n=32768;std::vector<double> l(n),r(n);uint64_t state=398171;
  for(size_t i=0;i<n;++i){state^=state<<13;state^=state>>7;state^=state<<17;l[i]=(state&1)?1.:-1.;r[i]=(state&2)?1.:-1.;if(i>n-4096)l[i]=r[i]=1.;}
  auto bl=l,br=r,sl=r,sr=l;Crossfeed whole(cfg,fs),blocks(cfg,fs),swap(cfg,fs);whole.process(l.data(),r.data(),n);swap.process(sl.data(),sr.data(),n);
  size_t at=0;while(at<n){const size_t count=std::min(n-at,1+(at*71+17)%1337);blocks.process(bl.data()+at,br.data()+at,count);at+=count;}
  ck(l==bl&&r==br,"arbitrary block boundaries are sample-exact");ck(l==sr&&r==sl,"left/right symmetry is sample-exact");
  double peak=0;for(size_t i=0;i<n;++i)peak=std::max({peak,std::abs(l[i]),std::abs(r[i])});ck(peak<=1.+1e-14,"bounded full-scale adversarial stereo adds no overload");
  std::vector<double> dc(static_cast<size_t>(fs*.05),1.),dr=dc;Crossfeed mono(cfg,fs);mono.process(dc.data(),dr.data(),dc.size());ck(std::abs(dc.back()-1)<1e-12&&dc==dr,"dual-mono DC reaches unity symmetrically");
  whole.reset();std::vector<double> z(1024,0.),zr=z;whole.process(z.data(),zr.data(),z.size());ck(std::all_of(z.begin(),z.end(),[](double x){return x==0;})&&z==zr,"reset removes previous track history");
 }
 Crossfeed f({true,700,12},48000);double l=2.,r=-2.;f.process(&l,&r,1);l=std::numeric_limits<double>::quiet_NaN();r=std::numeric_limits<double>::infinity();f.process(&l,&r,1);ck(l==0&&r==0&&f.non_finite()==2,"nonfinite stereo input is sanitized without poisoning IIR");
 std::vector<double> l2(4800,.1),r2(4800,-.1);f.process(l2.data(),r2.data(),l2.size());const double expected=.1*(f.direct_gain()-f.cross_gain());ck(std::abs(l2.back()-expected)<1e-13&&std::abs(r2.back()+expected)<1e-13,"overshoot/nonfinite transient recovers expected steady state");
}
static rta::audio::AudioBuffer fixture(size_t frames=8192){
 rta::audio::AudioBuffer in;in.rate=48000;in.bits=24;in.channels=2;in.raw.resize(frames*2);for(size_t i=0;i<frames;++i){in.raw[i*2]=static_cast<int32_t>(2000000*std::sin(2*kPi*997*i/in.rate));in.raw[i*2+1]=static_cast<int32_t>(1000000*std::cos(2*kPi*173*i/in.rate));}return in;
}
static void pipeline(){
 using namespace rta::engine;auto in=fixture();PipelineConfig cfg;cfg.gpuResample=false;Pipeline off(cfg);PipelineStats stats;auto bypass=off.process(in,&stats);ck(bypass.raw==in.raw&&stats.bitPerfect&&!stats.usedCrossfeed,"default Pipeline remains bit-perfect");
 cfg.crossfeed.enabled=true;Pipeline p(cfg),split(cfg),fresh(cfg);auto out=p.process(in,&stats);ck(!stats.bitPerfect&&stats.usedCrossfeed&&out.raw!=in.raw,"actual Pipeline executes opt-in crossfeed and reports it");
 std::vector<int32_t> joined;for(size_t at=0;at<in.frames();){const size_t n=std::min(in.frames()-at,at%991+1);auto b=in;b.raw.assign(in.raw.begin()+at*2,in.raw.begin()+(at+n)*2);auto o=split.process(b);joined.insert(joined.end(),o.raw.begin(),o.raw.end());at+=n;}ck(joined==out.raw,"real Pipeline crossfeed chunking equals whole input");
 p.seek_reset();ck(p.process(in).raw==fresh.process(in).raw,"real Pipeline seek clears crossfeed histories");
 Pipeline rawDouble(cfg);std::vector<std::vector<double>> planes;auto empty=rawDouble.process(in,&stats,&planes);ck(empty.raw.empty()&&stats.usedCrossfeed&&planes.size()==2,"PCM-to-DSD double path applies crossfeed before quantization");
 auto bad=cfg;bad.forceBitPerfect=true;Pipeline forced(bad);ck(!forced.willBeBitPerfect(in),"forced bit-perfect cannot claim crossfeed is transparent");rejects([&]{forced.process(in);},"forced bit-perfect and crossfeed conflict rejected");
 auto mono=in;mono.channels=1;rejects([&]{Pipeline x(cfg);x.process(mono);},"mono input cannot silently skip stereo feature");bad=cfg;bad.outChannels=6;rejects([&]{Pipeline x(bad);x.process(in);},"multichannel output cannot silently skip stereo feature");
 auto last=fixture(1);last.raw={4000000,0};Pipeline tail(cfg);std::vector<std::vector<double>> head;tail.process(last,nullptr,&head);ck(tail.algorithmic_latency_out_frames()==0,"crossfeed direct path has no removable delay");const size_t count=static_cast<size_t>(std::ceil(tail.crossfeed_tail_out_frames()));std::vector<std::vector<double>> rest;tail.drain_chunk(count,&rest);ck(count>0&&rest.size()==2&&rest[1].front()>0&&std::abs(rest[1].back())<1e-12,"actual pipeline drains crossfeed tail below -240 dBFS");
 std::printf("PIPELINE frames=%zu bitperfect_off=1 chunk_exact=1 seek_exact=1 tail_frames=%zu tail_last=%.5g\n",in.frames(),count,rest[1].back());
}
int main(){bypass_validation();response();bounds_blocks_reset();pipeline();std::printf("CROSSFEED checks=%d failures=%d\n",checks,failures);return failures?1:0;}
