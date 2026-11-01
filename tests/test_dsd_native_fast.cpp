// Full-byte/stat differential oracle; no audio device and no speed measurements.
#include "dsp/dsd_rate_convert.h"
#include "dsp/parallel.h"
#include "fixtures/dsd_native_scalar_reference.h"
#include <bit>
#include <cstdio>
#include <limits>
using namespace rta::dsp;
using rta::audio::DsdStream;
static size_t checks=0,failures=0,bytes=0,cases=0;
static void ck(bool pass,const char* what){++checks;if(!pass){++failures;std::printf("FAIL %s\n",what);}}
static void compare(const DsdStream&a,const DsdStream&b,const DsdRateStats&x,const DsdRateStats&y){
 ck(a.rate==b.rate&&a.channels==b.channels&&a.bits==b.bits&&a.container==b.container,"stream metadata");
 ck(a.data==b.data,"complete bytes including unused tail bits");for(const auto&c:a.data)bytes+=c.size();
 ck(x.overload==y.overload&&x.clamped==y.clamped&&x.nonFinite==y.nonFinite&&x.outBits==y.outBits&&x.pcmFrames==y.pcmFrames,"all integer stats");
 ck(std::bit_cast<uint64_t>(x.inPeak)==std::bit_cast<uint64_t>(y.inPeak)&&std::bit_cast<uint64_t>(x.gainDb)==std::bit_cast<uint64_t>(y.gainDb),"bitexact peak and gain");
 ck(x.effectiveOrder==y.effectiveOrder&&x.steppedDown==y.steppedDown&&x.stepDownReason==y.stepDownReason,"effective N and guard stats");
}
static DsdStream source(uint32_t rate,int ch,size_t n){
 DsdStream s;s.rate=rate;s.channels=ch;s.bits=n;s.container="fixture";s.data.assign(ch,std::vector<uint8_t>((n+7)/8,0));
 uint32_t rng=12345;
 for(int c=0;c<ch;++c)for(size_t i=0;i<n;++i){rng=rng*1664525u+1013904223u;bool v=i<n/3?((i/31+c)&1):i<2*n/3?(rng>>31):((i&1)!=0);if(v)s.setBit(c,i,true);}
 return s;
}
static DsdStream slice(const DsdStream&s,size_t pos,size_t count){
 DsdStream q;q.rate=s.rate;q.channels=s.channels;q.bits=count;q.data.assign(s.channels,std::vector<uint8_t>((count+7)/8,0));
 for(int c=0;c<s.channels;++c)for(size_t i=0;i<count;++i)if((s.data[c][(pos+i)/8]>>(7-((pos+i)&7)))&1)q.setBit(c,i,true);
 return q;
}
static void bits_case(int src,int dst,int channels,ModulatorFamily family=ModulatorFamily::Dsd,int order=5,uint32_t base=44100){
 auto plan=plan_dsd_rate(base*src,base*dst);DsdRateConvertConfig cfg;cfg.modFamily=family;cfg.modOrder=order;
 DsdRateConverter current(plan,cfg,channels);DsdRateConverterScalarReference old(plan,cfg,channels);
 const auto in=source(base*src,channels,9001),saved=in;
 for(int reset=0;reset<2;++reset){
  DsdStream a,b;a.bits=b.bits=16;a.data.assign(channels,std::vector<uint8_t>{0xa5,0x3c});b.data=a.data;
  if(reset){current.reset();old.reset();}
  size_t pos=0,block=0;const size_t sizes[]={1,7,16,8191};
  while(pos<in.bits){const auto n=std::min<size_t>(sizes[block++%4],in.bits-pos);auto part=slice(in,pos,n),copy=part;
   ck(current.process_bits(part,a)==old.process_bits(part,b),"written bits each irregular source block");
   ck(part.data==copy.data&&part.bits==copy.bits,"source DSD is read only");compare(a,b,current.stats(),old.stats());pos+=n;
  }
  current.finish(a);old.finish(b);compare(a,b,current.stats(),old.stats());
  auto done=a.data;auto count=a.bits;current.finish(a);old.finish(b);compare(a,b,current.stats(),old.stats());ck(a.data==done&&a.bits==count,"finish idempotent");
  ck(a.data[0][0]==0xa5&&a.data[0][1]==0x3c,"existing output prefix preserved");
 }
 ck(in.data==saved.data,"whole source unchanged");++cases;
 std::printf("BITS src=%d dst=%d ch=%d family=%s N=%d bytes=%zu fails=%zu\n",src,dst,channels,modulator_family_name(family),order,bytes,failures);
}
static void pcm_case(uint32_t src,uint32_t dst,int channels,int order){
 auto plan=plan_dsd_rate(src,dst);DsdRateConvertConfig cfg;cfg.modFamily=ModulatorFamily::Dsd;cfg.modOrder=order;cfg.headroomAuto=false;cfg.preAttenDb=-1.25;
 DsdRateConverter current(plan,cfg,channels);DsdRateConverterScalarReference old(plan,cfg,channels);
 for(int reset=0;reset<2;++reset){
  if(reset){current.reset();old.reset();}
  DsdStream a,b;a.bits=b.bits=8;a.data.assign(channels,std::vector<uint8_t>{0x97,0xff});b.data=a.data;
  size_t pos=0;const size_t n=9001;std::vector<std::vector<double>> pcm(channels,std::vector<double>(n));
  for(int c=0;c<channels;++c)for(size_t i=0;i<n;++i){double v=i<n/4?std::sin(.03*i+c):i<n/2?1.:0.;if(i==5)v=5.;if(i==9)v=-5.;if(i==17)v=std::numeric_limits<double>::quiet_NaN();if(i==18)v=std::numeric_limits<double>::infinity();if(i==19)v=-std::numeric_limits<double>::infinity();pcm[c][i]=v;}
  const size_t sizes[]={1,7,16,8191};size_t block=0;
  while(pos<n){size_t count=std::min(sizes[block++%4],n-pos);std::vector<std::vector<double>> part(channels);for(int c=0;c<channels;++c)part[c].assign(pcm[c].begin()+pos,pcm[c].begin()+pos+count);auto saved=part;
   ck(current.push_pcm(part,count,a)==old.push_pcm(part,count,b),"PCM seam written bits");
   for(int c=0;c<channels;++c)for(size_t i=0;i<count;++i)ck(std::bit_cast<uint64_t>(part[c][i])==std::bit_cast<uint64_t>(saved[c][i]),"caller PCM unchanged including NaN");
   compare(a,b,current.stats(),old.stats());pos+=count;
  }
  ck(current.push_pcm(pcm,0,a)==old.push_pcm(pcm,0,b),"zero PCM no op");
  current.finish(a);old.finish(b);compare(a,b,current.stats(),old.stats());
  bool ca=false,cb=false;try{current.push_pcm(pcm,1,a);}catch(const std::runtime_error&){ca=true;}try{old.push_pcm(pcm,1,b);}catch(const std::runtime_error&){cb=true;}ck(ca&&cb,"post-finish rejection");
 }
 ++cases;std::printf("PCM src=%u dst=%u ch=%d N=%d fails=%zu\n",src,dst,channels,order,failures);
}
static void edge_sinks(){
 auto plan=plan_dsd_rate(44100u*128,44100u*64);DsdRateConvertConfig cfg;cfg.modFamily=ModulatorFamily::Dsd;cfg.modOrder=5;
 DsdRateConverter current(plan,cfg,2);DsdRateConverterScalarReference old(plan,cfg,2);
 DsdStream a,b;current.finish(a);old.finish(b);compare(a,b,current.stats(),old.stats());ck(a.bits==0,"empty finish adds no settling bits");
 current.reset();old.reset();
 a={};b={};a.bits=b.bits=8;a.data=b.data={{0xa7,0xff},{0xb3,0x3f}};
 std::vector<std::vector<double>> pcm(2,std::vector<double>(17,.125));
 current.push_pcm(pcm,17,a);old.push_pcm(pcm,17,b);compare(a,b,current.stats(),old.stats());
 // Preserve the old API's sink-switch behavior, including its ordering of
 // settling the current sink and subsequently aligning a different destination.
 DsdStream ca,cb;ca.bits=cb.bits=16;ca.data=cb.data={{0x12,0x34},{0x56,0x78}};
 current.finish(ca);old.finish(cb);compare(ca,cb,current.stats(),old.stats());
 compare(a,b,current.stats(),old.stats());
 current.finish(ca);old.finish(cb);compare(ca,cb,current.stats(),old.stats());
 ++cases;std::printf("EDGE sink_switch_empty_finish fails=%zu\n",failures);
}
int main(){
 // Run a no-resampler route first, so existing resampler worker use cannot
 // accidentally satisfy this Native-modulation integration assertion.
 pcm_case(44100u*128,44100u*64,2,5);
 if(std::thread::hardware_concurrency()>1)ck(ParallelFor::instance().helper_count()>0,"native modulation itself uses persistent helpers");
 for(int src:{64,128,256})for(int dst:{512,1024})for(int ch:{1,2,6})bits_case(src,dst,ch);
 bits_case(128,512,2,ModulatorFamily::Ec,7);bits_case(256,1024,2,ModulatorFamily::Dsd,5,48000);
 // Direct appends, including non-byte-aligned chunk boundaries, stay untouched.
 for(int ch:{1,2,6})bits_case(64,64,ch);
 // Downsampling has no resampled ownership: never multiply caller pcm in place.
 // It also generates partial output bytes and exposes exact NaN peak behavior.
 for(int ch:{1,2,6})pcm_case(44100u*128,44100u*64,ch,5);
 pcm_case(44100u*64,44100u*1024,2,3);
 edge_sinks();
 if(std::thread::hardware_concurrency()>1)ck(ParallelFor::instance().helper_count()>0,"native channels use persistent helpers");
 std::printf("RESULT cases=%zu checks=%zu compared_bytes=%zu fails=%zu\n",cases,checks,bytes,failures);
 return failures?1:0;
}
