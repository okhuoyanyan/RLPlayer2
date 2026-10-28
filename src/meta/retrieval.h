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
#include "meta/prompt_profiles.h"   // 2026-10-27（261027-06 P1/P2/P3）：分类提示词 + 不过滤解析 + 名词外链
#include "meta/store.h"
#include "net/http_client.h"
#include "net/json.h"

#include <algorithm>
#include <chrono>
#include <future>
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
    // 2026-10-26：Search 带 need_content=true 时，官方直接 inline 返回正文（guides/search）。
    // 有 content 就不再单独调 fetch —— 少一次请求（¥5/千次）也少一次 RTT。
    std::string content;
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

// 2026-10-27（261027-06 P3）：名词 → 权威链接。只放确定性匹配出来的条目（种子表/材料 URL/页面标题），
// 匹配不上就不出现。前端可以直接渲染成外链；正文里同时会插一条「名词（url）」，
// 走既有 linkifyEscaped（裸 URL 变可点链接）—— 这样不改前端也能点开。
struct SummaryLink {
    std::string name;    // 命中的名词（原文写法）
    std::string url;     // 权威链接
    std::string source;  // wikipedia-zh / wikipedia-ja / wikipedia-en / moegirl / baike / discogs
    std::string kind;    // 人物 / 团体 / 场地 / 厂牌 / 作品 / 活动
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
    // 2026-10-24（261024-02 #1/#7）：分阶段耗时诊断（search/fetch/llm 各多少 ms），只用于日志与 A/B。
    std::string diag;
    int cache_ver = 0;   // 从磁盘缓存加载时的格式版本（0 = 不是从缓存来的 / 旧格式）
    // 2026-10-27（261027-06 P1/P3）：分类结果与名词外链
    std::string profile;         // 分类 id：classical / doujin / pop / rock / metal / jazz / ...
    std::string profile_label;   // 中文标签（古典/同人/...）
    std::vector<SummaryLink> links;   // 名词 → 权威链接（确定性匹配）
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
        // 2026-10-26（用户 261026-06 #2）：用户报「明明 QQ 音乐 / dizzylab 上有这张专辑，AI 却说找不到」。
        // 真因之一是本白名单**没有 dizzylab.net** —— 同人专辑的第一优先源，命中后被 filter_hits 当垃圾丢掉。
        // 这里补上同人/数字发行站点：dizzylab（同人）、Booth、OTOTOY、mora、Qobuz、HDtracks。
        "dizzylab.net", "booth.pm", "ototoy.jp", "mora.jp", "qobuz.com", "hdtracks.com",
        // VGMdb：同人/游戏/动画音乐最全的数据库（实测 authority=2~3），查同人专辑必用
        "vgmdb.net", "vgmdb.info",
        // 2026-10-28（真机实测发现的**第二处致命遗漏**）：
        //   把单曲搜索词改成「专辑+曲名」之后，命中最高的三条全被白名单丢掉：
        //     1. [1.30] touhou.arrangement-chronicle.com/.../Static World 会場限定CD
        //     2. [1.11] raw.githubusercontent.com/.../THBWiki-Markdown/.../Static_World_会場限定CD.md
        //     4. [0.58] thbwiki.cc/Static_World
        //   这三个是**东方同人圈最权威的资料来源**（THBWiki 是中文东方维基，
        //   arrangement-chronicle 是全量同人编曲数据库，VGMdb 只覆盖有商业登记的部分）。
        //   白名单不收 ⟹ 搜索结果被当成垃圾扔掉 ⟹ AI 又退回去靠记忆瞎编。
        //   这就是"搜索词对了但还是搜不到"的第二道坎。
        "thbwiki.cc", "thwiki.cc", "touhou.arrangement-chronicle.com",
        // THBWiki 的 Markdown 镜像托管在 GitHub raw 上（Delsin-Yu/THBWiki-Markdown）。
        // 只放行 raw 路径，不放行 github.com 主站（太泛，会把无关仓库放进来）。
        "raw.githubusercontent.com",
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
    // 2026-10-24（261024-01 #1）：实测 rerank 分布 0.87~1.66 → 降到 0.8。
    // 2026-10-26（261026-06 #2）**实测推翻上面这条**：同一 KEY 再打官方 search，
    //   query="Winged Step dizzylab" 的 8 条结果 rerank 只有 0.18/0.11/0.09/0.09/0.09/0.06/0.05/0.04；
    //   0.8 的闸把 8 条全判"低相关"（实测 pass=0），整条检索链等于空转 ——
    //   用户看到的"明明平台上有，AI 却说找不到"就是它。
    // 官方只保证 rerank_score 落在 0~3、>=1.5 为优质，没承诺每批都有 >=0.8 的条目。
    // ⟹ 不再拿绝对分数当硬闸（默认 0 = 不按分数砍）；质量交给 域名白名单 + authority +
    //    条数上限 + 提示词「材料没写就不许说」四重约束，分数只用于排序。
    double min_rerank_score = 0.0;
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
        "6. **简洁优先**：sections 最多 2 段、每段不超过 90 个汉字；标签信息优先放进 fields。"
        "（用户 261024-02 #1：资料整理太慢；输出越短越快，长文不是目标。）\n"
        "7. 结构化标签：JSON 还必须包含 \"fields\" 对象（键值都是字符串），用于界面上的「标签: 值」展示。"
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
        // 2026-10-27（回归 8.10）：旧解析器同样不许读时钟 —— 模型没给就当未知（0）。
        // 解析层是"回包的纯函数"，时间戳只由调用方在最后统一盖章（generated_at/expires_at）。

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
// 2026-10-27（用户 261027-06）P1「两步走 · 按类定制提示词」/ P2「输出不过滤」/ P3「名词外链」
//
// 旧路径的问题（用户原话「你过滤掉的信息，比展现的信息还多」）：
//   ① 提示词把可用字段写死成 10 个标签（field_schema_for_kind），模型被迫把 20 多条信息压成 5~6 条；
//   ② 硬门要求「每段至少 1 条引用 + 引用必须属于白名单」，一条不合规就整份资料被拒（内容全丢）；
//   ③ 分节（📀 专辑基本信息 / 🎵 曲目列表 / 💿 补充背景与解读）在缓存里，但前端只在 fields 为空时才渲染，
//      于是有标签时那些叙述全被丢掉。
// 新路径：
//   先 classify_profile（14 类）→ 按类给专用提示词并附上全部已知标签 → 模型输出什么就收什么
//   （标签自由、段落自由、引用可选）→ 名词自动挂权威链接。
// ---------------------------------------------------------------------------

// 分节是否镜像成标签行。当前前端 renderSummaryContent 只在 fields 为空时才渲染 sections，
// 所以必须把分节也放进 fields（标签=分节标题，值=段落文本），否则「💿 补充背景与解读」看不到。
// ⚠️ 一旦前端改成「fields 与 sections 同时渲染」，把这里返回 false，避免同一段内容出现两次。
inline bool summary_mirror_sections_into_fields() { return true; }

// 材料块（[M1]…）—— 新旧提示词共用
inline std::string summary_materials_block(const std::vector<KnowledgeMaterial>& materials,
                                           const std::vector<SearchHit>& hits) {
    std::ostringstream u;
    int idx = 1;
    for (const auto& m : materials) {
        u << "\n[M" << idx << "] 来源: " << m.source
          << " | 标题: " << m.title
          << " | 链接: " << m.url
          << " | 取回时间: " << m.fetched_at << "\n"
          << "正文:\n" << m.text << "\n";
        idx++;
    }
    for (const auto& h : hits) {
        // 2026-10-27（真实调用反馈）：必须把「这是什么页」告诉模型 —— 否则它会把 VGMdb 的
        // 组织页（/org/5044，22 条同团体目录）当成这张专辑的材料，字段全填成目录列表。
        const int rank = pp_page_kind_rank(h.url, h.title);
        u << "\n[M" << idx << "] 来源: search-fetch"
          << " | 标题: " << h.title
          << " | 链接: " << h.url
          << " | 日期: " << h.date
          << " | 页面类型: " << pp_page_kind_name(rank) << "\n"
          << (rank <= -1 ? "（注意：这是组织/艺术家页，不是本实体的专辑页；只能用来确认团体名与目录前缀，"
                           "不得把它的发行列表当成本专辑的曲目/日期/售价）\n" : "")
          << "摘要/正文:\n" << (h.snippet.empty() ? h.summary : h.snippet) << "\n";
        idx++;
    }
    return u.str();
}

// 分类信号：已知标签 + 材料正文（Discogs 的「流派与风格/乐团/指挥/厂牌」就是最好的分类线索）
inline ProfileSignal summary_make_signal(const std::string& title,
                                         const std::string& artist,
                                         const std::vector<KnowledgeMaterial>& materials,
                                         const std::vector<SearchHit>& hits,
                                         const std::vector<std::pair<std::string, std::string>>& album_tags,
                                         const std::vector<std::string>& track_titles,
                                         const std::string& genre_hint) {
    ProfileSignal sig;
    sig.title = title;
    sig.artist = artist;
    sig.hint = genre_hint;
    sig.tracks = track_titles;
    sig.track_count = (int)track_titles.size();
    for (const auto& kv : album_tags) {
        const std::string k = pp_lower_ascii(kv.first);
        if (k.find("genre") != std::string::npos || kv.first.find("流派") != std::string::npos ||
            kv.first.find("风格") != std::string::npos) {
            sig.genres.push_back(kv.second);
        } else if (k.find("label") != std::string::npos || kv.first.find("厂牌") != std::string::npos ||
                   kv.first.find("出版") != std::string::npos) {
            if (sig.label.empty()) sig.label = kv.second;
        } else if (k.find("catalog") != std::string::npos || kv.first.find("编号") != std::string::npos) {
            if (sig.catalog.empty()) sig.catalog = kv.second;
        }
    }
    std::ostringstream mt;
    for (const auto& m : materials) {
        mt << m.title << "\n" << m.text.substr(0, 4000) << "\n";
    }
    for (const auto& h : hits) {
        mt << h.title << "\n" << (h.snippet.empty() ? h.summary : h.snippet).substr(0, 800) << "\n";
    }
    sig.material_text = mt.str();
    return sig;
}

// 按 profile 生成的综合提示词（替代旧的固定 schema 提示词）
inline std::vector<LlmMessage> build_profile_synthesis_prompt(
    const ProfileDecision& dec,
    const std::string& entity,
    const std::string& kindLabel,
    const std::vector<KnowledgeMaterial>& materials,
    const std::vector<SearchHit>& hits,
    const std::vector<std::pair<std::string, std::string>>& known_tags = {}) {
    const ProfilePrompt pp = build_profile_prompt(dec, entity, kindLabel,
                                                  summary_materials_block(materials, hits), known_tags);
    return { LlmMessage{ "system", pp.system }, LlmMessage{ "user", pp.user } };
}

// ProfileDoc → SummaryResult：doc 里的每一条都进结果（不排序、不裁剪标签、不因为没引用而丢内容）
inline SummaryResult summary_from_profile_doc(const ProfileDoc& doc, const ProfileDecision& dec) {
    SummaryResult sr;
    if (!doc.ok) {
        sr.error = doc.error.empty() ? "模型输出无法解析" : doc.error;
        return sr;
    }
    sr.disclaimer = doc.disclaimer.empty() ? "AI 整理，仅供参考" : doc.disclaimer;
    sr.profile = music_profile_id(dec.profile);
    sr.profile_label = music_profile_label(dec.profile);

    sr.fields = doc.fields;
    for (const auto& s : doc.sections) {
        SummarySection sec;
        sec.heading = s.heading;
        for (const auto& p : s.paragraphs) {
            SummaryParagraph sp;
            sp.text = p.text;
            sp.citations = p.citations;
            sec.paragraphs.push_back(std::move(sp));
        }
        sr.sections.push_back(std::move(sec));
    }
    for (const auto& c : doc.citations) {
        SummaryCitation sc;
        sc.id = c.id;
        sc.title = c.title;
        sc.url = c.url;
        sc.source = c.source;
        // 2026-10-27（合并后回归 8.10 的真因）：**原样保留**模型给的 fetched_at；没给就是 0。
        // 绝不在这里读时钟 —— 否则同一个回包转两次结果不同，"传/不传 ProgressFn 逐字段相同"
        // 会跨秒随机失败（实测 a=1793108892 b=1793108894，同一次测试内两次调用）。
        // 纪律：解析/转换层必须是**回包的纯函数**。
        sc.fetched_at = c.fetched_at;
        sr.citations.push_back(std::move(sc));
    }
    // 分节镜像成标签行（见 summary_mirror_sections_into_fields 的注释）
    if (summary_mirror_sections_into_fields()) {
        for (const auto& sec : sr.sections) {
            // 曲目列表统一用「曲目列表」这个键名：前端 renderFieldGrid 在**本地有曲目**时
            // 会 drop 掉这个键（改用本地对齐表格），不归一化就会出现「本地表格 + AI 文本」两份列表。
            std::string label = sec.heading;
            const bool is_tracklist =
                (sec.heading.find("曲目列表") != std::string::npos ||
                 sec.heading.find("収録曲") != std::string::npos ||
                 pp_lower_ascii(sec.heading).find("tracklist") != std::string::npos ||
                 pp_lower_ascii(sec.heading).find("track list") != std::string::npos);
            if (is_tracklist) label = "曲目列表";
            bool exists = false;
            for (const auto& kv : sr.fields) {
                if (kv.first == label) { exists = true; break; }
            }
            if (exists) continue;
            std::string body;
            for (const auto& p : sec.paragraphs) {
                if (!body.empty()) body += "\n\n";
                body += p.text;
            }
            if (body.empty()) continue;
            sr.fields.emplace_back(label, body);
        }
    }
    // 引用可选：一条都没有（或全被白名单丢掉）时如实标 unverified，UI 会写「AI 整理」。
    // 2026-10-27（真实调用反馈）：materials_used 与 citations 语义对齐 ——
    // 它就是「实际被引用的 URL 集合」，不能一边有 citations 一边空着。
    {
        std::set<std::string> used;
        for (const auto& c : sr.citations) if (!c.url.empty()) used.insert(c.url);
        sr.materials_used.assign(used.begin(), used.end());
    }
    sr.unverified = sr.citations.empty();
    sr.validation_ok = true;
    sr.ok = true;
    return sr;
}

// 命中里是否有「主材料」（专辑/作品页）。全是组织/艺术家页时，材料对本实体基本没用 ——
// 真实案例：Static World 会场限定CD 只拿到 vgmdb.net/org/5044（22 条同团体目录），
// 模型于是把目录当内容。这里用来决定要不要再补一次「专辑页导向」的站点限定搜索。
inline bool summary_has_primary_material(const std::vector<SearchHit>& hits) {
    for (const auto& h : hits) {
        if (pp_page_kind_rank(h.url, h.title) >= 2) return true;
    }
    return false;
}

// 链接索引：种子表（人工核对过 URL）+ 材料/检索命中的 URL 与页面标题
inline LinkIndex summary_link_index(const std::vector<KnowledgeMaterial>& materials,
                                    const std::vector<SearchHit>& hits) {
    LinkIndex idx;
    idx.add_seed_table();
    for (const auto& m : materials) {
        if (m.url.empty()) continue;
        idx.add_from_url(m.url, m.title);
        // 2026-10-27（真实调用反馈 links=[]）：材料的**页面标题**就是正文里出现的那个名词
        //（如「Static World - VGMdb」→「Static World」），必须单独加进索引，
        // 否则正文里的团体名/专辑名一个都连不上。标题与 URL 同源，属确定关系。
        const std::string t = pp_strip_source_suffix(m.title);
        if (!t.empty() && t.size() <= 60) idx.add(t, m.url, m.source.empty() ? "material" : m.source, "名词");
        if (!m.subject.empty()) idx.add(m.subject, m.url, m.source.empty() ? "material" : m.source, "名词");
    }
    for (const auto& h : hits) {
        if (h.url.empty()) continue;
        idx.add_from_url(h.url, h.title);
        const std::string t = pp_strip_source_suffix(h.title);
        if (!t.empty() && t.size() <= 60) idx.add(t, h.url, "search", "名词");
    }
    return idx;
}

// 把结果里的名词挂上权威链接：正文插「名词（url）」，结构化的 links[] 另行回给前端。
inline void summary_apply_links(SummaryResult& sr, const LinkIndex& idx, int max_per_field = 6) {
    if (idx.empty()) return;
    auto remember = [&sr](const std::vector<LinkMatch>& ms) {
        for (const auto& m : ms) {
            if (!m.hit) continue;
            bool exists = false;
            for (const auto& l : sr.links) {
                if (l.name == m.name) { exists = true; break; }
            }
            if (exists) continue;
            SummaryLink l;
            l.name = m.name;
            l.url = m.url;
            l.source = m.source;
            l.kind = m.kind;
            sr.links.push_back(std::move(l));
        }
    };
    for (auto& kv : sr.fields) {
        // 已经是 URL 的值不再重复挂链接（「关联」「链接」这类字段本来就是网址）
        if (kv.second.rfind("http", 0) == 0) continue;
        std::vector<LinkMatch> ms;
        kv.second = linkify_text(kv.second, idx, max_per_field, &ms);
        remember(ms);
    }
    for (auto& sec : sr.sections) {
        for (auto& p : sec.paragraphs) {
            std::vector<LinkMatch> ms;
            p.text = linkify_text(p.text, idx, 3, &ms);
            remember(ms);
        }
    }
}

// 2026-10-27（父批准 #2）：**取回时刻由调用方盖章**。
// 分层纪律：
//   * 解析层（parse_profile_reply / summary_from_profile_doc / parse_synthesis_reply）
//     必须是"回包的纯函数"：模型给了 fetched_at 就透传，没给就是 0，**绝不读时钟**；
//   * 只有本函数（在 retrieval_fetch / retrieval_fetch_track 里、写缓存之前调用）才读时钟。
// 为什么盖在**写缓存之前**：缓存存的就是用户会看到的那份结果，时间戳若留到返回后再盖，
// 命中缓存的那次就会显示"未知取回时间"，同一张专辑两次打开看到的元数据不一致。
// 盖上之后再入缓存 ⟹ 首次生成的取回时刻被冻结，后续缓存命中显示同一个时刻（可复现）。
inline void summary_stamp_citations(SummaryResult& sr, long long now_sec = 0) {
    if (sr.citations.empty()) return;
    if (now_sec <= 0) now_sec = (long long)std::time(nullptr);
    for (auto& c : sr.citations) {
        if (c.fetched_at <= 0) c.fetched_at = now_sec;
    }
}

// 一条龙：宽松解析 + 转 SummaryResult + 挂链接
inline SummaryResult summary_build_from_profile_reply(const std::string& reply,
                                                      const ProfileDecision& dec,
                                                      const std::vector<std::string>& allowed_urls,
                                                      const LinkIndex& idx) {
    // 有白名单才校验（单曲路径没有权威材料集合，硬校验会把引用全丢掉）
    ProfileDoc doc = parse_profile_reply(reply, allowed_urls, !allowed_urls.empty());
    SummaryResult sr = summary_from_profile_doc(doc, dec);
    if (sr.ok) summary_apply_links(sr, idx, 6);
    return sr;
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
      // 2026-10-24（261024-01 #4）：缓存格式版本。v2 起才有结构化 fields；旧缓存（无 v 或 v<2）
      // 在读取时直接判过期，让下次请求重新生成 —— 否则升级后老用户永远看不到标签网格。
      // 2026-10-27（261027-06）：v4 起带 profile（分类）与 links（名词外链）。v3 缓存仍可读，
      // 只是没有这两项 —— 不强制重生成，避免升级后把用户已付过钱的资料全部作废。
      //
      // 2026-10-28（用户真机抓到的真事故，v5 的唯一理由）：
      //   提示词新增规则 9「引用外部页面必须先核对归属」。v4 及以前生成的缓存里可能含这类错误断言：
      //     「所属专辑：网易云音乐专辑页 id=148064894 收录本曲（专辑名与艺术家未能确证）」
      //   而那个页面实际是完全不相干的《火花》。这类错误带可点击链接、看起来像已核实的事实，
      //   比"没查到"更有害 ⟹ v4 缓存一律不复用，下次打开重新生成一次。
      //   （代价：已整理过的资料会重算一次，消耗一次额度。用户已确认接受。）
      << "  \"v\": 6,\n"
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
      << "  \"profile\": \"" << meta_jesc(sr.profile) << "\",\n"
      << "  \"profile_label\": \"" << meta_jesc(sr.profile_label) << "\",\n"
      << "  \"links\": [";
    for (size_t i = 0; i < sr.links.size(); ++i) {
        const auto& l = sr.links[i];
        o << (i ? ", " : "")
          << "{\"name\": \"" << meta_jesc(l.name) << "\", \"url\": \"" << meta_jesc(l.url)
          << "\", \"source\": \"" << meta_jesc(l.source) << "\", \"kind\": \"" << meta_jesc(l.kind) << "\"}";
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
    // 2026-10-24（261024-01 #4）：v2 之前的缓存没有结构化标签 → 视为已过期，重新生成一次
    //（否则升级后老用户永远看不到标签网格）。
    sr_out.cache_ver = (int)j["v"].n();
    // v3：消歧义过滤 + 400 降级链 + 输出精简（旧内容可能质量差/来自误判，统一重生成一次）
    // 注意用 1（1970 年）而不是 0：summary_is_expired 对 expires_at<=0 会回退到 generated_at+7d，
    // 设 0 等于"永不过期"（2026-10-24 实测：缓存 v2 文件一直 cache-hit，v3 失效没生效）。
    if ((int)j["v"].n() < 3) sr_out.expires_at = 1;
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

    // profile / links（v4；v3 缓存没有这两项 → 空，前端照旧渲染）
    sr_out.profile = j["profile"].s();
    sr_out.profile_label = j["profile_label"].s();
    sr_out.links.clear();
    {
        const rta::net::Json& larr = j["links"];
        if (larr.type == rta::net::Json::Arr) {
            for (size_t i = 0; i < larr.size(); ++i) {
                const auto& item = larr.at(i);
                if (item.type != rta::net::Json::Obj) continue;
                SummaryLink l;
                l.name = item["name"].s();
                l.url = item["url"].s();
                l.source = item["source"].s();
                l.kind = item["kind"].s();
                if (!l.name.empty() && !l.url.empty()) sr_out.links.push_back(std::move(l));
            }
        }
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
    // 2026-10-26（用户 261026-06 #2）：主搜索一条都没过闸时，再打一次「平台站点限定」搜索。
    // 空串 = 关闭。site: 是官方支持的查询语法（https://api-docs.deepseek.com/guides/search/ ）。
    bool enable_site_fallback = true;
    // 2026-10-27（真实调用反馈）：补 vgmdb.net / vgmdb.info —— 同人与游戏音乐最全的数据库，
    // 而且**专辑页 URL 形如 /album/<id>**，比组织页有用得多（见 pp_page_kind_rank）。
    std::string site_fallback_sites =
        "site:vgmdb.net OR site:y.qq.com OR site:dizzylab.net OR site:music.163.com OR site:bandcamp.com";
    // 2026-10-26（用户 261026-06 #2）：没有任何材料时，先让**模型自己**用 web_search/web_fetch
    // 查一轮（function calling），再据查到的内容写简介；查不到才退回纯记忆。
    bool enable_web_tools = true;
    int web_tool_rounds = 3;          // 最多几轮 chat（每轮可带多个工具调用）
    int web_tool_budget_ms = 15000;   // 工具环路总预算（超时即收手，不阻塞主流程）
    LlmConfig llm_cfg;
    FilterOptions filter_opts;
    std::string artist;           // 针对 Release 的艺术家辅助（用于 Wikipedia 与 Discogs 检索消歧）
    // 2026-10-28（用户真机：AI 把《19:30》归到了完全不相干的网易云专辑《火花》上）：
    //   **曲目所属的专辑名**。单曲检索必须带上它，否则裸曲名的搜索结果必然是垃圾 ——
    //   "19:30" 这种时间型曲名全网同名无数；而本库的 WAV/自抓文件普遍**没有任何标签**
    //   （artist/album/title 全空），旧代码于是只拿曲名 + 「歌曲 作词 作曲 编曲 发行」去搜，
    //   命中什么全看运气。有了 album 就能组成
    //     "Static World 会场限定CD 19:30 Static World" ⟹ 唯一指向这张同人碟。
    //   传空表示调用方不知道专辑（保持旧行为）。
    std::string album;
    std::string entity_id;        // 显式指定实体 ID（为空则自动计算）
    // ---- 2026-10-27（用户 261027-06 P1：「把所有专辑标签都交给模型」）----
    // 这些都是**可选**输入：rtaweb 现在只传 artist/entity_id，因此生产上标签多为空，
    // 分类主要靠材料正文（Discogs 的『流派与风格/乐团/指挥/厂牌』）+ 标题 + 曲目名 —— 够用。
    // 一旦调用方把本地标签接进来，分类精度与提示词信息量会再上一档。
    bool use_profile_prompts = true;                              // false = 回退旧的固定 schema 提示词
    std::vector<std::pair<std::string, std::string>> album_tags;  // 已知标签（流派/厂牌/目录号/…）
    std::vector<std::string> track_titles;                        // 曲目名（同人：原曲名常在曲名里）
    std::string genre_hint;                                       // 直接指定分类（如 "classical" / "古典"）
};

// ---------------------------------------------------------------------------
// 生成进度回调（用户反馈 261024-05：LLM 流式，别让用户枯等）
//
// 纪律（与 llm.h 的流式契约配套）：
//   1) 回调可能在**任意线程**被调用（真流式时是传输层网络线程）。SummaryProgressEmitter
//      用 mutex 保护全部状态，并在锁内拷贝一份快照再调回调 —— 回调只拿到自己的副本，
//      生产者从不读回调侧状态（只写不读），也不会因用户回调重入而死锁。
//   2) 不传回调（默认）时 retrieval_fetch 走的仍是原来的 llm_chat 非流式路径，
//      返回值逐字段不变（现有 test_retrieval / test_metacache 回归锁死）。
//   3) 重试从头开始：每次尝试独立开一条流、独立 chars/partial（attempt 递增），
//      绝不把上一次的半截拼到这一次后面。
// ---------------------------------------------------------------------------

enum class SummaryPhase { Idle = 0, Retrieving, Generating, Done, Failed };

inline const char* summary_phase_name(SummaryPhase p) {
    switch (p) {
        case SummaryPhase::Idle:       return "idle";
        case SummaryPhase::Retrieving: return "retrieval";
        case SummaryPhase::Generating: return "generate";
        case SummaryPhase::Done:       return "done";
        case SummaryPhase::Failed:     return "failed";
    }
    return "idle";
}

struct SummaryProgress {
    SummaryPhase phase = SummaryPhase::Idle;
    std::string phase_name;   // 同上（"retrieval"/"generate"/...），方便日志与 JSON 直出
    std::string detail;       // "knowledge+search" / "search hits=5" / "llm attempt=0" ...
    int attempt = 0;          // 生成尝试序号（0 = 首次；重试递增 → UI 收到新 attempt 应清空 partial）
    size_t chars = 0;         // 本次尝试已生成的**字节**数（== partial.size()，UTF-8）
    int delta_count = 0;      // 本次尝试已收到的增量次数
    std::string partial;      // 本次尝试已生成的部分文本（原始 JSON 片段；可能以半个多字节字符结尾）
    long long elapsed_ms = 0; // 距本次阶段开始的毫秒数
};

using ProgressFn = std::function<void(const SummaryProgress&)>;

class SummaryProgressEmitter {
public:
    explicit SummaryProgressEmitter(ProgressFn fn) : fn_(std::move(fn)) {}
    bool enabled() const { return static_cast<bool>(fn_); }

    // 阶段切换（检索/生成/完成/失败）
    void phase(SummaryPhase ph, const std::string& detail = {}) {
        SummaryProgress snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            phase_ = ph;
            detail_ = detail;
            phase_start_ = std::chrono::steady_clock::now();
            snap = snapshot_locked();
        }
        deliver(snap);
    }

    // 新一次生成尝试：清零 chars/partial/delta 并递增 attempt —— 重试必须从头开始
    void begin_attempt(int attempt, const std::string& detail = {}) {
        SummaryProgress snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            phase_ = SummaryPhase::Generating;
            detail_ = detail;
            attempt_ = attempt;
            chars_ = 0;
            delta_count_ = 0;
            partial_.clear();
            phase_start_ = std::chrono::steady_clock::now();
            snap = snapshot_locked();
        }
        deliver(snap);
    }

    // 一段增量文本（原样追加；不做 UTF-8 对齐 —— 完整文本由 llm_chat_stream 拼接）
    void delta(const std::string& text) {
        if (text.empty()) return;
        SummaryProgress snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            partial_ += text;
            chars_ = partial_.size();
            ++delta_count_;
            snap = snapshot_locked();
        }
        deliver(snap);
    }

private:
    SummaryProgress snapshot_locked() const {
        SummaryProgress s;
        s.phase = phase_;
        s.phase_name = summary_phase_name(phase_);
        s.detail = detail_;
        s.attempt = attempt_;
        s.chars = chars_;
        s.delta_count = delta_count_;
        s.partial = partial_;
        s.elapsed_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - phase_start_).count();
        return s;
    }
    void deliver(const SummaryProgress& s) {
        if (!fn_) return;
        try { fn_(s); } catch (...) { }   // UI 回调抛异常不得打断生成
    }

    ProgressFn fn_;
    mutable std::mutex mtx_;
    SummaryPhase phase_ = SummaryPhase::Idle;
    std::string detail_;
    int attempt_ = 0;
    size_t chars_ = 0;
    int delta_count_ = 0;
    std::string partial_;
    std::chrono::steady_clock::time_point phase_start_ = std::chrono::steady_clock::now();
};

