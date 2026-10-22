// provider.h — 元数据 provider:五源(dizzylab / QQ / 网易 / iTunes / lrclib)
//
// 对应 docs/元数据补全调研与规划.md §3 的 ② providers(P1 首项)。
// 端点与字段名来自 2026-10-21 的 node 真实探活(build/synchk/probe_providers.js,
// 每端点 ≤2 次请求、间隔 ≥700ms、UA 如实),字段名以探活输出为准:
//   * QQ 搜索  POST u.y.qq.com/cgi-bin/musicu.fcg  DoSearchForQQMusicDesktop — ✅
//       req_1.data.body.song.list[]:title / singer[].name / album.name / album.mid /
//       mid(歌曲级,如 0039MnYb0qxYhV) / file.media_mid(另一个 ID,勿混) /
//       interval(秒) / time_public("2003-07-31",歌曲级;专辑级也可能有)
//   * QQ 歌词  POST musicu.fcg GetPlayLyricInfo — ✅ 已验证(2026-10-21 晚,
//       build/synchk/probe_qq_lyric.js,真实请求 ×1):
//       param {"songMID":<歌曲级 mid>} 匿名可取到完整歌词 —— data.lyric 为 base64 的
//       LRC(实测「晴天」3400 字符 base64 → 1388 字符 UTF-8 LRC,[ti:晴天]
//       [ar:周杰伦]…),data.trans 为 base64 翻译。⚠️ 关键:必须用歌曲级 mid,
//       file.media_mid 是另一个 ID、取词为空(早间探活假阴性的根因,见 parse_qq_search)。
//   * 网易搜索 POST music.163.com/api/search/get — ✅
//       result.songs[]:name / artists[].name / album.{id,name,picUrl} / id / duration(毫秒)。
//       注意:探活样本里 album 可以没有 picUrl 字段(coverUrl 留空,不猜 URL)。
//   * 网易歌词 GET music.163.com/api/song/lyric?id=&lv=-1&kv=-1&tv=-1 — ✅
//       lrc.lyric(LRC 文本)/ tlyric.lyric(翻译,可为空)
//   * iTunes   GET itunes.apple.com/search — ✅
//       results[]:trackName / artistName(单串,已合并) / collectionName / primaryGenreName /
//       releaseDate(ISO) / trackId / collectionId / trackTimeMillis / artworkUrl100;
//       封面把 "100x100bb" 换 "1200x1200bb"(2026-10-21 smoke:1200 是可用上限)。
//   * lrclib   GET /api/get、/api/search — ✅(21 日早间调研时全天 503,21 日晚间探活 200;
//       503 重试+search 回退逻辑按调研文档 §5.1 保留)
//
// 设计要点(风格对齐 src/meta/llm.h):
//   * 头文件库全部 inline;namespace rta::meta;不用异常;无第三方依赖
//   * JSON 解析只用 net/json.h;JSON 写出用 ostringstream + 自带 pv_jesc(同 store.h meta_jesc 风格)
//   * HTTP 只用 net/http_client.h。双重编码坑:http_get/post 会把 URL path(含查询串)再过一遍
//     percent_encode_path(只编 <=0x20 / >=0x7F 与 " < > # \ ^ |;已编码的 % 不再编),
//     所以查询值先经 percent_enc_utf8 全量编码、再拼查询串 —— 不会二次编码(有专测)
//   * POST body 不经 percent_encode_path(http_client 只处理 path),网易表单体的编码由
//     percent_enc_utf8 自己完成
//   * 网络函数单次调用;唯一例外是 lrclib 对 503 重试 1 次(调研文档明文要求);
//     限频/熔断/代理归上层(调研文档 §3「网络纪律」)
//   * 搜索约定:err 为空 = 成功(0 条命中也是合法空结果);err 非空 = 失败
//   * 构造与解析全为纯函数,tests/test_metaproviders.cpp 离线喂写死报文(铁律 3);
//     网络函数本身不做联网测试
//
// P1.5 注册表化(2026-10-22):ProviderDesc + provider_registry() 在文件尾。
// enrich/设置页遍历注册表驱动;新增数据源 = 注册一个描述符 + 实现函数,零接线。
// 现有 search_qq/search_netease/search_itunes/lyric_* 函数原样保留(注册表指向它们/
// 薄包装 lyric_*_entry),对外兼容。
//
// dizzylab(2026-10-22,同人曲目第一优先源):站点是服务端渲染,纯 GET + HTML 提取;
// 详细实测依据、搜索策略与降级边界见「dizzylab」大节的头注(文件中部)。它排注册表第一位,
// 但 canLyric=false(无歌词接口,如实标注),采纳门槛一行未改。
#pragma once
// （原 WIN32_LEAN_AND_MEAN / NOMINMAX 前导已删：http_client.h -> platform/http.h 会在
//   windows.h 之前自己定义，行为不变。）
#include "net/http_client.h"
#include "net/json.h"
#include "meta/match.h"          // Candidate 的唯一定义(评分与 provider 共用,2026-10-21 统一)
#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace rta::meta {

// ---------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------

// Candidate 由 meta/match.h 统一定义(2026-10-21 统一:原 provider 侧重复定义删除,
// 字段并回 match.h 的唯一形态 —— enrich.h 同时用评分与溯源字段,两处定义会同名冲突)。

struct SrcErr { std::string msg; };     // 失败经出参带回,不抛异常(供上层错误词汇复用)

struct LyricResult {
    bool ok = false;
    std::string lrc, translation, source, err;
};

// 歌词统一入口的请求(P1.5 注册表化):元数据四件套 + 源内句柄。
// cands = enrich 侧按匹配分从高到低排好的候选 —— 有句柄的源(qq/netease)直接取用,
// 免去重复搜索;句柄挑选用 pv_lyric_song_id(纯函数,离线可测)。
struct LyricRequest {
    std::string title, artist, album;
    int durationSec = 0;
    std::vector<Candidate> cands;    // 源内句柄(有则用,免重复搜索)
};

// 数据源描述符(P1.5):新增数据源 = 往 provider_registry() 注册一项 + 实现函数,零接线。
//   id       设置键后缀与溯源名("qq"/"netease"/"itunes"/"lrclib";lrclib 仅歌词)
//   name     设置页显示名("QQ音乐"/"网易云"/"iTunes"/"lrclib 歌词")
//   canSearch 参与标签/封面搜索(true 才会被搜索链调用,search 才非空)
//   canLyric  参与歌词链(true 才会被歌词链调用,lyric 才非空)
//   search   std::vector<Candidate> (*)(const std::string& query, std::string& err)
//   lyric    LyricResult (*)(const LyricRequest&, std::string& err) —— 统一歌词入口
struct ProviderDesc {
    const char* id;
    const char* name;
    bool canSearch;
    bool canLyric;
    std::vector<Candidate> (*search)(const std::string&, std::string&);   // canSearch 才非空
    LyricResult (*lyric)(const LyricRequest&, std::string& err);          // canLyric 才非空
};

// ---------------------------------------------------------------------------
// 小工具(pv_ = provider detail)
// ---------------------------------------------------------------------------

// 如实 UA(lrclib 要求标识客户端;网络纪律「UA 如实」)
inline const char* pv_user_agent() { return "RLPlayer/1.0 (local music player; metadata provider)"; }

// JSON 字符串转义:\ " \n \r \t 与 <0x20 → \u00xx;>=0x80 字节(中文)原样透传。
// 与 store.h meta_jesc 同风格;本文件独立实现,不拉 store.h(避免带入文件系统依赖)。
inline std::string pv_jesc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    char b[8];
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// RFC 3986 百分号编码(UTF-8 字节串):只保留 unreserved(A-Za-z0-9-_.~),
// 其余一律 %XX 大写。用于网易 POST 表单体的值与各家 GET 查询串的值。
// 输出只含 unreserved + '%',而 http_client 的 percent_encode_path 不会再编 '%',
// 因此「先 self-encode 再拼 URL」不会双重编码(见 tests:双重编码守卫)。
inline std::string percent_enc_utf8(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o; o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unres = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unres) { o += (char)c; continue; }
        o += '%'; o += hex[c >> 4]; o += hex[c & 0x0F];
    }
    return o;
}

