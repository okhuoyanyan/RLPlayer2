// discogs.h — P3 资料层④b：Discogs L4 古典作品版本推荐（header-only，禁止 windows.h）
//
// 对应 docs/P3-资料层接口契约.md §4 与任务书要求：
//   * 匿名 API 实测可用：GET https://api.discogs.com/database/search?q=...&type=master|release
//   * 匿名限频约 25 req/min，内置令牌桶限流与 HTTP 429 熔断
//   * UA 明确 "RLPlayer/1.0.10 (metadata; contact: local)"
//   * 搜索流程：search master/release -> (master 则 /masters/{id}/versions) -> 产出 performances[]
//   * 字段提取：id/title/performers/conductor/orchestra/year/label/format/country/url/source/fetched_at
//   * 缓存：<metabase>/entities/<id>/versions.json，TTL 7 天；原子写 tmp+rename；离线回退
//   * complete 判定：performances.size() >= 5 且每条都有有效 url
//
// 平台边界：
//   * 禁止 include windows.h / winsock2.h / winhttp.h 等系统头
//   * HTTP 仅用 net/http_client.h，JSON 仅用 net/json.h，实体模型复用 meta/entities.h
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
#include <utility>
#include <vector>

namespace rta::meta {

namespace fsx = std::filesystem;

// ---------------------------------------------------------------------------
// 数据结构定义
// ---------------------------------------------------------------------------

struct DiscogsPerformance {
    std::string id;            // 如 "discogs-release:18245671" 或 "discogs-master:1820755"
    std::string title;
    std::string performers;
    std::string conductor;
    std::string orchestra;
    int year = 0;
    std::string label;
    std::string format;
    std::string country;
    std::string url;            // "https://www.discogs.com/release/..."
    std::string source = "discogs";
    long long fetched_at = 0;
};

struct WorkVersionsResult {
    bool ok = false;
    struct WorkInfo {
        std::string id;
        std::string title;
        std::string composer;
    } work;
    std::vector<DiscogsPerformance> performances;
    bool complete = false;      // performances.size() >= 5 且每条都有有效 url
    std::vector<std::string> errors;
    long long generated_at = 0;
    long long expires_at = 0;
    bool from_cache = false;
    bool offline = false;
};

struct DiscogsOptions {
    std::string metabase;
    bool refresh = false;
    bool offline = false;
    int timeout_ms = 15000;
};

// ---------------------------------------------------------------------------
// 令牌桶与熔断器：DiscogsRateLimiter
// ---------------------------------------------------------------------------

class DiscogsRateLimiter {
public:
    static DiscogsRateLimiter& instance() {
        static DiscogsRateLimiter inst;
        return inst;
    }

    // 纯函数：根据经过时间计算补充后的令牌数
    static double calc_refilled_tokens(double current, double elapsed_sec, double rate, double capacity) {
        if (elapsed_sec <= 0.0) return current;
        double next_t = current + elapsed_sec * rate;
        if (next_t > capacity) next_t = capacity;
        return next_t;
    }

    // 纯函数：是否可消费
    static bool can_consume(double tokens, double cost = 1.0) {
        return tokens >= cost;
    }

    // 纯函数：熔断器是否处于激活期
    static bool circuit_breaker_active(bool is_open, long long elapsed_ms, long long trip_duration_ms) {
        if (!is_open) return false;
        return elapsed_ms < trip_duration_ms;
    }

    void trip_circuit(int duration_sec = 60) {
        std::lock_guard<std::mutex> lk(mtx_);
        circuit_open_ = true;
        circuit_tripped_at_ = std::chrono::steady_clock::now();
        circuit_duration_sec_ = duration_sec;
    }

    void reset_circuit() {
        std::lock_guard<std::mutex> lk(mtx_);
        circuit_open_ = false;
    }

