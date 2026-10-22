// match.h — 候选评分与选择（纯函数：无网络、无设备、无 Windows 头）
//
// 元数据补全流水线的第 ③ 级（docs/元数据补全调研与规划.md §3）：
//   ② providers 产出候选 → ③ match（本文件）打分排序 → ④ fuse 融合（歧义时 LLM 裁决）
//
// 输入：vector<Candidate>（唯一定义在本文件；provider.h 的解析函数直接产出它，
//       enrich.h 接线时无需转换）与查询侧信息（qTitle/qArtist/qFeat/fileSec/qVersion，
//       来自 ① parse 的 ParsedName + 文件时长）。
// 输出：按总分降序的 Scored 列表 / 下标列表（match_rank）。不存指针，只存下标（重排安全）。
//
// 评分（全部 0~1，spec 定值）：
//   title_score    规范化后：相等=1.0；一方包含另一方=0.75；否则 1-编辑距离/maxLen（<0.45 记 0）；
//                  候选尾括号组（(Live)/(DJ版)/(inst.)…）先剥离再比——剥掉即豁免、不惩罚；
//                  查询侧 version 与候选括号组（规范化后）相同 → +0.1（封顶 1.0）
//   artist_score   qArtist 空 → 0.5 中性；候选 artist 拆 " / " 后任一与 qArtist 相等=1.0、
//                  相互包含=0.8；qFeat 命中候选任一 artist → +0.1（封顶 1.0）；
//                  qArtist 非空且完全不沾 → 0.0
//   duration_score candMs<=0 → 0.5 中性；差 ≤3s=1.0；≤10s=0.6；>10s=0.0
//   match_score    = 0.55*title + 0.30*artist + 0.15*duration
//
// 三档判定：
//   match_confident（bool，按任务口径）：top1 ≥ 0.80 且领先第二名 ≥ 0.10 → true；其余一律 false。
//   match_verdict（枚举，供上层区分「无可信」与「歧义区」）：
//     Confident = 上述 true 区；Weak = top1 < 0.45（无可信，标记待 LLM/人工）；
//     Ambiguous = 之间（含 top1 ≥ 0.80 但领先不足 0.10）——上层决定。
//
// 与任务 spec 的三处显式出入（均为加默认参/对称扩展，不传参即退化回 spec 行为）：
//   * title_score/match_score/match_score_all/match_rank/match_confident/match_verdict
//     尾部多一个默认参 qVersion——版本奖励的数据来源；不传 = 永无奖励，与 spec 签名行为一致；
//   * duration_score 对 fileSec<=0 也返回中性 0.5（spec 只写 candMs<=0；文件侧时长缺失
//     同样无法判断，惩罚候选没有道理——tests/test_metamatch.cpp 例 4 覆盖）；
//   * 括号组剥离含全角 （）（U+FF08/09）——这是 parse.h 的已知缺口（docs §5.10），
//     候选标题来自各平台、全角括号常见，match 侧自带，不动 parse.h。
//
// 设计取舍（铁律 3 自查）：
//   * 版本 +0.1 奖励只在 title 基分 > 0 时叠加：标题完全不沾时，括号组命中不该给候选续命
//     （错配率 ≤5% 是 P1 验收线，见 docs §4）；
//   * artist 空 → 直接 0.5 中性返回，qFeat 不再加成（中性路径保持中性）；
//   * artist 完全不沾 → 0.0 且 feat 命中不叠加（spec「完全不沾 → 0.0」按字面执行）；
//   * 编辑距离在规范化后的 UTF-8 字节上按单位代价计：CJK 一字差 = 3 字节差 = 3 次代价，
//     两侧同一口径，作为启发式相似度足够；阈值校准见 tests/test_metamatch.cpp 的实测样本；
//   * 规范化丢弃集为显式枚举（ASCII 标点/空白 + 全角/中文标点 + ★☆♪♥ 等装饰符号 + ・ー），
//     不做整段 NFKC（避免把假名半角化等过头折叠）。
//
// ⚠️ 未解决项（铁律 4）：无算法级未解。以本文件 + tests/test_metamatch.cpp 断言集作为
//     回归基线。providers 落地后如发现候选形态超出本文件假设（如全角 "／" 作多艺人分隔），
//     在此扩展，不改判定结构。
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace rta::meta {

// ---------------------------------------------------------------------------
// 常量与数据结构
// ---------------------------------------------------------------------------

// 权重与三档阈值（任务 spec 定值，集中成常量防散落笔误）
inline constexpr double kWeightTitle        = 0.55;
inline constexpr double kWeightArtist       = 0.30;
inline constexpr double kWeightDuration     = 0.15;
inline constexpr double kMatchConfidentTop  = 0.80;  // top1 ≥ 此值且领先 → Confident
inline constexpr double kMatchConfidentLead = 0.10;  // 领先第二名至少这么多
inline constexpr double kMatchWeakFloor     = 0.45;  // top1 < 此值 → Weak（无可信）

// 搜索候选（唯一的统一定义：provider.h 的解析结果直接填本结构，enrich.h 拿它同时
// 走评分与融合。评分只读 title/artist/durationMs/album；其余字段是溯源与取详情/
// 歌词/封面用的句柄，评分不用。2026-10-21 从「暂居本文件的占位形」扩为 provider
// 形超集（原 sourceId 并入 songId/albumId，全项目无人引用 sourceId，实测 grep 为零）。
struct Candidate {
    std::string title;          // 曲名（可含 "(Live)" 等尾括号版本标注，评分前剥离）
    std::string artist;         // 主艺人；多艺人用 " / " 分隔（与 docs §2 smoke 的 QQ/网易形态一致）
    long long   durationMs = 0; // 时长（毫秒）；<=0 视为缺失（duration_score 中性）
    std::string album;          // 专辑名（融合写入用；评分不用）
    std::string source;         // 来源 "qq"/"netease"/"itunes"/"lrclib"…（溯源；评分不用）
    std::string songId;         // 源内歌曲句柄（qq=歌曲级 mid、netease=数字 id 串、itunes=trackId；歌词用）
    std::string albumId;        // 源内专辑句柄（后续专辑页用；评分不用）
    std::string coverUrl;       // 可直接 GET 的封面 URL（下载归 enrich 层；评分不用）
    std::string genre, year;    // 有则填，无则空；year 只留 4 位年份（融合写入用；评分不用）
};

struct Scored {
    size_t      idx = 0;        // 输入 vector<Candidate> 的下标（不存指针，重排安全）
    double      score = 0;      // 总分 = 0.55*title + 0.30*artist + 0.15*dur
    double      titleScore = 0, artistScore = 0, durScore = 0;
    std::string why;            // 人读解释，如 "title:eq+ver artist:in+feat dur:ok:2s"
};

enum class MatchVerdict { Confident, Ambiguous, Weak };

// ---------------------------------------------------------------------------
// 实现细节（不对外的纯函数；mf_ = match detail）
// ---------------------------------------------------------------------------
namespace match_detail {

// ---- UTF-8 解码/编码（自包含，不依赖 windows.h / 第三方） ----

// 解码 s[i] 起的一个码点并前移 i；非法/截断序列按单字节前进并返回 U+FFFD
inline uint32_t mf_decode(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    size_t n;
    uint32_t cp;
    if (c < 0x80)                { ++i; return c; }
    else if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07u; }
    else { ++i; return 0xFFFDu; }                        // 非法首字节
    if (i + n > s.size()) { ++i; return 0xFFFDu; }       // 截断
    for (size_t k = 1; k < n; ++k) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFDu; } // 非法续字节
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    i += n;
    return cp;
}

