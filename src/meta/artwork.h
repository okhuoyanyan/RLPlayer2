// artwork.h — 封面 / 头像 / 艺人信息的**全信源统一调度**(2026-10-27)
//
// 用户原话(261026-05/06、261025-02)：
//   「我们的所有接入的数据源，应该是通用于封面、头像、歌词、艺术家等等所有内容收集，
//     不存在某个源只提供一种信息的情况。头像也不应该仅接入网易云一个信源，应该是全信源
//     接入。请你核查你当前信源的使用到底是怎么回事，谁告诉你直接入一个信源的。」
//
// 本文件回答的就是这一条：**谁能提供什么，由 provider.h 的能力矩阵声明；候选列表由矩阵
// 派生**，不再每类内容各写一段「先试 A 再试 B」的死代码，也不再有「头像只问网易云」。
//
// 三类内容的派生入口(全部只按 ArtCap 过滤 + 注册表顺序，没有第二份名单)：
//   art_cover_sources()   Cover 位，正序(注册表顺序=dizzylab→qq→netease→itunes→discogs)
//   art_artist_sources()  ArtistImage 位，正序(见下方「头像优先级」)
//   art_lyric_sources()   Lyrics 位，**逆序**(保持 P1 的 lrclib→netease→qq 优先级不变)
// 断言：tests/test_metaproviders.cpp 检查「每个声明了 ArtistImage 的源都有抓取实现」
// 与「每个抓取实现都在注册表里声明了该能力」—— 两边不一致会直接测试失败。
//
// ---------------------------------------------------------------------------
// 头像优先级为什么是 dizzylab 打头(2026-10-27 实测,真实库 245 位艺人)
// ---------------------------------------------------------------------------
//   * dizzylab 命中「用户」卡片时给的是**艺人自己的头像**(avatars/xxx.png!cover,
//     实测 Khrisma 68,713 B),而且一次 GET 同时带回简介与外链；
//   * 真实库是同人为主(Khrisma 62 首 / 炒饭 33 / MhT 27 / AnnyJuly 16),
//     这些人在网易云/QQ 基本查不到 —— 这正是「只接网易云」145/246 只剩首字母的根因；
//   * QQ 次之(musicu 歌手搜索覆盖大,实测蔡琴 17,197 B / TAMUSIC 49,625 B / 初音未来 23,951 B);
//   * 网易云第三(首个结果可能是粉丝号,必须精确名匹配后取 picUrl);
//   * Discogs 最后(匿名 25 req/min,只对前三源都落空的艺人补,内部单列节流)。
//
// ⚠️ 精确名匹配是**默认行为**(用户 2026-10-27 复核的硬要求)：宽松匹配会张冠李戴 ——
//    实测 dizzylab「炒饭」→「铁棍山药炒饭」、「MhT」→「MHTWSNBB666」、
//    「洛天依」→「超绝可爱洛天依」,QQ「Khrisma」→「Kharisma」。
//    需要宽松时显式 opts.allowFuzzy=true(默认关),并把命中的差异写进 reason。
//
// ---------------------------------------------------------------------------
// 缓存(沿用现有封面缓存那套「正缓存 + 负缓存」机制)
// ---------------------------------------------------------------------------
//   <dir>\<fnv16>.jpg    正缓存(命中头像本身)
//   <dir>\<fnv16>.src    溯源(source/url/bytes/ts) —— 报告与排查用
//   <dir>\<fnv16>.none   负缓存:文本,逐源写「src=<id> kind=miss|fail reason=...」+ ts
//   * 负缓存 7 天过期(与 rtaweb.cpp 既有 kArtistImgNegSec 同口径)；过期后自动重试；
//   * **瞬态失败(超时/断网/5xx)记 kind=fail,不参与「跳过该源」** —— 只记原因,
//     下次仍会重试,避免一次离线把所有艺人钉死成「没有头像」(261026-01 的老坑)；
//   * kind=miss(服务器明确答复没有)才在 TTL 内跳过该源 ⟹ 第 2、3 次请求零网络(实测)。
//
// 铁律 3/4 自查：网络函数不做假离线测试；离线测试覆盖(归一化/精确匹配/HTML 与 JSON
// 解析/负缓存与正缓存/派生顺序)，网络路径由真机实测(真实库 245 位艺人)给出数字。
#pragma once
#include "meta/provider.h"       // 能力矩阵 + Candidate + percent_enc_utf8 + pv_user_agent
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace rta::meta {

namespace fsx = std::filesystem;

// ===========================================================================
// 0) 传输钩子(离线测试唯一入口；生产走真实 http_get/http_post)
// ===========================================================================
struct ArtHttpRequest {
    std::string method;                  // "GET" / "POST"
    std::string url;
    std::string body;
    std::vector<std::string> headers;
    std::string contentType;             // POST 用
    int timeoutMs = 0;
};

using ArtHttpHook = std::function<rta::net::HttpResponse(const ArtHttpRequest&)>;

inline ArtHttpHook& art_http_hook() {
    static ArtHttpHook hook = nullptr;
    return hook;
}

inline void set_artwork_http_for_test(ArtHttpHook hook) { art_http_hook() = std::move(hook); }

inline rta::net::HttpResponse art_http(const ArtHttpRequest& q) {
    if (const auto& h = art_http_hook()) return h(q);
    if (q.method == "POST")
        return rta::net::http_post(q.url, q.body, q.headers,
                                   q.contentType.empty() ? "application/json" : q.contentType.c_str(),
                                   q.timeoutMs);
    return rta::net::http_get(q.url, q.headers, q.timeoutMs);
}

// ===========================================================================
// 1) 选项与结果
// ===========================================================================
struct ArtworkOpts {
    std::string cacheDir;                    // 空 = 不落盘(只取字节)
    bool useCache = true;
    long long negTtlSec = 7LL * 24 * 3600;   // 负缓存有效期(与 rtaweb.cpp 既有口径一致)
    int  totalBudgetMs = 12000;              // 单个艺人总预算(四源链最坏情况)
    int  perSourceMs   = 4000;               // 单源搜索/取值上限
    int  downloadMs    = 6000;               // 图片下载上限
    size_t minBytes    = 1024;               // 小于它当「没有头像」(既有 rtaweb 阈值)
    size_t maxBytes    = 12u * 1024u * 1024u;
    std::map<std::string, bool> srcOn;       // 源开关；未列出的源 = 开(与 EnrichOpts 同语义)
    bool allowFuzzy = false;                 // 默认精确匹配(见文件头 ⚠️)
    // 全名四源都落空时，退回「主艺人」(A & B / A feat. B 里的 A)再跑一遍同一条链。
    // 真实库实测：245 位里 106 位无头像，其中 43 位靠这条救回来(见 art_primary_artist)。
    bool splitPrimary = true;
    const char* onlySource = nullptr;        // 调试/实测：只跑一个源
    // 域名兜底(自动化测试用不可达地址验证「断网不崩、不写脏负缓存」)。
    // neteaseHost 与 rtaweb.cpp 既有环境变量 RLPLAYER_ARTISTIMG_HOST 同义。
    std::string neteaseHost, qqHost, dizzylabHost, discogsHost;

    bool enabled(const std::string& id) const {
        const auto it = srcOn.find(id);
        return it == srcOn.end() ? true : it->second;
    }
};

// 一个源的一次尝试(成功也留痕：报告要贴「URL + 字节数 + 源名」)
struct ArtSourceTry {
    std::string source;
    bool ok = false;
    bool transient = false;      // 瞬态失败 ⟹ 不写进「跳过该源」名单
    std::string url;
    std::string reason;          // 失败原因 / 成功备注(字节数)
    long long bytes = 0;
    int ms = 0;
};

struct ArtistImageResult {
    bool ok = false;
    bool fromCache = false;
    bool fromNegCache = false;
    std::string bytes;
    std::string source;          // 命中的源("dizzylab"/"qq"/...)
    std::string url;             // 命中的图片 URL
    std::vector<ArtSourceTry> tried;

    std::string summary() const {
        std::ostringstream o;
        o << (ok ? "ok" : "miss") << " src=" << (source.empty() ? "-" : source)
          << " bytes=" << bytes.size();
        if (!url.empty()) o << " url=" << url;
        if (fromCache) o << " cache=1";
        if (fromNegCache) o << " negcache=1";
        for (const ArtSourceTry& t : tried) {
            o << " | " << t.source << ":" << (t.ok ? "ok" : (t.transient ? "fail" : "miss"))
              << "(" << t.reason << "," << t.ms << "ms)";
        }
        return o.str();
    }
};

// ===========================================================================
// 2) 小工具(纯函数，离线可测)
// ===========================================================================
inline uint64_t art_fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= (uint64_t)c; h *= 1099511628211ull; }
    return h;
}

