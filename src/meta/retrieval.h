// retrieval.h — P3 资料层③：检索层（DeepSeek search/fetch + rerank/authority/白名单过滤 + 引用纪律）
//
// 对应 docs/P3-资料层接口契约.md §3 与 docs/元数据补全调研与规划.md §3「内容分级」L3：
//   * 编排：knowledge_fetch（L2权威文本） -> (可选) ds_search -> filter_hits
//            -> ds_fetch (前2~3页) -> llm_chat (Flash 综合)
//   * 纯函数过滤 filter_hits：rerank_score >= 1.5, authority >= 2, 域名白名单, URL去重, 排序, 上限
//   * 纯函数解析与校验 parse_synthesis_reply：
//     ① JSON 可解析
//     ② 每个 paragraph 的 citations >= 1（每段必须有引用）
//     ③ 每个 citation id 都能在 citations 列表中找到（无悬空引用）
//     ④ citation.url 必须属于「L2材料url集合 ∪ 通过过滤的检索url集合」（防幻觉硬门）
//     ⑤ citations.url 非空
//   * 缓存：<metabase>/entities/<entity_id>/summary-<lang>.json（原子写 tmp + rename，TTL 7天）
//   * 限频：每实体最多 1 search + 3 fetch + 1 chat，DeepSeek 5 QPS 账号级限速
//   * 错误快速失败，不将裸文本塞进页面
//
// 平台边界纪律：
//   * 禁止 include windows.h / winsock2.h / winhttp.h 等系统头（平台边界门禁自动扫描）
//   * HTTP 仅用 net/http_client.h，JSON 仅用 net/json.h，文件系统用 std::filesystem
//   * 实体 id 复用 entities.h / store.h 的 FNV-1a 64 位哈希
#pragma once

#include "meta/discogs.h"
#include "meta/entities.h"
#include "meta/knowledge.h"
#include "meta/llm.h"
#include "meta/store.h"
#include "net/http_client.h"
#include "net/json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rta::meta {

namespace fsx = std::filesystem;

// ---------------------------------------------------------------------------
// 数据结构定义
// ---------------------------------------------------------------------------

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
    std::string summary;
    std::string date;
    double rerankScore = 0.0;
    int authority = -1;
};

struct SearchResponse {
    bool ok = false;
    std::string error;
    std::vector<SearchHit> hits;
};

struct SummaryParagraph {
    std::string text;
    std::vector<std::string> citations;
};

struct SummarySection {
    std::string heading;
    std::vector<SummaryParagraph> paragraphs;
};

struct SummaryCitation {
    std::string id;
    std::string title;
    std::string url;
    std::string source;
    long long fetched_at = 0;
};

struct SummaryResult {
    bool ok = false;
    bool ai = true;
    std::string model = "deepseek-flash";
    long long generated_at = 0;
    long long expires_at = 0;
    std::string disclaimer;
    std::vector<SummarySection> sections;
    std::vector<SummaryCitation> citations;
    std::vector<std::string> materials_used;
    std::string error;
    bool from_cache = false;
    bool offline = false;
    bool validation_ok = false;
    // 2026-10-23（用户 261023-04 #14/#15）：AI 自有知识整理（没有任何权威材料可引用）。
    // UI 必须据此标注「AI 整理，未找到权威来源，仅供参考」，不能冒充有来源的综合。
    bool unverified = false;
    // 2026-10-24（用户 261024-01 #4）：结构化标签。AI 只填"确实知道"的字段，找不到的不出现。
    // 顺序保留（label,value），序列化成 JSON 对象供前端按 标签: 值 渲染。
    std::vector<std::pair<std::string, std::string>> fields;
};

// ---------------------------------------------------------------------------
// 域名白名单与 URL 纯函数
// ---------------------------------------------------------------------------

// 契约 §3.2 初版域名白名单常量数组
inline const std::vector<std::string>& default_domain_whitelist() {
    static const std::vector<std::string> kWhitelist = {
        // 百科 / 数据库
        "wikipedia.org", "moegirl.org.cn", "musicbrainz.org", "discogs.com", "allmusic.com",
        "rateyourmusic.com", "last.fm", "genius.com", "songfacts.com", "bangumi.tv", "bgm.tv",
        // 乐评 / 电台
        "pitchfork.com", "gramophone.co.uk", "bach-cantatas.com",
        // 2026-10-24（261024-01 #1）：官方 search 返回的高质量条目大量来自流媒体平台与官方站点，
        // 旧白名单只认百科/数据库 → 这些 hit 全被丢掉（Winged Step 就是这样变成"无材料"的）。
        "tidal.com", "spotify.com", "music.apple.com", "deezer.com", "bandcamp.com", "soundcloud.com",
        "y.qq.com", "qq.com", "music.163.com", "163.com", "music.amazon.com", "amazon.com",
        // 明日方舟 / 塞壬唱片官方
        "monster-siren.hypergryph.com", "hypergryph.com", "arknights.jp",
        // 通用（中文媒体常见）
        "zhihu.com", "bilibili.com", "weibo.com",
        "kuwo.cn", "douban.com", "prts.wiki", "yopu.co", "monster-siren.hypergryph.com"
    };
    return kWhitelist;
}

// ---------------------------------------------------------------------------
// 结构化标签 schema（2026-10-24，用户 261024-01 #4）
// 用户要求先把「每类实体该出现的标签」定下来，再让 AI 只填它确实知道的；找不到就不显示。
// 这里集中定义，prompt 与解析都用它排序（避免 map 按字母序把"发行时间"排到后面）。
// ---------------------------------------------------------------------------
inline const std::vector<std::string>& canonical_field_order() {
    static const std::vector<std::string> k = {
        // 歌曲
        "发行时间", "所属专辑", "歌曲风格", "歌词赏析", "演唱", "作词", "作曲", "编曲",
        "衍生作品", "时长", "版本",
        // 艺术家
        "简介", "国籍", "风格", "代表作品", "活跃年代", "厂牌", "合作",
        // 专辑
        "专辑编号", "艺术家", "专辑类型", "语种", "出版方", "专辑介绍", "曲目列表",
        "制作人员", "关联"
    };
    return k;
}

inline int canonical_field_rank(const std::string& label) {
    const auto& k = canonical_field_order();
    for (size_t i = 0; i < k.size(); ++i) if (k[i] == label) return (int)i;
    return 1000;
}

inline void sort_canonical_fields(std::vector<std::pair<std::string, std::string>>& f) {
    std::stable_sort(f.begin(), f.end(), [](const auto& a, const auto& b) {
        return canonical_field_rank(a.first) < canonical_field_rank(b.first);
    });
}

// 给 prompt 用的候选字段清单（按实体类型）
inline std::string field_schema_for_kind(const std::string& kindLabel) {
    if (kindLabel.find("专辑") != std::string::npos) {
        return "发行时间, 专辑编号, 艺术家, 专辑类型, 语种, 出版方, 专辑介绍, 曲目列表, 制作人员, 关联, 时长";
    }
    if (kindLabel.find("艺术家") != std::string::npos || kindLabel.find("歌手") != std::string::npos) {
        return "简介, 国籍, 风格, 代表作品, 活跃年代, 厂牌, 合作";
    }
    if (kindLabel.find("歌曲") != std::string::npos) {
        return "发行时间, 所属专辑, 歌曲风格, 歌词赏析, 演唱, 作词, 作曲, 编曲, 衍生作品, 时长, 版本";
    }
    return "简介, 发行时间, 艺术家, 专辑类型, 语种, 出版方, 风格, 代表作品";
}

