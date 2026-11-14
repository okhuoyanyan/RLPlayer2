// ============================================================================
// Windows signed incremental update protocol, schema 2.
//
// The version/SHA/path foundations were ported from BH2VOQ/RLPlayer-Mac
// src/net/update_check.h (GPL-3.0-or-later). Mac port copyright is retained:
// Copyright (C) 2026 August Hu <BH2VOQ@163.com>
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "net/json.h"
#include "net/update_signature.h"

namespace rta::update {

inline constexpr const char kDefaultManifestUrl[] =
    "https://rlplayer.rainlain.com/api/v1/update/windows/latest";
inline constexpr const char kWindowsUpdateHost[] = "rlplayer.rainlain.com";
inline constexpr const char kWindowsUpdateChannel[] = "stable";
inline constexpr long long kWindowsUpdateMetadataVersion = 1;

// ---------------------------------------------------------------------------
// Versions: SemVer comparison, including prerelease and ignored build metadata.
// ---------------------------------------------------------------------------
struct Version {
    bool valid = false;
    std::vector<long long> nums;
    std::vector<std::string> pre;
    std::string raw;
    bool has_pre() const { return !pre.empty(); }
};

namespace detail {
inline bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}
inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}
inline std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}
inline bool parse_num_segment(const std::string& s, long long& out) {
    if (!all_digits(s) || s.size() > 9) return false;
    out = std::atoll(s.c_str());
    return true;
}
} // namespace detail

