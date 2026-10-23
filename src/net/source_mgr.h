// source_mgr.h — 音源管理（本地文件 + Subsonic/Navidrome/Emby 远程）
//
// 核心原则：远程曲目一律先【完整下载原始文件】到本地缓存，再走与本地文件
// 完全相同的解码与 DSP 路径。绝不接受服务端转码流，因此音质与本地播放一致。
//
// 2026-10-24（用户 261024-01 #5）：单服务器 → 多服务器。
//   * 服务器列表存在 rlplayer-sources.conf（v2：version/nextid/active + srv<id>.<字段>）；
//   * 旧格式（kind/base/user/pass 四行）在 load() 里一次性只读迁移成 1 台，写回时变 v2；
//   * 同一时刻只有一台「启用」（active）—— 流媒体浏览/播放/下载都走它；
//   * 密码只写不读：GET /api/src/config 永不回传明文（见 rtaweb.cpp）。
#pragma once
#include "net/subsonic.h"
#include "net/emby.h"
#include "platform/path.h"          // exe 目录 / 数据目录 / UTF-8 拼接（本文件不再碰 Win32）
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>

namespace rta::net {

struct SourceConfig {
    std::string id;                    // 稳定标识（数字串）：conf 里写作 srv<id>.<字段>
    std::string name;                  // 用户起的名字；空 = 按主机名自动生成
    std::string kind = "subsonic";     // subsonic / emby
    std::string base, user, pass;
    bool valid() const { return !base.empty() && !user.empty(); }
    bool enabled() const { return valid(); }   // 旧调用点兼容
    // 默认显示名：取 URL 的 host[:port]（比"未命名"有用，且不含账号信息）
    std::string display_name() const {
        if (!name.empty()) return name;
        std::string b = base;
        const size_t p = b.find("//");
        if (p != std::string::npos) b = b.substr(p + 2);
        const size_t s = b.find('/');
        if (s != std::string::npos) b = b.substr(0, s);
        if (b.empty()) return kind == "emby" ? "Emby" : "Navidrome / Subsonic";
        return b;
    }
};

class SourceManager {
public:
    static SourceManager& instance() { static SourceManager m; return m; }