// 提取 URL 中的 host（小写，去除协议、路径、端口）
inline std::string extract_host_from_url(const std::string& url) {
    std::string s = url;
    const size_t scheme_pos = s.find("://");
    if (scheme_pos != std::string::npos) {
        s = s.substr(scheme_pos + 3);
    }
    const size_t slash_pos = s.find('/');
    if (slash_pos != std::string::npos) {
        s = s.substr(0, slash_pos);
    }
    const size_t colon_pos = s.find(':');
    if (colon_pos != std::string::npos) {
        s = s.substr(0, colon_pos);
    }
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

// 子域匹配判定：
// 1. 完全相同：host == domain（如 wikipedia.org == wikipedia.org）
// 2. 子域名：host 以 "." + domain 结尾（如 zh.wikipedia.org 属于 wikipedia.org）
// 3. 仿冒域名如 fake-wikipedia.org 或 wikipedia.org.evil.com 严格拒绝
inline bool is_host_in_whitelist(const std::string& host, const std::vector<std::string>& whitelist) {
    if (host.empty()) return false;
    for (const auto& domain : whitelist) {
        if (domain.empty()) continue;
        std::string d = domain;
        for (char& c : d) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        if (host == d) return true;
        if (host.size() > d.size() + 1 && host.compare(host.size() - d.size(), d.size(), d) == 0) {
            if (host[host.size() - d.size() - 1] == '.') {
                return true;
            }
        }
    }
    return false;
}

// URL 比较归一化：去掉末尾斜杠
inline std::string normalize_url_for_compare(const std::string& url) {
    std::string s = url;
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// 检索响应解析：parse_search_response
// ---------------------------------------------------------------------------

inline SearchResponse parse_search_response(const std::string& text) {
    SearchResponse r;
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        r.error = "响应不是合法 JSON 对象";
        return r;
    }
    if (j.has("error")) {
        const rta::net::Json& e = j["error"];
        std::string msg = e.has("message") ? e["message"].s() : e.s();
        if (msg.empty()) msg = "服务端未给出具体 message";
        r.error = "服务端错误: " + msg;
        return r;
    }
    if (!j.has("results")) {
        r.error = "响应缺少 results 字段";
        return r;
    }
    const rta::net::Json& arr = j["results"];
    if (arr.type != rta::net::Json::Arr) {
        r.error = "results 不是数组";
        return r;
    }
    r.hits.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& h = arr.at(i);
        if (h.type != rta::net::Json::Obj) continue;
        SearchHit hit;
        hit.title       = h["title"].s();
        hit.url         = h["url"].s();
        hit.snippet     = h["snippet"].s();
        hit.summary     = h["summary"].s();
        hit.date        = h["date"].s();
        hit.rerankScore = h["rerank_score"].n();
        hit.authority   = h["authority"].ok() ? (int)h["authority"].n() : -1;
        r.hits.push_back(std::move(hit));
    }
    r.ok = true;
    return r;
}

inline bool parse_search_response(const std::string& text,
                                  std::vector<SearchHit>& hits_out,
                                  std::string* err_out = nullptr) {
    SearchResponse resp = parse_search_response(text);
    if (!resp.ok) {
        if (err_out) *err_out = resp.error;
        return false;
    }
    hits_out = std::move(resp.hits);
    return true;
}

// ---------------------------------------------------------------------------
// 过滤纯函数：filter_hits
// ---------------------------------------------------------------------------

struct FilterOptions {
    // 2026-10-24（261024-01 #1）：实测官方 /v1/tools/search 对音乐实体的 rerank 分布是 0.87~1.66，
// 旧值 1.5 会把整批高质量结果（Tidal/豆瓣/酷我/PRTS）全过滤掉 → 永远退到 AI 记忆。
// 降到 0.8，质量闸改由 authority>=2 + 域名白名单承担。
    double min_rerank_score = 0.8;
    int min_authority = 2;
    std::vector<std::string> whitelist; // 空则使用 default_domain_whitelist()
    size_t max_hits = 8;
};

struct FilterStats {
    size_t total_in = 0;
    size_t dropped_rerank = 0;
    size_t dropped_authority = 0;
    size_t dropped_whitelist = 0;
    size_t dropped_duplicate = 0;
    size_t dropped_limit = 0;
    size_t total_passed = 0;
};

struct FilterResult {
    std::vector<SearchHit> hits;
    FilterStats stats;

    operator const std::vector<SearchHit>&() const { return hits; }
    operator std::vector<SearchHit>() && { return std::move(hits); }
};

inline FilterResult filter_hits(const std::vector<SearchHit>& hits,
                                const FilterOptions& opts = FilterOptions()) {
    FilterResult res;
    res.stats.total_in = hits.size();
    const auto& wl = opts.whitelist.empty() ? default_domain_whitelist() : opts.whitelist;

    // 1. 纯函数逐条过滤并统计原因
    std::vector<SearchHit> valid;
    for (const auto& h : hits) {
        if (h.url.empty()) {
            res.stats.dropped_whitelist++;
            continue;
        }
        const std::string host = extract_host_from_url(h.url);
        if (!is_host_in_whitelist(host, wl)) {
            res.stats.dropped_whitelist++;
            continue;
        }
        if (h.rerankScore < opts.min_rerank_score) {
            res.stats.dropped_rerank++;
            continue;
        }
        if (h.authority < opts.min_authority) {
            res.stats.dropped_authority++;
            continue;
        }
        valid.push_back(h);
    }

    // 2. 按 (rerankScore 降序, authority 降序) 稳定排序
    std::stable_sort(valid.begin(), valid.end(), [](const SearchHit& a, const SearchHit& b) {
        if (a.rerankScore != b.rerankScore) {
            return a.rerankScore > b.rerankScore;
        }
        return a.authority > b.authority;
    });

    // 3. 按 URL 去重，并受限于 max_hits
    std::set<std::string> seen_urls;
    for (auto& h : valid) {
        const std::string norm_url = normalize_url_for_compare(h.url);
        if (seen_urls.count(norm_url) > 0) {
            res.stats.dropped_duplicate++;
            continue;
        }
        seen_urls.insert(norm_url);

        if (res.hits.size() >= opts.max_hits) {
            res.stats.dropped_limit++;
            continue;
        }
        res.hits.push_back(std::move(h));
    }

    res.stats.total_passed = res.hits.size();
    return res;
}

inline std::vector<SearchHit> filter_hits(const std::vector<SearchHit>& hits,
                                         const FilterOptions& opts,
                                         FilterStats* stats_out) {
    FilterResult r = filter_hits(hits, opts);
    if (stats_out) *stats_out = r.stats;
    return r.hits;
}

// ---------------------------------------------------------------------------
// 提示词构造：build_synthesis_prompt
// ---------------------------------------------------------------------------

inline std::vector<LlmMessage> build_synthesis_prompt(
    const std::string& entity,
    const std::vector<KnowledgeMaterial>& materials,
    const std::vector<SearchHit>& hits,
    const std::string& lang = "zh",
    const std::string& kindLabel = "") {
    (void)lang;
    const std::string schema = field_schema_for_kind(kindLabel);
    std::string sys =
        "你是一个严谨的音乐资料整理与综合专家。你的任务是根据给定的权威材料与检索网页，为音乐实体编写结构化的背景介绍与艺术评价。\n\n"
        "【硬约束规则】（核心纪律，违反任何一条均判失败）：\n"
        "1. 只依据材料：你综合的所有事实必须只能依据下方提供的材料，严禁外推或凭记忆引入材料外的任何未验证事实！材料未提及的事实绝对不写。\n"
        "2. JSON 输出：输出必须是严格合法的 JSON 对象，不要输出任何 Markdown 标记（如 ```json 代码块），不要有任何问候或解释说明。\n"
        "3. 每条引用与段落引用纪律：\n"
        "   - JSON 必须包含 \"disclaimer\"、\"sections\"、\"citations\"。\n"
        "   - sections 为数组，每个元素包含 \"heading\"（章节标题）和 \"paragraphs\"（段落数组）。\n"
        "   - paragraphs 中每个段落对象必须包含 \"text\" 和 \"citations\" 数组。\n"
        "   - 每段至少 1 条引用：每个段落的 citations 数组至少包含 1 条引用 ID（如 [\"M1\"]），无引用支撑的陈述一律禁止输出！\n"
        "   - citations 数组列出所有引用的条目，每个条目包含 \"id\"、\"title\"、\"url\"、\"source\"、\"fetched_at\"。\n"
        "   - 引用 URL 必须原样复制自输入材料中对应的 url，严禁修改或编造 URL！\n"
        "4. 免责声明：disclaimer 固定为：\"仅依据下方来源材料综合整理；可能不完整，请以来源为准\"。\n"
        "5. 行文纪律：**不要提及「材料」「来源」「资料」「条目」「文中」等元话语**，也不要复述检索过程；"
        "直接以陈述句写事实（例：不要写「材料列出的代表歌曲包括…」，直接写「代表歌曲包括…」）。\n"
        "6. 结构化标签：JSON 还必须包含 \"fields\" 对象（键值都是字符串），用于界面上的「标签: 值」展示。"
        "**只填材料明确支撑的字段；材料没说的键直接不要出现**（禁止写「未知」「暂无」「—」占位）。\n"
        "   本实体可用字段（键名必须原样使用）：" + schema + "\n"
        "   若同一信息既有标签又值得展开叙述，可以在 sections 里写叙述、fields 里放一句结论，但不要整段复制。\n\n"
        "JSON 格式模板：\n"
        "{\n"
        "  \"disclaimer\": \"仅依据下方来源材料综合整理；可能不完整，请以来源为准\",\n"
        "  \"fields\": { \"发行时间\": \"2024-01-09\", \"专辑类型\": \"EP（数字）\" },\n"
        "  \"sections\": [\n"
        "    {\n"
        "      \"heading\": \"创作背景\",\n"
        "      \"paragraphs\": [\n"
        "        {\n"
        "          \"text\": \"……\",\n"
        "          \"citations\": [\"M1\"]\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ],\n"
        "  \"citations\": [\n"
        "    {\n"
        "      \"id\": \"M1\",\n"
        "      \"title\": \"……\",\n"
        "      \"url\": \"https://……\",\n"
        "      \"source\": \"wikipedia-zh\",\n"
        "      \"fetched_at\": 1792690000\n"
        "    }\n"
        "  ]\n"
        "}\n";

    std::ostringstream u;
    u << "请为音乐实体【" << entity << "】整理结构化音乐资料概述。\n\n"
      << "【给定参考材料列表】（编号 [M1], [M2] ...）：\n";

    int mat_idx = 1;
    for (const auto& m : materials) {
        u << "\n[M" << mat_idx << "] 来源: " << m.source
          << " | 标题: " << m.title
          << " | 链接: " << m.url
          << " | 取回时间: " << m.fetched_at << "\n"
          << "正文:\n" << m.text << "\n";
        mat_idx++;
    }
    for (const auto& h : hits) {
        u << "\n[M" << mat_idx << "] 来源: search-fetch"
          << " | 标题: " << h.title
          << " | 链接: " << h.url
          << " | 日期: " << h.date << "\n"
          << "摘要/正文:\n" << (h.snippet.empty() ? h.summary : h.snippet) << "\n";
        mat_idx++;
    }
    u << "\n请严格遵守约束：只依据材料，输出严格 JSON，每条引用必须真实有效，每段至少 1 条引用，引用必须对应 [M1] 等编号并在 citations 中完整保留真实 url。";

    return {
        LlmMessage{ "system", sys },
        LlmMessage{ "user", u.str() }
    };
}