inline std::string art_fnv_hex(const std::string& s) {
    char b[32];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)art_fnv1a(s));
    return std::string(b);
}

// 拉丁重音折叠表(只覆盖 Latin-1 Supplement 里最常见的那些；返回 nullptr = 不折叠)。
// 注意：不做 ASCII 大小写之外的任何"智能"处理，避免把不同名字折叠到一起。
inline const char* art_fold_latin(unsigned long cp) {
    switch (cp) {
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: return "a";
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: return "a";
        case 0x00C7: case 0x00E7: return "c";
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return "e";
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return "e";
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return "i";
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return "i";
        case 0x00D1: case 0x00F1: return "n";
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8: return "o";
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8: return "o";
        case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return "u";
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return "u";
        case 0x00DD: case 0x00FD: case 0x00FF: return "y";
        case 0x00DF: return "ss";
        default: return nullptr;
    }
}

// 艺人名归一化(只为「是否同一个名字」服务)：
//   * 首尾与内部连续空白折叠成单个空格；
//   * ASCII 大写 → 小写；
//   * 全角 ASCII(!..~、U+3000)折成半角(日文艺人名里全角括号常见)；
//   * **不去标点** —— 「周杰伦.」(网易云粉丝号)归一化后仍 != 「周杰伦」，正是我们要的。
inline std::string art_norm_name(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    bool lastSpace = true;                    // 连首部空白一起吃掉
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        unsigned long cp = c;
        size_t n = 1;
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        if (c >= 0x80) {
            if (i + n > s.size()) break;
            cp = (n == 2) ? (unsigned long)(c & 0x1Fu)
               : (n == 3) ? (unsigned long)(c & 0x0Fu)
                          : (unsigned long)(c & 0x07u);
            for (size_t k = 1; k < n; ++k)
                cp = (cp << 6) | (unsigned long)((unsigned char)s[i + k] & 0x3Fu);
            i += n;
        } else {
            i += 1;
        }
        if (cp == 0x3000u) cp = ' ';                       // 全角空格
        if (cp >= 0xFF01u && cp <= 0xFF5Eu) cp -= 0xFEE0u; // 全角 ASCII
        // 拉丁重音折成无音标字母：实测 "Orquesta de Camara Promusica de Malaga"(标签) 与
        // QQ 的 "Orquesta de Cámara Promúsica de Málaga" 是同一个人，只差音标 ——
        // 不做这一步就会「宁可留空」。折叠只影响比较，不改显示名。
        if (const char* fold = art_fold_latin(cp)) {
            if (!lastSpace && !o.empty()) { /* 折叠发生在词中间，不补空格 */ }
            lastSpace = false;
            for (const char* q = fold; *q; ++q) o += *q;   // ß → "ss" 这类多字符折叠
            continue;
        }
        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') {
            if (!lastSpace && !o.empty()) o += ' ';
            lastSpace = true;
            continue;
        }
        lastSpace = false;
        if (cp < 0x80u) {
            char ch = (char)cp;
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            o += ch;
        } else {                                            // 非 ASCII 码点原样回写 UTF-8
            if (cp < 0x800u) {
                o += (char)(0xC0u | (cp >> 6));
                o += (char)(0x80u | (cp & 0x3Fu));
            } else if (cp < 0x10000u) {
                o += (char)(0xE0u | (cp >> 12));
                o += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                o += (char)(0x80u | (cp & 0x3Fu));
            } else {
                o += (char)(0xF0u | (cp >> 18));
                o += (char)(0x80u | ((cp >> 12) & 0x3Fu));
                o += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                o += (char)(0x80u | (cp & 0x3Fu));
            }
        }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

inline bool art_name_equal(const std::string& a, const std::string& b) {
    const std::string na = art_norm_name(a), nb = art_norm_name(b);
    return !na.empty() && na == nb;
}

// 去首尾空白(只认 <=0x20；全角空格由 art_norm_name 折半角，这里不做二次折)
inline std::string art_trim_name(const std::string& raw) {
    size_t b = 0, e = raw.size();
    while (b < e && (unsigned char)raw[b] <= 0x20) ++b;
    while (e > b && (unsigned char)raw[e - 1] <= 0x20) --e;
    return raw.substr(b, e - b);
}

// 码点个数(不用 std::string::size() —— CJK 一字三字节，字节差会把「超绝可爱洛天依」判成超长)
inline size_t art_utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0u) != 0x80u) ++n;
    return n;
}

// 宽松匹配(默认关闭)：候选名**包含**查询名，且码点长度差 ≤ 6。
// 只用于显式 opts.allowFuzzy=true 的调试场景 —— 实测它有 4/6 概率贴错人，不该默认开。
// 它**不做**编辑距离：QQ 的「Kharisma」不会被当成「Khrisma」(拼写不同 = 不是包含关系)。
inline bool art_name_fuzzy(const std::string& cand, const std::string& query) {
    const std::string nc = art_norm_name(cand), nq = art_norm_name(query);
    if (nc.empty() || nq.empty()) return false;
    if (nc == nq) return true;
    if (nc.size() <= nq.size() || nc.find(nq) == std::string::npos) return false;
    return art_utf8_len(nc) - art_utf8_len(nq) <= 6;
}

