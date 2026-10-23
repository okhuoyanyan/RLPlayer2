// entities.h — P3 资料层①：实体模型（Person / Group / Label / Series / Work / Recording / Release / Track）
//
// 定案见 docs/元数据补全调研与规划.md §3「实体模型(P3+ 落地,先行定案)」/「内容分级」：
//   * Person 是【角色化】的：composer / conductor / performer / arranger / vocalist / lyricist
//     是 Person 的属性与边的标签，不是独立实体类型；
//   * Group = 乐团 / 社团 / 组合；Label = 厂牌，**一等实体**（不是 Release 上的字符串字段）；
//   * Series = 企划（游戏 / 动画 / 同人原作）；Work = 作品（交响曲 ≡ 流行歌 ≡ 同人 arrange 曲，
//     同一个概念）；Recording = 演绎版本；Release 挂厂牌 / 年份 / 版次；Track = 文件
//     （键是文件路径，可与既有 track hash 体系对应）；
//   * 实体之间的关系一律是【显式边】（结构化 from → rel → to + role），
//     绝不用 "A/B/C" 这种字符串拼接表达关系。
//
// 本文件硬性约束（逐条对应任务书）：
//   1. 实体 id 复用 MetaStore::hashOf（FNV-1a 64，与 net/library.h fnv_hex 同算法同常数），
//      **不另造第二套哈希**。id 输入 = kindTag + 0x1f + 归一化键；0x1f 分隔符沿用
//      library.h 的专辑键约定（"专辑键 = 专辑艺术家 + 0x1f + 专辑名"），避免拼接歧义
//      （"ab"+"c" 与 "a"+"bc" 必须给出不同 id）。
//   2. 落盘 <base>/entities/<kind>/<id>.json（§3 已给 MetaBase 预留 entities/）；
//      原子写 = tmp + rename，与 store.h MetaStore::save / saveCover 同语义
//      （写中途失败绝不破坏旧文件）。
//   3. 不 include windows.h；UTF-8 路径只经 meta_path_of / meta_string_of（同 store.h）。
//   4. 归一化【只用于判等，永不用于展示】：每个实体保留 name（首次写法）+ aliases
//      （全部来源写法，只增不删）。§3「每字段可溯源」是项目底线，alias 丢失=信息丢失。
//   5. 本里程碑不做任何网络调用、不动 DSP/播放/写回（P3① 是纯 schema + 存储 + 解析）。
//
// 头文件库：全部 inline，namespace rta::meta，不用异常（同 store.h / parse.h）。
#pragma once
#include "meta/store.h"          // MetaStore::hashOf / MetaValue / meta_path_of / meta_jesc
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rta::meta {

// store.h 已在本命名空间声明过同一别名；重复声明同一目标合法，这里不重复定义以免歧义。
// （保持 entities.h 可单独 include：store.h 一定在前面。）

// ---------------------------------------------------------------------------
// 码点级工具（归一化用；纯函数，任何字节串都有定义 —— 归一化不能因为畸形 UTF-8 卡死）
// ---------------------------------------------------------------------------
namespace entity_detail {

inline bool ent_is_ascii_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

inline std::string ent_trim_ascii_ws(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && ent_is_ascii_ws(s[b])) ++b;
    while (e > b && ent_is_ascii_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// UTF-8 单向解码：合法序列返回码点；非法/截断序列返回 U+FFFD 并且【只前进 1 字节】。
// 这样无论输入多脏，解码循环都严格前进（否则归一化会死循环）。
inline unsigned ent_u8_next(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; return (unsigned)c; }
    size_t n = 0;
    unsigned cp = 0;
    if ((c & 0xE0) == 0xC0)      { n = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07u; }
    else { ++i; return 0xFFFDu; }
    if (i + n >= s.size()) { ++i; return 0xFFFDu; }         // 截断
    for (size_t j = 1; j <= n; ++j) {
        const unsigned char b = (unsigned char)s[i + j];
        if ((b & 0xC0) != 0x80) { ++i; return 0xFFFDu; }    // 续字节不合法
        cp = (cp << 6) | (unsigned)(b & 0x3Fu);
    }
    i += n + 1;
    return cp;
}

inline void ent_u8_put(std::string& o, unsigned cp) {
    if (cp < 0x80) { o += (char)cp; return; }
    if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
        return;
    }
    if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
        return;
    }
    o += (char)(0xF0 | (cp >> 18));
    o += (char)(0x80 | ((cp >> 12) & 0x3F));
    o += (char)(0x80 | ((cp >> 6) & 0x3F));
    o += (char)(0x80 | (cp & 0x3F));
}

// 空白口径与 parse.h 的 pd_ws_len 一致（ASCII 空白 + U+3000 全角空格 + NBSP）。
inline bool ent_is_space_cp(unsigned cp) {
    if (cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x0B || cp == 0x0C) return true;
    return cp == 0x3000 || cp == 0xA0;
}

// 全半角折叠：U+FF01..U+FF5E 整段平移 0xFEE0 到 ASCII 0x21..0x7E（含全角括号/冒号/字母/
// 数字），U+3000 与 NBSP 折成半角空格。**只折宽度，不做简繁/假名转换**（那是语言学问题，
// 不是确定性的字符串归一化，做错会把不同实体并起来）。
// 反向验证留痕（2026-10-22 实测，铁律 3）：把本函数前三个 if 全部注释掉后重跑 test_entities，
// tests/test_entities.cpp [2] 的「全角字母折成半角」（ＡＢＣ社 vs abc社）与「全角括号+全角
// 冒号的 CV 标注被剥离」两条立刻 FAIL，恢复后全绿 —— 说明这两条断言真的在测全半角折叠，
// 不是在自证。注意 U+3000 那条断言仍 PASS：空白判定另有 ent_is_space_cp，不依赖本函数
// （如实记录：该断言测不到本行，测的是空白口径）。
inline unsigned ent_fold_width(unsigned cp) {
    if (cp >= 0xFF01u && cp <= 0xFF5Eu) return cp - 0xFEE0u;
    if (cp == 0x3000u) return 0x20u;
    if (cp == 0xA0u)   return 0x20u;
    return cp;
}

// 仅 ASCII 大小写折叠（不碰 >=0x80 的字节，长度不变 → 位置与原串一一对应，可安全用于
// 「先用小写串找位置、再切原串」的场景）。与 parse.h pd_lower_ascii 同口径。
inline std::string ent_ascii_lower(const std::string& s) {
    std::string o(s);
    for (size_t i = 0; i < o.size(); ++i)
        if (o[i] >= 'A' && o[i] <= 'Z') o[i] = (char)(o[i] - 'A' + 'a');
    return o;
}

inline bool ent_ascii_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 归一化主干：全半角折叠 → ASCII 小写 → 内部空白压缩成单个半角空格 → 去首尾空白。
// 输出是【判等键】，不用于展示。
inline std::string ent_fold(const std::string& raw) {
    std::vector<unsigned> cps;
    cps.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) cps.push_back(ent_fold_width(ent_u8_next(raw, i)));
    std::string o;
    o.reserve(raw.size());
    bool pendingSpace = false;
    bool any = false;
    for (unsigned cp : cps) {
        if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
        if (ent_is_space_cp(cp)) { if (any) pendingSpace = true; continue; }
        if (pendingSpace) { o += ' '; pendingSpace = false; }
        ent_u8_put(o, cp);
        any = true;
    }
    return o;
}

// 「(CV:xxx)」类配音标注：判等时剥除（同一社团/人物常带 CV 后缀写法）。
// 判据：括号内去空白小写后以 "cv" 开头，且 "cv" 之后不是字母数字（允许 "cv:" / "cv " / "cv"）。
// 注意全角「（ＣＶ：）」经宽度折叠后就是 ASCII 形式，不需要第二套规则。
inline bool ent_is_cv_annotation(const std::string& inner) {
    const std::string t = ent_trim_ascii_ws(ent_ascii_lower(inner));
    if (t.size() < 2 || t[0] != 'c' || t[1] != 'v') return false;
    if (t.size() == 2) return true;
    return !ent_ascii_alnum(t[2]);
}

// 从判等键里剥掉【一个】CV 标注组（尾部优先，其次首部）。返回是否真的剥掉了。
// UTF-8 自同步：')' / ']' / '(' / '[' 都是 ASCII 字节，绝不会出现在多字节序列内部，
// 所以逐字节找括号是安全的（同 parse.h 的做法）。
inline bool ent_strip_one_cv_group(std::string& s) {
    if (s.empty()) return false;
    const char last = s.back();
    if (last == ')' || last == ']') {
        const char open = (last == ')') ? '(' : '[';
        const size_t p = s.rfind(open);
        if (p != std::string::npos && p + 1 <= s.size() - 1) {
            const std::string inner = s.substr(p + 1, s.size() - 1 - (p + 1));
            if (ent_is_cv_annotation(inner)) {
                s = ent_trim_ascii_ws(s.substr(0, p));
                return true;
            }
        }
    }
    if (s[0] == '(' || s[0] == '[') {
        const char close = (s[0] == '(') ? ')' : ']';
        const size_t q = s.find(close, 1);
        if (q != std::string::npos && q > 1) {
            const std::string inner = s.substr(1, q - 1);
            if (ent_is_cv_annotation(inner)) {
                s = ent_trim_ascii_ws(s.substr(q + 1));
                return true;
            }
        }
    }
    return false;
}

} // namespace entity_detail

// ---------------------------------------------------------------------------
// 实体种类
// ---------------------------------------------------------------------------

// 顺序固定（落盘按 kind 子目录，测试按顺序遍历）；新增种类只允许追加，不许改名。
enum class EntityKind { Person = 0, Group, Label, Series, Work, Recording, Release, Track };

inline size_t entity_kind_count() { return 8; }

