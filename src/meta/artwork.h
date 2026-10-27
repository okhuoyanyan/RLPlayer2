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


// ===========================================================================
// 9) 网页取图（2026-10-27，用户 261026-04 #7 / #9 的根因修复）
// ===========================================================================
// 上一轮（build-v13，15:14）实测：API 兜底"机制通、命中 0/6" —— 日志里确实抓了候选页
// （<code>APIART/COV og page=... htmlLen=...</code>），但几乎全是 <code>ogImage=[]</code>：
//   ① dizzylab 的 /d/ 页是 Vue SPA，**没有** og:image（htmlLen=10922 全是框架）；
//   ② anilist.co/character 同样是 SPA（htmlLen=4907）；
//   ③ 模型抓回来的 web_fetch 是**剥掉图片的 Markdown**（llm.h 的 ds_fetch 明说这一点），
//      指望模型"看图给直链"结构上不可能；
//   ④ 只有 anihk.com 这种"服务端渲染 + og:image"的页面命中（Mugai无害P，28694 B）。
// 结论：**图不在 og:image 一个地方**。本段提供三样东西，全部是纯函数（可离线单测）：
//   · art_page_image_candidates() 多策略抽取（og / twitter / JSON-LD / image_src / itemprop /
//     JS 变量 / <img> 启发式），每条候选带 kind 与 score，按分排序；
//   · art_url_public_ok() 安全策略：**不再用域名白名单**（上一轮 url_reject 过严的根因之一），
//     改成"公网可访问性"判据 —— 非 http(s)、userinfo、非 80/443、IP 字面量、回环/内网一律拒；
//   · art_image_dims() 尺寸校验（PNG/JPEG/GIF/WebP）：1×1 占位图、追踪像素必须被拒。
// 再加一档**确定性源**（art_ext_artist_image）：bangumi / AniList / Apple Music。
// 为什么放这里而不是能力矩阵（provider.h）：本轮 provider.h 归另一个代理，且这三家是
// "网页/半结构化 API"而不是"音频平台"，走 API 兜底阶段更符合用户"satisfied by API"的意图。

// ---- 9.1 URL 分解 / 拼装 / 安全策略（纯函数） ----
struct ArtUrlParts {
    bool valid = false;
    bool https = false;
    std::string scheme, host;      // host 已小写、已去端口
    int port = 0;
    std::string full;              // 原串（去首尾空白后）
};

inline std::string art_ascii_lower(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        if (static_cast<unsigned char>(c) < 0x80 && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

inline ArtUrlParts art_url_parts(const std::string& raw) {
    ArtUrlParts u;
    size_t b = 0;
    size_t e = raw.size();
    while (b < e && static_cast<unsigned char>(raw[b]) <= 0x20) ++b;
    while (e > b && static_cast<unsigned char>(raw[e - 1]) <= 0x20) --e;
    if (e <= b) return u;
    const std::string s = raw.substr(b, e - b);
    const size_t p = s.find("://");
    if (p == std::string::npos) return u;
    std::string scheme = art_ascii_lower(s.substr(0, p));
    if (scheme != "http" && scheme != "https") return u;
    size_t hs = p + 3;
    size_t he = s.size();
    for (size_t i = hs; i < s.size(); ++i)
        if (s[i] == '/' || s[i] == '?' || s[i] == '#') { he = i; break; }
    const std::string auth = s.substr(hs, he - hs);
    if (auth.empty()) return u;
    if (auth.find('@') != std::string::npos) return u;      // userinfo ⟹ 不认（SSRF 常用手法）
    std::string host = auth;
    int port = (scheme == "https") ? 443 : 80;
    const size_t colon = auth.rfind(':');
    if (colon != std::string::npos) {
        host = auth.substr(0, colon);
        const std::string ps = auth.substr(colon + 1);
        if (ps.empty() || ps.size() > 5) return u;
        for (char c : ps)
            if (c < '0' || c > '9') return u;
        port = std::atoi(ps.c_str());
    }
    host = art_ascii_lower(host);
    if (host.empty() || host.front() == '[' || host.front() == '.' || host.back() == '.' ||
        host.find("..") != std::string::npos) return u;
    for (unsigned char c : host)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return u;
    u.valid = true;
    u.https = (scheme == "https");
    u.scheme = scheme;
    u.host = host;
    u.port = port;
    u.full = s;
    return u;
}

// 公网可访问性：只拒"不该从外部访问的位置"，**不按域名白名单**（白名单是上一轮 0 命中的根因之一）。
inline bool art_host_public(const std::string& host, std::string* why) {
    if (host.empty()) { if (why) *why = "empty_host"; return false; }
    if (host == "localhost" || host == "::1" || host.rfind("localhost.", 0) == 0) {
        if (why) *why = "loopback"; return false;
    }
    bool digitDot = true;
    for (unsigned char c : host)
        if (!((c >= '0' && c <= '9') || c == '.')) { digitDot = false; break; }
    if (digitDot) { if (why) *why = "ip_literal"; return false; }     // 补图不需要直连 IP
    if (host.find(':') != std::string::npos) { if (why) *why = "ip_literal"; return false; }
    if (host.rfind("127.", 0) == 0 || host.rfind("10.", 0) == 0 || host.rfind("192.168.", 0) == 0 ||
        host.rfind("169.254.", 0) == 0 || host.rfind("0.", 0) == 0) {
        if (why) *why = "private_ip"; return false;
    }
    if (host.rfind("172.", 0) == 0) {
        const size_t d = host.find('.', 4);
        if (d != std::string::npos) {
            const int o2 = std::atoi(host.substr(4, d - 4).c_str());
            if (o2 >= 16 && o2 <= 31) { if (why) *why = "private_ip"; return false; }
        }
    }
    if (host.rfind("100.64.", 0) == 0 || host.rfind("198.18.", 0) == 0) {
        if (why) *why = "private_ip"; return false;
    }
    return true;
}

// 可下载吗：http(s) + 公网主机 + 端口 80/443 + 长度合理。
// **故意不检查扩展名**：网易云 / dizzylab / QQ 的图床大量是 "?param=300y300"、"!cover" 这种形态。
inline bool art_url_public_ok(const std::string& raw, std::string* why) {
    if (raw.size() > 2048) { if (why) *why = "too_long"; return false; }
    const ArtUrlParts u = art_url_parts(raw);
    if (!u.valid) { if (why) *why = "bad_url"; return false; }
    if (u.port != 80 && u.port != 443) { if (why) *why = "odd_port"; return false; }
    if (!art_host_public(u.host, why)) return false;
    if (why) why->clear();
    return true;
}

inline std::string art_html_unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            if (i + 4 <= s.size() && s.compare(i, 4, "&amp") == 0) { o += '&'; i += 4; continue; }
            if (i + 5 <= s.size() && s.compare(i, 5, "&quot") == 0) { o += '"'; i += 5; continue; }
            if (i + 5 <= s.size() && s.compare(i, 5, "&#039") == 0) { o += '\''; i += 5; continue; }
            if (i + 4 <= s.size() && s.compare(i, 4, "&#47") == 0) { o += '/'; i += 4; continue; }
            if (i + 6 <= s.size() && s.compare(i, 6, "&nbsp;") == 0) { o += ' '; i += 6; continue; }
        }
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '/') { o += '/'; i += 2; continue; }   // JSON 里的 "\/"
        o += s[i++];
    }
    return o;
}