// 从「多艺人串」里取**主艺人**(第一段)。为什么需要它：真实库的 artist 标签大量写成
// "A, B feat. C" / "A & B" / "A、B"(实测 245 位里有 106 位查不到头像，绝大多数是这种串 ——
// 任何源都不会有「A, B feat. C」这个艺人)。取第一段后再走同一条精确匹配链，实测能救回 43 位。
//
// 规则保守(宁缺毋错)：
//   * 分隔符：feat./ft./vs./with、" & "、"、"、";"、","、"/"；
//   * 取**最早出现**的分隔符之前的片段，trim 后长度 ≥2；
//   * 头里括号不配平(如 "Peak( かりんと" 被截断) ⟹ 放弃；
//   * 只按 "/" 切时要求长度 ≥3 —— 这条专门保护 "AC/DC" 这类乐队名(切出 "AC" 就变成别人了)。
inline std::string art_primary_artist(const std::string& raw) {
    static const char* kSeps[] = { " feat. ", " feat ", " ft. ", " ft ", " vs. ", " vs ", " with ",
                                   " & ", "、", ";", ",", "/" };
    size_t best = std::string::npos;
    std::string sep;
    for (const char* s : kSeps) {
        const size_t p = raw.find(s);
        if (p == std::string::npos) continue;
        if (best == std::string::npos || p < best) { best = p; sep = s; }
    }
    if (best == std::string::npos) return std::string();
    std::string head = art_trim_name(raw.substr(0, best));
    // 只按 "/" 切时要求 ≥3 字(保护 "AC/DC");其余分隔符只要非空即可 ——
    // CJK 单字艺人("甲"、"光")是合法名字，不能因为短就丢掉。
    const size_t minLen = (sep == "/") ? 3 : 1;
    if (art_utf8_len(head) < minLen) return std::string();
    size_t open = 0, close = 0;
    for (size_t i = 0; i < head.size();) {
        const unsigned char c = (unsigned char)head[i];
        if (c == '(' || c == '[') { ++open; ++i; continue; }
        if (c == ')' || c == ']') { ++close; ++i; continue; }
        if (c == 0xEF && i + 2 < head.size() &&
            (unsigned char)head[i + 1] == 0xBC && (unsigned char)head[i + 2] == 0x88) { ++open; i += 3; continue; }
        if (c == 0xEF && i + 2 < head.size() &&
            (unsigned char)head[i + 1] == 0xBC && (unsigned char)head[i + 2] == 0x89) { ++close; i += 3; continue; }
        ++i;
    }
    if (open != close) return std::string();
    return head;
}

// 图片字节嗅探(不信任 Content-Type，只看魔数)：JPEG/PNG/WEBP/GIF/BMP
inline bool art_image_magic(const std::string& b) {
    if (b.size() < 4) return false;
    const unsigned char* p = (const unsigned char*)b.data();
    if (p[0] == 0xFF && p[1] == 0xD8) return true;                                   // JPEG
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return true;       // PNG
    if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return true;
    if (b.size() >= 6 && b.compare(0, 3, "GIF") == 0) return true;
    if (p[0] == 'B' && p[1] == 'M') return true;
    return false;
}

