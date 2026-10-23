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

    fs::current_path(oldCwd, ec);
    fs::remove_all(tmp, ec);

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
