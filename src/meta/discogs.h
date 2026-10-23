// discogs.h — P3 资料层④b：Discogs L4 古典作品版本推荐（header-only，禁止 windows.h）
//
// 对应 docs/P3-资料层接口契约.md §4 与任务书要求：
//   * 匿名 API 实测可用：GET https://api.discogs.com/database/search?q=...&type=release|master
//   * 匿名限频约 25 req/min，内置令牌桶限流与 HTTP 429 熔断
//   * UA 明确 "RLPlayer/1.0.10 (metadata; contact: local)"
//   * 详情解析：GET https://api.discogs.com/releases/{id} 提取 credits:
//     conductor、orchestra/ensemble、soloist[]、composer、year、label、country、format
//   * 演绎去重：dedupKey 规范化（conductor+ensemble+soloists）；非空按 dedupKey 去重（最早 year 为主，otherEditions 折叠计数）；
//     dedupKey 空时不猜，标记 dedupUnknown: true；
//   * complete 升级判定：去重后 distinct 演绎数 >= 5 且每条都有有效 url
//   * 缓存：<metabase>/entities/<id>/versions.json 向后兼容；release 详情缓存 7 天
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
#include <functional>
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
    std::string ensemble;
    std::string orchestra;     // 兼容旧字段，与 ensemble 保持同步
    std::vector<std::string> soloists;
    std::string composer;
    int year = 0;
    std::string label;
    std::string format;
    std::string country;
    std::string url;            // "https://www.discogs.com/release/..."
    std::string source = "discogs";
    long long fetched_at = 0;
    bool detailFetched = false;
    std::string dedupKey;
    int otherEditions = 0;
    bool dedupUnknown = false;
};

struct WorkVersionsResult {
    bool ok = false;
    struct WorkInfo {
        std::string id;
        std::string title;
        std::string composer;
    } work;
    int releaseCount = 0;       // 发行版本总数（去重前）
    int distinctCount = 0;      // 不同演绎数（去重后）
    std::vector<DiscogsPerformance> performances;
    bool complete = false;      // 去重后 distinct 演绎数 >= 5 且每条都有有效 url
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
    int max_releases = 12;      // 抓取 release 详情的上限条数（默认 12）
};

struct DiscogsReleaseDetail {
    int64_t id = 0;
    std::string title;
    std::string composer;
    std::string conductor;
    std::string ensemble;
    std::vector<std::string> soloists;
    int year = 0;
    std::string label;
    std::string format;
    std::string country;
    std::string url;
    std::vector<std::string> genres; // E-5: 大类流派
    std::vector<std::string> styles; // E-5: 细分风格
    long long fetched_at = 0;
    std::string notes;               // 专辑发行说明与描述
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
// 纯函数辅助：URL 编码、文本清洗、规范化与古典标题解析
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
                            "baroque", "consort", "collegium", "chorus", "choir" }) {
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

// 规范化演绎 Token：转小写、折叠拉丁重音变体、去消歧义后缀、标点收敛为空格
inline std::string normalize_dedup_token(const std::string& raw) {
    std::string s = clean_discogs_artist(raw);
    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z') {
                out.push_back((char)(c - 'A' + 'a'));
            } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                out.push_back((char)c);
            } else if (c == ' ' || c == '	') {
                if (!out.empty() && out.back() != ' ') out.push_back(' ');
            } else {
                // 标点转为空格
                if (!out.empty() && out.back() != ' ') out.push_back(' ');
            }
        } else if (c == 0xC3 && i + 1 < s.size()) {
            // Latin-1 Supplement (0xC3 0x80-0xBF)
            unsigned char c2 = (unsigned char)s[++i];
            if ((c2 >= 0x80 && c2 <= 0x85) || (c2 >= 0xA0 && c2 <= 0xA5)) {
                out.push_back('a');
            } else if (c2 == 0x87 || c2 == 0xA7) {
                out.push_back('c');
            } else if ((c2 >= 0x88 && c2 <= 0x8B) || (c2 >= 0xA8 && c2 <= 0xAB)) {
                out.push_back('e');
            } else if ((c2 >= 0x8C && c2 <= 0x8F) || (c2 >= 0xAC && c2 <= 0xAF)) {
                out.push_back('i');
            } else if (c2 == 0x91 || c2 == 0xB1) {
                out.push_back('n');
            } else if ((c2 >= 0x92 && c2 <= 0x96) || c2 == 0x98 ||
                       (c2 >= 0xB2 && c2 <= 0xB6) || c2 == 0xB8) {
                out.push_back('o');
            } else if ((c2 >= 0x99 && c2 <= 0x9C) || (c2 >= 0xB9 && c2 <= 0xBC)) {
                out.push_back('u');
            } else if (c2 == 0x9D || c2 == 0xBD || c2 == 0xBF) {
                out.push_back('y');
            } else {
                if (!out.empty() && out.back() != ' ') out.push_back(' ');
            }
        } else if (c == 0xC4 && i + 1 < s.size()) {
            unsigned char c2 = (unsigned char)s[++i];
            if (c2 == 0x8C || c2 == 0x8D || c2 == 0x86 || c2 == 0x87) out.push_back('c');
            else if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else if (c == 0xC5 && i + 1 < s.size()) {
            unsigned char c2 = (unsigned char)s[++i];
            if (c2 == 0xA0 || c2 == 0xA1) out.push_back('s');
            else if (c2 == 0xBD || c2 == 0xBE) out.push_back('z');
            else if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else {
            // 跳过多字节尾字节
            while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80) {
                ++i;
            }
        }
    }

    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