// 相对 → 绝对（head 里大量 "/path"、"//host/path"）。
inline std::string art_url_abs(const std::string& base, const std::string& refRaw) {
    std::string ref = refRaw;
    size_t b = 0, e = ref.size();
    while (b < e && static_cast<unsigned char>(ref[b]) <= 0x20) ++b;
    while (e > b && static_cast<unsigned char>(ref[e - 1]) <= 0x20) --e;
    ref = ref.substr(b, e - b);
    if (ref.empty()) return std::string();
    if (ref.rfind("data:", 0) == 0 || ref.rfind("blob:", 0) == 0 || ref.rfind("javascript:", 0) == 0)
        return std::string();
    const std::string low = art_ascii_lower(ref);
    if (low.rfind("http://", 0) == 0 || low.rfind("https://", 0) == 0) return ref;
    const ArtUrlParts bu = art_url_parts(base);
    if (!bu.valid) return std::string();
    if (ref.size() >= 2 && ref[0] == '/' && ref[1] == '/') return bu.scheme + ":" + ref;
    const std::string root = bu.scheme + "://" + bu.host +
                             ((bu.port != 80 && bu.port != 443) ? (":" + std::to_string(bu.port)) : std::string());
    if (ref[0] == '/') return root + ref;
    // 相对路径：取 base 的目录部分
    std::string dir = bu.full;
    const size_t q = dir.find_first_of("?#");
    if (q != std::string::npos) dir = dir.substr(0, q);
    const size_t sl = dir.find_last_of('/');
    if (sl == std::string::npos || sl <= bu.scheme.size() + 2) return root + "/" + ref;
    dir = dir.substr(0, sl + 1);
    while (ref.rfind("../", 0) == 0) {
        ref = ref.substr(3);
        const size_t p2 = dir.find_last_of('/', dir.size() - 2);
        dir = (p2 == std::string::npos) ? root + "/" : dir.substr(0, p2 + 1);
    }
    return dir + ref;
}

// ---- 9.2 多策略取图 ----
struct ArtImgCand {
    std::string url;      // 绝对 URL（已去引号/实体）
    std::string kind;      // og:image / twitter:image / json-ld / image_src / itemprop / jsvar / img
    int score = 0;
};