inline std::vector<LlmMessage> build_synthesis_prompt(
    EntityKind kind,
    const std::string& name,
    const std::vector<KnowledgeMaterial>& materials,
    const std::vector<SearchHit>& hits,
    const std::string& lang = "zh") {
    const std::string kindLabel = (kind == EntityKind::Release) ? "专辑" : "艺术家";
    return build_synthesis_prompt(name, materials, hits, lang, kindLabel);
}

// ---------------------------------------------------------------------------
// 综合响应解析与引用校验：parse_synthesis_reply
// ---------------------------------------------------------------------------

inline std::string retrieval_strip_code_fence(const std::string& text) {
    std::string s = llm_trim(text);
    if (s.rfind("```json", 0) == 0) {
        s = s.substr(7);
    } else if (s.rfind("```", 0) == 0) {
        s = s.substr(3);
    }
    if (s.size() >= 3 && s.substr(s.size() - 3) == "```") {
        s = s.substr(0, s.size() - 3);
    }
    return llm_trim(s);
}

// 校验硬规则：
// ① JSON 可解析
// ② 每个 paragraph citations >= 1（每段无引用 -> 拒绝）
// ③ 每个 citation id 都能在 citations 表里找到（悬空 -> 拒绝）
// ④ citation.url 必须属于 allowed_urls（若 enforce_allowed=true，不在允许集合 -> 拒绝）
// ⑤ citations.url 非空
// 拒绝时整体判失败（不许部分接受）
inline SummaryResult parse_synthesis_reply(const std::string& text,
                                          const std::vector<std::string>& allowed_urls,
                                          bool enforce_allowed_urls) {
    SummaryResult r;
    const std::string clean = retrieval_strip_code_fence(text);
    const rta::net::Json j = rta::net::json_parse(clean);

    // ① JSON 可解析
    if (j.type != rta::net::Json::Obj) {
        r.ok = false;
        r.error = "响应不是合法 JSON 对象";
        return r;
    }

    if (j.has("error")) {
        const rta::net::Json& e = j["error"];
        std::string msg = e.has("message") ? e["message"].s() : e.s();
        r.ok = false;
        r.error = "服务端报错: " + msg;
        return r;
    }

    // 免责声明
    r.disclaimer = j["disclaimer"].s();
    if (r.disclaimer.empty()) {
        r.disclaimer = "仅依据下方来源材料综合整理；可能不完整，请以来源为准";
    }

    // 解析 citations 表
    if (!j.has("citations") || j["citations"].type != rta::net::Json::Arr) {
        r.ok = false;
        r.error = "缺少 citations 数组";
        return r;
    }
    const auto& carr = j["citations"];
    if (carr.size() == 0) {
        r.ok = false;
        r.error = "citations 数组为空";
        return r;
    }

    std::set<std::string> allowed_set;
    for (const auto& u : allowed_urls) {
        if (!u.empty()) allowed_set.insert(normalize_url_for_compare(u));
    }

    std::map<std::string, SummaryCitation> cit_map;
    for (size_t i = 0; i < carr.size(); ++i) {
        const auto& item = carr.at(i);
        if (item.type != rta::net::Json::Obj) {
            r.ok = false;
            r.error = "citations 元素不是 JSON 对象";
            return r;
        }
        SummaryCitation c;
        c.id = item["id"].s();
        c.title = item["title"].s();
        c.url = item["url"].s();
        c.source = item["source"].s();
        c.fetched_at = (long long)item["fetched_at"].n();
        if (c.fetched_at <= 0) c.fetched_at = (long long)std::time(nullptr);

        if (c.id.empty()) {
            r.ok = false;
            r.error = "citation id 不能为空";
            return r;
        }

        // ⑤ citations.url 非空
        // 2026-10-23（261023-03 实测）：DeepSeek 偶尔把 citations[].url 留空，但 id 是真实编号 [Mk]。
        // 这里做**确定性修补**：id 形如 M<k> 且 k 落在材料范围内 → 用该材料自己的 URL 填回。
        // 安全性不变：URL 仍来自我们的材料集合（不是 LLM 编的），随后照样走 allowed 集合硬门校验。
        if (c.url.empty()) {
            int midx = 0;
            if (c.id.size() > 1 && (c.id[0] == 'M' || c.id[0] == 'm')) {
                bool all_digits = true;
                for (size_t ci = 1; ci < c.id.size(); ++ci) {
                    if (c.id[ci] < '0' || c.id[ci] > '9') { all_digits = false; break; }
                }
                if (all_digits) midx = std::atoi(c.id.c_str() + 1);
            }
            if (midx >= 1 && (size_t)midx <= allowed_urls.size() && !allowed_urls[(size_t)midx - 1].empty()) {
                c.url = allowed_urls[(size_t)midx - 1];
            }
        }
        if (c.url.empty()) {
            r.ok = false;
            r.error = "citation url 不能为空 (id=" + c.id + ")";
            return r;
        }

        // ④ citation.url 属于允许集合（防幻觉硬门）
        if (enforce_allowed_urls) {
            const std::string norm_u = normalize_url_for_compare(c.url);
            if (allowed_set.count(norm_u) == 0) {
                r.ok = false;
                r.error = "citation.url 不在允许来源集合中 (防幻觉硬门触发: " + c.url + ")";
                return r;
            }
        }

        cit_map[c.id] = c;
        r.citations.push_back(std::move(c));
    }

    // 解析 sections
    if (!j.has("sections") || j["sections"].type != rta::net::Json::Arr) {
        r.ok = false;
        r.error = "缺少 sections 数组";
        return r;
    }
    const auto& sarr = j["sections"];
    if (sarr.size() == 0) {
        r.ok = false;
        r.error = "sections 数组为空";
        return r;
    }

    std::set<std::string> used_urls;

    for (size_t i = 0; i < sarr.size(); ++i) {
        const auto& sitem = sarr.at(i);
        if (sitem.type != rta::net::Json::Obj) {
            r.ok = false;
            r.error = "section 元素不是 JSON 对象";
            return r;
        }
        SummarySection sec;
        sec.heading = sitem["heading"].s();
        if (sec.heading.empty()) sec.heading = "概述";

        if (!sitem.has("paragraphs") || sitem["paragraphs"].type != rta::net::Json::Arr) {
            r.ok = false;
            r.error = "section[" + std::to_string(i) + "] 缺少 paragraphs 数组";
            return r;
        }
        const auto& parr = sitem["paragraphs"];
        if (parr.size() == 0) {
            r.ok = false;
            r.error = "section[" + std::to_string(i) + "] 的 paragraphs 数组为空";
            return r;
        }

        for (size_t k = 0; k < parr.size(); ++k) {
            const auto& pitem = parr.at(k);
            if (pitem.type != rta::net::Json::Obj) {
                r.ok = false;
                r.error = "paragraph 元素不是 JSON 对象";
                return r;
            }
            SummaryParagraph sp;
            sp.text = pitem["text"].s();

            if (!pitem.has("citations") || pitem["citations"].type != rta::net::Json::Arr) {
                r.ok = false;
                r.error = "段落缺少 citations 数组";
                return r;
            }
            const auto& cit_arr = pitem["citations"];

            // ② 每个 paragraph citations >= 1
            if (cit_arr.size() == 0) {
                r.ok = false;
                r.error = "段落缺少引用 (每个 paragraph 的 citations 至少需要 1 条)";
                return r;
            }

            for (size_t c_idx = 0; c_idx < cit_arr.size(); ++c_idx) {
                const std::string cid = cit_arr.at(c_idx).s();
                if (cid.empty()) continue;

                // ③ 每个 citation id 都能在 citations 表里找到
                auto it = cit_map.find(cid);
                if (it == cit_map.end()) {
                    r.ok = false;
                    r.error = "citation id 悬空 (未在 citations 列表中找到: " + cid + ")";
                    return r;
                }
                sp.citations.push_back(cid);
                used_urls.insert(it->second.url);
            }

            if (sp.citations.empty()) {
                r.ok = false;
                r.error = "段落缺少有效引用 (citations 至少需要 1 条有效 id)";
                return r;
            }

            sec.paragraphs.push_back(std::move(sp));
        }
        r.sections.push_back(std::move(sec));
    }

    r.materials_used.assign(used_urls.begin(), used_urls.end());
    // 2026-10-24（261024-01 #4）：结构化标签（键值都是字符串；空值/占位一律丢弃）
    {
        const rta::net::Json& fj = j["fields"];
        if (fj.type == rta::net::Json::Obj) {
            for (const auto& kv : fj.obj) {
                const std::string k = kv.first;
                std::string v = kv.second.s();
                v = llm_trim(v);
                if (k.empty() || v.empty()) continue;
                if (v == "未知" || v == "暂无" || v == "—" || v == "-" || v == "无" || v == "N/A") continue;
                if (v.size() > 600) continue;
                r.fields.emplace_back(k, v);
            }
            sort_canonical_fields(r.fields);
        }
    }
    r.ok = true;
    r.validation_ok = true;
    return r;
}