// 统一聊天入口：有进度回调 → 流式（逐 delta 推进度）；没有回调 → 原来的 llm_chat
// （非流式，行为逐字节不变 —— 这是"不传回调时完全向后兼容"的唯一保证点）。
inline LlmReply retrieval_chat(const LlmConfig& cfg, const std::vector<LlmMessage>& msgs,
                               int maxTokens, float temperature, bool jsonObject,
                               SummaryProgressEmitter& progress) {
    if (!progress.enabled()) return llm_chat(cfg, msgs, maxTokens, temperature, jsonObject);
    const LlmStreamReply s = llm_chat_stream(cfg, msgs,
        [&progress](const std::string& d) { progress.delta(d); },
        maxTokens, temperature, jsonObject);
    LlmReply r;
    r.ok = s.ok;
    r.content = s.content;
    r.error = s.error;
    r.totalTokens = s.totalTokens;
    return r;
}


// ---------------------------------------------------------------------------
// 工具检索取证（2026-10-26，用户 261026-06 #2）
//
// 场景：L2（维基/萌娘/Discogs）与 L3（固定流水线 search+fetch）都没捞到东西时，
// 旧实现直接让模型「凭记忆写」→ 于是「明明 QQ 音乐 / dizzylab 上有这张专辑，AI 却说找不到」。
// 这里改成先让模型自己决定搜什么、抓哪页（llm_chat_tool_loop + web_search/web_fetch），
// 把工具返回的事实作为材料交给下一步生成；工具全挂就返回 ok=false，调用方退回纯记忆兜底。
//
// 纪律：预算硬上限（budget_ms），失败只写 diag、绝不抛、绝不阻塞主流程。
// ---------------------------------------------------------------------------

