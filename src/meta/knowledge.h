// knowledge.h — P3 资料层②：直连层（实体 → 权威文本材料）
//
// 对应 docs/元数据补全调研与规划.md §3「内容分级」L2 与 §4 P3 里程碑②：
//   * 维基百科 + 萌娘百科权威文本直连（生平/简介/消歧义）
//   * 每条材料必带来源 URL 与取回时间（fetched_at）
//   * 本地缓存落盘：<metabase>/entities/<entity_id>/knowledge.json（原子写 tmp + rename）
//   * 缓存 TTL 默认 30 天，过期离线回退（from_cache=true, offline=true）
//   * 多源材料互不覆盖，同 URL 严格去重
//   * 令牌桶限频（~1 req/s）+ 连续 3 次失败熔断（30s 冷却）
//   * 缺 key 的可选源（Discogs / Last.fm）返回明确错误，不发起请求、不伪造数据（如实标注未验证）
//
// 平台边界纪律：
//   * 禁止 include windows.h / winsock2.h 等系统头（平台边界门禁自动扫描）
//   * HTTP 仅用 net/http_client.h，JSON 仅用 net/json.h，文件系统用 std::filesystem
//   * 实体 id 复用 entities.h / store.h 的 FNV-1a 64 位哈希
#pragma once

#include "meta/entities.h"
#include "meta/store.h"
#include "net/http_client.h"
#include "net/json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace rta::meta {

namespace fsx = std::filesystem;

// ---------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------

struct KnowledgeMaterial {
    std::string source;      // wikipedia-zh / wikipedia-en / moegirl (可扩展 discogs/lastfm)
    std::string kind;        // biography / genre / similar / disambiguation / credits
    std::string subject;     // 查询用的实体名
    std::string title;       // 页面标题
    std::string text;        // 正文（纯文本；保留段落换行）
    std::string url;         // 可复制来源链接（必填，无 url 的材料不要产出）
    std::string lang;        // zh / en / ja
    long long fetched_at = 0;    // unix 秒
    long long expires_at = 0;    // TTL（默认 30 天；0=不过期）
    int authority = 0;           // 0..4：来源权威分级（维基=4、萌娘=3、AI 综合=1，后续 L3 用）
};

struct KnowledgeResult {
    std::string entityId;                                    // entities.h 的 id
    std::vector<KnowledgeMaterial> materials;
    std::string error;                                       // 综合错误文本
    std::vector<std::pair<std::string, std::string>> errors; // source → error 逐源错误
    bool from_cache = false;
    bool offline = false;
};

struct KnowledgeOpts {
    std::string lang = "zh";            // 语言代码：zh, en, ja 等
    bool refresh = false;               // 强制联网拉取（忽略未过期缓存）
    std::string metabase;               // 缓存根目录，默认空串（走 EntityStore::instance().baseDir()）
    int timeout_ms = 15000;             // HTTP 超时
    bool include_wikipedia = true;      // 启用维基百科
    bool include_moegirl = true;        // 启用萌娘百科
    std::string wiki_api_base;          // 自定义维基 API 端点（测试/覆盖用）
    std::string moegirl_api_base;       // 自定义萌娘 API 端点（测试/覆盖用）
    std::string discogs_token;          // Discogs API token
    std::string lastfm_api_key;         // Last.fm API key
};

using KnowledgeOptions = KnowledgeOpts;

// 纪律 UA（如实标识版本与用途）
inline const char* knowledge_user_agent() {
    return "RLPlayer/1.0.9 (metadata; contact: local)";
}

// ---------------------------------------------------------------------------
// URL 与百分号编码纯函数（单次编码守卫，防双重编码）
// ---------------------------------------------------------------------------

// RFC 3986 百分号编码（只保留 A-Za-z0-9-_.~，其余转 %XX）
inline std::string knowledge_percent_enc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unres = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unres) {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0x0F];
        }
    }
    return o;
}

