// parse.h — 文件名/标签解析器（纯规则，无网络、无设备、无 Windows 头）
//
// 元数据补全流水线的第一级（docs/元数据补全调研与规划.md §3）：
//   ① parse（本文件）  标签优先透传 → 文件名规则拆分 → LLM 回包解析与提示词生成
//   ② providers / ③ match / ④ fuse / ⑤ store …（后续阶段）
//
// 三个入口 + 一个提示词生成：
//   parse_from_tags(artist, title)  标签透传（都不空 = 可信，conf 1.0；否则 0.5）
//   parse_filename(stem)            文件名规则拆分（曲号 / " - " 切分 / version / feat）
//   parse_llm_parse(llmText)        解析 deepseek-flash 的回包（剥 ```json 围栏后取四键 JSON）
//   parse_llm_prompt(stem)          生成给 deepseek-flash 的提示词（只要求输出四键 JSON）
//
// 实现说明（风格对齐 src/net/library.h 的字段清洗思路）：
//   * 头文件库，全部 inline；namespace rta::meta；不用异常；无第三方依赖
//   * JSON 解析只用 src/net/json.h（rta::net::json_parse）
//   * JSON 字符串转义在本文件独立实现一份小函数 esc_json（对照 tools/rtaweb.cpp 的 jesc，
//     但不依赖 tools/；\r 按本项目约定丢弃）
//   * 输入一律按 UTF-8 字节串处理。UTF-8 自同步：ASCII 字节（括号、数字、空格）
//     不可能出现在多字节序列内部，因此逐字节扫描【】这类三字节括号是安全的
#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>
#include "net/json.h"

namespace rta::meta {

struct ParsedName {
    std::string artist, title, feat, version;
    std::string method;      // "tag" | "rule" | "llm"
    float confidence = 0.f;  // 规则解析的启发置信度 0~1
};

// ---------------------------------------------------------------------------
// 实现细节（不对外的纯函数；pd_ = parse detail）
// ---------------------------------------------------------------------------
namespace parse_detail {

inline bool pd_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
inline bool pd_isdigit(char c) { return c >= '0' && c <= '9'; }

// 单个「空白字符」的字节长度：ASCII 空白为 1；另认全角空格 U+3000（日文文件名
// 常用它当空格）与 NBSP U+00A0。返回 0 表示该位置不是空白。
inline size_t pd_ws_len(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    const unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') return 1;
    if (c == 0xE3 && i + 3 <= s.size() && s.compare(i, 3, "\xe3\x80\x80") == 0) return 3; // U+3000
    if (c == 0xC2 && i + 2 <= s.size() && s.compare(i, 2, "\xc2\xa0") == 0) return 2;    // NBSP
    return 0;
}

// 去首尾空白（含 U+3000 / NBSP）
inline std::string pd_trim(const std::string& s) {
    size_t b = 0;
    for (;;) { const size_t w = pd_ws_len(s, b); if (w == 0) break; b += w; }
    size_t e = s.size();
    for (;;) {
        while (e > b && pd_isspace(s[e - 1])) --e;
        bool ate = false;
        if (e >= 3 && s.compare(e - 3, 3, "\xe3\x80\x80") == 0) { e -= 3; ate = true; } // U+3000
        else if (e >= 2 && s.compare(e - 2, 2, "\xc2\xa0") == 0) { e -= 2; ate = true; } // NBSP
        if (!ate) break;
    }
    return s.substr(b, e - b);
}

// 缺陷 2 修复（2026-10-21）：仅当标准 " - " 未命中时使用的回退切分点查找。
// 从左到右找第一个连字符 '-'，同时满足：
//   * 前一个字符是闭括号：ASCII 的 ')' 或 ']'，或全角闭括号的三字节序列（连字符
//     紧贴其结尾字节，如 "（甜系女声）- "）；
//   * 后一个字符是空白（ASCII 空白或全角空格 U+3000；NBSP 一并按空白认，与
//     pd_ws_len 的空白口径一致）。
// 返回连字符下标；找不到返回 npos。
// ⚠️ 字节口径说明：规格原文「全角 ）(E3 80 89)、】(E3 80 91)、」(E3 80 97)」中
// 字形与字节不一致——）(U+FF09) 实为 EF BC 89、E3 80 89 实为 〉(U+3009)；
// 」(U+300D) 实为 E3 80 8D、E3 80 97 实为 】(U+3017 白色方头)。必过样例
// "人声音质（甜系女声）- 恋文" 里的是 U+FF09，按字节面值实现永远切不开。
// 故此处取并集：三个字形按真实字节 + 规格列出的三组字节，全部识别（实测 2026-10-21）。
// 注：'-' 是 ASCII 字节，UTF-8 自同步保证它不会出现在多字节序列内部，逐字节扫描
// 安全；三字节闭括号的匹配同样自对齐（0xE3/0xEF 只能是首字节，不会是续字节）。
inline size_t pd_find_bracket_hyphen(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '-') continue;
        if (i == 0) continue;
        const bool prevClose =
            s[i - 1] == ')' || s[i - 1] == ']' ||
            (i >= 3 && s.compare(i - 3, 3, "\xef\xbc\x89") == 0) ||  // ） U+FF09
            (i >= 3 && s.compare(i - 3, 3, "\xe3\x80\x91") == 0) ||  // 】 U+3011
            (i >= 3 && s.compare(i - 3, 3, "\xe3\x80\x8d") == 0) ||  // 」 U+300D
            (i >= 3 && s.compare(i - 3, 3, "\xe3\x80\x89") == 0) ||  // 〉 U+3009（规格字节 E3 80 89）
            (i >= 3 && s.compare(i - 3, 3, "\xe3\x80\x97") == 0);    // 】 U+3017（规格字节 E3 80 97）
        if (!prevClose) continue;                          // 前一字符非闭括号 → 跳过
        if (pd_ws_len(s, i + 1) == 0) continue;            // 连字符后须有空白（越界=非空白）
        return i;
    }
    return std::string::npos;
}