// 取年份:找第一段「连续数字且长度 ≥4」的数字串,取其前 4 位;找不到返回 ""。
//   "2003-07-31" → "2003";"2003-07-31T07:00:00Z" → "2003";""/"abc" → ""。
// 数字串本身 ≥5 位时取前 4(CompactDate "20030731" 也能取到 2003)。
// 合理窗口 [1000,2100]:取到的 4 位年份落在窗口内才返回,否则返回 ""。
//   ⚠️ 2026-10-22 实测事故:QQ 源对该曲返回 time_public="0000-00-00",旧版取出 "0000"
//   被采纳写回,文件 DATE 从 2022-12-24 变 "0000"。垃圾年份必须在源头丢弃;
//   本函数同时服务 QQ/netease/iTunes 三家解析,一处修三家受益
//   (写回侧另有第二道守卫,见 meta/writeback.h 的 writeback_year_ok)。
inline std::string pv_year4(const std::string& s) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        if (s[i] < '0' || s[i] > '9') { ++i; continue; }
        size_t j = i;
        while (j < n && s[j] >= '0' && s[j] <= '9') ++j;
        if (j - i >= 4) {
            const std::string y = s.substr(i, 4);
            const int v = (y[0] - '0') * 1000 + (y[1] - '0') * 100 +
                          (y[2] - '0') * 10 + (y[3] - '0');
            return (v >= 1000 && v <= 2100) ? y : std::string();
        }
        i = j;
    }
    return "";
}

// base64 解码(256 项查表,启动时构建一次):
//   * '='(表值 -2)结束数据段 —— 标准 padding;
//   * 其余表值 -1 的字节(空白/非法字符)宽松跳过 —— 真实载荷常带换行;
//   * 输出原始字节(QQ 歌词的 base64 解出来就是 UTF-8 的 LRC 文本)。
inline std::string pv_b64_decode(const std::string& in) {
    struct Tab {
        signed char v[256];
        Tab() {
            for (int i = 0; i < 256; ++i) v[i] = -1;
            static const char* a =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; a[i]; ++i) v[(unsigned char)a[i]] = (signed char)i;
            v[(unsigned char)'='] = -2;   // padding
        }
    };
    static const Tab T;
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        const signed char t = T.v[c];
        if (t == -2) break;             // '=' → 数据段结束
        if (t < 0) continue;            // 非法字符/空白:宽松跳过
        buf = (buf << 6) | (unsigned)t;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
    }
    return out;
}

// singer[] / artists[](各项含 "name")→ " / " 连接(任务 spec:多人 join 约定)
inline std::string pv_join_names(const rta::net::Json& arr) {
    std::string out;
    for (size_t i = 0; i < arr.size(); ++i) {
        const std::string n = arr.at(i)["name"].s();
        if (n.empty()) continue;
        if (!out.empty()) out += " / ";
        out += n;
    }
    return out;
}

// artworkUrl100 → 1200x1200bb(2026-10-21 smoke:#7 1200x1200bb 可用、100000x100000bb 已 400)。
// 找不到 "100x100bb" 标记时原样返回(不猜其它尺寸)。
inline std::string pv_artwork_1200(const std::string& url) {
    static const std::string from = "100x100bb";
    const size_t p = url.find(from);
    if (p == std::string::npos) return url;
    std::string o = url;
    o.replace(p, from.size(), "1200x1200bb");
    return o;
}

// GET + 503 单次重试(lrclib 当日过载场景,调研文档 §5.1:sleep 1500ms 后重试 1 次)。
// 其它状态码不重试,原样返回交调用方判断。
inline rta::net::HttpResponse pv_http_get_retry503(const std::string& url,
                                                   const std::vector<std::string>& headers) {
    rta::net::HttpResponse resp = rta::net::http_get(url, headers, 15000);
    if (resp.status == 503) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        resp = rta::net::http_get(url, headers, 15000);
    }
    return resp;
}

// ---------------------------------------------------------------------------
// QQ 搜索
// ---------------------------------------------------------------------------

// 搜索信封(实测:不要顶层 comm;键名 req_1 是惯例,与 module 无关)。
// 探活 ✅ 2026-10-21:HTTP 200,「周杰伦 晴天」精确命中《叶惠美》。
inline std::string qq_build_search_body(const std::string& query) {
    std::ostringstream o;
    o << "{\"req_1\":{\"method\":\"DoSearchForQQMusicDesktop\","
         "\"module\":\"music.search.SearchCgiService\","
         "\"param\":{\"search_type\":0,\"query\":\"" << pv_jesc(query) <<
         "\",\"page_num\":1,\"num_per_page\":8}}}";
    return o.str();
}

// 纯函数:解析 musicu.fcg 搜索回包。字段映射(探活实测键名):
//   title; singer[].name(" / " join);album.name / album.mid(→ albumId);
//   mid(歌曲级,→ songId,歌词接口用这个;缺失退 file.media_mid);
//   interval(秒 → durationMs);time_public(歌曲级优先,空则 album.time_public;取 4 位年份);
//   封面 = "https://y.gtimg.cn/music/photo_new/T002R800x800M000" + album.mid + ".jpg"(直接拼,不请求)。
// genre 字段响应里虽存在,但其取值形状未探活确认,不盲映射 → 留空。
// 上限 8 条;list 存在但为空 = 合法空结果(err 为空)。
inline std::vector<Candidate> parse_qq_search(const std::string& text, std::string& err) {
    err.clear();
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) { err = "qq search: 响应不是合法 JSON"; return {}; }
    const rta::net::Json& list = j["req_1"]["data"]["body"]["song"]["list"];
    if (list.type != rta::net::Json::Arr) { err = "qq search: 响应缺少 req_1.data.body.song.list"; return {}; }
    std::vector<Candidate> out;
    for (size_t i = 0; i < list.size() && out.size() < 8; ++i) {
        const rta::net::Json& c = list.at(i);
        Candidate d;
        d.source  = "qq";
        d.title   = c["title"].s();
        d.artist  = pv_join_names(c["singer"]);
        d.album   = c["album"]["name"].s();
        d.albumId = c["album"]["mid"].s();
        d.songId  = c["mid"].s();
        if (d.songId.empty()) d.songId = c["file"]["media_mid"].s();
        d.year    = pv_year4(c["time_public"].s());
        if (d.year.empty()) d.year = pv_year4(c["album"]["time_public"].s());
        const double interval = c["interval"].n();
        d.durationMs = interval > 0.0 ? (int)(interval * 1000.0) : 0;
        if (!d.albumId.empty())
            d.coverUrl = "https://y.gtimg.cn/music/photo_new/T002R800x800M000" + d.albumId + ".jpg";
        out.push_back(std::move(d));
    }
    return out;
}

// POST https://u.y.qq.com/cgi-bin/musicu.fcg(头 Referer: https://y.qq.com/;body 已是 JSON,
// 中文原样 UTF-8,与探活一致)。err 为空 = 成功。
inline std::vector<Candidate> search_qq(const std::string& query, std::string& err) {
    err.clear();
    const rta::net::HttpResponse resp = rta::net::http_post(
        "https://u.y.qq.com/cgi-bin/musicu.fcg", qq_build_search_body(query),
        { "Referer: https://y.qq.com/", "User-Agent: Mozilla/5.0" },
        "application/json", 15000);
    if (resp.status == 0) { err = "qq search: 网络请求失败: " + resp.error; return {}; }
    if (resp.status < 200 || resp.status >= 300) { err = "qq search: HTTP " + std::to_string(resp.status); return {}; }
    const std::string text(resp.body.begin(), resp.body.end());
    return parse_qq_search(text, err);
}

// ---------------------------------------------------------------------------
// 网易云搜索
// ---------------------------------------------------------------------------

// 表单体(UTF-8 百分号编码自己做;body 不经 http_client 的 path 编码,无双重编码问题)。
inline std::string netease_build_search_body(const std::string& query) {
    return "s=" + percent_enc_utf8(query) + "&type=1&limit=8&offset=0";
}

// 纯函数:解析 /api/search/get 回包。字段映射(探活实测键名):
//   name(→ title);artists[].name(" / " join);album.name / album.id(→ albumId);
//   id(→ songId,数值转十进制串);duration(毫秒);album.picUrl(→ coverUrl,
//   可能整字段缺失,探活实证 —— 缺失即留空,不猜 URL)。genre/year 该接口没有 → 留空。
inline std::vector<Candidate> parse_netease_search(const std::string& text, std::string& err) {
    err.clear();
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) { err = "netease search: 响应不是合法 JSON"; return {}; }
    const rta::net::Json& list = j["result"]["songs"];
    if (list.type != rta::net::Json::Arr) { err = "netease search: 响应缺少 result.songs"; return {}; }
    std::vector<Candidate> out;
    for (size_t i = 0; i < list.size() && out.size() < 8; ++i) {
        const rta::net::Json& c = list.at(i);
        Candidate d;
        d.source     = "netease";
        d.title      = c["name"].s();
        d.artist     = pv_join_names(c["artists"]);
        d.album      = c["album"]["name"].s();
        d.songId     = c["id"].s();
        d.albumId    = c["album"]["id"].s();
        d.coverUrl   = c["album"]["picUrl"].s();
        d.durationMs = (int)c["duration"].n();
        out.push_back(std::move(d));
    }
    return out;
}