// kind 的稳定标签（进 id 哈希、进 JSON、进目录名 —— 三处必须一致，改了就换 id）
inline const char* entity_kind_tag(EntityKind k) {
    switch (k) {
        case EntityKind::Person:    return "person";
        case EntityKind::Group:     return "group";
        case EntityKind::Label:     return "label";
        case EntityKind::Series:    return "series";
        case EntityKind::Work:      return "work";
        case EntityKind::Recording: return "recording";
        case EntityKind::Release:   return "release";
        case EntityKind::Track:     return "track";
    }
    return "person";
}

inline bool entity_kind_from_tag(const std::string& tag, EntityKind& out) {
    for (size_t i = 0; i < entity_kind_count(); ++i) {
        const EntityKind k = (EntityKind)i;
        if (tag == entity_kind_tag(k)) { out = k; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 归一化与 id
// ---------------------------------------------------------------------------

// 判等键（纯函数、可离线单测）。同一输入永远同一输出（幂等）。
// ⚠️ 设计取舍：只做「确定性」的字符串归一（大小写 / 全半角 / 空白 / CV 标注）。
// 不做简繁转换、不做假名罗马字化、不剥 "(Live)" "(Remix)" 这类【有语义】的括注 ——
// 剥了会把不同实体错并（错误合并比漏合并更难挽回：alias 里还留着原写法，人能看出来，
// 但 id 已经错了）。这类判断留给后续的人工/LLM 裁决阶段。
inline std::string entity_norm(const std::string& raw) {
    const std::string folded = entity_detail::ent_fold(raw);
    std::string o = folded;
    for (int guard = 0; guard < 8; ++guard)                       // 上限防病态输入
        if (!entity_detail::ent_strip_one_cv_group(o)) break;
    return o.empty() ? folded : o;                                // 整串就是标注 → 不返回空键
}

// 参与 id 的键归一化：
//   * Track 的键是文件路径（原样，见下），另外在实体属性里挂 "trackHash" =
//     MetaStore::hashOf(path) —— 即 <base>/tracks/<hash>.json 的文件名，
//     实体与既有曲目级 MetaBase 因此可以用一个 16 位十六进制串直接 join（任务书要求）。
//   * Track 的键 → **原样**（只去首尾 ASCII 空白）。
//     理由：Windows 路径虽然大小写不敏感，但真实曲库里「（）」「＠」全角字符普遍存在
//     （MUSIC 的「Non Human Music (非人类音乐）Vol.001」就是全角+半角混写）。
//     路径做宽度折叠会把两个不同文件并成同一个 id —— 这是实质数据损坏，不是归一化。
//   * 其余实体是人名/团体名/专辑名 → 走 entity_norm。
inline std::string entity_key_norm(EntityKind kind, const std::string& raw) {
    if (kind == EntityKind::Track) return entity_detail::ent_trim_ascii_ws(raw);
    return entity_norm(raw);
}

// 多段键（Recording = 标题×乐团×指挥×独奏；Release = 专辑×年份）：
// 段间用 0x1f 连接，避免 "ab"+"c" 与 "a"+"bc" 撞键（沿用 library.h 专辑键的分隔约定）。
inline std::string entity_norm_key(EntityKind kind, const std::vector<std::string>& parts) {
    std::string o;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) o += '\x1f';
        o += entity_key_norm(kind, parts[i]);
    }
    return o;
}

// 实体 id = FNV-1a 64 位十六进制（16 位小写）。
// 【复用证明】这里只调用 store.h 的 MetaStore::hashOf，而 store.h 的 hashOf 与
// net/library.h 的 fnv_hex 是同一算法同一常数（initial 1469598103934665603,
// prime 1099511628211）—— 测试 tests/test_entities.cpp 同时用「测试内独立实现的 FNV-1a」
// 和 net::fnv_hex 双向对照，证明这里没有第二套哈希。
inline std::string entity_make_id(const char* kindTag, const std::string& normKey) {
    std::string k(kindTag ? kindTag : "");
    k += '\x1f';
    k += normKey;
    return MetaStore::hashOf(k);
}

inline std::string entity_id(EntityKind kind, const std::string& raw) {
    return entity_make_id(entity_kind_tag(kind), entity_key_norm(kind, raw));
}

inline std::string entity_id_keyed(EntityKind kind, const std::vector<std::string>& parts) {
    return entity_make_id(entity_kind_tag(kind), entity_norm_key(kind, parts));
}

// ---------------------------------------------------------------------------
// 关系（显式边）：词表固定，方向固定
// ---------------------------------------------------------------------------

// 边方向的语义（任务书 §①，不要反过来）：
//   Work --has_recording--> Recording      作品被录成哪些演绎
//   Recording --released_as--> Release     某个演绎发行在哪个 Release
//   Release --on_label--> Label            厂牌（一等实体）
//   Release --in_series--> Series          企划
//   Release --by_group--> Group            同人社团（「专辑 @ 社团」目录名的社）
//   Release --has_track--> Track           这张碟里有哪些文件
//   Track --track_of--> Recording          文件对应哪个演绎（=任务书的 Track→Recording）
//   * --credit(role)--> Person/Group       署名：composer/conductor/performer/vocalist/…
//   流行退化：Artist --release_artist--> Release（跳过 Work 层）
namespace entity_rel {
inline constexpr const char* kCredit        = "credit";
inline constexpr const char* kHasRecording   = "has_recording";
inline constexpr const char* kReleasedAs     = "released_as";
inline constexpr const char* kOnLabel        = "on_label";
inline constexpr const char* kInSeries       = "in_series";
inline constexpr const char* kByGroup        = "by_group";
inline constexpr const char* kHasTrack       = "has_track";
inline constexpr const char* kTrackOf        = "track_of";
inline constexpr const char* kReleaseArtist  = "release_artist";
} // namespace entity_rel

// 边的合法 (rel, from-kind, to-kind) 三元组表：既是文档也是测试判据
// （tests/test_entities.cpp 按这张表逐条校验方向，防止「Work/Recording 写反」这类 bug）。
struct EntityEdgeRule {
    const char* rel;
    EntityKind  from;
    EntityKind  to;
};
inline const std::vector<EntityEdgeRule>& entity_edge_rules() {
    using K = EntityKind;
    static const std::vector<EntityEdgeRule> rules = {
        { entity_rel::kCredit,        K::Work,      K::Person  },
        { entity_rel::kCredit,        K::Recording, K::Person  },
        { entity_rel::kCredit,        K::Recording, K::Group   },
        { entity_rel::kCredit,        K::Release,   K::Person  },
        { entity_rel::kHasRecording,  K::Work,      K::Recording },
        { entity_rel::kReleasedAs,    K::Recording, K::Release },
        { entity_rel::kOnLabel,       K::Release,   K::Label   },
        { entity_rel::kInSeries,      K::Release,   K::Series  },
        { entity_rel::kByGroup,       K::Release,   K::Group   },
        { entity_rel::kHasTrack,      K::Release,   K::Track   },
        { entity_rel::kTrackOf,       K::Track,     K::Recording },
        { entity_rel::kReleaseArtist, K::Person,    K::Release },
        { entity_rel::kReleaseArtist, K::Group,     K::Release },
    };
    return rules;
}

// ---------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------

struct EntityEdge {
    std::string from;      // Entity::id
    std::string to;        // Entity::id
    std::string rel;       // entity_rel::*（固定词表）
    std::string role;      // credit 边的角色标签（composer/conductor/performer/…），其它边留空
    std::string source;    // 溯源："tag" / "dir" / "file" / "user" / 事实源名
    long long   ts = 0;

    // 去重键：同一 (from,rel,to,role) 只存一条（重跑抽取不会让边重复增长）
    std::string key() const { return from + '\x1f' + rel + '\x1f' + to + '\x1f' + role; }
};

struct Entity {
    std::string id;        // 16 位小写十六进制（FNV-1a 64，见 entity_make_id）
    EntityKind  kind = EntityKind::Person;
    std::string name;      // 展示名 = 首次写入的原始写法（归一化结果永不用于展示）
    std::string normKey;   // 判等键（多段用 0x1f 连接），落盘便于人看差异
    std::vector<std::string> aliases;               // 全部来源写法（只增不删）
    std::map<std::string, MetaValue> attrs;         // 属性：与 store.h 字段同结构（带溯源）
    std::vector<EntityEdge> edges;                  // 出边（与实体同文件，读回即得邻接）
    long long createdTs = 0;
    long long updatedTs = 0;

    // 追加来源写法。返回是否新增。**绝不删改已有 alias**。
    bool addAlias(const std::string& raw) {
        if (raw.empty()) return false;
        for (const auto& a : aliases)
            if (a == raw) return false;
        aliases.push_back(raw);
        return true;
    }
    const EntityEdge* findEdge(const std::string& rel, const std::string& to,
                               const std::string& role) const {
        for (const auto& e : edges)
            if (e.rel == rel && e.to == to && e.role == role) return &e;
        return nullptr;
    }
};

struct EntityGraph {
    std::map<std::string, Entity> entities;         // id → 实体（std::map：遍历顺序确定）
    const Entity* find(const std::string& id) const {
        const auto it = entities.find(id);
        return it == entities.end() ? nullptr : &it->second;
    }
    std::vector<EntityEdge> allEdges() const {
        std::vector<EntityEdge> o;
        for (const auto& kv : entities)
            for (const auto& e : kv.second.edges) o.push_back(e);
        return o;
    }
    size_t count(EntityKind k) const {
        size_t n = 0;
        for (const auto& kv : entities) if (kv.second.kind == k) ++n;
        return n;
    }
};

// 属性合并直接复用 store.h 的 MetaStore::mergeField（字段级合并策略的唯一事实源：
// user(100) > tag(90) > 事实源(50) > llm(10)）。这里借 TrackMeta 承载一次，
// 不复制那套优先级/置信度规则 —— 否则两处规则迟早漂移。
inline void entity_merge_attr(std::map<std::string, MetaValue>& attrs, const std::string& key,
                              const MetaValue& v) {
    TrackMeta tm;
    tm.fields = attrs;
    MetaStore::mergeField(tm, key, v);
    attrs = std::move(tm.fields);
}

// ---------------------------------------------------------------------------
// JSON 读写（人可读；落盘格式即协议）
// ---------------------------------------------------------------------------
//
// {"id":"…","kind":"person","name":"…","norm":"…","createdTs":1,"updatedTs":2,
//  "aliases":["…","…"],
//  "attrs":{"role":{"value":"composer","source":"tag","ts":1,"confidence":1}},
//  "edges":[{"from":"…","to":"…","rel":"has_recording","role":"","source":"tag","ts":1}]}

inline std::string entity_json(const Entity& e) {
    std::ostringstream o;
    o << "{\"id\":\"" << meta_jesc(e.id)
      << "\",\"kind\":\"" << meta_jesc(entity_kind_tag(e.kind))
      << "\",\"name\":\"" << meta_jesc(e.name)
      << "\",\"norm\":\"" << meta_jesc(e.normKey)
      << "\",\"createdTs\":" << e.createdTs
      << ",\"updatedTs\":" << e.updatedTs
      << ",\"aliases\":[";
    for (size_t i = 0; i < e.aliases.size(); ++i) {
        if (i) o << ',';
        o << '"' << meta_jesc(e.aliases[i]) << '"';
    }
    o << "],\"attrs\":{";
    bool first = true;
    for (const auto& kv : e.attrs) {
        if (!first) o << ',';
        first = false;
        const float conf = std::isfinite(kv.second.confidence) ? kv.second.confidence : 0.f;
        char cb[32];
        std::snprintf(cb, sizeof(cb), "%.4g", (double)conf);   // NaN/inf 会让 JSON 非法
        o << '"' << meta_jesc(kv.first)
          << "\":{\"value\":\""  << meta_jesc(kv.second.value)
          << "\",\"source\":\""  << meta_jesc(kv.second.source)
          << "\",\"ts\":"        << kv.second.ts
          << ",\"confidence\":"  << cb << "}";
    }
    o << "},\"edges\":[";
    for (size_t i = 0; i < e.edges.size(); ++i) {
        if (i) o << ',';
        const EntityEdge& ed = e.edges[i];
        o << "{\"from\":\""   << meta_jesc(ed.from)
          << "\",\"to\":\""   << meta_jesc(ed.to)
          << "\",\"rel\":\""  << meta_jesc(ed.rel)
          << "\",\"role\":\"" << meta_jesc(ed.role)
          << "\",\"source\":\"" << meta_jesc(ed.source)
          << "\",\"ts\":"     << ed.ts << "}";
    }
    o << "]}";
    return o.str();
}

// 解析落盘格式。判据：缺 id / kind 不认识 / aliases 不是数组 / edges 不是数组 → false
//（"损坏"必须显式失败，不能默默返回半截实体 —— 同 store.h load 的口径）。
inline bool entity_from_json(const std::string& text, Entity& out) {
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) return false;
    Entity e;
    e.id = j["id"].s();
    if (e.id.empty()) return false;
    if (!entity_kind_from_tag(j["kind"].s(), e.kind)) return false;
    if (j["aliases"].type != rta::net::Json::Arr) return false;
    if (j["edges"].type != rta::net::Json::Arr) return false;
    e.name        = j["name"].s();
    e.normKey     = j["norm"].s();
    e.createdTs   = (long long)j["createdTs"].n();
    e.updatedTs   = (long long)j["updatedTs"].n();
    for (size_t i = 0; i < j["aliases"].size(); ++i) e.addAlias(j["aliases"].at(i).s());
    const rta::net::Json& at = j["attrs"];
    if (at.type == rta::net::Json::Obj) {
        for (const auto& kv : at.obj) {
            if (kv.second.type != rta::net::Json::Obj) continue;
            MetaValue v;
            v.value      = kv.second["value"].s();
            v.source     = kv.second["source"].s();
            v.ts         = (long long)kv.second["ts"].n();
            v.confidence = (float)kv.second["confidence"].n();
            e.attrs[kv.first] = std::move(v);
        }
    }
    for (size_t i = 0; i < j["edges"].size(); ++i) {
        const rta::net::Json& ej = j["edges"].at(i);
        EntityEdge ed;
        ed.from   = ej["from"].s();
        ed.to     = ej["to"].s();
        ed.rel    = ej["rel"].s();
        ed.role   = ej["role"].s();
        ed.source = ej["source"].s();
        ed.ts     = (long long)ej["ts"].n();
        if (ed.from.empty() || ed.to.empty() || ed.rel.empty()) continue;   // 单条坏边不毁整实体
        e.edges.push_back(std::move(ed));
    }
    out = std::move(e);
    return true;
}

