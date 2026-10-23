// store.h — MetaBase 本地元数据存储（曲目级；实体级在后续阶段）
//
// 设计（docs/元数据补全调研与规划.md §3）：
//   * 每首曲目一个 JSON：<base>/tracks/<hash>.json，hash 是曲目路径的 FNV-1a 64 位
//     十六进制（与 net/library.h 的 fnv_hex 同一算法同一常数，专辑键/曲目键全项目一致）
//   * 每个字段带溯源 {value, source, ts, confidence}，合并优先级
//     user(100) > tag(90) > 事实源 itunes/qq/netease/lrclib/musicbrainz/wiki(50) > llm(10) > 其它(0)
//   * 封面缓存在 <base>/covers/<hash>.(jpg|png)，JSON 里存相对路径（forward slash）
//   * 写盘原子（tmp + rename，仿 net/library.h），解析只用 net/json.h，
//     写出用 ostringstream 手工拼接 + 转义（中文原样 UTF-8，不做 \u 转义）
//   * 不依赖 windows.h：UTF-8 窄字符串经 std::u8string 构造 fs::path，
//     宽字符转换由标准库完成，不经 ANSI 代码页
#pragma once
#include "net/json.h"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace rta::meta {

namespace fsx = std::filesystem;

// ---------------------------------------------------------------------------
// 小工具（独立实现，不拉 library.h / http_client.h，避免引入 windows.h）
// ---------------------------------------------------------------------------

