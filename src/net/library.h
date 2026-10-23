// library.h — 本地音乐库（索引 / 专辑 / 搜索 / 封面）
//
// 为什么要自己建索引：HQPlayer 的库管理一直被诟病（目录树 + 手动选文件），
// Roon 的做法是「艺术家 / 专辑 / 曲目」三层 + 全局搜索。这里照后者做，
// 但只依赖 ffprobe 读标签，不引入数据库。
//
// 设计要点：
//   * 索引落盘为 rlplayer-index.tsv，增量扫描：mtime+size 没变就复用旧记录，
//     只有新文件/改过的文件才真正跑 ffprobe（每文件一次进程，约 20-40 ms）
//   * 库根目录存 rlplayer-library.conf，每行一个
//   * 专辑键 = 专辑艺术家 + 0x1f + 专辑名（缺标签时退化为父目录名），
//     对外用 16 位十六进制哈希，避免 URL 里出现任意字符
//   * 扫描在后台线程做，查询永远不阻塞（读的是上一份完整的索引）
#pragma once
// （原 WIN32_LEAN_AND_MEAN / NOMINMAX 前导已删：include 链下方的平台头自己会在 windows.h
//   之前定义它们，行为不变；留着只会让"这个头依赖 Windows"看起来像真的。）
#include "audio/audio_loader.h"
#include "audio/media_info.h"
#include "net/scan_pool.h"
#include "meta/entities.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <cstdint>

namespace rta::net {

namespace fsx = std::filesystem;

// 16 位十六进制哈希（FNV-1a）
inline std::string fnv_hex(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= (uint64_t)c; h *= 1099511628211ull; }
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return std::string(b);
}

struct LibTrack {
    std::string path;         // 绝对路径（UTF-8）
    std::string title, artist, album, albumArtist, genre, codec;
    int    trackNo = 0, discNo = 0, year = 0;
    int    sampleRate = 0, bits = 0, channels = 0;
    double duration = 0;
    long long mtime = 0, size = 0;
    int    hasCover = 0;
};

struct LibAlbum {
    std::string key;          // 十六进制哈希（URL 用）
    std::string name, artist;
    int    year = 0;
    int    nTracks = 0;
    double duration = 0;
    bool   hasCover = false;
    std::vector<size_t> idx;  // tracks_ 下标，已按 disc/track 排序
};

class Library {
public:
    static Library& instance() { static Library L; return L; }

    // ---- 配置 ----
    std::vector<std::string> roots() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return roots_;
    }
    bool addRoot(const std::string& dir) {
        std::error_code ec;
        if (!fsx::is_directory(dir, ec)) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& r : roots_) if (r == dir) return true;
        roots_.push_back(dir);
        saveRootsLocked();
        return true;
    }
    bool removeRoot(const std::string& dir) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = std::find(roots_.begin(), roots_.end(), dir);
        if (it == roots_.end()) return false;
        roots_.erase(it);
        saveRootsLocked();
        return true;
    }

    // ---- 状态 ----
    bool scanning() const { return scanning_.load(); }
    int  scanned()  const { return scanned_.load(); }
    int  scanTotal() const { return total_.load(); }
    std::string scanMsg() const { std::lock_guard<std::mutex> lk(mtx_); return scanMsg_; }
    size_t trackCount() const { std::lock_guard<std::mutex> lk(mtx_); return tracks_.size(); }
    size_t albumCount() const { std::lock_guard<std::mutex> lk(mtx_); return albumVecLocked().size(); }

    void setScanThreads(size_t n) {
        std::lock_guard<std::mutex> lk(mtx_);
        scanThreads_ = n;
    }
    size_t scanThreads() const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (scanThreads_ > 0) return scanThreads_;
        return defaultScanThreads();
    }
    static size_t defaultScanThreads() {
        unsigned int h = std::thread::hardware_concurrency();
        return std::min(std::max(1u, h), 8u);
    }
    double lastScanMs() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastScanMs_;
    }
    int lastScanProbed() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastScanProbed_;
    }
    int lastScanCached() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastScanCached_;
    }

    // ---- 查询 ----
    std::vector<LibAlbum> albums() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return albumVecLocked();
    }
    bool album(const std::string& key, LibAlbum& out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto& v = albumVecLocked();
        for (auto& a : v) if (a.key == key) { out = a; return true; }
        return false;
    }
    bool trackAt(size_t i, LibTrack& out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (i >= tracks_.size()) return false;
        out = tracks_[i];
        return true;
    }
    // 返回命中曲目的下标（最多 limit 条）
    std::vector<size_t> search(const std::string& q, size_t limit = 300) const;

    // 某专辑的封面（字节 + mime），失败返回空
    bool albumCover(const std::string& key, std::vector<unsigned char>& bytes, std::string& mime);

    // ---- 扫描 ----
    void load();            // 读配置与索引
    void startScan(bool forceFull = false);
    void stopScan() {
        cancel_.store(true);
        std::shared_ptr<ScanPool> p;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            p = activePool_;
        }
        if (p) p->cancel();
    }
    void joinScan();

