// cast_service.h — UPnP 媒体投放后台会话编排与状态模型 (ROADMAP P5b)
#pragma once
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include "net/upnp.h"
#include "net/upnp_control.h"
#include "audio/wav.h"
#include "audio/audio_loader.h"
#include "audio/dsd_reader.h"
#include "dsp/dop.h"
#include "engine/pipeline.h"
#include "audio/cast_upsample_plan.h"
#include "analysis/health_stream.h"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <functional>
#include <cstdint>

namespace rta::net {

namespace fs = std::filesystem;

inline std::string cast_lower_str(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ---------------- 供源格式表（2026-10-29）----------------
// 先说清一件容易误判的事：投放链路**永远**把曲目解码成 WAV 再供源，所以
//   · HTTP 的 Content-Type = audio/wav
//   · DIDL 的 res@protocolInfo = http-get:*:audio/wav:*
// 是"与线上字节一致"的，不是把 FLAC/APE 谎称成 WAV。表里这些 MIME 只用来说明"源是什么"，
// 以及将来要做"原样直通（不转码）"时该声明什么 —— 现在不参与线上声明。
inline std::string cast_source_mime(const std::string& ext) {
    if (ext == ".flac") return "audio/flac";
    if (ext == ".wav" || ext == ".wave") return "audio/wav";
    if (ext == ".dsf") return "audio/dsf";
    if (ext == ".dff") return "audio/dff";
    if (ext == ".ape") return "audio/ape";
    if (ext == ".alac" || ext == ".m4a") return "audio/mp4";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".aif" || ext == ".aiff") return "audio/aiff";
    if (ext == ".wv") return "audio/x-wavpack";
    if (ext == ".ogg") return "audio/ogg";
    if (ext == ".opus") return "audio/opus";
    if (ext == ".wma") return "audio/x-ms-wma";
    if (ext == ".tta") return "audio/tta";
    if (ext == ".tak") return "audio/tak";
    if (ext == ".iso") return "application/x-sacd-iso";
    return "application/octet-stream";
}
// 人话：给 UI/日志用
inline std::string cast_source_kind(const std::string& ext) {
    if (ext == ".flac") return "FLAC 无损";
    if (ext == ".wav" || ext == ".wave") return "WAV";
    if (ext == ".dsf" || ext == ".dff") return "DSD";
    if (ext == ".ape") return "APE 无损";
    if (ext == ".alac" || ext == ".m4a") return "ALAC/AAC";
    if (ext == ".mp3") return "MP3 有损";
    if (ext == ".aif" || ext == ".aiff") return "AIFF";
    if (ext == ".wv") return "WavPack";
    if (ext == ".ogg" || ext == ".opus") return "Ogg/Opus";
    if (ext == ".wma") return "WMA";
    return ext.empty() ? "未知" : ext;
}

// 只读前 1MB 判断"这个 WAV 能不能原样交给渲染器"（不做整曲解码，几毫秒）。
// 判据 = 渲染器实际能吃的那一档：整型 PCM、16/24-bit、≤2 声道、≤192k。
// 返回 false 时 why 里写清是哪一条不合格（会进日志与 UI 的 warning）。
inline bool cast_wav_can_serve_raw(const std::string& path, rta::audio::WavHeaderInfo& hi, std::string& why) {
    std::FILE* f = upnp_detail::open_file_utf8(path, L"rb", "rb");
    if (!f) { why = "文件打不开"; return false; }
    std::vector<unsigned char> buf(1u << 20);
    const size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n < 12) { why = "文件过短"; return false; }
    std::string err;
    const int rc = rta::audio::parse_wav_header(buf.data(), n, hi, &err);
    if (rc <= 0) {
        why = err.empty() ? std::string("WAV 头在 1MB 内没解析出来（有超大附属块？）") : err;
        return false;
    }
    if (hi.fmtTag != 1) { why = "不是整型 PCM（fmtTag=" + std::to_string((int)hi.fmtTag) + "）"; return false; }
    if (hi.bits != 16 && hi.bits != 24) {
        why = std::to_string((int)hi.bits) + "-bit（渲染器普遍只认 16/24-bit）";
        return false;
    }
    if (hi.channels < 1 || hi.channels > 2) {
        why = std::to_string((int)hi.channels) + " 声道（多数 DLNA 渲染器只支持 ≤2）";
        return false;
    }
    if (hi.rate > 192000u) { why = std::to_string(hi.rate) + "Hz（超过 192k）"; return false; }
    return true;
}

inline std::string make_cast_temp_wav_path(const char* tag) {
    std::error_code ec;
    auto td = fs::temp_directory_path(ec);
    static std::atomic<uint64_t> s_seq{0};
    auto name = "rtacast_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + "_" + std::to_string(++s_seq) + "_" + (tag ? tag : "") + ".wav";
    auto u8=(td / name).u8string();return std::string(u8.begin(),u8.end());
}

struct CastState {
    bool        running = false;
    bool        naturalEnd = false;
    bool        cleanupApplied=false;double cleanupMeanReductionDb=0,cleanupModifiedBinFraction=0;
    rta::dsp::RecordingCleanupAutoState cleanupAutoState=rta::dsp::RecordingCleanupAutoState::Manual;
    bool cleanupAutoEstimateValid=false,cleanupAutoFrozenForDrain=false;
    double cleanupEstimatedNoiseFloorDbfs=-144,cleanupAppliedNoiseFloorDbfs=-144,cleanupAutoEvidenceCoverage=0;
    size_t cleanupAutoLearningFrames=0,cleanupAutoRejectedMusicFrames=0;
    double      prepareProgress = 0.0;
    std::string  prepareNote;
    std::string device;
    std::string sourceFile;
    uint16_t    servePort = 8912;
    std::string phase = "stopped"; // discovering, serving, playing, stopped, error
    double      positionSec = 0.0;
    std::string transportState = "STOPPED";
    int64_t     servedBytes = 0;
    int         requestCount = 0;
    // 261024-01 #9：UPnP 作为输出后端时，播放器（底栏进度条 / 时长 / 格式 / 暂停）
    // 的状态统一取这次投放会话 —— 一条会话、一个状态源，避免两套控制互相打架。
    double      durationSec = 0.0;
    uint32_t    sampleRate = 0;
    int         bits = 0;
    bool        paused = false;
    std::string rateMatch = "unknown"; // match, mismatch, unknown（唯一依据 = GetProtocolInfo 的 Sink）
    std::string warning;
    std::string error;
    // ---- 2026-10-29：bit-perfect 真实探测（取代 rtaweb 里硬编码的 "no"）----
    // bitPerfect: yes / no / unknown —— 只有"软件侧逐位 + 渲染器 Sink 显式支持该格式 + 已取流在播"
    // 三条同时成立才给 yes；能证明不成立给 no；其余 unknown（前端应显示"无法判定"而不是"非 bit-perfect"）。
    std::string bitPerfect = "unknown";
    std::string bitPerfectWhy;
    bool        softwareBitPerfect = false;  // 软件侧（解码→供源）是否逐位，不含渲染器
    std::string serveFormat;                 // 实际供源格式，如 "WAV 44100Hz/16bit/2ch（源 .flac）"
    std::string sinkSummary;                 // 渲染器 Sink 能力摘要
    std::string servedUrl;                   // 渲染器要取的 URL（排障用）
};

struct CastStartParams {
    std::string dev;
    std::string file;
    uint16_t    port = 8912;
    uint32_t    rate = 0;
    int         bits = 0;
    double      seconds = 0.0;
    bool        dop = false;
    bool        upsample = false;
    bool        forceRate = false;
    uint32_t    rateCeiling = 192000;
    std::string requestedRate = "max";
    bool cleanup=false,cleanupAuto=true;double cleanupStrength=.5,cleanupNoiseDb=-101.1;
    int twoStage=-1;
    std::string filter = "poly-sinc-tight-lp", filterNx;
    double      cueStart = -1.0, cueEnd = -1.0;
    uint64_t    diskReserveBytes = 1048576;
    uint64_t    maxOutputBytes = 0xFFFFFFFFull - 36; // RIFF/WAVE, not RF64
};

struct CastStartResult {
    bool ok = false;
    std::string error;
};