inline SummaryResult parse_synthesis_reply(const std::string& text,
                                          const std::vector<std::string>& allowed_urls) {
    return parse_synthesis_reply(text, allowed_urls, true);
}

inline SummaryResult parse_synthesis_reply(const std::string& text) {
    return parse_synthesis_reply(text, {}, false);
}

inline SummaryResult parse_synthesis_reply(const std::string& text,
                                          const std::set<std::string>& allowed_urls) {
    std::vector<std::string> v(allowed_urls.begin(), allowed_urls.end());
    return parse_synthesis_reply(text, v, true);
}

// ---------------------------------------------------------------------------
// 缓存序列化、反序列化、落盘与路径
// ---------------------------------------------------------------------------

inline fsx::path summary_cache_path(const std::string& baseDir,
                                    const std::string& entity_id,
                                    const std::string& lang) {
    const std::string b = baseDir.empty() ? EntityStore::instance().baseDir() : baseDir;
    const std::string l = lang.empty() ? "zh" : lang;
    return meta_path_of(b) / "entities" / entity_id / ("summary-" + l + ".json");
}

inline std::string summary_to_json(const SummaryResult& sr) {
    std::ostringstream o;
    o << "{\n"
      << "  \"ok\": " << (sr.ok ? "true" : "false") << ",\n"
      << "  \"ai\": " << (sr.ai ? "true" : "false") << ",\n"
      << "  \"model\": \"" << meta_jesc(sr.model) << "\",\n"
      << "  \"generated_at\": " << sr.generated_at << ",\n"
      << "  \"expires_at\": " << sr.expires_at << ",\n"
      << "  \"disclaimer\": \"" << meta_jesc(sr.disclaimer) << "\",\n"
      << "  \"fields\": {";
{
    bool first_f = true;
    for (const auto& kv : sr.fields) {
        if (!first_f) o << ", ";
        first_f = false;
        o << "\"" << meta_jesc(kv.first) << "\": \"" << meta_jesc(kv.second) << "\"";
    }
    o << "},\n"
      << "  \"sections\": [\n";
}
    for (size_t i = 0; i < sr.sections.size(); ++i) {
        const auto& sec = sr.sections[i];
        o << "    {\n"
          << "      \"heading\": \"" << meta_jesc(sec.heading) << "\",\n"
          << "      \"paragraphs\": [\n";
        for (size_t j = 0; j < sec.paragraphs.size(); ++j) {
            const auto& p = sec.paragraphs[j];
            o << "        {\n"
              << "          \"text\": \"" << meta_jesc(p.text) << "\",\n"
              << "          \"citations\": [";
            for (size_t k = 0; k < p.citations.size(); ++k) {
                if (k) o << ", ";
                o << "\"" << meta_jesc(p.citations[k]) << "\"";
            }
            o << "]\n"
              << "        }" << (j + 1 < sec.paragraphs.size() ? ",\n" : "\n");
        }
        o << "      ]\n"
          << "    }" << (i + 1 < sr.sections.size() ? ",\n" : "\n");
    }
    o << "  ],\n"
      << "  \"citations\": [\n";
    for (size_t i = 0; i < sr.citations.size(); ++i) {
        const auto& c = sr.citations[i];
        o << "    {\n"
          << "      \"id\": \"" << meta_jesc(c.id) << "\",\n"
          << "      \"title\": \"" << meta_jesc(c.title) << "\",\n"
          << "      \"url\": \"" << meta_jesc(c.url) << "\",\n"
          << "      \"source\": \"" << meta_jesc(c.source) << "\",\n"
          << "      \"fetched_at\": " << c.fetched_at << "\n"
          << "    }" << (i + 1 < sr.citations.size() ? ",\n" : "\n");
    }
    o << "  ],\n"
      << "  \"materials_used\": [";
    for (size_t i = 0; i < sr.materials_used.size(); ++i) {
        if (i) o << ", ";
        o << "\"" << meta_jesc(sr.materials_used[i]) << "\"";
    }
    o << "],\n"
      << "  \"from_cache\": " << (sr.from_cache ? "true" : "false") << ",\n"
      << "  \"offline\": " << (sr.offline ? "true" : "false") << ",\n"
      << "  \"unverified\": " << (sr.unverified ? "true" : "false") << "\n"
      << "}\n";
    return o.str();
}

inline bool summary_from_json(const std::string& text,
                              SummaryResult& sr_out,
                              std::string* err_out = nullptr) {
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        if (err_out) *err_out = "summary cache corrupted: not a JSON object";
        return false;
    }

    sr_out.ok = j.has("ok") ? j["ok"].b : true;
    sr_out.ai = j.has("ai") ? j["ai"].b : true;
    sr_out.model = j["model"].s();
    if (sr_out.model.empty()) sr_out.model = "deepseek-flash";
    sr_out.generated_at = (long long)j["generated_at"].n();
    sr_out.expires_at = (long long)j["expires_at"].n();
    if (sr_out.expires_at <= 0 && sr_out.generated_at > 0) {
        sr_out.expires_at = sr_out.generated_at + 7 * 86400; // 默认 TTL 7 天
    }
    sr_out.disclaimer = j["disclaimer"].s();
    if (j.has("from_cache")) sr_out.from_cache = j["from_cache"].b;
    if (j.has("offline")) sr_out.offline = j["offline"].b;
    if (j.has("unverified")) sr_out.unverified = j["unverified"].b;
    // 结构化标签（旧缓存没有这个字段 → 空数组，兼容）
    {
        const rta::net::Json& fj = j["fields"];
        if (fj.type == rta::net::Json::Obj) {
            for (const auto& kv : fj.obj) {
                const std::string v = llm_trim(kv.second.s());
                if (kv.first.empty() || v.empty()) continue;
                sr_out.fields.emplace_back(kv.first, v);
            }
            sort_canonical_fields(sr_out.fields);
        }
    }

    // citations
    const rta::net::Json& carr = j["citations"];
    if (carr.type != rta::net::Json::Arr) {
        if (err_out) *err_out = "summary cache corrupted: citations is not an array";
        return false;
    }
    sr_out.citations.clear();
    for (size_t i = 0; i < carr.size(); ++i) {
        const auto& item = carr.at(i);
        if (item.type != rta::net::Json::Obj) continue;
        SummaryCitation sc;
        sc.id = item["id"].s();
        sc.title = item["title"].s();
        sc.url = item["url"].s();
        sc.source = item["source"].s();
        sc.fetched_at = (long long)item["fetched_at"].n();
        sr_out.citations.push_back(std::move(sc));
    }

    // sections
    const rta::net::Json& sarr = j["sections"];
    if (sarr.type != rta::net::Json::Arr) {
        if (err_out) *err_out = "summary cache corrupted: sections is not an array";
        return false;
    }
    sr_out.sections.clear();
    for (size_t i = 0; i < sarr.size(); ++i) {
        const auto& sitem = sarr.at(i);
        if (sitem.type != rta::net::Json::Obj) continue;
        SummarySection sec;
        sec.heading = sitem["heading"].s();
        const auto& parr = sitem["paragraphs"];
        if (parr.type == rta::net::Json::Arr) {
            for (size_t k = 0; k < parr.size(); ++k) {
                const auto& pitem = parr.at(k);
                if (pitem.type != rta::net::Json::Obj) continue;
                SummaryParagraph sp;
                sp.text = pitem["text"].s();
                const auto& cit_arr = pitem["citations"];
                if (cit_arr.type == rta::net::Json::Arr) {
                    for (size_t c_idx = 0; c_idx < cit_arr.size(); ++c_idx) {
                        sp.citations.push_back(cit_arr.at(c_idx).s());
                    }
                }
                sec.paragraphs.push_back(std::move(sp));
            }
        }
        sr_out.sections.push_back(std::move(sec));
    }

    // materials_used
    sr_out.materials_used.clear();
    const rta::net::Json& marr = j["materials_used"];
    if (marr.type == rta::net::Json::Arr) {
        for (size_t i = 0; i < marr.size(); ++i) {
            sr_out.materials_used.push_back(marr.at(i).s());
        }
    }

    sr_out.validation_ok = true;
    return true;
}