private:
    Library() = default;

    void saveRootsLocked();
    void saveIndexLocked();
    const std::vector<LibAlbum>& albumVecLocked() const;
    static std::string readFile(const std::string& p);
    static void writeFile(const std::string& p, const std::string& s);
    static std::string exeDirOrEmpty();   // 2026-10-23：库配置/索引固定落 exe 目录（见 confPath 注释）
    static std::string libDataDir();      // 同样可被 RLPLAYER_LIBRARY_DIR 覆盖（测试/多实例）
    static std::string confPath();
    static std::string indexPath();

    mutable std::mutex mtx_;
    std::vector<std::string> roots_;
    std::vector<LibTrack> tracks_;
    mutable std::vector<LibAlbum> albums_;
    mutable bool albumsDirty_ = true;
    mutable std::map<std::string, std::pair<std::vector<unsigned char>, std::string>> coverCache_;

    std::thread th_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<int>  scanned_{0};
    std::atomic<int>  total_{0};
    std::string scanMsg_;

    size_t scanThreads_ = 0;
    double lastScanMs_ = 0.0;
    int lastScanProbed_ = 0;
    int lastScanCached_ = 0;
    std::shared_ptr<ScanPool> activePool_;
};

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------

inline std::string lib_clean_field(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

// Windows 上 fs::path 的窄字符串构造按 ANSI（GBK）解释，UTF-8 路径必须先转宽字符
inline std::string u8s(const std::u8string& s) { return std::string((const char*)s.data(), s.size()); }
inline fsx::path p8(const std::string& s) { return fsx::path(rta::audio::widen(s)); }

// 同目录是否存在独立封面文件（与 get_cover 的候选顺序/扩展名保持一致；只 stat）
inline bool lib_has_cover_file(const std::string& trackPath) {
    std::error_code ec;
    fsx::path dir = p8(trackPath).parent_path();
    static const char* kNames[] = { "cover", "folder", "front", "Cover", "Folder" };
    static const char* kExts[]  = { ".jpg", ".jpeg", ".png", ".JPG", ".PNG" };
    for (const char* n : kNames)
        for (const char* e : kExts)
            if (fsx::exists(dir / (std::string(n) + e), ec)) return true;
    return false;
}

inline bool lib_is_audio(const fsx::path& p) {
    std::string ext = u8s(p.extension().u8string());
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    static const char* kExt[] = { ".wav", ".wave", ".flac", ".m4a", ".alac", ".mp3", ".aac",
                                  ".ogg", ".opus", ".wma", ".ape", ".wv", ".dsf", ".dff",
                                  ".aif", ".aiff", ".aifc", ".caf", ".mka", ".mpc" };
    for (auto* x : kExt) if (ext == x) return true;
    return false;
}

// 一次 ffprobe 取全部信息（比 media_info.h 的两次调用快一倍）
inline LibTrack lib_probe_one(const std::string& file, const std::string& ffprobePath = "") {
    LibTrack t;
    t.path = file;
    const std::string ffprobe = !ffprobePath.empty() ? ffprobePath : rta::audio::find_tool("ffprobe");
    if (ffprobe.empty()) return t;

    std::string out;
    const std::wstring cmd =
        L"\"" + rta::audio::widen(ffprobe) + L"\" -v error -select_streams a:0"
        L" -show_entries stream=codec_name,sample_rate,channels,bits_per_raw_sample"
        L" -show_entries format=duration:format_tags=title,artist,album,album_artist,date,genre,track,disc,tracktotal"
        L" -of default=nw=1 \"" + rta::audio::widen(file) + L"\"";
    if (rta::audio::run_hidden(cmd, &out) != 0) return t;

    std::string title, artist, album, aartist, date, genre, track, disc;
    std::string line;
    for (size_t i = 0; i <= out.size(); ++i) {
        if (i == out.size() || out[i] == '\n') {
            const size_t e = line.find('=');
            if (e != std::string::npos) {
                std::string k = line.substr(0, e);
                const std::string v = line.substr(e + 1);
                for (auto& c : k) c = (char)std::tolower((unsigned char)c);
                if (k.compare(0, 4, "tag:") == 0) k = k.substr(4);
                auto put = [&](std::string& dst) { dst = v; while (!dst.empty() && (dst.back() == '\r' || dst.back() == ' ')) dst.pop_back(); };
                if      (k == "title")        put(title);
                else if (k == "artist")       put(artist);
                else if (k == "album")        put(album);
                else if (k == "album_artist") put(aartist);
                else if (k == "albumartist")  put(aartist);
                else if (k == "date")         put(date);
                else if (k == "genre")        put(genre);
                else if (k == "track")        put(track);
                else if (k == "disc")         put(disc);
                else if (k == "codec_name")   put(t.codec);
                else if (k == "sample_rate")  t.sampleRate = atoi(v.c_str());
                else if (k == "channels")     t.channels = atoi(v.c_str());
                else if (k == "bits_per_raw_sample") t.bits = atoi(v.c_str());
                else if (k == "duration")     t.duration = atof(v.c_str());
            }
            line.clear();
        } else if (line.size() < 8192) line += out[i];
    }
    t.title = title; t.artist = artist; t.album = album;
    t.albumArtist = aartist; t.genre = genre;
    t.trackNo = atoi(track.c_str());
    t.discNo  = atoi(disc.c_str());
    t.year    = atoi(date.c_str());
    if (t.bits <= 0) t.bits = (t.codec == "flac" || t.codec == "alac") ? 16 : 16;
    // 封面存在性（2026-10-23 修：此前 hasCover 恒 0，UI 因此不请求封面 → 「封面全部不显示」）
    // 只 stat 同目录 cover/folder/front（与 get_cover 的查找顺序一致），不启动 ffmpeg，扫描开销可忽略。
    // 内嵌封面无法靠 stat 判断，这里只保证「有独立封面文件」的专辑能显示；没有独立文件的专辑
    // 仍会在 get_cover 阶段尝试内嵌抽取（UI 用 img onerror 兜底，不会裂图）。
    t.hasCover = lib_has_cover_file(t.path) ? 1 : 0;
    return t;
}

// 2026-10-23 修复「曲库根丢失 / 索引漂移」：
//   旧实现返回相对文件名 → 实际落在进程 cwd，而 8899 实例是从 exe 目录启动、自检沙箱是另一个 cwd，
//   两边看到的是不同的 library.conf / index.tsv（用户升级后曲库变空、封面全无，就是这么来的）。
//   统一改为 **exe 目录绝对路径**（取不到 exe 目录时退回相对名，保证老行为可用）。
inline std::string Library::exeDirOrEmpty() {
    const std::wstring w = rta::audio::exe_dir_w();
    if (w.empty()) return std::string();
    std::string u = rta::audio::wide_to_utf8(w);
    while (!u.empty() && (u.back() == '\\' || u.back() == '/')) u.pop_back();
    return u;
}
// 配置/索引落点：
//   1) 环境变量 RLPLAYER_LIBRARY_DIR（测试 / 多实例 / 便携部署用；测试必须用它，否则会读到真实库配置）
//   2) 否则用 exe 目录（与 rlplayer-settings.conf 一致，避免 8899 实例与自检沙箱看到两份不同的库）
//   3) 再取不到 exe 目录才退回相对名（老行为）
inline std::string Library::libDataDir() {
    if (const char* env = std::getenv("RLPLAYER_LIBRARY_DIR")) {
        if (*env) { std::string s(env); while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back(); if (!s.empty()) return s; }
    }
    return exeDirOrEmpty();
}
inline std::string Library::confPath() {
    const std::string d = libDataDir();
    return d.empty() ? std::string("rlplayer-library.conf") : d + "\\rlplayer-library.conf";
}
inline std::string Library::indexPath() {
    const std::string d = libDataDir();
    return d.empty() ? std::string("rlplayer-index.tsv") : d + "\\rlplayer-index.tsv";
}
inline std::string Library::readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        // 老版本用 rta- 前缀：读不到新文件时回退一次（迁移用，不重建用户的库）
        if (p.rfind("rlplayer-", 0) == 0) {
            std::ifstream f2("rta-" + p.substr(9), std::ios::binary);
            if (f2) return std::string((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
        }
        return "";
    }
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}
inline void Library::writeFile(const std::string& p, const std::string& s) {
    std::string tmp = p + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return; f.write(s.data(), (std::streamsize)s.size()); }
    std::error_code ec;
    fsx::rename(tmp, p, ec);
}

