// Actual Pipeline preset inventory. Device-free; no playback, no GPU dispatch.
#include "engine/pipeline.h"
#include "review_response.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <psapi.h>
using namespace rta;namespace fs=std::filesystem;
void contract(std::ostream& o,const dsp::FilterPreset& p){
 const auto m=dsp::preset_metrics(p,44100,dsp::content_hz_for_rate(44100));
 o<<"\"preset\":"<<std::quoted(p.name)<<",\"phase\":"<<std::quoted(dsp::phase_name(p.phase))<<",\"window\":"<<std::quoted(dsp::window_name(p.window))
 <<",\"input_rate\":44100,\"output_rate\":384000,\"pass_hz\":"<<m.passHz<<",\"mirror_stop_hz\":"<<44100.-m.passHz
 <<",\"apodizing\":"<<(p.apodizing?"true":"false")<<",\"apod_F1_ratio\":"<<p.apodF1<<",\"apod_F2_ratio\":"<<p.apodF2
 <<",\"apod_contract_F1_hz\":"<<(p.apodizing?p.apodF1*22050.:0.)<<",\"apod_contract_F2_hz\":"<<(p.apodizing?p.apodF2*22050.:0.)
 <<",\"declared_stop_target_db\":"<<m.ceilingDb<<",\"excluded_for_content\":"<<(m.excluded?"true":"false")
 <<",\"preset_taps\":"<<p.taps<<",\"preset_min_taps_per_input\":"<<p.tapsPerInput<<",\"requested_two_stage\":"<<(p.twoStage?"true":"false")
 <<",\"stop_contract_note\":\"mirror_stop_hz is image edge; apod_contract_F2_hz is a distinct rolloff endpoint; neither is silently substituted\"";
}
void leaf(std::ostream& o,const dsp::PolyphaseResampler& rs,const fs::path& dir,const char* stem){
 std::vector<double> h;rs.prototype_into(h);if(h.empty()||!dsp::filter_cache::finite_coefficients(h))throw std::runtime_error("empty/nonfinite actual coefficient object");
 std::ofstream f(dir/(std::string(stem)+".f64le"),std::ios::binary);f.write((char*)h.data(),h.size()*8);f.close();if(!f)throw std::runtime_error("coefficient export IO failure");
 const auto& c=rs.config();o<<"{\"file\":"<<std::quoted(std::string(stem)+".f64le")<<",\"count\":"<<h.size()<<",\"L\":"<<rs.upFactor()<<",\"M\":"<<rs.downFactor()
 <<",\"T\":"<<rs.tapsPerPhase()<<",\"input_rate\":"<<c.inRate<<",\"output_rate\":"<<c.outRate<<",\"coefficient_rate\":"<<uint64_t(c.inRate)*rs.upFactor()<<",\"reference_gain\":"<<std::quoted(std::to_string(rs.upFactor()))
 <<",\"transition\":"<<c.transition<<",\"attenuation\":"<<c.attenDb<<",\"window\":"<<std::quoted(dsp::window_name(c.window))<<",\"phase\":"<<std::quoted(dsp::phase_name(c.phase))
 <<",\"apodizing\":"<<(c.apodizing?"true":"false")<<",\"apod_F1_ratio\":"<<c.apodF1<<",\"apod_F2_ratio\":"<<c.apodF2<<"}";
}
int main(int argc,char** argv){
 _putenv_s("RLPLAYER_FILTER_CACHE","0");
 try{
  if(argc==3&&std::string(argv[1])=="--catalog"){
   std::ofstream o(fs::u8path(argv[2]));o<<std::setprecision(17)<<"[";bool first=true;for(auto& p:dsp::filter_presets()){if(!first)o<<",";first=false;o<<"{";contract(o,p);o<<"}";}o<<"]";return !o;
  }
  if(argc!=5||std::string(argv[1])!="--one")throw std::runtime_error("Usage: --catalog path OR --one preset directory max-preflight-bytes");
  auto* p=dsp::find_preset(argv[2]);if(!p)throw std::runtime_error("unknown preset");const auto dir=fs::u8path(argv[3]);fs::create_directories(dir);
  auto guard=review2::production_config(*p,44100,384000);guard.allowAutomaticCascade=true;const auto plan=dsp::plan_resampler(guard);
  if(plan.designPeakBytes>std::stoull(argv[4])){std::ofstream o(dir/"inventory.json");o<<std::setprecision(17)<<"{";contract(o,*p);o<<",\"status\":\"RESOURCE_ADMISSION_REJECTED_BEFORE_DESIGN\",\"preflight_peak_bytes\":"<<plan.designPeakBytes<<",\"actual_pipeline_prepared\":false}";return 3;}
  engine::PipelineConfig cfg;cfg.outRate=384000;cfg.autoTaps=true;cfg.useFixed=false;cfg.gpuResample=false;engine::apply_filter_preset(cfg,*p);
  const auto start=std::chrono::steady_clock::now();engine::Pipeline pipeline(cfg);audio::AudioBuffer input;input.rate=44100;input.bits=24;input.channels=1;input.raw={0};input.isFloat=false;std::vector<std::vector<double>> output;pipeline.process(input,nullptr,&output);const auto* rs=pipeline.prepared_resampler();if(!rs)throw std::runtime_error("actual Pipeline failed to prepare a resampler");
  if(rs->coefficient_count()!=plan.coefficientCount||rs->upFactor()!=plan.L||rs->downFactor()!=plan.M)throw std::runtime_error("preflight/actual Pipeline identity mismatch");
  std::ofstream o(dir/"inventory.json");o<<std::setprecision(17)<<"{";contract(o,*p);o<<",\"status\":\"ACTUAL_PIPELINE_EXPORTED\",\"actual_pipeline_prepared\":true,\"effective_cascade\":"<<(rs->two_stage()?"true":"false")<<",\"preflight_peak_bytes\":"<<plan.designPeakBytes<<",\"coefficient_count\":"<<rs->coefficient_count()<<",\"stages\":[";
  if(rs->two_stage()){leaf(o,*rs->first_stage(),dir,"stage1");o<<",";leaf(o,*rs->second_stage(),dir,"stage2");}else leaf(o,*rs,dir,"stage1");
  PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof m;GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);
  o<<"],\"elapsed_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<",\"peak_working_set_bytes\":"<<m.PeakWorkingSetSize<<",\"private_usage_bytes\":"<<m.PrivateUsage<<",\"process_commit_bytes\":"<<m.PagefileUsage<<",\"peak_process_commit_bytes\":"<<m.PeakPagefileUsage<<"}";return !o;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 2;}
}
