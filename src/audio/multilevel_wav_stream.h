// Strict bounded-memory local RIFF reader for offline multilevel export.
#pragma once
#include "wav.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
namespace rta::audio {
class MultilevelWavStream {
 std::ifstream file_; uint64_t offset_=0,dataBytes_=0,position_=0;uint16_t align_=0;
 void read(void* p,size_t n){if(!file_.read(static_cast<char*>(p),std::streamsize(n)))throw std::runtime_error("truncated WAV read");}
 void seek(uint64_t n){file_.seekg(std::streamoff(n));if(!file_)throw std::runtime_error("WAV seek failed");}
public:
 uint32_t rate=0;int channels=0,bits=0;bool isFloat=false;
 explicit MultilevelWavStream(const std::filesystem::path& path):file_(path,std::ios::binary){
  if(!file_)throw std::runtime_error("cannot open source WAV");
  const uint64_t length=std::filesystem::file_size(path);uint8_t h[12];read(h,12);
  if(memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4))throw std::invalid_argument("only RIFF/WAVE supported; RF64/RIFX unsupported");
  const uint64_t end=uint64_t(detail::rd32(h+4))+8;
  if(end!=length||end<12)throw std::invalid_argument("WAV RIFF length/EOF mismatch");
  bool fmt=false,data=false;uint64_t at=12;
  while(at<end){
   if(end-at<8)throw std::invalid_argument("incomplete WAV chunk header");seek(at);uint8_t c[8];read(c,8);uint64_t size=detail::rd32(c+4),body=at+8;
   if(size>end-body||(size&1)>end-body-size)throw std::invalid_argument("WAV chunk exceeds RIFF boundary");
   if(!memcmp(c,"fmt ",4)){
    if(fmt||size<16||size>65536)throw std::invalid_argument("duplicate/invalid WAV fmt");fmt=true;std::vector<uint8_t>b(size);read(b.data(),b.size());
    uint16_t tag=detail::rd16(b.data());channels=detail::rd16(b.data()+2);rate=detail::rd32(b.data()+4);align_=detail::rd16(b.data()+12);bits=detail::rd16(b.data()+14);
    if(tag==0xfffe){
     static constexpr uint8_t tail[12]={0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
     if(size<40||detail::rd16(b.data()+16)<22||uint64_t(detail::rd16(b.data()+16))+18>size||detail::rd16(b.data()+18)!=bits||memcmp(b.data()+28,tail,12))throw std::invalid_argument("unsupported extensible WAV format");
     uint32_t sub=detail::rd32(b.data()+24);if(sub!=1&&sub!=3)throw std::invalid_argument("unsupported WAV subtype GUID");tag=uint16_t(sub);
    }
    if((tag!=1&&tag!=3)||channels<1||channels>32||rate<8000||rate>768000||(bits!=8&&bits!=16&&bits!=24&&bits!=32)||(tag==3&&bits!=32))throw std::invalid_argument("unsupported WAV encoding/rate/channels");
    isFloat=tag==3;const uint32_t expected=uint32_t(channels)*(bits/8);
    if(align_!=expected||uint64_t(rate)*align_!=detail::rd32(b.data()+8))throw std::invalid_argument("WAV byte rate/block alignment mismatch");
   }else if(!memcmp(c,"data",4)){
    if(data)throw std::invalid_argument("multiple WAV data chunks unsupported");data=true;offset_=body;dataBytes_=size;
   }
   at=body+size+(size&1);
  }
  if(!fmt||!data||!dataBytes_||dataBytes_%align_)throw std::invalid_argument("missing/empty/partial-frame WAV data");seek(offset_);
 }
 uint64_t frames()const{return dataBytes_/align_;}
 uint64_t remaining()const{return frames()-position_;}
 AudioBuffer read_frames(size_t requested){
  if(!requested||requested>65536)throw std::invalid_argument("WAV read block must be 1..65536 frames");
  const size_t n=size_t(std::min<uint64_t>(requested,remaining()));AudioBuffer out;out.rate=rate;out.channels=channels;out.bits=bits;out.isFloat=isFloat;
  std::vector<uint8_t> bytes(n*align_);if(!bytes.empty())read(bytes.data(),bytes.size());out.raw.resize(n*channels);
  for(size_t i=0;i<out.raw.size();++i){out.raw[i]=decode_sample(bytes.data()+i*(bits/8),bits,isFloat);if(isFloat&&!audio_finite(out.sample(i/channels,int(i%channels))))throw std::invalid_argument("WAV contains nonfinite samples");}
  position_+=n;return out;
 }
};
}