inline void Library::load() {
    {
        std::string conf = readFile(confPath());
        if (conf.size() >= 3 && (unsigned char)conf[0] == 0xEF && (unsigned char)conf[1] == 0xBB && (unsigned char)conf[2] == 0xBF)
            conf = conf.substr(3);
        std::lock_guard<std::mutex> lk(mtx_);
        roots_.clear();
        std::string line;
        for (size_t i = 0; i <= conf.size(); ++i) {
            if (i == conf.size() || conf[i] == '\n') {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (!line.empty() && line[0] != '#') roots_.push_back(line);
                line.clear();
            } else line += conf[i];
        }
        if (roots_.empty()) {
            // 首次运行：探一遍常见位置，别一打开就是空的（HQPlayer 被诟病的就是这个）
            std::vector<std::string> cands;
            if (const char* up = std::getenv("USERPROFILE")) {
                cands.push_back(std::string(up) + "\\Music");
                cands.push_back(std::string(up) + "\\音乐");
            }
            for (const char* d : { "D:\\Music", "E:\\Music", "D:\\音乐", "E:\\音乐",
                                   "D:\\MUSIC", "E:\\MUSIC", "C:\\Users\\Public\\Music" })
                cands.push_back(d);
            std::error_code ec;
            for (const auto& c : cands) {
                if (!fsx::is_directory(p8(c), ec)) continue;
                bool dup = false;
                for (auto& r : roots_) if (r == c) dup = true;
                if (!dup) roots_.push_back(c);
                if (roots_.size() >= 4) break;
            }
            if (roots_.empty()) roots_.push_back("C:\\Users\\Public\\Music");
        }
    }
    // 索引
    std::string idx = readFile(indexPath());
    if (idx.size() >= 3 && (unsigned char)idx[0] == 0xEF && (unsigned char)idx[1] == 0xBB && (unsigned char)idx[2] == 0xBF)
        idx = idx.substr(3);
    std::vector<LibTrack> loaded;
    if (!idx.empty()) {
        std::string line;
        for (size_t i = 0; i <= idx.size(); ++i) {
            if (i == idx.size() || idx[i] == '\n') {
                if (!line.empty() && line[0] != '#') {
                    std::vector<std::string> f;
                    size_t s = 0;
                    for (size_t k = 0; k <= line.size(); ++k) {
                        if (k == line.size() || line[k] == '\t') { f.push_back(line.substr(s, k - s)); s = k + 1; }
                    }
                    if (f.size() >= 16) {
                        LibTrack t;
                        t.path        = f[0];
                        t.mtime       = _strtoi64(f[1].c_str(), nullptr, 10);
                        t.size        = _strtoi64(f[2].c_str(), nullptr, 10);
                        t.album       = f[3];
                        t.albumArtist = f[4];
                        t.artist      = f[5];
                        t.title       = f[6];
                        t.trackNo     = atoi(f[7].c_str());
                        t.discNo      = atoi(f[8].c_str());
                        t.year        = atoi(f[9].c_str());
                        t.sampleRate  = atoi(f[10].c_str());
                        t.bits        = atoi(f[11].c_str());
                        t.channels    = atoi(f[12].c_str());
                        t.duration    = atof(f[13].c_str());
                        t.hasCover    = atoi(f[14].c_str());
                        t.genre       = f[15];
                        if (!t.path.empty()) loaded.push_back(t);
                    }
                }
                line.clear();
            } else line += idx[i];
        }
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tracks_.swap(loaded);
        albumsDirty_ = true;
    }
    std::string sconf = readFile("rlplayer-settings.conf");
    if (sconf.size() >= 3 && (unsigned char)sconf[0] == 0xEF && (unsigned char)sconf[1] == 0xBB && (unsigned char)sconf[2] == 0xBF)
        sconf = sconf.substr(3);
    if (!sconf.empty()) {
        std::string line;
        for (size_t i = 0; i <= sconf.size(); ++i) {
            if (i == sconf.size() || sconf[i] == '\n') {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                const size_t e = line.find('=');
                if (e != std::string::npos) {
                    const std::string k = line.substr(0, e);
                    const std::string v = line.substr(e + 1);
                    if (k == "scanThreads" || k == "scanthreads") {
                        const int st = atoi(v.c_str());
                        if (st > 0) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            scanThreads_ = (size_t)st;
                        }
                    }
                }
                line.clear();
            } else line += sconf[i];
        }
    }
}

