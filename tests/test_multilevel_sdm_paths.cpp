// Four actual production paths, including 3-section and 4-section realizations.
// Pure in-memory numbers: no driver, sink, network, user profile or real library.
#include "dsp/modulator.h"
#include <vector>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace rta::dsp;
static size_t checks=0,failures=0,cases=0;
void ck(bool v,const char* label){++checks;if(!v){++failures;if(failures<25)printf("FAIL case=%zu %s\n",cases,label);}}
void state(const SdmModulator&a,const SdmModulator&b){ck(a.overload()==b.overload()&&a.clamped()==b.clamped()&&a.nonFinite()==b.nonFinite()&&a.effectiveOrder()==b.effectiveOrder()&&a.levels()==b.levels(),"observable counters/order/levels match");}
struct Abort{};
void run(SdmConfig cfg){
 ++cases;SdmConfig right=cfg;right.ditherLsb=cfg.ditherLsb?0.:.0005;
 SdmModulator a(cfg),b(right),ordinary(cfg),binary(cfg),left(cfg),r(right);
 constexpr size_t n=8192;std::vector<double>x(n),z(n),ref(n),rz(n);
 for(size_t i=0;i<n;++i){x[i]=.31*std::sin(.013*i)+.07;z[i]=.19*std::cos(.017*i)-.03;}
 x[0]=5.;x[1]=-5.;z[6]=1.;z[7]=-1.;x[511]=std::numeric_limits<double>::quiet_NaN();x[512]=std::numeric_limits<double>::infinity();x[513]=-std::numeric_limits<double>::infinity();z[1001]=std::numeric_limits<double>::quiet_NaN();
 for(size_t i=0;i<n;++i){ref[i]=a.process(x[i]);rz[i]=b.process(z[i]);}
 size_t cursor=0,part=0;const size_t sizes[]={1,7,31,1024,333};
 size_t empty=0;ordinary.process_block(nullptr,0,[&](size_t,double){++empty;});binary.process_binary_block(nullptr,0,[&](size_t,double){++empty;});ck(!empty,"empty blocks do not consume input");
 while(cursor<n){size_t take=std::min(n-cursor,sizes[part++%5]);
  ordinary.process_block(x.data()+cursor,take,[&](size_t i,double y){ck(i<take&&y==ref[cursor+i],"ordinary block equals scalar sample");});
  binary.process_binary_block(x.data()+cursor,take,[&](size_t i,double y){ck(i<take&&y==ref[cursor+i],"binary block equals scalar sample");});
  bool ok=SdmModulator::try_process_binary_stereo_bits_block(left,r,x.data()+cursor,z.data()+cursor,take,[&](size_t i,bool l,bool rr){ck(i<take&&(l?1.:-1.)==ref[cursor+i]&&(rr?1.:-1.)==rz[cursor+i],"stereo both lanes equal independent scalar samples");});
  ck(ok,"actual 5/7 order stereo path accepted");cursor+=take;
 }
 state(a,ordinary);state(a,binary);state(a,left);state(b,r);ck(a.nonFinite()==3&&b.nonFinite()==1,"nonfinite input counters survive all paths");
 for(size_t i=0;i<1024;++i){double xx=.11*std::sin(.031*i);double v=a.process(xx);ck(ordinary.process(xx)==v&&binary.process(xx)==v&&left.process(xx)==v,"suffix exposes identical hidden state");}
 // Copy + reset preserve the established RNG contract: reset histories/counters,
 // not the already-advanced dither sequence. Equal RNG positions remain exact.
 SdmModulator ra(cfg),rb(cfg);for(size_t i=0;i<1024;++i){ra.process(x[i]);rb.process(-.2);}ra.reset();rb.reset();
 for(size_t i=0;i<1024;++i)ck(ra.process(.15)==rb.process(.15),"reset clears differing histories at identical RNG position");
 for(int mode:{0,1,2}){
  SdmModulator m(cfg),mr(right);bool caught=false;constexpr size_t stop=512;
  try{if(mode==0)m.process_block(x.data(),n,[&](size_t i,double){if(i==stop)throw Abort{};});
   else if(mode==1)m.process_binary_block(x.data(),n,[&](size_t i,double){if(i==stop)throw Abort{};});
   else SdmModulator::try_process_binary_stereo_bits_block(m,mr,x.data(),z.data(),n,[&](size_t i,bool,bool){if(i==stop)throw Abort{};});
  }catch(const Abort&){caught=true;}ck(caught,"sink exception propagated");
  for(size_t i=stop+1;i<n;++i){ck(m.process(x[i])==ref[i],"throw commits current sample and resumes exact mono history");if(mode==2)ck(mr.process(z[i])==rz[i],"throw commits exact right channel history");}
 }
 printf("CASE %zu fs=%.0f cutoff=%.0f requested=%d effective=%d sections=%d dither=%g failures=%zu\n",cases,cfg.fs,cfg.cutoffHz,cfg.order,a.effectiveOrder(),a.ntf().sections.count,cfg.ditherLsb,failures);
}
int main(){
 for(unsigned base:{44100u,48000u})for(unsigned factor:{64u,128u,256u,512u,1024u})for(int order:{5,7})for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Ahm})for(double cutoff:{20000.,100000.})for(double dither:{0.,.0005}){SdmConfig c;c.fs=double(base)*factor;c.order=order;c.family=family;c.oneBitOutput=true;c.cutoffHz=cutoff;c.ditherLsb=dither;run(c);}
 SdmConfig c;c.fs=49152000;c.cutoffHz=20000;c.order=5;c.oneBitOutput=true;SdmModulator a(c);c.cutoffHz=100000;SdmModulator b(c);auto aa=a,bb=b;size_t called=0;
 ck(!SdmModulator::try_process_binary_stereo_bits_block(a,b,nullptr,nullptr,5,[&](size_t,bool,bool){++called;})&&!called,"heterogeneous section/polynomial pair rejects without reading input");
 for(int i=0;i<1000;++i){ck(a.process(.1)==aa.process(.1)&&b.process(-.1)==bb.process(-.1),"rejected pair state remains unconsumed");}
 printf("RESULT cases=%zu checks=%zu failures=%zu audio_device_calls=0\n",cases,checks,failures);return failures?1:0;
}