// 维基百科规范页面 URL 构造：空格转下划线，保留 ASCII 可打印子集，CJK 字符百分号编码
inline std::string build_wiki_page_url(const std::string& lang, const std::string& title) {
    std::string enc;
    static const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < title.size(); ++i) {
        unsigned char c = (unsigned char)title[i];
        if (c == ' ') {
            enc += '_';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
                   c == '(' || c == ')' || c == ':' || c == '@' || c == '!') {
            enc += (char)c;
        } else {
            enc += '%';
            enc += hex[c >> 4];
            enc += hex[c & 0x0F];
        }
    }
    const std::string l = lang.empty() ? "zh" : lang;
    return "https://" + l + ".wikipedia.org/wiki/" + enc;
}

// 萌娘百科页面 URL 构造
inline std::string build_moegirl_page_url(const std::string& title) {
    std::string enc;
    static const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < title.size(); ++i) {
        unsigned char c = (unsigned char)title[i];
        if (c == ' ') {
            enc += '_';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
                   c == '(' || c == ')' || c == ':' || c == '@' || c == '!') {
            enc += (char)c;
        } else {
            enc += '%';
            enc += hex[c >> 4];
            enc += hex[c & 0x0F];
        }
    }
    return "https://zh.moegirl.org.cn/" + enc;
}

inline std::string build_opensearch_url(const std::string& api_base, const std::string& query, int limit = 5) {
    return api_base + "?action=opensearch&search=" + knowledge_percent_enc(query) +
           "&limit=" + std::to_string(limit) + "&format=json";
}

inline std::string build_query_extract_url(const std::string& api_base, const std::string& title) {
    return api_base + "?action=query&prop=extracts&exintro=1&explaintext=1&redirects=1&format=json&formatversion=2&titles=" +
           knowledge_percent_enc(title);
}

// ---------------------------------------------------------------------------
// 报文解析纯函数
// ---------------------------------------------------------------------------

// 解析 opensearch JSON: [ "query", [ "title1", ... ], [ "desc1", ... ], [ "url1", ... ] ]
inline bool parse_opensearch(const std::string& json_text,
                             std::string& query_out,
                             std::vector<std::string>& titles_out,
                             std::vector<std::string>& urls_out) {
    const rta::net::Json j = rta::net::json_parse(json_text);
    if (j.type != rta::net::Json::Arr || j.size() < 2) return false;
    query_out = j.at(0).s();
    titles_out.clear();
    urls_out.clear();
    const rta::net::Json& titles = j.at(1);
    if (titles.type == rta::net::Json::Arr) {
        for (size_t i = 0; i < titles.size(); ++i) {
            titles_out.push_back(titles.at(i).s());
        }
    }
    if (j.size() >= 4) {
        const rta::net::Json& urls = j.at(3);
        if (urls.type == rta::net::Json::Arr) {
            for (size_t i = 0; i < urls.size(); ++i) {
                urls_out.push_back(urls.at(i).s());
            }
        }
    }
    return true;
}

// 解析 MediaWiki query extracts 响应
// 兼容 formatversion=2 (pages 是数组) 与 formatversion=1 (pages 是键为 pageid 的对象)
inline bool parse_mediawiki_extract(const std::string& json_text,
                                    std::string& title_out,
                                    std::string& extract_out,
                                    bool& missing_out,
                                    std::string& redirect_to) {
    const rta::net::Json j = rta::net::json_parse(json_text);
    if (j.type != rta::net::Json::Obj) return false;
    const rta::net::Json& q = j["query"];
    if (q.type != rta::net::Json::Obj) return false;

    redirect_to.clear();
    const rta::net::Json& red = q["redirects"];
    if (red.type == rta::net::Json::Arr && red.size() > 0) {
        redirect_to = red.at(0)["to"].s();
    }

    const rta::net::Json& pages = q["pages"];
    if (pages.type == rta::net::Json::Arr && pages.size() > 0) {
        const rta::net::Json& p = pages.at(0);
        title_out = p["title"].s();
        missing_out = (p.has("missing") && p["missing"].b) || (p.has("pageid") && p["pageid"].n() < 0);
        extract_out = p["extract"].s();
        return true;
    }
    if (pages.type == rta::net::Json::Obj && !pages.obj.empty()) {
        const rta::net::Json& p = pages.obj.begin()->second;
        title_out = p["title"].s();
        missing_out = p.has("missing") || (p.has("pageid") && p["pageid"].n() < 0);
        extract_out = p["extract"].s();
        return true;
    }
    return false;
}