inline std::string art_oneline(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) o += (c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

// ===========================================================================
// 3) 艺人候选(各源解析后的统一形态) + 精确挑选
// ===========================================================================
struct ArtArtistCandidate {
    std::string source;
    std::string id;          // 源内 id(netease artist id / qq singerMID / dizzylab user id / discogs id)
    std::string name;
    std::string imageUrl;
    std::string pageUrl;
    std::string bio;
    bool fanAccount = false; // 网易云的粉丝号(有 accountId 字段) —— 精确匹配后仍优先排除
};

// 精确匹配挑选(核心防错位规则)：
//   ① 名字归一化后完全相等；若有多条，优先 fanAccount=false 的第一条；
//   ② allowFuzzy 时才退化为包含匹配；
//   ③ 都没有 → nullptr(宁可留空，不贴别人的脸)。
inline const ArtArtistCandidate* art_pick_artist(const std::vector<ArtArtistCandidate>& list,
                                                 const std::string& query, bool allowFuzzy) {
    const ArtArtistCandidate* exact = nullptr;
    for (const ArtArtistCandidate& c : list) {
        if (!art_name_equal(c.name, query)) continue;
        if (c.imageUrl.empty() && c.bio.empty()) continue;
        if (!c.fanAccount) return &c;
        if (!exact) exact = &c;
    }
    if (exact) return exact;
    if (!allowFuzzy) return nullptr;
    for (const ArtArtistCandidate& c : list) {
        if (c.fanAccount) continue;
        if (c.imageUrl.empty() && c.bio.empty()) continue;
        if (art_name_fuzzy(c.name, query)) return &c;
    }
    return nullptr;
}

// ---- 3.1 网易云 artist 搜索(type=100) ----
// 返回结构(2026-10-27 实测)：result.artists[]: {id,name,picUrl,accountId?,alias[]}
inline std::vector<ArtArtistCandidate> art_parse_netease_artists(const std::string& text) {
    std::vector<ArtArtistCandidate> out;
    const rta::net::Json j = rta::net::json_parse(text);
    const rta::net::Json& arr = j["result"]["artists"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& a = arr.at(i);
        ArtArtistCandidate c;
        c.source = "netease";
        c.id = a["id"].s();
        c.name = a["name"].s();
        c.imageUrl = a["picUrl"].s();
        if (c.id.empty() || c.name.empty()) continue;
        c.fanAccount = a.has("accountId") && a["accountId"].type != rta::net::Json::Null;
        c.pageUrl = "https://music.163.com/#/artist?id=" + c.id;
        out.push_back(std::move(c));
    }
    return out;
}

// ---- 3.2 QQ musicu 歌手搜索(search_type=1) ----
// 返回结构(2026-10-27 实测)：req.data.body.singer.list[]:
//   {singerMID, singerName, singerPic("http://y.gtimg.cn/music/photo_new/T001R150x150M000<mid>_2.jpg")}
inline std::vector<ArtArtistCandidate> art_parse_qq_singers(const std::string& text) {
    std::vector<ArtArtistCandidate> out;
    const rta::net::Json j = rta::net::json_parse(text);
    const rta::net::Json& arr = j["req"]["data"]["body"]["singer"]["list"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& s = arr.at(i);
        ArtArtistCandidate c;
        c.source = "qq";
        c.id = s["singerMID"].s();
        c.name = s["singerName"].s();
        c.imageUrl = s["singerPic"].s();
        if (c.id.empty() || c.name.empty()) continue;
        if (c.imageUrl.empty())
            c.imageUrl = "https://y.qq.com/music/photo_new/T001R300x300M000" + c.id + ".jpg";
        c.pageUrl = "https://y.qq.com/n/ryqq/singer/" + c.id;
        out.push_back(std::move(c));
    }
    return out;
}

// ---- 3.3 dizzylab 搜索页「用户」段 ----
// 实测结构(2026-10-27)：
//   <h4 class="text-warning">用户</h4>
//   <a href="/u/68242"> <div class="card">… <img src="https://cdn.dizzylab.net/media/avatars/X.png!cover" …>
//     <h1>Khrisma</h1> <p …>月華社所属。东方/编曲/音游/公共交通/F1。</p> …
inline std::string art_attr(const std::string& s, size_t pos, const char* attr, size_t endLimit) {
    const std::string key = std::string(attr) + "=\"";
    const size_t p = s.find(key, pos);
    if (p == std::string::npos || p >= endLimit) return std::string();
    const size_t b = p + key.size();
    const size_t e = s.find('"', b);
    if (e == std::string::npos || e > endLimit) return std::string();
    return s.substr(b, e - b);
}

inline std::string art_tag_text(const std::string& s, size_t pos, const char* tag, size_t endLimit) {
    const std::string open = std::string("<") + tag;
    size_t p = s.find(open, pos);
    if (p == std::string::npos || p >= endLimit) return std::string();
    p = s.find('>', p);
    if (p == std::string::npos || p >= endLimit) return std::string();
    ++p;
    const std::string close = std::string("</") + tag + ">";
    const size_t e = s.find(close, p);
    if (e == std::string::npos || e > endLimit) return std::string();
    return pv_html_unescape(s.substr(p, e - p));
}

inline std::vector<ArtArtistCandidate> art_parse_dizzylab_users(const std::string& html) {
    std::vector<ArtArtistCandidate> out;
    const std::string marker = "<h4 class=\"text-warning\">用户</h4>";
    const size_t sec = html.find(marker);
    if (sec == std::string::npos) return out;              // 没有「用户」段 = 该名字没有用户命中
    size_t secEnd = html.size();
    const size_t nextH4 = html.find("<h4 class=\"text-warning\">", sec + marker.size());
    if (nextH4 != std::string::npos) secEnd = nextH4;

    const std::string cardOpen = "<a href=\"/u/";
    size_t p = sec;
    while (p < secEnd) {
        const size_t a = html.find(cardOpen, p);
        if (a == std::string::npos || a >= secEnd) break;
        const size_t idBegin = a + cardOpen.size();
        const size_t idEnd = html.find('"', idBegin);
        if (idEnd == std::string::npos || idEnd > secEnd) break;
        const std::string uid = html.substr(idBegin, idEnd - idBegin);
        size_t cardEnd = html.find(cardOpen, idEnd);
        if (cardEnd == std::string::npos || cardEnd > secEnd) cardEnd = secEnd;

        ArtArtistCandidate c;
        c.source = "dizzylab";
        c.id = uid;
        c.pageUrl = "https://dizzylab.net/u/" + uid;
        const size_t img = html.find("<img", idEnd);
        if (img != std::string::npos && img < cardEnd) c.imageUrl = art_attr(html, img, "src", cardEnd);
        c.name = art_tag_text(html, idEnd, "h1", cardEnd);
        c.bio  = art_tag_text(html, idEnd, "p", cardEnd);
        if (!c.name.empty()) out.push_back(std::move(c));
        p = (cardEnd > a) ? cardEnd : (a + 1);
    }
    return out;
}

// ---- 3.4 Discogs 歌手搜索 + 详情 ----
// 搜索(2026-10-27 实测)：results[]: {id, type:"artist", title, uri}
inline std::vector<ArtArtistCandidate> art_parse_discogs_search(const std::string& text) {
    std::vector<ArtArtistCandidate> out;
    const rta::net::Json j = rta::net::json_parse(text);
    const rta::net::Json& arr = j["results"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& r = arr.at(i);
        if (r["type"].s() != "artist") continue;
        ArtArtistCandidate c;
        c.source = "discogs";
        c.id = r["id"].s();
        c.name = r["title"].s();
        if (c.id.empty() || c.name.empty()) continue;
        c.pageUrl = "https://www.discogs.com/artist/" + c.id;
        out.push_back(std::move(c));
    }
    return out;
}

// 详情(2026-10-27 实测)：images[]: {type:"primary"|"secondary", uri}；profile 为简介正文
inline ArtArtistCandidate art_parse_discogs_artist(const std::string& text) {
    ArtArtistCandidate c;
    c.source = "discogs";
    const rta::net::Json j = rta::net::json_parse(text);
    c.id = j["id"].s();
    c.name = j["name"].s();
    c.pageUrl = j["uri"].s();
    c.bio = j["profile"].s();
    const rta::net::Json& imgs = j["images"];
    std::string secondary;
    for (size_t i = 0; i < imgs.size(); ++i) {
        const rta::net::Json& im = imgs.at(i);
        const std::string uri = im["uri"].s();
        if (uri.empty()) continue;
        if (im["type"].s() == "primary") { c.imageUrl = uri; break; }
        if (secondary.empty()) secondary = uri;
    }
    if (c.imageUrl.empty()) c.imageUrl = secondary;
    return c;
}

// ---- 3.5 iTunes 歌手(无 artwork，只给外链/ID) ----
inline std::vector<ArtArtistCandidate> art_parse_itunes_artists(const std::string& text) {
    std::vector<ArtArtistCandidate> out;
    const rta::net::Json j = rta::net::json_parse(text);
    const rta::net::Json& arr = j["results"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& a = arr.at(i);
        ArtArtistCandidate c;
        c.source = "itunes";
        c.id = a["artistId"].s();
        c.name = a["artistName"].s();
        c.pageUrl = a["artistLinkUrl"].s();
        if (c.id.empty() || c.name.empty()) continue;
        out.push_back(std::move(c));   // imageUrl 留空：iTunes 搜索响应确实没有 artwork 字段
    }
    return out;
}

// ===========================================================================
// 4) 能力矩阵 → 候选源列表(数据驱动的唯一入口)
// ===========================================================================
inline std::vector<const ProviderDesc*> art_sources_with(ArtCap cap,
                                                        const std::map<std::string, bool>& srcOn,
                                                        bool reverse = false) {
    std::vector<const ProviderDesc*> out;
    const auto& reg = provider_registry();
    auto on = [&](const ProviderDesc& pd) {
        const auto it = srcOn.find(pd.id);
        return it == srcOn.end() ? true : it->second;
    };
    if (reverse) {
        for (auto it = reg.rbegin(); it != reg.rend(); ++it)
            if (pv_has_cap(it->caps, cap) && on(*it)) out.push_back(&*it);
    } else {
        for (const ProviderDesc& pd : reg)
            if (pv_has_cap(pd.caps, cap) && on(pd)) out.push_back(&pd);
    }
    return out;
}

inline std::vector<const ProviderDesc*> art_cover_sources(const std::map<std::string, bool>& srcOn) {
    return art_sources_with(ArtCap::Cover, srcOn, false);
}
inline std::vector<const ProviderDesc*> art_artist_sources(const std::map<std::string, bool>& srcOn) {
    return art_sources_with(ArtCap::ArtistImage, srcOn, false);
}
// 歌词链保持 P1 的逆序(lrclib → netease → qq)：注册表逆序 + Lyrics 过滤，顺序与改造前逐项相同。
inline std::vector<const ProviderDesc*> art_lyric_sources(const std::map<std::string, bool>& srcOn) {
    return art_sources_with(ArtCap::Lyrics, srcOn, true);
}

// 封面候选 URL 链(enrich 侧用)：按匹配分从高到低把所有候选的 coverUrl 去重排队。
// 这是「封面只问第一首」老缺陷的修法 —— 首选候选的 URL 404/超时后仍有机会用别的候选/别的源。
inline std::vector<std::string> art_cover_urls_by_score(const std::vector<Scored>& sortedDesc,
                                                       const std::vector<Candidate>& cands) {
    std::vector<std::string> urls;
    std::set<std::string> seen;
    for (const Scored& s : sortedDesc) {
        if (s.idx >= cands.size()) continue;
        const std::string& u = cands[s.idx].coverUrl;
        if (u.empty() || !seen.insert(u).second) continue;
        urls.push_back(u);
    }
    return urls;
}

// 逐 URL 下载封面：第一个「>minBytes 且是图片」的胜出；每个 URL 都留失败原因。
struct CoverFetchResult {
    bool ok = false;
    std::string bytes, url, err;
    std::vector<std::string> tried;   // "url -> reason"
};

inline CoverFetchResult art_fetch_cover_urls(const std::vector<std::string>& urls,
                                             const ArtworkOpts& opts) {
    CoverFetchResult r;
    if (urls.empty()) { r.err = "无候选封面 URL"; return r; }
    for (const std::string& u : urls) {
        ArtHttpRequest q;
        q.method = "GET";
        q.url = u;
        q.headers = { "User-Agent: Mozilla/5.0" };
        q.timeoutMs = opts.downloadMs;
        const rta::net::HttpResponse resp = art_http(q);
        if (resp.status != 200) {
            r.tried.push_back(u + " -> HTTP " + std::to_string(resp.status) +
                              (resp.error.empty() ? "" : (" " + resp.error)));
            continue;
        }
        std::string body(resp.body.begin(), resp.body.end());
        if (body.size() < opts.minBytes) {
            r.tried.push_back(u + " -> " + std::to_string(body.size()) + "B(太小)");
            continue;
        }
        if (body.size() > opts.maxBytes) {
            r.tried.push_back(u + " -> " + std::to_string(body.size()) + "B(超上限)");
            continue;
        }
        if (!art_image_magic(body)) {
            r.tried.push_back(u + " -> 非图片字节");
            continue;
        }
        r.ok = true;
        r.url = u;
        r.bytes = std::move(body);
        return r;
    }
    r.err = r.tried.empty() ? "全部候选失败" : r.tried.back();
    return r;
}

// ===========================================================================
// 5) 头像：磁盘缓存(正/负 + 溯源)
// ===========================================================================
struct ArtCachePaths {
    std::string jpg, src, none;
};

inline ArtCachePaths art_cache_paths(const std::string& dir, const std::string& name) {
    ArtCachePaths p;
    const std::string fn = art_fnv_hex(name);
    if (!dir.empty()) {
        p.jpg  = dir + "\\" + fn + ".jpg";
        p.src  = dir + "\\" + fn + ".src";
        p.none = dir + "\\" + fn + ".none";
    }
    return p;
}

inline bool art_read_file(const std::string& path, std::string& out, size_t cap) {
    if (path.empty()) return false;
    std::error_code ec;
    const auto sz = fsx::file_size(fsx::u8path(path), ec);
    if (ec || sz == 0 || sz > cap) return false;
    std::ifstream f(fsx::u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign((size_t)sz, '\0');
    f.read(&out[0], (std::streamsize)sz);
    return (size_t)f.gcount() == (size_t)sz;
}

inline bool art_write_file(const std::string& path, const std::string& bytes) {
    if (path.empty()) return false;
    std::error_code ec;
    const size_t slash = path.find_last_of('\\');
    if (slash != std::string::npos) fsx::create_directories(fsx::u8path(path.substr(0, slash)), ec);
    std::ofstream f(fsx::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty()) f.write(bytes.data(), (std::streamsize)bytes.size());
    return (bool)f;
}

// 负缓存内容：每行一条
//   ts=<unix>
//   src=<id> kind=miss|fail reason=<一行文本>
struct ArtNegEntry {
    std::string source;
    bool miss = false;      // true = 服务器明确答复没有(可跳过)；false = 瞬态失败(仍重试)
    std::string reason;
};
struct ArtNegCache {
    bool present = false;
    bool fresh = false;
    long long ts = 0;
    std::vector<ArtNegEntry> entries;
};

inline ArtNegCache art_neg_read(const std::string& path, long long ttlSec, long long nowSec) {
    ArtNegCache n;
    std::string text;
    if (!art_read_file(path, text, 64u * 1024u)) return n;
    n.present = true;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("ts=", 0) == 0) { n.ts = std::atoll(line.c_str() + 3); continue; }
        if (line.rfind("src=", 0) != 0) continue;
        ArtNegEntry e;
        const size_t sp = line.find(' ');
        e.source = line.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
        const size_t k = line.find("kind=");
        if (k != std::string::npos) e.miss = (line.compare(k + 5, 4, "miss") == 0);
        const size_t r = line.find("reason=");
        if (r != std::string::npos) e.reason = line.substr(r + 7);
        n.entries.push_back(std::move(e));
    }
    n.fresh = (nowSec - n.ts) <= ttlSec;
    return n;
}