// ---------------------------------------------------------------------------
// 原子写（tmp + rename）
// ---------------------------------------------------------------------------

// 与 store.h MetaStore::save / saveCover 完全同语义：
//   ① 先写 <file>.tmp；② 写成功才 rename 覆盖；③ 写失败直接返回 false，旧文件保持原样。
// 为什么不能直接覆盖写：写到一半崩溃会留下截断的 JSON，整个实体（含全部 alias）就没了。
inline bool entity_write_atomic(const fsx::path& file, const std::string& bytes) {
    std::error_code ec;
    const fsx::path dir = file.parent_path();
    if (!dir.empty()) fsx::create_directories(dir, ec);
    fsx::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;                       // 打不开临时文件：不动旧文件
        f.write(bytes.data(), (std::streamsize)bytes.size());
        if (!f) return false;                       // 写失败：不动旧文件
    }
    fsx::rename(tmp, file, ec);                     // 同盘 rename，原子生效
    if (ec) { fsx::remove(tmp, ec); return false; }
    return true;
}

inline bool entity_read_file(const fsx::path& file, std::string& out) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.empty()) return false;
    out = std::move(s);
    return true;
}

// ---------------------------------------------------------------------------
// EntityStore：<base>/entities/<kind>/<id>.json（单例模式照 MetaStore）
// ---------------------------------------------------------------------------

class EntityStore {
public:
    static EntityStore& instance() { static EntityStore S; return S; }

    // 默认 "rlplayer-meta"（与 MetaStore 同一 MetaBase 根）；测试切到临时目录
    void setBaseDir(const std::string& dir) {
        std::lock_guard<std::mutex> lk(mtx_);
        baseDir_ = dir.empty() ? "rlplayer-meta" : dir;
    }
    std::string baseDir() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return baseDir_;
    }

    fsx::path entitiesDir() const { return meta_path_of(baseDir()) / "entities"; }
    fsx::path kindDir(EntityKind k) const { return entitiesDir() / entity_kind_tag(k); }
    fsx::path pathOf(EntityKind k, const std::string& id) const {
        return kindDir(k) / (id + ".json");
    }

    // 覆盖写（原子）。返回是否真的落盘。
    bool save(const Entity& e) {
        if (e.id.empty()) return false;
        return entity_write_atomic(pathOf(e.kind, e.id), entity_json(e));
    }

    // 读回；文件不存在/损坏 → false（out 保持不变）。
    bool loadAt(EntityKind k, const std::string& id, Entity& out) const {
        std::string s;
        if (!entity_read_file(pathOf(k, id), s)) return false;
        Entity e;
        if (!entity_from_json(s, e)) return false;
        if (e.id != id) return false;                   // id 对不上 = 文件放错目录/被改坏
        out = std::move(e);
        return true;
    }

    // 只给 id（kind 未定时）→ 依次查 8 个 kind 目录（8 次 exists，代价可忽略，不需要额外索引）。
    bool load(const std::string& id, Entity& out) const {
        for (size_t i = 0; i < entity_kind_count(); ++i)
            if (loadAt((EntityKind)i, id, out)) return true;
        return false;
    }

    // 读旧 → 合并（alias 只增不删、属性按 mergeField 策略、边按 key 去重）→ 原子写回。
    // 这是「同一实体在几百首里反复出现」的落盘入口：重跑不会丢来源写法，也不会让边重复。
    bool upsert(Entity& e) {
        Entity old;
        if (loadAt(e.kind, e.id, old)) {
            e.createdTs = old.createdTs != 0 ? old.createdTs : e.createdTs;
            for (const auto& a : old.aliases) e.addAlias(a);
            for (const auto& kv : old.attrs) entity_merge_attr(e.attrs, kv.first, kv.second);
            for (const auto& ed : old.edges)
                if (!e.findEdge(ed.rel, ed.to, ed.role)) e.edges.push_back(ed);
            if (e.updatedTs < old.updatedTs) e.updatedTs = old.updatedTs;
        }
        return save(e);
    }

    // 整张图落盘（每实体一个文件）
    size_t saveGraph(const EntityGraph& g) {
        size_t n = 0;
        for (const auto& kv : g.entities) {
            Entity e = kv.second;
            if (upsert(e)) ++n;
        }
        return n;
    }

    std::vector<std::string> listIds(EntityKind k) const {
        std::vector<std::string> o;
        std::error_code ec;
        const fsx::path dir = kindDir(k);
        if (!fsx::exists(dir, ec)) return o;
        for (const auto& de : fsx::directory_iterator(dir, ec)) {
            if (!de.is_regular_file(ec)) continue;
            const std::string name = meta_string_of(de.path().filename());
            if (name.size() <= 5) continue;
            if (name.compare(name.size() - 5, 5, ".json") != 0) continue;   // 跳过残留 .tmp
            o.push_back(name.substr(0, name.size() - 5));
        }
        return o;
    }

private:
    EntityStore() = default;
    mutable std::mutex mtx_;
    std::string baseDir_ = "rlplayer-meta";
};

// ---------------------------------------------------------------------------
// 古典 / 合作标签扩读（大小写不敏感）
// ---------------------------------------------------------------------------

struct ClassicalField {
    std::string value;   // 标签值（原样，不归一化）
    std::string key;     // 命中的原始标签键（溯源用：COMPOSER / composer / 作曲 …）
};

