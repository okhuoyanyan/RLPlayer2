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
#include "meta/cue.h"          // CUE 整轨分轨解析（仅头文件；library 侧把它变成虚拟曲目）
#include "meta/tags_native.h"   // 原生标签快速通道（FLAC/WAV/MP3/OGG；lib_probe_one 的原生分支）
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
#include <cmath>
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
    // 来源标记（2026-10-25）：album / discNo 是不是「目录结构判据回填」的（= 该字段没有真实标签）。
    // 不落盘、也不占索引列（列数/顺序必须与老索引一致）：落盘值本身就是「真实标签优先」的结果，
    // 读回后不需要再判来源。只用于①防止覆盖真实标签 ②测试/调试能量出「这值从哪来」。
    bool   albumFromDir = false;
    bool   discFromDir  = false;

    // ---- CUE 整轨分轨（2026-10-25 第二里程碑）----
    // 一张 .cue 把「一个整轨音频文件」切成 N 首：**path 仍然是物理音频路径**（播放器照旧能播，
    // 封面/搜索/播放入口都不需要改），这首在文件里的位置由 cueOff/cueLen 给出。
    //   cuePath 非空  ⟺ 这是一条虚拟曲目（整轨被 cue 认领，不再单独出现）
    //   cueNo         = cue 文本里的 TRACK 号（可对回原文）
    //   cueOff/cueLen = 起点/长度，单位是**采样帧**（不是秒，也不是 1/75 帧；-1 = 未知）
    // 落盘语义：索引 v4 起末尾 4 列就是这四个字段（见 indexTextLocked）。
    std::string cuePath;      // 所属 .cue 的绝对路径（UTF-8）；空 = 普通（非 cue）曲目
    int         cueNo  = 0;
    long long   cueOff = 0;
    long long   cueLen = 0;
    bool isCueTrack() const { return !cuePath.empty(); }
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

// 封面在线抓取（2026-10-26）：「专辑键 → 专辑名 + 艺术家 + 拼好的搜索词」。
// 为什么需要反查：抓取线程只拿到 key，而网易云的专辑搜索接口吃的是**文字**（s=专辑名 艺术家）；
// 而专辑名/艺术家在库里有两种来源（真实标签 / 目录回填，见 albumVecLocked 的 aName 推导），
// 只有 Library 自己知道最终拼出来的那个值 —— 由本模块给，调用方不要自己再猜一遍。
struct LibCoverTerm {
    std::string key;      // 专辑键（与 /api/lib/cover?k= 同一个）
    std::string name;     // 专辑名（可能来自目录回填）
    std::string artist;   // 专辑艺术家（albumArtist 优先，退回 artist；可能为空）
    std::string term;     // 已拼好的搜索词："名 艺术家"，艺术家为空时只有名
};

// API 补图（2026-10-26）：「专辑键 → 名 + 艺术家 + 前 N 首曲名」。
// 为什么还要曲名：同人自出版碟（例：东方同人 VOL.2）的**专辑名**太泛，公开网页上搜不到；
// 只有曲名能把模型见过的那张碟钉住。缺了它，模型只能瞎猜 —— 猜出来的错图比空白更糟。
struct LibApiCoverTerm {
    std::string key;                     // 专辑键（与 /api/lib/cover?k= 同一个）
    std::string name;                    // 专辑名（成品口径，与 /api/lib/albums 显示的一致）
    std::string artist;                  // 专辑艺术家（albumArtist 优先，退回 artist；可能为空）
    std::vector<std::string> titles;     // 前 N 首曲名（按 disc/track 顺序；空标题跳过）
};

// addRoot 的完整结果：ok=false 时 why 是人话原因（HTTP 层原样回给 UI，别只说"目录不存在"）
struct addRootResult { bool ok = false; std::string why; };

// 清理结果（/api/lib/prune 与 /api/lib/remove 共用）
struct LibCleanResult { int removed = 0; size_t tracks = 0; };

// 前置声明：类内联实现（addRoot 等）要用（定义在下方"内部工具"一节，见 p8 注释）
inline std::string lib_valid_utf8_impl_placeholder();   // 占位避免误用（见下）
inline bool lib_valid_utf8(const std::string& s);
inline fsx::path p8(const std::string& s);
inline bool lib_path_under(const std::string& a, const std::string& b);
inline bool lib_file_exists(const std::string& p);

class Library {
public:
    static Library& instance() { static Library L; return L; }

    // ---- 配置 ----
    std::vector<std::string> roots() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return roots_;
    }
    // 加根：**必须**先过 UTF-8 校验、再用宽字符构造 path（见 p8 注释里的实测崩溃）。
    addRootResult addRootEx(const std::string& dir) {
        if (dir.empty()) return { false, "目录为空" };
        if (!lib_valid_utf8(dir)) return { false, "路径编码非 UTF-8" };
        std::error_code ec;
        bool ok = false;
        try {
            ok = fsx::is_directory(p8(dir), ec);
        } catch (const std::exception& e) {
            return { false, std::string("路径无法解析: ") + e.what() };
        }
        if (!ok) return { false, "目录不存在: " + dir };
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& r : roots_) if (r == dir) return { true, "" };
        roots_.push_back(dir);
        saveRootsLocked();
        return { true, "" };
    }
    bool addRoot(const std::string& dir) { return addRootEx(dir).ok; }
    bool removeRoot(const std::string& dir) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = std::find(roots_.begin(), roots_.end(), dir);
        if (it == roots_.end()) return false;
        roots_.erase(it);
        saveRootsLocked();
        return true;
    }

    // ---- 状态 ----
    // 2026-10-25（261025-01 一.1/8）：/api/lib/status 在扫描期间要 <50ms 返回，所以这里
    // **一律不碰 mtx_** —— 扫描线程会在收尾时长时间持有 mtx_（整理索引/落盘），旧实现的
    // status/trackCount 都排在它后面。改为：进度文本走 msgMtx_（只做一次字符串赋值），
    // 计数/耗时走原子量（snapshot 无锁），专辑数用上次整理好的缓存值。
    bool scanning() const { return scanning_.load(); }
    int  scanned()  const { return scanned_.load(); }
    int  scanTotal() const { return total_.load(); }
    std::string scanMsg() const { std::lock_guard<std::mutex> lk(msgMtx_); return scanMsg_; }
    size_t trackCount() const { return trackCount_.load(); }
    // 费用高（会触发专辑整理）；交互热路径请用 albumCountFast()
    size_t albumCount() const { std::lock_guard<std::mutex> lk(mtx_); return albumVecLocked().size(); }
    size_t albumCountFast() const { return albumCount_.load(); }
    int lastScanRemoved() const { return removedMissing_.load(); }

    // status 快照（无锁／最多一次短锁读 msg）：/api/lib/status 与 /api/lib/stats 共用
    struct Snapshot {
        bool   scanning = false;
        int    scanned = 0, total = 0;
        size_t tracks = 0, albums = 0;
        double lastScanMs = 0.0;
        int    probed = 0, cached = 0, removed = 0;
        std::string msg;
    };
    Snapshot snapshot() const {
        Snapshot s;
        s.scanning = scanning_.load();
        s.scanned  = scanned_.load();
        s.total    = total_.load();
        s.tracks   = trackCount_.load();
        s.albums   = albumCount_.load();
        s.lastScanMs = lastScanMs_.load();
        s.probed   = lastScanProbed_.load();
        s.cached   = lastScanCached_.load();
        s.removed  = removedMissing_.load();
        s.msg      = scanMsg();
        return s;
    }

    void setScanThreads(size_t n) { scanThreads_.store(n); }
    size_t scanThreads() const {
        const size_t n = scanThreads_.load();
        return n > 0 ? n : defaultScanThreads();
    }
    static size_t defaultScanThreads() {
        unsigned int h = std::thread::hardware_concurrency();
        return std::min(std::max(1u, h), 8u);
    }
    double lastScanMs() const { return lastScanMs_.load(); }
    int lastScanProbed() const { return lastScanProbed_.load(); }
    int lastScanCached() const { return lastScanCached_.load(); }
    // 2026-10-26：索引是否需要重新解析标签（v5 之前写的索引）。启动时用它决定要不要自动扫一次。
    bool indexNeedsRetag() const { return idxHasHeader && !idxIsV5; }

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

    // ---- 封面在线抓取用的「按 album+artist 反查」（2026-10-26）----
    // ① 全库快照：所有「内嵌流与同目录封面文件都没有」的专辑（hasCover 在扫描时已算好：
    //    内嵌 video 流 **或** 同目录 cover/folder/front 任一存在 ⟹ hasCover=1，见 lib_probe 尾部）。
    //    文件系统 IO 由调用方做（磁盘正/负缓存留在 rtaweb），本模块只回答「库里有没有图」。
    std::vector<LibCoverTerm> albumsWithoutCover() const;
    // ② 单张反查：按 key 取回专辑名/艺术家/搜索词。key 不在库里返回 false。
    bool albumCoverTerm(const std::string& key, LibCoverTerm& out) const;

    // ---- API 补图用的「元数据包」反查（2026-10-26）----
    // 与上面两个同一口径（无封面 = hasCover=0），只是多带前 N 首曲名；
    // 只读、不落盘、不改索引。目录 IO（缓存 stat）仍由调用方做。
    std::vector<LibApiCoverTerm> albumsWithoutCoverTitles(size_t maxTitles = 3) const;
    bool albumCoverTermTitles(const std::string& key, size_t maxTitles, LibApiCoverTerm& out) const;
    // 艺术家 → 其专辑名（去重，最多 maxAlbums 张；名字**精确**匹配库里的 artist 口径）。
    std::vector<std::string> artistAlbumNames(const std::string& artist, size_t maxAlbums = 6) const;

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

    // ---- 删除 / 清理（261025-01 一.3：以前只能"按原路径重扫"，删掉的文件永远留在索引里）----
    // 剔除「文件已不存在」的记录并落盘；返回删掉的条数。扫描开始前会自动跑一次。
    LibCleanResult pruneMissing();
    // 剔除 dir 之下（含子目录）的全部记录并落盘；用于 /api/lib/remove?path=<目录>
    LibCleanResult removeUnder(const std::string& dir);
    // 剔除单个文件记录（精确路径，大小写不敏感）
    bool removePath(const std::string& path);

private:
    Library() = default;

    void saveRootsLocked();
    // 2026-10-26：升级迁移记录（首次启动沿用旧版本目录的库根时置位，用于日志与 UI 提示）
    mutable bool confMigrated_ = false;
    // 2026-10-26：索引版本标记。idxIsV5=false 且确有版本头 ⟹ 旧索引，标签必须重探（否则修复永远不生效）。
    bool idxIsV5 = false;
    bool idxIsV4 = false;
    bool idxHasHeader = false;
    mutable std::string confMigratedFrom_;
    void saveIndexLocked();
    std::string indexTextLocked() const;          // 持锁只做内存序列化（写盘放到锁外）
    LibCleanResult pruneMissingLocked();          // 扫描线程内部用：调用方已持锁 */
    void applyCleanLocked(std::vector<LibTrack>&& kept, int removed);
    const std::vector<LibAlbum>& albumVecLocked() const;
    static std::string readFile(const std::string& p);
    static void writeFile(const std::string& p, const std::string& s);
    static std::string exeDirOrEmpty();   // 2026-10-23：库配置/索引固定落 exe 目录（见 confPath 注释）
    static std::string libDataDir();      // 同样可被 RLPLAYER_LIBRARY_DIR 覆盖（测试/多实例）
    static std::string confLeaf(const char* base);   // 非默认端口加 .p<port> 后缀，避免实例互相踩库
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
    mutable std::mutex msgMtx_;        // 只保护 scanMsg_（与 mtx_ 分离：status 不再排在索引工作后面）
    std::string scanMsg_;

    std::atomic<size_t> scanThreads_{0};
    std::atomic<double> lastScanMs_{0.0};
    std::atomic<int> lastScanProbed_{0};
    std::atomic<int> lastScanCached_{0};
    std::atomic<int> removedMissing_{0};
    std::atomic<size_t> trackCount_{0};
    mutable std::atomic<size_t> albumCount_{0};   // albumVecLocked() 是 const
    std::shared_ptr<ScanPool> activePool_;

