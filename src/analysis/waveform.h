// Real whole-file peak envelope. Native PCM/DSD and a bounded decoder pipe;
// never opens an output device and never creates a decoded PCM temporary file.
#pragma once
#include "analysis/health_stream.h"
#include "platform/path.h"
#include "platform/process.h"
#include "platform/fs.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace rta::analysis {
struct WaveformResult {
    double duration=0;
    std::vector<double> bins;
    std::string error;
    bool cancelled=false;
    size_t decodedFrames=0;
};
namespace waveform_detail {
constexpr size_t kMaxBins=4096;
using Cancel=std::function<bool()>;
inline bool stop(const Cancel& cancelled){return cancelled&&cancelled();}
inline std::string quote(const std::string& value){
    std::string out="\"";size_t slashes=0;
    for(char c:value){if(c=='\\'){++slashes;continue;}if(c=='\"'){out.append(slashes*2+1,'\\');out+='\"';}else{out.append(slashes,'\\');out+=c;}slashes=0;}
    out.append(slashes*2,'\\');return out+'\"';
}
inline std::string decimal(double value){std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(17)<<value;return s.str();}
inline std::string escaped(const std::string& value){std::string s;for(unsigned char c:value){switch(c){case '\"':s+="\\\"";break;case '\\':s+="\\\\";break;case '\n':s+="\\n";break;case '\r':s+="\\r";break;case '\t':s+="\\t";break;default:if(c<32)s+=' ';else s+=char(c);}}return s;}
// At most 4096 bins, each representing exactly width consecutive frames.
// Pair merging preserves real peaks, including opposite-polarity channels.
struct PeakEnvelope {
    std::vector<double> bins;
    uint64_t frames=0,width=1;
    void add(double peak){
        if(frames%width==0){
            if(bins.size()==kMaxBins){for(size_t i=0;i<bins.size()/2;++i)bins[i]=std::max(bins[i*2],bins[i*2+1]);bins.resize(bins.size()/2);width*=2;}
            bins.push_back(peak);
        }else bins.back()=std::max(bins.back(),peak);
        ++frames;
    }
};
inline void add_buffer(PeakEnvelope& envelope,const rta::audio::AudioBuffer& block,size_t frames){
    for(size_t f=0;f<frames;++f){double peak=0;for(int c=0;c<block.channels;++c){double sample=block.sample(f,c);if(!std::isfinite(sample))throw std::runtime_error("Waveform: non-finite PCM sample");peak=std::max(peak,std::abs(sample));}envelope.add(std::min(1.,peak));}
}
template<class Source> inline WaveformResult native(Source& source,double clipStart,double clipLength,const Cancel& cancelled){
    WaveformResult result;const auto rate=source.rate();const size_t total=source.totalFrames();
    if(!rate||source.channels()<1||source.channels()>32||!total)throw std::runtime_error("Waveform: empty or invalid PCM source");
    const long double firstValue=std::floor(static_cast<long double>(clipStart)*rate+.5L);
    if(firstValue>=total)throw std::runtime_error("Waveform: passage starts beyond the track");
    const size_t first=static_cast<size_t>(firstValue);
    size_t wanted=total-first;
    if(clipLength>0){const long double value=std::floor(static_cast<long double>(clipLength)*rate+.5L);if(value<wanted)wanted=static_cast<size_t>(value);}
    if(!wanted)throw std::runtime_error("Waveform: empty passage");
    PeakEnvelope envelope;rta::audio::AudioBuffer block;
    for(size_t done=0;done<wanted;){if(stop(cancelled)){result.cancelled=true;return result;}size_t n=source.read(first+done,std::min<size_t>(8192,wanted-done),block);if(!n||n>block.frames())throw std::runtime_error("Waveform: PCM stream ended before the declared duration");add_buffer(envelope,block,n);done+=n;}
    result.decodedFrames=size_t(envelope.frames);result.duration=double(envelope.frames)/rate;result.bins=std::move(envelope.bins);return result;
}
struct PcmInfo{uint32_t rate=0;int channels=0;};
inline PcmInfo probe(const std::string& path,const Cancel& cancelled){
    const auto ffprobe=rta::audio::find_tool("ffprobe");if(ffprobe.empty())throw std::runtime_error("Waveform: bundled ffprobe is missing");
    std::string text;const auto process=rta::platform::run_hidden_stream(quote(ffprobe)+" -v error -select_streams a:0 -show_entries stream=sample_rate,channels -of default=noprint_wrappers=1 "+quote(path),[&](const void* data,size_t size){if(text.size()+size>65536)return false;text.append(static_cast<const char*>(data),size);return true;},cancelled);
    if(process.cancelled)throw std::runtime_error("Waveform: cancelled");if(!process.started||process.exit_code)throw std::runtime_error("Waveform: decoder probe failed: "+process.output);
    PcmInfo info;std::istringstream lines(text);std::string line;while(std::getline(lines,line)){if(line.rfind("sample_rate=",0)==0)info.rate=uint32_t(std::stoul(line.substr(12)));if(line.rfind("channels=",0)==0)info.channels=std::stoi(line.substr(9));}
    if(!info.rate||info.rate>4000000||info.channels<1||info.channels>32)throw std::runtime_error("Waveform: invalid decoder stream format");return info;
}
inline WaveformResult ffmpeg(const std::string& path,PcmInfo info,double clipStart,double clipLength,const Cancel& cancelled,const std::string& beforeInput="",const std::string& filter=""){
    auto ff=rta::audio::find_tool("ffmpeg");if(ff.empty())throw std::runtime_error("Waveform: bundled ffmpeg is missing");
    std::string command=quote(ff)+" -v error -nostdin "+beforeInput+" -i "+quote(path)+" -map 0:a:0 -vn -sn -dn";
    if(clipStart>0)command+=" -ss "+decimal(clipStart);if(clipLength>0)command+=" -t "+decimal(clipLength);if(!filter.empty())command+=" -af "+quote(filter);
    command+=" -c:a pcm_f32le -f f32le pipe:1";
    PeakEnvelope envelope;std::vector<unsigned char> bytes;bytes.reserve(65536+128);std::string decodeError;
    const size_t frameBytes=size_t(info.channels)*sizeof(float);
    auto process=rta::platform::run_hidden_stream(command,[&](const void* data,size_t size){
        const auto* begin=static_cast<const unsigned char*>(data);bytes.insert(bytes.end(),begin,begin+size);size_t used=0;
        while(bytes.size()-used>=frameBytes){double peak=0;for(int c=0;c<info.channels;++c){float sample=0;std::memcpy(&sample,bytes.data()+used+size_t(c)*sizeof(float),sizeof(float));if(!std::isfinite(sample)){decodeError="Waveform: non-finite decoded PCM";return false;}peak=std::max(peak,std::abs(double(sample)));}envelope.add(std::min(1.,peak));used+=frameBytes;}
        if(used){const size_t remain=bytes.size()-used;std::memmove(bytes.data(),bytes.data()+used,remain);bytes.resize(remain);}return !stop(cancelled);
    },cancelled);
    WaveformResult result;if(stop(cancelled)){result.cancelled=true;return result;}
    if(!decodeError.empty())throw std::runtime_error(decodeError);
    if(!process.started||process.exit_code||process.cancelled)throw std::runtime_error("Waveform: decoding failed: "+process.output);
    if(!bytes.empty()||!envelope.frames)throw std::runtime_error("Waveform: empty or truncated decoded PCM");
    result.duration=double(envelope.frames)/info.rate;result.decodedFrames=size_t(envelope.frames);result.bins=std::move(envelope.bins);return result;
}
// DVD-A MLP needs compressed AOB packet extraction. Only compressed packets are
// spooled; PCM still streams through the bounded pipe. LPCM never spools.
inline WaveformResult dvd_mlp(const std::string& path,const rta::audio::DvdAudioTrack& track,double clipStart,double clipLength,const Cancel& cancelled){
    auto info=rta::audio::dvd_audio_info(path);auto firstPts=rta::audio::dvd_first_mlp_pes_pts(path,track);
    auto skip=rta::audio::dvd_mlp_trim_samples(track.pts,firstPts,info.rate),count=(uint64_t(track.durationPts)*info.rate+45000)/90000;
    const auto temp=rta::platform::from_utf8(rta::platform::temp_dir_utf8())/("rlplayer-waveform-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".aob");
    struct Remove{std::filesystem::path path;~Remove(){std::error_code ec;std::filesystem::remove(path,ec);}}remove{temp};
    std::unique_ptr<FILE,decltype(&fclose)> source(rta::audio::sacd_detail::open(rta::audio::dvd_physical_path(path)),fclose);
    std::ofstream out(temp,std::ios::binary);if(!source||!out)throw std::runtime_error("Waveform: cannot prepare compressed DVD-A packets");
    uint64_t start=uint64_t(track.firstSector)*2048,end=(uint64_t(track.lastSector)+1)*2048,base=0;std::vector<unsigned char> buffer(65536);
    for(const auto& extent:track.aobs){uint64_t a=std::max(base,start),z=std::min(base+extent.bytes,end);for(uint64_t at=a;at<z;){if(stop(cancelled)){WaveformResult result;result.cancelled=true;return result;}auto n=size_t(std::min<uint64_t>(buffer.size(),z-at));if(!rta::audio::iso_extent_at(source.get(),extent,at-base,buffer.data(),n))throw std::runtime_error("Waveform: compressed DVD-A read failed");out.write(reinterpret_cast<char*>(buffer.data()),n);if(!out)throw std::runtime_error("Waveform: compressed DVD-A spool failed");at+=n;}base+=extent.bytes;}
    out.close();
    // Trim packet overlap by sample index before applying the requested CUE span.
    std::string filter="atrim=start_sample="+std::to_string(skip)+":end_sample="+std::to_string(skip+count)+",asetpts=PTS-STARTPTS";
    if(clipStart>0||clipLength>0){filter+=",atrim=start="+decimal(clipStart);if(clipLength>0)filter+=":duration="+decimal(clipLength);}
    return ffmpeg(rta::platform::to_utf8(temp),{uint32_t(info.rate),info.channels},0,0,cancelled,"-f mpeg",filter);
}
inline std::string identity(const std::string& path,double start,double length){
    auto physical=rta::platform::from_utf8(rta::audio::sacd_physical_path(path));std::error_code ec;
    if(!std::filesystem::is_regular_file(physical,ec))throw std::runtime_error("Waveform: source file cannot be read");
    auto size=std::filesystem::file_size(physical,ec);if(ec)throw std::runtime_error("Waveform: source size is unavailable");auto modified=std::filesystem::last_write_time(physical,ec);if(ec)throw std::runtime_error("Waveform: source modification time is unavailable");
    auto canonical=std::filesystem::weakly_canonical(physical,ec);if(ec)canonical=physical;
    return "RLW336-v1|"+rta::platform::to_utf8(canonical)+"|"+path+"|"+std::to_string(size)+"|"+std::to_string(modified.time_since_epoch().count())+"|"+decimal(start)+"|"+decimal(length);
}
inline std::string hash(const std::string& key){uint64_t value=14695981039346656037ull;for(unsigned char c:key){value^=c;value*=1099511628211ull;}std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<value;return out.str();}
inline bool cache_read(const std::filesystem::path& file,const std::string& key,WaveformResult& value){
    std::error_code ec;auto bytes=std::filesystem::file_size(file,ec);if(ec||bytes>256*1024||bytes<24)return false;std::ifstream in(file,std::ios::binary);char magic[8];uint32_t length=0,count=0;in.read(magic,8);in.read(reinterpret_cast<char*>(&length),4);if(!in||std::string(magic,8)!="RLW336v1"||length>65536||length!=key.size())return false;std::string stored(length,'\0');in.read(stored.data(),length);in.read(reinterpret_cast<char*>(&value.duration),8);in.read(reinterpret_cast<char*>(&count),4);if(!in||stored!=key||!std::isfinite(value.duration)||value.duration<=0||count<1||count>kMaxBins)return false;value.bins.resize(count);in.read(reinterpret_cast<char*>(value.bins.data()),count*sizeof(double));if(!in)return false;return std::all_of(value.bins.begin(),value.bins.end(),[](double b){return std::isfinite(b)&&b>=0&&b<=1;});
}
inline void cache_write(const std::filesystem::path& file,const std::string& key,const WaveformResult& value){
    try{std::error_code ec;std::filesystem::create_directories(file.parent_path(),ec);if(ec)return;auto temporary=file;temporary+=".part";std::ofstream out(temporary,std::ios::binary);uint32_t length=uint32_t(key.size()),count=uint32_t(value.bins.size());out.write("RLW336v1",8);out.write(reinterpret_cast<const char*>(&length),4);out.write(key.data(),length);out.write(reinterpret_cast<const char*>(&value.duration),8);out.write(reinterpret_cast<const char*>(&count),4);out.write(reinterpret_cast<const char*>(value.bins.data()),count*sizeof(double));out.close();if(out)rta::platform::atomic_replace(temporary,file);}catch(...){}
}
inline std::string json(const std::string& state,const WaveformResult& value){
    std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"ok\":"<<(state=="error"?"false":"true")<<",\"state\":\""<<state<<"\",\"duration\":"<<std::setprecision(12)<<value.duration<<",\"bins\":[";
    for(size_t i=0;i<value.bins.size();++i){if(i)out<<',';out<<std::setprecision(8)<<value.bins[i];}out<<"],\"error\":\""<<escaped(value.error)<<"\"}";return out.str();
}
} // namespace waveform_detail

// Synchronous offline seam used by regression tests and the one worker below.
inline WaveformResult waveform_compute(const std::string& file,double clipStart=0,double clipLength=0,const waveform_detail::Cancel& cancelled={}){
    WaveformResult result;auto start=std::chrono::steady_clock::now();
    auto cancel=[&](){return waveform_detail::stop(cancelled)||std::chrono::steady_clock::now()-start>std::chrono::minutes(5);};
    try{
        if(!std::isfinite(clipStart)||!std::isfinite(clipLength)||clipStart<0||clipLength<0)throw std::runtime_error("Waveform: invalid passage range");
        if(cancel()){result.cancelled=true;return result;}
        const auto key=rta::audio::image_track_key(file);const auto ext=rta::audio::lowerExt(key.physical);
        if(ext==".iso"){
            if(key.kind!="sacd"&&rta::audio::sacd_tracks(file).empty()){
                auto track=rta::audio::dvd_audio_select(file);if(track.mlp)return waveform_detail::dvd_mlp(file,track,clipStart,clipLength,cancel);
                rta::audio::DvdAudioPcmStream source;std::string error;if(!source.open(file,&error))throw std::runtime_error(error);return waveform_detail::native(source,clipStart,clipLength,cancel);
            }
            rta::audio::DsdPcmStream source;std::string error;if(!source.open(file,rta::audio::dsd_pcm_settings(),&error))throw std::runtime_error(error);return waveform_detail::native(source,clipStart,clipLength,cancel);
        }
        detail::HealthSource source;std::string error;
        if(source.open(file,&error))return waveform_detail::native(source,clipStart,clipLength,cancel);
        return waveform_detail::ffmpeg(file,waveform_detail::probe(file,cancel),clipStart,clipLength,cancel);
    }catch(const std::exception& error){result.error=error.what();result.cancelled=cancel();return result;}
}
namespace waveform_detail {
class Service {
    struct Job{std::string file,key,cacheDir;double start=0,length=0;uint64_t generation=0;};
    struct Entry{std::string state;WaveformResult result;std::chrono::steady_clock::time_point written;};
    std::mutex mutex_;std::condition_variable ready_;std::optional<Job> queued_;std::map<std::string,Entry> entries_;
    std::atomic<uint64_t> generation_{0};std::atomic<bool> stopping_{false};std::string desiredKey_,activeKey_;uint64_t activeGeneration_=0;std::thread worker_;
    void run(){for(;;){Job job;{
        std::unique_lock<std::mutex> lock(mutex_);ready_.wait(lock,[&]{return stopping_||queued_.has_value();});if(stopping_)return;job=std::move(*queued_);queued_.reset();activeKey_=job.key;activeGeneration_=job.generation;
    }
        auto cancelled=[&]{return stopping_||generation_.load()!=job.generation;};
        auto result=waveform_compute(job.file,job.start,job.length,cancelled);
        if(!cancelled()&&!result.cancelled&&result.error.empty()){try{if(identity(job.file,job.start,job.length)!=job.key){result.bins.clear();result.duration=0;result.error="Waveform: source changed during decoding";}}catch(const std::exception&e){result.bins.clear();result.duration=0;result.error=e.what();}}
        if(!cancelled()&&!result.cancelled&&result.error.empty()&&!job.cacheDir.empty())cache_write(rta::platform::from_utf8(job.cacheDir)/(hash(job.key)+".rlw"),job.key,result);
        std::lock_guard<std::mutex> lock(mutex_);activeKey_.clear();
        if(cancelled()||result.cancelled){auto it=entries_.find(job.key);if(it!=entries_.end()&&it->second.state=="pending"&&!(queued_&&queued_->key==job.key))entries_.erase(it);continue;}
        entries_[job.key]={result.error.empty()?"ready":"error",std::move(result),std::chrono::steady_clock::now()};
        while(entries_.size()>32){auto oldest=entries_.end();for(auto it=entries_.begin();it!=entries_.end();++it)if(it->second.state!="pending"&&(oldest==entries_.end()||it->second.written<oldest->second.written))oldest=it;if(oldest==entries_.end())break;entries_.erase(oldest);}
    }}
public:
    Service():worker_([this]{run();}){}
    ~Service(){stopping_=true;++generation_;ready_.notify_all();if(worker_.joinable())worker_.join();}
    std::string request(const std::string& file,const std::string& cacheDir,double start,double length,bool fresh=false){
        WaveformResult pending;pending.duration=length;std::string key;
        try{if(file.empty()||!std::isfinite(start)||!std::isfinite(length)||start<0||length<0)throw std::runtime_error("Waveform: invalid file or passage");key=identity(file,start,length);}catch(const std::exception&e){pending.duration=0;pending.error=e.what();return json("error",pending);}
        std::lock_guard<std::mutex> lock(mutex_);if(desiredKey_!=key){desiredKey_=key;++generation_;}
        auto it=entries_.find(key);
        if(it!=entries_.end()){
            if(it->second.state=="ready")return json("ready",it->second.result);
            if(!fresh&&it->second.state=="error"&&std::chrono::steady_clock::now()-it->second.written<std::chrono::seconds(2))return json("error",it->second.result);
            if(it->second.state=="pending"&&((activeKey_==key&&activeGeneration_==generation_)||(queued_&&queued_->key==key)))return json("pending",it->second.result);
        }
        WaveformResult cached;if(!cacheDir.empty()&&cache_read(rta::platform::from_utf8(cacheDir)/(hash(key)+".rlw"),key,cached)){entries_[key]={"ready",cached,std::chrono::steady_clock::now()};return json("ready",cached);}
        if(queued_&&queued_->key!=key){auto stale=entries_.find(queued_->key);if(stale!=entries_.end()&&stale->second.state=="pending")entries_.erase(stale);}
        queued_=Job{file,key,cacheDir,start,length,generation_};entries_[key]={"pending",pending,std::chrono::steady_clock::now()};ready_.notify_one();return json("pending",pending);
    }
};
inline Service& service(){static Service instance;return instance;}
} // namespace waveform_detail
inline std::string waveform_request_json(const std::string& file,const std::string& cacheDir,double clipStart=0,double clipLength=0,bool fresh=false){return waveform_detail::service().request(file,cacheDir,clipStart,clipLength,fresh);}
} // namespace rta::analysis