// 消歧义页面判定
inline bool is_disambiguation_page(const std::string& title, const std::string& extract) {
    // 标题含有消歧义标注
    if (title.find("(消歧义)") != std::string::npos ||
        title.find("(消歧義)") != std::string::npos ||
        title.find("(disambiguation)") != std::string::npos) {
        return true;
    }
    // 正文含有维基百科或萌娘百科常用消歧义起手句
    const std::vector<std::string> disambig_keywords = {
        "may refer to:",
        "may also refer to:",
        "可以指：",
        "可以指:",
        "可以指下列",
        "可以是：",
        "可以是:",
        "通常指：",
        "通常指:",
        "是指：",
        "包含以下含义：",
        "包含以下含義：",
        "维基媒体消歧义页",
        "維基媒體消歧義頁",
        "消歧义页"
    };
    for (const auto& kw : disambig_keywords) {
        if (extract.find(kw) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 缓存序列化、反序列化、合并与路径
// ---------------------------------------------------------------------------

inline fsx::path knowledge_cache_path(const std::string& baseDir, const std::string& entity_id) {
    const std::string b = baseDir.empty() ? EntityStore::instance().baseDir() : baseDir;
    return meta_path_of(b) / "entities" / entity_id / "knowledge.json";
}

inline std::string knowledge_to_json(const std::vector<KnowledgeMaterial>& materials,
                                     const std::string& entity_id,
                                     const std::string& subject) {
    std::ostringstream o;
    const long long now_sec = (long long)std::time(nullptr);
    o << "{\n"
      << "  \"entity_id\": \"" << meta_jesc(entity_id) << "\",\n"
      << "  \"subject\": \"" << meta_jesc(subject) << "\",\n"
      << "  \"updated_at\": " << now_sec << ",\n"
      << "  \"materials\": [\n";
    for (size_t i = 0; i < materials.size(); ++i) {
        const KnowledgeMaterial& m = materials[i];
        o << "    {\n"
          << "      \"source\": \""     << meta_jesc(m.source) << "\",\n"
          << "      \"kind\": \""       << meta_jesc(m.kind) << "\",\n"
          << "      \"subject\": \""    << meta_jesc(m.subject) << "\",\n"
          << "      \"title\": \""      << meta_jesc(m.title) << "\",\n"
          << "      \"text\": \""       << meta_jesc(m.text) << "\",\n"
          << "      \"url\": \""        << meta_jesc(m.url) << "\",\n"
          << "      \"lang\": \""       << meta_jesc(m.lang) << "\",\n"
          << "      \"fetched_at\": "   << m.fetched_at << ",\n"
          << "      \"expires_at\": "   << m.expires_at << ",\n"
          << "      \"authority\": "    << m.authority << "\n"
          << "    }" << (i + 1 < materials.size() ? ",\n" : "\n");
    }
    o << "  ]\n}\n";
    return o.str();
}

inline bool knowledge_from_json(const std::string& text,
                                std::vector<KnowledgeMaterial>& materials_out,
                                std::string& entity_id_out,
                                std::string& subject_out,
                                std::string* err_out = nullptr) {
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        if (err_out) *err_out = "knowledge cache corrupted: not a JSON object";
        return false;
    }
    entity_id_out = j["entity_id"].s();
    subject_out = j["subject"].s();
    const rta::net::Json& arr = j["materials"];
    if (arr.type != rta::net::Json::Arr) {
        if (err_out) *err_out = "knowledge cache corrupted: materials is not an array";
        return false;
    }
    materials_out.clear();
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& item = arr.at(i);
        if (item.type != rta::net::Json::Obj) continue;
        KnowledgeMaterial m;
        m.source     = item["source"].s();
        m.kind       = item["kind"].s();
        m.subject    = item["subject"].s();
        m.title      = item["title"].s();
        m.text       = item["text"].s();
        m.url        = item["url"].s();
        m.lang       = item["lang"].s();
        m.fetched_at = (long long)item["fetched_at"].n();
        m.expires_at = (long long)item["expires_at"].n();
        m.authority  = (int)item["authority"].n();
        if (m.url.empty()) continue;  // 必带 url，无 url 的材料丢弃
        materials_out.push_back(std::move(m));
    }
    return true;
}

// 多源材料互不覆盖；同 URL 去重（以新材料覆盖旧材料）
inline void knowledge_merge_materials(std::vector<KnowledgeMaterial>& target,
                                      const std::vector<KnowledgeMaterial>& incoming) {
    for (const auto& inc : incoming) {
        if (inc.url.empty()) continue;
        bool found = false;
        for (auto& tgt : target) {
            if (tgt.url == inc.url) {
                tgt = inc;
                found = true;
                break;
            }
        }
        if (!found) {
            target.push_back(inc);
        }
    }
}

inline bool knowledge_is_expired(const KnowledgeMaterial& m, long long now_sec = 0) {
    if (m.expires_at == 0) return false;
    if (now_sec <= 0) now_sec = (long long)std::time(nullptr);
    return now_sec >= m.expires_at;
}

inline bool knowledge_has_unexpired(const std::vector<KnowledgeMaterial>& materials, long long now_sec = 0) {
    if (materials.empty()) return false;
    for (const auto& m : materials) {
        if (!knowledge_is_expired(m, now_sec)) return true;
    }
    return false;
}

inline bool knowledge_save_cache(const std::string& baseDir,
                                 const std::string& entity_id,
                                 const std::string& subject,
                                 const std::vector<KnowledgeMaterial>& materials) {
    if (entity_id.empty()) return false;
    const fsx::path p = knowledge_cache_path(baseDir, entity_id);
    const std::string content = knowledge_to_json(materials, entity_id, subject);
    return entity_write_atomic(p, content);
}

inline bool knowledge_load_cache(const std::string& baseDir,
                                 const std::string& entity_id,
                                 std::vector<KnowledgeMaterial>& materials_out,
                                 std::string* err_out = nullptr) {
    if (entity_id.empty()) return false;
    const fsx::path p = knowledge_cache_path(baseDir, entity_id);
    std::string text;
    if (!entity_read_file(p, text)) {
        if (err_out) *err_out = "cache file not found or empty";
        return false;
    }
    std::string id_out, sub_out;
    return knowledge_from_json(text, materials_out, id_out, sub_out, err_out);
}

// ---------------------------------------------------------------------------
// 限频（令牌桶/最小间隔）与熔断控制器
// ---------------------------------------------------------------------------

struct KnowledgeGate {
    int min_interval_ms = 1000;      // 1 req/s
    int fail_limit = 3;              // 连续 3 次失败触发熔断
    int cooldown_sec = 30;           // 熔断冷却 30 秒
    std::chrono::steady_clock::time_point last_call{};
    int fails = 0;
    std::chrono::steady_clock::time_point cool_until{};
    std::mutex mtx;

    // 纯逻辑判定（供测试与离线模拟）
    bool acquire_check(std::chrono::steady_clock::time_point now, long long& sleep_ms, std::string& err) {
        if (now < cool_until) {
            const long long left = std::chrono::duration_cast<std::chrono::seconds>(cool_until - now).count() + 1;
            err = "连续失败熔断中，剩余 " + std::to_string(left) + "s 不发起请求";
            return false;
        }
        if (last_call.time_since_epoch().count() != 0) {
            const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_call).count();
            if (diff < min_interval_ms) {
                sleep_ms = min_interval_ms - diff;
            } else {
                sleep_ms = 0;
            }
        } else {
            sleep_ms = 0;
        }
        return true;
    }

    bool acquire(std::string& err) {
        std::lock_guard<std::mutex> lk(mtx);
        long long sleep_ms = 0;
        const auto now = std::chrono::steady_clock::now();
        if (!acquire_check(now, sleep_ms, err)) return false;
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
        last_call = std::chrono::steady_clock::now();
        return true;
    }

    void record_success() {
        std::lock_guard<std::mutex> lk(mtx);
        fails = 0;
    }

    void record_failure_at(std::chrono::steady_clock::time_point now) {
        if (++fails >= fail_limit) {
            cool_until = now + std::chrono::seconds(cooldown_sec);
            fails = 0;
        }
    }

    void record_failure() {
        std::lock_guard<std::mutex> lk(mtx);
        record_failure_at(std::chrono::steady_clock::now());
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mtx);
        last_call = {};
        fails = 0;
        cool_until = {};
    }
};