// 规范字段名 → 接受的标签键（全部小写比较）。别名表是**唯一事实源**：
// 新增来源键只在这里加一行。
//   * 英文键：常见于 flac/mp3 的手工标签与 ffprobe 归一后的键；
//   * 中文键：本库真实样本大量使用「作曲 / 作词 / 编曲」写法；
//   * publisher：writeback.h 写回时把 label 映射到 publisher（实测 m4a/ID3 家族口径），
//     读回时它必须能反查成 Label，否则写回→重读会丢厂牌。
inline std::string entity_classical_canon(const std::string& keyLower) {
    static const std::map<std::string, std::string> kMap = {
        // 角色
        { "composer",         "composer" }, { "music",            "composer" },
        { "作曲",             "composer" }, { "music_by",         "composer" },
        { "conductor",        "conductor" }, { "指挥",            "conductor" },
        { "performer",        "performer" }, { "soloist",         "performer" },
        { "演奏",             "performer" }, { "演奏者",          "performer" },
        { "ensemble",         "ensemble"  }, { "orchestra",       "ensemble"  },
        { "乐团",             "ensemble"  }, { "演奏团体",        "ensemble"  },
        { "choir",            "ensemble"  }, { "chorus",          "ensemble"  },
        { "lyricist",         "lyricist"  }, { "作词",            "lyricist"  },
        { "lyrics_by",        "lyricist"  },
        { "vocalist",         "vocalist"  }, { "vocal",           "vocalist"  },
        { "演唱",             "vocalist"  },
        { "arranger",         "arranger"  }, { "编曲",            "arranger"  },
        { "arrangement",      "arranger"  },
        // 古典结构
        { "work",             "work"      }, { "work_title",      "work"      },
        { "作品",             "work"      }, { "曲目",            "work"      },
        { "opus",             "opus"      }, { "op",              "opus"      },
        { "作品号",           "opus"      },
        { "catalog",          "catalog"   }, { "catalogue",       "catalog"   },
        { "catalog_number",   "catalog"   }, { "catno",           "catalog"   },
        { "编号",             "catalog"   },
        { "part",             "part"      }, { "parts",           "part"      },
        { "段",               "part"      },
        { "movement",         "movement"  }, { "movements",       "movement"  },
        { "乐章",             "movement"  },
        // 厂牌 / 企划
        { "label",            "label"     }, { "publisher",       "label"     },
        { "厂牌",             "label"     }, { "发行",            "label"     },
        { "series",           "series"    }, { "企划",            "series"    },
        { "原作",             "series"    },
    };
    const auto it = kMap.find(keyLower);
    return it == kMap.end() ? std::string() : it->second;
}

// 读一组标签（键大小写不敏感，值去首尾 ASCII 空白）。同一规范字段多个标签键命中时，
// 取 **键名字典序最小** 的那个 —— 排序确定的，不是「先遇到者胜」（后者会随
// std::map 之外的数据结构变化而漂移）。空值不覆盖已有值。
// 返回 map 的 value.key 是「命中的原始标签键」—— 归一字段与来源键的对应关系
// 因此完整可查（§3 溯源底线：能回答「这个 composer 是从哪个标签读来的」）。
inline std::map<std::string, ClassicalField> entity_read_classical(
        const std::map<std::string, std::string>& tags) {
    std::map<std::string, ClassicalField> out;
    for (const auto& kv : tags) {
        const std::string canon = entity_classical_canon(entity_detail::ent_ascii_lower(kv.first));
        if (canon.empty()) continue;
        const std::string v = entity_detail::ent_trim_ascii_ws(kv.second);
        if (v.empty()) continue;
        if (out.count(canon)) continue;                       // 键名字典序最小者胜
        out[canon] = ClassicalField{ v, kv.first };
    }
    return out;
}

inline std::string entity_classical_get(const std::map<std::string, ClassicalField>& cf,
                                        const std::string& canon) {
    const auto it = cf.find(canon);
    return it == cf.end() ? std::string() : it->second.value;
}

// ---------------------------------------------------------------------------
// 标题里的古典线索（真实样本兜底：TEST 站的古典文件没有 OPUS 标签，作品号只在标题里）
// ---------------------------------------------------------------------------

// 从标题抠作品号：认 "Op. 326" / "Op.326" / "Op 326" / "opus 326"（大小写不敏感）。
// 成因：MUSIC 真实样本「动态（高电平）- Magic Bullets Fast Polka, Op. 326.wav」ffprobe
// 实测只有 album/artist/title/disc/ID/LYRICS 六个键，**没有 OPUS 标签**（2026-10-22 只读核验）。
// 返回规范展示形态 "Op. 326"；找不到返回 ""。
inline std::string entity_parse_opus(const std::string& text) {
    const std::string low = entity_detail::ent_ascii_lower(text);
    for (size_t i = 0; i < low.size(); ++i) {
        if (i != 0 && entity_detail::ent_ascii_alnum(low[i - 1])) continue;   // 词边界
        size_t p = 0;
        if (low.compare(i, 4, "opus") == 0)      p = i + 4;
        else if (low.compare(i, 2, "op") == 0)   p = i + 2;
        else continue;
        while (p < low.size() && (low[p] == '.' || low[p] == ' ' || low[p] == '\t')) ++p;
        if (p >= low.size() || low[p] < '0' || low[p] > '9') continue;        // "open" 不误判
        size_t q = p;
        while (q < low.size() && low[q] >= '0' && low[q] <= '9' && q - p < 4) ++q;
        return "Op. " + text.substr(p, q - p);
    }
    return "";
}

// 从标题抠乐章号：认「II.」「III)」这类罗马数字，要求
//   ① 位于串首或 ':','-','/',' ','(','.' 之后（防止 "Violin" 里的 "i" 命中）；
//   ② 长度 2..4 且紧跟 '.' 或 ')'（"Live" 的 "liv" 后面是 'e'，不中）；
// 返回大写罗马数字（"Ma Vlast: II. Vltava" → "II"）；找不到返回 ""。
inline bool entity_is_roman_char(char c) {
    return c == 'i' || c == 'v' || c == 'x' || c == 'l' || c == 'c' || c == 'd' || c == 'm';
}

inline std::string entity_parse_movement(const std::string& text) {
    const std::string low = entity_detail::ent_ascii_lower(text);
    for (size_t i = 0; i < low.size(); ++i) {
        if (i != 0) {
            const char p = low[i - 1];
            if (!(p == ':' || p == '-' || p == '/' || p == ' ' || p == '(' || p == '.')) continue;
        }
        if (!entity_is_roman_char(low[i])) continue;
        size_t j = i;
        while (j < low.size() && entity_is_roman_char(low[j])) ++j;
        const size_t len = j - i;
        if (len < 2 || len > 4) continue;
        if (j >= low.size() || (low[j] != '.' && low[j] != ')')) continue;
        std::string o = text.substr(i, len);
        for (char& c : o)
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        return o;
    }
    return "";
}

// ---------------------------------------------------------------------------
// 名称启发（确定性白名单，不做统计猜测；每一条都写明依据与已知识别风险）
// ---------------------------------------------------------------------------

// 厂牌判定：名含「唱片」或以 Records / Record 结尾（大小写不敏感）。
// 依据：本库真实样本「塞壬唱片-MSR」（MUSIC\专辑-感动我的&RainLain\2021，ffprobe ARTIST）。
// ⚠️ 已知风险（铁律 4）：这是启发式 —— 把「合辑/发行品牌」都当厂牌可能误判（例如
// 「某某社」不是厂牌）。误判只影响 facet 视图，用户手改可覆盖；不当作已验证事实。
inline bool entity_looks_label(const std::string& raw) {
    const std::string n = entity_norm(raw);
    if (n.empty()) return false;
    if (n.find("唱片") != std::string::npos) return true;
    if (n.size() >= 7 && n.compare(n.size() - 7, 7, "records") == 0) return true;
    if (n.size() >= 6 && n.compare(n.size() - 6, 6, "record") == 0) return true;
    return false;
}

// 厂牌兼企划（Label×Series 双挂）：§3 明写本库样本「塞壬唱片-MSR = Label×Series」。
// 判定与 entity_looks_label 同源（唱片公司自有企划序列），两个实体 id 不同（kind 进哈希），
// 但共享同一原始写法（各自 alias 都保留原始写法）。
inline bool entity_label_also_series(const std::string& raw) {
    return entity_looks_label(raw);
}

// 艺人名的实体种类启发：乐团/合唱/乐队/组合/社/団 或 Orchestra/Ensemble/Choir/Band/
// Quartet/Trio/Singers/Philharmonic/Symphony → Group；否则 Person（个人/同人作者）。
// 依据：真实样本「Czech National Symphony Orchestra」(ffprobe ARTIST, TEST 站交响诗)
// 是乐团；「澤田なつ」是个人/同人（§3 原文）。⚠️ 同样是启发式：像「团子」这类昵称会被
// 误判成 Group；单字「社」「团」「団」更宽（「社会科学」这类名字也会中）。误判只影响
// 分类展示，但**kind 进 id，所以种类判错会得到另一个 id** —— 这是已知取舍：宁可把同一
// 实体拆成两条（人能看见、能手工合并），也不要错并（错并会悄悄丢掉一条的来源写法）。
inline EntityKind entity_artist_kind(const std::string& raw) {
    const std::string n = entity_norm(raw);
    static const char* kWord[] = { "orchestra", "ensemble", "choir", "chorus", "band",
                                   "quartet", "trio", "singers", "philharmonic", "symphony",
                                   "group", "project" };
    for (const char* w : kWord)
        if (n.find(w) != std::string::npos) return EntityKind::Group;
    static const char* kCjk[] = { "乐团", "樂團", "乐队", "樂隊", "合唱", "组合", "組合",
                                  "社团", "社團", "社", "団", "团" };
    for (const char* w : kCjk)
        if (raw.find(w) != std::string::npos) return EntityKind::Group;
    return EntityKind::Person;
}

