#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include "meta/retrieval.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <thread>
using namespace rta::meta;
static int fails=0, checks=0;
static void check(bool ok,const char* what){++checks;std::cout<<(ok?"PASS ":"FAIL ")<<what<<"\n";if(!ok)++fails;}

// Only loopback HTTP. Each cold pair waits for both opensearch requests, proving
// that both knowledge_get calls read the same empty pre-fetch snapshot.
struct LocalWiki {
 SOCKET listener=INVALID_SOCKET;unsigned short port=0;std::thread acceptThread;
 std::atomic<bool> stop{false};std::atomic<int> requests{0};
 std::mutex barrierMu;std::condition_variable barrier;int opens=0;std::vector<std::thread> workers;
 LocalWiki(){
  listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in sa{};sa.sin_family=AF_INET;sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(listener==INVALID_SOCKET||bind(listener,(sockaddr*)&sa,sizeof(sa))||listen(listener,8))return;
  int sz=sizeof(sa);getsockname(listener,(sockaddr*)&sa,&sz);port=ntohs(sa.sin_port);
  acceptThread=std::thread([this]{while(!stop){fd_set f;FD_ZERO(&f);FD_SET(listener,&f);timeval tv{0,100000};if(select(0,&f,nullptr,nullptr,&tv)<=0)continue;
   SOCKET c=accept(listener,nullptr,nullptr);if(c==INVALID_SOCKET)continue;
   workers.emplace_back([this,c]{DWORD timeout=2000;setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));std::string req;char buf[2048];int n;
    while(req.find("\r\n\r\n")==std::string::npos&&(n=recv(c,buf,sizeof(buf),0))>0)req.append(buf,n);
    ++requests;const std::string lang=req.find("/en/")!=std::string::npos?"en":"zh";const bool open=req.find("action=opensearch")!=std::string::npos;
    if(open){std::unique_lock<std::mutex> lk(barrierMu);++opens;barrier.notify_all();barrier.wait_for(lk,std::chrono::seconds(2),[this]{return opens>=2;});}
    const std::string body=open?"[\"Test\",[\"Test\"],[\"Musician\"],[\"https://"+lang+".wikipedia.org/wiki/Test\"]]":"{\"query\":{\"pages\":[{\"title\":\"Test\",\"extract\":\"Music biography in "+lang+"\"}]}}";
    const std::string response="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    size_t off=0;while(off<response.size()){n=send(c,response.data()+off,(int)(response.size()-off),0);if(n<=0)break;off+=n;}
    shutdown(c,SD_BOTH);closesocket(c);
   });
  }});
 }
 ~LocalWiki(){stop=true;if(acceptThread.joinable())acceptThread.join();for(auto& w:workers)w.join();if(listener!=INVALID_SOCKET)closesocket(listener);}
 std::string url(const std::string& lang)const{return "http://127.0.0.1:"+std::to_string(port)+"/"+lang+"/api.php";}
};
int main(){
 WSADATA wd{};WSAStartup(MAKEWORD(2,2),&wd);
 const auto temp=fsx::temp_directory_path()/("rlplayer-audit-app-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fsx::create_directories(temp);
 const std::string base=meta_string_of(temp);const auto now=(long long)std::time(nullptr);
 auto makeSummary=[&](std::string album){SummaryResult s;s.ok=true;s.generated_at=now;s.expires_at=now+86400;s.fields={{"album",album}};return s;};
 const auto idA=summary_track_entity_id("Intro","Artist","Album A"),idB=summary_track_entity_id("Intro","Artist","Album B");
 check(idA!=idB,"same artist/title, different albums have different cache and job keys");
 check(summary_save_cache(base,idA,"zh",makeSummary("Album A")),"seed album A cache");
 check(summary_save_cache(base,idB,"zh",makeSummary("Album B")),"seed album B cache");
 RetrievalOptions ro;ro.metabase=base;ro.llm_cfg.apiKey="audit-placeholder";int apiCalls=0;ro.llm_cfg.transport=[&](const auto&,const auto&,const auto&){++apiCalls;return rta::net::HttpResponse{};};
 ro.album="Album A";auto a=retrieval_fetch_track("Intro","Artist","zh",ro);ro.album="Album B";auto b=retrieval_fetch_track("Intro","Artist","zh",ro);
 check(a.ok&&b.ok&&a.from_cache&&b.from_cache&&a.fields[0].second=="Album A"&&b.fields[0].second=="Album B"&&apiCalls==0,"production retrieval returns the correct album from each cache, HTTP=0");
 const std::string broken="{\"v\":7,\"ok\":true,\"generated_at\":"+std::to_string(now)+",\"expires_at\":"+std::to_string(now+86400)+",\"citations\":[],\"sections\":[],\"fields\":{\"album\":\"PARTIAL";
 entity_write_atomic(summary_cache_path(base,"broken","zh"),broken);SummaryResult sr;
 check(!summary_load_cache(base,"broken","zh",sr),"production summary loader rejects a truncated current-version cache");
 entity_write_atomic(knowledge_cache_path(base,"broken"),"{\"materials\":[{\"url\":\"https://en.wikipedia.org/wiki/Test\",\"lang\":\"en\",\"text\":\"PARTIAL");
 std::vector<KnowledgeMaterial> mats;check(!knowledge_load_cache(base,"broken",mats),"production knowledge loader rejects truncated cache");
 KnowledgeMaterial km;km.lang="zh";km.url="https://zh.wikipedia.org/wiki/Test";km.text="Chinese cache";km.expires_at=now+86400;
 knowledge_save_cache(base,"lang","Test",{km});KnowledgeOpts ko;ko.metabase=base;ko.lang="en";ko.include_wikipedia=false;ko.include_moegirl=false;
 auto kr=knowledge_get("lang","Test",ko);check(kr.materials.empty(),"English offline query never returns a Chinese cache");
 ko.lang="zh";ko.refresh=true;kr=knowledge_get("lang","Test",ko);check(kr.materials.size()==1&&kr.offline&&kr.materials[0].lang=="zh","matching language still supports offline fallback");
 auto& gate=gate_wikipedia();gate.reset();gate.min_interval_ms=0;
 int concurrentKept=0,languageIsolated=0,totalRequests=0;
 for(int round=0;round<8;++round){LocalWiki wiki;check(wiki.port!=0,"loopback mock bound ephemeral port");KnowledgeOpts z,e;z.metabase=e.metabase=base;z.lang="zh";e.lang="en";z.include_moegirl=e.include_moegirl=false;z.timeout_ms=e.timeout_ms=2000;z.wiki_api_base=wiki.url("zh");e.wiki_api_base=wiki.url("en");KnowledgeResult rz,re;const auto id="concurrent-"+std::to_string(round);
  std::thread tz([&]{rz=knowledge_get(id,"Test",z);}),te([&]{re=knowledge_get(id,"Test",e);});tz.join();te.join();totalRequests+=wiki.requests;
  if(rz.materials.size()==1&&re.materials.size()==1&&rz.materials[0].lang=="zh"&&re.materials[0].lang=="en")++languageIsolated;
  mats.clear();if(knowledge_load_cache(base,id,mats)&&mats.size()==2)++concurrentKept;
 }
 std::cout<<"knowledge concurrent rounds=8 preserved="<<concurrentKept<<" language_isolated="<<languageIsolated<<" loopback_HTTP="<<totalRequests<<"\n";
 check(concurrentKept==8&&languageIsolated==8&&totalRequests==32,"concurrent production fetch preserves both languages and filters each response");
 // A warm zh cache plus a new en fetch must return only English, while disk keeps both.
 {LocalWiki wiki;wiki.opens=2;ko.lang="en";ko.refresh=true;ko.include_wikipedia=true;ko.wiki_api_base=wiki.url("en");ko.timeout_ms=2000;kr=knowledge_get("lang","Test",ko);mats.clear();knowledge_load_cache(base,"lang",mats);check(kr.materials.size()==1&&kr.materials[0].lang=="en"&&mats.size()==2,"fresh English fetch never returns mixed-language disk contents");}
 std::atomic<int> writeFailures=0;int badRounds=0;const auto file=temp/"atomic.json";
 for(int round=0;round<30;++round){std::atomic<int> ready=0;std::atomic<bool> go=false;std::vector<std::thread> ts;
  for(int n=0;n<12;++n)ts.emplace_back([&,n]{const std::string payload="{\"data\":\""+std::string(100000+n*10000,'A'+n)+"\"}";++ready;while(!go)std::this_thread::yield();if(!entity_write_atomic(file,payload))++writeFailures;});
  while(ready<12)std::this_thread::yield();go=true;for(auto& t:ts)t.join();std::string bytes;rta::net::Json parsed;if(!entity_read_file(file,bytes)||!rta::net::json_parse_strict(bytes,parsed))++badRounds;
 }
 std::cout<<"atomic writes=360 failures="<<writeFailures<<" invalid_final_json_rounds="<<badRounds<<"/30\n";
 check(writeFailures==0&&badRounds==0,"concurrent production atomic writes all succeed and leave valid JSON");
 MetaStore::instance().setBaseDir(base);int storeBad=0;
 for(int round=0;round<30;++round){std::atomic<int> ready=0;std::atomic<bool> go=false;std::vector<std::thread> ts;
  for(int n=0;n<12;++n)ts.emplace_back([&,n]{TrackMeta t;t.path="same-track.wav";t.fields["title"].value=std::string(100000+n*10000,'A'+n);++ready;while(!go)std::this_thread::yield();MetaStore::instance().save(t);});
  while(ready<12)std::this_thread::yield();go=true;for(auto& t:ts)t.join();TrackMeta result;if(!MetaStore::instance().load("same-track.wav",result))++storeBad;
 }
 std::cout<<"MetaStore writes=360 invalid_final_rounds="<<storeBad<<"/30\n";check(storeBad==0,"concurrent production MetaStore writes remain valid");
 EntityStore::instance().setBaseDir(base);std::vector<std::thread> aliasThreads;
 for(int n=0;n<12;++n)aliasThreads.emplace_back([&,n]{Entity e;e.id=entity_id(EntityKind::Person,"Audit Artist");e.kind=EntityKind::Person;e.name="Audit Artist";e.normKey=entity_norm(e.name);e.addAlias("alias-"+std::to_string(n));EntityStore::instance().upsert(e);});
 for(auto& t:aliasThreads)t.join();Entity mergedEntity;
 check(EntityStore::instance().loadAt(EntityKind::Person,entity_id(EntityKind::Person,"Audit Artist"),mergedEntity)&&mergedEntity.aliases.size()==12,"concurrent entity upsert retains all 12 aliases");
 entity_write_atomic(EntityStore::instance().pathOf(EntityKind::Person,"broken-entity"),"{\"id\":\"broken-entity\",\"kind\":\"person\",\"aliases\":[],\"edges\":[],\"name\":\"PARTIAL");
 Entity brokenEntity;check(!EntityStore::instance().loadAt(EntityKind::Person,"broken-entity",brokenEntity),"production entity loader rejects a truncated name after complete required fields");
 // Observe real serialized requests at the supported transport boundary.
 std::vector<std::string> languageRequests;LlmConfig languageCfg;languageCfg.apiKey="audit-placeholder";languageCfg.usagePath=meta_string_of(temp/"language-usage.json");
 languageCfg.transport=[&](const std::string&,const std::string& body,const auto&){languageRequests.push_back(body);
  const std::string content=R"({"fields":{"简介":"Known information only"},"sections":[{"heading":"Biography","paragraphs":[{"text":"Limited verified information is available."}]}]})";
  const std::string reply="{\"choices\":[{\"message\":{\"content\":\""+llm_esc(content)+"\"},\"finish_reason\":\"stop\"}],\"usage\":{\"total_tokens\":10}}";rta::net::HttpResponse response;response.ok=true;response.status=200;response.body.assign(reply.begin(),reply.end());return response;};
 auto plain=summary_ai_only_generate("Language Test","艺术家","en",languageCfg);
 check(plain.ok&&!languageRequests.empty()&&languageRequests.back().find("[OUTPUT LANGUAGE: en]")!=std::string::npos,"plain fallback sends requested English rule in the actual LLM request");
 ProfileContext profile;profile.valid=true;auto profiled=summary_ai_only_generate("Language Test","艺术家","ja",languageCfg,nullptr,"",&profile);
 check(profiled.ok&&languageRequests.back().find("[OUTPUT LANGUAGE: ja]")!=std::string::npos,"profile fallback sends requested Japanese rule in the actual LLM request");
 KnowledgeMaterial languageMaterial;languageMaterial.lang="en";languageMaterial.url="https://en.wikipedia.org/wiki/Test";languageMaterial.text="Known music biography.";languageMaterial.expires_at=now+86400;knowledge_save_cache(base,entity_id(EntityKind::Person,"Language Main"),"Language Main",{languageMaterial});
 RetrievalOptions languageOptions;languageOptions.metabase=base;languageOptions.llm_cfg=languageCfg;languageOptions.enable_search=false;languageOptions.enable_web_tools=false;
 auto languageMain=retrieval_fetch(EntityKind::Person,"Language Main","en",languageOptions);
 check(languageMain.ok&&languageRequests.back().find("[OUTPUT LANGUAGE: en]")!=std::string::npos&&languageRequests.back().find("Preserve structural JSON property names and schema field keys")!=std::string::npos,"production profile synthesis preserves schema and sends requested prose language");
 check(build_synthesis_prompt("Test",{}, {},"fr")[0].content.find("[OUTPUT LANGUAGE: fr]")!=std::string::npos&&build_track_prompt("Intro","Artist",{},"es")[0].content.find("[OUTPUT LANGUAGE: es]")!=std::string::npos,"legacy synthesis and song prompt honor requested language");
 auto oldVersion=summary_to_json(makeSummary("Old language cache"));const auto versionToken="\"v\": "+std::to_string(kSummaryCacheVer);const auto versionAt=oldVersion.find(versionToken);if(versionAt!=std::string::npos)oldVersion.replace(versionAt,versionToken.size(),"\"v\":7");
 SummaryResult oldSummary;check(summary_from_json(oldVersion,oldSummary)&&summary_is_expired(oldSummary),"v7 potentially wrong-language summaries expire after language fix");
 const unsigned char coverBytes[]={1,2,3,4};const auto coverRel=MetaStore::instance().saveCover("same-track.wav",coverBytes,4,"png");check(!coverRel.empty(),"cover first write succeeds");
 const auto coverPath=temp/meta_path_of(coverRel);HANDLE held=CreateFileW(coverPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 check(held!=INVALID_HANDLE_VALUE,"hold cover target against replacement");
 check(MetaStore::instance().saveCover("same-track.wav",coverBytes,4,"png").empty(),"cover reports replacement failure instead of a false successful path");
 if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
 size_t remainingTemp=0;for(const auto& de:fsx::directory_iterator(temp))if(de.path().filename().string().find(".tmp.")!=std::string::npos)++remainingTemp;check(remainingTemp==0,"no successful-write temporary files remain");
 std::error_code ec;fsx::remove_all(temp,ec);WSACleanup();std::cout<<"checks="<<checks<<" failures="<<fails<<"\n";return fails?1:0;
}