inline bool art_looks_image_url(const std::string& u) {
    const std::string l = art_ascii_lower(u);
    if (l.empty()) return false;
    if (l.rfind("data:", 0) == 0 || l.rfind("blob:", 0) == 0) return false;
    if (l.find(".svg") != std::string::npos) return false;          // 矢量图/图标，不当封面
    static const char* kExt[] = { ".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".avif" };
    for (const char* x : kExt)
        if (l.find(x) != std::string::npos) return true;
    if (l.find("param=") != std::string::npos) return true;          // 网易云 ?param=300y300
    if (l.find("/image") != std::string::npos || l.find("avatar") != std::string::npos ||
        l.find("photo") != std::string::npos || l.find("img") != std::string::npos) return true;
    return false;
}

// 从标签串里取属性值（tag 必须是**小写副本**，返回值也来自小写副本 —— 非 ASCII 字节大小写折叠不动）
inline std::string art_tag_attr(const std::string& tag, const char* name) {
    const std::string n = art_ascii_lower(name);
    size_t p = 0;
    while ((p = tag.find(n, p)) != std::string::npos) {
        const bool leftOk = (p == 0) || tag[p - 1] == ' ' || tag[p - 1] == '\t' ||
                            tag[p - 1] == '\n' || tag[p - 1] == '\r' || tag[p - 1] == '/' ||
                            tag[p - 1] == '"' || tag[p - 1] == '\'';
        size_t q = p + n.size();
        if (!leftOk) { p = q; continue; }
        while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t' || tag[q] == '\n' || tag[q] == '\r')) ++q;
        if (q >= tag.size() || tag[q] != '=') { p = q; continue; }
        ++q;
        while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t' || tag[q] == '\n' || tag[q] == '\r')) ++q;
        if (q >= tag.size()) return std::string();
        std::string v;
        if (tag[q] == '"' || tag[q] == '\'') {
            const char qc = tag[q++];
            while (q < tag.size() && tag[q] != qc) v += tag[q++];
        } else {
            while (q < tag.size() && tag[q] != ' ' && tag[q] != '>' && tag[q] != '\t' &&
                   tag[q] != '\n' && tag[q] != '\r') v += tag[q++];
        }
        return v;
    }
    return std::string();
}

// 把一段"非结构化"文本里的第一个像图片的 URL 抠出来（JSON-LD / JS 变量共用）。
inline std::string art_first_image_url_in(const std::string& text, size_t from, size_t limit) {
    const size_t end = (limit == 0 || from + limit > text.size()) ? text.size() : from + limit;
    size_t i = from;
    while (i < end) {
        const size_t h = text.find("http", i);
        const size_t q1 = text.find('"', i);
        const size_t q2 = text.find('\'', i);
        size_t start = std::string::npos;
        char quote = 0;
        if (h != std::string::npos && h < end) { start = h; quote = 0; }
        if (q1 != std::string::npos && q1 < end && (start == std::string::npos || q1 < start)) {
            // 引号里可能是 "/path" 形式的相对路径
            if (q1 + 1 < end) { start = q1 + 1; quote = '"'; }
        }
        if (q2 != std::string::npos && q2 < end && (start == std::string::npos || q2 < start)) {
            if (q2 + 1 < end) { start = q2 + 1; quote = '\''; }
        }
        if (start == std::string::npos) return std::string();
        size_t e = start;
        if (quote) {
            while (e < end && text[e] != quote) ++e;
        } else {
            while (e < end && text[e] != '"' && text[e] != '\'' && text[e] != ' ' &&
                   text[e] != ')' && text[e] != ']' && text[e] != ',' && text[e] != '\\' &&
                   text[e] != '>' && text[e] != '<' && text[e] != '\n' && text[e] != '\r') ++e;
        }
        std::string v = art_html_unescape(text.substr(start, e - start));
        if (art_looks_image_url(v)) return v;
        i = (e > i) ? e : i + 1;
    }
    return std::string();
}

// 页面 → 候选图（按 score 降序、URL 去重）。nameHint 只用于 <img> 的 alt/title 加权。
inline std::vector<ArtImgCand> art_page_image_candidates(const std::string& html,
                                                         const std::string& baseUrl,
                                                         const std::string& nameHint = std::string()) {
    std::vector<ArtImgCand> out;
    if (html.empty()) return out;
    const std::string low = art_ascii_lower(html);
    const std::string hint = art_ascii_lower(nameHint);
    auto push = [&](const std::string& raw, const char* kind, int score) {
        if (raw.empty()) return;
        const std::string abs = art_url_abs(baseUrl, raw);
        if (abs.empty()) return;
        std::string probe;
        if (!art_url_public_ok(abs, &probe)) return;
        for (const ArtImgCand& c : out)
            if (c.url == abs) return;                          // 去重（先到先得 = 高分先到）
        ArtImgCand c;
        c.url = abs;
        c.kind = kind;
        c.score = score;
        out.push_back(std::move(c));
    };

    // ① <meta ...>：og:image / og:image:secure_url / twitter:image / twitter:image:src / thumbnail
    for (size_t i = 0; (i = low.find("<meta", i)) != std::string::npos;) {
        size_t e = low.find('>', i);
        if (e == std::string::npos) e = std::min(low.size(), i + 600);
        const std::string tag = low.substr(i, e - i);
        std::string key = art_tag_attr(tag, "property");
        if (key.empty()) key = art_tag_attr(tag, "name");
        if (key.empty()) key = art_tag_attr(tag, "itemprop");
        std::string val = art_tag_attr(tag, "content");
        if (val.empty()) val = art_tag_attr(tag, "value");
        if (!key.empty() && !val.empty()) {
            if (key == "og:image:secure_url")     push(val, "og:secure", 104);
            else if (key == "og:image" || key == "og:image:url") push(val, "og:image", 103);
            else if (key == "twitter:image")      push(val, "twitter", 100);
            else if (key == "twitter:image:src")  push(val, "twitter", 99);
            else if (key == "thumbnail" || key == "thumbnailurl" || key == "image") push(val, "meta", 96);
            else if (key == "itemprop:image")     push(val, "itemprop", 92);
        }
        i = e + 1;
    }
    // ② <link rel="image_src" href="..."> / rel="apple-touch-icon" 不收（小图标）
    for (size_t i = 0; (i = low.find("<link", i)) != std::string::npos;) {
        size_t e = low.find('>', i);
        if (e == std::string::npos) e = std::min(low.size(), i + 600);
        const std::string tag = low.substr(i, e - i);
        const std::string rel = art_tag_attr(tag, "rel");
        if (rel.find("image_src") != std::string::npos) push(art_tag_attr(tag, "href"), "image_src", 94);
        i = e + 1;
    }
    // ③ itemprop="image"（img / meta / link 都见过）
    for (size_t i = 0; (i = low.find("itemprop=\"image\"", i)) != std::string::npos;) {
        size_t s = (i > 300) ? i - 300 : 0;
        size_t e = std::min(low.size(), i + 500);
        const std::string tag = low.substr(s, e - s);
        std::string v = art_tag_attr(tag, "content");
        if (v.empty()) v = art_tag_attr(tag, "src");
        if (v.empty()) v = art_tag_attr(tag, "href");
        push(v, "itemprop", 92);
        i += 16;
    }
    // ④ JSON-LD：<script type="application/ld+json">…</script> 里的 image / thumbnailUrl
    for (size_t i = 0; (i = low.find("application/ld+json", i)) != std::string::npos;) {
        const size_t s = low.find('>', i);
        if (s == std::string::npos) break;
        size_t e = low.find("</script", s);
        if (e == std::string::npos) e = low.size();
        const std::string block = low.substr(s + 1, e - s - 1);
        for (const char* key : { "\"image\"", "\"thumbnailurl\"", "\"primaryimageofpage\"" }) {
            size_t p = 0;
            while ((p = block.find(key, p)) != std::string::npos) {
                const size_t colon = block.find(':', p + std::strlen(key));
                if (colon != std::string::npos)
                    push(art_first_image_url_in(block, colon + 1, 400), "json-ld", 90);
                p += std::strlen(key);
            }
        }
        i = e + 8;
    }
    // ⑤ JS/内联 JSON 变量：'pic' / 'cover' / 'avatar' / 'img1v1Url' / 'singerPic' …
    //    这条路是"宽松兜底"，所以只在**值确实像图片**时收（下载侧还有魔数/尺寸两道闸）。
    for (const char* key : { "\"pic\"", "\"picurl\"", "\"cover\"", "\"coverurl\"", "\"avatar\"",
                             "\"avatarurl\"", "\"headimg\"", "\"img1v1url\"", "\"singerpic\"",
                             "\"artistpic\"", "\"albumcover\"", "\"imgurl\"", "\"imageurl\"" }) {
        size_t p = 0;
        while ((p = low.find(key, p)) != std::string::npos) {
            size_t q = p + std::strlen(key);
            while (q < low.size() && (low[q] == ' ' || low[q] == '\t' || low[q] == '\n' || low[q] == '\r')) ++q;
            if (q >= low.size() || low[q] != ':') { p = q + 1; continue; }
            push(art_first_image_url_in(low, q + 1, 300), "jsvar", 70);
            p = q + 1;
        }
    }
    // ⑥ <img> 启发式（最后兜底）：按 class/alt/文件名 + 尺寸属性打分；图标/logo/占位一律跳过
    // 站点家具（2026-10-27 实测踩到的坑）：dizzylab 有个 /static/dl-n1.jpg（867x225 站招横幅），
    // 它既过魔数也过尺寸，于是被当成"Mugai无害P 的头像"落盘了 —— 错图比空白更糟，必须在这层拦住。
    static const char* kBad[] = { "logo", "icon", "sprite", "placeholder", "blank", "spacer", "1x1",
                                  "pixel", "loading", "nophoto", "no-photo", "noimage", "no-image",
                                  "default_avatar", "defaultavatar", "banner", "qrcode",
                                  "/static/", "/assets/", "/theme/", "/skin/", "/template/",
                                  "/common/", "/shared/", "favicon", "avatar_default",
                                  "share-", "btn-", "-btn", "nav-", "-nav", "header", "footer",
                                  "watermark", "qlogo" };
    for (size_t i = 0; (i = low.find("<img", i)) != std::string::npos;) {
        size_t e = low.find('>', i);
        if (e == std::string::npos) e = std::min(low.size(), i + 800);
        const std::string tag = low.substr(i, e - i);
        std::string src = art_tag_attr(tag, "src");
        if (src.empty()) src = art_tag_attr(tag, "data-src");
        if (src.empty()) src = art_tag_attr(tag, "data-original");
        if (!src.empty()) {
            const std::string sl = art_ascii_lower(src);
            bool bad = false;
            for (const char* b2 : kBad)
                if (sl.find(b2) != std::string::npos) { bad = true; break; }
            if (!bad) {
                int sc = 20;
                const std::string alt = art_tag_attr(tag, "alt") + " " + art_tag_attr(tag, "title") + " " +
                                        art_tag_attr(tag, "class");
                if (sl.find("avatar") != std::string::npos || sl.find("head") != std::string::npos ||
                    sl.find("singer") != std::string::npos || sl.find("artist") != std::string::npos ||
                    sl.find("portrait") != std::string::npos || sl.find("profile") != std::string::npos) sc += 45;
                if (sl.find("cover") != std::string::npos || sl.find("album") != std::string::npos ||
                    sl.find("jacket") != std::string::npos || sl.find("artwork") != std::string::npos) sc += 45;
                if (alt.find("avatar") != std::string::npos || alt.find("cover") != std::string::npos ||
                    alt.find("album") != std::string::npos || alt.find("头像") != std::string::npos ||
                    alt.find("封面") != std::string::npos) sc += 20;
                if (!hint.empty() && alt.find(hint) != std::string::npos) sc += 30;
                const int w = std::atoi(art_tag_attr(tag, "width").c_str());
                const int h2 = std::atoi(art_tag_attr(tag, "height").c_str());
                if (w >= 120 || h2 >= 120) sc += 15;
                if ((w > 0 && w <= 48) || (h2 > 0 && h2 <= 48)) sc -= 40;   // 小图 = 图标/缩略
                // 只收"有明确暗示"的图：score 基线 20 意味着它只是页面里的普通插图，
                // 收它就是在赌 —— 实测那次赌输（落了一张站招横幅）。≥45 = 命中 avatar/cover/artist
                // 之类的强暗示（或 alt 里有名字）。
                if (sc >= 45) push(src, "img", sc);
            }
        }
        i = e + 1;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const ArtImgCand& a, const ArtImgCand& b) { return a.score > b.score; });
    return out;
}

