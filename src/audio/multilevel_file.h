#pragma once
// RLPlayer offline multilevel sample container v1. Not DSD/DoP/DAC/HQP format.
// All integers are little endian; payload is interleaved unsigned 6/8-bit index
// stored in one byte, with y=2*index/(levels-1)-1. Complete footer is mandatory.
#ifndef _WIN32
#error RLPlayer multilevel atomic writer currently targets Windows only
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <vector>
#include <string>
#include <array>
#include <atomic>
#include <stdexcept>
#include <cstdint>
#include <cmath>
#include <limits>
#include <algorithm>
namespace rta::audio::multilevel {
namespace fs=std::filesystem;
inline constexpr size_t header_bytes=128,footer_bytes=32;
inline constexpr uint64_t fnv_offset=14695981039346656037ull,fnv_prime=1099511628211ull;
struct Format {
 uint32_t rate=0,source_rate=0,channels=0;
 uint16_t bits=8,requested_order=7,effective_order=7;
 uint16_t requested_levels=256,effective_levels=256;
 uint64_t source_frames=0;
 std::string metadata="{}";
};
struct Cancelled:std::runtime_error {Cancelled():std::runtime_error("multilevel export cancelled before publication") {}};
struct Ledger {uint64_t source_valid_output_frames=0,pipeline_tail_frames=0;};
inline void put16(unsigned char* p,uint16_t v){p[0]=static_cast<unsigned char>(v);p[1]=static_cast<unsigned char>(v>>8);}
inline void put32(unsigned char* p,uint32_t v){for(unsigned k=0;k<4;++k)p[k]=static_cast<unsigned char>(v>>(8*k));}
inline void put64(unsigned char* p,uint64_t v){for(unsigned k=0;k<8;++k)p[k]=static_cast<unsigned char>(v>>(8*k));}
inline uint64_t checksum(uint64_t state,const unsigned char* p,size_t n){for(size_t i=0;i<n;++i)state=(state^p[i])*fnv_prime;return state;}
inline std::array<unsigned char,128> make_header(const Format& f,const Ledger& ledger,uint64_t frames,bool sealed){
 std::array<unsigned char,128> out{};const unsigned char magic[8]={'R','L','M','U','L','T','I',0};std::copy_n(magic,8,out.data());
 put16(out.data()+8,1);put16(out.data()+10,128);put32(out.data()+12,0x01020304);
 put64(out.data()+16,f.rate);put32(out.data()+24,f.channels);put16(out.data()+28,f.bits);
 put16(out.data()+30,f.requested_order);put16(out.data()+32,f.effective_order);
 put16(out.data()+34,f.requested_levels);put16(out.data()+36,f.effective_levels);put16(out.data()+38,sealed?1:0);
 put32(out.data()+40,f.source_rate);put32(out.data()+44,uint32_t(f.metadata.size()));put64(out.data()+48,f.source_frames);
 put64(out.data()+56,ledger.source_valid_output_frames);put64(out.data()+64,ledger.pipeline_tail_frames);
 put64(out.data()+72,frames);put64(out.data()+80,frames*f.channels);return out;
}
class Writer {
 Format format_;fs::path target_,temporary_;HANDLE handle_=INVALID_HANDLE_VALUE;uint64_t frames_=0;bool committed_=false;
 static void fail(const char* what){throw std::runtime_error(std::string(what)+" (Win32="+std::to_string(GetLastError())+")");}
 void write(const void* bytes,size_t count){const auto* p=static_cast<const unsigned char*>(bytes);while(count){DWORD done=0;const DWORD take=DWORD(std::min<size_t>(count,1u<<20));if(!WriteFile(handle_,p,take,&done,nullptr)||done!=take)fail("multilevel write failed");p+=done;count-=done;}}
 void seek(uint64_t offset){LARGE_INTEGER n;n.QuadPart=LONGLONG(offset);if(!SetFilePointerEx(handle_,n,nullptr,FILE_BEGIN))fail("multilevel seek failed");}
 void live()const{if(handle_==INVALID_HANDLE_VALUE||committed_)throw std::logic_error("multilevel writer is closed");}
public:
 Writer(const fs::path& output,Format format):format_(std::move(format)),target_(fs::absolute(output)){
  if(target_.extension()!=L".rlmultilevel")throw std::invalid_argument("output must use .rlmultilevel, never a DAC/DSD extension");
  if(!format_.rate||!format_.source_rate||!format_.source_frames||!format_.channels||format_.channels>32)throw std::invalid_argument("invalid rate/channel/source ledger");
  if((format_.bits!=6&&format_.bits!=8)||format_.requested_levels!=(1u<<format_.bits)||format_.effective_levels!=format_.requested_levels)throw std::invalid_argument("container requires genuine 64 or 256 levels, never a one-bit downgrade");
  if(!format_.requested_order||format_.requested_order>8||!format_.effective_order||format_.effective_order>8||format_.effective_order>format_.requested_order)throw std::invalid_argument("invalid requested/effective order");
  if(format_.metadata.empty()||format_.metadata.size()>65536)throw std::invalid_argument("metadata must be a bounded UTF-8 JSON object");
  static std::atomic<uint64_t> serial{0};
  for(unsigned attempt=0;attempt<32;++attempt){temporary_=target_;temporary_+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++serial);
   handle_=CreateFileW(temporary_.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
   if(handle_!=INVALID_HANDLE_VALUE)break;if(GetLastError()!=ERROR_FILE_EXISTS)fail("cannot create private multilevel temporary");}
  if(handle_==INVALID_HANDLE_VALUE)fail("cannot allocate unique multilevel temporary");
  try{auto header=make_header(format_,{},0,false);write(header.data(),header.size());write(format_.metadata.data(),format_.metadata.size());}catch(...){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;DeleteFileW(temporary_.c_str());throw;}
 }
 Writer(const Writer&)=delete;Writer& operator=(const Writer&)=delete;
 ~Writer(){if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);if(!committed_&&!temporary_.empty())DeleteFileW(temporary_.c_str());}
 uint64_t frames()const{return frames_;}
 const fs::path& temporary_path_for_test()const{return temporary_;}
 void append_codes(const unsigned char* codes,size_t count){
  live();if(count%format_.channels)throw std::invalid_argument("interleaved payload must contain complete frames");if(count&&!codes)throw std::invalid_argument("null multilevel payload");
  const uint64_t added=count/format_.channels;if(added>std::numeric_limits<uint64_t>::max()/format_.channels-frames_)throw std::overflow_error("multilevel frame ledger overflow");
  for(size_t i=0;i<count;++i)if(codes[i]>=format_.effective_levels)throw std::invalid_argument("multilevel index outside declared levels");
  write(codes,count);frames_+=added;
 }
 void append_quantized(const std::vector<std::vector<double>>& planes){
  live();if(planes.size()!=format_.channels)throw std::invalid_argument("multilevel channel mismatch");const size_t n=planes[0].size();
  if(n>std::numeric_limits<size_t>::max()/format_.channels)throw std::overflow_error("multilevel block size overflow");std::vector<unsigned char> codes(n*format_.channels);
  for(size_t c=0;c<planes.size();++c){if(planes[c].size()!=n)throw std::invalid_argument("multilevel plane length mismatch");for(size_t i=0;i<n;++i){
   const double y=planes[c][i];if(!std::isfinite(y)||y< -1.||y>1.)throw std::invalid_argument("nonfinite/out-of-range quantizer sample");const double last=format_.effective_levels-1.;const long index=std::lround((y+1.)*(.5*last));
   if(index<0||index>=format_.effective_levels||std::abs(y-(2.*index/last-1.))>1e-12)throw std::invalid_argument("sample is not on the declared multilevel grid");codes[i*format_.channels+c]=static_cast<unsigned char>(index);
  }}append_codes(codes.data(),codes.size());
 }
 void finalize(const Ledger& ledger,const std::atomic_bool* cancel=nullptr){
  auto check_cancel=[&]{if(cancel&&cancel->load())throw Cancelled{};};
  live();check_cancel();if(!ledger.source_valid_output_frames||ledger.source_valid_output_frames>std::numeric_limits<uint64_t>::max()-ledger.pipeline_tail_frames||ledger.source_valid_output_frames+ledger.pipeline_tail_frames!=frames_)throw std::invalid_argument("source+tail ledger does not equal actual payload frames");
  if(frames_>(std::numeric_limits<uint64_t>::max()-128-format_.metadata.size())/format_.channels)throw std::overflow_error("multilevel total size overflow");
  const auto header=make_header(format_,ledger,frames_,true);seek(0);write(header.data(),header.size());if(!FlushFileBuffers(handle_))fail("multilevel flush failed");
  const uint64_t prefix=128+format_.metadata.size()+frames_*format_.channels;uint64_t remaining=prefix,hash=fnv_offset;seek(0);std::array<unsigned char,65536> block{};
  while(remaining){check_cancel();const DWORD take=DWORD(std::min<uint64_t>(remaining,block.size()));DWORD got=0;if(!ReadFile(handle_,block.data(),take,&got,nullptr)||got!=take)fail("multilevel checksum read failed");hash=checksum(hash,block.data(),got);remaining-=got;}
  std::array<unsigned char,32> end{};const unsigned char magic[8]={'R','L','E','N','D','v','1',0};std::copy_n(magic,8,end.data());put64(end.data()+8,frames_);put64(end.data()+16,frames_*format_.channels);put64(end.data()+24,hash);write(end.data(),end.size());
  if(!FlushFileBuffers(handle_))fail("multilevel completion flush failed");if(!CloseHandle(handle_)){handle_=INVALID_HANDLE_VALUE;fail("multilevel close failed");}handle_=INVALID_HANDLE_VALUE;
  check_cancel();
  if(!MoveFileExW(temporary_.c_str(),target_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))fail("multilevel atomic replacement failed");committed_=true;
 }
};
}