// Stream decoder: native WAV keeps integer samples; compressed formats use the
// bundled decoder's declared PCM format. No complete-track PCM allocation.
class CastPcmReader {
public:
    ~CastPcmReader() { close(); }
    bool open(const std::string& path, std::atomic<bool>& cancel, const CastStartParams* params=nullptr) {
        cancel_=&cancel;
        if(cancel.load())throw std::runtime_error("投放准备已取消");
        std::string input=path, inputOptions, filter;
        rta::audio::FfmpegPcmChoice pcm;
        bool dvdMlp=false;
        if(rta::audio::lowerExt(path)==".iso") {
            // Selecting first validates the entire virtual key, including its
            // media kind. Never strip a suffix and fall back to the first track.
            const auto track=rta::audio::dvd_audio_select(path);
            const auto info=rta::audio::dvd_audio_info(path);
            rate=info.rate;channels=info.channels;bits=info.bits==20?24:info.bits;
            frames=(uint64_t(track.durationPts)*rate+45000)/90000;
            if(!track.mlp) {dvd_=std::make_unique<rta::audio::DvdLpcmReader>();dvd_->open(path);return true;}
            dvdMlp=true;expectedFrames_=frames;
            // Reject an impossible RIFF budget before extracting a large AOB.
            const uint64_t limit=params?std::min<uint64_t>(params->maxOutputBytes,0xFFFFFFFFull-36):0xFFFFFFFFull-36;
            const int minimumBits=params&&params->bits?params->bits:bits;
            if(minimumBits!=16&&minimumBits!=24&&minimumBits!=32)throw std::runtime_error("投放输出位深仅支持 16/24/32");
            if(frames>limit/(uint64_t(channels)*(minimumBits/8)))throw std::runtime_error("投放 WAV 超过 RIFF 4GiB 或输出预算上限");
            const auto skip=rta::audio::dvd_mlp_trim_samples(track.pts,rta::audio::dvd_first_mlp_pes_pts(path,track),rate);
            // The source is a bounded selected AOB range, not an ISO filename
            // passed to ffmpeg. PCM goes directly into the cancellable pipe.
            aobPath_=make_cast_temp_wav_path("mlp-source");
            std::unique_ptr<FILE,decltype(&fclose)> source(rta::audio::sacd_detail::open(rta::audio::dvd_physical_path(path)),fclose);
            std::unique_ptr<FILE,decltype(&fclose)> out(upnp_detail::open_file_utf8(aobPath_,L"wb","wb"),fclose);
            if(!source||!out)throw std::runtime_error("DVD-A MLP: 无法创建选轨临时文件");
            uint64_t first=uint64_t(track.firstSector)*2048,last=(uint64_t(track.lastSector)+1)*2048,base=0,copied=0;
            std::array<uint8_t,65536> chunk{};
            for(const auto& extent:track.aobs) {
                uint64_t at=std::max(first,base),end=std::min(last,base+extent.bytes);
                while(at<end) {
                    if(cancel.load())throw std::runtime_error("投放准备已取消");
                    size_t n=(size_t)std::min<uint64_t>(chunk.size(),end-at);
                    std::error_code ec;const auto space=fs::space(fs::u8path(aobPath_).parent_path(),ec);
                    const auto reserve=params?params->diskReserveBytes:1048576;
                    if(!ec&&(space.available<n||space.available-n<reserve))throw std::runtime_error("投放准备磁盘空间不足");
                    if(!rta::audio::iso_extent_at(source.get(),extent,at-base,chunk.data(),n)||fwrite(chunk.data(),1,n,out.get())!=n)throw std::runtime_error("DVD-A MLP: 选轨提取短读写");
                    at+=n;copied+=n;
                }
                base+=extent.bytes;
            }
            if(copied!=last-first||fflush(out.get()))throw std::runtime_error("DVD-A MLP: 选轨范围不完整");
            out.reset();input=aobPath_;inputOptions=" -f mpeg";
            filter=" -af atrim=start_sample="+std::to_string(skip)+":end_sample="+std::to_string(skip+frames);
            pcm.codec=bits==16?"pcm_s16le":bits==24?"pcm_s24le":"pcm_s32le";
        }
        if (wav_.open(path)) { native_=true; rate=wav_.rate(); bits=wav_.bits(); channels=wav_.channels(); floating=wav_.isFloat(); frames=wav_.totalFrames(); return true; }
        const auto tool=rta::audio::find_tool("ffmpeg");
        if(tool.empty()) throw std::runtime_error("投放解码需要包内 ffmpeg.exe");
        if(!dvdMlp)pcm=rta::audio::probe_ffmpeg_pcm(rta::audio::find_tool("ffprobe"),path);
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE wr=nullptr;
        if(!CreatePipe(&rd_,&wr,&sa,0)) throw std::runtime_error("投放解码管道创建失败");
        SetHandleInformation(rd_,HANDLE_FLAG_INHERIT,0);
        HANDLE nul=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
        STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=wr;si.hStdError=nul;si.hStdInput=nullptr;
        auto cmd=L"\""+rta::audio::widen(tool)+L"\" -nostdin -v error"+rta::audio::widen(inputOptions)+L" -i \""+rta::audio::widen(input)+L"\" -map 0:a:0 -vn"+rta::audio::widen(filter)+L" -c:a "+rta::audio::widen(pcm.codec)+L" -f wav pipe:1";
        PROCESS_INFORMATION pi{};
        const bool ok=CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)!=0;
        CloseHandle(wr);CloseHandle(nul);
        if(!ok) throw std::runtime_error("无法启动投放解码器");
        process_=pi.hProcess;CloseHandle(pi.hThread);
        std::vector<uint8_t> h; rta::audio::WavHeaderInfo hi;
        while(h.size()<(1u<<20)) {
            uint8_t b; if(!bytes(&b,1)) throw std::runtime_error("投放解码器未输出有效 WAV 头"); h.push_back(b);
            const int rc=rta::audio::parse_wav_header(h.data(),h.size(),hi);
            if(rc<0) throw std::runtime_error("投放解码器 WAV 头非法");
            if(rc>0) {if(dvdMlp&&(hi.rate!=rate||hi.bits!=bits||hi.channels!=channels||hi.isFloat))throw std::runtime_error("DVD-A MLP: 解码格式与音轨头不一致");rate=hi.rate;bits=hi.bits;channels=hi.channels;floating=hi.isFloat;return true;}
        }
        throw std::runtime_error("投放解码器 WAV 头超过 1MiB");
    }
    size_t read(rta::audio::AudioBuffer& out, size_t count=4096) {
        if(cancel_&&cancel_->load())throw std::runtime_error("投放准备已取消");
        if(dvd_) {
            const size_t n=dvd_->read(pos_,count,out);pos_+=n;
            // Native DVD unpacking uses a left-aligned int32 buffer. Restore
            // its exact source precision at this boundary, with no quantizer.
            if(bits<32)for(auto& v:out.raw)v>>=(32-bits);
            out.bits=bits;return n;
        }
        if(native_) {auto n=wav_.read(pos_,count,out);pos_+=n;return n;}
        if(expectedFrames_){if(pos_>=expectedFrames_){out.raw.clear();return 0;}count=(size_t)std::min<uint64_t>(count,expectedFrames_-pos_);}
        const size_t stride=(size_t)channels*(bits/8);std::vector<uint8_t> b(count*stride);
        const size_t got=bytes(b.data(),b.size());
        if(got%stride) throw std::runtime_error("投放解码输出截断（不足一帧）");
        out.rate=rate;out.bits=bits;out.channels=channels;out.isFloat=floating;out.raw.resize(got/(bits/8));
        for(size_t i=0;i<out.raw.size();++i)out.raw[i]=rta::audio::decode_sample(b.data()+i*(bits/8),bits,floating);
        if(!got && process_ && WaitForSingleObject(process_,10000)==WAIT_OBJECT_0) { DWORD rc=0;GetExitCodeProcess(process_,&rc);if(rc)throw std::runtime_error("投放解码失败，退出码="+std::to_string((int)rc)); }
        pos_+=got/stride;
        if(expectedFrames_&&got<b.size())throw std::runtime_error("DVD-A MLP: 解码在 IFO 轨尾之前截断");
        return got/stride;
    }
    uint32_t rate=0;int bits=0,channels=0;bool floating=false;uint64_t frames=0;