// ---- 9.3 图片尺寸（1×1 占位图 / 追踪像素必须被拒） ----
// 只解析 PNG/JPEG/GIF/WebP 的头部；解析不出就返回 false（调用方按"未知尺寸"处理）。
inline bool art_image_dims(const std::string& b, int* w, int* h) {
    if (b.size() < 16) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data());
    // PNG：IHDR 宽高在 16..24（大端）
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G' && b.size() >= 24) {
        const unsigned int ww = ((unsigned)p[16] << 24) | ((unsigned)p[17] << 16) | ((unsigned)p[18] << 8) | p[19];
        const unsigned int hh = ((unsigned)p[20] << 24) | ((unsigned)p[21] << 16) | ((unsigned)p[22] << 8) | p[23];
        if (ww == 0 || hh == 0 || ww > 20000 || hh > 20000) return false;
        if (w) *w = (int)ww;
        if (h) *h = (int)hh;
        return true;
    }
    // GIF：6..10（小端）
    if (b.size() >= 10 && b.compare(0, 3, "GIF") == 0) {
        const unsigned int ww = (unsigned)p[6] | ((unsigned)p[7] << 8);
        const unsigned int hh = (unsigned)p[8] | ((unsigned)p[9] << 8);
        if (ww == 0 || hh == 0) return false;
        if (w) *w = (int)ww;
        if (h) *h = (int)hh;
        return true;
    }
    // WebP：VP8X / VP8 / VP8L
    if (b.size() >= 30 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) {
        if (b.compare(12, 4, "VP8X") == 0) {
            const unsigned int ww = (unsigned)p[24] | ((unsigned)p[25] << 8) | ((unsigned)p[26] << 16);
            const unsigned int hh = (unsigned)p[27] | ((unsigned)p[28] << 8) | ((unsigned)p[29] << 16);
            if (ww == 0 || hh == 0) return false;
            if (w) *w = (int)(ww + 1);
            if (h) *h = (int)(hh + 1);
            return true;
        }
        if (b.compare(12, 4, "VP8 ") == 0 && b.size() >= 30) {
            const unsigned int ww = ((unsigned)p[26] | ((unsigned)p[27] << 8)) & 0x3FFF;
            const unsigned int hh = ((unsigned)p[28] | ((unsigned)p[29] << 8)) & 0x3FFF;
            if (ww == 0 || hh == 0) return false;
            if (w) *w = (int)ww;
            if (h) *h = (int)hh;
            return true;
        }
        if (b.compare(12, 4, "VP8L") == 0 && b.size() >= 25) {
            const unsigned bits = (unsigned)p[21] | ((unsigned)p[22] << 8) | ((unsigned)p[23] << 16) | ((unsigned)p[24] << 24);
            const unsigned ww = (bits & 0x3FFF) + 1;
            const unsigned hh = ((bits >> 14) & 0x3FFF) + 1;
            if (w) *w = (int)ww;
            if (h) *h = (int)hh;
            return true;
        }
    }
    // JPEG：走 SOFn 段
    if (p[0] == 0xFF && p[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < b.size()) {
            if (p[i] != 0xFF) { ++i; continue; }
            const unsigned char mk = p[i + 1];
            if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) { i += 2; continue; }
            const size_t len = ((size_t)p[i + 2] << 8) | p[i + 3];
            if (len < 2) return false;
            const bool isSof = (mk >= 0xC0 && mk <= 0xC3) || (mk >= 0xC5 && mk <= 0xC7) ||
                               (mk >= 0xC9 && mk <= 0xCB) || (mk >= 0xCD && mk <= 0xCF);
            if (isSof) {
                if (i + 8 >= b.size()) return false;
                const unsigned int hh = ((unsigned)p[i + 5] << 8) | p[i + 6];
                const unsigned int ww = ((unsigned)p[i + 7] << 8) | p[i + 8];
                if (ww == 0 || hh == 0) return false;
                if (w) *w = (int)ww;
                if (h) *h = (int)hh;
                return true;
            }
            i += 2 + len;
        }
        return false;
    }
    return false;
}

// 图片闸（魔数 + 字节下限 + 最小边）。为什么加尺寸：用户明确要求"1×1 占位图必须被拒"，
// 而 1×1 PNG 只有 ~70 B（字节闸能挡），1×1 GIF 却可以做到 43 B~2 KB 之间任意大小。
inline bool art_image_ok(const std::string& b, size_t minBytes, int minSide, std::string* why,
                         int* wOut = nullptr, int* hOut = nullptr, double maxAspect = 2.6) {
    if (b.size() < minBytes) {
        if (why) *why = "too_small " + std::to_string((long long)b.size()) + "B";
        return false;
    }
    if (!art_image_magic(b)) { if (why) *why = "not_image"; return false; }
    int w = 0, h = 0;
    const bool known = art_image_dims(b, &w, &h);
    if (wOut) *wOut = w;
    if (hOut) *hOut = h;
    if (known && minSide > 0 && (w < minSide || h < minSide)) {
        if (why) *why = "too_small_dims " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }
    // 长宽比闸（2026-10-27 实测事故）：酷我页面里有张 64x316 的"唱片条"装饰图，
    // 它恰好等于最小边 64、也能过魔数，于是被当成「秦勇/游戏科学/8082Audio 的头像」落盘。
    // 头像与封面都是接近方形的，横幅/分隔条/跑马灯则是极端长宽比 —— 用比例一刀切掉。
    if (known && maxAspect > 0.0) {
        const double ar = (double)w / (double)h;
        if (ar > maxAspect || ar < 1.0 / maxAspect) {
            if (why) *why = "bad_aspect " + std::to_string(w) + "x" + std::to_string(h);
            return false;
        }
    }
    if (why) why->clear();
    return true;
}