inline void Library::saveRootsLocked() {
    std::string s = "# RLPlayer 音乐库根目录，每行一个\n";
    for (auto& r : roots_) s += r + "\n";
    writeFile(confPath(), s);
}

inline void Library::saveIndexLocked() {
    std::string s;
    s.reserve(tracks_.size() * 160 + 64);
    s += "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre\n";
    char buf[256];
    for (auto& t : tracks_) {
        s += lib_clean_field(t.path); s += '\t';
        std::snprintf(buf, sizeof(buf), "%lld\t%lld\t", t.mtime, t.size); s += buf;
        s += lib_clean_field(t.album);       s += '\t';
        s += lib_clean_field(t.albumArtist); s += '\t';
        s += lib_clean_field(t.artist);      s += '\t';
        s += lib_clean_field(t.title);       s += '\t';
        std::snprintf(buf, sizeof(buf), "%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%d\t", t.trackNo, t.discNo, t.year,
                      t.sampleRate, t.bits, t.channels, t.duration, t.hasCover);
        s += buf;
        s += lib_clean_field(t.genre);
        s += '\n';
    }
    writeFile(indexPath(), s);
}

inline const std::vector<LibAlbum>& Library::albumVecLocked() const {
    if (!albumsDirty_) return albums_;
    albums_.clear();
    std::map<std::string, size_t> byKey;
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const auto& t = tracks_[i];
        std::string aName = t.album;
        std::string aArtist = t.albumArtist.empty() ? t.artist : t.albumArtist;
        int discForSort = t.discNo;
        if (aName.empty() || discForSort == 0) {
            int inferredDisc = 0;
            const auto albDir = rta::meta::entity_find_album_dir_for_track(p8(t.path), &inferredDisc);
            if (!albDir.empty()) {
                if (aName.empty()) {
                    std::string a, c;
                    rta::meta::entity_parse_album_circle(u8s(albDir.filename().u8string()), a, c);
                    aName = a.empty() ? u8s(albDir.filename().u8string()) : a;
                }
                if (discForSort == 0 && inferredDisc > 0) discForSort = inferredDisc;
            } else if (aName.empty()) {
                std::string dn = u8s(p8(t.path).parent_path().filename().u8string());
                aName = dn.empty() ? "未知专辑" : dn;
            }
        }
        const std::string rawKey = aArtist + "\x1f" + aName;
        auto it = byKey.find(rawKey);
        if (it == byKey.end()) {
            LibAlbum a; a.key = fnv_hex(rawKey); a.name = aName; a.artist = aArtist; a.year = t.year;
            byKey[rawKey] = albums_.size();
            albums_.push_back(a);
            it = byKey.find(rawKey);
        }
        LibAlbum& a = albums_[it->second];
        a.idx.push_back(i);
        a.nTracks = (int)a.idx.size();
        a.duration += t.duration;
        if (t.hasCover) a.hasCover = true;
        if (a.year == 0) a.year = t.year;
    }
    for (auto& a : albums_) {
        std::sort(a.idx.begin(), a.idx.end(), [&](size_t x, size_t y) {
            const auto& p = tracks_[x]; const auto& q = tracks_[y];
            int pDisc = p.discNo, qDisc = q.discNo;
            if (pDisc == 0) rta::meta::entity_find_album_dir_for_track(p8(p.path), &pDisc);
            if (qDisc == 0) rta::meta::entity_find_album_dir_for_track(p8(q.path), &qDisc);
            if (pDisc != qDisc) return pDisc < qDisc;
            if (p.trackNo != q.trackNo) return p.trackNo < q.trackNo;
            return p.path < q.path;
        });
    }
    std::sort(albums_.begin(), albums_.end(), [](const LibAlbum& a, const LibAlbum& b) {
        const std::string x = a.artist + "\x1f" + a.name, y = b.artist + "\x1f" + b.name;
        return x < y;
    });
    albumsDirty_ = false;
    return albums_;
}

