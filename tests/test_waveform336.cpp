// Offline production decoder regression. No audio output endpoint is opened.
#include "analysis/waveform.h"
#include "dsp/modulator.h"
#include <iostream>
#include <numeric>

namespace fs=std::filesystem;
using rta::analysis::waveform_compute;
using rta::analysis::WaveformResult;
static int failures=0,checks=0;
static void check(bool pass,const char* name){++checks;std::cout<<(pass?"PASS ":"FAIL ")<<name<<'\n';if(!pass)++failures;}
static std::string utf(const fs::path& p){return rta::platform::to_utf8(p);}
static void write_fixture(const fs::path& path,bool silence=false,double offset=0){
    rta::audio::AudioBuffer b;b.rate=48000;b.channels=2;b.bits=16;b.raw.resize(48000*4*2);
    for(size_t f=0;f<b.frames();++f){double sample=silence?0:std::min(.99,offset+double(f/48000)*.25);b.setSample(f,0,sample);b.setSample(f,1,-sample);}
    rta::audio::write_wav(utf(path),b);
}
static bool all(const std::vector<double>& bins,double value){return !bins.empty()&&std::all_of(bins.begin(),bins.end(),[=](double b){return std::abs(b-value)<1e-7;});}
static bool equal(const WaveformResult& a,const WaveformResult& b){return a.error.empty()&&b.error.empty()&&a.duration==b.duration&&a.bins==b.bins;}
static std::string wait_ready(rta::analysis::waveform_detail::Service& service,const std::string& file,const std::string& cache,double start=0,double length=0){
    std::string response;auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    do{response=service.request(file,cache,start,length);if(response.find("\"state\":\"ready\"")!=std::string::npos||response.find("\"state\":\"error\"")!=std::string::npos)return response;std::this_thread::sleep_for(std::chrono::milliseconds(10));}while(std::chrono::steady_clock::now()<limit);return response;
}
int main(int argc,char** argv){
    auto run=argc>1?rta::platform::from_utf8(argv[1]):fs::temp_directory_path()/("rlplayer-waveform336-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(run);
    std::cout<<"EVIDENCE "<<utf(run)<<'\n';
    try{
        const auto file=run/fs::path(u8"峰值 & 反相.wav"),silent=run/"silence.wav",flac=run/fs::path(u8"峰值 & 反相.flac"),dsf=run/"real-stream.dsf",cache=run/"waveformcache";
        write_fixture(file);write_fixture(silent,true);
        auto wav=waveform_compute(utf(file));check(wav.error.empty()&&wav.duration==4&&wav.decodedFrames==192000,"WAV decodes all 192000 frames without playback");
        check(wav.bins.size()<=4096&&wav.bins.size()==3000,"whole-track peak memory is bounded to 4096 bins");
        bool segments=wav.bins.size()==3000;for(size_t i=0;i<wav.bins.size();++i)segments=segments&&std::abs(wav.bins[i]-double(i/750)*.25)<1e-7;
        check(segments,"four known energy segments occur at their actual time positions");
        check(!wav.bins.empty()&&*std::max_element(wav.bins.begin(),wav.bins.end())==.75,"opposite-polarity stereo retains its real peak instead of cancelling to mono");
        auto zero=waveform_compute(utf(silent));check(zero.error.empty()&&all(zero.bins,0),"digital silence has only exact zero bins");
        auto cue=waveform_compute(utf(file),1,2);bool cueEnergy=cue.bins.size()==3000;for(size_t i=0;i<cue.bins.size();++i)cueEnergy=cueEnergy&&std::abs(cue.bins[i]-(i<1500?.25:.5))<1e-7;
        check(cue.error.empty()&&cue.duration==2&&cue.decodedFrames==96000&&cueEnergy,"CUE range is cropped and rebased to passage-relative time");
        auto tail=waveform_compute(utf(file),3.5,10);check(tail.error.empty()&&tail.duration==.5&&all(tail.bins,.75),"a requested passage is clamped to actual EOF without fabricated data");
        check(!waveform_compute(utf(file),5,1).error.empty(),"a range beyond EOF is rejected explicitly");
        check(!waveform_compute(utf(file),-1,1).error.empty(),"negative passage starts are rejected");
        check(!waveform_compute(utf(file),0,std::numeric_limits<double>::quiet_NaN()).error.empty(),"non-finite passage duration is rejected");
        int cancelChecks=0;auto cancelled=waveform_compute(utf(file),0,0,[&]{return ++cancelChecks>4;});check(cancelled.cancelled&&cancelChecks<10&&cancelled.bins.empty(),"native decoding checks cancellation between bounded blocks");
        const auto ffmpeg=rta::audio::find_tool("ffmpeg");check(!ffmpeg.empty(),"actual bundled ffmpeg is available for compressed fixtures");
        const auto fixture=argc>2?rta::platform::from_utf8(argv[2]):fs::path(__FILE__).parent_path()/"fixtures"/"waveform336.flac";
        fs::copy_file(fixture,flac,fs::copy_options::overwrite_existing);
        check(fs::exists(flac)&&fs::file_size(flac)>0,"known anti-phase FLAC fixture is present for actual bundled decoding");
        auto compressed=waveform_compute(utf(flac));if(!compressed.error.empty())std::cout<<compressed.error<<'\n';check(equal(wav,compressed),"actual FLAC streaming decode has exactly the WAV peak envelope");
        auto compressedCue=waveform_compute(utf(flac),1,2);check(equal(cue,compressedCue),"compressed CUE extraction matches the native passage exactly");
        // Verify binary stdout and diagnostic stderr remain separate even while
        // cancelling an exact child handle, with no process-name/tree operation.
        size_t rawBytes=0,maxChunk=0;bool allZero=true;auto began=std::chrono::steady_clock::now();
        auto process=rta::platform::run_hidden_stream(rta::analysis::waveform_detail::quote(ffmpeg)+" -v info -nostdin -re -stream_loop -1 -i "+rta::analysis::waveform_detail::quote(utf(silent))+" -t 30 -c:a pcm_f32le -f f32le pipe:1",[&](const void* data,size_t n){rawBytes+=n;maxChunk=std::max(maxChunk,n);auto* b=static_cast<const unsigned char*>(data);for(size_t i=0;i<n;++i)allZero=allZero&&b[i]==0;return true;},[&]{return std::chrono::steady_clock::now()-began>std::chrono::milliseconds(350);});
        auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count();
        check(process.started&&process.cancelled&&milliseconds<2000,"stream cancellation terminates only its owned child promptly");
        check(rawBytes>0&&maxChunk<=65536&&allZero&&!process.output.empty()&&process.output.size()<=65536,"PCM bytes are bounded and never contaminated by stderr");
        std::cout<<"STREAM bytes="<<rawBytes<<" max_chunk="<<maxChunk<<" cancel_ms="<<milliseconds<<" stderr="<<process.output.size()<<'\n';
        rta::audio::DsdStream bits;bits.rate=2822400;bits.channels=2;bits.bits=141120;bits.data.assign(2,std::vector<uint8_t>((bits.bits+7)/8));
        for(int c=0;c<2;++c){rta::dsp::SdmConfig cfg;cfg.fs=bits.rate;cfg.order=5;cfg.oneBitOutput=true;cfg.ditherLsb=.0005;rta::dsp::SdmModulator mod(cfg);for(size_t f=0;f<bits.bits;++f)bits.setBit(c,f,mod.process(.001*std::sin(2*rta::dsp::kPi*(997+c*211)*f/bits.rate))>0);}
        rta::audio::write_dsf(utf(dsf),bits);auto dsd=waveform_compute(utf(dsf));auto reference=rta::audio::read_dsd_native(utf(dsf),nullptr);
        uint64_t bucket=1;while((reference.frames()+bucket-1)/bucket>4096)bucket*=2;
        std::vector<double> expected;for(size_t first=0;first<reference.frames();first+=size_t(bucket)){double peak=0;for(size_t f=first;f<std::min(reference.frames(),first+size_t(bucket));++f)for(int c=0;c<reference.channels;++c)peak=std::max(peak,std::min(1.,std::abs(reference.sample(f,c))));expected.push_back(peak);}
        check(dsd.error.empty()&&dsd.decodedFrames==reference.frames()&&dsd.bins==expected,"actual DSF streaming peaks match an independent whole-file decoder scan");
        check(dsd.duration==double(reference.frames())/reference.rate&&dsd.duration>.049&&dsd.duration<.051,"DSF waveform uses the real decoded duration");
        rta::analysis::waveform_detail::Service service;auto initial=service.request(utf(file),utf(cache),0,0);check(initial.find("\"state\":\"pending\"")!=std::string::npos,"waveform request returns asynchronous pending instead of blocking the UI");
        auto ready=wait_ready(service,utf(file),utf(cache));check(ready.find("\"state\":\"ready\"")!=std::string::npos&&ready.find("\"duration\":4")!=std::string::npos,"asynchronous production worker publishes the actual envelope");
        const auto key=rta::analysis::waveform_detail::identity(utf(file),0,0);const auto cacheFile=cache/(rta::analysis::waveform_detail::hash(key)+".rlw");WaveformResult disk;
        check(rta::analysis::waveform_detail::cache_read(cacheFile,key,disk)&&disk.bins==wav.bins,"disk cache stores real bins with the full identity, not only a hash");
        rta::analysis::waveform_detail::Service cold;check(cold.request(utf(file),utf(cache),0,0).find("\"state\":\"ready\"")!=std::string::npos,"a new worker reuses the validated disk envelope without decoding");
        auto time=fs::last_write_time(file);write_fixture(file,false,.125);fs::last_write_time(file,time+std::chrono::seconds(1));auto keyAfter=rta::analysis::waveform_detail::identity(utf(file),0,0);check(keyAfter!=key,"same-size file changes invalidate the cache by modification time");
        auto refreshed=wait_ready(service,utf(file),utf(cache));WaveformResult newer;check(refreshed.find("\"state\":\"ready\"")!=std::string::npos&&rta::analysis::waveform_detail::cache_read(cache/(rta::analysis::waveform_detail::hash(keyAfter)+".rlw"),keyAfter,newer)&&newer.bins!=disk.bins&&newer.bins.front()==.125,"changed source gets fresh measured data instead of stale cached bins");
        const auto clippedKey=rta::analysis::waveform_detail::identity(utf(file),1,2);check(clippedKey!=keyAfter,"CUE interval is part of cache identity");
        check(service.request(utf(run/"missing.flac"),utf(cache),0,0).find("\"state\":\"error\"")!=std::string::npos,"missing sources return explicit errors with no synthetic waveform");
        auto wrong=disk;wrong.bins.front()=std::numeric_limits<double>::quiet_NaN();const auto corrupt=cache/"corrupt.rlw";rta::analysis::waveform_detail::cache_write(corrupt,key,wrong);WaveformResult invalid;check(!rta::analysis::waveform_detail::cache_read(corrupt,key,invalid),"non-finite cached bins are rejected");
        check(!rta::analysis::waveform_detail::cache_read(cacheFile,clippedKey,invalid),"a cache hit must match the entire source and passage identity");
        check(service.request(utf(file),utf(cache),0,0,true).find("\"state\":\"ready\"")!=std::string::npos,"explicit retry reuses a valid ready result instead of starting duplicate work");
        const auto broken=run/"broken.flac";{std::ofstream f(broken,std::ios::binary);f<<"not an audio file";}
        auto failed=wait_ready(service,utf(broken),utf(cache));check(failed.find("\"state\":\"error\"")!=std::string::npos,"a real decoder error is published as a clear failure");
        auto retry=service.request(utf(broken),utf(cache),0,0,true);check(retry.find("\"state\":\"pending\"")!=std::string::npos,"explicit retry bypasses the two-second error cache immediately");
        // More than 4096 input frames cannot grow the envelope unboundedly.
        rta::analysis::waveform_detail::PeakEnvelope longTrack;for(size_t i=0;i<2000000;++i)longTrack.add(i==1200000?1:0);
        check(longTrack.bins.size()<=4096&&*std::max_element(longTrack.bins.begin(),longTrack.bins.end())==1,"adaptive bin merging retains isolated real peaks over two million frames");
        std::cout<<"RESULT checks="<<checks<<" failures="<<failures<<" wav_bins="<<wav.bins.size()<<" flac_bins="<<compressed.bins.size()<<" dsf_frames="<<dsd.decodedFrames<<" max_bins="<<longTrack.bins.size()<<'\n';
    }catch(const std::exception& e){++failures;std::cout<<"EXCEPTION "<<e.what()<<'\n';}
    return failures?1:0;
}
