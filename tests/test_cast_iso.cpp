// SPDX-License-Identifier: GPL-3.0-or-later
// Public cast entry/stream regressions; all supplied media are numeric fixtures.
#include "net/cast_service.h"
#include <cstdio>
#include <fstream>
#include <limits>
using namespace rta::net;
int main(int argc,char**argv) {
 SetConsoleOutputCP(CP_UTF8);setvbuf(stdout,nullptr,_IONBF,0);
 if(argc<3)return 2;
 int failures=0,checks=0;auto ck=[&](bool ok,const char*label){++checks;failures+=!ok;printf("%s %s\n",ok?"PASS":"FAIL",label);};
 const std::string root=argv[1],sacd=root+"/sacd-raw.iso#sacd=2ch:2",dvd=root+"/synthetic-dvda.iso#dvda=1:1:2";
 CastStartParams p;p.file=sacd;p.dev="http://127.0.0.1:1/desc";
 auto& manager=CastManager::instance();auto denied=manager.start(p);
 ck(!denied.ok&&denied.error.find("DoP")!=std::string::npos,"SACD virtual track enforces DSD DoP policy before device connection");
 p.dop=true;auto accepted=manager.start(p);ck(accepted.ok,"SACD virtual track physical image exists and cast accepts selection");manager.stop();
 std::atomic<bool> cancel{false};
 try {CastPcmReader reader;reader.open(dvd,cancel);rta::audio::AudioBuffer out;auto n=reader.read(out,5000);bool exact=n==480&&reader.rate==48000&&reader.bits==16&&out.channels==2;
  for(size_t i=0;i<n&&exact;++i)for(int c=0;c<2;++c)exact&=out.sample(i,c)==double(int(((480+i)*43+c*991)%20001)-10000)/32768.;
  ck(exact,"DVD virtual second track preserves true 16-bit format and every formula sample");
 }catch(const std::exception&e){printf("error=%s\n",e.what());ck(false,"DVD virtual second track is decoded by bounded cast PCM reader");}
 // Exercise packing with an independently decoded source stream. SACD/DST
 // decoding has its separate scalar/FFmpeg oracles; this seam verifies transport
 // never changes the already decoded DSD, including every bit and marker.
 uint64_t comparedBits=0;
 for(const char* image:{"sacd-raw.iso","sacd-dst.iso"})for(const char* area:{"2ch","mch"})for(int track=1;track<=2;++track) {
  p.file=root+"/"+image+"#sacd="+area+":"+std::to_string(track);p.dop=true;
  const auto source=rta::audio::read_dsd(p.file);const std::string output=root+"/cast-test-dop.wav";
  {std::ofstream reference(fs::u8path(root+"/"+image+"-"+area+"-"+std::to_string(track)+".dsdref"),std::ios::binary);for(const auto& channel:source.data)reference.write((const char*)channel.data(),channel.size());}
  try {auto prepared=cast_prepare_sacd_dop(p,output,cancel);auto wav=rta::audio::read_wav(output);bool exact=wav.rate==176400&&wav.bits==24&&wav.channels==source.channels&&wav.frames()*16==source.bits;
   for(size_t n=0;n<wav.frames();++n)for(int c=0;c<wav.channels;++c){uint32_t v=uint32_t(wav.raw[n*wav.channels+c]);exact&=((v>>16)&255)==(n%2?0xfa:5);exact&=((v>>8)&255)==source.data[c][n*2];exact&=(v&255)==source.data[c][n*2+1];}
   comparedBits+=source.bits*source.channels;printf("source=%s frames=%zu ch=%d\n",p.file.c_str(),wav.frames(),wav.channels);ck(exact,"raw/DST SACD DoP preserves every DSD bit, marker phase and exact first/last frame");
  }catch(const std::exception&e){printf("error=%s\n",e.what());ck(false,"SACD DoP packing succeeds");}
  fs::remove(fs::u8path(output));
 }
 printf("dsd_bits_compared=%llu\n",(unsigned long long)comparedBits);
 // Every DVD navigation profile is compared with the original numeric formula,
 // including second-track offsets. No production decoder supplies the oracle.
 uint64_t comparedPcm=0;
 for(const char* image:{"synthetic-dvda.iso","synthetic-dvda-udf102.iso","synthetic-dvda-udf150.iso"})for(int group=1;group<=3;++group)for(int track=1;track<=(group==3?1:2);++track) {
  p.file=root+"/"+image+"#dvda="+std::to_string(group)+":1:"+std::to_string(track);p.dop=false;
  try {CastPcmReader reader;reader.open(p.file,cancel,&p);rta::audio::AudioBuffer out;bool exact=reader.rate==(group==2?96000:48000)&&reader.bits==(group==2?24:16)&&!reader.floating;
   size_t pos=0,n=0;const size_t total=group==3?1200:group==2?320:480;
   while((n=reader.read(out,71))){for(size_t i=0;i<n;++i)for(int c=0;c<2;++c){const auto sample=int(((pos+i+(group==3?0:(track-1)*total))*43+c*991)%20001)-10000;exact&=out.sample(i,c)==double(sample)/32768.;++comparedPcm;}pos+=n;}
   ck(exact&&pos==total,"ISO9660/UDF LPCM/MLP cast reader preserves source precision, exact selected-track start/end and every numeric sample");
  }catch(const std::exception&e){printf("error=%s\n",e.what());ck(false,"DVD LPCM/MLP cast reader succeeds");}
 }
 printf("pcm_samples_compared=%llu\n",(unsigned long long)comparedPcm);
 // Nonzero low bits and signed endpoints distinguish lossless 20/24-bit
 // normalization from a misleading 16-bit tone widened with zero padding.
 for(int precision:{20,24}) {
  std::ifstream in(fs::u8path(root+"/synthetic-dvda.iso"),std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(in)),{});
  const size_t sector=32*2048;std::vector<uint8_t> header(b.begin()+sector,b.begin()+sector+40);std::fill(b.begin()+sector,b.begin()+sector+2048,0);std::copy(header.begin(),header.end(),b.begin()+sector);b[sector+35]=uint8_t(((precision-16)/4)<<4|15);
  auto value=[&](size_t n,int c){if(n==0)return c?(1<<(precision-1))-1:-(1<<(precision-1));if(n==319)return c?-(1<<(precision-1)):(1<<(precision-1))-1;return int((n*104729+c*17917)%(1u<<precision))-(1<<(precision-1));};
  size_t at=sector+40;
  for(size_t n=0;n<320;n+=2){int v[4]={value(n,0),value(n,1),value(n+1,0),value(n+1,1)};for(auto x:v){uint32_t high=uint32_t(x>>(precision-16));b[at++]=uint8_t(high>>8);b[at++]=uint8_t(high);}if(precision==24){for(auto x:v)b[at++]=uint8_t(x);}else{b[at++]=uint8_t(((v[0]&15)<<4)|(v[1]&15));b[at++]=uint8_t(((v[2]&15)<<4)|(v[3]&15));}}
  const size_t pes=at-(sector+20),padding=sector+2048-at-6;b[sector+18]=uint8_t(pes>>8);b[sector+19]=uint8_t(pes);b[at+2]=1;b[at+3]=0xbe;b[at+4]=uint8_t(padding>>8);b[at+5]=uint8_t(padding);
  const std::string path=root+"/cast-test-precision.iso",output=root+"/cast-test-precision.wav";{std::ofstream o(fs::u8path(path),std::ios::binary);o.write((const char*)b.data(),b.size());}
  p.file=path+"#dvda=2:1:1";p.dop=false;
  try{const auto prepared=cast_prepare_pcm(p,output,cancel,{},{});const auto out=rta::audio::read_wav(output);bool exact=prepared.bits==24&&!prepared.changed&&out.frames()==320;for(size_t n=0;n<out.frames();++n)for(int c=0;c<2;++c)exact&=out.raw[n*2+c]==value(n,c)*(1<<(24-precision));ck(exact,"20/24-bit signed endpoints and nonzero low bits survive DVD cast Pipeline without unwanted dither");}catch(const std::exception&e){printf("error=%s\n",e.what());ck(false,"DVD full-precision cast succeeds");}
  fs::remove(fs::u8path(path));fs::remove(fs::u8path(output));
 }
 // Same MLP PES/sector, IFO-selected second half: 600 initial samples must go.
 {
  std::ifstream in(fs::u8path(root+"/synthetic-dvda.iso"),std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(in)),{});
  auto be=[&](size_t at,uint32_t value){for(int i=3;i>=0;--i){b[at+i]=uint8_t(value);value>>=8;}};
  const size_t record=27*2048+2048+16+18;be(record+6,1125);be(record+10,1125);
  const std::string trimmed=root+"/cast-test-pts.iso";{std::ofstream o(fs::u8path(trimmed),std::ios::binary);o.write((const char*)b.data(),b.size());}
  p.file=trimmed+"#dvda=3:1:1";p.dop=false;
  try{CastPcmReader reader;reader.open(p.file,cancel,&p);rta::audio::AudioBuffer out;auto n=reader.read(out,2000);bool exact=n==600;for(size_t i=0;i<n;++i)for(int c=0;c<2;++c)exact&=out.raw[i*2+c]==int(((i+600)*43+c*991)%20001)-10000;ck(exact,"MLP same-sector IFO PTS trims exactly 600 leading samples and preserves 600 selected samples");}catch(const std::exception&e){printf("error=%s\n",e.what());ck(false,"MLP PTS trimming succeeds");}
  // No timestamp means an exact start cannot be established: explicit refusal.
  b[40*2048+14+7]&=~0x80;{std::ofstream o(fs::u8path(trimmed),std::ios::binary);o.write((const char*)b.data(),b.size());}
  bool rejected=false;try{CastPcmReader reader;reader.open(p.file,cancel,&p);}catch(const std::exception&e){rejected=std::string(e.what()).find("PTS")!=std::string::npos;}
  ck(rejected,"MLP first audio PES without PTS is refused instead of guessing the first sample");fs::remove(fs::u8path(trimmed));
 }
 // The RIFF and disk/cancellation guards apply equally to DSD transport.
 p.file=sacd;p.dop=true;const std::string failed=root+"/cast-test-partial.wav";
 auto reject=[&](const char*label){bool rejected=false;try{cast_prepare_sacd_dop(p,failed,cancel);}catch(const std::exception&e){printf("reason=%s\n",e.what());rejected=true;}ck(rejected&&!fs::exists(fs::u8path(failed)),label);};
 p.maxOutputBytes=1024;reject("DoP output budget rejects before a partial WAV is retained");p.maxOutputBytes=0xFFFFFFFFull-36;
 p.diskReserveBytes=std::numeric_limits<uint64_t>::max();reject("DoP disk reserve rejects and removes incomplete WAV");p.diskReserveBytes=1048576;
 bool cancelled=false;try{cast_prepare_sacd_dop(p,failed,cancel,[&](double progress,const std::string&){if(progress>0)cancel=true;});}catch(const std::exception&){cancelled=true;}
 ck(cancelled&&!fs::exists(fs::u8path(failed)),"cancelling SACD preparation after first block removes partial WAV");cancel=false;
 p.file=root+"/synthetic-dvda-udf150.iso#dvda=3:1:1";p.dop=false;cancelled=false;
 try{cast_prepare_pcm(p,failed,cancel,{},[&](double,const std::string&){cancel=true;});}catch(const std::exception&){cancelled=true;}
 ck(cancelled&&!fs::exists(fs::u8path(failed)),"cancelling UDF MLP preparation removes incomplete WAV and selected AOB");cancel=false;
 for(const char* suffix:{"#sacd=2ch:0","#sacd=2ch:999","#sacd=2ch:2147483648","#sacd=2ch:1#sacd=2ch:2","#dvda=1:1:1"}){p.file=root+"/sacd-raw.iso"+suffix;auto r=manager.start(p);ck(!r.ok,"malformed, missing or foreign image selection is rejected before renderer discovery");}
 p.file=root+"/synthetic-dvda.iso#sacd=2ch:1";ck(!manager.start(p).ok,"SACD selector on DVD-A is rejected without first-track fallback");
 p.file=sacd;p.cleanup=true;ck(!manager.start(p).ok,"SACD DoP rejects PCM cleanup");p.cleanup=false;p.cueStart=0;ck(!manager.start(p).ok,"ISO CUE slicing remains explicitly prohibited");p.cueStart=-1;
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