inline std::string art_neg_render(long long nowSec, const std::vector<ArtSourceTry>& tried) {
    std::ostringstream o;
    o << "ts=" << nowSec << "\n";
    for (const ArtSourceTry& t : tried)
        o << "src=" << t.source << " kind=" << (t.transient ? "fail" : "miss")
          << " reason=" << art_oneline(t.reason) << "\n";
    return o.str();
}

// ===========================================================================
// 6) 各源的「取艺人头像」实现(每个返回 1 个 ArtSourceTry + 图片字节)
// ===========================================================================
struct ArtFetchOut {
    bool ok = false;
    bool transient = false;
    std::string bytes, url, reason;
};

inline std::string art_env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

inline std::string art_trim_host(std::string host) {
    while (!host.empty() && (host.back() == '/' || host.back() == ' ')) host.pop_back();
    return host;
}

// 下载并校验图片(各源共用)
inline ArtFetchOut art_download_image(const std::string& url, const std::vector<std::string>& headers,
                                      const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    if (url.empty()) { out.reason = "无图片 URL"; return out; }
    ArtHttpRequest q;
    q.method = "GET";
    q.url = url;
    q.headers = headers;
    q.timeoutMs = std::max(500, std::min(opts.downloadMs, budgetMs));
    const rta::net::HttpResponse resp = art_http(q);
    if (resp.status != 200) {
        out.transient = (resp.status == 0 || resp.status >= 500);
        out.reason = "下载 HTTP " + std::to_string(resp.status) +
                     (resp.error.empty() ? "" : (" " + resp.error));
        return out;
    }
    std::string body(resp.body.begin(), resp.body.end());
    if (body.size() < opts.minBytes) {
        out.reason = "图片太小 " + std::to_string(body.size()) + "B";
        return out;
    }
    if (body.size() > opts.maxBytes) {
        out.reason = "图片超上限 " + std::to_string(body.size()) + "B";
        return out;
    }
    if (!art_image_magic(body)) { out.reason = "非图片字节(魔数不符)"; return out; }
    out.ok = true;
    out.url = url;
    out.bytes = std::move(body);
    return out;
}