inline bool summary_is_expired(const SummaryResult& sr, long long now_sec = 0) {
    if (now_sec <= 0) now_sec = (long long)std::time(nullptr);
    long long exp = sr.expires_at;
    if (exp <= 0 && sr.generated_at > 0) {
        exp = sr.generated_at + 7 * 86400; // TTL 7 天
    }
    if (exp <= 0) return true;
    return now_sec >= exp;
}

inline bool summary_save_cache(const std::string& baseDir,
                               const std::string& entity_id,
                               const std::string& lang,
                               const SummaryResult& sr) {
    if (entity_id.empty()) return false;
    const fsx::path p = summary_cache_path(baseDir, entity_id, lang);
    const std::string content = summary_to_json(sr);
    return entity_write_atomic(p, content);
}

inline bool summary_load_cache(const std::string& baseDir,
                               const std::string& entity_id,
                               const std::string& lang,
                               SummaryResult& sr_out,
                               std::string* err_out = nullptr) {
    if (entity_id.empty()) return false;
    const fsx::path p = summary_cache_path(baseDir, entity_id, lang);
    std::string text;
    if (!entity_read_file(p, text)) {
        if (err_out) *err_out = "cache file not found or empty";
        return false;
    }
    return summary_from_json(text, sr_out, err_out);
}

// ---------------------------------------------------------------------------
// 限频器（DeepSeek 5 QPS 账号级限速：最小间隔 220ms）
// ---------------------------------------------------------------------------

struct RetrievalRateLimiter {
    std::mutex mtx;
    std::chrono::steady_clock::time_point last_call{};
    int min_interval_ms = 220; // 5 QPS = 200ms，加 20ms 安全裕量

    void acquire() {
        std::unique_lock<std::mutex> lk(mtx);
        const auto now = std::chrono::steady_clock::now();
        if (last_call.time_since_epoch().count() != 0) {
            const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_call).count();
            if (diff < min_interval_ms) {
                const auto wait_ms = min_interval_ms - diff;
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                lk.lock();
            }
        }
        last_call = std::chrono::steady_clock::now();
    }
};

inline RetrievalRateLimiter& retrieval_rate_limiter() {
    static RetrievalRateLimiter lim;
    return lim;
}

// ---------------------------------------------------------------------------
// 编排检索：retrieval_fetch
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Discogs Release 材料提取（供专辑简介结合 Wikipedia 使用）
// ---------------------------------------------------------------------------

inline KnowledgeMaterial retrieval_fetch_discogs_release(const std::string& name,
                                                         const std::string& artist = "",
                                                         int timeout_ms = 10000) {
    KnowledgeMaterial m;
    if (name.empty()) return m;

    std::string q = !artist.empty() ? (artist + " " + name) : name;
    if (!discogs_transport_hook() && !DiscogsRateLimiter::instance().acquire(2500)) {
        return m;
    }

    const std::string searchUrl = "https://api.discogs.com/database/search?q=" +
                                  discogs_url_encode(q) + "&type=release";
    const std::vector<std::string> headers = {
        "User-Agent: RLPlayer/1.0.10 (metadata; contact: local)"
    };

    rta::net::HttpResponse resp;
    if (discogs_transport_hook()) {
        resp = discogs_transport_hook()(searchUrl, headers, timeout_ms);
    } else {
        resp = rta::net::http_get(searchUrl, headers, timeout_ms);
    }

    if (resp.status != 200 || resp.body.empty()) {
        if (resp.status == 429) DiscogsRateLimiter::instance().trip_circuit(60);
        return m;
    }

    std::vector<SearchResultItem> items;
    std::string perr;
    const std::string bodyStr(resp.body.begin(), resp.body.end());
    if (!parse_discogs_search_json(bodyStr, items, &perr) || items.empty()) {
        return m;
    }

    const SearchResultItem& top = items[0];
    DiscogsReleaseDetail det;
    bool detailOk = false;
    if (top.id > 0) {
        if (discogs_transport_hook() || DiscogsRateLimiter::instance().acquire(2500)) {
            std::string relUrl = "https://api.discogs.com/releases/" + std::to_string(top.id);
            rta::net::HttpResponse detResp;
            if (discogs_transport_hook()) {
                detResp = discogs_transport_hook()(relUrl, headers, timeout_ms);
            } else {
                detResp = rta::net::http_get(relUrl, headers, timeout_ms);
            }
            if (detResp.status == 200 && !detResp.body.empty()) {
                std::string detBody(detResp.body.begin(), detResp.body.end());
                std::string detErr;
                if (parse_discogs_release_json(detBody, det, &detErr)) {
                    detailOk = true;
                }
            } else if (detResp.status == 429) {
                DiscogsRateLimiter::instance().trip_circuit(60);
            }
        }
    }

    const int64_t relId = (detailOk && det.id > 0) ? det.id : top.id;
    m.source = "discogs";
    m.kind = "release";
    m.subject = name;
    m.title = detailOk && !det.title.empty() ? det.title : (!top.title.empty() ? top.title : name);
    if (detailOk && !det.url.empty()) {
        m.url = det.url;
    } else if (!top.uri.empty()) {
        m.url = top.uri.rfind("http", 0) == 0 ? top.uri : ("https://www.discogs.com" + top.uri);
    } else if (relId > 0) {
        m.url = "https://www.discogs.com/release/" + std::to_string(relId);
    }
    m.fetched_at = (long long)std::time(nullptr);

    std::ostringstream ss;
    ss << "专辑: " << m.title << "\n";
    if (!artist.empty()) ss << "艺术家: " << artist << "\n";
    int yr = detailOk && det.year > 0 ? det.year : top.year;
    if (yr > 0) ss << "发行年份: " << yr << "\n";
    std::string lab = detailOk && !det.label.empty() ? det.label : top.label;
    if (!lab.empty()) ss << "厂牌: " << lab << "\n";
    std::string fmt = detailOk && !det.format.empty() ? det.format : top.format;
    if (!fmt.empty()) ss << "格式: " << fmt << "\n";
    std::string cntry = detailOk && !det.country.empty() ? det.country : top.country;
    if (!cntry.empty()) ss << "发行地区: " << cntry << "\n";
    const auto& gnrs = detailOk && !det.genres.empty() ? det.genres : top.genres;
    const auto& styls = detailOk && !det.styles.empty() ? det.styles : top.styles;
    if (!gnrs.empty() || !styls.empty()) {
        ss << "流派与风格: " << format_discogs_genre(gnrs, styls) << "\n";
    }
    if (detailOk) {
        if (!det.conductor.empty()) ss << "指挥: " << det.conductor << "\n";
        if (!det.ensemble.empty()) ss << "乐团/组合: " << det.ensemble << "\n";
        if (!det.composer.empty()) ss << "作曲: " << det.composer << "\n";
        if (!det.notes.empty()) ss << "专辑说明与发行背景:\n" << det.notes << "\n";
    }
    m.text = ss.str();
    return m;
}

struct RetrievalOptions {
    std::string metabase;
    bool refresh = false;
    bool offline = false;         // 强制离线测试/回退
    bool enable_search = true;
    int max_search_hits = 8;
    int max_fetch_pages = 3;      // 最多拉取前 2~3 页正文
    int timeout_ms = 15000;
    LlmConfig llm_cfg;
    FilterOptions filter_opts;
    std::string artist;           // 针对 Release 的艺术家辅助（用于 Wikipedia 与 Discogs 检索消歧）
    std::string entity_id;        // 显式指定实体 ID（为空则自动计算）
};

