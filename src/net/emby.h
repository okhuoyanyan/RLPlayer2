// emby.h — Emby 客户端
//
// 与 Subsonic 客户端相同的硬约束：播放只取【原始文件】
//   GET /Items/{id}/File?api_key=...   ← 服务端直接把磁盘上的文件发出来，不转码
// 绝不用 /Audio/{id}/stream（默认可能走转码管线）。
#pragma once
#include "net/http_client.h"
#include "net/json.h"
#include "net/subsonic.h"      // 复用 SubEntry
#include <string>
#include <vector>
#include <cstdio>

namespace rta::net {

struct EmbyConfig {
    std::string base, user, pass;
    bool valid() const { return !base.empty() && !user.empty(); }
};

class Emby {
public:
    Emby() {}
    explicit Emby(const EmbyConfig& c) { set_config(c); }

    void set_config(const EmbyConfig& c) {
        cfg_ = c;
        while (!cfg_.base.empty() && cfg_.base.back() == '/') cfg_.base.pop_back();
        token_.clear(); userId_.clear(); id_.clear();
        // DeviceId 固定即可（Emby 用它区分设备）
        id_ = "rlplayer";
    }
    const EmbyConfig& config() const { return cfg_; }
    bool configured() const { return cfg_.valid(); }
    bool authed() const { return !token_.empty() && !userId_.empty(); }

    // 登录（需要 POST JSON）。成功后 token_/userId_ 有效。
    bool login(std::string& err) {
        if (!cfg_.valid()) { err = "未配置服务器"; return false; }
        if (authed()) return true;

        std::string body = "{\"Username\":\"" + json_escape(cfg_.user) +
                           "\",\"Pw\":\"" + json_escape(cfg_.pass) + "\"}";
        const std::vector<std::string> hdrs = {
            "X-Emby-Authorization: MediaBrowser Client=\"RLPlayer\", Device=\"Desktop\", "
            "DeviceId=\"" + id_ + "\", Version=\"1.0.0\""
        };
        auto r = http_post(cfg_.base + "/Users/AuthenticateByName", body, hdrs);
        if (!r.ok) {
            if (r.status == 401) err = "用户名或密码错误";
            else err = r.ok ? "登录失败" : r.error;
            return false;
        }
        const Json j = json_parse(std::string(r.body.begin(), r.body.end()));
        token_  = j["AccessToken"].s();
        userId_ = j["User"]["Id"].s();
        if (token_.empty() || userId_.empty()) {
            // 有的服务器/反向代理会以 200 + {"error":...} 的形式返回失败，
            // 把服务端的话原样透出来，比一个笼统的"缺少字段"有用得多。
            const std::string sv = j["error"].s();
            err = sv.empty() ? "登录响应缺少 AccessToken / User.Id" : sv;
            token_.clear(); userId_.clear();
            return false;
        }
        return true;
    }

    // 浏览：返回子项（0 = 根）
    std::vector<SubEntry> browse(const std::string& parentId, std::string& err) {
        std::vector<SubEntry> out;
        if (!login(err)) return out;
        std::string q = cfg_.base + "/Users/" + userId_ + "/Items?IncludeItemTypes="
                        "MusicArtist,MusicAlbum,Audio&Recursive=false&SortBy=SortName&Limit=500"
                        "&Fields=RunTimeTicks,Artists,Album,AlbumId,Container";
        if (!parentId.empty()) q += "&ParentId=" + parentId;
        q += "&api_key=" + token_;

        auto r = http_get(q);
        if (!r.ok) { err = r.error; return out; }
        const Json j = json_parse(std::string(r.body.begin(), r.body.end()));
        const Json& items = j["Items"];
        for (size_t i = 0; i < items.size(); ++i) out.push_back(to_entry(items.at(i)));
        return out;
    }

    std::vector<SubEntry> search(const std::string& query, std::string& err) {
        std::vector<SubEntry> out;
        if (!login(err)) return out;
        std::string q = cfg_.base + "/Users/" + userId_ + "/Items?searchTerm=" + url_enc(query) +
                        "&IncludeItemTypes=Audio&Recursive=true&Limit=200"
                        "&Fields=RunTimeTicks,Artists,Album&api_key=" + token_;
        auto r = http_get(q);
        if (!r.ok) { err = r.error; return out; }
        const Json j = json_parse(std::string(r.body.begin(), r.body.end()));
        const Json& items = j["Items"];
        for (size_t i = 0; i < items.size(); ++i) out.push_back(to_entry(items.at(i)));
        return out;
    }

    // ★ 原始文件
    HttpResponse download(const std::string& itemId) {
        std::string err;
        if (!login(err)) { HttpResponse r; r.error = err; return r; }
        return http_get(cfg_.base + "/Items/" + itemId + "/File?api_key=" + token_, {}, 60000);
    }

    HttpResponse cover(const std::string& itemId, int maxWidth = 400) {
        std::string err;
        if (!login(err)) { HttpResponse r; r.error = err; return r; }
        return http_get(cfg_.base + "/Items/" + itemId + "/Images/Primary?maxWidth=" +
                        std::to_string(maxWidth) + "&api_key=" + token_, {}, 15000);
    }

private:
    EmbyConfig cfg_;
    std::string token_, userId_, id_;

    static std::string json_escape(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else o += c;
        }
        return o;
    }
    static std::string url_enc(const std::string& s) {
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
        e.id = n["Id"].s();
        e.title = n["Name"].s();
        const std::string type = n["Type"].s();
        e.isDir = (type == "MusicArtist" || type == "MusicAlbum" || type == "Folder");
        e.album = n["Album"].s();
        // Emby 的艺术家在 Artists 数组里
        const Json& arts = n["Artists"];
        if (arts.size() > 0) e.artist = arts.at(0).s();
        if (e.artist.empty()) e.artist = n["AlbumArtist"].s();
        // RunTimeTicks: 1 秒 = 10,000,000
        const double ticks = n["RunTimeTicks"].n();
        if (ticks > 0) e.duration = ticks / 10000000.0;
        e.suffix = n["Container"].s();
        e.coverId = e.id;
        return e;
    }
};

} // namespace rta::net