inline std::vector<size_t> Library::search(const std::string& q, size_t limit) const {
    std::vector<size_t> hits;
    std::string needle = q;
    for (auto& c : needle) c = (char)std::tolower((unsigned char)c);
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < tracks_.size() && hits.size() < limit; ++i) {
        if (needle.empty()) { hits.push_back(i); continue; }
        std::string hay = tracks_[i].title + " " + tracks_[i].artist + " " + tracks_[i].album + " " + tracks_[i].albumArtist;
        for (auto& c : hay) c = (char)std::tolower((unsigned char)c);
        if (hay.find(needle) != std::string::npos) hits.push_back(i);
    }
    return hits;
}

inline bool Library::albumCover(const std::string& key, std::vector<unsigned char>& bytes, std::string& mime) {
    std::string firstPath;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = coverCache_.find(key);
        if (it != coverCache_.end()) {
            if (it->second.first.empty()) return false;
            bytes = it->second.first; mime = it->second.second; return true;
        }
        const auto& v = albumVecLocked();
        for (auto& a : v) if (a.key == key && !a.idx.empty()) { firstPath = tracks_[a.idx[0]].path; break; }
    }
    if (firstPath.empty()) return false;
    std::vector<unsigned char> b = rta::audio::get_cover(firstPath, mime);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (coverCache_.size() > 256) coverCache_.clear();
        coverCache_[key] = { b, mime };
    }
    bytes.swap(b);
    return !bytes.empty();
}