// ---------------------------------------------------------------------------
// AI 自有知识整理（无权威材料时的兜底；2026-10-23 用户 261023-04 #14/#15）
//
// 背景：用户实测「塞壬唱片」等实体维基没有条目 → 旧逻辑直接回「未找到权威材料，拒绝综合」，
// 页面上什么都不显示。用户原话「AI 呢？不会自己搜么？一定要维基百科？那要 AI 干嘛？」
//
// 折中方案（不放弃防幻觉纪律）：
//   * 只在 L2 权威材料与 L3 检索正文都为 0 时才启用；
//   * 结果标记 unverified=true，disclaimer 明确写「AI 整理（未找到权威来源，仅供参考）」，
//     UI 必须把这条标注显示出来，不许冒充带来源的综合；
//   * prompt 里禁止编造可核查的精确数据（日期/销量/榜单名次/成员名单），不确定就写「资料有限」；
//   * 结果同样写 summary 缓存（7 天），所以第二次打开不再调用 LLM。
// ---------------------------------------------------------------------------

inline std::vector<LlmMessage> build_ai_only_prompt(const std::string& entityLabel,
                                                    const std::string& kindLabel,
                                                    const std::string& lang) {
    (void)lang;
    std::string sys =
        "你是严谨的音乐资料编辑。当前没有任何可引用的权威材料，请仅凭你已有的知识为实体写一段简短介绍。\n"
        "【纪律】\n"
        "1. 只写你有把握的共性信息（风格类型、大致活动年代、代表作品方向、行业位置）；\n"
        "2. 严禁编造可核查的精确数据：具体发行日期、销量数字、榜单名次、成员真实姓名、获奖年份等，"
        "如果没有十足把握就不要写；\n"
        "3. 不确定就写「资料有限」，不要用模糊话术凑字数；\n"
        "4. 只输出严格合法的 JSON 对象，不要 Markdown 代码块、不要任何解释文字。\n"
        "5. 结构化标签：输出 \"fields\" 对象（键值都是字符串），只填你有把握的字段，没把握的键直接不要出现"
        "（禁止写「未知」「暂无」「—」）。本实体可用字段（键名原样使用）：" + field_schema_for_kind(kindLabel) + "\n"
        "JSON 模板：\n"
        "{\"sections\":[{\"heading\":\"简介\",\"paragraphs\":[{\"text\":\"……\"}]}],\"fields\":{\"发行时间\":\"……\"}}\n";
    std::string user = "请为" + kindLabel + "【" + entityLabel + "】写 2~3 段简介，"
                       "可用小标题（如 风格与定位 / 代表作品 / 影响与评价）。直接输出 JSON。";
    return { LlmMessage{ "system", sys }, LlmMessage{ "user", user } };
}

// 宽松解析：只要求 sections/paragraphs 结构，不要求 citations（因为没有材料可引）。
inline SummaryResult parse_ai_only_reply(const std::string& text) {
    SummaryResult r;
    std::string t = text;
    // 容忍围栏式代码块包裹
    const size_t fence = t.find("\x60\x60\x60");
    if (fence != std::string::npos) {
        const size_t s = t.find('\n', fence);
        const size_t e = t.rfind("\x60\x60\x60");
        if (s != std::string::npos && e != std::string::npos && e > s) t = t.substr(s + 1, e - s - 1);
    }
    const rta::net::Json j = rta::net::json_parse(t);
    if (j.type != rta::net::Json::Obj) { r.error = "AI 输出不是 JSON 对象"; return r; }
    const rta::net::Json& sarr = j["sections"];
    if (sarr.type != rta::net::Json::Arr || sarr.size() == 0) { r.error = "AI 输出缺少 sections"; return r; }
    for (size_t i = 0; i < sarr.size(); ++i) {
        const auto& sitem = sarr.at(i);
        if (sitem.type != rta::net::Json::Obj) continue;
        SummarySection sec;
        sec.heading = sitem["heading"].s();
        if (sec.heading.empty()) sec.heading = "简介";
        const rta::net::Json& parr = sitem["paragraphs"];
        if (parr.type == rta::net::Json::Arr) {
            for (size_t k = 0; k < parr.size(); ++k) {
                const auto& p = parr.at(k);
                if (p.type != rta::net::Json::Obj) continue;
                SummaryParagraph para;
                para.text = p["text"].s();
                if (!para.text.empty()) sec.paragraphs.push_back(std::move(para));
            }
        }
        if (!sec.paragraphs.empty()) r.sections.push_back(std::move(sec));
    }
    if (r.sections.empty()) { r.error = "AI 输出没有可用段落"; return r; }
    // 结构化标签（AI 自有知识路径同样输出；空值/占位丢弃）
    {
        const rta::net::Json& fj = j["fields"];
        if (fj.type == rta::net::Json::Obj) {
            for (const auto& kv : fj.obj) {
                const std::string v = llm_trim(kv.second.s());
                if (kv.first.empty() || v.empty()) continue;
                if (v == "未知" || v == "暂无" || v == "—" || v == "-" || v == "无" || v == "N/A") continue;
                if (v.size() > 600) continue;
                r.fields.emplace_back(kv.first, v);
            }
            sort_canonical_fields(r.fields);
        }
    }
    r.ok = true;
    r.unverified = true;
    r.validation_ok = true;
    r.disclaimer = "AI 整理（未找到权威来源，仅供参考）";
    return r;
}

inline SummaryResult summary_ai_only_generate(const std::string& entityName,
                                              const std::string& kindLabel,
                                              const std::string& lang,
                                              const LlmConfig& cfg) {
    const std::vector<LlmMessage> base_msgs = build_ai_only_prompt(entityName, kindLabel, lang);
    SummaryResult parsed;
    std::string last_err;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<LlmMessage> msgs = base_msgs;
        if (attempt > 0) {
            msgs.push_back(LlmMessage{ "user",
                "上一次输出不合规（" + last_err + "）。请只输出严格 JSON：" +
                "{\"sections\":[{\"heading\":\"简介\",\"paragraphs\":[{\"text\":\"……\"}]}]}，不要任何其它文字。" });
        }
        retrieval_rate_limiter().acquire();
        const LlmReply rep = llm_chat(cfg, msgs, 2000, 0.3f, true);
        if (!rep.ok) { last_err = rep.error; continue; }
        parsed = parse_ai_only_reply(rep.content);
        if (parsed.ok) break;
        last_err = parsed.error;
    }
    if (parsed.ok) {
        parsed.model = cfg.model;
        parsed.generated_at = (long long)std::time(nullptr);
        parsed.expires_at = parsed.generated_at + 7 * 86400;
        parsed.from_cache = false;
        parsed.offline = false;
    }
    return parsed;
}

// 歌曲级简介（用户 261023-04 #7：单曲页要能单独看「歌曲」这一层）。
// 没有专门的权威源（维基/唱片条目通常到专辑层），所以走 AI 自有知识 + 明确标注 unverified；
// 按 artist+title 归键缓存，同一首歌只整理一次。
// 歌曲级 prompt：允许使用 search 命中正文（歌词/制作信息），只输出 fields + 短叙述。
// 语言要求：不引用编号（不需要 citations），但严禁编造材料外的年份/榜单/销量。
inline std::vector<LlmMessage> build_track_prompt(const std::string& title,
                                                  const std::string& artist,
                                                  const std::vector<SearchHit>& hits,
                                                  const std::string& lang) {
    (void)lang;
    const std::string schema = field_schema_for_kind("歌曲");
    std::string sys =
        "你是严谨的音乐资料编辑。请依据下方提供的检索材料，为这首歌曲整理结构化资料。\n"
        "【纪律】\n"
        "1. 只写材料能支撑的事实；材料没说的不要猜，也不要写「未知/暂无」。\n"
        "2. 严禁编造可核查的精确数据（发行日期、榜单名次、销量、作者名单），除非材料明确给出。\n"
        "3. 只输出严格 JSON 对象，不要 Markdown、不要解释。\n"
        "4. 结构：{\"fields\":{...},\"sections\":[{\"heading\":\"简介\",\"paragraphs\":[{\"text\":\"…\"}]}]}\n"
        "   fields 只填有把握的键，键名必须从下面清单里选：\n   " + schema + "\n"
        "5. 叙述（sections）最多两段，简短；不要把字段内容整段复制。\n";
    std::ostringstream u;
    u << "歌曲：【" << title << "】";
    if (!artist.empty()) u << "，演唱/艺术家：" << artist;
    u << "\n\n【检索材料】\n";
    int idx = 1;
    for (const auto& h : hits) {
        u << "\n[" << idx << "] " << h.title << " | " << h.url;
        if (!h.date.empty()) u << " | " << h.date;
        u << "\n" << (h.snippet.empty() ? h.summary : h.snippet) << "\n";
        idx++;
    }
    u << "\n直接输出 JSON。";
    return { LlmMessage{ "system", sys }, LlmMessage{ "user", u.str() } };
}