// POST https://music.163.com/api/search/get,UA "Mozilla/5.0"(接口要求),
// Content-Type application/x-www-form-urlencoded。err 为空 = 成功。
inline std::vector<Candidate> search_netease(const std::string& query, std::string& err) {
    err.clear();
    const rta::net::HttpResponse resp = rta::net::http_post(
        "https://music.163.com/api/search/get", netease_build_search_body(query),
        { "User-Agent: Mozilla/5.0" },
        "application/x-www-form-urlencoded", 15000);
    if (resp.status == 0) { err = "netease search: 网络请求失败: " + resp.error; return {}; }
    if (resp.status < 200 || resp.status >= 300) { err = "netease search: HTTP " + std::to_string(resp.status); return {}; }
    const std::string text(resp.body.begin(), resp.body.end());
    return parse_netease_search(text, err);
}

// ---------------------------------------------------------------------------
// iTunes 搜索
// ---------------------------------------------------------------------------

// GET 查询 URL:term 全量百分号编码后拼串 —— 输出只含 unreserved + '%',
// http_client 的 percent_encode_path 不会再编(双重编码守卫见测试)。
inline std::string itunes_build_search_url(const std::string& query) {
    return "https://itunes.apple.com/search?term=" + percent_enc_utf8(query) +
           "&media=music&entity=song&country=TW&limit=8";
}

// 纯函数:解析 /search 回包。字段映射(探活实测键名):
//   trackName / artistName(单串,iTunes 已合并多人)/ collectionName /
//   primaryGenreName(→ genre)/ releaseDate(ISO,取 4 位年份)/
//   trackId(→ songId)/ collectionId(→ albumId)/ trackTimeMillis(→ durationMs)/
//   artworkUrl100(→ coverUrl,"100x100bb"→"1200x1200bb")。
inline std::vector<Candidate> parse_itunes_search(const std::string& text, std::string& err) {
    err.clear();
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) { err = "itunes search: 响应不是合法 JSON"; return {}; }
    const rta::net::Json& list = j["results"];
    if (list.type != rta::net::Json::Arr) { err = "itunes search: 响应缺少 results"; return {}; }
    std::vector<Candidate> out;
    for (size_t i = 0; i < list.size() && out.size() < 8; ++i) {
        const rta::net::Json& c = list.at(i);
        Candidate d;
        d.source     = "itunes";
        d.title      = c["trackName"].s();
        d.artist     = c["artistName"].s();
        d.album      = c["collectionName"].s();
        d.genre      = c["primaryGenreName"].s();
        d.year       = pv_year4(c["releaseDate"].s());
        d.songId     = c["trackId"].s();
        d.albumId    = c["collectionId"].s();
        d.durationMs = (int)c["trackTimeMillis"].n();
        d.coverUrl   = pv_artwork_1200(c["artworkUrl100"].s());
        out.push_back(std::move(d));
    }
    return out;
}

// GET https://itunes.apple.com/search?...&country=TW&limit=8。err 为空 = 成功。
// (WinHTTP 默认 UA 即 "RLPlayer/1.0",如实;iTunes 接口不要求特定 UA。)
inline std::vector<Candidate> search_itunes(const std::string& query, std::string& err) {
    err.clear();
    const rta::net::HttpResponse resp = rta::net::http_get(itunes_build_search_url(query), {}, 15000);
    if (resp.status == 0) { err = "itunes search: 网络请求失败: " + resp.error; return {}; }
    if (resp.status < 200 || resp.status >= 300) { err = "itunes search: HTTP " + std::to_string(resp.status); return {}; }
    const std::string text(resp.body.begin(), resp.body.end());
    return parse_itunes_search(text, err);
}

// ---------------------------------------------------------------------------
// dizzylab(同人曲目第一优先元数据源)
// ---------------------------------------------------------------------------
// 为什么排第一:曲库(MUSIC,653 首,同人 555)的目录名是「专辑 @ 社团」形态,而
// QQ/网易/iTunes 对同人社团的发行基本没有条目 —— P1.5 复核里 6 首「正确拒采」的曲目
// 有 4 首在 dizzylab 上有完整曲目表(见 build/p16_sample_report.txt 与本轮验收报告)。
// dizzylab 是这类发行的一手目录(作品 = 专辑,含曲名/艺术家/时长),故注册表把它排在
// 四源之前。采纳门槛不因排序变化 —— enrich_adopt 一行未动(宁缺毋错)。
//
// 站点实测(2026-10-22,只读 GET、未登录;主管探活 + 本轮逐页复核):
//   * GET /search/?s=<关键词> → 服务端渲染。参数名必须是 s(?q= 也回 200 但零结果 ——
//     当初正是踩此坑误判成 SPA)。结果按 <h4 class="text-warning">社团/作品/用户</h4> 分段;
//     作品段卡片:<a href="/d/<catalog>"> 内含 <h1>作品名</h1> + <h3>社团名</h3>
//     + <img src="https://cdn.dizzylab.net/media/cover/<catalog>.jpg!cover"> + 一段 scoped <style>
//   * GET /d/<catalog> → 服务端渲染,含完整曲目表:
//       <li style="font-size:12px" data-id="0" data-audio="https://streaming.dizzylab.net/...">
//         <span class="t-title">1. intro - Rockflos123 (01:32)</span>
//     另含 <h1 style='margin-top:18px'>作品名</h1>、社团 <h1 class="p-1"><a href='/l/社团名'>、
//     封面 https://cdn.dizzylab.net/media/cover/<catalog>.jpg(实测 200:GKSL-003 35KB / NE-06 684KB)
//   * data-audio 只是串流地址。红线「绝不下载音频」:代码只读它所在 li 的兄弟结构,
//     全文件没有任何 http_get 指向 streaming.dizzylab.net。
//
// 搜索策略(这条是想清楚才写的):enrich 传给 provider 的只有 artist+" "+title 一个串,
// 不含专辑/社团;而 dizzylab 全文检索是按全词 AND —— 实测 "Pixel Library" 命中、
// "Khrisma Pixel Library" 零命中(曲名前的艺人词把结果掐死)。所以本源做「后缀放宽阶梯」:
// 全串 → 末 3 词 → 末 2 词 → 末 1 词(去重,最多 3 次请求),取第一个有卡片的变体。
// 同人曲的曲名通常落在末段,末 2 词正好是 "Pick me!" / "Pixel Library" / "Retro Shoujo"。
//
// 曲目表 → 候选:抓到专辑页后按「轨名规范化后与查询串互为子串」筛轨(命中就只回这几轨,
// 时长随候选带出);若作品名本身与查询互为子串(用户直接拿专辑名搜),退回整张曲目表;
// 都不沾则本专辑不出候选 —— 错误专辑不该往 match 里灌噪声(宁缺毋错)。
//
// 降级(本文件最硬的要求):provider 挂在播放线程链路上(meta_on_play → enrich_track),
// 这里抛异常 = 整进程死。全部解析都是「找不到就返回空/跳过该字段」,不抛、不崩、不猜;
// 结构缺失(半个卡片、没有曲目表、HTML 截断)一律优雅降级为空候选 + err 说明。
//
// ⚠️ 已知边界(铁律 4):
//   * Candidate 没有 url 字段(match.h 是唯一定义,不归本任务改),作品 URL 由 albumId
//     (catalog)结构性可导出:dizzylab_build_album_url(albumId)。
//   * 曲目行的「曲名 - 艺术家」按最后一个 " - " 切分:标题里带 " - " 时正确,艺术家名里
//     带 " - " 时会切错(实测样本 37 轨无此形态,记录备查)。
//   * 专辑页没有发售日期字段(实测 4 张专辑页无 YYYY-MM-DD),year 留空;
//     tags 链接可当 genre,但语义未核实,不猜 → genre 留空。
//   * durationMs 填得对(单测覆盖)。**2026-10-22 已修**:此前 probe_media 不取时长导致
//     enrich 侧 fileSec 恒 0、match 的「时长差 ±3s」从 P1 起一直是中性分;现在 ffprobe 带
//     format=duration(media_info.h),fileSec 有真值,该信号端到端生效(test_media_duration 覆盖)。

namespace dl_detail {

// ASCII 空白(曲目行/标题里只认这些;全角空格在 dizzylab 文本里未出现)
inline bool dl_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string dl_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && dl_ws(s[a])) ++a;
    while (b > a && dl_ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// 追加一个码点为 UTF-8(实体反转义要用;非法码点返回 false,调用方保持原文)
inline bool dl_put_cp(std::string& o, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFFul) return false;
    if (cp < 0x80ul) { o += (char)cp; }
    else if (cp < 0x800ul) {
        o += (char)(0xC0ul | (cp >> 6));
        o += (char)(0x80ul | (cp & 0x3Ful));
    } else if (cp < 0x10000ul) {
        o += (char)(0xE0ul | (cp >> 12));
        o += (char)(0x80ul | ((cp >> 6) & 0x3Ful));
        o += (char)(0x80ul | (cp & 0x3Ful));
    } else {
        o += (char)(0xF0ul | (cp >> 18));
        o += (char)(0x80ul | ((cp >> 12) & 0x3Ful));
        o += (char)(0x80ul | ((cp >> 6) & 0x3Ful));
        o += (char)(0x80ul | (cp & 0x3Ful));
    }
    return true;
}

