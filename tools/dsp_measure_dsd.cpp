// Real DSD-purpose cascade: streamed DD16 FFT, actual read-only stage kernels.
#include "dsp_measure_cli.h"
#include "dsp_measure_stages.h"
using namespace dsp_measure;
static void export_stage(const Options&o,const std::string&name,const std::vector<double>&h) {
  if(o.coefficients.empty())return;
  auto path=std::filesystem::u8path(o.coefficients)/(name+".f64");
  std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path,std::ios::binary);f.write((char*)h.data(),h.size()*sizeof(double));
  if(!f)throw std::runtime_error("stage coefficient export failed");
}
static int selftest() {
  const auto&p=*rta::dsp::find_preset("poly-sinc-short-lp");
  auto s=production_stages(p,44100,705600);
  size_t n=1;while(n<16*((s.first.size()-1)+(s.second.size()+s.f2-1)/s.f2))n*=2;
  const size_t fullN=n*s.f2;
  std::vector<DC> streamed(fullN/2+1);
  measure_stages(s,16,[&](size_t j,DC v){streamed[j]=v;});
  auto threaded=production_stages(p,44100,705600);measure_stages(threaded);
  const bool threadedSame=std::abs(threaded.result.stopDb-s.result.stopDb)<1e-12&&std::abs(threaded.result.passDb-s.result.passDb)<1e-12;
  auto c=review2::production_config(p,44100,705600);
  rta::dsp::apply_resample_purpose(c,rta::dsp::ResamplePurpose::Dsd);
  rta::dsp::PolyphaseResampler real(c);std::vector<double> h;real.prototype_into(h);
  std::vector<DC> full(fullN);for(size_t i=0;i<h.size();++i)full[i].r.h=h[i]/real.upFactor();
  fft(full);
  for(auto&x:h)x/=real.upFactor();
  auto envelope=process_envelope(s,h);
  double maxAbsolute=0,maxPassDb=0,stopProcess=-6000,stopStages=-6000;
  size_t first=(size_t)std::ceil((44100-s.result.passHz)/705600*fullN);
  for(size_t j=0;j<streamed.size();++j) {
    maxAbsolute=std::max(maxAbsolute,mag(sub(streamed[j],full[j])));
    if(double(j)*705600/fullN<=s.result.passHz)
      maxPassDb=std::max(maxPassDb,std::abs(db(mag(streamed[j]))-db(mag(full[j]))));
    if(j>=first){stopProcess=std::max(stopProcess,db(mag(full[j])));stopStages=std::max(stopStages,db(mag(streamed[j])));}
  }
  // Actual floating process() impulse has rounded intermediate convolution.
  // Report its difference explicitly; the product is the ideal stage response.
  printf("SELFTEST all_bins=%zu full_fft=%zu stripes=%u max_complex_absolute_error=%.17g max_pass_difference_db=%.17g process_stop_db=%.12f stage_stop_db=%.12f peak_delta_db=%.12f\n",
    streamed.size(),fullN,s.f2,maxAbsolute,maxPassDb,stopProcess,stopStages,stopStages-stopProcess);
  const double bound=db(std::pow(10.,stopStages/20)+envelope.deltaL1);
  printf("PROCESS_ENVELOPE delta_l1=%.17g bounded_peak_db=%.12f observed_process_peak_db=%.12f\n",envelope.deltaL1,bound,stopProcess);
  bool ok=threadedSame&&maxAbsolute<=envelope.deltaL1&&maxPassDb<1e-8&&stopProcess<=bound;
  auto bad=production_stages(p,44100,705600);bad.second[0]+=.001;bad.second[1]-=.001;
  measure_stages(bad);printf("NEGATIVE_CONTROL stage2_fault_stop_db=%.12f status=%s\n",bad.result.stopDb,bad.result.status.c_str());
  return ok&&bad.result.stopFail&&bad.result.stopDb>-100?0:1;
}
int main(int argc,char**argv) {
 try {
  if(argc==2&&std::string(argv[1])=="--selftest")return selftest();
  auto o=parse(argc,argv);if(o.manual||!o.baseline.empty()||o.topology=="off")throw std::runtime_error("DSD streamed tool requires real DSD-purpose cascade; no manual/PCM baseline or forced off");
  if(o.help){puts("dsp_measure_dsd: same explicit options as filter_sweep; defaults to all79 x (44100->45158400,48000->49152000). --points evaluates a single real cascade. --selftest compares every small-L streamed bin against full process-impulse DD FFT, plus deliberately damaged stage2. --structure-only exports/builds actual stages without spectral scan. Every quality scan uses streamed DD FFT >=16 times effective impulse length; still NOT continuous-frequency proof.");return 0;}
  if(o.output=="filter_sweep_audit.tsv")o.output="dsd_stage_audit.tsv";
  std::filesystem::path path=std::filesystem::u8path(o.output);
  if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
  std::ofstream os(path);if(!os)throw std::runtime_error("cannot open output");os<<std::setprecision(17);
  std::ofstream manifest;if(!o.manifest.empty()){auto mp=std::filesystem::u8path(o.manifest);if(mp.has_parent_path())std::filesystem::create_directories(mp.parent_path());manifest.open(mp);if(!manifest)throw std::runtime_error("cannot open manifest");manifest<<"# DSD purpose; finite-grid process envelope; not continuous-band proof\n";}
  os<<"# DSD-purpose, actual stage coefficients; ideal stage transfer product, floating process-roundoff checked separately; finite grid NOT continuous-band proof.\n";
  if(o.points.empty())os<<"identity\tin_rate\tout_rate\tpreset\tT\tstage2_T\tstage1_taps\tstage2_taps\tF1\tF2\teffective_taps\tfft_n\tstripe_n\tstop_bins\tstop_hz\tstop_db\tpass_hz\tpass_db\ttarget\tkind\tstatus\treused\tdc_db\telapsed_s\tproduct_peak_db\tprocess_delta_l1\tbounded_peak_db\tprocess_witness_db\tprocess_pass_bound_abs_db\tdecision\tprocess_stream_peak_db\tprocess_stream_stop_hz\tviolation_hz\tviolation_db\n";
  else os<<"identity\tin_rate\tout_rate\tpreset\tstage1_rate\tstage2_rate\tfrequency_hz\tstage1_db\tstage2_db\tdd_db\n";
  std::vector<std::pair<unsigned,unsigned>> groups={{44100,45158400},{48000,49152000}};if(o.in)groups={{o.in,o.out}};
  struct Cache {StageData s;size_t stripeN;ProcessEnvelope envelope;};std::vector<Cache> cache;
  unsigned selected=0,measured=0,quality=0,excluded=0,unresolved=0,structural=0,reusedCount=0;
  const auto start=std::chrono::steady_clock::now();setvbuf(stdout,nullptr,_IONBF,0);
  for(auto[in,out]:groups)for(const auto&p:rta::dsp::filter_presets()) {
    if((o.apod&&!p.apodizing)||(!o.preset.empty()&&o.preset!=p.name))continue;
    if(selected++<o.offset)continue;if(o.limit&&measured>=o.limit)continue;
    ++measured;auto id=identity(in,out,p.name);auto t0=std::chrono::steady_clock::now();
    try {
      auto s=production_stages(p,in,out);auto&r=s.result;
      if(o.inject){s.second.at(0)+=.001;s.second.at(1)-=.001;}
      auto stem=std::to_string(in)+"-"+std::to_string(out)+"-"+p.name;
      export_stage(o,stem+"-stage1",s.first);export_stage(o,stem+"-stage2",s.second);
      if(o.list){puts(id.c_str());continue;}
      if(!o.points.empty()) {
        for(double hz:frequencies(o.points,out)) {
          const double d1=direct(s.first,std::remainder(hz,double(in)*s.f1),double(in)*s.f1),d2=direct(s.second,hz,out);
          os<<id<<'\t'<<in<<'\t'<<out<<'\t'<<p.name<<'\t'<<double(in)*s.f1<<'\t'<<out<<'\t'<<hz<<'\t'<<d1<<'\t'<<d2<<'\t'<<d1+d2<<'\n';
        }
        os.flush();continue;
      }
      bool reused=false;size_t stripeN=0;
      double delta=0,boundedPeak=-6000,processWitness=-6000,passBound=0,processStreamPeak=-6000;
      double violationHz=0,violationDb=-6000,processStreamHz=0;
      std::string decision="STRUCTURE_ONLY";
      if(o.structure){r.dcDb=stage_direct(s,0);r.finite=std::isfinite(r.dcDb)&&std::abs(r.dcDb)<1e-4;r.status="STRUCTURE_ONLY";r.kind="NOT_MEASURED";}
      else {
        auto actual=actual_process_impulse(s);
        if(o.inject){actual.at(0)+=.001;actual.at(1)-=.001;}
        const std::vector<double>* actualRef=nullptr;
        for(const auto&v:cache)if(equal_stage_coefficients(s,v.s)
          &&actual.size()==v.envelope.impulse.size()&&std::memcmp(actual.data(),v.envelope.impulse.data(),actual.size()*sizeof(double))==0
          &&r.passHz/in==v.s.result.passHz/v.s.in&&(in-r.passHz)/in==(v.s.in-v.s.result.passHz)/v.s.in) {
          auto&old=v.s.result;double ratio=double(in)/v.s.in;
          r.stopDb=old.stopDb;r.stopHz=old.stopHz*ratio;r.passDb=old.passDb;r.passWorstHz=old.passWorstHz*ratio;r.dcDb=old.dcDb;r.fftN=old.fftN;r.stopBins=old.stopBins;
          stripeN=v.stripeN;reused=true;++reusedCount;delta=v.envelope.deltaL1;actualRef=&v.envelope.impulse;classify(r);break;
        }
        if(!reused){printf("BEGIN %s F1=%u F2=%u stageN=%zu,%zu\n",id.c_str(),s.f1,s.f2,s.first.size(),s.second.size());stripeN=measure_stages(s,o.factor,{},true);auto envelope=process_envelope(s,std::move(actual));delta=envelope.deltaL1;cache.push_back({s,stripeN,std::move(envelope)});actualRef=&cache.back().envelope.impulse;}
        boundedPeak=db(std::pow(10.,r.stopDb/20)+delta);
        const double absPass=std::abs(r.passDb);
        passBound=std::max(db(std::pow(10.,absPass/20)+delta),-db(std::max(1e-300,std::pow(10.,-absPass/20)-delta)));
        processWitness=direct(*actualRef,r.stopHz,out);
        double passWitness=0;if(passBound>=.01)passWitness=direct(*actualRef,r.passWorstHz,out);
        r.stopFail=processWitness>-r.target+1e-8;r.passFail=std::abs(passWitness)>.01+1e-10;
        if(r.stopFail){violationHz=r.stopHz;violationDb=processWitness;}
        else if(r.passFail){violationHz=r.passWorstHz;violationDb=passWitness;}
        r.uncertain=!(r.stopFail||r.passFail)&&(boundedPeak>=-r.target-1e-8||passBound>=.01-1e-10);
        decision=r.stopFail||r.passFail?"CONFIRMED_PROCESS_WITNESS_FAIL":r.uncertain?"PROCESS_BOUND_UNRESOLVED":"FINITE_GRID_PROCESS_BOUND_MET";
        if(r.uncertain) {
          // Expensive but exact same finite grid over the *actual* rounded
          // process impulse. Reuse the streamed transform with an identity
          // first kernel; this folds the real impulse for every stripe.
          printf("PROCESS_FALLBACK %s bound_db=%.12f target=%.1f actual_N=%zu\n",id.c_str(),boundedPeak,r.target,actualRef->size());
          StageData probe=s;probe.first={1};probe.second=*actualRef;
          probe.result.stopDb=-6000;probe.result.passDb=0;probe.result.stopHz=in-r.passHz;
          measure_stages(probe,o.factor,{},true);processStreamPeak=probe.result.stopDb;
          r.stopFail=probe.result.stopFail;r.passFail=probe.result.passFail;r.uncertain=probe.result.uncertain;
          processStreamHz=probe.result.stopHz;
          if(r.stopFail){violationHz=probe.result.stopHz;violationDb=probe.result.stopDb;}
          else if(r.passFail){violationHz=probe.result.passWorstHz;violationDb=probe.result.passDb;}
          decision=r.stopFail||r.passFail?"CONFIRMED_PROCESS_STREAM_FAIL":r.uncertain?"PROCESS_STREAM_PRECISION_UNRESOLVED":"DIRECT_PROCESS_STREAMED_GRID_MET";
        }
        r.kind=r.stopFail?(r.passFail?"STOP_AND_PASS":"STOP"):(r.passFail?"PASS":r.uncertain?"ARITHMETIC_BOUND":"NONE");
        r.status=r.excluded?"EXCLUDED":decision;
      }
      if(!r.finite)++structural;else if(r.excluded)++excluded;else if(r.stopFail||r.passFail)++quality;else if(r.uncertain)++unresolved;
      double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
      os<<id<<'\t'<<in<<'\t'<<out<<'\t'<<p.name<<'\t'<<r.T<<'\t'<<r.stage2T<<'\t'<<s.first.size()<<'\t'<<s.second.size()<<'\t'<<s.f1<<'\t'<<s.f2<<'\t'<<r.taps<<'\t'<<r.fftN<<'\t'<<stripeN<<'\t'<<r.stopBins<<'\t'<<r.stopHz<<'\t'<<r.stopDb<<'\t'<<r.passWorstHz<<'\t'<<r.passDb<<'\t'<<r.target<<'\t'<<r.kind<<'\t'<<r.status<<'\t'<<reused<<'\t'<<r.dcDb<<'\t'<<seconds<<'\t'<<r.stopDb<<'\t'<<delta<<'\t'<<boundedPeak<<'\t'<<processWitness<<'\t'<<passBound<<'\t'<<decision<<'\t'<<processStreamPeak<<'\t'<<processStreamHz<<'\t'<<violationHz<<'\t'<<violationDb<<'\n';os.flush();
      printf("DONE %s T=%d,%d grid=%zu stop=%.12f Hz=%.12f pass=%.12f %s reused=%d seconds=%.3f\n",id.c_str(),r.T,r.stage2T,r.fftN,r.stopDb,r.stopHz,r.passDb,r.status.c_str(),reused,seconds);
      if(manifest.is_open()){manifest<<id<<'/'<<r.status<<'\n';manifest.flush();}
    }catch(const std::exception&e){++structural;fprintf(stderr,"STRUCTURE_FAIL %s: %s\n",id.c_str(),e.what());}
  }
  printf("DSD_RESULT measured=%u unique_spectra=%zu reused=%u sampled_fail=%u excluded=%u unresolved=%u structural_fail=%u elapsed_s=%.3f\n",measured,cache.size(),reusedCount,quality,excluded,unresolved,structural,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
  if(!measured||structural)return 2;return quality?1:(unresolved?3:0);
 }catch(const std::exception&e){fprintf(stderr,"ERROR %s\n",e.what());return 2;}
}
