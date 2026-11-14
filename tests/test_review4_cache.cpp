#include <mutex>
#include <cstdlib>
#include "dsp/filter_cache.h"
#include "dsp/filter_design.h"
#include <bit>
#include <cfenv>
#include <chrono>
#include <limits>
#include <thread>
#include <atomic>
#include <new>
static std::atomic<size_t> rejectAllocationAt{0};
void* operator new(size_t n) { if(auto limit=rejectAllocationAt.load();limit && n>=limit)throw std::bad_alloc();if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
#include <psapi.h>

namespace fc = rta::dsp::filter_cache;
static int checks=0, fails=0;
static void ck(bool ok, const char* name) { ++checks; if(!ok)++fails; std::printf("%s %s\n",ok?"PASS":"FAIL",name); }
static void raw(const std::string& key, uint64_t count, const std::vector<double>& h, bool extra=false) {
 std::ofstream f(rta::platform::from_utf8(fc::path_of(key)),std::ios::binary|std::ios::trunc);
 uint64_t sum=fc::fnv1a(h.data(),h.size()*sizeof(double));
 f.write("RLF1",4); f.write((char*)&count,8); f.write((char*)&sum,8);
 f.write((const char*)h.data(),h.size()*sizeof(double)); if(extra)f.put('x');
}
static SIZE_T peak() { PROCESS_MEMORY_COUNTERS p{}; p.cb=sizeof p; GetProcessMemoryInfo(GetCurrentProcess(),&p,sizeof p); return p.PeakWorkingSetSize; }
static size_t files() { size_t n=0; for(auto& e:std::filesystem::directory_iterator(rta::platform::from_utf8(fc::directory()))) if(e.is_regular_file()) ++n; return n; }
int main() {
 auto isolated=std::filesystem::temp_directory_path() / ("rlplayer-review4-cache-"+
     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 _putenv_s("RLPLAYER_FILTER_CACHE_DIR",rta::platform::to_utf8(isolated).c_str());
 _putenv_s("RLPLAYER_FILTER_CACHE","");
 std::printf("cache_dir=%s\n",fc::directory().c_str());
 ck(files()==0,"isolated_empty_start"); if(files()!=0)return 2;
 std::vector<double> good(fc::kMinTapsToCache,1.0/double(fc::kMinTapsToCache)),out{123};
 fc::store("valid",good); ck(fc::load("valid",out)&&out==good,"valid_roundtrip");
 raw("trailing",good.size(),good,true); ck(!fc::load("trailing",out),"reject_trailing_byte");
 auto bad=good; bad[100]=std::numeric_limits<double>::quiet_NaN();
 raw("nan",bad.size(),bad); ck(!fc::load("nan",out),"reject_checksummed_nan");
 bad[100]=std::numeric_limits<double>::infinity();
 raw("inf",bad.size(),bad); ck(!fc::load("inf",out),"reject_checksummed_inf");
 fc::store("store_inf",bad); ck(!std::filesystem::exists(rta::platform::from_utf8(fc::path_of("store_inf"))),"refuse_store_nonfinite");
 raw("short",1ull<<24,{}); SIZE_T p0=peak(); bool hit=fc::load("short",out); SIZE_T p1=peak();
 ck(!hit,"reject_truncated_large_header"); std::printf("short_header_peak_growth_bytes=%zu\n",size_t(p1-p0));
 ck(p1-p0 < 32ull*1024*1024,"size_check_precedes_allocation");
 out={123}; raw("wrong_checksum",good.size(),good); {
  std::fstream f(rta::platform::from_utf8(fc::path_of("wrong_checksum")),std::ios::binary|std::ios::in|std::ios::out); f.seekp(24);f.put('x');
 }
 ck(!fc::load("wrong_checksum",out)&&out.empty(),"failure_clears_output");
 out={123}; bool threw=false, allocationHit=false;
 rejectAllocationAt.store(good.size()*sizeof(double));
 try { allocationHit=fc::load("valid",out); } catch(const std::bad_alloc&) {threw=true;}
 rejectAllocationAt.store(0);
 ck(!threw&&!allocationHit&&out.empty(),"allocation_failure_is_safe_miss");
 // Negative control: a checksum authenticates bytes, never a DSP design.
 auto wrong=good;wrong[0]+=.01;wrong[1]-=.01;raw("finite_wrong_design",wrong.size(),wrong);
 ck(fc::load("finite_wrong_design",out)&&out==wrong,"negative_control_checksum_cannot_certify_design");
 for(size_t i=0;files()<32&&i<40;++i)fc::store("fill_"+std::to_string(i),good);
 std::printf("files_before_extra=%zu\n",files()); fc::store("extra",good); std::printf("files_after_extra=%zu\n",files());
 ck(files()<=fc::kMaxFiles,"hard_32_file_cap");
 fc::store("wrong_checksum",good); ck(fc::load("wrong_checksum",out)&&out==good,"repair_corrupt_entry_at_capacity");
 // Hidden runtime input: same explicit key under different IEEE rounding modes.
 std::fesetround(FE_TONEAREST); auto sig1=fc::signature({4096,.125,180},"round");
 auto a=rta::dsp::design_lowpass({4096,.125,180,false});
 std::fesetround(FE_DOWNWARD); auto sig2=fc::signature({4096,.125,180},"round");
 auto b=rta::dsp::design_lowpass({4096,.125,180,false}); std::fesetround(FE_TONEAREST);
 size_t different=0; for(size_t i=0;i<a.size();++i) if(std::bit_cast<uint64_t>(a[i])!=std::bit_cast<uint64_t>(b[i]))++different;
 std::printf("rounding_different=%zu same_signature=%d\n",different,int(sig1==sig2));
 ck(different==0||sig1!=sig2,"rounding_mode_separates_different_designs");
#ifdef REVIEW4_FIXED
 raw("nan",bad.size(),bad); fc::store("valid",good);
 out={123}; ck(fc::load_checked("missing",out)==fc::LoadStatus::NotFound&&out.empty(),"status_missing");
 ck(fc::load_checked("nan",out)==fc::LoadStatus::Invalid,"status_invalid");
 ck(fc::load("valid",out,good.size()),"expected_count_matches");
 ck(!fc::load("valid",out,good.size()+1)&&out.empty(),"expected_count_rejects");
 auto ka=fc::design_mutex_for("a"), ka2=fc::design_mutex_for("a"), kb=fc::design_mutex_for("b");
 ck(ka==ka2&&ka!=kb,"per_key_mutex_identity");
 ka->lock(); bool acquired=kb->try_lock(); if(acquired)kb->unlock(); ka->unlock();
 ck(acquired,"unrelated_design_is_not_blocked");
 std::weak_ptr<std::mutex> weak=ka; ka.reset();ka2.reset();
 ck(weak.expired(),"expired_design_lock_released");
 ck(fc::store_checked("new_full",good)==fc::StoreStatus::Stored && fc::load("new_full",out),"capacity_evicts_LRU_instead_of_permanent_full");
 ck(fc::store_checked("over_budget",good,{good.size()*8,32})==fc::StoreStatus::SkippedSize,"explicit_oversize_quota_skip");
 std::vector<std::thread> threads;
 for(int n=0;n<8;++n) threads.emplace_back([&,n]{auto local=good; local[0]=n;fc::store("valid",local);});
 for(auto& t:threads)t.join();
 ck(fc::load("valid",out)&&out.size()==good.size()&&out[0]>=0&&out[0]<8&&out[1]==good[1],"parallel_writers_complete_payload");
 _putenv_s("RLPLAYER_FILTER_CACHE","0"); out={123};
 ck(fc::load_checked("valid",out)==fc::LoadStatus::Disabled&&out.empty(),"disabled_is_explicit");
 ck(fc::store_checked("disabled",good)==fc::StoreStatus::Disabled,"disabled_store_is_explicit");
 _putenv_s("RLPLAYER_FILTER_CACHE","");
#endif
 std::printf("checks=%d failures=%d\n",checks,fails);return fails?1:0;
}