private:
    size_t bytes(uint8_t* dst,size_t want) {
        size_t done=0; auto last=std::chrono::steady_clock::now();
        while(done<want) {
            if(cancel_ && cancel_->load())throw std::runtime_error("投放准备已取消");
            DWORD avail=0;if(!PeekNamedPipe(rd_,nullptr,0,nullptr,&avail,nullptr))break;
            if(avail) {DWORD n=0;if(!ReadFile(rd_,dst+done,(DWORD)std::min<size_t>(want-done,avail),&n,nullptr)||!n)break;done+=n;last=std::chrono::steady_clock::now();}
            else {if(WaitForSingleObject(process_,0)==WAIT_OBJECT_0)break;if(std::chrono::steady_clock::now()-last>std::chrono::seconds(30))throw std::runtime_error("投放解码器 30 秒未产生数据");Sleep(2);}
        }
        return done;
    }
    void close() {if(rd_){CloseHandle(rd_);rd_=nullptr;}if(process_){WaitForSingleObject(process_,10000);CloseHandle(process_);process_=nullptr;}if(!aobPath_.empty()){std::error_code ec;fs::remove(fs::u8path(aobPath_),ec);}}
    rta::analysis::detail::WavPcmStream wav_;std::unique_ptr<rta::audio::DvdLpcmReader>dvd_;std::string aobPath_;uint64_t expectedFrames_=0;bool native_=false;size_t pos_=0;HANDLE rd_=nullptr,process_=nullptr;std::atomic<bool>* cancel_=nullptr;
};

// Match an entire renderer tuple. Never combine the rate of one entry with
// the bit depth/channels of another entry (the old independent-max trap).
inline UpnpSinkCaps cast_parse_sink_caps(const std::string& sink) {
    auto caps=parse_sink_caps(sink);size_t pos=0,index=0;
    while(pos<sink.size() && index<caps.entries.size()) {
        auto end=sink.find(',',pos);auto token=cast_lower_str(upnp_trim(sink.substr(pos,end==std::string::npos?end:end-pos)));
        if(!token.empty()) {
            auto& entry=caps.entries[index++];
            for(const auto* key:{";bits=",";bitspersample="}) {auto at=token.find(key);if(at!=std::string::npos){int b=atoi(token.c_str()+at+strlen(key));if(b==16||b==24||b==32)entry.bits=b;}}
        }
        if(end==std::string::npos)break;pos=end+1;
    }
    return caps;
}
inline bool cast_tuple_compatible(const UpnpSinkCaps& caps,int bits,int channels) {
    for(const auto& e:caps.entries)if(e.is_wav() && (!e.bits||e.bits==bits) && (!e.channels||e.channels==channels))return true;
    return false;
}
inline std::vector<uint32_t> cast_compatible_rates(const UpnpSinkCaps& caps,int bits,int channels) {
    std::vector<uint32_t> rates;
    for(const auto& e:caps.entries) if(e.is_wav() && e.rate && (!e.bits||e.bits==bits) && (!e.channels||e.channels==channels)) rates.push_back(e.rate);
    return rates;
}
struct CastPreparedPcm {
    uint32_t rate=0;int bits=0,channels=0;uint64_t frames=0;
    bool floating=false,changed=false,cleanupApplied=false;
    double cleanupMeanReductionDb=0,cleanupModifiedBinFraction=0;
    rta::dsp::RecordingCleanupAutoState cleanupAutoState=rta::dsp::RecordingCleanupAutoState::Manual;
    bool cleanupAutoEstimateValid=false,cleanupAutoFrozenForDrain=false;
    double cleanupEstimatedNoiseFloorDbfs=-144,cleanupAppliedNoiseFloorDbfs=-144,cleanupAutoEvidenceCoverage=0;
    size_t cleanupAutoLearningFrames=0,cleanupAutoRejectedMusicFrames=0;
    std::string note;
};

// Resolve with the same strict navigation/key parser used by local playback.
// A foreign or missing selection must fail before any renderer is contacted.
inline bool cast_is_sacd_image(const std::string& path) {
    const auto key=rta::audio::image_track_key(path);
    if(rta::audio::lowerExt(path)!=".iso")return false;
    if(key.kind=="sacd"){rta::audio::sacd_select(path);return true;}
    if(key.kind=="dvda"){rta::audio::dvd_audio_select(path);return false;}
    if(!rta::audio::sacd_tracks(path).empty()){rta::audio::sacd_select(path);return true;}
    rta::audio::dvd_audio_select(path);return false;
}

inline CastPreparedPcm cast_prepare_sacd_dop(const CastStartParams& p,const std::string& output,
        std::atomic<bool>& cancel,std::function<void(double,const std::string&)> progress={}) {
    if(!p.dop)throw std::runtime_error("SACD DSD 投放必须开启 DoP，拒绝转换 PCM");
    if(p.cleanup)throw std::runtime_error("DSD DoP 逐位供源不经过 PCM 清理");
    if(cancel.load())throw std::runtime_error("投放准备已取消");
    rta::audio::SacdReader reader;reader.open(p.file);
    const auto& track=reader.track();const uint64_t bytesPerChannel=track.bits()/8;
    if(track.bits()%16)throw std::runtime_error("SACD: 音轨尾不足一个完整 DoP 帧，拒绝截断");
    CastPreparedPcm result;result.rate=track.rate/16;result.bits=24;result.channels=track.channels;result.frames=track.bits()/16;
    result.note="SACD DoP v1.1 分块打包：原始 DSD 位保持不变（未做 PCM 转换）";
    const uint64_t limit=std::min<uint64_t>(p.maxOutputBytes,0xFFFFFFFFull-36);
    if(result.frames>limit/(uint64_t(result.channels)*3))throw std::runtime_error("投放 WAV 超过 RIFF 4GiB 或输出预算上限");
    FILE* f=upnp_detail::open_file_utf8(output,L"wb","wb");if(!f)throw std::runtime_error("投放临时 DoP WAV 无法创建");
    struct Guard{FILE*f;std::string path;bool ok=false;~Guard(){if(f)fclose(f);if(!ok){std::error_code ec;fs::remove(fs::u8path(path),ec);}}}guard{f,output};
    const uint64_t dataBytes=result.frames*result.channels*3;
    std::vector<uint8_t> h;h.insert(h.end(),{'R','I','F','F'});rta::audio::detail::wr32(h,(uint32_t)dataBytes+36);h.insert(h.end(),{'W','A','V','E','f','m','t',' '});rta::audio::detail::wr32(h,16);rta::audio::detail::wr16(h,1);rta::audio::detail::wr16(h,(uint16_t)result.channels);rta::audio::detail::wr32(h,result.rate);rta::audio::detail::wr32(h,result.rate*result.channels*3);rta::audio::detail::wr16(h,(uint16_t)(result.channels*3));rta::audio::detail::wr16(h,24);h.insert(h.end(),{'d','a','t','a'});rta::audio::detail::wr32(h,(uint32_t)dataBytes);
    if(fwrite(h.data(),1,h.size(),f)!=h.size())throw std::runtime_error("DoP WAV 头写入失败");
    uint64_t position=0;std::vector<std::vector<uint8_t>> planes;
    if(progress)progress(0,result.note);
    while(position<bytesPerChannel) {
        if(cancel.load())throw std::runtime_error("投放准备已取消");
        const size_t n=(size_t)std::min<uint64_t>(4096,bytesPerChannel-position);
        if(reader.read(n,planes)!=n||n%2)throw std::runtime_error("SACD: DoP 音轨短读或半帧");
        std::vector<uint8_t> block;block.reserve(n/2*result.channels*3);
        for(size_t i=0;i<n;i+=2)for(int c=0;c<result.channels;++c){block.push_back(planes[c][i+1]);block.push_back(planes[c][i]);block.push_back(((position+i)/2&1)?rta::dsp::DOP_MARKER_B:rta::dsp::DOP_MARKER_A);}
        std::error_code ec;const auto parent=fs::u8path(output).parent_path();const auto space=fs::space(parent.empty()?fs::current_path():parent,ec);
        if(!ec&&(space.available<block.size()||space.available-block.size()<p.diskReserveBytes))throw std::runtime_error("投放准备磁盘空间不足");
        if(fwrite(block.data(),1,block.size(),f)!=block.size())throw std::runtime_error("DoP WAV 写入失败");
        position+=n;if(progress)progress(double(position)/bytesPerChannel,result.note);
    }
    if(cancel.load())throw std::runtime_error("投放准备已取消");
    if(fflush(f))throw std::runtime_error("DoP WAV 完成写入失败");guard.ok=true;return result;
}