inline KnowledgeGate& gate_wikipedia() {
    static KnowledgeGate g;
    return g;
}

inline KnowledgeGate& gate_moegirl() {
    static KnowledgeGate g;
    return g;
}

// ---------------------------------------------------------------------------
// 维基百科直连源
// ---------------------------------------------------------------------------

inline KnowledgeResult knowledge_wikipedia(const std::string& subject,
                                           const std::string& lang = "zh",
                                           const KnowledgeOpts& opts = {}) {
    KnowledgeResult res;
    if (subject.empty()) {
        res.error = "查询主体名称为空";
        return res;
    }
    const std::string l = lang.empty() ? "zh" : lang;
    const std::string api_base = !opts.wiki_api_base.empty()
                                     ? opts.wiki_api_base
                                     : ("https://" + l + ".wikipedia.org/w/api.php");

    std::string gate_err;
    if (!gate_wikipedia().acquire(gate_err)) {
        res.error = "wikipedia: " + gate_err;
        return res;
    }

    const std::string opensearch_url = build_opensearch_url(api_base, subject, 5);
    const std::vector<std::string> headers = {
        std::string("User-Agent: ") + knowledge_user_agent()
    };
    const rta::net::HttpResponse open_resp = rta::net::http_get(opensearch_url, headers, opts.timeout_ms);
    if (!open_resp.ok || open_resp.status != 200) {
        gate_wikipedia().record_failure();
        res.error = "wikipedia opensearch 请求失败: " +
                    (open_resp.status == 0 ? open_resp.error : ("HTTP " + std::to_string(open_resp.status)));
        return res;
    }
    gate_wikipedia().record_success();

    const std::string open_json(open_resp.body.begin(), open_resp.body.end());
    std::string query_out;
    std::vector<std::string> candidates;
    std::vector<std::string> cand_urls;
    if (!parse_opensearch(open_json, query_out, candidates, cand_urls) || candidates.empty()) {
        res.error = "wikipedia: 未找到匹配候选条目";
        return res;
    }

    // 匹配候选：「不猜」策略
    std::string target_title;
    std::string target_url;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i] == subject || entity_norm(candidates[i]) == entity_norm(subject)) {
            target_title = candidates[i];
            if (i < cand_urls.size() && !cand_urls[i].empty()) target_url = cand_urls[i];
            break;
        }
    }

    const long long now_sec = (long long)std::time(nullptr);
    // 没有唯一确定匹配 -> 产出 kind="disambiguation" 材料，不猜测
    if (target_title.empty()) {
        KnowledgeMaterial mat;
        mat.source = "wikipedia-" + l;
        mat.kind = "disambiguation";
        mat.subject = subject;
        mat.title = subject;
        mat.lang = l;
        mat.fetched_at = now_sec;
        mat.expires_at = now_sec + 30 * 86400LL;
        mat.authority = 4;
        mat.url = build_wiki_page_url(l, subject);
        mat.text = "找到多个候选标题（未消歧义，不猜测）：\n";
        for (const auto& c : candidates) {
            mat.text += "- " + c + "\n";
        }
        res.materials.push_back(std::move(mat));
        return res;
    }

    // 2) Query extracts 获取摘要
    if (!gate_wikipedia().acquire(gate_err)) {
        res.error = "wikipedia: " + gate_err;
        return res;
    }

    const std::string query_url = build_query_extract_url(api_base, target_title);
    const rta::net::HttpResponse q_resp = rta::net::http_get(query_url, headers, opts.timeout_ms);
    if (!q_resp.ok || q_resp.status != 200) {
        gate_wikipedia().record_failure();
        res.error = "wikipedia query 请求失败: " +
                    (q_resp.status == 0 ? q_resp.error : ("HTTP " + std::to_string(q_resp.status)));
        return res;
    }
    gate_wikipedia().record_success();

    const std::string q_json(q_resp.body.begin(), q_resp.body.end());
    std::string title_out, extract_out, redirect_to;
    bool missing = false;
    if (!parse_mediawiki_extract(q_json, title_out, extract_out, missing, redirect_to)) {
        res.error = "wikipedia: 解析 extract 响应失败";
        return res;
    }
    if (missing) {
        res.error = "wikipedia: 条目不存在";
        return res;
    }

    KnowledgeMaterial mat;
    mat.source = "wikipedia-" + l;
    mat.subject = subject;
    mat.title = title_out.empty() ? target_title : title_out;
    mat.text = extract_out;
    mat.lang = l;
    mat.fetched_at = now_sec;
    mat.expires_at = now_sec + 30 * 86400LL;
    mat.authority = 4;
    mat.url = !target_url.empty() ? target_url : build_wiki_page_url(l, mat.title);
    mat.kind = is_disambiguation_page(mat.title, extract_out) ? "disambiguation" : "biography";

    res.materials.push_back(std::move(mat));
    return res;
}