// 组合 dedupKey：由 conductor + ensemble + soloists 规范化组成；全空则返回空串
inline std::string build_dedup_key(const std::string& conductor,
                                   const std::string& ensemble,
                                   const std::vector<std::string>& soloists) {
    const std::string c = normalize_dedup_token(conductor);
    const std::string e = normalize_dedup_token(ensemble);
    std::vector<std::string> sList;
    for (const auto& s : soloists) {
        std::string ns = normalize_dedup_token(s);
        if (!ns.empty()) sList.push_back(ns);
    }
    std::sort(sList.begin(), sList.end());
    sList.erase(std::unique(sList.begin(), sList.end()), sList.end());

    std::string sJoined;
    for (size_t i = 0; i < sList.size(); ++i) {
        if (i) sJoined += "+";
        sJoined += sList[i];
    }

    // 若指挥、乐团与独奏均缺失，返回空串
    if (c.empty() && e.empty() && sJoined.empty()) {
        return "";
    }
    return c + "|" + e + "|" + sJoined;
}

// 格式化可读的演出人员字符串
inline std::string format_performers_string(const std::string& conductor,
                                            const std::string& ensemble,
                                            const std::vector<std::string>& soloists,
                                            const std::string& fallback) {
    std::ostringstream oss;
    if (!soloists.empty()) {
        for (size_t i = 0; i < soloists.size(); ++i) {
            if (i) oss << ", ";
            oss << soloists[i];
        }
    }
    if (!conductor.empty()) {
        if (oss.tellp() > 0) oss << ", ";
        oss << conductor;
    }
    if (!ensemble.empty()) {
        if (oss.tellp() > 0) oss << ", ";
        oss << ensemble;
    }
    std::string res = oss.str();
    return !res.empty() ? res : fallback;
}

