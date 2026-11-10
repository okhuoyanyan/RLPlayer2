// Actual scalar production resamplers; independent mathematics lives in Python.
#include "dsp/resampler.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
namespace fs=std::filesystem;using namespace rta::dsp;
void binary(const fs::path&p,const std::vector<double>&v){std::ofstream f(p,std::ios::binary);f.write((const char*)v.data(),v.size()*8);if(!f)throw std::runtime_error("export failed");}
int main(int argc,char**argv){try{if(argc!=2)return 2;_putenv_s("RLPLAYER_FILTER_CACHE","0");fs::path dir=argv[1];fs::create_directories(dir);std::ofstream o(dir/"cases.json");o<<std::setprecision(17)<<"[";size_t id=0;
 for(auto ratio:{std::pair{3u,2u},{2u,3u},{5u,4u},{4u,5u},{5u,12u},{7u,12u}})for(int ch:{1,2,6}){
  ResamplerConfig c;c.inRate=ratio.first*1000;c.outRate=ratio.second*1000;c.channels=ch;c.minTapsPerInput=7;c.stage2Taps=7;c.totalTaps=35;c.attenDb=60;c.transition=.25;c.twoStage=ratio.second==12;c.cpuKernel=ResamplerCpuKernel::Scalar;c.cpuParallel=false;c.useFixed=false;c.applyTapFloor=false;
  PolyphaseResampler identity(c);std::vector<const PolyphaseResampler*>stages;if(identity.two_stage())stages={identity.first_stage(),identity.second_stage()};else stages={&identity};
  const int M=identity.downFactor();for(int sourcePhase=0;sourcePhase<M;++sourcePhase)for(int boundary:{0,6}){
   size_t N=size_t(std::ceil(identity.tailInputSamples()))+M+31;std::vector<std::vector<double>>x(ch,std::vector<double>(N));
   for(int z=0;z<ch;++z){x[z][sourcePhase+boundary]=double(z+1)/16;x[z][sourcePhase+boundary+1]=-.125;x[z][M+boundary+3]=.25;}
   for(size_t chunk:{N,size_t(1),size_t(7),size_t(13)}){
    const std::string stem="case-"+std::to_string(id++);PolyphaseResampler r(c);std::vector<std::vector<double>>y(ch);size_t maxThreads=0;
    // Empty input must not consume clock/history. Reset between the complete
    // test cases is constructor-only; every nonempty block retains state.
    const double* nil[6]{};double* nothing[6]{};if(r.process(nil,0,nothing)!=0)throw std::runtime_error("empty block consumed input");
    for(size_t pos=0;pos<N;pos+=chunk){size_t n=std::min(chunk,N-pos);std::vector<std::vector<double>>b(ch,std::vector<double>(r.maxOutFrames(n)));std::vector<const double*>ip(ch);std::vector<double*>op(ch);for(int z=0;z<ch;++z){ip[z]=x[z].data()+pos;op[z]=b[z].data();}size_t got=r.process(ip.data(),n,op.data());maxThreads=std::max(maxThreads,r.cpu_parallel_threads_last_block());for(int z=0;z<ch;++z)y[z].insert(y[z].end(),b[z].begin(),b[z].begin()+got);}
    if(id>1)o<<",";o<<"{\"id\":"<<std::quoted(stem)<<",\"in_rate\":"<<c.inRate<<",\"out_rate\":"<<c.outRate<<",\"channels\":"<<ch<<",\"source_phase_mod_M\":"<<sourcePhase<<",\"boundary_offset\":"<<boundary<<",\"chunk\":"<<chunk<<",\"input_frames\":"<<N<<",\"output_frames\":"<<y[0].size()<<",\"max_threads\":"<<maxThreads<<",\"stages\":[";
    for(size_t j=0;j<stages.size();++j){if(j)o<<",";const auto&s=*stages[j];std::vector<double>h;s.prototype_into(h);std::string file=stem+"-h"+std::to_string(j)+".f64le";binary(dir/file,h);o<<"{\"file\":"<<std::quoted(file)<<",\"L\":"<<s.upFactor()<<",\"M\":"<<s.downFactor()<<",\"T\":"<<s.tapsPerPhase()<<"}";}
    o<<"],\"input\":[";for(int z=0;z<ch;++z){if(z)o<<",";std::string file=stem+"-x"+std::to_string(z)+".f64le";binary(dir/file,x[z]);o<<std::quoted(file);}o<<"],\"output\":[";for(int z=0;z<ch;++z){if(z)o<<",";std::string file=stem+"-y"+std::to_string(z)+".f64le";binary(dir/file,y[z]);o<<std::quoted(file);}o<<"]}";
   }
  }
 }o<<"]";if(!o)return 2;std::cout<<"actual_cases="<<id<<" device_calls=0\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 2;}}
