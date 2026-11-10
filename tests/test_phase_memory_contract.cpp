// Allocation-lifetime and numerical contract. This is an offline, device-free test.
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>
static std::atomic<size_t> live{0}, peak{0}, allocations{0};
// Synchronize the real FFT at its scratch allocation, after bit reversal.
// A fixed sleep races a fast FFT and can report failure after it completed.
static thread_local bool pauseFftAllocation=false;
static std::atomic_bool allocationReached{false}, allocationReleased{false};
struct alignas(std::max_align_t) AllocationHeader { size_t bytes; };
void* operator new(size_t n){
 auto* h=static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader)+(n?n:1)));if(!h)throw std::bad_alloc();h->bytes=n;
 const auto current=live.fetch_add(n)+n;auto prior=peak.load();while(current>prior&&!peak.compare_exchange_weak(prior,current)){}
 allocations.fetch_add(1);
 if(pauseFftAllocation){
  pauseFftAllocation=false;allocationReached=true;
  while(!allocationReleased.load())std::this_thread::yield();
 }
 return h+1;
}
void operator delete(void* p) noexcept {if(!p)return;auto* h=static_cast<AllocationHeader*>(p)-1;live.fetch_sub(h->bytes);std::free(h);}
void operator delete(void* p,size_t) noexcept {::operator delete(p);}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete[](void* p,size_t) noexcept {::operator delete(p);}
#include "dsp/phase_design_precision.h"
#include "dsp/filter_families.h"
#include "phase_precision_before_fixture.h"
#include "phase_minimum_before_fixture.h"
static int checks=0,failures=0;
void ck(bool ok,const char* name){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",name);}
struct Measurement{size_t peakBytes,allocations;};
template<class F> auto measure(F&& f,Measurement& m){size_t base=live.load(),a=allocations.load();peak.store(base);auto result=f();m={peak.load()-base,allocations.load()-a};return result;}
bool identical(const std::vector<double>& a,const std::vector<double>& b){return a.size()==b.size()&&!std::memcmp(a.data(),b.data(),a.size()*sizeof(double));}
int main(){
 using namespace rta::dsp;
 std::atomic_bool cancelled=true;bool caught=false;
 {DesignCancellationScope scope(&cancelled);try{phase_detail::convolve({1},{1});}catch(const DesignCancelled&){caught=true;}}
 ck(caught,"DD_pre_cancelled_before_scratch_allocation");
 bool badShape=false;try{std::vector<phase_detail::DC> x(3);phase_detail::fft(x);}catch(const std::invalid_argument&){badShape=true;}
 ck(badShape,"DD_non_power_of_two_rejected");
 ck(sizeof(phase_detail::roots)/sizeof(phase_detail::roots[0])>=27,"DD_41M_support_stage26_root_exists");
 // Cooperative cancellation during a real (small-memory) DD FFT.
 cancelled=false;std::vector<phase_detail::DC> pending(1<<18);caught=false;
 std::atomic_bool fftDone{false};
 std::chrono::steady_clock::time_point cancelStart;
 std::thread cancelling([&]{
  while(!allocationReached.load()&&!fftDone.load())std::this_thread::yield();
  cancelStart=std::chrono::steady_clock::now();cancelled=true;allocationReleased=true;
 });
 // Observe the allocation on this thread rather than its requested byte size:
 // MSVC's vector adds alignment padding to large allocations.
 pauseFftAllocation=true;
 {DesignCancellationScope scope(&cancelled);try{phase_detail::fft(pending);}catch(const DesignCancelled&){caught=true;}}
 const auto cancelEnd=std::chrono::steady_clock::now();fftDone=true;cancelling.join();pauseFftAllocation=false;
 const auto cancelMs=std::chrono::duration<double,std::milli>(cancelEnd-cancelStart).count();
 std::printf("DD_cancel_after_scratch_reached=%d caught=%d response_ms=%.6f\n",int(allocationReached.load()),int(caught),cancelMs);
 ck(allocationReached&&caught&&cancelMs>=0&&cancelMs<250,"DD_inflight_cancel_under_250ms");
 // An exact rational oracle: integer convolution cannot accumulate rounding
 // error here, and /2^40 converts to a representable double independently of FFT.
 std::vector<double> a(97),b(83);std::vector<int64_t> ia(a.size()),ib(b.size()),exact(a.size()+b.size()-1);
 for(size_t i=0;i<a.size();++i){ia[i]=int((i*173+31)%2001)-1000;a[i]=std::ldexp(double(ia[i]),-20);}
 for(size_t i=0;i<b.size();++i){ib[i]=int((i*137+73)%2001)-1000;b[i]=std::ldexp(double(ib[i]),-20);}
 for(size_t i=0;i<a.size();++i)for(size_t j=0;j<b.size();++j)exact[i+j]+=ia[i]*ib[j];
 Measurement oldm{},newm{};
 auto before=measure([&]{return phase_before::convolve(a,b);},oldm);auto after=measure([&]{return phase_detail::convolve(a,b);},newm);
 ck(identical(before,after),"DD_convolution_bits_unchanged");double maxError=0;for(size_t i=0;i<after.size();++i)maxError=std::max(maxError,std::abs(after[i]-std::ldexp(double(exact[i]),-40)));
 std::printf("independent_integer_oracle_max_abs=%.17g\n",maxError);ck(maxError<1e-25,"independent_exact_rational_convolution_oracle");
 a.resize(2049);b.resize(2051);for(size_t i=0;i<a.size();++i)a[i]=std::sin(.12*i)*std::exp(-.001*i);for(size_t i=0;i<b.size();++i)b[i]=std::cos(.17*i)*std::exp(-.002*i);
 before=measure([&]{return phase_before::convolve(a,b);},oldm);after=measure([&]{return phase_detail::convolve(a,b);},newm);
 ck(identical(before,after),"DD_large_convolution_bits_unchanged");std::printf("DD_before_peak=%zu after_peak=%zu before_allocations=%zu after_allocations=%zu\n",oldm.peakBytes,newm.peakBytes,oldm.allocations,newm.allocations);
 ck(newm.allocations<oldm.allocations,"DD_scratch_allocation_count_reduced");
 // Multiple support/padding/real-symmetry combinations: do not shorten L or N.
 for(size_t length:{size_t(17),size_t(128),size_t(1025)})for(size_t padding:{size_t(8),size_t(32),size_t(64)})for(bool real:{false,true}){
  auto h=design_lowpass({int(length),.125,140,false});auto x=phase_before::to_minimum_phase(h,padding,real);auto y=to_minimum_phase(h,padding,real);ck(identical(x,y),"minimum_phase_original_bits_all_padding_and_symmetry");
 }
 auto h=design_lowpass({4097,.125,140,false});
 before=measure([&]{return phase_before::to_minimum_phase(h,32,true);},oldm);after=measure([&]{return to_minimum_phase(h,32,true);},newm);
 ck(identical(before,after),"minimum_phase_memory_probe_bit_identical");
 std::printf("MP_L=%zu padding=32 old_heap_peak=%zu new_heap_peak=%zu ratio=%.9f old_allocations=%zu new_allocations=%zu\n",h.size(),oldm.peakBytes,newm.peakBytes,double(newm.peakBytes)/oldm.peakBytes,oldm.allocations,newm.allocations);
 ck(newm.peakBytes<=oldm.peakBytes*.60,"MP_live_heap_peak_at_most_60_percent");
 // Do not claim these allocator figures are operating-system working set or commit.
 std::printf("phase_memory_contract checks=%d failures=%d metric=tracked_cpp_live_allocation_bytes\n",checks,failures);return failures?1:0;
}