// ---- 9.4 名字归一（简繁折叠）：只用于"候选名是不是这个人" ----
// 为什么必须有它：bangumi 的艺名是**繁体**（天門），用户库里是简体（天门）——
// 不做折叠就只能留空；但也不能放宽到编辑距离（那会把"徐瞑蕾"匹配成"徐娇"）。
inline unsigned long art_fold_cjk_t2s(unsigned long cp) {
    // 只覆盖「人名里真正常见」的简繁对；同形字显式写出来（免得被当成漏项）。
    switch (cp) {
        case 0x9580: return 0x95E8;   // 門→门
        case 0x6A02: return 0x4E50;   // 樂→乐
        case 0x7121: return 0x65E0;   // 無→无
        case 0x70BA: return 0x4E3A;   // 為→为
        case 0x5CF6: return 0x5C9B;   // 島→岛
        case 0x6FA4: return 0x6CFD;   // 澤→泽
        case 0x5BAE: return 0x5BAB;   // 宮→宫
        case 0x6BBA: return 0x6A31;   // 櫻→樱
        case 0x6A39: return 0x6811;   // 樹→树
        case 0x9ED1: return 0x9EC4;   // 黑→黄(异体，仅防颠倒)
        case 0x9F8D: return 0x9F99;   // 龍→龙
        case 0x83EF: return 0x534E;   // 華→华
        case 0x8449: return 0x53F6;   // 葉→叶
        case 0x6A4B: return 0x6865;   // 橋→桥
        case 0x908A: return 0x8FB9;   // 邊→边
        case 0x6FF1: return 0x6EE8;   // 濱→滨
        case 0x7E6A: return 0x7ED8;   // 繪→绘
        case 0x5B78: return 0x5B66;   // 學→学
        case 0x6703: return 0x4F1A;   // 會→会
        case 0x570B: return 0x56FD;   // 國→国
        case 0x9EDE: return 0x70B9;   // 點→点
        case 0x96FB: return 0x7535;   // 電→电
        case 0x8ECA: return 0x8F66;   // 車→车
        case 0x6771: return 0x4E1C;   // 東→东
        case 0x8A9E: return 0x8BED;   // 語→语
        case 0x8A71: return 0x8BDD;   // 話→话
        case 0x8AAA: return 0x8BF4;   // 說→说
        case 0x8A18: return 0x8BB0;   // 記→记
        case 0x8A08: return 0x8BA1;   // 計→计
        case 0x8A2D: return 0x8BBE;   // 設→设
        case 0x8A31: return 0x8BB8;   // 許→许
        case 0x8A8D: return 0x8BA4;   // 認→认
        case 0x8ABF: return 0x8C03;   // 調→调
        case 0x8ACB: return 0x8BF7;   // 請→请
        case 0x8AD6: return 0x8BBA;   // 論→论
        case 0x8B8A: return 0x53D8;   // 變→变
        case 0x8B93: return 0x8BA9;   // 讓→让
        case 0x8B77: return 0x62A4;   // 護→护
        case 0x9322: return 0x94B1;   // 錢→钱
        case 0x9280: return 0x94F6;   // 銀→银
        case 0x9435: return 0x94C1;   // 鐵→铁
        case 0x9577: return 0x957F;   // 長→长
        case 0x95DC: return 0x5173;   // 關→关
        case 0x967D: return 0x9633;   // 陽→阳
        case 0x9670: return 0x9634;   // 陰→阴
        case 0x969B: return 0x9645;   // 際→际
        case 0x968A: return 0x961F;   // 隊→队
        case 0x96A8: return 0x968F;   // 隨→随
        case 0x96E3: return 0x96BE;   // 難→难
        case 0x96F2: return 0x4E91;   // 雲→云
        case 0x9748: return 0x7075;   // 靈→灵
        case 0x98A8: return 0x98CE;   // 風→风
        case 0x98DB: return 0x98DE;   // 飛→飞
        case 0x98EF: return 0x996D;   // 飯→饭
        case 0x99AC: return 0x9A6C;   // 馬→马
        case 0x9A57: return 0x9A8C;   // 驗→验
        case 0x9AD4: return 0x4F53;   // 體→体
        case 0x9B6F: return 0x9C81;   // 魯→鲁
        case 0x9BCA: return 0x9C8D;   // 鯊→鲨
        case 0x9CE5: return 0x9E1F;   // 鳥→鸟
        case 0x9E97: return 0x4E3D;   // 麗→丽
        case 0x9EC3: return 0x9EC4;   // 黃→黄
        case 0x9EBB: return 0x9EBB;   // 麻(同形)
        case 0x9F13: return 0x9F13;
        case 0x4E26: return 0x5E76;   // 並→并
        case 0x500B: return 0x4E2A;   // 個→个
        case 0x5099: return 0x5907;   // 備→备
        case 0x50B7: return 0x4F24;   // 傷→伤
        case 0x50C5: return 0x4EC5;   // 僅→仅
        case 0x50D1: return 0x4FA8;   // 僑→侨
        case 0x50F9: return 0x4EF7;   // 價→价
        case 0x5104: return 0x4EBF;   // 億→亿
        case 0x511F: return 0x507F;   // 償→偿
        case 0x512A: return 0x4F18;   // 優→优
        case 0x5132: return 0x50A8;   // 儲→储
        case 0x5147: return 0x51F6;   // 兇→凶
        case 0x514C: return 0x5151;   // 兌→兑
        case 0x5157: return 0x515C;   // 兜→兜
        case 0x5169: return 0x4E24;   // 兩→两
        case 0x518A: return 0x518C;   // 冊→册
        case 0x5225: return 0x522B;   // 別→别
        case 0x5229: return 0x5229;   // 利
        case 0x5247: return 0x5219;   // 則→则
        case 0x5287: return 0x5267;   // 劇→剧
        case 0x52D5: return 0x52A8;   // 動→动
        case 0x52D9: return 0x52A1;   // 務→务
        case 0x52DD: return 0x80DC;   // 勝→胜
        case 0x52DE: return 0x52B3;   // 勞→劳
        case 0x52F5: return 0x52B1;   // 勵→励
        case 0x52F8: return 0x529D;   // 勸→劝
        case 0x5331: return 0x532E;   // 匱→匮
        case 0x5340: return 0x533A;   // 區→区
        case 0x5354: return 0x534F;   // 協→协
        case 0x535E: return 0x535E;   // 卞
        case 0x5379: return 0x6064;   // 恤
        case 0x5399: return 0x538D;   // 厙→厍
        case 0x53AD: return 0x538C;   // 厭→厌
        default: return cp;
    }
}

// 「这是不是同一个名字」的宽松判据（**只用于候选验收**，不参与任何精确匹配的业务逻辑）：
//   ① 去掉成对括号里的补充说明（"水瀬いのり (水濑祈)" → "水瀬いのり"）、空白、常见标点；
//   ② 简繁折叠；
//   ③ 完全相等才算命中 —— 不做编辑距离（那会把"徐瞑蕾"匹配成"徐娇"）。
inline std::string art_name_key(const std::string& raw) {
    std::string noParen;
    for (size_t i = 0; i < raw.size();) {
        const unsigned char c = (unsigned char)raw[i];
        size_t n = 1;
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        if (i + n > raw.size()) break;
        // 全角/半角括号：跳过整段（只跳到配对的右括号；找不到就丢掉到串尾，与 art_trim 的语义一致）
        const bool open = (c == '(' || c == '[') ||
                          (c == 0xEF && i + 2 < raw.size() && (unsigned char)raw[i + 1] == 0xBC &&
                           ((unsigned char)raw[i + 2] == 0x88 || (unsigned char)raw[i + 2] == 0x89));
        if (open) {
            size_t j = i + n;
            int depth = 1;
            while (j < raw.size() && depth > 0) {
                const unsigned char d = (unsigned char)raw[j];
                size_t m = 1;
                if (d >= 0xF0) m = 4; else if (d >= 0xE0) m = 3; else if (d >= 0xC0) m = 2;
                if (j + m > raw.size()) break;
                if (d == '(' || d == '[') ++depth;
                else if (d == ')' || d == ']') --depth;
                else if (d == 0xEF && j + 2 < raw.size() && (unsigned char)raw[j + 1] == 0xBC) {
                    if ((unsigned char)raw[j + 2] == 0x88) ++depth;
                    else if ((unsigned char)raw[j + 2] == 0x89) --depth;
                }
                j += m;
            }
            i = j;
            continue;
        }
        noParen.append(raw, i, n);
        i += n;
    }
    std::string o;
    o.reserve(noParen.size());
    for (size_t i = 0; i < noParen.size();) {
        const unsigned char c = (unsigned char)noParen[i];
        if (c < 0x80) {
            const char ch = (char)c;
            // 只处理 ASCII 标点：多字节标点（、·（））在下方的非 ASCII 分支里折/丢，
            // 这里若写成 '·' 这种多字节字面量会直接编译失败（char 放不下）。
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '.' || ch == ',' ||
                ch == '&' || ch == '_' || ch == '-' || ch == '\'' || ch == '"' || ch == '/') {
                ++i;
                continue;
            }
            if (ch >= 'A' && ch <= 'Z') { o += (char)(ch - 'A' + 'a'); ++i; continue; }
            o += ch;
            ++i;
            continue;
        }
        size_t n = 1;
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        if (i + n > noParen.size()) break;
        unsigned long cp = (n == 2) ? (unsigned long)(c & 0x1Fu)
                         : (n == 3) ? (unsigned long)(c & 0x0Fu)
                                    : (unsigned long)(c & 0x07u);
        for (size_t k = 1; k < n; ++k)
            cp = (cp << 6) | (unsigned long)((unsigned char)noParen[i + k] & 0x3Fu);
        if (cp == 0x3000u) { ++i; i += 0; continue; }                 // 全角空格丢掉
        if (cp == 0x30FBu || cp == 0x00B7u || cp == 0x2027u) { i += n; continue; }  // ・ · ‧
        if (cp >= 0xFF01u && cp <= 0xFF5Eu) cp -= 0xFEE0u;             // 全角 ASCII
        cp = art_fold_cjk_t2s(cp);
        if (cp < 0x80u) { o += (char)cp; i += n; continue; }
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
        i += n;
    }
    return o;
}

