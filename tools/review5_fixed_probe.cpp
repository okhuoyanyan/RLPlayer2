// Read-only inspection of actual production Pipeline fixed coefficient tables.
// Explicit-instantiation member-pointer access changes no production source/layout.
#include "engine/pipeline.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
using R=rta::dsp::PolyphaseResampler;namespace fs=std::filesystem;
struct FixedTag {using type=std::vector<std::vector<int64_t>> R::*;friend type get(FixedTag);};
struct HistTag {using type=std::vector<std::vector<int64_t>> R::*;friend type get(HistTag);};
template<class Tag,typename Tag::type M>struct Access {friend typename Tag::type get(Tag){return M;}};
template struct Access<FixedTag,&R::phasesFixed_>;
template struct Access<HistTag,&R::histFixed_>;
void leaf(const R&r,const fs::path&dir,int i,std::ostream&o){
 std::vector<double>h;r.prototype_into(h);const auto&c=r.config();const auto&table=r.*get(FixedTag{});const auto&hist=r.*get(HistTag{});
 const auto name="stage"+std::to_string(i);std::ofstream f(dir/(name+".f64le"),std::ios::binary);f.write((const char*)h.data(),h.size()*8);f.close();if(!f)throw std::runtime_error("double export failure");
 bool match=true;if(c.useFixed){if(table.size()!=size_t(r.upFactor()))throw std::runtime_error("actual fixed table absent");std::vector<int64_t>flat(h.size()),oracle;rta::dsp::fixed_coeffs_from_double(h,oracle);
  for(int p=0;p<r.upFactor();++p){if(table[p].size()!=size_t(r.tapsPerPhase()))throw std::runtime_error("fixed phase length mismatch");for(int u=0;u<r.tapsPerPhase();++u)flat[p+u*r.upFactor()]=table[p][u];}
  match=flat==oracle;if(!match)throw std::runtime_error("actual fixed table differs from quantizer reconstruction");std::ofstream q(dir/(name+".q62le"),std::ios::binary);q.write((const char*)flat.data(),flat.size()*8);q.close();if(!q)throw std::runtime_error("fixed export failure");if(hist.size()!=2||hist[0].empty())throw std::runtime_error("fixed kernel did not populate actual 2ch integer history");
 }else if(!table.empty())throw std::runtime_error("unexpected fixed table in double stage");
 o<<"{\"index\":"<<i<<",\"count\":"<<h.size()<<",\"T\":"<<r.tapsPerPhase()<<",\"L\":"<<r.upFactor()<<",\"M\":"<<r.downFactor()<<",\"input_rate\":"<<c.inRate<<",\"output_rate\":"<<c.outRate<<",\"coefficient_rate\":"<<uint64_t(c.inRate)*r.upFactor()<<",\"reference_gain\":"<<r.upFactor()<<",\"useFixed\":"<<(c.useFixed?"true":"false")<<",\"actual_fixed_table_phases\":"<<table.size()<<",\"actual_fixed_history_channels\":"<<hist.size()<<",\"reconstruction_match\":"<<(match?"true":"false")<<",\"double_file\":"<<std::quoted(name+".f64le")<<",\"fixed_file\":";
 if(c.useFixed)o<<std::quoted(name+".q62le");else o<<"null";o<<"}";
}
int main(int argc,char**argv){try{_putenv_s("RLPLAYER_FILTER_CACHE","0");if(argc!=5)throw std::runtime_error("preset in out directory");auto*p=rta::dsp::find_preset(argv[1]);if(!p)throw std::runtime_error("unknown preset");unsigned in=std::stoul(argv[2]),out=std::stoul(argv[3]);fs::path dir=argv[4];fs::create_directories(dir);
 rta::engine::PipelineConfig c;c.outRate=out;c.autoTaps=true;c.useFixed=true;c.gpuResample=false;c.sourceBits=24;c.outBits=24;rta::engine::apply_filter_preset(c,*p);rta::engine::Pipeline pipeline(c);rta::audio::AudioBuffer b;b.rate=in;b.bits=24;b.channels=2;b.isFloat=false;b.raw.resize(64);for(size_t i=0;i<32;++i){b.setSample(i,0,(int(i)%7-3)/1024.);b.setSample(i,1,(int(i)%5-2)/2048.);}
 rta::engine::PipelineStats st;std::vector<std::vector<double>>y;pipeline.process(b,&st,&y);if(!st.usedFixed)throw std::runtime_error("actual Pipeline usedFixed false");if(st.cpuParallelThreads>2)throw std::runtime_error("thread bound exceeded");auto*r=pipeline.prepared_resampler();if(!r)throw std::runtime_error("no actual resampler");
 std::ofstream o(dir/"actual.json");o<<std::setprecision(17)<<"{\"object\":\"actual production Pipeline, 2ch integer24, sourceBits24/outBits24, no processing, CPU offline\",\"preset\":"<<std::quoted(p->name)<<",\"input_rate\":"<<in<<",\"output_rate\":"<<out<<",\"channels\":2,\"bits\":24,\"stats_usedFixed\":true,\"cpu_parallel_threads\":"<<st.cpuParallelThreads<<",\"fixed_coefficient_bits\":"<<rta::dsp::kFixedCoeffBits<<",\"actual_cascade\":"<<(r->two_stage()?"true":"false")<<",\"stages\":[";
 if(r->two_stage()){leaf(*r->first_stage(),dir,1,o);o<<",";leaf(*r->second_stage(),dir,2,o);}else leaf(*r,dir,1,o);o<<"]}";if(!o)throw std::runtime_error("manifest write failure");return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 2;}}