    bool is_circuit_open() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!circuit_open_) return false;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - circuit_tripped_at_).count();
        if (elapsed >= circuit_duration_sec_) {
            circuit_open_ = false;
            return false;
        }
        return true;
    }

    bool acquire(int wait_max_ms = 3500) {
        const auto start = std::chrono::steady_clock::now();
        while (true) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (circuit_open_) {
                    const auto now = std::chrono::steady_clock::now();
                    const auto el = std::chrono::duration_cast<std::chrono::seconds>(now - circuit_tripped_at_).count();
                    if (el < circuit_duration_sec_) {
                        return false;
                    }
                    circuit_open_ = false;
                }

                const auto now = std::chrono::steady_clock::now();
                const double el_sec = std::chrono::duration<double>(now - last_refill_).count();
                tokens_ = calc_refilled_tokens(tokens_, el_sec, refill_rate_, capacity_);
                last_refill_ = now;

                if (tokens_ >= 1.0) {
                    tokens_ -= 1.0;
                    return true;
                }
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= wait_max_ms) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void set_tokens(double t) {
        std::lock_guard<std::mutex> lk(mtx_);
        tokens_ = t;
    }

    double get_tokens() {
        std::lock_guard<std::mutex> lk(mtx_);
        return tokens_;
    }

private:
    DiscogsRateLimiter()
        : capacity_(5.0),
          refill_rate_(25.0 / 60.0), // 匿名限频 25 req/min
          tokens_(5.0),
          last_refill_(std::chrono::steady_clock::now()),
          circuit_open_(false),
          circuit_duration_sec_(60) {}

    std::mutex mtx_;
    double capacity_;
    double refill_rate_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
    bool circuit_open_;
    std::chrono::steady_clock::time_point circuit_tripped_at_;
    int circuit_duration_sec_;
};

// ---------------------------------------------------------------------------
// 纯函数辅助：URL 编码、文本清洗与古典标题解析
// ---------------------------------------------------------------------------

inline std::string discogs_url_encode(const std::string& val) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (size_t i = 0; i < val.size(); ++i) {
        unsigned char c = (unsigned char)val[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else if (c == ' ') {
            escaped << '+';
        } else {
            escaped << '%' << std::uppercase;
            escaped.width(2);
            escaped << (int)c;
            escaped.width(0);
        }
    }
    return escaped.str();
}

// 清理 Discogs 艺术家条目里的消歧义后缀，如 "Mahler*" -> "Mahler", "Artist (2)" -> "Artist"
inline std::string clean_discogs_artist(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && (s.back() == ' ' || s.back() == '	' || s.back() == '*')) {
        s.pop_back();
    }
    while (!s.empty() && (s.front() == ' ' || s.front() == '	')) {
        s.erase(s.begin());
    }
    // 去掉尾部的 " (N)"
    const size_t lp = s.rfind(" (");
    if (lp != std::string::npos && s.back() == ')') {
        bool allNum = true;
        for (size_t i = lp + 2; i + 1 < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') { allNum = false; break; }
        }
        if (allNum && lp + 2 < s.size()) {
            s = s.substr(0, lp);
        }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '	')) s.pop_back();
    return s;
}

inline bool is_likely_orchestra(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (const char* kw : { "orchestra", "philharmonic", "philharmoniker", "orkest",
                            "symphon", "staatskapelle", "ensemble", "kammerorchester",
                            "chamber", "academy of", "sinfonieorchester", "sinfonica",
                            "baroque", "consort", "collegium" }) {
        if (lower.find(kw) != std::string::npos) return true;
    }
    return false;
}

inline bool is_likely_conductor(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (lower.rfind("sir ", 0) == 0 || lower.find("dirigent") != std::string::npos ||
        lower.find("conductor") != std::string::npos) {
        return true;
    }
    return false;
}

