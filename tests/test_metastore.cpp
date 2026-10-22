// test_metastore.cpp — MetaBase 本地元数据存储：哈希 / 字段合并 / 存取往返 / 目录隔离
//
// 判据说明（铁律 3）：
//   * hashOf 的正确性不只看「稳定/不同」，还对照 net/library.h 的 fnv_hex
//     （同算法同常数是设计要求，保证与专辑键体系一致）；
//   * 往返测试特意放入中文/引号/反斜杠/换行/制表/独立 \r，验证转义与 UTF-8 原样存储；
//   * 损坏 JSON 用乱码/截断/空文件三种形态，判据是 load 返回 false 而非不崩溃。
// 不依赖网络，不碰真实的 rlplayer-meta/（baseDir 切到临时目录）。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "meta/store.h"
#include "net/library.h"   // 仅测试用：校验 hashOf 与 fnv_hex 同常数同算法
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using rta::meta::MetaStore;
using rta::meta::TrackMeta;
using rta::meta::MetaValue;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

static std::string read_file_bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// fs::path -> UTF-8 窄串（不经 ANSI 代码页，临时目录含非 ASCII 时也不乱）
static std::string u8s(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string((const char*)u.data(), u.size());
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== MetaBase 元数据存储验证 ==\n");

    // 隔离：baseDir 切到临时目录，不碰真实的 rlplayer-meta/
    const fs::path tmp = fs::temp_directory_path() / "rta_meta_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::string baseU8 = u8s(tmp);

    // ---- 1) hashOf ----
    std::printf("[1] hashOf\n");
    ck(MetaStore::hashOf("D:/Music/a.flac") == MetaStore::hashOf("D:/Music/a.flac"),
       "hashOf 同路径两次调用相同");
    ck(MetaStore::hashOf("D:/Music/a.flac") != MetaStore::hashOf("D:/Music/b.flac"),
       "hashOf 不同路径不同");
    const std::string h1 = MetaStore::hashOf("D:/Music/a.flac");
    ck(h1.size() == 16 && h1.find_first_not_of("0123456789abcdef") == std::string::npos,
       "hashOf 是 16 位小写十六进制", h1);
    ck(MetaStore::hashOf("D:/音乐/晴天.flac") == rta::net::fnv_hex("D:/音乐/晴天.flac"),
       "hashOf 与 library.h fnv_hex 完全一致（含 UTF-8 路径）");
    ck(MetaStore::hashOf("") == rta::net::fnv_hex(""),
       "hashOf 与 fnv_hex 空串一致");

    // ---- 2) mergeField ----
    std::printf("[2] mergeField\n");
    {   // 非空可写入空字段（任何来源，含 rank 0 的未知来源）
        TrackMeta tm; tm.path = "p1";
        MetaValue v; v.value = "晴天"; v.source = "llm"; v.ts = 10; v.confidence = 0.3f;
        MetaStore::mergeField(tm, "title", v);
        MetaValue w; w.value = "流行"; w.source = "unknown-src"; w.ts = 1;
        MetaStore::mergeField(tm, "genre", w);
        ck(tm.fields["title"].value == "晴天" && tm.fields["title"].source == "llm",
           "空字段任何来源可写入（llm）");
        ck(tm.fields["genre"].value == "流行" && tm.fields["genre"].source == "unknown-src",
           "空字段任何来源可写入（未知来源 rank 0）");
    }
    {   // user 覆盖 itunes（即使 confidence/ts 都更低）
        TrackMeta tm;
        MetaValue a; a.value = "专辑版"; a.source = "itunes"; a.ts = 100; a.confidence = 0.9f;
        MetaStore::mergeField(tm, "album", a);
        MetaValue u; u.value = "我改的"; u.source = "user"; u.ts = 1; u.confidence = 0.1f;
        MetaStore::mergeField(tm, "album", u);
        ck(tm.fields["album"].value == "我改的" && tm.fields["album"].source == "user",
           "user 覆盖 itunes（无视 confidence/ts）");
    }
    {   // llm 永远赢不了事实源
        TrackMeta tm;
        MetaValue q; q.value = "晴天"; q.source = "qq"; q.ts = 100; q.confidence = 0.5f;
        MetaStore::mergeField(tm, "title", q);
        MetaValue l; l.value = "AI 幻觉"; l.source = "llm"; l.ts = 999999; l.confidence = 1.0f;
        MetaStore::mergeField(tm, "title", l);
        ck(tm.fields["title"].value == "晴天" && tm.fields["title"].source == "qq",
           "llm 高置信度高 ts 也赢不了事实源");
    }
    {   // 同级比 confidence
        TrackMeta tm;
        MetaValue a; a.value = "低分"; a.source = "netease"; a.ts = 5; a.confidence = 0.4f;
        MetaStore::mergeField(tm, "artist", a);
        MetaValue b; b.value = "高分"; b.source = "netease"; b.ts = 1; b.confidence = 0.8f;
        MetaStore::mergeField(tm, "artist", b);
        ck(tm.fields["artist"].value == "高分", "同级 confidence 高者胜");
        MetaValue c; c.value = "更低"; c.source = "netease"; c.ts = 999; c.confidence = 0.2f;
        MetaStore::mergeField(tm, "artist", c);
        ck(tm.fields["artist"].value == "高分", "同级 confidence 低者被忽略");
    }
    {   // 同级同 confidence：ts 新者胜
        TrackMeta tm;
        MetaValue a; a.value = "旧"; a.source = "wiki"; a.ts = 100; a.confidence = 0.6f;
        MetaStore::mergeField(tm, "composer", a);
        MetaValue b; b.value = "新"; b.source = "wiki"; b.ts = 200; b.confidence = 0.6f;
        MetaStore::mergeField(tm, "composer", b);
        ck(tm.fields["composer"].value == "新", "同级同 confidence：ts 新者胜");
        MetaValue c; c.value = "更旧"; c.source = "wiki"; c.ts = 50; c.confidence = 0.6f;
        MetaStore::mergeField(tm, "composer", c);
        ck(tm.fields["composer"].value == "新", "同级同 confidence：ts 旧者被忽略");
    }
    {   // 空值永不覆盖非空（user 也不例外）
        TrackMeta tm;
        MetaValue a; a.value = "叶惠美"; a.source = "tag"; a.ts = 1;
        MetaStore::mergeField(tm, "album", a);
        MetaValue e; e.value = ""; e.source = "user"; e.ts = 999;
        MetaStore::mergeField(tm, "album", e);
        ck(tm.fields["album"].value == "叶惠美" && tm.fields["album"].source == "tag",
           "user 的空值不覆盖非空");
    }
    {   // 字段值为空串时，非空值可写入
        TrackMeta tm;
        MetaValue e; e.value = ""; e.source = "qq"; e.ts = 5;
        MetaStore::mergeField(tm, "genre", e);
        MetaValue g; g.value = "Mandopop"; g.source = "llm"; g.ts = 1; g.confidence = 0.2f;
        MetaStore::mergeField(tm, "genre", g);
        ck(tm.fields["genre"].value == "Mandopop" && tm.fields["genre"].source == "llm",
           "非空值可写入空值字段（空字段任何来源可写）");
    }

    // ---- 3) setBaseDir 到临时目录 + save/load 往返 ----
    std::printf("[3] save/load 往返\n");
    MetaStore& ms = MetaStore::instance();
    ms.setBaseDir(baseU8);
    ck(ms.baseDir() == baseU8, "baseDir 已切到临时目录", ms.baseDir());

    TrackMeta tm;
    tm.path = "D:\\Music\\叶惠美\\01 晴天.flac";
    tm.coverRelPath = "covers/deadbeefdeadbeef.jpg";
    { MetaValue v; v.value = "晴天";                          v.source = "qq";          v.ts = 1792000000; v.confidence = 0.9f;  tm.fields["title"] = v; }
    { MetaValue v; v.value = "周杰伦 \"Jay\" Chou\\作品";      v.source = "user";        v.ts = 1792000001; v.confidence = 1.f;   tm.fields["artist"] = v; }
    { MetaValue v; v.value = "叶惠美";                        v.source = "tag";         v.ts = 100;        v.confidence = 0.99f; tm.fields["album"] = v; }
    { MetaValue v; v.value = "第一行\n第二行\r\n第三行\t缩进";   v.source = "lrclib";      v.ts = 1792000002; v.confidence = 0.85f; tm.fields["lyrics"] = v; }
    { MetaValue v; v.value = "2003";                          v.source = "itunes";      v.ts = 5;          v.confidence = 0.7f;  tm.fields["year"] = v; }
    { MetaValue v; v.value = "方文山";                        v.source = "musicbrainz"; v.ts = 6;          v.confidence = 0.95f; tm.fields["composer"] = v; }
    ms.save(tm);

    const std::string h = MetaStore::hashOf(tm.path);
    ck(fs::exists(rta::meta::meta_path_of(baseU8) / "tracks" / (h + ".json"), ec),
       "save 落在 <base>/tracks/<hash>.json");

    // 原始文件检查：中文原样 UTF-8（不 \u 转义），引号/反斜杠/换行/回车/制表被转义
    const std::string raw = read_file_bytes(rta::meta::meta_path_of(baseU8) / "tracks" / (h + ".json"));
    ck(!raw.empty(), "JSON 文件已写出");
    ck(raw.find("晴天") != std::string::npos && raw.find("叶惠美") != std::string::npos,
       "中文原样 UTF-8 写出");
    ck(raw.find("\\u53f6") == std::string::npos && raw.find("\\u6674") == std::string::npos,
       "中文不做 \\u 转义");
    ck(raw.find("\\\"Jay\\\"") != std::string::npos, "引号被转义为 \\\"");
    ck(raw.find("Chou\\\\作品") != std::string::npos, "反斜杠被转义为 \\\\");
    ck(raw.find("第一行\\n第二行\\r\\n第三行\\t缩进") != std::string::npos,
       "换行/回车/制表被转义（含孤立 \\r）");

    TrackMeta rt;
    const bool loaded = ms.load(tm.path, rt);
    ck(loaded, "load 读回成功");
    if (loaded) {
        ck(rt.path == tm.path, "path 往返一致", rt.path);
        ck(rt.coverRelPath == tm.coverRelPath, "coverRelPath 往返一致", rt.coverRelPath);
        ck(rt.fields.size() == tm.fields.size(),
           "字段数一致", std::to_string(rt.fields.size()) + " vs " + std::to_string(tm.fields.size()));
        bool allEq = true;
        std::string diff;
        for (const auto& kv : tm.fields) {
            auto it = rt.fields.find(kv.first);
            if (it == rt.fields.end()) { allEq = false; diff = kv.first + " 缺失"; break; }
            const MetaValue& a = kv.second;
            const MetaValue& b = it->second;
            if (a.value != b.value || a.source != b.source || a.ts != b.ts ||
                a.confidence != b.confidence) {
                allEq = false;
                diff = kv.first + ": [" + b.value + "|" + b.source + "|" +
                       std::to_string(b.ts) + "|" + std::to_string(b.confidence) + "]";
                break;
            }
        }
        ck(allEq, "全部字段的 value/source/ts/confidence 往返一致", diff);
    }

    // ---- 4) 损坏 / 缺失 ----
    std::printf("[4] 损坏与缺失\n");
    TrackMeta no;
    ck(!ms.load("D:/不存在_从未保存.flac", no), "未保存的路径 load 返回 false");
    {
        const fs::path cf = rta::meta::meta_path_of(baseU8) / "tracks" /
                            (MetaStore::hashOf("D:/corrupt.flac") + ".json");
        { std::ofstream f(cf, std::ios::binary); f << "this is { not json !!!"; }
        TrackMeta bad;
        ck(!ms.load("D:/corrupt.flac", bad), "乱码 JSON load 返回 false");
    }
    {
        const fs::path cf = rta::meta::meta_path_of(baseU8) / "tracks" /
                            (MetaStore::hashOf("D:/trunc.flac") + ".json");
        { std::ofstream f(cf, std::ios::binary); f << "{\"path\":\"x\",\"fields\":"; }
        TrackMeta bad;
        ck(!ms.load("D:/trunc.flac", bad), "截断 JSON（fields 非对象）load 返回 false");
    }
    {
        const fs::path cf = rta::meta::meta_path_of(baseU8) / "tracks" /
                            (MetaStore::hashOf("D:/empty.flac") + ".json");
        { std::ofstream f(cf, std::ios::binary); }
        TrackMeta bad;
        ck(!ms.load("D:/empty.flac", bad), "空文件 load 返回 false");
    }

    // ---- 封面 ----
    std::printf("[5] 封面\n");
    {
        const std::vector<unsigned char> jpg = { 0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4 };
        const std::string rel = ms.saveCover("D:/coverA.flac", jpg.data(), jpg.size(), "jpg");
        const std::string want = "covers/" + MetaStore::hashOf("D:/coverA.flac") + ".jpg";
        ck(rel == want, "saveCover 返回相对路径 covers/<hash>.jpg", rel);
        std::vector<unsigned char> got; std::string mime;
        ck(ms.cover("D:/coverA.flac", got, mime) && got == jpg && mime == "image/jpeg",
           "cover 读回字节与 mime=image/jpeg", mime);
    }
    {
        const std::vector<unsigned char> png = { 0x89, 'P', 'N', 'G', 9, 9 };
        const std::string rel = ms.saveCover("D:/coverB.flac", png.data(), png.size(), ".PNG");
        const bool okExt = rel.size() > 4 && rel.substr(rel.size() - 4) == ".png";
        ck(okExt, "ext 大小写与前置点归一为 .png", rel);
        std::vector<unsigned char> got; std::string mime;
        ck(ms.cover("D:/coverB.flac", got, mime) && got == png && mime == "image/png",
           "png 封面读回 mime=image/png", mime);
    }
    {
        const std::vector<unsigned char> b = { 1, 2, 3 };
        ck(ms.saveCover("D:/x.flac", b.data(), b.size(), "webp").empty(),
           "cover() 读不回的扩展名（webp）被拒绝");
        ck(ms.saveCover("D:/x.flac", nullptr, 0, "jpg").empty(), "空数据被拒绝");
        std::vector<unsigned char> got; std::string mime;
        ck(!ms.cover("D:/无封面.flac", got, mime), "无封面返回 false");
    }

    fs::remove_all(tmp, ec);

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
