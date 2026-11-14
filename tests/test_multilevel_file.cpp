#include "audio/multilevel_file.h"
#include <fstream>
#include <iterator>
#include <iostream>
using namespace rta::audio::multilevel;
static int checks=0,failures=0;
void ck(bool v,const char* what){++checks;if(!v){++failures;std::cout<<"FAIL "<<what<<'\n';}}
std::string bytes(const fs::path&p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){try{
 if(argc!=2)return 2;fs::path dir=fs::u8path(argv[1]);fs::create_directories(dir);
 for(int bits:{6,8}){
  Format f;f.rate=f.source_rate=2822400;f.channels=2;f.bits=uint16_t(bits);f.requested_levels=f.effective_levels=uint16_t(1u<<bits);f.source_frames=(1u<<bits)/2;f.metadata="{\"kind\":\"all-codeword-golden\"}";
  const auto out=dir/("golden-"+std::to_string(bits)+".rlmultilevel");std::vector<unsigned char> codes(1u<<bits);for(size_t i=0;i<codes.size();++i)codes[i]=static_cast<unsigned char>(i);
  Writer writer(out,f);writer.append_codes(codes.data(),codes.size());writer.finalize({f.source_frames,0});ck(fs::exists(out),"golden writer committed complete file");
 }
 Format f;f.rate=f.source_rate=2822400;f.channels=1;f.bits=6;f.requested_levels=f.effective_levels=64;f.source_frames=1;
 const auto cancelled=dir/"cancelled.rlmultilevel";fs::path scratch;
 {Writer w(cancelled,f);scratch=w.temporary_path_for_test();unsigned char index=17;w.append_codes(&index,1);}
 ck(!fs::exists(cancelled)&&!fs::exists(scratch),"abandoned/cancelled writer leaves no false complete file");
 const auto invalid=dir/"invalid.rlmultilevel";bool rejected=false;
 try{Writer w(invalid,f);unsigned char index=64;w.append_codes(&index,1);}catch(const std::invalid_argument&){rejected=true;}
 ck(rejected&&!fs::exists(invalid),"invalid six-bit index rejected before commit");
 rejected=false;try{Writer w(invalid,f);w.append_quantized({{.2}});}catch(const std::invalid_argument&){rejected=true;}
 ck(rejected&&!fs::exists(invalid),"off-grid values cannot be sign-collapsed or silently requantized");
 rejected=false;try{Writer w(invalid,f);unsigned char index=0;w.append_codes(&index,1);w.finalize({2,0});}catch(const std::invalid_argument&){rejected=true;}
 ck(rejected&&!fs::exists(invalid),"false source/tail ledger cannot be sealed");
 rejected=false;try{Writer w(invalid,f);unsigned char code=17;w.append_codes(&code,1);std::atomic_bool cancel{true};w.finalize({1,0},&cancel);}catch(const Cancelled&){rejected=true;}
 ck(rejected&&!fs::exists(invalid),"finalization observes cancellation before publishing a complete-looking file");
 const auto heldPath=dir/"held.rlmultilevel";{std::ofstream o(heldPath,std::ios::binary);o<<"old bytes remain";}const auto old=bytes(heldPath);
 HANDLE held=CreateFileW(heldPath.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);ck(held!=INVALID_HANDLE_VALUE,"hold existing destination against delete/replacement");
 rejected=false;{try{Writer w(heldPath,f);scratch=w.temporary_path_for_test();unsigned char code=17;w.append_codes(&code,1);w.finalize({1,0});}catch(const std::runtime_error&){rejected=true;}}
 ck(rejected&&bytes(heldPath)==old&&!fs::exists(scratch),"failed atomic replacement preserves every old byte and cleans owned temporary");if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
 {Writer w(heldPath,f);unsigned char code=17;w.append_codes(&code,1);w.finalize({1,0});}
 ck(bytes(heldPath)!=old,"same writer succeeds after sharing denial is released");
 std::cout<<"checks="<<checks<<" failures="<<failures<<" device_calls=0\n";return failures?1:0;
 }catch(const std::exception&e){std::cerr<<"ERROR "<<e.what()<<'\n';return 2;}}