    // ---------------- 服务器列表 ----------------
    std::vector<SourceConfig> servers() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return servers_;
    }
    std::string active_id() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return active_;
    }

    // 取当前启用的服务器（没有则返回空配置；旧调用点 config() 语义不变）
    SourceConfig config() const {
        std::lock_guard<std::mutex> lk(mtx_);
        const SourceConfig* c = active_locked();
        return c ? *c : SourceConfig{};
    }

    // 兼容旧接口：更新当前启用服务器；一台都没有时新建一台。
    void set_config(const SourceConfig& c) {
        std::lock_guard<std::mutex> lk(mtx_);
        SourceConfig* a = mutable_active_locked();
        if (a) {
            const std::string keepId = a->id;
            *a = c;
            a->id = keepId;
            if (a->name.empty()) a->name = a->display_name();
        } else {
            SourceConfig n = c;
            n.id = next_id_locked();
            if (n.name.empty()) n.name = n.display_name();
            servers_.push_back(n);
            active_ = n.id;
        }
        apply_active_locked();
        save_locked();
    }

    // 新增一台；返回新 id（失败返回空串）
    std::string add_server(SourceConfig c) {
        std::lock_guard<std::mutex> lk(mtx_);
        c.id = next_id_locked();
        if (c.name.empty()) c.name = c.display_name();
        servers_.push_back(c);
        if (active_.empty()) active_ = c.id;
        apply_active_locked();
        save_locked();
        return c.id;
    }

    // 修改一台（keepPass=true 且 c.pass 为空 → 保留原密码）
    bool update_server(const std::string& id, SourceConfig c, bool keepPass) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& s : servers_) {
            if (s.id != id) continue;
            if (keepPass && c.pass.empty()) c.pass = s.pass;
            c.id = id;
            if (c.name.empty()) c.name = c.display_name();
            s = c;
            apply_active_locked();
            save_locked();
            return true;
        }
        return false;
    }

    bool remove_server(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto it = std::find_if(servers_.begin(), servers_.end(),
                                     [&](const SourceConfig& s) { return s.id == id; });
        if (it == servers_.end()) return false;
        servers_.erase(it);
        if (active_ == id) {
            active_.clear();
            for (const auto& s : servers_) if (s.valid()) { active_ = s.id; break; }
        }
        apply_active_locked();
        save_locked();
        return true;
    }

    bool set_active(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& s : servers_) {
            if (s.id != id) continue;
            active_ = id;
            apply_active_locked();
            save_locked();
            return true;
        }
        return false;
    }

    // ---------------- 连通性 ----------------
    bool ping(std::string& err) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const SourceConfig* c = active_locked();
        if (!c) { err = "还没有启用任何流媒体服务器"; return false; }
        return ping_cfg_locked(*c, err);
    }
    // 测试列表里任意一台（不影响当前启用）
    bool ping_server(const std::string& id, std::string& err) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const SourceConfig* c = find_locked(id);
        if (!c) { err = "服务器不存在（可能已被删除）"; return false; }
        return ping_cfg_locked(*c, err);
    }
    // 测试「还没保存」的表单内容（添加/编辑时先测再存）
    bool ping_config(const SourceConfig& c, std::string& err) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return ping_cfg_locked(c, err);
    }

    // ---------------- 浏览 / 搜索 / 封面 / 原始文件 ----------------
    std::vector<SubEntry> browse(const std::string& id, std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        const SourceConfig* c = active_locked();
        if (!c) { err = "还没有启用任何流媒体服务器"; return {}; }
        apply_active_locked();
        if (c->kind == "emby") return emby_.browse(id, err);
        return sub_.browse(id, err);
    }
    std::vector<SubEntry> indexes(std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        const SourceConfig* c = active_locked();
        if (!c) { err = "还没有启用任何流媒体服务器"; return {}; }
        apply_active_locked();
        if (c->kind == "emby") return emby_.browse("", err);
        return sub_.indexes(err);
    }
    std::vector<SubEntry> search(const std::string& q, std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        const SourceConfig* c = active_locked();
        if (!c) { err = "还没有启用任何流媒体服务器"; return {}; }
        apply_active_locked();
        if (c->kind == "emby") return emby_.search(q, err);
        return sub_.search(q, err);
    }
    HttpResponse cover(const std::string& id, int size = 400) {
        std::lock_guard<std::mutex> lk(mtx_);
        const SourceConfig* c = active_locked();
        if (!c) return HttpResponse{};
        apply_active_locked();
        if (c->kind == "emby") return emby_.cover(id, size);
        return sub_.cover(id, size);
    }

    // ★ 下载原始文件到缓存，返回本地路径。失败返回空串。
    std::string fetch(const std::string& id, const std::string& suffix, std::string& err) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = cache_.find(id);
            if (it != cache_.end()) {
                std::error_code ec;
                if (std::filesystem::exists(it->second, ec) && std::filesystem::file_size(it->second, ec) > 0)
                    return it->second;
            }
        }
        HttpResponse r;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            const SourceConfig* c = active_locked();
            if (!c) { err = "还没有启用任何流媒体服务器"; return ""; }
            apply_active_locked();
            r = (c->kind == "emby") ? emby_.download(id) : sub_.download(id);
        }
        if (!r.ok || r.body.empty()) { err = r.ok ? "下载为空" : r.error; return ""; }

        // 拒绝明显的转码响应（服务端返回 HTML 或 JSON 说明出错了）
        const std::string ct = r.contentType;
        if (ct.find("json") != std::string::npos || ct.find("html") != std::string::npos) {
            err = "服务端返回的不是音频（Content-Type: " + ct + "）";
            return "";
        }

        const std::string dir = cache_dir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::string safeId;
        for (char c : id) safeId += (isalnum((unsigned char)c) ? c : '_');
        std::string ext = suffix.empty() ? std::string(".audio") : ("." + suffix);
        const auto u8p = (std::filesystem::path(dir) / (safeId + ext)).u8string();
        const std::string path(reinterpret_cast<const char*>(u8p.data()), u8p.size());

        std::ofstream f(path, std::ios::binary);
        if (!f) { err = "无法写入缓存"; return ""; }
        f.write((const char*)r.body.data(), (std::streamsize)r.body.size());
        f.close();

        std::lock_guard<std::mutex> lk(mtx_);
        cache_[id] = path;
        return path;
    }

    size_t cache_count() const { std::lock_guard<std::mutex> lk(mtx_); return cache_.size(); }

    // 从磁盘加载（启动时调用一次）。旧格式（单服务器四行）只读迁移，不改盘。
    void load(const std::string& confPath) {
        std::lock_guard<std::mutex> lk(mtx_);
        confPath_ = confPath;
        servers_.clear();
        active_.clear();
        next_id_ = 1;
        std::ifstream f(confPath);
        if (!f) return;

        std::map<std::string, std::map<std::string, std::string>> rows;   // id → 字段
        std::string legacyKind, legacyBase, legacyUser, legacyPass;
        std::string line;
        int nextId = 0, maxId = 0;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t e = line.find('=');
            if (e == std::string::npos) continue;
            const std::string k = line.substr(0, e);
            const std::string v = line.substr(e + 1);
            if (k == "nextid") { nextId = atoi(v.c_str()); continue; }
            if (k == "active") { active_ = v; continue; }
            if (k == "kind") { legacyKind = v; continue; }
            if (k == "base") { legacyBase = v; continue; }
            if (k == "user") { legacyUser = v; continue; }
            if (k == "pass") { legacyPass = v; continue; }
            if (k.rfind("srv", 0) != 0) continue;               // version=2 行等
            const size_t dot = k.find('.', 3);
            if (dot == std::string::npos) continue;
            const std::string id = k.substr(3, dot - 3);
            const std::string field = k.substr(dot + 1);
            if (id.empty()) continue;
            rows[id][field] = v;
            maxId = (std::max)(maxId, atoi(id.c_str()));
        }
        if (!rows.empty()) {
            for (const auto& kv : rows) {
                const auto& r = kv.second;
                SourceConfig c;
                c.id = kv.first;
                auto get = [&](const char* n) {
                    const auto it = r.find(n);
                    return it == r.end() ? std::string() : it->second;
                };
                c.name = get("name");
                c.kind = get("kind").empty() ? "subsonic" : get("kind");
                c.base = get("base");
                c.user = get("user");
                c.pass = get("pass");
                servers_.push_back(c);
            }
            std::sort(servers_.begin(), servers_.end(), [](const SourceConfig& a, const SourceConfig& b) {
                return atoi(a.id.c_str()) < atoi(b.id.c_str());
            });
        } else if (!legacyBase.empty() || !legacyUser.empty() || !legacyPass.empty()) {
            // 旧格式（单服务器）：迁移成 id=1，名字留给 display_name 自动生成
            SourceConfig c;
            c.id = "1";
            c.kind = legacyKind.empty() ? "subsonic" : legacyKind;
            c.base = legacyBase;
            c.user = legacyUser;
            c.pass = legacyPass;
            c.name = c.display_name();
            servers_.push_back(c);
            if (active_.empty()) active_ = "1";
            maxId = 1;
        }
        next_id_ = (std::max)((std::max)(nextId, maxId + 1), 1);
        // active 指向不存在的 id（conf 被手改过）时回退到第一台可用服务器
        if (!active_.empty() && !find_locked(active_)) active_.clear();
        if (active_.empty())
            for (const auto& s : servers_) if (s.valid()) { active_ = s.id; break; }
        // 还没落盘成 v2 的老 conf：这里不主动写，下一次保存时自然升级。
        apply_active_locked();
    }