// 按演绎去重核心纯函数
//   * dedupKey 非空：按 dedupKey 归组，保留最早 year 的记录为主条目，其余折叠计数 otherEditions
//   * dedupKey 为空：不臆造合并，保留并标记 dedupUnknown: true
inline std::vector<DiscogsPerformance> deduplicate_performances(
    const std::vector<DiscogsPerformance>& rawList,
    int& releaseCount_out,
    int& distinctCount_out) {

    releaseCount_out = (int)rawList.size();

    std::vector<std::pair<std::string, std::vector<size_t>>> groups;
    std::map<std::string, size_t> keyToGroupIdx;
    std::vector<size_t> unknownIndices;

    for (size_t i = 0; i < rawList.size(); ++i) {
        const auto& p = rawList[i];
        if (p.dedupKey.empty()) {
            unknownIndices.push_back(i);
        } else {
            auto it = keyToGroupIdx.find(p.dedupKey);
            if (it == keyToGroupIdx.end()) {
                keyToGroupIdx[p.dedupKey] = groups.size();
                groups.push_back({p.dedupKey, {i}});
            } else {
                groups[it->second].second.push_back(i);
            }
        }
    }

    std::vector<DiscogsPerformance> result;
    result.reserve(groups.size() + unknownIndices.size());

    // 1. 已知演绎组：挑出最早年份的那条
    for (const auto& grp : groups) {
        const auto& indices = grp.second;
        size_t bestIdx = indices[0];
        int bestYear = rawList[bestIdx].year;

        for (size_t k = 1; k < indices.size(); ++k) {
            size_t idx = indices[k];
            int y = rawList[idx].year;
            if (bestYear <= 0 && y > 0) {
                bestIdx = idx;
                bestYear = y;
            } else if (bestYear > 0 && y > 0 && y < bestYear) {
                bestIdx = idx;
                bestYear = y;
            }
        }

        DiscogsPerformance primary = rawList[bestIdx];
        primary.otherEditions = (int)indices.size() - 1;
        primary.dedupUnknown = false;
        result.push_back(std::move(primary));
    }

    // 2. 未明演绎项：不臆造，独立保留
    for (size_t idx : unknownIndices) {
        DiscogsPerformance item = rawList[idx];
        item.otherEditions = 0;
        item.dedupUnknown = true;
        result.push_back(std::move(item));
    }

    // 3. 排序：已知年份升序，年份未知置后
    std::stable_sort(result.begin(), result.end(), [](const DiscogsPerformance& a, const DiscogsPerformance& b) {
        if (a.year > 0 && b.year > 0) return a.year < b.year;
        if (a.year > 0 && b.year <= 0) return true;
        if (a.year <= 0 && b.year > 0) return false;
        return false;
    });

    distinctCount_out = (int)result.size();
    return result;
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
    std::vector<std::string> genres; // E-5: 大类流派
    std::vector<std::string> styles; // E-5: 细分风格
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
        // E-5: 解析 genre 与 style（支持数组与单串）
        if (it["genre"].type == rta::net::Json::Arr) {
            for (size_t k = 0; k < it["genre"].size(); ++k) {
                const std::string g = it["genre"].at(k).s();
                if (!g.empty()) sri.genres.push_back(g);
            }
        } else if (it["genre"].type == rta::net::Json::Str) {
            const std::string g = it["genre"].s();
            if (!g.empty()) sri.genres.push_back(g);
        }
        if (it["style"].type == rta::net::Json::Arr) {
            for (size_t k = 0; k < it["style"].size(); ++k) {
                const std::string s = it["style"].at(k).s();
                if (!s.empty()) sri.styles.push_back(s);
            }
        } else if (it["style"].type == rta::net::Json::Str) {
            const std::string s = it["style"].s();
            if (!s.empty()) sri.styles.push_back(s);
        }
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

// 解析 GET /releases/{id} 详情及 credits
inline bool parse_discogs_release_json(const std::string& text,
                                       DiscogsReleaseDetail& det_out,
                                       std::string* err_out = nullptr) {
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        if (err_out) *err_out = "release json 不是合法对象";
        return false;
    }
    if (j.has("message")) {
        if (err_out) *err_out = "Discogs API 错误: " + j["message"].s();
        return false;
    }

    det_out.id = (int64_t)j["id"].n();
    det_out.title = j["title"].s();

    // 年份提取
    if (j["year"].n() > 0) {
        det_out.year = (int)j["year"].n();
    } else if (j.has("released")) {
        std::string rs = j["released"].s();
        if (rs.size() >= 4) det_out.year = std::atoi(rs.substr(0, 4).c_str());
    }

    // 国家
    det_out.country = j["country"].s();

    // 厂牌
    if (j["labels"].type == rta::net::Json::Arr && j["labels"].size() > 0) {
        const auto& l0 = j["labels"].at(0);
        if (l0.type == rta::net::Json::Obj) det_out.label = l0["name"].s();
        else if (l0.type == rta::net::Json::Str) det_out.label = l0.s();
    } else if (j["label"].type == rta::net::Json::Str) {
        det_out.label = j["label"].s();
    }

    // 格式
    if (j["formats"].type == rta::net::Json::Arr && j["formats"].size() > 0) {
        const auto& f0 = j["formats"].at(0);
        if (f0.type == rta::net::Json::Obj) {
            std::string fname = f0["name"].s();
            std::string fdesc;
            if (f0["descriptions"].type == rta::net::Json::Arr) {
                for (size_t d = 0; d < f0["descriptions"].size(); ++d) {
                    if (!fdesc.empty()) fdesc += ", ";
                    fdesc += f0["descriptions"].at(d).s();
                }
            }
            if (!fname.empty() && !fdesc.empty()) det_out.format = fname + ", " + fdesc;
            else if (!fname.empty()) det_out.format = fname;
            else det_out.format = fdesc;
        } else if (f0.type == rta::net::Json::Str) {
            det_out.format = f0.s();
        }
    } else if (j["format"].type == rta::net::Json::Str) {
        det_out.format = j["format"].s();
    }

    // URL
    if (j.has("uri")) {
        std::string u = j["uri"].s();
        if (u.rfind("http", 0) == 0) det_out.url = u;
        else if (!u.empty() && u.front() == '/') det_out.url = "https://www.discogs.com" + u;
    }
    if (det_out.url.empty() && det_out.id > 0) {
        det_out.url = "https://www.discogs.com/release/" + std::to_string(det_out.id);
    }

    // 提取 release notes / 描述
    if (j.has("notes") && j["notes"].type == rta::net::Json::Str) {
        det_out.notes = j["notes"].s();
    }

    // E-5: genres / styles 提取（支持 genres/genre, styles/style 数组或单串）
    auto extract_str_list = [&](const rta::net::Json& obj, const char* k1, const char* k2, std::vector<std::string>& out) {
        const rta::net::Json& f = obj.has(k1) ? obj[k1] : (obj.has(k2) ? obj[k2] : rta::net::Json());
        if (f.type == rta::net::Json::Arr) {
            for (size_t i = 0; i < f.size(); ++i) {
                const std::string s = f.at(i).s();
                if (!s.empty()) out.push_back(s);
            }
        } else if (f.type == rta::net::Json::Str) {
            const std::string s = f.s();
            if (!s.empty()) out.push_back(s);
        }
    };
    extract_str_list(j, "genres", "genre", det_out.genres);
    extract_str_list(j, "styles", "style", det_out.styles);

    // 直接键提取（支持 fixture 与简化 mock）
    if (j.has("conductor")) det_out.conductor = clean_discogs_artist(j["conductor"].s());
    if (j.has("ensemble")) det_out.ensemble = clean_discogs_artist(j["ensemble"].s());
    else if (j.has("orchestra")) det_out.ensemble = clean_discogs_artist(j["orchestra"].s());
    if (j.has("composer")) det_out.composer = clean_discogs_artist(j["composer"].s());
    if (j.has("soloists") && j["soloists"].type == rta::net::Json::Arr) {
        for (size_t i = 0; i < j["soloists"].size(); ++i) {
            std::string s = clean_discogs_artist(j["soloists"].at(i).s());
            if (!s.empty()) det_out.soloists.push_back(s);
        }
    } else if (j.has("soloist") && j["soloist"].type == rta::net::Json::Str) {
        std::string s = clean_discogs_artist(j["soloist"].s());
        if (!s.empty()) det_out.soloists.push_back(s);
    }

    // credits 条目处理闭包
    auto process_artist_entry = [&](const rta::net::Json& a) {
        if (a.type != rta::net::Json::Obj) return;
        std::string rawName = a["name"].s();
        std::string name = clean_discogs_artist(rawName);
        if (name.empty()) return;

        std::string role = a["role"].s();
        std::string roleLower = role;
        for (char& ch : roleLower) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');

        // 排除制作/技术角色
        for (const char* prodKw : {
            "mastered", "producer", "engineer", "recorded by", "mixed by", "artwork",
            "design", "photography", "liner notes", "lacquer", "management", "layout",
            "illustration", "typography", "published by", "manufactured by", "distributed by",
            "legal", "score", "libretto", "text by", "lyrics by", "reissue", "compiled by",
            "direct metal mastering" }) {
            if (roleLower.find(prodKw) != std::string::npos) return;
        }

        // 作曲家识别
        if (roleLower.find("composed by") != std::string::npos ||
            roleLower.find("composer") != std::string::npos ||
            roleLower.find("music by") != std::string::npos ||
            roleLower.find("written-by") != std::string::npos ||
            roleLower.find("written by") != std::string::npos) {
            if (det_out.composer.empty()) det_out.composer = name;
            return;
        }

        // 指挥识别
        if (roleLower.find("conductor") != std::string::npos ||
            roleLower.find("conducted by") != std::string::npos ||
            roleLower.find("dirigent") != std::string::npos ||
            roleLower.find("direction") != std::string::npos ||
            roleLower.find("chorus master") != std::string::npos) {
            if (det_out.conductor.empty()) det_out.conductor = name;
            return;
        }

        // 乐团/合奏识别
        if (roleLower.find("orchestra") != std::string::npos ||
            roleLower.find("ensemble") != std::string::npos ||
            roleLower.find("philharmonic") != std::string::npos ||
            roleLower.find("philharmoniker") != std::string::npos ||
            roleLower.find("symphony") != std::string::npos ||
            roleLower.find("sinfonieorchester") != std::string::npos ||
            roleLower.find("staatskapelle") != std::string::npos ||
            roleLower.find("orkest") != std::string::npos ||
            roleLower.find("kammerorchester") != std::string::npos ||
            roleLower.find("chorus") != std::string::npos ||
            roleLower.find("choir") != std::string::npos ||
            roleLower.find("consort") != std::string::npos ||
            roleLower.find("collegium") != std::string::npos ||
            roleLower.find("baroque") != std::string::npos ||
            roleLower.find("quartet") != std::string::npos ||
            is_likely_orchestra(name)) {
            if (det_out.ensemble.empty()) det_out.ensemble = name;
            return;
        }

        // 独奏识别
        if (roleLower.find("soloist") != std::string::npos ||
            roleLower.find("solo") != std::string::npos) {
            if (std::find(det_out.soloists.begin(), det_out.soloists.end(), name) == det_out.soloists.end()) {
                det_out.soloists.push_back(name);
            }
            return;
        }

        // 独奏乐器/人声声部识别
        for (const char* instKw : {
            "piano", "violin", "cello", "violoncello", "viola", "flute", "oboe",
            "clarinet", "bassoon", "trumpet", "horn", "trombone", "tuba", "harp",
            "harpsichord", "organ", "guitar", "lute", "soprano", "tenor", "baritone",
            "contralto", "vocals" }) {
            if (roleLower.find(instKw) != std::string::npos) {
                if (std::find(det_out.soloists.begin(), det_out.soloists.end(), name) == det_out.soloists.end()) {
                    det_out.soloists.push_back(name);
                }
                return;
            }
        }
    };

    // 1. 处理 extraartists
    if (j.has("extraartists") && j["extraartists"].type == rta::net::Json::Arr) {
        for (size_t i = 0; i < j["extraartists"].size(); ++i) {
            process_artist_entry(j["extraartists"].at(i));
        }
    }

    // 2. 处理 artists (若 conductor 或 ensemble 仍空)
    if (j.has("artists") && j["artists"].type == rta::net::Json::Arr) {
        for (size_t i = 0; i < j["artists"].size(); ++i) {
            const auto& a = j["artists"].at(i);
            std::string rawName = a["name"].s();
            std::string name = clean_discogs_artist(rawName);
            if (name.empty()) continue;
            std::string role = a["role"].s();
            if (!role.empty()) {
                process_artist_entry(a);
            } else {
                if (det_out.ensemble.empty() && is_likely_orchestra(name)) {
                    det_out.ensemble = name;
                } else if (det_out.conductor.empty() && is_likely_conductor(name)) {
                    det_out.conductor = name;
                }
            }
        }
    }

    // 3. 处理 tracklist 中的 extraartists / artists
    if (j.has("tracklist") && j["tracklist"].type == rta::net::Json::Arr) {
        for (size_t i = 0; i < j["tracklist"].size(); ++i) {
            const auto& tr = j["tracklist"].at(i);
            if (tr.type != rta::net::Json::Obj) continue;
            if (tr.has("extraartists") && tr["extraartists"].type == rta::net::Json::Arr) {
                for (size_t k = 0; k < tr["extraartists"].size(); ++k) {
                    process_artist_entry(tr["extraartists"].at(k));
                }
            }
            if (tr.has("artists") && tr["artists"].type == rta::net::Json::Arr) {
                for (size_t k = 0; k < tr["artists"].size(); ++k) {
                    const auto& a = tr["artists"].at(k);
                    std::string name = clean_discogs_artist(a["name"].s());
                    if (det_out.ensemble.empty() && is_likely_orchestra(name)) {
                        det_out.ensemble = name;
                    } else if (det_out.conductor.empty() && is_likely_conductor(name)) {
                        det_out.conductor = name;
                    } else {
                        process_artist_entry(a);
                    }
                }
            }
        }
    }

    // 4. 仍缺失时使用标题解析兜底
    if (det_out.conductor.empty() || det_out.ensemble.empty()) {
        std::string p, c, o, wt;
        parse_classical_title(det_out.title, det_out.composer, p, c, o, wt);
        if (det_out.conductor.empty() && !c.empty()) det_out.conductor = c;
        if (det_out.ensemble.empty() && !o.empty()) det_out.ensemble = o;
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

inline fsx::path release_detail_cache_path(const std::string& baseDir, int64_t release_id) {
    fsx::path p = baseDir.empty() ? meta_path_of("rlplayer-meta") : meta_path_of(baseDir);
    p /= "discogs";
    p /= "releases";
    p /= (std::to_string(release_id) + ".json");
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
      << "  \"releaseCount\": " << r.releaseCount << ",\n"
      << "  \"distinctCount\": " << r.distinctCount << ",\n"
      << "  \"release_count\": " << r.releaseCount << ",\n"
      << "  \"distinct_count\": " << r.distinctCount << ",\n"
      << "  \"performances\": [\n";
    for (size_t i = 0; i < r.performances.size(); ++i) {
        const auto& p = r.performances[i];
        o << "    {\n"
          << "      \"id\": \"" << meta_jesc(p.id) << "\",\n"
          << "      \"title\": \"" << meta_jesc(p.title) << "\",\n"
          << "      \"performers\": \"" << meta_jesc(p.performers) << "\",\n"
          << "      \"conductor\": \"" << meta_jesc(p.conductor) << "\",\n"
          << "      \"ensemble\": \"" << meta_jesc(p.ensemble) << "\",\n"
          << "      \"orchestra\": \"" << meta_jesc(!p.orchestra.empty() ? p.orchestra : p.ensemble) << "\",\n"
          << "      \"composer\": \"" << meta_jesc(p.composer) << "\",\n"
          << "      \"soloists\": [";
        for (size_t k = 0; k < p.soloists.size(); ++k) {
            if (k) o << ", ";
            o << "\"" << meta_jesc(p.soloists[k]) << "\"";
        }
        o << "],\n"
          << "      \"year\": " << p.year << ",\n"
          << "      \"label\": \"" << meta_jesc(p.label) << "\",\n"
          << "      \"format\": \"" << meta_jesc(p.format) << "\",\n"
          << "      \"country\": \"" << meta_jesc(p.country) << "\",\n"
          << "      \"url\": \"" << meta_jesc(p.url) << "\",\n"
          << "      \"source\": \"" << meta_jesc(p.source) << "\",\n"
          << "      \"fetched_at\": " << p.fetched_at << ",\n"
          << "      \"detailFetched\": " << (p.detailFetched ? "true" : "false") << ",\n"
          << "      \"dedupKey\": \"" << meta_jesc(p.dedupKey) << "\",\n"
          << "      \"otherEditions\": " << p.otherEditions << ",\n"
          << "      \"dedupUnknown\": " << (p.dedupUnknown ? "true" : "false") << "\n"
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
    if (j.has("releaseCount")) r_out.releaseCount = (int)j["releaseCount"].n();
    else if (j.has("release_count")) r_out.releaseCount = (int)j["release_count"].n();
    if (j.has("distinctCount")) r_out.distinctCount = (int)j["distinctCount"].n();
    else if (j.has("distinct_count")) r_out.distinctCount = (int)j["distinct_count"].n();

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
            if (item.has("ensemble")) p.ensemble = item["ensemble"].s();
            if (item.has("orchestra")) p.orchestra = item["orchestra"].s();
            if (p.orchestra.empty()) p.orchestra = p.ensemble;
            if (p.ensemble.empty()) p.ensemble = p.orchestra;

            p.soloists.clear();
            if (item.has("soloists") && item["soloists"].type == rta::net::Json::Arr) {
                for (size_t k = 0; k < item["soloists"].size(); ++k) {
                    p.soloists.push_back(item["soloists"].at(k).s());
                }
            }
            if (item.has("composer")) p.composer = item["composer"].s();
            p.year = (int)item["year"].n();
            p.label = item["label"].s();
            p.format = item["format"].s();
            p.country = item["country"].s();
            p.url = item["url"].s();
            p.source = item["source"].s();
            if (p.source.empty()) p.source = "discogs";
            p.fetched_at = (long long)item["fetched_at"].n();

            p.detailFetched = item.has("detailFetched") ? item["detailFetched"].b : false;
            p.dedupKey = item.has("dedupKey") ? item["dedupKey"].s() : "";
            p.otherEditions = item.has("otherEditions") ? (int)item["otherEditions"].n() : 0;
            p.dedupUnknown = item.has("dedupUnknown") ? item["dedupUnknown"].b : (p.dedupKey.empty());

            r_out.performances.push_back(std::move(p));
        }
    }
    if (r_out.distinctCount == 0 && !r_out.performances.empty()) {
        r_out.distinctCount = (int)r_out.performances.size();
    }
    if (r_out.releaseCount == 0 && !r_out.performances.empty()) {
        int totalRel = 0;
        for (const auto& p : r_out.performances) totalRel += (1 + p.otherEditions);
        r_out.releaseCount = totalRel;
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

inline bool release_detail_load_cache(const std::string& baseDir,
                                      int64_t release_id,
                                      std::string& json_out,
                                      long long now_sec = 0) {
    if (release_id <= 0) return false;
    const fsx::path p = release_detail_cache_path(baseDir, release_id);
    std::string text;
    if (!entity_read_file(p, text)) return false;
    if (now_sec <= 0) now_sec = (long long)std::time(nullptr);

    // 检查缓存内记录的过期时间
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type == rta::net::Json::Obj && j.has("_expires_at")) {
        long long exp = (long long)j["_expires_at"].n();
        if (exp > 0 && now_sec >= exp) {
            return false; // 已过期
        }
    }
    json_out = std::move(text);
    return true;
}

inline bool release_detail_save_cache(const std::string& baseDir,
                                      int64_t release_id,
                                      const std::string& json_str) {
    if (release_id <= 0 || json_str.empty()) return false;
    const fsx::path p = release_detail_cache_path(baseDir, release_id);

    // 注入 TTL 字段 (_cached_at, _expires_at 7天)
    const long long now_sec = (long long)std::time(nullptr);
    const long long exp_sec = now_sec + 7 * 86400;

        std::string toWrite = json_str;
    size_t firstBrace = toWrite.find('{');
    if (firstBrace != std::string::npos) {
        std::string inject = "{\n  \"_cached_at\": " + std::to_string(now_sec) +
                             ",\n  \"_expires_at\": " + std::to_string(exp_sec) + ",\n";
        toWrite.replace(firstBrace, 1, inject);
    }
    return entity_write_atomic(p, toWrite);
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
    const long long now_sec = (long long)std::time(nullptr);

    // 1. 检查缓存
    WorkVersionsResult cached;
    std::string cache_err;
    const bool have_cache = versions_load_cache(baseDir, eid, cached, &cache_err);
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

    // 检查缓存中的演绎详情是否完整（旧缓存缺 detailFetched 或为 false 视为未取详情，可刷新）
    bool cachedDetailsComplete = false;
    if (have_cache && cached.ok && !cached.performances.empty()) {
        cachedDetailsComplete = true;
        for (const auto& cp : cached.performances) {
            if (!cp.detailFetched) {
                cachedDetailsComplete = false;
                break;
            }
        }
    }

    if (!opts.refresh && cache_unexpired && cachedDetailsComplete) {
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

    // 3. 执行搜索 (优先 type=release 检索独立发行版)
    const std::vector<std::string> reqHeaders = {
        "User-Agent: RLPlayer/1.0.10 (metadata; contact: local)",
        "Accept: application/json"
    };

    const int targetFetchCount = opts.max_releases > 0 ? opts.max_releases : 12;
    std::string searchUrl;
    if (!composer.empty()) {
        searchUrl = "https://api.discogs.com/database/search?release_title=" +
                    discogs_url_encode(workTitle) +
                    "&artist=" + discogs_url_encode(composer) +
                    "&type=release&per_page=" + std::to_string(targetFetchCount + 3);
    } else {
        searchUrl = "https://api.discogs.com/database/search?q=" +
                    discogs_url_encode(workTitle) +
                    "&type=release&per_page=" + std::to_string(targetFetchCount + 3);
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

    // 回退一：若精准搜索为空且存在 composer，退化至全文字词搜索 q=<composer> <workTitle>&type=release
    if (searchHits.empty() && !composer.empty()) {
        std::string fallbackUrl = "https://api.discogs.com/database/search?q=" +
                                  discogs_url_encode(composer + " " + workTitle) +
                                  "&type=release&per_page=" + std::to_string(targetFetchCount + 3);
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

    // 回退二：若仍无结果，尝试不限定 type（可能命中 master 或 fixture）
    if (searchHits.empty()) {
        std::string fallbackUrl2 = "https://api.discogs.com/database/search?q=" +
                                   discogs_url_encode(workTitle) +
                                   "&per_page=" + std::to_string(targetFetchCount + 3);
        if (limiter.acquire(opts.timeout_ms)) {
            auto fbResp2 = rta::net::http_get(fallbackUrl2, reqHeaders, opts.timeout_ms);
            if (fbResp2.status == 429) {
                limiter.trip_circuit(60);
            } else if (fbResp2.ok) {
                const std::string fbBody(fbResp2.body.begin(), fbResp2.body.end());
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
        res.ok = true; // 契约：搜索无版本不是协议错误，如实返回 0 条
        res.complete = false;
        res.releaseCount = 0;
        res.distinctCount = 0;
        res.generated_at = now_sec;
        res.expires_at = now_sec + 7 * 86400;
        return res;
    }

    // 4. 收集待抓取详情的候选 release 列表（最多 targetFetchCount 条）
    struct CandidateItem {
        int64_t id = 0;
        std::string type;
        std::string title;
        int year = 0;
        std::string label;
        std::string format;
        std::string country;
        std::string uri;
    };
    std::vector<CandidateItem> candidates;

    for (const auto& hit : searchHits) {
        if ((int)candidates.size() >= targetFetchCount) break;

        if (hit.type == "master" && hit.id > 0) {
            // master 类型拉取其 versions
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
                            if (vi.id <= 0) continue;
                            bool exists = false;
                            for (const auto& c : candidates) {
                                if (c.id == vi.id) { exists = true; break; }
                            }
                            if (!exists) {
                                CandidateItem ci;
                                ci.id = vi.id;
                                ci.type = "release";
                                ci.title = !vi.title.empty() ? vi.title : hit.title;
                                ci.year = vi.year > 0 ? vi.year : hit.year;
                                ci.label = vi.label;
                                ci.format = vi.format;
                                ci.country = vi.country;
                                ci.uri = "/release/" + std::to_string(vi.id);
                                candidates.push_back(std::move(ci));
                                if ((int)candidates.size() >= targetFetchCount) break;
                            }
                        }
                    }
                }
            }
        } else {
            bool exists = false;
            for (const auto& c : candidates) {
                if (c.id == hit.id) { exists = true; break; }
            }
            if (!exists) {
                CandidateItem ci;
                ci.id = hit.id;
                ci.type = hit.type;
                ci.title = hit.title;
                ci.year = hit.year;
                ci.label = hit.label;
                ci.format = hit.format;
                ci.country = hit.country;
                ci.uri = hit.uri;
                candidates.push_back(std::move(ci));
            }
        }
    }

    // 5. 对候选 release 抓取 GET /releases/{id} 详情并解析 credits
    std::vector<DiscogsPerformance> rawList;

    for (const auto& cand : candidates) {
        DiscogsPerformance dp;
        dp.id = (cand.type == "master" ? "discogs-master:" : "discogs-release:") + std::to_string(cand.id);
        dp.source = "discogs";
        dp.fetched_at = now_sec;

        bool detailSuccess = false;
        std::string detailJson;

        // 若不是 master，调用 releases API 抓取 credits
        if (cand.type != "master" && cand.id > 0) {
            // 先查本地 release 缓存
            if (!opts.refresh && release_detail_load_cache(baseDir, cand.id, detailJson, now_sec)) {
                detailSuccess = true;
            } else if (!opts.offline) {
                if (limiter.acquire(opts.timeout_ms < 5000 ? opts.timeout_ms : 5000)) {
                    std::string relUrl = "https://api.discogs.com/releases/" + std::to_string(cand.id);
                    auto relResp = rta::net::http_get(relUrl, reqHeaders, opts.timeout_ms);
                    if (relResp.status == 429) {
                        limiter.trip_circuit(60);
                    } else if (relResp.ok) {
                        detailJson = std::string(relResp.body.begin(), relResp.body.end());
                        release_detail_save_cache(baseDir, cand.id, detailJson);
                        detailSuccess = true;
                    }
                }
            }
        }

        DiscogsReleaseDetail det;
        if (detailSuccess && parse_discogs_release_json(detailJson, det)) {
            dp.title = !det.title.empty() ? det.title : cand.title;
            dp.conductor = det.conductor;
            dp.ensemble = det.ensemble;
            dp.orchestra = det.ensemble;
            dp.soloists = det.soloists;
            dp.composer = !det.composer.empty() ? det.composer : composer;
            dp.year = det.year > 0 ? det.year : cand.year;
            dp.label = !det.label.empty() ? det.label : cand.label;
            dp.country = !det.country.empty() ? det.country : cand.country;
            dp.format = !det.format.empty() ? det.format : cand.format;
            dp.url = !det.url.empty() ? det.url : ("https://www.discogs.com/release/" + std::to_string(cand.id));
            dp.detailFetched = true;
            dp.performers = format_performers_string(dp.conductor, dp.ensemble, dp.soloists, cand.title);
        } else {
            // 详情不可达时，回退至基础标题解析
            std::string perfArtists, perfConductor, perfOrchestra, perfWorkTitle;
            parse_classical_title(cand.title, composer, perfArtists, perfConductor, perfOrchestra, perfWorkTitle);
            dp.title = !perfWorkTitle.empty() ? perfWorkTitle : cand.title;
            dp.performers = perfArtists;
            dp.conductor = perfConductor;
            dp.ensemble = perfOrchestra;
            dp.orchestra = perfOrchestra;
            dp.composer = composer;
            dp.year = cand.year;
            dp.label = cand.label;
            dp.format = cand.format;
            dp.country = cand.country;
            dp.url = cand.type == "master"
                ? ("https://www.discogs.com/master/" + std::to_string(cand.id))
                : ("https://www.discogs.com/release/" + std::to_string(cand.id));
            dp.detailFetched = false;
        }

        dp.dedupKey = build_dedup_key(dp.conductor, dp.ensemble, dp.soloists);
        rawList.push_back(std::move(dp));
    }

    // 6. 按演绎去重（dedupKey 相同保留最早 year，折叠计数 otherEditions；空 key 保留并标 dedupUnknown: true）
    res.performances = deduplicate_performances(rawList, res.releaseCount, res.distinctCount);

    res.ok = true;
    res.generated_at = now_sec;
    res.expires_at = now_sec + 7 * 86400;

    // complete 判定升级：去重后 distinct 演绎数 >= 5 且每条都有有效 url
    bool allUrlsOk = !res.performances.empty();
    for (const auto& p : res.performances) {
        if (p.url.empty()) { allUrlsOk = false; break; }
    }
    res.complete = (res.distinctCount >= 5) && allUrlsOk;

    // 7. 落盘持久化缓存
    if (!res.performances.empty()) {
        versions_save_cache(baseDir, eid, res);
    }

    return res;
}

// ---------------------------------------------------------------------------
// E-5: Discogs 流派与风格（styles / genres）提取与检索纯函数/服务
// ---------------------------------------------------------------------------

// 纯函数：将 Discogs styles 与 genres 格式化为规范的流派字符串
// 优先使用具体的 styles（如 "Modern, Impressionist"）；若 styles 为空则回退到大类 genres（如 "Classical"）
inline std::string format_discogs_genre(const std::vector<std::string>& genres,
                                        const std::vector<std::string>& styles) {
    std::ostringstream oss;
    if (!styles.empty()) {
        for (size_t i = 0; i < styles.size(); ++i) {
            if (i) oss << ", ";
            oss << styles[i];
        }
    } else if (!genres.empty()) {
        for (size_t i = 0; i < genres.size(); ++i) {
            if (i) oss << ", ";
            oss << genres[i];
        }
    }
    return oss.str();
}

using DiscogsTransportHook = std::function<rta::net::HttpResponse(
    const std::string& url, const std::vector<std::string>& headers, int timeout_ms)>;

inline DiscogsTransportHook& discogs_transport_hook() {
    static DiscogsTransportHook h = nullptr;
    return h;
}

inline void set_discogs_transport_for_test(DiscogsTransportHook hook) {
    discogs_transport_hook() = std::move(hook);
}

// 根据 artist / title / album 从 Discogs 检索 styles/genres 事实（L1 结构化事实源）
inline std::string discogs_lookup_genre(const std::string& artist,
                                        const std::string& title,
                                        const std::string& album,
                                        std::string& err) {
    err.clear();
    std::string q;
    if (!artist.empty() && !title.empty()) q = artist + " " + title;
    else if (!title.empty()) q = title;
    else if (!album.empty()) q = album;
    if (q.empty()) return "";

    if (!discogs_transport_hook() && !DiscogsRateLimiter::instance().acquire(2500)) {
        err = "discogs: rate limited or circuit open";
        return "";
    }

    const std::string url = "https://api.discogs.com/database/search?q=" +
                            discogs_url_encode(q) + "&type=release";
    const std::vector<std::string> headers = {
        "User-Agent: RLPlayer/1.0.10 (metadata; contact: local)"
    };

    rta::net::HttpResponse resp;
    if (discogs_transport_hook()) {
        resp = discogs_transport_hook()(url, headers, 5000);
    } else {
        resp = rta::net::http_get(url, headers, 10000);
    }

    if (resp.status != 200 || resp.body.empty()) {
        if (resp.status == 429) DiscogsRateLimiter::instance().trip_circuit(60);
        err = "discogs HTTP " + std::to_string(resp.status);
        return "";
    }

    std::vector<SearchResultItem> items;
    std::string perr;
    const std::string bodyStr(resp.body.begin(), resp.body.end());
    if (!parse_discogs_search_json(bodyStr, items, &perr) || items.empty()) {
        err = perr;
        return "";
    }

    for (const auto& it : items) {
        const std::string g = format_discogs_genre(it.genres, it.styles);
        if (!g.empty()) return g;
    }
    return "";
}

} // namespace rta::meta