// ---------------------------------------------------------------------------
// 年份清洗（Release 判等键专用；纯函数）
// ---------------------------------------------------------------------------

// 从任意标签串里取【4 位年份】。取不到（或无合理年份）返回 ""。
//   "2022-08-16" → "2022"；"20030731" → "2003"（紧凑日期形态）；"1996" → "1996"（不误伤）；
//   "0000" / "" / "abc" / "999" / "99999" / "-1" → ""（垃圾一律不猜，宁缺毋滥）。
// 为什么需要它：Release 的判等键是「专辑 × 年份」，而 s.year 来自 year/date 标签的**原样串**
// （真实库里就是 "2022-08-16"）。整段进键会把同一张碟按发行日期的月/日差异拆成两个 Release
// 实体 —— 静默数据分裂，不报错，最难发现（实测见 tests/test_entities.cpp [11]）。
// 判据与 src/meta/provider.h 的 pv_year4 是**同一个合理窗口 [1000,2100]**：
// P2 曾发生 QQ 返回 "0000-00-00"、取出 "0000" 被采纳写回、把 DATE 2022-12-24 改成 0000 的事故，
// 窗口外年份必须在源头丢弃（写回侧另有第二道守卫 writeback_year_ok）。
//
// ⚠️ 刻意重复一份而不 include provider.h（方案 (a)，理由）：
//   ① entities.h 是纯 schema / 存储头，provider.h 会拖进 net/http_client.h（winsock2）+ match.h，
//      schema 层依赖网络栈是倒退，也会让这个头在无网络组件的地方编不动；
//   ② 用例不同：pv_year4 产出的是【候选年份】（宽进），本函数产出的是【判等键的一段】（严出）——
//      见下面"唯一差异"。共用一个函数就得塞开关，判据反而不再单一。
//   ⇒ 代价是两处窗口数字可能漂移。改动任一处窗口/形态时，请同时看 provider.h 的 pv_year4
//     与 tests/test_metaproviders.cpp 的窗口断言（那边 1000/2100/0000/0999/2101 都钉住了）。
//
// 与 pv_year4 的**唯一**差异（2026-10-22 实测对照 25 个输入，只有 1 个不同）：
//   数字串长度既不是 4 也不是 8 时，本函数整段跳过；pv_year4 对任何 ≥4 位的数字串一律截前 4 位。
//   实测差异样本："20221" → pv_year4="2022"，entity_year4=""（其余 24 个输入两者一致，
//   含 "99999"：那边截出 9999 但**窗口外**，同样回 ""；"0000" 两边都回 ""）。
// 这里更保守的原因：5/6/7/9+ 位的纯数字串在标签里是垃圾或曲目序号，截前 4 位得到的"年份"
// 是伪造的 —— 它进了 Release 判等键就会把一张真专辑与另一堆垃圾年份的曲目并到一起
// （错误合并比漏合并更难挽回）。宁可回 ""（退化为"无日期版"，见下面的刻意取舍）。
// 8 位（YYYYMMDD）是真实存在的紧凑日期形态，仍然接受。
inline std::string entity_year4(const std::string& raw) {
    const size_t n = raw.size();
    for (size_t i = 0; i < n;) {
        if (raw[i] < '0' || raw[i] > '9') { ++i; continue; }
        size_t j = i;
        while (j < n && raw[j] >= '0' && raw[j] <= '9') ++j;
        const size_t len = j - i;
        if (len == 4 || len == 8) {
            const std::string y = raw.substr(i, 4);
            const int v = (y[0] - '0') * 1000 + (y[1] - '0') * 100 +
                          (y[2] - '0') * 10 + (y[3] - '0');
            // 首个候选决定结果：不在其后继续找第二个数字段，否则曲目序号/时长会被当成年代
            return (v >= 1000 && v <= 2100) ? y : std::string();
        }
        i = j;                                          // 长度不合形态 → 跳过整段继续扫
    }
    return "";
}

// ---------------------------------------------------------------------------
// 输入种子 + 「专辑 @ 社团」目录名解析
// ---------------------------------------------------------------------------

struct MetaSeed {
    std::string trackPath;                 // Track 实体键（原样）
    std::string title, artist, albumArtist, album, year;
    std::string circle;                    // 同人社团（来自「专辑 @ 社团」目录名）
    std::string series;                    // 企划（显式）
    std::string label;                     // 厂牌（显式）
    std::map<std::string, std::string> tags;   // 其余标签（键大小写不敏感；古典字段在这里）
};

// 拆「专辑 @ 社团」目录名。本库真实形态（MUSIC\专辑-原创&同人 下 40+ 个目录）：
//   "2 Pixels @ 月華社" → album="2 Pixels", circle="月華社"
//   "Utapia -Extra- @ Haru.jpg" → album="Utapia -Extra-", circle="Haru.jpg"
//   "Non Human Music (非人类音乐）Vol.001" → 没有 @ → album=整串, circle 空
// 分隔符认 ASCII '@' 与全角 '＠'(U+FF20 = EF BC A0)。
inline void entity_parse_album_circle(const std::string& dirName, std::string& album,
                                      std::string& circle) {
    album.clear();
    circle.clear();
    const std::string s = entity_detail::ent_trim_ascii_ws(dirName);
    size_t at = std::string::npos;
    size_t atLen = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '@') { at = i; atLen = 1; break; }
        if (i + 3 <= s.size() && s.compare(i, 3, "\xef\xbc\xa0") == 0) { at = i; atLen = 3; break; }
    }
    if (at == std::string::npos) { album = s; return; }
    album  = entity_detail::ent_trim_ascii_ws(s.substr(0, at));
    circle = entity_detail::ent_trim_ascii_ws(s.substr(at + atLen));
}

// ---------------------------------------------------------------------------
// 目录结构判据：专辑目录 vs 分类目录（P3 批量入口专用；只读）
// ---------------------------------------------------------------------------
//
// 真实库结构只有两层：MUSIC/<分类目录>/<专辑目录>/<曲目>，但**两层的形状并不总是一样**：
//   MUSIC\专辑-原创&同人\A Cup of Fragrance @ 月華社\01. xx.flac   ← 分类下是专辑
//   MUSIC\TEST站测评曲\解析 - 雪琥珀.wav                          ← 这一层自己就是专辑目录
// 若批量入口无脑「父目录 = 专辑名」，遇到真正的分类层就会得到 "专辑-原创&同人" 这种名字，
// Release 名退化成分类名（富页「这张专辑有哪些曲」会变成「这个分类下有什么」，且跨专辑错并）。
//
// 判据用【结构】而不是名字白名单（"专辑-…" / "TEST站测评曲" 这种名字换台机器/换用户就失效，
// 还会把用户自建的 "专辑-单曲收集" 误当分类）：
//   该目录下**直接**有音频文件 → 专辑目录；只含子目录、无直接音频 → 分类目录；
//   两者都没有（空目录）→ 既不是专辑也不是分类（未知，宁可留空）。
// 这条判据在真实库上的实测覆盖率见 tests/test_entities.cpp [12]（77 个同人专辑目录全部命中）。
//
// ⚠️ 已知边界（铁律 4，2026-10-22 实测）：
//   ① 多碟盒装形态（专辑目录里只有 CD1/CD2 子目录、音频全在子目录）：本判据会把它判成
//      分类目录 → 上层应继续下钻一层（那时 CD1 才是「直接有音频」的那层）。本库当前**没有**
//      这种形态（实测 87 个叶子目录里 80 个直接带音频，剩下 7 个见 ②），所以没为它加
//      "多碟聚合"逻辑。遇到时是漏判（专辑名回退成曲名），不是错判（不会写进分类名）。
//   ② 实测这 7 个"叶子目录无直接音频"的样本：6 个是专辑目录里的图片子目录
//      （"Scans"/"Scan"，只有 jpg），1 个是**空目录** VOL1（0 个条目）。
//      它们既无音频也无子目录 → is_album=false 且 is_category=false，落在"未知"桶里
//      → entity_album_dir_name 返回 ""，不会产生专辑名，也不会被当分类继续下钻。
//      这正是"三态（专辑/分类/未知）而不是二态"的理由。
inline bool entity_audio_ext_ok(const std::string& extWithDot) {
    std::string e = entity_detail::ent_ascii_lower(extWithDot);
    if (e.empty()) return false;
    if (e[0] != '.') e.insert(e.begin(), '.');
    // 与 net/library.h lib_is_audio 同一张扩展名表（**刻意重复**：entities.h 不 include
    // library.h —— 那会经由 dsp 头拉进 windows.h，而本文件第 3 条硬性约束就是「不 include
    // windows.h」。两边都只认后缀，判据本身没有第二种口径；新增格式请同时改两处。）
    static const char* kExt[] = { ".wav", ".wave", ".flac", ".m4a", ".alac", ".mp3", ".aac",
                                  ".ogg", ".opus", ".wma", ".ape", ".wv", ".dsf", ".dff",
                                  ".aif", ".aiff", ".aifc", ".caf", ".mka", ".mpc" };
    for (const char* x : kExt) if (e == x) return true;
    return false;
}

struct EntityDirScan {
    size_t audioFiles = 0;    // 直接位于本目录的音频文件数（不含子目录里的）
    size_t subdirs    = 0;    // 直接子目录数
    size_t otherFiles = 0;    // 封面/cue/log/压缩包等非音频文件（不影响判定，仅供诊断）
    bool   readable   = false; // 目录存在且能列出来
};