inline int dl_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 标签名匹配:s[i] == '<',可带 '/' 与空白,名后必须是 '>' 或空白(避免 <h1x 误判)
inline bool dl_tag_is(const std::string& s, size_t i, const char* name) {
    if (i >= s.size() || s[i] != '<') return false;
    ++i;
    if (i < s.size() && s[i] == '/') ++i;
    for (const char* p = name; *p; ++p, ++i) {
        if (i >= s.size()) return false;
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *p) return false;
    }
    if (i >= s.size()) return false;
    return s[i] == '>' || dl_ws(s[i]);
}

} // namespace dl_detail

// HTML 实体反转义(&amp; &quot; &#39; &lt; &gt; &nbsp; + 数字实体 &#NNN; &#xHH;)。
// 为什么必须做:dizzylab 的社团简介/曲目名里实测有 &amp; (MhT-TH21 的
// "Composed &amp; Arranged")与 &#39;(NE-06 的 "Alice&#39;s Adventures");不反转义
// 就把 &amp; 这种脏数据写进 MetaBase 再写回文件标签。
// 未知实体(&foo;)与不合法数字实体原样保留 —— 不猜、不丢数据。
inline std::string pv_html_unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') { o += s[i]; ++i; continue; }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { o += s[i]; ++i; continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        bool ok = true;
        std::string rep;
        if      (ent == "amp")  rep = "&";
        else if (ent == "lt")   rep = "<";
        else if (ent == "gt")   rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "apos") rep = "'";
        else if (ent == "nbsp") rep = " ";          // NBSP → 普通空格(match 的 norm 本也丢弃)
        else if (ent.size() >= 2 && ent[0] == '#') {
            const bool hex = (ent[1] == 'x' || ent[1] == 'X');
            size_t k = hex ? 2 : 1;
            unsigned long cp = 0;
            bool any = false;
            for (; k < ent.size(); ++k) {
                const int d = hex ? dl_detail::dl_hexval(ent[k])
                                  : ((ent[k] >= '0' && ent[k] <= '9') ? ent[k] - '0' : -1);
                if (d < 0) { ok = false; break; }
                cp = cp * (hex ? 16ul : 10ul) + (unsigned long)d;
                any = true;
                if (cp > 0x10FFFFul) { ok = false; break; }
            }
            if (!any) ok = false;
            if (ok && !dl_detail::dl_put_cp(rep, cp)) ok = false;
        } else ok = false;
        if (!ok) { o += s[i]; ++i; continue; }     // 未知/非法实体:原样保留
        o += rep;
        i = semi + 1;
    }
    return o;
}