inline void mf_encode(std::string& o, const uint32_t cp) {
    if (cp < 0x80u) {
        o += (char)cp;
    } else if (cp < 0x800u) {
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

// 多字节「空白/标点/装饰符号」丢弃集。全角标点 （）！？：； 等不走这里——它们在
// U+FF01..FF5E 内，先折叠成 ASCII 再按 ASCII 标点丢弃。ー（U+30FC 长音）是 spec 点名的
// 剥离对象（「（）《》【】・ー 等」）。
inline bool mf_is_drop_cp(const uint32_t cp) {
    if (cp < 0x80) return false;
    switch (cp) {
        case 0x00A0:                                              // NBSP
        case 0x00B7:                                              // ·  中点
        case 0x2010: case 0x2013: case 0x2014:                    // ‐ – —  连接/破折
        case 0x2018: case 0x2019: case 0x201C: case 0x201D:       // ' ' " " 弯引号
        case 0x2026:                                              // …  省略号
        case 0x2605: case 0x2606:                                 // ★ ☆
        case 0x2661: case 0x2665:                                 // ♡ ♥
        case 0x266A: case 0x266B:                                 // ♪ ♫
        case 0x3000:                                              // 　全角空格
        case 0x3001: case 0x3002:                                 // 、 。
        case 0x3008: case 0x3009:                                 // 〈 〉
        case 0x300A: case 0x300B:                                 // 《 》
        case 0x300C: case 0x300D:                                 // 「 」
        case 0x300E: case 0x300F:                                 // 『 』
        case 0x3010: case 0x3011:                                 // 【 】
        case 0x301C:                                              // 〜
        case 0x30FB:                                              // ・ 中点（spec 点名）
        case 0x30FC:                                              // ー 长音（spec 点名）
        case 0xFF61: case 0xFF62: case 0xFF63: case 0xFF64:       // ｡｢｣､ 半角标点
        case 0xFF65:                                              // ･ 半角中点
        case 0xFF70:                                              // ｰ 半角长音
            return true;
        default:
            return false;
    }
}

// 任务 spec 的 norm：小写(ASCII) + 去全部空白（含全角空格/NBSP）+ 去标点（ASCII 与
// 全角（）《》【】・ー 等）+ 全角数字字母（U+FF01..FF5E 全角形式）转半角。
// 输出仍是 UTF-8；非 ASCII 非标点的字符（汉字/假名/谚文…）原样保留。
inline std::string mf_norm(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = mf_decode(s, i);
        if (cp >= 0xFF01u && cp <= 0xFF5Eu) cp -= 0xFEE0u;   // 全角形式 → 半角
        if (cp < 0x80u) {
            char c = (char)cp;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
            continue;                                        // 其余 ASCII（标点/空白/符号）丢弃
        }
        if (mf_is_drop_cp(cp)) continue;
        mf_encode(o, cp);
    }
    return o;
}

// ---- 尾括号组剥离（豁免 + 版本奖励素材） ----

struct mf_bracket_fam { const char* open; const char* close; size_t len; };

// 尾部成对括号组四族：() [] 【】 （）。（）是 parse.h 已知缺口（docs §5.10），match 侧自带
inline const mf_bracket_fam* mf_close_fam(const std::string& s, const size_t end) {
    static const mf_bracket_fam kFams[] = {
        { "(",            ")",            1 },
        { "[",            "]",            1 },
        { "\xe3\x80\x90", "\xe3\x80\x91", 3 },   // 【】
        { "\xef\xbc\x88", "\xef\xbc\x89", 3 },   // （）U+FF08/09
    };
    for (const mf_bracket_fam& f : kFams)
        if (end >= f.len && s.compare(end - f.len, f.len, f.close) == 0) return &f;
    return nullptr;
}

// 尾部空白长度（ASCII 空白 / 全角空格 U+3000 / NBSP）
inline size_t mf_tail_ws(const std::string& s, size_t e) {
    for (;;) {
        while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
        if (e >= 3 && s.compare(e - 3, 3, "\xe3\x80\x80") == 0) { e -= 3; continue; }
        if (e >= 2 && s.compare(e - 2, 2, "\xc2\xa0") == 0)     { e -= 2; continue; }
        return e;
    }
}

// 从尾部连续摘除成对括号组（组间允许空白），组内原文经 *groups 按左→右带出；
// 返回是否至少摘除一组。同族按深度配对，嵌套整组保留（仿 parse.h 的 pd_strip_trailing_groups）。
// 注：UTF-8 自同步保证 ASCII 括号字节不会出现在多字节序列内部，逐字节扫描安全。
inline bool mf_strip_trailing_groups(std::string& s, std::vector<std::string>* groups) {
    bool any = false;
    for (;;) {
        const size_t end = mf_tail_ws(s, s.size());
        if (end == 0) break;
        const mf_bracket_fam* fam = mf_close_fam(s, end);
        if (!fam) break;
        int depth = 1;
        bool found = false;
        size_t op = 0;
        for (size_t i = end - fam->len; i-- > 0;) {
            if (s.compare(i, fam->len, fam->open) == 0) {
                if (--depth == 0) { found = true; op = i; break; }
            } else if (s.compare(i, fam->len, fam->close) == 0) {
                ++depth;
            }
        }
        if (!found || op + fam->len > end - fam->len) break;   // 非配对/防御式上界 → 停
        if (groups) {
            const std::string inner = s.substr(op + fam->len, (end - fam->len) - (op + fam->len));
            if (!inner.empty()) groups->insert(groups->begin(), inner);
        }
        s.resize(op);
        any = true;
    }
    return any;
}

// ---- 编辑距离（单位代价，两行 DP） ----

inline size_t mf_levenshtein(const std::string& a, const std::string& b) {
    if (a.empty()) return b.size();
    if (b.empty()) return a.size();
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0u : 1u;
            size_t m = prev[j] + 1;
            if (cur[j - 1] + 1 < m)        m = cur[j - 1] + 1;
            if (prev[j - 1] + cost < m)    m = prev[j - 1] + cost;
            cur[j] = m;
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

// ---- 分项打分（分数 + 人读标签，供 why） ----

struct mf_tagged { double score = 0; std::string tag; };

inline mf_tagged mf_title(const std::string& qTitle, const std::string& candTitle,
                          const std::string& qVersion) {
    mf_tagged r;
    std::vector<std::string> cg;
    std::string ccore = candTitle;
    mf_strip_trailing_groups(ccore, &cg);     // 候选尾括号组：豁免 + 版本奖励素材
    std::string qcore = qTitle;
    mf_strip_trailing_groups(qcore, nullptr); // 查询侧对称豁免（parse 之后一般已无括号）
    const std::string q = mf_norm(qcore);
    const std::string c = mf_norm(ccore);
    double base;
    if (q.empty() && c.empty())      { base = 0.5;  r.tag = "na"; }  // 双空：无从判，中性
    else if (q.empty() || c.empty()) { base = 0.0;  r.tag = "no"; }
    else if (q == c)                 { base = 1.0;  r.tag = "eq"; }
    else if (q.find(c) != std::string::npos ||
             c.find(q) != std::string::npos) { base = 0.75; r.tag = "sub"; }
    else {
        const size_t d = mf_levenshtein(q, c);
        const size_t m = (q.size() > c.size()) ? q.size() : c.size();
        const double sim = (m == 0) ? 1.0 : 1.0 - (double)d / (double)m;
        if (sim < 0.45) { base = 0.0; r.tag = "no"; }
        else {
            base = sim;
            char b[24];
            std::snprintf(b, sizeof(b), "fz:%.2f", sim);
            r.tag = b;
        }
    }
    // 版本奖励：查询 version 与候选剥出的括号组（规范化）相同 → +0.1，封顶 1.0；
    // 只在基分 > 0 时叠加（标题完全不沾时不给括号组续命，见头注）
    if (base > 0.0) {
        const std::string qv = mf_norm(qVersion);
        if (!qv.empty()) {
            for (const std::string& g : cg) {
                if (mf_norm(g) == qv) {
                    base += 0.1;
                    if (base > 1.0) base = 1.0;
                    r.tag += "+ver";
                    break;
                }
            }
        }
    }
    r.score = base;
    return r;
}

inline mf_tagged mf_artist(const std::string& qArtist, const std::string& qFeat,
                           const std::string& candArtist) {
    mf_tagged r;
    const std::string q = mf_norm(qArtist);
    if (q.empty()) { r.score = 0.5; r.tag = "na"; return r; }  // 中性路径（feat 不叠加）
    const std::string f = mf_norm(qFeat);
    const char kSep[] = " / ";
    double best = 0.0;
    bool featHit = false;
    size_t b = 0;
    for (;;) {
        const size_t cut = candArtist.find(kSep, b);
        const std::string part = candArtist.substr(
            b, (cut == std::string::npos ? candArtist.size() : cut) - b);
        const std::string pn = mf_norm(part);
        if (!pn.empty()) {
            if (pn == q) best = 1.0;                           // 任一相等 = 1.0
            else if (best < 0.8 &&
                     (pn.find(q) != std::string::npos || q.find(pn) != std::string::npos))
                best = 0.8;                                    // 相互包含 = 0.8
            if (!f.empty() &&
                (pn == f || pn.find(f) != std::string::npos || f.find(pn) != std::string::npos))
                featHit = true;                                // feat 命中候选任一 artist
        }
        if (cut == std::string::npos) break;
        b = cut + 3;
    }
    if (best <= 0.0) { r.score = 0.0; r.tag = "no"; return r; } // 完全不沾 → 0（feat 不叠加）
    r.score = best + (featHit ? 0.1 : 0.0);
    if (r.score > 1.0) r.score = 1.0;
    r.tag = (best >= 1.0) ? "eq" : "in";
    if (featHit) r.tag += "+feat";
    return r;
}

inline mf_tagged mf_duration(const int fileSec, const int candMs) {
    mf_tagged r;
    if (fileSec <= 0 || candMs <= 0) { r.score = 0.5; r.tag = "na"; return r; } // 任一侧缺失 → 中性
    long long diff = (long long)fileSec * 1000 - (long long)candMs;
    if (diff < 0) diff = -diff;
    char b[24];
    if (diff <= 3000) {
        std::snprintf(b, sizeof(b), "ok:%llds", (long long)(diff / 1000));
        r.score = 1.0;
    } else if (diff <= 10000) {
        std::snprintf(b, sizeof(b), "near:%llds", (long long)(diff / 1000));
        r.score = 0.6;
    } else {
        r.score = 0.0;
        r.tag = "far";
        return r;
    }
    r.tag = b;
    return r;
}

} // namespace match_detail

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------

// 任务 spec 的 norm（命名加前缀，避免 rta::meta 里出现过于通用的 norm）
inline std::string match_norm(const std::string& s) { return match_detail::mf_norm(s); }

inline double title_score(const std::string& qTitle, const std::string& candTitle,
                          const std::string& qVersion = std::string()) {
    return match_detail::mf_title(qTitle, candTitle, qVersion).score;
}

inline double artist_score(const std::string& qArtist, const std::string& qFeat,
                           const std::string& candArtist) {
    return match_detail::mf_artist(qArtist, qFeat, candArtist).score;
}

inline double duration_score(const int fileSec, const int candMs) {
    return match_detail::mf_duration(fileSec, candMs).score;
}

// 单候选打分（含分项与 why；idx 由 match_score_all 填，单用时保持 0）
inline Scored match_score_detail(const std::string& qTitle, const std::string& qArtist,
                                 const std::string& qFeat, const int fileSec,
                                 const Candidate& cand,
                                 const std::string& qVersion = std::string()) {
    Scored s;
    const match_detail::mf_tagged t = match_detail::mf_title(qTitle, cand.title, qVersion);
    const match_detail::mf_tagged a = match_detail::mf_artist(qArtist, qFeat, cand.artist);
    int cms = 0;                                                  // <=0 保持 0（缺失→中性）
    if (cand.durationMs > (long long)INT32_MAX) cms = INT32_MAX;  // 防溢出：超 int 视为「远」
    else if (cand.durationMs > 0)               cms = (int)cand.durationMs;
    const match_detail::mf_tagged d = match_detail::mf_duration(fileSec, cms);
    s.titleScore = t.score;
    s.artistScore = a.score;
    s.durScore = d.score;
    s.score = kWeightTitle * t.score + kWeightArtist * a.score + kWeightDuration * d.score;
    s.why = "title:" + t.tag + " artist:" + a.tag + " dur:" + d.tag;
    return s;
}

// 任务 spec 的 match_score：0.55*title + 0.30*artist + 0.15*duration
inline double match_score(const std::string& qTitle, const std::string& qArtist,
                          const std::string& qFeat, const int fileSec, const Candidate& cand,
                          const std::string& qVersion = std::string()) {
    return match_score_detail(qTitle, qArtist, qFeat, fileSec, cand, qVersion).score;
}

// 全候选打分并按总分降序排；并列先比 titleScore，再按下标升序（确定性总序）
inline std::vector<Scored> match_score_all(const std::vector<Candidate>& cands,
                                           const std::string& qTitle,
                                           const std::string& qArtist,
                                           const std::string& qFeat, const int fileSec,
                                           const std::string& qVersion = std::string()) {
    std::vector<Scored> v;
    v.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) {
        Scored s = match_score_detail(qTitle, qArtist, qFeat, fileSec, cands[i], qVersion);
        s.idx = i;
        v.push_back(std::move(s));
    }
    std::sort(v.begin(), v.end(), [](const Scored& x, const Scored& y) {
        if (x.score != y.score)           return x.score > y.score;
        if (x.titleScore != y.titleScore) return x.titleScore > y.titleScore;
        return x.idx < y.idx;
    });
    return v;
}