inline Version parse_version(const std::string& input) {
    Version v;
    v.raw = input;
    std::string s = detail::trim(input);
    if (s.empty()) return v;
    if (s[0] == 'v' || s[0] == 'V') s.erase(s.begin());
    const size_t plus = s.find('+');
    if (plus != std::string::npos) s.resize(plus);
    std::string core = s;
    const size_t dash = s.find('-');
    if (dash != std::string::npos) {
        core = s.substr(0, dash);
        const std::string pre = s.substr(dash + 1);
        if (pre.empty()) return v;
        size_t pos = 0;
        while (pos <= pre.size()) {
            const size_t dot = pre.find('.', pos);
            const std::string token = dot == std::string::npos ? pre.substr(pos) : pre.substr(pos, dot - pos);
            if (token.empty()) return v;
            for (unsigned char c : token) if (!std::isalnum(c) && c != '-') return v;
            v.pre.push_back(token);
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
    }
    if (core.empty()) return v;
    size_t pos = 0;
    while (pos <= core.size()) {
        const size_t dot = core.find('.', pos);
        const std::string token = dot == std::string::npos ? core.substr(pos) : core.substr(pos, dot - pos);
        long long number = 0;
        if (!detail::parse_num_segment(token, number)) return v;
        v.nums.push_back(number);
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    v.valid = !v.nums.empty() && v.nums.size() <= 4;
    return v;
}

enum class VersionCmp { Less = -1, Equal = 0, Greater = 1, Invalid = 2 };
inline VersionCmp compare_versions(const Version& a, const Version& b) {
    if (!a.valid || !b.valid) return VersionCmp::Invalid;
    const size_t count = std::max(a.nums.size(), b.nums.size());
    for (size_t i = 0; i < count; ++i) {
        const long long x = i < a.nums.size() ? a.nums[i] : 0;
        const long long y = i < b.nums.size() ? b.nums[i] : 0;
        if (x < y) return VersionCmp::Less;
        if (x > y) return VersionCmp::Greater;
    }
    if (a.pre.empty() && b.pre.empty()) return VersionCmp::Equal;
    if (a.pre.empty()) return VersionCmp::Greater;
    if (b.pre.empty()) return VersionCmp::Less;
    const size_t countPre = std::min(a.pre.size(), b.pre.size());
    for (size_t i = 0; i < countPre; ++i) {
        const std::string& x = a.pre[i];
        const std::string& y = b.pre[i];
        const bool xd = detail::all_digits(x), yd = detail::all_digits(y);
        if (xd && yd) {
            if (x.size() != y.size()) return x.size() < y.size() ? VersionCmp::Less : VersionCmp::Greater;
        } else if (xd != yd) {
            return xd ? VersionCmp::Less : VersionCmp::Greater;
        }
        const int compared = x.compare(y);
        if (compared < 0) return VersionCmp::Less;
        if (compared > 0) return VersionCmp::Greater;
    }
    if (a.pre.size() == b.pre.size()) return VersionCmp::Equal;
    return a.pre.size() < b.pre.size() ? VersionCmp::Less : VersionCmp::Greater;
}
inline bool is_newer(const std::string& remote, const std::string& local) {
    return compare_versions(parse_version(remote), parse_version(local)) == VersionCmp::Greater;
}

// ---------------------------------------------------------------------------
// FIPS 180-4 SHA-256. File hashing is streaming and bounded to 64 KiB.
// ---------------------------------------------------------------------------
namespace detail_sha256 {
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline void block(uint32_t h[8], const uint8_t p[64]) {
    static const uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    static_assert(sizeof(K) / sizeof(K[0]) == 64, "SHA-256 requires all 64 round constants");
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        const uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1=rotr(e,6)^rotr(e,11)^rotr(e,25), ch=(e&f)^(~e&g);
        const uint32_t t1=hh+s1+ch+K[i]+w[i], s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
        const uint32_t mj=(a&b)^(a&c)^(b&c), t2=s0+mj;
        hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
inline void init(uint32_t h[8]) {
    h[0]=0x6a09e667u;h[1]=0xbb67ae85u;h[2]=0x3c6ef372u;h[3]=0xa54ff53au;
    h[4]=0x510e527fu;h[5]=0x9b05688cu;h[6]=0x1f83d9abu;h[7]=0x5be0cd19u;
}
inline std::string hex_of(const uint32_t h[8]) {
    static const char* hx="0123456789abcdef";
    std::string out; out.reserve(64);
    for (int i=0;i<8;++i) for(int j=3;j>=0;--j) {
        const uint8_t b=(uint8_t)((h[i]>>(8*j))&255);
        out.push_back(hx[b>>4]);out.push_back(hx[b&15]);
    }
    return out;
}
} // namespace detail_sha256

inline std::string sha256_hex(const void* data, size_t len) {
    uint32_t h[8]; detail_sha256::init(h);
    const uint8_t* p=static_cast<const uint8_t*>(data); size_t left=len;
    while(left>=64){detail_sha256::block(h,p);p+=64;left-=64;}
    uint8_t tail[128]{}; if(left)std::memcpy(tail,p,left);tail[left]=0x80;
    const size_t tailLen=left<56?64:128;const uint64_t bits=(uint64_t)len*8;
    for(int i=0;i<8;++i)tail[tailLen-1-i]=(uint8_t)(bits>>(8*i));
    detail_sha256::block(h,tail);if(tailLen==128)detail_sha256::block(h,tail+64);
    return detail_sha256::hex_of(h);
}
inline std::string sha256_hex(const std::string& s){return sha256_hex(s.data(),s.size());}

inline bool sha256_file(const std::string& path, std::string& outHex, std::string& err) {
    std::ifstream file(std::filesystem::u8path(path),std::ios::binary);
    if(!file){err="无法打开文件";return false;}
    uint32_t h[8];detail_sha256::init(h);uint64_t total=0;
    char buffer[65536];uint8_t pending[64];size_t pendingSize=0;
    while(file){
        file.read(buffer,sizeof(buffer));const size_t got=(size_t)file.gcount();
        if(!got)break;total+=got;size_t offset=0;
        if(pendingSize){
            const size_t need=64-pendingSize,take=std::min(need,got);
            std::memcpy(pending+pendingSize,buffer,take);pendingSize+=take;offset=take;
            if(pendingSize==64){detail_sha256::block(h,pending);pendingSize=0;}
        }
        while(offset+64<=got){detail_sha256::block(h,(const uint8_t*)buffer+offset);offset+=64;}
        if(offset<got){pendingSize=got-offset;std::memcpy(pending,buffer+offset,pendingSize);}
    }
    if(!file.eof()){err="读取文件失败";return false;}
    uint8_t tail[128]{};if(pendingSize)std::memcpy(tail,pending,pendingSize);tail[pendingSize]=0x80;
    const size_t tailLen=pendingSize<56?64:128;const uint64_t bits=total*8;
    for(int i=0;i<8;++i)tail[tailLen-1-i]=(uint8_t)(bits>>(8*i));
    detail_sha256::block(h,tail);if(tailLen==128)detail_sha256::block(h,tail+64);
    outHex=detail_sha256::hex_of(h);return true;
}
inline bool hex_equals_ci(const std::string& a,const std::string& b){
    if(a.empty()||a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i)if(std::tolower((unsigned char)a[i])!=std::tolower((unsigned char)b[i]))return false;
    return true;
}
inline bool is_hex64(const std::string& s){
    if(s.size()!=64)return false;for(unsigned char c:s)if(!std::isxdigit(c))return false;return true;
}
inline std::string lower_ascii(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c=(char)(c-'A'+'a');return s;}

// ---------------------------------------------------------------------------
// URL and path security. Automatic traffic is never a manual-link decision.
// ---------------------------------------------------------------------------
inline bool update_download_url_allowed(const std::string& url) {
    if(url.size()>4096)return false;
    if(url=="https://rlplayer.rainlain.com/api/v1/events/external")return true;
    const std::string prefix="https://rlplayer.rainlain.com/api/v1/update/windows/";
    if(url.rfind(prefix,0)!=0||url.size()<=prefix.size())return false;
    for(unsigned char c:url)if(c<=0x20||c==0x7f||c=='\\'||c=='@')return false;
    const std::string tail=url.substr(prefix.size());
    if(tail=="latest"||tail=="report")return true;
    return tail.rfind("objects/",0)==0&&tail.size()>8&&tail.find('/',8)==std::string::npos;
}
inline bool manual_download_url_allowed(const std::string& url) {
    if(url.empty()||url.size()>4096||url.rfind("https://",0)!=0)return false;
    for(unsigned char c:url)if(c<=0x20||c==0x7f||c=='\\')return false;
    const size_t slash=url.find('/',8);if(slash==std::string::npos||slash==8)return false;
    const std::string authority=detail::lower(url.substr(8,slash-8));
    if(authority.find('@')!=std::string::npos||authority.find(':')!=std::string::npos)return false;
    return authority=="github.com"||authority=="pan.baidu.com";
}
inline bool safe_install_rel(const std::string& p) {
    if(p.empty()||p.size()>512||p.find("..")!=std::string::npos||p.find("://")!=std::string::npos)return false;
    if((p.size()>=2&&p[1]==':')||p[0]=='\\'||p[0]=='/')return false;
    for(unsigned char c:p)if(c<0x20||c==0x7f||c==':'||c=='<'||c=='>'||c=='"'||c=='|'||c=='?'||c=='*')return false;
    size_t begin=0;
    while(begin<p.size()){
        const size_t end=p.find_first_of("\\/",begin);
        const auto part=p.substr(begin,end==std::string::npos?end:end-begin);
        if(part.empty()||part.back()=='.'||part.back()==' ')return false;
        const auto name=lower_ascii(part.substr(0,part.find('.')));
        if(name=="con"||name=="prn"||name=="aux"||name=="nul"||
           (name.size()==4&&(name.substr(0,3)=="com"||name.substr(0,3)=="lpt")&&name[3]>='1'&&name[3]<='9'))return false;
        if(end==std::string::npos)break;begin=end+1;if(begin==p.size())return false;
    }
    return true;
}
inline std::string norm_rel(std::string p){for(char& c:p)if(c=='/')c='\\';return p;}
inline std::string norm_rel2(std::string rel){
    for(char& c:rel){if(c=='/')c='\\';else if(c>='A'&&c<='Z')c=(char)(c-'A'+'a');}
    while(!rel.empty()&&(rel.front()=='\\'||rel.front()=='.'))rel.erase(rel.begin());return rel;
}
inline bool starts_with_dir(const std::string& rel,const std::string& dir){
    return rel.size()>=dir.size()&&rel.compare(0,dir.size(),dir)==0&&
           (rel.size()==dir.size()||rel[dir.size()]=='\\');
}
inline const std::vector<std::string>& protected_files(){
    static const std::vector<std::string> value={
        "rlplayer-settings.conf","rlplayer-library.conf","rlplayer-llm.conf","rlplayer-feedback.conf",
        "rlplayer-playlist.txt","rlplayer-queue.txt","rlplayer-savedqueues.conf","rlplayer-favs.json","rlplayer-sources.conf",
        "rlplayer-peq.txt","rlplayer-workbench-profiles.json","rlplayer-index.tsv","rlplayer-recent.txt","rlplayer-diag.json",
        "rlplayer-playback.log","launcher.log","rlplayer-llm-usage.json","rlplayer-update.conf"};
    return value;
}
inline const std::vector<std::string>& protected_dirs(){
    static const std::vector<std::string> value={"data","covercache","trackcovercache","webview2runtime",
        "webview2","playlists","rlplayer-meta","updates","dumps","feedback-outbox"};
    return value;
}
inline const std::vector<std::string>& protected_allow(){
    static const std::vector<std::string> value={"data\\opra_index.json","data\\i18n\\ar.json",
        "data\\i18n\\en.json","data\\i18n\\es.json","data\\i18n\\fr.json","data\\i18n\\ja.json","data\\i18n\\ru.json"};
    return value;
}
inline bool is_protected_rel(const std::string& raw){
    const std::string rel=norm_rel2(raw);if(rel.empty())return true;
    for(const auto& allowed:protected_allow())if(rel==allowed)return false;
    for(const auto& dir:protected_dirs())if(starts_with_dir(rel,dir))return true;
    const size_t slash=rel.find_last_of('\\');const std::string leaf=slash==std::string::npos?rel:rel.substr(slash+1);
    for(const auto& file:protected_files())if(rel==file||leaf==file)return true;
    return false;
}
inline bool safe_object_name(const std::string& object){
    if(object.size()!=68||object.compare(64,4,".zip")!=0)return false;
    for(size_t i=0;i<64;++i)if(!((object[i]>='0'&&object[i]<='9')||(object[i]>='a'&&object[i]<='f')))return false;
    return true;
}

// ---------------------------------------------------------------------------
// Strict RFC3339 UTC timestamps and schema-2 payload.
// ---------------------------------------------------------------------------
inline long long days_from_civil(int y,unsigned m,unsigned d){
    y-=m<=2;const int era=(y>=0?y:y-399)/400;const unsigned yoe=(unsigned)(y-era*400);
    const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    const unsigned doe=yoe*365+yoe/4-yoe/100+doy;
    return (long long)era*146097+(long long)doe-719468;
}
inline long long parse_rfc3339_epoch(const std::string& text){
    if(text.size()<20||text[4]!='-'||text[7]!='-'||(text[10]!='T'&&text[10]!='t')||
       text[13]!=':'||text[16]!=':')return -1;
    auto number=[&](size_t at,size_t count,int& out){out=0;for(size_t i=0;i<count;++i){
        const char c=text[at+i];if(c<'0'||c>'9')return false;out=out*10+c-'0';}return true;};
    int y,mo,d,h,mi,s;
    if(!number(0,4,y)||!number(5,2,mo)||!number(8,2,d)||!number(11,2,h)||!number(14,2,mi)||!number(17,2,s))return -1;
    size_t at=19;if(at<text.size()&&text[at]=='.'){++at;const size_t begin=at;
        while(at<text.size()&&std::isdigit((unsigned char)text[at]))++at;if(at==begin||at-begin>9)return -1;}
    if(at+1!=text.size()||(text[at]!='Z'&&text[at]!='z'))return -1;
    static const int lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(y<1970||mo<1||mo>12||h>23||mi>59||s>59)return -1;
    int maxDay=lengths[mo-1];if(mo==2&&((y%4==0&&y%100!=0)||y%400==0))++maxDay;
    if(d<1||d>maxDay)return -1;
    const long long days=days_from_civil(y,(unsigned)mo,(unsigned)d);
    const long long epoch=days*86400+h*3600+mi*60+s;
    return epoch>=0?epoch:-1;
}
inline bool manifest_size_field(const rta::net::Json& object,const char* key,long long& out,bool required=false){
    out=0;if(!object.has(key))return !required;const auto& value=object[key];
    if(value.type!=rta::net::Json::Num||!std::isfinite(value.num)||value.num<0||
       value.num>1099511627776.0||std::floor(value.num)!=value.num)return false;
    out=(long long)value.num;return true;
}

struct ManifestFile {
    std::string path,sha256,object,objectSha256;
    long long size=0,objectSize=0;
};
struct ManualPackage {
    std::string cnUrl,globalUrl,extractionCode,sha256;
    long long size=0;
};
struct ManifestExt {
    bool ok=false,manual=false;
    std::string error,errorCode,version,installMode,reason,notes,publishedAt,expiresAt,channel;
    long long publishedEpoch=0,expiresEpoch=0,metadataVersion=0;
    std::vector<ManifestFile> files;
    std::vector<std::string> deletes;
    ManualPackage manualInfo;
    long long fullSize=0; // Manual link metadata only; never a server-side full package.
    std::string fullSha;
};

inline ManifestExt parse_manifest_payload(const std::string& payload,const std::string& localVersion,
                                          long long nowEpoch,const std::string& channel){
    ManifestExt m;
    auto fail=[&](const char* code,const std::string& error){m.ok=false;m.errorCode=code;m.error=error;return m;};
    if(payload.size()>2*1024*1024)return fail("payload-size","更新清单过大");
    rta::net::Json j;if(!rta::net::json_parse_strict(payload,j)||j.type!=rta::net::Json::Obj)
        return fail("payload-json","更新 payload 不是严格 JSON 对象");
    static const std::set<std::string> allowed={"schema","platform","channel","metadata_version","version",
        "published_at","expires_at","install_mode","reason","files","delete","manual","notes"};
    for(const auto& item:j.obj)if(!allowed.count(item.first))return fail("payload-field","未知更新字段："+item.first);
    long long schema=0,metadata=0;
    if(!manifest_size_field(j,"schema",schema,true)||schema!=2)return fail("schema","更新 schema 必须为 2");
    if(!manifest_size_field(j,"metadata_version",metadata,true)||metadata!=kWindowsUpdateMetadataVersion)
        return fail("metadata-version","不支持的更新 metadata_version");
    m.metadataVersion=metadata;
    for(const char* key:{"platform","channel","version","published_at","expires_at","install_mode","reason","notes"})
        if(j[key].type!=rta::net::Json::Str)return fail("payload-field",std::string("更新字段必须为字符串：")+key);
    if(j["platform"].str!="windows")return fail("platform","更新平台不是 windows");
    if(channel.empty()||j["channel"].str!=channel)return fail("channel","更新 channel 不匹配");
    m.channel=channel;m.version=j["version"].str;m.reason=j["reason"].str;m.notes=j["notes"].str;
    const Version remote=parse_version(m.version),local=parse_version(localVersion);
    if(!remote.valid||!local.valid)return fail("version","本地或目标版本号非法");
    const VersionCmp comparison=compare_versions(remote,local);
    if(comparison==VersionCmp::Less)return fail("rollback","拒绝版本回滚");
    m.publishedAt=j["published_at"].str;m.expiresAt=j["expires_at"].str;
    m.publishedEpoch=parse_rfc3339_epoch(m.publishedAt);m.expiresEpoch=parse_rfc3339_epoch(m.expiresAt);
    if(m.publishedEpoch<0||m.expiresEpoch<0||m.publishedEpoch>m.expiresEpoch)
        return fail("timestamp","published_at/expires_at 不是合法 UTC 时间");
    if(nowEpoch<0||m.expiresEpoch<=nowEpoch)return fail("expired","更新清单已过期");
    m.installMode=j["install_mode"].str;
    if(m.installMode!="incremental"&&m.installMode!="manual")return fail("install-mode","install_mode 非法");
    const auto& manual=j["manual"];if(manual.type!=rta::net::Json::Obj)return fail("manual","manual 必须为对象");
    for(const auto& item:manual.obj)if(item.first!="cn_url"&&item.first!="global_url"&&
        item.first!="extraction_code"&&item.first!="sha256"&&item.first!="size")return fail("manual","manual 含未知字段");
    for(const char* key:{"cn_url","global_url","extraction_code","sha256"})
        if(manual[key].type!=rta::net::Json::Str)return fail("manual","manual 字段类型非法");
    m.manualInfo.cnUrl=manual["cn_url"].str;m.manualInfo.globalUrl=manual["global_url"].str;
    m.manualInfo.extractionCode=manual["extraction_code"].str;m.manualInfo.sha256=lower_ascii(manual["sha256"].str);
    if(!manifest_size_field(manual,"size",m.manualInfo.size,true)||m.manualInfo.size<=0||
       !is_hex64(m.manualInfo.sha256)||!manual_download_url_allowed(m.manualInfo.cnUrl)||
       !manual_download_url_allowed(m.manualInfo.globalUrl)||m.manualInfo.extractionCode.size()>64)
        return fail("manual","手动外链、提取码、摘要或大小非法");
    m.fullSize=m.manualInfo.size;m.fullSha=m.manualInfo.sha256;
    if(j["files"].type!=rta::net::Json::Arr||j["files"].size()>4096)return fail("files","files 必须为有限数组");
    std::set<std::string> destinations;
    std::map<std::string,std::pair<std::string,long long>> objects;
    for(size_t i=0;i<j["files"].size();++i){
        const auto& e=j["files"].at(i);if(e.type!=rta::net::Json::Obj)return fail("files","files[] 必须为对象");
        for(const auto& item:e.obj)if(item.first!="path"&&item.first!="sha256"&&item.first!="size"&&
            item.first!="object"&&item.first!="object_sha256"&&item.first!="object_size")
            return fail("files","files[] 含外部地址或未知字段");
        for(const char* key:{"path","sha256","object","object_sha256"})
            if(e[key].type!=rta::net::Json::Str)return fail("files","files[] 字符串字段缺失");
        ManifestFile f;f.path=e["path"].str;f.sha256=lower_ascii(e["sha256"].str);
        f.object=e["object"].str;f.objectSha256=lower_ascii(e["object_sha256"].str);
        if(!safe_install_rel(f.path)||!is_hex64(f.sha256)||!safe_object_name(f.object)||
           f.object!=f.sha256+".zip"||!is_hex64(f.objectSha256)||
           !manifest_size_field(e,"size",f.size,true)||!manifest_size_field(e,"object_size",f.objectSize,true)||
           f.objectSize<=0||f.objectSize>2147483647LL)
            return fail("files","files[] 路径、对象名、摘要或大小非法");
        f.path=norm_rel(f.path);
        const auto definition=objects.emplace(f.object,std::make_pair(f.objectSha256,f.objectSize));
        if(!destinations.insert(norm_rel2(f.path)).second||
           (!definition.second&&(definition.first->second.first!=f.objectSha256||
                                 definition.first->second.second!=f.objectSize)))
            return fail("files","files[] 目标重复或同内容对象定义不一致");
        m.files.push_back(f);
    }
    if(j["delete"].type!=rta::net::Json::Arr||j["delete"].size()>4096)return fail("delete","delete 必须为有限数组");
    std::set<std::string> deleted;
    for(size_t i=0;i<j["delete"].size();++i){const auto& e=j["delete"].at(i);
        if(e.type!=rta::net::Json::Str||!safe_install_rel(e.str))return fail("delete","delete 路径非法");
        const std::string path=norm_rel(e.str);if(destinations.count(norm_rel2(path))||!deleted.insert(norm_rel2(path)).second)
            return fail("delete","delete 与 files 重叠或包含重复项");m.deletes.push_back(path);
    }
    if(m.installMode=="manual"||(!remote.nums.empty()&&!local.nums.empty()&&remote.nums[0]!=local.nums[0]))m.manual=true;
    if(!m.manual&&m.files.empty()&&m.deletes.empty())return fail("empty-plan","增量清单没有文件或删除项");
    m.ok=true;return m;
}

// ---------------------------------------------------------------------------
// Check decisions. Failure, rollback and expiry are never "up to date".
// ---------------------------------------------------------------------------
enum class Decision { UpdateAvailable,UpToDate,FailedNetwork,FailedAuth,FailedHttp,FailedParse,FailedBadUrl,
    FailedSignature,FailedExpired,FailedRollback,FailedQuota };
inline const char* decision_code(Decision d){switch(d){
    case Decision::UpdateAvailable:return "update-available";case Decision::UpToDate:return "up-to-date";
    case Decision::FailedNetwork:return "network-error";case Decision::FailedAuth:return "auth-failed";
    case Decision::FailedHttp:return "http-error";case Decision::FailedParse:return "parse-error";
    case Decision::FailedBadUrl:return "bad-url";case Decision::FailedSignature:return "signature-error";
    case Decision::FailedExpired:return "expired";case Decision::FailedRollback:return "rollback";
    case Decision::FailedQuota:return "quota-exceeded";}return "unknown";}
struct CheckResult {
    Decision decision=Decision::FailedNetwork;std::string remoteVersion,downloadUrl,sha256,notes,error,errorCode;
    long long size=0;int status=0;bool manual=false;
    const char* code()const{return decision_code(decision);}
    bool up_to_date()const{return decision==Decision::UpToDate;}
    bool available()const{return decision==Decision::UpdateAvailable;}
    bool ok()const{return up_to_date()||available();}
};
inline CheckResult evaluate_update_check(const std::string& localVersion,bool httpOk,int httpStatus,
    const std::string& body,long long nowEpoch=(long long)std::time(nullptr),
    const std::string& channel=kWindowsUpdateChannel,const UpdateSignatureKey& key=UpdateSignatureKey{}){
    CheckResult r;r.status=httpStatus;
    if(!httpOk&&httpStatus==0){r.decision=Decision::FailedNetwork;r.errorCode="network";r.error="网络不可用";return r;}
    if(httpStatus==401||httpStatus==403){r.decision=Decision::FailedAuth;r.errorCode="auth";r.error="更新服务器鉴权失败";return r;}
    if(httpStatus==429){r.decision=Decision::FailedQuota;r.errorCode="quota_exceeded";
        r.error="quota_exceeded：本月增量配额已用完，请到百度网盘或 GitHub 下载完整版，或下月重试";return r;}
    if(!httpOk||httpStatus<200||httpStatus>=300){r.decision=Decision::FailedHttp;r.errorCode="http";
        r.error="更新服务器返回 HTTP "+std::to_string(httpStatus);return r;}
    auto verified=verify_windows_update_envelope(body,key);
    if(!verified.ok){r.decision=Decision::FailedSignature;r.errorCode=verified.errorCode;r.error=verified.error;return r;}
    ManifestExt m=parse_manifest_payload(verified.payload,localVersion,nowEpoch,channel);
    if(!m.ok){r.errorCode=m.errorCode;r.error=m.error;
        if(m.errorCode=="expired")r.decision=Decision::FailedExpired;
        else if(m.errorCode=="rollback")r.decision=Decision::FailedRollback;
        else r.decision=Decision::FailedParse;return r;}
    r.remoteVersion=m.version;r.notes=m.notes;r.manual=m.manual;
    r.decision=compare_versions(parse_version(m.version),parse_version(localVersion))==VersionCmp::Greater?
        Decision::UpdateAvailable:Decision::UpToDate;return r;
}

// ---------------------------------------------------------------------------
// Downloaded object verification, retained for all update I/O callers.
// ---------------------------------------------------------------------------
enum class VerifyFailReason { None,MissingExpectedSha,OpenFailed,SizeMismatch,Sha256Mismatch };
struct VerifyResult { bool ok=false;VerifyFailReason reason=VerifyFailReason::None;std::string error,actualSha; };
inline VerifyResult verify_downloaded_file(const std::string& path,const std::string& expectedSha,long long expectedSize){
    VerifyResult r;if(!is_hex64(expectedSha)){r.reason=VerifyFailReason::MissingExpectedSha;r.error="缺少合法 sha256";return r;}
    std::error_code ec;const auto size=std::filesystem::file_size(std::filesystem::u8path(path),ec);
    if(ec){r.reason=VerifyFailReason::OpenFailed;r.error="下载文件不存在或不可读";return r;}
    if(expectedSize>=0&&(long long)size!=expectedSize){r.reason=VerifyFailReason::SizeMismatch;
        r.error="文件大小不符（期望 "+std::to_string(expectedSize)+"，实际 "+std::to_string((long long)size)+"）";return r;}
    std::string hex,err;if(!sha256_file(path,hex,err)){r.reason=VerifyFailReason::OpenFailed;r.error=err;return r;}
    r.actualSha=hex;if(!hex_equals_ci(hex,expectedSha)){r.reason=VerifyFailReason::Sha256Mismatch;r.error="sha256 校验失败";return r;}
    r.ok=true;return r;
}

} // namespace rta::update
