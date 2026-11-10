// Device-free actual Pipeline comparison. Spectral numbers are finite points only.
#include "engine/pipeline.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <psapi.h>
#ifndef REVIEW_BASELINE
#include "review_response.h"
#endif
using namespace rta; namespace fs=std::filesystem;
// One fixed, calibrated measurement implementation for both source versions.
#include "review5_gaussian_response.h"
namespace rp=review5_response;
struct Point {double f,db,delay;};
std::vector<Point> points;
void leaf(std::ostream& o,const dsp::PolyphaseResampler& r,const fs::path& dir,int index){
 std::vector<double> h;r.prototype_into(h);auto c=r.config();const double fsamp=double(c.inRate)*r.upFactor();
 std::string file="stage"+std::to_string(index)+".f64le";std::ofstream b(dir/file,std::ios::binary);b.write((char*)h.data(),h.size()*8);b.close();if(!b)throw std::runtime_error("coefficient write failed");
 double e=0,pre=0,post=0,centroid=0;size_t peak=0;for(size_t i=0;i<h.size();++i)if(std::abs(h[i])>std::abs(h[peak]))peak=i;
 for(size_t i=0;i<h.size();++i){double q=h[i]*h[i];e+=q;centroid+=i*q;if(i<peak)pre+=q;if(i>peak)post+=q;}
 for(auto& p:points){auto w=rp::rotation(std::remainder(p.f,fsamp),fsamp);rp::DC z{{1,0},{0,0}},s,j;
  for(size_t i=0;i<h.size();++i){rp::DC a{rp::mul({h[i],0},z.r),rp::mul({h[i],0},z.i)};s=rp::add(s,a);j=rp::add(j,{rp::mul({double(i),0},a.r),rp::mul({double(i),0},a.i)});z=rp::mul(z,w);}
  double re=s.r.h+s.r.l,im=s.i.h+s.i.l,jr=j.r.h+j.r.l,ji=j.i.h+j.i.l;double mag=std::hypot(re,im)/r.upFactor();
  p.db+=20*std::log10(std::max(mag,1e-300));p.delay+=(jr*re+ji*im)/(re*re+im*im)/fsamp;
 }
 o<<"{\"file\":"<<std::quoted(file)<<",\"count\":"<<h.size()<<",\"L\":"<<r.upFactor()<<",\"M\":"<<r.downFactor()<<",\"T\":"<<r.tapsPerPhase()<<",\"coefficient_rate\":"<<fsamp<<",\"reference_gain\":"<<r.upFactor()<<",\"window\":"<<std::quoted(dsp::window_name(c.window))<<",\"phase\":"<<std::quoted(dsp::phase_name(c.phase))<<",\"attenuation\":"<<c.attenDb<<",\"declared_gaussian_alpha\":";
 if(c.window==dsp::WindowKind::Gaussian)o<<3+.011*std::max(0.,c.attenDb-60);else o<<"null";
 o<<",\"peak_index\":"<<peak<<",\"peak_seconds\":"<<peak/fsamp<<",\"pre_peak_energy_fraction\":"<<pre/e<<",\"post_peak_energy_fraction\":"<<post/e<<",\"energy_centroid_seconds\":"<<centroid/e/fsamp<<"}";
}
int main(int argc,char**argv){_putenv_s("RLPLAYER_FILTER_CACHE","0");try{
 if(argc==3&&std::string(argv[1])=="--catalog"){std::ofstream o(argv[2]);o<<"[";bool first=true;for(auto&p:dsp::filter_presets())if(p.window==dsp::WindowKind::Gaussian){if(!first)o<<",";first=false;o<<std::quoted(p.name);}o<<"]";return !o;}
 if(argc!=5)throw std::runtime_error("preset input output directory");auto*p=dsp::find_preset(argv[1]);if(!p)throw std::runtime_error("unknown preset");unsigned in=std::stoul(argv[2]),out=std::stoul(argv[3]);fs::path dir=fs::u8path(argv[4]);fs::create_directories(dir);std::ofstream o(dir/"row.json");o<<std::setprecision(17);
 auto metrics=dsp::preset_metrics(*p,in,dsp::content_hz_for_rate(in));double fp=metrics.passHz,stop=in-fp;
 int t=p->tapsPerInput>0?p->tapsPerInput:dsp::solve_taps_per_phase(p->window,p->transition,p->apodizing,p->apodF1,p->apodF2,p->taps,0,double(in));
 uint64_t n=uint64_t(t)*(out/std::gcd(in,out)),estimate=n*32+(32ull<<20);
#ifndef REVIEW_BASELINE
 auto guard=review2::production_config(*p,in,out);guard.allowAutomaticCascade=true;estimate=dsp::plan_resampler(guard).designPeakBytes;
#else
 if(p->phase!=dsp::PhaseKind::Linear&&n<=(1u<<24)){uint64_t fft=1;while(fft<std::max<uint64_t>(n/2,1)*64)fft*=2;estimate=fft*56+n*64+(32ull<<20);}
#endif
 o<<"{\"preset\":"<<std::quoted(p->name)<<",\"input_rate\":"<<in<<",\"output_rate\":"<<out<<",\"source_support_T\":"<<t<<",\"legacy_single_count_estimate\":"<<n<<",\"preflight_peak_bytes\":"<<estimate<<",\"pass_hz\":"<<fp<<",\"mirror_stop_hz\":"<<stop<<",\"apodizing\":"<<(p->apodizing?"true":"false")<<",\"F1_hz\":"<<(p->apodizing?p->apodF1*in/2:fp)<<",\"F2_hz\":";
 if(p->apodizing)o<<p->apodF2*in/2;else o<<"null";o<<",\"phase_mix\":"<<dsp::phase_mix()<<",\"phase\":"<<std::quoted(dsp::phase_name(p->phase));
 if(estimate>(768ull<<20)&&n<=(1u<<24)){o<<",\"status\":\"RESOURCE_ADMISSION_REJECTED\",\"reason\":\"conservative preallocation estimate exceeds 768MiB; no fabricated response\"}";return 3;}
 engine::PipelineConfig c;c.outRate=out;c.autoTaps=true;c.useFixed=false;c.gpuResample=false;engine::apply_filter_preset(c,*p);
 auto start=std::chrono::steady_clock::now();engine::Pipeline pipeline(c);audio::AudioBuffer input;input.rate=in;input.channels=1;input.bits=24;input.isFloat=false;input.raw={0};std::vector<std::vector<double>> output;
 try{pipeline.process(input,nullptr,&output);}catch(const std::exception&e){o<<",\"status\":\"NATIVE_PREPARE_REJECTED\",\"reason\":"<<std::quoted(e.what())<<"}";return 3;}
 double cold=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();auto*r=pipeline.prepared_resampler();if(!r)throw std::runtime_error("missing actual object");
 // Fixed 15-point screen: exact edges plus source-domain offsets, never a band certificate.
 std::vector<double> frequencies={0,fp*.5,fp*.9,fp,stop,stop+double(in)/t*.25,stop+double(in)/t*.5,stop+double(in)/t,stop+double(in)/t*2,stop+double(in)/t*4,double(in)*.75,double(in),double(out)/2};if(p->apodizing)frequencies.push_back(p->apodF2*in/2);for(double f:frequencies)points.push_back({f,0,0});
 o<<",\"status\":\"ACTUAL_PIPELINE_POINT_SCREEN\",\"cold_prepare_seconds\":"<<cold<<",\"effective_cascade\":"<<(r->two_stage()?"true":"false")<<",\"coefficient_count\":"<<r->coefficient_count()<<",\"stages\":[";
 if(r->two_stage()){leaf(o,*r->first_stage(),dir,1);o<<",";leaf(o,*r->second_stage(),dir,2);}else leaf(o,*r,dir,1);
 o<<"],\"points\":[";for(size_t i=0;i<points.size();++i){if(i)o<<",";auto q=points[i];o<<"{\"hz\":"<<q.f<<",\"desired_component_db\":"<<q.db<<",\"group_delay_seconds\":";if(std::isfinite(q.delay))o<<q.delay;else o<<"null";o<<"}";}
 o<<"]";
 // Actual float32 2^-10 impulse, never opening an audio endpoint. Small blocks
 // keep resampler output under the internal parallel-dispatch threshold.
 pipeline.seek_reset();input.bits=32;input.isFloat=true;std::vector<double> impulse;size_t total=size_t(std::ceil(r->tailInputSamples()))+8,maxThreads=0;
 for(size_t k=0;k<total;k+=32){input.raw.assign(std::min<size_t>(32,total-k),0);if(k==0)input.setSample(0,0,1./1024);engine::PipelineStats stats;pipeline.process(input,&stats,&output);maxThreads=std::max(maxThreads,stats.cpuParallelThreads);if(!output.empty())impulse.insert(impulse.end(),output[0].begin(),output[0].end());}
 if(maxThreads>2)throw std::runtime_error("CPU thread admission violated");size_t pk=0;double energy=0,pre=0,post=0,centroid=0;for(size_t i=0;i<impulse.size();++i)if(std::abs(impulse[i])>std::abs(impulse[pk]))pk=i;
 for(size_t i=0;i<impulse.size();++i){double q=impulse[i]*impulse[i];energy+=q;centroid+=i*q;if(i<pk)pre+=q;if(i>pk)post+=q;}
 if(!(energy>0))throw std::runtime_error("zero actual impulse");o<<",\"actual_impulse\":{\"input_peak\":0.0009765625,\"source_frames\":"<<total<<",\"output_frames\":"<<impulse.size()<<",\"peak_index\":"<<pk<<",\"peak_seconds\":"<<double(pk)/out<<",\"energy_centroid_seconds\":"<<centroid/energy/out<<",\"pre_peak_energy_fraction\":"<<pre/energy<<",\"post_peak_energy_fraction\":"<<post/energy<<",\"max_cpu_parallel_threads\":"<<maxThreads<<",\"scope\":\"one actual source/output phase; not a symmetry test\"}";
 PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof m;GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);o<<",\"peak_working_set_bytes\":"<<m.PeakWorkingSetSize<<",\"peak_commit_bytes\":"<<m.PeakPagefileUsage<<",\"spectral_scope\":\"FINITE_POINTS_ONLY_NOT_CONTINUOUS; desired transfer factors composed at same physical Hz; no alias sum guarantee\"}";return !o;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 2;}}