inline bool art_name_same_loose(const std::string& a, const std::string& b) {
    const std::string ka = art_name_key(a), kb = art_name_key(b);
    if (ka.empty() || kb.empty()) return false;
    return ka == kb;
}

// ---- 9.5 确定性扩展源（bangumi / AniList / Apple Music）----
// 为什么是这三家（2026-10-27 联网实测，六个失败样本逐个过）：
//   · bangumi(api.bgm.tv) —— 中文动画数据库，**免 key**，人名是汉字，images.large 就是官方头像；
//     实测「水瀬いのり (水濑祈)」「手嶌 葵」「天门(←天門)」「天门 (てんもん)」四个样本的第一名都是同名命中；
//   · AniList(graphql) —— 免 key，name.native 是**日文原名**，可以做"同名验收"而不用猜罗马字
//     （用它查「水瀬いのり」直接返回 native=水瀬いのり）；「天门」查不到（Kanji-only 不在它的搜索索引里），
//     这条由 bangumi 兜住了 —— 两家互补，不是重复;
//   · Apple Music —— iTunes 的 musicArtist 搜索只给 artistId（不给图），但艺人页 music.apple.com/jp/artist/<id>
//     带 og:image（实测 水瀬いのり 1057869661 → is1-ssl.mzstatic.com/.../1200x630cw.png）。
// 三家都要求**名字匹配**才采纳（art_name_same_loose），绝不"第一个结果就用"。
struct ArtWebFill {
    bool ok = false;
    std::string bytes, url, source;
    std::vector<ArtSourceTry> tried;
    std::string summary() const {
        std::ostringstream o;
        o << (ok ? "ok" : "miss") << " src=" << (source.empty() ? "-" : source)
          << " bytes=" << bytes.size();
        if (!url.empty()) o << " url=" << url;
        for (const ArtSourceTry& t : tried)
            o << " | " << t.source << ":" << (t.ok ? "ok" : (t.transient ? "fail" : "miss"))
              << "(" << art_oneline(t.reason) << "," << t.ms << "ms)";
        return o.str();
    }
};

inline std::string art_json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { o += ' '; }
        else o += c;
    }
    return o;
}

// 名字变体（去括号 / 去空格 / 主艺人）——给"哪家源用什么写法"用。
inline std::vector<std::string> art_name_variants(const std::string& raw) {
    std::vector<std::string> out;
    auto add = [&](const std::string& v) {
        const std::string t = art_trim_name(v);
        if (t.empty()) return;
        // 去重按**字面**比较：不能用 art_name_same_loose —— 它会把「水瀬いのり」和
        // 「水瀬いのり (水濑祈)」判成同一个，于是"去括号的短名"永远进不了变体表。
        for (const std::string& x : out)
            if (x == t) return;
        out.push_back(t);
    };
    add(raw);
    // 去括号：用 art_name_key 的规则反向重建太绕，这里直接截到第一个 "(" / "（"
    size_t cut = std::string::npos;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '(' || raw[i] == '[') { cut = i; break; }
        if ((unsigned char)raw[i] == 0xEF && i + 2 < raw.size() &&
            (unsigned char)raw[i + 1] == 0xBC &&
            ((unsigned char)raw[i + 2] == 0x88 || (unsigned char)raw[i + 2] == 0x89)) { cut = i; break; }
    }
    if (cut != std::string::npos && cut > 0) add(raw.substr(0, cut));
    const std::string prim = art_primary_artist(raw);
    if (!prim.empty()) add(prim);
    return out;
}

// 相关性闸：页面里到底有没有提到这个名字？
// 为什么必须有（2026-10-27 实测事故）：搜索"徐瞑蕾 头像"返回的是百度百科《守候》条目，
// 那条目有 og:image、能过全部图片闸，于是被当成"徐瞑蕾的头像"落盘 —— 人完全不对。
// 判据很朴素但有效：页面的 UTF-8 文本里必须出现名字本身（或用它的简写变体，如去括号后的
// "水瀬いのり"）。名字是用户数据，只做**包含**查找，不做任何解释/执行。
inline bool art_html_has_name(const std::string& html, const std::string& rawName) {
    if (html.empty() || rawName.empty()) return false;
    const std::string low = art_ascii_lower(html);
    const std::vector<std::string> vars = art_name_variants(rawName);
    for (const std::string& v : vars) {
        if (art_utf8_len(v) < 2) continue;                 // 单字名（"甲"）不作判据，误命中太多
        if (low.find(art_ascii_lower(v)) != std::string::npos) return true;
        const std::string key = art_name_key(v);           // 简繁折叠后的形态（天门 ← 天門）
        if (key.size() >= 6 && low.find(key) != std::string::npos) return true;
        if (key.size() >= 6 && art_name_key(html).find(key) != std::string::npos) return true;
    }
    return false;
}

// 页面标题（<title> / og:title / twitter:title）——比"正文提到名字"强得多的相关性判据。
// 实测事故（2026-10-27）：百度百科《守候》条目正文里确实提到"演唱者为徐瞑蕾"，但那张 og:image
// 是**游戏/单曲的封面**，不是她的头像。而"页面标题里有没有这个名字"正好把两类分开：
//   · 人物/艺人条目：标题 = 名字（或含名字）  ⟹ 收
//   · 歌曲/游戏/专辑条目：标题 = 作品名      ⟹ 拒
inline std::string art_html_title(const std::string& html) {
    std::string out;
    const std::string low = art_ascii_lower(html);
    const size_t t0 = low.find("<title");
    if (t0 != std::string::npos) {
        const size_t s0 = low.find('>', t0);
        const size_t e0 = (s0 == std::string::npos) ? std::string::npos : low.find("</title", s0);
        if (s0 != std::string::npos && e0 != std::string::npos && e0 > s0)
            out = art_html_unescape(html.substr(s0 + 1, e0 - s0 - 1));
    }
    for (const char* key : { "og:title", "twitter:title" }) {
        size_t p = 0;
        while ((p = low.find(key, p)) != std::string::npos) {
            const size_t cs = low.find("content=\"", p);
            if (cs == std::string::npos) break;
            const size_t ce = low.find('"', cs + 9);
            if (ce == std::string::npos) break;
            if (!out.empty()) out += " | ";
            out += art_html_unescape(html.substr(cs + 9, ce - cs - 9));
            p = ce;
        }
    }
    return out;
}

