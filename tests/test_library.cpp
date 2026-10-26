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

    fs::remove_all(tmp2, ec);

    fs::current_path(oldCwd, ec);
    fs::remove_all(tmp, ec);

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}