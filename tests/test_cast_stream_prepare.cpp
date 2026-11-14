#include "net/cast_service.h"
#include "net/http_client.h"
#include <cstdio>
#include <limits>
#include <fstream>
using namespace rta::net;
int main(){int checks=0,failures=0;auto check=[&](bool ok,const char*label){++checks;failures+=!ok;printf("%s %s\n",ok?"PASS":"FAIL",label);};
 std::atomic<bool> cancel{false};CastStartParams p;p.file="cast-input.wav";p.bits=24;p.rate=176400;
 auto fixture=[&](uint32_t rate){rta::audio::AudioBuffer a;a.rate=rate;a.bits=24;a.channels=2;a.raw.assign(8192*2,0);for(int c=0;c<2;++c){a.setSample(0,c,.5);a.setSample(8191,c,.5);}rta::audio::write_wav(p.file,a);return a;};
 auto a=fixture(44100);auto r=cast_prepare_pcm(p,"cast-output.wav",cancel,{},{});auto out=rta::audio::read_wav("cast-output.wav");
 double head=0,tail=0;for(size_t i=0;i<100;i++)head=std::max(head,std::abs(out.sample(i,0)));for(size_t i=32700;i<32800;i++)tail=std::max(tail,std::abs(out.sample(i,0)));
 printf("rate=%u frames=%zu head=%g tail=%g\n",out.rate,out.frames(),head,tail);check(out.rate==176400 && head>.1&&tail>.1,"44.1 to 176.4 preserves source head and last impulse after delay compensation/drain");
 a=fixture(48000);p.rate=192000;r=cast_prepare_pcm(p,"cast-output.wav",cancel,{},{});out=rta::audio::read_wav("cast-output.wav");check(out.rate==192000&&out.frames()>32768,"48 to 192 uses same-family fourfold rate with complete natural tail");
 p.rate=0;p.upsample=false;r=cast_prepare_pcm(p,"cast-output.wav",cancel,{},{});out=rta::audio::read_wav("cast-output.wav");check(out.raw==a.raw&&out.rate==a.rate,"upsampling off preserves every source PCM sample");
 // Full-scale transient and a subsequent quiet tone exercise overload recovery.
 for(size_t i=0;i<a.frames();++i)for(int c=0;c<2;++c)a.setSample(i,c,i<1000?((i%2)?1.0:-1.0):.001*std::sin(2*3.141592653589793*1000*i/48000));
 rta::audio::write_wav(p.file,a);p.rate=192000;cast_prepare_pcm(p,"cast-output.wav",cancel,{},{});out=rta::audio::read_wav("cast-output.wav");double rms=0;for(size_t i=16000;i<28000;i++)rms+=out.sample(i,0)*out.sample(i,0);rms=std::sqrt(rms/12000);printf("recovery_rms=%g\n",rms);check(rms>.0005&&rms<.0009,"full-scale transient recovers to quiet 1kHz tone instead of locking at full-scale");
 // Output is final before registration; HTTP bytes and Range are independently fetched.
 UpnpHttpServer server;bool bound=false;for(int port=19840;port<19860&&!bound;++port)bound=server.start((uint16_t)port,"127.0.0.1");check(bound,"loopback source binds");
 if(bound){auto token=server.register_file("cast-output.wav","audio/wav");auto url="http://127.0.0.1:"+std::to_string(server.port())+token;auto response=http_get(url,{},5000);std::ifstream f("cast-output.wav",std::ios::binary);std::string expected((std::istreambuf_iterator<char>(f)),{});check(response.status==200&&std::string(response.body.begin(),response.body.end())==expected,"real HTTP GET returns exact completed WAV bytes");auto rr=http_get(url,{"Range: bytes=100-199"},5000);check(rr.status==206&&std::string(rr.body.begin(),rr.body.end())==expected.substr(100,100),"real HTTP Range returns stable requested bytes");server.stop();}
 auto caps=parse_sink_caps("http-get:*:audio/wav;rate=192000;channels=6:*,http-get:*:audio/wav;rate=96000;channels=2:*");auto rates=cast_compatible_rates(caps,24,2);check(rates.size()==1&&rates[0]==96000,"complete format tuple prevents stereo using rate declared only for 6 channels");
 check(rta::audio::decide_cast_target_rate(44100,"192000",{96000,192000},192000,true).targetRate==44100,"cross-family request does not silently resample");check(rta::audio::decide_cast_target_rate(44100,"max",{88200,176400,352800},192000,true).targetRate==176400,"ceiling selects highest legal same-family tuple");auto force=rta::audio::decide_cast_target_rate(44100,"176400",{44100},192000,true,true);check(force.targetRate==176400&&force.forced,"explicit force bypasses declarations and remains labelled");check(!rta::audio::decide_cast_target_rate(44100,"192000",{},192000,true,true).upsample,"force cannot bypass same-family rule");
 auto rejection=[&](const char*label){bool rejected=false;try{cast_prepare_pcm(p,"cast-failed.wav",cancel,{},{});}catch(const std::exception& e){rejected=true;printf("reason=%s\n",e.what());}check(rejected&&!std::filesystem::exists("cast-failed.wav"),label);};
 p.maxOutputBytes=1024;rejection("RIFF/output budget failure removes partial temporary WAV");p.maxOutputBytes=0xFFFFFFFFull-36;p.diskReserveBytes=std::numeric_limits<uint64_t>::max();rejection("insufficient free-space reserve fails explicitly and removes partial WAV");p.diskReserveBytes=1048576;
 bool cancelled=false;try{cast_prepare_pcm(p,"cast-failed.wav",cancel,{},[&](double,const std::string&){cancel.store(true);});}catch(const std::exception&){cancelled=true;}check(cancelled&&!std::filesystem::exists("cast-failed.wav"),"cancellation during preparation removes incomplete WAV");cancel.store(false);
 auto tuple=cast_parse_sink_caps("http-get:*:audio/wav;rate=192000;channels=2;bits=16:*,http-get:*:audio/wav;rate=96000;channels=2;bits=24:*");auto exact=cast_compatible_rates(tuple,24,2);check(exact.size()==1&&exact[0]==96000,"rate and bit depth remain from the same declared tuple");

 // A real lossless decoder process feeds bounded PCM blocks, without a full-track buffer.
 const auto ff=rta::audio::find_tool("ffmpeg");
 if(!ff.empty()) {
  p.file="cast-input.wav";p.bits=24;p.rate=0;p.cleanup=false;
  const auto cmd=L"\""+rta::audio::widen(ff)+L"\" -nostdin -y -v error -i cast-input.wav cast-input.flac";
  check(rta::audio::run_hidden(cmd)==0,"generate lossless compressed fixture with actual decoder");
  p.file="cast-input.flac";auto decoded=cast_prepare_pcm(p,"cast-output.wav",cancel,{},{});auto pcm=rta::audio::read_wav("cast-output.wav");
  check(pcm.raw==a.raw&&pcm.rate==48000,"compressed FLAC is streamed and preserves every decoded PCM sample with upsampling off");
  p.rate=192000;auto up=cast_prepare_pcm(p,"cast-output.wav",cancel,{},{});check(up.rate==192000,"compressed source streams through production Pipeline for 48 to 192k");
 }
 rta::audio::AudioBuffer floating;floating.rate=48000;floating.bits=32;floating.isFloat=true;floating.channels=2;floating.raw.resize(128*2);for(size_t n=0;n<128;++n)for(int ch=0;ch<2;++ch)floating.setSample(n,ch,n==0?1.25:1.234567e-7);
 rta::audio::write_wav("cast-float.wav",floating);p.file="cast-float.wav";p.rate=0;p.bits=0;cast_prepare_pcm(p,"cast-output.wav",cancel,{},{});auto untouched=rta::audio::read_wav("cast-output.wav");check(untouched.isFloat&&untouched.raw==floating.raw,"upsampling off preserves float samples including above-full-scale values");
 p.file="cast-input.wav";p.rate=0;p.bits=24;p.cleanup=true;p.cleanupNoiseDb=-95;
 auto clean=cast_prepare_pcm(p,"cast-output.wav",cancel,{},{});auto cleaned=rta::audio::read_wav("cast-output.wav");
 check(clean.cleanupApplied&&clean.changed&&cleaned.frames()>a.frames(),"cast cleanup is actually applied and its complete tail is written");p.cleanup=false;
 auto& mgr=CastManager::instance();p.cueStart=0;check(!mgr.start(p).ok,"unified cast entry rejects CUE including zero-second start");p.cueStart=-1;p.cueEnd=2;check(!mgr.start(p).ok,"unified cast entry rejects end-only CUE");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