// 标题里有没有这个名字：汉字按"变体包含"，ASCII 按 ≥5 字符的独立词（Mugai ← "Mugai无害P"，
// 这样 anihk 那种纯英文条目页也能过；而 "Peak" 这种 4 字母的常见词不算，避免误命中）。
inline bool art_title_has_name(const std::string& title, const std::string& rawName) {
    if (title.empty() || rawName.empty()) return false;
    const std::string lowTitle = art_ascii_lower(title);
    const std::string keyTitle = art_name_key(title);   // 去空格/标点 + 简繁折叠后的形态
    for (const std::string& v : art_name_variants(rawName)) {
        if (art_utf8_len(v) < 2) continue;
        if (lowTitle.find(art_ascii_lower(v)) != std::string::npos) return true;
        // "手嶌 葵"（用户库带空格）vs 标题里的 "手嶌葵"：折叠后比一次；
        // 阈值 6 字节（≈2 个汉字）避免单字名在标题里碰巧出现。
        const std::string vk = art_name_key(v);
        if (vk.size() >= 6 && keyTitle.find(vk) != std::string::npos) return true;
    }
    // ASCII 词：从名字里切 [A-Za-z0-9] 连续段，长度 ≥5 才当判据
    std::string cur;
    auto flush = [&]() {
        if (cur.size() >= 5 && lowTitle.find(art_ascii_lower(cur)) != std::string::npos) return true;
        cur.clear();
        return false;
    };
    for (unsigned char c : rawName) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) cur += (char)c;
        else if (flush()) return true;
    }
    if (flush()) return true;
    return false;
}

// ---- 9.5b 音频平台官方接口优先（用户 261026-04 #7/#9 的"官方接口"要求）----
// 为什么必须补这一段（2026-10-27 实测）：搜「徐瞑蕾 头像」返回的候选页里就有
//   https://y.music.163.com/m/artist?id=30165378
// 但那个页面是 JS 壳（title="网易云音乐"，HTML 里没有图），按"从 HTML 抽图"永远拿不到。
// 而**官方接口** https://music.163.com/api/artist/30165378 直接回
//   {"artist":{"name":"徐瞑蕾","picUrl":"https://p2.music.126.net/...jpg","img1v1Url":"..."}}
// ⟹ 正确分工是：从候选页/HTML 里抠出**平台 id**，再用官方接口换图（还自带 name 可做同名验收）。
// 这条比"解析网页"稳得多，也比问模型便宜得多；QQ 那边 script 路径已覆盖（musicu.fcg），这里不重复。
inline std::vector<std::string> art_page_netease_artist_ids(const std::string& html, const std::string& pageUrl) {
    std::vector<std::string> out;
    const std::string hay = art_ascii_lower(pageUrl + "\n" + html);
    static const char* kPat[] = { "music.163.com/artist?id=", "music.163.com/#/artist?id=",
                                  "y.music.163.com/m/artist?id=", "\"artistid\":", "artist?id=" };
    for (const char* pat : kPat) {
        size_t p = 0;
        while ((p = hay.find(pat, p)) != std::string::npos) {
            size_t q = p + std::strlen(pat);
            while (q < hay.size() && (hay[q] == ' ' || hay[q] == '"')) ++q;
            std::string id;
            while (q < hay.size() && hay[q] >= '0' && hay[q] <= '9' && id.size() < 12) id += hay[q++];
            if (id.size() >= 4) {
                bool dup = false;
                for (const std::string& x : out) if (x == id) { dup = true; break; }
                if (!dup) out.push_back(id);
                if (out.size() >= 3) return out;
            }
            p = q;
        }
    }
    return out;
}

inline ArtFetchOut art_ext_netease_id(const std::string& id, const std::string& wantName,
                                      const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    ArtHttpRequest q;
    q.method = "GET";
    q.url = "https://music.163.com/api/artist/" + id;
    q.headers = { std::string("User-Agent: ") + pv_user_agent(), "Referer: https://music.163.com/" };
    q.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse resp = art_http(q);
    if (resp.status == 0) { out.transient = true; out.reason = "请求失败: " + resp.error; return out; }
    if (resp.status != 200) { out.transient = (resp.status >= 500 || resp.status == 429);
                              out.reason = "接口 HTTP " + std::to_string(resp.status); return out; }
    const rta::net::Json j = rta::net::json_parse(std::string(resp.body.begin(), resp.body.end()));
    const std::string nm = j["artist"]["name"].s();
    if (nm.empty()) { out.reason = "接口无 artist.name"; return out; }
    if (!wantName.empty() && !art_name_same_loose(nm, wantName)) {
        out.reason = "id=" + id + " 名字不符(接口=" + art_oneline(nm) + ")";
        return out;
    }
    // ★ 占位图识别（2026-10-27 实测事故）：徐瞑蕾(id=30165378) 在网易云**没有自定义头像**，
    // 接口回 picId=0 + 默认剪影图（p1.music.126.net/.../5639395138885805.jpg，640x420 的
    // 歌手剪影）。它有 149 KB、过魔数、过尺寸，什么闸都拦不住 —— 只有 picId==0 这条能拦。
    // 对比：周杰伦 id=6452 的 picId=109951169164936450（正常）。
    if (j["artist"]["picId"].n() <= 0.0) {
        out.reason = "id=" + id + " 网易云无自定义头像(picId=0，返回的是默认剪影)";
        return out;
    }
    std::string img = j["artist"]["picUrl"].s();
    if (img.empty()) img = j["artist"]["img1v1Url"].s();
    if (img.empty()) { out.reason = "id=" + id + " 无 picUrl"; return out; }
    ArtFetchOut dl = art_download_image(img, { std::string("User-Agent: ") + pv_user_agent(),
                                               "Referer: https://music.163.com/" }, opts, budgetMs);
    if (!dl.ok) { dl.reason = "id=" + id + " " + dl.reason; return dl; }
    return dl;
}

inline ArtFetchOut art_ext_bangumi(const std::string& query, const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    ArtHttpRequest q;
    q.method = "POST";
    q.url = "https://api.bgm.tv/v0/search/persons";
    q.body = "{\"keyword\":\"" + art_json_escape(query) + "\",\"filter\":{\"type\":[1]}}";
    q.contentType = "application/json";
    q.headers = { std::string("User-Agent: ") + pv_user_agent(), "Accept: application/json" };
    q.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse resp = art_http(q);
    if (resp.status == 0) { out.transient = true; out.reason = "请求失败: " + resp.error; return out; }
    if (resp.status != 200) { out.transient = (resp.status >= 500 || resp.status == 429);
                              out.reason = "搜索 HTTP " + std::to_string(resp.status); return out; }
    const rta::net::Json j = rta::net::json_parse(std::string(resp.body.begin(), resp.body.end()));
    const rta::net::Json& arr = j["data"];
    if (arr.size() == 0) { out.reason = "搜索 0 条"; return out; }
    std::string first = arr.at(0)["name"].s();
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& it = arr.at(i);
        const std::string nm = it["name"].s();
        const std::string cn = it["name_cn"].s();
        if (!art_name_same_loose(nm, query) && !art_name_same_loose(cn, query)) continue;
        std::string img = it["images"]["large"].s();
        if (img.empty()) img = it["images"]["medium"].s();
        if (img.empty()) { out.reason = "同名「" + art_oneline(nm) + "」无图"; return out; }
        ArtFetchOut dl = art_download_image(img, { std::string("User-Agent: ") + pv_user_agent(),
                                                   "Referer: https://bgm.tv/" }, opts, budgetMs);
        if (!dl.ok) { dl.reason = "候选「" + art_oneline(nm) + "」" + dl.reason; return dl; }
        return dl;
    }
    out.reason = "无同名(首个=" + art_oneline(first) + ")";
    return out;
}