inline KnowledgeResult knowledge_wikipedia(const Entity& entity,
                                           const std::string& lang = "zh",
                                           const KnowledgeOpts& opts = {}) {
    return knowledge_wikipedia(entity.name, lang, opts);
}

// ---------------------------------------------------------------------------
// 萌娘百科直连源
// ---------------------------------------------------------------------------

inline KnowledgeResult knowledge_moegirl(const std::string& subject,
                                         const KnowledgeOpts& opts = {}) {
    KnowledgeResult res;
    if (subject.empty()) {
        res.error = "查询主体名称为空";
        return res;
    }
    // zh.moegirl.org.cn/api.php 实测免除 Cloudflare Challenge 且数据最全
    const std::string api_base = !opts.moegirl_api_base.empty()
                                     ? opts.moegirl_api_base
                                     : "https://zh.moegirl.org.cn/api.php";

    std::string gate_err;
    if (!gate_moegirl().acquire(gate_err)) {
        res.error = "moegirl: " + gate_err;
        return res;
    }

    const std::string opensearch_url = build_opensearch_url(api_base, subject, 5);
    const std::vector<std::string> headers = {
        std::string("User-Agent: ") + knowledge_user_agent()
    };
    const rta::net::HttpResponse open_resp = rta::net::http_get(opensearch_url, headers, opts.timeout_ms);
    if (!open_resp.ok || open_resp.status != 200) {
        gate_moegirl().record_failure();
        res.error = "moegirl opensearch 请求失败: " +
                    (open_resp.status == 0 ? open_resp.error : ("HTTP " + std::to_string(open_resp.status)));
        return res;
    }
    gate_moegirl().record_success();

    const std::string open_json(open_resp.body.begin(), open_resp.body.end());
    std::string query_out;
    std::vector<std::string> candidates;
    std::vector<std::string> cand_urls;
    if (!parse_opensearch(open_json, query_out, candidates, cand_urls) || candidates.empty()) {
        res.error = "moegirl: 未找到匹配候选条目";
        return res;
    }

    std::string target_title;
    std::string target_url;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i] == subject || entity_norm(candidates[i]) == entity_norm(subject)) {
            target_title = candidates[i];
            if (i < cand_urls.size() && !cand_urls[i].empty()) target_url = cand_urls[i];
            break;
        }
    }

    const long long now_sec = (long long)std::time(nullptr);
    if (target_title.empty()) {
        KnowledgeMaterial mat;
        mat.source = "moegirl";
        mat.kind = "disambiguation";
        mat.subject = subject;
        mat.title = subject;
        mat.lang = "zh";
        mat.fetched_at = now_sec;
        mat.expires_at = now_sec + 30 * 86400LL;
        mat.authority = 3;
        mat.url = build_moegirl_page_url(subject);
        mat.text = "找到多个候选标题（未消歧义，不猜测）：\n";
        for (const auto& c : candidates) {
            mat.text += "- " + c + "\n";
        }
        res.materials.push_back(std::move(mat));
        return res;
    }

    if (!gate_moegirl().acquire(gate_err)) {
        res.error = "moegirl: " + gate_err;
        return res;
    }

    const std::string query_url = build_query_extract_url(api_base, target_title);
    const rta::net::HttpResponse q_resp = rta::net::http_get(query_url, headers, opts.timeout_ms);
    if (!q_resp.ok || q_resp.status != 200) {
        gate_moegirl().record_failure();
        res.error = "moegirl query 请求失败: " +
                    (q_resp.status == 0 ? q_resp.error : ("HTTP " + std::to_string(q_resp.status)));
        return res;
    }
    gate_moegirl().record_success();

    const std::string q_json(q_resp.body.begin(), q_resp.body.end());
    std::string title_out, extract_out, redirect_to;
    bool missing = false;
    if (!parse_mediawiki_extract(q_json, title_out, extract_out, missing, redirect_to)) {
        res.error = "moegirl: 解析 extract 响应失败";
        return res;
    }
    if (missing) {
        res.error = "moegirl: 条目不存在";
        return res;
    }

    KnowledgeMaterial mat;
    mat.source = "moegirl";
    mat.subject = subject;
    mat.title = title_out.empty() ? target_title : title_out;
    mat.text = extract_out;
    mat.lang = "zh";
    mat.fetched_at = now_sec;
    mat.expires_at = now_sec + 30 * 86400LL;
    mat.authority = 3;
    mat.url = !target_url.empty() ? target_url : build_moegirl_page_url(mat.title);
    mat.kind = is_disambiguation_page(mat.title, extract_out) ? "disambiguation" : "biography";

    res.materials.push_back(std::move(mat));
    return res;
}

