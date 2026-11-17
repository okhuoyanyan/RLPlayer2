// Effective DAC resolution belongs to the final quantizer, not the ASIO/WASAPI
// container. These tests inspect actual packed samples and pre-DSD doubles.
#include "engine/pipeline.h"
#include <cstdio>
#include <limits>
using namespace rta;
static int checks=0,failures=0;
static void ck(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
template<class F>void rejects(F action,const char* what){bool failed=false;try{action();}catch(const std::invalid_argument&){failed=true;}ck(failed,what);}
static audio::AudioBuffer floating(size_t n,uint32_t rate=96000){audio::AudioBuffer a;a.rate=rate;a.channels=2;a.bits=32;a.isFloat=true;a.raw.resize(n*2);return a;}
static void grids(){
 for(int valid:{8,16,18,20,24,32})for(int container:{16,24,32}){
  const int effective=std::min(valid,container),low=container-effective;const int64_t shift=int64_t(1)<<low;const double step=std::ldexp(1.,1-effective);
  auto in=floating(4096);for(size_t i=0;i<in.frames();++i)for(int c=0;c<2;++c)in.setSample(i,c,.81*std::sin(2*dsp::kPi*(997+c*173)*i/in.rate));
  in.setSample(0,0,1);in.setSample(0,1,-1);in.setSample(1,0,2);in.setSample(1,1,-2);
  engine::PipelineConfig cfg;cfg.outBits=container;cfg.pcmValidBits=valid;cfg.gpuResample=false;engine::Pipeline p(cfg);engine::PipelineStats stats;auto out=p.process(in,&stats);
  bool zeros=true,oracle=true;for(size_t i=0;i<out.frames();++i)for(int c=0;c<2;++c){const int32_t v=out.raw[i*2+c];zeros&=(uint32_t(v)&uint32_t(shift-1))==0;
   const double x=std::clamp(in.sample(i,c),-1.,1.-step);const int64_t expected=std::llround(x/step)*shift;oracle&=int64_t(v)==expected;}
  ck(out.bits==container&&!out.isFloat&&out.frames()==in.frames(),"manual DAC precision preserves transport format and frame count");
  ck(zeros,"actual transport samples have all unused low bits zero");ck(oracle,"none quantization preserves high-bit signal and saturates correct effective grid");ck(!stats.bitPerfect,"manual precision is not misreported as raw bit-perfect");
  std::printf("GRID requested=%d effective=%d container=%d low_zero_bits=%d samples=%zu\n",valid,effective,container,low,out.raw.size());
 }
}
static void lowlevel_blocks_dsd(){
 for(int valid:{16,18,20,24})for(auto mode:{dsp::Shaper::TPDF,dsp::Shaper::Shaped}){
  const double step=std::ldexp(1.,1-valid);auto in=floating(65536);for(size_t i=0;i<in.frames();++i)for(int c=0;c<2;++c)in.setSample(i,c,.25*step);
  engine::PipelineConfig cfg;cfg.outBits=32;cfg.pcmValidBits=valid;cfg.shaper=mode;cfg.gpuResample=false;engine::Pipeline whole(cfg),blocks(cfg),dsd(cfg);
  auto all=whole.process(in);std::vector<int32_t> parts;for(size_t at=0;at<in.frames();){const size_t n=std::min(in.frames()-at,1+(at*19+31)%1023);auto b=in;b.raw.assign(in.raw.begin()+at*2,in.raw.begin()+(at+n)*2);const auto o=blocks.process(b);parts.insert(parts.end(),o.raw.begin(),o.raw.end());at+=n;}
  ck(all.raw==parts,"effective-grid quantizer history survives arbitrary Pipeline chunks");double sum=0;const int64_t lsb=int64_t(1)<<(32-valid);bool grid=true;for(auto v:all.raw){sum+=double(v)/lsb;grid&=(uint32_t(v)&uint32_t(lsb-1))==0;}
  ck(grid&&std::abs(sum/all.raw.size()-.25)<.015,"dither preserves sub-LSB content on effective DAC grid");
  std::vector<std::vector<double>> doubles;const auto noPcm=dsd.process(in,nullptr,&doubles);bool exact=noPcm.raw.empty();for(const auto& channel:doubles)for(double x:channel)exact&=x==.25*step;
  ck(exact,"pre-DSD double path does not inherit manual PCM valid bits or dither");
  std::printf("LOWLEVEL bits=%d mode=%s container=32 mean_lsb=%.8f lowbits_zero=1 chunk_exact=1 DSD_unquantized=1\n",valid,dsp::shaper_name(mode),sum/all.raw.size());
 }
}
static void overload(){
 for(int valid:{16,18,20,24}){
  auto in=floating(131072,384000);for(size_t i=0;i<in.frames();++i){double x=(i<16384?1.:.1)*std::sin(2*dsp::kPi*997*i/in.rate);if(i==16384)x=2.;if(i==16385)x=-2.;for(int c=0;c<2;++c)in.setSample(i,c,x);}
  engine::PipelineConfig cfg;cfg.outBits=32;cfg.pcmValidBits=valid;cfg.shaper=dsp::auto_pcm_shaper(valid,384000);cfg.gpuResample=false;engine::Pipeline p(cfg);auto out=p.process(in);
  double xx=0,xy=0,yy=0;size_t n=0,rails=0;const int64_t lsb=int64_t(1)<<(32-valid);bool grid=true;
  for(size_t i=65536;i<out.frames();++i){const auto v=out.raw[i*2];const double x=in.sample(i,0),y=out.sample(i,0);xx+=x*x;xy+=x*y;yy+=y*y;++n;rails+=std::abs(double(v))>=2147483648.-lsb;grid&=(uint32_t(v)&uint32_t(lsb-1))==0;}
  const double gain=xy/xx,rms=std::sqrt(yy/n);ck(grid&&rails==0,"effective-grid output recovers from full-scale and isolated overload without rail lock");ck(std::abs(gain-1)<.002&&rms>.06&&rms<.08,"recovered coherent gain and RMS match low-level input");
  std::printf("RECOVERY valid_bits=%d auto=%s gain=%.8f rms=%.8f rails=%zu\n",valid,dsp::shaper_name(cfg.shaper),gain,rms,rails);
 }
}
static void low_precision_auto_recovery(){
 // 8..15 are legal manual DAC precisions. A high-order automatic shaper can
 // turn an 8-bit .1 sine into roughly .48 RMS with persistent rail samples.
 // Test the actual final packed grid and the same seeded quantizer's counters,
 // rather than accepting finite output or testing only a quiet input.
 for(int valid=8;valid<16;++valid)for(uint32_t rate:{48000u,176400u,384000u,1536000u}){
  auto in=floating(65536,rate);
  for(size_t i=0;i<in.frames();++i){double x=(i<8192?1.:.1)*std::sin(2*dsp::kPi*997*i/rate);if(i==8192)x=2.;if(i==8193)x=-2.;for(int c=0;c<2;++c)in.setSample(i,c,x);}
  engine::PipelineConfig cfg;cfg.outBits=32;cfg.pcmValidBits=valid;cfg.pcmAutoDither=true;cfg.gpuResample=false;
  engine::Pipeline p(cfg);engine::PipelineStats stats;const auto out=p.process(in,&stats);
  const auto mode=dsp::auto_pcm_shaper(valid,rate);dsp::Quantizer observer(mode,valid,cfg.seed,rate);
  const int64_t lsb=int64_t(1)<<(32-valid);const double step=std::ldexp(1.,1-valid);
  bool grid=true,exact=true;double xx=0,xy=0,yy=0,mean=0;size_t rails=0,n=0;long long beforeOver=0,beforeClamp=0;
  for(size_t i=0;i<in.frames();++i){const double x=in.sample(i,0),expected=observer.process(x),y=out.sample(i,0);const int32_t packed=out.raw[i*2];
   grid&=(uint32_t(packed)&uint32_t(lsb-1))==0;exact&=int64_t(packed)==std::llround(expected*2147483648.);
   if(i==32767){beforeOver=observer.overload();beforeClamp=observer.clamped();}
   if(i>=32768){xx+=x*x;xy+=x*y;yy+=y*y;mean+=y;++n;rails+=y<=-1.||y>=1.-step;}
  }
  const double gain=xy/xx,rms=std::sqrt(yy/n);const auto extraOver=observer.overload()-beforeOver,extraClamp=observer.clamped()-beforeClamp;
  ck(mode==dsp::Shaper::TPDF&&stats.pcmShaper==dsp::Shaper::TPDF,"auto 8..15 bits uses TPDF at every actual rate");
  ck(grid&&exact,"low-precision Pipeline uses the exact final quantizer and preserves packed low zeros");
  ck(rails==0&&extraOver==0&&extraClamp==0,"low-precision auto stops clipping/clamping after full-scale and isolated overload");
  ck(std::abs(gain-1.)<.003&&rms>.06&&rms<.08&&std::abs(mean/n)<.003,"low-precision auto recovers coherent gain/RMS/mean without rail noise");
  std::printf("LOW_PRECISION_RECOVERY bits=%d fs=%u auto=%s gain=%.8f rms=%.8f rails=%zu overload_delta=%lld clamp_delta=%lld grid_exact=%d\n",valid,rate,dsp::shaper_name(mode),gain,rms,rails,extraOver,extraClamp,grid&&exact?1:0);
 }
}
static void defaults_and_errors(){
 auto in=floating(10);for(size_t i=0;i<in.frames();++i)for(int c=0;c<2;++c)in.setSample(i,c,.125);in.isFloat=false;in.bits=24;in.raw.assign(20,1234567);
 engine::PipelineConfig cfg;cfg.gpuResample=false;engine::Pipeline p(cfg);engine::PipelineStats stats;ck(p.process(in,&stats).raw==in.raw&&stats.bitPerfect,"default validbits=0 keeps original bit-perfect path");
 for(int bad:{-1,1,7,33})rejects([&]{auto c=cfg;c.pcmValidBits=bad;engine::Pipeline x(c);x.process(in);},"invalid manual bit depth rejected, never guessed");
 cfg.forceBitPerfect=true;cfg.pcmValidBits=18;engine::Pipeline forced(cfg);ck(!forced.willBeBitPerfect(in),"forced bitperfect cannot ignore manual precision");rejects([&]{forced.process(in);},"manual precision/forced bitperfect conflict rejected");
}
static void manual_precision_boundary(){
 auto in=floating(128,384000);for(size_t i=0;i<in.frames();++i)for(int c=0;c<2;++c)in.setSample(i,c,.25/128.);
 for(int valid:{8,15})for(int container:{16,24,32})for(auto mode:{dsp::Shaper::NS9,dsp::Shaper::LNS15}){
  engine::PipelineConfig cfg;cfg.outBits=container;cfg.pcmValidBits=valid;cfg.shaper=mode;cfg.gpuResample=false;
  bool rejected=false,explained=false;try{engine::Pipeline p(cfg);p.process(in);}catch(const std::invalid_argument& e){rejected=true;const std::string why=e.what();explained=why.find("16 bit")!=std::string::npos&&why.find("TPDF")!=std::string::npos;}
  ck(rejected&&explained,"unsafe manual PCM combination fails before preparing DSP with an actionable precision error");
  cfg.pcmAutoDither=true;engine::Pipeline automatic(cfg);engine::PipelineStats st;const auto pcm=automatic.process(in,&st);
  ck(pcm.frames()==in.frames()&&st.pcmShaper==dsp::Shaper::TPDF,"auto low-bit TPDF remains available instead of an unsafe manual candidate");
  cfg.pcmAutoDither=false;engine::Pipeline dsd(cfg);std::vector<std::vector<double>> planes;const auto noPcm=dsd.process(in,&st,&planes);
  bool exact=noPcm.raw.empty()&&st.pcmShaper==dsp::Shaper::None;for(const auto& p:planes)for(double x:p)exact&=x==.25/128.;
  ck(exact,"pre-DSD doubleOut does not apply or reject an unused PCM quantizer");
 }
 for(int valid:{8,15})for(auto mode:{dsp::Shaper::None,dsp::Shaper::RPDF,dsp::Shaper::TPDF,dsp::Shaper::Gauss1,dsp::Shaper::Shaped,dsp::Shaper::NS1,dsp::Shaper::NS4,dsp::Shaper::NS5}){
  engine::PipelineConfig cfg;cfg.outBits=32;cfg.pcmValidBits=valid;cfg.shaper=mode;cfg.gpuResample=false;engine::Pipeline p(cfg);engine::PipelineStats st;const auto pcm=p.process(in,&st);
  ck(pcm.frames()==in.frames()&&st.pcmShaper==mode,"stable low-bit manual modes remain selectable without silent replacement");
 }
 for(int valid:{16,24})for(auto mode:{dsp::Shaper::NS9,dsp::Shaper::LNS15}){
  engine::PipelineConfig cfg;cfg.outBits=32;cfg.pcmValidBits=valid;cfg.shaper=mode;cfg.gpuResample=false;engine::Pipeline p(cfg);engine::PipelineStats st;p.process(in,&st);
  ck(st.pcmShaper==mode,"manual NS9/LNS15 at supported precision retain the requested algorithm");
 }
 auto narrow=in;narrow.bits=8;narrow.isFloat=false;narrow.raw.assign(256,1);engine::PipelineConfig cfg;cfg.pcmValidBits=24;cfg.shaper=dsp::Shaper::NS9;cfg.gpuResample=false;
 rejects([&]{engine::Pipeline p(cfg);p.process(narrow);},"actual narrow container overrides a higher requested DAC precision for guard validation");
}
int main(){grids();lowlevel_blocks_dsd();overload();low_precision_auto_recovery();defaults_and_errors();manual_precision_boundary();std::printf("PCM_VALID_BITS checks=%d failures=%d\n",checks,failures);return failures?1:0;}
