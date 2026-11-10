// Offline WAV -> actual Pipeline -> true AHM 64/256-level quantizer -> v1 file.
// No COM/audio device or backend is opened. This is not a DAC/DSD/DoP transport.
#include "audio/multilevel_file.h"
#include "audio/multilevel_wav_stream.h"
#include "engine/pipeline.h"
#include "dsp/modulator.h"
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <optional>
#include <shellapi.h>
#include <thread>
#pragma comment(lib,"shell32.lib")
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
namespace ml=rta::audio::multilevel;
static std::atomic_bool cancelled{false};
static BOOL WINAPI console_cancel(DWORD code){if(code==CTRL_C_EVENT||code==CTRL_BREAK_EVENT||code==CTRL_CLOSE_EVENT){cancelled=true;return TRUE;}return FALSE;}
struct Cancelled{};
struct Watchdog {
 std::atomic_bool done{false};std::thread worker;
 explicit Watchdog(std::optional<uint64_t> milliseconds){if(milliseconds)worker=std::thread([this,limit=*milliseconds]{const auto start=Clock::now();while(!done.load()){if(std::chrono::duration<double,std::milli>(Clock::now()-start).count()>=limit){cancelled=true;return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}});}
 ~Watchdog(){done=true;if(worker.joinable())worker.join();}
};
static std::vector<std::string> utf8_arguments(){
 int count=0;LPWSTR* values=CommandLineToArgvW(GetCommandLineW(),&count);if(!values)throw std::runtime_error("cannot read native Windows arguments");
 struct Release {LPWSTR* p;~Release(){LocalFree(p);}} release{values};std::vector<std::string> out;
 for(int i=0;i<count;++i){const int length=int(wcslen(values[i]));int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,values[i],length,nullptr,0,nullptr,nullptr);if(!bytes&&length)throw std::invalid_argument("invalid UTF-16 command argument");std::string value(bytes,'\0');if(bytes&&!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,values[i],length,value.data(),bytes,nullptr,nullptr))throw std::runtime_error("cannot convert Windows argument to UTF-8");out.push_back(std::move(value));}return out;
}
static uint64_t integer(const std::string&s){uint64_t n=0;auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),n);if(e!=std::errc{}||p!=s.data()+s.size())throw std::invalid_argument("invalid unsigned integer: "+s);return n;}
static double number(const std::string&s){size_t used=0;const double n=std::stod(s,&used);if(used!=s.size()||!std::isfinite(n))throw std::invalid_argument("invalid finite number: "+s);return n;}
static std::string q(const std::string&s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else if(c=='\t')out+="\\t";else if(c>=32)out+=c;}return out+'"';}
static bool same_path(const fs::path&a,const fs::path&b){auto x=fs::absolute(a).lexically_normal().wstring(),y=fs::absolute(b).lexically_normal().wstring();return CompareStringOrdinal(x.c_str(),-1,y.c_str(),-1,TRUE)==CSTR_EQUAL;}
static void report_atomic(const fs::path&target,const std::string&text){
 auto tmp=target;tmp+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
 HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot create private report temporary");
 try{DWORD done=0;if(text.size()>MAXDWORD||!WriteFile(h,text.data(),DWORD(text.size()),&done,nullptr)||done!=text.size()||!FlushFileBuffers(h))throw std::runtime_error("report write failed");CloseHandle(h);h=INVALID_HANDLE_VALUE;if(!MoveFileExW(tmp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("report atomic replacement failed");}
 catch(...){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);DeleteFileW(tmp.c_str());throw;}
}
int main(int,char**){
 bool dataCommitted=false;
 try{
  SetConsoleOutputCP(CP_UTF8);SetConsoleCtrlHandler(console_cancel,TRUE);
  auto nativeArgs=utf8_arguments();std::vector<char*> pointers;for(auto& arg:nativeArgs)pointers.push_back(arg.data());const int argc=int(pointers.size());char** argv=pointers.data();
  if(argc==1||(argc==2&&std::string(argv[1])=="--help")){
   std::cout<<"rlmultilevel INPUT.wav --out OUTPUT.rlmultilevel [--rate 2822400] [--bits 6|8] [--order 5|7] [--cutoff 100000] [--filter poly-sinc-short-lp] [--chunk 256] [--report report.json]\n"
            <<"Offline multilevel sample indices only. Not DSD, DoP, DAC transport or private HQP/AHM compatibility.\nInput: bounded-memory RIFF WAV (RF64/RIFX unsupported); rate 8k..768k; channels 1..32; output 44.1/48k families x64/128/256/512/1024. Input decoding streams bounded blocks.\n";return argc==1?2:0;
  }
  fs::path input=fs::u8path(argv[1]),output,report;uint32_t outputRate=0;int bits=8,order=7;double cutoff=100000.;size_t chunk=256;std::string presetName="poly-sinc-short-lp";std::optional<uint64_t> cancelAfter,cancelMs;
  for(int i=2;i<argc;++i){std::string option=argv[i];if(i+1>=argc)throw std::invalid_argument("incomplete option: "+option);const std::string value=argv[++i];
   if(option=="--out")output=fs::u8path(value);else if(option=="--report")report=fs::u8path(value);else if(option=="--rate"){auto n=integer(value);if(!n||n>UINT32_MAX)throw std::invalid_argument("output rate outside uint32 range");outputRate=uint32_t(n);}
   else if(option=="--bits"){auto n=integer(value);if(n!=6&&n!=8)throw std::invalid_argument("only genuine 6/8-bit multilevel output is supported");bits=int(n);}
   else if(option=="--order"){auto n=integer(value);if(n!=5&&n!=7)throw std::invalid_argument("order must be 5 or 7");order=int(n);}
   else if(option=="--cutoff")cutoff=number(value);else if(option=="--filter")presetName=value;
   else if(option=="--chunk"){auto n=integer(value);if(!n||n>4096)throw std::invalid_argument("chunk must be 1..4096 source frames");chunk=size_t(n);}
   else if(option=="--cancel-after-source-frames")cancelAfter=integer(value);
   else if(option=="--cancel-after-ms")cancelMs=integer(value);
   else throw std::invalid_argument("unknown option: "+option);
  }
  if(output.empty()||output.extension()!=L".rlmultilevel")throw std::invalid_argument("--out must name .rlmultilevel, never .dsf/.dff/.wav");
  if(same_path(input,output)||(!report.empty()&&(same_path(input,report)||same_path(output,report))))throw std::invalid_argument("input/data/report paths must be distinct");
  const uint64_t fileBytes=fs::file_size(input);
  const size_t requestedChunk=chunk;
  const auto started=Clock::now();rta::audio::MultilevelWavStream source(input);
  if(!source.frames()||source.channels<1||source.channels>32)throw std::invalid_argument("empty/unsupported WAV input");
  if(source.rate<8000||source.rate>768000)throw std::invalid_argument("source WAV rate must be 8000..768000 Hz");
  if(!outputRate){if(source.rate%44100==0)outputRate=44100*64;else if(source.rate%48000==0)outputRate=48000*64;else throw std::invalid_argument("non-44.1/48k source requires explicit --rate");}
  bool supportedRate=false;for(uint32_t base:{44100u,48000u})for(uint32_t multiple:{64u,128u,256u,512u,1024u})supportedRate|=outputRate==base*multiple;
  if(!supportedRate)throw std::invalid_argument("multilevel output rate must be a 44.1/48k family x64/128/256/512/1024 rate");
  // Bound temporary output planes independently of an extreme rate/channel ratio.
  constexpr uint64_t maxBlockSamples=1u<<20;const uint64_t boundedChunk=maxBlockSamples*source.rate/(uint64_t(outputRate)*source.channels);chunk=std::min(chunk,size_t(std::max<uint64_t>(1,boundedChunk)));
  if(!(cutoff>0&&cutoff<outputRate*.5))throw std::invalid_argument("cutoff must lie strictly below output Nyquist");
  const auto* preset=rta::dsp::find_preset(presetName);if(!preset)throw std::invalid_argument("unknown filter preset");
  // This process never reads a user cache/profile or opens a GPU backend. Cold
  // prepareMs comes from the actual Pipeline, not a reconstructed filter.
  _putenv_s("RLPLAYER_FILTER_CACHE","0");
  rta::engine::PipelineConfig config;config.outRate=outputRate;config.outBits=32;config.sourceBits=source.bits;config.autoTaps=true;config.useFixed=false;config.gpuResample=false;config.forceBitPerfect=false;rta::engine::apply_filter_preset(config,*preset);
  rta::dsp::DesignCancellationScope designCancellation(&cancelled);
  rta::engine::Pipeline pipeline(config);rta::engine::PipelineStats stats;
  std::vector<std::unique_ptr<rta::dsp::SdmModulator>> modulators;
  rta::dsp::SdmConfig sdm;sdm.fs=outputRate;sdm.order=order;sdm.family=rta::dsp::ModulatorFamily::Ahm;sdm.ahmBits=bits;sdm.oneBitOutput=false;sdm.cutoffHz=cutoff;sdm.ditherLsb=.0005;
  for(int c=0;c<source.channels;++c){modulators.push_back(std::make_unique<rta::dsp::SdmModulator>(sdm));if(modulators.back()->levels()!=(1<<bits)||modulators.back()->levels_clamped())throw std::runtime_error("requested multilevel output was downgraded; refusing sign collapse");}
  const auto& actual=*modulators[0];std::unique_ptr<ml::Writer> writer;uint64_t sourceOutput=0,headRemaining=0,headTrim=0,tail=0;double prepareMs=0,firstProcessMs=0;size_t processed=0;
  std::cout<<"OFFLINE_EXPORT: no audio device; genuine AHM "<<(1<<bits)<<" levels; .rlmultilevel is not a DAC/DSD/DoP transport\n";
  auto consume=[&](const std::vector<std::vector<double>>& planes,size_t take){
   if(cancelled.load())throw Cancelled{};if(planes.size()!=size_t(source.channels))throw std::runtime_error("Pipeline channel count mismatch");
   for(const auto& p:planes)if(p.size()<take)throw std::runtime_error("Pipeline returned uneven planes");
   const size_t drop=size_t(std::min<uint64_t>(headRemaining,take));headRemaining-=drop;take-=drop;if(!take)return;
   std::vector<std::vector<double>> values(source.channels,std::vector<double>(take));
   for(int c=0;c<source.channels;++c)modulators[c]->process_block(planes[c].data()+drop,take,[&](size_t i,double y){values[c][i]=y;});
   writer->append_quantized(values);
  };
  Watchdog watchdog(cancelMs);
  while(processed<source.frames()){
   if(cancelled.load()||(cancelAfter&&processed>=*cancelAfter))throw Cancelled{};
   auto part=source.read_frames(chunk);const size_t n=part.frames();if(!n)throw std::runtime_error("source ended before declared frames");
   std::vector<std::vector<double>> planes;const auto before=Clock::now();pipeline.process(part,&stats,&planes);
   if(!writer){
    prepareMs=stats.prepareMs;firstProcessMs=std::chrono::duration<double,std::milli>(Clock::now()-before).count();
    headTrim=uint64_t(std::llround(pipeline.algorithmic_latency_out_frames()));headRemaining=headTrim;tail=uint64_t(std::ceil(pipeline.convolution_tail_out_frames()+pipeline.resampler_tail_out_frames()));
    std::ostringstream meta;meta<<std::setprecision(17)<<"{\"kind\":\"offline-real-AHM-multilevel\",\"not_dac_transport\":true,\"pipeline\":\"actual Pipeline doubleOut before PCM quantization\",\"filter\":"<<q(presetName)<<",\"cold_filter_cache\":\"disabled\",\"prepare_ms\":"<<prepareMs<<",\"first_process_ms\":"<<firstProcessMs<<",\"source_bits\":"<<source.bits<<",\"source_float\":"<<(source.isFloat?"true":"false")<<",\"head_trim_frames\":"<<headTrim<<",\"requested_order\":"<<order<<",\"effective_order\":"<<actual.effectiveOrder()<<",\"sections\":"<<actual.ntf().sections.count<<",\"step_down_reason\":"<<q(actual.stepDownReason())<<",\"requested_levels\":"<<(1<<bits)<<",\"effective_levels\":"<<actual.levels()<<",\"one_bit_output\":false,\"cutoff_hz\":"<<cutoff<<",\"dither_lsb\":"<<sdm.ditherLsb<<"}";
    ml::Format format;format.rate=outputRate;format.source_rate=source.rate;format.channels=source.channels;format.bits=uint16_t(bits);format.requested_order=uint16_t(order);format.effective_order=uint16_t(actual.effectiveOrder());format.requested_levels=uint16_t(1<<bits);format.effective_levels=uint16_t(actual.levels());format.source_frames=source.frames();format.metadata=meta.str();writer=std::make_unique<ml::Writer>(output,format);
   }
   const size_t got=planes.empty()?0:planes[0].size();sourceOutput+=got;consume(planes,got);processed+=n;
  }
  uint64_t drained=0;const uint64_t needed=headTrim+tail;
  while(drained<needed){
   if(cancelled.load())throw Cancelled{};const size_t inputFrames=std::min<size_t>(chunk,std::max<size_t>(1,size_t(std::ceil(double(needed-drained)*source.rate/outputRate))));
   std::vector<std::vector<double>> planes;pipeline.drain_chunk(inputFrames,&planes);const size_t got=planes.empty()?0:planes[0].size();if(!got)throw std::runtime_error("Pipeline ended before complete causal tail drained");const size_t take=size_t(std::min<uint64_t>(got,needed-drained));consume(planes,take);drained+=take;
  }
  if(headRemaining||!sourceOutput||writer->frames()!=sourceOutput+tail)throw std::runtime_error("actual source/trim/tail frame ledger mismatch");
  long long overload=0,clamp=0,nonfinite=0;for(const auto& m:modulators){overload+=m->overload();clamp+=m->clamped();nonfinite+=m->nonFinite();}
  if(nonfinite)throw std::runtime_error("nonfinite quantizer repair occurred; refusing a misleading complete export");
  if(cancelled.load())throw Cancelled{};writer->finalize({sourceOutput,tail},&cancelled);dataCommitted=true;
  const double elapsed=std::chrono::duration<double>(Clock::now()-started).count(),sourceSeconds=double(source.frames())/source.rate;
  std::ostringstream result;result<<std::setprecision(17)<<"{\"schema\":1,\"status\":\"COMPLETE_OFFLINE_MULTILEVEL_EXPORT\",\"audio_device_calls\":0,\"source_rate\":"<<source.rate<<",\"source_file_bytes\":"<<fileBytes<<",\"riff_max_file_bytes\":"<<uint64_t(UINT32_MAX)+8<<",\"requested_chunk_source_frames\":"<<requestedChunk<<",\"effective_chunk_source_frames\":"<<chunk<<",\"source_frames\":"<<source.frames()<<",\"source_duration_seconds\":"<<sourceSeconds<<",\"rate\":"<<outputRate<<",\"channels\":"<<source.channels<<",\"requested_order\":"<<order<<",\"effective_order\":"<<actual.effectiveOrder()<<",\"requested_levels\":"<<(1<<bits)<<",\"effective_levels\":"<<actual.levels()<<",\"one_bit_output\":false,\"sections\":"<<actual.ntf().sections.count<<",\"cutoff_hz\":"<<cutoff<<",\"source_valid_output_frames\":"<<sourceOutput<<",\"head_trim_frames\":"<<headTrim<<",\"pipeline_tail_frames\":"<<tail<<",\"file_frames\":"<<writer->frames()<<",\"cold_prepare_ms\":"<<prepareMs<<",\"first_process_ms\":"<<firstProcessMs<<",\"whole_complete_seconds\":"<<elapsed<<",\"source_realtime_ratio\":"<<sourceSeconds/elapsed<<",\"overload\":"<<overload<<",\"clamped\":"<<clamp<<",\"nonfinite\":"<<nonfinite<<"}\n";
  if(!report.empty())report_atomic(report,result.str());std::cout<<result.str();return 0;
 }catch(const rta::dsp::DesignCancelled&){std::cerr<<"CANCELLED_DESIGN no new complete multilevel file was published\n";return 130;}
 catch(const ml::Cancelled&){std::cerr<<"CANCELLED_COMMIT no new complete multilevel file was published\n";return 130;}
 catch(const Cancelled&){std::cerr<<"CANCELLED no new complete multilevel file was published\n";return 130;}
 catch(const std::exception&e){std::cerr<<"ERROR "<<e.what()<<" data_file_committed="<<(dataCommitted?"true":"false")<<'\n';return 2;}
}
