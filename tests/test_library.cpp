// test_library.cpp — 音乐库索引：解析 / 专辑分组 / 搜索 / 库根目录
//
// 判据说明（铁律 3）：这里验证的是「索引文件 -> 内存模型 -> 查询结果」这条链路，
// 用构造好的 TSV 直接喂进去，不依赖 ffprobe，也不依赖磁盘上有真实音频。
// 真实扫描的端到端验证放在 verify.ps1 的库扫描检查里（会用真文件跑）。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "net/library.h"
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <direct.h>

namespace fs = std::filesystem;
using rta::net::Library;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}


// ---------------------------------------------------------------------------
// 真库封面审计（2026-10-27）：P1/P3 判据的取证工具，**严格只读**（只读索引/stat/抽内嵌图，
// 不写 MUSIC、不写索引、不开设备）。为什么不另建 tools/ 下的小程序：本轮 CMakeLists 禁改，
// 不能新增 target ⟹ 放在已有 test_library 里，用环境变量开启（不设 = 完全不运行，ctest 不受影响）。
//
// 环境变量：
//   RLPLAYER_COVER_AUDIT_LIB  = 含 rlplayer-index.tsv 的库目录（**复制**用户库目录，别指向用户正在用的那份）
//   RLPLAYER_COVER_AUDIT_NET  = covercache 目录（可选；把"在线抓到的网图"计入覆盖率）
//   RLPLAYER_COVER_AUDIT_DIR  = 只详列这个目录前缀下的专辑（可选）
//   RLPLAYER_COVER_AUDIT_DEEP = 1 → 对每张专辑都真取一次字节（慢；用来验"src=embed 就真能取到"）
// 输出一律以 AUDIT 开头，便于抓取。
// ---------------------------------------------------------------------------
static void coverAudit(const std::string& libDir) {
    using rta::net::LibAlbum;
    const std::string netDir = rta::net::lib_env_utf8(L"RLPLAYER_COVER_AUDIT_NET", "RLPLAYER_COVER_AUDIT_NET");
    const std::string onlyDir = rta::net::lib_env_utf8(L"RLPLAYER_COVER_AUDIT_DIR", "RLPLAYER_COVER_AUDIT_DIR");
    const bool deep = !rta::net::lib_env_utf8(L"RLPLAYER_COVER_AUDIT_DEEP", "RLPLAYER_COVER_AUDIT_DEEP").empty();

    _putenv_s("RLPLAYER_LIBRARY_DIR", libDir.c_str());
    _putenv_s("RLPLAYER_PORT", "");
    Library& lib = Library::instance();
    lib.load();
    const auto albums = lib.albums();
    std::printf("AUDIT lib=%s tracks=%zu albums=%zu deep=%d\n", libDir.c_str(), lib.trackCount(), albums.size(), deep ? 1 : 0);

    auto dirOfAlbum = [&](const LibAlbum& a) -> std::string {
        if (a.idx.empty()) return std::string();
        rta::net::LibTrack t;
        if (!lib.trackAt(a.idx[0], t)) return std::string();
        return rta::net::lib_dir_album_key(rta::net::u8s(rta::net::p8(t.path).parent_path().u8string()));
    };
    auto netHas = [&](const std::string& key) {
        if (netDir.empty()) return false;
        std::error_code e;
        const auto p = std::filesystem::u8path(netDir + "\\" + key + ".jpg");
        return std::filesystem::exists(p, e) && std::filesystem::file_size(p, e) > 1024;
    };
    auto oldBytes = [&](const LibAlbum& a) {          // 模拟修复前的取图路径（get_cover：同目录文件优先）
        std::vector<std::string> withCv, without;
        for (size_t ix : a.idx) {
            rta::net::LibTrack t;
            if (!lib.trackAt(ix, t)) continue;
            if (t.hasCover) { if (withCv.size() < 3) withCv.push_back(t.path); }
            else if (without.size() < 3) without.push_back(t.path);
        }
        withCv.insert(withCv.end(), without.begin(), without.end());
        for (const auto& p : withCv) {
            std::string m;
            std::vector<unsigned char> b = rta::audio::get_cover(p, m);
            if (b.size() >= 1024) return b;
        }
        return std::vector<unsigned char>();
    };

    std::map<std::string, std::vector<std::string>> byDir;      // 目录键 → 专辑名
    for (const auto& a : albums) byDir[dirOfAlbum(a)].push_back(a.name);

    int nEmbed = 0, nDirFile = 0, nNone = 0, nNet = 0, nNewCov = 0, nOldCov = 0;
    int nMultiDirs = 0, nMultiAlbums = 0, nMultiTracks = 0, nMultiNoCover = 0;
    int nEmbedFail = 0, nDirFail = 0;
    std::map<std::string, std::vector<std::string>> dupAfter, dupBefore;   // 目录+字节哈希 → 专辑名
    std::ostringstream detail;
    for (const auto& a : albums) {
        const std::string dir = dirOfAlbum(a);
        const bool multi = a.dirAlbums > 1;
        if (a.coverSrc == rta::net::kCoverEmbedded) ++nEmbed;
        else if (a.coverSrc == rta::net::kCoverDirFile) ++nDirFile;
        else ++nNone;
        const bool net = netHas(a.key);
        if (net) ++nNet;
        if (a.coverSrc != rta::net::kCoverNone || net) ++nNewCov;
        if (a.hasCover) ++nOldCov;                  // 老口径 = 任一曲目 cv=1（= LibAlbum::hasCover 旧语义）
        if (multi) { ++nMultiAlbums; nMultiTracks += a.nTracks; if (a.coverSrc == rta::net::kCoverNone && !net) ++nMultiNoCover; }

        std::vector<unsigned char> nb; std::string nm;
        if (multi || deep) {
            if (lib.albumCover(a.key, nb, nm)) {
                if (multi) dupAfter[dir + "\x1f" + rta::net::fnv_hex(std::string(nb.begin(), nb.end()))].push_back(a.name);
            } else if (a.coverSrc == rta::net::kCoverEmbedded) ++nEmbedFail;
            else if (a.coverSrc == rta::net::kCoverDirFile)   ++nDirFail;
        }
        if (multi) {
            const auto ob = oldBytes(a);
            if (ob.size() >= 1024)
                dupBefore[dir + "\x1f" + rta::net::fnv_hex(std::string(ob.begin(), ob.end()))].push_back(a.name);
        }
        const bool show = onlyDir.empty() ? multi : (dir.compare(0, onlyDir.size(), onlyDir) == 0);
        if (show) {
            detail << "AUDIT ALBUM dir=" << dir << " name=" << a.name << " n=" << a.nTracks
                   << " dirAlbums=" << a.dirAlbums
                   << " src=" << (a.coverSrc == rta::net::kCoverEmbedded ? "embed"
                                  : a.coverSrc == rta::net::kCoverDirFile ? "dir" : "none")
                   << " bytes=" << nb.size();
            if (!nb.empty()) detail << " hash=" << rta::net::fnv_hex(std::string(nb.begin(), nb.end())).substr(0, 12);
            if (multi) {
                const auto ob = oldBytes(a);
                detail << " oldBytes=" << ob.size();
                if (ob.size() >= 1024) detail << " oldHash=" << rta::net::fnv_hex(std::string(ob.begin(), ob.end())).substr(0, 12);
            }
            detail << "\n";
        }
    }
    for (const auto& kv : byDir) if (kv.second.size() > 1) ++nMultiDirs;

    int dupAfterGroups = 0, dupBeforeGroups = 0;
    for (const auto& kv : dupAfter) if (kv.second.size() > 1) ++dupAfterGroups;
    for (const auto& kv : dupBefore) if (kv.second.size() > 1) ++dupBeforeGroups;

    std::printf("AUDIT dirs=%zu multi-dirs=%d multi-albums=%d multi-tracks=%d multi-albums-would-be-nocover=%d\n",
                byDir.size(), nMultiDirs, nMultiAlbums, nMultiTracks, nMultiNoCover);
    std::printf("AUDIT NEW src[embed=%d dir=%d none=%d] net=%d covered=%d/%zu\n",
                nEmbed, nDirFile, nNone, nNet, nNewCov, albums.size());
    std::printf("AUDIT OLD(emulated: any cv=1) covered=%d/%zu net=%d covered+net=%d/%zu\n",
                nOldCov, albums.size(), nNet, nOldCov + nNet, albums.size());
    std::printf("AUDIT DUP same-dir-shared-image AFTER(groups>1)=%d BEFORE(emulated)=%d\n", dupAfterGroups, dupBeforeGroups);
    std::printf("AUDIT EXTRACT-FAIL embed=%d dir=%d\n", nEmbedFail, nDirFail);
    std::printf("%s", detail.str().c_str());
    if (!dupBefore.empty()) {
        int shown = 0;
        for (const auto& kv : dupBefore) {
            if (kv.second.size() < 2 || shown >= 20) continue;
            ++shown;
            std::printf("AUDIT SHARED-BEFORE dir=%s n=%d :", kv.first.substr(0, kv.first.find('\x1f')).c_str(), (int)kv.second.size());
            for (const auto& n : kv.second) std::printf(" %s |", n.c_str());
            std::printf("\n");
        }
    }
    // 前 20 个多专辑目录
    int shownDirs = 0;
    for (const auto& kv : byDir) {
        if (kv.second.size() < 2 || shownDirs >= 20) continue;
        ++shownDirs;
        std::printf("AUDIT MULTIDIR %s albums=%d :", kv.first.c_str(), (int)kv.second.size());
        for (const auto& n : kv.second) std::printf(" %s |", n.c_str());
        std::printf("\n");
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== 音乐库索引验证 ==\n");
    // 真库审计模式（环境变量开启；ctest 不设 ⟹ 不参与门禁，见 coverAudit 的注释）
    {
        const std::string auditLib = rta::net::lib_env_utf8(L"RLPLAYER_COVER_AUDIT_LIB", "RLPLAYER_COVER_AUDIT_LIB");
        if (!auditLib.empty()) { coverAudit(auditLib); std::printf("\n审计完成\n"); return 0; }
    }

    const fs::path tmp = fs::temp_directory_path() / "rta_lib_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::string oldCwd = fs::current_path().string();
    fs::current_path(tmp, ec);
    // 2026-10-23：库配置/索引默认落 exe 目录（修「升级后曲库变空」）。测试必须显式改到临时目录，
    // 否则会读写真实 exe 目录里的 rlplayer-library.conf / index.tsv，污染用户库并让断言全错。
    _putenv_s("RLPLAYER_LIBRARY_DIR", tmp.string().c_str());

    // 构造索引：2 张专辑 + 1 个没有专辑标签的目录级条目
    // 字段顺序必须与 library.h 的 load() 一致
    {
        std::ofstream f("rlplayer-index.tsv", std::ios::binary);
        // 2026-10-23：索引多了格式版本头（Library 会校验，不匹配就整份丢弃重建）。
        // 测试夹具必须与真实 saveIndexLocked() 的输出格式保持一致，否则加载后为空。
        // 2026-10-25：v4 起末尾追加 cue / cueN / cueOff / cueLen 四列（CUE 虚拟分轨）；
        // **前 16 列逐字不变**，所以老夹具只要补上 4 个空列就能继续用。
        f << "#libIndexV\t4\n";
        f << "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre"
             "\tcue\tcueN\tcueOff\tcueLen\n";
        auto row = [&](const std::string& path, const std::string& alb, const std::string& aa,
                       const std::string& art, const std::string& ttl, int trk, int disc,
                       int yr, int sr, int bits, int ch, double dur, int cov) {
            f << path << "\t111\t222\t" << alb << "\t" << aa << "\t" << art << "\t" << ttl
              << "\t" << trk << "\t" << disc << "\t" << yr << "\t" << sr << "\t" << bits
              << "\t" << ch << "\t" << dur << "\t" << cov << "\tRock" << "\t\t0\t0\t0\n";
        };
        row("D:\\Music\\Beatles\\Abbey Road\\02.flac", "Abbey Road", "The Beatles", "The Beatles", "Something", 2, 1, 1969, 44100, 16, 2, 182.0, 1);
        row("D:\\Music\\Beatles\\Abbey Road\\01.flac", "Abbey Road", "The Beatles", "The Beatles", "Come Together", 1, 1, 1969, 44100, 16, 2, 259.0, 1);
        // 同专辑名、不同专辑艺术家 -> 必须分成两张，不能合并
        row("D:\\Music\\Other\\Abbey Road\\01.flac", "Abbey Road", "Someone Else", "Someone Else", "Track A", 1, 1, 2001, 96000, 24, 2, 100.0, 0);
        // 多碟：第 2 碟应排在第 1 碟之后
        row("D:\\Music\\Big\\Live\\d2t1.flac", "Live Box", "Big Band", "Big Band", "Encore", 1, 2, 1990, 48000, 16, 2, 300.0, 0);
        row("D:\\Music\\Big\\Live\\d1t1.flac", "Live Box", "Big Band", "Big Band", "Overture", 1, 1, 1990, 48000, 16, 2, 200.0, 0);
        // 无专辑标签 -> 用父目录名当专辑名
        row("D:\\Music\\Loose\\solo.wav", "", "", "无名氏", "即兴", 0, 0, 0, 44100, 16, 1, 60.0, 0);
    }

    Library& lib = Library::instance();
    lib.load();

    ck(lib.trackCount() == 6, "曲目数 = 6", "实际 " + std::to_string(lib.trackCount()));
    auto albums = lib.albums();
    ck(albums.size() == 4, "专辑数 = 4（同名不同艺术家不合并）", "实际 " + std::to_string(albums.size()));

    // 找 Abbey Road / The Beatles
    rta::net::LibAlbum ab;
    bool found = false;
    std::string abKey;
    for (auto& a : albums) if (a.name == "Abbey Road" && a.artist == "The Beatles") { ab = a; found = true; abKey = a.key; }
    ck(found, "能找到 Abbey Road / The Beatles");
    ck(ab.nTracks == 2, "该专辑 2 首", "实际 " + std::to_string(ab.nTracks));
    if (found && ab.idx.size() == 2) {
        rta::net::LibTrack t0, t1;
        lib.trackAt(ab.idx[0], t0);
        lib.trackAt(ab.idx[1], t1);
        ck(t0.trackNo == 1 && t1.trackNo == 2, "专辑内按音轨号排序（01 在 02 前）",
           "首曲 trackNo=" + std::to_string(t0.trackNo));
        ck(t0.title == "Come Together", "曲名解析正确", t0.title);
        ck(t0.sampleRate == 44100 && t0.bits == 16 && t0.channels == 2, "流参数解析正确");
    }
    ck(ab.hasCover, "专辑封面标记透传（02/01 有封面）");

    // 多碟排序
    for (auto& a : albums) if (a.name == "Live Box") {
        rta::net::LibTrack x, y;
        lib.trackAt(a.idx[0], x); lib.trackAt(a.idx[1], y);
        ck(x.discNo == 1 && y.discNo == 2, "多碟按碟号排序", "首曲 disc=" + std::to_string(x.discNo));
    }

    // 无专辑标签的兜底
    bool loose = false;
    for (auto& a : albums) if (a.name == "Loose") loose = true;
    ck(loose, "无专辑标签时用父目录名当专辑名");

    // 搜索
    ck(lib.search("abbey").size() == 3, "搜索 abbey 命中 3 首", "实际 " + std::to_string(lib.search("abbey").size()));
    ck(lib.search("OVERTURE").size() == 1, "搜索不区分大小写");
    ck(lib.search("即兴").size() == 1, "搜索支持 UTF-8 中文");
    ck(lib.search("不存在的词").empty(), "搜索无结果时返回空");
    ck(lib.search("", 100).size() == 6, "空串返回全部曲目");

    // 专辑键：稳定的十六进制
    ck(abKey.size() == 16 && abKey.find_first_not_of("0123456789abcdef") == std::string::npos,
       "专辑键是 16 位十六进制", abKey);
    ck(rta::net::fnv_hex("a") == rta::net::fnv_hex("a"), "哈希稳定");

    // 不存在的专辑
    rta::net::LibAlbum nope;
    ck(!lib.album("0000000000000000", nope), "查询不存在的专辑返回 false（除非真撞上哈希）");

    // 库根目录
    const std::string rootDir = (tmp / "music").string();
    fs::create_directories(rootDir, ec);
    ck(lib.addRoot(rootDir), "可添加库根目录");
    bool has = false;
    for (auto& r : lib.roots()) if (r == rootDir) has = true;
    ck(has, "根目录已记录");
    ck(!lib.addRoot("Z:\\不存在的目录"), "添加不存在的目录被拒绝");
    ck(lib.removeRoot(rootDir), "可移除库根目录");
    ck(fs::exists("rlplayer-library.conf"), "根目录落盘为 rlplayer-library.conf");


    // =======================================================================
    // [2] 目录结构判据 -> 扫描入库回填（2026-10-25）
    //
    // 判据（铁律 3）：这一节要证明的不是「接口看着对」，而是**落盘索引**里 album 列有值、
    // disc 列是 1/1/2/2，且内存模型里的 discNo 就是落盘值 —— 即扫描入库处真的把
    // entity_find_album_dir_for_track 的结果写进了 LibTrack，而不是像以前那样只在读取时纠正。
    // 夹具是「只有 4 字节 'fLaC' 头的假 .flac」→ ffprobe 必然失败 → album 必然为空 → 走回填路径；
    // 「真实标签优先」用 lib_fill_album_disc_from_dir 直接喂已带标签的 LibTrack（单元级）。
    // 真 ffprobe + 真标签 + HTTP 接口的端到端证据见 tests/verify_libtag_dirfill.ps1。
    // =======================================================================
    auto readAll = [](const fs::path& p) -> std::string {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    auto splitTsv = [](const std::string& line) {
        std::vector<std::string> f; size_t s = 0;
        for (size_t k = 0; k <= line.size(); ++k)
            if (k == line.size() || line[k] == '\t') { f.push_back(line.substr(s, k - s)); s = k + 1; }
        return f;
    };

    const fs::path tmp2 = fs::temp_directory_path() / "rta_lib_dirfill";
    fs::remove_all(tmp2, ec);
    fs::create_directories(tmp2, ec);
    _putenv_s("RLPLAYER_LIBRARY_DIR", tmp2.string().c_str());
    _putenv_s("RLPLAYER_PORT", "");       // 默认文件名（不加 .p<port> 后缀），与 confLeaf 的规则一致

    const fs::path music = tmp2 / "Music";
    auto mkAudio = [&](const char* rel) {
        const fs::path p = music / rel;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary); f << "fLaC";
    };
    mkAudio("ArtistA/BoxSetAlbum/CD1/01-Track1.flac");
    mkAudio("ArtistA/BoxSetAlbum/CD1/02-Track2.flac");
    mkAudio("ArtistA/BoxSetAlbum/CD2/01-Track3.flac");
    mkAudio("ArtistA/BoxSetAlbum/CD2/02-Track4.flac");
    mkAudio("Compilations/SummerHits/01-SongA.flac");
    mkAudio("Compilations/SummerHits/02-SongB.flac");
    mkAudio("ArtistB/SoloAlbum/01-Intro.flac");
    mkAudio("ArtistC/DirNameDifferent/01-Only.flac");
    {   // BoxSetAlbum/misc 只有非音频文件：不得进库，也不得把 misc 当专辑
        std::ofstream f(music / "ArtistA/BoxSetAlbum/misc/booklet.txt", std::ios::binary); f << "booklet\n";
    }
    {
        std::ofstream f(tmp2 / "rlplayer-library.conf", std::ios::binary);
        f << "# test roots\n" << music.string() << "\n";
    }

    lib.load();                            // 无索引 → 曲目清空，根目录来自 conf
    ck(lib.roots().size() == 1, "扫描夹具：库根 = 1", std::to_string(lib.roots().size()));
    ck(lib.trackCount() == 0, "扫描夹具：起点曲目数 0");
    lib.startScan(true);                   // 全量扫描（真实入口 /api/lib/scan?full=1 走的同一函数）
    lib.joinScan();
    ck(!lib.scanning(), "扫描结束 scanning=false");
    ck(lib.trackCount() == 8, "8 首入库（misc/ 的非音频被排除）", std::to_string(lib.trackCount()));

    for (size_t i = 0; i < lib.trackCount(); ++i) {
        rta::net::LibTrack t; lib.trackAt(i, t);
        if (t.path.find("BoxSetAlbum") == std::string::npos) continue;
        const bool cd1 = t.path.find("CD1") != std::string::npos;
        ck(t.album == "BoxSetAlbum", "回填：盒装曲目 album = BoxSetAlbum", t.album);
        ck(t.albumFromDir, "回填：albumFromDir=true（来源=目录判据）");
        const std::string what = std::string("回填：discNo = ") + (cd1 ? "1" : "2");
        ck(t.discNo == (cd1 ? 1 : 2), what.c_str(), std::to_string(t.discNo));
        ck(t.discFromDir, "回填：discFromDir=true");
    }

    auto albums2 = lib.albums();
    ck(albums2.size() == 4, "专辑数 = 4（盒装合并成 1 张 + SummerHits + SoloAlbum + DirNameDifferent）",
       std::to_string(albums2.size()));
    std::string boxKey;
    for (auto& a : albums2) if (a.name == "BoxSetAlbum") boxKey = a.key;
    ck(!boxKey.empty(), "存在专辑 BoxSetAlbum");
    if (!boxKey.empty()) {
        rta::net::LibAlbum ba; lib.album(boxKey, ba);
        ck(ba.nTracks == 4, "BoxSetAlbum 4 首（CD1/CD2 合并）", std::to_string(ba.nTracks));
        std::string seq;
        for (size_t k = 0; k < ba.idx.size(); ++k) {
            rta::net::LibTrack t; lib.trackAt(ba.idx[k], t);
            seq += (k ? "," : "") + std::to_string(t.discNo);
        }
        ck(seq == "1,1,2,2", "BoxSetAlbum 曲目 discNo 序列 = 1,1,2,2（=/api/lib/album 的 d 字段）", seq);
        ck(ba.artist == "ArtistA" || ba.artist.empty(), "盒装专辑归类在 ArtistA 下", ba.artist);
    }
    bool miscIn = false, catAsAlbum = false;
    for (size_t i = 0; i < lib.trackCount(); ++i) {
        rta::net::LibTrack t; lib.trackAt(i, t);
        if (t.path.find("misc") != std::string::npos) miscIn = true;
        if (t.album == "Compilations") catAsAlbum = true;
    }
    ck(!miscIn, "misc/ 未进库");
    ck(!catAsAlbum, "分类层 Compilations 未被当成专辑名");

    // ---- 落盘断言：列数/顺序不变 + album/disc 如实 ----
    fs::path idxFile;
    for (auto& de : fs::directory_iterator(tmp2, ec))
        if (de.path().filename().string().rfind("rlplayer-index", 0) == 0) idxFile = de.path();
    ck(!idxFile.empty(), "索引已落盘");
    std::string idxText = readAll(idxFile);
    std::string header; std::vector<std::vector<std::string>> rows;
    {
        std::string line;
        for (size_t i = 0; i <= idxText.size(); ++i) {
            if (i == idxText.size() || idxText[i] == '\n') {
                if (!line.empty() && line[0] != '#') rows.push_back(splitTsv(line));
                else if (line.rfind("#path\t", 0) == 0) header = line;
                line.clear();
            } else line += idxText[i];
        }
    }
    // 判据（铁律 3）：不是"表头里出现了 cue"，而是**前 16 列与 v3 逐字相同**（老读取路径只看前 16 列，
    // 列序一动就串列）+ 末尾恰好追加 4 列。两条都写死，改任何一列都会红。
    const std::string kV3Head = "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre";
    ck(header == kV3Head + "\tcue\tcueN\tcueOff\tcueLen",
       "索引表头 = 前 16 列逐字不变 + 末尾追加 cue/cueN/cueOff/cueLen（v4）", header);
    ck(header.rfind(kV3Head, 0) == 0 && header.size() > kV3Head.size(),
       "前 16 列表头与 v3 逐字相同（老索引/老读取路径不会串列）");
    ck(rows.size() == 8, "索引 8 行", std::to_string(rows.size()));
    bool all20 = true;
    for (auto& r : rows) if (r.size() != 20) all20 = false;
    ck(all20, "每行 20 列（前 16 列原样 + 末尾 4 列 cue 字段）");
    std::map<std::string, std::pair<std::string, int>> boxRows;
    for (auto& r : rows)
        if (r.size() == 20 && r[0].find("BoxSetAlbum") != std::string::npos)
            boxRows[fs::path(r[0]).filename().string()] = { r[3], atoi(r[8].c_str()) };
    ck(boxRows.size() == 4, "TSV 里盒装 4 行", std::to_string(boxRows.size()));
    bool tsvAlb = true, tsvDisc = true;
    std::string tsvDetail;
    for (auto& kv : boxRows) {
        const int want = (kv.first == "01-Track1.flac" || kv.first == "02-Track2.flac") ? 1 : 2;
        if (kv.second.first != "BoxSetAlbum") tsvAlb = false;
        if (kv.second.second != want) tsvDisc = false;
        tsvDetail += kv.first + "=" + kv.second.first + "/" + std::to_string(kv.second.second) + " ";
    }
    ck(tsvAlb, "TSV album 列 = BoxSetAlbum（4 行）", tsvDetail);
    ck(tsvDisc, "TSV disc 列 = 1/1/2/2", tsvDetail);

    // ---- 真实标签优先（单元级：ffprobe 读到的标签不得被目录名覆盖）----
    {
        rta::net::LibTrack tagged;
        tagged.path = (music / "ArtistA/BoxSetAlbum/CD1/01-Track1.flac").string();
        tagged.album = "Real Tag Album";     // 模拟 ffprobe 读到的 album 标签
        tagged.discNo = 2;                   // 模拟真实碟号标签（故意与目录 CD1=1 冲突）
        rta::net::lib_fill_album_disc_from_dir(tagged);
        ck(tagged.album == "Real Tag Album", "有真实 album 标签时不覆盖（album 仍为标签值）", tagged.album);
        ck(!tagged.albumFromDir, "真实标签未被标记成目录来源");
        ck(tagged.discNo == 2 && !tagged.discFromDir, "有真实碟号标签时不覆盖（仍为 2）",
           std::to_string(tagged.discNo));

        rta::net::LibTrack half;
        half.path = (music / "ArtistA/BoxSetAlbum/CD2/01-Track3.flac").string();
        half.album = "Real Tag Album";       // 只有 album 标签，没有 disc 标签
        rta::net::lib_fill_album_disc_from_dir(half);
        ck(half.album == "Real Tag Album" && !half.albumFromDir, "半标签：album 保留标签值");
        ck(half.discNo == 2 && half.discFromDir, "半标签：缺碟号时按目录补 2（CD2）",
           std::to_string(half.discNo));
    }

    // ---- 重启回读：落盘值直接可用，专辑键不变 ----
    lib.load();
    ck(lib.trackCount() == 8, "重启后 8 首（新索引可读）", std::to_string(lib.trackCount()));
    if (!boxKey.empty()) {
        rta::net::LibAlbum ba;
        const bool ok = lib.album(boxKey, ba);
        ck(ok && ba.nTracks == 4, "重启后专辑键不变（落盘 album 与读时推算同名同键）");
        std::string seq;
        for (size_t k = 0; k < ba.idx.size(); ++k) {
            rta::net::LibTrack t; lib.trackAt(ba.idx[k], t);
            seq += (k ? "," : "") + std::to_string(t.discNo);
        }
        ck(seq == "1,1,2,2", "重启后碟号仍为 1,1,2,2（读的是落盘值）", seq);
    }

    // ---- [3] 老索引自愈：album 空 / disc 0 的旧记录在**增量**扫描里被补上 ----
    {
        std::string patched;
        {
            std::string line;
            for (size_t i = 0; i <= idxText.size(); ++i) {
                if (i == idxText.size() || idxText[i] == '\n') {
                    if (!line.empty() && line[0] != '#') {
                        auto f = splitTsv(line);
                        if (f.size() == 20) { f[3] = ""; f[8] = "0"; }   // 模拟老索引：album 空、disc 0
                        for (size_t k = 0; k < f.size(); ++k) { if (k) patched += '\t'; patched += f[k]; }
                        patched += '\n';
                    } else if (!line.empty()) { patched += line; patched += '\n'; }
                    line.clear();
                } else line += idxText[i];
            }
        }
        { std::ofstream f(idxFile, std::ios::binary | std::ios::trunc); f << patched; }
        lib.load();
        ck(lib.trackCount() == 8, "老格式索引仍能读（列数/顺序未变）");
        lib.startScan(false);                // 增量：mtime+size 未变 → 走缓存分支
        lib.joinScan();
        ck(lib.lastScanProbed() == 0, "增量扫描 0 次 ffprobe（旧记录被复用）", std::to_string(lib.lastScanProbed()));
        int healed = 0;
        for (size_t i = 0; i < lib.trackCount(); ++i) {
            rta::net::LibTrack t; lib.trackAt(i, t);
            if (t.path.find("BoxSetAlbum") != std::string::npos && t.album == "BoxSetAlbum" && t.discNo > 0)
                ++healed;
        }
        ck(healed == 4, "老索引经增量扫描自愈：4 首盒装曲目重新拿到 album/disc", std::to_string(healed));
        const std::string idx2 = readAll(idxFile);
        ck(idx2.find("BoxSetAlbum") != std::string::npos && idx2.find("#libIndexV\t5") != std::string::npos,
           "自愈结果已重新落盘（版本头是 v4）");
    }


    // =======================================================================
    // [4] 261025-01 一.1/2/3/5/8（大面积扫描 / 中文路径 / 删除清理 / 切标签不重扫）
    //
    // 判据（铁律 3）：这一节证明「UTF-8 -> Win32 的路径转换链不抛异常、不误判」以及
    // 「清理真的同时改内存与落盘」。**不含 HTTP 层**：真实的 /api/lib/scan、
    // /api/lib/status 延迟、3000 首大库计时在 tests/verify_scan_261025.ps1（端到端唯一判据）。
    // =======================================================================
    std::printf("\n== [4] 中文路径 / 大库 / 删除清理 ==\n");
    // 注意：连测试自己都得小心 —— path / "中文" 走的是同一个 ACP 窄串构造，一样会抛。
    auto U = [](const char* s) { return fs::u8path(s); };
    auto p2u = [](const fs::path& p) {
        const std::u8string u = p.u8string();
        return std::string((const char*)u.data(), u.size());
    };
    // ---- 4.1 纯函数：UTF-8 校验与路径前缀判定 ----
    {
        ck(rta::net::lib_valid_utf8("D:\\音乐\\专辑"), "合法 UTF-8 路径通过校验");
        ck(!rta::net::lib_valid_utf8("D:\\\xC4\xE3\xBA\xC3"), "GBK 字节被拒（否则 fs::u8path 会抛异常）");
        ck(rta::net::lib_path_under("D:\\mu\\a.flac", "D:\\mu"), "前缀判定：子路径命中");
        ck(rta::net::lib_path_under("d:\\MU\\a.flac", "D:\\mu"), "前缀判定：大小写不敏感");
        ck(!rta::net::lib_path_under("D:\\music\\a.flac", "D:\\mu"), "前缀判定：不把 mu 当 music 的前缀");
    }
    // ---- 4.2 中文/日文根目录：能加、能扫、能落盘（修前这里会让进程 0xC0000409） ----
    {
        const fs::path tmp4 = fs::temp_directory_path() / "rta_lib_utf8";
        fs::remove_all(tmp4, ec);
        fs::create_directories(tmp4 / U("lib"), ec);
        _putenv_s("RLPLAYER_LIBRARY_DIR", p2u(tmp4 / U("lib")).c_str());
        // 前几节留下的库根必须先清掉，否则这一节会连它们一起扫（本测试踩过一次：11 首）
        for (auto r : lib.roots()) lib.removeRoot(r);
        const fs::path zhRoot = tmp4 / U("中文音乐库");
        fs::create_directories(zhRoot / U("专辑甲"), ec);
        fs::create_directories(zhRoot / U("日语-アルバム"), ec);
        auto mk = [&](const char* rel) { std::ofstream f(zhRoot / U(rel), std::ios::binary); f << "fLaC"; };
        mk("专辑甲/第一首.flac");
        mk("专辑甲/第二首（全角括号）.flac");
        mk("日语-アルバム/曲名.flac");
        const auto ar = lib.addRootEx(p2u(zhRoot));
        ck(ar.ok, "含中文/日文的根目录能被 addRoot", ar.why);
        ck(!lib.addRootEx("Z:\\不存在").ok, "不存在的目录仍被拒");
        lib.load();                       // 清空索引（conf 里已有中文根）
        lib.startScan(true);
        lib.joinScan();
        ck(lib.trackCount() == 3, "中文根下 3 首全部入库", std::to_string(lib.trackCount()));
        ck(!lib.scanning(), "扫描正常结束（没有 fatal 异常）");
        ck(lib.scanMsg().find("无法访问") == std::string::npos, "扫描消息里没有坏根告警", lib.scanMsg());
        auto albums4 = lib.albums();
        bool hasZh = false, hasJp = false;
        for (auto& a : albums4) { if (a.name == "专辑甲") hasZh = true; if (a.name == "日语-アルバム") hasJp = true; }
        ck(hasZh && hasJp, "专辑列表里中文/日文专辑都在", std::to_string(albums4.size()) + " 张");
        ck(lib.albumCountFast() == albums4.size(), "snapshot 用的专辑数与真实一致");
        lib.load();
        ck(lib.trackCount() == 3, "索引落盘后能读回（宽字符文件流）", std::to_string(lib.trackCount()));
        // ---- 4.3 删除清理：prune / removeUnder / 根目录消失时的保护 ----
        fs::remove(zhRoot / U("专辑甲") / U("第二首（全角括号）.flac"), ec);
        const auto pr = lib.pruneMissing();
        ck(pr.removed == 1 && lib.trackCount() == 2, "pruneMissing 剔除 1 条已删文件的记录",
           std::to_string(pr.removed) + " / " + std::to_string(lib.trackCount()));
        lib.load();
        ck(lib.trackCount() == 2, "prune 的结果已落盘", std::to_string(lib.trackCount()));
        const auto rm = lib.removeUnder(p2u(zhRoot / U("日语-アルバム")));
        ck(rm.removed == 1 && lib.trackCount() == 1, "removeUnder 按目录移除 1 条",
           std::to_string(rm.removed) + " / " + std::to_string(lib.trackCount()));
        lib.load();
        ck(lib.trackCount() == 1, "removeUnder 的结果已落盘", std::to_string(lib.trackCount()));
        fs::rename(zhRoot, tmp4 / U("中文音乐库-OFFLINE"), ec);   // 根忽然不可访问（外接盘拔出）
        lib.startScan(false);
        lib.joinScan();
        ck(lib.trackCount() == 1, "根目录不可访问时保留其下旧记录（旧实现会掉成 0）",
           std::to_string(lib.trackCount()) + " msg=" + lib.scanMsg());
        ck(lib.scanMsg().find("无法访问") != std::string::npos, "扫描消息如实报告坏根", lib.scanMsg());
        fs::rename(tmp4 / U("中文音乐库-OFFLINE"), zhRoot, ec);
        fs::remove_all(tmp4, ec);
    }
    // ---- 4.4 大库专辑整理：3000 首必须快（旧实现在比较器里扫目录，全在锁内） ----
    {
        const fs::path tmp5 = fs::temp_directory_path() / "rta_lib_big";
        fs::remove_all(tmp5, ec);
        fs::create_directories(tmp5, ec);
        _putenv_s("RLPLAYER_LIBRARY_DIR", p2u(tmp5).c_str());
        {
            std::ofstream f(tmp5 / "rlplayer-index.tsv", std::ios::binary);
            f << "#libIndexV\t4\n";
            f << "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre"
                 "\tcue\tcueN\tcueOff\tcueLen\n";
            for (int d = 0; d < 300; ++d)
                for (int t = 0; t < 10; ++t) {
                    char row[512];
                    std::snprintf(row, sizeof(row),
                        "D:\\fake\\Album%04d\\t%d.flac\t111\t222\t\t\tArtist%03d\tTitle%d\t%d\t0\t0\t44100\t16\t2\t60.0\t0\tRock"
                        "\t\t0\t0\t0\n",
                        d, t, d, t, t + 1);
                    f << row;
                }
        }
        lib.load();
        ck(lib.trackCount() == 3000, "3000 首索引载入", std::to_string(lib.trackCount()));
        const auto tA = std::chrono::steady_clock::now();
        auto big = lib.albums();
        const double msBig = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tA).count();
        ck(big.size() == 300, "3000 首整理成 300 张专辑", std::to_string(big.size()));
        ck(msBig < 1000.0, "3000 首专辑整理 < 1000ms（这个数直接进单线程 HTTP 的同步路径）",
           std::to_string((int)msBig) + " ms");
        std::printf("  [INFO] 3000 首专辑整理 %.1f ms（旧实现：比较器内目录扫描，实测秒级）\n", msBig);
        const auto tB = std::chrono::steady_clock::now();
        lib.albums();
        const double msCache = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tB).count();
        ck(msCache < msBig + 5.0, "第二次 albums() 走缓存（不重复整理）", std::to_string((int)msCache) + " ms");
        fs::remove_all(tmp5, ec);
    }


    // =======================================================================
    // [5] CUE 整轨分轨（2026-10-25 第二里程碑）：v4 索引 / 迁移 / 真实扫描 / 删 cue 回归
    //
    // 判据（铁律 3：要证明的不是"有 cue 字段"，而是下面每一条都能被单独否定）：
    //   ① 真实扫描（startScan = /api/lib/scan 走同一个入口）：10 秒整轨 + 3 轨 GBK cue
    //      ⟹ 库里 **3 首**（整轨不再单独出现），dur = 4/4/2，path 是**同一个物理文件**；
    //   ② 落盘索引是 v4：前 16 列与 v3 逐字相同 + 末尾 4 列 cue 字段；重新 load() 后 cue 字段仍在；
    //   ③ 迁移：v3 索引必须整份丢弃（否则整轨曲目按 mtime+size 被复用，cue 永远不生效）；
    //   ④ 删掉 .cue 再**增量**扫描 ⟹ 回到 1 首整轨（回归：不能被旧索引里那条"末轨虚拟曲目"缓存命中）；
    //   ⑤ cue 的 FILE 大小写与磁盘不一致（ALBUM.WAV vs album.wav）⟹ 仍展开，path 用磁盘真名；
    //   ⑥ 坏 cue / 全是数据轨 / 整轨不存在 / 两张 cue 抢同一整轨 ⟹ 退回整轨（不丢歌、不产生幽灵曲目）。
    // 需要 ffprobe（末轨长度 = 文件总时长 - 末轨起点）；缺工具即失败，与 writeback/media_duration 同类门禁。
    // =======================================================================
    std::printf("\n== [5] CUE 整轨分轨（真实扫描） ==\n");
    {
        const std::string ffprobe = rta::audio::find_tool("ffprobe");
        ck(!ffprobe.empty(),
           "ffprobe 可用（末轨长度必须知道文件总时长；本仓同类门禁：writeback / media_duration）", ffprobe);
        auto u8_to_gbk = [](const std::string& u8) {
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
            if (n <= 0) return std::string();
            std::wstring w((size_t)n, L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), w.data(), n);
            const int m = ::WideCharToMultiByte(936, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            if (m <= 0) return std::string();
            std::string s((size_t)m, '\0');
            ::WideCharToMultiByte(936, 0, w.c_str(), (int)w.size(), s.data(), m, nullptr, nullptr);
            return s;
        };
        auto writeText = [](const fs::path& p, const std::string& s) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write(s.data(), (std::streamsize)s.size());
        };
        // 真正可探测的 WAV（RIFF + fmt16 + data；全零 = 数字静音，不打开任何设备、不出声）
        auto writeWav = [](const fs::path& p, int sr, int ch, int seconds) {
            const int bits = 16;
            const long long frames = (long long)sr * seconds;
            const long long dataBytes = frames * ch * (bits / 8);
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            auto le = [&](long long v, int n) { for (int i = 0; i < n; ++i) f.put((char)((v >> (8 * i)) & 0xFF)); };
            f.write("RIFF", 4); le(36 + dataBytes, 4); f.write("WAVE", 4);
            f.write("fmt ", 4); le(16, 4); le(1, 2); le(ch, 2); le(sr, 4);
            le((long long)sr * ch * (bits / 8), 4); le(ch * (bits / 8), 2); le(bits, 2);
            f.write("data", 4); le(dataBytes, 4);
            std::vector<char> zero(65536, 0);
            for (long long left = dataBytes; left > 0; ) {
                const long long k = std::min<long long>(left, (long long)zero.size());
                f.write(zero.data(), (std::streamsize)k);
                left -= k;
            }
        };
        // 换库目录 + 换根 + 全量/增量扫描（与 /api/lib/scan 同一个 Library 实现）
        auto scanLib = [&](const fs::path& root, const fs::path& libDir, bool full) {
            // 库目录必须先存在：writeFile 到不存在的目录会静默失败 ⟹ conf 读不到 ⟹ load() 回落到
            // 「自动探测常见目录」，于是扫的是用户真实音乐目录（本轮实测踩到：明明 1 个 wav，结果 4 首）
            fs::create_directories(libDir, ec);
            _putenv_s("RLPLAYER_LIBRARY_DIR", p2u(libDir).c_str());
            _putenv_s("RLPLAYER_PORT", "");
            for (auto r : lib.roots()) lib.removeRoot(r);
            const auto ar = lib.addRootEx(p2u(root));
            lib.load();
            lib.startScan(full);
            lib.joinScan();
            return ar;
        };
        auto trackOf = [&](size_t i) { rta::net::LibTrack t; lib.trackAt(i, t); return t; };
        auto albumNamed = [&](const std::string& name) {
            rta::net::LibAlbum a;
            for (auto& x : lib.albums()) if (x.name == name) { a = x; return a; }
            return a;
        };
        auto countIn = [&](const std::string& dirName) {
            int c = 0;
            for (size_t i = 0; i < lib.trackCount(); ++i) {
                rta::net::LibTrack t; lib.trackAt(i, t);
                if (t.path.find(dirName) != std::string::npos) ++c;
            }
            return c;
        };
        auto readIdx = [&](const fs::path& dir, fs::path& out) {
            out.clear();
            std::error_code e;
            for (auto& de : fs::directory_iterator(dir, e))
                if (de.path().filename().string().rfind("rlplayer-index", 0) == 0) out = de.path();
        };

        const fs::path tmp6 = fs::temp_directory_path() / "rta_lib_cue";
        fs::remove_all(tmp6, ec);
        const long long SEC = 44100;   // 1 秒的采样数（44.1k）

        // ---------------- ① 端到端扫描：10 秒整轨 + 3 轨 GBK cue ----------------
        {
            const fs::path libA = tmp6 / "libA";
            const fs::path rootA = tmp6 / U("音乐库A");
            const fs::path albA = rootA / U("整轨专辑");
            fs::create_directories(albA, ec);
            const fs::path wavA = albA / U("album.wav");
            writeWav(wavA, 44100, 2, 10);
            const std::string cueA =
                "TITLE \"整轨测试专辑\"\r\nPERFORMER \"测试艺术家\"\r\n"
                "REM GENRE \"Classical\"\r\nREM DATE 2019\r\n"
                "FILE \"album.wav\" WAVE\r\n"
                "  TRACK 01 AUDIO\r\n    TITLE \"第一首 四秒\"\r\n    INDEX 01 00:00:00\r\n"
                "  TRACK 02 AUDIO\r\n    TITLE \"第二首 四秒\"\r\n    INDEX 01 00:04:00\r\n"
                "  TRACK 03 AUDIO\r\n    TITLE \"第三首 两秒\"\r\n    INDEX 01 00:08:00\r\n";
            const std::string gbkA = u8_to_gbk(cueA);
            ck(!gbkA.empty() && gbkA != cueA, "GBK 编码成功且与 UTF-8 不同（真的走了 CP936 存盘）");
            writeText(albA / U("album.cue"), gbkA);

            const auto arA = scanLib(rootA, libA, true);
            ck(arA.ok, "CUE 夹具：库根可加", arA.why);
            ck(lib.trackCount() == 3, "10 秒整轨 + 3 轨 cue ⟹ 库里 3 首（整轨不再单独出现）",
               std::to_string(lib.trackCount()) + " msg=" + lib.scanMsg());
            const std::string wavU8 = p2u(wavA);
            const std::string cueU8 = p2u(albA / U("album.cue"));
            const rta::net::LibAlbum aa = albumNamed("整轨测试专辑");
            ck(aa.nTracks == 3, "专辑名 = cue 的全局 TITLE（GBK 中文逐字相等），3 首",
               std::to_string(aa.nTracks));
            ck(aa.artist == "测试艺术家", "专辑艺术家 = cue 的 PERFORMER", aa.artist);
            ck(std::abs(aa.duration - 10.0) < 1e-6,
               "专辑总时长 = 10 秒 = 整轨时长（不是 3×10）", std::to_string(aa.duration));
            if (aa.idx.size() == 3) {
                const rta::net::LibTrack v0 = trackOf(aa.idx[0]);
                const rta::net::LibTrack v1 = trackOf(aa.idx[1]);
                const rta::net::LibTrack v2 = trackOf(aa.idx[2]);
                ck(v0.title == "第一首 四秒" && v1.title == "第二首 四秒" && v2.title == "第三首 两秒",
                   "3 个中文曲名逐字相等（GBK cue → 库 → 专辑视图）",
                   v0.title + " | " + v1.title + " | " + v2.title);
                ck(v0.path == wavU8 && v1.path == wavU8 && v2.path == wavU8,
                   "3 首的 path 是同一个物理文件（播放器照旧能吃）", v2.path);
                ck(v0.trackNo == 1 && v1.trackNo == 2 && v2.trackNo == 3, "专辑内曲目号 1/2/3");
                ck(v0.cueNo == 1 && v1.cueNo == 2 && v2.cueNo == 3, "cueNo 保留 cue 里的 TRACK 号");
                ck(v0.isCueTrack() && v1.isCueTrack() && v2.isCueTrack(), "3 首都带 isCueTrack()");
                ck(v0.sampleRate == 44100 && v0.channels == 2 && v0.bits == 16,
                   "采样率/声道/位深来自物理文件（ffprobe 透传）", std::to_string(v0.sampleRate));
                ck(v0.cuePath == cueU8 && v1.cuePath == cueU8, "cuePath 回填成 .cue 的绝对路径");
                ck(v0.cueOff == 0 && v1.cueOff == 4 * SEC && v2.cueOff == 8 * SEC,
                   "cueOff = 0 / 176400 / 352800（4 秒 @44100）",
                   std::to_string(v0.cueOff) + "/" + std::to_string(v1.cueOff) + "/" + std::to_string(v2.cueOff));
                ck(v0.cueLen == 4 * SEC && v1.cueLen == 4 * SEC && v2.cueLen == 2 * SEC,
                   "cueLen = 4/4/2 秒（末轨 = 文件总时长 10s - 8s，靠 ffprobe 的总时长）",
                   std::to_string(v0.cueLen) + "/" + std::to_string(v1.cueLen) + "/" + std::to_string(v2.cueLen));
                ck(std::abs(v0.duration - 4.0) < 1e-6 && std::abs(v1.duration - 4.0) < 1e-6 &&
                   std::abs(v2.duration - 2.0) < 1e-6,
                   "dur = 4 / 4 / 2（= /api/lib/album 的 d.dur 字段）",
                   std::to_string(v0.duration) + "/" + std::to_string(v1.duration) + "/" + std::to_string(v2.duration));
                double s = -1, e = -1;
                ck(rta::net::lib_cue_range_sec(v1, s, e) &&
                   std::abs(s - 4.0) < 1e-9 && std::abs(e - 8.0) < 1e-9,
                   "lib_cue_range_sec(第 2 首) = 4.0..8.0 秒（播放侧要用的区间）",
                   std::to_string(s) + ".." + std::to_string(e));
            }

            // ---- 认领的整轨**每次扫描都重探**（不靠 mtime+size 缓存）----
            // 为什么要单独判：末轨长度 = 文件总时长 - 末轨起点，而上一次若 ffprobe 失败（duration=0），
            // 缓存复用会让末轨永远是 0 秒。铁律 3：这条必须能区分"重探"与"命中缓存"。
            lib.startScan(false);            // 一个字节都不改，走增量入口
            lib.joinScan();
            ck(lib.lastScanProbed() == 1 && lib.lastScanCached() == 0,
               "cue 认领的整轨在增量扫描里仍被重探（1 次 ffprobe、0 次缓存复用）",
               std::to_string(lib.lastScanProbed()) + " probed / " + std::to_string(lib.lastScanCached()) + " cached");
            ck(lib.trackCount() == 3, "空跑一次增量扫描后仍是 3 首", std::to_string(lib.trackCount()));

            // ---- ② 落盘索引：v4 + 前 16 列原样 + 末尾 4 列 ----
            fs::path idxA;
            readIdx(libA, idxA);
            ck(!idxA.empty(), "v4 索引已落盘");
            const std::string textA = readAll(idxA);
            ck(textA.find("#libIndexV\t5") != std::string::npos, "落盘版本头 = #libIndexV 5（2026-10-26 升版：标签修复 + 专辑归并）");
            std::string headA;
            std::vector<std::vector<std::string>> rowsA;
            {
                std::string line;
                for (size_t i = 0; i <= textA.size(); ++i) {
                    if (i == textA.size() || textA[i] == '\n') {
                        if (!line.empty() && line[0] != '#') rowsA.push_back(splitTsv(line));
                        else if (line.rfind("#path\t", 0) == 0) headA = line;
                        line.clear();
                    } else line += textA[i];
                }
            }
            const std::string kV3Head =
                "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre";
            ck(headA == kV3Head + "\tcue\tcueN\tcueOff\tcueLen",
               "表头 = v3 的 16 列逐字不变 + 末尾 4 列 cue/cueN/cueOff/cueLen", headA);
            ck(rowsA.size() == 3, "索引 3 行（3 条虚拟曲目，没有整轨那一行）", std::to_string(rowsA.size()));
            bool rows20 = rowsA.size() == 3;
            for (auto& r : rowsA) if (r.size() != 20) rows20 = false;
            ck(rows20, "每行 20 列");
            bool cueCols = rowsA.size() == 3;
            for (size_t i = 0; cueCols && i < rowsA.size(); ++i) {
                if (rowsA[i][16] != cueU8) cueCols = false;
                if (atoi(rowsA[i][17].c_str()) != (int)i + 1) cueCols = false;
                if (_strtoi64(rowsA[i][18].c_str(), nullptr, 10) != (long long)i * 4 * SEC) cueCols = false;
                if (_strtoi64(rowsA[i][19].c_str(), nullptr, 10) <= 0) cueCols = false;
            }
            ck(cueCols, "落盘 cue 列 = 绝对路径 / cueN=1,2,3 / cueOff=0,176400,352800 / cueLen>0");

            // ---- 重启回读：cue 字段必须真的从索引里回来 ----
            lib.load();
            ck(lib.trackCount() == 3, "重启后仍是 3 首（v4 索引可读）", std::to_string(lib.trackCount()));
            const rta::net::LibAlbum ab2 = albumNamed("整轨测试专辑");
            ck(ab2.nTracks == 3, "重启后专辑仍 3 首", std::to_string(ab2.nTracks));
            if (ab2.idx.size() == 3) {
                const rta::net::LibTrack t2 = trackOf(ab2.idx[1]);
                ck(t2.isCueTrack() && t2.cueNo == 2 && t2.cueOff == 4 * SEC && t2.cueLen == 4 * SEC,
                   "重启后第 2 首的 cueNo/cueOff/cueLen 仍在（真的落盘了，不是内存巧合）",
                   std::to_string(t2.cueOff) + "/" + std::to_string(t2.cueLen));
            }

            // ---- ③ 迁移：v3 索引整份丢弃，一次全量重建 ----
            {
                const std::string v3 = "#libIndexV\t3\n" + kV3Head + "\n" +
                    wavU8 + "\t1\t222\t整轨测试专辑\t测试艺术家\t测试艺术家\talbum\t1\t0\t0\t44100\t16\t2\t10.0\t0\tClassical\n";
                writeText(idxA, v3);
                lib.load();
                ck(lib.trackCount() == 0, "v3 索引被整份丢弃（不迁移旧行，否则整轨曲目会被复用）",
                   std::to_string(lib.trackCount()));
                lib.startScan(false);   // 增量入口：没有旧行 ⟹ 全部重探
                lib.joinScan();
                ck(lib.trackCount() == 3, "v3 → v4 迁移后重新扫出 3 首虚拟曲目（cue 生效）",
                   std::to_string(lib.trackCount()));
            }

            // ---- ④ 删 cue 回归：删掉 .cue 后**增量**扫描必须回到 1 首整轨 ----
            fs::remove(albA / U("album.cue"), ec);
            lib.startScan(false);
            lib.joinScan();
            ck(lib.trackCount() == 1, "删掉 .cue 后回到 1 首整轨（旧索引里的虚拟曲目不能被缓存命中）",
               std::to_string(lib.trackCount()) + " msg=" + lib.scanMsg());
            if (lib.trackCount() == 1) {
                const rta::net::LibTrack t = trackOf(0);
                ck(t.path == wavU8 && !t.isCueTrack() && t.cueNo == 0 && t.cueOff == 0,
                   "整轨曲目：path 不变、cue 字段清零", t.path);
                ck(t.title == "album", "曲名回退到文件名（不再是 cue 第 3 轨的标题）", t.title);
                ck(std::abs(t.duration - 10.0) < 1e-6, "整轨时长 = 10 秒", std::to_string(t.duration));
            }
        }

        // ---------------- ⑤ 大小写不一致：cue 写 ALBUM.WAV、磁盘 album.wav ----------------
        {
            const fs::path libC = tmp6 / "libC";
            const fs::path rootC = tmp6 / U("音乐库C");
            const fs::path albC = rootC / U("大小写");
            fs::create_directories(albC, ec);
            const fs::path wavC = albC / U("album.wav");
            writeWav(wavC, 44100, 2, 10);
            writeText(albC / U("case.cue"),
                      "TITLE \"大小写专辑\"\nPERFORMER \"P\"\nFILE \"ALBUM.WAV\" WAVE\n"
                      "  TRACK 01 AUDIO\n    TITLE \"c1\"\n    INDEX 01 00:00:00\n"
                      "  TRACK 02 AUDIO\n    TITLE \"c2\"\n    INDEX 01 00:06:00\n");
            const auto arC = scanLib(rootC, libC, true);
            ck(arC.ok, "大小写夹具：库根可加");
            ck(lib.trackCount() == 2, "cue 的 FILE 与磁盘大小写不一致 ⟹ 仍展开 2 首",
               std::to_string(lib.trackCount()));
            bool pathReal = lib.trackCount() == 2;
            for (size_t i = 0; pathReal && i < lib.trackCount(); ++i) {
                const rta::net::LibTrack t = trackOf(i);
                if (t.path != p2u(wavC)) pathReal = false;
            }
            ck(pathReal, "path = 磁盘真名（album.wav，不是 cue 里的 ALBUM.WAV）");
            bool lenOk = lib.trackCount() == 2;
            for (size_t i = 0; lenOk && i < lib.trackCount(); ++i) {
                const rta::net::LibTrack t = trackOf(i);
                if (i == 0 && t.cueLen != 6 * SEC) lenOk = false;
                if (i == 1 && t.cueLen != 4 * SEC) lenOk = false;
            }
            ck(lenOk, "长度按索引换算：轨 1 = 6 秒、轨 2 = 10-6 = 4 秒");
        }

        // ---------------- ⑥ 退回整轨：坏 cue / 全数据轨 / 整轨不存在 / 被抢 ----------------
        {
            const fs::path libD = tmp6 / "libD";
            const fs::path rootD = tmp6 / U("音乐库D");
            auto mk = [&](const char* dir, const char* wavName, int sec) {
                const fs::path d = rootD / U(dir);
                fs::create_directories(d, ec);
                writeWav(d / U(wavName), 44100, 2, sec);
                return d;
            };
            // ① 坏 cue：TRACK 没有 INDEX → 解析器丢轨 → 认领不成立 → 整轨照旧
            const fs::path d1 = mk("broken", "album.wav", 10);
            writeText(d1 / U("broken.cue"),
                      "TITLE \"坏\"\nFILE \"album.wav\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"x\"\n");
            // ② 全是数据轨（MODE1/2352）
            const fs::path d2 = mk("dataonly", "album.wav", 10);
            writeText(d2 / U("data.cue"),
                      "FILE \"album.wav\" WAVE\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
            // ③ cue 指向的整轨不存在（只剩 cue）：不许产生幽灵曲目，同目录的音频照旧整轨
            const fs::path d3 = mk("ghost", "ghost.wav", 10);
            writeText(d3 / U("ghost.cue"),
                      "TITLE \"幽灵\"\nFILE \"gone.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
            // ④ 两张 cue 抢同一整轨：按路径排序先到先得（a.cue 在前）
            const fs::path d4 = mk("dup", "album.wav", 10);
            writeText(d4 / U("a.cue"),
                      "TITLE \"抢A\"\nFILE \"album.wav\" WAVE\n"
                      "  TRACK 01 AUDIO\n    TITLE \"A1\"\n    INDEX 01 00:00:00\n"
                      "  TRACK 02 AUDIO\n    TITLE \"A2\"\n    INDEX 01 00:05:00\n");
            writeText(d4 / U("b.cue"),
                      "TITLE \"抢B\"\nFILE \"album.wav\" WAVE\n"
                      "  TRACK 01 AUDIO\n    TITLE \"B1\"\n    INDEX 01 00:00:00\n"
                      "  TRACK 02 AUDIO\n    TITLE \"B2\"\n    INDEX 01 00:05:00\n");
            const auto arD = scanLib(rootD, libD, true);
            ck(arD.ok, "退回整轨夹具：库根可加");
            ck(lib.trackCount() == 5,
               "坏 cue / 数据轨 / 整轨缺失 各退回 1 首整轨 + 被抢的只展开一次 = 5 首",
               std::to_string(lib.trackCount()) + " msg=" + lib.scanMsg());
            ck(countIn("broken") == 1, "坏 cue ⟹ 退回 1 首整轨（不丢歌）", std::to_string(countIn("broken")));
            ck(countIn("dataonly") == 1, "全是数据轨的 cue ⟹ 退回 1 首整轨",
               std::to_string(countIn("dataonly")));
            ck(countIn("ghost") == 1, "cue 指向的整轨不存在 ⟹ 不产生幽灵曲目，同目录音频照旧整轨",
               std::to_string(countIn("ghost")));
            ck(countIn("dup") == 2, "两张 cue 抢同一整轨 ⟹ 只展开一次（先到先得，不重复）",
               std::to_string(countIn("dup")));
            bool fallbackClean = true;
            std::string fbTitles;
            for (size_t i = 0; i < lib.trackCount(); ++i) {
                const rta::net::LibTrack t = trackOf(i);
                if (t.path.find("dup") != std::string::npos) continue;
                if (t.isCueTrack()) fallbackClean = false;
                fbTitles += t.title + " ";
            }
            ck(fallbackClean, "退回整轨的 3 首没有 cue 字段（不假装自己是虚拟曲目）", fbTitles);
            std::string dupTitles;
            for (size_t i = 0; i < lib.trackCount(); ++i) {
                const rta::net::LibTrack t = trackOf(i);
                if (t.path.find("dup") != std::string::npos) dupTitles += t.title + " ";
            }
            ck(dupTitles == "A1 A2 ", "被抢后只用先到的 a.cue 展开（B1/B2 不重复出现）", dupTitles);
        }
        fs::remove_all(tmp6, ec);
    }

    // =======================================================================
    // [6] ISO 镜像：识别 + 入库 + 播放报错（2026-10-27，用户 261027-01 #3）
    //
    // 判据（铁律 3：每一条都能被单独否定）：
    //   ① `.iso` 真的进了库（trackCount 说出来），并且 needsConvert()=true（= 不当可播音轨）；
    //   ② 分得清 SACD 与普通数据盘：Library::isoKindOf() = sacd / data；
    //   ③ 播放走 read_any 得到的是「带指引的拒绝」（SACD 提示 / 不是音频镜像），
    //      而不是 ffmpeg 的 Invalid data（这条正是用户反馈的形态）；
    //   ④ 重启（load 索引）后 still needsConvert() —— 标记由路径现算，不靠落盘列。
    // 夹具是**真的 ISO9660 镜像**（主卷描述符 CD001 + 根目录记录），不是随便的文件改名。
    // =======================================================================
    std::printf("\n== [6] ISO 镜像识别 / 入库 / 播放报错 ==\n");
    {
        // 造最小可解析的 ISO9660 镜像：PVD 在 16 号扇区，根目录在 20 号扇区
        auto makeIso = [](const fs::path& p, const std::string& volId,
                          const std::vector<std::string>& rootNames) {
            const size_t sectors = 40;
            std::vector<unsigned char> img(sectors * 2048, 0);
            auto putLe32 = [&](size_t at, unsigned int v) {
                for (int i = 0; i < 4; ++i) img[at + (size_t)i] = (unsigned char)((v >> (8 * i)) & 0xFF);
            };
            auto putBe32 = [&](size_t at, unsigned int v) {
                for (int i = 0; i < 4; ++i) img[at + (size_t)i] = (unsigned char)((v >> (8 * (3 - i))) & 0xFF);
            };
            const size_t pvd = (size_t)16 * 2048;
            img[pvd] = 0x01;
            std::memcpy(&img[pvd + 1], "CD001", 5);
            img[pvd + 6] = 0x01;
            std::string v = volId; v.resize(32, ' ');
            std::memcpy(&img[pvd + 40], v.data(), 32);
            // 根目录记录（PVD 偏移 156，34 字节）：extent LBA = 20，长度 = 2048，目录标志
            img[pvd + 156] = 34;
            putLe32(pvd + 156 + 2, 20);  putBe32(pvd + 156 + 6, 20);
            putLe32(pvd + 156 + 10, 2048); putBe32(pvd + 156 + 14, 2048);
            img[pvd + 156 + 25] = 0x02;
            img[pvd + 156 + 32] = 1;      // 名字长度 1（.）
            // 卷描述符终止符
            const size_t term = (size_t)17 * 2048;
            img[term] = 0xFF;
            std::memcpy(&img[term + 1], "CD001", 5);
            img[term + 6] = 0x01;
            // 根目录条目：. / .. / 若干子项
            size_t off = (size_t)20 * 2048;
            unsigned int lba = 21;
            auto addRec = [&](const std::string& name, unsigned char flags) {
                size_t len = 33 + name.size();
                if (len & 1) ++len;
                img[off] = (unsigned char)len;
                putLe32(off + 2, lba);  putBe32(off + 6, lba);
                putLe32(off + 10, 2048); putBe32(off + 14, 2048);
                img[off + 25] = flags;
                img[off + 32] = (unsigned char)name.size();
                if (!name.empty()) std::memcpy(&img[off + 33], name.data(), name.size());
                off += len;
            };
            addRec(std::string(1, '\0'), 0x02);   // .
            addRec(std::string(1, '\x01'), 0x02); // ..
            for (auto& n : rootNames) { addRec(n, 0x02); ++lba; }
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write((const char*)img.data(), (std::streamsize)img.size());
        };

        const fs::path tmp7 = fs::temp_directory_path() / "rta_lib_iso";
        fs::remove_all(tmp7, ec);
        const fs::path libG = tmp7 / "libG";
        const fs::path rootG = tmp7 / U("音乐库G");
        const fs::path albSacd = rootG / U("SACD镜像");
        const fs::path albData = rootG / U("数据盘");
        fs::create_directories(albSacd, ec);
        fs::create_directories(albData, ec);
        const fs::path isoSacd = albSacd / U("album.iso");
        const fs::path isoData = albData / U("data.iso");
        makeIso(isoSacd, "SACD_MUSIC", { "SACD", "README.TXT" });       // 根目录有 SACD/
        makeIso(isoData, "MY_DATA_DISC", { "README.TXT", "SETUP.EXE" }); // 没有 SACD 目录

        // 本节的扫描助手（[5] 节的 scanLib/trackOf 在它自己的块作用域里，这里重新给一份）
        auto scanLib6 = [&](bool full) {
            fs::create_directories(libG, ec);
            _putenv_s("RLPLAYER_LIBRARY_DIR", p2u(libG).c_str());
            _putenv_s("RLPLAYER_PORT", "");
            for (auto r : lib.roots()) lib.removeRoot(r);
            const auto ar = lib.addRootEx(p2u(rootG));
            lib.load();
            lib.startScan(full);
            lib.joinScan();
            return ar;
        };
        auto trackOf6 = [&](size_t i) { rta::net::LibTrack t; lib.trackAt(i, t); return t; };

        const auto arG = scanLib6(true);
        ck(arG.ok, ".iso 夹具：库根可加", arG.why);
        ck(lib.trackCount() == 2, "两个 .iso 都入库了（能看见）",
           std::to_string(lib.trackCount()) + " msg=" + lib.scanMsg());
        ck(lib.needsConvertCount() == 2, "两首都标着「需转换」（needsConvertCount=2）",
           std::to_string(lib.needsConvertCount()));
        const std::string sSacd = p2u(isoSacd), sData = p2u(isoData);
        ck(lib.isoKindOf(sSacd) == "sacd", "根目录含 SACD/ ⟹ isoKindOf = sacd", lib.isoKindOf(sSacd));
        ck(lib.isoKindOf(sData) == "data", "没有 SACD 目录 ⟹ isoKindOf = data", lib.isoKindOf(sData));

        bool flags5 = lib.trackCount() == 2;
        std::string codecs;
        for (size_t i = 0; flags5 && i < lib.trackCount(); ++i) {
            const rta::net::LibTrack t = trackOf6(i);
            if (!t.needsConvert()) flags5 = false;
            if (t.sampleRate != 0 || t.duration != 0.0) flags5 = false;   // 不给"看起来有效"的流参数
            codecs += t.codec + " | ";
        }
        ck(flags5, "入库记录：needsConvert=true、sr=0、dur=0（不当可播音轨）", codecs);
        ck(codecs.find("SACD ISO") != std::string::npos && codecs.find("数据盘") != std::string::npos,
           "codec 字段带出「SACD ISO（需转换）」/「ISO 数据盘（非音频镜像）」", codecs);
        ck(lib.scanMsg().find("需转换") != std::string::npos,
           "扫描消息里如实报「N 个 ISO 镜像需转换」", lib.scanMsg());

        // ③ 播放（= read_any，/api/lib/play 的解码入口）必须是带指引的拒绝
        {
            std::string m1, m2;
            bool t1 = false, t2 = false;
            try { (void)rta::audio::read_any(sSacd, nullptr); }
            catch (const rta::audio::UnsupportedFormat& e) { t1 = true; m1 = e.what(); }
            catch (const std::exception& e) { m1 = e.what(); }
            try { (void)rta::audio::read_any(sData, nullptr); }
            catch (const rta::audio::UnsupportedFormat& e) { t2 = true; m2 = e.what(); }
            catch (const std::exception& e) { m2 = e.what(); }
            ck(t1 && m1.find("SACD ISO") != std::string::npos &&
               m1.find("sacd_extract") != std::string::npos,
               "播 SACD ISO ⟹ 「SACD ISO 镜像暂不支持直接播放…请先用 sacd_extract 转成分轨 .dsf」(UnsupportedFormat)",
               m1);
            ck(t2 && m2.find("不是音频镜像") != std::string::npos,
               "播普通数据盘 ISO ⟹ 「这不是音频镜像…」（不是 SACD 提示，也不是 Invalid data）", m2);
            ck(m1.find("Invalid data") == std::string::npos && m2.find("Invalid data") == std::string::npos,
               "两条文案都不含 ffmpeg 的 Invalid data", m1 + " || " + m2);
        }

        // ④ 重启：标记由路径现算，旧索引（v5，没有这一列）读回来也是对的
        lib.load();
        ck(lib.trackCount() == 2, "重启后 .iso 仍在库里", std::to_string(lib.trackCount()));
        bool stillFlag = lib.trackCount() == 2;
        for (size_t i = 0; stillFlag && i < lib.trackCount(); ++i)
            if (!trackOf6(i).needsConvert()) stillFlag = false;
        ck(stillFlag, "重启后 needsConvert() 仍为 true（不依赖落盘列）");
        {
            // 索引没有 codec 列 ⟹ 重启后 trackAt 会用路径补一条内存标签，UI 的「需转换」才不会消失
            std::string lb;
            for (size_t i = 0; i < lib.trackCount(); ++i) lb += trackOf6(i).codec + " | ";
            ck(lb.find("需转换") != std::string::npos,
               "重启后 /api/lib/album 的 codec 字段仍有「需转换」（trackAt 按扩展名补，纯内存）", lb);
        }
        ck(lib.isoKindOf(sSacd) == "sacd", "重启后 isoKindOf 仍答 sacd（读镜像头，随时可问）");
        fs::remove_all(tmp7, ec);
    }

    // =======================================================================
    // [8] 封面来源：一目录多专辑不得共用目录封面（2026-10-27，用户 2.0 发包前 BUG）
    //
    // 现场：D:\...\2023 一个目录里 **10 张不同专辑**（10 个 ALBUM 标签），只有一张 cover.jpg。
    //   老实现 hasCover = 「任一曲目 cv=1」，而 cv 含「同目录有封面文件」⟹ 10 张专辑共用那张图；
    //   那张图实测是「瓦依那 - 大梦.flac」（ALBUM=乐队的夏天3 第7期）自己的内嵌封面。
    // 判据（铁律 3：每条都能被单独否定）：
    //   ① 一目录多专辑：目录里的 cover.jpg **不得**外借 ⟹ 没有内嵌的专辑 hasCover=false、
    //      albumCover() 返回 false（/api/lib/cover 据此 404、前端露首字母）；
    //   ② 一目录一专辑：目录封面照旧可用（覆盖率不回归），且 albumCover 返回的**就是那个文件**；
    //   ③ 多碟装 CD1/CD2：仍合成**一张**专辑、仍共享封面（目录归一不能误伤）；
    //   ④ 一目录多专辑 + 其中一首有真内嵌：那张专辑取到的是**它自己的内嵌图**，不是目录那张。
    // 库数据直接喂索引 TSV（= 扫描后的落盘形态），不需要 ffprobe；只有 ④ 要真 FLAC（有门禁）。
    // =======================================================================
    std::printf("\n== [8] 封面来源：一目录多专辑不得共用目录封面 ==\n");
    {
        auto writeJpg = [](const fs::path& p, size_t n, unsigned char fill) {
            std::vector<char> b(n, (char)fill);
            b[0] = (char)0xFF; b[1] = (char)0xD8; b[2] = (char)0xFF; b[3] = (char)0xE0;   // JPEG 魔数
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write(b.data(), (std::streamsize)b.size());
        };
        auto readBytes = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        const fs::path tmp8 = fs::temp_directory_path() / "rta_lib_covsrc";
        fs::remove_all(tmp8, ec);
        const fs::path libH  = tmp8 / U("libH");
        const fs::path rootH = tmp8 / U("音乐库H");
        const fs::path dirMix  = rootH / U("混装目录");
        const fs::path dirOne  = rootH / U("单专辑目录");
        const fs::path dirCD1  = rootH / U("双碟专辑") / U("CD1");
        const fs::path dirCD2  = rootH / U("双碟专辑") / U("CD2");
        const fs::path dirEmbd = rootH / U("内嵌混装");
        for (const fs::path& d : { dirMix, dirOne, dirCD1, dirCD2, dirEmbd })
            fs::create_directories(d, ec);
        writeJpg(dirMix  / U("cover.jpg"), 4096, 0x11);   // 混装目录：只属于其中一张，不许外借
        writeJpg(dirOne  / U("cover.jpg"), 2048, 0x22);
        writeJpg(dirCD1  / U("cover.jpg"), 3072, 0x33);
        writeJpg(dirCD2  / U("cover.jpg"), 5120, 0x44);
        writeJpg(dirEmbd / U("cover.jpg"), 6144, 0x55);

        // ④ 需要一份**真有内嵌封面**的 FLAC：用 ffmpeg 生成（全零音频，不开设备、不出声）
        const std::string ffmpeg = rta::audio::find_tool("ffmpeg");
        const fs::path flacEmb = dirEmbd / U("有内嵌.flac");
        bool embdReady = false;
        if (!ffmpeg.empty()) {
            const fs::path tmpJpg = tmp8 / U("embed.jpg");
            auto runFF = [&](const std::wstring& c) { return rta::audio::run_hidden(c, nullptr); };
            // 用 testsrc2（细节丰富）而不是纯色：纯色 JPEG 只有几百字节，会被 albumCover 的
            // "< 1 KB = 占位图"判据当成占位（那是既有的、正确的判据），夹具就测不出东西了。
            runFF(L"\"" + rta::audio::widen(ffmpeg) + L"\" -v error -y -f lavfi -i \"testsrc2=s=320x320:d=1\""
                  L" -frames:v 1 -c:v mjpeg -q:v 2 \"" + rta::audio::widen(p2u(tmpJpg)) + L"\"");
            runFF(L"\"" + rta::audio::widen(ffmpeg) + L"\" -v error -y -f lavfi -i \"anullsrc=r=44100:cl=stereo\""
                  L" -i \"" + rta::audio::widen(p2u(tmpJpg)) + L"\" -map 0:a -map 1:v -c:a flac -c:v copy"
                  L" -disposition:v attached_pic -t 1 \"" + rta::audio::widen(p2u(flacEmb)) + L"\"");
            std::string em0;
            embdReady = fs::exists(flacEmb, ec) && rta::net::lib_cover_embedded_bytes(p2u(flacEmb), em0).size() > 1024;
        }
        ck(embdReady, "④ 夹具：生成了带内嵌封面的 FLAC（缺 ffmpeg 则本节只剩 ①②③ 有判定力）");

        fs::create_directories(libH, ec);
        {
            std::ofstream f(libH / U("rlplayer-index.tsv"), std::ios::binary);
            f << "#libIndexV\t5\n";
            f << "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre"
                 "\tcue\tcueN\tcueOff\tcueLen\n";
            auto row = [&](const fs::path& p, const char* alb, const char* aa, const char* art,
                           const char* ttl, int trk, int disc, int cov) {
                f << p2u(p) << "\t111\t222\t" << alb << "\t" << aa << "\t" << art << "\t" << ttl
                  << "\t" << trk << "\t" << disc << "\t0\t44100\t16\t2\t60.0\t" << cov
                  << "\tRock\t\t0\t0\t0\n";
            };
            // ① 一目录多专辑（BUG 现场的最小复现）：hasCover=1 是因为**目录里有封面文件**
            row(dirMix / U("01.flac"), "混装甲", "甲", "甲", "t1", 1, 1, 1);
            row(dirMix / U("02.flac"), "混装乙", "乙", "乙", "t2", 1, 1, 1);
            row(dirMix / U("03.flac"), "混装丙", "丙", "丙", "t3", 1, 1, 1);
            // ② 一目录一专辑（回归基线：这里必须照旧能取到目录封面）
            row(dirOne / U("a.flac"), "单人专辑", "单人", "单人", "a", 1, 1, 1);
            row(dirOne / U("b.flac"), "单人专辑", "单人", "单人", "b", 2, 1, 1);
            // ③ 多碟装：两个目录、同一个专辑名 ⟹ 必须仍是一张专辑、共享封面
            row(dirCD1 / U("01.flac"), "双碟专辑", "双碟", "双碟", "c1", 1, 1, 1);
            row(dirCD2 / U("01.flac"), "双碟专辑", "双碟", "双碟", "c2", 1, 2, 1);
            // ④ 一目录多专辑 + 其中一首有真内嵌
            if (embdReady) row(flacEmb, "有内嵌", "内", "内", "e1", 1, 1, 1);
            row(dirEmbd / U("没内嵌.flac"), "没内嵌", "无", "无", "e2", 1, 1, 1);
        }
        // 切库：先清旧根、再指新库目录（顺序与 [5]/[6] 的 scanLib 一致，否则会扫到用户真实目录）
        for (auto r : lib.roots()) lib.removeRoot(r);
        _putenv_s("RLPLAYER_LIBRARY_DIR", p2u(libH).c_str());
        _putenv_s("RLPLAYER_PORT", "");
        lib.addRootEx(p2u(rootH));
        lib.load();
        ck(lib.trackCount() == (size_t)(embdReady ? 9 : 8), "夹具索引载入", std::to_string(lib.trackCount()));

        auto albumByName = [&](const char* nm, rta::net::LibAlbum& out) {
            for (const auto& a : lib.albums()) if (a.name == nm) { out = a; return true; }
            return false;
        };

        // ---- ① 一目录多专辑：目录封面不得外借 ----
        rta::net::LibAlbum a1;
        ck(albumByName("混装甲", a1), "① 混装甲存在");
        ck(a1.dirAlbums == 3, "① 该目录被认出有 3 张专辑（dirAlbums=3）", std::to_string(a1.dirAlbums));
        ck(rta::net::lib_has_cover_file(p2u(dirMix / U("01.flac"))),
           "① 夹具成立：该目录里确实有 cover.jpg（不是「本来就没图」）");
        ck(!a1.hasCover && a1.coverSrc == rta::net::kCoverNone,
           "① 目录封面不得外借 ⟹ hasCover=false、src=none",
           std::string("hasCover=") + (a1.hasCover ? "1" : "0") + " src=" + std::to_string(a1.coverSrc));
        {
            std::vector<unsigned char> b; std::string m;
            ck(!lib.albumCover(a1.key, b, m), "① albumCover() 返回 false（/api/lib/cover 会回 404，前端露首字母）");
        }

        // ---- ② 一目录一专辑：照旧可用（覆盖率不回归）----
        rta::net::LibAlbum b1;
        ck(albumByName("单人专辑", b1), "② 单人专辑存在");
        ck(b1.hasCover && b1.coverSrc == rta::net::kCoverDirFile, "② 一目录一专辑：目录封面照旧可用（src=dir）",
           std::string("src=") + std::to_string(b1.coverSrc));
        {
            std::vector<unsigned char> b; std::string m;
            const bool ok = lib.albumCover(b1.key, b, m);
            const auto exp = readBytes(dirOne / U("cover.jpg"));
            ck(ok && !b.empty() && b == exp, "② albumCover() 返回的正是该目录的 cover.jpg（逐字节相同）",
               std::to_string(b.size()) + " vs " + std::to_string(exp.size()));
        }

        // ---- ③ 多碟装：仍一张专辑、仍共享封面 ----
        rta::net::LibAlbum c1;
        ck(albumByName("双碟专辑", c1), "③ 双碟专辑存在");
        ck(c1.nTracks == 2, "③ CD1/CD2 两个目录仍合成一张专辑（2 首）", std::to_string(c1.nTracks));
        ck(c1.dirAlbums == 1, "③ 目录键归一后只有 1 张专辑（dirAlbums=1）", std::to_string(c1.dirAlbums));
        ck(c1.hasCover && c1.coverSrc == rta::net::kCoverDirFile, "③ 多碟装仍用目录封面（src=dir，未被误伤）");
        {
            std::vector<unsigned char> b; std::string m;
            const bool ok = lib.albumCover(c1.key, b, m);
            const auto exp = readBytes(dirCD1 / U("cover.jpg"));
            ck(ok && !b.empty() && b == exp, "③ 共享的是第一张碟自己目录里的封面", std::to_string(b.size()));
            bool orderOk = c1.idx.size() == 2;
            if (orderOk) {
                rta::net::LibTrack t0, t1;
                lib.trackAt(c1.idx[0], t0); lib.trackAt(c1.idx[1], t1);
                orderOk = (t0.discNo == 1 && t1.discNo == 2);
            }
            ck(orderOk, "③ 合并后仍按碟号排序（CD1 在前、CD2 在后）");
        }

        // ---- ④ 一目录多专辑 + 真内嵌：取自己的图，不是目录那张 ----
        if (embdReady) {
            rta::net::LibAlbum d1;
            ck(albumByName("有内嵌", d1), "④ 有内嵌的专辑存在");
            ck(d1.hasCover && d1.coverSrc == rta::net::kCoverEmbedded,
               "④ 同目录两张专辑：有内嵌的那张判为 embed",
               std::string("hasCover=") + (d1.hasCover ? "1" : "0") + " src=" + std::to_string(d1.coverSrc));
            std::vector<unsigned char> b; std::string m;
            const bool ok = lib.albumCover(d1.key, b, m);
            const auto dirCov = readBytes(dirEmbd / U("cover.jpg"));
            ck(ok && b.size() > 1024 && b != dirCov,
               "④ 取到的是它自己的内嵌图，绝不是同目录那张 cover.jpg",
               std::to_string(b.size()) + " vs dir=" + std::to_string(dirCov.size()));
            std::string em;
            const auto emb = rta::net::lib_cover_embedded_bytes(p2u(flacEmb), em);
            ck(!emb.empty() && b == emb, "④ 与该 FLAC 的内嵌封面逐字节相同", std::to_string(emb.size()));
            rta::net::LibAlbum d2;
            ck(albumByName("没内嵌", d2), "④ 同目录里没内嵌的那张存在");
            ck(!d2.hasCover && d2.coverSrc == rta::net::kCoverNone,
               "④ 同目录里没内嵌的那张：不显示（而不是显示别人的图）",
               std::string("hasCover=") + (d2.hasCover ? "1" : "0"));
            std::vector<unsigned char> b2; std::string m2;
            ck(!lib.albumCover(d2.key, b2, m2), "④ 它的 albumCover() 也是 false（端点 404，不是共享图）");
        }

        // ---- ⑤ 重读索引后判定不变（来源不落盘、由路径 + 目录计数现算）----
        lib.load();
        rta::net::LibAlbum again;
        ck(albumByName("混装甲", again) && !again.hasCover && again.coverSrc == rta::net::kCoverNone,
           "⑤ 重新载入索引后 ① 的结论不变（不依赖任何落盘的新列）");
        rta::net::LibAlbum again2;
        ck(albumByName("单人专辑", again2) && again2.hasCover && again2.coverSrc == rta::net::kCoverDirFile,
           "⑤ 重新载入索引后 ② 的结论不变");
        fs::remove_all(tmp8, ec);
    }


    // =======================================================================
    // [9] 单文件封面（GET /api/cover?file=）：库内文件按专辑来源取图，库外保持旧行为
    //
    // 为什么单开一节（2026-10-27 第二轮）：/api/cover 是**单文件**接口，老实现直接 get_cover()，
    //   而它第 1 步就是"同目录 cover.jpg 优先" ⟹ 一目录多专辑时底栏/单曲页会把别人那张图发给这首
    //   （与 /api/lib/cover 同源，只是入口不同；实测 2023 目录 10 首都返回 229855B/63d90cda…）。
    // 判据：
    //   ① 多专辑目录里有内嵌的那首 ⟹ 返回**它自己**的内嵌图（≠ 目录那张）；
    //   ② 同专辑里没内嵌的那首 ⟹ 退到**同一张专辑**的内嵌图（仍是同一张碟，不是目录图）；
    //   ③ 同目录里没内嵌的那张专辑 ⟹ 返回空（不借别人的），而不是目录图；
    //   ④ 一目录一专辑 ⟹ 照旧用目录封面（覆盖率不回归）；
    //   ⑤ 不在库里的文件（拖进来的临时文件）⟹ trackCover 返回 false，调用方仍走 get_cover（同目录优先）。
    // =======================================================================
    std::printf("\n== [9] 单文件封面：库内按来源、库外保持旧行为 ==\n");
    {
        auto writeJpg = [](const fs::path& p, size_t n, unsigned char fill) {
            std::vector<char> b(n, (char)fill);
            b[0] = (char)0xFF; b[1] = (char)0xD8; b[2] = (char)0xFF; b[3] = (char)0xE0;
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write(b.data(), (std::streamsize)b.size());
        };
        auto readBytes = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        const fs::path tmp9 = fs::temp_directory_path() / "rta_lib_filecover";
        fs::remove_all(tmp9, ec);
        const fs::path libJ   = tmp9 / U("libJ");
        const fs::path rootJ  = tmp9 / U("音乐库J");
        const fs::path dirMix = rootJ / U("混装单曲");
        const fs::path dirOne = rootJ / U("单专辑");
        const fs::path dirOut = tmp9 / U("库外目录");
        for (const fs::path& d : { dirMix, dirOne, dirOut }) fs::create_directories(d, ec);
        writeJpg(dirMix / U("cover.jpg"), 4096, 0x77);
        writeJpg(dirOne / U("cover.jpg"), 2048, 0x88);
        writeJpg(dirOut / U("cover.jpg"), 3072, 0x99);

        // 真 FLAC：① 有内嵌封面 ② 同专辑但无内嵌（要 ffmpeg；缺工具则 ①② 只能跳过）
        const std::string ffmpeg = rta::audio::find_tool("ffmpeg");
        const fs::path flacEmb   = dirMix / U("有内嵌.flac");
        const fs::path flacNoPic = dirMix / U("无内嵌.flac");
        bool ffOk = false;
        if (!ffmpeg.empty()) {
            const fs::path tmpJpg = tmp9 / U("embed.jpg");
            auto runFF = [&](const std::wstring& c) { return rta::audio::run_hidden(c, nullptr); };
            runFF(L"\"" + rta::audio::widen(ffmpeg) + L"\" -v error -y -f lavfi -i \"testsrc2=s=320x320:d=1\""
                  L" -frames:v 1 -c:v mjpeg -q:v 2 \"" + rta::audio::widen(p2u(tmpJpg)) + L"\"");
            runFF(L"\"" + rta::audio::widen(ffmpeg) + L"\" -v error -y -f lavfi -i \"anullsrc=r=44100:cl=stereo\""
                  L" -i \"" + rta::audio::widen(p2u(tmpJpg)) + L"\" -map 0:a -map 1:v -c:a flac -c:v copy"
                  L" -disposition:v attached_pic -t 1 \"" + rta::audio::widen(p2u(flacEmb)) + L"\"");
            runFF(L"\"" + rta::audio::widen(ffmpeg) + L"\" -v error -y -f lavfi -i \"anullsrc=r=44100:cl=stereo\""
                  L" -c:a flac -t 1 \"" + rta::audio::widen(p2u(flacNoPic)) + L"\"");
            std::string em0;
            ffOk = fs::exists(flacEmb, ec) && fs::exists(flacNoPic, ec) &&
                   rta::net::lib_cover_embedded_bytes(p2u(flacEmb), em0).size() > 1024;
        }
        ck(ffOk, "夹具：两份真 FLAC（一份有内嵌、一份没有）");

        fs::create_directories(libJ, ec);
        {
            std::ofstream f(libJ / U("rlplayer-index.tsv"), std::ios::binary);
            f << "#libIndexV\t5\n";
            f << "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre"
                 "\tcue\tcueN\tcueOff\tcueLen\n";
            auto row = [&](const fs::path& p, const char* alb, const char* ttl, int trk, int cov) {
                f << p2u(p) << "\t111\t222\t" << alb << "\t" << alb << "\t" << alb << "\t" << ttl
                  << "\t" << trk << "\t1\t0\t44100\t16\t2\t60.0\t" << cov << "\tRock\t\t0\t0\t0\n";
            };
            if (ffOk) { row(flacEmb, "甲", "a1", 1, 1); row(flacNoPic, "甲", "a2", 2, 1); }
            row(dirMix / U("幽灵.flac"), "乙", "b1", 1, 1);          // 不存在 + hasCover=1（只有目录文件）
            row(dirOne / U("单曲.flac"), "单", "c1", 1, 1);
        }
        for (auto r : lib.roots()) lib.removeRoot(r);
        _putenv_s("RLPLAYER_LIBRARY_DIR", p2u(libJ).c_str());
        _putenv_s("RLPLAYER_PORT", "");
        lib.addRootEx(p2u(rootJ));
        lib.load();

        const auto dirCovMix = readBytes(dirMix / U("cover.jpg"));
        const auto dirCovOne = readBytes(dirOne / U("cover.jpg"));
        // 负向对照：get_cover 本身仍"同目录优先" —— 这正是必须由 trackCover 接管的原因
        {
            std::string m; const auto g = rta::audio::get_cover(p2u(dirMix / U("幽灵.flac")), m);
            ck(g == dirCovMix, "负向对照：get_cover() 对多专辑目录仍返回目录那张图（旧行为本身没改）");
        }
        {
            std::vector<unsigned char> b; std::string m;
            const bool known = lib.trackCover(p2u(dirMix / U("幽灵.flac")), b, m);
            ck(known, "③ 库内文件 → trackCover 认领（返回 true）");
            ck(b.empty(), "③ 同目录没有属于自己的图 ⟹ 返回空，绝不外借目录那张", std::to_string(b.size()));
        }
        {
            std::vector<unsigned char> b; std::string m;
            const bool known = lib.trackCover(p2u(dirOne / U("单曲.flac")), b, m);
            ck(known && b == dirCovOne, "④ 一目录一专辑：仍返回该目录的 cover.jpg（覆盖率不回归）",
               std::to_string(b.size()));
        }
        // ⑤ 库外文件：必须"认领失败"，由调用方走 get_cover（= 旧行为，同目录优先）
        {
            std::vector<unsigned char> b; std::string m;
            const bool known = lib.trackCover(p2u(dirOut / U("临时曲.flac")), b, m);
            ck(!known, "⑤ 不在库里的文件 → trackCover 返回 false（调用方保持旧行为）");
            std::string m2;
            const auto g = rta::audio::get_cover(p2u(dirOut / U("临时曲.flac")), m2);
            ck(g == readBytes(dirOut / U("cover.jpg")), "⑤ 库外文件的旧行为仍是「同目录封面优先」（未被改动）");
        }
        if (ffOk) {
            std::string em; const auto own = rta::net::lib_cover_embedded_bytes(p2u(flacEmb), em);
            std::vector<unsigned char> b1; std::string m1;
            const bool k1 = lib.trackCover(p2u(flacEmb), b1, m1);
            ck(k1 && !own.empty() && b1 == own && b1 != dirCovMix,
               "① 多专辑目录里：返回它**自己**的内嵌图，不是目录那张",
               std::to_string(b1.size()) + " vs dir=" + std::to_string(dirCovMix.size()));
            std::vector<unsigned char> b2; std::string m2;
            const bool k2 = lib.trackCover(p2u(flacNoPic), b2, m2);
            ck(k2 && b2 == own, "② 同专辑里没内嵌的那首：退到同一张专辑的内嵌图（不是目录图）",
               std::to_string(b2.size()));
        }
        // ⑥ /api/meta 的 hasCover 口径（第三轮修）：与 /api/cover 同一份判据 —— 逐字复制那一行逻辑
        auto metaHasCover = [&](const fs::path& p) {
            std::vector<unsigned char> b; std::string m;
            if (!lib.trackCover(p2u(p), b, m)) b = rta::audio::get_cover(p2u(p), m);
            return b.size() > 128;
        };
        ck(!metaHasCover(dirMix / U("幽灵.flac")),
           "⑥ /api/meta 口径：多专辑目录里没有自有图的那首 hasCover=false（旧口径 = true）");
        ck(metaHasCover(dirOne / U("单曲.flac")),
           "⑥ /api/meta 口径：一目录一专辑仍有图（hasCover=true，不回归）");
        if (ffOk) ck(metaHasCover(flacEmb), "⑥ /api/meta 口径：有内嵌的那首 hasCover=true");

        // 重启后一致
        lib.load();
        {
            std::vector<unsigned char> b; std::string m;
            const bool known = lib.trackCover(p2u(dirMix / U("幽灵.flac")), b, m);
            ck(known && b.empty(), "重新载入索引后 ③ 的结论不变");
        }
        fs::remove_all(tmp9, ec);
    }

    fs::remove_all(tmp2, ec);

    fs::current_path(oldCwd, ec);
    fs::remove_all(tmp, ec);

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}