inline void parse_classical_title(const std::string& rawTitle,
                                  const std::string& composer,
                                  std::string& performers_out,
                                  std::string& conductor_out,
                                  std::string& orchestra_out,
                                  std::string& work_title_out) {
    performers_out.clear();
    conductor_out.clear();
    orchestra_out.clear();
    work_title_out.clear();

    std::string artistPart;
    std::string titlePart;
    const size_t dashPos = rawTitle.find(" - ");
    if (dashPos != std::string::npos) {
        artistPart = rawTitle.substr(0, dashPos);
        titlePart = rawTitle.substr(dashPos + 3);
    } else {
        titlePart = rawTitle;
    }
    work_title_out = titlePart;

    if (artistPart.empty()) return;

    // 按逗号 / 斜杠 / " And " 切割
    std::vector<std::string> artists;
    std::string cur;
    for (size_t i = 0; i < artistPart.size(); ++i) {
        if (artistPart[i] == ',' || artistPart[i] == '/') {
            if (!cur.empty()) { artists.push_back(cur); cur.clear(); }
        } else if (i + 5 <= artistPart.size() && artistPart.substr(i, 5) == " And ") {
            if (!cur.empty()) { artists.push_back(cur); cur.clear(); }
            i += 4;
        } else {
            cur += artistPart[i];
        }
    }
    if (!cur.empty()) artists.push_back(cur);

    std::string compLower = composer;
    for (char& c : compLower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    std::vector<std::string> validArtists;
    for (auto& a : artists) {
        std::string cl = clean_discogs_artist(a);
        if (cl.empty()) continue;
        std::string aLower = cl;
        for (char& c : aLower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

        // 如果是作曲家本人，在演出列表中剥离（避免把作曲家当成指挥/演奏者）
        if (!compLower.empty() && (aLower == compLower || aLower.find(compLower) != std::string::npos ||
                                   compLower.find(aLower) != std::string::npos)) {
            continue;
        }

        if (orchestra_out.empty() && is_likely_orchestra(cl)) {
            orchestra_out = cl;
        } else if (conductor_out.empty() && is_likely_conductor(cl)) {
            conductor_out = cl;
        }
        validArtists.push_back(cl);
    }

    // 若有乐团但尚未识别出指挥，且有其他个人艺术家，推断个人为指挥/主奏
    if (conductor_out.empty() && !orchestra_out.empty()) {
        for (const auto& a : validArtists) {
            if (a != orchestra_out && !is_likely_orchestra(a)) {
                conductor_out = a;
                break;
            }
        }
    }

    std::ostringstream pss;
    for (size_t i = 0; i < validArtists.size(); ++i) {
        if (i) pss << ", ";
        pss << validArtists[i];
    }
    performers_out = pss.str();
    if (performers_out.empty()) {
        performers_out = clean_discogs_artist(artistPart);
    }
}

// ---------------------------------------------------------------------------
// Discogs JSON 响应解析
// ---------------------------------------------------------------------------

struct SearchResultItem {
    int64_t id = 0;
    std::string type;       // "master" 或 "release"
    std::string title;
    int year = 0;
    std::string label;
    std::string format;
    std::string country;
    std::string uri;
};

inline bool parse_discogs_search_json(const std::string& text,
                                      std::vector<SearchResultItem>& items_out,
                                      std::string* err_out = nullptr) {
    items_out.clear();
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        if (err_out) *err_out = "搜索响应不是合法的 JSON 对象";
        return false;
    }
    if (j.has("message")) {
        if (err_out) *err_out = "Discogs API 错误: " + j["message"].s();
        return false;
    }
    if (!j.has("results") || j["results"].type != rta::net::Json::Arr) {
        if (err_out) *err_out = "响应缺少 results 数组";
        return false;
    }
    const auto& arr = j["results"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& it = arr.at(i);
        if (it.type != rta::net::Json::Obj) continue;
        SearchResultItem sri;
        sri.id = (int64_t)it["id"].n();
        sri.type = it["type"].s();
        sri.title = it["title"].s();
        if (it["year"].ok()) {
            std::string ys = it["year"].s();
            if (ys.empty() && it["year"].n() > 0) sri.year = (int)it["year"].n();
            else sri.year = std::atoi(ys.c_str());
        }
        if (it["label"].type == rta::net::Json::Arr && it["label"].size() > 0) {
            sri.label = it["label"].at(0).s();
        } else if (it["label"].type == rta::net::Json::Str) {
            sri.label = it["label"].s();
        }
        if (it["format"].type == rta::net::Json::Arr) {
            std::ostringstream fs;
            for (size_t k = 0; k < it["format"].size(); ++k) {
                if (k) fs << ", ";
                fs << it["format"].at(k).s();
            }
            sri.format = fs.str();
        } else if (it["format"].type == rta::net::Json::Str) {
            sri.format = it["format"].s();
        }
        sri.country = it["country"].s();
        sri.uri = it["uri"].s();
        items_out.push_back(std::move(sri));
    }
    return true;
}

struct MasterVersionItem {
    int64_t id = 0;
    std::string title;
    int year = 0;
    std::string label;
    std::string format;
    std::string country;
};

inline bool parse_discogs_versions_json(const std::string& text,
                                        std::vector<MasterVersionItem>& versions_out,
                                        std::string* err_out = nullptr) {
    versions_out.clear();
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        if (err_out) *err_out = "版本响应不是合法的 JSON 对象";
        return false;
    }
    if (j.has("message")) {
        if (err_out) *err_out = "Discogs API 错误: " + j["message"].s();
        return false;
    }
    if (!j.has("versions") || j["versions"].type != rta::net::Json::Arr) {
        if (err_out) *err_out = "响应缺少 versions 数组";
        return false;
    }
    const auto& arr = j["versions"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& it = arr.at(i);
        if (it.type != rta::net::Json::Obj) continue;
        MasterVersionItem mvi;
        mvi.id = (int64_t)it["id"].n();
        mvi.title = it["title"].s();
        if (it["released"].ok()) {
            std::string rs = it["released"].s();
            if (rs.size() >= 4) mvi.year = std::atoi(rs.substr(0, 4).c_str());
        }
        if (it["label"].type == rta::net::Json::Arr && it["label"].size() > 0) {
            mvi.label = it["label"].at(0).s();
        } else if (it["label"].type == rta::net::Json::Str) {
            mvi.label = it["label"].s();
        }
        if (it["format"].type == rta::net::Json::Arr) {
            std::ostringstream fs;
            for (size_t k = 0; k < it["format"].size(); ++k) {
                if (k) fs << ", ";
                fs << it["format"].at(k).s();
            }
            mvi.format = fs.str();
        } else if (it["format"].type == rta::net::Json::Str) {
            mvi.format = it["format"].s();
        }
        mvi.country = it["country"].s();
        versions_out.push_back(std::move(mvi));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 缓存与 JSON 序列化
// ---------------------------------------------------------------------------

inline fsx::path versions_cache_path(const std::string& baseDir, const std::string& entity_id) {
    fsx::path p = baseDir.empty() ? meta_path_of("rlplayer-meta") : meta_path_of(baseDir);
    p /= "entities";
    p /= entity_id;
    p /= "versions.json";
    return p;
}

inline std::string work_versions_to_json(const WorkVersionsResult& r) {
    std::ostringstream o;
    o << "{\n"
      << "  \"ok\": " << (r.ok ? "true" : "false") << ",\n"
      << "  \"work\": {\n"
      << "    \"id\": \"" << meta_jesc(r.work.id) << "\",\n"
      << "    \"title\": \"" << meta_jesc(r.work.title) << "\",\n"
      << "    \"composer\": \"" << meta_jesc(r.work.composer) << "\"\n"
      << "  },\n"
      << "  \"performances\": [\n";
    for (size_t i = 0; i < r.performances.size(); ++i) {
        const auto& p = r.performances[i];
        o << "    {\n"
          << "      \"id\": \"" << meta_jesc(p.id) << "\",\n"
          << "      \"title\": \"" << meta_jesc(p.title) << "\",\n"
          << "      \"performers\": \"" << meta_jesc(p.performers) << "\",\n"
          << "      \"conductor\": \"" << meta_jesc(p.conductor) << "\",\n"
          << "      \"orchestra\": \"" << meta_jesc(p.orchestra) << "\",\n"
          << "      \"year\": " << p.year << ",\n"
          << "      \"label\": \"" << meta_jesc(p.label) << "\",\n"
          << "      \"format\": \"" << meta_jesc(p.format) << "\",\n"
          << "      \"country\": \"" << meta_jesc(p.country) << "\",\n"
          << "      \"url\": \"" << meta_jesc(p.url) << "\",\n"
          << "      \"source\": \"" << meta_jesc(p.source) << "\",\n"
          << "      \"fetched_at\": " << p.fetched_at << "\n"
          << "    }" << (i + 1 < r.performances.size() ? ",\n" : "\n");
    }
    o << "  ],\n"
      << "  \"complete\": " << (r.complete ? "true" : "false") << ",\n"
      << "  \"generated_at\": " << r.generated_at << ",\n"
      << "  \"expires_at\": " << r.expires_at << ",\n"
      << "  \"errors\": [";
    for (size_t i = 0; i < r.errors.size(); ++i) {
        if (i) o << ", ";
        o << "\"" << meta_jesc(r.errors[i]) << "\"";
    }
    o << "],\n"
      << "  \"from_cache\": " << (r.from_cache ? "true" : "false") << ",\n"
      << "  \"offline\": " << (r.offline ? "true" : "false") << "\n"
      << "}\n";
    return o.str();
}

inline bool work_versions_from_json(const std::string& text,
                                    WorkVersionsResult& r_out,
                                    std::string* err_out = nullptr) {
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        if (err_out) *err_out = "versions json 不是合法对象";
        return false;
    }
    r_out.ok = j.has("ok") ? j["ok"].b : true;
    if (j.has("work") && j["work"].type == rta::net::Json::Obj) {
        r_out.work.id = j["work"]["id"].s();
        r_out.work.title = j["work"]["title"].s();
        r_out.work.composer = j["work"]["composer"].s();
    }
    r_out.complete = j.has("complete") ? j["complete"].b : false;
    r_out.generated_at = (long long)j["generated_at"].n();
    r_out.expires_at = (long long)j["expires_at"].n();
    if (r_out.expires_at <= 0 && r_out.generated_at > 0) {
        r_out.expires_at = r_out.generated_at + 7 * 86400; // TTL 7 天
    }
    if (j.has("from_cache")) r_out.from_cache = j["from_cache"].b;
    if (j.has("offline")) r_out.offline = j["offline"].b;

    r_out.performances.clear();
    if (j.has("performances") && j["performances"].type == rta::net::Json::Arr) {
        const auto& arr = j["performances"];
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& item = arr.at(i);
            if (item.type != rta::net::Json::Obj) continue;
            DiscogsPerformance p;
            p.id = item["id"].s();
            p.title = item["title"].s();
            p.performers = item["performers"].s();
            p.conductor = item["conductor"].s();
            p.orchestra = item["orchestra"].s();
            p.year = (int)item["year"].n();
            p.label = item["label"].s();
            p.format = item["format"].s();
            p.country = item["country"].s();
            p.url = item["url"].s();
            p.source = item["source"].s();
            if (p.source.empty()) p.source = "discogs";
            p.fetched_at = (long long)item["fetched_at"].n();
            r_out.performances.push_back(std::move(p));
        }
    }
    r_out.errors.clear();
    if (j.has("errors") && j["errors"].type == rta::net::Json::Arr) {
        for (size_t i = 0; i < j["errors"].size(); ++i) {
            r_out.errors.push_back(j["errors"].at(i).s());
        }
    }
    return true;
}

