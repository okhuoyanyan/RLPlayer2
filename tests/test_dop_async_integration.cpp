// The GUI producer ordering seam, with a synchronous whole-output oracle.
// No device, GPU API or timing assertion. Production rtaweb owns queue/epoch.
#include "dsp/dop_async.h"
#include "audio/render_loop.h"
#include <iostream>
#include <thread>
using namespace rta::dsp;
#define REQUIRE(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";return 1;}}while(false)
using Planes=std::vector<std::vector<double>>;
static Planes input(size_t n,size_t position){Planes p(2,std::vector<double>(n));for(size_t c=0;c<2;++c)for(size_t i=0;i<n;++i)p[c][i]=.12*std::sin((position+i)*.013*(c+1));return p;}
static auto mods(){SdmConfig c;c.fs=2822400;c.order=5;c.oneBitOutput=true;c.ditherLsb=.0005;std::vector<std::unique_ptr<SdmModulator>> m;for(int i=0;i<2;++i)m.push_back(std::make_unique<SdmModulator>(c));return m;}
struct Tagged{uint64_t epoch;std::vector<int32_t> words;};
int main(){
 // Use actual production mailbox methods. An earlier consumed request must
 // retain its own epoch even if a second seek arrives during slow discard.
 rta::render::RenderQ rq;
 REQUIRE(rq.consume_seek()==std::make_pair(-1ll,uint64_t(1)));
 const auto firstEpoch=rq.publish_seek(100);const auto first=rq.consume_seek();
 REQUIRE(first==std::make_pair(100ll,firstEpoch));
 const auto secondEpoch=rq.publish_seek(200);REQUIRE(secondEpoch==firstEpoch+1);
 REQUIRE(first==std::make_pair(100ll,firstEpoch));REQUIRE(first.second!=rq.epoch.load());
 REQUIRE(rq.consume_seek()==std::make_pair(200ll,secondEpoch));REQUIRE(rq.consume_seek().first==-1);
 // Coalesce unconsumed seeks but never combine one request's frame with another epoch.
 rq.publish_seek(300);const auto newest=rq.publish_seek(400);REQUIRE(rq.consume_seek()==std::make_pair(400ll,newest));
 rta::render::RenderQ interleave;std::mutex gateMutex;std::condition_variable gateCv;bool captured=false,release=false;
 interleave.publish_seek(100);
 std::thread producer([&]{
  const auto request=interleave.consume_seek();
  {std::unique_lock lock(gateMutex);captured=true;gateCv.notify_all();gateCv.wait(lock,[&]{return release;});}
  rta::audio::AudioBuffer out;out.raw={int32_t(request.first)};
  {std::lock_guard lock(interleave.mtx);interleave.q.emplace_back(std::move(out),request.second);}
 });
 {std::unique_lock lock(gateMutex);gateCv.wait(lock,[&]{return captured;});}
 const auto lateEpoch=interleave.publish_seek(200);
 {std::lock_guard lock(gateMutex);release=true;}gateCv.notify_all();producer.join();
 REQUIRE(interleave.q.size()==1&&interleave.q.front().first.raw[0]==100);
 REQUIRE(interleave.q.front().second==lateEpoch-1&&interleave.q.front().second!=lateEpoch);
 REQUIRE(interleave.consume_seek()==std::make_pair(200ll,lateEpoch));
 rta::render::RenderQ stress;std::atomic<bool> published=false;std::atomic<bool> paired=true;size_t consumed=0;
 std::thread writer([&]{for(long long frame=1;frame<=2000;++frame)stress.publish_seek(frame);published.store(true);});
 do{auto request=stress.consume_seek();if(request.first>=0){++consumed;if(request.second!=uint64_t(request.first+1))paired.store(false);}}while(!published.load());
 writer.join();auto last=stress.consume_seek();if(last.first>=0){++consumed;REQUIRE(last.second==uint64_t(last.first+1));}
 REQUIRE(paired.load()&&consumed>0);
 size_t compared=0;
 for(size_t initialSkip:{0u,3u,39u,200u}){
  auto am=mods(),sm=mods();DopAsyncStream async(am);DopModulationStream sync;
  std::vector<Tagged> queue,oracle;const auto producer=std::this_thread::get_id();rta::render::RenderQ seekState;uint64_t epoch=seekState.epoch.load();bool done=false;size_t skip=initialSkip;
  auto flush=[&]{if(!async.pending())return;auto r=async.take();if(!r.words.empty()){if(std::this_thread::get_id()!=producer||done)throw std::logic_error("queue owner violation");queue.push_back({epoch,std::move(r.words)});}};
  auto emit=[&](Planes p,bool finish=false){
   // p is next GPU/pipeline result, produced before retrieving prior CPU job.
   flush();const size_t n=p.empty()?0:p[0].size();const size_t drop=std::min(skip,n);skip-=drop;
   std::vector<int32_t> expected;std::string why;if(!sync.process(p,sm,drop,finish,expected,&why))throw std::runtime_error(why);
   if(!expected.empty())oracle.push_back({epoch,std::move(expected)});async.submit(std::move(p),drop,finish);
  };
  size_t pos=0;for(size_t n:{15u,1u,31u,1001u,16u}){auto p=input(n,pos);pos+=n;emit(std::move(p));}
  // EOF drain contributes real samples, then two flushes straddle finish padding.
  emit(input(43,pos));emit(input(7,pos+43));flush();const auto pending=async.pending_bits();REQUIRE(pending==sync.pending_bits());emit({},true);flush();REQUIRE(async.pending_bits()==0);done=true;
  REQUIRE(queue.size()==oracle.size());for(size_t i=0;i<queue.size();++i){REQUIRE(queue[i].epoch==oracle[i].epoch&&queue[i].words==oracle[i].words);compared+=queue[i].words.size();}
  // Start a new producer epoch; queued old audio and an outstanding partial job
  // are discarded. A seek preroll still warms SDM before dropping its samples.
  done=false;emit(input(31,10000));REQUIRE(async.pending());
  const auto publishedSeek=seekState.publish_seek(20000);const auto [requestedFrame,requestedEpoch]=seekState.consume_seek();
  async.discard();async.reset_for_seek();sync.discard_pending();for(auto& m:sm)m->reset();
  queue.clear();oracle.clear();epoch=requestedEpoch;skip=19;REQUIRE(requestedFrame==20000&&epoch==publishedSeek);
  emit(input(47,20000));emit(input(15,20047));flush();emit({},true);flush();done=true;
  REQUIRE(queue.size()==oracle.size());for(size_t i=0;i<queue.size();++i){REQUIRE(queue[i].epoch==2&&queue[i].words==oracle[i].words);compared+=queue[i].words.size();}
 }
 // Abort/producer exception: done may precede async destruction, but the worker
 // has no queue callback and therefore cannot enqueue after done.
 auto am=mods();std::vector<Tagged> queue;bool done=false;
 {DopAsyncStream async(am);async.submit(input(10007,0),0,false);done=true;}
 REQUIRE(done&&queue.empty());
 {DopAsyncStream async(am);try{async.submit(input(10007,0),0,false);throw std::runtime_error("producer source failed");}catch(const std::runtime_error&){done=true;}REQUIRE(queue.empty());}
 REQUIRE(queue.empty());
 std::cout<<"PASS actual RenderQ paired seek/coalescing/CV interleave/2000publish stress; 4 producer integration sequences "<<compared<<" whole words: producer(N+1)->flush(N)->submit(N+1), EOF drain/two flushes, seek discard/reset/new epoch, abort/exception no worker enqueue\n";
}
