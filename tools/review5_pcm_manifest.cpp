// Bounded actual-production PCM manifest/finite point screen. No audio device.
#include "engine/pipeline.h"
#include "dsp/response_precision.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <psapi.h>
using namespace rta;namespace fs=std::filesystem;namespace rp=dsp::response_precision;
struct TableTag {using type=std::vector<std::vector<int64_t>> dsp::PolyphaseResampler::*;friend type field(TableTag);};
template<class Tag,typename Tag::type P>struct ReadMember {friend typename Tag::type field(Tag){return P;}};
template struct ReadMember<TableTag,&dsp::PolyphaseResampler::phasesFixed_>;
struct Point {double hz,db=0,delay=0,phase=0;};std::vector<Point>points;
double value(rp::DD v){return v.h+v.l;}
rp::DD q62(int64_t q){constexpr int64_t base=INT64_C(4294967296);int64_t hi=q/base,lo=q-hi*base;return rp::add(rp::DD{std::ldexp(double(hi),-30),0},rp::DD{std::ldexp(double(lo),-62),0});}
template<class T>void binary(const fs::path&p,const std::vector<T>&v){std::ofstream f(p,std::ios::binary);f.write((const char*)v.data(),v.size()*sizeof(T));if(!f)throw std::runtime_error("artifact write failure");}
void leaf(const dsp::PolyphaseResampler&r,const fs::path&dir,int index,std::ostream&o){
 auto c=r.config();const int L=r.upFactor(),M=r.downFactor(),T=r.tapsPerPhase();std::vector<double>h;r.prototype_into(h);const auto&fixed=r.*field(TableTag{});std::vector<rp::DD>coeff;coeff.reserve(h.size());std::vector<int64_t>q;
 if(c.useFixed){if(fixed.size()!=size_t(L))throw std::runtime_error("actual fixed table missing");q.resize(h.size());for(int p=0;p<L;++p){if(fixed[p].size()!=size_t(T))throw std::runtime_error("actual fixed phase length wrong");for(int u=0;u<T;++u)q[p+u*L]=fixed[p][u];}for(auto v:q)coeff.push_back(q62(v));}
 else for(double v:h)coeff.push_back({v,0});
 std::string name="stage"+std::to_string(index);binary(dir/(name+".f64le"),h);if(c.useFixed)binary(dir/(name+".q62le"),q);
 const double high=double(c.inRate)*L;size_t peak=0;double sym=0;rp::DD energy,pre,post,centroid;
 for(size_t i=0;i<coeff.size();++i){if(std::abs(value(coeff[i]))>std::abs(value(coeff[peak])))peak=i;sym=std::max(sym,std::abs(value(rp::add(coeff[i],rp::neg(coeff[coeff.size()-1-i])))));}
 for(size_t i=0;i<coeff.size();++i){auto e=rp::mul(coeff[i],coeff[i]);energy=rp::add(energy,e);centroid=rp::add(centroid,rp::mul({double(i),0},e));if(i<peak)pre=rp::add(pre,e);if(i>peak)post=rp::add(post,e);}
 for(auto&p:points){rp::DC z{{1,0},{0,0}},sum,j;auto w=rp::rotation(std::remainder(p.hz,high),high);
  for(size_t i=0;i<coeff.size();++i){rp::DC v{rp::mul(coeff[i],z.r),rp::mul(coeff[i],z.i)};sum=rp::add(sum,v);j=rp::add(j,{rp::mul({double(i),0},v.r),rp::mul({double(i),0},v.i)});z=rp::mul(z,w);}
  double re=value(sum.r),im=value(sum.i),jr=value(j.r),ji=value(j.i);double mag=std::hypot(re,im)/L;p.db+=20*std::log10(std::max(mag,1e-300));p.phase+=std::atan2(im,re);p.delay+=(re*jr+im*ji)/(re*re+im*im)/high;
 }
 o<<"{\"index\":"<<index<<",\"input_rate\":"<<c.inRate<<",\"output_rate\":"<<c.outRate<<",\"L\":"<<L<<",\"M\":"<<M<<",\"T\":"<<T<<",\"count\":"<<h.size()<<",\"coefficient_rate\":"<<high<<",\"reference_gain\":"<<L<<",\"cutoff_norm_nyquist\":"<<std::min(1./L,1./M)*(1-c.transition*.5)<<",\"transition\":"<<c.transition<<",\"attenuation\":"<<c.attenDb<<",\"window\":"<<std::quoted(dsp::window_name(c.window))<<",\"phase\":"<<std::quoted(dsp::phase_name(c.phase))<<",\"phase_mix\":"<<c.designPhaseMix<<",\"apodizing\":"<<(c.apodizing?"true":"false")<<",\"apod_F1_ratio\":"<<c.apodF1<<",\"apod_F2_ratio\":"<<c.apodF2<<",\"high_precision_design\":"<<(c.highPrecisionDesign?"true":"false")<<",\"useFixed\":"<<(c.useFixed?"true":"false")<<",\"double_file\":"<<std::quoted(name+".f64le")<<",\"fixed_file\":";
 if(c.useFixed)o<<std::quoted(name+".q62le");else o<<"null";
 o<<",\"peak_index\":"<<peak<<",\"peak_seconds\":"<<peak/high<<",\"energy_centroid_seconds\":"<<value(centroid)/value(energy)/high<<",\"pre_peak_energy_fraction\":"<<value(pre)/value(energy)<<",\"post_peak_energy_fraction\":"<<value(post)/value(energy)<<",\"max_mirror_coefficient_difference\":"<<sym<<",\"time_scope\":\"per-stage exact-coefficient DD energy statistics, not full pipeline impulse\"}";
}
int main(int argc,char**argv){try{_putenv_s("RLPLAYER_FILTER_CACHE","0");if(argc==3&&std::string(argv[1])=="--catalog"){std::ofstream o(argv[2]);o<<"[";bool first=true;for(auto&p:dsp::filter_presets()){if(!first)o<<",";first=false;o<<std::quoted(p.name);}o<<"]";return !o;}
 if(argc!=6)throw std::runtime_error("preset input_rate output_rate directory max_tool_bytes");auto*p=dsp::find_preset(argv[1]);if(!p)throw std::runtime_error("unknown preset");unsigned in=std::stoul(argv[2]),out=std::stoul(argv[3]);uint64_t limit=std::stoull(argv[5]);fs::path dir=argv[4];fs::create_directories(dir);std::ofstream o(dir/"row.json");o<<std::setprecision(17)<<"{\"preset\":"<<std::quoted(p->name)<<",\"input_rate\":"<<in<<",\"output_rate\":"<<out<<",\"channels\":2,\"input_bits\":24,\"source_bits\":24,\"output_bits\":24,\"isFloat\":false,\"requested_useFixed\":true,\"gpu_requested\":false,\"automatic_taps\":true,\"automatic_cascade\":true,\"engineering_context\":\"explicit compatible offline PCM target; GUI automatic prefer policy is a separate gate\",\"tool_admission_bytes\":"<<limit;
 dsp::ResamplerConfig rc;rc.inRate=in;rc.outRate=out;rc.channels=2;rc.autoTaps=true;rc.allowAutomaticCascade=true;dsp::apply_preset(rc,*p);dsp::ResamplerPlan plan;
 try{rc=dsp::resolve_auto_taps(rc);plan=dsp::plan_resampler(rc);}catch(const std::exception&e){o<<",\"status\":\"NATIVE_PLAN_REJECTION\",\"reason\":"<<std::quoted(e.what())<<"}";return 3;}
 // Pipeline makes this same decision from its actual first stage and 2ch rate.
 bool expectedFixed=double(plan.stages[0].T)*plan.stages[0].config.outRate*2<=250e6;rc.useFixed=expectedFixed;plan=dsp::plan_resampler(rc);const uint64_t peakBound=plan.designPeakBytes+(64ull<<20);
 o<<",\"planner_peak_bytes\":"<<plan.designPeakBytes<<",\"tool_peak_bound_bytes\":"<<peakBound<<",\"planned_stage_count\":"<<plan.stageCount<<",\"planned_coefficient_count\":"<<plan.coefficientCount<<",\"product_max_prototype_taps\":"<<rc.maxPrototypeTaps;
 if(peakBound>limit){o<<",\"status\":\"RESOURCE_TOOL_ADMISSION\",\"reason\":\"conservative tool workspace limit, not a product unsupported declaration\"}";return 3;}
 auto bandRate=std::min(in,out);auto metrics=dsp::preset_metrics(*p,bandRate,dsp::content_hz_for_rate(bandRate));double fp=metrics.passHz,stop=double(bandRate)-fp,bin=double(bandRate)/std::max(1,plan.stages[0].T);
 std::vector<double>freq={0,fp*.25,fp*.5,fp*.75,fp*.9,fp,stop,stop+bin*.25,stop+bin*.5,stop+bin,stop+bin*2,stop+bin*4,bandRate*.75,double(bandRate)};if(p->apodizing)freq.push_back(p->apodF2*bandRate/2.);for(double f:freq)points.push_back({f});
 engine::PipelineConfig cfg;cfg.outRate=out;cfg.autoTaps=true;cfg.useFixed=true;cfg.sourceBits=24;cfg.outBits=24;cfg.gpuResample=false;engine::apply_filter_preset(cfg,*p);engine::Pipeline pipe(cfg);audio::AudioBuffer input;input.rate=in;input.bits=24;input.channels=2;input.isFloat=false;input.raw={0,0};engine::PipelineStats stats;std::vector<std::vector<double>>samples;
 auto start=std::chrono::steady_clock::now();try{pipe.process(input,&stats,&samples);}catch(const std::exception&e){o<<",\"status\":\"NATIVE_PREPARE_REJECTION\",\"reason\":"<<std::quoted(e.what())<<"}";return 3;}double cold=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();const auto*r=pipe.prepared_resampler();if(!r)throw std::runtime_error("actual resampler absent");if(r->coefficient_count()!=plan.coefficientCount||stats.usedFixed!=expectedFixed)throw std::runtime_error("actual Pipeline/preflight identity mismatch");if(stats.cpuParallelThreads>2)throw std::runtime_error("tool CPU thread limit exceeded");
 o<<",\"status\":\"ACTUAL_PIPELINE_FINITE_POINT_SCREEN\",\"proof_scope\":\"POINTS_ONLY_NOT_CONTINUOUS; desired factor response, not all decimator alias paths\",\"actual_usedFixed\":"<<(stats.usedFixed?"true":"false")<<",\"actual_cascade\":"<<(r->two_stage()?"true":"false")<<",\"cpu_threads_last_block\":"<<stats.cpuParallelThreads<<",\"phase_mix\":"<<rc.designPhaseMix<<",\"pass_hz\":"<<fp<<",\"mirror_stop_hz\":"<<stop<<",\"apodizing\":"<<(p->apodizing?"true":"false")<<",\"apod_F1_hz\":";if(p->apodizing)o<<p->apodF1*bandRate/2.;else o<<"null";o<<",\"apod_F2_hz\":";if(p->apodizing)o<<p->apodF2*bandRate/2.;else o<<"null";
 o<<",\"declared_stop_target_db\":"<<metrics.ceilingDb<<",\"content_excluded\":"<<(metrics.excluded?"true":"false")<<",\"cold_prepare_seconds\":"<<cold<<",\"stages\":[";
 if(r->two_stage()){leaf(*r->first_stage(),dir,1,o);o<<",";leaf(*r->second_stage(),dir,2,o);}else leaf(*r,dir,1,o);
 o<<"],\"points\":[";for(size_t i=0;i<points.size();++i){if(i)o<<",";const auto&q=points[i];o<<"{\"hz\":"<<q.hz<<",\"kind\":"<<std::quoted(q.hz<=fp?"pass":q.hz>=stop?"mirror_stop":"transition")<<",\"desired_response_db\":"<<q.db<<",\"group_delay_seconds\":";if(std::isfinite(q.delay))o<<q.delay;else o<<"null";o<<",\"phase_radians_wrapped\":"<<std::remainder(q.phase,2*dsp::kPi)<<"}";}
 PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof m;GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);o<<"],\"peak_working_set_bytes\":"<<m.PeakWorkingSetSize<<",\"peak_commit_bytes\":"<<m.PeakPagefileUsage<<",\"memory_scope\":\"whole probe peak incl prepare/export/DD measurement; not isolated player residency\",\"fixed_measurement\":\"actual private Q62 integers split into exact signed high32 and low32 DD components before response accumulation\"}";return !o;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 2;}}