// 剥行首曲号：前缀 "01" "01." "01 -" "01_" "[01]" "(01)" 等（一到三位数字+可选分隔）。
// 规则（按任务 spec）：
//   * 数字后面必须跟分隔符或空白才算曲号；纯数字开头且后面是文字的不动（"99Luft"）
//   * 超过三位数字不是曲号；整串只有数字也不是曲号（剥了就没题了）
//   * "5.1" 这类「数字.数字」开头是版本/声道标注，不当曲号剥
//   * 剥完若剩下空串则整体不剥
inline std::string pd_strip_trackno(const std::string& s) {
    // [NN] / (NN) 括号形式
    if (s.size() >= 3 && (s[0] == '[' || s[0] == '(')) {
        const char closer = (s[0] == '[') ? ']' : ')';
        size_t k = 1, nd = 0;
        while (k < s.size() && pd_isdigit(s[k])) { ++k; if (++nd > 3) return s; }
        if (nd == 0 || k >= s.size() || s[k] != closer) return s;
        size_t q = k + 1;
        // 括号组之后必须是分隔符/空白（"[01]abc" 不算曲号）
        if (q < s.size() && !pd_isspace(s[q]) && s[q] != '.' && s[q] != '_' && s[q] != '-') return s;
        while (q < s.size() && (pd_isspace(s[q]) || s[q] == '.' || s[q] == '_' || s[q] == '-')) ++q;
        const std::string rest = pd_trim(s.substr(q));
        return rest.empty() ? s : rest;
    }
    if (s.empty() || !pd_isdigit(s[0])) return s;
    size_t k = 0, nd = 0;
    while (k < s.size() && pd_isdigit(s[k])) { ++k; if (++nd > 3) return s; } // 一到三位
    if (k >= s.size()) return s;                                             // 纯数字串不动
    const char c = s[k];
    if (!pd_isspace(c) && c != '.' && c != '_' && c != '-') return s;        // 后面是文字 → 不动
    if (c == '.' && k + 1 < s.size() && pd_isdigit(s[k + 1])) return s;      // "5.1" 不剥
    // 消费数字后的分隔符与空白（"01 - Artist" → "Artist"，"01.Artist" → "Artist"）
    size_t q = k;
    while (q < s.size() && (pd_isspace(s[q]) || s[q] == '.' || s[q] == '_' || s[q] == '-')) ++q;
    const std::string rest = pd_trim(s.substr(q));
    return rest.empty() ? s : rest;
}