// ---- 6.1 网易云：POST /api/search/get type=100 → 精确名匹配 → picUrl ----
inline ArtFetchOut art_src_netease_artist(const std::string& name, const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    const std::string host = art_trim_host(opts.neteaseHost.empty()
        ? art_env("RLPLAYER_ARTISTIMG_HOST") : opts.neteaseHost);
    const std::string base = host.empty() ? "https://music.163.com" : host;
    ArtHttpRequest q;
    q.method = "POST";
    q.url = base + "/api/search/get";
    q.body = "s=" + percent_enc_utf8(name) + "&type=100&limit=5&offset=0";
    q.headers = { "User-Agent: Mozilla/5.0", "Referer: " + base + "/" };
    q.contentType = "application/x-www-form-urlencoded";
    q.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse resp = art_http(q);
    if (resp.status != 200) {
        out.transient = (resp.status == 0 || resp.status >= 500);
        out.reason = "搜索 HTTP " + std::to_string(resp.status) +
                     (resp.error.empty() ? "" : (" " + resp.error));
        return out;
    }
    const std::vector<ArtArtistCandidate> list =
        art_parse_netease_artists(std::string(resp.body.begin(), resp.body.end()));
    if (list.empty()) { out.reason = "搜索 0 条艺人"; return out; }
    const ArtArtistCandidate* hit = art_pick_artist(list, name, opts.allowFuzzy);
    if (!hit) {
        out.reason = "无同名艺人(候选 " + std::to_string(list.size()) + " 条,首个=" +
                     art_oneline(list.front().name) + ")";
        return out;
    }
    ArtFetchOut dl = art_download_image(hit->imageUrl,
                                        { "User-Agent: Mozilla/5.0", "Referer: " + base + "/" },
                                        opts, budgetMs);
    if (!dl.ok) { dl.reason = "候选「" + art_oneline(hit->name) + "」" + dl.reason; return dl; }
    return dl;
}

// ---- 6.2 QQ：POST musicu.fcg search_type=1 → singerPic ----
inline std::string art_qq_search_body(const std::string& name) {
    std::string esc;
    esc.reserve(name.size() + 8);
    for (char c : name) { if (c == '"' || c == '\\') esc += '\\'; esc += c; }
    std::ostringstream o;
    o << "{\"comm\":{\"ct\":\"19\",\"cv\":\"1859\",\"uin\":\"0\"},\"req\":{"
         "\"method\":\"DoSearchForQQMusicDesktop\",\"module\":\"music.search.SearchCgiService\","
         "\"param\":{\"num_per_page\":8,\"page_num\":1,\"query\":\"" << esc << "\","
         "\"search_type\":1}}}";
    return o.str();
}

inline ArtFetchOut art_src_qq_artist(const std::string& name, const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    const std::string base = art_trim_host(opts.qqHost.empty()
        ? art_env("RLPLAYER_ARTISTIMG_HOST_QQ") : opts.qqHost);
    const std::string url = (base.empty() ? "https://u.y.qq.com" : base) + "/cgi-bin/musicu.fcg";
    ArtHttpRequest q;
    q.method = "POST";
    q.url = url;
    q.body = art_qq_search_body(name);
    q.headers = { "User-Agent: Mozilla/5.0", "Referer: https://y.qq.com/" };
    q.contentType = "application/json";
    q.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse resp = art_http(q);
    if (resp.status != 200) {
        out.transient = (resp.status == 0 || resp.status >= 500);
        out.reason = "搜索 HTTP " + std::to_string(resp.status) +
                     (resp.error.empty() ? "" : (" " + resp.error));
        return out;
    }
    const std::vector<ArtArtistCandidate> list =
        art_parse_qq_singers(std::string(resp.body.begin(), resp.body.end()));
    if (list.empty()) { out.reason = "搜索 0 条歌手"; return out; }
    const ArtArtistCandidate* hit = art_pick_artist(list, name, opts.allowFuzzy);
    if (!hit) { out.reason = "无同名歌手(首个=" + art_oneline(list.front().name) + ")"; return out; }
    std::string img = hit->imageUrl;                  // singerPic 是 T001R150x150…
    const size_t p = img.find("150x150");             // 换成 300x300 同 id 取更大图(实测 200)
    if (p != std::string::npos) img.replace(p, 7, "300x300");
    ArtFetchOut dl = art_download_image(img, { "User-Agent: Mozilla/5.0", "Referer: https://y.qq.com/" },
                                        opts, budgetMs);
    if (!dl.ok) { dl.reason = "候选「" + art_oneline(hit->name) + "」" + dl.reason; return dl; }
    return dl;
}

// ---- 6.3 dizzylab：GET /search/?s=<艺人> → 「用户」段 → avatars/…!cover ----
// 走 provider.h 的 dizzylab 通道(1 req/s 限频 + 连续失败熔断)，但请求本身经 art_http，
// 这样离线测试只需要注入一个钩子。
inline ArtFetchOut art_src_dizzylab_artist(const std::string& name, const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    std::string gerr;
    if (!dl_gate_acquire(gerr)) { out.reason = gerr; out.transient = true; return out; }
    const std::string host = art_trim_host(opts.dizzylabHost.empty()
        ? art_env("RLPLAYER_ARTISTIMG_HOST_DZ") : opts.dizzylabHost);
    ArtHttpRequest q;
    q.method = "GET";
    q.url = (host.empty() ? "https://dizzylab.net" : host) + "/search/?s=" + percent_enc_utf8(name);
    q.headers = { std::string("User-Agent: ") + pv_user_agent() };
    q.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse resp = art_http(q);
    if (resp.status == 0) {
        dl_gate_fail();
        out.transient = true;
        out.reason = "搜索请求失败: " + resp.error;
        return out;
    }
    if (resp.status < 200 || resp.status >= 300) {
        dl_gate_fail();
        out.reason = "搜索 HTTP " + std::to_string(resp.status);
        return out;
    }
    dl_gate_success();
    const std::vector<ArtArtistCandidate> list =
        art_parse_dizzylab_users(std::string(resp.body.begin(), resp.body.end()));
    if (list.empty()) { out.reason = "搜索页无「用户」命中"; return out; }
    const ArtArtistCandidate* hit = art_pick_artist(list, name, opts.allowFuzzy);
    if (!hit) { out.reason = "无同名用户(首个=" + art_oneline(list.front().name) + ")"; return out; }
    ArtFetchOut dl = art_download_image(hit->imageUrl,
                                        { std::string("User-Agent: ") + pv_user_agent() },
                                        opts, budgetMs);
    if (!dl.ok) { dl.reason = "候选「" + art_oneline(hit->name) + "」" + dl.reason; return dl; }
    return dl;
}

// ---- 6.4 Discogs：/database/search?type=artist → /artists/{id} images[] ----
// 匿名限频约 25 req/min ⟹ 进程级最小间隔 2500ms（一次搜索+一次详情 = 2 跳）。
// 429(匿名限频)后的进程级冷却：命中 429 就把 Discogs 整体停 300 s，期间该源直接返回
// 「瞬态失败(冷却中)」—— 既不再白等 2.5 s 节流，也不会把「被限频」写成「这个人没有头像」。
inline std::atomic<long long>& art_discogs_cool_until() {
    static std::atomic<long long> t{0};
    return t;
}
inline std::mutex& art_discogs_mtx() { static std::mutex m; return m; }
inline std::chrono::steady_clock::time_point& art_discogs_last() {
    static std::chrono::steady_clock::time_point t{};
    return t;
}
inline void art_discogs_throttle() {
    if (art_http_hook()) return;                  // 离线测试不真睡
    std::lock_guard<std::mutex> lk(art_discogs_mtx());
    auto& last = art_discogs_last();
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0) {
        const auto wait = std::chrono::milliseconds(2500) - (now - last);
        if (wait.count() > 0) std::this_thread::sleep_for(wait);
    }
    last = now;
}

// Discogs 搜索 + 详情两步的公共实现(头像与简介都用它，避免两套节流/解析)。
struct ArtDiscogsLookup {
    bool ok = false;
    bool transient = false;
    std::string reason;
    ArtArtistCandidate detail;
};

