#include "dsp/filter_cache.h"
#include <limits>
#include <thread>
#include <cstdio>
#include <cmath>
namespace fc=rta::dsp::filter_cache;
int failures=0,checks=0;
void ck(bool ok,const char* name){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",name);}
std::vector<double> taps(){return std::vector<double>(fc::kMinTapsToCache,.125);}
constexpr uint64_t entryBytes=20+fc::kMinTapsToCache*8;
std::filesystem::path dir;
void setup(const std::string& path){_putenv_s("RLPLAYER_FILTER_CACHE_DIR",path.c_str());dir=rta::platform::from_utf8(fc::directory());}
PROCESS_INFORMATION launch(const std::string& args){
 wchar_t module[32768];GetModuleFileNameW(nullptr,module,32768);
 std::wstring cmd=L"\""+std::wstring(module)+L"\" "+rta::platform::from_utf8(args).wstring();
 STARTUPINFOW s{};s.cb=sizeof s;PROCESS_INFORMATION p{};
 if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&s,&p))++failures;
 return p;
}
DWORD join(PROCESS_INFORMATION p){if(!p.hProcess)return 999;WaitForSingleObject(p.hProcess,30000);DWORD code=999;GetExitCodeProcess(p.hProcess,&code);CloseHandle(p.hThread);CloseHandle(p.hProcess);return code;}
void clear_owned(){fc::ProcessLock lock("directory");std::vector<fc::Entry> e;uint64_t bytes;fc::inventory(e,bytes);for(auto& x:e){std::error_code ec;std::filesystem::remove(x.path,ec);}}
struct WorkerReport {unsigned stored=0,busy=0,auditBusy=0;bool complete=false;};
WorkerReport report(DWORD pid){WorkerReport r;std::ifstream f(dir/("worker-result-"+std::to_string(pid)+".log"));r.complete=bool(f>>r.stored>>r.busy>>r.auditBusy);return r;}
bool payload_matches(const std::string& key,const std::vector<double>& h,int workers){
 if(h.size()!=fc::kMinTapsToCache||!std::all_of(h.begin()+1,h.end(),[](double x){return x==.125;}))return false;
 if(key.starts_with("old_"))return h[0]==.125;
 if(key=="same")return h[0]>=0&&h[0]<workers&&h[0]==std::floor(h[0])&&std::bit_cast<uint64_t>(h[0])==std::bit_cast<uint64_t>(double(int(h[0])));
 if(key.starts_with("worker_")){const auto end=key.find('_',7);if(end==std::string::npos)return false;const auto id=key.substr(7,end-7);return h[0]>=0&&h[0]<workers&&h[0]==std::floor(h[0])&&id==std::to_string(int(h[0]))&&std::bit_cast<uint64_t>(h[0])==std::bit_cast<uint64_t>(double(int(h[0])));}
 return false;
}
int main(int argc,char** argv){
 if(argc>2){setup(argv[2]);auto h=taps();
  if(std::string(argv[1])=="unsafe")return fc::directory().empty()?0:9;
  if(std::string(argv[1])=="lock"){
   fc::ProcessLock lock("directory");std::ofstream(dir/"lock.ready")<<"1";
   for(int n=0;n<200&&!std::filesystem::exists(dir/"lock.done");++n)Sleep(10);return 0;
  }
  if(std::string(argv[1])=="audit-lock"){
   fc::ProcessLock lock("directory",1000);if(!lock)return 10;std::ofstream(dir/"audit-lock.ready")<<"1";
   for(int n=0;n<1000&&!std::filesystem::exists(dir/"audit-busy.ready");++n)Sleep(10);
   return std::filesystem::exists(dir/"audit-busy.ready")?0:11;
  }
  if(std::string(argv[1])=="crash"){
   fc::ProcessLock lock("directory");std::ofstream f(dir/"fc_crash.bin.tmp.123");f<<"RLF1";f.flush();std::_Exit(77);
  }
  if(std::string(argv[1])=="pin"){
   fc::DesignLock pin("pinned"); std::ofstream(dir/"pin.ready")<<"1";
   for(int n=0;n<500&&!std::filesystem::exists(dir/"pin.done");++n)Sleep(10);return 0;
  }
  if(std::string(argv[1])=="flight"){
   fc::DesignLock pin("single");if(!pin)return 4;std::vector<double> out;
   if(!fc::load("single",out)){std::ofstream(dir/"design.count",std::ios::app)<<"design\n";Sleep(20);if(fc::store_checked("single",h)!=fc::StoreStatus::Stored)return 5;}
   return 0;
  }
  const int id=std::atoi(argv[3]);const bool same=std::string(argv[1])=="same"; const bool byteBound=std::string(argv[1])=="bytes"; const fc::CacheLimits limits{byteBound?entryBytes*3:fc::kMaxBytes,32};
#ifdef RTA_FILTER_CACHE_TESTING
  if(std::string(argv[1])=="io-fault")fc::testing::remainingWriteBytes=0;
#endif
  WorkerReport counts;
  for(int n=0;n<4;++n){h[0]=id;auto status=fc::store_checked(same?"same":("worker_"+std::to_string(id)+"_"+std::to_string(n)),h,limits);
   const auto error=GetLastError();std::ofstream trace(dir/("worker-trace-"+std::to_string(GetCurrentProcessId())+".log"),std::ios::app);
   trace<<"worker="<<id<<" iteration="<<n<<" status="<<int(status)<<" error="<<error<<std::endl;
   if(status!=fc::StoreStatus::Stored&&status!=fc::StoreStatus::Busy)return 6;
   if(status==fc::StoreStatus::Stored)++counts.stored;else ++counts.busy;
   // The cache explicitly permits bounded Busy. A test-side audit uses the same
   // bounded mutex and may also be busy: that says nothing about the ledger.
   // Recheck under lock when available; parent MUST audit after all writers join.
   fc::ProcessLock lock("directory",1000);if(!lock){++counts.auditBusy;trace<<"audit_busy"<<std::endl;std::ofstream(dir/"audit-busy.ready")<<"1";continue;}std::vector<fc::Entry> e;uint64_t bytes;
   if(!fc::inventory(e,bytes,false)||e.size()>32||bytes>limits.maxBytes){trace<<"bad_inventory entries="<<e.size()<<" bytes="<<bytes<<std::endl;return 8;}
  }std::ofstream(dir/("worker-result-"+std::to_string(GetCurrentProcessId())+".log"))<<counts.stored<<" "<<counts.busy<<" "<<counts.auditBusy;return 0;
 }
 auto path=std::filesystem::temp_directory_path()/("rlp-cache-transactions-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));setup(rta::platform::to_utf8(path));
 auto h=taps();std::vector<double> out;ck(fc::enabled(),"test_build_bound");
 ck(fc::can_store(h.size()*8)==fc::ProbeStatus::Ready,"prewarm_probe_ready_without_design");
 std::atomic_bool preCancelled=true;
 ck(fc::can_store(h.size()*8,{},&preCancelled)==fc::ProbeStatus::Cancelled,"prewarm_probe_cancelled_without_io");
 {rta::dsp::DesignCancellationScope scope(&preCancelled);ck(fc::store_checked("cancelled",h)==fc::StoreStatus::Cancelled,"cancelled_design_does_not_write_cache");}
 ck(fc::can_store(entryBytes, {entryBytes-1,32})==fc::ProbeStatus::SkippedSize,"prewarm_probe_oversize_skips");
 ck(fc::store_checked("../escape",h)==fc::StoreStatus::Invalid&&fc::path_of("../escape").empty(),"reject_path_escape");
 auto bad=h;bad[0]=std::numeric_limits<double>::quiet_NaN();ck(fc::store_checked("nan",bad)==fc::StoreStatus::Invalid,"reject_nonfinite_write");
 fc::CacheLimits small{entryBytes*2,2};
 ck(fc::store_checked("a",h,small)==fc::StoreStatus::Stored,"write_a");Sleep(2);
 ck(fc::store_checked("b",h,small)==fc::StoreStatus::Stored,"write_b");Sleep(2);
 ck(fc::load("a",out)&&out==h,"bit_exact_read_touch");Sleep(2);
 ck(fc::store_checked("c",h,small)==fc::StoreStatus::Stored&&!fc::load("b",out)&&fc::load("a",out),"LRU_keeps_recent_a_evicts_b");
 ck(fc::store_checked("too_big",h,{entryBytes-1,32})==fc::StoreStatus::SkippedSize,"oversize_skips_before_io");
 {fc::DesignLock a("a"),c("c");ck(fc::can_store(h.size()*8,small)==fc::ProbeStatus::Full,"prewarm_probe_all_pinned_skips");ck(fc::store_checked("pinned_new",h,small)==fc::StoreStatus::Full,"all_pinned_skip");}
 ck(fc::store_checked("a",h,{entryBytes,1})==fc::StoreStatus::Stored,"same_key_replacement_never_exceeds_budget");
#ifdef RTA_FILTER_CACHE_TESTING
 fc::testing::remainingWriteBytes=20;ck(fc::store_checked("diskfull",h)==fc::StoreStatus::IoError,"disk_full_injected_at_payload");fc::testing::remainingWriteBytes=UINT64_MAX;
 fc::testing::failCommit=true;ck(fc::store_checked("a",bad)==fc::StoreStatus::Invalid,"invalid_never_commits");
 auto changed=h;changed[0]=.25;ck(fc::store_checked("a",changed)==fc::StoreStatus::IoError,"commit_failure_reported");fc::testing::failCommit=false;
 ck(fc::load("a",out)&&out==h,"commit_failure_preserves_complete_old_payload");
#endif
 {std::vector<fc::Entry> e;uint64_t bytes=0;fc::ProcessLock lock("directory");ck(fc::inventory(e,bytes,false)&&e.size()==1&&bytes==entryBytes,"faults_leave_no_temporary_bytes");}
 ck(join(launch("crash "+rta::platform::to_utf8(path)))==77,"child_exits_mid_transaction");
 ck(fc::store_checked("recovered",h)==fc::StoreStatus::Stored&&!std::filesystem::exists(dir/"fc_crash.bin.tmp.123"),"abandoned_lock_and_temp_recovered");
 ck(!fc::load("crash",out),"partial_file_never_hit");
 // Windows read-only destination is a real permission failure, without changing ACLs.
 const auto readonlyPath=rta::platform::from_utf8(fc::path_of("a"));
 fc::store("a",h);SetFileAttributesW(readonlyPath.c_str(),FILE_ATTRIBUTE_READONLY);
 ck(fc::store_checked("a",h)==fc::StoreStatus::IoError,"real_readonly_destination_is_safe_io_failure");
 SetFileAttributesW(readonlyPath.c_str(),FILE_ATTRIBUTE_NORMAL);
 ck(fc::load("a",out)&&out==h,"readonly_failure_preserves_old_complete_payload");
 // Never follow a file reparse point out of the cache directory.
 auto outside=path.parent_path()/(path.filename().string()+"-outside");std::filesystem::create_directories(outside);
 std::ofstream(outside/"do-not-modify")<<"user data";
 const auto link=dir/"fc_link.bin";
 const bool madeLink=CreateSymbolicLinkW(link.c_str(),(outside/"do-not-modify").c_str(),SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)!=FALSE;
 ck(madeLink,"create_reparse_negative_control");
 if(madeLink){ck(fc::store_checked("link",h)==fc::StoreStatus::IoError&&!fc::load("link",out),"reparse_file_refused");std::filesystem::remove(link);}
 const auto directoryLink=path.parent_path()/(path.filename().string()+"-junction");
 const bool madeDirLink=CreateSymbolicLinkW(directoryLink.c_str(),outside.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)!=FALSE;
 ck(madeDirLink,"create_directory_reparse_negative_control");
 if(madeDirLink){ck(join(launch("unsafe "+rta::platform::to_utf8(directoryLink)))==0,"reparse_directory_refused_before_access");std::filesystem::remove(directoryLink);}
 std::ifstream untouched(outside/"do-not-modify");std::string original;std::getline(untouched,original);ck(original=="user data","external_user_data_unchanged");
 auto lockChild=launch("lock "+rta::platform::to_utf8(path));for(int n=0;n<200&&!std::filesystem::exists(dir/"lock.ready");++n)Sleep(5);
 const auto lockStart=std::chrono::steady_clock::now();ck(fc::load_checked("a",out)==fc::LoadStatus::Busy,"held_directory_lock_is_explicit_busy");
 ck(std::chrono::steady_clock::now()-lockStart<std::chrono::milliseconds(500),"directory_wait_is_bounded");
 std::ofstream(dir/"lock.done")<<"1";ck(join(lockChild)==0,"directory_lock_child_releases_without_kill");
 // A non-cache user file must survive reclamation.
 std::ofstream(dir/"user_notes.tmp")<<"keep";fc::store("keep_user",h);ck(std::filesystem::exists(dir/"user_notes.tmp"),"only_owned_cache_files_are_reclaimed");
 // A real second process holds the directory through the worker's store AND
 // audit deadlines. It releases only after observing audit_busy, not a timing guess.
 clear_owned();auto delayed=launch("audit-lock "+rta::platform::to_utf8(path));
 for(int n=0;n<200&&!std::filesystem::exists(dir/"audit-lock.ready");++n)Sleep(5);
 auto contended=launch("bytes "+rta::platform::to_utf8(path)+" 0");const auto contendedExit=join(contended);const auto delayedExit=join(delayed);const auto forced=report(contended.dwProcessId);
 ck(contendedExit==0&&delayedExit==0&&forced.complete&&forced.busy>=1&&forced.auditBusy>=1&&forced.stored>=1,"bounded_audit_busy_is_not_corruption_and_writer_progresses");
 {fc::ProcessLock lock("directory",1000);std::vector<fc::Entry> e;uint64_t bytes=0;bool valid=bool(lock)&&fc::inventory(e,bytes,false)&&e.size()<=32&&bytes<=entryBytes*3;for(auto& x:e)valid=fc::load(x.key,out)&&payload_matches(x.key,out,1)&&valid;ck(valid&&!e.empty(),"forced_contention_final_quota_and_exact_payloads");}
 auto incorrect=h;incorrect[0]=0;ck(payload_matches("worker_0_0",incorrect,1),"payload_oracle_accepts_expected_worker");incorrect.back()=.25;ck(!payload_matches("worker_0_0",incorrect,1),"payload_oracle_rejects_wrong_tail");incorrect=h;incorrect[0]=1;ck(!payload_matches("worker_0_0",incorrect,2),"payload_oracle_rejects_wrong_worker");
#ifdef RTA_FILTER_CACHE_TESTING
 auto ioFault=launch("io-fault "+rta::platform::to_utf8(path)+" 0");ck(join(ioFault)==6&&!report(ioFault.dwProcessId).complete,"worker_io_error_is_not_accepted_as_busy");
#endif
 for(int n:{2,4,8})for(int mode:{0,1,2}){
  const bool same=mode==1,byteBound=mode==2;const fc::CacheLimits limits{byteBound?entryBytes*3:fc::kMaxBytes,32};
  clear_owned();for(int k=0;k<(byteBound?3:32);++k)fc::store_checked("old_"+std::to_string(k),h,limits);
  std::vector<PROCESS_INFORMATION> children;for(int k=0;k<n;++k)children.push_back(launch(std::string(byteBound?"bytes ":same?"same ":"worker ")+rta::platform::to_utf8(path)+" "+std::to_string(k)));
  bool ok=true;unsigned stored=0,busy=0,auditBusy=0;for(auto p:children){const auto code=join(p);const auto r=report(p.dwProcessId);stored+=r.stored;busy+=r.busy;auditBusy+=r.auditBusy;std::printf("child processes=%d mode=%d pid=%lu exit=%lu stored=%u busy=%u audit_busy=%u report=%d\n",n,mode,p.dwProcessId,code,r.stored,r.busy,r.auditBusy,r.complete);ok=code==0&&r.complete&&r.stored+r.busy==4&&ok;}
  std::vector<fc::Entry> e;uint64_t bytes=0;{fc::ProcessLock lock("directory",1000);ok=bool(lock)&&fc::inventory(e,bytes,false)&&e.size()<=32&&bytes<=limits.maxBytes&&ok;}
  for(auto& x:e)ok=fc::load(x.key,out)&&payload_matches(x.key,out,n)&&ok;
  std::printf("processes=%d mode=%d entries=%zu bytes=%llu stored=%u busy=%u audit_busy=%u\n",n,mode,e.size(),(unsigned long long)bytes,stored,busy,auditBusy);ck(ok&&stored>0,"multiprocess_commit_invariants_and_complete_payloads");
 }
 clear_owned();std::vector<PROCESS_INFORMATION> children;for(int n=0;n<8;++n)children.push_back(launch("flight "+rta::platform::to_utf8(path)));
 bool ok=true;for(auto p:children)ok=join(p)==0&&ok;std::ifstream count(dir/"design.count");std::string line;int designs=0;while(std::getline(count,line))++designs;ck(ok&&designs==1,"8_process_same_key_single_flight");
 clear_owned();fc::store("pinned",h);auto pin=launch("pin "+rta::platform::to_utf8(path));for(int n=0;n<200&&!std::filesystem::exists(dir/"pin.ready");++n)Sleep(5);
 ck(fc::store_checked("blocked",h,{entryBytes,1})==fc::StoreStatus::Full,"cross_process_pin_prevents_eviction");
 std::atomic_bool cancel=false;std::thread cancelling([&]{Sleep(20);cancel=true;});const auto t=std::chrono::steady_clock::now();{fc::DesignLock wait("pinned",30000,&cancel);ck(!wait,"design_wait_can_cancel");}cancelling.join();
 ck(std::chrono::steady_clock::now()-t<std::chrono::milliseconds(250),"cancel_response_under_250ms");std::ofstream(dir/"pin.done")<<"1";ck(join(pin)==0,"pinned_child_finished_without_kill");
 std::printf("cache_transactions checks=%d failures=%d dir=%s\n",checks,failures,rta::platform::to_utf8(path).c_str());return failures?1:0;
}