// 从尾部连续摘除成对的 (...)、（...）、[...]、【...】 组（组间允许空白）。
// 组内原文（不含括号本身）经 *groups 带出，顺序为原文从左到右；
// 遇到非配对括号或组间夹有文字（非连续）即停。返回是否至少摘除一组。
// 同类型括号按深度计数配对，嵌套整体算一组、组内原文原样保留（如 "(a (b) c)"）。
// 缺陷 3 修复（2026-10-21，调研文档 §5 未解决项 10）：括号族补入全角圆括号
// （ = EF BC 88、） = EF BC 89（各三字节）。与 【】 同为「同型等长三字节对」，
// 深度配对逻辑照旧复用；UTF-8 自同步保证 0xEF 首字节不会撞上多字节序列内部
// （同 pd_find_bracket_hyphen 对 ）的既有识别，此处只扩摘组集合，不动它）。
// MUSIC 曲库实测 10 首受影响：「不夜城 Night Fall（feat.冬名）」feat 提不到、
// 「砂糖星辰（Song for ANK & Kumako）」副题不进 version。
inline bool pd_strip_trailing_groups(std::string& s, std::vector<std::string>* groups) {
    bool any = false;
    for (;;) {
        size_t end = s.size();
        while (end > 0 && pd_isspace(s[end - 1])) --end;   // 组间空白：先跳过
        if (end == 0) break;
        // 识别尾部的 closer 类型
        const char* opener = nullptr;
        const char* closerSeq = nullptr;
        size_t blen = 0;                                   // 括号的字节长度（同族 opener/closer 等长）
        if (s[end - 1] == ')')                { opener = "(";            closerSeq = ")";         blen = 1; }
        else if (s[end - 1] == ']')           { opener = "[";            closerSeq = "]";         blen = 1; }
        else if (end >= 3 && s.compare(end - 3, 3, "\xef\xbc\x89") == 0)
                                              { opener = "\xef\xbc\x88"; closerSeq = "\xef\xbc\x89"; blen = 3; } // （） U+FF08/FF09
        else if (end >= 3 && s.compare(end - 3, 3, "\xe3\x80\x91") == 0)
                                              { opener = "\xe3\x80\x90"; closerSeq = "\xe3\x80\x91"; blen = 3; } // 【】
        else break;                                        // 尾部不是括号 → 停
        // 从 closer 往前按同类型深度计数找配对 opener
        int depth = 1;
        bool found = false;
        size_t op = 0;
        for (size_t i = end - blen; i-- > 0;) {
            if (s.compare(i, blen, opener) == 0) {
                if (--depth == 0) { found = true; op = i; break; }
            } else if (s.compare(i, blen, closerSeq) == 0) {
                ++depth;
            }
        }
        if (!found || op + blen > end - blen) break;       // 非配对 → 停（防御式上界检查）
        const std::string inner = s.substr(op + blen, (end - blen) - (op + blen));
        if (groups && !inner.empty()) groups->insert(groups->begin(), inner); // 前插 → 保持左到右
        s.resize(op);                                      // 摘掉本组（含括号），继续看前一组
        any = true;
    }
    return any;
}

// feat 标记 token（大小写不敏感；spec 列出 " feat." " feat_" " ft." " ft_" " featuring "，
// 前导空格即「前边界」的载体，这里存纯 token + 边界要求）：
//   * 前边界：token 首字符的前一个字节必须是空白（或位于行首）
//   * "featuring" 无标点自分隔，额外要求后面跟空白；"feat."/"feat_"/"ft."/"ft_" 自带终止符
struct pd_feat_tok { const char* s; size_t n; bool spaceAfter; };

// 在小写化的串里找首次出现的 feat 标记，返回 token 起点（npos=没有），token 长度经 tokLen 带出
inline size_t pd_find_feat(const std::string& low, size_t& tokLen) {
    static const pd_feat_tok kToks[] = {
        { "feat.", 5, false }, { "feat_", 5, false },
        { "ft.",   3, false }, { "ft_",   3, false },
        { "featuring", 9, true },
    };
    const size_t nToks = sizeof(kToks) / sizeof(kToks[0]);
    for (size_t i = 0; i < low.size(); ++i) {
        if (i != 0 && !pd_isspace(low[i - 1])) continue;   // 前边界：空白或行首
        for (size_t t = 0; t < nToks; ++t) {
            const pd_feat_tok& tk = kToks[t];
            if (i + tk.n > low.size()) continue;
            if (low.compare(i, tk.n, tk.s) != 0) continue;
            if (tk.spaceAfter && (i + tk.n >= low.size() || !pd_isspace(low[i + tk.n]))) continue;
            tokLen = tk.n;
            return i;
        }
    }
    return std::string::npos;
}

