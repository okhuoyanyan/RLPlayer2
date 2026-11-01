#include "engine/pipeline.h"
#include <bit>
#include <climits>
#include <cstdio>
#include <tuple>
using namespace rta::dsp;
using Old=int(*)(const double*,int,long long,const double*,int,int,const int*,const int*,long long,double*);
using Compact=int(*)(const double*,int,long long,const double*,int,int,int,long long,long long,long long,long long,double*const*);
static int checks=0,fails=0;static size_t samples=0;
static void ck(bool yes,const char*t){++checks;if(!yes){++fails;std::printf("FAIL %s\n",t);}}
static bool equal(const std::vector<double>&a,const std::vector<double>&b){
 if(a.size()!=b.size())return false;
 for(size_t i=0;i<a.size();++i){++samples;if(std::bit_cast<uint64_t>(a[i])!=std::bit_cast<uint64_t>(b[i]))return false;}return true;
}
struct Data{
 int L,N,H;std::vector<double> hist,phase,out,x,y;std::vector<int> base,indices;
 Data(int l=16,int n=64,int h=64):L(l),N(n),H(h),hist(h*2),phase(l*32),out(n*2,123.5),x(n,123.5),y(n,123.5),base(n),indices(n){
  for(size_t i=0;i<hist.size();++i)hist[i]=((int)(i%29)-14)/8388608.;
  for(size_t i=0;i<phase.size();++i)phase[i]=((int)((i*17)%101)-50)/1048576.;
  for(int i=0;i<n;++i){base[i]=i/l;indices[i]=i%l;}
 }
 int old(Old f){return f(hist.data(),2,H,phase.data(),L,32,base.data(),indices.data(),N,out.data());}
 int compact(Compact f){double*p[]={x.data(),y.data()};return f(hist.data(),2,H,phase.data(),L,1,32,0,0,N,N,p);}
 std::vector<double> inter()const{std::vector<double>v(N*2);for(int n=0;n<N;++n){v[n*2]=x[n];v[n*2+1]=y[n];}return v;}
};
static void faults(rta::platform::SharedLib& lib,Old old,Compact compact){
 auto set=(void(*)(int,int))lib.symbol("fixture_fault");auto hits=(int(*)())lib.symbol("fixture_fault_hits");
 if(!set)return;
 auto invalidate=(void(*)())lib.symbol("rta_gpu_resample_invalidate");
 ck(invalidate!=nullptr,"phase fault setup can invalidate cached coefficients");
 if(!invalidate)return;
 Data small;ck(small.old(old)==small.N,"old warmup");auto expectedOld=small.out;
 auto restoreOld=[&](){ck(small.old(old)==small.N,"old small allocation recovered");ck(equal(small.out,expectedOld),"old recovered coefficients/output");};
 // Both ABIs share two coefficient slots. Force an actual upload before the
 // phase-copy faults; a cache hit must not silently skip the injected failure.
 auto failOld=[&](Data&d,int nthAlloc,int nthCopy){if(nthCopy)invalidate();int before=hits();set(nthAlloc,nthCopy);ck(d.old(old)==-1,"old injected failure returned");ck(hits()==before+1,"old fault injection executed");set(0,0);restoreOld();};
 Data largerHist(16,64,256);failOld(largerHist,1,0);
 Data largerPhase(32,1024,64);failOld(largerPhase,2,0); // phase replacement succeeds; index allocation fails
 Data largerIndices(16,1024,64);failOld(largerIndices,2,0); // pair's second allocation fails
 failOld(largerIndices,3,0); // new index pair commits, output growth fails
 Data phaseCopy(64,1024,64);failOld(phaseCopy,0,2); // replacement phase buffer, H2D failure
 Data sameDimensions;for(auto&v:sameDimensions.phase)v+=1./1048576.;
 failOld(sameDimensions,0,2); // SAME allocation: partial new coefficients, then old identity
 ck(small.compact(compact)==small.N,"compact warmup");auto expectedCompact=small.inter();
 auto restoreNew=[&](){ck(small.compact(compact)==small.N,"compact small allocation recovered");ck(equal(small.inter(),expectedCompact),"compact recovered coefficients/output");};
 auto failNew=[&](Data&d,int nthAlloc,int nthCopy){if(nthCopy)invalidate();int before=hits();set(nthAlloc,nthCopy);ck(d.compact(compact)==-1,"compact injected failure returned");ck(hits()==before+1,"compact fault injection executed");set(0,0);restoreNew();};
 failNew(largerHist,1,0);failNew(largerPhase,2,0); // phase replacement then output allocation failure
 failNew(phaseCopy,0,2);
 failNew(sameDimensions,0,2); // Same-capacity partial H2D failure is distinct from growth.
 ck(hits()==10,"all ten allocation/copy faults actually executed");
 std::printf("FAULT_RECOVERY actual_faults=%d restored_small_blocks=10\n",hits());
}
int main(int argc,char**argv){
 setvbuf(stdout,nullptr,_IONBF,0);const std::string mode=argc>1?argv[1]:"legacy";bool real=mode=="real";
 set_gpu_vendor("amd");rta::platform::SharedLib lib;ck(lib.open("rtagpu.dll"),"private backend loaded");
 auto old=(Old)lib.symbol("rta_gpu_resample");auto compact=(Compact)lib.symbol("rta_gpu_resample_compact_planar");ck(old!=nullptr,"old ABI preserved");
 ck((mode=="legacy")==(!compact),"optional ABI presence");
 if(real){
  double values[4]{};auto probe=(int(*)(double*))lib.symbol("fixture_probe");ck(probe&&probe(values)==0,"gfx1201 actual constant probe");for(double v:values)ck(v==1.25,"constant output content");
  faults(lib,old,compact);
  for(auto spec:{std::tuple<int,int,long long,long long>{16,1,0,-31},{1024,1,(1LL<<42)+17,0},{1280,147,10000000007LL,0}}){
   auto[L,M,start,first]=spec;if(start)first=start*M/L-32;Data d(L,257,128);
   for(int n=0;n<d.N;++n){long long num=(start+n)*M;d.base[n]=(int)(num/L-first);d.indices[n]=(int)(num%L);}
   double*p[]={d.x.data(),d.y.data()};ck(d.old(old)==d.N,"direct old output");
   ck(compact(d.hist.data(),2,d.H,d.phase.data(),L,M,32,start,first,d.N,d.N,p)==d.N,"direct exact compact output");
   ck(equal(d.out,d.inter()),"negative firstAbs/large absolute counter/generic M exact");
  }
  Data d;double*p[]={d.x.data(),d.y.data()};auto invalid=[&](long long start,long long first,long long n,long long cap,int M,int ch,long long H){
    ck(compact(d.hist.data(),ch,H,d.phase.data(),16,M,32,start,first,n,cap,p)==-1,"invalid count/capacity/history rejected");
    ck(d.x[0]==123.5&&d.y[0]==123.5,"invalid request did not overwrite output");
  };
  invalid(0,0,64,63,1,2,64);invalid(LLONG_MAX,0,2,64,1,2,64);
  invalid(LLONG_MAX/2,0,64,64,3,2,64);invalid(0,LLONG_MIN,64,64,1,2,64);
  invalid(0,0,64,64,1,2,LLONG_MAX);invalid(0,0,LLONG_MAX,LLONG_MAX,1,2,64);
  invalid(0,0,64,64,1,2,1);invalid(0,0,64,64,1,65536,64);
 }
 for(auto io:{std::pair<unsigned,unsigned>{44100,45158400},{48000,49152000},{44100,384000}})for(bool two:{false,true}){
  ResamplerConfig cfg;cfg.inRate=io.first;cfg.outRate=io.second;cfg.channels=2;cfg.minTapsPerInput=32;cfg.twoStage=two;cfg.stage2Taps=64;
  PolyphaseResampler cpu(cfg),gpu(cfg);gpu.set_use_gpu(true);
  for(int reset=0;reset<2;++reset){if(reset){cpu.reset();gpu.reset();}size_t start=0;
   for(size_t n:{size_t(0),size_t(1),size_t(3),size_t(17),size_t(127),size_t(5),size_t(257),size_t(0),size_t(231)}){
    std::vector<std::vector<double>>in(2,std::vector<double>(n)),a(2),b(2);for(int c=0;c<2;++c)for(size_t i=0;i<n;++i)in[c][i]=.00001*std::sin(.017*(start+i)+.3*c);
    auto run=[&](PolyphaseResampler&r,std::vector<std::vector<double>>&o){for(auto&v:o)v.resize(r.maxOutFrames(n));const double*ip[]={in[0].data(),in[1].data()};double*op[]={o[0].data(),o[1].data()};size_t got=r.process(ip,n,op);for(auto&v:o)v.resize(got);return got;};
    size_t na=run(cpu,a),nb=run(gpu,b);ck(na==nb,"irregular block count");ck(equal(a[0],b[0])&&equal(a[1],b[1]),"complete stream bitexact vs CPU");
    ck(!cpu.gpu_compact_stage_mask_last_block(),"CPU does not mark compact GPU");
    unsigned mask=na?(gpu.two_stage()?3u:1u):0u;ck(gpu.gpu_stage_mask_last_block()==mask,"GPU all stages");
    unsigned wanted=real&&gpu.downFactor()==1&&(gpu.upFactor()&(gpu.upFactor()-1))==0?mask:0;
    ck(gpu.gpu_compact_stage_mask_last_block()==wanted,"compact actual stages or genuine old fallback");start+=n;
   }
  }
 }
 for(bool two:{false,true}) {
  ResamplerConfig cfg;cfg.inRate=44100;cfg.outRate=705600;cfg.channels=1;cfg.minTapsPerInput=32;cfg.useFixed=true;cfg.twoStage=two;
  PolyphaseResampler fixed(cfg);fixed.set_use_gpu(true);
  std::vector<double>in(32,.00001),out(fixed.maxOutFrames(in.size()));const double*ip[]={in.data()};double*op[]={out.data()};
  fixed.process(ip,in.size(),op);ck(fixed.gpu_compact_stage_mask_last_block()==(real&&two?2u:0u),"fixed first stage never marked compact GPU");
 }
 if(compact) {
  ResamplerConfig cfg;cfg.inRate=44100;cfg.outRate=45158400;cfg.channels=1;cfg.minTapsPerInput=32;
  PolyphaseResampler guarded(cfg);guarded.set_use_gpu(true);
  for(size_t count:{size_t(INT_MAX)/1024+1,size_t(LLONG_MAX)/1024+1}) {
   bool rejected=false;try{guarded.process(nullptr,count,nullptr);}catch(const std::length_error&){rejected=true;}
   ck(rejected,"host compact range rejected before input/history access");
  }
 }
 // Same dimensions are not a coefficient identity: alternate independent objects.
 {
  ResamplerConfig cfg;cfg.inRate=44100;cfg.outRate=705600;cfg.channels=1;cfg.minTapsPerInput=32;
  auto other=cfg;other.transition=.08;other.attenDb=240;
  PolyphaseResampler cpuA(cfg),gpuA(cfg),cpuB(other),gpuB(other);gpuA.set_use_gpu(true);gpuB.set_use_gpu(true);
  std::vector<double>in(73,.00001);
  auto compare=[&](PolyphaseResampler&cpu,PolyphaseResampler&gpu){
   std::vector<double>a(cpu.maxOutFrames(in.size())),b(gpu.maxOutFrames(in.size()));const double*ip[]={in.data()};double*ap[]={a.data()},*bp[]={b.data()};
   size_t na=cpu.process(ip,in.size(),ap),nb=gpu.process(ip,in.size(),bp);a.resize(na);b.resize(nb);
   ck(equal(a,b),"same L/T different coefficients across objects remain bitexact");
  };
  for(int n=0;n<4;++n){compare(cpuA,gpuA);compare(cpuB,gpuB);}
 }
 // Pipeline compact mask is post-process and clears on early-return bypass.
 {rta::engine::PipelineConfig cfg;cfg.outRate=705600;cfg.minTapsPerInput=32;cfg.twoStage=true;cfg.stage2Taps=64;
  rta::engine::Pipeline pipe(cfg);rta::audio::AudioBuffer in;in.rate=44100;in.channels=1;in.bits=24;in.raw.assign(16,3);
  rta::engine::PipelineStats st;std::vector<std::vector<double>>planes;pipe.process(in,&st,&planes);
  ck(st.gpuCompactResampleStageMask==(real?3u:0u),"Pipeline compact mask");
  in.rate=cfg.outRate;pipe.process(in,&st);ck(!st.gpuCompactResampleStageMask,"Pipeline bypass compact zero");}
 std::printf("RESULT mode=%s checks=%d samples=%zu fails=%d\n",mode.c_str(),checks,samples,fails);return fails?1:0;
}