// 按总分降序的下标列表（任务 spec 的输出形态）
inline std::vector<size_t> match_rank(const std::vector<Candidate>& cands,
                                      const std::string& qTitle, const std::string& qArtist,
                                      const std::string& qFeat, const int fileSec,
                                      const std::string& qVersion = std::string()) {
    const std::vector<Scored> v = match_score_all(cands, qTitle, qArtist, qFeat, fileSec, qVersion);
    std::vector<size_t> r;
    r.reserve(v.size());
    for (const Scored& s : v) r.push_back(s.idx);
    return r;
}

// 对 match_score_all 的输出（已按分降序）判三档
inline MatchVerdict match_verdict(const std::vector<Scored>& sortedDesc) {
    if (sortedDesc.empty()) return MatchVerdict::Weak;
    const double top = sortedDesc.front().score;
    if (top < kMatchWeakFloor) return MatchVerdict::Weak;
    // 无第二名（单候选）时「领先」条件视为自然成立
    const bool lead = sortedDesc.size() < 2 ||
                      (top - sortedDesc[1].score) >= kMatchConfidentLead;
    if (top >= kMatchConfidentTop && lead) return MatchVerdict::Confident;
    return MatchVerdict::Ambiguous;
}

inline MatchVerdict match_verdict(const std::vector<Candidate>& cands,
                                  const std::string& qTitle, const std::string& qArtist,
                                  const std::string& qFeat, const int fileSec,
                                  const std::string& qVersion = std::string()) {
    return match_verdict(match_score_all(cands, qTitle, qArtist, qFeat, fileSec, qVersion));
}

// 任务 spec 的 bool 判定：只有 Confident 区返回 true（Ambiguous/Weak 均 false，
// 上层用 match_verdict 区分「歧义区」与「无可信」）
inline bool match_confident(const std::vector<Scored>& sortedDesc) {
    return match_verdict(sortedDesc) == MatchVerdict::Confident;
}

inline bool match_confident(const std::vector<Candidate>& cands,
                            const std::string& qTitle, const std::string& qArtist,
                            const std::string& qFeat, const int fileSec,
                            const std::string& qVersion = std::string()) {
    return match_verdict(cands, qTitle, qArtist, qFeat, fileSec, qVersion) ==
           MatchVerdict::Confident;
}

} // namespace rta::meta
