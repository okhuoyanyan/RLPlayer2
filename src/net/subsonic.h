// subsonic.h — Subsonic / Navidrome 客户端
//
// 关键设计：播放只走 download.view（原始文件），绝不走 stream.view 的转码参数。
// 这是"不降低音质"的硬约束 —— Subsonic 协议允许服务端转码，
// 一旦请求里带了 maxBitRate / format 就会拿到有损流。
#pragma once
#include "net/http_client.h"
#include "net/md5.h"
#include "net/json.h"
#include <string>
#include <vector>
#include <random>

namespace rta::net {

struct SubConfig {
    std::string base;      // 例: http://192.168.50.10:4533
    std::string user;
    std::string pass;
    bool        valid() const { return !base.empty() && !user.empty(); }
};

struct SubEntry {
    std::string id, title, artist, album, suffix, contentType, coverId;
    bool        isDir = false;
    double      duration = 0;
    long long   size = 0;
};

class Subsonic {
public:
    Subsonic() {}
    explicit Subsonic(const SubConfig& c) : cfg_(c) { make_salt(); }

    void set_config(const SubConfig& c) { cfg_ = c; make_salt(); cfg_.base = strip_trailing(cfg_.base); }
    const SubConfig& config() const { return cfg_; }
    bool configured() const { return cfg_.valid(); }

    // 只做连通性与鉴权检查
    bool ping(std::string& err) const {
        auto r = http_get(auth_url("/rest/ping.view", {}));
        if (!r.ok) { err = r.error; return false; }
        const Json j = json_parse(std::string(r.body.begin(), r.body.end()));
        const Json& sr = j["subsonic-response"];
        if (!sr.ok()) { err = "响应不是 Subsonic 格式"; return false; }
        if (sr["status"].s() != "ok") {
            err = sr["error"]["message"].s();
            if (err.empty()) err = "鉴权失败";
            return false;
        }
        return true;
    }

    // 浏览某一层（id 为空 = 根）
    std::vector<SubEntry> browse(const std::string& id, std::string& err) const {
        std::vector<SubEntry> out;
        std::vector<std::pair<std::string, std::string>> q;
        if (!id.empty()) q.push_back({ "id", id });
        auto r = http_get(auth_url("/rest/getMusicDirectory.view", q));
        if (!r.ok) { err = r.error; return out; }
        const Json j = json_parse(std::string(r.body.begin(), r.body.end()));
        const Json& sr = j["subsonic-response"];
        if (sr["status"].s() != "ok") { err = sr["error"]["message"].s(); return out; }
        const Json& dir = sr["directory"];
        const Json& ch = dir["child"];
        for (size_t i = 0; i < ch.size(); ++i) out.push_back(to_entry(ch.at(i)));
        return out;
    }

    // 顶层：用 getIndexes 拿艺术家/目录
    std::vector<SubEntry> indexes(std::string& err) const {
        std::vector<SubEntry> out;
        auto r = http_get(auth_url("/rest/getIndexes.view", {}));
        if (!r.ok) { err = r.error; return out; }
        const Json j = json_parse(std::string(r.body.begin(), r.body.end()));
        const Json& sr = j["subsonic-response"];
        if (sr["status"].s() != "ok") { err = sr["error"]["message"].s(); return out; }
        const Json& ix = sr["indexes"]["index"];
        for (size_t i = 0; i < ix.size(); ++i) {
            const Json& artist = ix.at(i)["artist"];
            for (size_t k = 0; k < artist.size(); ++k) out.push_back(to_entry(artist.at(k)));
        }
        return out;
    }

    std::vector<SubEntry> search(const std::string& query, std::string& err) const {
        std::vector<SubEntry> out;
        auto r = http_get(auth_url("/rest/search3.view", { {"query", query} }));
        if (!r.ok) { err = r.error; return out; }
        const Json j = json_parse(std::string(r.body.begin(), r.body.end()));
        const Json& sr = j["subsonic-response"];
        if (sr["status"].s() != "ok") { err = sr["error"]["message"].s(); return out; }
        const Json& songs = sr["searchResult3"]["song"];
        for (size_t i = 0; i < songs.size(); ++i) out.push_back(to_entry(songs.at(i)));
        return out;
    }

    // ★ 原始文件（不转码）
    HttpResponse download(const std::string& id) const {
        return http_get(auth_url("/rest/download.view", { {"id", id} }), {}, 60000);
    }

    HttpResponse cover(const std::string& id, int size = 400) const {
        return http_get(auth_url("/rest/getCoverArt.view",
                                 { {"id", id}, {"size", std::to_string(size)} }), {}, 15000);
    }

    // 解析 "http://host:port/prefix?u=..&t=..&s=..&v=..&c=..&f=json" 形式
    std::string auth_url(const std::string& path,
                         const std::vector<std::pair<std::string, std::string>>& q) const {
        std::string u = strip_trailing(cfg_.base) + path;
        u += "?u=" + urlencode(cfg_.user);
        u += "&t=" + md5_hex(cfg_.pass + salt_);
        u += "&s=" + salt_;
        u += "&v=1.16.1&c=RTA&f=json";
        for (const auto& kv : q) u += "&" + kv.first + "=" + urlencode(kv.second);
        return u;
    }

private:
    SubConfig cfg_;
    std::string salt_;

    void make_salt() {
        static const char* cs = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)GetTickCount64());
        salt_.clear();
        for (int i = 0; i < 10; ++i) salt_ += cs[rng() % 36];
    }

    static std::string strip_trailing(std::string s) {
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }

    static std::string urlencode(const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
            else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
        }
        return o;
    }

    static SubEntry to_entry(const Json& n) {
        SubEntry e;
        e.id          = n["id"].s();
        e.title       = n["title"].s();
        e.artist      = n["artist"].s();
        e.album       = n["album"].s();
        e.suffix      = n["suffix"].s();
        e.contentType = n["contentType"].s();
        e.coverId     = n["coverArt"].s();
        e.duration    = n["duration"].n();
        e.size        = (long long)n["size"].n();
        e.isDir       = (n["isDir"].type == Json::Bool) ? n["isDir"].b : false;
        if (e.title.empty()) e.title = n["name"].s();
        return e;
    }
};

} // namespace rta::net