inline KnowledgeResult knowledge_moegirl(const Entity& entity,
                                         const KnowledgeOpts& opts = {}) {
    return knowledge_moegirl(entity.name, opts);
}

// ---------------------------------------------------------------------------
// 可选源（Discogs / Last.fm）—— 本机缺 key，如实报明确错误，零发请求，零产材料
// ---------------------------------------------------------------------------

inline KnowledgeResult knowledge_discogs(const std::string& subject,
                                         const KnowledgeOpts& opts = {}) {
    KnowledgeResult res;
    (void)subject;
    if (opts.discogs_token.empty()) {
        res.error = "discogs: API token 未配置（未验证）";
        return res;
    }
    res.error = "discogs: 接口未实装（未验证）";
    return res;
}

inline KnowledgeResult knowledge_discogs(const Entity& entity,
                                         const KnowledgeOpts& opts = {}) {
    return knowledge_discogs(entity.name, opts);
}

inline KnowledgeResult knowledge_lastfm(const std::string& subject,
                                        const KnowledgeOpts& opts = {}) {
    KnowledgeResult res;
    (void)subject;
    if (opts.lastfm_api_key.empty()) {
        res.error = "lastfm: API key 未配置（未验证）";
        return res;
    }
    res.error = "lastfm: 接口未实装（未验证）";
    return res;
}