inline void Library::joinScan() {
    if (th_.joinable()) th_.join();
}

inline void Library::startScan(bool forceFull) {
    if (scanning_.load()) return;
    joinScan();
    const auto roots = this->roots();
    // 旧索引（增量基准）
    std::vector<LibTrack> old = [&] { std::lock_guard<std::mutex> lk(mtx_); return tracks_; }();

    cancel_.store(false);
    scanning_.store(true);
    scanned_.store(0);
    total_.store(0);
    { std::lock_guard<std::mutex> lk(mtx_); scanMsg_ = "正在遍历目录…"; }

    const size_t thCount = this->scanThreads();

    th_ = std::thread([this, roots, old, forceFull, thCount]() {
        const auto tStart = std::chrono::steady_clock::now();

        // 老记录按路径建索引，便于复用
        std::unordered_map<std::string, size_t> oldByPath;
        oldByPath.reserve(old.size() * 2);
        for (size_t i = 0; i < old.size(); ++i) oldByPath[old[i].path] = i;

        std::vector<LibTrack> fresh;
        std::vector<std::string> walked;
        std::error_code ec;
        for (const auto& root : roots) {
            if (cancel_.load()) break;
            if (!fsx::is_directory(root, ec)) continue;
            for (auto it = fsx::recursive_directory_iterator(
                     root, fsx::directory_options::skip_permission_denied, ec);
                 it != fsx::recursive_directory_iterator(); ) {
                if (cancel_.load()) break;
                if (ec) { it.increment(ec); continue; }
                const auto& ep = it->path();
                std::string name = u8s(ep.filename().u8string());
                if (!name.empty() && name[0] == '.') { it.disable_recursion_pending(); it.increment(ec); continue; }
                if (it->is_directory(ec)) { it.increment(ec); continue; }
                if (lib_is_audio(ep)) {
                    walked.push_back(u8s(ep.u8string()));
                    total_.store((int)walked.size());
                }
                it.increment(ec);
            }
        }
        if (cancel_.load()) { scanning_.store(false); return; }

        const int n = (int)walked.size();
        total_.store(n);
        if (n == 0) {
            const auto tEnd = std::chrono::steady_clock::now();
            const double durMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
            std::lock_guard<std::mutex> lk(mtx_);
            tracks_.clear();
            albumsDirty_ = true;
            lastScanMs_ = durMs;
            lastScanProbed_ = 0;
            lastScanCached_ = 0;
            scanMsg_ = "完成：0 首";
            saveIndexLocked();
            scanning_.store(false);
            return;
        }

        fresh.resize((size_t)n);
        std::atomic<int> completed{0};
        std::atomic<int> probedCount{0};
        std::atomic<int> cachedCount{0};

        const std::string ffprobe = rta::audio::find_tool("ffprobe");

        auto pool = std::make_shared<ScanPool>(thCount);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            activePool_ = pool;
        }

        for (int i = 0; i < n; ++i) {
            if (cancel_.load()) break;
            const bool submitted = pool->submit([this, i, n, &walked, &oldByPath, &old, forceFull, &ffprobe,
                                                 &fresh, &completed, &probedCount, &cachedCount]() {
                if (cancel_.load()) return;
                const std::string& p = walked[(size_t)i];
                long long mt = 0, sz = 0;
                {
                    std::error_code e2;
                    auto wp = p8(p);
                    auto ft = fsx::last_write_time(wp, e2);
                    if (!e2) mt = (long long)ft.time_since_epoch().count();
                    auto s = fsx::file_size(wp, e2);
                    if (!e2) sz = (long long)s;
                }
                auto oit = oldByPath.find(p);
                if (!forceFull && oit != oldByPath.end()) {
                    const LibTrack& o = old[oit->second];
                    if (o.mtime == mt && o.size == sz) {
                        fresh[(size_t)i] = o;
                        cachedCount.fetch_add(1, std::memory_order_relaxed);
                        const int c = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                        scanned_.store(c);
                        return;
                    }
                }
                LibTrack t = lib_probe_one(p, ffprobe);
                t.mtime = mt;
                t.size = sz;
                if (t.title.empty()) {
                    std::string sn = u8s(p8(p).stem().u8string());
                    t.title = sn.empty() ? p : sn;
                }
                fresh[(size_t)i] = std::move(t);
                probedCount.fetch_add(1, std::memory_order_relaxed);
                const int c = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                scanned_.store(c);
                if (c % 8 == 0 || c == n) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    scanMsg_ = "读取标签 " + std::to_string(c) + "/" + std::to_string(n);
                }
            });
            if (!submitted) break;
        }

        pool->wait(false);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            activePool_.reset();
        }

        if (cancel_.load()) {
            scanning_.store(false);
            return;
        }

        const auto tEnd = std::chrono::steady_clock::now();
        const double durMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

        {
            std::lock_guard<std::mutex> lk(mtx_);
            tracks_.swap(fresh);
            albumsDirty_ = true;
            lastScanMs_ = durMs;
            lastScanProbed_ = probedCount.load();
            lastScanCached_ = cachedCount.load();
            const size_t nt = tracks_.size();
            scanMsg_ = "完成：" + std::to_string(nt) + " 首 (" + std::to_string((int)(durMs + 0.5)) + " ms)";
            saveIndexLocked();
        }
        scanning_.store(false);
    });
}