inline ArtDiscogsLookup art_discogs_lookup(const std::string& name, const ArtworkOpts& opts,
                                           int budgetMs) {
    ArtDiscogsLookup out;
    const long long nowSec = (long long)std::time(nullptr);
    const long long cool = art_discogs_cool_until().load();
    if (nowSec < cool) {
        out.transient = true;
        out.reason = "429 冷却中(剩 " + std::to_string(cool - nowSec) + "s)";
        return out;
    }
    const std::string base = art_trim_host(opts.discogsHost.empty()
        ? art_env("RLPLAYER_ARTISTIMG_HOST_DC") : opts.discogsHost);
    const std::string root = base.empty() ? "https://api.discogs.com" : base;
    const std::vector<std::string> hdr = { std::string("User-Agent: ") + pv_user_agent() };

    art_discogs_throttle();
    ArtHttpRequest q;
    q.method = "GET";
    q.url = root + "/database/search?q=" + percent_enc_utf8(name) + "&type=artist&per_page=5";
    q.headers = hdr;
    q.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse sr = art_http(q);
    if (sr.status == 429) {
        // 限频 = 瞬态：绝不当成「没有此人」写进 7 天负缓存
        art_discogs_cool_until().store(nowSec + 300);
        out.transient = true;
        out.reason = "搜索 HTTP 429(匿名限频,冷却 300s)";
        return out;
    }
    if (sr.status != 200) {
        out.transient = (sr.status == 0 || sr.status >= 500);
        out.reason = "搜索 HTTP " + std::to_string(sr.status);
        return out;
    }
    const std::vector<ArtArtistCandidate> list =
        art_parse_discogs_search(std::string(sr.body.begin(), sr.body.end()));
    if (list.empty()) { out.reason = "搜索 0 条艺人"; return out; }
    const ArtArtistCandidate* hit = art_pick_artist(list, name, opts.allowFuzzy);
    if (!hit) { out.reason = "无同名艺人(首个=" + art_oneline(list.front().name) + ")"; return out; }

    art_discogs_throttle();
    ArtHttpRequest dq;
    dq.method = "GET";
    dq.url = root + "/artists/" + hit->id;
    dq.headers = hdr;
    dq.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse dr = art_http(dq);
    if (dr.status != 200) {
        out.transient = (dr.status == 0 || dr.status >= 500);
        out.reason = "详情 HTTP " + std::to_string(dr.status);
        return out;
    }
    out.detail = art_parse_discogs_artist(std::string(dr.body.begin(), dr.body.end()));
    out.ok = true;
    return out;
}

inline ArtFetchOut art_src_discogs_artist(const std::string& name, const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    const ArtDiscogsLookup lk = art_discogs_lookup(name, opts, budgetMs);
    if (!lk.ok) { out.reason = lk.reason; out.transient = lk.transient; return out; }
    if (lk.detail.imageUrl.empty()) { out.reason = "艺人 " + lk.detail.id + " 无 images[]"; return out; }
    ArtFetchOut dl = art_download_image(lk.detail.imageUrl,
                                        { std::string("User-Agent: ") + pv_user_agent() },
                                        opts, budgetMs);
    if (!dl.ok) { dl.reason = "艺人 " + lk.detail.id + " " + dl.reason; return dl; }
    return dl;
}

// ---- 6.5 抓取实现表(与能力矩阵的交集由 art_artist_image_plan() 求) ----
struct ArtArtistSource {
    const char* id;
    ArtFetchOut (*fetch)(const std::string&, const ArtworkOpts&, int);
};

inline const std::vector<ArtArtistSource>& art_artist_fetchers() {
    static const std::vector<ArtArtistSource> tbl = {
        { "dizzylab", art_src_dizzylab_artist },
        { "qq",       art_src_qq_artist       },
        { "netease",  art_src_netease_artist  },
        { "discogs",  art_src_discogs_artist  },
    };
    return tbl;
}

inline const ArtArtistSource* art_fetcher_of(const std::string& id) {
    for (const ArtArtistSource& s : art_artist_fetchers())
        if (id == s.id) return &s;
    return nullptr;
}

// 头像候选源 = 能力矩阵(ArtistImage 位, 注册表顺序) ∩ 抓取实现表（并遵守源开关）
// ⟹ 新增一个源只要在注册表声明 ArtistImage + 在 art_artist_fetchers() 注册实现，
//    头像链自动包含它，不用改任何循环(用户要求「以后新增一个源只改注册表一处」)。
inline std::vector<const ArtArtistSource*> art_artist_image_plan(const ArtworkOpts& opts) {
    std::vector<const ArtArtistSource*> out;
    for (const ProviderDesc* pd : art_artist_sources(opts.srcOn)) {
        if (opts.onlySource && std::string(opts.onlySource) != pd->id) continue;
        if (const ArtArtistSource* f = art_fetcher_of(pd->id)) out.push_back(f);
    }
    return out;
}