inline KnowledgeResult knowledge_lastfm(const Entity& entity,
                                        const KnowledgeOpts& opts = {}) {
    return knowledge_lastfm(entity.name, opts);
}

// ---------------------------------------------------------------------------
// 综合查询入口（缓存检查 → 多源拉取 → URL 去重合并 → 原子写盘 → 离线回退）
// ---------------------------------------------------------------------------

inline KnowledgeResult knowledge_get(const std::string& entity_id,
                                     const std::string& subject,
                                     const KnowledgeOpts& opts = {}) {
    KnowledgeResult res;
    res.entityId = entity_id;
    const std::string baseDir = !opts.metabase.empty()
                                    ? opts.metabase
                                    : EntityStore::instance().baseDir();
    const fsx::path cache_path = knowledge_cache_path(baseDir, entity_id);

    std::vector<KnowledgeMaterial> cached_materials;
    std::string cache_id, cache_sub, cache_err;
    std::string cache_bytes;
    const bool cache_file_exists = entity_read_file(cache_path, cache_bytes);
    bool cache_valid = false;
    if (cache_file_exists) {
        if (knowledge_from_json(cache_bytes, cached_materials, cache_id, cache_sub, &cache_err)) {
            cache_valid = true;
        }
    }

    const long long now_sec = (long long)std::time(nullptr);
    const bool has_unexpired = cache_valid && knowledge_has_unexpired(cached_materials, now_sec);

    // 1) 命中未过期缓存且未要求 refresh -> 直接返回缓存
    if (!opts.refresh && has_unexpired) {
        res.materials = cached_materials;
        res.from_cache = true;
        res.offline = false;
        return res;
    }

    // 2) 联网拉取
    std::vector<KnowledgeMaterial> fresh_materials;
    std::vector<std::string> net_errors;

    if (opts.include_wikipedia) {
        KnowledgeResult wkr = knowledge_wikipedia(subject, opts.lang, opts);
        if (!wkr.materials.empty()) {
            fresh_materials.insert(fresh_materials.end(), wkr.materials.begin(), wkr.materials.end());
        } else if (!wkr.error.empty()) {
            net_errors.push_back(wkr.error);
            res.errors.push_back({ "wikipedia-" + opts.lang, wkr.error });
        }
    }

    // 萌娘百科仅在查询中文（或显式开启）时拉取
    if (opts.include_moegirl && (opts.lang == "zh" || opts.lang.empty())) {
        KnowledgeResult mkr = knowledge_moegirl(subject, opts);
        if (!mkr.materials.empty()) {
            fresh_materials.insert(fresh_materials.end(), mkr.materials.begin(), mkr.materials.end());
        } else if (!mkr.error.empty()) {
            net_errors.push_back(mkr.error);
            res.errors.push_back({ "moegirl", mkr.error });
        }
    }

    // 3) 处理网络拉取成功结果
    if (!fresh_materials.empty()) {
        std::vector<KnowledgeMaterial> merged = cached_materials;
        knowledge_merge_materials(merged, fresh_materials);
        knowledge_save_cache(baseDir, entity_id, subject, merged);

        res.materials = std::move(merged);
        res.from_cache = false;
        res.offline = false;
        return res;
    }

    // 4) 网络未产出新材料时：若有缓存（即使过期），离线回退！
    if (cache_valid && !cached_materials.empty()) {
        res.materials = std::move(cached_materials);
        res.from_cache = true;
        res.offline = true;
        if (!net_errors.empty()) {
            res.error = "网络拉取无新材料，已离线回退到缓存: " + net_errors[0];
        }
        return res;
    }

    // 既无网络材料也无缓存
    res.from_cache = false;
    res.offline = false;
    if (!cache_err.empty()) {
        res.error = cache_err;
    } else if (!net_errors.empty()) {
        std::string errs;
        for (size_t i = 0; i < net_errors.size(); ++i) {
            if (i) errs += "; ";
            errs += net_errors[i];
        }
        res.error = errs;
    } else {
        res.error = "未找到任何相关权威材料";
    }
    return res;
}

inline KnowledgeResult knowledge_get(EntityKind kind,
                                     const std::string& name,
                                     const KnowledgeOpts& opts = {}) {
    const std::string id = entity_id(kind, name);
    return knowledge_get(id, name, opts);
}

inline KnowledgeResult knowledge_get(const Entity& entity,
                                     const KnowledgeOpts& opts = {}) {
    const std::string id = entity.id.empty() ? entity_id(entity.kind, entity.name) : entity.id;
    return knowledge_get(id, entity.name, opts);
}

// 契约别名：knowledge_fetch（对应 docs/P3-资料层接口契约.md §2 签名）
inline KnowledgeResult knowledge_fetch(EntityKind kind,
                                       const std::string& name,
                                       const KnowledgeOptions& opt = {}) {
    return knowledge_get(kind, name, opt);
}

inline KnowledgeResult knowledge_fetch(const Entity& entity,
                                       const KnowledgeOptions& opt = {}) {
    return knowledge_get(entity, opt);
}

} // namespace rta::meta