// 真实批量入口：从音乐库 LibTrack 扫描结果构建实体图
inline rta::meta::EntityGraph entity_graph_from_library_tracks(
    const std::vector<LibTrack>& tracks, long long ts = 0) {
    std::vector<rta::meta::MetaSeed> seeds;
    seeds.reserve(tracks.size());
    for (const auto& t : tracks) {
        std::map<std::string, std::string> tags;
        if (!t.title.empty()) tags["title"] = t.title;
        if (!t.artist.empty()) tags["artist"] = t.artist;
        if (!t.album.empty()) tags["album"] = t.album;
        if (!t.albumArtist.empty()) tags["albumArtist"] = t.albumArtist;
        if (!t.genre.empty()) tags["genre"] = t.genre;
        if (t.year > 0) tags["year"] = std::to_string(t.year);
        if (t.discNo > 0) {
            tags["disc"] = std::to_string(t.discNo);
            tags["discNo"] = std::to_string(t.discNo);
        }
        if (t.trackNo > 0) tags["track"] = std::to_string(t.trackNo);
        seeds.push_back(rta::meta::entity_seed_from_track_path(t.path, tags));
    }
    return rta::meta::entity_batch_build_graph(seeds, ts);
}

inline rta::meta::EntityGraph entity_graph_from_library(const Library& lib, long long ts = 0) {
    std::vector<LibTrack> trs;
    const size_t n = lib.trackCount();
    trs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        LibTrack t;
        if (lib.trackAt(i, t)) trs.push_back(std::move(t));
    }
    return entity_graph_from_library_tracks(trs, ts);
}

} // namespace rta::net