// HTML 片段 → 纯文本:先整段丢弃 <style>/<script>(卡片里 h1/h3 之后紧跟一段 scoped
// style,CSS 文本会污染标题),再丢其余标签(边界补空格防 "a</b>c" 粘成 "ac"),
// 然后实体反转义、折叠空白、trim。
inline std::string pv_html_text(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') { o += s[i]; ++i; continue; }
        if (dl_detail::dl_tag_is(s, i, "style") || dl_detail::dl_tag_is(s, i, "script")) {
            const char* close = dl_detail::dl_tag_is(s, i, "style") ? "</style>" : "</script>";
            const size_t e = s.find(close, i);
            i = (e == std::string::npos) ? s.size() : e + std::string(close).size();
            o += ' ';
            continue;
        }
        const size_t e = s.find('>', i);
        if (e == std::string::npos) { i = s.size(); break; }   // 标签未闭合:停(截断页降级)
        i = e + 1;
        o += ' ';
    }
    const std::string un = pv_html_unescape(o);
    std::string t;
    t.reserve(un.size());
    bool prevWs = true;                                       // 前导空白直接吃掉
    for (const char c : un) {
        if (dl_detail::dl_ws(c)) {
            if (!prevWs) { t += ' '; prevWs = true; }
            continue;
        }
        t += c;
        prevWs = false;
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

// ---------------------------------------------------------------------------
// dizzylab:URL 构造(纯函数,测试整串精确匹配)
// ---------------------------------------------------------------------------

// 查询值走既有 percent_enc_utf8(中文/空格/标点全量编码);输出只含 unreserved + '%',
// http_client 的 percent_encode_path 不会再编 —— 无双重编码(与四源同一条守卫)。
inline std::string dizzylab_build_search_url(const std::string& query) {
    return "https://www.dizzylab.net/search/?s=" + percent_enc_utf8(query);
}

// catalog 只允许 [A-Za-z0-9_-](实测 GKSL-003 / MhT-TH21 / SSP1 / NE-06 全符合):
// 非法 catalog 一律不拼 URL —— 防把卡片里的怪字符带进请求路径。
inline bool dizzylab_catalog_ok(const std::string& catalog) {
    if (catalog.empty() || catalog.size() > 40) return false;
    for (const char c : catalog) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

inline std::string dizzylab_build_album_url(const std::string& catalog) {
    return "https://www.dizzylab.net/d/" + catalog;
}

// 封面实测两种变体都 200:裸 .jpg(GKSL-003 35KB / NE-06 684KB)与 .jpg!cover(16KB 缩略)。
// 取裸图(清晰度更高);enrich 的封面门是 >2KB,两种都能过。
inline std::string dizzylab_cover_url(const std::string& catalog) {
    return "https://cdn.dizzylab.net/media/cover/" + catalog + ".jpg";
}

// ---------------------------------------------------------------------------
// dizzylab:搜索结果页解析(纯函数)
// ---------------------------------------------------------------------------

// 作品卡片(搜索页粒度 = 专辑)。标题/社团名/封面都来自卡片内第一个 h1/h3/img。
struct DlWorkCard {
    std::string catalog;      // /d/ 后的编号(→ Candidate.albumId)
    std::string album;        // 作品名(卡片第一个 <h1>)
    std::string circle;       // 社团名(卡片第一个 <h3>;缺则空 → 降级)
};

// 「作品」段的起止:标题字面量 >作品</h4> 之后 → 下一个 <h4 之前。
// 找不到该标题(含搜索零结果页)= 空串区间;社团段/用户段的卡片一律不取。
inline bool dl_work_section(const std::string& html, size_t& begin, size_t& end) {
    static const std::string kMark = ">作品</h4>";
    const size_t p = html.find(kMark);
    if (p == std::string::npos) return false;
    begin = p + kMark.size();
    const size_t n = html.find("<h4", begin);
    end = (n == std::string::npos) ? html.size() : n;
    return begin < end;
}

// 从 [pos,end) 里取第一个 <tag ...>…</tag> 的纯文本;找不到返回空串(降级,不报错)
inline std::string dl_first_tag_text(const std::string& s, const size_t pos, const size_t end,
                                    const char* tag) {
    size_t i = pos;
    while (i < end) {
        if (dl_detail::dl_tag_is(s, i, tag)) break;
        ++i;
    }
    if (i >= end) return "";
    const size_t gt = s.find('>', i);
    if (gt == std::string::npos || gt >= end) return "";
    const std::string close = std::string("</") + tag + ">";
    const size_t ce = s.find(close, gt + 1);
    if (ce == std::string::npos || ce > end) return "";
    return pv_html_text(s.substr(gt + 1, ce - gt - 1));
}

// 解析作品段里全部 /d/<catalog> 卡片。返回 0 条 = 搜索无结果(合法),err 保持空。
inline std::vector<DlWorkCard> dl_parse_work_cards(const std::string& html) {
    std::vector<DlWorkCard> out;
    size_t begin = 0, end = 0;
    if (!dl_work_section(html, begin, end)) return out;

    size_t i = begin;
    while (i < end) {
        // 卡片起点:href="/d/xxx" 或 href='/d/xxx'(站点两种引号都出现过)。
        // 前缀 "href=" + 引号 + "/d/" 共 9 字节;引号字符记下来用于找闭合引号。
        size_t a = std::string::npos;
        char quote = '"';
        {
            const size_t q1 = html.find("href=\"/d/", i);
            const size_t q2 = html.find("href='/d/", i);
            if (q1 != std::string::npos && q1 < end) { a = q1; quote = '"'; }
            if (q2 != std::string::npos && q2 < end && (a == std::string::npos || q2 < a)) {
                a = q2; quote = '\'';
            }
        }
        if (a == std::string::npos) break;
        const size_t catStart = a + 9;                      // "href=" + quote + "/d/"
        const size_t catEnd = html.find(quote, catStart);
        if (catEnd == std::string::npos || catEnd > end) break;
        DlWorkCard c;
        c.catalog = html.substr(catStart, catEnd - catStart);

        // 卡片终点:下一个 </a>;防御式上限 = 下一个 <a (卡片之间锚点不嵌套,实测)
        size_t close = html.find("</a>", catEnd);
        if (close == std::string::npos || close > end) close = end;
        const size_t nextA = html.find("<a ", catEnd);
        if (nextA != std::string::npos && nextA < close) close = nextA;

        if (dizzylab_catalog_ok(c.catalog)) {
            c.album = dl_first_tag_text(html, catEnd, close, "h1");
            c.circle = dl_first_tag_text(html, catEnd, close, "h3");
            if (!c.album.empty()) out.push_back(std::move(c));   // 没作品名的卡片不要(降级)
        }
        i = (close > i) ? close : i + 1;
    }
    return out;
}

// 搜索页 → 候选(卡片粒度:title=作品名, artist=社团名, album=作品名, albumId=编号,
// coverUrl=按编号拼的封面)。供 search_dizzylab 选专辑用,同时也是离线单测的入口。
// err 非空 = 页面不像 dizzylab 搜索页(h4 段缺失);空 = 合法(含 0 条命中)。
inline std::vector<Candidate> parse_dizzylab_search(const std::string& text, std::string& err) {
    err.clear();
    std::vector<Candidate> out;
    const std::vector<DlWorkCard> cards = dl_parse_work_cards(text);
    if (cards.empty()) {
        // 「空」的三种成因必须分开(判据错了会把改版静默当成无结果,或把无结果当成故障):
        //   ① 不是 dizzylab 搜索页(网关错误页/空响应/被劫持)→ err,如实报错;
        //   ② 是搜索页但这次真没命中 → err 保持空(合法空结果,上层继续试其它源)。
        // ⚠️ 实测依据(2026-10-22,GET /search/?s=zzzqqq不存在):**真实零结果页一个
        //   <h4 class="text-warning"> 段都没有**(社团/作品/用户三段全不渲染),只剩搜索表单。
        //   所以「没有作品段」不能当作结构变化的证据 —— 页面身份靠搜索表单识别。
        //   代价:站点大改版且恰好保留表单时,会退化成「无结果」而不是报错。这是有意的取舍
        //   (报错会让 6 首同人曲的 note 被噪声填满,而改版本身有离线 fixture 先红来提醒)。
        const bool looksDizzylab = text.find("action=\"/albums/search\"") != std::string::npos ||
                                   text.find("id=\"search-field\"") != std::string::npos ||
                                   text.find("text-warning\">作品</h4>") != std::string::npos ||
                                   text.find("dizzylab") != std::string::npos;
        if (!looksDizzylab) { err = "dizzylab search: 响应不像 dizzylab 搜索页"; return out; }
        return out;                                        // 合法空结果
    }
    for (const DlWorkCard& c : cards) {
        Candidate d;
        d.source   = "dizzylab";
        d.title    = c.album;
        d.artist   = c.circle;
        d.album    = c.album;
        d.albumId  = c.catalog;
        d.coverUrl = dizzylab_cover_url(c.catalog);
        out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------
// dizzylab:专辑页解析(纯函数)—— 曲目行格式实测「序号. 曲名 - 艺术家 (MM:SS)」
// ---------------------------------------------------------------------------

struct DlTrack {
    std::string title, artist;
    int durationMs = 0;          // 0 = 该行没带时长(评分侧按中性处理)
};

// 一行曲目文本 → (曲名, 艺术家, 时长)。任何一段缺失都不算错,只是该字段为空。
// 「曲名 - 艺术家」按最后一个 " - " 切:标题里带 " - " 时切法正确(艺术家带 " - " 时
// 会切错,实测样本无此形态 —— 见头注已知边界)。
inline bool dl_parse_track_line(const std::string& raw, DlTrack& t) {
    t = DlTrack{};                       // 出参先清空:复用同一 DlTrack 时不能让上一条的时长残留
    std::string s = pv_html_text(raw);
    if (s.empty()) return false;

    // ① 尾时长 "(MM:SS)" / "(H:MM:SS)":取最后一个 '(' 到行尾的 ')' 之间
    const size_t lp = s.rfind('(');
    if (lp != std::string::npos && !s.empty() && s.back() == ')') {
        const std::string in = s.substr(lp + 1, s.size() - lp - 2);
        size_t colons = 0;
        bool digitsOk = true;
        int parts[3] = { 0, 0, 0 }, n = 0;
        for (const char c : in) {
            if (c == ':') { ++colons; if (n < 2) ++n; continue; }
            if (c < '0' || c > '9') { digitsOk = false; break; }
            if (n < 3) parts[n] = parts[n] * 10 + (c - '0');
        }
        if (digitsOk && colons >= 1 && colons <= 2) {
            int sec = 0;
            if (colons == 1)      sec = parts[0] * 60 + parts[1];
            else                  sec = parts[0] * 3600 + parts[1] * 60 + parts[2];
            if (sec > 0 && sec < 36000) t.durationMs = sec * 1000;
            s = dl_detail::dl_trim(s.substr(0, lp));        // 时长从文本里剥掉
        }
    }
    if (s.empty()) return false;

    // ② 前导序号 "12. "
    size_t p = 0;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    if (p > 0 && p < s.size() && s[p] == '.') {
        ++p;
        while (p < s.size() && dl_detail::dl_ws(s[p])) ++p;
        s = s.substr(p);
    }
    if (s.empty()) return false;

    // ③ "曲名 - 艺术家"(最后一个 " - ")
    const size_t dash = s.rfind(" - ");
    if (dash != std::string::npos) {
        t.title  = dl_detail::dl_trim(s.substr(0, dash));
        t.artist = dl_detail::dl_trim(s.substr(dash + 3));
    } else {
        t.title  = dl_detail::dl_trim(s);
        t.artist.clear();
    }
    return !t.title.empty();
}

// 专辑页 → 候选(轨粒度:title/artist/album + durationMs + coverUrl)。
// 曲目表 = 全部 <span class="t-title">…</span>(实测 NE-06 11 轨 / GKSL-003 2 轨 /
// SSP1 14 轨 / MhT-TH21 10 轨);解析失败的行跳过,不猜。
// err 非空 = 页面里找不到曲目表(结构变化/被拦截)—— 调用方据此把该专辑当一个失败。
inline std::vector<Candidate> dl_parse_album_ex(const std::string& text, const std::string& catalog,
                                             std::string& err, std::string* outCircle) {
    err.clear();
    std::vector<Candidate> out;
    if (outCircle) outCircle->clear();
    if (!dizzylab_catalog_ok(catalog)) { err = "dizzylab album: 编号非法"; return out; }

    // 作品名 = 页面第一个 <h1>(实测 <h1 style='margin-top:18px'>作品名</h1>);
    // 社团名 = 社团那个 h1 里的 /l/ 链接文本(实测 <h1 class="p-1"...><a href='/l/社团'>)
    std::string albumTitle = dl_first_tag_text(text, 0, text.size(), "h1");
    std::string circle;
    {
        size_t i = 0;
        while (i < text.size()) {
            if (dl_detail::dl_tag_is(text, i, "h1")) {
                const size_t gt = text.find('>', i);
                if (gt == std::string::npos) break;
                const size_t ce = text.find("</h1>", gt + 1);
                if (ce == std::string::npos) break;
                const std::string seg = text.substr(gt + 1, ce - gt - 1);
                const size_t l = seg.find("/l/");
                if (l != std::string::npos) {
                    const size_t q = seg.find_first_of("\"'", l);
                    if (q != std::string::npos) {
                        const char quote = seg[q];
                        const size_t q2 = seg.find(quote, q + 1);
                        if (q2 != std::string::npos) {
                            const size_t n1 = seg.find('>', q2);
                            if (n1 != std::string::npos) {
                                const size_t n2 = seg.find("</a>", n1 + 1);
                                if (n2 != std::string::npos) {
                                    circle = pv_html_text(seg.substr(n1 + 1, n2 - n1 - 1));
                                    if (!circle.empty()) break;
                                }
                            }
                        }
                    }
                }
                i = ce + 5;
                continue;
            }
            ++i;
        }
    }
    if (circle.empty()) {
        // 退路:<meta name="description" content='来自<社团>的<作品>:…'>(实测两页都成立)
        const size_t d = text.find("name=\"description\"");
        if (d != std::string::npos) {
            const size_t c0 = text.find("content=", d);
            if (c0 != std::string::npos) {
                const size_t q0 = text.find_first_of("\"'", c0);
                if (q0 != std::string::npos) {
                    const char quote = text[q0];
                    const size_t q1 = text.find(quote, q0 + 1);
                    if (q1 != std::string::npos) {
                        std::string desc = pv_html_unescape(text.substr(q0 + 1, q1 - q0 - 1));
                        const size_t p1 = desc.find("\u6765\u81ea");   // 来自
                        if (p1 != std::string::npos) {
                            const std::string rest = desc.substr(p1 + 6);   // 6 = "来自" UTF-8 字节数
                            const size_t p2 = rest.find("\u7684");         // 的
                            if (p2 != std::string::npos) circle = rest.substr(0, p2);
                        }
                    }
                }
            }
        }
    }
    if (albumTitle.empty()) albumTitle = catalog;      // 极端降级:用编号当专辑名(至少可溯源)

    static const std::string kSpan = "<span class=\"t-title\">";
    size_t i = 0;
    while ((i = text.find(kSpan, i)) != std::string::npos) {
        const size_t b = i + kSpan.size();
        const size_t e = text.find("</span>", b);
        if (e == std::string::npos) break;
        DlTrack t;
        if (dl_parse_track_line(text.substr(b, e - b), t)) {
            Candidate d;
            d.source     = "dizzylab";
            d.title      = t.title;
            d.artist     = t.artist;
            d.album      = albumTitle;
            d.albumId    = catalog;
            d.durationMs = t.durationMs;
            d.coverUrl   = dizzylab_cover_url(catalog);
            out.push_back(std::move(d));
        }
        i = e + 7;
    }
    if (out.empty()) err = "dizzylab album: 未找到曲目表(页面结构变化或该作品无试听轨)";
    if (outCircle) *outCircle = circle;
    return out;
}

// 专辑页 → 候选(公开形态:不带社团名出参;单测与外部调用用这个)
inline std::vector<Candidate> parse_dizzylab_album(const std::string& text, const std::string& catalog,
                                                   std::string& err) {
    return dl_parse_album_ex(text, catalog, err, nullptr);
}

// ---------------------------------------------------------------------------
// dizzylab:搜索关键词「后缀放宽阶梯」(纯函数)
// ---------------------------------------------------------------------------

// 全串 → 末 3 词 → 末 2 词 → 末 1 词;去重保序;最多 kMax 次(每次 1 个请求,礼节)。
// 为什么要放宽:实测 dizzylab 检索是全词 AND,"Khrisma Pixel Library" 零命中而
// "Pixel Library" 命中(见头注)。
inline constexpr size_t kDizzylabMaxSearchAttempts = 3;

inline std::vector<std::string> pv_dl_query_ladder(const std::string& query) {
    // ① 切词:ASCII 空白 + 全角空格 U+3000(E3 80 80)都当分隔。
    //    为什么不直接把原串当第一个变体:全角空格发到表单里会被编成 %E3%80%80,
    //    dizzylab 不把它当分隔 —— 归一成一个普通空格才有意义(测试覆盖该形态)。
    std::vector<std::string> toks;
    std::string cur;
    size_t i = 0;
    while (i < query.size()) {
        if (query.compare(i, 3, "\xE3\x80\x80") == 0) {       // 全角空格
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            i += 3;
            continue;
        }
        const char c = query[i];
        if (dl_detail::dl_ws(c)) {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
        ++i;
    }
    if (!cur.empty()) toks.push_back(cur);

    // ② 组装变体:全串 → 末 3 → 末 2 → 末 1(去重保序,最多 kMax 个)
    std::vector<std::string> out;
    auto joinFrom = [&](const size_t from) {
        std::string v;
        for (size_t k = from; k < toks.size(); ++k) {
            if (!v.empty()) v += ' ';
            v += toks[k];
        }
        return v;
    };
    auto push = [&](const std::string& v) {
        if (v.size() < 2) return;                       // 过短关键词太宽,不发
        for (const std::string& x : out) if (x == v) return;   // 去重
        out.push_back(v);
    };
    if (!toks.empty()) push(joinFrom(0));                // 全串(空白已归一)
    for (const size_t n : { (size_t)3, (size_t)2, (size_t)1 }) {
        if (toks.size() <= n) continue;                  // 与全串相同时跳过(去重已覆盖)
        push(joinFrom(toks.size() - n));
        if (out.size() >= kDizzylabMaxSearchAttempts) break;
    }
    if (out.size() > kDizzylabMaxSearchAttempts) out.resize(kDizzylabMaxSearchAttempts);
    return out;
}

// ---------------------------------------------------------------------------
// dizzylab:限频 + 失败熔断(网络礼节)
// ---------------------------------------------------------------------------
// 为什么需要熔断:dizzylab 是独立小站(非 CDN 大厂),播放链路会在每首歌触发一次
// meta_on_play;若站点挂了/被限流,不能每首歌都重试四次把它砸穿。规则照抄「上次失败
// 就停手」的通用做法(项目既有源只做单次调用,熔断由本源自带 —— 全文件唯一的连续
// 失败计数):
//   * 相邻两次请求间隔 ≥ kDizzylabMinIntervalMs(1 req/s);
//   * 连续 kDizzylabFailLimit 次失败(网络失败 / 非 2xx / 解析异常)→ 冷却
//     kDizzylabCooldownSec 秒,期内任何调用直接返回空 + err(不发请求);
//   * 任一次成功 → 失败计数清零。
// 计数是进程级静态量(不落盘):重启即重试,符合「小站恢复后自动可用」。
inline constexpr int  kDizzylabMinIntervalMs = 1000;
inline constexpr int  kDizzylabFailLimit      = 3;
inline constexpr int  kDizzylabCooldownSec    = 120;
inline constexpr int  kDizzylabTimeoutMs      = 15000;   // 与四源同(15s)

struct DlGate {
    std::chrono::steady_clock::time_point lastCall{};   // 上次请求发出时刻
    int  fails = 0;                                      // 连续失败次数
    std::chrono::steady_clock::time_point coolUntil{};   // 冷却截止
};
inline DlGate& dl_gate() { static DlGate g; return g; }
inline std::mutex& dl_gate_mtx() { static std::mutex m; return m; }

// 取到「可以发请求」的许可:限频 sleep + 熔断判定都在锁内做(临界区最多 1s 的 sleep,
// 但 dizzylab 只有这一条调用路径,且 enrich 侧本来就是串行,阻塞可接受)。
// 返回 false = 熔断中,调用方直接返回空候选(不重试、不抛)。
inline bool dl_gate_acquire(std::string& err) {
    std::lock_guard<std::mutex> lk(dl_gate_mtx());
    DlGate& g = dl_gate();
    const auto now = std::chrono::steady_clock::now();
    if (now < g.coolUntil) {
        const long long left = (long long)std::chrono::duration_cast<std::chrono::seconds>(
                                   g.coolUntil - now).count() + 1;
        err = "dizzylab: 连续失败熔断中,剩 " + std::to_string(left) + "s 不发起请求";
        return false;
    }
    if (g.lastCall.time_since_epoch().count() != 0) {
        const auto wait = std::chrono::milliseconds(kDizzylabMinIntervalMs) - (now - g.lastCall);
        if (wait.count() > 0) std::this_thread::sleep_for(wait);
    }
    g.lastCall = std::chrono::steady_clock::now();
    return true;
}

inline void dl_gate_success() {
    std::lock_guard<std::mutex> lk(dl_gate_mtx());
    dl_gate().fails = 0;
}

// 记一次失败;达阈值 → 开冷却窗(冷却结束自动恢复,失败计数清零)
inline void dl_gate_fail() {
    std::lock_guard<std::mutex> lk(dl_gate_mtx());
    DlGate& g = dl_gate();
    if (++g.fails >= kDizzylabFailLimit) {
        g.coolUntil = std::chrono::steady_clock::now() +
                      std::chrono::seconds(kDizzylabCooldownSec);
        g.fails = 0;
    }
}

// 单次 GET(带 UA + 限频 + 熔断记账)。返回空 body 表示失败,err 已写明原因。
inline std::string dl_http_get(const std::string& url, std::string& err) {
    if (!dl_gate_acquire(err)) return "";
    const rta::net::HttpResponse resp = rta::net::http_get(
        url, { std::string("User-Agent: ") + pv_user_agent() }, kDizzylabTimeoutMs);
    if (resp.status == 0) {
        dl_gate_fail();
        err = "请求失败: " + resp.error;
        return "";
    }
    if (resp.status < 200 || resp.status >= 300) {
        dl_gate_fail();
        err = "HTTP " + std::to_string(resp.status);
        return "";
    }
    dl_gate_success();
    return std::string(resp.body.begin(), resp.body.end());
}

// ---------------------------------------------------------------------------
// dizzylab:搜索(网络函数)—— 搜索页阶梯 → 选专辑 → 专辑页拿曲目表
// ---------------------------------------------------------------------------

// 一条曲目的「是否与查询沾边」:双方规范化后互为子串(用 match 侧的 norm,全半角/大小写/
// 标点全部折叠,同人曲名里的 !/・/全角括号差异不会导致漏筛)。
inline bool dl_text_hit(const std::string& qNorm, const std::string& cand) {
    const std::string c = match_norm(cand);
    if (qNorm.empty() || c.empty()) return false;
    return qNorm.find(c) != std::string::npos || c.find(qNorm) != std::string::npos;
}

// 搜索候选数上限(与四源一致:各源 ≤8 条)。专辑页按排序后的前 kDizzylabMaxAlbums 张抓,
// 每张 1 次请求 —— 礼节上限:最多 kDizzylabMaxSearchAttempts(搜索) + 2(专辑)= 5 次/次调用。
inline constexpr size_t kDizzylabMaxAlbums = 2;

// 搜索:GET /search/?s=…(阶梯)→ 作品卡片按与查询串的相似度排序 → 取前 2 张专辑页 →
// 每张专辑按轨筛候选。err 空 = 成功(0 条也是合法空结果)。
inline std::vector<Candidate> search_dizzylab(const std::string& query, std::string& err) {
    err.clear();
    const std::string q = dl_detail::dl_trim(query);
    if (q.size() < 2) { err = "dizzylab search: 查询串过短"; return {}; }
    const std::string qNorm = match_norm(q);

    // ① 搜索页阶梯:第一个拿到作品卡片的变体就算命中
    std::vector<Candidate> cards;
    std::vector<std::string> tried;
    for (const std::string& variant : pv_dl_query_ladder(q)) {
        tried.push_back(variant);
        std::string herr;
        const std::string html = dl_http_get(dizzylab_build_search_url(variant), herr);
        if (html.empty()) {
            // 熔断中/网络失败:不再往下试(继续试只是更礼貌地失败)
            err = herr;
            return {};
        }
        std::string perr;
        cards = parse_dizzylab_search(html, perr);
        if (!perr.empty()) { err = perr; return {}; }
        if (!cards.empty()) break;
    }
    if (cards.empty()) {
        // 合法空结果:不是错误(让上层继续试其它源)。note 里带上试过的关键词供排查。
        err.clear();
        return {};
    }

    // ② 选专辑:作品名/社团名与查询串沾边优先,其余按原序补位(最多 2 张)
    std::vector<size_t> order;
    for (size_t i = 0; i < cards.size(); ++i)
        if (dl_text_hit(qNorm, cards[i].album) || dl_text_hit(qNorm, cards[i].artist))
            order.push_back(i);
    for (size_t i = 0; i < cards.size() && order.size() < kDizzylabMaxAlbums; ++i) {
        bool dup = false;
        for (const size_t k : order) if (k == i) dup = true;
        if (!dup) order.push_back(i);
    }
    if (order.size() > kDizzylabMaxAlbums) order.resize(kDizzylabMaxAlbums);

    // ③ 逐张专辑页拿曲目表,按轨筛(见头注「曲目表 → 候选」)
    std::vector<Candidate> out;
    std::string lastErr;
    for (const size_t ci : order) {
        if (out.size() >= 8) break;
        const Candidate& card = cards[ci];
        std::string herr;
        const std::string ahtml = dl_http_get(dizzylab_build_album_url(card.albumId), herr);
        if (ahtml.empty()) { lastErr = herr; continue; }     // 网络失败/熔断:交给上层记 note
        std::string aerr, circle;
        std::vector<Candidate> tracks = dl_parse_album_ex(ahtml, card.albumId, aerr, &circle);
        if (tracks.empty()) { if (!aerr.empty()) lastErr = aerr; continue; }

        // 作品名是否本身就是用户在找的东西(拿专辑名搜):命中则整张都给
        const bool albumItselfHit = dl_text_hit(qNorm, card.album);
        for (Candidate& t : tracks) {
            if (out.size() >= 8) break;
            if (!albumItselfHit && !dl_text_hit(qNorm, t.title)) continue;
            // 艺术家优先取【曲目行自带的那一轨】(实测 "1. Pixel Library - Khrisma"),
            // 它才是真实艺人;卡片 h3 / 专辑页 /l/ 是社团名,仅在一轨没写艺术家时兜底。
            // 用社团名当 artist 会让 artist_score 归零(0/1.0 之差直接决定过不过采纳门槛)。
            if (t.artist.empty()) t.artist = !card.artist.empty() ? card.artist : circle;
            out.push_back(std::move(t));
        }
    }
    if (out.empty() && !lastErr.empty()) err = lastErr;    // 有专辑但一条轨都没筛出来:如实报
    return out;
}

// ---------------------------------------------------------------------------
// 歌词:URL / body 构造(纯函数)
// ---------------------------------------------------------------------------

// GET https://lrclib.net/api/get?track_name=&artist_name=&album_name=&duration=(秒)
inline std::string lrclib_build_get_url(const std::string& title, const std::string& artist,
                                        const std::string& album, int durationSec) {
    return "https://lrclib.net/api/get?track_name=" + percent_enc_utf8(title) +
           "&artist_name=" + percent_enc_utf8(artist) +
           "&album_name=" + percent_enc_utf8(album) +
           "&duration=" + std::to_string(durationSec < 0 ? 0 : durationSec);
}

// GET https://lrclib.net/api/search?track_name=&artist_name=
inline std::string lrclib_build_search_url(const std::string& title, const std::string& artist) {
    return "https://lrclib.net/api/search?track_name=" + percent_enc_utf8(title) +
           "&artist_name=" + percent_enc_utf8(artist);
}

// GET https://music.163.com/api/song/lyric?id=<id>&lv=-1&kv=-1&tv=-1
inline std::string netease_build_lyric_url(long long songId) {
    return "https://music.163.com/api/song/lyric?id=" + std::to_string(songId) + "&lv=-1&kv=-1&tv=-1";
}

// QQ 歌词信封。✅ 已验证(2026-10-21 晚,probe_qq_lyric.js):param {"songMID":<歌曲级 mid>}
// 匿名返回完整 base64 LRC。早期探活 data.lyric 为空是因为喂了 file.media_mid(另一个 ID),
// 与 param 形状无关;调用方必须传歌曲级 mid(parse_qq_search 的 songId 已修正)。
inline std::string qq_build_lyric_body(const std::string& songMid) {
    std::ostringstream o;
    o << "{\"req_1\":{\"method\":\"GetPlayLyricInfo\","
         "\"module\":\"music.musichallSong.PlayLyricInfo\","
         "\"param\":{\"songMID\":\"" << pv_jesc(songMid) << "\"}}}";
    return o.str();
}

// ---------------------------------------------------------------------------
// 歌词:响应解析(纯函数)
// ---------------------------------------------------------------------------

// lrclib 单对象:get 的响应对象 / search 命中项共用。syncedLyrics 优先,空则 plainLyrics;
// 两者都空 → ok=false。
inline LyricResult pv_lrclib_obj(const rta::net::Json& j) {
    LyricResult r;
    r.source = "lrclib";
    const std::string synced = j["syncedLyrics"].s();
    if (!synced.empty()) { r.lrc = synced; r.ok = true; return r; }
    const std::string plain = j["plainLyrics"].s();
    if (!plain.empty()) { r.lrc = plain; r.ok = true; return r; }
    r.err = "lrclib: 无歌词字段";
    return r;
}

// /api/get 响应(单 JSON 对象)
inline LyricResult parse_lrclib_get(const std::string& text) {
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        LyricResult r; r.source = "lrclib";
        r.err = "lrclib: 响应不是合法 JSON";
        return r;
    }
    return pv_lrclib_obj(j);
}

// /api/search 响应(JSON 数组)→ 取第一条(synced 优先,空则 plain;与 get 同口径)
inline LyricResult parse_lrclib_search_first(const std::string& text) {
    LyricResult r;
    r.source = "lrclib";
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Arr) { r.err = "lrclib search: 响应不是 JSON 数组"; return r; }
    if (j.size() == 0) { r.err = "lrclib search: 无结果"; return r; }
    return pv_lrclib_obj(j.at(0));
}

// 网易歌词:lrc.lyric(LRC 文本)+ tlyric.lyric(翻译,可为空)。探活 ✅。
inline LyricResult parse_netease_lyric(const std::string& text) {
    LyricResult r;
    r.source = "netease";
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) { r.err = "netease lyric: 响应不是合法 JSON"; return r; }
    r.lrc        = j["lrc"]["lyric"].s();
    r.translation = j["tlyric"]["lyric"].s();
    if (r.lrc.empty()) { r.err = "netease lyric: 响应缺少 lrc.lyric 或歌词为空"; return r; }
    r.ok = true;
    return r;
}

// QQ 歌词:req_1.data.lyric 是 base64 的 LRC → pv_b64_decode;data.trans(可为空)是
// base64 的翻译。端到端已验证(2026-10-21,见头注与 qq_build_lyric_body);
// 解析逻辑另由写死报文测试覆盖(tests/test_metaproviders.cpp)。
inline LyricResult parse_qq_lyric(const std::string& text) {
    LyricResult r;
    r.source = "qq";
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) { r.err = "qq lyric: 响应不是合法 JSON"; return r; }
    const std::string b64 = j["req_1"]["data"]["lyric"].s();
    if (b64.empty()) { r.err = "qq lyric: 响应缺少 req_1.data.lyric 或歌词为空"; return r; }
    r.lrc = pv_b64_decode(b64);
    if (r.lrc.empty()) { r.err = "qq lyric: base64 解码结果为空(lyric 内容异常)"; return r; }
    const std::string tb64 = j["req_1"]["data"]["trans"].s();
    if (!tb64.empty()) r.translation = pv_b64_decode(tb64);
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// 歌词:网络函数
// ---------------------------------------------------------------------------

// lrclib:GET /api/get → syncedLyrics(空则 plainLyrics);
// 404/无果 → 回退 GET /api/search 取第一条;
// 503(当日过载)→ pv_http_get_retry503 内重试 1 次(sleep 1500ms),仍 503 则如实填 err 放弃。
inline LyricResult lyric_lrclib(const std::string& title, const std::string& artist,
                                int durationSec, const std::string& album) {
    LyricResult r;
    r.source = "lrclib";
    const std::vector<std::string> headers = { std::string("User-Agent: ") + pv_user_agent() };

    rta::net::HttpResponse resp =
        pv_http_get_retry503(lrclib_build_get_url(title, artist, album, durationSec), headers);
    if (resp.status == 0) { r.err = "lrclib: 网络请求失败: " + resp.error; return r; }
    if (resp.status == 503) {
        r.err = "lrclib: /api/get HTTP 503(服务过载,重试 1 次后仍 503)";
        return r;
    }
    if (resp.status == 200) {
        const std::string text(resp.body.begin(), resp.body.end());
        r = parse_lrclib_get(text);
        if (r.ok) return r;             // 拿到词,直接用
    } else if (resp.status != 404) {    // 404 → 走 search 回退;其它错误如实报
        r.err = "lrclib: /api/get HTTP " + std::to_string(resp.status);
        return r;
    }

    // ② 回退 /api/search,取第一条
    resp = pv_http_get_retry503(lrclib_build_search_url(title, artist), headers);
    if (resp.status == 0) { r = LyricResult{}; r.source = "lrclib"; r.err = "lrclib: search 回退网络请求失败: " + resp.error; return r; }
    if (resp.status == 503) { r = LyricResult{}; r.source = "lrclib"; r.err = "lrclib: /api/search HTTP 503(服务过载,重试 1 次后仍 503)"; return r; }
    if (resp.status != 200) {
        r = LyricResult{}; r.source = "lrclib";
        r.err = "lrclib: /api/search HTTP " + std::to_string(resp.status);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    return parse_lrclib_search_first(text);
}

// 网易歌词:GET /api/song/lyric?id=&lv=-1&kv=-1&tv=-1,UA "Mozilla/5.0"。探活 ✅。
inline LyricResult lyric_netease(long long songId) {
    const rta::net::HttpResponse resp = rta::net::http_get(
        netease_build_lyric_url(songId), { "User-Agent: Mozilla/5.0" }, 15000);
    if (resp.status == 0) {
        LyricResult r; r.source = "netease";
        r.err = "netease lyric: 网络请求失败: " + resp.error;
        return r;
    }
    if (resp.status < 200 || resp.status >= 300) {
        LyricResult r; r.source = "netease";
        r.err = "netease lyric: HTTP " + std::to_string(resp.status);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    return parse_netease_lyric(text);
}

// QQ 歌词:POST musicu.fcg GetPlayLyricInfo。
// ✅ 端到端已验证(2026-10-21 晚,build/synchk/probe_qq_lyric.js,真实请求 ×1):
// 歌曲级 mid 匿名取到完整 base64 LRC(「晴天」解码 1388 字符,[ti:晴天][ar:周杰伦])。
// 前提:songMid 必须是歌曲级 mid;喂 file.media_mid 会得到空 lyric(往期「未验证」
// 标注的根因,已于本日修正并把标注升级为已验证事实 —— 铁律 4 留痕)。
inline LyricResult lyric_qq(const std::string& songMid) {
    const rta::net::HttpResponse resp = rta::net::http_post(
        "https://u.y.qq.com/cgi-bin/musicu.fcg", qq_build_lyric_body(songMid),
        { "Referer: https://y.qq.com/", "User-Agent: Mozilla/5.0" },
        "application/json", 15000);
    if (resp.status == 0) {
        LyricResult r; r.source = "qq";
        r.err = "qq lyric: 网络请求失败: " + resp.error;
        return r;
    }
    if (resp.status < 200 || resp.status >= 300) {
        LyricResult r; r.source = "qq";
        r.err = "qq lyric: HTTP " + std::to_string(resp.status);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    LyricResult r = parse_qq_lyric(text);
    if (!r.ok && r.err.find("缺少 req_1.data.lyric") != std::string::npos) {
        r.err += "(可能传了 file.media_mid 而非歌曲级 mid,或该曲确无歌词)";
    }
    return r;
}

// ---------------------------------------------------------------------------
// 注册表(P1.5):歌词统一入口(薄包装,内部复用上面的现有函数)+ 静态注册表
// ---------------------------------------------------------------------------

// 歌词句柄挑选(纯函数,离线可测):按候选数组顺序找第一个来自 source 且 songId 非空的
// 候选,返回其 songId。调用方(enrich)传入的 cands 已按匹配分从高到低排好,
// 所以「第一个」=「分数最高」,与 P1 的 ed_lyric_song_id 语义一致。
inline std::string pv_lyric_song_id(const std::vector<Candidate>& cands, const char* source) {
    for (const Candidate& c : cands)
        if (c.source == source && !c.songId.empty()) return c.songId;
    return "";
}

// QQ 歌词入口:从 cands 找歌曲级 mid → lyric_qq。找不到句柄 = 本源静默不参与
// (ok=false、err 空 —— 上层与 P1 相同地落到「各源无词/未启用」,不算错误)。
inline LyricResult lyric_qq_entry(const LyricRequest& rq, std::string& err) {
    err.clear();
    LyricResult r; r.source = "qq";
    const std::string mid = pv_lyric_song_id(rq.cands, "qq");
    if (mid.empty()) return r;                       // 候选里没有 qq 句柄:不参与
    r = lyric_qq(mid);                               // songMid = 歌曲级 mid(任务 A)
    err = r.err;
    return r;
}

// 网易歌词入口:从 cands 找数字 id → lyric_netease。句柄缺失/非法(id<=0)不参与,
// 与 P1 的 sid/strtoll 门完全一致。
inline LyricResult lyric_netease_entry(const LyricRequest& rq, std::string& err) {
    err.clear();
    LyricResult r; r.source = "netease";
    const std::string sid = pv_lyric_song_id(rq.cands, "netease");
    if (sid.empty()) return r;
    const long long id = std::strtoll(sid.c_str(), nullptr, 10);
    if (id <= 0) return r;
    r = lyric_netease(id);
    err = r.err;
    return r;
}

// lrclib 歌词入口:直接用元数据四件套走 get → search 回退(本源无句柄概念)。
inline LyricResult lyric_lrclib_entry(const LyricRequest& rq, std::string& err) {
    err.clear();
    const LyricResult r = lyric_lrclib(rq.title, rq.artist, rq.durationSec, rq.album);
    err = r.err;
    return r;
}

// 静态注册表(唯一事实源):顺序 = 搜索优先级。
//   dizzylab → qq → netease → itunes   (搜索链按正向遍历)
//   lrclib → netease → qq              (歌词链按【逆序】+ canLyric 过滤)
//
// dizzylab 排第一的理由(2026-10-22,见本源头注):曲库是同人为主(555/653),
// 目录名=「专辑 @ 社团」,而 QQ/网易/iTunes 对同人社团发行基本无条目 —— 它是这类
// 曲目的正解。⚠️ 排第一【不改变采纳门槛】:enrich_adopt 仍要求
// confident ‖ LLM pick≥0 ‖ top≥0.80,宁可拒采也不写错(宁缺毋错,§1.6.A P1.5 验收)。
//
// lrclib 挪到表尾后歌词链逆序仍是 lrclib→netease→qq(dizzylab canLyric=false 自动跳过),
// 与 P1 的固定歌词顺序完全一致 —— tests/test_metaproviders.cpp 有该顺序断言。
inline const std::vector<ProviderDesc>& provider_registry() {
    static const std::vector<ProviderDesc> reg = {
        { "dizzylab", "dizzylab",   true,  false, search_dizzylab, nullptr             },
        { "qq",       "QQ音乐",      true,  true,  search_qq,       lyric_qq_entry      },
        { "netease",  "网易云",       true,  true,  search_netease,  lyric_netease_entry },
        { "itunes",   "iTunes",      true,  false, search_itunes,   nullptr             },
        { "lrclib",   "lrclib 歌词",  false, true,  nullptr,         lyric_lrclib_entry  },
    };
    return reg;
}

} // namespace rta::meta