// UTF-8 窄字符串 -> fs::path（Windows 上按 UTF-8 解释，不经 ANSI 代码页）
inline fsx::path meta_path_of(const std::string& s) {
    return fsx::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
// fs::path -> UTF-8 窄字符串
inline std::string meta_string_of(const fsx::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// JSON 字符串转义：\ " \n \r \t 与 <0x20 控制字符（\u00xx）；
// 其余字节（含 UTF-8 中文）原样透传，不做 \u 转义
inline std::string meta_jesc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------

struct MetaValue {
    std::string value;
    std::string source;      // "user" "tag" "itunes" "qq" "netease" "lrclib" "musicbrainz" "wiki" "llm"
    long long   ts = 0;      // Unix 秒
    float       confidence = 0.f;
};

struct TrackMeta {
    std::string path;
    // key 固定用："title" "artist" "albumArtist" "album" "year" "genre"
    //              "trackNo" "discNo" "composer" "conductor" "label" "lyrics"
    std::map<std::string, MetaValue> fields;
    std::string coverRelPath;    // 相对 baseDir，如 "covers/xx.jpg"
};

// 来源优先级：user(100) > tag(90) > 事实源(50) > llm(10) > 其它(0)
inline int meta_source_rank(const std::string& source) {
    if (source == "user") return 100;
    if (source == "tag")  return 90;
    if (source == "itunes" || source == "qq" || source == "netease" ||
        source == "lrclib" || source == "musicbrainz" || source == "wiki" ||
        source == "discogs" || source == "dizzylab") return 50;
    if (source == "llm") return 10;
    return 0;
}

// ---------------------------------------------------------------------------
// MetaStore（单例，仿 Library::instance 模式）
// ---------------------------------------------------------------------------
class MetaStore {
public:
    static MetaStore& instance() { static MetaStore S; return S; }

    // 由宿主（rtaweb）在启动时设定：exe 目录下的 rlplayer-meta，或旧位置（工作目录）回退。
    // 见 rtaweb.cpp 里的 Metabase 初始化注释（2026-10-23 真机 DEBUG）。

    // 默认 "rlplayer-meta"（相对工作目录，同 rlplayer-library.conf 的约定）；测试切到临时目录
    //
    // 2026-10-23 真机 DEBUG：用户反馈「升级后封面/歌词没了」。根因是这里的默认值是**相对路径**，
    // 落点随进程工作目录漂移 —— 旧版从仓库根启动（metabase 在仓库根），新版从 exe 目录启动，
    // 于是既有的歌词/封面缓存全部"看不见"（数据没丢，只是指到了另一个空目录）。
    // 处理：默认改为 **exe 目录**（与 rlplayer-settings.conf / rlplayer-library.conf 一致），
    // 但对**旧位置**做一次只读回退（exe 目录下没有、而工作目录下有，就用工作目录那份），
    // 这样老用户的缓存不用迁移也能继续用。
    void setBaseDir(const std::string& dir) {
        std::lock_guard<std::mutex> lk(mtx_);
        baseDir_ = dir.empty() ? "rlplayer-meta" : dir;
    }
    std::string baseDir() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return baseDir_;
    }

    // FNV-1a 64 位 -> 16 位十六进制（与 net/library.h fnv_hex 同算法同常数，独立实现）
    static std::string hashOf(const std::string& trackPath) {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : trackPath) { h ^= (uint64_t)c; h *= 1099511628211ull; }
        char b[24];
        std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
        return std::string(b);
    }

    // 读 <base>/tracks/<hash>.json；文件不存在或损坏返回 false（out 保持不变）
    bool load(const std::string& trackPath, TrackMeta& out) {
        const fsx::path file = tracksDir() / (hashOf(trackPath) + ".json");
        std::error_code ec;
        if (!fsx::exists(file, ec)) return false;
        std::ifstream f(file, std::ios::binary);
        if (!f) return false;
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (s.empty()) return false;
        const rta::net::Json j = rta::net::json_parse(s);
        if (j.type != rta::net::Json::Obj) return false;
        // save() 永远写这两个键；缺 "fields" 或它不是对象 = 损坏/截断
        if (!j.has("fields") || j["fields"].type != rta::net::Json::Obj) return false;
        TrackMeta tm;
        tm.path         = j["path"].s();
        tm.coverRelPath = j["cover"].s();
        for (const auto& kv : j["fields"].obj) {
            const rta::net::Json& fv = kv.second;
            if (fv.type != rta::net::Json::Obj) continue;
            MetaValue v;
            v.value      = fv["value"].s();
            v.source     = fv["source"].s();
            v.ts         = (long long)fv["ts"].n();
            v.confidence = (float)fv["confidence"].n();
            tm.fields[kv.first] = std::move(v);
        }
        out = std::move(tm);
        return true;
    }

    // 原子写（tmp + rename）；目录不存在则创建。文件格式人可读：
    // {"path":"...","cover":"covers/xx.jpg","fields":{"title":{"value":"...","source":"qq","ts":1792000000,"confidence":0.9},...}}
    void save(const TrackMeta& tm) {
        std::ostringstream o;
        o << "{\"path\":\"" << meta_jesc(tm.path)
          << "\",\"cover\":\"" << meta_jesc(tm.coverRelPath)
          << "\",\"fields\":{";
        bool first = true;
        for (const auto& kv : tm.fields) {
            if (!first) o << ',';
            first = false;
            const MetaValue& v = kv.second;
            const float conf = std::isfinite(v.confidence) ? v.confidence : 0.f; // NaN/inf 会让 JSON 非法
            char cb[32];
            std::snprintf(cb, sizeof(cb), "%.4g", (double)conf);
            o << '"' << meta_jesc(kv.first)
              << "\":{\"value\":\""   << meta_jesc(v.value)
              << "\",\"source\":\""   << meta_jesc(v.source)
              << "\",\"ts\":"         << v.ts
              << ",\"confidence\":"   << cb << "}";
        }
        o << "}}";

        const fsx::path dir = tracksDir();
        std::error_code ec;
        fsx::create_directories(dir, ec);
        fsx::path file = dir / (hashOf(tm.path) + ".json");
        fsx::path tmp  = file;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            const std::string s = o.str();
            f.write(s.data(), (std::streamsize)s.size());
            if (!f) return;                 // 写失败：不动旧文件
        }
        fsx::rename(tmp, file, ec);         // 同盘 rename，原子生效
    }

    // 字段级合并：
    //   * 字段为空（无条目或值为空）→ 任何来源都可写入；
    //   * 新值为空 → 永不覆盖非空值；
    //   * 优先级高 → 覆盖；优先级低 → 忽略；
    //   * 同级 → confidence 高者胜，再按 ts 新者胜。
    static void mergeField(TrackMeta& tm, const std::string& field, const MetaValue& v) {
        auto it = tm.fields.find(field);
        if (it == tm.fields.end() || it->second.value.empty()) {
            tm.fields[field] = v;                       // 空字段：任何来源可写
            return;
        }
        if (v.value.empty()) return;                    // 空值永不覆盖非空
        const int oldRank = meta_source_rank(it->second.source);
        const int newRank = meta_source_rank(v.source);
        if (newRank > oldRank) { tm.fields[field] = v; return; }
        if (newRank < oldRank) return;
        if (v.confidence > it->second.confidence) { tm.fields[field] = v; return; }
        if (v.confidence < it->second.confidence) return;
        if (v.ts > it->second.ts) tm.fields[field] = v; // 同级同置信度：新者胜
    }

    // 读 <base>/covers/<hash>.(jpg|png)；找到即返回字节与 mime
    bool cover(const std::string& trackPath, std::vector<unsigned char>& bytes, std::string& mime) {
        bytes.clear(); mime.clear();
        const std::string h = hashOf(trackPath);
        const fsx::path dir = coversDir();
        static const char* kExts[] = { ".jpg", ".png" };
        for (const char* ext : kExts) {
            std::error_code ec;
            const fsx::path p = dir / (h + ext);
            if (!fsx::exists(p, ec)) continue;
            std::ifstream f(p, std::ios::binary);
            if (!f) continue;
            std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
            if (b.empty()) continue;
            bytes.swap(b);
            mime = (ext[1] == 'p') ? "image/png" : "image/jpeg";
            return true;
        }
        return false;
    }

    // 原子保存封面到 <base>/covers/<hash>.<ext>，返回相对路径（如 "covers/xx.jpg"）。
    // ext 接受 "jpg"/".jpg"/"jpeg"/"png"（大小写均可），jpeg 归一为 jpg；
    // 其余扩展名拒绝（cover() 只认 jpg|png，存了也读不回）。失败返回 ""。
    std::string saveCover(const std::string& trackPath, const unsigned char* data, size_t n,
                          const std::string& ext) {
        if (!data || n == 0) return "";
        std::string e = ext;
        if (!e.empty() && e[0] == '.') e = e.substr(1);
        for (char& c : e) c = (char)std::tolower((unsigned char)c);
        if (e == "jpeg") e = "jpg";
        if (e != "jpg" && e != "png") return "";
        const std::string h = hashOf(trackPath);
        const fsx::path dir = coversDir();
        std::error_code ec;
        fsx::create_directories(dir, ec);
        fsx::path file = dir / (h + "." + e);
        fsx::path tmp  = file;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return "";
            f.write((const char*)data, (std::streamsize)n);
            if (!f) return "";
        }
        fsx::rename(tmp, file, ec);
        return "covers/" + h + "." + e;
    }

private:
    MetaStore() = default;

    fsx::path tracksDir() const { return meta_path_of(baseDir()) / "tracks"; }
    fsx::path coversDir() const { return meta_path_of(baseDir()) / "covers"; }

    mutable std::mutex mtx_;
    std::string baseDir_ = "rlplayer-meta";
};

} // namespace rta::meta