inline bool versions_save_cache(const std::string& baseDir,
                                const std::string& entity_id,
                                const WorkVersionsResult& r) {
    if (entity_id.empty()) return false;
    const fsx::path p = versions_cache_path(baseDir, entity_id);
    const std::string content = work_versions_to_json(r);
    return entity_write_atomic(p, content);
}

inline bool versions_load_cache(const std::string& baseDir,
                                const std::string& entity_id,
                                WorkVersionsResult& r_out,
                                std::string* err_out = nullptr) {
    if (entity_id.empty()) return false;
    const fsx::path p = versions_cache_path(baseDir, entity_id);
    std::string text;
    if (!entity_read_file(p, text)) {
        if (err_out) *err_out = "缓存文件不存在或不可读";
        return false;
    }
    return work_versions_from_json(text, r_out, err_out);
}

inline bool versions_is_expired(const WorkVersionsResult& r, long long now_sec = 0) {
    if (now_sec <= 0) now_sec = (long long)std::time(nullptr);
    long long exp = r.expires_at;
    if (exp <= 0 && r.generated_at > 0) {
        exp = r.generated_at + 7 * 86400; // 默认 TTL 7 天
    }
    if (exp <= 0) return true;
    return now_sec >= exp;
}

// ---------------------------------------------------------------------------
// 核心接口：discogs_versions
// ---------------------------------------------------------------------------