public:
    // 2026-10-25（移植自协作者 mac 分支 7799e09，我们树上的真雷）：
    // instance() 用的是函数内 static，进程退出时静态析构会来跑这个析构；如果 th_ 还 joinable，
    // std::thread 的析构函数会直接 std::terminate ⟹ MSVC 报成 0xC0000409。
    // rtaweb 目前靠 main 末尾 std::_Exit(0) 跳过静态析构把它盖住，但只要多一个使用者、或多一条
    // 走 return 的退出路径就会踩。这里做成幂等收尾：停扫描 → 汇总 → join（try/catch 兜底）。
    ~Library() {
        try {
            cancel_.store(true);
            stopScan();
            joinScan();
            if (th_.joinable()) th_.join();
        } catch (...) {
            // 退出路径上绝不能让异常逃出去（那同样是 std::terminate）
        }
    }
};

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------

inline std::string lib_clean_field(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

// Windows 上 fs::path 的窄字符串构造按 ANSI（GBK）解释，UTF-8 路径必须先转宽字符。
//
// ★★ 2026-10-25 实测（261025-01 一.2「扫描中文/日语路径直接卡死」的根因，外部评审 P0#1）：
// 把 UTF-8 窄串直接喂 fs（fs::is_directory / recursive_directory_iterator / exists 的
// std::string 重载）并不是"判成非目录"那么温和 —— MS STL 的 path(std::string) 走 ACP，
// 转换失败时**抛 std::filesystem_error**：
//     "No mapping for the Unicode character exists in the target multi-byte code page."
// 而扫描线程里没有 catch ⟹ std::terminate ⟹ 进程以 0xC0000409 直接死（实测：
//   conf 里含中文根 → 启动打印"开始首次扫描"后进程消失，exit=0xC0000409；
//   同一夹具换成 ASCII 根 → 一切正常；同一路径改用 wide → is_directory=1）。
// 另有一处同族（本轮一并修）：rtaweb 的 /api/files 用窄串 dir 调 fs::exists → "目录不存在"。
// 结论（铁律）：**本文件内任何 fs::path 都必须由 p8()/widen() 从宽字符构造，禁止窄串直传。**
// 旧注释只写了"必须先转宽字符"，没写"不转会抛异常打死进程"，所以 3 处漏了（addRoot / 扫描根 / 遍历器）。
inline std::string u8s(const std::u8string& s) { return std::string((const char*)s.data(), s.size()); }
inline fsx::path p8(const std::string& s) { return fsx::path(rta::audio::widen(s)); }

// 严格 UTF-8 校验（与 rtaweb 的 valid_utf8 同一判据）：非法字节不交给 fs —— 一旦交给 fs::u8path
// 会抛异常（服务对 handler 异常没有全局兜底时就是 termination）。拒绝：续字节错配、截断、
// 过短编码、代理区、>U+10FFFF。
inline bool lib_valid_utf8(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        int cont; uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { cont = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { cont = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { cont = 3; cp = c & 0x07u; }
        else return false;
        if (i + (size_t)cont >= n) return false;
        for (int k = 1; k <= cont; ++k) {
            const unsigned char cc = p[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }
        if ((cont == 1 && cp < 0x80) || (cont == 2 && cp < 0x800) || (cont == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += (size_t)cont + 1;
    }
    return true;
}

// 路径 a 是否在目录 b 之下（Windows：大小写不敏感，且要求分隔符对齐 —— 否则 "D:\\mu" 会命中 "D:\\music"）
inline bool lib_path_under(const std::string& a, const std::string& b) {
    if (b.empty() || a.size() < b.size()) return false;
    for (size_t i = 0; i < b.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    if (a.size() == b.size()) return true;
    return a[b.size()] == '\\' || a[b.size()] == '/';
}

// 同目录是否存在独立封面文件（与 get_cover 的候选顺序/扩展名保持一致；只 stat）
inline bool lib_has_cover_file(const std::string& trackPath) {
    std::error_code ec;
    const fsx::path dir = p8(trackPath).parent_path();
    static const char* kNames[] = { "cover", "folder", "front", "Cover", "Folder" };
    static const char* kExts[]  = { ".jpg", ".jpeg", ".png", ".JPG", ".PNG" };
    // 候选名全是 ASCII 常量，但依然走 widen：《p8 注释》那条纪律是"fs::path 一律从宽字符构造"，
    // 否则将来有人把常量改成别的东西，又回到 ACP 那条会抛异常的路径上。
    for (const char* n : kNames)
        for (const char* e : kExts)
            if (fsx::exists(dir / rta::audio::widen(std::string(n) + e), ec)) return true;
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

// ---------------------------------------------------------------------------
// CUE 整轨分轨 —— library 侧（2026-10-25 第二里程碑）
//
// 场景：发烧友的「整轨 FLAC/WAV + .cue」在库里应当显示成 N 首（而不是 1 首 40 分钟的大曲目）。
// 分工：
//   * src/meta/cue.h        解析（文本 → 带 1/75 帧偏移的轨列表），不改；
//   * 本节的 lib_cue_expand 纯函数（探测表 → N 条虚拟曲目），不读盘/不起进程，可逐字段断言；
//   * Library::startScan    认领整轨（文件真的存在、探测成功、没被别的 cue 抢走）→ 整轨不再单独
//                           入库，改成 N 条虚拟曲目；任何一步不成立都**退回整轨**（不丢歌）。
// 判据：tests/test_cue.cpp（契约，139 条逐字段断言）+ tests/test_library.cpp 的 [5] 节（真实扫描）。
//
// ⚠️ 未接入（如实记录，铁律 4）：
//   1) 播放偏移属于 tools/rtaweb.cpp（不在本条工作范围）：这里只保证库能给出 cueOff/cueLen 与
//      lib_cue_range_sec()，播放侧还没用它来 seek；
//   2) 实体图（entity_graph_from_library_tracks）按 path 建种子，同一整轨的 N 首 path 相同 ——
//      这条链路的去重/归并尚未按 cue 区分；
//   3) 多 FILE cue 的「每份文件一个时长」已支持，但同一 cue 里跨文件的曲目顺序 = FILE 出现顺序。
// ---------------------------------------------------------------------------

// 路径比较键：大小写不敏感 + '/' 统一成 '\' + 去掉尾分隔符。
// 为什么需要：Windows 文件系统不敏感，而 cue 里的 FILE 常与磁盘真名大小写不一致
// （tests/test_cue.cpp 用例 16：cue 写 ALBUM.WAV、磁盘是 album.wav）。
// **只用于比较/查找，绝不拿它去打开文件**（真名要保留，否则播放侧拿到的是错的拼写）。
inline std::string lib_path_key(const std::string& p) {
    std::string k = p;
    for (auto& c : k) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    while (!k.empty() && k.back() == '\\') k.pop_back();
    return k;
}

// .cue 扩展名识别（大小写不敏感；".cue.txt" 不算，见 test_cue.cpp 用例 15）
inline bool lib_is_cue(const fsx::path& p) {
    std::string ext = u8s(p.extension().u8string());
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".cue";
}

// cue 的 FILE 指向的音频**此刻是否真的在磁盘上**。
// 为什么必须真 stat：只用扩展名/文本会把「整轨已删除、只剩 cue」也认领走，于是 cue 里的 N 首
// 变成 N 条打不开的幽灵曲目（而整轨本来也不会出现）——这正是「丢歌」的形态。
inline bool lib_cue_audio_exists(const std::string& p) {
    if (p.empty() || !lib_valid_utf8(p)) return false;
    std::error_code ec;
    try {
        const fsx::path wp = p8(p);
        if (!fsx::is_regular_file(wp, ec)) return false;
        return lib_is_audio(wp);
    } catch (const std::exception&) { return false; }
}

// 纯函数：一份已解析的 cue + 物理文件探测表 → N 条虚拟曲目。
//   * 不读盘、不起进程（判据可复现：test_cue.cpp 用构造的 probes 断言**具体采样数**）。
//   * probes 的键是物理文件路径，大小写/分隔符不敏感；值里的 path 应当是**磁盘真名**。
//   * 文件没探测到（不存在）⟹ 只跳过那份文件的轨，其余照常展开（降级，不丢歌）。
//   * 长度 = 下一轨起点 - 本轨起点；每份文件的末轨 = 文件总时长 - 本轨起点
//     （所以要 ffprobe 的 format duration；拿不到就如实标 -1 / 0，不编数字）。
//   * 采样率未知（ffprobe 失败）⟹ cueOff = -1，时长退回 cue 自带的 1/75 帧差值。
//   * 专辑级 trackNo 连续 1..N（跨文件也不重复），cueNo 保留 cue 里的原始 TRACK 号。
inline std::vector<LibTrack> lib_cue_expand(const rta::meta::CueSheet& sh,
                                            const std::map<std::string, LibTrack>& probes) {
    std::vector<LibTrack> out;
    if (sh.tracks.empty() || probes.empty()) return out;

    std::map<std::string, const LibTrack*> probeByKey;
    for (const auto& kv : probes) probeByKey[lib_path_key(kv.first)] = &kv.second;

    // 按 FILE 分组（保持 cue 里的出现顺序 —— 跨文件 trackNo 要连续）；组内按 INDEX 01 升序
    std::vector<std::string> fileOrder;
    std::map<std::string, std::vector<const rta::meta::CueTrack*>> byFile;
    for (const auto& ct : sh.tracks) {
        if (!ct.audio || ct.index1 < 0) continue;          // 数据轨 / 没有 INDEX 的轨不产生曲目
        const std::string k = lib_path_key(ct.file);
        if (byFile.find(k) == byFile.end()) fileOrder.push_back(k);
        byFile[k].push_back(&ct);
    }

    // 专辑艺术家：全局 PERFORMER 优先；只有轨级 PERFORMER 的 cue 取第一条非空的，
    // 否则 albumVecLocked 会退化成 t.artist ⟹ 每首各自成一张专辑（一张 12 轨的专辑变 12 张）。
    std::string albArtist = sh.performer;
    if (albArtist.empty()) {
        for (const auto& ct : sh.tracks)
            if (ct.audio && !ct.performer.empty()) { albArtist = ct.performer; break; }
    }
    const int year = atoi(sh.date.c_str());

    for (const auto& fk : fileOrder) {
        auto pit = probeByKey.find(fk);
        if (pit == probeByKey.end()) continue;             // 该文件不存在/没探测到 → 跳过（降级）
        const LibTrack& probe = *pit->second;
        std::vector<const rta::meta::CueTrack*> trs = byFile[fk];
        std::stable_sort(trs.begin(), trs.end(),
                         [](const rta::meta::CueTrack* a, const rta::meta::CueTrack* b) {
                             return a->index1 < b->index1;
                         });
        const int sr = probe.sampleRate;
        for (size_t i = 0; i < trs.size(); ++i) {
            const rta::meta::CueTrack& ct = *trs[i];
            LibTrack t = probe;                            // 采样率/位深/声道/封面/mtime/size 透传
            t.path = probe.path.empty() ? ct.file : probe.path;   // 用磁盘真名，不用 cue 里的拼写
            // ---- 曲名：TITLE → 轨 PERFORMER → 「文件名 #NN」 ----
            t.title = ct.title;
            if (t.title.empty()) t.title = ct.performer;
            if (t.title.empty()) {
                std::string stem = u8s(p8(ct.file).stem().u8string());
                if (stem.empty()) stem = ct.file;
                char b[32];
                std::snprintf(b, sizeof(b), " #%02d", ct.number);
                t.title = stem + b;
            }
            t.artist      = ct.performer.empty() ? albArtist : ct.performer;
            t.album       = sh.albumTitle.empty() ? ct.albumTitle : sh.albumTitle;
            t.albumArtist = albArtist;
            t.genre       = sh.genre;
            t.year        = year;
            t.trackNo     = (int)out.size() + 1;           // 专辑内连续 1..N
            t.discNo      = 0;
            t.cuePath     = sh.cuePath;
            t.cueNo       = ct.number;
            t.cueOff      = rta::meta::cue_frames_to_samples(ct.index1, sr);
            // ---- 长度：下一轨起点（同文件）或 文件总时长 - 本轨起点 ----
            long long lenSamples = -1;
            if (t.cueOff >= 0) {
                long long endSamples = -1;
                if (i + 1 < trs.size())
                    endSamples = rta::meta::cue_frames_to_samples(trs[i + 1]->index1, sr);
                else if (probe.duration > 0.0)
                    endSamples = (long long)std::llround(probe.duration * (double)sr);
                if (endSamples >= t.cueOff) lenSamples = endSamples - t.cueOff;
            }
            t.cueLen = lenSamples;
            // ---- 时长：与播放侧同一口径（采样数）；采样率未知才退回 cue 的帧差值 ----
            if (t.cueLen >= 0 && sr > 0) t.duration = (double)t.cueLen / (double)sr;
            else if (i + 1 < trs.size())  t.duration = rta::meta::cue_frames_to_sec(trs[i + 1]->index1 - ct.index1);
            else if (probe.duration > 0.0) {
                t.duration = probe.duration - (double)ct.index1 / 75.0;
                if (t.duration < 0.0) t.duration = 0.0;
            } else t.duration = 0.0;
            out.push_back(std::move(t));
        }
    }
    return out;
}

// 虚拟曲目在整轨里的播放区间（秒）。返回 false ⟹ 播放侧应退回整轨（头尾未知就别假装知道）。
inline bool lib_cue_range_sec(const LibTrack& t, double& startSec, double& endSec) {
    startSec = 0.0;
    endSec   = 0.0;
    if (!t.isCueTrack() || t.cueOff < 0 || t.cueLen <= 0 || t.sampleRate <= 0) return false;
    startSec = (double)t.cueOff / (double)t.sampleRate;
    endSec   = (double)(t.cueOff + t.cueLen) / (double)t.sampleRate;
    return endSec > startSec;
}

// ---------------------------------------------------------------------------
// 目录结构判据 → 扫描入库回填（2026-10-25）
//
// 背景（真机实测）：albumVecLocked() 在**读时**用 entity_find_album_dir_for_track() 纠正专辑名/碟号，
// 所以 UI 看起来是对的；但那只改了一个局部变量 —— 落盘的 rlplayer-index.tsv 里 album 列仍为空、
// disc 列恒 0，/api/lib/album 的曲目对象 d 也恒 0。读时纠正治不了「落盘数据与接口字段不说实话」。
//
// 这里把**同一份判据**挪到扫描入库处（ffprobe 探测之后）执行一次，结果写进 LibTrack：
//   * 真实标签优先：t.album 非空不写 album；t.discNo > 0 不写碟号。回填过的字段打 *FromDir 标记。
//   * 专辑名的取法与 albumVecLocked 逐字一致（先 entity_parse_album_circle 拆「专辑 @ 社团」），
//     否则落盘名 ≠ 读时推算名 → 专辑键（albumArtist \x1f album 的 FNV）会变，老链接/缓存全失效。
//   * 索引列数/顺序不变（不加列）：落盘值已按「真实标签优先」定好，读回后无需再判来源。
//     代价：目录**改名**后要等下一次全量扫描（/api/lib/scan?full=1）才会刷新 —— 增量扫描只按
//     mtime+size 复用旧记录，见 startScan 里的缓存分支（那里对「album 空 / disc 0」的老记录会自愈）。
// ---------------------------------------------------------------------------

// 判据给一首曲目推出的专辑名与碟号（只依赖父目录，故同一目录的结果必然相同）
struct LibDirAlbum {
    std::string album;      // 已拆过「专辑 @ 社团」的专辑名（与 albumVecLocked 的 aName 同源同算法）
    int  disc = 0;          // 目录推断的碟号（0 = 没推断出来）
    bool fromDir = false;   // true = 命中真实专辑目录；false = 只有父目录名兜底（判据没命中）
};

inline LibDirAlbum lib_dir_album_for(const std::string& trackPath) {
    LibDirAlbum r;
    int inferredDisc = 0;
    const fsx::path albDir = rta::meta::entity_find_album_dir_for_track(p8(trackPath), &inferredDisc);
    if (!albDir.empty()) {
        const std::string dn = u8s(albDir.filename().u8string());
        std::string a, c;
        rta::meta::entity_parse_album_circle(dn, a, c);
        r.album   = a.empty() ? dn : a;
        r.disc    = inferredDisc > 0 ? inferredDisc : 0;
        r.fromDir = true;
        return r;
    }
    // 兜底与 albumVecLocked 同源：判据没命中时用父目录名（那里也是这么做的），保证读时/写时同名同键。
    const std::string dn = u8s(p8(trackPath).parent_path().filename().u8string());
    r.album = dn.empty() ? std::string("未知专辑") : dn;
    return r;
}

// 按父目录 memo 推断结果：并行扫描时同一目录会被多个线程问到，目录扫描（stat）不必重复做。
// 键 = 父目录绝对路径（判据只依赖它），不是曲目路径。
struct LibDirAlbumMemo {
    std::mutex mtx;
    std::unordered_map<std::string, LibDirAlbum> m;
    LibDirAlbum get(const std::string& parentDir, const std::string& trackPath) {
        { std::lock_guard<std::mutex> lk(mtx); auto it = m.find(parentDir); if (it != m.end()) return it->second; }
        LibDirAlbum r = lib_dir_album_for(trackPath);
        std::lock_guard<std::mutex> lk(mtx);
        if (m.size() < 65536) m.emplace(parentDir, r);
        return r;
    }
};

// 把目录判据回填进一首曲目：真实标签优先，已有的字段绝不被覆盖。
inline void lib_fill_album_disc_from_dir(LibTrack& t, LibDirAlbumMemo* memo = nullptr) {
    if (!t.album.empty() && t.discNo > 0) return;          // 两样都有真实标签 → 连目录都不用看
    const std::string pdir = u8s(p8(t.path).parent_path().u8string());
    const LibDirAlbum da = memo ? memo->get(pdir, t.path) : lib_dir_album_for(t.path);
    if (t.album.empty() && !da.album.empty()) { t.album = da.album; t.albumFromDir = true; }
    if (t.discNo <= 0 && da.disc > 0)         { t.discNo = da.disc; t.discFromDir = true; }
}

// 一次 ffprobe 取全部信息（比 media_info.h 的两次调用快一倍）
inline LibTrack lib_probe_one(const std::string& file, const std::string& ffprobePath = "") {
    LibTrack t;
    t.path = file;
    // ===================== 原生标签快速通道（调度层，2026-10-25） =====================
    // 261025-01 一.1/一.8：首次全量扫描 45.9 首/s（3000 首 65.4 s），瓶颈是**每个文件起一次 ffprobe 进程**。
    // src/meta/tags_native.h 就地解析 FLAC/WAV/MP3/OGG 的头部与标签块，语义与下面 ffprobe 分支逐字段对齐
    // （见该文件顶部的实测依据：LAME delay/padding、WAV data 截断、OGG 末页 granulepos、bits 的 N/A 口径…）。
    //
    // 保底原则：原生通道**只在结构完全说得通时**返回 true；任何解析失败/不支持的变体（ID3v2.2、
    // unsynchronisation、数字流派引用、Opus、RF64、"id3 " 块…）一律返回 false，原样落到下面的 ffprobe，
    // 保证「关掉原生通道」与「开着」得到同一份入库结果（tests/verify 与 tests/test_tags_native.cpp 判据）。
    // 环境变量 RLPLAYER_NATIVE_TAGS=0 可整体关闭（默认开）。
    if (rta::meta::native::enabled()) {
        rta::meta::native::TagInfo ni;
        std::string why;
        if (rta::meta::native::probe_file(file, &ni, &why)) {
            t.title = ni.title; t.artist = ni.artist; t.album = ni.album;
            t.albumArtist = ni.albumArtist;         // （提前：下面文件名兜底要用它去对「艺人 - 」前缀）
            // ---- 261026-02 #4 规则 3：文件名兜底（**只对残缺值生效**，四类容器通用）----
            // 通用能力，不认容器、不认具体文件名：判据只有"这个字段是不是残缺值"
            // （detail::tag_is_corrupt）+ "文件名能不能推出一条像样的名字"。总开关 RLPLAYER_TAG_GUESS。
            // 放在这里而不是 tags_native.h：ffprobe 回退分支同样要享受这条规则（同一份代码路径）。
            // 与 probe_file 里的那份**完全同源**（同一个 detail::qm_guess_title_from_filename），
            // probe_file 的先跑一次，这里再跑一次是幂等的（都只对残缺值生效）。
            if (rta::meta::native::guess_enabled()) {
                if (rta::meta::native::detail::tag_is_corrupt(t.title)) {
                    const std::string g = rta::meta::native::detail::qm_guess_title_from_filename(
                        file, t.artist, t.albumArtist);
                    if (!g.empty() && !rta::meta::native::detail::tag_is_corrupt(g)) {
                        std::fprintf(stderr, "TAGGUESS file=%s field=title from=filename value=%s old=%s\n",
                                     file.c_str(), g.c_str(), t.title.c_str());
                        t.title = g;
                    }
                }
            }
            t.genre = ni.genre;
            t.trackNo = ni.trackNo; t.discNo = ni.discNo; t.year = ni.year;
            t.sampleRate = ni.sampleRate; t.channels = ni.channels;
            t.bits = ni.bits; t.duration = ni.duration; t.codec = ni.codec;
            // 与下面 ffprobe 分支的老口径逐条对齐：
            //   * ffprobe 的 bits_per_raw_sample 对 16-bit WAV / MP3 / OGG 是 N/A ⟹ 旧代码统一写成 16；
            //     FLAC/WAV-24bit 则报真值（24）。这里同样是「<=0 就 16」。
            if (t.bits <= 0) t.bits = 16;
            // 同目录独立封面（只 stat）与内嵌封面任一成立即算有封面 —— 与下面同一行逻辑。
            t.hasCover = ni.hasCover ? 1 : 0;
            if (!t.hasCover && lib_has_cover_file(t.path)) t.hasCover = 1;
            return t;
        }
    }
    const std::string ffprobe = !ffprobePath.empty() ? ffprobePath : rta::audio::find_tool("ffprobe");
    if (ffprobe.empty()) return t;

    std::string out;
    // 2026-10-23 真机 DEBUG（用户反馈「1.3.x 封面不显示」，实测曲目 C:\...\谭晶 - 游京 (长征版).flac）：
    // 那首是**单文件 + 内嵌封面**（同目录没有 cover.jpg），而 hasCover 只看同目录文件 → cv:false → UI 不显示封面；
    // 但 /api/cover 其实能抽出 706,780 字节的图。所以必须在这里同时问「有没有视频流（=内嵌封面）」。
    // 做法：去掉 -select_streams（一次读出所有流），流里 codec_type=video 即内嵌封面/图片。
    //   ⚠️ 注意：某些容器把封面报成 video 流，所以只看 codec_type 即可，不看 codec_name。
    // * 2026-10-25（261025-01 关联缺陷，实测）：只取 format_tags 会漏掉 **OGG/Opus 的 Vorbis Comment**
    // —— 它们挂在 stream_tags 上。实测 ffprobe -show_entries format_tags=... 对
    //   "夏微 - 天鸟.ogg" 返回空，而 stream_tags 给出
    //   ALBUM=翻唱集 / ARTIST=夏微 / TITLE=天鸟（Cover：奥華子）
    // ⟹ 该文件在库里退化成"文件名当曲名、目录名当专辑"。
    // 修法：一次调用同时取 stream_tags 与 format_tags，并用 -of default=nw=0（带 [STREAM]/[FORMAT]
    // 段标记）解析，从而确定性地实现「format 优先、缺失回退 stream」——
    // 不依赖 ffprobe 的输出顺序（只靠"后者覆盖前者"是脆的：一旦顺序变了，stream 就会覆盖真标签）。
    // 另测：mp3(ID3) / wav(INFO) / m4a 的标签都在 format_tags，行为不变。
    const std::wstring cmd =
        L"\"" + rta::audio::widen(ffprobe) + L"\" -v error"
        L" -show_entries stream=codec_type,codec_name,sample_rate,channels,bits_per_raw_sample"
        L":stream_tags=title,artist,album,album_artist,date,genre,track,disc"
        L":format=duration:format_tags=title,artist,album,album_artist,date,genre,track,disc,tracktotal"
        L" -of default=nw=0 \"" + rta::audio::widen(file) + L"\"";
    if (rta::audio::run_hidden(cmd, &out) != 0) return t;

    std::string title, artist, album, aartist, date, genre, track, disc;
    bool curIsAudio = false;   // 当前解析到的流是不是音频（封面流的字段不能覆盖音频参数）
    bool inFormat = false;     // 当前是否在 [FORMAT] 段（=标签权威来源；[STREAM] 段只做兜底）
    std::string line;
    for (size_t i = 0; i <= out.size(); ++i) {
        if (i == out.size() || out[i] == '\n') {
            // -of default=nw=0 会打印段标记：[STREAM] / [/STREAM] / [FORMAT] / [/FORMAT]
            if (line == "[FORMAT]") { inFormat = true; line.clear(); continue; }
            if (line == "[/FORMAT]") { inFormat = false; line.clear(); continue; }
            if (line == "[STREAM]") { inFormat = false; curIsAudio = false; line.clear(); continue; }
            const size_t e = line.find('=');
            if (e != std::string::npos) {
                std::string k = line.substr(0, e);
                const std::string v = line.substr(e + 1);
                for (auto& c : k) c = (char)std::tolower((unsigned char)c);
                if (k.compare(0, 4, "tag:") == 0) k = k.substr(4);
                // format 段 = 真标签，覆盖；stream 段 = 只填空（OGG/Opus 的 Vorbis Comment 在这里）
                auto put = [&](std::string& dst) {
                    if (!inFormat && !dst.empty()) return;
                    dst = v; while (!dst.empty() && (dst.back() == '\r' || dst.back() == ' ')) dst.pop_back(); };
                // 2026-10-23：内嵌封面判定 —— 出现任意 codec_type=video 的流即视为有封面
                if (k == "codec_type") {
                    std::string v2 = v; while (!v2.empty() && (v2.back() == '\r' || v2.back() == ' ')) v2.pop_back();
                    if (v2 == "video") t.hasCover = 1;
                    curIsAudio = (v2 == "audio");
                    line.clear(); continue;
                }
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
                // 2026-10-23：去掉 -select_streams 后，封面流（mjpeg）的字段会覆盖音频流
                // （实测：24-bit FLAC + mjpeg 封面 → t.bits 被写成 8）。所以这里只在「当前流是音频」时采信。
                else if (k == "sample_rate")  { if (curIsAudio) t.sampleRate = atoi(v.c_str()); }
                else if (k == "channels")     { if (curIsAudio) t.channels = atoi(v.c_str()); }
                else if (k == "bits_per_raw_sample") { if (curIsAudio) t.bits = atoi(v.c_str()); }
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
    // 只置位不清零：上面若已从内嵌封面流判定为 1，不能被同目录探测覆盖回 0。
    // （两种情况任一成立即视为有封面：内嵌 PICTURE/APIC 流，或同目录 cover/folder/front 文件。）
    if (!t.hasCover && lib_has_cover_file(t.path)) t.hasCover = 1;
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
// 环境变量读成 UTF-8：**不能用 std::getenv** —— MS CRT 的 getenv 把环境块按 ANSI(ACP) 返回，
// 含中文的路径（例如 RLPLAYER_LIBRARY_DIR=D:\数据库\音乐）拿到的是 GBK 字节，后面 widen(CP_UTF8)
// 会把它们变成 U+FFFD ⟹ 读到的 conf/index 路径是错的 ⟹ 库配置"消失"、索引写不进去，
// 紧接着 load() 的自动探测又去扫用户真实音乐目录（实测：库里冒出别人的专辑）。
// 一律走 _wgetenv + wide_to_utf8。
inline std::string lib_env_utf8(const wchar_t* wname, const char* nname) {
    // 两份来源都要看，因为两边都可能被"按 ACP 转过一次"：
    //   * 窄环境块（getenv）：系统环境本身是 UTF-16，CRT 会按 **ACP** 转成窄串 ⟹ 含中文的
    //     目录（例如用户名是中文的 USERPROFILE）在这里就已经是 GBK 字节了。
    //   * 宽环境块（_wgetenv）：准确；但用 _putenv_s 由本进程写进去的窄串（测试/便携启动器
    //     常这么干）是 UTF-8，CRT 再按 ACP 转宽 ⟹ 宽版本反而是坏的。
    // 判据：窄串能通过严格 UTF-8 校验就当 UTF-8 用，否则退回宽串。
    if (nname) {
        if (const char* n = std::getenv(nname)) {
            std::string s(n);
            if (!s.empty() && lib_valid_utf8(s)) return s;
        }
    }
    const wchar_t* v = _wgetenv(wname);
    return v ? rta::audio::wide_to_utf8(v) : std::string();
}
inline std::string Library::libDataDir() {
    std::string s = lib_env_utf8(L"RLPLAYER_LIBRARY_DIR", "RLPLAYER_LIBRARY_DIR");
    if (!s.empty()) {
        while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
        if (!s.empty()) return s;
    }
    return exeDirOrEmpty();
}
// 文件名区分实例：默认实例（8899）用原名；--port 指定的其它实例加 ".p<端口>" 后缀。
// 为什么不再共用一份：自检沙箱（8907/8908/8911）会写自己的库根与索引，共用会把用户库覆盖成空
// （1.3.x「升级后曲库变空」的另一半原因）。端口从 RLPLAYER_PORT 取（rtaweb 启动时设置）。
inline std::string Library::confLeaf(const char* base) {
    // ★ 2026-10-26（261026-04 真机根因）：旧规则「非 8899 端口就加 .p<port>」把**用户数据切成多套**：
    //   默认端口被占（例如还开着上一版实例）→ rtaweb 换端口启动 → 读写另一份 conf/索引 →
    //   用户看到「曲库变空 / 296 张旧卡片 / 改动全不生效」。
    // 新规则：**只有显式隔离才加后缀** ——
    //   ① RLPLAYER_LIBRARY_DIR 已设置（自检沙箱、测试脚本都走这条）；
    //   ② 显式 RLPLAYER_ISOLATE=1。
    //   正常启动即使换了端口，也一律用主库，绝不再因为端口不同而分裂数据。
    const std::string port = lib_env_utf8(L"RLPLAYER_PORT", "RLPLAYER_PORT");
    std::string name(base);
    // RLPLAYER_LIBRARY_DIR 本身就已经是隔离目录 ⟹ 用不带后缀的文件名（测试脚本/沙箱都按这个名字准备数据）。
    // 只有「同一个目录里并行跑多个端口」这种极端场景，才用 RLPLAYER_ISOLATE=1 显式要求按端口再分一份。
    const bool hasDir = !lib_env_utf8(L"RLPLAYER_LIBRARY_DIR", "RLPLAYER_LIBRARY_DIR").empty();
    const bool multiInstance = lib_env_utf8(L"RLPLAYER_ISOLATE", "RLPLAYER_ISOLATE") == std::string("1");
    if (!hasDir && multiInstance && !port.empty() && port != "8899") name += ".p" + port;
    return name;
}
inline std::string Library::confPath() {
    const std::string d = libDataDir(), leaf = confLeaf("rlplayer-library.conf");
    return d.empty() ? leaf : d + "\\" + leaf;
}
inline std::string Library::indexPath() {
    const std::string d = libDataDir(), leaf = confLeaf("rlplayer-index.tsv");
    return d.empty() ? leaf : d + "\\" + leaf;
}
// 配置/索引文件的读写：路径是 UTF-8，流一律用**宽字符文件名**打开。
// （窄串 ctor 与 p8 注释里那条 ACP 转换是同一类隐患：exe 装在中文目录下时，
//   一旦转换按 ACP 走，读到的就是空配置 → 库"变空"、索引被重建。宽字符没有这个问题。）
inline std::string Library::readFile(const std::string& p) {
    std::ifstream f(rta::audio::widen(p).c_str(), std::ios::binary);
    if (!f) {
        // 老版本用 rta- 前缀：读不到新文件时回退一次（迁移用，不重建用户的库）
        if (p.rfind("rlplayer-", 0) == 0) {
            std::ifstream f2(rta::audio::widen("rta-" + p.substr(9)).c_str(), std::ios::binary);
            if (f2) return std::string((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
        }
        return "";
    }
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}
inline void Library::writeFile(const std::string& p, const std::string& s) {
    const std::string tmp = p + ".tmp";
    {
        std::ofstream f(rta::audio::widen(tmp).c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(s.data(), (std::streamsize)s.size());
        // ★ 2026-10-25（索引一致性审查）：必须在 flush 之后才能 rename。
        //   旧实现出了作用域才析构 → 析构时才 flush；写盘出错时剩下的只有"截断的 tmp"，
        //   而 rename 不看内容。这里把"写失败"变成"绝不替换旧索引"，并在 rename 前检查。
        f.flush();
        if (!f.good()) { f.close(); std::error_code e2; fsx::remove(p8(tmp), e2); return; }
    }
    std::error_code ec;
    fsx::rename(p8(tmp), p8(p), ec);
    // rename 不能覆盖已存在文件时（个别网络盘/杀软占用），退化为"删旧再改名"；
    // 仍然失败就保留 tmp（不回滚旧索引 —— 旧索引比没有索引好）。
    if (ec) {
        std::error_code e2;
        fsx::remove(p8(p), e2);
        ec.clear();
        fsx::rename(p8(tmp), p8(p), ec);
    }
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
            // ★ 2026-10-26（261026-04 真机根因之四）：升级迁移。
            // 新版本解压到新目录后没有 conf，旧版本目录里却有用户的库根 ——
            // 若不做迁移，用户升级后看到的是"空库/4 张"，会觉得新版本什么都没了。
            // 依次在 exe 同级、上级、以及上级目录下其它 RLPlayer-* 目录里找 conf，找到就沿用。
            {
                std::vector<std::string> dirs;
                const std::string self = libDataDir();
                auto parentOf = [](std::string s) {
                    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
                    const size_t k = s.find_last_of("\\/");
                    return (k == std::string::npos) ? std::string() : s.substr(0, k);
                };
                const std::string up1 = parentOf(self);
                const std::string up2 = parentOf(up1);
                if (!up1.empty()) { dirs.push_back(up1); dirs.push_back(up1 + "\\data"); }
                if (!up2.empty()) dirs.push_back(up2);
                // 扫 dist 下其它版本目录（最多 40 个，按名字倒序 = 版本从新到旧）
                std::error_code ec2;
                if (!up2.empty()) {
                    std::vector<std::string> vers;
                    for (auto it = fsx::directory_iterator(p8(up2), ec2);
                         !ec2 && it != fsx::directory_iterator(); it.increment(ec2)) {
                        const std::string nm = u8s(it->path().filename().u8string());
                        if (nm.rfind("RLPlayer-", 0) == 0) vers.push_back(u8s(it->path().u8string()));
                    }
                    std::sort(vers.begin(), vers.end(), std::greater<std::string>());
                    if (vers.size() > 40) vers.resize(40);
                    for (auto& v : vers) { dirs.push_back(v); dirs.push_back(v + "\\data"); }
                }
                std::fprintf(stderr, "LIBROOTMIG self=[%s] candidates=%zu\n", self.c_str(), dirs.size());
                for (const auto& d : dirs) {
                    if (d == self) continue;
                    const std::string cf = d + "\\rlplayer-library.conf";
                    std::error_code ec;
                    if (!fsx::exists(p8(cf), ec)) continue;
                    std::string other = readFile(cf);
                    if (other.size() >= 3 && (unsigned char)other[0] == 0xEF) other = other.substr(3);
                    std::string ln;
                    for (size_t i = 0; i <= other.size(); ++i) {
                        if (i == other.size() || other[i] == '\n') {
                            while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
                            if (!ln.empty() && ln[0] != '#') roots_.push_back(ln);
                            ln.clear();
                        } else ln += other[i];
                    }
                    if (!roots_.empty()) {
                        confMigrated_ = true;
                        confMigratedFrom_ = cf;
                        break;
                    }
                }
            }
        }
        if (roots_.empty()) {
            // 首次运行：探一遍常见位置，别一打开就是空的（HQPlayer 被诟病的就是这个）
            std::vector<std::string> cands;
            const std::string up = lib_env_utf8(L"USERPROFILE", "USERPROFILE");   // 同上：不能只信 getenv（中文用户名）
            if (!up.empty()) {
                cands.push_back(up + "\\Music");
                cands.push_back(up + "\\音乐");
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
    // 2026-10-23：索引格式版本校验。旧索引（无 #libIndexV 或版本不符）直接丢弃，
    // 本次扫描会全量重建 —— 否则升级后「扫描逻辑改了但旧索引被复用」，修复永远不生效。
    // 代价：升级后首次启动会全量扫一遍（653 首约 10 秒，并行 8 线程），之后照旧增量。
    // v4 = 2026-10-25 起「CUE 虚拟分轨」：末尾追加 cue / cueN / cueOff / cueLen 四列。
    // 为什么版本不符要整份丢弃：v3 索引里那些 40 分钟的"整轨曲目"如果被复用，cue 永远不会生效
    // （增量扫描按 mtime+size 复用旧记录，而 cue 文件本身不在旧记录里）。代价：升级后首次启动
    // 全量扫一遍（653 首约 10 秒）。
    {
        // 2026-10-26（261026-04）升到 v5：本轮改了两件会影响**已落盘字段**的事 ——
        //   ① 标签占位修复（R1/R1b/R3 会改 title/artist/album 的值）；
        //   ② 专辑分组口径（键 = 专辑名 + 目录，不再含 artist）。
        // v4 索引里存的是旧解析器写下的旧值，且版本号没变 ⟹ 会被原样复用，
        // 于是"代码改了、界面不变"（实机证据：1.5.2 包扫同一份 MUSIC 全新库是 181 张，
        // 而用户机器上仍是 296 张、满屏问号）。规则：口径一变就升版本，让旧索引整份作废。
        // 代价：升级后首次启动全量重扫一次（659 首约 0.8 s，8 线程）。
        // v5 = 2026-10-26：标签占位修复 + 专辑归并口径。**低版本索引不再整份丢弃**，
        //      而是「读路径/mtime/size，但标签必须重探」（见 startScan 里的 forceRetag）。
        //      为什么不丢弃：整份丢弃会让「用户的库结构」也一起没了（真机踩到：升级后曲库变空/只剩 4 首）。
        idxIsV5 = (idx.find("#libIndexV\t5") != std::string::npos);
        idxHasHeader = (idx.find("#libIndexV\t") != std::string::npos);
        idxIsV4 = (idx.find("#libIndexV\t4") != std::string::npos);
        // v3 及更早：**整份丢弃**（沿用既有铁律）。
        // 为什么 v3 不能像 v4 那样"只重探标签"：v3 里存在 40 分钟的"整轨曲目"记录，
        // 一旦被复用，CUE 分轨永远不生效（测试 [v3 索引被整份丢弃] 就是守这条）。
        if (!idx.empty() && !idxIsV5 && !idxIsV4) idx.clear();
    }
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
                        // v4 追加列（v3 只有 16 列；前 16 列的读取路径与 v3 逐字相同）
                        if (f.size() >= 20) {
                            t.cuePath = f[16];
                            t.cueNo   = atoi(f[17].c_str());
                            t.cueOff  = _strtoi64(f[18].c_str(), nullptr, 10);
                            t.cueLen  = _strtoi64(f[19].c_str(), nullptr, 10);
                        }
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
        trackCount_.store(tracks_.size());
        albumCount_.store(0);          // 下次 albums() 会重建并回填（见 albumVecLocked）
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
                        if (st > 0) scanThreads_.store((size_t)st);
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
    writeFile(indexPath(), indexTextLocked());
}

// 持锁只做内存序列化；写盘由调用方在**锁外**做（扫描收尾时 3000+ 行的 tmp+rename 不该
// 让 /api/lib/status 排在它后面 —— 261025-01 一.1「扫描时界面卡死」的一半原因就是这个锁）。
inline std::string Library::indexTextLocked() const {
    std::string s;
    s.reserve(tracks_.size() * 160 + 64);
    // 索引格式版本：改动「扫描产出字段」时必须 +1，否则旧索引会因为 mtime+size 未变而被整份复用，
    // 新逻辑永远不生效（2026-10-23 真机踩到：加了内嵌封面判定，用户那首 cv 仍是 false）。
    // v2 = 2026-10-23 起 hasCover 同时覆盖「内嵌封面流」与「同目录封面文件」。
    // v3 = 2026-10-23 起「音频流字段不再被封面流覆盖」(24-bit 不再变 8)
    // v4 = 2026-10-25 起 CUE 虚拟分轨：**前 16 列逐字不变**，末尾追加 cue / cueN / cueOff / cueLen。
    //      为什么追加而不是插列：老读取路径（含用户手里 v3 索引）只看前 16 列，列序不动才不会串列。
    s += "#libIndexV\t5\n";
    s += "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre"
         "\tcue\tcueN\tcueOff\tcueLen\n";
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
        s += lib_clean_field(t.genre);     s += '\t';
        s += lib_clean_field(t.cuePath);   s += '\t';
        std::snprintf(buf, sizeof(buf), "%d\t%lld\t%lld\n", t.cueNo, t.cueOff, t.cueLen);
        s += buf;
    }
    return s;
}

// 专辑整理（读时推算）。
//
// ★ 2026-10-25 性能重写（261025-01 一.5「切标签每次都像重新扫描」/ 一.8「导入几千首后要等几分钟」）：
// 旧实现在 **std::sort 的比较器里**调 entity_find_album_dir_for_track()（每次都是一轮目录 stat +
// 逐文件判定）。N 首曲目的比较次数是 O(N logN)，每比较 2 次目录扫描 ⟹ 3000 首约 3~6 万次目录扫描，
// 全在 mtx_ 里做；/api/lib/albums 与 /api/lib/stats 都走这里，而 rtaweb 的 HTTP 是单线程 accept 循环
// ⟹ 一次专辑重建能把整个控制面堵住十几秒，看着就像"卡死/又在扫描"。
// 现在：① 每个目录只 stat 一次（LibDirAlbumMemo，键=父目录）；② 碟号预先算进 discOf[]，
// 比较器里**不再碰文件系统**；③ 结束时把专辑数写进 albumCount_（status 无锁读）。
// 专辑键用的目录归一：把 CD1 / Disc 2 / disk_3 / CD-01 之类的后缀去掉，让多碟装仍归成一张专辑。
// 只做小写化 + 去尾部碟号，不动中间路径（避免把 "CD Collection" 这类正常目录名改坏）。
inline std::string lib_dir_album_key(const std::string& dir) {
    std::string d;
    d.reserve(dir.size());
    for (char c : dir) d.push_back((char)std::tolower((unsigned char)c));
    for (;;) {
        size_t e = d.size();
        while (e > 0 && (d[e - 1] == '\\' || d[e - 1] == '/')) --e;
        size_t digitsEnd = e;
        while (e > 0 && std::isdigit((unsigned char)d[e - 1])) --e;
        if (e == digitsEnd) break;
        size_t q = e;
        if (q > 0 && (d[q - 1] == '-' || d[q - 1] == '_' || d[q - 1] == ' ')) --q;
        if (q >= 2 && d.compare(q - 2, 2, "cd") == 0) { d.resize(q - 2); continue; }
        if (q >= 4 && d.compare(q - 4, 4, "disc") == 0) { d.resize(q - 4); continue; }
        if (q >= 4 && d.compare(q - 4, 4, "disk") == 0) { d.resize(q - 4); continue; }
        break;
    }
    return d;
}

inline const std::vector<LibAlbum>& Library::albumVecLocked() const {
    if (!albumsDirty_) return albums_;
    albums_.clear();
    const size_t n = tracks_.size();
    LibDirAlbumMemo memo;               // 局部 memo：同目录只扫一次
    std::vector<int> discOf(n, 0);      // 预先算好排序用碟号（comparator 里必须零 IO）
    std::map<std::string, size_t> byKey;
    for (size_t i = 0; i < n; ++i) {
        const auto& t = tracks_[i];
        std::string aName = t.album;
        std::string aArtist = t.albumArtist.empty() ? t.artist : t.albumArtist;
        int discForSort = t.discNo;
        std::string pdir = u8s(p8(t.path).parent_path().u8string());
        if (aName.empty() || discForSort == 0) {
            const LibDirAlbum da = memo.get(pdir, t.path);
            if (da.fromDir) {
                if (aName.empty()) aName = da.album;
                if (discForSort == 0 && da.disc > 0) discForSort = da.disc;
            } else if (aName.empty()) {
                aName = da.album.empty() ? std::string("未知专辑") : da.album;
            }
        }
        discOf[i] = discForSort;
        // ★ 2026-10-26（261026-02 #3 的根因之一）：专辑键**不能带 artist**。
        // 旧键 = (albumArtist?:artist) + "\x1f" + album ⟹ 同一张碟里每首 artist 不同就被切成多个"专辑"：
        // 实测本库 654 首被切成 **301 张卡**，而物理目录只有 87 个（RESOLVERS 一张碟裂成 19 张）。
        // 后果：专辑页看着零碎、封面按裂开的卡逐张去搜（同一张碟搜 19 次）、覆盖率分母被人为放大。
        // 新键 = 专辑名 + "\x1f" + 目录（目录做 CD1/Disc 2 归一，保证多碟装仍合成一张）。
        const std::string dirKey = lib_dir_album_key(pdir);
        const std::string rawKey = aName + "\x1f" + dirKey;
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
            const int pDisc = discOf[x], qDisc = discOf[y];
            if (pDisc != qDisc) return pDisc < qDisc;
            const auto& p = tracks_[x]; const auto& q = tracks_[y];
            if (p.trackNo != q.trackNo) return p.trackNo < q.trackNo;
            return p.path < q.path;
        });
    }
    std::sort(albums_.begin(), albums_.end(), [](const LibAlbum& a, const LibAlbum& b) {
        const std::string x = a.artist + "\x1f" + a.name, y = b.artist + "\x1f" + b.name;
        return x < y;
    });
    albumsDirty_ = false;
    albumCount_.store(albums_.size());
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

// 261025-02 第 6 条（封面缺失的真根因，实测 145/246 位艺术家只剩首字母）：
//   get_cover() 在「既无同目录 cover.*、也没有内嵌封面」时**不返回空**，而是返回一个 1x1 透明 PNG
//   占位图（media_info.h:365-375，77~200 字节）。旧实现只按索引里的首曲问一次：
//     * 首曲没图 + 专辑后面几首有图 ⟹ 整张专辑 cv:true 却是空白（img 不报错，onerror 兜不住）；
//     * 结果被写进 coverCache_，**永不重试**（即使换了一张专辑的查询来源）。
//   现在的规则：
//     1) 按 cv 排序（有封面的曲目优先），而不是只看 idx[0]；
//     2) 拿到的是"可疑小图"（< 1 KB）就继续问下一首，直到拿到真图或问完；
//     3) 空结果写进"负缓存"（first 为空，永久），小图**不**写缓存（下次还能重试）。
//   判据：tests/test_library.cpp 的画布用例 + 真库抽样（docs/261025-02-封面与字体核查.md）。
inline bool Library::albumCover(const std::string& key, std::vector<unsigned char>& bytes, std::string& mime) {
    // 抽出顺序：先本专辑 cv=1 的曲目，再其余曲目（最多各 3 首，避免对 200 首的合辑起 200 次 ffmpeg）
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = coverCache_.find(key);
        if (it != coverCache_.end()) {
            if (it->second.first.empty()) return false;
            bytes = it->second.first; mime = it->second.second; return true;
        }
        const auto& v = albumVecLocked();
        std::vector<std::string> withCv, without;
        for (auto& a : v) {
            if (a.key != key) continue;
            for (size_t ix : a.idx) {
                if (ix >= tracks_.size()) continue;
                if (tracks_[ix].hasCover) { if (withCv.size() < 3) withCv.push_back(tracks_[ix].path); }
                else                      { if (without.size() < 3) without.push_back(tracks_[ix].path); }
            }
            break;
        }
        order = withCv;
        order.insert(order.end(), without.begin(), without.end());
    }
    if (order.empty()) return false;
    // < 1 KB 的图基本只可能是占位/损坏：真实封面（含 APIC 缩略图）极少这么小。
    // 这里不直接判死，而是"继续试下一首"——全试完仍只有小图才用它（总比空白好）。
    std::vector<unsigned char> b;
    std::string smallMime;
    std::vector<unsigned char> small;
    for (const auto& p : order) {
        std::string m;
        std::vector<unsigned char> cand = rta::audio::get_cover(p, m);
        if (cand.size() >= 1024) { b.swap(cand); mime = m; break; }
        if (small.empty() && !cand.empty()) { small.swap(cand); smallMime = m; }
    }
    const bool usable = (b.size() >= 1024);          // true = 真封面；false = 只有占位图
    if (!usable) { b.swap(small); mime = smallMime.empty() ? "image/png" : smallMime; }
    // 只有"确定拿不到"（空）或"拿到真封面"才进缓存：占位图不缓存，下次打开 albums 页还能重试。
    if (!usable) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (coverCache_.size() > 256) coverCache_.clear();
        coverCache_[key] = { std::vector<unsigned char>(), "image/png" };   // 负缓存
        bytes.clear();
        return false;                                 // 让 <img onerror> 露回首字母占位（不是一块白）
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (coverCache_.size() > 256) coverCache_.clear();
        coverCache_[key] = { b, mime };
    }
    bytes.swap(b);
    return !bytes.empty();
}

// ---------------- 封面在线抓取：按 album+artist 反查（2026-10-26）----------------
// 为什么由 Library 出这份数据，而不是让 rtaweb 自己拼：
//   /api/lib/albums 里的 name/artist 是 albumVecLocked() 的**成品**（真实标签，缺了才用目录名回填，
//   且 albumArtist 优先于 artist）。抓取侧只拿到专辑键，若自己回头翻 tracks_ 拼搜索词，
//   就会与 UI 上显示的那张专辑不一致（缺标签的专辑在 UI 上叫目录名，tracks_ 里却是空串）。
// 只读：不落盘、不改索引；专辑表还没整理时由 albumVecLocked() 触发一次整理（与 albums() 同代价）。
inline std::string lib_cover_term(const std::string& name, const std::string& artist) {
    std::string t = name;
    if (!artist.empty()) { if (!t.empty()) t += ' '; t += artist; }
    return t;
}

inline std::vector<LibCoverTerm> Library::albumsWithoutCover() const {
    std::vector<LibCoverTerm> out;
    std::lock_guard<std::mutex> lk(mtx_);
    const auto& v = albumVecLocked();
    out.reserve(v.size());
    for (const auto& a : v) {
        if (a.hasCover) continue;                 // 库里已有图（内嵌流或同目录封面文件）
        LibCoverTerm t;
        t.key = a.key; t.name = a.name; t.artist = a.artist;
        t.term = lib_cover_term(a.name, a.artist);
        out.push_back(std::move(t));
    }
    return out;
}

inline bool Library::albumCoverTerm(const std::string& key, LibCoverTerm& out) const {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& a : albumVecLocked()) {
        if (a.key != key) continue;
        out.key = a.key; out.name = a.name; out.artist = a.artist;
        out.term = lib_cover_term(a.name, a.artist);
        return true;
    }
    return false;
}

// ---------------- API 补图：元数据包反查（2026-10-26）----------------
// 调用方已持锁时用：取 a.idx 里前 maxTitles 个**非空**曲名（idx 已按 disc/track 排好）。
inline void lib_api_titles_locked(const std::vector<LibTrack>& tr, const LibAlbum& a,
                                 size_t maxTitles, std::vector<std::string>& out) {
    out.clear();
    if (maxTitles == 0) return;
    for (size_t ix : a.idx) {
        if (out.size() >= maxTitles) break;
        if (ix >= tr.size()) continue;
        if (!tr[ix].title.empty()) out.push_back(tr[ix].title);
    }
}

inline std::vector<LibApiCoverTerm> Library::albumsWithoutCoverTitles(size_t maxTitles) const {
    std::vector<LibApiCoverTerm> out;
    std::lock_guard<std::mutex> lk(mtx_);
    const auto& v = albumVecLocked();
    out.reserve(v.size());
    for (const auto& a : v) {
        if (a.hasCover) continue;                  // 库里已有图：与 albumsWithoutCover() 同一判据
        LibApiCoverTerm t;
        t.key = a.key; t.name = a.name; t.artist = a.artist;
        lib_api_titles_locked(tracks_, a, maxTitles, t.titles);
        out.push_back(std::move(t));
    }
    return out;
}

inline bool Library::albumCoverTermTitles(const std::string& key, size_t maxTitles,
                                         LibApiCoverTerm& out) const {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& a : albumVecLocked()) {
        if (a.key != key) continue;
        out.key = a.key; out.name = a.name; out.artist = a.artist;
        lib_api_titles_locked(tracks_, a, maxTitles, out.titles);
        return true;
    }
    return false;
}

inline std::vector<std::string> Library::artistAlbumNames(const std::string& artist,
                                                         size_t maxAlbums) const {
    std::vector<std::string> out;
    if (artist.empty() || maxAlbums == 0) return out;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& a : albumVecLocked()) {
        if (a.artist != artist) continue;
        if (a.name.empty()) continue;
        bool dup = false;
        for (const auto& s : out) if (s == a.name) { dup = true; break; }
        if (dup) continue;
        out.push_back(a.name);
        if (out.size() >= maxAlbums) break;
    }
    return out;
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
    { std::lock_guard<std::mutex> lk(msgMtx_); scanMsg_ = "正在遍历目录…"; }

    const size_t thCount = this->scanThreads();

    th_ = std::thread([this, roots, old, forceFull, thCount]() {
        // ★ 整个扫描体包一层 catch：扫描是**后台线程**，任何未捕获异常都会 std::terminate
        //   （MSVC 报 0xC0000409），进程直接消失 —— 261025-01 一.2「扫描中文路径直接卡死」
        //   就是这么发生的（根因见 p8 注释）。异常只允许降级成一条错误文案。
        try {
        const auto tStart = std::chrono::steady_clock::now();

        // 老记录按路径建索引，便于复用
        std::unordered_map<std::string, size_t> oldByPath;
        oldByPath.reserve(old.size() * 2);
        for (size_t i = 0; i < old.size(); ++i) oldByPath[old[i].path] = i;

        // ---- 扫描前先剔除「文件已不存在」的记录并落盘（261025-01 一.3）----
        // 这样即使本次遍历中途被取消/失败，索引里也不会永久留着已删除文件的行。
        // ★ 先在**锁外**把存在性问完（N 次 stat，3000 首约 50~150ms），锁内只做一次整体替换 ——
        //   否则 /api/lib/stats（要走 mtx_ 取 roots）在扫描刚启动时会被这一下堵住上百毫秒。
        int gone = 0;
        {
            std::unordered_map<std::string, bool> existsMemo;
            existsMemo.reserve(old.size() * 2);
            std::vector<const std::string*> dead;
            for (const auto& o : old) {
                auto itm = existsMemo.find(o.path);
                bool alive;
                if (itm == existsMemo.end()) { alive = lib_file_exists(o.path); existsMemo.emplace(o.path, alive); }
                else alive = itm->second;
                if (!alive) dead.push_back(&o.path);
            }
            if (!dead.empty()) {
                std::unordered_map<std::string, char> deadSet;
                deadSet.reserve(dead.size() * 2);
                for (auto* d : dead) deadSet[*d] = 1;
                std::string text;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    std::vector<LibTrack> kept;
                    kept.reserve(tracks_.size());
                    for (auto& t : tracks_) {
                        if (deadSet.count(t.path)) { ++gone; continue; }
                        kept.push_back(std::move(t));
                    }
                    if (gone > 0) {
                        tracks_.swap(kept);
                        albumsDirty_ = true;
                        albumCount_.store(0);
                        trackCount_.store(tracks_.size());
                        text = indexTextLocked();
                    }
                }
                if (gone > 0) writeFile(indexPath(), text);
            }
            removedMissing_.store(gone);
        }

        std::vector<LibTrack> fresh;
        std::vector<std::string> walked;
        std::vector<std::string> cueWalked;  // 单独的 .cue 收集（见下方「CUE 认领判定」）
        std::vector<std::string> badRoots;   // 打不开的根（含所有含非 ASCII 的旧版本症状）
        std::error_code ec;
        size_t seen = 0;
        auto lastTick = std::chrono::steady_clock::now();
        for (const auto& root : roots) {
            if (cancel_.load()) break;
            // ★ p8()：UTF-8 窄串直接喂 fs 会抛 filesystem_error（实测），必须转宽字符
            bool isDir = false;
            try {
                if (!lib_valid_utf8(root)) { badRoots.push_back(root); continue; }
                isDir = fsx::is_directory(p8(root), ec);
            } catch (const std::exception&) { isDir = false; }
            if (!isDir) { badRoots.push_back(root); continue; }
            for (auto it = fsx::recursive_directory_iterator(
                     p8(root), fsx::directory_options::skip_permission_denied, ec);
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
                } else if (lib_is_cue(ep)) {
                    // .cue 不是音频，不进 walked（否则会被当成曲目去 ffprobe）；
                    // 它只用来「认领整轨」，展开出的虚拟曲目在探测之后追加。
                    cueWalked.push_back(u8s(ep.u8string()));
                }
                // ---- 分片让出（261025-01 一.1：大库扫描不能把机器/界面拖死）----
                // 每 200 个目录项让出一次时间片，并且每 150ms 汇报一次「已发现多少首」，
                // 用户至少能看到进度在动；遍历本身是单线程 I/O，让出成本可忽略。
                if ((++seen & 0x3F) == 0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration<double, std::milli>(now - lastTick).count() >= 150.0) {
                        lastTick = now;
                        std::lock_guard<std::mutex> lk(msgMtx_);
                        scanMsg_ = "正在遍历目录… " + std::to_string(walked.size()) + " 首";
                    }
                    std::this_thread::yield();
                }
                it.increment(ec);
            }
        }
        if (cancel_.load()) { scanning_.store(false); return; }

        const int n = (int)walked.size();
        total_.store(n);
        // 只有 cue、没有任何可遍历音频（例如整轨在库根之外）时不能走"0 首"分支：
        // 那条分支会清空索引，而 cue 认领的整轨恰恰不在 walked 里。
        if (n == 0 && cueWalked.empty()) {
            const auto tEnd = std::chrono::steady_clock::now();
            const double durMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
            // ★ 2026-10-25 防"库被清空"：旧实现在 n==0 时无条件 tracks_.clear() —— 而"n==0"有两个
            //   完全不同的来源：①库里真的没有音频；②根目录打不开（旧版本遇中文根就是这种）。
            //   ②若不区分，一次打不开的根就把用户整份索引清空并落盘（261025-01 用户反馈的
            //   "扫描后音乐全没了"）。现在：有坏根就保留这些根下的老记录，只在真没有坏根时清空。
            std::vector<LibTrack> kept;
            if (!badRoots.empty()) {
                for (auto& o : old)
                    for (auto& br : badRoots)
                        if (lib_path_under(o.path, br)) { kept.push_back(o); break; }
            }
            std::string text;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                tracks_.swap(kept);
                albumsDirty_ = true;
                albumCount_.store(0);
                trackCount_.store(tracks_.size());
                lastScanMs_.store(durMs);
                lastScanProbed_.store(0);
                lastScanCached_.store(0);
                text = indexTextLocked();
            }
            writeFile(indexPath(), text);          // 锁外写盘
            {
                std::lock_guard<std::mutex> lk(msgMtx_);
                std::string m = "完成：0 首";
                if (!badRoots.empty()) {
                    m += "（" + std::to_string(badRoots.size()) + " 个根目录无法访问，已保留其下 " +
                         std::to_string(trackCount_.load()) + " 条旧记录：" + badRoots[0];
                    if (badRoots.size() > 1) m += " 等";
                    m += "）";
                }
                scanMsg_ = m;
            }
            scanning_.store(false);
            return;
        }

        // =====================================================================
        // CUE 认领判定（在 ffprobe 探测之前做：解析只是读几个 KB 的文本，很便宜）
        //
        // 认领条件（缺一条就**退回整轨**，绝不因为 cue 写错而丢歌）：
        //   ① cue 能解析出 >=1 条「AUDIO 且带 INDEX 01」的轨；
        //   ② 这些轨的 FILE 指向的音频**真实存在**（按大小写归一 key 与遍历结果对齐，路径取磁盘真名）；
        //   ③ 该文件没被别的 cue 先认领（两张 cue 指同一整轨时先到先得，后者退回整轨）；
        //   ④ 探测成功（拿得到采样率）—— 见下面的 finalize：否则整轨会变成 N 首「偏移未知」的
        //      幽灵曲目（每首都从文件头开始播），比整轨更难用。
        // 认领成功的文件不再单独入库，改由 lib_cue_expand 展开成 N 条虚拟曲目。
        // =====================================================================
        struct CueJob {
            std::string cuePath;
            rta::meta::CueSheet sh;
            std::map<std::string, LibTrack> slots;   // key = lib_path_key(FILE 路径)，value.path = 磁盘真名
        };
        std::vector<CueJob> cueJobs;
        std::unordered_map<std::string, char> cueCand;   // 候选认领（决定哪些文件必须**重探**）
        if (!cueWalked.empty()) {
            std::unordered_map<std::string, std::string> walkedByKey;   // 归一 key → 磁盘真名
            walkedByKey.reserve(walked.size() * 2 + 8);
            for (const auto& w : walked) walkedByKey.emplace(lib_path_key(w), w);
            std::sort(cueWalked.begin(), cueWalked.end());   // 稳定：抢同一整轨时结果每次一致
            for (const auto& cf : cueWalked) {
                if (cancel_.load()) break;
                CueJob job;
                job.cuePath = cf;
                job.sh = rta::meta::cue_parse_file(cf);
                if (job.sh.tracks.empty()) continue;         // 解析失败/全坏轨 → 整轨照旧（不丢歌）
                for (const auto& ct : job.sh.tracks) {
                    if (!ct.audio || ct.index1 < 0) continue;
                    const std::string k = lib_path_key(ct.file);
                    if (cueCand.count(k) || job.slots.count(k)) continue;
                    auto wit = walkedByKey.find(k);
                    const std::string real = (wit != walkedByKey.end()) ? wit->second : ct.file;
                    // 不在遍历结果里时仍要 stat：整轨可能就在库根之外（cue 在库内）
                    if (wit == walkedByKey.end() && !lib_cue_audio_exists(real)) continue;
                    LibTrack slot;
                    slot.path = real;
                    job.slots.emplace(k, std::move(slot));
                    cueCand[k] = 1;
                }
                if (!job.slots.empty()) cueJobs.push_back(std::move(job));
            }
        }
        // 探测结果回填表：认领文件在并行池里**强制重探**（不能复用 mtime+size 缓存 ——
        // 上次 ffprobe 失败时 duration=0，复用会让末轨永远 0 秒），探到后在这里共享给 cue 展开。
        std::mutex cueProbeMtx;
        std::map<std::string, LibTrack> cueProbeMap;

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

        // 目录结构判据的按目录 memo（见 lib_dir_album_for / LibDirAlbumMemo 的注释）
        LibDirAlbumMemo dirMemo;
        std::chrono::steady_clock::time_point lastMsg = std::chrono::steady_clock::now();

        for (int i = 0; i < n; ++i) {
            if (cancel_.load()) break;
            const bool submitted = pool->submit([this, i, n, &walked, &oldByPath, &old, forceFull, &ffprobe,
                                                 &fresh, &completed, &probedCount, &cachedCount, &dirMemo, &lastMsg,
                                                 &cueCand, &cueProbeMtx, &cueProbeMap]() {
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
                const std::string pkey = lib_path_key(p);
                const bool cueOwned = (cueCand.count(pkey) != 0);
                auto oit = oldByPath.find(p);
                // ★ cue 认领的文件**每次扫描都重探**（cueOwned）：末轨长度要用文件总时长，而
                //   mtime+size 只证明"文件没变"，不证明"上次探测成功"。
                // ★ 缓存候选里排除 cue 虚拟曲目（isCueTrack）：否则删掉 .cue 后，整轨会命中
                //   旧索引里那条"末轨虚拟曲目"的记录，结果库里只剩 1 首且歌名是 cue 的第 N 轨。
                // ★ 2026-10-25（索引一致性审查）：**不要**给"探测失败的记录"加特殊重探逻辑。
                //   我先加的 "sampleRate==0 就重探" 与 "仍失败时搬回旧标签列" 两条都被既有契约否决，
                //   原因写在这里免得下一个人再踩（判据：tests/test_library.cpp 与 tests/test_cue.cpp）：
                //   ① "sr==0 就重探"：sr==0 并不只来自探测失败 —— WAV 的 data 段声明长度与实际不符时
                //      ffprobe 报不出采样率，那是**探测成功但真不知道**；老索引夹具正是这条，
                //      而契约要求"增量扫描 0 次 ffprobe"（缓存复用是性能前提，3000 首全重探要几十秒）。
                //   ② "仍失败时搬回旧标签列"：会绕过同一处缓存分支里的"老索引自愈"
                //      （album 空/disc 0 就地补），老索引在增量里永远修不好。
                //   结论：探测失败的自愈留在**源头**（探测失败时 sr/bits/ch/dur 如实写 0，
                //   不写"看起来有效"的值）；索引侧保证的是"列出的是什么就是什么"。
                //   索引一致性真正修掉的三条见 lib_probe [3][4][5] 的说明。
                // 2026-10-26：旧版本索引（v4 及更早）里的标签是旧解析器写的，必须重探；
                // 现代价只有一次（655 首约 1 s），换来标签修复与专辑归并真正生效。
                const bool retagOld = idxIsV4;   // 只有 v4 需要"留结构、重探标签"；v5 直接复用，v3 已整份丢弃
                if (!forceFull && !retagOld && oit != oldByPath.end()) {
                    const LibTrack& o = old[oit->second];
                    if (!o.isCueTrack() && o.mtime == mt && o.size == sz) {
                        // 复用旧记录，但仍要过一遍目录回填：老代码写的索引版本头也是 v3，但 album 列空、
                        // disc 列恒 0；这里就地补上 → 增量扫描也能自愈，不必靠升 #libIndexV 把用户的整份索引作废。
                        // 已有真实标签值的记录不会被改（lib_fill_album_disc_from_dir 只填空字段）。
                        LibTrack t2 = o;
                        lib_fill_album_disc_from_dir(t2, &dirMemo);
                        fresh[(size_t)i] = std::move(t2);
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
                // ★ 目录结构判据回填（ffprobe 探测之后、落盘之前）：真实标签优先，只填空字段。
                //   与 albumVecLocked 的读时纠正用同一份判据（entity_find_album_dir_for_track），
                //   所以专辑键/曲目顺序都不变，只是落盘数据与 /api/lib/album 的 d 字段变成如实值。
                lib_fill_album_disc_from_dir(t, &dirMemo);
                if (cueOwned) {   // 把这次强制重探的结果交给 cue 展开（见下方 finalize）
                    std::lock_guard<std::mutex> lk(cueProbeMtx);
                    cueProbeMap[pkey] = t;
                }
                fresh[(size_t)i] = std::move(t);
                probedCount.fetch_add(1, std::memory_order_relaxed);
                const int c = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                scanned_.store(c);
                // 进度文案：改为**限频**（每 100ms 一次）而不是每 8 首一次 —— 3000 首时旧实现要
                // 抢 375 次全局锁，每次都会把 /api/lib/stats 顶到后面（铁律：状态查询不该被进度文案堵住）。
                if (c == n || std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - lastMsg).count() >= 100.0) {
                    lastMsg = std::chrono::steady_clock::now();
                    std::lock_guard<std::mutex> lk(msgMtx_);
                    scanMsg_ = "读取标签 " + std::to_string(c) + "/" + std::to_string(n);
                }
            });
            if (!submitted) break;
            if ((i & 0x3F) == 0) std::this_thread::yield();   // 分片让出（提交侧）
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

        // ---- CUE 展开（整轨 → N 条虚拟曲目）----
        // 认领成功的整轨不再单独入库；任何一份 cue 失败都只是"它自己的整轨照旧"，不牵连别的。
        std::vector<LibTrack> cueTracks;
        std::unordered_map<std::string, char> cueClaimed;   // 最终认领（整轨从 fresh 里摘掉）
        int cueOk = 0, cueLost = 0;
        for (auto& job : cueJobs) {
            std::map<std::string, LibTrack> probes;
            for (auto& kv : job.slots) {
                LibTrack pr;
                auto pit = cueProbeMap.find(kv.first);
                if (pit != cueProbeMap.end()) pr = pit->second;
                else {
                    // 没被遍历到（整轨在库根之外）或 n==0：这里补一次探测
                    pr = lib_probe_one(kv.second.path, ffprobe);
                    if (pr.path.empty()) pr.path = kv.second.path;
                    std::error_code e2;
                    const fsx::path wp = p8(kv.second.path);
                    auto ft = fsx::last_write_time(wp, e2);
                    if (!e2) pr.mtime = (long long)ft.time_since_epoch().count();
                    auto fsz = fsx::file_size(wp, e2);
                    if (!e2) pr.size = (long long)fsz;
                }
                if (pr.sampleRate <= 0) continue;    // 探测失败 → 该文件退回整轨（不产生幽灵曲目）
                probes.emplace(kv.first, std::move(pr));
            }
            if (probes.empty()) { ++cueLost; continue; }
            std::vector<LibTrack> vt = lib_cue_expand(job.sh, probes);
            if (vt.empty()) { ++cueLost; continue; }
            for (const auto& kv : probes) cueClaimed[kv.first] = 1;
            for (auto& t : vt) {
                lib_fill_album_disc_from_dir(t, &dirMemo);   // 与物理曲目同一份判据（只填空字段）
                cueTracks.push_back(std::move(t));
            }
            ++cueOk;
        }
        if (!cueTracks.empty()) {
            std::vector<LibTrack> kept;
            kept.reserve(fresh.size() + cueTracks.size());
            for (auto& t : fresh) {
                if (t.path.empty()) continue;                          // 未探测的槽（cue 认领/取消）
                if (cueClaimed.count(lib_path_key(t.path))) continue;   // 认领的整轨不再单独出现
                kept.push_back(std::move(t));
            }
            for (auto& t : cueTracks) kept.push_back(std::move(t));
            fresh.swap(kept);
        }

        const auto tEnd = std::chrono::steady_clock::now();
        const double durMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

        // 打不开的根：保留下面的老记录（不因为它们"这次没被遍历到"就被删掉）
        // ★ 2026-10-25（索引一致性审查）：坏根恢复时**必须**再问一次"文件还在不在"。
        //   下面的 prune 只对"扫描开始那一刻"成立；两次扫描之间文件可以被删/移走，而坏根分支
        //   会把整份旧记录原样搬回来 —— 「删掉文件 + 根目录同时不可读」于是让已删记录**复活**。
        //   判据：build-dbg-net 的 lib_probe [3]（修前 FAIL=GHOST RESURRECTED，修后 PASS）。
        int badKeepSkipped = 0;   // 坏根恢复时因"文件已不存在"而被拒的条数（只用于文案）
        if (!badRoots.empty()) {
            const std::unordered_map<std::string, size_t> freshByPath = [&] {
                std::unordered_map<std::string, size_t> m;
                m.reserve(fresh.size() * 2);
                for (size_t k = 0; k < fresh.size(); ++k) if (!fresh[k].path.empty()) m[fresh[k].path] = k;
                return m;
            }();
            for (auto& o : old) {
                if (freshByPath.count(o.path)) continue;
                // 已被本次 cue 认领的整轨：它的曲目已经由 lib_cue_expand 重新展开，
                // 再从旧索引里搬一条（整轨或旧虚拟曲目）回来就是重复。
                if (cueClaimed.count(lib_path_key(o.path))) continue;
                for (auto& br : badRoots)
                    if (lib_path_under(o.path, br)) {
                        if (lib_file_exists(o.path)) fresh.push_back(o);
                        else ++badKeepSkipped;
                        break;
                    }
            }
        }

        const size_t nt = fresh.size();
        std::string text;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            tracks_.swap(fresh);
            albumsDirty_ = true;
            lastScanMs_.store(durMs);
            lastScanProbed_.store(probedCount.load());
            lastScanCached_.store(cachedCount.load());
            trackCount_.store(nt);
            // ★ 扫描结束就把专辑整理**做掉**（261025-01 一.8「导入几千首要等几分钟才开始播放」）：
            //   旧实现把这次整理推给"第一个 /api/lib/albums 请求"，而 HTTP 是单线程 ⟹ 用户
            //   点第一个标签时整站被堵住。放在扫描线程里做，交互路径就再也没有这一笔账。
            albumVecLocked();
            text = indexTextLocked();
        }
        writeFile(indexPath(), text);          // 锁外写盘（见 indexTextLocked 注释）
        {
            std::lock_guard<std::mutex> lk(msgMtx_);
            std::string m = "完成：" + std::to_string(nt) + " 首 (" + std::to_string((int)(durMs + 0.5)) + " ms)";
            if (!badRoots.empty()) {
                m += "；" + std::to_string(badRoots.size()) + " 个根目录无法访问（已保留其下旧记录";
                if (badKeepSkipped > 0) m += "，跳过 " + std::to_string(badKeepSkipped) + " 条已不存在的文件";
                m += "）";
            }
            const int gone = removedMissing_.load();
            if (gone > 0) m += "；已剔除 " + std::to_string(gone) + " 条不存在的文件";
            if (!cueWalked.empty())
                m += "；CUE 展开 " + std::to_string(cueOk) + " 张/" + std::to_string(cueTracks.size()) +
                     " 首" + (cueLost > 0 ? "（" + std::to_string(cueLost) + " 张退回整轨）" : "");
            scanMsg_ = m;
        }
        scanning_.store(false);
        }
        catch (const std::exception& e) {
            // 任何异常都不许把进程带走：降级成一条错误文案，索引维持扫描前的样子。
            { std::lock_guard<std::mutex> lk(msgMtx_); scanMsg_ = std::string("扫描失败：") + e.what(); }
            scanning_.store(false);
        }
        catch (...) {
            { std::lock_guard<std::mutex> lk(msgMtx_); scanMsg_ = "扫描失败：未知异常"; }
            scanning_.store(false);
        }
    });
}

// ---------------------------------------------------------------------------
// 删除 / 清理（261025-01 一.3）
//
// 用户原话：「扫描是积累记录，不能消除，删除唯一方法是按照原路径重扫」——即删掉的文件
// 永远留在索引里（因为索引 = 上次扫描的产物，而"删掉的文件"不在遍历结果里时，
// 只有在**整份替换**的那一刻才会消失；一旦那次扫描被取消/根目录打不开，旧行就永久留着）。
// 这里给三条明确出口：
//   * 扫描开始前自动 prune（文件已不存在的记录直接剔除，见 startScan）
//   * POST /api/lib/prune             —— 手动剔除不存在的文件
//   * POST /api/lib/remove?path=<dir> —— 按目录移除（含子目录）
// ---------------------------------------------------------------------------
inline bool lib_file_exists(const std::string& p) {
    std::error_code ec;
    try { return fsx::exists(p8(p), ec); } catch (const std::exception&) { return false; }
}

// 调用方须持有 mtx_。返回被剔除的条数（不落盘 —— 由调用方决定时机）
inline LibCleanResult Library::pruneMissingLocked() {
    std::vector<LibTrack> kept;
    kept.reserve(tracks_.size());
    int removed = 0;
    for (auto& t : tracks_) {
        if (lib_file_exists(t.path)) kept.push_back(std::move(t));
        else ++removed;
    }
    if (removed > 0) {
        tracks_.swap(kept);
        albumsDirty_ = true;
        albumCount_.store(0);
        trackCount_.store(tracks_.size());
    }
    LibCleanResult r; r.removed = removed; r.tracks = tracks_.size();
    return r;
}

inline LibCleanResult Library::pruneMissing() {
    LibCleanResult r;
    std::string text;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        r = pruneMissingLocked();
        removedMissing_.store(r.removed);
        if (r.removed == 0) return r;                   // 没变化就不必重写索引
        text = indexTextLocked();
    }
    writeFile(indexPath(), text);
    return r;
}

inline LibCleanResult Library::removeUnder(const std::string& dir) {
    LibCleanResult r;
    if (dir.empty() || !lib_valid_utf8(dir)) return r;
    std::string text;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<LibTrack> kept;
        kept.reserve(tracks_.size());
        for (auto& t : tracks_) {
            if (lib_path_under(t.path, dir)) { ++r.removed; continue; }
            kept.push_back(std::move(t));
        }
        if (r.removed > 0) {
            tracks_.swap(kept);
            albumsDirty_ = true;
            albumCount_.store(0);
            trackCount_.store(tracks_.size());
            text = indexTextLocked();
        }
        r.tracks = tracks_.size();
    }
    if (r.removed > 0) writeFile(indexPath(), text);
    return r;
}

inline bool Library::removePath(const std::string& path) {
    if (path.empty()) return false;
    std::string text;
    bool hit = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<LibTrack> kept;
        kept.reserve(tracks_.size());
        for (auto& t : tracks_) {
            if (!hit && lib_path_under(t.path, path) && t.path.size() == path.size()) { hit = true; continue; }
            kept.push_back(std::move(t));
        }
        if (!hit) return false;
        tracks_.swap(kept);
        albumsDirty_ = true;
        albumCount_.store(0);
        trackCount_.store(tracks_.size());
        text = indexTextLocked();
    }
    writeFile(indexPath(), text);
    return true;
}

// 真实批量入口：从音乐库 LibTrack 扫描结果构建实体图
inline rta::meta::EntityGraph entity_graph_from_library_tracks(
    const std::vector<LibTrack>& tracks, long long ts = 0) {
    std::vector<rta::meta::MetaSeed> seeds;
    seeds.reserve(tracks.size());
    for (const auto& t : tracks) {
        // ★ 2026-10-25（CUE × 实体图）：Track 实体的键是**文件路径**，而 CUE 虚拟曲目的 path
        //   与整轨其余曲目完全相同 —— 直接按 path 建种子会让一张 12 轨的专辑只留下 1 个 Track
        //   实体（其余 11 首的「Track→Recording」边全挂在同一个实体上，按 (from,rel,to,role)
        //   去重后还会互相覆盖）。
        //   唯一性由 keyPath（path + 区间）承担，展示名仍是原路径（用户视角就是"哪个文件哪一段"）。
        //   非 cue 曲目的 keyPath == path ⟹ Track id 与既有实现**逐位相同**（老缓存/老链接不失效）。
        std::string keyPath = t.path;
        if (t.isCueTrack()) {
            keyPath += "#cue:" + std::to_string(t.cueNo) + "@" + std::to_string(t.cueOff) +
                       "+" + std::to_string(t.cueLen);
        }
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
        rta::meta::MetaSeed s = rta::meta::entity_seed_from_track_path(t.path, tags);
        if (keyPath != t.path) s.trackPath = keyPath;   // 仅 CUE 虚拟曲目：Track 键加区间
        seeds.push_back(std::move(s));
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