inline ArtFetchOut art_ext_anilist(const std::string& query, const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    ArtHttpRequest q;
    q.method = "POST";
    q.url = "https://graphql.anilist.co";
    q.body = std::string("{\"query\":\"query($s:String){Page(perPage:5){staff(search:$s){id name{full native} "
                         "image{large}}}}\",\"variables\":{\"s\":\"") + art_json_escape(query) + "\"}}";
    q.contentType = "application/json";
    q.headers = { std::string("User-Agent: ") + pv_user_agent(), "Accept: application/json" };
    q.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse resp = art_http(q);
    if (resp.status == 0) { out.transient = true; out.reason = "请求失败: " + resp.error; return out; }
    if (resp.status != 200) { out.transient = (resp.status >= 500 || resp.status == 429);
                              out.reason = "搜索 HTTP " + std::to_string(resp.status); return out; }
    const rta::net::Json j = rta::net::json_parse(std::string(resp.body.begin(), resp.body.end()));
    const rta::net::Json& arr = j["data"]["Page"]["staff"];
    if (arr.size() == 0) { out.reason = "搜索 0 条"; return out; }
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& it = arr.at(i);
        const std::string full = it["name"]["full"].s();
        const std::string nat  = it["name"]["native"].s();
        if (!art_name_same_loose(nat, query) && !art_name_same_loose(full, query)) continue;
        const std::string img = it["image"]["large"].s();
        if (img.empty() || img.find("/default.") != std::string::npos) {
            out.reason = "同名「" + art_oneline(nat.empty() ? full : nat) + "」无图";
            return out;
        }
        ArtFetchOut dl = art_download_image(img, { std::string("User-Agent: ") + pv_user_agent(),
                                                   "Referer: https://anilist.co/" }, opts, budgetMs);
        if (!dl.ok) { dl.reason = "候选「" + art_oneline(nat.empty() ? full : nat) + "」" + dl.reason; return dl; }
        return dl;
    }
    out.reason = "无同名(首个=" + art_oneline(arr.at(0)["name"]["native"].s().empty()
                                                ? arr.at(0)["name"]["full"].s()
                                                : arr.at(0)["name"]["native"].s()) + ")";
    return out;
}

inline ArtFetchOut art_ext_apple(const std::string& query, const ArtworkOpts& opts, int budgetMs) {
    ArtFetchOut out;
    ArtHttpRequest q;
    q.method = "GET";
    q.url = "https://itunes.apple.com/search?term=" + percent_enc_utf8(query) +
            "&entity=musicArtist&limit=5&country=jp";
    q.headers = { std::string("User-Agent: ") + pv_user_agent() };
    q.timeoutMs = std::max(500, std::min(opts.perSourceMs, budgetMs));
    const rta::net::HttpResponse resp = art_http(q);
    if (resp.status == 0) { out.transient = true; out.reason = "请求失败: " + resp.error; return out; }
    if (resp.status != 200) { out.transient = (resp.status >= 500 || resp.status == 429);
                              out.reason = "搜索 HTTP " + std::to_string(resp.status); return out; }
    const rta::net::Json j = rta::net::json_parse(std::string(resp.body.begin(), resp.body.end()));
    const rta::net::Json& arr = j["results"];
    if (arr.size() == 0) { out.reason = "搜索 0 条"; return out; }
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& it = arr.at(i);
        const std::string nm = it["artistName"].s();
        if (!art_name_same_loose(nm, query)) continue;          // 罗马字名与汉字名不同名 ⟹ 不采纳
        const long long id = (long long)it["artistId"].n();
        if (id <= 0) continue;
        ArtHttpRequest pq;
        pq.method = "GET";
        pq.url = "https://music.apple.com/jp/artist/" + std::to_string(id);
        pq.headers = { std::string("User-Agent: ") + pv_user_agent() };
        pq.timeoutMs = std::max(800, std::min(opts.perSourceMs, budgetMs));
        const rta::net::HttpResponse pr = art_http(pq);
        if (pr.status != 200) { out.transient = (pr.status == 0 || pr.status >= 500);
                                out.reason = "艺人页 HTTP " + std::to_string(pr.status); return out; }
        const std::string html(pr.body.begin(), pr.body.end());
        const std::vector<ArtImgCand> cands = art_page_image_candidates(html, pq.url, query);
        for (size_t c = 0; c < cands.size() && c < 2; ++c) {
            ArtFetchOut dl = art_download_image(cands[c].url,
                                                { std::string("User-Agent: ") + pv_user_agent(),
                                                  "Referer: https://music.apple.com/" }, opts, budgetMs);
            if (dl.ok) return dl;
            out.reason = cands[c].kind + " " + dl.reason;
        }
        if (!cands.empty()) return out;
        out.reason = "艺人页无候选图";
        return out;
    }
    out.reason = "无同名(首个=" + art_oneline(arr.at(0)["artistName"].s()) + ")";
    return out;
}

// 扩展链总入口：按"对中文/日文名字覆盖最好的顺序"跑，每个源只用**同名**结果。
// 只做网络与字节校验，不落盘（落盘仍由 rtaweb 的 apifill_write_blob 负责，保持单一写者）。
inline ArtWebFill art_ext_artist_image(const std::string& rawName, const ArtworkOpts& opts) {
    ArtWebFill res;
    const std::string name = art_trim_name(rawName);
    if (name.empty()) return res;
    const std::vector<std::string> variants = art_name_variants(name);
    struct Src { const char* id; ArtFetchOut (*fn)(const std::string&, const ArtworkOpts&, int); };
    static const Src kSrc[] = {
        { "bgm",     art_ext_bangumi },
        { "anilist", art_ext_anilist },
        { "apple",   art_ext_apple   },
    };
    int remaining = opts.totalBudgetMs > 0 ? opts.totalBudgetMs : 12000;
    for (const Src& s : kSrc) {
        for (const std::string& v : variants) {
            if (remaining <= 500) {
                ArtSourceTry t; t.source = s.id; t.reason = "总预算耗尽"; t.transient = true;
                res.tried.push_back(t);
                break;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const ArtFetchOut fo = s.fn(v, opts, remaining);
            ArtSourceTry t;
            t.source = s.id;
            t.ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
            remaining -= t.ms;
            t.ok = fo.ok;
            t.transient = fo.transient;
            t.url = fo.url;
            t.bytes = (long long)fo.bytes.size();
            if (fo.ok) {
                t.reason = std::to_string(fo.bytes.size()) + "B";
                res.tried.push_back(t);
                res.ok = true;
                res.bytes = fo.bytes;
                res.url = fo.url;
                res.source = std::string(s.id) + ":" + v;
                return res;
            }
            t.reason = (v == name ? std::string() : ("[" + v + "] ")) + fo.reason;
            res.tried.push_back(t);
            if (fo.transient) break;                  // 瞬态(断网/429)：这一源不再换写法重试
        }
    }
    return res;
}

} // namespace rta::meta