struct WebEvidence {
    bool ok = false;
    std::string text;     // 给生成阶段看的材料（模型自己写的要点 + 来源）
    std::vector<std::string> urls;   // 工具实际命中的页面（可进 allowed_urls）
    std::string diag;
    std::string error;
    int toolCalls = 0;
};

// 从工具结果 JSON 里抽 results[].url（失败返回空）
inline std::vector<std::string> web_evidence_urls(const std::vector<LlmToolTrace>& trace) {
    std::vector<std::string> out;
    for (const auto& tr : trace) {
        if (!tr.ok) continue;
        const rta::net::Json j = rta::net::json_parse(tr.result);
        if (j.type != rta::net::Json::Obj) continue;
        if (j.has("results") && j["results"].type == rta::net::Json::Arr) {
            const auto& arr = j["results"];
            for (size_t i = 0; i < arr.size(); ++i) {
                const std::string u = arr.at(i)["url"].s();
                if (!u.empty() && std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
            }
        }
        const std::string self = j["url"].s();
        if (!self.empty() && std::find(out.begin(), out.end(), self) == out.end()) out.push_back(self);
    }
    return out;
}

inline WebEvidence retrieval_web_evidence(const LlmConfig& cfg,
                                          const std::string& entityName,
                                          const std::string& kindLabel,
                                          const std::string& lang,
                                          int maxRounds = 3,
                                          int budgetMs = 20000) {
    WebEvidence ev;
    if (cfg.apiKey.empty()) { ev.error = "未配置 api_key（rlplayer-llm.conf）"; return ev; }
    const bool zh = (lang.empty() || lang == "zh");
    std::string sys =
        "你是联网检索助手。你只有两个工具：web_search（搜网页）与 web_fetch（抓网页正文）。\n"
        "【任务】用工具查证下面这个" + kindLabel + "的客观事实：发行时间、曲目、艺术家/制作人员、出版方、"
        "风格、简介。\n"
        "【硬规则】\n"
        "1. 必须先调用 web_search；查不到再换关键词（可加 site: 语法限定站点，例如 "
        "site:y.qq.com OR site:dizzylab.net OR site:music.163.com OR site:bandcamp.com）。\n"
        "2. 只有工具返回的内容才算事实。**不许**用你自己的记忆补年份、曲目、厂牌、销量。\n"
        "3. 找到看起来对的平台页就用 web_fetch 读全文，再据此作答。\n"
        "4. 最终回答：先用 3~6 条要点列出「查到的事实」，每条后面用括号写出处 URL；"
        "确实查不到的写「未查到」。不要写与实体无关的内容。\n";
    std::string user = std::string("实体：") + (zh ? "" : "") + "【" + entityName + "】（" + kindLabel + "）\n";
    user += zh ? "请开始检索并给出要点。" : "Please search and report the key facts.";
    std::vector<LlmMessage> msgs{ LlmMessage{ "system", sys }, LlmMessage{ "user", user } };

    const LlmToolLoopResult loop = llm_chat_tool_loop(cfg, msgs, /*maxTokens=*/900,
                                                      /*temperature=*/0.2f, maxRounds, budgetMs,
                                                      /*toolTimeoutMs=*/12000);
    ev.toolCalls = loop.toolCalls;
    ev.diag = loop.diag;
    ev.urls = web_evidence_urls(loop.trace);
    if (!loop.ok) {
        ev.error = loop.error;
        // 即使模型没给出总结，只要工具真的返回过内容，也可以把原始片段当材料用
        if (!ev.urls.empty()) {
            std::string raw;
            for (const auto& tr : loop.trace) {
                if (!tr.ok || tr.result.empty()) continue;
                raw += "- " + llm_truncate_utf8(tr.result, 1500) + "\n";
            }
            if (!raw.empty()) { ev.ok = true; ev.text = raw; }
        }
        return ev;
    }
    ev.text = llm_truncate_utf8(loop.content, 4000);
    ev.ok = !ev.text.empty() && loop.toolCalls > 0;
    if (!ev.ok && ev.text.empty()) ev.error = "模型未产出要点";
    return ev;
}

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

// 2026-10-27（261027-06）：AI 自有知识路径也要按 profile 说话（分类 + 专用提示词 + 名词外链）。
struct ProfileContext {
    ProfileDecision decision;
    LinkIndex links;
    bool valid = false;
};

inline std::vector<LlmMessage> build_ai_only_prompt(const std::string& entityLabel,
                                                    const std::string& kindLabel,
                                                    const std::string& lang,
                                                    const std::string& evidence = std::string()) {
    (void)lang;
    const bool has_ev = !evidence.empty();
    std::string sys =
        std::string("你是严谨的音乐资料编辑。") +
        (has_ev ? "下面有【联网检索材料】（web_search / web_fetch 工具返回），请以材料为准写介绍。\n"
                : "当前没有任何可引用的权威材料，请仅凭你已有的知识为实体写一段简短介绍。\n") +
        (has_ev ? ("【联网检索材料】\n" + evidence + "\n【材料结束】\n") : std::string()) +
        "【纪律】\n"
        "1. 只写你有把握的共性信息（风格类型、大致活动年代、代表作品方向、行业位置）；\n"
        "2. 严禁编造可核查的精确数据：具体发行日期、销量数字、榜单名次、成员真实姓名、获奖年份等，"
        + std::string(has_ev
            ? "材料里明确给出的可以写（并尽量在句末标注来源域名）；材料没给的不要补；\n"
            : "如果没有十足把握就不要写；\n")
        + "3. 不确定就写「资料有限」，不要用模糊话术凑字数；\n"
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
                                              const LlmConfig& cfg,
                                              ProgressFn on_progress = nullptr,
                                              const std::string& evidence = std::string(),
                                              const ProfileContext* ctx = nullptr) {
    SummaryProgressEmitter progress(on_progress);   // 拷贝（不是 move）：嵌套兜底路径还要复用同一个回调
    // 2026-10-27（261027-06）：有 ctx 就按分类提示词写（"不过滤"同样适用），没有就走旧的简短简介。
    const bool use_profile = (ctx && ctx->valid);
    std::vector<LlmMessage> base_msgs;
    if (use_profile) {
        std::string material;
        if (!evidence.empty()) {
            material = "\n[M1] 来源: web-tools | 标题: 联网检索（web_search/web_fetch） | 链接: \n正文:\n" + evidence + "\n";
        }
        const ProfilePrompt pp = build_profile_prompt(ctx->decision, entityName + (kindLabel.empty() ? "" : ("（" + kindLabel + "）")),
                                                      kindLabel, material, {});
        base_msgs = { LlmMessage{ "system", pp.system }, LlmMessage{ "user", pp.user } };
    } else {
        base_msgs = build_ai_only_prompt(entityName, kindLabel, lang, evidence);
    }
    SummaryResult parsed;
    std::string last_err;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<LlmMessage> msgs = base_msgs;
        if (attempt > 0) {
            msgs.push_back(LlmMessage{ "user",
                "上一次输出不合规（" + last_err + "）。请只输出严格 JSON：" +
                "{\"disclaimer\":\"AI 整理，仅供参考\",\"fields\":{\"标签\":\"值\"},"
                "\"sections\":[{\"heading\":\"📀 基本信息\",\"paragraphs\":[\"……\"]}]}，不要任何其它文字。" });
        }
        retrieval_rate_limiter().acquire();
        progress.begin_attempt(attempt, "ai_only attempt=" + std::to_string(attempt));
        const LlmReply rep = retrieval_chat(cfg, msgs, use_profile ? 3200 : 2000, 0.3f, true, progress);
        if (!rep.ok) { last_err = rep.error; continue; }
        if (use_profile) {
            ProfileDoc doc = parse_profile_reply(rep.content);
            parsed = summary_from_profile_doc(doc, ctx->decision);
            if (parsed.ok) summary_apply_links(parsed, ctx->links, 6);
        } else {
            parsed = parse_ai_only_reply(rep.content);
        }
        if (parsed.ok) break;
        last_err = parsed.error;
    }
    if (parsed.ok) {
        parsed.model = cfg.model;
        parsed.generated_at = (long long)std::time(nullptr);
        parsed.expires_at = parsed.generated_at + 7 * 86400;
        parsed.from_cache = false;
        parsed.offline = false;
        // 有联网检索材料时：仍然 unverified（没做逐段引用校验），但标注要如实说明「含联网检索」
        if (!evidence.empty()) parsed.disclaimer = "AI 整理（含联网检索工具结果，未逐段校验）";
        if (use_profile) parsed.unverified = true;   // 无逐段引用校验 → 必须如实标注
        progress.phase(SummaryPhase::Done, "ai_only ok");
    } else {
        progress.phase(SummaryPhase::Failed, last_err);
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
                                           const RetrievalOptions& opts,
                                           ProgressFn on_progress = nullptr) {
    SummaryProgressEmitter progress(on_progress);   // 拷贝（不是 move）：嵌套兜底路径还要复用同一个回调
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

    progress.phase(SummaryPhase::Retrieving, "track search");
    // 2026-10-24（261024-02 #1/#7）：分阶段计时（与专辑/艺术家路径同口径，日志可对比）
    const auto t_trk0 = std::chrono::steady_clock::now();
    long long trk_search_ms = 0, trk_fetch_ms = 0, trk_llm_ms = 0;
    std::string trk_ev_diag;   // 工具取证这一段的诊断（261026-06 #2）
    // 2026-10-24（261024-01 #1）：歌曲级资料也走**官方 search + fetch**，
    // 而不是只凭模型记忆（用户对"AI 只会瞎编"的批评就来自这里）。
    std::vector<SearchHit> hits;
    if (opts.enable_search) {
        std::string query;
        // 2026-10-28（用户真机）：单曲检索的前缀里**必须带上所属专辑名**。
        //   实测反例：《Static World 会场限定CD》的 02「19：30」是**无任何标签的 WAV**，
        //   artist 为空 ⟹ 旧代码只搜「19:30 歌曲 作词 作曲 编曲 发行」，
        //   结果模型把这首挂到了完全不相干的网易云专辑《火花》（id=148064894）上。
        //   组成「专辑 + 曲名 + 艺术家」之后，指向性从"全网红同名"变成"这一张碟的这一首"。
        //   顺序上把专辑放最前：搜索引擎对前置词权重更高。
        std::string qBase;
        if (!opts.album.empty()) qBase += opts.album;
        if (!title.empty()) { if (!qBase.empty()) qBase += ' '; qBase += title; }
        // 艺术家只有在「专辑与曲名里都没有它」时才追加，避免又一个重复词（见 Release 分支的同款处理）
        if (!artist.empty() && (opts.album + ' ' + title).find(artist) == std::string::npos) {
            if (!qBase.empty()) qBase += ' ';
            qBase += artist;
        }
        if (qBase.empty()) qBase = title;
        if (lang == "zh" || lang.empty()) {
            query = qBase + " 歌曲 作词 作曲 编曲 发行";
        } else if (lang == "ja") {
            query = qBase + " 楽曲 作詞 作曲 編曲";
        } else {
            query = qBase + " song credits release lyrics";
        }
        retrieval_rate_limiter().acquire();
        const DsSearchReply s_rep = ds_search(cfg, query, opts.max_search_hits > 0 ? opts.max_search_hits : 8);
        trk_search_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_trk0).count();
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
            // 2026-10-27（同专辑路径）：单曲也按「曲目页 > 组织/专辑页」重排，别把组织页当主材料
            std::stable_sort(hits.begin(), hits.end(), [](const SearchHit& a, const SearchHit& b) {
                return pp_page_kind_rank(a.url, a.title) > pp_page_kind_rank(b.url, b.title);
            });
            const size_t fetch_count = std::min(hits.size(), (size_t)2);
            const auto t_f0 = std::chrono::steady_clock::now();
            const LlmConfig cfg_f = cfg;
            std::vector<std::future<DsFetchReply>> futs;
            for (size_t i = 0; i < fetch_count; ++i) {
                const std::string fu = hits[i].url;
                futs.push_back(std::async(std::launch::async, [fu, cfg_f]() {
                    retrieval_rate_limiter().acquire();
                    return ds_fetch(cfg_f, fu, 10000);
                }));
            }
            for (size_t i = 0; i < fetch_count; ++i) {
                const DsFetchReply f_rep = futs[i].get();
                if (f_rep.ok && !f_rep.content.empty()) {
                    hits[i].snippet = f_rep.content.substr(0, 1200);
                    if (!f_rep.title.empty()) hits[i].title = f_rep.title;
                }
            }
            trk_fetch_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_f0).count();
        }
    }

    SummaryResult sr;
    // 2026-10-27（用户 261027-06 P4）：单曲页也要「赏析 / 背景解读」。
    // 单曲同样先分类（同人曲靠曲名/原曲名就能判定），提示词按类定制，并强制要求一节
    // 「💿 补充背景与解读」；解析走宽松路径（标签自由，引用可选）。
    ProfileSignal trk_sig;
    trk_sig.title = title;
    trk_sig.artist = artist;
    trk_sig.hint = opts.genre_hint;
    {
        std::ostringstream mt;
        for (const auto& h : hits) {
            mt << h.title << "\n" << (h.snippet.empty() ? h.summary : h.snippet).substr(0, 1200) << "\n";
        }
        trk_sig.material_text = mt.str();
    }
    const ProfileDecision trk_dec = classify_profile(trk_sig);
    const bool trk_profile = opts.use_profile_prompts;
    LinkIndex trk_idx;
    if (trk_profile) trk_idx = summary_link_index({}, hits);
    if (!hits.empty()) {
        std::string last_err;
        const std::string trk_material = summary_materials_block({}, hits);
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::vector<LlmMessage> msgs;
            if (trk_profile) {
                const ProfilePrompt pp = build_profile_prompt(
                    trk_dec, artist.empty() ? title : (artist + " - " + title), "歌曲", trk_material, opts.album_tags);
                msgs = { LlmMessage{ "system", pp.system }, LlmMessage{ "user", pp.user } };
            } else {
                msgs = build_track_prompt(title, artist, hits, lang);
            }
            if (attempt > 0) {
                msgs.push_back(LlmMessage{ "user", "上一次输出不合规（" + last_err +
                    "）。请只输出严格 JSON：{\"fields\":{\"标签\":\"值\"},"
                    "\"sections\":[{\"heading\":\"💿 补充背景与解读\",\"paragraphs\":[\"……\"]}]}" });
            }
            retrieval_rate_limiter().acquire();
            progress.begin_attempt(attempt, "track llm attempt=" + std::to_string(attempt));
            const auto t_l0 = std::chrono::steady_clock::now();
            const LlmReply rep = retrieval_chat(cfg, msgs, trk_profile ? 3200 : 2200, 0.3f, true, progress);
            trk_llm_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_l0).count();
            if (!rep.ok) { last_err = rep.error; continue; }
            sr = trk_profile ? summary_build_from_profile_reply(rep.content, trk_dec, {}, trk_idx)
                             : parse_ai_only_reply(rep.content);
            if (sr.ok) break;
            last_err = sr.error;
        }
    }
    if (!sr.ok) {
        // 检索不到材料 → 先让模型自己用 search/fetch 查一轮（261026-06 #2），
        // 查到就据实写；工具挂/超时则退回 AI 自有知识（仍标 unverified，UI 只显示「AI 整理」）。
        std::string ev_text;
        if (opts.enable_web_tools) {
            progress.phase(SummaryPhase::Retrieving, "web tools");
            const WebEvidence ev = retrieval_web_evidence(
                cfg, artist.empty() ? title : (artist + " - " + title), "歌曲", lang,
                opts.web_tool_rounds, opts.web_tool_budget_ms);
            trk_ev_diag = " tools={" + ev.diag + " " + (ev.ok ? "evidence=ok" : "evidence=none:" + ev.error) + "}";
            if (ev.ok) ev_text = ev.text;
            progress.phase(SummaryPhase::Retrieving, "generate");
        }
        ProfileContext trk_ctx;
        if (trk_profile) {
            trk_ctx.decision = trk_dec;
            trk_ctx.links = trk_idx;
            trk_ctx.valid = true;
        }
        sr = summary_ai_only_generate(artist.empty() ? title : (artist + " - " + title), "歌曲", lang,
                                      cfg, on_progress, ev_text, trk_ctx.valid ? &trk_ctx : nullptr);
    } else {
        sr.unverified = true;   // 检索正文未做逐段引用校验 → 仍按"AI 整理"标注
        sr.disclaimer = "AI 整理";
    }
    if (sr.ok) {
        sr.generated_at = (long long)std::time(nullptr);
        sr.expires_at = sr.generated_at + 7 * 86400;
        summary_stamp_citations(sr, sr.generated_at);   // 父批准 #2：单曲路径同样由调用方盖章
        sr.diag = "search=" + std::to_string(trk_search_ms) + "ms fetch=" + std::to_string(trk_fetch_ms) +
                  "ms llm=" + std::to_string(trk_llm_ms) + "ms hits=" + std::to_string(hits.size()) +
                  (trk_ev_diag.empty() ? std::string() : (" fallback=ai_only" + trk_ev_diag)) +
                  " profile=" + music_profile_id(trk_dec.profile) +
                  " fields=" + std::to_string(sr.fields.size()) +
                  " links=" + std::to_string(sr.links.size());
        summary_save_cache(baseDir, eid, lang, sr);
        progress.phase(SummaryPhase::Done, "track ok");
    } else {
        progress.phase(SummaryPhase::Failed, sr.error);
    }
    return sr;
}


