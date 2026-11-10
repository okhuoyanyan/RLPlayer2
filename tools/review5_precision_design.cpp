// Read-only numerical primitive dump. Does not open a device or claim Pipeline identity.
#include "dsp/filter_design.h"
#include "dsp/coefficient_precision.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
int main(int argc,char** argv){
 if(argc==3&&std::string(argv[1])=="--guards"){
  using namespace rta::dsp::coefficient_precision;int passed=0,total=0;
  auto rejects=[&](int n,double cutoff,double beta,double gain){++total;try{(void)lowpass(n,cutoff,beta,gain);}catch(const std::exception&){++passed;}};
  rejects(2,.1,8,1);rejects(3,0,8,1);rejects(3,.1,std::nextafter(100.,INFINITY),1);rejects(3,.1,-1,1);rejects(3,.1,NAN,1);rejects(3,INFINITY,8,1);rejects(3,.1,8,0);
  ++total;try{auto h=lowpass(3,.1,100,1);if(h.size()==3&&std::isfinite(h[1]))++passed;}catch(...){}
  std::atomic_bool cancel=true;++total;try{rta::dsp::DesignCancellationScope scope(&cancel);(void)lowpass(81732,.00296875,36.50926,147);}catch(const rta::dsp::DesignCancelled&){++passed;}
  const int previous=std::fegetround();std::fesetround(FE_DOWNWARD);++total;try{(void)lowpass(3,.1,8,1);}catch(const std::runtime_error&){++passed;}std::fesetround(previous);
  std::ofstream o(argv[2]);o<<"{\"checks\":"<<total<<",\"passed\":"<<passed<<",\"failed\":"<<(total-passed)<<",\"beta_100_accepted\":true,\"beta_above_100_rejected\":true}";return passed==total?0:1;
 }
 if(argc==3&&std::string(argv[1])=="--samples"){
  using namespace rta::dsp::coefficient_precision;std::ofstream out(argv[2]);out<<std::setprecision(17)<<"[";bool first=true;
  auto emit=[&](const char* kind,int n,int i,double beta,double cutoff,double x,DD result){if(!first)out<<",";first=false;out<<"{\"kind\":"<<std::quoted(kind)<<",\"n\":"<<n<<",\"i\":"<<i<<",\"beta\":"<<beta<<",\"cutoff\":"<<cutoff<<",\"x\":"<<x<<",\"hi\":"<<result.hi<<",\"lo\":"<<result.lo<<"}";};
  for(double beta:{0.,1e-8,8.,20.,36.509260000000005,60.,100.}){
   emit("i0",0,0,beta,0,0,bessel_i0({beta,0}));
   for(int n:{1,3,4,17,513,81732})for(int i:{0,n/2,n-1})emit("window",n,i,beta,0,0,window(n,i,beta));
  }
  for(double cutoff:{1.,.5,.00296875,std::ldexp(1.,-40),std::nextafter(1.,0.)})for(double x:{0.,.5,-.5,1.,-1.,1000000.5})emit("sinc",0,0,0,cutoff,x,sinc(cutoff,x));
  std::mt19937_64 random(0x34126);for(int j=0;j<100;++j){int n=3+int(random()%100000),i=int(random()%unsigned(n));double beta=double(random()%1000000)/20000.;emit("window",n,i,beta,0,0,window(n,i,beta));double c=double((random()%1000000)+1)/1000001.,x=double(int(random()%2000000)-1000000)*.5;emit("sinc",0,0,0,c,x,sinc(c,x));}
  out<<"]";std::cout<<"DD scalar samples written\n";return !out;
 }
 if(argc==3&&std::string(argv[1])=="--dd"){
  const auto dir=std::filesystem::path(argv[2]);std::filesystem::create_directories(dir);
  const double cutoff=std::min(1./147.,1./320.)*(1.-.1*.5),beta=rta::dsp::kaiser_beta_for_attenuation(340);
  for(double gain:{1.,147.}){auto h=rta::dsp::coefficient_precision::lowpass(81732,cutoff,beta,gain);std::ofstream f(dir/(gain==1?"dd-unit.f64le":"dd-raw.f64le"),std::ios::binary);f.write((char*)h.data(),h.size()*8);}
  std::cout<<"DD finite FIR generated; EXPERIMENTAL_NOT_PIPELINE\n";return 0;
 }
 if(argc!=3)return 2;const int n=81732;const double gain=147,atten=340,transition=.1;
 const double cutoff=std::min(1./147.,1./320.)*(1.-transition*.5);
 const double beta=rta::dsp::kaiser_beta_for_attenuation(atten);const auto dir=std::filesystem::path(argv[1]);std::filesystem::create_directories(dir);
 auto dump=[&](const char* name,const std::vector<double>& h){std::ofstream f(dir/name,std::ios::binary);f.write((char*)h.data(),h.size()*8);if(!f)throw std::runtime_error("dump failed");};
 const auto w=rta::dsp::kaiser_window(n,beta);std::vector<double> sinc(n),product(n);
 for(int i=0;i<n;++i){double x=i-(n-1)*.5;sinc[i]=x==0?cutoff:std::sin(rta::dsp::kPi*cutoff*x)/(rta::dsp::kPi*x);product[i]=sinc[i]*w[i];}
 auto unit=rta::dsp::design_lowpass({n,cutoff,atten,false});auto raw=unit;for(auto& x:raw)x*=gain;
 dump("production-window.f64le",w);dump("production-sinc.f64le",sinc);dump("production-product.f64le",product);dump("primitive-raw.f64le",raw);
 std::ifstream f(argv[2],std::ios::binary);std::vector<double> actual(n);f.read((char*)actual.data(),n*8);size_t different=0;for(int i=0;i<n;++i)if(std::bit_cast<uint64_t>(actual[i])!=std::bit_cast<uint64_t>(raw[i]))++different;
 std::ofstream j(dir/"parameters.json");j<<std::setprecision(17)<<"{\"status\":\"EXPERIMENTAL_NOT_PIPELINE\",\"n\":"<<n<<",\"reference_gain\":147,\"attenuation\":340,\"transition\":0.1,\"cutoff_binary64\":"<<cutoff<<",\"beta_binary64\":"<<beta<<",\"primitive_vs_actual_different\":"<<different<<"}";
 std::cout<<std::setprecision(17)<<"n="<<n<<" cutoff="<<cutoff<<" beta="<<beta<<" primitive_vs_actual_different="<<different<<"\n";return different?1:0;
}
