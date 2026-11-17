// Windows signed, same-origin, deterministic single-file incremental updates.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "version.h"
#include "net/http_client.h"
#include "net/http_authority.h"
#include "net/json.h"
#include "net/update_check.h"
#include "platform/fs.h"
#include "platform/http.h"
#include "platform/path.h"
#include "platform/process.h"
#include "platform/time.h"

namespace rta::update {

inline std::string current_version(){return std::string(RLPLAYER_VERSION);}
inline int cmp_versions(const std::string& a,const std::string& b){
    switch(compare_versions(parse_version(a),parse_version(b))){
        case VersionCmp::Less:return -1;case VersionCmp::Greater:return 1;case VersionCmp::Equal:return 0;
        default:return -1;}
}
inline std::string install_dir(){
    if(const char* e=std::getenv("RLPLAYER_UPDATE_DIR");e&&*e)return std::string(e);
    static const std::string dir=rta::platform::module_dir_utf8();return dir;
}
inline std::string join_path(const std::string& a,const std::string& b){
    if(a.empty())return b;if(b.empty())return a;const char last=a.back();
    return (last=='\\'||last=='/')?a+b:a+"\\"+b;
}
inline bool path_exists(const std::string& p){return rta::platform::file_exists(rta::platform::from_utf8(p));}
inline bool is_dir(const std::string& p){return rta::platform::is_directory(rta::platform::from_utf8(p));}
inline long long file_size(const std::string& p){return rta::platform::file_size(rta::platform::from_utf8(p));}
inline bool ensure_dir(const std::string& p){
    if(p.empty())return false;if(is_dir(p))return true;const size_t slash=p.find_last_of("\\/");
    if(slash!=std::string::npos&&slash>0)ensure_dir(p.substr(0,slash));
    rta::platform::create_directory(rta::platform::from_utf8(p));return is_dir(p);
}
inline std::string read_text_file(const std::string& p){
    std::ifstream f(rta::platform::from_utf8(p),std::ios::binary);
    return f?std::string((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>()):std::string();
}
inline bool write_text_file(const std::string& p,const std::string& text){
    std::ofstream f(rta::platform::from_utf8(p),std::ios::binary|std::ios::trunc);if(!f)return false;
    f.write(text.data(),(std::streamsize)text.size());return(bool)f;
}
inline bool rmtree(const std::string& dir){
    if(!is_dir(dir)){if(path_exists(dir)){rta::platform::set_normal_attributes(rta::platform::from_utf8(dir));
        return rta::platform::remove_file(rta::platform::from_utf8(dir));}return true;}
    std::vector<rta::platform::DirEntry> entries;
    if(rta::platform::list_directory(rta::platform::from_utf8(dir),entries))for(const auto& e:entries){
        const std::string full=join_path(dir,e.name);if(e.is_dir)rmtree(full);
        else{rta::platform::set_normal_attributes(rta::platform::from_utf8(full));
            rta::platform::remove_file(rta::platform::from_utf8(full));}}
    return rta::platform::remove_directory(rta::platform::from_utf8(dir));
}
inline void list_files_rel(const std::string& root,const std::string& rel,std::vector<std::string>& out){
    const std::string dir=rel.empty()?root:join_path(root,rel);std::vector<rta::platform::DirEntry> entries;
    if(!rta::platform::list_directory(rta::platform::from_utf8(dir),entries))return;
    for(const auto& e:entries){const std::string child=rel.empty()?e.name:rel+"\\"+e.name;
        if(e.is_dir)list_files_rel(root,child,out);else out.push_back(child);}
}
inline int count_files(const std::string& dir,int cap=100000){
    std::vector<std::string> files;list_files_rel(dir,"",files);return(int)std::min<size_t>(files.size(),(size_t)cap);
}
inline std::string updates_root(){return join_path(install_dir(),"updates");}
inline std::string version_dir(const std::string& version){return join_path(updates_root(),version);}
inline std::string tar_exe(){
    const std::string system=rta::platform::system_dir_utf8();
    if(!system.empty()&&path_exists(system+"\\tar.exe"))return system+"\\tar.exe";return "tar.exe";
}

// Configuration contains only skip/client and an explicitly local test manifest.
inline std::string conf_path(){return join_path(install_dir(),"rlplayer-update.conf");}
inline std::map<std::string,std::string> conf_read(){
    std::map<std::string,std::string> values;std::ifstream file(rta::platform::from_utf8(conf_path()));std::string line;
    while(std::getline(file,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.empty()||line[0]=='#')continue;const size_t equal=line.find('=');
        if(equal!=std::string::npos&&equal>0)values[lower_ascii(line.substr(0,equal))]=line.substr(equal+1);}
    return values;
}
inline bool conf_write(const std::map<std::string,std::string>& values){
    std::string text="# RLPlayer signed update configuration\r\n";
    for(const auto& item:values)text+=item.first+"="+item.second+"\r\n";return write_text_file(conf_path(),text);
}
inline std::string skip_version(){const auto values=conf_read();const auto it=values.find("skip");
    return it==values.end()?std::string():it->second;}
inline void set_skip_version(const std::string& version){auto values=conf_read();
    if(version.empty())values.erase("skip");else values["skip"]=version;conf_write(values);}
inline bool allow_local_urls(){const char* e=std::getenv("RLPLAYER_UPDATE_ALLOW_LOCAL");return e&&*e&&std::strcmp(e,"0")!=0;}
inline bool local_update_url_allowed(const std::string& url){
    if(!allow_local_urls()||url.size()>4096||url.rfind("http://",0)!=0)return false;
    for(unsigned char c:url)if(c<=0x20||c==0x7f||c=='\\'||c=='@')return false;
    const size_t slash=url.find('/',7);if(slash==std::string::npos)return false;
    const auto authority=rta::net::parse_http_authority(url.substr(7,slash-7),80);
    return authority.valid&&(authority.host=="127.0.0.1"||authority.host=="localhost")&&
           url.substr(slash).rfind("/api/v1/update/windows/",0)==0;
}
inline bool url_allowed(const std::string& url){return update_download_url_allowed(url)||local_update_url_allowed(url);}
inline bool manifest_url_allowed(const std::string& url){
    if(url==kDefaultManifestUrl)return true;if(!local_update_url_allowed(url))return false;
    const size_t api=url.find("/api/v1/update/windows/");return url.substr(api)=="/api/v1/update/windows/latest";
}
inline std::string manifest_url(){
    if(const char* e=std::getenv("RLPLAYER_UPDATE_MANIFEST");e&&*e&&manifest_url_allowed(e))return std::string(e);
    const auto values=conf_read();const auto it=values.find("manifest_url");
    if(it!=values.end()&&manifest_url_allowed(it->second))return it->second;return kDefaultManifestUrl;
}
inline std::string update_origin(const std::string& manifestUrl){
    const size_t api=manifestUrl.find("/api/v1/update/windows/");return api==std::string::npos?std::string():manifestUrl.substr(0,api);
}
inline std::string object_url_for(const std::string& manifestUrl,const std::string& object){
    if(!safe_object_name(object)||!manifest_url_allowed(manifestUrl))return std::string();
    const std::string url=update_origin(manifestUrl)+"/api/v1/update/windows/objects/"+object;
    return url_allowed(url)?url:std::string();
}
inline std::string report_url_for(const std::string& manifestUrl){
    if(!manifest_url_allowed(manifestUrl))return std::string();
    const std::string url=update_origin(manifestUrl)+"/api/v1/update/windows/report";return url_allowed(url)?url:std::string();
}

// ---------------------------------------------------------------------------
// Local raw-SHA plan. Protected files are excluded before any network request.
// WebView2 is compared locally but never staged; only an actual runtime delta requires FULL.
// ---------------------------------------------------------------------------
struct PlanItem {
    std::string path,wantSha,haveSha,object,objectSha;
    long long wantSize=0,objectSize=0;bool same=false;
};
struct Plan {
    bool ok=false,manual=false;std::string error,reason;std::vector<PlanItem> items;
    int toDownload=0,unchanged=0,protectedSkipped=0,protectedDeletes=0;
    long long downloadBytes=0,skippedBytes=0;std::vector<std::string> missing,deletes;
};
inline Plan plan_incremental(const std::vector<ManifestFile>& files,const std::string& dir,
                             const std::vector<std::string>& deletes={}){
    Plan plan;
    for(const auto& file:files){
        const bool runtime=starts_with_dir(norm_rel2(file.path),"webview2runtime");
        if(!runtime&&is_protected_rel(file.path)){++plan.protectedSkipped;continue;}
        PlanItem item;item.path=file.path;item.wantSha=file.sha256;item.wantSize=file.size;
        item.object=file.object;item.objectSha=file.objectSha256;item.objectSize=file.objectSize;
        std::string hex,error;if(sha256_file(join_path(dir,file.path),hex,error)){item.haveSha=hex;item.same=hex_equals_ci(hex,file.sha256);}
        if(runtime){++plan.protectedSkipped;if(!item.same){plan.manual=true;plan.reason="webview2-update";}continue;}
        if(item.same){++plan.unchanged;plan.skippedBytes+=file.size;}
        else{++plan.toDownload;plan.downloadBytes+=file.objectSize;plan.missing.push_back(file.path);}
        plan.items.push_back(item);
    }
    std::set<std::string> seen;
    for(const auto& path:deletes){
        if(starts_with_dir(norm_rel2(path),"webview2runtime")){
            ++plan.protectedDeletes;plan.manual=true;plan.reason="webview2-update";continue;
        }
        if(is_protected_rel(path)){++plan.protectedDeletes;continue;}
        if(seen.insert(norm_rel2(path)).second)plan.deletes.push_back(path);
    }
    plan.ok=true;return plan;
}

inline std::string json_escape(const std::string& s){
    std::string out;out.reserve(s.size()+8);for(unsigned char c:s){switch(c){
        case '"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\n':out+="\\n";break;
        case '\r':out+="\\r";break;case '\t':out+="\\t";break;
        default:if(c<0x20){char b[8];std::snprintf(b,sizeof(b),"\\u%04x",(unsigned)c);out+=b;}else out+=(char)c;}}
    return out;
}
inline std::string size_text(long long n){
    if(n<=0)return "—";char b[64];
    if(n>=1024LL*1024*1024)std::snprintf(b,sizeof(b),"%.2f GB",(double)n/(1024.0*1024*1024));
    else if(n>=1024LL*1024)std::snprintf(b,sizeof(b),"%.1f MB",(double)n/(1024.0*1024));
    else std::snprintf(b,sizeof(b),"%.1f KB",(double)n/1024.0);return b;
}

struct UpdateState {
    std::mutex mtx;bool checking=false,hasChecked=false;unsigned long long checkedMs=0;
    CheckResult cr;ManifestExt man;Plan plan;std::string manifestUrl,checkError,planJson;
    bool downloading=false;std::string phase="idle";long long downloaded=0,total=0;double speedBps=0;
    std::string dlError,dlVersion;bool staged=false;int stagedFiles=0;
    std::string shaActual,shaExpected;unsigned long long dlStartMs=0,dlEndMs=0;
    std::string stagingDir;int perFileFetches=0;
};
inline UpdateState& state(){static UpdateState value;return value;}
inline unsigned long long now_ms(){return rta::platform::monotonic_ms();}
inline constexpr unsigned long long kCheckTtlMs=300ull*1000ull;

// Receipts use a persisted CSPRNG id or an existing feedback client id.
inline std::string update_client_id(){
    static std::mutex mutex;static std::string cached;std::lock_guard<std::mutex> lock(mutex);
    if(!cached.empty())return cached;
    auto valid=[](const std::string& id){if(id.size()<8||id.size()>128)return false;
        for(unsigned char c:id)if(!std::isalnum(c)&&c!='-'&&c!='_')return false;return true;};
    for(const std::string path:{join_path(install_dir(),"rlplayer-feedback.conf"),conf_path()}){
        std::ifstream file(rta::platform::from_utf8(path));std::string line;
        while(std::getline(file,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();const size_t equal=line.find('=');
            if(equal==std::string::npos)continue;const std::string key=lower_ascii(line.substr(0,equal));
            if(key=="client"||key=="client_id"){const std::string id=line.substr(equal+1);if(valid(id)){cached=id;return cached;}}}
    }
    unsigned char random[16]{};if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return std::string();
    static const char hex[]="0123456789abcdef";for(unsigned char c:random){cached.push_back(hex[c>>4]);cached.push_back(hex[c&15]);}
    auto values=conf_read();values["client"]=cached;if(!conf_write(values))cached.clear();return cached;
}
inline bool post_update_report(const std::string& manifestUrl,const std::string& client,const std::string& from,
    const std::string& to,bool ok,long long ms,const std::string& errorCode){
    const std::string url=report_url_for(manifestUrl);if(url.empty()||client.empty())return false;
    const std::string body="{\"client\":\""+json_escape(client)+"\",\"from\":\""+json_escape(from)+
        "\",\"to\":\""+json_escape(to)+"\",\"ok\":"+(ok?"true":"false")+",\"ms\":"+std::to_string(std::max<long long>(0,ms))+
        ",\"errorCode\":\""+json_escape(errorCode)+"\"}";
    rta::platform::HttpRequest request;request.method="POST";request.url=url;request.body=body;request.timeoutMs=3000;
    request.allowRedirects=false;request.proxy=rta::net::http_proxy();
    request.headers={"User-Agent: RLPlayer/"+current_version(),"Content-Type: application/json"};
    auto response=rta::net::http_detail::send(request);return response.ok&&response.status>=200&&response.status<300;
}
inline void report_update_async(const std::string& from,const std::string& to,bool ok,long long ms,const std::string& errorCode){
    const std::string client=update_client_id(),url=manifest_url();if(client.empty())return;
    std::thread([=]{(void)post_update_report(url,client,from,to,ok,ms,errorCode);}).detach();
}
inline bool post_external_click(const std::string& manifestUrl,const std::string& target){
    if(target!="baidu"&&target!="github")return false;
    const std::string url=update_origin(manifestUrl)+"/api/v1/events/external";
    if(!url_allowed(url))return false;
    rta::platform::HttpRequest request;request.method="POST";request.url=url;
    request.body="{\"target\":\""+target+"\"}";request.timeoutMs=3000;request.allowRedirects=false;
    request.proxy=rta::net::http_proxy();request.headers={"User-Agent: RLPlayer/"+current_version(),"Content-Type: application/json"};
    auto response=rta::net::http_detail::send(request);return response.ok&&response.status>=200&&response.status<300;
}
inline void report_external_click_async(const std::string& target){
    const std::string url=manifest_url();std::thread([=]{(void)post_external_click(url,target);}).detach();
}

inline bool fetch_and_evaluate(int timeoutMs,std::string& err){
    timeoutMs=std::max(1,std::min(timeoutMs<=0?8000:timeoutMs,10000));const std::string current=current_version(),url=manifest_url();
    rta::platform::HttpRequest request;request.method="GET";request.url=url;request.timeoutMs=timeoutMs;
    request.allowRedirects=false;request.proxy=rta::net::http_proxy();request.headers={"User-Agent: RLPlayer/"+current,"X-RLPlayer-Client: "+update_client_id(),"X-RLPlayer-Platform: windows"};
    auto response=rta::net::http_detail::send(request);const std::string body(response.body.begin(),response.body.end());
    CheckResult result=evaluate_update_check(current,response.ok,response.status,body);
    auto& shared=state();{
        std::lock_guard<std::mutex> lock(shared.mtx);shared.cr=result;shared.manifestUrl=url;shared.man=ManifestExt{};
        shared.plan=Plan{};shared.planJson.clear();shared.checkError=result.ok()?std::string():result.error;
    }
    if(!result.ok()){err=result.error;return false;}
    if(result.decision!=Decision::UpdateAvailable)return true;
    auto verified=verify_windows_update_envelope(body);
    ManifestExt manifest=parse_manifest_payload(verified.payload,current,(long long)std::time(nullptr),kWindowsUpdateChannel);
    if(!manifest.ok){err=manifest.error;std::lock_guard<std::mutex> lock(shared.mtx);shared.checkError=err;
        shared.cr.decision=Decision::FailedParse;shared.cr.error=err;return false;}
    Plan plan;if(manifest.manual){plan.ok=true;plan.manual=true;plan.reason=manifest.reason;}
    else plan=plan_incremental(manifest.files,install_dir(),manifest.deletes);
    if(!plan.ok){err=plan.error;return false;}
    std::string planJson="[";int count=0;for(const auto& item:plan.items){if(item.same)continue;
        if(count++)planJson+=",";if(count>60){planJson+="\"...\"";break;}
        planJson+="{\"path\":\""+json_escape(item.path)+"\",\"size\":"+std::to_string(item.wantSize)+
            ",\"objectSize\":"+std::to_string(item.objectSize)+",\"have\":\""+json_escape(item.haveSha)+"\"}";}
    planJson+="]";
    {std::lock_guard<std::mutex> lock(shared.mtx);shared.man=manifest;shared.plan=plan;shared.planJson=planJson;
        if(!skip_version().empty()&&cmp_versions(skip_version(),manifest.version)==0){shared.cr.decision=Decision::UpToDate;shared.cr.error.clear();}}
    return true;
}
inline std::string check_json(){
    auto& shared=state();CheckResult result;Plan plan;ManifestExt manifest;std::string error,url,planJson;
    bool checking=false,checked=false;unsigned long long at=0;
    {std::lock_guard<std::mutex> lock(shared.mtx);result=shared.cr;plan=shared.plan;manifest=shared.man;
        error=shared.checkError;url=shared.manifestUrl;planJson=shared.planJson;checking=shared.checking;
        checked=shared.hasChecked;at=shared.checkedMs;}
    const long long age=checked?(long long)((now_ms()-at)/1000):-1;
    const bool manual=manifest.manual||plan.manual;
    const std::string& manualReason=plan.reason.empty()?manifest.reason:plan.reason;
    std::string j="{";
    j+="\"ok\":"+std::string(result.ok()?"true":"false")+",\"decision\":\""+result.code()+"\"";
    j+=",\"current\":\""+json_escape(current_version())+"\",\"latest\":\""+json_escape(result.remoteVersion)+"\"";
    j+=",\"hasUpdate\":"+std::string(result.available()?"true":"false")+",\"upToDate\":"+(result.up_to_date()?"true":"false");
    j+=",\"error\":\""+json_escape(error)+"\",\"errorCode\":\""+json_escape(result.errorCode)+"\"";
    // 2026-11-15：外语界面按 errorCode 给本地化文案；HTTP 状态码单独给出来，
    // 前端才能拼「Update server returned HTTP 429」，而不是回落中文整句。
    j+=",\"httpStatus\":"+std::to_string(result.status);
    j+=",\"notes\":\""+json_escape(result.notes)+"\",\"publishedAt\":\""+json_escape(manifest.publishedAt)+"\"";
    j+=",\"expiresAt\":\""+json_escape(manifest.expiresAt)+"\",\"manifestUrl\":\""+json_escape(url)+"\"";
    j+=",\"checking\":"+std::string(checking?"true":"false")+",\"checked\":"+(checked?"true":"false")+",\"ageSec\":"+std::to_string(age);
    j+=",\"cached\":"+std::string(checked&&!checking&&age>=0&&age<300?"true":"false");
    j+=",\"skipVersion\":\""+json_escape(skip_version())+"\",\"filesTotal\":"+std::to_string((long long)manifest.files.size());
    j+=",\"filesToDownload\":"+std::to_string(plan.toDownload)+",\"filesUnchanged\":"+std::to_string(plan.unchanged);
    j+=",\"downloadBytes\":"+std::to_string(plan.downloadBytes)+",\"skippedBytes\":"+std::to_string(plan.skippedBytes);
    j+=",\"downloadText\":\""+json_escape(size_text(plan.downloadBytes))+"\",\"skippedText\":\""+json_escape(size_text(plan.skippedBytes))+"\"";
    j+=",\"manual\":"+std::string(manual?"true":"false")+",\"installMode\":\""+json_escape(manual?"manual":manifest.installMode)+"\"";
    j+=",\"cn\":\""+json_escape(manifest.manualInfo.cnUrl)+"\",\"global\":\""+json_escape(manifest.manualInfo.globalUrl)+"\"";
    j+=",\"extractionCode\":\""+json_escape(manifest.manualInfo.extractionCode)+"\",\"fullSha\":\""+json_escape(manifest.manualInfo.sha256)+"\"";
    j+=",\"fullSize\":"+std::to_string(manifest.manualInfo.size)+",\"fullBytes\":"+std::to_string(manual?manifest.manualInfo.size:0);
    j+=",\"reason\":\""+json_escape(manualReason)+"\",\"deltaBytes\":0,\"deltaUrl\":\"\",\"url\":\"\",\"sizeBytes\":0";
    j+=",\"prerelease\":"+std::string(parse_version(result.remoteVersion).has_pre()?"true":"false");
    j+=",\"files\":"+(planJson.empty()?"[]":planJson)+",\"deletes\":"+std::to_string((long long)plan.deletes.size());
    j+=",\"installDir\":\""+json_escape(install_dir())+"\"}";return j;
}
inline void check_worker(){std::string error;fetch_and_evaluate(8000,error);auto& shared=state();
    std::lock_guard<std::mutex> lock(shared.mtx);shared.hasChecked=true;shared.checkedMs=now_ms();shared.checking=false;
    if(shared.phase=="checking")shared.phase="idle";}
inline bool start_check(bool force){auto& shared=state();{std::lock_guard<std::mutex> lock(shared.mtx);
    // Cache failures too: UI completion polling must not start a new HTTP request
    // after every error. Keep the original error; explicit force bypasses this TTL.
    if(shared.checking)return true;if(!force&&shared.hasChecked&&now_ms()-shared.checkedMs<kCheckTtlMs)return true;
    shared.checking=true;if(shared.phase=="idle")shared.phase="checking";}std::thread(check_worker).detach();return true;}
inline bool check_now(int timeoutMs){auto& shared=state();{std::lock_guard<std::mutex> lock(shared.mtx);shared.checking=true;}
    std::string error;const bool ok=fetch_and_evaluate(timeoutMs,error);{std::lock_guard<std::mutex> lock(shared.mtx);
        shared.hasChecked=true;shared.checkedMs=now_ms();shared.checking=false;if(!ok&&shared.checkError.empty())shared.checkError=error;}return ok;
}

// ---------------------------------------------------------------------------
// Range/ETag object downloader. No alternative host and no full fallback exists.
// ---------------------------------------------------------------------------
struct ContentRange { bool valid=false,unsatisfiable=false;long long start=0,end=-1,total=-1; };
inline bool parse_decimal(const std::string& text,long long& value){
    if(text.empty()||text.size()>18)return false;long long n=0;for(char c:text){if(c<'0'||c>'9')return false;
        if(n>(9223372036854775807LL-(c-'0'))/10)return false;n=n*10+c-'0';}value=n;return true;
}
inline ContentRange parse_content_range(const std::string& raw){
    ContentRange out;std::string text=detail::trim(raw);if(text.rfind("bytes ",0)!=0)return out;text=text.substr(6);
    const size_t slash=text.find('/');if(slash==std::string::npos||!parse_decimal(text.substr(slash+1),out.total))return out;
    const std::string range=text.substr(0,slash);if(range=="*"){out.valid=out.unsatisfiable=true;return out;}
    const size_t dash=range.find('-');if(dash==std::string::npos||!parse_decimal(range.substr(0,dash),out.start)||
       !parse_decimal(range.substr(dash+1),out.end)||out.end<out.start||out.total<=0||out.end>=out.total)return out;
    out.valid=true;return out;
}
inline long long retry_after_seconds(const std::string& value){
    long long seconds=0;if(parse_decimal(detail::trim(value),seconds))return std::min<long long>(seconds,86400);
    if(value.empty())return -1;SYSTEMTIME system{};if(!WinHttpTimeToSystemTime(rta::platform::detail::widen(value).c_str(),&system))return -1;
    FILETIME file{};if(!SystemTimeToFileTime(&system,&file))return -1;ULARGE_INTEGER ticks{};ticks.LowPart=file.dwLowDateTime;
    ticks.HighPart=file.dwHighDateTime;const long long target=(long long)(ticks.QuadPart/10000000ULL-11644473600LL);
    return std::max<long long>(0,target-(long long)std::time(nullptr));
}
struct ObjectDownloadResult { bool ok=false;std::string error,errorCode,path,sha256;int attempts=0;long long bytes=0; };

inline ObjectDownloadResult download_object_with_resume(const ManifestFile& file,const std::string& manifestUrl,
                                                        const std::string& workDir,int timeoutMs=30000){
    ObjectDownloadResult result;const std::string url=object_url_for(manifestUrl,file.object);
    if(url.empty()){result.errorCode="bad-object-url";result.error="对象 URL 非法";return result;}
    if(!ensure_dir(workDir)){result.errorCode="io";result.error="无法创建对象目录";return result;}
    const std::string part=join_path(workDir,file.object+".part"),meta=part+".meta",chunk=part+".chunk",finalPath=join_path(workDir,file.object);
    auto remove=[](const std::string& path){rta::platform::remove_file(rta::platform::from_utf8(path));};
    auto finish=[&](int attempts)->ObjectDownloadResult{
        result.attempts=attempts;VerifyResult verified=verify_downloaded_file(part,file.objectSha256,file.objectSize);
        if(!verified.ok){remove(part);remove(meta);result.errorCode=verified.reason==VerifyFailReason::SizeMismatch?"object-size":"object-sha256";
            result.error=verified.error;return result;}
        remove(finalPath);if(!rta::platform::move_file(rta::platform::from_utf8(part),rta::platform::from_utf8(finalPath),true)){
            result.errorCode="io";result.error="无法提交下载对象";return result;}
        remove(meta);result.ok=true;result.path=finalPath;result.sha256=verified.actualSha;result.bytes=file.objectSize;return result;
    };
    if(file_size(part)==file.objectSize&&file.objectSize>0){
        auto ready=finish(0);if(ready.ok||path_exists(part))return ready;
        result=ObjectDownloadResult{};
    }
    if(file_size(part)>file.objectSize){remove(part);remove(meta);}
    for(int attempt=1;attempt<=4;++attempt){
        result.attempts=attempt;const long long offset=std::max<long long>(0,file_size(part));
        remove(chunk);std::ofstream output(rta::platform::from_utf8(chunk),std::ios::binary|std::ios::trunc);
        if(!output){result.errorCode="io";result.error="无法写入 .part";return result;}
        bool writeFailed=false,tooLarge=false;long long received=0;std::string preview;
        rta::platform::HttpRequest request;request.method="GET";request.url=url;request.timeoutMs=timeoutMs;
        request.allowRedirects=false;request.discardBody=true;request.proxy=rta::net::http_proxy();
        request.headers={"User-Agent: RLPlayer/"+current_version(),"Accept: application/zip","X-RLPlayer-Client: "+update_client_id(),"X-RLPlayer-Platform: windows"};
        std::string etag;
        {std::ifstream in(rta::platform::from_utf8(meta));std::string line;while(std::getline(in,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.rfind("etag=",0)==0){etag=line.substr(5);break;}}}
        if(offset>0){request.headers.push_back("Range: bytes="+std::to_string(offset)+"-");
            if(!etag.empty())request.headers.push_back("If-Range: "+etag);}
        request.onChunk=[&](const char* data,size_t size){
            if(preview.size()<4096)preview.append(data,std::min(size,4096-preview.size()));
            if(received+(long long)size>file.objectSize){tooLarge=true;return;}
            output.write(data,(std::streamsize)size);if(!output){writeFailed=true;return;}received+=(long long)size;
            auto& shared=state();std::lock_guard<std::mutex> lock(shared.mtx);shared.downloaded+=(long long)size;
        };
        auto response=rta::net::http_detail::send(request);output.close();
        if(response.status==429){remove(chunk);result.errorCode="quota_exceeded";result.error="更新对象额度已用尽";
            const long long wait=retry_after_seconds(response.retryAfter);if(wait>60||attempt==4)return result;
            if(wait>0)std::this_thread::sleep_for(std::chrono::seconds(wait));continue;}
        if(response.status==416){remove(chunk);remove(part);remove(meta);if(attempt==4){result.errorCode="range";result.error="Range 不可满足";return result;}continue;}
        const bool retryable=response.status==0||response.status==408||response.status==425||
            response.status==500||response.status==502||response.status==503||response.status==504;
        if(retryable){remove(chunk);if(attempt==4)break;const long long wait=retry_after_seconds(response.retryAfter);
            if(wait>60){result.errorCode="retry-after";result.error="服务器要求过长的重试等待";return result;}
            std::this_thread::sleep_for(std::chrono::milliseconds(wait>=0?wait*1000:100*attempt));continue;}
        if(writeFailed||tooLarge){remove(chunk);result.errorCode="object-size";result.error="对象写入失败或超过声明大小";return result;}
        if(response.status!=200&&response.status!=206){remove(chunk);result.errorCode="http";
            result.error="对象下载 HTTP "+std::to_string(response.status);return result;}
        if(response.contentLength>=0&&response.contentLength!=received){remove(chunk);result.errorCode="content-length";
            result.error="Content-Length 与实际对象块不符";return result;}
        if(response.status==200){if(received!=file.objectSize){remove(chunk);result.errorCode="object-size";result.error="完整对象大小不符";return result;}
            if(!rta::platform::move_file(rta::platform::from_utf8(chunk),rta::platform::from_utf8(part),true)){
                remove(chunk);result.errorCode="io";result.error="无法替换 .part";return result;}
        }else{
            const ContentRange range=parse_content_range(response.contentRange);
            if(!range.valid||range.unsatisfiable||range.start!=offset||range.total!=file.objectSize||
               range.end-range.start+1!=received){remove(chunk);result.errorCode="bad-content-range";result.error="206 Content-Range 不严格匹配";return result;}
            const std::string combined=part+".combined";std::ofstream merged(rta::platform::from_utf8(combined),std::ios::binary|std::ios::trunc);
            std::ifstream old(rta::platform::from_utf8(part),std::ios::binary),added(rta::platform::from_utf8(chunk),std::ios::binary);
            merged<<old.rdbuf()<<added.rdbuf();old.close();added.close();merged.close();
            if(!merged||file_size(combined)!=file.objectSize){remove(combined);remove(chunk);result.errorCode="object-size";result.error="续传合并大小不符";return result;}
            if(!rta::platform::move_file(rta::platform::from_utf8(combined),rta::platform::from_utf8(part),true)){
                remove(combined);remove(chunk);result.errorCode="io";result.error="无法提交续传 .part";return result;}remove(chunk);
        }
        if(!response.etag.empty())write_text_file(meta,"etag="+response.etag+"\r\n");
        else if(response.status==200&&etag.empty())remove(meta);
        return finish(attempt);
    }
    result.errorCode=result.errorCode.empty()?"network":result.errorCode;if(result.error.empty())result.error="对象下载有界重试失败";return result;
}

// ---------------------------------------------------------------------------
// ZIP central-directory policy, then system tar extraction and raw rehash.
// ---------------------------------------------------------------------------
inline unsigned zip16(const unsigned char* p){return(unsigned)p[0]|((unsigned)p[1]<<8);}
inline unsigned long zip32(const unsigned char* p){return(unsigned long)zip16(p)|((unsigned long)zip16(p+2)<<16);}
inline bool zip_validate_single_file(const std::string& zipPath,const std::string& expectedRel,long long expectedSize,std::string& error){
    error.clear();std::ifstream file(rta::platform::from_utf8(zipPath),std::ios::binary);if(!file){error="ZIP 不可读";return false;}
    const long long size=file_size(zipPath);if(size<22||size>2147483647LL){error="ZIP 大小非法";return false;}
    const long long tailSize=std::min<long long>(size,65557);file.seekg(size-tailSize);std::string tail((size_t)tailSize,'\0');
    file.read(tail.data(),tailSize);if(!file){error="ZIP 尾部读取失败";return false;}
    size_t eocd=std::string::npos;for(size_t i=tail.size()-22;;--i){if(zip32((const unsigned char*)tail.data()+i)==0x06054b50){eocd=i;break;}if(i==0)break;}
    if(eocd==std::string::npos){error="ZIP EOCD 缺失";return false;}const auto* end=(const unsigned char*)tail.data()+eocd;
    const unsigned comment=zip16(end+20);if(eocd+22+comment!=tail.size()||zip16(end+4)||zip16(end+6)||zip16(end+8)!=1||zip16(end+10)!=1){
        error="ZIP 必须为单卷单文件且无尾部垃圾";return false;}
    const unsigned long centralSize=zip32(end+12),centralOffset=zip32(end+16),eocdOffset=size-tailSize+eocd;
    if(centralSize>1048576||centralOffset+centralSize!=eocdOffset){error="ZIP 中央目录越界";return false;}
    file.seekg(centralOffset);std::string central((size_t)centralSize,'\0');file.read(central.data(),centralSize);if(!file||centralSize<46){error="ZIP 中央目录不可读";return false;}
    const auto* c=(const unsigned char*)central.data();if(zip32(c)!=0x02014b50){error="ZIP 中央项签名非法";return false;}
    const unsigned flags=zip16(c+8),method=zip16(c+10),nameSize=zip16(c+28),extraSize=zip16(c+30),commentSize=zip16(c+32);
    const unsigned long rawSize=zip32(c+24),compressedSize=zip32(c+20),external=zip32(c+38),localOffset=zip32(c+42);
    if((flags&1)||(method!=0&&method!=8)||46u+nameSize+extraSize+commentSize!=centralSize||
       rawSize!=(unsigned long)expectedSize||(external&0x10)||((external>>16)&0xf000)==0xa000){error="ZIP 加密/方法/大小/文件类型非法";return false;}
    const std::string name=central.substr(46,nameSize);std::string expected=expectedRel;for(char& ch:expected)if(ch=='\\')ch='/';
    if(name!=expected||!safe_install_rel(expectedRel)){error="ZIP 路径穿越或文件名不匹配 name="+name+" expected="+expected+
        " safe="+std::to_string(safe_install_rel(expectedRel));return false;}
    file.seekg(localOffset);std::string local(30,'\0');file.read(local.data(),30);if(!file||zip32((const unsigned char*)local.data())!=0x04034b50){error="ZIP 本地头非法";return false;}
    const auto* l=(const unsigned char*)local.data();const unsigned localName=zip16(l+26),localExtra=zip16(l+28);
    if(zip16(l+6)!=flags||zip16(l+8)!=method||localName!=nameSize){error="ZIP 本地头与中央目录不一致 flags="+
        std::to_string(zip16(l+6))+"/"+std::to_string(flags)+" method="+std::to_string(zip16(l+8))+"/"+std::to_string(method)+
        " name="+std::to_string(localName)+"/"+std::to_string(nameSize);return false;}
    std::string localNameText(localName,'\0');file.read(localNameText.data(),localName);if(!file||localNameText!=name){error="ZIP 本地文件名不匹配";return false;}
    if((unsigned long long)localOffset+30+localName+localExtra+compressedSize>centralOffset){error="ZIP 数据越界";return false;}
    return true;
}
inline bool unzip_to(const std::string& zip,const std::string& dst,std::string& error){
    rmtree(dst);if(!ensure_dir(dst)){error="无法创建解压目录";return false;}
    const std::string command="\""+tar_exe()+"\" -xf \""+zip+"\" -C \""+dst+"\"";
    auto process=rta::platform::run_hidden(command,true);if(!process.started){error="无法启动 tar.exe";return false;}
    if(process.exit_code!=0){error="ZIP 解压失败（tar 退出码 "+std::to_string(process.exit_code)+"）";return false;}return true;
}
inline bool extract_verified_object(const std::string& zip,const ManifestFile& file,const std::string& unpack,
                                    const std::string& destination,std::string& error){
    VerifyResult zipResult=verify_downloaded_file(zip,file.objectSha256,file.objectSize);
    if(!zipResult.ok){error=zipResult.error;return false;}
    if(!zip_validate_single_file(zip,"payload.bin",file.size,error))return false;
    if(!unzip_to(zip,unpack,error))return false;
    const std::string source=join_path(unpack,"payload.bin");VerifyResult raw=verify_downloaded_file(source,file.sha256,file.size);
    if(!raw.ok){error="ZIP raw 复核失败："+raw.error;return false;}
    std::vector<std::string> files;list_files_rel(unpack,"",files);
    if(files.size()!=1||norm_rel2(files[0])!="payload.bin"){error="ZIP 含有额外文件";return false;}
    const size_t slash=destination.find_last_of("\\/");if(slash!=std::string::npos&&!ensure_dir(destination.substr(0,slash))){error="无法创建暂存目录";return false;}
    if(!rta::platform::move_file(rta::platform::from_utf8(source),rta::platform::from_utf8(destination),true)){error="无法移入暂存文件";return false;}
    rmtree(unpack);return true;
}

// ---------------------------------------------------------------------------
// Staging completeness and worker.
// ---------------------------------------------------------------------------
inline std::string staging_marker_path(const std::string& staging){return join_path(staging,"complete.txt");}
inline std::vector<std::string> staging_deletes(const std::string& staging){
    std::vector<std::string> out;const std::string text=read_text_file(staging_marker_path(staging));size_t pos=0;
    while(pos<text.size()){size_t end=text.find('\n',pos);if(end==std::string::npos)end=text.size();
        std::string line=text.substr(pos,end-pos);pos=end+1;if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.rfind("delete=",0)==0&&safe_install_rel(line.substr(7)))out.push_back(line.substr(7));}return out;
}
inline bool staging_verify(const std::string& staging,std::string& error){
    error.clear();if(!is_dir(staging)){error="暂存目录不存在";return false;}
    const std::string text=read_text_file(staging_marker_path(staging));if(text.empty()){error="暂存缺少 complete.txt";return false;}
    const bool signedStaging=text.rfind("version=",0)==0;
    int files=0;size_t pos=0;while(pos<text.size()){size_t end=text.find('\n',pos);if(end==std::string::npos)end=text.size();
        std::string line=text.substr(pos,end-pos);pos=end+1;if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.rfind("delete=",0)==0){if(!safe_install_rel(line.substr(7))){error="删除台账路径非法";return false;}continue;}
        if(line.rfind("file=",0)!=0)continue;const std::string rest=line.substr(5);const size_t a=rest.find('\t'),b=a==std::string::npos?a:rest.find('\t',a+1);
        if(a==std::string::npos||(signedStaging&&b==std::string::npos)){error="完成标记格式损坏";return false;}
        const std::string rel=rest.substr(0,a),sizeText=rest.substr(a+1,(b==std::string::npos?rest.size():b)-a-1),
            sha=b==std::string::npos?std::string():rest.substr(b+1);long long expected=0;
        if(!safe_install_rel(rel)||!parse_decimal(sizeText,expected)||(signedStaging&&!is_hex64(sha))||
           (!sha.empty()&&!is_hex64(sha))){error="完成标记含非法文件";return false;}
        if(is_protected_rel(rel)){if(signedStaging){error="新协议暂存含受保护文件";return false;}continue;}
        if(sha.empty()){if(file_size(join_path(staging,rel))!=expected){error="旧暂存文件大小不符："+rel;return false;}}
        else{VerifyResult result=verify_downloaded_file(join_path(staging,rel),sha,expected);
            if(!result.ok){error="暂存校验失败："+rel+"："+result.error;return false;}}++files;}
    if(files==0&&staging_deletes(staging).empty()){error="完成标记没有文件或删除项";return false;}return true;
}

inline void download_worker(){
    auto& shared=state();ManifestExt manifest;Plan plan;std::string manifestUrl;
    {std::lock_guard<std::mutex> lock(shared.mtx);manifest=shared.man;plan=shared.plan;manifestUrl=shared.manifestUrl;}
    const std::string version=manifest.version,dir=version_dir(version),staging=join_path(dir,"staging"),objects=join_path(dir,"objects");
    auto fail=[&](const std::string& error,const std::string& code){unsigned long long elapsed=0;{
        std::lock_guard<std::mutex> lock(shared.mtx);shared.phase="error";
        shared.dlError=(code=="quota_exceeded"?"quota_exceeded：":"")+error;shared.downloading=false;
        shared.staged=false;shared.dlEndMs=now_ms();elapsed=shared.dlEndMs-shared.dlStartMs;}
        report_update_async(current_version(),version,false,(long long)elapsed,code);};
    if(manifest.manual||plan.manual){fail("该版本只能手动更新","manual-required");return;}
    if(!ensure_dir(dir)||!ensure_dir(objects)){fail("无法创建更新工作目录","io");return;}
    rmtree(staging);if(!ensure_dir(staging)){fail("无法创建 staging","io");return;}
    std::map<std::string,ManifestFile> byPath;for(const auto& file:manifest.files)byPath[norm_rel2(file.path)]=file;
    for(const auto& rel:plan.missing){const auto it=byPath.find(norm_rel2(rel));if(it==byPath.end()){fail("计划缺少清单对象","plan");return;}
        {std::lock_guard<std::mutex> lock(shared.mtx);shared.phase="downloading-one-by-one";shared.shaExpected=it->second.objectSha256;}
        auto downloaded=download_object_with_resume(it->second,manifestUrl,objects,30000);
        if(!downloaded.ok){fail("对象下载失败："+rel+"："+downloaded.error,downloaded.errorCode);return;}
        {std::lock_guard<std::mutex> lock(shared.mtx);shared.phase="verifying";shared.shaActual=downloaded.sha256;++shared.perFileFetches;}
        const std::string unpack=join_path(dir,"unpacked"),destination=join_path(staging,rel);std::string error;
        if(!extract_verified_object(downloaded.path,it->second,unpack,destination,error)){fail("对象解压/raw 校验失败："+rel+"："+error,"zip-or-raw");return;}
    }
    {std::lock_guard<std::mutex> lock(shared.mtx);shared.phase="verifying";}
    std::string marker="version="+version+"\r\n";for(const auto& item:plan.items){if(item.same)continue;
        marker+="file="+item.path+"\t"+std::to_string(item.wantSize)+"\t"+item.wantSha+"\r\n";}
    for(const auto& rel:plan.deletes)marker+="delete="+rel+"\r\n";
    marker+="at="+std::to_string((long long)std::time(nullptr))+"\r\n";
    if(!write_text_file(staging_marker_path(staging),marker)){fail("无法写完成标记","io");return;}
    std::string error;if(!staging_verify(staging,error)){fail("暂存自检失败："+error,"staging");return;}
    {std::lock_guard<std::mutex> lock(shared.mtx);shared.phase="ready";shared.downloading=false;shared.staged=true;
        shared.stagedFiles=count_files(staging);shared.dlVersion=version;shared.stagingDir=staging;shared.dlEndMs=now_ms();}
}
inline bool start_download(std::string& error){
    auto& shared=state();{std::lock_guard<std::mutex> lock(shared.mtx);if(shared.downloading)return true;
        if(shared.cr.decision!=Decision::UpdateAvailable){error=shared.cr.up_to_date()?"已经是最新版本":"请先检查更新";return false;}
        if(shared.man.manual||shared.plan.manual){error="该版本需要手动更新，不会自动下载";return false;}
        if(shared.plan.toDownload==0&&shared.plan.deletes.empty()){error="没有需要下载或删除的文件";return false;}
        shared.downloading=true;shared.phase="downloading";shared.downloaded=0;shared.total=shared.plan.downloadBytes;
        shared.speedBps=0;shared.dlError.clear();shared.dlVersion=shared.man.version;shared.staged=false;shared.stagedFiles=0;
        shared.shaActual.clear();shared.shaExpected.clear();shared.dlStartMs=now_ms();shared.dlEndMs=0;shared.perFileFetches=0;
        shared.stagingDir=join_path(version_dir(shared.man.version),"staging");}
    std::thread(download_worker).detach();return true;
}
inline std::string progress_json(){
    auto& shared=state();std::string phase,error,version,shaA,shaE,staging;long long downloaded=0,total=0;double speed=0;
    bool staged=false;int files=0,perFile=0;unsigned long long begin=0,end=0;
    {std::lock_guard<std::mutex> lock(shared.mtx);phase=shared.phase;error=shared.dlError;version=shared.dlVersion;
        downloaded=shared.downloaded;total=shared.total;speed=shared.speedBps;staged=shared.staged;files=shared.stagedFiles;
        shaA=shared.shaActual;shaE=shared.shaExpected;begin=shared.dlStartMs;end=shared.dlEndMs;perFile=shared.perFileFetches;staging=shared.stagingDir;}
    const double percent=total>0?100.0*(double)downloaded/(double)total:0;
    const unsigned long long elapsed=end>begin?end-begin:(begin>0?now_ms()-begin:0);std::string j="{\"ok\":true";
    j+=",\"phase\":\""+json_escape(phase)+"\",\"version\":\""+json_escape(version)+"\",\"downloaded\":"+std::to_string(downloaded);
    j+=",\"total\":"+std::to_string(total)+",\"pct\":"+std::to_string((int)(percent*10)/10.0)+",\"speed\":"+std::to_string((long long)speed);
    j+=",\"speedText\":\""+json_escape(speed>0?size_text((long long)speed)+"/s":"—")+"\",\"elapsedMs\":"+std::to_string(elapsed);
    j+=",\"staged\":"+std::string(staged?"true":"false")+",\"files\":"+std::to_string(files)+",\"perFileFetches\":"+std::to_string(perFile);
    j+=",\"sha256\":\""+json_escape(shaA)+"\",\"expectedSha256\":\""+json_escape(shaE)+"\",\"stagingDir\":\""+json_escape(staging)+"\"";
    j+=",\"error\":\""+json_escape(error)+"\",\"canApply\":"+std::string(staged&&phase=="ready"?"true":"false")+"}";return j;
}
inline void restore_staging_state(){
    auto& shared=state();std::lock_guard<std::mutex> lock(shared.mtx);if(shared.staged)return;
    std::vector<rta::platform::DirEntry> entries;if(!rta::platform::list_directory(rta::platform::from_utf8(updates_root()),entries))return;
    std::string best;for(const auto& entry:entries){if(!entry.is_dir)continue;const std::string bare=
        (!entry.name.empty()&&(entry.name[0]=='v'||entry.name[0]=='V'))?entry.name.substr(1):entry.name;
        const std::string staging=join_path(version_dir(bare),"staging");std::string error;
        if(staging_verify(staging,error)&&(best.empty()||cmp_versions(bare,best)>0))best=bare;}
    if(best.empty())return;const std::string staging=join_path(version_dir(best),"staging");shared.staged=true;
    shared.stagedFiles=count_files(staging);shared.dlVersion=best;shared.stagingDir=staging;shared.phase="ready";
}

} // namespace rta::update
