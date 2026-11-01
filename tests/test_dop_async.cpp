// Public asynchronous seam, synchronous full-word oracle, no audio devices.
#include "dsp/dop_async.h"
#include <iostream>
#include <limits>
#include <cstdlib>
#include <new>
#include <system_error>
// Only the selected worker's first allocation fails. This reaches the actual
// production stream allocation path, without mocking the DSP implementation.
static thread_local bool failAllocation=false;
struct InjectedAllocationFailure:std::bad_alloc {const char* what() const noexcept override{return "injected worker allocation";}};
void* operator new(std::size_t n){if(failAllocation){failAllocation=false;throw InjectedAllocationFailure{};}if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc{};}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
using namespace rta::dsp;
#define REQUIRE(x) do { if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";return 1;} } while(false)
using Mods=std::vector<std::unique_ptr<SdmModulator>>;
static Mods make_mods(SdmConfig c,int n=2){Mods m;for(int i=0;i<n;++i)m.push_back(std::make_unique<SdmModulator>(c));return m;}
template<class E,class F> bool rejects(F f,const char* message=nullptr){try{f();}catch(const E& e){return !message||std::string(e.what()).find(message)!=std::string::npos;}catch(...){return false;}return false;}
struct Gate {
 std::mutex mutex;std::condition_variable cv;bool entered=false,release=false;
 std::thread::id worker;
 std::thread launch(std::function<void()> job){return std::thread([this,job=std::move(job)]{
  {std::unique_lock lock(mutex);entered=true;worker=std::this_thread::get_id();cv.notify_all();cv.wait(lock,[this]{return release;});}job();
 });}
 bool wait(){std::unique_lock lock(mutex);return cv.wait_for(lock,std::chrono::seconds(5),[this]{return entered;});}
 void open(){{std::lock_guard lock(mutex);release=true;}cv.notify_all();}
};
int main(){
 size_t cases=0,samples=0,words=0;
 for(auto family:{ModulatorFamily::Dsd,ModulatorFamily::Chebyshev,ModulatorFamily::Ec,ModulatorFamily::Ahm})
 for(double fs:{2822400.,22579200.,45158400.})for(int order:{3,5,7})for(int channels:{1,2,6}){
  SdmConfig cfg;cfg.fs=fs;cfg.family=family;cfg.order=order;cfg.oneBitOutput=true;cfg.cutoffHz=100000;cfg.ditherLsb=.0005;
  auto a=make_mods(cfg,channels),b=make_mods(cfg,channels);DopAsyncStream async(a);DopModulationStream sync;
  size_t pos=0;
  for(size_t n:{15u,1u,31u,7u,4096u,1001u}){
   std::vector<std::vector<double>> input(channels,std::vector<double>(n));
   for(int c=0;c<channels;++c)for(size_t i=0;i<n;++i){size_t k=pos+i;double v=.62*std::sin(2*kPi*(c+1)*997.*k/fs);if(k==12)v=5.;if(k==18)v=std::numeric_limits<double>::quiet_NaN();if(k==23)v=-std::numeric_limits<double>::infinity();input[c][i]=v;}
   const size_t drop=n==31?7:0;std::vector<int32_t> expected;REQUIRE(sync.process(input,b,drop,false,expected));
   async.submit(std::move(input),drop,false);REQUIRE(async.pending());
   REQUIRE(rejects<std::logic_error>([&]{async.submit({},0,true);}));
   REQUIRE(rejects<std::logic_error>([&]{async.pending_bits();}));
   REQUIRE(rejects<std::logic_error>([&]{async.reset_for_seek();}));
   auto result=async.take();REQUIRE(result.words==expected);REQUIRE(result.wallSeconds>=0);
   REQUIRE(result.pendingBitsAfter==sync.pending_bits());REQUIRE(async.pending_bits()==sync.pending_bits());REQUIRE(!async.pending());
   if(n==15)REQUIRE(result.pendingBitsAfter==15);
   pos+=n;samples+=n*channels;words+=result.words.size();
  }
  std::vector<int32_t> expected;REQUIRE(sync.process({},b,0,true,expected));async.submit({},0,true);auto eof=async.take();
  REQUIRE(eof.words==expected&&eof.pendingBitsAfter==0);words+=eof.words.size();
  // Seek preserves marker phase while clearing all 15 pending bits and SDM history.
  std::vector<std::vector<double>> partial(channels,std::vector<double>(31,.01));
  REQUIRE(sync.process(partial,b,0,false,expected));async.submit(partial,0,false);REQUIRE(async.take().words==expected);REQUIRE(async.pending_bits()==15);
  async.reset_for_seek();sync.discard_pending();for(auto& m:b)m->reset();REQUIRE(async.pending_bits()==0);
  REQUIRE(sync.process(partial,b,3,true,expected));async.submit(partial,3,true);REQUIRE(async.take().words==expected);
  for(int c=0;c<channels;++c){REQUIRE(a[c]->clamped()==b[c]->clamped());REQUIRE(a[c]->overload()==b[c]->overload());REQUIRE(a[c]->nonFinite()==b[c]->nonFinite());}
  REQUIRE(rejects<std::logic_error>([&]{async.take();}));++cases;
 }
 SdmConfig cfg;cfg.fs=2822400;cfg.order=5;cfg.oneBitOutput=true;cfg.ditherLsb=.0005;
 auto a=make_mods(cfg);DopAsyncStream async(a);
 // Invalid dimensions are delivered by take, not lost in the worker; no state consumed.
 async.submit({{.1},{}},0,false);REQUIRE(rejects<std::invalid_argument>([&]{async.take();},"lengths differ"));REQUIRE(!async.pending());REQUIRE(async.pending_bits()==0);
 async.submit({{.1},{.1}},2,false);REQUIRE(rejects<std::invalid_argument>([&]{async.discard();},"preroll"));REQUIRE(!async.pending());
 async.submit({{.1},{.1}},0,true);REQUIRE(async.take().words.size()==2);
 // A rejected multilevel input is also an asynchronous result and can be destroyed safely.
 SdmConfig multilevel=cfg;multilevel.family=ModulatorFamily::Ahm;multilevel.oneBitOutput=false;auto multi=make_mods(multilevel);
 {DopAsyncStream invalid(multi);invalid.submit({{.1},{.1}},0,false);REQUIRE(rejects<std::invalid_argument>([&]{invalid.take();},"1-bit"));}
 // Factory failures do not leave pending=true, and the next submit retries.
 int attempts=0;auto retryMods=make_mods(cfg);DopAsyncStream retry(retryMods,[&](std::function<void()> job){if(++attempts==1)throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));return std::thread(std::move(job));});
 REQUIRE(rejects<std::system_error>([&]{retry.submit({},0,true);}));REQUIRE(!retry.pending());retry.submit({},0,true);REQUIRE(retry.take().words.empty());REQUIRE(attempts==2);
 // An exception inside real stream allocation preserves its original dynamic
 // type/message; the aborted stream rejects new work until explicit reset.
 auto failedMods=make_mods(cfg);DopAsyncStream failed(failedMods,[](std::function<void()> job){return std::thread([job=std::move(job)]{failAllocation=true;job();});});
 failed.submit({{.1},{.1}},0,false);
 REQUIRE(rejects<InjectedAllocationFailure>([&]{failed.take();},"injected worker allocation"));REQUIRE(!failed.pending());
 REQUIRE(rejects<std::logic_error>([&]{failed.submit({},0,true);}));REQUIRE(rejects<std::logic_error>([&]{failed.pending_bits();}));
 failed.reset_for_seek();failed.submit({{.1},{.1}},0,true);REQUIRE(failed.take().words.size()==2);
 int launches=0;Gate gate;auto gatedMods=make_mods(cfg);DopAsyncStream gated(gatedMods,[&](std::function<void()> job){++launches;return gate.launch(std::move(job));});
 gated.submit({{.1},{.1}},0,false);const bool entered=gate.wait();
 // Worker is held by a CV, while the producer performs independent next-block work.
 // This is an ordering proof; no throughput threshold or sleep is used.
 uint64_t independent=0;for(uint64_t i=1;i<=1000;++i)independent+=i;
 const bool outstanding=gated.pending();const bool otherThread=gate.worker!=std::this_thread::get_id();gate.open();
 REQUIRE(entered&&outstanding&&otherThread&&independent==500500);gated.take();
 gated.submit({},0,true);gated.discard();gated.submit({},0,true);gated.take();REQUIRE(launches==1);
 // Destructor joins a pending job and suppresses an unobserved error.
 auto dyingMods=make_mods(cfg);{DopAsyncStream dying(dyingMods);dying.submit({{std::numeric_limits<double>::quiet_NaN()},{std::numeric_limits<double>::quiet_NaN()}},0,true);}
 REQUIRE(dyingMods[0]->nonFinite()>0&&dyingMods[1]->nonFinite()>0);
 {DopAsyncStream dying(dyingMods);dying.submit({{.1},{}},0,false);}
 // Full-scale/transient -> quiet recovery: oracle equality plus no continued clamp growth.
 auto recoveryMods=make_mods(cfg),referenceMods=make_mods(cfg);DopAsyncStream recovery(recoveryMods);DopModulationStream reference;
 std::vector<int32_t> recoveryOutput;
 auto run=[&](std::vector<std::vector<double>> input,bool finish){std::vector<int32_t> expect;if(!reference.process(input,referenceMods,0,finish,expect))return false;recovery.submit(std::move(input),0,finish);recoveryOutput=recovery.take().words;return recoveryOutput==expect;};
 std::vector<std::vector<double>> overload(2,std::vector<double>(256,1.));overload[0][0]=overload[1][0]=5.;
 REQUIRE(run(overload,false));REQUIRE(run(std::vector<std::vector<double>>(2,std::vector<double>(32768,0.)),false));
 const auto clamp=recoveryMods[0]->clamped();const auto over=recoveryMods[0]->overload();
 REQUIRE(run(std::vector<std::vector<double>>(2,std::vector<double>(8192,0.)),true));
 REQUIRE(recoveryMods[0]->clamped()==clamp&&recoveryMods[0]->overload()==over);
 size_t bitCount=0,same=0;int prior=0;double dc=0,group=0,groupEnergy=0;size_t groups=0;
 for(size_t w=0;w<recoveryOutput.size();w+=2)for(int i=15;i>=0;--i){int bit=(uint32_t(recoveryOutput[w])&(1u<<i))?1:-1;if(bitCount&&bit==prior)++same;prior=bit;++bitCount;dc+=bit;group+=bit;if(bitCount%64==0){groupEnergy+=(group/64)*(group/64);++groups;group=0;}}
 const double sameFraction=double(same)/(bitCount-1),quietRms=std::sqrt(groupEnergy/groups);
 REQUIRE(bitCount==8192&&sameFraction<.60&&std::fabs(dc/bitCount)<.05&&quietRms<.10);
 std::cout<<"quiet recovery same_fraction="<<sameFraction<<" 64bit_mean_RMS="<<quietRms<<" post_settle_clamp_growth="<<(recoveryMods[0]->clamped()-clamp)<<" overload_growth="<<(recoveryMods[0]->overload()-over)<<"\n";
 std::cout<<"PASS "<<cases<<" configurations "<<samples<<" input samples "<<words<<" full oracle words; 15bit/drop/EOF/seek/NaN/transient recovery; CV producer overlap; single persistent worker; exceptions/original-internal-exception/poison-reset/retry/destructor\n";
}