// ===========================================================================
// 7) 头像总入口：缓存 → 逐源(精确匹配) → 落盘/负缓存
// ===========================================================================
inline ArtistImageResult artwork_fetch_artist_image(const std::string& rawName,
                                                    const ArtworkOpts& opts) {
    ArtistImageResult res;
    const std::string name = art_trim_name(rawName);
    if (name.empty()) {
        ArtSourceTry t;
        t.source = "-";
        t.reason = "名字为空";
        res.tried.push_back(t);
        return res;
    }

    const long long nowSec = (long long)std::time(nullptr);
    const ArtCachePaths paths = art_cache_paths(opts.cacheDir, name);

    // ① 正缓存
    if (opts.useCache && !opts.cacheDir.empty()) {
        std::string bytes;
        if (art_read_file(paths.jpg, bytes, opts.maxBytes) && art_image_magic(bytes)) {
            res.ok = res.fromCache = true;
            res.bytes = std::move(bytes);
            std::string meta;
            if (art_read_file(paths.src, meta, 16u * 1024u)) {
                std::istringstream in(meta);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.rfind("source=", 0) == 0) res.source = line.substr(7);
                    else if (line.rfind("url=", 0) == 0) res.url = line.substr(4);
                }
            }
            return res;
        }
    }

    // ② 负缓存(逐源跳过 kind=miss 的源；kind=fail 的仍重试)
    ArtNegCache neg;
    std::set<std::string> skip;
    if (opts.useCache && !opts.cacheDir.empty()) {
        neg = art_neg_read(paths.none, opts.negTtlSec, nowSec);
        if (neg.present && neg.fresh)
            for (const ArtNegEntry& e : neg.entries)
                if (e.miss) skip.insert(e.source);
    }

    // ③ 逐源尝试(顺序 = 能力矩阵派生)。同一个循环体跑两轮：
    //    第 1 轮 = 标签里的完整艺人名；第 2 轮 = 主艺人(仅当 opts.splitPrimary 且第 1 轮全落空)。
    const std::vector<const ArtArtistSource*> plan = art_artist_image_plan(opts);
    int remaining = opts.totalBudgetMs;
    bool anyTransient = false;
    bool didNetwork = false;

    // 跑完一整条链；命中就落盘并返回 true。primary 非空表示「主艺人回退轮」。
    auto runChain = [&](const std::string& who, const std::string& primary) -> bool {
        for (const ArtArtistSource* src : plan) {
            if (primary.empty() && skip.count(src->id)) {
                ArtSourceTry t;
                t.source = src->id;
                t.reason = "负缓存:该源上次明确无此人(7 天内不重问)";
                res.tried.push_back(t);
                continue;
            }
            if (remaining <= 0) {
                ArtSourceTry t;
                t.source = src->id;
                t.reason = "总预算耗尽,未尝试";
                t.transient = true;
                res.tried.push_back(t);
                anyTransient = true;
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const ArtFetchOut fo = src->fetch(who, opts, remaining);
            didNetwork = true;
            const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
            remaining -= ms;
            ArtSourceTry t;
            t.source = src->id;
            t.ok = fo.ok;
            t.transient = fo.transient;
            t.url = fo.url;
            t.reason = (primary.empty() ? std::string() : ("主艺人「" + primary + "」")) + fo.reason;
            t.bytes = (long long)fo.bytes.size();
            t.ms = ms;
            if (fo.ok) {
                t.reason = std::to_string(fo.bytes.size()) + "B" +
                           (primary.empty() ? std::string() : ("(主艺人「" + primary + "」)"));
                res.tried.push_back(t);
                res.ok = true;
                res.bytes = fo.bytes;
                res.source = src->id + (primary.empty() ? std::string() : (" <-主艺人 " + primary));
                res.url = fo.url;
                // ④ 落盘正缓存 + 溯源，并清掉旧负缓存
                if (opts.useCache && !opts.cacheDir.empty()) {
                    art_write_file(paths.jpg, res.bytes);
                    std::ostringstream meta;
                    meta << "source=" << src->id << "\nurl=" << fo.url
                         << "\nbytes=" << res.bytes.size() << "\nts=" << nowSec
                         << "\nname=" << art_oneline(name) << "\n";
                    if (!primary.empty()) meta << "primary=" << art_oneline(primary) << "\n";
                    art_write_file(paths.src, meta.str());
                    std::error_code ec;
                    fsx::remove(fsx::u8path(paths.none), ec);
                }
                return true;
            }
            if (fo.transient) anyTransient = true;
            res.tried.push_back(t);
        }
        return false;
    };

    if (runChain(name, std::string())) return res;

    // ③b 主艺人回退(A & B / A feat. B → A)。用户库大量 artist 标签是多艺人串，
    //     这类串在任何源都不存在，但主艺人通常有头像；取到的是「这首歌的主艺人」的脸，
    //     与「张冠李戴」不同(不是别人)，因此默认开、且 .src 里记 primary= 便于核对。
    if (opts.splitPrimary && remaining > 0) {
        const std::string prim = art_primary_artist(name);
        if (!prim.empty() && art_norm_name(prim) != art_norm_name(name)) {
            if (runChain(prim, prim)) return res;
        }
    }

    // ⑤ 全落空：写负缓存(逐源原因)。
    //    规则：只有**至少一个源明确答复「没有此人」**时才写 .none —— 若全是瞬态失败
    //    (断网/超时)，写 .none 会把「这次没连上」污染成「这个人没有头像」，
    //    与 rtaweb.cpp 既有纪律「断网不写脏缓存」一致(261026-01 踩过)。
    //    混合情况仍写：明确没有的源记 kind=miss(下次跳过)，超时的记 kind=fail(下次重试)。
    res.fromNegCache = (!didNetwork && neg.present && neg.fresh);
    bool anyAuthoritativeMiss = false;
    for (const ArtSourceTry& t : res.tried)
        if (!t.transient && t.source != "-" && t.reason.rfind("负缓存", 0) != 0)
            anyAuthoritativeMiss = true;
    if (opts.useCache && !opts.cacheDir.empty() && anyAuthoritativeMiss) {
        std::vector<ArtSourceTry> toWrite;
        for (const ArtSourceTry& t : res.tried) {
            if (t.source == "-") continue;
            if (t.reason.rfind("负缓存", 0) == 0) continue;   // 上次的结论不用重写
            toWrite.push_back(t);
        }
        if (!toWrite.empty()) art_write_file(paths.none, art_neg_render(nowSec, toWrite));
    }
    (void)anyTransient;
    return res;
}

// ===========================================================================
// 8) 艺人简介/外链（能力矩阵 ArtistBio/ArtistLink 派生；dizzylab 与 Discogs 提供）
// ===========================================================================
struct ArtistProfileResult {
    bool ok = false;
    std::string name, bio, link, source;
    std::vector<ArtSourceTry> tried;
};

inline ArtistProfileResult artwork_fetch_artist_profile(const std::string& rawName,
                                                        const ArtworkOpts& opts) {
    ArtistProfileResult res;
    const std::string name = art_trim_name(rawName);
    if (name.empty()) return res;
    int remaining = opts.totalBudgetMs;
    for (const ProviderDesc* pd : art_sources_with(ArtCap::ArtistBio, opts.srcOn)) {
        if (opts.onlySource && std::string(opts.onlySource) != pd->id) continue;
        if (remaining <= 0) break;
        const auto t0 = std::chrono::steady_clock::now();
        ArtSourceTry t;
        t.source = pd->id;
        if (std::string(pd->id) == "dizzylab") {
            std::string gerr;
            if (!dl_gate_acquire(gerr)) {
                t.reason = gerr;
                t.transient = true;
            } else {
                const std::string host = art_trim_host(opts.dizzylabHost.empty()
                    ? art_env("RLPLAYER_ARTISTIMG_HOST_DZ") : opts.dizzylabHost);
                ArtHttpRequest q;
                q.method = "GET";
                q.url = (host.empty() ? "https://dizzylab.net" : host) +
                        "/search/?s=" + percent_enc_utf8(name);
                q.headers = { std::string("User-Agent: ") + pv_user_agent() };
                q.timeoutMs = std::max(500, std::min(opts.perSourceMs, remaining));
                const rta::net::HttpResponse resp = art_http(q);
                const std::vector<ArtArtistCandidate> list =
                    art_parse_dizzylab_users(std::string(resp.body.begin(), resp.body.end()));
                const ArtArtistCandidate* hit = art_pick_artist(list, name, opts.allowFuzzy);
                if (hit && !hit->bio.empty()) {
                    res.ok = true;
                    res.name = hit->name;
                    res.bio = hit->bio;
                    res.link = hit->pageUrl;
                    res.source = pd->id;
                    t.ok = true;
                    t.url = hit->pageUrl;
                    t.reason = std::to_string(hit->bio.size()) + "B 简介";
                } else {
                    t.reason = hit ? "命中但无简介" : "无同名用户";
                }
            }
        } else {   // discogs
            const ArtDiscogsLookup lk = art_discogs_lookup(name, opts, remaining);
            if (lk.ok && !lk.detail.bio.empty()) {
                res.ok = true;
                res.name = lk.detail.name;
                res.bio = lk.detail.bio;
                res.link = lk.detail.pageUrl;
                res.source = pd->id;
                t.ok = true;
                t.url = lk.detail.pageUrl;
                t.reason = std::to_string(lk.detail.bio.size()) + "B 简介";
            } else {
                t.reason = lk.ok ? "命中但无 profile" : lk.reason;
                t.transient = lk.transient;
            }
        }
        t.ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
        remaining -= t.ms;
        res.tried.push_back(t);
        if (res.ok) return res;
    }
    return res;
}

} // namespace rta::meta