inline CastPreparedPcm cast_prepare_pcm(const CastStartParams& p,const std::string& output,
        std::atomic<bool>& cancel,const UpnpSinkCaps& caps,
        std::function<void(double,const std::string&)> progress) {
    rta::dsp::DesignCancellationScope cancellation(&cancel);
    CastPcmReader reader;reader.open(p.file,cancel,&p);
    CastPreparedPcm result;result.rate=reader.rate;result.bits=p.bits?p.bits:reader.bits;result.channels=reader.channels;
    if(result.bits!=16 && result.bits!=24 && result.bits!=32)throw std::runtime_error("投放输出位深仅支持 16/24/32");
    if(caps.ok && !p.bits && !cast_tuple_compatible(caps,result.bits,result.channels)) {
        for(int candidate:{24,16})if(candidate<=reader.bits && cast_tuple_compatible(caps,candidate,result.channels)){result.bits=candidate;break;}
    }
    const auto rates=cast_compatible_rates(caps,result.bits,result.channels);
    auto decision=rta::audio::decide_cast_target_rate(reader.rate,p.rate?std::to_string(p.rate):p.requestedRate,rates,p.rateCeiling,p.upsample||p.rate>0,p.forceRate);
    // An explicit incompatible WAV tuple must not become 'undeclared'.
    if(caps.ok && !cast_tuple_compatible(caps,result.bits,result.channels) && !p.forceRate && decision.upsample) {decision.targetRate=reader.rate;decision.upsample=false;decision.noteZh="渲染器未声明匹配位深/声道的 WAV 升频格式，按源直供";}
    result.rate=decision.targetRate?decision.targetRate:reader.rate;result.note=decision.noteZh;
    rta::engine::PipelineConfig cfg;cfg.outRate=result.rate;cfg.outBits=result.bits;cfg.gpuResample=false;cfg.autoTaps=true;
    cfg.recordingCleanup.enabled=p.cleanup;cfg.recordingCleanup.strength=p.cleanupStrength;cfg.recordingCleanup.noiseFloorDbfs=p.cleanupNoiseDb;
    cfg.recordingCleanup.noiseMode=p.cleanupAuto?rta::dsp::RecordingCleanupNoiseMode::Auto:rta::dsp::RecordingCleanupNoiseMode::Manual;
    if(decision.upsample) {const std::string name=reader.rate>48000&&!p.filterNx.empty()?p.filterNx:p.filter;const auto* fp=rta::dsp::find_preset(name=="auto"?"poly-sinc-tight-lp":name,false);if(!fp)throw std::runtime_error("未知投放滤波器: "+p.filter);rta::engine::apply_filter_preset(cfg,*fp);}
    rta::engine::apply_resample_purpose(cfg,rta::dsp::ResamplePurpose::Pcm);
    if(p.twoStage>=0){cfg.twoStage=p.twoStage!=0;cfg.allowAutomaticCascade=false;}
    result.floating=reader.floating && !decision.upsample && !(p.cleanup&&p.cleanupStrength>0) && (!p.bits||p.bits==reader.bits);
    result.changed=decision.upsample || result.bits!=reader.bits || (reader.floating&&!result.floating) || (p.cleanup&&p.cleanupStrength>0);
    if(result.bits<reader.bits || decision.upsample)cfg.shaper=rta::dsp::Shaper::TPDF;
    rta::engine::Pipeline pipeline(cfg);
    FILE* f=upnp_detail::open_file_utf8(output,L"wb","wb");if(!f)throw std::runtime_error("投放临时 WAV 无法创建（空间/权限）");
    struct Guard{FILE* f;std::string path;bool ok=false;~Guard(){if(f)std::fclose(f);if(!ok){std::error_code ec;fs::remove(fs::u8path(path),ec);}}} guard{f,output};
    uint8_t blank[44]{};if(std::fwrite(blank,1,44,f)!=44)throw std::runtime_error("投放 WAV 头写入失败");
    uint64_t skipped=0,latency=0,inputFrames=0,dataBytes=0;
    auto write=[&](const rta::audio::AudioBuffer& a){
        const size_t skip=(size_t)std::min<uint64_t>(a.frames(),latency-skipped);skipped+=skip;
        std::vector<uint8_t> b; b.reserve((a.frames()-skip)*(size_t)a.channels*(result.bits/8));
        for(size_t n=skip;n<a.frames();++n)for(int c=0;c<a.channels;++c){uint32_t v=(uint32_t)a.raw[n*(size_t)a.channels+c];for(int k=0;k<result.bits/8;++k)b.push_back((uint8_t)(v>>(k*8)));}
        if(dataBytes+b.size()>std::min<uint64_t>(p.maxOutputBytes,0xFFFFFFFFull-36))throw std::runtime_error("投放 WAV 超过 RIFF 4GiB 上限，请降低目标格式或拆分曲目");
        std::error_code ec;auto space=fs::space(fs::u8path(output).parent_path().empty()?fs::current_path():fs::u8path(output).parent_path(),ec);
        if(!ec && (space.available<b.size() || space.available-b.size()<p.diskReserveBytes))throw std::runtime_error("投放准备磁盘空间不足（保留 1MiB）");
        if(std::fwrite(b.data(),1,b.size(),f)!=b.size())throw std::runtime_error("投放 WAV 写入失败（磁盘满/权限）");
        dataBytes+=b.size();result.frames+=a.frames()-skip;
    };
    rta::audio::AudioBuffer block;
    while(reader.read(block)) {
        if(cancel.load())throw std::runtime_error("投放准备已取消");
        auto out=result.floating?block:pipeline.process(block);if(inputFrames==0)latency=(uint64_t)std::ceil(pipeline.algorithmic_latency_out_frames());
        inputFrames+=block.frames();write(out);
        if(progress)progress(reader.frames?std::min(.99,(double)inputFrames/reader.frames):0.0,result.note);
    }
    if(cancel.load())throw std::runtime_error("投放准备已取消");
    uint64_t drain=(uint64_t)std::ceil((latency+pipeline.resampler_tail_out_frames()+(pipeline.convolution_tail_out_frames()+pipeline.recording_cleanup_tail_out_frames()))*(double)reader.rate/result.rate)+2;
    while(drain && latency+pipeline.resampler_tail_out_frames()+(pipeline.convolution_tail_out_frames()+pipeline.recording_cleanup_tail_out_frames())>0) {
        if(cancel.load())throw std::runtime_error("投放准备已取消");size_t n=(size_t)std::min<uint64_t>(drain,4096);write(pipeline.drain_chunk(n));drain-=n;
    }
    std::vector<uint8_t> h;h.insert(h.end(),{'R','I','F','F'});rta::audio::detail::wr32(h,(uint32_t)dataBytes+36);h.insert(h.end(),{'W','A','V','E','f','m','t',' '});rta::audio::detail::wr32(h,16);rta::audio::detail::wr16(h,result.floating?3:1);rta::audio::detail::wr16(h,(uint16_t)result.channels);rta::audio::detail::wr32(h,result.rate);rta::audio::detail::wr32(h,result.rate*result.channels*(result.bits/8));rta::audio::detail::wr16(h,(uint16_t)(result.channels*(result.bits/8)));rta::audio::detail::wr16(h,(uint16_t)result.bits);h.insert(h.end(),{'d','a','t','a'});rta::audio::detail::wr32(h,(uint32_t)dataBytes);
    if(std::fseek(f,0,SEEK_SET)||std::fwrite(h.data(),1,h.size(),f)!=h.size()||std::fflush(f))throw std::runtime_error("投放 WAV 完成写入失败");
    if(const auto* observed=pipeline.prepared_recording_cleanup_stats()){
        result.cleanupApplied=observed->spectralFrames>0;result.cleanupMeanReductionDb=observed->meanReductionDb;result.cleanupModifiedBinFraction=observed->modifiedBinFraction;
        result.cleanupAutoState=observed->autoState;result.cleanupAutoEstimateValid=observed->autoEstimateValid;
        result.cleanupEstimatedNoiseFloorDbfs=observed->estimatedNoiseFloorDbfs;result.cleanupAppliedNoiseFloorDbfs=observed->appliedNoiseFloorDbfs;
        result.cleanupAutoEvidenceCoverage=observed->autoEvidenceCoverage;result.cleanupAutoLearningFrames=observed->autoLearningFrames;
        result.cleanupAutoRejectedMusicFrames=observed->autoRejectedMusicFrames;result.cleanupAutoFrozenForDrain=observed->autoFrozenForDrain;
    }
    guard.ok=true;if(progress)progress(1.0,result.note);return result;
}

class CastManager {
public:
    static CastManager& instance() {
        static CastManager s_inst;
        return s_inst;
    }

    CastManager() = default;
    ~CastManager() {
        stop();
    }

    CastManager(const CastManager&) = delete;
    CastManager& operator=(const CastManager&) = delete;

    std::vector<UpnpDevice> get_devices(bool forceRescan = false) {
        std::lock_guard<std::mutex> lk(devMutex_);
        auto now = std::chrono::steady_clock::now();
        double ageSec = std::chrono::duration<double>(now - lastScanTime_).count();
        if (forceRescan || cachedDevices_.empty() || ageSec > 15.0) {
            cachedDevices_ = upnp_discover(1200);
            lastScanTime_ = std::chrono::steady_clock::now();
        }
        return cachedDevices_;
    }

