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

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== 音乐库索引验证 ==\n");

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
        f << "#libIndexV\t3\n";
        f << "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre\n";
        auto row = [&](const std::string& path, const std::string& alb, const std::string& aa,
                       const std::string& art, const std::string& ttl, int trk, int disc,
                       int yr, int sr, int bits, int ch, double dur, int cov) {
            f << path << "\t111\t222\t" << alb << "\t" << aa << "\t" << art << "\t" << ttl
              << "\t" << trk << "\t" << disc << "\t" << yr << "\t" << sr << "\t" << bits
              << "\t" << ch << "\t" << dur << "\t" << cov << "\tRock\n";
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
    ck(header == "#path\tmtime\tsize\talbum\talbumArtist\tartist\ttitle\ttrack\tdisc\tyear\tsr\tbits\tch\tdur\thasCover\tgenre",
       "索引表头逐字不变（16 列、顺序不变 → 老索引可继续读）", header);
    ck(rows.size() == 8, "索引 8 行", std::to_string(rows.size()));
    bool all16 = true;
    for (auto& r : rows) if (r.size() != 16) all16 = false;
    ck(all16, "每行 16 列（没有新增列）");
    std::map<std::string, std::pair<std::string, int>> boxRows;
    for (auto& r : rows)
        if (r.size() == 16 && r[0].find("BoxSetAlbum") != std::string::npos)
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
                        if (f.size() == 16) { f[3] = ""; f[8] = "0"; }   // 模拟老索引：album 空、disc 0
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
        ck(idx2.find("BoxSetAlbum") != std::string::npos && idx2.find("#libIndexV\t3") != std::string::npos,
           "自愈结果已重新落盘（版本头仍是 v3）");
    }

    fs::remove_all(tmp2, ec);

    fs::current_path(oldCwd, ec);
    fs::remove_all(tmp, ec);

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