inline SummaryResult retrieval_fetch_track(const std::string& title,
                                           const std::string& artist,
                                           const std::string& lang,
                                           const RetrievalOptions& opts) {
    const std::string baseDir = !opts.metabase.empty() ? opts.metabase : EntityStore::instance().baseDir();
    const std::string eid = entity_make_id("track", entity_norm(artist) + "\x1f" + entity_norm(title));
    SummaryResult cached;
    if (summary_load_cache(baseDir, eid, lang, cached) && cached.ok && !opts.refresh &&
        !summary_is_expired(cached)) {
        cached.from_cache = true;
        return cached;
    }
    LlmConfig cfg = opts.llm_cfg;
    if (cfg.apiKey.empty()) cfg = llm_config_load();
    if (cfg.apiKey.empty()) {
        if (cached.ok) { cached.from_cache = true; cached.offline = true; return cached; }
        SummaryResult err;
        err.error = "未配置 LLM API key（rlplayer-llm.conf）";
        return err;
    }

    // 2026-10-24（261024-01 #1）：歌曲级资料也走**官方 search + fetch**，
    // 而不是只凭模型记忆（用户对"AI 只会瞎编"的批评就来自这里）。
    std::vector<SearchHit> hits;
    if (opts.enable_search) {
        std::string query;
        if (lang == "zh" || lang.empty()) {
            query = (artist.empty() ? title : (artist + " " + title)) + " 歌曲 作词 作曲 编曲 发行";
        } else if (lang == "ja") {
            query = (artist.empty() ? title : (artist + " " + title)) + " 楽曲 作詞 作曲 編曲";
        } else {
            query = (artist.empty() ? title : (artist + " " + title)) + " song credits release lyrics";
        }
        retrieval_rate_limiter().acquire();
        const DsSearchReply s_rep = ds_search(cfg, query, opts.max_search_hits > 0 ? opts.max_search_hits : 8);
        if (s_rep.ok) {
            std::vector<SearchHit> raw;
            for (const auto& dh : s_rep.hits) {
                SearchHit sh;
                sh.title = dh.title; sh.url = dh.url; sh.snippet = dh.snippet; sh.summary = dh.summary;
                sh.date = dh.date; sh.rerankScore = dh.rerankScore; sh.authority = dh.authority;
                raw.push_back(std::move(sh));
            }
            FilterResult fr = filter_hits(raw, opts.filter_opts);
            hits = std::move(fr.hits);
            const size_t fetch_count = std::min(hits.size(), (size_t)2);
            for (size_t i = 0; i < fetch_count; ++i) {
                retrieval_rate_limiter().acquire();
                const DsFetchReply f_rep = ds_fetch(cfg, hits[i].url);
                if (f_rep.ok && !f_rep.content.empty()) {
                    hits[i].snippet = f_rep.content.substr(0, 1200);
                    if (!f_rep.title.empty()) hits[i].title = f_rep.title;
                }
            }
        }
    }

    SummaryResult sr;
    if (!hits.empty()) {
        std::string last_err;
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::vector<LlmMessage> msgs = build_track_prompt(title, artist, hits, lang);
            if (attempt > 0) {
                msgs.push_back(LlmMessage{ "user", "上一次输出不合规（" + last_err +
                    "）。请只输出严格 JSON：{\"fields\":{...},\"sections\":[{\"heading\":\"简介\",\"paragraphs\":[{\"text\":\"…\"}]}]}" });
            }
            retrieval_rate_limiter().acquire();
            const LlmReply rep = llm_chat(cfg, msgs, 2200, 0.3f, true);
            if (!rep.ok) { last_err = rep.error; continue; }
            sr = parse_ai_only_reply(rep.content);
            if (sr.ok) break;
            last_err = sr.error;
        }
    }
    if (!sr.ok) {
        // 检索不到材料 → 退回 AI 自有知识（仍标 unverified，UI 只显示「AI 整理」）
        sr = summary_ai_only_generate(artist.empty() ? title : (artist + " - " + title), "歌曲", lang, cfg);
    } else {
        sr.unverified = true;   // 检索正文未做逐段引用校验 → 仍按"AI 整理"标注
        sr.disclaimer = "AI 整理";
    }
    if (sr.ok) {
        sr.generated_at = (long long)std::time(nullptr);
        sr.expires_at = sr.generated_at + 7 * 86400;
        summary_save_cache(baseDir, eid, lang, sr);
    }
    return sr;
}