    CastState get_state() {
        std::lock_guard<std::mutex> lk(mtx_);
        return state_;
    }

    // 2026-10-29：投放链路原先一行日志都没有 —— 用户报"绝大多数失败"时，
    // rlplayer-playback.log 里查不到任何 CAST 记录，只能靠猜。
    // 主控在 rtaweb 启动处接一行即可把 CAST 全流程写进同一份播放日志：
    //   rta::net::CastManager::instance().set_logger([](const std::string& s) { play_log(s); });
    // 没接也不崩（退回 stderr）。
    void set_logger(std::function<void(const std::string&)> fn) {
        std::lock_guard<std::mutex> lk(logMx_);
        logger_ = std::move(fn);
    }
    void log(const std::string& s) {
        std::function<void(const std::string&)> fn;
        {
            std::lock_guard<std::mutex> lk(logMx_);
            fn = logger_;
        }
        if (fn) fn("CAST " + s);
        else std::fprintf(stderr, "[CAST] %s\n", s.c_str());
    }

    CastStartResult start(const CastStartParams& p) {
        if (p.cueStart >= 0.0 || p.cueEnd >= 0.0) return {false,"UPnP 暂不支持 CUE 分轨；拒绝播放整个镜像"};
        if (p.bits != 0 && p.bits != 16 && p.bits != 24 && p.bits != 32) return {false,"投放位深仅支持 16/24/32"};
        if (p.file.empty()) {
            return {false, "未指定音频文件路径"};
        }

        std::string ext,physical;bool sacdImage=false;
        try {
            physical=rta::audio::image_track_key(p.file).physical;
            ext=cast_lower_str(rta::audio::lowerExt(p.file));
            if(ext==".iso")sacdImage=cast_is_sacd_image(p.file);
        }catch(const std::exception&e){return {false,std::string("音频镜像曲目无效: ")+e.what()};}
        bool isDsd = (ext == ".dsf" || ext == ".dff" || sacdImage);
        if(isDsd&&p.cleanup)return {false,"DSD DoP 逐位供源不经过 PCM 清理，请关闭录音清理或切到本地 PCM 输出"};
        if (isDsd && !p.dop) {
            return {false, "源文件为 DSD 格式 (" + p.file + ")。未开启 DoP 模式，为保证音质拒绝转码为 PCM 降质输出（请勾选 DoP）"};
        }

        std::error_code ec;
        if (!fs::exists(fs::u8path(physical), ec) || ec) {
            return {false, "音频文件不存在: " + p.file};
        }

        std::lock_guard<std::mutex> lk(mtx_);
        if (state_.running) {
            return {false, "已有投放会话正在进行中，请先停止"};
        }

        if (workerThread_.joinable()) {
            workerThread_.join();
        }

        stopReq_.store(false);
        pauseReq_.store(false);
        seekReq_.store(-1.0);
        state_.naturalEnd = false;state_.cleanupApplied=false;state_.cleanupMeanReductionDb=state_.cleanupModifiedBinFraction=0;
        state_.cleanupAutoState=p.cleanup&&p.cleanupAuto?rta::dsp::RecordingCleanupAutoState::Learning:rta::dsp::RecordingCleanupAutoState::Manual;
        state_.cleanupAutoEstimateValid=state_.cleanupAutoFrozenForDrain=false;
        state_.cleanupEstimatedNoiseFloorDbfs=state_.cleanupAppliedNoiseFloorDbfs=-144;
        state_.cleanupAutoEvidenceCoverage=0;state_.cleanupAutoLearningFrames=state_.cleanupAutoRejectedMusicFrames=0;
        state_.prepareProgress = 0;state_.prepareNote.clear();
        state_.running = true;
        state_.device = p.dev;
        state_.sourceFile = p.file;
        state_.servePort = p.port ? p.port : 8912;
        state_.phase = "discovering";
        state_.positionSec = 0.0;
        state_.transportState = "STOPPED";
        state_.servedBytes = 0;
        state_.requestCount = 0;
        state_.rateMatch = "unknown";
        state_.durationSec = 0.0;
        state_.sampleRate = 0;
        state_.bits = 0;
        state_.paused = false;
        state_.warning.clear();
        state_.error.clear();
        state_.bitPerfect = "unknown";
        state_.bitPerfectWhy.clear();
        state_.softwareBitPerfect = false;
        state_.serveFormat.clear();
        state_.sinkSummary.clear();
        state_.servedUrl.clear();

        workerThread_ = std::thread([this, p]() {
            run_worker(p);
        });

        return {true, ""};
    }

    // 播放器控制映射（后端 = UPnP 时 /api/pause、/api/seek 转交给投放会话）：
    // HTTP 线程只写原子量，真正的 SOAP 动作在 worker 的轮询循环里做（不在请求线程里阻塞）。
    void set_paused(bool on) { pauseReq_.store(on); }
    void seek_seconds(double sec) { seekReq_.store(sec < 0.0 ? -1.0 : sec); }