private:
    SourceManager() = default;
    std::vector<SourceConfig> servers_;
    std::string active_;
    int next_id_ = 1;
    Subsonic sub_;
    Emby emby_;
    mutable std::mutex mtx_;
    std::map<std::string, std::string> cache_;
    std::string confPath_;
    // 已经下发给 sub_/emby_ 的配置（相同就不再 set_config，保住 Emby 的登录 token 缓存）
    std::string applied_kind_, applied_base_, applied_user_, applied_pass_;

    // 远程缓存目录 = <数据目录>/remote-cache。
    // 迁移前后等价：数据目录在 Windows 上就是 exe 同目录（platform/path.h §6 规则）。
    static std::string cache_dir() {
        return rta::platform::join(rta::platform::data_dir_utf8(), "remote-cache");
    }

    SourceConfig* find_locked(const std::string& id) {
        for (auto& s : servers_) if (s.id == id) return &s;
        return nullptr;
    }
    const SourceConfig* find_locked(const std::string& id) const {
        for (const auto& s : servers_) if (s.id == id) return &s;
        return nullptr;
    }
    const SourceConfig* active_locked() const {
        if (active_.empty()) return nullptr;
        return find_locked(active_);
    }
    SourceConfig* mutable_active_locked() {
        if (active_.empty()) return nullptr;
        return find_locked(active_);
    }
    std::string next_id_locked() {
        while (find_locked(std::to_string(next_id_))) ++next_id_;
        return std::to_string(next_id_++);
    }
    static bool ping_cfg_locked(const SourceConfig& c, std::string& err) {
        if (!c.valid()) { err = "请先填写服务器地址与用户名"; return false; }
        if (c.kind == "emby") {
            Emby e{EmbyConfig{c.base, c.user, c.pass}};
            return e.login(err);
        }
        Subsonic s{SubConfig{c.base, c.user, c.pass}};
        return s.ping(err);
    }
    // 把当前启用的配置同步给两个客户端（相同则跳过 —— Emby 登录后 token 可复用）
    void apply_active_locked() {
        const SourceConfig* c = active_locked();
        const std::string kind = c ? c->kind : "";
        const std::string base = c ? c->base : "";
        const std::string user = c ? c->user : "";
        const std::string pass = c ? c->pass : "";
        if (kind == applied_kind_ && base == applied_base_ && user == applied_user_ && pass == applied_pass_) return;
        applied_kind_ = kind; applied_base_ = base; applied_user_ = user; applied_pass_ = pass;
        sub_.set_config(SubConfig{ base, user, pass });
        emby_.set_config(EmbyConfig{ base, user, pass });
    }
    static std::string one_line(std::string v) {
        for (char& ch : v) if (ch == '\n' || ch == '\r') ch = ' ';
        return v;
    }

    void save_locked() {
        if (confPath_.empty()) {
            // 配置 = <配置目录>/rlplayer-sources.conf（Windows: exe 同目录）
            confPath_ = rta::platform::join(rta::platform::config_dir_utf8(),
                                            "rlplayer-sources.conf");
        }
        std::ofstream f(confPath_, std::ios::trunc);
        if (!f) return;
        f << "version=2\n";
        f << "nextid=" << next_id_ << "\n";
        f << "active=" << active_ << "\n";
        for (const auto& s : servers_) {
            f << "srv" << s.id << ".name=" << one_line(s.name) << "\n";
            f << "srv" << s.id << ".kind=" << one_line(s.kind) << "\n";
            f << "srv" << s.id << ".base=" << one_line(s.base) << "\n";
            f << "srv" << s.id << ".user=" << one_line(s.user) << "\n";
            f << "srv" << s.id << ".pass=" << one_line(s.pass) << "\n";
        }
    }
};

} // namespace rta::net
