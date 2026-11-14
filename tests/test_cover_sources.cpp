// Actual cover-chain boundary with controlled HTTP transport: no player/device,
// credentials, files or external network in the default regression run.
#include "meta/artwork.h"
#include <cstdio>
#include <future>
#include <mutex>
#include <string>
#include <vector>

using namespace rta::meta;
static int checks=0, failures=0;
static void ck(bool ok,const char* what){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",what);}
static rta::net::HttpResponse response(int status,const std::string& body={}) {
    rta::net::HttpResponse r;r.status=status;r.ok=status==200;r.body.assign(body.begin(),body.end());return r;
}
static ArtworkOpts only(std::initializer_list<const char*> ids) {
    ArtworkOpts o;o.useCache=false;o.totalBudgetMs=1800;o.perSourceMs=1200;o.downloadMs=1500;o.minBytes=32;
    for(const auto& p:provider_registry())o.srcOn[p.id]=false;
    for(const auto* id:ids)o.srcOn[id]=true;
    return o;
}
static const unsigned char png[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,64,0,0,0,64,8,2,0,0,0,37,11,230,137,0,0,0,128,73,68,65,84,120,156,213,206,65,17,0,32,12,192,176,82,67,40,194,18,154,17,177,7,215,40,200,218,231,82,38,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,39,113,18,231,239,192,212,3,103,252,1,154,224,45,105,76,0,0,0,0,73,69,78,68,174,66,96,130};
static const std::string image(reinterpret_cast<const char*>(png),sizeof(png));
static const char* qq=R"({"req_1":{"data":{"body":{"song":{"list":[{"title":"Song","singer":[{"name":"Artist"}],"album":{"name":"Album","mid":"a"}},{"title":"Song","singer":[{"name":"Artist"}],"album":{"name":"Album","mid":"b"}},{"title":"Song","singer":[{"name":"Artist"}],"album":{"name":"Album","mid":"c"}}]}}}}})";
static const char* netease=R"({"result":{"songs":[{"name":"Song","artists":[{"name":"Artist"}],"album":{"name":"Album","id":2,"picUrl":"https://images.example/owned.png"}}]}})";
static const char* mbid="76df3287-6cda-33eb-8e9a-044b5e15ffdd";
// Minimized from the real public 200/code200 search response captured on
// 2026-11-10: matching album18915 rows have picId, but no album.picUrl.
static const char* neMissingPicture=R"({"code":200,"result":{"songs":[{"name":"爱在西元前","artists":[{"name":"周杰伦"}],"album":{"name":"范特西","id":18915,"picId":109951165606034156}},{"name":"安静","artists":[{"name":"周杰伦"}],"album":{"name":"范特西","id":18915,"picId":109951165606034156}}]}})";
static std::string neAlbum(const std::string& id="18915",const std::string& album="范特西",const std::string& artist="周杰伦") {
    return "{\"code\":200,\"album\":{\"id\":"+id+",\"name\":\""+album+"\",\"artists\":[{\"name\":\""+artist+"\"}],\"picUrl\":\"https://images.example/netease.png\"}}";
}
static const char* dizzyCard=R"(<form action="/albums/search" id="search-field"></form><h4 class="text-warning">作品</h4><a href="/d/GKSL-003"><div class="card"><div class="card-body"><img src="https://cdn.dizzylab.net/media/cover/GKSL-003.jpg!cover"><h1>2 Pixels</h1><h3>月華社</h3></div></div></a>)";
static std::string mbRelease(const std::string& album="Album",const std::string& artist="Artist",const std::string& id=mbid) {
    return "{\"releases\":[{\"id\":\""+id+"\",\"title\":\""+album+"\",\"artist-credit\":[{\"name\":\""+artist+"\",\"artist\":{\"name\":\""+artist+"\"}}]}]}";
}
int main(int argc,char** argv) {
    // Opt-in public samples only, never part of CTest. Uses production WinHTTP,
    // TLS validation and proxy behavior with no transport hook or credentials.
    if (argc > 1 && std::string(argv[1]).rfind("--live-",0)==0) {
        const std::string source=std::string(argv[1]).substr(7);auto o=only({});o.minBytes=1024;o.totalBudgetMs=6000;o.perSourceMs=2000;o.downloadMs=3000;
        const auto start=std::chrono::steady_clock::now();std::string bytes;bool ok=false;
        if(source=="caa") {
            rta::net::HttpBudgetScope budget(3000);
            const std::string url=std::string("https://coverartarchive.org/release/")+mbid+"/front-1200";
            const auto r=art_download_image(url,{std::string("User-Agent: ")+pv_user_agent()},o,3000);ok=r.ok;bytes=r.bytes;
            std::printf("LIVE caa url=%s ok=%d bytes=%zu reason=%s\n",url.c_str(),ok,bytes.size(),r.reason.c_str());
        } else if(source=="musicbrainz") {
            const auto r=art_cover_search_source("musicbrainz","Kind of Blue Miles Davis","Kind of Blue","Miles Davis",o,6000);
            std::printf("LIVE musicbrainz candidates=%d urls=%zu transient=%d reason=%s\n",r.cands,r.urls.size(),r.transient,r.why.c_str());
            ok=!r.urls.empty(); // Only the search sample; CAA has a separate one-request probe.
        } else if(source=="discogs") {
            o=only({"discogs"});o.minBytes=1024;o.totalBudgetMs=12000;o.perSourceMs=2500;o.downloadMs=2500;
            const auto r=art_multi_source_image(false,"Kind of Blue","Miles Davis",o);ok=r.ok;bytes=r.bytes;
            std::printf("LIVE %s release=%s\n",r.summary().c_str(),r.releaseId.c_str());
        } else return 3;
        if(ok&&!bytes.empty()&&argc>2)art_write_file(argv[2],bytes);
        std::printf("LIVE elapsed_ms=%lld budget_ms=%d tls_validation=production credentials=none\n",(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count(),source=="caa"?3000:o.totalBudgetMs);
        return ok?0:2;
    }
    {
        std::vector<ArtHttpRequest> calls;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){calls.push_back(q);return response(401,"{\"message\":\"Authentication required\"}");});
        const auto r=art_multi_source_image(false,"Album","Artist",only({"discogs"}));
        ck(!calls.empty()&&calls[0].url.find("api.discogs.com/database/search")!=std::string::npos,"enabled registered Discogs Cover source is actually called");
        ck(!r.ok&&!r.tried.empty()&&r.tried[0].source=="discogs"&&r.tried[0].reason.find("401")!=std::string::npos,"Discogs authentication failure is explicit and cannot claim a hit");
    }
    {
        std::vector<ArtHttpRequest> calls;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){
            calls.push_back(q);
            if(q.url.find("musicu.fcg")!=std::string::npos)return response(200,qq);
            if(q.url.find("y.gtimg.cn")!=std::string::npos)return response(404);
            if(q.url.find("music.163.com/api/search")!=std::string::npos)return response(200,netease);
            if(q.url=="https://images.example/owned.png")return response(200,image);
            return response(500);
        });
        const auto r=art_multi_source_image(false,"Album","Artist",only({"qq","netease"}));
        ck(r.ok&&r.source=="netease"&&r.matchedAlbum=="Album"&&r.matchedArtist=="Artist","fast image failures preserve elapsed budget for the next successful source");
        int qqImages=0,neSearch=0;for(const auto&q:calls){qqImages+=q.url.find("y.gtimg.cn")!=std::string::npos;neSearch+=q.url.find("music.163.com/api/search")!=std::string::npos;}
        ck(qqImages==3&&neSearch==1,"all eligible first-source images fail before the real next-source attempt");
    }
    {
        int timeout=0;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){timeout=q.timeoutMs;return response(404);});
        art_download_image("https://images.example/missing.png",{},only({}),17);
        ck(timeout>0&&timeout<=17,"a small remaining budget is not raised to the old 500ms timeout floor");
    }
    {
        bool registered=false;for(const auto&p:provider_registry())if(std::string(p.id)=="musicbrainz")registered=pv_has_cap(p.caps,ArtCap::Cover)&&!p.canSearch&&!p.canLyric;
        ck(registered,"MusicBrainz is an explicit cover provider without claiming song search or lyrics");
        std::vector<ArtHttpRequest> calls;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){calls.push_back(q);return q.url.find("musicbrainz.org/ws/2/")!=std::string::npos?response(200,mbRelease()):response(200,image);});
        const auto r=art_multi_source_image(false,"Album","Artist",only({"musicbrainz"}));
        ck(r.ok&&r.source=="musicbrainz"&&r.releaseId==mbid&&r.matchedAlbum=="Album"&&r.matchedArtist=="Artist","verified MusicBrainz release ID binds the actual CAA front image");
        ck(calls.size()==2&&calls[1].url==std::string("https://coverartarchive.org/release/")+mbid+"/front-1200","CAA image is fetched from the exact matched release MBID");
        bool ua=false;for(const auto&q:calls)for(const auto&h:q.headers)if(h.find("User-Agent: RLPlayer/")==0&&h.find("https://rlplayer.rainlain.com")!=std::string::npos)ua=true;
        ck(ua,"cover requests identify the real app with a meaningful contact URL");
        calls.clear();auto off=only({});const auto absent=art_multi_source_image(false,"Album","Artist",off);
        ck(!absent.ok&&calls.empty(),"disabled registry sources including MusicBrainz issue zero requests");
    }
    for (int status : {503,429}) {
        set_artwork_http_for_test([&](const ArtHttpRequest&q){return q.url.find("musicbrainz.org/ws/2/")!=std::string::npos?response(200,mbRelease()):response(status);});
        const auto r=art_multi_source_image(false,"Album","Artist",only({"musicbrainz"}));
        ck(!r.ok&&!r.tried.empty()&&r.tried.back().transient,"CAA service failure/rate limit is transient rather than an album miss");
        const auto neg=art_neg_render(100,r.tried);
        ck(neg.find("src=musicbrainz kind=fail")!=std::string::npos&&neg.find("kind=miss")==std::string::npos,"transient CAA failure cannot produce a permanent negative-cache entry");
    }
    {
        set_artwork_http_for_test([&](const ArtHttpRequest&){return response(401);});
        const auto r=art_multi_source_image(false,"Album","Artist",only({"discogs"}));
        ck(!r.ok&&!r.tried.empty()&&r.tried.back().transient,"an authentication failure is not cached as authoritative no-artwork");
    }
    {
        int calls=0;set_artwork_http_for_test([&](const ArtHttpRequest&){++calls;return response(404);});
        const auto r=art_multi_source_image(false,"Unmatched directory","",only({"musicbrainz","discogs"}));
        ck(!r.ok&&calls==0,"a directory grouping without a trusted artist never initiates album artwork search");
    }
    {
        int images=0;set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("/database/search?")!=std::string::npos)return response(200,R"({"results":[{"id":123}]})");
            if(q.url.find("/releases/123")!=std::string::npos)return response(200,R"({"id":999,"title":"Album","artists":[{"name":"Artist"}],"images":[{"type":"primary","uri":"https://images.example/wrong-release.png"}]})");
            ++images;return response(200,image);
        });
        const auto r=art_multi_source_image(false,"Album","Artist",only({"discogs"}));
        ck(!r.ok&&images==0,"Discogs detail response cannot forge the release ID selected by search");
    }
    {
        auto o=only({"qq"});o.qqHost="http://127.0.0.1:12345";std::vector<ArtHttpRequest> calls;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){calls.push_back(q);if(q.url.find(o.qqHost)!=0)return response(500);return q.url.find("/cgi-bin/musicu.fcg")!=std::string::npos?response(200,qq):response(200,image);});
        const auto r=art_multi_source_image(false,"Album","Artist",o);
        ck(r.ok&&calls.size()==2&&calls[0].url.find(o.qqHost)==0&&calls[1].url.find(o.qqHost)==0,"explicit QQ fixture host bounds both actual search and generated CDN requests");
    }
    for(const std::string kind : {"album","artist","id"}) {
        int imageCalls=0;set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("musicbrainz.org/ws/2/")!=std::string::npos)return response(200,mbRelease(kind=="album"?"Album (Live)":"Album",kind=="artist"?"Artist & Guest":"Artist",kind=="id"?"../../wrong-release":mbid));
            ++imageCalls;return response(200,image);
        });
        const auto r=art_multi_source_image(false,"Album","Artist",only({"musicbrainz"}));
        ck(!r.ok&&imageCalls==0,kind=="album"?"a different album edition cannot supply artwork":kind=="artist"?"partial artist credit cannot supply another collaboration's artwork":"invalid MBID cannot become an image path");
    }
    {
        std::vector<std::chrono::steady_clock::time_point> starts;std::mutex mutex;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("musicbrainz.org/ws/2/")!=std::string::npos){std::lock_guard<std::mutex> lock(mutex);starts.push_back(std::chrono::steady_clock::now());return response(200,mbRelease());}
            return response(200,image);
        });
        auto request=[](){auto o=only({"musicbrainz"});o.totalBudgetMs=4000;return art_multi_source_image(false,"Album","Artist",o);};
        auto a=std::async(std::launch::async,request),b=std::async(std::launch::async,request);const auto ra=a.get(),rb=b.get();
        long long gap=starts.size()==2?std::chrono::duration_cast<std::chrono::milliseconds>(starts[1]-starts[0]).count():0;
        std::printf("MusicBrainz concurrent request gap=%lldms\n",gap);
        ck(ra.ok&&rb.ok&&starts.size()==2&&gap>=1000,"MusicBrainz global rate gate spaces requests from separate workers by one second");
    }
    {
        dl_gate_reset_for_test();int requests=0;
        set_artwork_http_for_test([&](const ArtHttpRequest&){++requests;return response(404);});
        auto o=only({"dizzylab","qq","netease","itunes","discogs","musicbrainz"});o.totalBudgetMs=15000;
        const auto r=art_multi_source_image(false,"Album","Artist",o);std::string order;
        for(const auto&t:r.tried){if(!order.empty())order+=',';order+=t.source;}
        ck(!r.ok&&requests==6&&order=="dizzylab,qq,netease,itunes,discogs,musicbrainz","every enabled Cover registry entry has a real request and ordered failure trail");
    }
    {
        int imageCalls=0;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("/database/search?")!=std::string::npos)return response(200,R"({"results":[{"id":123}]})");
            if(q.url.find("/releases/123")!=std::string::npos)return response(200,R"({"id":123,"title":"Album","artists":[{"name":"Artist"}],"images":[{"type":"primary","uri":"https://images.example/owned.png"}]})");
            ++imageCalls;return response(200,image);
        });
        const auto r=art_multi_source_image(false,"Album","Artist",only({"discogs"}));
        ck(r.ok&&imageCalls==1&&r.source=="discogs"&&r.releaseId=="123"&&r.matchedAlbum=="Album"&&r.matchedArtist=="Artist","Discogs matching release detail contributes its own verified image and provenance");
    }
    {
        const std::string second="76df3287-6cda-33eb-8e9a-044b5e15ffde";int images=0;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("musicbrainz.org/ws/2/")!=std::string::npos)return response(200,"{\"releases\":[{\"id\":\""+std::string(mbid)+"\",\"title\":\"Album\",\"artist-credit\":[{\"name\":\"Artist\"}]},{\"id\":\""+second+"\",\"title\":\"Album\",\"artist-credit\":[{\"name\":\"Artist\"}]}]}");
            ++images;return q.url.find(second)!=std::string::npos?response(200,image):response(404);
        });
        const auto r=art_multi_source_image(false,"Album","Artist",only({"musicbrainz"}));
        ck(r.ok&&images==2&&r.releaseId==second,"CAA404 advances to the next independently verified release instead of losing the album");
    }
    {
        int requests=0;set_artwork_http_for_test([&](const ArtHttpRequest&){++requests;return response(503);});
        const auto r=art_multi_source_image(false,"Album","Artist",only({"musicbrainz"}));
        ck(!r.ok&&requests==1&&!r.tried.empty()&&r.tried.back().transient,"MusicBrainz503 keeps a transient trail without query-variant retry bursts");
    }
    {
        int details=0,images=0;set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("/api/search/get")!=std::string::npos)return response(200,neMissingPicture);
            if(q.url.find("/api/album/18915")!=std::string::npos){++details;return response(200,neAlbum());}
            ++images;return response(200,image);
        });
        const auto r=art_multi_source_image(false,"范特西","周杰伦",only({"netease"}));
        ck(r.ok&&details==1&&images==1&&r.releaseId=="18915"&&r.matchedAlbum=="范特西"&&r.matchedArtist=="周杰伦","real NetEase search without picUrl resolves one verified album detail before downloading");
    }
    for(const std::string mismatch : {"id","album","artist"}) {
        int details=0,images=0;set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("/api/search/get")!=std::string::npos)return response(200,neMissingPicture);
            if(q.url.find("/api/album/18915")!=std::string::npos){++details;return response(200,neAlbum(mismatch=="id"?"18916":"18915",mismatch=="album"?"范特西 (Live)":"范特西",mismatch=="artist"?"其他艺人":"周杰伦"));}
            ++images;return response(200,image);
        });
        const auto r=art_multi_source_image(false,"范特西","周杰伦",only({"netease"}));
        ck(!r.ok&&details==1&&images==0,"NetEase detail wrong ID edition or complete artist is rejected before image download");
    }
    for(int status : {503,429}) {
        int details=0;set_artwork_http_for_test([&](const ArtHttpRequest&q){if(q.url.find("/api/search/get")!=std::string::npos)return response(200,neMissingPicture);++details;return response(status);});
        const auto r=art_multi_source_image(false,"范特西","周杰伦",only({"netease"}));
        ck(!r.ok&&details==1&&!r.tried.empty()&&r.tried.back().transient,"NetEase detail service failure is transient and duplicate album rows do not retry it");
    }
    {
        dl_gate_reset_for_test();int images=0;set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("/search/?")!=std::string::npos)return response(200,q.url.find(percent_enc_utf8("月華社"))==std::string::npos?dizzyCard:R"(<form action="/albums/search" id="search-field"></form>)");
            ++images;return response(200,image);
        });
        const auto r=art_multi_source_image(false,"2 Pixels","月華社",only({"dizzylab"}));
        ck(r.ok&&images==1&&r.releaseId=="GKSL-003"&&r.matchedArtist=="月華社","real dizzylab title-keyword search avoids a combined-phrase miss while preserving identity checks");
        images=0;const auto other=art_multi_source_image(false,"2 Pixels","其他社团",only({"dizzylab"}));
        ck(!other.ok&&images==0,"dizzylab album-only query still rejects the same title credited to a different artist");
    }
    {
        int searchTimeout=0,detailTimeout=0,imageTimeout=0;
        set_artwork_http_for_test([&](const ArtHttpRequest&q){
            if(q.url.find("/api/search/get")!=std::string::npos){searchTimeout=q.timeoutMs;std::this_thread::sleep_for(std::chrono::milliseconds(130));return response(200,neMissingPicture);}
            if(q.url.find("/api/album/18915")!=std::string::npos){detailTimeout=q.timeoutMs;return response(200,neAlbum());}
            imageTimeout=q.timeoutMs;return response(200,image);
        });
        auto o=only({"netease"});o.totalBudgetMs=750;o.perSourceMs=750;o.downloadMs=750;
        art_multi_source_image(false,"范特西","周杰伦",o);
        ck(detailTimeout>0&&detailTimeout<=searchTimeout-120&&imageTimeout<=detailTimeout,"NetEase detail and image timeouts share elapsed search budget without a timeout floor");
        int calls=0;o.srcOn["netease"]=false;set_artwork_http_for_test([&](const ArtHttpRequest&){++calls;return response(200);});
        art_multi_source_image(false,"范特西","周杰伦",o);
        ck(calls==0,"disabled NetEase performs neither search album detail nor image requests");
    }
    {
        set_artwork_http_for_test(nullptr);art_discogs_last()={};
        std::vector<std::chrono::steady_clock::time_point> starts;
        for(int n=0;n<3;++n){art_discogs_throttle();starts.push_back(std::chrono::steady_clock::now());}
        const auto a=std::chrono::duration_cast<std::chrono::milliseconds>(starts[1]-starts[0]).count();
        const auto b=std::chrono::duration_cast<std::chrono::milliseconds>(starts[2]-starts[1]).count();
        std::printf("Discogs real non-network throttle gaps=%lld,%lldms\n",(long long)a,(long long)b);
        ck(a>=2500&&b>=2500,"Discogs records actual admission time so a third call cannot burst after a wait");
    }
    set_artwork_http_for_test(nullptr);
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