inline SummaryResult retrieval_fetch(EntityKind kind,
                                     const std::string& name,
                                     const std::string& lang = "zh",
                                     const RetrievalOptions& opts = {},
                                     ProgressFn on_progress = nullptr) {
    // 进度回调（可为空）：空时全程走 llm_chat，行为与改造前逐字段一致。
    SummaryProgressEmitter progress(on_progress);
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

    // 进度：走到这里才真正联网/生成（缓存命中与离线早退不推事件 —— 那种路径本来就不等）
    progress.phase(SummaryPhase::Retrieving, "knowledge+search");
    // 2026-10-24（261024-02 #1/#7）：分阶段计时（A/B 与"为什么慢"都靠它，不许拍脑袋）
    const auto t_stage0 = std::chrono::steady_clock::now();
    auto ms_since = [](std::chrono::steady_clock::time_point a) {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - a).count();
    };
    long long diag_knowledge_ms = 0, diag_search_ms = 0, diag_fetch_ms = 0, diag_llm_ms = 0;
    int diag_site_fallback = 0;   // 站点限定补搜的原始命中数（0 = 没触发/没命中）
    // 3. Step 1: 拉取 L2 权威材料（Release 整合 Wikipedia 与 Discogs）
    //
    // 2026-10-24（用户 261024-04 #4「专辑页面 AI 整理资料非常慢」）：
    //   Step 1（维基/萌娘/Discogs）与 Step 2/3（官方 search + 逐页 fetch）**互不依赖** ——
    //   材料只用于最后拼提示词。旧实现严格串行，实测 knowledge=3435ms 完整叠在 search+fetch 之前
    //   （build/rlplayer-playback.log 09:55:47 与 09:57:56 两行 diag）。
    //   现在把 Step 1 整段装进 lambda 丢到后台线程，本线程同时跑 Step 2/3，最后统一 join：
    //   关键路径从 knowledge+search+fetch 变成 max(knowledge, search+fetch)。
    //   并发纪律：lambda 只写 kr/diag_knowledge_ms；Step 2/3 只写 valid_hits/fetched_hits/
    //   diag_search_ms/diag_fetch_ms —— 两组变量不重叠，allowed_urls 一律等 join 之后再拼。
    //   （lambda 体保持原缩进，纯为把改动面压到最小，逻辑与串行版逐行一致。）
    KnowledgeResult kr;          // Step 1 的产出；只由后台 lambda 写，join 之后本线程才读
    auto knowledge_stage = [&]() {
    KnowledgeOpts kopts;
    kopts.lang = lang;
    kopts.metabase = opts.metabase;
    kopts.refresh = opts.refresh;
    kopts.timeout_ms = opts.timeout_ms;
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

    // 2026-10-24（261024-02 #1）：消歧义页不是资料。
    // 实测艺术家 "Irotas" 命中维基消歧义页，正文只有"Iroha (ILLIT)、IRAS 17163-3907、IOTA…"候选列表，
    // 喂给模型就产出"XX 的页面目前仅显示多个候选标题…"这种垃圾（用户看到的"艺术家资料质量极差"）。
    // 这里直接丢弃消歧义材料：剩下的走检索/搜索，什么都没有就走 AI 整理兜底。
    kr.materials.erase(
        std::remove_if(kr.materials.begin(), kr.materials.end(),
                       [](const KnowledgeMaterial& m) { return m.kind == "disambiguation"; }),
        kr.materials.end());

        diag_knowledge_ms = ms_since(t_stage0);
    };   // knowledge_stage —— Step 1 全程在后台线程里跑

    // 4. Step 2: (可选) ds_search「创作背景/评价/版本点评」检索（每实体最多 1 次）
    //    与上面的 Step 1 **并行**（见 knowledge_stage 注释）。
    std::vector<SearchHit> valid_hits;
    std::vector<SearchHit> fetched_hits;
    std::vector<std::string> allowed_urls;
    std::future<void> f_knowledge;
    try {
        f_knowledge = std::async(std::launch::async, knowledge_stage);
    } catch (...) {
        knowledge_stage();     // 起不了线程（极端资源不足）→ 退回串行，行为与旧版一致
    }

    const auto t_search0 = std::chrono::steady_clock::now();
    // 2026-10-27（真实调用反馈 Static World 会场限定CD）：**搜索前**先分类一次。
    // 只用标题/艺术家/本地标签（材料还没到），用来定制检索词；材料到手后再分一次（更准），
    // 两次结果分别用于「搜什么」与「怎么写」。
    const MusicProfile pre_profile = classify_profile(
        summary_make_signal(name, opts.artist, {}, {}, opts.album_tags, opts.track_titles,
                            opts.genre_hint)).profile;
    if (opts.enable_search) {
        std::string query;
        // 2026-10-24（261024-01 #1）：官方 /v1/tools/search 的召回质量与 query 强相关。
        // 实测「专辑名 + 艺术家 + EP 发行 曲目 制作」比「背景 评价」命中多得多（评价类词会把结果带偏到乐评站）。
        if (kind == EntityKind::Release) {
            // 2026-10-28（用户真机对比：他直接用 DeepSeek 搜索能拿到极准确的资料，软件里却是垃圾）：
            //   根因是**搜索词里同一个词出现了两次**。
            //   本库的目录命名是「专辑名 @ 社团」，entity_parse_album_circle 已把 @ 拆掉，
            //   于是 name = "Static World 会场限定CD"、artist = "Static World"
            //   ⟹ 旧代码 artist + " " + name 拼出
            //        "Static World Static World 会场限定CD 专辑 EP 发行 曲目 制作"
            //   同一个词重复两次会显著拉低搜索引擎的相关性排序 —— 实测拿回来的正是
            //   完全不相干的《火花》（网易云 id=148064894）。
            //   修法：**name 里已经含 artist 时就不再重复拼**（同人碟的专辑名普遍带社团名）。
            std::string qTarget;
            const bool nameHasArtist = !opts.artist.empty() &&
                                       name.find(opts.artist) != std::string::npos;
            if (!opts.artist.empty() && !nameHasArtist) qTarget = opts.artist + " " + name;
            else                                       qTarget = name;
            // 分隔符会稀释 query，换成空格。@ / ＠ 一并清掉（防御：万一上游没拆干净，
            // 出现在 query 里的 @ 只是噪音）。
            for (char& ch : qTarget) {
                if (ch == '/' || ch == '&' || ch == '|' || ch == '+' || ch == '@') ch = ' ';
            }
            // 折叠连续空格（"A  B" → "A B"），并去掉首尾空格
            std::string collapsed;
            collapsed.reserve(qTarget.size());
            bool prevSpace = true;
            for (char ch : qTarget) {
                if (ch == ' ') { if (!prevSpace) { collapsed.push_back(' '); prevSpace = true; } }
                else { collapsed.push_back(ch); prevSpace = false; }
            }
            while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
            // 2026-10-28：suffix 与 base 也要去重（「会场限定」在两边各出现一次会稀释 query）。
            // 见 prompt_profiles.h 的 pp_query_merge_suffix 注释。
            query = pp_query_merge_suffix(collapsed.empty() ? name : collapsed,
                                          pp_search_query_suffix(pre_profile, lang));
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

        // 2026-10-26（用户 261026-06 #2）：need_content=true → 正文随 search 一起回来
        //（官方 guides/search「请求字段」），省掉「先 search 再 fetch 同一页」的第二次计费与 RTT。
        DsSearchOptions sopt;
        sopt.count = opts.max_search_hits > 0 ? opts.max_search_hits : 5;
        sopt.needContent = true;
        sopt.oriQuestion = !opts.artist.empty() ? (opts.artist + " " + name) : name;
        retrieval_rate_limiter().acquire();
        const DsSearchReply s_rep = ds_search(cfg, query, sopt);
        diag_search_ms = ms_since(t_search0);
        progress.phase(SummaryPhase::Retrieving,
                       "search hits=" + std::to_string(s_rep.hits.size()));
        if (s_rep.ok) {
            auto to_hits = [](const std::vector<DsHit>& in) {
                std::vector<SearchHit> out;
                out.reserve(in.size());
                for (const auto& dh : in) {
                    SearchHit sh;
                    sh.title = dh.title;
                    sh.url = dh.url;
                    sh.snippet = dh.snippet;
                    sh.summary = dh.summary;
                    sh.content = dh.content;
                    sh.date = dh.date;
                    sh.rerankScore = dh.rerankScore;
                    sh.authority = dh.authority;
                    out.push_back(std::move(sh));
                }
                return out;
            };
            std::vector<SearchHit> raw_hits = to_hits(s_rep.hits);

            // 过滤与排序
            FilterResult fr = filter_hits(raw_hits, opts.filter_opts);
            valid_hits = std::move(fr.hits);

            // 2026-10-26（用户 261026-06 #2）：主搜索一条都没过闸 → 再打一次「平台站点限定」搜索。
            // 用户报的「明明 QQ 音乐 / dizzylab 上有这张专辑，AI 却说找不到」有两个真因：
            //   ① 白名单没有 dizzylab.net（已补）；
            //   ② 平台页的 authority 常是 1~2、rerank 0.6~1.4，默认闸（authority>=2 & rerank>=0.8）
            //      会把它们整批丢掉，于是永远退到「AI 整理想不起」。
            // 这里用官方 site: 语法（guides/search「搜索语法」）直接点名平台，并放宽闸门
            // —— 放宽是安全的：URL 已经限定在这几个站点，不存在"垃圾站混进来"。
            if (valid_hits.empty() && opts.enable_site_fallback && !opts.site_fallback_sites.empty()) {
                DsSearchOptions sopt2 = sopt;
                sopt2.siteRestrict = opts.site_fallback_sites;
                const std::string q2 = !opts.artist.empty() ? (opts.artist + " " + name) : name;
                retrieval_rate_limiter().acquire();
                const DsSearchReply s_rep2 = ds_search(cfg, q2, sopt2);
                diag_search_ms = ms_since(t_search0);
                if (s_rep2.ok && !s_rep2.hits.empty()) {
                    FilterOptions rel = opts.filter_opts;
                    rel.min_authority = 0;        // 平台页 authority 常见 0~2（-1=未知），不设门
                    rel.min_rerank_score = 0.0;   // site: 已限定来源，相关性交给排序
                    FilterResult f2 = filter_hits(to_hits(s_rep2.hits), rel);
                    valid_hits = std::move(f2.hits);
                    diag_site_fallback = (int)s_rep2.hits.size();
                    progress.phase(SummaryPhase::Retrieving,
                                   "site-fallback hits=" + std::to_string(valid_hits.size()));
                }
            }

            // 2026-10-27（真实调用反馈第 2 轮）：命中的**全是组织/艺术家页**（没有一张专辑页）⟹
            // 再补一次「专辑页导向」的站点限定搜索，并把结果**并入**（不是替换）。
            // 旧逻辑只在「零命中」时补搜，于是「搜到了一条 org 页」就被当成"有材料"，
            // 专辑页永远没机会被搜到 —— 这正是用户看到"全是同团体目录"的直接原因。
            // 只对**专辑**这么做：艺术家的「主材料」本来就是艺术家页，再补搜专辑页纯属浪费一次搜索费。
            if (s_rep.ok && kind == EntityKind::Release && !valid_hits.empty() &&
                !summary_has_primary_material(valid_hits) &&
                opts.enable_site_fallback && !opts.site_fallback_sites.empty()) {
                DsSearchOptions sopt3 = sopt;
                sopt3.siteRestrict = opts.site_fallback_sites;
                const std::string q3 = (!opts.artist.empty() ? (opts.artist + " " + name) : name) +
                                       pp_search_query_suffix(pre_profile, lang);
                retrieval_rate_limiter().acquire();
                const DsSearchReply s_rep3 = ds_search(cfg, q3, sopt3);
                diag_search_ms = ms_since(t_search0);
                if (s_rep3.ok && !s_rep3.hits.empty()) {
                    FilterOptions rel3 = opts.filter_opts;
                    rel3.min_authority = 0;
                    rel3.min_rerank_score = 0.0;
                    FilterResult f3 = filter_hits(to_hits(s_rep3.hits), rel3);
                    for (auto& h : f3.hits) {
                        bool dup = false;
                        for (const auto& x : valid_hits) {
                            if (x.url == h.url) { dup = true; break; }
                        }
                        if (!dup) valid_hits.push_back(h);
                    }
                    diag_site_fallback = (int)s_rep3.hits.size();
                    progress.phase(SummaryPhase::Retrieving,
                                   "album-page-fallback hits=" + std::to_string(valid_hits.size()));
                }
            }

            // 2026-10-27（真实调用反馈 Static World）：**专辑页优先**。
            // 实测可复现：DeepSeek 把 vgmdb.net/org/5044（组织页，22 条同团体目录）排在第一，
            // 我们照单全收 ⟹ 模型只能看见「同团体目录」，字段全成了目录列表。
            // 这里按页面类型重排（专辑/发行/作品页 > 通用 > 组织/艺术家页），
            // 让 fetch 的配额（最多 4 页）优先花在真正的专辑页上。
            std::stable_sort(valid_hits.begin(), valid_hits.end(),
                             [](const SearchHit& a, const SearchHit& b) {
                                 return pp_page_kind_rank(a.url, a.title) > pp_page_kind_rank(b.url, b.title);
                             });

            // Step 3: ds_fetch 取前几页正文（每实体最多 4 次 fetch；官方 fetch 返回正文质量好）。
            // 2026-10-24（261024-02 #1 速度）：4 次 fetch 串行要 4×RTT，改并行（限频器内部仍有 220ms 间隔，
            // 不会打爆 QPS），顺序只影响结果合并顺序、不影响内容。
            // 2026-10-25（并发记账 P0）：这 4 路并行 fetch 每路都会进 ds_fetch → llm_record_usage，
            // 也就是 4 条线程同时做「读用量 JSON → +1 → 写回」。记账侧已在 src/meta/llm.h 里加了
            // 进程内互斥锁 + 唯一 tmp 名 + .bak 回退（旧实现无锁且共用固定 .tmp 名，会丢更新/写坏文件），
            // 所以本处可以继续并行，无需在这一层串行化或自行加锁。
            const size_t fetch_count = std::min(valid_hits.size(),
                (size_t)std::max(1, std::min(opts.max_fetch_pages, 4)));
            const auto t_fetch0 = std::chrono::steady_clock::now();
            const LlmConfig cfg_fetch = cfg;
            // need_content 已经带回正文的页不再 fetch（省一次计费；正文质量同级）
            std::vector<size_t> need_fetch;
            for (size_t i = 0; i < fetch_count; ++i) {
                if (valid_hits[i].content.empty()) need_fetch.push_back(i);
            }
            std::vector<std::future<DsFetchReply>> fetch_futs;
            fetch_futs.reserve(need_fetch.size());
            for (size_t idx : need_fetch) {
                const std::string fu = valid_hits[idx].url;
                fetch_futs.push_back(std::async(std::launch::async, [fu, cfg_fetch]() {
                    retrieval_rate_limiter().acquire();
                    return ds_fetch(cfg_fetch, fu, 12000);   // 12s 超时：慢页不拖累整体
                }));
            }
            for (size_t k = 0; k < need_fetch.size(); ++k) {
                SearchHit& fh = valid_hits[need_fetch[k]];
                const DsFetchReply f_rep = fetch_futs[k].get();
                if (f_rep.ok && !f_rep.content.empty()) {
                    fh.content = f_rep.content;
                    if (!f_rep.title.empty()) fh.title = f_rep.title;
                }
            }
            // 正文统一落进 snippet（提示词只读 snippet，避免为一个字段再加一路拼接）
            for (size_t i = 0; i < fetch_count; ++i) {
                if (!valid_hits[i].content.empty()) valid_hits[i].snippet = valid_hits[i].content.substr(0, 1500);
            }
            for (size_t i = 0; i < valid_hits.size(); ++i) fetched_hits.push_back(valid_hits[i]);
            diag_fetch_ms = ms_since(t_fetch0);
            progress.phase(SummaryPhase::Retrieving,
                           "fetch pages=" + std::to_string(fetch_count) +
                           " fetched=" + std::to_string(need_fetch.size()));
        }
    }

    // ---- Step 1 / Step 2+3 汇合点（谁慢等谁）----
    // 旧实现在 Step 2 之前就把材料 URL 塞进 allowed_urls；两段并行后必须等两边都写完再拼，
    // 否则给 parse_synthesis_reply 的允许集合会缺条（表现是"引用校验失败"，极难查）。
    if (f_knowledge.valid()) {
        try {
            f_knowledge.get();
        } catch (...) {
            // 后台线程抛异常：宁可材料为空走 AI 兜底，也不能让任务线程带着未捕获异常退出
            //（那会让 /api/entity/summary 的 job 永远停在 running，前端一直转圈）。
            if (diag_knowledge_ms == 0) diag_knowledge_ms = ms_since(t_stage0);
        }
    }
    for (const auto& m : kr.materials) {
        if (!m.url.empty()) allowed_urls.push_back(m.url);
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
        // 2026-10-24（261024-02 #1）：同样只回退 v3+ 缓存（旧缓存可能是消歧义垃圾，见下方说明）
        if (have_cache && cached.ok && cached.cache_ver >= 6) {
            cached.from_cache = true;
            cached.offline = true;
            cached.error = "未检索到任何权威材料，已回退至本地缓存";
            progress.phase(SummaryPhase::Done, "cache_fallback");
            return cached;
        }
        if (!cfg.apiKey.empty()) {
            const std::string label = !opts.artist.empty() ? (opts.artist + " - " + name) : name;
            const std::string kind_label = (kind == EntityKind::Release) ? "专辑" : "艺术家";
            // 2026-10-24（261024-04 #4）：这条兜底路径以前**不带 diag**，于是日志里只剩
            // "unverified=1"、看不出时间花在哪。AI 整理同样是 10~30s 的 LLM 调用，必须计时。
            const auto t_ai0 = std::chrono::steady_clock::now();
            // 2026-10-26（用户 261026-06 #2）：先让模型自己用 search/fetch 查一轮，
            // 查到就用查到的写（这才是「AI 会自己搜」）；工具挂/超时 → evidence 为空 → 退回纯记忆。
            std::string ev_text;
            std::string ev_diag;
            if (opts.enable_web_tools) {
                progress.phase(SummaryPhase::Retrieving, "web tools");
                const auto t_ev0 = std::chrono::steady_clock::now();
                const WebEvidence ev = retrieval_web_evidence(cfg, label, kind_label, lang,
                                                              opts.web_tool_rounds, opts.web_tool_budget_ms);
                diag_llm_ms += (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_ev0).count();
                ev_diag = " tools={" + ev.diag + " " + (ev.ok ? "evidence=ok" : "evidence=none:" + ev.error) + "}";
                if (ev.ok) {
                    ev_text = ev.text;
                    for (const auto& u : ev.urls) allowed_urls.push_back(u);
                }
                progress.phase(SummaryPhase::Retrieving, "generate");
            }
            // 2026-10-27（261027-06 P1/P2）：这条兜底也先分类（材料为空 → 只能靠标题/艺术家/标签），
            // 然后按类给专用提示词；解析走宽松路径，模型写多少就展示多少。
            ProfileContext nodata_ctx;
            if (opts.use_profile_prompts) {
                const ProfileSignal s0 = summary_make_signal(name, opts.artist, {}, {}, opts.album_tags,
                                                             opts.track_titles, opts.genre_hint);
                nodata_ctx.decision = classify_profile(s0);
                nodata_ctx.links.add_seed_table();
                nodata_ctx.valid = true;
            }
            SummaryResult ai_res = summary_ai_only_generate(label, kind_label, lang, cfg, on_progress, ev_text,
                                                            nodata_ctx.valid ? &nodata_ctx : nullptr);
            diag_llm_ms += (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_ai0).count();
            if (ai_res.ok) {
                ai_res.diag = "knowledge=" + std::to_string(diag_knowledge_ms) + "ms search=" +
                              std::to_string(diag_search_ms) + "ms fetch=" +
                              std::to_string(diag_fetch_ms) + "ms llm=" +
                              std::to_string(diag_llm_ms) + "ms hits=0 fallback=ai_only" + ev_diag +
                              " profile=" + (ai_res.profile.empty() ? "generic" : ai_res.profile) +
                              " fields=" + std::to_string(ai_res.fields.size()) +
                              " links=" + std::to_string(ai_res.links.size());
                summary_stamp_citations(ai_res);      // 父批准 #2：调用方盖章，盖在写缓存之前
                summary_save_cache(baseDir, eid, lang, ai_res);
                return ai_res;
            }
        }
        SummaryResult err_res;
        err_res.ok = false;
        err_res.error = "未找到任何相关权威材料，AI 整理也未成功（防幻觉兜底）";
        progress.phase(SummaryPhase::Failed, err_res.error);
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
    const std::string kindLabel = (kind == EntityKind::Release) ? "专辑" : "艺术家";

    // 2026-10-27（用户 261027-06 P1）：第一步分类 → 第二步按类定制提示词。
    const ProfileSignal prof_signal = summary_make_signal(name, opts.artist, kr.materials, fetched_hits,
                                                          opts.album_tags, opts.track_titles, opts.genre_hint);
    const ProfileDecision prof_dec = classify_profile(prof_signal);
    std::vector<std::pair<std::string, std::string>> known_tags = opts.album_tags;
    for (const auto& kv : pp_signal_tags(prof_signal)) {
        bool dup = false;
        for (const auto& k2 : known_tags) {
            if (k2.first == kv.first) { dup = true; break; }
        }
        if (!dup) known_tags.push_back(kv);
    }
    // 2026-10-27（P3）：名词 → 权威链接的索引（种子表 + 材料/命中的 URL 与页面标题）
    const LinkIndex link_idx = summary_link_index(kr.materials, fetched_hits);
    const bool use_profile = opts.use_profile_prompts;
    const std::vector<LlmMessage> msgs = use_profile
        ? build_profile_synthesis_prompt(prof_dec, promptEntityName, kindLabel, kr.materials, fetched_hits, known_tags)
        : build_synthesis_prompt(promptEntityName, kr.materials, fetched_hits, lang, kindLabel);
    SummaryResult parsed;
    std::string last_err;
    // 2026-10-24（261024-02 #1）：400/非 JSON 的降级链。
    // 用户实测"艺术家栏永远显示 暂无该实体的资料（HTTP 400: 响应不是合法 JSON）"——
    // 根因是请求体在个别实体上被服务端判非法（非法 UTF-8/超长），重发原样请求只会再 400。
    // 降级顺序：①原样（max_tokens 2600）→ ②精简材料重发（每条正文截断、去掉 fetch 长文）
    //          → ③AI 自有知识兜底（标「AI 整理」，总比"暂无资料"好，且不冒充有来源）。
    std::vector<SearchHit> reduced_hits = fetched_hits;
    for (auto& h : reduced_hits) {
        h.snippet = h.snippet.substr(0, 400);
        h.summary = h.summary.substr(0, 400);
    }
    std::vector<KnowledgeMaterial> reduced_mats = kr.materials;
    for (auto& m : reduced_mats) m.text = m.text.substr(0, 1500);

    // 2026-10-27（261027-06 P2）：token 上限从 2600 提到 4000。
    // 「不过滤」就是要求模型写全（20+ 条标签 + 逐条曲目 + 两节解读），2600 会把它截成半截 JSON，
    // 截断比"少写"更糟：整份资料会被判失败。max_tokens 只是上限，模型不写就不计费。
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<LlmMessage> attempt_msgs;
        int attempt_tokens = use_profile ? 4000 : 2600;
        if (attempt == 0) {
            attempt_msgs = msgs;
        } else if (attempt == 1) {
            attempt_msgs = use_profile
                ? build_profile_synthesis_prompt(prof_dec, promptEntityName, kindLabel, reduced_mats, reduced_hits, known_tags)
                : build_synthesis_prompt(promptEntityName, reduced_mats, reduced_hits, lang, kindLabel);
            attempt_tokens = use_profile ? 3200 : 2000;
            attempt_msgs.push_back(LlmMessage{ "user",
                "上一次请求失败（" + last_err + "）。请重新输出严格 JSON："
                "能确定的标签一条都不要少（fields 键名自由），解读放在 sections；"
                "citations 的 url 必须原样复制材料链接。" });
        } else {
            break;   // 第三次交给下面的 AI 整理兜底
        }
        retrieval_rate_limiter().acquire();
        // 每次尝试（含重试）都从这里重新开一条流：begin_attempt 清零 chars/partial
        progress.begin_attempt(attempt, "llm attempt=" + std::to_string(attempt) +
                                        " tokens=" + std::to_string(attempt_tokens));
        const auto t_llm0 = std::chrono::steady_clock::now();
        const LlmReply chat_rep = retrieval_chat(cfg, attempt_msgs, attempt_tokens, 0.2f, true, progress);
        const long long llm_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_llm0).count();
        if (!chat_rep.ok) {
            last_err = "LLM 综合失败: " + chat_rep.error;
            diag_llm_ms += llm_ms;
            continue;
        }
        diag_llm_ms += llm_ms;
        // 2026-10-27（261027-06 P2）：宽松解析 —— 标签自由、段落自由、引用可选；
        // 引用不在白名单只丢那一条，不再否掉整份资料（旧实现一条坏引用 = 整页空白）。
        parsed = use_profile
            ? summary_build_from_profile_reply(chat_rep.content, prof_dec, allowed_urls, link_idx)
            : parse_synthesis_reply(chat_rep.content, allowed_urls);
        if (parsed.ok) break;
        last_err = "综合结果校验失败: " + parsed.error;
    }
    if (!parsed.ok) {
        // 2026-10-24（261024-02 #1）：只回退 **v3+** 的缓存。旧格式缓存内容可能是消歧义垃圾
        //（实测 Irotas：v2 缓存里是"页面仅显示多个候选标题…"），拿它当兜底等于把坏内容永久固化。
        if (have_cache && cached.ok && cached.cache_ver >= 6) {
            cached.from_cache = true;
            cached.offline = true;
            cached.error = "综合校验失败 (" + last_err + ")，已回退至本地缓存";
            progress.phase(SummaryPhase::Done, "cache_fallback");
            return cached;
        }
        // 2026-10-27（261027-06）：兜底也走同一套分类提示词（用户要的是"无论哪条路径都别过滤"）
        ProfileContext fb_ctx;
        fb_ctx.decision = prof_dec;
        fb_ctx.links = link_idx;
        fb_ctx.valid = use_profile;
        SummaryResult ai_fb = summary_ai_only_generate(promptEntityName,
            kindLabel, lang, cfg, on_progress, std::string(), use_profile ? &fb_ctx : nullptr);
        if (ai_fb.ok) {
            ai_fb.error = "权威综合失败，已用 AI 自有知识整理（" + last_err + "）";
            ai_fb.diag = "fallback=ai_only; " + last_err;
            ai_fb.generated_at = now_sec;
            ai_fb.expires_at = now_sec + 3 * 86400;   // 兜底内容 TTL 短一些，方便恢复后重生成
            summary_stamp_citations(ai_fb, now_sec);  // 父批准 #2：调用方盖章，盖在写缓存之前
            summary_save_cache(baseDir, eid, lang, ai_fb);
            return ai_fb;
        }
        SummaryResult err_res;
        err_res.ok = false;
        err_res.error = last_err;
        progress.phase(SummaryPhase::Failed, last_err);
        return err_res;
    }

    // 设置元数据与落盘
    parsed.model = cfg.model;
    parsed.generated_at = now_sec;
    parsed.expires_at = now_sec + 7 * 86400; // TTL 7 天
    parsed.from_cache = false;
    parsed.offline = false;
    parsed.validation_ok = true;
    // 2026-10-27（父批准 #2）：这里统一盖「取回时刻」，与 generated_at 用同一个 now_sec。
    summary_stamp_citations(parsed, now_sec);

    parsed.diag = "knowledge=" + std::to_string(diag_knowledge_ms) + "ms search=" +
                  std::to_string(diag_search_ms) + "ms fetch=" + std::to_string(diag_fetch_ms) +
                  "ms llm=" + std::to_string(diag_llm_ms) + "ms hits=" +
                  std::to_string(fetched_hits.size()) +
                  (diag_site_fallback > 0 ? (" sitefb=" + std::to_string(diag_site_fallback)) : std::string()) +
                  // 2026-10-27（261027-06 P1）：把分类结果写进日志，便于核查"分错类"的实例
                  " profile=" + music_profile_id(prof_dec.profile) +
                  "(" + std::to_string(prof_dec.score) + "," + prof_dec.hint_source + ")" +
                  " fields=" + std::to_string(parsed.fields.size()) +
                  " links=" + std::to_string(parsed.links.size()) +
                  (use_profile ? "" : " legacy_prompt=1");
    summary_save_cache(baseDir, eid, lang, parsed);
    progress.phase(SummaryPhase::Done, "llm ok");

    return parsed;
}

inline SummaryResult retrieval_fetch(const std::string& kind_str,
                                     const std::string& name,
                                     const std::string& lang = "zh",
                                     const RetrievalOptions& opts = {},
                                     ProgressFn on_progress = nullptr) {
    EntityKind k = EntityKind::Person;
    if (!entity_kind_from_tag(kind_str, k)) {
        k = EntityKind::Person;
    }
    return retrieval_fetch(k, name, lang, opts, std::move(on_progress));
}

inline SummaryResult retrieval_fetch(const Entity& entity,
                                     const std::string& lang = "zh",
                                     const RetrievalOptions& opts = {},
                                     ProgressFn on_progress = nullptr) {
    return retrieval_fetch(entity.kind, entity.name, lang, opts, std::move(on_progress));
}

} // namespace rta::meta