// 仅 ASCII 大小写折叠（不碰 >=0x80 的字节，长度不变，位置与原串一一对应）
inline std::string pd_lower_ascii(const std::string& s) {
    std::string o(s);
    for (size_t i = 0; i < o.size(); ++i)
        if (o[i] >= 'A' && o[i] <= 'Z') o[i] = (char)(o[i] - 'A' + 'a');
    return o;
}

// 缺陷 1 修复（2026-10-21）：对 pd_strip_trailing_groups 摘出的一个括号组做分类。
// 组内容（去首尾空白）若以 feat 标记开头——大小写不敏感，token 集合与 pd_find_feat
// 相同（feat. / feat_ / ft. / ft_ / featuring）；token 位于组首即行首，前边界天然
// 满足；token 后允许直接跟内容或有空白（括号内整段本就是标注，不沿用 pd_find_feat
// 对 featuring 的「后随空白」要求）——则去掉 token 与后续空白，剩余非空时作为
// 合作艺人并入 feat（feat 已非空则用 " / " 追加）并返回；
// 剩余为空或没有 feat 标记 → 组原文进 *versionGroups（照旧进 version）。
inline void pd_classify_group(const std::string& group, std::string& feat,
                              std::vector<std::string>* versionGroups) {
    const std::string g = pd_trim(group);
    if (!g.empty()) {
        const std::string low = pd_lower_ascii(g);
        static const pd_feat_tok kToks[] = {
            { "feat.", 5, false }, { "feat_", 5, false },
            { "ft.",   3, false }, { "ft_",   3, false },
            { "featuring", 9, false },   // 组内 token 后允许直接跟内容，不要求空白
        };
        const size_t nToks = sizeof(kToks) / sizeof(kToks[0]);
        for (size_t t = 0; t < nToks; ++t) {
            const pd_feat_tok& tk = kToks[t];
            if (low.compare(0, tk.n, tk.s) != 0) continue;
            size_t p = tk.n;                   // 去掉 token 与后续空白
            for (;;) { const size_t w = pd_ws_len(g, p); if (w == 0) break; p += w; }
            const std::string rest = pd_trim(g.substr(p));
            if (rest.empty()) break;           // 剩余为空 → 照旧进 version
            if (!feat.empty()) feat += " / ";
            feat += rest;
            return;
        }
    }
    versionGroups->push_back(g);
}

} // namespace parse_detail

// ---------------------------------------------------------------------------
// 入口 1：标签透传
// ---------------------------------------------------------------------------

// 标签可信时直接透传，不做规则拆分。artist 与 title 都非空 → confidence=1；
// 否则（缺一或全缺）→ confidence=0.5（提示上层值得用文件名/LLM 再兜一道）。
// method 恒为 "tag"。
inline ParsedName parse_from_tags(const std::string& artist, const std::string& title) {
    ParsedName r;
    r.method = "tag";
    r.artist = artist;   // 透传原值，不清洗不改写
    r.title  = title;
    const bool both = !parse_detail::pd_trim(artist).empty() && !parse_detail::pd_trim(title).empty();
    r.confidence = both ? 1.0f : 0.5f;
    return r;
}

// ---------------------------------------------------------------------------
// 入口 2：文件名规则拆分
// ---------------------------------------------------------------------------