// 只读扫描一层（不递归）。全部用 error_code 重载：非 UTF-8 名字、权限不足、路径不存在都不抛异常
// （同 store.h 的口径 —— 遍历失败就是 readable=false，由调用方当"未知"处理，绝不猜）。
inline EntityDirScan entity_scan_dir(const fsx::path& dir) {
    EntityDirScan s;
    std::error_code ec;
    if (!fsx::is_directory(dir, ec)) return s;
    fsx::directory_iterator it(dir, ec), end;
    if (ec) return s;
    s.readable = true;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fsx::directory_entry& de = *it;
        std::error_code ec2;
        if (de.is_directory(ec2)) { ++s.subdirs; continue; }
        if (!de.is_regular_file(ec2)) continue;
        // 扩展名走 meta_string_of（UTF-8）→ ent_ascii_lower；不用窄 fopen/ANSI API，
        // 中文目录名（本库绝大多数）在 Windows 上才不会因代码页而找不到
        if (entity_audio_ext_ok(meta_string_of(de.path().extension()))) ++s.audioFiles;
        else ++s.otherFiles;
    }
    return s;
}

// 多碟盒装子目录判定：CD1..CDn / Disc1..n / Disk1..n（大小写不敏感，允许前导空格/连字符）
inline bool entity_is_disc_subdir_name(const std::string& name, int* discOut = nullptr) {
    std::string s = entity_detail::ent_ascii_lower(entity_detail::ent_trim_ascii_ws(name));
    if (s.empty()) return false;
    size_t pos = 0;
    if (s.rfind("cd", 0) == 0) {
        pos = 2;
    } else if (s.rfind("disc", 0) == 0 || s.rfind("disk", 0) == 0) {
        pos = 4;
    } else {
        return false;
    }
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '-' || s[pos] == '_' || s[pos] == '.')) {
        ++pos;
    }
    if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return false;
    int num = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        num = num * 10 + (s[pos] - '0');
        if (num > 9999) return false;
        ++pos;
    }
    if (pos < s.size()) {
        char c = s[pos];
        if (c != ' ' && c != '-' && c != '_' && c != ':' && c != '(' && c != '[' && c != '.') {
            return false;
        }
    }
    if (discOut) *discOut = num;
    return num > 0;
}

// 判定一个目录是否是多碟盒装专辑目录：
// 自身不要求有直接音频，但其直接子目录中【所有含音频的子目录】全部呈现 CD1..CDn / Disc1..n 碟片形态（至少 1 个碟片子目录）。
inline bool entity_dir_is_boxset(const fsx::path& dir, std::vector<int>* discsOut = nullptr) {
    std::error_code ec;
    if (!fsx::is_directory(dir, ec) || ec) return false;
    size_t discSubdirs = 0;
    size_t audioSubdirs = 0;
    std::vector<int> discs;

    for (fsx::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const fsx::path subPath = it->path();
        const std::string subName = meta_string_of(subPath.filename());
        int dNo = 0;
        const bool isDiscName = entity_is_disc_subdir_name(subName, &dNo);
        const EntityDirScan s = entity_scan_dir(subPath);
        if (s.audioFiles > 0) {
            ++audioSubdirs;
            if (isDiscName) {
                ++discSubdirs;
                if (dNo > 0) discs.push_back(dNo);
            }
        }
    }
    if (audioSubdirs > 0 && discSubdirs == audioSubdirs) {
        if (discsOut) {
            std::sort(discs.begin(), discs.end());
            discs.erase(std::unique(discs.begin(), discs.end()), discs.end());
            *discsOut = std::move(discs);
        }
        return true;
    }
    return false;
}

// 判定某个目录是否是多碟盒装的碟片子目录（例如 MyBoxSet/CD1）
inline bool entity_dir_is_disc_subdir(const fsx::path& dir, int* discOut = nullptr) {
    const std::string name = meta_string_of(dir.filename());
    if (!entity_is_disc_subdir_name(name, discOut)) return false;
    const fsx::path parent = dir.parent_path();
    return entity_dir_is_boxset(parent);
}

// 专辑目录 = ① 直接含音频文件的目录（普通单碟专辑，且自身不是盒装下的 CD1/CD2 碟片子目录）；
//           ② 或者自身为多碟盒装专辑目录（音频位于 CD1..CDn 子目录中）。
inline bool entity_dir_is_album(const fsx::path& dir) {
    if (entity_dir_is_disc_subdir(dir)) return false;
    const EntityDirScan s = entity_scan_dir(dir);
    if (s.audioFiles > 0) return true;
    return entity_dir_is_boxset(dir);
}

// 分类目录 = 自己无直接音频、有子目录、且不是多碟盒装也不是碟片子目录。
inline bool entity_dir_is_category(const fsx::path& dir) {
    if (entity_dir_is_disc_subdir(dir)) return false;
    if (entity_dir_is_boxset(dir)) return false;
    const EntityDirScan s = entity_scan_dir(dir);
    return s.readable && s.audioFiles == 0 && s.subdirs > 0;
}

// 给定曲目路径，查找其所属的真实专辑目录路径（处理单碟与多碟盒装），并可导出推断的碟号
inline fsx::path entity_find_album_dir_for_track(const fsx::path& trackPath, int* discOut = nullptr) {
    std::error_code ec;
    fsx::path p = trackPath.parent_path();
    if (p.empty()) return fsx::path();
    // 1. 若父目录是盒装的碟片子目录（MyBoxSet/CD1），其真正的专辑目录是其上层盒装目录
    if (entity_dir_is_disc_subdir(p, discOut)) {
        return p.parent_path();
    }
    // 2. 若父目录本身就是专辑目录（普通单碟）
    if (entity_dir_is_album(p)) {
        if (discOut) *discOut = 1;
        return p;
    }
    return fsx::path();
}

// 给定一个目录，若它是**专辑目录**则返回它的目录名（UTF-8 原样，交给
// entity_parse_album_circle 继续拆「专辑 @ 社团」）；否则返回 ""。
// ★ 识别不到真实专辑目录时【宁可留空】：调用方拿到 "" 就不要设 album，
//   让 entity_graph_from_seed 走「专辑空则退化为曲名」的既有兜底 —— 绝不把分类名当专辑名写进
//   Release 判等键（写进去之后 id 就固定错了，后面所有曲目都会并到那个假专辑上）。
inline std::string entity_album_dir_name(const fsx::path& dir) {
    if (!entity_dir_is_album(dir)) return "";
    return meta_string_of(dir.filename());
}

// 原始标签 map → 种子。键**大小写不敏感**（ffprobe 的 format_tags 对同一语义可能给
// "ALBUM"/"album"，Vorbis 家族大写、ID3 家族小写 —— 本库两种都有，见 writeback.h 头注）。
// 这样 ffprobe 输出、TrackMeta、以及将来任何来源都能走同一条入口，不需要为每个来源
// 写一套字段映射（那是既有 provider/parse 层的活，本文件只负责 schema）。
// 各字段取「键名字典序最小」的那个值，规则与 entity_read_classical 一致（确定性）。
inline MetaSeed entity_seed_from_tags(const std::string& trackPath,
                                     const std::map<std::string, std::string>& tags) {
    // 规范名 → 接受的键（全小写）
    struct Pick { const char* field; const char* const* keys; size_t n; };
    static const char* kTitle[]  = { "title" };
    static const char* kArtist[] = { "artist" };
    static const char* kAlbum[]  = { "album" };
    static const char* kAlbumArtist[] = { "albumartist", "album_artist", "album artist" };
    static const char* kYear[]   = { "year", "date" };
    static const char* kLabel[]  = { "label", "publisher" };
    static const Pick kPicks[] = {
        { "title",       kTitle,       1 },
        { "artist",      kArtist,      1 },
        { "album",       kAlbum,       1 },
        { "albumArtist", kAlbumArtist, 3 },
        { "year",        kYear,        2 },
        { "label",       kLabel,       2 },
    };
    MetaSeed s;
    s.trackPath = trackPath;
    for (const auto& kv : tags) {                 // std::map 按 key 字典序遍历（确定性）
        const std::string low = entity_detail::ent_ascii_lower(kv.first);
        const std::string v = entity_detail::ent_trim_ascii_ws(kv.second);
        if (v.empty()) continue;
        s.tags[kv.first] = v;                     // 全量留存（古典字段由 entity_read_classical 读）
        for (const Pick& p : kPicks) {
            bool hit = false;
            for (size_t i = 0; i < p.n; ++i) if (low == p.keys[i]) { hit = true; break; }
            if (!hit) continue;
            // 同一字段多种键都命中时保留先到的（= 字典序最小），不被后者覆盖
            const std::string field = p.field;
            std::string& slot = (field == "title")       ? s.title
                              : (field == "artist")      ? s.artist
                              : (field == "album")       ? s.album
                              : (field == "albumArtist") ? s.albumArtist
                              : (field == "year")        ? s.year
                                                         : s.label;
            if (slot.empty()) slot = v;
        }
    }
    return s;
}

// 从曲目路径与原始标签提取种子，并接入目录结构判据（多碟盒装/「专辑 @ 社团」目录名/碟号推断）
inline MetaSeed entity_seed_from_track_path(const std::string& trackPath,
                                            const std::map<std::string, std::string>& rawTags = {}) {
    MetaSeed s = entity_seed_from_tags(trackPath, rawTags);
    int discFromDir = 0;
    const fsx::path albDir = entity_find_album_dir_for_track(meta_path_of(trackPath), &discFromDir);
    if (!albDir.empty()) {
        const std::string dn = meta_string_of(albDir.filename());
        std::string a, c;
        entity_parse_album_circle(dn, a, c);
        if (s.album.empty() && !a.empty()) s.album = a;
        if (s.circle.empty() && !c.empty()) s.circle = c;
        if (discFromDir > 0) {
            auto it = s.tags.find("disc");
            if (it == s.tags.end()) it = s.tags.find("discNo");
            if (it == s.tags.end()) it = s.tags.find("discno");
            if (it == s.tags.end() || it->second.empty() || it->second == "0") {
                s.tags["disc"] = std::to_string(discFromDir);
                s.tags["discNo"] = std::to_string(discFromDir);
            }
        }
    }
    return s;
}