inline SummaryResult retrieval_fetch(EntityKind kind,
                                     const std::string& name,
                                     const std::string& lang = "zh",
                                     const RetrievalOptions& opts = {}) {
    const std::string eid = !opts.entity_id.empty()
        ? opts.entity_id
        : ((!opts.artist.empty() && kind == EntityKind::Release)
            ? entity_make_id("release", entity_norm(opts.artist) + "\x1f" + entity_norm(name))
            : entity_id(kind, name));
    const std::string baseDir = !opts.metabase.empty() ? opts.metabase : EntityStore::instance().baseDir();

    // 1. 检查既有本地缓存
    SummaryResult cached;
    std::string cache_err;
    const bool have_cache = summary_load_cache(baseDir, eid, lang, cached, &cache_err);
    const long long now_sec = (long long)std::time(nullptr);
    const bool cache_unexpired = have_cache && cached.ok && !summary_is_expired(cached, now_sec);

    // 显式离线模式：直接读缓存并打上 offline 标识
    if (opts.offline) {
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            return cached;
        }
        SummaryResult err_res;
        err_res.ok = false;
        err_res.error = "离线模式且无有效本地缓存";
        return err_res;
    }

    // 命中未过期缓存且未要求 refresh -> 直接返回
    if (!opts.refresh && cache_unexpired) {
        cached.from_cache = true;
        cached.offline = false;
        return cached;
    }

    // 2. 读取 LLM 配置（优先用 opts 传入，否则从现有路径读取）
    LlmConfig cfg = opts.llm_cfg;
    if (cfg.apiKey.empty()) {
        cfg = llm_config_load();
        if (cfg.apiKey.empty()) {
            cfg = llm_config_load("build/rlplayer-llm.conf");
        }
    }
    if (cfg.apiKey.empty()) {
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            cached.error = "未配置 LLM API key，已回退至本地缓存";
            return cached;
        }
        SummaryResult err_res;
        err_res.ok = false;
        err_res.error = "未配置 LLM API key（rlplayer-llm.conf），无法执行检索与综合";
        return err_res;
    }

    // 3. Step 1: 拉取 L2 权威材料（Release 整合 Wikipedia 与 Discogs）
    KnowledgeOpts kopts;
    kopts.lang = lang;
    kopts.metabase = opts.metabase;
    kopts.refresh = opts.refresh;
    kopts.timeout_ms = opts.timeout_ms;
    KnowledgeResult kr;
    // 「有真正正文的材料」：有 url 且不是维基消歧义页（消歧义页正文只有候选标题列表，
    // 以前用 materials.empty() 判断会把它当成成功 → 艺术家消歧回退永远不触发，白付 LLM 费用）。
    auto has_real_material = [](const KnowledgeResult& r) {
        for (const auto& m : r.materials) {
            if (!m.url.empty() && m.kind != "disambiguation") return true;
        }
        return false;
    };
    if (kind == EntityKind::Release) {
        // 2026-10-23 DEBUG：Release 原先直接调 knowledge_wikipedia（纯网络、不读不写 L2 缓存），
        // 同一张专辑每次生成都要重打维基（失败还串行换写法再打两轮）。改回带缓存的 knowledge_fetch，
        // 只有拿不到「非消歧义」正文时，才用「艺术家+专辑名」限定再试（罕见路径，最多 2 次）。
        kr = knowledge_fetch(kind, name, kopts);
        if (!has_real_material(kr) && !opts.artist.empty()) {
            const std::string variants[2] = { opts.artist + " " + name, name + " (" + opts.artist + ")" };
            for (const auto& q : variants) {
                if (has_real_material(kr)) break;
                KnowledgeResult k2 = knowledge_wikipedia(q, lang, kopts);
                if (has_real_material(k2)) {
                    kr.materials.insert(kr.materials.end(), k2.materials.begin(), k2.materials.end());
                }
            }
        }
        KnowledgeMaterial dm = retrieval_fetch_discogs_release(name, opts.artist, opts.timeout_ms);
        if (!dm.url.empty()) {
            kr.materials.push_back(std::move(dm));
        }
        // 有真正文时丢掉消歧义候选列表（否则 LLM 会把候选标题当成事实写进简介）
        if (has_real_material(kr)) {
            std::vector<KnowledgeMaterial> kept;
            for (auto& m : kr.materials) {
                if (m.kind != "disambiguation") kept.push_back(std::move(m));
            }
            kr.materials.swap(kept);
        }
    } else {
        kr = knowledge_fetch(kind, name, kopts);
    }

    // 4. Step 2: (可选) ds_search「创作背景/评价/版本点评」检索（每实体最多 1 次）
    std::vector<SearchHit> valid_hits;
    std::vector<SearchHit> fetched_hits;
    std::vector<std::string> allowed_urls;
    for (const auto& m : kr.materials) {
        if (!m.url.empty()) allowed_urls.push_back(m.url);
    }

    if (opts.enable_search) {
        std::string query;
        // 2026-10-24（261024-01 #1）：官方 /v1/tools/search 的召回质量与 query 强相关。
        // 实测「专辑名 + 艺术家 + EP 发行 曲目 制作」比「背景 评价」命中多得多（评价类词会把结果带偏到乐评站）。
        if (kind == EntityKind::Release) {
            std::string qTarget = !opts.artist.empty() ? (opts.artist + " " + name) : name;
            // 艺术家串里的分隔符（"塞壬唱片-MSR / Ivoris"）会稀释 query，换成空格
            for (char& ch : qTarget) if (ch == '/' || ch == '&' || ch == '|' || ch == '+') ch = ' ';
            if (lang == "zh" || lang.empty()) {
                query = qTarget + " EP 专辑 发行时间 曲目 制作 语种 出版方";
            } else if (lang == "ja") {
                query = qTarget + " EP アルバム 発売日 収録曲 制作";
            } else {
                query = qTarget + " EP album release date tracklist credits";
            }
        } else {
            std::string qn = name;
            for (char& ch : qn) if (ch == '/' || ch == '&' || ch == '|' || ch == '+') ch = ' ';
            if (lang == "zh" || lang.empty()) {
                query = qn + " 歌手 音乐人 简介 国籍 风格 代表作品";
            } else if (lang == "ja") {
                query = name + " アーティスト プロフィール 出身 ジャンル 代表曲";
            } else {
                query = name + " artist musician biography genre discography";
            }
        }

        retrieval_rate_limiter().acquire();
        const DsSearchReply s_rep = ds_search(cfg, query, opts.max_search_hits > 0 ? opts.max_search_hits : 5);
        if (s_rep.ok) {
            std::vector<SearchHit> raw_hits;
            raw_hits.reserve(s_rep.hits.size());
            for (const auto& dh : s_rep.hits) {
                SearchHit sh;
                sh.title = dh.title;
                sh.url = dh.url;
                sh.snippet = dh.snippet;
                sh.summary = dh.summary;
                sh.date = dh.date;
                sh.rerankScore = dh.rerankScore;
                sh.authority = dh.authority;
                raw_hits.push_back(std::move(sh));
            }

            // 过滤与排序
            FilterResult fr = filter_hits(raw_hits, opts.filter_opts);
            valid_hits = std::move(fr.hits);

            // Step 3: ds_fetch 取前几页正文（每实体最多 4 次 fetch；官方 fetch 返回正文质量好，
            // 262024-01 #1 用户要求对齐官方 search 结果质量）
            const size_t fetch_count = std::min(valid_hits.size(),
                (size_t)std::max(1, std::min(opts.max_fetch_pages, 4)));
            for (size_t i = 0; i < fetch_count; ++i) {
                SearchHit fh = valid_hits[i];
                retrieval_rate_limiter().acquire();
                const DsFetchReply f_rep = ds_fetch(cfg, fh.url);
                if (f_rep.ok && !f_rep.content.empty()) {
                    fh.snippet = f_rep.content.substr(0, 1500);
                    if (!f_rep.title.empty()) fh.title = f_rep.title;
                }
                fetched_hits.push_back(std::move(fh));
            }
            for (size_t i = fetch_count; i < valid_hits.size(); ++i) {
                fetched_hits.push_back(valid_hits[i]);
            }
        }
    }

    // 将通过白名单与过滤的检索 URL 也纳入允许集合
    for (const auto& h : fetched_hits) {
        if (!h.url.empty()) allowed_urls.push_back(h.url);
    }

    // 若 L2 和 L3 均无材料：
    //   旧逻辑直接拒绝综合 → 用户实测「塞壬唱片」这类没有维基条目的实体，页面永远空白
    //   （261023-04 #14/#15：「一定要维基百科？那要 AI 干嘛？」）。
    //   现改为：有旧缓存先用缓存；否则若配了 key，退到 AI 自有知识整理（unverified=true，UI 必须标注），
    //   结果写缓存 7 天，第二次打开不再调用。有材料时仍走严格引用路径，不受影响。
    if (kr.materials.empty() && fetched_hits.empty()) {
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            cached.error = "未检索到任何权威材料，已回退至本地缓存";
            return cached;
        }
        if (!cfg.apiKey.empty()) {
            const std::string label = !opts.artist.empty() ? (opts.artist + " - " + name) : name;
            const std::string kind_label = (kind == EntityKind::Release) ? "专辑" : "艺术家";
            SummaryResult ai_res = summary_ai_only_generate(label, kind_label, lang, cfg);
            if (ai_res.ok) {
                summary_save_cache(baseDir, eid, lang, ai_res);
                return ai_res;
            }
        }
        SummaryResult err_res;
        err_res.ok = false;
        err_res.error = "未找到任何相关权威材料，AI 整理也未成功（防幻觉兜底）";
        return err_res;
    }

    // 5. Step 4: llm_chat 综合
    // 2026-10-23（261023-03 实测）：DeepSeek 偶发返回「缺 citations 数组 / 半截 JSON / 引用 url 为空」，
    // 单次调用失败就让页面空着、用户会以为功能坏了。这里**自动重试一次**（最多 2 次调用，
    // 只在失败时多花一次钱），仍失败才如实报错并回退旧缓存。
    // 另外 2000 tokens 对「专辑」类资料（维基正文 + Discogs 发行说明）会截断成半截 JSON，
    // 已提到 4000（实测揺れる想い/海阔天空通过）。
    const std::string promptEntityName = (!opts.artist.empty() && kind == EntityKind::Release)
        ? (opts.artist + " - " + name)
        : name;
    const std::vector<LlmMessage> msgs = build_synthesis_prompt(
        promptEntityName, kr.materials, fetched_hits, lang,
        (kind == EntityKind::Release) ? "专辑" : "艺术家");
    SummaryResult parsed;
    std::string last_err;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // 第二次不要原样重发（确定性失败会白花一次 4000-token 调用）：追加一条修复提示。
        std::vector<LlmMessage> attempt_msgs = msgs;
        if (attempt > 0) {
            attempt_msgs.push_back(LlmMessage{ "user",
                "上一次输出未通过校验（" + last_err + "）。请只输出严格合法的 JSON 对象："
                "必须包含 citations 数组（每条含 id/title/url/source，url 必须原样复制自上面材料的链接），"
                "sections 里每段至少 1 条引用；内容请精简到 3 段以内，不要任何 Markdown 或解释文字。" });
        }
        retrieval_rate_limiter().acquire();
        const LlmReply chat_rep = llm_chat(cfg, attempt_msgs, 4000, 0.2f, true);
        if (!chat_rep.ok) {
            last_err = "LLM 综合失败: " + chat_rep.error;
            continue;
        }
        parsed = parse_synthesis_reply(chat_rep.content, allowed_urls);
        if (parsed.ok) break;
        last_err = "综合结果校验失败: " + parsed.error;
    }
    if (!parsed.ok) {
        // 重试仍失败：有旧缓存则离线回退，否则如实报错（不伪造内容）
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            cached.error = "综合校验失败 (" + last_err + ")，已回退至本地缓存";
            return cached;
        }
        SummaryResult err_res;
        err_res.ok = false;
        err_res.error = last_err;
        return err_res;
    }

    // 设置元数据与落盘
    parsed.model = cfg.model;
    parsed.generated_at = now_sec;
    parsed.expires_at = now_sec + 7 * 86400; // TTL 7 天
    parsed.from_cache = false;
    parsed.offline = false;
    parsed.validation_ok = true;

    summary_save_cache(baseDir, eid, lang, parsed);

    return parsed;
}

inline SummaryResult retrieval_fetch(const std::string& kind_str,
                                     const std::string& name,
                                     const std::string& lang = "zh",
                                     const RetrievalOptions& opts = {}) {
    EntityKind k = EntityKind::Person;
    if (!entity_kind_from_tag(kind_str, k)) {
        k = EntityKind::Person;
    }
    return retrieval_fetch(k, name, lang, opts);
}

inline SummaryResult retrieval_fetch(const Entity& entity,
                                     const std::string& lang = "zh",
                                     const RetrievalOptions& opts = {}) {
    return retrieval_fetch(entity.kind, entity.name, lang, opts);
}

} // namespace rta::meta