    bool stop() {
        stopReq_.store(true);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        std::lock_guard<std::mutex> lk(mtx_);
        state_.running = false;
        state_.phase = "stopped";
        state_.transportState = "STOPPED";
        state_.paused = false;
        return true;
    }

private:
    void run_worker(const CastStartParams& p) {
        std::string tempWavPath;
        bool needCleanupTemp = false;
        std::string wavToServe = p.file;
        AudioSourceFormat srcFormat;
        double durationSec = 0.0;   // 供播放器进度条用（源文件时长，秒）
        int64_t serveBytes = 0;     // 供源文件字节数（res@size 与"是否播完"的判据）
        // 软件侧判定：解码结果是否**原封不动**交给渲染器（无重采样/无位深转换/无音量/无 EQ/无抖动）
        bool softwareBitExact = true;
        std::string softwareWhy = "逐位直通（解码后按源采样率/位深/声道写 WAV，未做重采样/音量/EQ/抖动）";
        std::string serveFormat;
        std::string preWarn;        // 供源前的已知风险（会写进 state_.warning，让用户先看到）

        auto fail = [this, &tempWavPath, &needCleanupTemp](const std::string& err) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                state_.running = false;
                state_.phase = "error";
                state_.error = err;
                // 会话失败：bit-perfect 结论必须退回 unknown（"设备没开机"不等于"链路非 bit-perfect"），
                // 但把失败原因写进 why，前端 tooltip 能直接看到。
                if (state_.bitPerfect != "no") {
                    state_.bitPerfect = "unknown";
                    state_.bitPerfectWhy = err;
                }
                if (needCleanupTemp && !tempWavPath.empty()) {
                    std::error_code ec;fs::remove(fs::u8path(tempWavPath),ec);
                }
            }
            log("FAIL " + err);
        };

        // 1) 发现渲染器
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.phase = "discovering";
        }

        UpnpDevice targetDev;
        bool foundDev = false;

        if (!p.dev.empty() && (p.dev.rfind("http://", 0) == 0 || p.dev.rfind("https://", 0) == 0)) {
            targetDev.location = p.dev;
            const size_t sp = p.dev.find("//");
            if (sp != std::string::npos) {
                const size_t slash = p.dev.find('/', sp + 2);
                std::string hostport = p.dev.substr(sp + 2, (slash == std::string::npos ? p.dev.size() : slash) - (sp + 2));
                const std::string path = (slash == std::string::npos) ? "/" : p.dev.substr(slash);
                const size_t colon = hostport.find(':');
                targetDev.host = (colon == std::string::npos) ? hostport : hostport.substr(0, colon);
                targetDev.port = (colon == std::string::npos) ? 80 : std::atoi(hostport.substr(colon + 1).c_str());

                std::string xml;
                if (upnp_detail::http_get(targetDev.host, targetDev.port, path, xml)) {
                    targetDev.name = upnp_detail::xml_tag(xml, "friendlyName");
                    targetDev.model = upnp_detail::xml_tag(xml, "modelName");
                    targetDev.udn = upnp_detail::xml_tag(xml, "UDN");
                    targetDev.control = upnp_detail::extract_service_control_url(xml, "AVTransport");
                    if (targetDev.control.empty()) targetDev.control = upnp_detail::xml_tag(xml, "controlURL");
                    targetDev.controlConnection = upnp_detail::extract_service_control_url(xml, "ConnectionManager");
                    foundDev = true;
                }
            }
            if (!foundDev) {
                fail("无法从指定设备描述 URL 获取描述: " + p.dev);
                return;
            }
        } else {
            auto devs = upnp_discover(2000);
            if (devs.empty()) {
                fail("局域网内未发现任何 UPnP 渲染器（请检查设备是否开机与网络配置）");
                return;
            }
            if (!p.dev.empty()) {
                std::string query = cast_lower_str(p.dev);
                for (const auto& d : devs) {
                    if (cast_lower_str(d.name).find(query) != std::string::npos ||
                        cast_lower_str(d.model).find(query) != std::string::npos ||
                        d.host.find(query) != std::string::npos) {
                        targetDev = d;
                        foundDev = true;
                        break;
                    }
                }
                if (!foundDev) {
                    fail("未找到名称/型号包含 "" + p.dev + "" 的 UPnP 渲染器");
                    return;
                }
            } else {
                targetDev = devs[0];
                foundDev = true;
            }
        }

        if (stopReq_.load()) {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = "stopped";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.device = targetDev.name.empty() ? targetDev.host : targetDev.name;
            state_.phase = "serving";
        }

        UpnpControlClient client(targetDev.av_transport_url(), targetDev.connection_manager_url());
        log("DEVICE " + (targetDev.name.empty() ? targetDev.host : targetDev.name) +
            " model=" + targetDev.model + " av=" + client.av_transport_url() +
            " cm=" + client.connection_manager_url());

        // 4a) 先问渲染器"你到底收什么"（ConnectionManager.GetProtocolInfo）。
        //     放在 SetAVTransportURI 之前：① 能在投之前就告诉用户"它不支持这个格式"；
        //     ② bit-perfect 判定只认这份 Sink，不再拿我们自己发过去的 DIDL 回显当证据。
        UpnpProtocolInfo proto = client.get_protocol_info();
        UpnpSinkCaps caps = cast_parse_sink_caps(proto.ok ? proto.sink : std::string());
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.sinkSummary = caps.ok ? caps.summary() : std::string("(GetProtocolInfo 无响应)");
        }
        log(std::string("SINK ") + (proto.ok ? "ok" : "FAIL") + " " +
            (caps.ok ? caps.summary() : std::string("(空)")) +
            (proto.ok ? "" : (" err=" + proto.error.str())));

        // 2) 准备供源音频
        std::string ext = cast_lower_str(rta::audio::lowerExt(p.file));
        bool sacdImage=false;
        try {if(ext==".iso")sacdImage=cast_is_sacd_image(p.file);}
        catch(const std::exception&e){fail(e.what());return;}
        bool isDsd = (ext == ".dsf" || ext == ".dff" || sacdImage);
        if(sacdImage) {
            try {
                {std::lock_guard<std::mutex>lk(mtx_);state_.phase="preparing";}
                tempWavPath=make_cast_temp_wav_path("sacd-dop");needCleanupTemp=true;
                auto prepared=cast_prepare_sacd_dop(p,tempWavPath,stopReq_,[this](double v,const std::string&note){std::lock_guard<std::mutex>lk(mtx_);state_.prepareProgress=v;state_.prepareNote=note;});
                wavToServe=tempWavPath;srcFormat.sampleRate=prepared.rate;srcFormat.bits=prepared.bits;srcFormat.channels=prepared.channels;srcFormat.isDsd=true;srcFormat.encoding="DSD over PCM (DoP v1.1)";
                durationSec=double(prepared.frames)/prepared.rate;softwareWhy=prepared.note;
                serveFormat="WAV 24bit/"+std::to_string(prepared.rate)+"Hz/"+std::to_string(prepared.channels)+"ch（源 SACD DSD 以 DoP v1.1 打包）";
            }catch(const std::exception&e){if(stopReq_.load()){std::lock_guard<std::mutex>lk(mtx_);state_.running=false;state_.phase="stopped";return;}fail(std::string("SACD DoP 准备失败: ")+e.what());return;}
        }else if (isDsd) {
            try {
                rta::audio::DsdStream dsd = rta::audio::read_dsd(p.file);
                if (dsd.rate == 0 || dsd.channels == 0 || dsd.data.empty()) {
                    fail("DSD 文件读取失败或格式无效: " + p.file);
                    return;
                }
                const uint32_t carrierRate = dsd.rate / 16u;
                const size_t carrierFrames = (size_t)(dsd.bits / 16u);

                rta::audio::AudioBuffer dopBuf;
                dopBuf.rate = carrierRate;
                dopBuf.channels = dsd.channels;
                dopBuf.bits = 24;
                dopBuf.isFloat = false;
                dopBuf.raw.resize(carrierFrames * (size_t)dsd.channels);

                for (int c = 0; c < dsd.channels; ++c) {
                    bool phaseA = true;
                    for (size_t n = 0; n < carrierFrames; ++n) {
                        uint8_t b0 = dsd.data[(size_t)c][n * 2 + 0];
                        uint8_t b1 = dsd.data[(size_t)c][n * 2 + 1];
                        uint32_t bits16 = ((uint32_t)b0 << 8) | (uint32_t)b1;
                        uint32_t marker = (phaseA ? rta::dsp::DOP_MARKER_A : rta::dsp::DOP_MARKER_B) << 16;
                        int32_t v = (int32_t)(marker | bits16);
                        if (v & 0x800000) v |= (int32_t)0xFF000000;
                        dopBuf.raw[n * (size_t)dsd.channels + (size_t)c] = v;
                        phaseA = !phaseA;
                    }
                }

                tempWavPath = make_cast_temp_wav_path("dop");
                rta::audio::write_wav(tempWavPath, dopBuf);
                wavToServe = tempWavPath;
                needCleanupTemp = true;

                srcFormat.sampleRate = carrierRate;
                srcFormat.bits = 24;
                srcFormat.channels = dsd.channels;
                srcFormat.isDsd = true;
                srcFormat.encoding = "DSD over PCM (DoP v1.1)";
                durationSec = (double)dsd.bits / (double)dsd.rate;
                serveFormat = "WAV 24bit/" + std::to_string(carrierRate) + "Hz/" +
                              std::to_string(dsd.channels) + "ch（源 DSD 以 DoP v1.1 打包）";
                softwareWhy = "DoP v1.1 打包：载波逐位无损（未做 PCM 转换）";
            } catch (const std::exception& e) {
                fail(std::string("DSD 读取/打包失败: ") + e.what());
                return;
            }
        } else {
            rta::audio::WavHeaderInfo hi; std::string whyNot;
            if (!p.upsample && !p.cleanup && p.rate == 0 && p.bits == 0 &&
                (ext == ".wav" || ext == ".wave") && cast_wav_can_serve_raw(p.file,hi,whyNot)) {
                srcFormat.sampleRate=hi.rate;srcFormat.bits=hi.bits;srcFormat.channels=hi.channels;
                srcFormat.encoding="Linear PCM";
                durationSec=hi.byteRate?(double)hi.dataDeclaredBytes/hi.byteRate:0;
                softwareWhy="源 WAV 原样供源（未解码、未改写，字节级一致）";
            } else {
                try {
                    {std::lock_guard<std::mutex> lk(mtx_);state_.phase="preparing";}
                    tempWavPath=make_cast_temp_wav_path("pcm");needCleanupTemp=true;
                    const auto prepared=cast_prepare_pcm(p,tempWavPath,stopReq_,caps,[this](double v,const std::string& note){std::lock_guard<std::mutex> lk(mtx_);state_.prepareProgress=v;state_.prepareNote=note;});
                    wavToServe=tempWavPath;srcFormat.sampleRate=prepared.rate;srcFormat.bits=prepared.bits;
                    srcFormat.channels=prepared.channels;srcFormat.encoding=prepared.floating?"IEEE Float PCM":"Linear PCM";
                    durationSec=prepared.rate?(double)prepared.frames/prepared.rate:0;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);state_.cleanupApplied=prepared.cleanupApplied;state_.cleanupMeanReductionDb=prepared.cleanupMeanReductionDb;state_.cleanupModifiedBinFraction=prepared.cleanupModifiedBinFraction;
                        state_.cleanupAutoState=prepared.cleanupAutoState;state_.cleanupAutoEstimateValid=prepared.cleanupAutoEstimateValid;
                        state_.cleanupEstimatedNoiseFloorDbfs=prepared.cleanupEstimatedNoiseFloorDbfs;state_.cleanupAppliedNoiseFloorDbfs=prepared.cleanupAppliedNoiseFloorDbfs;
                        state_.cleanupAutoEvidenceCoverage=prepared.cleanupAutoEvidenceCoverage;state_.cleanupAutoLearningFrames=prepared.cleanupAutoLearningFrames;
                        state_.cleanupAutoRejectedMusicFrames=prepared.cleanupAutoRejectedMusicFrames;state_.cleanupAutoFrozenForDrain=prepared.cleanupAutoFrozenForDrain;
                    }
                    softwareBitExact=!prepared.changed;softwareWhy=prepared.note;
                    if(!softwareBitExact)softwareWhy+="；生产 Pipeline 已处理，不是逐位直通";
                } catch(const std::exception& e) {if(stopReq_.load()){std::lock_guard<std::mutex> lk(mtx_);state_.running=false;state_.phase="stopped";return;}fail(std::string("投放准备失败: ")+e.what());return;}
            }
        }

        if (serveFormat.empty()) {
            // 走到这里说明上面三个分支都没有填 serveFormat（.wav 直通分支）
            serveFormat = "WAV " + std::to_string(srcFormat.bits) + "bit/" +
                          std::to_string(srcFormat.sampleRate) + "Hz/" +
                          std::to_string(srcFormat.channels) + "ch（audio/wav）";
        }

        // 供源文件字节数：res@size 用它；"是否播完"也用它
        {
            std::error_code ec2;
            const auto sz = fs::file_size(fs::u8path(wavToServe), ec2);
            if (!ec2) serveBytes = (int64_t)sz;
        }

        {
            // 供源格式与时长：播放器底栏（/api/status）在 UPnP 后端下直接取这份数据
            std::lock_guard<std::mutex> lk(mtx_);
            state_.sampleRate = srcFormat.sampleRate;
            state_.bits = srcFormat.bits;
            state_.durationSec = durationSec > 0.0 ? durationSec : 0.0;
            state_.serveFormat = serveFormat;
            state_.softwareBitPerfect = softwareBitExact;
            if (!preWarn.empty()) state_.warning = preWarn;
            // 软件侧已经确定不是逐位直通 ⟹ 这里就能给 no（不必等渲染器）
            if (!softwareBitExact) {
                state_.bitPerfect = "no";
                state_.bitPerfectWhy = "软件侧已改动：" + softwareWhy;
            } else {
                state_.bitPerfect = "unknown";
                state_.bitPerfectWhy = "软件侧：" + softwareWhy + "；渲染器侧：等待 GetProtocolInfo 与取流证据";
            }
        }
        log("PREP " + serveFormat + " | bytes=" + std::to_string(serveBytes) +
            " | " + softwareWhy);

        if (stopReq_.load()) {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = "stopped";
            state_.paused = false;
            if (needCleanupTemp && !tempWavPath.empty()) fs::remove(fs::u8path(tempWavPath));
            return;
        }

        // 3) 启动本地 HTTP 供源服务
        // 2026-10-24（261024-02 #5 实测）：8912 可能被别的程序占用（本机 Clash/verge-mihomo 就长期监听
        // 0.0.0.0:8912）⟹ 投放直接失败。改为在 8912..8921 里挑第一个能绑上的端口，
        // 实际端口写回 state_.servePort（UI 与取流 URL 都以 state 为准，不假设 8912）。
        const uint16_t wantPort = p.port ? p.port : 8912;
        UpnpHttpServer server;
        uint16_t servePort = wantPort;
        bool bound = false;
        // 回环渲染器（测试/本机）只绑 127.0.0.1：既不给外部网卡开端口，也不会触发防火墙弹窗。
        const bool loopbackDev = p.dev.find("127.0.0.1") != std::string::npos ||
                                 p.dev.find("localhost") != std::string::npos ||
                                 p.dev.find("[::1]") != std::string::npos;
        const char* bindIp = loopbackDev ? "127.0.0.1" : "0.0.0.0";
        for (uint16_t i = 0; i < 10; ++i) {
            servePort = (uint16_t)(wantPort + i);
            if (server.start(servePort, bindIp)) { bound = true; break; }
            if (stopReq_.load()) break;
        }
        if (!bound) {
            fail(std::string("无法在 ") + bindIp + ":" + std::to_string(wantPort) + "–" + std::to_string((int)wantPort + 9) +
                 " 启动 HTTP 供源服务（端口全被占用）");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.servePort = servePort;
        }

        // ★ 2026-10-29：供源前先自检"这个文件我们真的打得开吗"。
        // 旧实现要等渲染器发来 GET 才发现打不开（返回 404），用户看到的是"播两秒自动停"。
        {
            const std::string rdErr = UpnpHttpServer::check_file_readable(wavToServe);
            if (!rdErr.empty()) {
                server.stop();
                fail("供源文件不可读：" + rdErr +
                     "（WAV 直通路径曾因窄 fopen 打开 UTF-8 中文路径失败 —— 已改为宽字符打开，若仍报此错请看权限/占用）");
                return;
            }
        }

        std::string localIp = upnp_local_ip_for(targetDev.host, (uint16_t)targetDev.port);
        std::string streamPath;
        try {
            streamPath = server.register_file(wavToServe, "audio/wav");
        } catch (const std::exception& e) {
            server.stop();
            fail(std::string("Secure stream token generation failed: ") + e.what());
            return;
        }
        std::string streamUrl = "http://" + localIp + ":" + std::to_string(server.port()) + streamPath;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.servedUrl = streamUrl;
        }
        log("SERVE url=" + streamUrl + " file=" + wavToServe + " port=" + std::to_string(server.port()));

        // 4) UPnP 控制动作 SetAVTransportURI & Play

        if (caps.ok) {
            const auto ans = caps.match_stream(srcFormat.sampleRate, srcFormat.bits, srcFormat.channels);
            if (ans == UpnpSinkCaps::Answer::No) {
                const std::string w = "渲染器 Sink 未声明支持 " + serveFormat + "（Sink: " + caps.summary() +
                                      "）⟹ 它大概率会拒收或自行转换；若无声请换设备或降低源格式。";
                preWarn = preWarn.empty() ? w : (preWarn + " " + w);
                std::lock_guard<std::mutex> lk(mtx_);
                state_.bitPerfect = "no";
                state_.bitPerfectWhy = w;
                log("PREFLIGHT_MISMATCH " + w);
            }
        }

        UpnpDidlItem didl;
        didl.title = p.file;
        size_t lastSlash = didl.title.find_last_of("\\/");
        if (lastSlash != std::string::npos) didl.title = didl.title.substr(lastSlash + 1);
        didl.creator = "RLPlayer Cast";
        didl.uri = streamUrl;
        // 线上字节 = WAV ⟹ protocolInfo 必须与之一致（不是把 FLAC 谎称成 WAV，而是我们**已经**解码成 WAV）。
        didl.protocolInfo = "http-get:*:audio/wav:*";
        didl.sampleRate = srcFormat.sampleRate;
        didl.bits = srcFormat.bits;
        didl.channels = srcFormat.channels;
        didl.durationSec = durationSec;      // 旧实现从没填过 ⟹ 部分渲染器无法拖动/无法预算缓冲
        didl.sizeBytes = serveBytes;
        std::string didlXml = build_didl_lite(didl);
        log("DIDL " + didlXml.substr(0, 400));

        UpnpActionResult setRes = client.set_av_transport_uri(streamUrl, didlXml);
        if (!setRes.ok) {
            server.stop();
            // ★ 绝不吞 SOAP Fault：UPnPError 的 code 与 description 原样进 state_.error（前端可读）
            fail("SetAVTransportURI 失败: " + setRes.error.str() +
                 "（HTTP " + std::to_string(setRes.httpStatus) + "；URI=" + streamUrl + "）");
            return;
        }

        UpnpActionResult playRes = client.play("1");
        if (!playRes.ok) {
            client.stop();
            server.stop();
            fail("Play 失败: " + playRes.error.str() + "（HTTP " + std::to_string(playRes.httpStatus) + "）");
            return;
        }
        log("PLAY ok");

        // 5) 轮询与状态更新
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.phase = "playing";
            state_.transportState = "PLAYING";
        }

        auto startTime = std::chrono::steady_clock::now();
        bool pausedApplied = false;
        std::string lastUpnpErr;    // 最近一次 SOAP 失败的 UPnPError 原文（收尾时拼进错误里）
        std::string stopReason;     // 退出轮询的原因；空 = 正常结束
        int transportFailStreak = 0;
        bool naturalEnd = false;

        while (!stopReq_.load()) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            if (p.seconds > 0.0 && elapsed >= p.seconds) {
                break;
            }

            // 261024-01 #9：暂停 / 跳转请求（来自播放器底栏）。HTTP 线程只写原子量，
            // SOAP 动作在这里做 —— 请求线程不会因为渲染器慢响应而卡住整个 Web UI。
            const bool wantPause = pauseReq_.load();
            if (wantPause != pausedApplied) {
                const UpnpActionResult pr = wantPause ? client.pause() : client.play("1");
                if (pr.ok) {
                    pausedApplied = wantPause;
                    std::lock_guard<std::mutex> lk(mtx_);
                    state_.paused = wantPause;
                }
            }
            {
                const double seekSec = seekReq_.exchange(-1.0);
                if (seekSec >= 0.0) {
                    char hms[32]{};
                    const int total = (int)(seekSec + 0.5);
                    std::snprintf(hms, sizeof(hms), "%02d:%02d:%02d",
                                  total / 3600, (total % 3600) / 60, total % 60);
                    const UpnpActionResult sr = client.seek_rel_time(hms);
                    if (sr.ok) {
                        std::lock_guard<std::mutex> lk(mtx_);
                        state_.positionSec = seekSec;
                    }
                }
            }

            UpnpTransportInfo transport = client.get_transport_info();
            UpnpPositionInfo pos = client.get_position_info();
            UpnpMediaInfo media = client.get_media_info();
            (void)media;   // 回显的 DIDL 是我们自己发过去的，拿它当"渲染器能力"是自证（见 upnp_control.h 注释）

            if (!transport.ok && transport.error.has_error()) lastUpnpErr = transport.error.str();
            if (!pos.ok && pos.error.has_error()) lastUpnpErr = pos.error.str();
            transportFailStreak = transport.ok ? 0 : (transportFailStreak + 1);
            if(transport.ok && !transport.status.empty() && transport.status!="OK") {
                stopReason="渲染器报告播放错误: "+transport.status;
                log("TRANSPORT_ERROR "+stopReason);break;
            }

            const bool fetched = server.total_requests_served() > 0;
            const bool playingNow = (transport.state == "PLAYING" || transport.state == "PAUSED_PLAYBACK");

            // ★ 2026-10-29：bit-perfect 真实探测（只认 Sink + 软件侧逐位 + 取流/在播证据）。
            //   旧实现是 rtaweb 里硬编码 ps.bpState="no"，与链路无关 —— 用户看到的"非 bit-perfect"是假的。
            BitPerfectInput bpIn;
            bpIn.src = srcFormat;
            bpIn.caps = caps;
            bpIn.softwareBitExact = softwareBitExact;
            bpIn.softwareWhy = softwareWhy;
            bpIn.dop = (srcFormat.isDsd && isDsd);
            bpIn.fetched = fetched;
            bpIn.playing = playingNow;
            const BitPerfectVerdict bp = judge_upnp_bit_perfect(bpIn);

            const int64_t served = server.total_bytes_served();
            const int reqs = (int)server.total_requests_served();
            const bool complete = (serveBytes > 0 && served >= serveBytes);

            {
                std::lock_guard<std::mutex> lk(mtx_);
                // 旧实现在 SOAP 失败时仍显示 "PLAYING"（撒谎）。现在只在真有答案时更新。
                if (transport.ok && !transport.state.empty()) state_.transportState = transport.state;
                state_.positionSec = parse_time_hh_mm_ss(pos.relTime);
                state_.servedBytes = served;
                state_.requestCount = reqs;
                state_.rateMatch = (bp.state == BitPerfectState::Yes) ? "match"
                                  : (bp.state == BitPerfectState::No) ? "mismatch" : "unknown";
                state_.bitPerfect = bp.str();
                state_.bitPerfectWhy = bp.why;
                // 渲染器自己"暂停"了（用户没按暂停）—— 这正是现场说的"播放会自动暂停"。
                // 旧实现 paused 只看用户请求，渲染器进 PAUSED_PLAYBACK 时界面还显示在播。
                if (transport.ok && transport.state == "PAUSED_PLAYBACK") {
                    state_.paused = true;
                } else if (transport.ok && transport.state == "PLAYING") {
                    state_.paused = false;
                }
                // warning 不再被每一轮覆盖（旧实现会把"防火墙/格式"提示冲掉）
                std::string warn = preWarn;
                if (transport.ok && transport.state == "PAUSED_PLAYBACK" && !pauseReq_.load()) {
                    warn += (warn.empty() ? "" : " ");
                    warn += "渲染器自行进入 PAUSED_PLAYBACK（不是本机发起的暂停）：常见于取流中断、"
                            "缓冲不足或被设备判定为不支持的格式。";
                }
                if (!caps.ok && elapsed > 3.0) {
                    warn += (warn.empty() ? "" : " ");
                    warn += "渲染器没有返回 GetProtocolInfo 能力（部分老设备如此），无法判断它是否原样接收。";
                }
                if (elapsed > 8.0 && reqs == 0) {
                    warn += (warn.empty() ? "" : " ");
                    warn += "渲染器没有来取流：请检查 Windows 防火墙是否允许 rtaweb.exe 入站 TCP " +
                            std::to_string(server.port()) + "，或设备是否支持该格式";
                }
                if (server.not_found_count() > 0) {
                    warn += (warn.empty() ? "" : " ");
                    warn += "渲染器取流收到了 " + std::to_string(server.not_found_count()) +
                            " 次 404：" + server.last_error();
                }
                state_.warning = warn;
            }

            // 结束判定：
            //   · 供源字节基本发完 ⟹ 正常播完，不算失败；
            //   · 设备回 STOPPED/NO_MEDIA_PRESENT 但流没发完 ⟹ **必须**给出可读原因，
            //     绝不静默"自动暂停"（旧实现就是直接 break，界面只剩一个播放键变回"播放"）。
            if (transport.ok && (transport.state == "STOPPED" || transport.state == "NO_MEDIA_PRESENT")) {
                const double grace = fetched ? 2.0 : 8.0;   // 没取过流的多给几秒（老设备起播慢）
                if (elapsed > grace) {
                    if (!complete) {
                        std::string why = "渲染器在 " + std::to_string((int)elapsed) + " 秒后报告 " +
                                          transport.state + "，但只取走 " + std::to_string(served) + "/" +
                                          std::to_string(serveBytes) + " 字节";
                        if (reqs == 0) {
                            why += "（一次都没来取流：检查 Windows 防火墙是否放行入站 TCP " +
                                   std::to_string(server.port()) + "，或设备不支持该格式）";
                        } else if (server.not_found_count() > 0) {
                            why += "（取流被 404 拒绝：" + server.last_error() + "）";
                        } else {
                            why += "（流发到一半被设备中断：常见原因是格式/采样率/声道数不被支持、"
                                   "DoP 不被识别、或设备侧解码出错）";
                        }
                        if (!lastUpnpErr.empty()) why += "；最近 UPnP 错误: " + lastUpnpErr;
                        stopReason = why;
                        log("STOPPED_EARLY " + why);
                    }
                    naturalEnd=complete && fetched && stopReason.empty();
                    break;
                }
            }
            // 与渲染器彻底失联（连续 20 次 = 约 10 秒拿不到任何状态）：不要假装还在播
            if (transportFailStreak >= 20) {
                stopReason = "与渲染器失联（连续 20 次 GetTransportInfo 失败）" +
                             (lastUpnpErr.empty() ? std::string() : "；最近 UPnP 错误: " + lastUpnpErr);
                log("LOST " + stopReason);
                break;
            }

            for (int i = 0; i < 5; ++i) {
                if (stopReq_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // 6) 结束收尾
        log(std::string("TEARDOWN ") + (stopReason.empty() ? "ok" : ("error=" + stopReason)) +
            " served=" + std::to_string(server.total_bytes_served()) +
            " reqs=" + std::to_string(server.total_requests_served()) +
            " 404s=" + std::to_string(server.not_found_count()));
        client.stop();
        server.stop();
        if (needCleanupTemp && !tempWavPath.empty()) {
            fs::remove(fs::u8path(tempWavPath));
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.running = false;
            state_.phase = stopReason.empty() ? "stopped" : "error";
            state_.naturalEnd = naturalEnd && !stopReq_.load();
            state_.transportState = "STOPPED";
            state_.paused = false;
            if (!stopReason.empty()) {
                state_.error = stopReason;
                // 会话失败时不能留着 "yes"（那会变成"bit-perfect 但它没响"）
                if (state_.bitPerfect == "yes") state_.bitPerfect = "unknown";
                state_.bitPerfectWhy = stopReason;
            }
        }
    }

    std::mutex              mtx_;
    CastState               state_;
    std::atomic<bool>       stopReq_{false};
    std::atomic<bool>       pauseReq_{false};
    std::atomic<double>     seekReq_{-1.0};
    std::thread             workerThread_;

    std::mutex              logMx_;
    std::function<void(const std::string&)> logger_;

    std::mutex              devMutex_;
    std::vector<UpnpDevice> cachedDevices_;
    std::chrono::steady_clock::time_point lastScanTime_;
};

} // namespace rta::net