// store.h 的 TrackMeta → 种子（12 个常规字段原样透传成 tags，另把常用字段提到明面上）
// 实现委托给 entity_seed_from_track_path：字段映射只有一份，且自动接线目录结构判据。
inline MetaSeed entity_seed_from_track(const TrackMeta& tm) {
    std::map<std::string, std::string> flat;
    for (const auto& kv : tm.fields)
        if (!kv.second.value.empty()) flat[kv.first] = kv.second.value;
    return entity_seed_from_track_path(tm.path, flat);
}

// ---------------------------------------------------------------------------
// EntityBuilder：纯内存建图（不碰磁盘，便于离线单测）
// ---------------------------------------------------------------------------

class EntityBuilder {
public:
    explicit EntityBuilder(long long ts) : ts_(ts) {}

    // 单键实体（Person/Group/Label/Series/Track）。返回 id。
    std::string add(EntityKind kind, const std::string& raw, const std::string& source) {
        return addKeyed(kind, std::vector<std::string>{ raw }, raw, source);
    }

    // 复合键实体（Work = 作品名×作曲家；Recording = 标题×乐团×指挥×独奏；Release = 专辑×年份）。
    // display 是展示名（人类可读），parts 是判等键材料 —— 两者分开，避免「展示改个写法就换 id」。
    std::string addKeyed(EntityKind kind, const std::vector<std::string>& parts,
                         const std::string& display, const std::string& source) {
        const std::string normKey = entity_norm_key(kind, parts);
        const std::string id = entity_make_id(entity_kind_tag(kind), normKey);
        auto it = g_.entities.find(id);
        if (it == g_.entities.end()) {
            Entity e;
            e.id = id;
            e.kind = kind;
            e.name = display;
            e.normKey = normKey;
            e.createdTs = ts_;
            e.updatedTs = ts_;
            e.addAlias(display);   // 首次写法即 alias（§3 每字段可溯源：原始写法绝不丢）
            it = g_.entities.emplace(id, std::move(e)).first;
        } else {
            // 同一 id 的其它写法：全部保留（归一化只用于判等，display 仍是首次写法）
            it->second.addAlias(display);
            it->second.updatedTs = ts_;
        }
        // 记录该实体首次出现的来源。confidence=0 且 rank 为「其它(0)」→ 任何真实来源
        // (tag/事实源)都能覆盖它，所以这行只是兜底痕迹，不会挡住后续更可信的来源。
        attr(id, "source", source, source, 0.0f);
        return id;
    }

    // 属性（带溯源）。合并策略复用 store.h mergeField（见 entity_merge_attr）。
    void attr(const std::string& id, const std::string& key, const std::string& value,
              const std::string& source, float confidence = 1.0f) {
        if (value.empty() || key.empty()) return;
        const auto it = g_.entities.find(id);
        if (it == g_.entities.end()) return;
        MetaValue v;
        v.value = value;
        v.source = source;
        v.ts = ts_;
        v.confidence = confidence;
        entity_merge_attr(it->second.attrs, key, v);
        it->second.updatedTs = ts_;
    }

    // 显式边（同一 (from,rel,to,role) 去重）
    void link(const std::string& from, const char* rel, const std::string& to,
              const std::string& role, const std::string& source) {
        if (from.empty() || to.empty() || !rel || !*rel) return;
        const auto it = g_.entities.find(from);
        if (it == g_.entities.end()) return;
        if (it->second.findEdge(rel, to, role)) return;
        EntityEdge e;
        e.from = from;
        e.to = to;
        e.rel = rel;
        e.role = role;
        e.source = source;
        e.ts = ts_;
        it->second.edges.push_back(std::move(e));
        it->second.updatedTs = ts_;
    }

    // 合并另一张实体图（去重、alias 追加、属性按 mergeField 策略、边去重）
    void merge(const EntityGraph& other) {
        for (const auto& kv : other.entities) {
            const Entity& src = kv.second;
            auto it = g_.entities.find(src.id);
            if (it == g_.entities.end()) {
                g_.entities[src.id] = src;
                it = g_.entities.find(src.id);
                if (it->second.createdTs == 0) it->second.createdTs = ts_;
                it->second.updatedTs = ts_;
            } else {
                for (const auto& a : src.aliases) it->second.addAlias(a);
                for (const auto& akv : src.attrs) entity_merge_attr(it->second.attrs, akv.first, akv.second);
                for (const auto& ed : src.edges) {
                    if (!it->second.findEdge(ed.rel, ed.to, ed.role)) {
                        it->second.edges.push_back(ed);
                    }
                }
                if (ts_ > it->second.updatedTs) it->second.updatedTs = ts_;
            }
        }
    }

    const EntityGraph& graph() const { return g_; }
    EntityGraph take() { return std::move(g_); }

private:
    long long ts_;
    EntityGraph g_;
};

// ---------------------------------------------------------------------------
// 标签 → 实体图（里程碑① 的核心：古典四层 / 流行退化 / 同人双挂）
// ---------------------------------------------------------------------------

