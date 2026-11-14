// SPDX-License-Identifier: GPL-3.0-or-later
// Portable-profile artwork recovery. Startup writes only missing records;
// probe/read are strictly read-only and never turn history into online evidence.
#pragma once
#include "platform/artwork_file_io.h"
#include "net/update_check.h" // Shared strict JSON, SHA-256 and numeric version primitives.
#include <filesystem>
#include <fstream>
#include <atomic>
#include <array>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <tuple>
#include <charconv>

namespace rta::net::artwork_migration {
namespace fs = std::filesystem;
struct HistoryMember {std::string path,cuePath;long long cueNo=0,cueOff=0,cueLen=0;};
struct HistoryAlbumBinding {
    std::string key, legacyKey, album, artist, identitySource;
    std::string membersSha256;
    size_t memberCount=0;
};
inline std::string history_members_sha256(const std::vector<HistoryMember>&members);
struct MigrationReport {
    size_t profiles=0, typedRecords=0, metaRecords=0, historyRecords=0, rejected=0, failed=0;
};
struct HistoryInfo {
    std::string sha256, mime, sourceVersion, sourceKey;
    size_t bytes=0;
    std::string source="history";
};
namespace detail {
constexpr size_t maxImage = 24u*1024*1024, maxIndex = 16u*1024*1024, maxMeta = 2u*1024*1024;
struct Budget {
    size_t remaining=512u*1024*1024, entries=65536;
    bool take(size_t n) { if(n>remaining)return false;remaining-=n;return true; }
    bool next() { if(!entries)return false;--entries;return true; }
};
inline fs::path path(const std::string&s) { return fs::u8path(s); }
inline std::string utf8(const fs::path&p) { auto s=p.u8string();return std::string(reinterpret_cast<const char*>(s.data()),s.size()); }
inline std::string hash(const std::string&s) { return rta::update::sha256_hex(s); }
inline std::string member_hash(const std::vector<HistoryMember>&members) {
    if(members.empty()||members.size()>65536)return {};
    std::vector<std::array<std::string,5>> tuples;
    for(const auto&m:members)tuples.push_back({m.path,m.cuePath,std::to_string(m.cueNo),std::to_string(m.cueOff),std::to_string(m.cueLen)});
    std::sort(tuples.begin(),tuples.end());std::string bytes="rlplayer-history-members-v1\n";
    for(const auto&t:tuples)for(const auto&field:t){if(field.size()>maxIndex||bytes.size()>maxIndex-field.size())return {};bytes+=std::to_string(field.size())+":"+field;}
    return hash(bytes);
}
inline bool hex(const std::string&s,size_t n) {
    return s.size()==n&&std::all_of(s.begin(),s.end(),[](unsigned char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
inline std::string fnv(const std::string&s) {
    uint64_t h=1469598103934665603ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}
    char b[24];std::snprintf(b,sizeof b,"%016llx",(unsigned long long)h);return b;
}
inline std::string lower(std::string s) { for(char&c:s)if(c>='A'&&c<='Z')c+=32;return s; }
inline std::string normalized_track(std::string s) { for(char&c:s)if(c=='/')c='\\';while(s.size()>1&&s.back()=='\\')s.pop_back();return lower(s); }
inline std::string quote(const std::string&s) {
    std::string out="\"";const char* h="0123456789abcdef";
    for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=(char)c;}else if(c<32){out+="\\u00";out+=h[c>>4];out+=h[c&15];}else out+=(char)c;}
    return out+"\"";
}
// Check every existing component. In particular, std::filesystem::is_symlink
// alone misses Windows directory junctions and other reparse point types.
inline bool safe(const fs::path&p) {return rta::platform::artwork_io::safe(p);}
inline bool present(const fs::path&p) {return rta::platform::artwork_io::present(p);}
inline bool read(const fs::path&p,size_t maximum,std::string&bytes) {return rta::platform::artwork_io::read(p,maximum,bytes);}
inline bool json(const fs::path&p,size_t maximum,Json&j,std::string*raw=nullptr) {
    std::string s;if(!read(p,maximum,s)||!json_parse_strict(s,j)||j.type!=Json::Obj)return false;if(raw)*raw=std::move(s);return true;
}
inline bool image(const std::string&s) {
    if(s.size()<=128||s.size()>maxImage)return false;
    const auto b=(const unsigned char*)s.data();
    return (b[0]==0xff&&b[1]==0xd8&&b[2]==0xff)||
      (s.size()>=8&&s.compare(0,8,"\x89PNG\r\n\x1a\n",8)==0)||
      s.compare(0,6,"GIF87a")==0||s.compare(0,6,"GIF89a")==0||
      (s.size()>=12&&s.compare(0,4,"RIFF")==0&&s.compare(8,4,"WEBP")==0);
}
inline std::string mime(const std::string&s) {
    if(s.compare(1,3,"PNG")==0)return "image/png";
    if(s.compare(0,3,"GIF")==0)return "image/gif";
    if(s.compare(0,4,"RIFF")==0)return "image/webp";return "image/jpeg";
}
// CREATE_NEW + rename without REPLACE_EXISTING preserves current user writes.
// A blob may survive a failed final declaration; retries verify/reuse that blob.
inline bool publish(const fs::path&p,const std::string&bytes) {return rta::platform::artwork_io::publish(p,bytes);}
inline bool blob(const fs::path&p,const std::string&bytes) {
    if(present(p)){std::string prior;return read(p,maxImage,prior)&&prior==bytes;}
    return publish(p,bytes);
}
struct Profile {fs::path dir;std::string version;std::vector<long long> nums;bool previous=false;};
inline bool profile(const fs::path&p,Profile&out) {
    static const std::regex pattern("^RLPlayer-([0-9]{1,9}\\.[0-9]{1,9}\\.[0-9]{1,9})-win64(\\.previous-[a-f0-9]{32})?$");
    std::smatch m;const std::string name=utf8(p.filename());if(!std::regex_match(name,m,pattern))return false;
    auto v=rta::update::parse_version(m[1]);if(!v.valid)return false;
    out={p,m[1],v.nums,m[2].matched};return safe(p);
}
inline std::vector<Profile> profiles(const fs::path&root,const std::string&currentVersion={}) {
    Profile self;const bool named=profile(root,self);
    if(!currentVersion.empty()){
        auto version=rta::update::parse_version(currentVersion);
        if(!version.valid||version.has_pre()||version.nums.size()!=3)return {};
        self.nums=version.nums;
    }else if(!named)return {};
    std::vector<Profile> out;std::error_code ec;size_t count=0;
    for(fs::directory_iterator it(root.parent_path(),ec),end;!ec&&it!=end&&count++<4096;it.increment(ec)){
        Profile p;if(it->path()==root||!profile(it->path(),p)||!it->is_directory(ec))continue;
        if(p.nums>self.nums)continue;out.push_back(p);
    }
    std::sort(out.begin(),out.end(),[](const Profile&a,const Profile&b){
        if(a.nums!=b.nums)return a.nums>b.nums;
        if(a.previous!=b.previous)return !a.previous;
        return a.dir.native()>b.dir.native();
    });
    if(out.size()>64)out.resize(64);return out;
}
inline fs::path absolute(const std::string&s) {std::error_code ec;auto p=fs::absolute(path(s),ec).lexically_normal();return ec?fs::path():p;}
inline bool current_declared(const fs::path&root,const std::string&key) {
    const std::string slug=key.rfind("track:",0)==0?key.substr(6):key;
    const auto dir=root/(key.rfind("track:",0)==0?"trackcovercache":"covercache");
    return present(dir/(slug+".jpg.source.json"))||present(dir/(slug+".jpg"));
}
inline bool valid_key(const std::string&key) {return hex(key,16)||(key.rfind("track:",0)==0&&hex(key.substr(6),16));}
struct Typed {std::string key,slug,leaf,bytes,raw;};
inline bool typed(const fs::path&file,Typed&out) {
    Json j;out={};if(!json(file,8192,j,&out.raw))return false;
    out.key=j["key"].s();if(!valid_key(out.key))return false;out.slug=out.key.rfind("track:",0)==0?out.key.substr(6):out.key;
    if(utf8(file.filename())!=out.slug+".jpg.source.json")return false;
    const bool track=out.key.rfind("track:",0)==0;
    if(utf8(file.parent_path().filename())!=(track?"trackcovercache":"covercache"))return false;
    const std::string source=j["source"].s(),sha=j["sha256"].s();
    const bool online=source=="online"&&j["version"].type==Json::Num&&j["version"].n()==2&&
        !j["albumName"].s().empty()&&!j["albumArtist"].s().empty()&&
        j["matchUrl"].s().rfind("http",0)==0&&!j["matchProvider"].s().empty();
    if(!(online||(j["version"].type==Json::Num&&j["version"].n()==1&&(source=="manual"||source=="extracted")))||!hex(sha,64))return false;
    out.leaf=out.slug+"."+sha+".blob";
    if(j["blob"].s()!=out.leaf||!read(file.parent_path()/out.leaf,maxImage,out.bytes)||!image(out.bytes)||
       j["bytes"].type!=Json::Num||j["bytes"].n()!=(double)out.bytes.size()||hash(out.bytes)!=sha)return false;
    return true;
}
inline void typed_from(const Profile&p,const fs::path&root,Budget&budget,MigrationReport&r,std::set<std::string>&blocked) {
    for(const char*folder:{"covercache","trackcovercache"}){
        const auto dir=p.dir/folder;if(!safe(dir))continue;std::error_code ec;
        for(fs::directory_iterator it(dir,ec),end;!ec&&it!=end&&budget.next();it.increment(ec)){
            auto name=utf8(it->path().filename());
            if(name.size()!=32||name.substr(16)!=".jpg.source.json")continue;
            const std::string key=std::string(folder)=="trackcovercache"?"track:"+name.substr(0,16):name.substr(0,16);
            if(current_declared(root,key)||blocked.count(key))continue;
            // The nearest declaration owns this key, even while its source is
            // incomplete/locked. Never report success by falling back past it.
            blocked.insert(key);
            Json intent;std::string intentRaw;
            if(json(it->path(),8192,intent,&intentRaw)&&intent["version"].type==Json::Num&&intent["version"].n()==1&&
               intent["key"].s()==key&&intent["source"].s()=="reset"){
                if(!publish(root/folder/name,intentRaw))++r.failed;else ++r.typedRecords;
                continue;
            }
            Typed t;if(!typed(it->path(),t)){++r.rejected;continue;}
            if(!budget.take(t.bytes.size()+t.raw.size())){++r.failed;return;}
            if(!blob(root/folder/t.leaf,t.bytes)||!publish(root/folder/name,t.raw)){++r.failed;continue;}
            ++r.typedRecords;
        }
    }
}
inline bool reset(const fs::path&root,const std::string&track) {
    return current_declared(root,"track:"+fnv(normalized_track(track)));
}
inline void meta_from(const Profile&p,const fs::path&root,Budget&budget,MigrationReport&r,const std::set<std::string>&blocked,std::set<std::string>&metadataSeen) {
    const auto dir=p.dir/"rlplayer-meta/tracks";if(!safe(dir))return;std::error_code ec;
    for(fs::directory_iterator it(dir,ec),end;!ec&&it!=end&&budget.next();it.increment(ec)){
        const std::string name=utf8(it->path().filename());
        if(name.size()!=21||name.substr(16)!=".json"||!hex(name.substr(0,16),16))continue;
        if(!metadataSeen.insert(name).second)continue;
        const auto destination=root/"rlplayer-meta/tracks"/name;
        // Existing metadata is one authoritative record, never field-merged with an older install.
        if(present(destination))continue;
        Json j;std::string raw;if(!json(it->path(),maxMeta,j,&raw)||j["path"].type!=Json::Str||
            j["path"].s().empty()||fnv(j["path"].s())!=name.substr(0,16)||j["fields"].type!=Json::Obj){++r.rejected;continue;}
        const std::string h=name.substr(0,16),cover=j["cover"].s();
        std::string coverBytes;fs::path coverPath;
        if(!cover.empty()){
            if(cover!="covers/"+h+".jpg"&&cover!="covers/"+h+".png"){++r.rejected;continue;}
            coverPath=path(cover);if(!read(p.dir/"rlplayer-meta"/coverPath,maxImage,coverBytes)||!image(coverBytes)){++r.rejected;continue;}
        }
        // MetaStore also probes hash.jpg/png even without a cover field.
        if(cover.empty())for(const char*ext:{".jpg",".png"}){
            std::string b;if(read(p.dir/"rlplayer-meta/covers"/(h+ext),maxImage,b)&&image(b)){coverPath=fs::path("covers")/(h+ext);coverBytes=std::move(b);break;}
        }
        if(!budget.take(raw.size()+coverBytes.size())){++r.failed;return;}
        if(!coverBytes.empty()&&!reset(root,j["path"].s())&&!blocked.count("track:"+fnv(normalized_track(j["path"].s())))){
            const auto to=root/"rlplayer-meta"/coverPath;
            if(present(to)){
                std::string current;
                if(!read(to,maxImage,current)||!image(current)){++r.failed;continue;}
            }else if(!blob(to,coverBytes)){++r.failed;continue;}
        }
        if(!publish(destination,raw)){++r.failed;continue;}++r.metaRecords;
    }
}
inline std::string dir_key(std::string s) {
    s=lower(s);for(;;){
        size_t e=s.size();while(e&&(s[e-1]=='\\'||s[e-1]=='/'))--e;
        size_t digits=e;while(e&&s[e-1]>='0'&&s[e-1]<='9')--e;if(e==digits)break;
        size_t q=e;if(q&&(s[q-1]=='-'||s[q-1]=='_'||s[q-1]==' '))--q;
        if(q>=2&&s.compare(q-2,2,"cd")==0){s.resize(q-2);continue;}
        if(q>=4&&(s.compare(q-4,4,"disc")==0||s.compare(q-4,4,"disk")==0)){s.resize(q-4);continue;}break;
    }return s;
}
struct OldAlbum {std::string key,legacy,album,artist;bool consistent=true;std::vector<HistoryMember> members;std::string sourceMetadataSha256;};
inline std::map<std::string,std::array<std::string,3>> user_overrides(const Profile&p,std::string&metadataSha) {
    std::map<std::string,std::array<std::string,3>> out;
    metadataSha=hash("");std::map<std::string,std::string> ledger;
    const auto dir=p.dir/"rlplayer-meta/tracks";if(!safe(dir))return out;
    std::error_code ec;size_t entries=0,remaining=32u*1024*1024;
    for(fs::directory_iterator it(dir,ec),end;!ec&&it!=end&&entries++<8192;it.increment(ec)){
        const std::string name=utf8(it->path().filename());if(name.size()!=21||name.substr(16)!=".json"||!hex(name.substr(0,16),16))continue;
        Json j;std::string raw;if(!json(it->path(),maxMeta,j,&raw)||raw.size()>remaining)continue;remaining-=raw.size();
        const std::string track=j["path"].s();if(track.empty()||fnv(track)!=name.substr(0,16)||j["fields"].type!=Json::Obj)continue;
        ledger[name]=hash(raw);
        std::array<std::string,3> values;size_t i=0;
        for(const char*field:{"album","albumArtist","artist"}){const auto&v=j["fields"][field];if(v["source"].s()=="user")values[i]=v["value"].s();++i;}
        if(std::any_of(values.begin(),values.end(),[](const std::string&s){return !s.empty();}))out.emplace(track,std::move(values));
    }
    std::string proof;for(const auto&v:ledger)proof+=v.first+":"+v.second+"\n";metadataSha=hash(proof);return out;
}
inline bool integer(const std::string&s,long long&n) {
    if(s.empty())return false;auto parsed=std::from_chars(s.data(),s.data()+s.size(),n);
    return parsed.ec==std::errc()&&parsed.ptr==s.data()+s.size();
}
inline bool old_albums(const Profile&p,std::vector<OldAlbum>&out,std::string&indexSha) {
    std::string raw;if(!read(p.dir/"rlplayer-index.tsv",maxIndex,raw))return false;
    indexSha=hash(raw);
    if(raw.rfind("\xef\xbb\xbf",0)==0)raw.erase(0,3);
    if(raw.find("#libIndexV\t5")==std::string::npos&&raw.find("#libIndexV\t6")==std::string::npos)return false;
    std::string metadataSha;const auto overrides=user_overrides(p,metadataSha);
    std::map<std::string,OldAlbum> groups;size_t offset=0,count=0;
    while(offset<raw.size()&&count++<65536){
        const auto end=raw.find('\n',offset);std::string line=raw.substr(offset,end==std::string::npos?end:end-offset);offset=end==std::string::npos?raw.size():end+1;
        if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;
        std::vector<std::string> f;size_t at=0;for(;;){auto tab=line.find('\t',at);f.push_back(line.substr(at,tab==std::string::npos?tab:tab-at));if(tab==std::string::npos)break;at=tab+1;}
        if(f.size()<16||f[0].empty())continue;
        const auto override=overrides.find(f[0]);
        if(override!=overrides.end()){
            const auto&v=override->second;if(!v[0].empty())f[3]=v[0];if(!v[1].empty())f[4]=v[1];
            if((f.size()<20||f[16].empty())&&!v[2].empty())f[5]=v[2];
        }
        if(f[3].empty())continue;
        const std::string album=f[3],artist=f[4].empty()?f[5]:f[4];
        // Preserve the actual index path spelling. Only the historical algorithm's
        // ASCII case fold and trailing disc-directory collapse affect its key.
        const auto parent=path(f[0]).parent_path();
        const std::string k=fnv(album+"\x1f"+dir_key(utf8(parent)));
        auto it=groups.find(k);
        if(it==groups.end())it=groups.emplace(k,OldAlbum{k,fnv(artist+"\x1f"+album),album,artist,true}).first;
        // Historical albumVecLocked assigned album artist from the first index
        // member. Later track performers may differ on a compilation; they did
        // not rename that browsing album or its legacy key. Reproduce that
        // exact stored binding instead of inventing a new compilation policy.
        else if(it->second.album!=album)it->second.consistent=false;
        HistoryMember member{f[0]};
        if(f.size()>=20){member.cuePath=f[16];if(!integer(f[17],member.cueNo)||!integer(f[18],member.cueOff)||!integer(f[19],member.cueLen))it->second.consistent=false;}
        it->second.members.push_back(std::move(member));
    }
    for(auto&v:groups)if(v.second.consistent){v.second.sourceMetadataSha256=metadataSha;out.push_back(std::move(v.second));}return true;
}
inline bool eligible(const HistoryAlbumBinding&b) {
    const bool group=b.identitySource=="directory_inferred"&&hex(b.membersSha256,64)&&b.memberCount>0&&b.memberCount<=65536;
    const bool album=(b.identitySource=="tagged"||b.identitySource=="user_assigned")&&!b.artist.empty();
    return hex(b.key,16)&&hex(b.legacyKey,16)&&!b.album.empty()&&(album||group)&&b.legacyKey==fnv(b.artist+"\x1f"+b.album);
}
struct HistoryRecord {HistoryInfo info;fs::path blob;};
inline bool history_record(const fs::path&root,const HistoryAlbumBinding&b,HistoryRecord&r) {
    r={};if(!eligible(b)||current_declared(root,b.key))return false;
    Json j;if(!json(root/"history-artwork"/(b.key+".json"),8192,j))return false;
    const std::string sha=j["sha256"].s(),sourceKey=j["sourceKey"].s(),version=j["sourceVersion"].s();
    auto parsed=rta::update::parse_version(version);
    const bool group=j["kind"].s()=="historical_group";
    if((group&&(j["version"].n()!=2||j["membersSha256"].s()!=b.membersSha256||!hex(b.membersSha256,64)||b.memberCount==0||b.memberCount>65536||!hex(j["sourceMetadataSha256"].s(),64)||j["memberCount"].type!=Json::Num||j["memberCount"].n()!=(double)b.memberCount))||
       (!group&&(j["version"].n()!=1||b.identitySource=="directory_inferred")))return false;
    if(j["version"].type!=Json::Num||j["source"].s()!="history"||
       j["key"].s()!=b.key||j["legacyKey"].s()!=b.legacyKey||j["albumName"].s()!=b.album||j["albumArtist"].s()!=b.artist||
       !hex(sha,64)||(sourceKey!=b.key&&sourceKey!=b.legacyKey)||!hex(j["sourceIndexSha256"].s(),64)||!parsed.valid||parsed.has_pre()||
       j["bytes"].type!=Json::Num||j["bytes"].n()<=128||j["bytes"].n()>maxImage||std::floor(j["bytes"].n())!=j["bytes"].n())return false;
    const std::string leaf=b.key+"."+sha+".blob";if(j["blob"].s()!=leaf)return false;
    r.blob=root/"history-artwork"/leaf;r.info={sha,j["mime"].s(),version,sourceKey,(size_t)j["bytes"].n()};r.info.source=group?"historical_group":"history";
    return safe(r.blob);
}
struct Proof {rta::platform::artwork_io::FileStamp attrs{};std::string sha,mime;};
inline std::mutex& proof_mutex(){static std::mutex m;return m;}
inline std::unordered_map<fs::path,Proof>& proofs(){static std::unordered_map<fs::path,Proof> m;return m;}
inline bool attributes(const fs::path&p,rta::platform::artwork_io::FileStamp&a) {return rta::platform::artwork_io::capture_stamp(p,a);}
inline bool same(const rta::platform::artwork_io::FileStamp&a,const rta::platform::artwork_io::FileStamp&b) {return rta::platform::artwork_io::same_stamp(a,b);}
inline void remember(const fs::path&p,const std::string&sha,const std::string&mime,const rta::platform::artwork_io::FileStamp&a){
    std::lock_guard<std::mutex> lock(proof_mutex());if(proofs().size()>=8192)proofs().clear();proofs()[p]={a,sha,mime};
}
inline bool verified(const HistoryRecord&r,std::string&bytes) {
    rta::platform::artwork_io::FileStamp before{},after{};
    if(!attributes(r.blob,before)||!read(r.blob,maxImage,bytes)||bytes.size()!=r.info.bytes||
       !image(bytes)||mime(bytes)!=r.info.mime||hash(bytes)!=r.info.sha256||!attributes(r.blob,after)||!same(before,after)){bytes.clear();return false;}
    remember(r.blob,r.info.sha256,r.info.mime,after);return true;
}
} // namespace detail

inline std::string history_members_sha256(const std::vector<HistoryMember>&members) {return detail::member_hash(members);}

inline MigrationReport migrate_profile_artwork(const std::string&currentExeDir,const std::string&currentVersion={}) {
    MigrationReport r;const auto root=detail::absolute(currentExeDir);if(root.empty()||!detail::safe(root))return r;
    detail::Budget budget;std::set<std::string> blocked,metadataSeen;
    for(const auto&p:detail::profiles(root,currentVersion)){
        ++r.profiles;detail::typed_from(p,root,budget,r,blocked);detail::meta_from(p,root,budget,r,blocked,metadataSeen);
    }return r;
}
inline MigrationReport import_legacy_history(const std::string&currentExeDir,const std::vector<HistoryAlbumBinding>&bindings,const std::string&currentVersion={}) {
    MigrationReport r;const auto root=detail::absolute(currentExeDir);if(root.empty()||!detail::safe(root))return r;
    std::map<std::string,HistoryAlbumBinding> eligible;
    for(const auto&b:bindings)if(detail::eligible(b))eligible.emplace(b.key,b);
    detail::Budget budget;std::set<std::string> blocked;
    for(const auto&p:detail::profiles(root,currentVersion)){
        ++r.profiles;std::vector<detail::OldAlbum> albums;std::string indexSha;
        for(const auto&v:eligible){
            const auto&b=v.second;
            if(detail::present(p.dir/"covercache"/(b.key+".jpg.source.json"))||
               detail::present(p.dir/"covercache"/(b.legacyKey+".jpg.source.json")))blocked.insert(b.key);
        }
        if(!detail::old_albums(p,albums,indexSha))continue;
        for(const auto&a:albums){
            if(!budget.next())return r;
            auto found=eligible.find(a.key);
            if(found==eligible.end())continue;
            const auto&b=found->second;
            const bool group=b.identitySource=="directory_inferred";
            if(group&&(a.members.size()!=b.memberCount||detail::member_hash(a.members)!=b.membersSha256))continue;
            if(a.album!=b.album||a.artist!=b.artist||a.legacy!=b.legacyKey||blocked.count(b.key)||detail::current_declared(root,b.key)||
                detail::present(root/"history-artwork"/(b.key+".json")))continue;
            for(const std::string sourceKey:{a.key,a.legacy}){
                // Only old untyped JPEG-cache records, never an orphan .jpg hidden
                // by a newer typed/reset/malformed declaration.
                if(detail::present(p.dir/"covercache"/(sourceKey+".jpg.source.json")))continue;
                std::string bytes;
                if(!detail::read(p.dir/"covercache"/(sourceKey+".jpg"),detail::maxImage,bytes)||!detail::image(bytes))continue;
                if(!budget.take(bytes.size())){++r.failed;return r;}
                const std::string sha=detail::hash(bytes),leaf=b.key+"."+sha+".blob";
                const std::string raw="{\"version\":"+std::string(group?"2":"1")+",\"source\":\"history\",\"kind\":"+detail::quote(group?"historical_group":"album")+",\"key\":"+detail::quote(b.key)+
                    ",\"legacyKey\":"+detail::quote(b.legacyKey)+",\"albumName\":"+detail::quote(b.album)+",\"albumArtist\":"+detail::quote(b.artist)+
                    ",\"sourceVersion\":"+detail::quote(p.version)+",\"sourceKey\":"+detail::quote(sourceKey)+",\"sourceIndexSha256\":"+detail::quote(indexSha)+
                    ",\"sourceMetadataSha256\":"+detail::quote(a.sourceMetadataSha256)+
                    ",\"membersSha256\":"+detail::quote(detail::member_hash(a.members))+",\"memberCount\":"+std::to_string(a.members.size())+
                    ",\"sha256\":"+detail::quote(sha)+",\"mime\":"+detail::quote(detail::mime(bytes))+",\"bytes\":"+std::to_string(bytes.size())+",\"blob\":"+detail::quote(leaf)+"}";
                if(!detail::blob(root/"history-artwork"/leaf,bytes)||!detail::publish(root/"history-artwork"/(b.key+".json"),raw)){++r.failed;break;}
                ++r.historyRecords;break;
            }
        }
    }return r;
}
inline bool read_history(const std::string&currentExeDir,const HistoryAlbumBinding&binding,std::vector<unsigned char>&bytes,std::string&mime,HistoryInfo*info=nullptr) {
    bytes.clear();mime.clear();detail::HistoryRecord r;const auto root=detail::absolute(currentExeDir);
    if(root.empty()||!detail::history_record(root,binding,r))return false;
    std::string raw;if(!detail::verified(r,raw))return false;
    bytes.assign(raw.begin(),raw.end());mime=r.info.mime;if(info)*info=r.info;return true;
}
inline bool probe_history(const std::string&currentExeDir,const HistoryAlbumBinding&binding,HistoryInfo*info=nullptr) {
    detail::HistoryRecord r;const auto root=detail::absolute(currentExeDir);
    if(root.empty()||!detail::history_record(root,binding,r))return false;
    rta::platform::artwork_io::FileStamp attrs{};if(!detail::attributes(r.blob,attrs)||
       attrs.bytes!=r.info.bytes)return false;
    bool cached=false;{
        std::lock_guard<std::mutex> lock(detail::proof_mutex());const auto it=detail::proofs().find(r.blob);
        cached=it!=detail::proofs().end()&&it->second.sha==r.info.sha256&&it->second.mime==r.info.mime&&detail::same(it->second.attrs,attrs);
    }
    if(!cached){std::string raw;if(!detail::verified(r,raw))return false;}
    if(info)*info=r.info;return true;
}
} // namespace rta::net::artwork_migration