// 规则（按序，对齐任务 spec）：
//   1) 去首尾空白；
//   2) 去行首曲号（见 pd_strip_trackno）；
//   3) 以第一个 " - "（空格-连字符-空格）切分：前=artist，后=title；
//      未命中时回退一次「闭括号紧跟连字符+空白」的切分（见 pd_find_bracket_hyphen，
//      如 "人声音质（甜系女声）- 恋文"，confidence 与标准切分同 0.8 基准）；
//      仍没有分隔符 → 全部作为 title，confidence=0.3（" _ " 不是分隔符，见例 3）；
//   4) version 只作用于 title 段：从尾部连续摘除成对括号组，" / " 连接（左→右）；
//      每组先过 pd_classify_group 分类：组内容以 feat 标记开头（feat./feat_/ft./
//      ft_/featuring，大小写不敏感）→ 摘进 feat（已非空用 " / " 追加），
//      如 "未来永劫 (feat.可不)" 的 feat=可不 而非 version="feat.可不"；
//   5) feat 只作用于 title 段：首个 feat 标记之后的文本为 feat（feat 内再做一次
//      第 4 条的括号摘除，fgroups 同样走 pd_classify_group 分类），title 截到标记之前；
//   6) artist 段保持原样（括号里的别名/读音不拆）；
//   7) 置信度：切分成功且 title 非空 → 0.8；有 feat/version 各 +0.05，封顶 0.9；
//      artist 空 → 0.4（防御路径：正常 trim 后切分出空 artist 理论上不可达）；
//      title 摘完括号/feat 后变空 → 按弱结果 0.3 处理。
// method 恒为 "rule"。
inline ParsedName parse_filename(const std::string& stem) {
    ParsedName r;
    r.method = "rule";
    namespace pd = parse_detail;
    std::string s = pd::pd_trim(stem);
    if (s.empty()) return r;                          // 空输入 → 全空 + confidence 0
    s = pd::pd_trim(pd::pd_strip_trackno(s));

    const size_t cut = s.find(" - ");
    float conf;
    if (cut == std::string::npos) {
        // 缺陷 2 修复（2026-10-21）：标准 " - " 未命中时，回退做一次「闭括号紧跟
        // 连字符+空白」的切分（"人声音质（甜系女声）- 恋文"：连字符前是全角 ）无
        // 空格，标准切分够不着）。找不到则维持原状：全部作为 title。
        const size_t fc = pd::pd_find_bracket_hyphen(s);
        if (fc == std::string::npos) {
            r.title = s;                              // 规则 3：无分隔符 → 全部作为 title
            conf = 0.3f;
        } else {
            r.artist = pd::pd_trim(s.substr(0, fc));  // artist 含闭括号，仅去首尾空白
            r.title  = pd::pd_trim(s.substr(fc + 1)); // 连字符后去前导空白
            if      (r.title.empty())  conf = 0.3f;   // 只有 artist，没题 → 弱
            else if (r.artist.empty()) conf = 0.4f;   // 规则 7：artist 空
            else                       conf = 0.8f;   // 与标准 " - " 切分同基准
        }
    } else {
        r.artist = pd::pd_trim(s.substr(0, cut));     // 规则 6：artist 原样保留
        r.title  = pd::pd_trim(s.substr(cut + 3));
        if      (r.title.empty())  conf = 0.3f;       // 只有 artist，没题 → 弱
        else if (r.artist.empty()) conf = 0.4f;       // 规则 7：artist 空
        else                       conf = 0.8f;       // 规则 7：切分成功且 title 非空
    }

    // 规则 4：version（只对 title 段）。摘出的括号组先收集，稍后统一分类
    //（缺陷 1 修复：feat 标记开头的组改摘进 feat，不再错进 version）。
    std::vector<std::string> groups;
    pd::pd_strip_trailing_groups(r.title, &groups);
    r.title = pd::pd_trim(r.title);

    // 规则 5：feat（只对 title 段），feat 内再做括号摘除
    std::vector<std::string> fgroups;
    {
        size_t tokLen = 0;
        const size_t fp = pd::pd_find_feat(pd::pd_lower_ascii(r.title), tokLen);
        if (fp != std::string::npos) {
            std::string f = pd::pd_trim(r.title.substr(fp + tokLen));
            pd::pd_strip_trailing_groups(f, &fgroups);
            r.feat = pd::pd_trim(f);
            r.title = pd::pd_trim(r.title.substr(0, fp));
        }
    }
    // 缺陷 1 修复（2026-10-21）：每个摘出的括号组先过 pd_classify_group 分类——
    // 组内容以 feat 标记开头 → 摘进 feat（已非空用 " / " 追加）；无标记或剩余为空
    // → 照旧进 version。title 组在前、feat 段组在后，与原 version 连接顺序一致。
    {
        std::vector<std::string> vgroups;
        for (const std::string& g : groups)  pd::pd_classify_group(g, r.feat, &vgroups);
        for (const std::string& g : fgroups) pd::pd_classify_group(g, r.feat, &vgroups);
        for (const std::string& g : vgroups) {
            if (g.empty()) continue;
            if (!r.version.empty()) r.version += " / ";
            r.version += g;
        }
    }

    // 规则 7：加成与封顶（加成只挂在 0.8 基准上；0.3/0.4 的弱路径不加成）
    if (conf >= 0.8f) {
        if (!r.feat.empty())    conf += 0.05f;
        if (!r.version.empty()) conf += 0.05f;
        if (conf > 0.9f) conf = 0.9f;
    }
    if (r.title.empty()) conf = 0.3f;                 // 摘完变空题 → 弱结果
    r.confidence = conf;
    return r;
}