inline WorkVersionsResult discogs_versions(const std::string& workTitle,
                                           const std::string& composer = "",
                                           const DiscogsOptions& opts = {}) {
    WorkVersionsResult res;
    res.work.title = workTitle;
    res.work.composer = composer;

    if (workTitle.empty()) {
        res.ok = false;
        res.errors.push_back("未指定作品名称");
        return res;
    }

    const std::string eid = !composer.empty()
        ? entity_id_keyed(EntityKind::Work, {composer, workTitle})
        : entity_id(EntityKind::Work, workTitle);
    res.work.id = eid;

    const std::string baseDir = !opts.metabase.empty() ? opts.metabase : EntityStore::instance().baseDir();

    // 1. 检查缓存
    WorkVersionsResult cached;
    std::string cache_err;
    const bool have_cache = versions_load_cache(baseDir, eid, cached, &cache_err);
    const long long now_sec = (long long)std::time(nullptr);
    const bool cache_unexpired = have_cache && cached.ok && !versions_is_expired(cached, now_sec);

    if (opts.offline) {
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            return cached;
        }
        res.ok = false;
        res.offline = true;
        res.errors.push_back("离线模式且无有效本地缓存");
        return res;
    }

    if (!opts.refresh && cache_unexpired) {
        cached.from_cache = true;
        cached.offline = false;
        return cached;
    }

    // 2. 限频检查与通行证获取
    auto& limiter = DiscogsRateLimiter::instance();
    if (limiter.is_circuit_open()) {
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            cached.errors.push_back("Discogs API 限频熔断中，已回退至本地缓存");
            return cached;
        }
        res.ok = false;
        res.errors.push_back("Discogs API 处于限频熔断期（HTTP 429 保护），请稍后重试");
        return res;
    }

    if (!limiter.acquire(opts.timeout_ms < 5000 ? opts.timeout_ms : 5000)) {
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            cached.errors.push_back("Discogs 请求限流等待超时，已回退至本地缓存");
            return cached;
        }
        res.ok = false;
        res.errors.push_back("Discogs 请求频次超限（约 25 req/min），请稍候重试");
        return res;
    }

    // 3. 执行搜索 (search master/release)
    const std::vector<std::string> reqHeaders = {
        "User-Agent: RLPlayer/1.0.10 (metadata; contact: local)",
        "Accept: application/json"
    };

    // 优先用 release_title + artist 搜索，更精准
    std::string searchUrl;
    if (!composer.empty()) {
        searchUrl = "https://api.discogs.com/database/search?release_title=" +
                    discogs_url_encode(workTitle) +
                    "&artist=" + discogs_url_encode(composer) +
                    "&type=master&per_page=5";
    } else {
        searchUrl = "https://api.discogs.com/database/search?q=" +
                    discogs_url_encode(workTitle) +
                    "&type=master&per_page=5";
    }

    auto httpResp = rta::net::http_get(searchUrl, reqHeaders, opts.timeout_ms);
    if (httpResp.status == 429) {
        limiter.trip_circuit(60);
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            cached.errors.push_back("Discogs HTTP 429，触发熔断，已回退至本地缓存");
            return cached;
        }
        res.ok = false;
        res.errors.push_back("Discogs API 返回 429 Too Many Requests，触发 60s 熔断");
        return res;
    }

    std::vector<SearchResultItem> searchHits;
    std::string parseErr;
    const std::string respBody(httpResp.body.begin(), httpResp.body.end());
    parse_discogs_search_json(respBody, searchHits, &parseErr);

    // 如果精准搜索为空且存在 composer，退化至全文字词搜索 q=<composer> <workTitle>
    if (searchHits.empty() && !composer.empty()) {
        std::string fallbackUrl = "https://api.discogs.com/database/search?q=" +
                                  discogs_url_encode(composer + " " + workTitle) +
                                  "&type=master&per_page=5";
        if (limiter.acquire(opts.timeout_ms)) {
            auto fbResp = rta::net::http_get(fallbackUrl, reqHeaders, opts.timeout_ms);
            if (fbResp.status == 429) {
                limiter.trip_circuit(60);
            } else if (fbResp.ok) {
                const std::string fbBody(fbResp.body.begin(), fbResp.body.end());
                parse_discogs_search_json(fbBody, searchHits);
            }
        }
    }

    if (searchHits.empty()) {
        if (have_cache && cached.ok) {
            cached.from_cache = true;
            cached.offline = true;
            cached.errors.push_back("网络搜索无结果，已回退至本地缓存");
            return cached;
        }
        res.ok = true; // 契约：搜索无版本不是协议错误，而是如实返回 0 条
        res.complete = false;
        res.generated_at = now_sec;
        res.expires_at = now_sec + 7 * 86400;
        return res;
    }

    // 4. 对命中的 master 拉取 /masters/{id}/versions
    std::vector<DiscogsPerformance> perfs;
    size_t masterIdx = 0;
    while (masterIdx < searchHits.size() && perfs.size() < 10) {
        const auto& hit = searchHits[masterIdx++];
        std::string perfArtists, perfConductor, perfOrchestra, perfWorkTitle;
        parse_classical_title(hit.title, composer, perfArtists, perfConductor, perfOrchestra, perfWorkTitle);

        if (hit.type == "master" && hit.id > 0) {
            std::string vUrl = "https://api.discogs.com/masters/" + std::to_string(hit.id) + "/versions?per_page=10";
            if (limiter.acquire(opts.timeout_ms)) {
                auto vResp = rta::net::http_get(vUrl, reqHeaders, opts.timeout_ms);
                if (vResp.status == 429) {
                    limiter.trip_circuit(60);
                } else if (vResp.ok) {
                    const std::string vBody(vResp.body.begin(), vResp.body.end());
                    std::vector<MasterVersionItem> vItems;
                    if (parse_discogs_versions_json(vBody, vItems)) {
                        for (const auto& vi : vItems) {
                            DiscogsPerformance dp;
                            dp.id = "discogs-release:" + std::to_string(vi.id);
                            dp.title = !vi.title.empty() ? vi.title : (!perfWorkTitle.empty() ? perfWorkTitle : workTitle);
                            dp.performers = perfArtists;
                            dp.conductor = perfConductor;
                            dp.orchestra = perfOrchestra;
                            dp.year = vi.year > 0 ? vi.year : hit.year;
                            dp.label = vi.label;
                            dp.format = vi.format;
                            dp.country = vi.country;
                            dp.url = "https://www.discogs.com/release/" + std::to_string(vi.id);
                            dp.source = "discogs";
                            dp.fetched_at = now_sec;
                            perfs.push_back(std::move(dp));
                            if (perfs.size() >= 10) break;
                        }
                    }
                }
            }
        } else {
            // 直接由搜索命中项产生
            DiscogsPerformance dp;
            dp.id = (hit.type == "master" ? "discogs-master:" : "discogs-release:") + std::to_string(hit.id);
            dp.title = !perfWorkTitle.empty() ? perfWorkTitle : hit.title;
            dp.performers = perfArtists;
            dp.conductor = perfConductor;
            dp.orchestra = perfOrchestra;
            dp.year = hit.year;
            dp.label = hit.label;
            dp.format = hit.format;
            dp.country = hit.country;
            dp.url = hit.type == "master"
                ? ("https://www.discogs.com/master/" + std::to_string(hit.id))
                : ("https://www.discogs.com/release/" + std::to_string(hit.id));
            dp.source = "discogs";
            dp.fetched_at = now_sec;
            perfs.push_back(std::move(dp));
        }
    }

    // 若拉取 versions 后条目不足 5 条，将剩余 searchHits 补充加入
    if (perfs.size() < 5) {
        for (const auto& hit : searchHits) {
            std::string hid = (hit.type == "master" ? "discogs-master:" : "discogs-release:") + std::to_string(hit.id);
            bool exists = false;
            for (const auto& p : perfs) {
                if (p.id == hid) { exists = true; break; }
            }
            if (!exists) {
                std::string perfArtists, perfConductor, perfOrchestra, perfWorkTitle;
                parse_classical_title(hit.title, composer, perfArtists, perfConductor, perfOrchestra, perfWorkTitle);
                DiscogsPerformance dp;
                dp.id = hid;
                dp.title = !perfWorkTitle.empty() ? perfWorkTitle : hit.title;
                dp.performers = perfArtists;
                dp.conductor = perfConductor;
                dp.orchestra = perfOrchestra;
                dp.year = hit.year;
                dp.label = hit.label;
                dp.format = hit.format;
                dp.country = hit.country;
                dp.url = hit.type == "master"
                    ? ("https://www.discogs.com/master/" + std::to_string(hit.id))
                    : ("https://www.discogs.com/release/" + std::to_string(hit.id));
                dp.source = "discogs";
                dp.fetched_at = now_sec;
                perfs.push_back(std::move(dp));
                if (perfs.size() >= 5) break;
            }
        }
    }

    res.ok = true;
    res.performances = std::move(perfs);
    res.generated_at = now_sec;
    res.expires_at = now_sec + 7 * 86400;

    // complete 判定：>= 5 条且每条都有有效 url
    bool allUrlsOk = !res.performances.empty();
    for (const auto& p : res.performances) {
        if (p.url.empty()) { allUrlsOk = false; break; }
    }
    res.complete = (res.performances.size() >= 5) && allUrlsOk;

    // 5. 落盘缓存（仅在成功拿到条目时）
    if (!res.performances.empty()) {
        versions_save_cache(baseDir, eid, res);
    }

    return res;
}

} // namespace rta::meta