// 古典四层：Composer --credit--> Work --has_recording--> Recording(乐团×指挥×独奏)
//           --released_as--> Release --on_label--> Label
// 流行退化：Artist（Person/Group）--release_artist--> Release --has_track--> Track
//           --track_of--> Recording（跳过 Work 层）
// 同人：Release --by_group--> Group(社团)；厂牌兼企划时同一写法同时生成 Label 与 Series。
//
// 判据「是否走古典路径」= COMPOSER 非空。理由：没有作曲家的「作品」只是曲名，
// 硬造 Work 层会让流行曲也长出空壳 Work（噪声）。有 COMPOSER 才说明这是古典/艺术音乐语境。
inline EntityGraph entity_graph_from_seed(const MetaSeed& s, long long ts) {
    EntityBuilder b(ts);
    const std::map<std::string, ClassicalField> cf = entity_read_classical(s.tags);

    // ---- 厂牌 / 企划 / 社团 ----
    std::string labelName = entity_classical_get(cf, "label");
    if (labelName.empty()) labelName = s.label;
    std::string seriesName = entity_classical_get(cf, "series");
    if (seriesName.empty()) seriesName = s.series;
    // 厂牌型艺人名（真实样本「塞壬唱片-MSR」）：它本身就是 Label，不是艺人 Person
    if (labelName.empty() && entity_looks_label(s.artist)) labelName = s.artist;
    if (seriesName.empty() && entity_label_also_series(labelName)) seriesName = labelName;
    const bool artistIsLabel = entity_looks_label(s.artist) && !s.artist.empty();

    // ---- 古典标签（大小写不敏感；空则留空）----
    const std::string composer  = entity_classical_get(cf, "composer");
    std::string conductor       = entity_classical_get(cf, "conductor");
    std::string performer       = entity_classical_get(cf, "performer");
    std::string ensemble        = entity_classical_get(cf, "ensemble");
    const std::string lyricist  = entity_classical_get(cf, "lyricist");
    const std::string vocalist  = entity_classical_get(cf, "vocalist");
    const std::string arranger  = entity_classical_get(cf, "arranger");
    const bool classical = !composer.empty();

    // 古典样本常常只有 ARTIST（TEST 站「Czech National Symphony Orchestra」），
    // 而 artist 被判成 Group → 当作乐团（ensemble），否则会丢掉「乐团×指挥」这一层。
    if (classical && ensemble.empty() && !s.artist.empty() &&
        entity_artist_kind(s.artist) == EntityKind::Group) {
        ensemble = s.artist;
    }

    // 作品号 / 乐章 / 分轨：标签优先，标题兜底（真实样本只有标题里带 Op. 326 / II.）
    std::string opus = entity_classical_get(cf, "opus");
    if (opus.empty()) opus = entity_parse_opus(s.title);
    const std::string catalog = entity_classical_get(cf, "catalog");
    std::string movement = entity_classical_get(cf, "movement");
    if (movement.empty()) movement = entity_parse_movement(s.title);
    const std::string part = entity_classical_get(cf, "part");

    // ---- Release（专辑 × 年份；年份空也是合法键的一段）----
    // ★ 判等键里的年份段只用 entity_year4 清洗过的【4 位年】，完整原始串进属性 date。
    // 理由：s.year 是 year/date 标签的**原样串**（真实库里就是 "2022-08-16"）。整段进键意味
    // 「同一张碟里两首曲子的发行日期差一天 → 两个 Release 实体」，实测本库 85 张 ≥2 轨专辑里
    // Swallow's Lies 就是这么被拆成 2 个 Release 的（同一目录、同一 album 标签）。
    // 发行日期按月/日是常态差异（同日多版 / 区域差异 / 单曲加曲单独标注），
    // 不该影响「这是同一张专辑」的判定 —— 那属于版次差异，用属性表达，不用判等键表达。
    const std::string releaseName = s.album.empty() ? s.title : s.album;
    const std::string releaseYear = entity_year4(s.year);
    const std::string releaseId = b.addKeyed(EntityKind::Release,
                                             { releaseName, releaseYear }, releaseName, "tag");
    // 原始日期串（可能精确到月/日）原样留档；判等键丢掉的信息必须能在属性里找回来，否则
    // 「同碟收敛」的代价就是**信息丢失**而不是取舍。
    // 注意这里**不做清洗**（连 "0000" 也照实存）：属性是溯源记录，不是判等材料 ——
    // 把垃圾值改写成空串反而抹掉了「源数据本来是脏的」这条信息。它不进键，所以不产生幽灵版次；
    // year 属性则由清洗结果给出（垃圾 → 空，即"没有可信年份"）。两者口径不同是刻意的。
    if (!s.year.empty())  b.attr(releaseId, "date", s.year, "tag");       // 原始串：不清洗
    if (!releaseYear.empty()) b.attr(releaseId, "year", releaseYear, "tag"); // 4 位年：与判等键一致
    if (!s.albumArtist.empty()) b.attr(releaseId, "albumArtist", s.albumArtist, "tag");
    //
    // ⚠️ 刻意取舍（不是疏忽）：空 year 与有 year 的同名专辑**不合并**（键里空段 vs "2022"）。
    // 无日期版 vs 有日期版可能是**不同版次**（首版/再版/会场限定），合并会丢掉版次信息，
    // 而错误合并比漏合并更难挽回（alias 里留有原写法，但 id 已经错了 —— 同 entity_norm 的取舍）。
    // 代价：同一张碟若部分曲目带日期、部分不带，会分裂成两个 Release。这是已知且被接受的风险，
    // 由 P3 的「候选合并」环节（人工/LLM 裁决）兜底，不在这里静默合并。

    // ---- Recording ----
    std::string recordingId;
    if (classical) {
        const std::string workName = entity_classical_get(cf, "work").empty()
                                   ? s.title : entity_classical_get(cf, "work");
        const std::string workId = b.addKeyed(EntityKind::Work,
                                              { workName, composer }, workName, "tag");
        if (!opus.empty())     b.attr(workId, "opus", opus, "tag");
        if (!catalog.empty())  b.attr(workId, "catalog", catalog, "tag");
        const std::string composerId = b.add(EntityKind::Person, composer, "tag");
        b.attr(composerId, "role", "composer", "tag");
        // 古典四层第一层：Composer → Work（role=composer，Person 角色化，不另立类型）
        b.link(workId, entity_rel::kCredit, composerId, "composer", "tag");

        // Recording 的判等键 = 标题×乐团×指挥×独奏（四段恒定，缺项留空）
        recordingId = b.addKeyed(EntityKind::Recording,
                                 { s.title, ensemble, conductor, performer }, s.title, "tag");
        b.link(workId, entity_rel::kHasRecording, recordingId, "", "tag");
        if (!movement.empty()) b.attr(recordingId, "movement", movement, "tag");
        if (!part.empty())     b.attr(recordingId, "part", part, "tag");
        if (!conductor.empty()) {
            const std::string pid = b.add(EntityKind::Person, conductor, "tag");
            b.attr(pid, "role", "conductor", "tag");
            b.link(recordingId, entity_rel::kCredit, pid, "conductor", "tag");
        }
        if (!performer.empty()) {
            const std::string pid = b.add(EntityKind::Person, performer, "tag");
            b.attr(pid, "role", "performer", "tag");
            b.link(recordingId, entity_rel::kCredit, pid, "performer", "tag");
        }
        if (!ensemble.empty()) {
            const std::string gid = b.add(EntityKind::Group, ensemble, "tag");
            b.attr(gid, "role", "ensemble", "tag");
            b.link(recordingId, entity_rel::kCredit, gid, "ensemble", "tag");
        }
    } else {
        // 流行退化：Artist → Release → Track（再 Track → Recording）
        if (!s.artist.empty() && !artistIsLabel) {
            const EntityKind ak = entity_artist_kind(s.artist);
            const std::string artistId = b.add(ak, s.artist, "tag");
            b.attr(artistId, "role", "artist", "tag");
            b.link(artistId, entity_rel::kReleaseArtist, releaseId, "", "tag");
        }
        // 流行 Recording 键 = 标题×艺人（古典是四段，两套形状不同但各自确定）
        recordingId = b.addKeyed(EntityKind::Recording, { s.title, s.artist }, s.title, "tag");
        if (!vocalist.empty()) {
            const std::string pid = b.add(EntityKind::Person, vocalist, "tag");
            b.attr(pid, "role", "vocalist", "tag");
            b.link(recordingId, entity_rel::kCredit, pid, "vocalist", "tag");
        }
        if (!lyricist.empty()) {
            const std::string pid = b.add(EntityKind::Person, lyricist, "tag");
            b.attr(pid, "role", "lyricist", "tag");
            b.link(recordingId, entity_rel::kCredit, pid, "lyricist", "tag");
        }
        if (!arranger.empty()) {
            const std::string pid = b.add(EntityKind::Person, arranger, "tag");
            b.attr(pid, "role", "arranger", "tag");
            b.link(recordingId, entity_rel::kCredit, pid, "arranger", "tag");
        }
    }

    // ---- Recording → Release（两种路径都要，任务书明写 Recording→Release）----
    b.link(recordingId, entity_rel::kReleasedAs, releaseId, "", "tag");

    // ---- Track（文件）→ Recording（任务书：Track→Recording，方向不可反）----
    const std::string trackId = b.addKeyed(EntityKind::Track,
                                           { s.trackPath },
                                           s.trackPath.empty() ? std::string("(no path)") : s.trackPath,
                                           "file");
    if (!s.trackPath.empty()) {
        b.attr(trackId, "path", s.trackPath, "file");
        // 与既有曲目级 MetaBase 的连接键：store.h 的 <base>/tracks/<hash>.json 用的就是它
        // （MetaStore::hashOf == net/library.h fnv_hex，同一算法同一常数，全项目一致）。
        b.attr(trackId, "trackHash", MetaStore::hashOf(s.trackPath), "file");
    }
    // 写入 Track 上的 discNo / trackNo 属性（供多碟判据与排序溯源）
    for (const auto& k : { "discNo", "disc", "discno", "DISCNUMBER" }) {
        auto it = s.tags.find(k);
        if (it != s.tags.end() && !it->second.empty()) {
            b.attr(trackId, "discNo", it->second, "tag");
            break;
        }
    }
    for (const auto& k : { "trackNo", "track", "trackno", "TRACKNUMBER" }) {
        auto it = s.tags.find(k);
        if (it != s.tags.end() && !it->second.empty()) {
            b.attr(trackId, "trackNo", it->second, "tag");
            break;
        }
    }
    b.link(trackId, entity_rel::kTrackOf, recordingId, "", "file");
    b.link(releaseId, entity_rel::kHasTrack, trackId, "", "file");

    // ---- 厂牌 / 企划 / 社团 ----
    if (!labelName.empty()) {
        const std::string labelId = b.add(EntityKind::Label, labelName, "tag");
        b.attr(labelId, "role", "label", "tag");
        b.link(releaseId, entity_rel::kOnLabel, labelId, "", "tag");
    }
    if (!seriesName.empty()) {
        const std::string seriesId = b.add(EntityKind::Series, seriesName, "tag");
        b.attr(seriesId, "role", "series", "tag");
        b.link(releaseId, entity_rel::kInSeries, seriesId, "", "tag");
    }
    if (!s.circle.empty()) {
        const std::string gid = b.add(EntityKind::Group, s.circle, "dir");
        b.attr(gid, "role", "circle", "dir");
        b.link(releaseId, entity_rel::kByGroup, gid, "", "dir");
    }
    return b.take();
}

// ---------------------------------------------------------------------------
// 真实批量入口：从音乐库扫描结果 / 种子列表构建实体图
// ---------------------------------------------------------------------------

// 辅助：获取某个 Release 实体下关联的碟片数
inline size_t entity_release_disc_count(const EntityGraph& g, const std::string& releaseId) {
    const Entity* rel = g.find(releaseId);
    if (!rel || rel->kind != EntityKind::Release) return 0;
    auto itDiscs = rel->attrs.find("discCount");
    if (itDiscs == rel->attrs.end()) itDiscs = rel->attrs.find("discs");
    if (itDiscs != rel->attrs.end()) {
        int n = std::atoi(itDiscs->second.value.c_str());
        if (n > 0) return (size_t)n;
    }
    std::set<int> discs;
    for (const auto& e : rel->edges) {
        if (e.rel == entity_rel::kHasTrack) {
            const Entity* tr = g.find(e.to);
            if (tr) {
                auto itD = tr->attrs.find("discNo");
                if (itD == tr->attrs.end()) itD = tr->attrs.find("disc");
                if (itD != tr->attrs.end()) {
                    int d = std::atoi(itD->second.value.c_str());
                    if (d > 0) discs.insert(d);
                }
            }
        }
    }
    return discs.empty() ? 1 : discs.size();
}

// 批量入口核心：将多个种子聚合构建为统一实体图（多碟盒装、同专辑不同曲目均收敛到同一 Release）
inline EntityGraph entity_batch_build_graph(const std::vector<MetaSeed>& seeds, long long ts = 0) {
    EntityBuilder b(ts);
    std::map<std::string, std::set<int>> releaseDiscs;

    for (const auto& s : seeds) {
        EntityGraph gSeed = entity_graph_from_seed(s, ts);
        std::string releaseId;
        for (const auto& kv : gSeed.entities) {
            if (kv.second.kind == EntityKind::Release) {
                releaseId = kv.second.id;
                break;
            }
        }
        int dNo = 0;
        for (const auto& k : { "discNo", "disc", "discno", "DISCNUMBER" }) {
            auto it = s.tags.find(k);
            if (it != s.tags.end() && !it->second.empty()) {
                dNo = std::atoi(it->second.c_str());
                if (dNo > 0) break;
            }
        }
        if (dNo > 0 && !releaseId.empty()) {
            releaseDiscs[releaseId].insert(dNo);
        }
        b.merge(gSeed);
    }

    for (const auto& kv : releaseDiscs) {
        if (!kv.second.empty()) {
            const std::string dCountStr = std::to_string(kv.second.size());
            b.attr(kv.first, "discCount", dCountStr, "dir");
            b.attr(kv.first, "discs", dCountStr, "dir");
        }
    }

    return b.take();
}

// 从文件路径列表批量构建（自动应用目录结构判据与多碟聚合）
inline EntityGraph entity_batch_from_paths(const std::vector<std::string>& filePaths, long long ts = 0) {
    std::vector<MetaSeed> seeds;
    seeds.reserve(filePaths.size());
    for (const auto& p : filePaths) {
        seeds.push_back(entity_seed_from_track_path(p));
    }
    return entity_batch_build_graph(seeds, ts);
}

// 从 TrackMeta 列表批量构建
inline EntityGraph entity_batch_from_tracks(const std::vector<TrackMeta>& tracks, long long ts = 0) {
    std::vector<MetaSeed> seeds;
    seeds.reserve(tracks.size());
    for (const auto& tm : tracks) {
        seeds.push_back(entity_seed_from_track(tm));
    }
    return entity_batch_build_graph(seeds, ts);
}

} // namespace rta::meta
