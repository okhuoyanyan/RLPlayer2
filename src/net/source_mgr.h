// source_mgr.h — 音源管理（本地文件 + Subsonic/Navidrome 远程）
//
// 核心原则：远程曲目一律先【完整下载原始文件】到本地缓存，再走与本地文件
// 完全相同的解码与 DSP 路径。绝不接受服务端转码流，因此音质与本地播放一致。
#pragma once
#include "net/subsonic.h"
#include "net/emby.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <filesystem>

namespace rta::net {

struct SourceConfig {
    std::string kind = "subsonic";   // 目前只实现 subsonic；emby 预留
    std::string base, user, pass;
    bool enabled() const { return !base.empty() && !user.empty(); }
};

class SourceManager {
public:
    static SourceManager& instance() { static SourceManager m; return m; }

    void set_config(const SourceConfig& c) {
        std::lock_guard<std::mutex> lk(mtx_);
        cfg_ = c;
        sub_.set_config(SubConfig{ c.base, c.user, c.pass });
        emby_.set_config(EmbyConfig{ c.base, c.user, c.pass });
        save_locked();
    }
    SourceConfig config() const { std::lock_guard<std::mutex> lk(mtx_); return cfg_; }

    // 从磁盘加载（启动时调用一次）
    void load(const std::string& confPath) {
        std::lock_guard<std::mutex> lk(mtx_);
        confPath_ = confPath;
        std::ifstream f(confPath);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            const size_t e = line.find('=');
            if (e == std::string::npos) continue;
            const std::string k = line.substr(0, e), v = line.substr(e + 1);
            if (k == "kind") cfg_.kind = v;
            else if (k == "base") cfg_.base = v;
            else if (k == "user") cfg_.user = v;
            else if (k == "pass") cfg_.pass = v;
        }
        sub_.set_config(SubConfig{ cfg_.base, cfg_.user, cfg_.pass });
        emby_.set_config(EmbyConfig{ cfg_.base, cfg_.user, cfg_.pass });
    }

    bool ping(std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cfg_.kind == "emby") return emby_.login(err);
        return sub_.ping(err);
    }
    std::vector<SubEntry> browse(const std::string& id, std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cfg_.kind == "emby") return emby_.browse(id, err);
        return sub_.browse(id, err);
    }
    std::vector<SubEntry> indexes(std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cfg_.kind == "emby") return emby_.browse("", err);
        return sub_.indexes(err);
    }
    std::vector<SubEntry> search(const std::string& q, std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cfg_.kind == "emby") return emby_.search(q, err);
        return sub_.search(q, err);
    }
    HttpResponse cover(const std::string& id, int size = 400) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cfg_.kind == "emby") return emby_.cover(id, size);
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
            r = (cfg_.kind == "emby") ? emby_.download(id) : sub_.download(id);
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

private:
    SourceManager() = default;
    SourceConfig cfg_;
    Subsonic     sub_;
    Emby         emby_;
    mutable std::mutex mtx_;
    std::map<std::string, std::string> cache_;
    std::string  confPath_;

    static std::string cache_dir() {
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring dir(buf);
        const size_t s = dir.find_last_of(L"\\/");
        if (s != std::wstring::npos) dir = dir.substr(0, s);
        std::wstring full = dir + L"\\remote-cache";
        const int n = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string o((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, o.data(), n, nullptr, nullptr);
        if (!o.empty() && o.back() == '\0') o.pop_back();
        return o;
    }

    void save_locked() {
        if (confPath_.empty()) {
            wchar_t buf[MAX_PATH];
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            std::wstring dir(buf);
            const size_t s = dir.find_last_of(L"\\/");
            if (s != std::wstring::npos) dir = dir.substr(0, s);
            std::wstring full = dir + L"\\rlplayer-sources.conf";
            const int n = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string o((size_t)n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, o.data(), n, nullptr, nullptr);
            if (!o.empty() && o.back() == '\0') o.pop_back();
            confPath_ = o;
        }
        std::ofstream f(confPath_, std::ios::trunc);
        if (!f) return;
        f << "kind=" << cfg_.kind << "\n";
        f << "base=" << cfg_.base << "\n";
        f << "user=" << cfg_.user << "\n";
        f << "pass=" << cfg_.pass << "\n";
    }
};

} // namespace rta::net