// ---------------------------------------------------------------------------
// 入口 3：LLM 回包解析
// ---------------------------------------------------------------------------

// 剥 ```json ... ``` 围栏：取围栏内内容；没有围栏则原样返回。
// 围栏起始行上的语言标记（json/text…）随该行一起丢弃。
inline std::string parse_llm_strip_fence(const std::string& text) {
    const size_t f1 = text.find("```");
    if (f1 == std::string::npos) return text;
    const size_t cs = f1 + 3;
    const size_t nl = text.find('\n', cs);
    const size_t bodyBegin = (nl == std::string::npos) ? cs : nl + 1;
    const size_t f2 = text.find("```", bodyBegin);
    return text.substr(bodyBegin, (f2 == std::string::npos ? text.size() : f2) - bodyBegin);
}

// 解析 deepseek-flash 的回包：剥围栏后取 {...} 交给 src/net/json.h 解析，
// 读 {"artist","title","feat","version"} 四键。成功 → method="llm"、confidence=0.7；
// 解析失败（无 JSON 对象/对象为空）→ 全字段空、method="llm"、title 留空、confidence=0。
inline ParsedName parse_llm_parse(const std::string& llmText) {
    ParsedName r;
    r.method = "llm";
    const std::string body = parse_llm_strip_fence(llmText);
    const size_t b1 = body.find('{');
    if (b1 == std::string::npos) return r;
    const size_t b2 = body.rfind('}');
    if (b2 == std::string::npos || b2 <= b1) return r;
    const rta::net::Json j = rta::net::json_parse(body.substr(b1, b2 - b1 + 1));
    if (j.type != rta::net::Json::Obj) return r;
    if (!(j.has("artist") || j.has("title") || j.has("feat") || j.has("version"))) return r;
    r.artist  = j["artist"].s();
    r.title   = j["title"].s();
    r.feat    = j["feat"].s();
    r.version = j["version"].s();
    r.confidence = 0.7f;
    return r;
}

// ---------------------------------------------------------------------------
// 入口 4：提示词生成（给 deepseek-flash）
// ---------------------------------------------------------------------------

// JSON 字符串转义（本文件独立实现，对照 tools/rtaweb.cpp 的 jesc，不依赖 tools；
// \r 与项目出参约定一致直接丢弃，其他 <0x20 控制字符走 \uXXXX）。
// 注：Windows 文件名不可能含 `"` 与 `\`，故对合法文件名 esc_json(stem)==stem 逐字节一致，
// 「提示词内含 stem 原文」的要求因此始终成立。
inline std::string esc_json(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    char b[8];
    for (unsigned char c : s) {
        if (c == '"')       { o += "\\\""; }
        else if (c == '\\') { o += "\\\\"; }
        else if (c == '\n') { o += "\\n"; }
        else if (c == '\t') { o += "\\t"; }
        else if (c == '\r') { /* 丢弃（对齐项目内 jesc 约定） */ }
        else if (c < 0x20)  { std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c); o += b; }
        else                { o += (char)c; }
    }
    return o;
}

// 生成解析提示词：要求模型只输出 artist/title/feat/version 四键 JSON，不要多余文字；
// 提示词内含 stem 原文（经 esc_json 嵌入，见上注）。
inline std::string parse_llm_prompt(const std::string& stem) {
    std::ostringstream o;
    o << "你是音乐元数据解析助手。下面给出一个音频文件的文件名，"
         "请解析出艺术家、曲名、featuring 合作艺人与版本信息。\n"
         "只输出一个 JSON 对象，不要输出任何其他文字、解释或 Markdown 代码块围栏，格式如下：\n"
         "{\"artist\":\"艺术家\",\"title\":\"曲名\",\"feat\":\"合作艺人\",\"version\":\"版本\"}\n"
         "说明：feat 是 feat./ft./featuring 之后的合作艺人，没有则填空字符串；"
         "version 是曲名尾部括号里的版本说明（如 Live、DJ版、Single Version、Inst），没有则填空字符串；"
         "四个键必须全部存在。\n"
         "文件名："
      << esc_json(stem) << "\n";
    return o.str();
}

} // namespace rta::meta
