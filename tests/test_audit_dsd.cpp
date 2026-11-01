// Offline production-branch regressions. Never opens an audio device.
#include "dsp/dsd_rate_convert.h"
#include "engine/pipeline.h"
#include <cstdio>
#include <cmath>
using namespace rta::dsp;
using rta::audio::DsdStream;
static int failures=0;
static void check(bool ok,const char* name,double actual=0,double expected=0){printf("[%s] %s actual=%.9f expected=%.9f\n",ok?"PASS":"FAIL",name,actual,expected);if(!ok)++failures;}
static double mean(const DsdStream& s,size_t from){const size_t to=std::min<size_t>(s.bits,from+32768);double v=0;for(size_t i=from;i<to;++i)v+=((s.data[0][i>>3]>>(7-(i&7)))&1)?1:-1;return v/(to-from);}
int main(){ setvbuf(stdout,nullptr,_IONBF,0);
 for(uint32_t target:{2822400u,3072000u,5644800u,6144000u}){
  DsdRateConvertConfig cfg;cfg.modFamily=ModulatorFamily::Dsd;cfg.modOrder=5;
  DsdRateConverter c(plan_dsd_rate(target*2,target),cfg,1);
  const double expected=sdm_headroom_db_for_fs(1.05,target);
  check(std::fabs(c.gain_db()-expected)<1e-12,"constructor uses destination-rate headroom",c.gain_db(),expected);
 }
 check(dsd_convert_safety_headroom_db()<=sdm_headroom_db_for_fs(1.05,2822400),"unknown rate uses conservative headroom",dsd_convert_safety_headroom_db(),sdm_headroom_db_for_fs(1.05,2822400));
 for(auto fam:{ModulatorFamily::Dsd,ModulatorFamily::Ahm})for(double dc:{-.2,0.,.2}){
  DsdRateConvertConfig cfg;cfg.modFamily=fam;cfg.modOrder=5;cfg.headroomAuto=false;
  DsdRateConverter c(plan_dsd_rate(5644800,2822400),cfg,1);
  std::vector<std::vector<double>> pcm(1,std::vector<double>(131072,dc));DsdStream out;
  c.push_pcm(pcm,pcm[0].size(),out);c.finish(out);
  const double observed=mean(out,32768);
  printf("family=%s input_dc=%.3f ",modulator_family_name(fam),dc);
  check(std::fabs(observed-dc)<.002,"packed 1-bit output preserves DC including silence",observed,dc);
  check(c.stats().overload==0,"DC test does not confuse overload with quantizer mismatch",c.stats().overload,0);
 }
 DsdStream in;in.rate=2822400;in.channels=2;in.bits=(1u<<19)+23;in.data.assign(2,std::vector<uint8_t>((in.bits+7)/8));
 for(size_t c=0;c<2;++c)for(size_t i=0;i<in.data[c].size();++i)in.data[c][i]=(uint8_t)((i*73+c*51+19)&255);
 // Unused low bits are not audio; clear them to make byte equality meaningful.
 for(auto& c:in.data)c.back()&=0xFE;
 DsdStream out;DsdRateConvertConfig cfg;
 DsdConvertResult r; try {r=convert_dsd_stream(in,in.rate,cfg,out,false);} catch(const std::exception& e){printf("direct exception=%s\n",e.what());check(false,"direct conversion must not throw");}
 check(out.bits==in.bits,"direct whole-stream conversion retains all chunks",(double)out.bits,(double)in.bits);
 check(out.data==in.data,"direct whole-stream conversion is bit perfect");
 check(r.inBits==in.bits && r.outBits==in.bits,"reported bit count matches complete input",(double)r.outBits,(double)in.bits);
 // Independent long-silence reference proves EOF keeps audio, rather than
 // merely adding a duration margin. Measurement decimators also drain their
 // own states so they cannot hide the recovered last impulse a second time.
 for(bool down:{false,true})for(auto kind:{IntegratorKind::Fir,IntegratorKind::Iir,IntegratorKind::Cic})for(bool last:{false,true}){
  DsdStream src;src.rate=down?5644800:2822400;src.channels=1;src.bits=65536;src.data.assign(1,std::vector<uint8_t>(8192,0x69));src.data[0][last?8191:0]=0xFF;
  const uint32_t dst=down?2822400:5644800;DsdRateConvertConfig cc;cc.modFamily=ModulatorFamily::Ec;cc.modOrder=5;cc.headroomAuto=false;cc.preAttenDb=-6.020599913;cc.decimator=kind;
  auto plan=plan_dsd_rate(src.rate,dst);DsdRateConverter whole(plan,cc,1),chunked(plan,cc,1),reference(plan,cc,1);DsdStream a,b,ref;
  whole.process_bits(src,a);whole.finish(a);const auto firstFinish=a;whole.finish(a);
  check(a.data==firstFinish.data && a.bits==firstFinish.bits,"finish is idempotent after draining");
  for(size_t at=0;at<src.bits;at+=1000){DsdStream part;part.rate=src.rate;part.channels=1;part.bits=std::min<uint64_t>(1000,src.bits-at);part.data.assign(1,{});part.data[0].assign(src.data[0].begin()+at/8,src.data[0].begin()+(at+part.bits+7)/8);chunked.process_bits(part,b);}chunked.finish(b);
  check(a.bits==b.bits && a.data==b.data,"DSD edge response and full tail are bit identical across chunks");
  // A balanced 1-bit pattern is not mathematical zero: stopping its ultrasonic
  // content creates a different endpoint transient. Construct the reference
  // independently with a much longer run of exact zero at the linear seam.
  auto sourceIc=conversion_integrator(kind,DsdConversion::Narrow,src.rate,plan.midRate,cc.decimatorAttenDb,cc.twoStageDecimator);
  DsdDecimator sourceDec(sourceIc);std::vector<std::vector<double>> referencePcm(1);
  sourceDec.process_bits(src.data[0].data(),(size_t)src.bits,referencePcm[0]);sourceDec.process_zeros(65536,referencePcm[0]);
  reference.push_pcm(referencePcm,referencePcm[0].size(),ref);reference.finish(ref);
  const size_t sourceDelay=(size_t)std::llround(sourceDec.latencyInputBits()*dst/src.rate+reference.latencyOutBits())-(size_t)std::llround(reference.latencyOutBits());
  if(sourceDelay){DsdStream aligned=ref;aligned.bits-=sourceDelay;aligned.data.assign(1,std::vector<uint8_t>((aligned.bits+7)/8,0));for(size_t i=0;i<aligned.bits;++i)if(ref.bit(0,i+sourceDelay))aligned.setBit(0,i,true);ref=std::move(aligned);}
  auto ic=conversion_integrator(IntegratorKind::Fir,DsdConversion::Narrow,dst,176400);DsdDecimator da(ic),dr(ic);std::vector<double> ya,yr;da.process_bits(a.data[0].data(),(size_t)a.bits,ya);da.drain(ya);dr.process_bits(ref.data[0].data(),(size_t)ref.bits,yr);dr.drain(yr);
  // Include all causal audio support and the measurement FIR response, but
  // exclude the transport stop after the separate 2ms settling-silence suffix.
  // A finite +/-1 stream abruptly replaced by mathematical zero has its own
  // endpoint transient; it is not the input impulse we are measuring.
  const size_t settle=(size_t)std::ceil(.002*dst);
  const size_t audioEnd=(size_t)std::ceil((double)(a.bits-settle+da.tailInputBits())*176400/dst);
  double pa=0,pr=0;for(size_t n=0;n<std::min(audioEnd,ya.size());++n)pa=std::max(pa,std::fabs(ya[n]));for(size_t n=0;n<std::min(audioEnd,yr.size());++n)pr=std::max(pr,std::fabs(yr[n]));
  printf("tail_case down=%d kind=%s last=%d bits=%llu reference_peak=%.9f finished_peak=%.9f\n",int(down),integrator_name(kind),int(last),a.bits,pr,pa);
  check(pr>.005 && std::fabs(pa-pr)<.003,"first/last impulse matches independent long-silence reference",pa,pr);
 }
 for(double fs:{45158400.,49152000.}){
  SdmConfig c;c.fs=fs;c.order=7;c.cutoffHz=100000;c.ditherLsb=.0005;c.oneBitOutput=true;
  // 3.0.10 replaces the old polynomial quality fallback with factor-preserving
  // sections. The frozen old failure is retained in the audit evidence; the
  // dedicated highrate test now enforces the absolute and relative error floor.
  check(stable_sdm_order(7,fs,2,ModulatorFamily::Dsd)==7,"DSD1024 factor-preserving helper permits measured 7th order");
  SdmModulator production(c);check(production.effectiveOrder()==7 && production.uses_sections() && !production.steppedDown(),"constructor uses actual 7th order with guards enabled");
  auto ahm=c;ahm.family=ModulatorFamily::Ahm;SdmModulator ah(ahm);check(ah.effectiveOrder()==7 && ah.uses_sections() && ah.levels()==2,"AHM packed output uses the same sectioned 1-bit realization");
  auto spectral=[&](bool bypass){auto cc=c;cc.bypassOrderGuard=bypass;SdmModulator m(cc);const size_t N=1<<18,warm=(size_t)(fs*.03);const size_t fund=(size_t)std::llround(1000*N/fs);const double fin=fund*fs/N;for(size_t i=0;i<warm;++i)m.process(.62*sin(2*kPi*fin*i/fs));double np=0;for(size_t rep=0;rep<2;++rep){std::vector<cplx>b(N);for(size_t i=0;i<N;++i)b[i]=cplx(m.process(.62*sin(2*kPi*fin*(warm+rep*N+i)/fs))*(.5-.5*cos(2*kPi*i/N)),0);fft(b);for(size_t k=1;k<=size_t(20000*N/fs);++k)if(!(k+3>=fund && k<=fund+3))np+=2*std::norm(b[k])/(double(N)*N*.375);}return 10*log10(np/2+1e-300);};
  double baseline=spectral(false),unguarded=spectral(true);printf("fs=%.0f protected_noise_dbfs=%.6f bypass7_noise_dbfs=%.6f\n",fs,baseline,unguarded);
  check(baseline < -155 && baseline == unguarded,"guards-enabled 7th order meets quality and equals diagnostic bypass",baseline,unguarded);
  SdmModulator rec(c);const size_t total=(size_t)(fs*.30),tail=(size_t)(fs*.25);long long ov=0;double acc=0,sq=0,last=0;size_t count=0,same=0;
  for(size_t n=0;n<total;++n){double amp=(n>=size_t(fs*.05)&&n<size_t(fs*.10))?1:.62;double x=amp*sin(2*kPi*997*n/fs);if(n==size_t(fs*.15))x=5;if(n==tail)ov=rec.overload();double y=rec.process(x);acc+=y;if(n%1024==1023){if(n>=tail){sq+=(acc/1024)*(acc/1024);++count;}acc=0;}if(n>=tail && n>tail && y==last)++same;last=y;}
  const double rms=sqrt(sq/count),sameRate=double(same)/(total-tail-1);printf("fs=%.0f recovery_rms=%.9f same_rate=%.9f tail_overloads=%lld\n",fs,rms,sameRate,rec.overload()-ov);
  check(rec.overload()>0 && rec.overload()==ov,"full-scale plus spike overload recovers without continuing clamps",rec.overload()-ov,0);
  check(fabs(rms-.62/sqrt(2.))<.04 && sameRate<.65,"DSD1024 recovered audio RMS and bit-run rate",rms,.62/sqrt(2.));
 }
 {
  rta::audio::AudioBuffer pcm;pcm.rate=44100;pcm.bits=16;pcm.channels=1;pcm.raw.resize(2048);
  for(size_t i=0;i<pcm.frames();++i)pcm.raw[i]=(int32_t)(10000*sin(2*kPi*997*i/44100));
  rta::engine::PipelineConfig pc;pc.outRate=705600;pc.taps=1024;pc.twoStage=false;pc.gpuResample=false;
  rta::engine::Pipeline full(pc),parts(pc),integer(pc);rta::engine::PipelineStats ps;
  std::vector<std::vector<double>> a,b(1);
  auto metadata=full.process(pcm,&ps,&a);
  check(metadata.raw.empty() && ps.outFrames==a[0].size(),"double output reports format and sample count");
  for(size_t n=0;n<pcm.frames();n+=113){auto seg=pcm;size_t end=std::min(n+113,pcm.frames());seg.raw.assign(pcm.raw.begin()+n,pcm.raw.begin()+end);std::vector<std::vector<double>> p;parts.process(seg,nullptr,&p);b[0].insert(b[0].end(),p[0].begin(),p[0].end());}
  check(a==b,"16-bit source double DSP output is identical across 113-frame chunks");
  auto quant=integer.process(pcm);size_t different=0;
  for(size_t n=0;n<a[0].size();++n)if(a[0][n]!=quant.sample(n,0))++different;
  check(different>100,"DSD double path retains interpolation detail lost by integer PCM quantization",(double)different,100);
 }
 printf("audit_dsd failures=%d\n",failures);return failures?1:0;
}

