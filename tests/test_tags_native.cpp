// test_tags_native.cpp — 原生标签快速通道（261025-01 一.1/一.8：首次全量扫描 15000 首要等几分钟）
//
// 判据（铁律 3：每条判据都能被单独否定）：
//   [1] 单元：**手工构造** 4 类容器的字节流，逐字段断言解析结果；同时断言「该回退的必须回退」：
//       RF64 / 短 bext（<337）/ ID3v2.2 / unsynchronisation / 帧标志位 / 数字流派引用 / Opus /
//       FLAC total_samples=0 / OGG 末页缺失 …… 一条条点名，回退错了就是红。
//       WAV 的 "id3 " 块与 bext 现在有 10 组正向夹具：两种块顺序（LIST→id3 / id3→LIST，断言的是
//       **先到先得** —— oracle 是 lib_probe_one 的实际行为，不是 ffmpeg 的语义，见 [1] 里的长注释）、
//       bext 的 origination_date（含"bext 在前才生效"与"空日期不占位"）、块内重复键的两条反向规则、
//       APIC 封面与描述补缺（描述压过 TIT2/INAM）、数字流派是否真的入库、块内不是 ID3v2、ID3v2.4 ——
//       除了断言字段值，每组还走一遍 [2] 的 A/B。
//   [2] A/B（唯一能证明「开关变量不影响入库结果」的判据）：build/testdata 与 MUSIC 里各取真实文件
//       （FLAC/WAV/MP3/OGG 各 5 个），用 lib_probe_one 在 RLPLAYER_NATIVE_TAGS=1/0 下逐字段比对
//       （title/artist/album/albumArtist/genre/year/trackNo/discNo/sr/bits/ch/hasCover + 时长 ±1 帧）。
//       实现抽成 abOne()，[1] 的夹具与 [5] 的全量扫描用的是**同一条判据**。
//   [3] 吞吐：N 个「1 秒静音 FLAC」的**首次全量扫描**（Library::startScan(true)，与 /api/lib/scan 同一入口）
//       对比「每个文件起一次 ffprobe」的基线，给出首/s 与单首耗时（附 3000/15000 首的推算）。
//   [4] 回归：同一份夹具，RLPLAYER_NATIVE_TAGS=0/1 各扫一次，**索引文件必须逐字节相同**
//       （列数/字段/时长小数位全都要对得上，只比字段不算数）。
//   [5] WAV 专项（261025 收尾）：MUSIC 的 71 个 WAV **逐个** A/B（0 差异）、原生命中数必须 71/71
//       （改动前 19/71，差的 52 个就是 51 个 "id3 " 块 + 1 个 bext），并把整个 MUSIC 曲库
//       用 Library 全量扫两遍（RLPLAYER_NATIVE_TAGS=0/1）比索引字节。
//
// ⚠️ 未解决项（铁律 4）：
//   1) WAV 的 iXML/axml 仍**主动回退** ffprobe：XML 字段到 ffprobe 键的映射没实测过，
//      本库 71 个 WAV 里 0 个样本。同理 "id3 " 块里的 ID3v2.2/unsynchronisation/帧标志位，
//      以及 bext 长度 < 337（ffmpeg 会整份拒绝）——这些继续回退，宁可慢一次。
//   2) MUSIC 不存在时 [2]/[5] 只用 build/testdata 的文件并如实打印「真实文件数不足」，不假装覆盖。
//   3) [5] 的全库扫描要起 655 次 ffprobe（一遍），ctest 里约多花 15~20 s；这是"索引逐字节"这条
//      判据的成本，不能靠抽查代替。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "net/library.h"
#include "meta/tags_native.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <direct.h>

namespace fs = std::filesystem;
using rta::net::Library;
using rta::net::LibTrack;
using rta::meta::native::TagInfo;

static int g_fail = 0, g_pass = 0;
static void ck(bool cond, const std::string& what, const std::string& detail = "") {
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", what.c_str()); }
    else { ++g_fail; std::printf("  [FAIL] %s  %s\n", what.c_str(), detail.c_str()); }
}

static std::wstring W(const std::string& s) {
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
static std::string U8(const fs::path& p) { auto u = p.u8string(); return std::string((const char*)u.data(), u.size()); }

// ---------------------------------------------------------------------------
// 编码辅助
// ---------------------------------------------------------------------------
static void put8(std::vector<unsigned char>& v, uint32_t x)  { v.push_back((unsigned char)(x & 0xFF)); }
static void put16le(std::vector<unsigned char>& v, uint32_t x) { put8(v, x); put8(v, x >> 8); }
static void put32le(std::vector<unsigned char>& v, uint32_t x) { put16le(v, x & 0xFFFF); put16le(v, x >> 16); }
static void put16be(std::vector<unsigned char>& v, uint32_t x) { put8(v, x >> 8); put8(v, x); }
static void put32be(std::vector<unsigned char>& v, uint32_t x) { put16be(v, x >> 16); put16be(v, x & 0xFFFF); }
static void put(std::vector<unsigned char>& v, const void* p, size_t n) {
    const unsigned char* c = (const unsigned char*)p; v.insert(v.end(), c, c + n);
}
static void putStr(std::vector<unsigned char>& v, const std::string& s) { put(v, s.data(), s.size()); }
static void syncsafe32(std::vector<unsigned char>& v, uint32_t x) {
    put8(v, (x >> 21) & 0x7F); put8(v, (x >> 14) & 0x7F); put8(v, (x >> 7) & 0x7F); put8(v, x & 0x7F);
}
static void writeFile(const fs::path& p, const std::vector<unsigned char>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)b.data(), (std::streamsize)b.size());
}

// Vorbis comment：vendor + [KEY=value]*
static std::vector<unsigned char> vorbisComment(const std::vector<std::pair<std::string, std::string>>& kv,
                                                const std::string& vendor = "test") {
    std::vector<unsigned char> v;
    put32le(v, (uint32_t)vendor.size()); putStr(v, vendor);
    put32le(v, (uint32_t)kv.size());
    for (auto& p : kv) {
        const std::string s = p.first + "=" + p.second;
        put32le(v, (uint32_t)s.size()); putStr(v, s);
    }
    return v;
}

// ---- FLAC ----
struct FlacSpec {
    int sr = 48000, ch = 2, bps = 24;
    long long total = 96000;                       // 2.0 s @48k；0 = 流式（未知）
    std::vector<std::pair<std::string, std::string>> tags;
    bool picture = false;
    std::string pictureMime = "image/jpeg";
    bool id3prefix = false;
};
static std::vector<unsigned char> buildFlac(const FlacSpec& s) {
    std::vector<unsigned char> out;
    if (s.id3prefix) {                             // 前置一个空 ID3v2.3 标签（ffprobe 对 FLAC 也一样跳过）
        putStr(out, "ID3"); out.push_back(3); out.push_back(0); out.push_back(0);
        syncsafe32(out, 10); putStr(out, "1234567890");
    }
    putStr(out, "fLaC");
    struct Block { int type; std::vector<unsigned char> body; };
    std::vector<Block> blocks;
    {
        std::vector<unsigned char> si(34, 0);
        si[0] = 0x10; si[2] = 0x10;
        const unsigned long long v = ((unsigned long long)s.sr << 44) |
                                     ((unsigned long long)(s.ch - 1) << 41) |
                                     ((unsigned long long)(s.bps - 1) << 36) |
                                     (unsigned long long)s.total;
        for (int i = 0; i < 8; ++i) si[10 + i] = (unsigned char)((v >> (56 - 8 * i)) & 0xFF);
        blocks.push_back({0, si});
    }
    blocks.push_back({4, vorbisComment(s.tags)});
    if (s.picture) {
        std::vector<unsigned char> pic;
        put32be(pic, 3);                                   // picture type = front cover
        put32be(pic, (uint32_t)s.pictureMime.size()); putStr(pic, s.pictureMime);
        put32be(pic, 0);                                   // description
        put32be(pic, 1); put32be(pic, 1); put32be(pic, 24); put32be(pic, 0);
        const std::string img = "\xFF\xD8\xFF\xE0 dummy";
        put32be(pic, (uint32_t)img.size()); putStr(pic, img);
        blocks.push_back({6, pic});
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        const bool last = (i + 1 == blocks.size());
        out.push_back((unsigned char)((last ? 0x80 : 0x00) | blocks[i].type));
        const uint32_t n = (uint32_t)blocks[i].body.size();
        put8(out, (n >> 16) & 0xFF); put8(out, (n >> 8) & 0xFF); put8(out, n & 0xFF);
        put(out, blocks[i].body.data(), blocks[i].body.size());
    }
    return out;
}

// ---- ID3v2 帧构造（MP3 的 ID3v2 前缀与 WAV 的 "id3 " 块共用同一套帧）----
static void putUtf16le(std::vector<unsigned char>& body, const std::string& utf8) {   // UTF-8 -> UTF-16LE
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return;
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), w.data(), n);
    for (wchar_t c : w) { body.push_back((unsigned char)(c & 0xFF)); body.push_back((unsigned char)((c >> 8) & 0xFF)); }
}
static void id3TextFrame(std::vector<unsigned char>& tag, int ver, const char* id, const std::string& value,
                         unsigned char enc = 1) {
    std::vector<unsigned char> body;
    body.push_back(enc);
    if (enc == 1) {                                             // UTF-16LE + BOM
        body.push_back(0xFF); body.push_back(0xFE);
        putUtf16le(body, value);
        body.push_back(0); body.push_back(0);
    } else {
        putStr(body, value);
    }
    putStr(tag, id);
    if (ver == 4) syncsafe32(tag, (uint32_t)body.size()); else put32be(tag, (uint32_t)body.size());
    put16be(tag, 0);
    put(tag, body.data(), body.size());
}
static void id3ApicFrame(std::vector<unsigned char>& tag, int ver, const std::string& mime,
                         const std::string& desc, const std::string& img = "\xFF\xD8\xFF\xE0 dummy") {
    std::vector<unsigned char> body;
    body.push_back(0);                                          // ISO-8859-1
    putStr(body, mime); body.push_back(0);
    body.push_back(3);                                          // front cover
    putStr(body, desc); body.push_back(0);
    putStr(body, img);
    putStr(tag, "APIC");
    if (ver == 4) syncsafe32(tag, (uint32_t)body.size()); else put32be(tag, (uint32_t)body.size());
    put16be(tag, 0);
    put(tag, body.data(), body.size());
}
// ---- WAV ----
struct WavSpec {
    int sr = 44100, ch = 2, bits = 16, fmtTag = 1;
    long long frames = 44100;                      // 1.0 s
    long long dataDecl = -1;                       // -1 = 与真实字节一致；其它值用于「声明超长」
    std::vector<std::pair<std::string, std::string>> info;   // INAM/IART/IPRD/IGNR/ICRD/ITRK
    bool id3chunk = false, bext = false, rf64 = false, extensible = false;
    // ---- "id3 " 块（ID3v2 in RIFF）----
    bool id3First = false;                         // true ⟹ id3 块排在 LIST 之前（默认排在 LIST 之后）
    bool id3BadMagic = false;                      // 块内不是 ID3v2（ffmpeg 忽略它、不报错也不加标签）
    int  id3ver = 3;
    std::string id3Title = "ID3T", id3Artist, id3Album, id3AlbumArtist, id3Genre, id3Date, id3Track, id3Disc;
    std::string id3Title2;                         // 第二个 TIT2（同一个标签内应当「先到先得」）
    bool id3Apic = false;
    std::string id3ApicMime = "image/jpeg", id3ApicDesc;
    // ---- bext ----
    bool bextFirst = false;                        // true ⟹ bext 排在 LIST 之前
    std::string bextDate;                          // origination_date（空 ⟹ 不写；ffmpeg 也不写 date 键）
    long long bextSize = 602;                      // < 337 ⟹ ffprobe 整份拒绝，原生必须回退
};
static void wavChunk(std::vector<unsigned char>& out, const char* id, const std::vector<unsigned char>& body) {
    putStr(out, id); put32le(out, (uint32_t)body.size()); put(out, body.data(), body.size());
    if (body.size() & 1u) out.push_back(0);
}
static std::vector<unsigned char> buildWavId3Chunk(const WavSpec& s) {
    std::vector<unsigned char> tag;
    if (s.id3BadMagic) {
        const std::string junk = "XXXX-not-an-id3v2-tag";
        putStr(tag, junk);
        return tag;
    }
    std::vector<unsigned char> frames;
    if (!s.id3Title.empty())       id3TextFrame(frames, s.id3ver, "TIT2", s.id3Title);
    if (!s.id3Title2.empty())      id3TextFrame(frames, s.id3ver, "TIT2", s.id3Title2);
    if (!s.id3Artist.empty())      id3TextFrame(frames, s.id3ver, "TPE1", s.id3Artist);
    if (!s.id3Album.empty())       id3TextFrame(frames, s.id3ver, "TALB", s.id3Album);
    if (!s.id3AlbumArtist.empty()) id3TextFrame(frames, s.id3ver, "TPE2", s.id3AlbumArtist);
    if (!s.id3Genre.empty())       id3TextFrame(frames, s.id3ver, "TCON", s.id3Genre);
    if (!s.id3Date.empty())        id3TextFrame(frames, s.id3ver, s.id3ver == 4 ? "TDRC" : "TYER", s.id3Date);
    if (!s.id3Track.empty())       id3TextFrame(frames, s.id3ver, "TRCK", s.id3Track);
    if (!s.id3Disc.empty())        id3TextFrame(frames, s.id3ver, "TPOS", s.id3Disc);
    if (s.id3Apic)                 id3ApicFrame(frames, s.id3ver, s.id3ApicMime, s.id3ApicDesc);
    putStr(tag, "ID3");
    tag.push_back((unsigned char)s.id3ver); tag.push_back(0); tag.push_back(0);
    syncsafe32(tag, (uint32_t)frames.size());
    put(tag, frames.data(), frames.size());
    return tag;
}
static std::vector<unsigned char> buildWavBextChunk(const WavSpec& s) {
    std::vector<unsigned char> b((size_t)s.bextSize, 0);
    if (b.size() > 3) std::memcpy(b.data(), "desc", 4);
    if (b.size() >= 267) std::memcpy(b.data() + 256, "Logic Pro X", 11);
    if (b.size() >= 330 && !s.bextDate.empty()) {
        const size_t m = std::min<size_t>(s.bextDate.size(), 10);
        std::memcpy(b.data() + 320, s.bextDate.data(), m);
    }
    return b;
}
static std::vector<unsigned char> buildWav(const WavSpec& s) {
    std::vector<unsigned char> data((size_t)(s.frames * s.ch * (s.bits / 8)), 0);
    std::vector<unsigned char> fmt;
    if (s.extensible) {
        put16le(fmt, 0xFFFE); put16le(fmt, (uint32_t)s.ch); put32le(fmt, (uint32_t)s.sr);
        put32le(fmt, (uint32_t)(s.sr * s.ch * (s.bits / 8))); put16le(fmt, (uint32_t)(s.ch * (s.bits / 8)));
        put16le(fmt, (uint32_t)s.bits); put16le(fmt, 22); put16le(fmt, (uint32_t)s.bits); put32le(fmt, 3);
        put32le(fmt, (uint32_t)s.fmtTag); put16le(fmt, 0); put16le(fmt, 0x0010);
        const unsigned char guidTail[8] = { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        put(fmt, guidTail, 8);
    } else {
        put16le(fmt, (uint32_t)s.fmtTag); put16le(fmt, (uint32_t)s.ch); put32le(fmt, (uint32_t)s.sr);
        put32le(fmt, (uint32_t)(s.sr * s.ch * (s.bits / 8))); put16le(fmt, (uint32_t)(s.ch * (s.bits / 8)));
        put16le(fmt, (uint32_t)s.bits);
    }
    std::vector<unsigned char> chunks;
    {   // fmt
        putStr(chunks, "fmt "); put32le(chunks, (uint32_t)fmt.size()); put(chunks, fmt.data(), fmt.size());
    }
    // 块顺序可调：id3First/bextFirst 用来造「LIST 在前 / id3 在前」两种夹具
    if (s.id3chunk && s.id3First)  wavChunk(chunks, "id3 ", buildWavId3Chunk(s));
    if (s.bext && s.bextFirst)     wavChunk(chunks, "bext", buildWavBextChunk(s));
    if (!s.info.empty()) {
        std::vector<unsigned char> body;
        putStr(body, "INFO");
        for (auto& kv : s.info) {
            std::vector<unsigned char> vb;
            putStr(vb, kv.second); vb.push_back(0);
            if (vb.size() & 1u) vb.push_back(0);
            putStr(body, kv.first); put32le(body, (uint32_t)vb.size()); put(body, vb.data(), vb.size());
        }
        putStr(chunks, "LIST"); put32le(chunks, (uint32_t)body.size()); put(chunks, body.data(), body.size());
    }
    {   // data
        putStr(chunks, "data");
        put32le(chunks, (uint32_t)(s.dataDecl >= 0 ? s.dataDecl : (long long)data.size()));
        put(chunks, data.data(), data.size());
    }
    if (s.id3chunk && !s.id3First) wavChunk(chunks, "id3 ", buildWavId3Chunk(s));
    if (s.bext && !s.bextFirst)    wavChunk(chunks, "bext", buildWavBextChunk(s));
    std::vector<unsigned char> out;
    putStr(out, s.rf64 ? "RF64" : "RIFF");
    put32le(out, (uint32_t)(chunks.size() + 4));
    putStr(out, "WAVE");
    put(out, chunks.data(), chunks.size());
    return out;
}

// ---- MP3 ----
static std::vector<unsigned char> mpeg1Frame(int brIdx = 9, int srIdx = 0, int pad = 0, int mode = 0) {
    static const int kBr[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int kSr[3] = { 44100, 48000, 32000 };
    std::vector<unsigned char> f;
    f.push_back(0xFF); f.push_back(0xFB);                       // MPEG1 Layer III
    f.push_back((unsigned char)((brIdx << 4) | (srIdx << 2) | (pad << 1)));
    f.push_back((unsigned char)(mode << 6));
    const int len = 144 * kBr[brIdx] * 1000 / kSr[srIdx] + pad;
    f.resize((size_t)len, 0);
    return f;
}
struct Mp3Spec {
    int ver = 3;                                  // 3 / 4 / 2（2 = 不支持，应回退）
    bool unsync = false, id3v1 = false, xing = true, lameTag = true;
    int xingFrames = 2, delay = 576, padding = 300;
    std::vector<std::pair<int, std::string>> textFrames;   // 用 id3TextFrame 的顺序：3=TIT2? 不用——直接给 id
    std::string title, artist, album, albumArtist, genre, date, track, disc;
    bool apic = false;
    std::string apicMime = "image/jpeg", apicDesc;
    int audioFrames = 2;
};
static std::vector<unsigned char> buildMp3(const Mp3Spec& s) {
    std::vector<unsigned char> tag;
    if (s.ver == 3 || s.ver == 4) {
        if (!s.title.empty())      id3TextFrame(tag, s.ver, "TIT2", s.title);
        if (!s.artist.empty())     id3TextFrame(tag, s.ver, "TPE1", s.artist);
        if (!s.album.empty())      id3TextFrame(tag, s.ver, "TALB", s.album);
        if (!s.albumArtist.empty())id3TextFrame(tag, s.ver, "TPE2", s.albumArtist);
        if (!s.genre.empty())      id3TextFrame(tag, s.ver, "TCON", s.genre);
        if (!s.date.empty())       id3TextFrame(tag, s.ver, s.ver == 4 ? "TDRC" : "TYER", s.date);
        if (!s.track.empty())      id3TextFrame(tag, s.ver, "TRCK", s.track);
        if (!s.disc.empty())       id3TextFrame(tag, s.ver, "TPOS", s.disc);
        if (s.apic)                id3ApicFrame(tag, s.ver, s.apicMime, s.apicDesc);
    } else {
        putStr(tag, "TT2"); put32be(tag, 5); put16be(tag, 0); tag.push_back(0); putStr(tag, "v22");
    }
    std::vector<unsigned char> out;
    if (s.ver == 3 || s.ver == 4 || s.ver == 2) {
        putStr(out, "ID3"); out.push_back((unsigned char)s.ver); out.push_back(0);
        out.push_back((unsigned char)(s.unsync ? 0x80 : 0x00));
        syncsafe32(out, (uint32_t)tag.size());
        put(out, tag.data(), tag.size());
    }
    // 首帧（可带 Xing/Info + LAME 标签）
    {
        std::vector<unsigned char> f = mpeg1Frame(9, 0, 0);
        if (s.xing) {
            const size_t xing = 4 + 32;                        // MPEG1 立体声 sideinfo = 32
            if (f.size() < xing + 120 + 24) f.resize(xing + 120 + 24, 0);
            std::memcpy(f.data() + xing, "Info", 4);
            uint32_t flags = 0x01;
            f[xing + 4] = 0; f[xing + 5] = 0; f[xing + 6] = 0; f[xing + 7] = (unsigned char)flags;
            const uint32_t nf = (uint32_t)s.xingFrames;
            f[xing + 8] = (unsigned char)(nf >> 24); f[xing + 9] = (unsigned char)(nf >> 16);
            f[xing + 10] = (unsigned char)(nf >> 8); f[xing + 11] = (unsigned char)nf;
            if (s.lameTag) {
                const size_t lt = xing + 120;
                std::memcpy(f.data() + lt, "LAME3.100", 9);
                f[lt + 21] = (unsigned char)(s.delay >> 4);
                f[lt + 22] = (unsigned char)(((s.delay & 0xF) << 4) | ((s.padding >> 8) & 0xF));
                f[lt + 23] = (unsigned char)(s.padding & 0xFF);
            }
        }
        put(out, f.data(), f.size());
    }
    for (int i = 0; i < s.audioFrames; ++i) {
        std::vector<unsigned char> f = mpeg1Frame(9, 0, 0);
        put(out, f.data(), f.size());
    }
    if (s.id3v1) {
        std::vector<unsigned char> v1(128, 0);
        std::memcpy(v1.data(), "TAG", 3);
        std::memcpy(v1.data() + 3, "V1TITLE", 7);
        put(out, v1.data(), v1.size());
    }
    return out;
}

// ---- OGG（Vorbis）----
static std::vector<unsigned char> oggPage(const std::vector<std::vector<unsigned char>>& packets,
                                          long long granule, uint32_t serial, bool bos, bool eos,
                                          const std::vector<int>& overrideSegs = {}) {
    std::vector<unsigned char> body;
    std::vector<int> segs;
    for (auto& p : packets) {
        size_t left = p.size();
        const unsigned char* q = p.data();
        while (left >= 255) { segs.push_back(255); left -= 255; q += 255; }
        segs.push_back((int)left);
        body.insert(body.end(), p.begin(), p.end());
    }
    if (!overrideSegs.empty()) segs = overrideSegs;
    std::vector<unsigned char> page;
    putStr(page, "OggS"); page.push_back(0); page.push_back((unsigned char)((bos ? 2 : 0) | (eos ? 4 : 0)));
    for (int i = 0; i < 8; ++i) page.push_back((unsigned char)((granule >> (8 * i)) & 0xFF));
    put32le(page, serial); put32le(page, 0); put32le(page, 0);
    page.push_back((unsigned char)segs.size());
    for (int s : segs) page.push_back((unsigned char)s);
    put(page, body.data(), body.size());
    return page;      // 注意：CRC 全零 —— 原生解析不校验 CRC（ffprobe 会校验，所以 [2] 不用构造的 ogg）
}
static std::vector<unsigned char> buildOgg(int sr, int ch, long long granule,
                                           const std::vector<std::pair<std::string, std::string>>& tags) {
    std::vector<unsigned char> id;
    id.push_back(0x01); putStr(id, "vorbis");
    put32le(id, 0); id.push_back((unsigned char)ch); put32le(id, (uint32_t)sr);
    put32le(id, 0); put32le(id, 128000); put32le(id, 0);
    id.push_back(0xB8); id.push_back(0x01);
    std::vector<unsigned char> cmt;
    cmt.push_back(0x03); putStr(cmt, "vorbis");
    const std::vector<unsigned char> vc = vorbisComment(tags);
    put(cmt, vc.data(), vc.size());
    std::vector<unsigned char> out;
    const uint32_t serial = 0x12345678;
    std::vector<unsigned char> p0 = oggPage({ id }, 0, serial, true, false);
    std::vector<unsigned char> p1 = oggPage({ cmt }, 0, serial, false, false);
    std::vector<unsigned char> audio(200, 0);
    std::vector<unsigned char> p2 = oggPage({ audio }, granule, serial, false, true);
    out.insert(out.end(), p0.begin(), p0.end());
    out.insert(out.end(), p1.begin(), p1.end());
    out.insert(out.end(), p2.begin(), p2.end());
    return out;
}

// ---------------------------------------------------------------------------
// 环境/路径辅助
// ---------------------------------------------------------------------------
static void setNative(bool on) { _putenv_s("RLPLAYER_NATIVE_TAGS", on ? "1" : "0"); }

static bool probeNative(const fs::path& p, TagInfo& t, std::string& why) {
    setNative(true);
    return rta::meta::native::probe_file(U8(p), &t, &why);
}

static LibTrack probeFfprobe(const fs::path& p) {
    setNative(false);
    return rta::net::lib_probe_one(U8(p));
}
// 原生通道的**入库结果**（lib_probe_one 会把 bits<=0 写成 16，并补同目录封面）
static LibTrack probeFfprobePathWithNative(const fs::path& p) {
    setNative(true);
    return rta::net::lib_probe_one(U8(p));
}

static void setLibDir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    _putenv_s("RLPLAYER_LIBRARY_DIR", U8(dir).c_str());
    _putenv_s("RLPLAYER_PORT", "");
}

// 找仓库根（含 CMakeLists.txt 与 build/ 的目录）
static fs::path findRepoRoot() {
    std::error_code ec;
    fs::path p = fs::current_path(ec);
    for (int i = 0; i < 6 && !p.empty(); ++i) {
        if (fs::exists(p / "CMakeLists.txt", ec) && fs::exists(p / "src", ec)) return p;
        p = p.parent_path();
    }
    return fs::current_path(ec);
}

static std::vector<std::string> collectAudio(const fs::path& dir, const std::string& ext, size_t limit) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto& de : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!de.is_regular_file(ec)) continue;
        const std::string e = rta::meta::native::detail::lower_ext(U8(de.path()));
        if (e != ext) continue;
        out.push_back(U8(de.path()));
        if (out.size() >= limit) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ===========================================================================
// A/B 单文件比对（[2] 与 [5] 共用同一条判据，避免"抽查用 A 口径、全量用 B 口径"）
//   两边都走 lib_probe_one（即**入库结果**），字段与容差完全按 261025-01 的验收口径：
//   title/artist/album/albumArtist/genre/track/disc/year/sr/bits/ch/hasCover 逐字段，
//   时长按 ±1 帧（1/sr）。
// ===========================================================================
struct AbResult {
    std::vector<std::string> diffs;
    bool hit = false;                 // 原生通道是否真的命中了这个文件（而不是悄悄回退）
    std::string why;                  // hit=false 时的回退原因
    LibTrack ff, nt;
};
// 2026-10-26（261025-02 #6）：解析器现在会**剔除"全是问号"的占位标题**（用户库里 16 个 WAV
// 的 RIFF INFO 里就是字面 0x3F，而同文件的 ID3 里是完好的日文）。这会让原生结果与 ffprobe **故意不同**：
// ffprobe 给 "?????????"，我们给真名。所以 A/B 判据要区分两件事：
//   * 「原生错」⟹ 仍然 FAIL；
//   * 「ffprobe 是占位、原生是真名」⟹ 记为 allowedImprove（有上限），并用专门断言钉住。
static bool tgIsPlaceholderTag(const std::string& s) {
    // 261026-02 #4：口径改成与实现**同一份**判据（src/meta/tags_native.h 的 tag_is_corrupt），
    // 否则"混合占位 -> 真名"这类已解释的改进会被当成"原生错"。
    // 说明：这里的函数名保留旧名，避免改动十几处调用点；语义是"这个值是不是坏的（问号占大头）"。
    return rta::meta::native::detail::tag_is_corrupt(s);
}
static int g_allowedImprove = 0;

// 两份索引的容错比较（2026-10-26，261025-02 #6）：除了 title 列里
// 「ffprobe 是占位问号、原生是真名」这一类**已解释的改进**，其余必须逐字节相同。
struct IndexCmp { bool same = true; int improve = 0; std::string why; };
static IndexCmp cmpIndexWithPlaceholderImprove(const std::string& a, const std::string& b) {
    IndexCmp r;
    std::vector<std::string> la, lb;
    auto split = [](const std::string& s, std::vector<std::string>& out) {
        std::string cur;
        for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur.push_back(c); }
        if (!cur.empty()) out.push_back(cur);
    };
    split(a, la); split(b, lb);
    if (la.size() != lb.size()) { r.same = false; r.why = "行数不同 " + std::to_string(la.size()) + "/" + std::to_string(lb.size()); return r; }
    for (size_t i = 0; i < la.size(); ++i) {
        if (la[i] == lb[i]) continue;
        auto ca = la[i], cb = lb[i];
        std::vector<std::string> fa, fb;
        auto sp = [](const std::string& s, std::vector<std::string>& o) {
            std::string cur; for (char c : s) { if (c == '\t') { o.push_back(cur); cur.clear(); } else cur.push_back(c); } o.push_back(cur);
        };
        sp(ca, fa); sp(cb, fb);
        if (fa.size() != fb.size()) { r.same = false; r.why = "行 " + std::to_string(i) + " 列数不同"; return r; }
        bool okLine = true;
        for (size_t k = 0; k < fa.size(); ++k) {
            if (fa[k] == fb[k]) continue;
            // 3=album 4=albumArtist 5=artist 6=title：都是标签文本列，都可能出现"标签本身是问号"的文件
            if (k >= 3 && k <= 6 && tgIsPlaceholderTag(fa[k]) && !tgIsPlaceholderTag(fb[k])) { ++r.improve; continue; }
            okLine = false;
            r.why = "行 " + std::to_string(i) + " 列 " + std::to_string(k) + " ff=[" + fa[k] + "] nat=[" + fb[k] + "]";
            break;
        }
        if (!okLine) { r.same = false; return r; }
    }
    return r;
}
static AbResult abOne(const std::string& f) {
    AbResult r;
    setNative(false);
    r.ff = rta::net::lib_probe_one(f);
    setNative(true);
    const unsigned long long before = rta::meta::native::stats().parsed.load();
    r.nt = rta::net::lib_probe_one(f);
    r.hit = rta::meta::native::stats().parsed.load() != before;
    if (!r.hit) {
        TagInfo tmp;
        rta::meta::native::probe_file(f, &tmp, &r.why);    // 只为拿到回退原因（会重复解析一次，可忽略）
    }
    auto eq = [&](const char* n, const std::string& a, const std::string& b) {
        if (a == b) return;
        // 允许的改进：ffprobe 拿到的是"全是问号"的占位标题，原生解析拿到了真名。
        if (tgIsPlaceholderTag(a) && !b.empty() && !tgIsPlaceholderTag(b)) {
            ++g_allowedImprove;
            std::printf("       [IMPROVE] %s: ff 占位 -> nat=[%s]\n", n, b.c_str());
            return;
        }
        r.diffs.push_back(std::string(n) + " ff=[" + a + "] nat=[" + b + "]");
    };
    eq("title", r.ff.title, r.nt.title);
    eq("artist", r.ff.artist, r.nt.artist);
    eq("album", r.ff.album, r.nt.album);
    eq("albumArtist", r.ff.albumArtist, r.nt.albumArtist);
    eq("genre", r.ff.genre, r.nt.genre);
    if (r.ff.trackNo != r.nt.trackNo) r.diffs.push_back("track " + std::to_string(r.ff.trackNo) + "/" + std::to_string(r.nt.trackNo));
    if (r.ff.discNo != r.nt.discNo) r.diffs.push_back("disc " + std::to_string(r.ff.discNo) + "/" + std::to_string(r.nt.discNo));
    if (r.ff.year != r.nt.year) r.diffs.push_back("year " + std::to_string(r.ff.year) + "/" + std::to_string(r.nt.year));
    if (r.ff.sampleRate != r.nt.sampleRate) r.diffs.push_back("sr " + std::to_string(r.ff.sampleRate) + "/" + std::to_string(r.nt.sampleRate));
    if (r.ff.bits != r.nt.bits) r.diffs.push_back("bits " + std::to_string(r.ff.bits) + "/" + std::to_string(r.nt.bits));
    if (r.ff.channels != r.nt.channels) r.diffs.push_back("ch " + std::to_string(r.ff.channels) + "/" + std::to_string(r.nt.channels));
    if (r.ff.hasCover != r.nt.hasCover) r.diffs.push_back("cover " + std::to_string(r.ff.hasCover) + "/" + std::to_string(r.nt.hasCover));
    const double frame = r.ff.sampleRate > 0 ? 1.0 / r.ff.sampleRate : 1e-6;
    if (!(std::fabs(r.ff.duration - r.nt.duration) <= frame))
        r.diffs.push_back("dur " + std::to_string(r.ff.duration) + "/" + std::to_string(r.nt.duration));
    return r;
}

// ===========================================================================
// [1] 单元：手工构造字节流
// ===========================================================================
static void unitTests(const fs::path& dir) {
    std::printf("\n== [1] 单元：手工构造的容器字节流 ==\n");
    std::error_code ec;
    fs::create_directories(dir, ec);

    // ---- FLAC：完整标签 + 封面 ----
    {
        FlacSpec s;
        s.tags = { {"TITLE","怪少女"},{"ARTIST","敌门"},{"ALBUM","Utapia"},{"ALBUMARTIST","VA"},
                   {"DISCNUMBER","2"},{"TRACKNUMBER","07"},{"GENRE","Electronic"},{"DATE","2020-12-31"} };
        s.picture = true;
        const fs::path p = dir / "full.flac";
        writeFile(p, buildFlac(s));
        TagInfo t; std::string why;
        const bool ok = probeNative(p, t, why);
        ck(ok, "FLAC 完整标签：解析成功", why);
        ck(t.title == "怪少女" && t.artist == "敌门" && t.album == "Utapia" && t.albumArtist == "VA",
           "FLAC 标签逐字相等（中文/UTF-8）", t.title + "|" + t.artist + "|" + t.album + "|" + t.albumArtist);
        ck(t.trackNo == 7 && t.discNo == 2 && t.year == 2020 && t.genre == "Electronic",
           "FLAC track/disc/year/genre（TRACKNUMBER=07、DATE=2020-12-31）",
           std::to_string(t.trackNo) + "/" + std::to_string(t.discNo) + "/" + std::to_string(t.year) + "/" + t.genre);
        ck(t.sampleRate == 48000 && t.channels == 2 && t.bits == 24, "FLAC STREAMINFO 参数",
           std::to_string(t.sampleRate) + "/" + std::to_string(t.channels) + "/" + std::to_string(t.bits));
        ck(std::fabs(t.duration - 2.0) < 1e-9, "FLAC 时长 = total_samples/sr = 2.000000",
           std::to_string(t.duration));
        ck(t.hasCover, "FLAC PICTURE(image/jpeg) ⟹ hasCover=1");
    }
    // ---- FLAC：封面 MIME 空 ⟹ 与 ffprobe 一致：不算封面 ----
    {
        FlacSpec s; s.pictureMime = ""; s.picture = true; s.tags = { {"TITLE","nopic"} };
        const fs::path p = dir / "empty_mime.flac";
        writeFile(p, buildFlac(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "FLAC 空 MIME 封面：仍可解析", why);
        ck(!t.hasCover, "FLAC PICTURE 的 MIME 为空 ⟹ hasCover=0（复刻 ffprobe：它不建封面流）");
    }
    // ---- FLAC：total_samples = 0 ⟹ 回退 ----
    {
        FlacSpec s; s.total = 0; s.tags = { {"TITLE","stream"} };
        const fs::path p = dir / "unknown_len.flac";
        writeFile(p, buildFlac(s));
        TagInfo t; std::string why;
        ck(!probeNative(p, t, why) && why.find("总样本数") != std::string::npos,
           "FLAC total_samples=0（流式）⟹ 回退 ffprobe", why);
    }
    // ---- FLAC：ID3v2 前缀 ----
    {
        FlacSpec s; s.id3prefix = true; s.tags = { {"TITLE","prefixed"} };
        const fs::path p = dir / "id3prefix.flac";
        writeFile(p, buildFlac(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "FLAC 带 ID3v2 前缀：仍能定位 fLaC", why);
        ck(t.title == "prefixed", "FLAC 带 ID3v2 前缀：标签取自 Vorbis comment", t.title);
    }

    // ---- WAV：fmt + data + LIST/INFO ----
    {
        WavSpec s;
        s.info = { {"INAM","W 标题"},{"IART","W 艺术家"},{"IPRD","W 专辑"},{"IGNR","Rock"},
                   {"ICRD","2001"},{"ITRK","3"} };
        const fs::path p = dir / "info.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "WAV+INFO：解析成功", why);
        ck(t.title == "W 标题" && t.artist == "W 艺术家" && t.album == "W 专辑" && t.genre == "Rock",
           "WAV INFO 逐字相等（INAM/IART/IPRD/IGNR）", t.title + "|" + t.artist + "|" + t.album + "|" + t.genre);
        ck(t.trackNo == 3 && t.year == 2001, "WAV ITRK→track、ICRD→year",
           std::to_string(t.trackNo) + "/" + std::to_string(t.year));
        ck(std::fabs(t.duration - 1.0) < 1e-9, "WAV 时长 = data/blockAlign/sr = 1.000000", std::to_string(t.duration));
        ck(t.sampleRate == 44100 && t.channels == 2 && t.bits == 0,
           "WAV 16-bit ⟹ TagInfo.bits=0（ffprobe 的 bits_per_raw_sample 是 N/A）",
           std::to_string(t.sampleRate) + "/" + std::to_string(t.channels) + "/" + std::to_string(t.bits));
        // 入库口径：lib_probe_one 把 <=0 的位深统一写成 16（与旧 ffprobe 路径逐字一致）
        const LibTrack lt = probeFfprobePathWithNative(p);
        ck(lt.bits == 16, "WAV 16-bit 经 lib_probe_one ⟹ LibTrack.bits=16", std::to_string(lt.bits));
    }
    // ---- WAV：24-bit 报真值；float32 报 16（ffprobe 口径） ----
    {
        WavSpec s; s.bits = 24; s.info = { {"INAM","hires"} };
        const fs::path p = dir / "b24.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why) && t.bits == 24, "WAV 24-bit ⟹ bits=24（与 ffprobe 的 bits_per_raw_sample=24 一致）",
           std::to_string(t.bits));
        WavSpec f; f.bits = 32; f.fmtTag = 3; f.info = { {"INAM","f32"} };
        const fs::path q = dir / "f32.wav";
        writeFile(q, buildWav(f));
        TagInfo t2; std::string why2;
        ck(probeNative(q, t2, why2) && t2.bits == 0, "WAV float32 ⟹ bits=0（ffprobe 报 N/A，调用方统一写 16）",
           std::to_string(t2.bits) + " " + why2);
    }
    // ---- WAV：data 声明超长 ⟹ 按实际长度截断（ffmpeg 行为） ----
    {
        WavSpec s; s.dataDecl = 100000000;
        const fs::path p = dir / "trunc.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "WAV data 声明超长：仍解析", why);
        ck(std::fabs(t.duration - 1.0) < 1e-6, "WAV data 声明超长 ⟹ 时长按实际字节（1.0s）", std::to_string(t.duration));
    }
    // ---- WAV：extensible PCM ----
    {
        WavSpec s; s.extensible = true; s.bits = 24; s.sr = 48000; s.frames = 48000;
        const fs::path p = dir / "ext.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "WAV WAVE_FORMAT_EXTENSIBLE(PCM) 可解析", why);
        ck(t.sampleRate == 48000 && t.bits == 24 && std::fabs(t.duration - 1.0) < 1e-9,
           "WAV extensible：sr/bits/时长", std::to_string(t.sampleRate) + "/" + std::to_string(t.bits));
    }
    // ---- WAV：RF64 仍然回退（唯一没动的"必须回退"项）----
    {
        WavSpec r; r.rf64 = true;
        const fs::path p = dir / "rf64.wav";
        writeFile(p, buildWav(r));
        TagInfo t; std::string why;
        ck(!probeNative(p, t, why), "RF64 ⟹ 回退 ffprobe", why);
    }

    // ---- WAV：LIST/INFO ↔ "id3 " 块的合并顺序（**先到先得**，不是"后者覆盖"）----
    // 这条判据的 oracle 是 lib_probe_one，不是 ffmpeg 的语义（两者不同！）：
    //   ffmpeg 对不同的块是**追加**，ffprobe 会打出两条同键 tag；而 lib_probe_one 因为段标记带
    //   '\r' 匹配不上 [FORMAT]，put() 只在目标为空时赋值 ⟹ 入库的是**第一个非空值**。
    // 实测（本机 ffprobe 9.0 + 本仓库 library.h）：LIST→id3 的文件最终取 INFO；id3→LIST 取 ID3。
    {
        WavSpec a;
        a.info = { {"INAM","INFO 标题"},{"IART","INFO 艺术家"},{"IPRD","INFO 专辑"},
                   {"IGNR","Rock"},{"ICRD","2001"},{"ITRK","3"} };
        a.id3chunk = true;
        a.id3Title = "ID3 标题"; a.id3Artist = "ID3 艺术家"; a.id3Album = "ID3 专辑";
        a.id3Genre = "Jazz"; a.id3Date = "1999"; a.id3Track = "7/12"; a.id3Disc = "2";
        const fs::path p = dir / "list_then_id3.wav";
        writeFile(p, buildWav(a));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "WAV LIST→id3：原生解析成功（不再回退）", why);
        ck(t.title == "INFO 标题" && t.artist == "INFO 艺术家" && t.album == "INFO 专辑" &&
           t.genre == "Rock" && t.year == 2001 && t.trackNo == 3,
           "LIST→id3 ⟹ 取 INFO（先到先得：ffprobe 先打 INFO 那条，lib_probe_one 只认第一条）",
           t.title + "|" + t.artist + "|" + t.album + "|" + t.genre + "|" +
           std::to_string(t.year) + "|" + std::to_string(t.trackNo));
        ck(t.discNo == 2, "LIST→id3 ⟹ ID3 独有的 TPOS 仍然保留（逐键先到先得，不是整块丢弃）",
           std::to_string(t.discNo));

        WavSpec b = a;
        b.id3First = true;                                  // id3 块排在 LIST 之前
        const fs::path q = dir / "id3_then_list.wav";
        writeFile(q, buildWav(b));
        TagInfo t2; std::string why2;
        ck(probeNative(q, t2, why2), "WAV id3→LIST：原生解析成功（不再回退）", why2);
        ck(t2.title == "ID3 标题" && t2.artist == "ID3 艺术家" && t2.album == "ID3 专辑" &&
           t2.genre == "Jazz" && t2.year == 1999 && t2.trackNo == 7 && t2.discNo == 2,
           "id3→LIST ⟹ 取 ID3（title/artist/album/genre/date/track(7/12→7)/disc）",
           t2.title + "|" + t2.genre + "|" + std::to_string(t2.year) + "|" +
           std::to_string(t2.trackNo) + "|" + std::to_string(t2.discNo));

        // 两个夹具都走一遍 lib_probe_one 的 A/B（与真实文件同一条判据）：
        // 既证明「原生值与 ffprobe 路径逐字段一致」，也证明「确实没有回退」（hit）。
        for (const fs::path& fx : { p, q }) {
            const AbResult r = abOne(U8(fx));
            ck(r.diffs.empty(), "覆盖顺序夹具与 ffprobe 逐字段一致（" + U8(fx.filename()) + "）",
               r.diffs.empty() ? "" : r.diffs[0]);
            ck(r.hit, "覆盖顺序夹具由原生通道解析（没有回退 ffprobe）（" + U8(fx.filename()) + "）", r.why);
        }
    }

    // ---- WAV：块内重复键的两条反向规则（同一 INFO 块 = 后者赢；同一 ID3 标签 = 前者赢）----
    {
        WavSpec s;
        s.info = { {"INAM","FirstINAM"},{"INAM","SecondINAM"} };
        const fs::path p = dir / "two_inam.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why) && t.title == "SecondINAM",
           "同一个 LIST/INFO 块内 INAM×2 ⟹ 后者赢（ffmpeg 自己就覆盖了）", t.title);
        const AbResult r = abOne(U8(p));
        ck(r.diffs.empty(), "INFO 重复键夹具与 ffprobe 一致", r.diffs.empty() ? "" : r.diffs[0]);

        WavSpec b; b.id3chunk = true; b.id3Title = "FirstTIT2"; b.id3Title2 = "SecondTIT2";
        const fs::path q = dir / "two_tit2.wav";
        writeFile(q, buildWav(b));
        TagInfo t2; std::string why2;
        ck(probeNative(q, t2, why2) && t2.title == "FirstTIT2",
           "同一个 ID3 标签内 TIT2×2 ⟹ 前者赢（ffmpeg 用 DONT_OVERWRITE）", t2.title);
        const AbResult r2 = abOne(U8(q));
        ck(r2.diffs.empty(), "ID3 重复帧夹具与 ffprobe 一致", r2.diffs.empty() ? "" : r2.diffs[0]);
    }

    // ---- WAV：bext 的 origination_date → date（back 同样遵守块顺序）----
    {
        WavSpec a;
        a.info = { {"INAM","B 标题"},{"ICRD","2001"} };
        a.bext = true; a.bextDate = "2022-08-16";
        const fs::path p = dir / "list_then_bext.wav";
        writeFile(p, buildWav(a));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "WAV LIST→bext：原生解析成功", why);
        ck(t.date == "2001" && t.year == 2001 && t.title == "B 标题",
           "LIST→bext ⟹ 取先到的 ICRD（date=2001，bext 的 2022-08-16 被丢弃）",
           t.date + "|" + std::to_string(t.year));

        WavSpec b = a; b.bextFirst = true;                   // bext 在 LIST 之前
        const fs::path q = dir / "bext_then_list.wav";
        writeFile(q, buildWav(b));
        TagInfo t2; std::string why2;
        ck(probeNative(q, t2, why2), "WAV bext→LIST：原生解析成功", why2);
        ck(t2.date == "2022-08-16" && t2.year == 2022,
           "bext→LIST ⟹ bext 的 origination_date 先到（date=2022-08-16 → year 2022）",
           t2.date + "|" + std::to_string(t2.year));

        // origination_date 为空：ffmpeg 不写 date 键 ⟹ 后面 LIST 的 ICRD 仍然能进（空值不占位）
        WavSpec c = b; c.bextDate.clear();                   // bext 在前但没日期，LIST 的 ICRD 随后
        const fs::path s = dir / "bext_nodate.wav";
        writeFile(s, buildWav(c));
        TagInfo t3; std::string why3;
        ck(probeNative(s, t3, why3), "WAV bext 无 origination_date：原生解析成功", why3);
        ck(t3.date == "2001", "bext 日期为空 ⟹ 不占位，后面的 ICRD 生效（date=2001）", t3.date);

        for (const fs::path& fx : { p, q, s }) {
            const AbResult r = abOne(U8(fx));
            ck(r.diffs.empty(), "bext 夹具与 ffprobe 逐字段一致（" + U8(fx.filename()) + "）",
               r.diffs.empty() ? "" : r.diffs[0]);
            ck(r.hit, "bext 夹具由原生通道解析（" + U8(fx.filename()) + "）", r.why);
        }
    }

    // ---- WAV：bext 太短仍然回退（实测 ffprobe 9.0 会整份文件报 invalid data）----
    {
        WavSpec s; s.bext = true; s.bextSize = 330; s.bextDate = "2022-08-16";
        const fs::path p = dir / "bext330.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(!probeNative(p, t, why) && why.find("bext") != std::string::npos,
           "WAV bext 只有 330 字节 ⟹ 回退 ffprobe（<337 时 ffprobe 连流都不给）", why);
        const AbResult r = abOne(U8(p));
        ck(r.diffs.empty() && !r.hit, "短 bext 夹具：回退后结果与 ffprobe 一致（都是空曲目）",
           r.diffs.empty() ? "" : r.diffs[0]);
    }

    // ---- WAV：id3 块里的 APIC ⟹ 封面流（MIME 判据与 MP3 完全同一条）----
    {
        WavSpec s; s.id3chunk = true; s.id3Title = "带封面"; s.id3Apic = true;
        const fs::path p = dir / "id3_apic.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why) && t.hasCover, "WAV id3 APIC(image/jpeg) ⟹ hasCover=1", why);

        WavSpec u = s; u.id3ApicMime = "image/JPEG";       // 大写：APIC 的 MIME 判据大小写不敏感（实测）
        const fs::path q = dir / "id3_apic_upper.wav";
        writeFile(q, buildWav(u));
        TagInfo t2; std::string why2;
        ck(probeNative(q, t2, why2) && t2.hasCover, "WAV id3 APIC(image/JPEG 大写) ⟹ hasCover=1", why2);

        WavSpec v = s; v.id3ApicMime = "image/xyz";        // 未知类型：ffprobe 不建流
        const fs::path rp = dir / "id3_apic_bad.wav";
        writeFile(rp, buildWav(v));
        TagInfo t3; std::string why3;
        ck(probeNative(rp, t3, why3) && !t3.hasCover, "WAV id3 APIC(未知 MIME) ⟹ hasCover=0", why3);

        // APIC 描述 = 封面流的 title ⟹ **先打印、先入库**，所以它压过 TIT2/INAM（实测两例）
        WavSpec w; w.id3chunk = true; w.id3Title.clear(); w.id3Apic = true; w.id3ApicDesc = "MyDesc";
        const fs::path dp = dir / "id3_apic_desc.wav";
        writeFile(dp, buildWav(w));
        TagInfo t4; std::string why4;
        ck(probeNative(dp, t4, why4), "WAV 只有 APIC（无 TIT2）：仍可解析", why4);
        ck(t4.title == "MyDesc", "无 TIT2 时曲名取 APIC 描述（复刻 lib_probe_one 的流标签补缺）", t4.title);

        WavSpec x = s; x.id3ApicDesc = "DescWins";          // 同时有 TIT2 ⟹ 仍是描述赢
        const fs::path xp = dir / "id3_apic_desc_title.wav";
        writeFile(xp, buildWav(x));
        TagInfo t5; std::string why5;
        ck(probeNative(xp, t5, why5) && t5.title == "DescWins",
           "TIT2 与 APIC 描述同时存在 ⟹ 取描述（流标签先打印、先入库）", t5.title);

        WavSpec y = w; y.info = { {"INAM","InfoTitle"} };  // 描述 vs 后面的 INAM ⟹ 仍是描述
        const fs::path yp = dir / "id3_apic_desc_info.wav";
        writeFile(yp, buildWav(y));
        TagInfo t6; std::string why6;
        ck(probeNative(yp, t6, why6) && t6.title == "MyDesc",
           "INFO 的 INAM 也压不过 APIC 描述（流标签在前）", t6.title);

        for (const fs::path& fx : { p, q, rp, dp, xp, yp }) {
            const AbResult r = abOne(U8(fx));
            ck(r.diffs.empty(), "APIC 夹具与 ffprobe 逐字段一致（" + U8(fx.filename()) + "）",
               r.diffs.empty() ? "" : r.diffs[0]);
        }
    }

    // ---- WAV：id3 里的数字流派引用只在"它真的入库"时回退 ----
    {
        WavSpec s; s.id3chunk = true; s.id3Genre = "(17)";
        const fs::path p = dir / "id3_numgenre.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(!probeNative(p, t, why) && why.find("流派") != std::string::npos,
           "id3 TCON='(17)' 是最终的 genre ⟹ 回退 ffprobe（不抄 192 条流派表）", why);
        const AbResult r = abOne(U8(p));
        ck(r.diffs.empty() && !r.hit, "数字流派夹具：回退后与 ffprobe 一致", r.diffs.empty() ? "" : r.diffs[0]);

        // 反过来：INFO 的 IGNR 在前 ⟹ 数字 TCON 根本进不了最终标签 ⟹ 不必回退
        WavSpec b = s; b.id3First = false; b.info = { {"IGNR","InfoGenre"} };
        const fs::path q = dir / "numgenre_after_info.wav";
        writeFile(q, buildWav(b));
        TagInfo t2; std::string why2;
        ck(probeNative(q, t2, why2) && t2.genre == "InfoGenre",
           "INFO 的 IGNR 先到 ⟹ 数字 TCON 不入库，genre 取 InfoGenre 且无需回退",
           why2 + "|" + t2.genre);
        const AbResult r2 = abOne(U8(q));
        ck(r2.diffs.empty() && r2.hit, "数字流派未入库的夹具：原生解析且与 ffprobe 一致",
           r2.diffs.empty() ? r2.why : r2.diffs[0]);
    }

    // ---- WAV：id3 块内不是 ID3v2 ⟹ 与 ffprobe 一样忽略（不加标签、也不回退）----
    {
        WavSpec s; s.id3chunk = true; s.id3BadMagic = true; s.info = { {"INAM","只有 INFO"} };
        const fs::path p = dir / "id3_badmagic.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "WAV id3 块内是垃圾字节：仍能原生解析（ffmpeg 也忽略它）", why);
        ck(t.title == "只有 INFO", "垃圾 id3 块不产生标签（title 取 INFO）", t.title);
        const AbResult r = abOne(U8(p));
        ck(r.diffs.empty() && r.hit, "垃圾 id3 块夹具：原生解析且与 ffprobe 一致",
           r.diffs.empty() ? r.why : r.diffs[0]);
    }

    // ---- WAV：id3 块里的 ID3v2.4（TDRC）----
    {
        WavSpec s; s.id3chunk = true; s.id3ver = 4; s.id3Title = "v24"; s.id3Date = "2010-05-01";
        s.id3Track = "5";
        const fs::path p = dir / "id3v24.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "WAV id3 块里的 ID3v2.4：解析成功", why);
        ck(t.title == "v24" && t.year == 2010 && t.trackNo == 5,
           "ID3v2.4 的 TDRC→year、TRCK→track", t.title + "|" + std::to_string(t.year) + "|" +
           std::to_string(t.trackNo));
        const AbResult r = abOne(U8(p));
        ck(r.diffs.empty(), "ID3v2.4 夹具与 ffprobe 逐字段一致", r.diffs.empty() ? "" : r.diffs[0]);
    }
    // ---- MP3：ID3v2.3 + Xing + LAME delay/padding ----
    {
        Mp3Spec s;
        s.title = "M 标题"; s.artist = "M 艺术家"; s.album = "M 专辑"; s.albumArtist = "M AA";
        s.genre = "Jazz"; s.date = "1999"; s.track = "7/12"; s.disc = "2"; s.apic = true;
        s.xingFrames = 10; s.delay = 576; s.padding = 720;
        const fs::path p = dir / "id3v23.mp3";
        writeFile(p, buildMp3(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "MP3(ID3v2.3+Xing) 解析成功", why);
        ck(t.title == "M 标题" && t.artist == "M 艺术家" && t.album == "M 专辑" && t.albumArtist == "M AA",
           "MP3 文本帧逐字相等（UTF-16）", t.title + "|" + t.artist + "|" + t.album + "|" + t.albumArtist);
        ck(t.genre == "Jazz" && t.year == 1999 && t.trackNo == 7 && t.discNo == 2,
           "MP3 TCON/TYER/TRCK(7/12)/TPOS", t.genre + "|" + std::to_string(t.year) + "|" +
           std::to_string(t.trackNo) + "|" + std::to_string(t.discNo));
        const double want = (10.0 * 1152.0 - 576.0 - 720.0) / 44100.0;
        ck(std::fabs(t.duration - want) < 1e-9,
           "MP3 时长 = (frames*1152 - LAME delay - padding)/sr",
           std::to_string(t.duration) + " want " + std::to_string(want));
        ck(t.sampleRate == 44100 && t.channels == 2 && t.bits == 0,
           "MP3 sr/ch/bits(0→16)", std::to_string(t.sampleRate) + "/" + std::to_string(t.channels) + "/" + std::to_string(t.bits));
        ck(t.hasCover, "MP3 APIC(image/jpeg) ⟹ hasCover=1");
    }
    // ---- MP3：ID3v2.4 TDRC + APIC 描述成为补缺曲名 ----
    {
        Mp3Spec s; s.ver = 4; s.date = "2010-05-01"; s.apic = true; s.apicDesc = "CoverArt";
        s.xing = false; s.audioFrames = 3;
        const fs::path p = dir / "id3v24.mp3";
        writeFile(p, buildMp3(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "MP3(ID3v2.4，无 Xing) 解析成功", why);
        ck(t.year == 2010, "MP3 TDRC=2010-05-01 ⟹ year=2010", std::to_string(t.year));
        ck(t.title == "CoverArt", "无 TIT2 时曲名取 APIC 描述（复刻 ffprobe 的流标签补缺）", t.title);
    }
    // ---- MP3：无 Xing 的 CBR 估算 ----
    {
        Mp3Spec s; s.xing = false; s.audioFrames = 4; s.title = "cbr";
        const fs::path p = dir / "cbr.mp3";
        writeFile(p, buildMp3(s));
        TagInfo t; std::string why;
        const bool ok = probeNative(p, t, why);
        ck(ok, "MP3 无 Xing：解析成功", why);
        // 估算公式 = (文件字节 - ID3v2 长度) * 8 / 首帧码率（与 ffmpeg 同式，实测相等）。
        // 夹具里每个 128k@44.1k 帧 = 144*128000/44100 = 417 字节，共 (1 + audioFrames) 帧。
        const int frameLen = 144 * 128000 / 44100;
        const double want = (double)(1 + s.audioFrames) * (double)frameLen * 8.0 / 128000.0;
        ck(std::fabs(t.duration - want) < 0.02,
           "MP3 CBR 估算 = (字节-ID3v2)*8/码率", std::to_string(t.duration) + " want " + std::to_string(want));
    }
    // ---- MP3：必须回退的四类 ----
    {
        Mp3Spec v2; v2.ver = 2;
        const fs::path p = dir / "v22.mp3";
        writeFile(p, buildMp3(v2));
        TagInfo t; std::string why;
        ck(!probeNative(p, t, why), "ID3v2.2 ⟹ 回退 ffprobe", why);

        Mp3Spec un; un.unsync = true;
        const fs::path q = dir / "unsync.mp3";
        writeFile(q, buildMp3(un));
        TagInfo t2; std::string why2;
        ck(!probeNative(q, t2, why2), "ID3v2 unsynchronisation ⟹ 回退 ffprobe", why2);

        Mp3Spec num; num.genre = "(17)";
        const fs::path r = dir / "numeric_genre.mp3";
        writeFile(r, buildMp3(num));
        TagInfo t3; std::string why3;
        ck(!probeNative(r, t3, why3) && why3.find("流派") != std::string::npos,
           "TCON 数字流派引用 ⟹ 回退 ffprobe（不抄表就不给错值）", why3);
    }
    // ---- MP3：只有 ID3v1 ⟹ 不读（ffprobe 也返回零标签） ----
    {
        Mp3Spec s; s.ver = 0; s.id3v1 = true; s.xing = false; s.audioFrames = 3;
        const fs::path p = dir / "id3v1.mp3";
        writeFile(p, buildMp3(s));
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "只有 ID3v1 的 MP3：仍可解析（时长/流参数）", why);
        ck(t.title.empty() && t.artist.empty() && t.album.empty(),
           "只有 ID3v1 ⟹ 标签为空（与 ffprobe 一致：它不读 ID3v1）", t.title);
    }

    // ---- OGG：Vorbis comment + 末页 granulepos ----
    {
        std::vector<std::pair<std::string, std::string>> tags = {
            {"TITLE","O 标题"},{"ARTIST","O 艺术家"},{"ALBUM","O 专辑"},
            {"TRACKNUMBER","4"},{"DISCNUMBER","2"},{"DATE","2005"},{"GENRE","Folk"} };
        const fs::path p = dir / "vorbis.ogg";
        writeFile(p, buildOgg(44100, 2, 132300, tags));      // 3.0 s
        TagInfo t; std::string why;
        ck(probeNative(p, t, why), "OGG(Vorbis) 解析成功", why);
        ck(t.title == "O 标题" && t.artist == "O 艺术家" && t.album == "O 专辑",
           "OGG 注释逐字相等", t.title + "|" + t.artist + "|" + t.album);
        ck(t.trackNo == 4 && t.discNo == 2 && t.year == 2005 && t.genre == "Folk",
           "OGG TRACKNUMBER/DISCNUMBER/DATE/GENRE", std::to_string(t.trackNo) + "/" +
           std::to_string(t.discNo) + "/" + std::to_string(t.year) + "/" + t.genre);
        ck(t.sampleRate == 44100 && t.channels == 2, "OGG 识别头 sr/ch",
           std::to_string(t.sampleRate) + "/" + std::to_string(t.channels));
        ck(std::fabs(t.duration - 3.0) < 1e-9, "OGG 时长 = 末页 granulepos/sr = 3.000000", std::to_string(t.duration));
    }
    // ---- OGG：Opus ⟹ 回退；末页缺失 ⟹ 回退 ----
    {
        std::vector<unsigned char> id;
        id.push_back(0x01); putStr(id, "OpusHead");
        id.resize(32, 0);                            // 识别头 >= 30 字节，才会走到「不是 Vorbis」这一步
        std::vector<unsigned char> out = oggPage({ id }, 0, 7, true, true);
        const fs::path p = dir / "opus.ogg";
        writeFile(p, out);
        TagInfo t; std::string why;
        ck(!probeNative(p, t, why) && why.find("Opus") != std::string::npos, "OGG/Opus ⟹ 回退 ffprobe", why);

        std::vector<unsigned char> trunc = buildOgg(44100, 2, 132300, { {"TITLE","x"} });
        trunc.resize(trunc.size() - 40);            // 砍掉末页尾部
        const fs::path q = dir / "trunc.ogg";
        writeFile(q, trunc);
        TagInfo t2; std::string why2;
        ck(!probeNative(q, t2, why2), "OGG 末页被截断 ⟹ 回退 ffprobe", why2);
    }
}

// ===========================================================================
// [2] A/B：真实文件逐字段比对
// ===========================================================================
static int abReal(const fs::path& root) {
    std::printf("\n== [2] A/B：真实文件逐字段比对（native vs ffprobe） ==\n");
    struct Pick { const char* ext; std::vector<std::string> files; };
    std::vector<Pick> picks;
    for (const char* e : { ".flac", ".wav", ".mp3", ".ogg" }) picks.push_back({ e, {} });
    // MUSIC 优先（真实用户文件），再用 build/testdata / uploads / lyrtest 补齐到 5 个
    const fs::path dirs[] = { root / "MUSIC", root / "build" / "testdata",
                              root / "build" / "uploads", root / "build" / "lyrtest" };
    for (auto& d : dirs) {
        for (auto& pk : picks) {
            if (pk.files.size() >= 5) continue;
            auto v = collectAudio(d, pk.ext, 5 - pk.files.size());
            pk.files.insert(pk.files.end(), v.begin(), v.end());
        }
    }

    int diffs = 0, compared = 0;
    for (auto& pk : picks) {
        std::printf("  -- %s：%zu 个文件 --\n", pk.ext, pk.files.size());
        if (pk.files.empty()) { ck(false, std::string("有 ") + pk.ext + " 真实文件可比对"); continue; }
        for (auto& f : pk.files) {
            const AbResult r = abOne(f);
            ++compared;
            std::string base = U8(fs::path(W(f)).filename());
            std::printf("     %s %-28s dur=%.3f->%.3f cover=%d/%d %s\n",
                        r.hit ? "NAT" : "FBK", base.c_str(), r.ff.duration, r.nt.duration,
                        r.ff.hasCover, r.nt.hasCover, r.hit ? "" : r.why.c_str());
            if (!r.diffs.empty()) {
                ++diffs;
                std::printf("       [DIFF] %s\n", f.c_str());
                for (auto& x : r.diffs) std::printf("              %s\n", x.c_str());
            }
        }
    }
    ck(diffs == 0, "A/B：全部真实文件逐字段一致（含 hasCover / 位深 / 时长 ±1 帧）",
       std::to_string(diffs) + " 处不一致");
    // 允许的改进要有上界：库里这类"标签本身是问号"的文件实测 16 个（本轮修好 6 个），
    // 超过 24 说明出现了新一类差异，必须重新核对而不是当成"改进"放过（铁律 3）。
    ck(g_allowedImprove <= 24, "A/B 里「ffprobe 占位 -> 原生真名」的改进数在预期内",
       std::to_string(g_allowedImprove) + " 处（上限 24）");
    ck(compared >= 12, "A/B 覆盖的真实文件数 >= 12（理想 20：每类 5 个）", std::to_string(compared) + " 个");
    return compared;
}

// ===========================================================================
// [3] 吞吐 + [4] 回归（同一批夹具）
// ===========================================================================
static void throughputAndRegression(const fs::path& root) {
    std::printf("\n== [3] 吞吐：首次全量扫描（1 秒静音 FLAC） ==\n");
    const int kFiles = [] {
        const char* v = std::getenv("RL_TEST_TAGS_SCALE");
        const int n = v ? std::atoi(v) : 0;
        return n > 0 ? n : 1000;
    }();
    std::error_code ec;
    const fs::path work = fs::temp_directory_path() / "rta_tags_bench";
    fs::remove_all(work, ec);
    fs::create_directories(work / "music", ec);
    // 一个 1 秒静音 FLAC（带标签），复制成 kFiles 个文件（30 张专辑 × N）
    FlacSpec s;
    s.sr = 44100; s.ch = 2; s.bps = 16; s.total = 44100;
    s.tags = { {"TITLE","Silent"},{"ARTIST","Tester"},{"ALBUM","Bench"},{"TRACKNUMBER","1"},
               {"DATE","2024"},{"GENRE","Test"} };
    const std::vector<unsigned char> tpl = buildFlac(s);
    const fs::path tplPath = work / "tpl.flac";
    writeFile(tplPath, tpl);
    const int perAlbum = 50;
    const int albums = std::max(1, kFiles / perAlbum);
    int made = 0;
    for (int a = 0; a < albums && made < kFiles; ++a) {
        char dirName[64];
        std::snprintf(dirName, sizeof(dirName), "Album%03d", a);
        const fs::path d = work / "music" / dirName;
        fs::create_directories(d, ec);
        for (int t = 1; t <= perAlbum && made < kFiles; ++t) {
            char name[64];
            std::snprintf(name, sizeof(name), "%03d track.flac", t);
            fs::copy_file(tplPath, d / name, fs::copy_options::overwrite_existing, ec);
            ++made;
        }
    }
    fs::remove(tplPath, ec);
    std::printf("  夹具：%d 个 FLAC（每个 1.0 s，%zu 字节）\n", made, tpl.size());

    auto scanOnce = [&](bool native, const char* tag) {
        const fs::path lib = work / (native ? "lib_native" : "lib_ffprobe");
        fs::remove_all(lib, ec);
        setLibDir(lib);
        Library& L = Library::instance();
        for (auto r : L.roots()) L.removeRoot(r);
        L.addRootEx(U8(work / "music"));
        L.load();
        setNative(native);
        const unsigned long long p0 = rta::meta::native::stats().parsed.load();
        const auto t0 = std::chrono::steady_clock::now();
        L.startScan(true);                 // 与 /api/lib/scan?full=1 同一入口
        L.joinScan();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const size_t n = L.trackCount();
        const unsigned long long parsed = rta::meta::native::stats().parsed.load() - p0;
        std::printf("  %-8s %zu 首 %8.1f ms  %7.2f 首/s  %6.2f ms/首  原生命中=%llu  %s\n",
                    tag, n, ms, (double)n * 1000.0 / ms, ms / (double)(n ? n : 1),
                    (unsigned long long)parsed, L.scanMsg().c_str());
        return ms;
    };
    const double msFf = scanOnce(false, "ffprobe");
    const double msNat = scanOnce(true, "native");
    const double ffRate = (double)made * 1000.0 / msFf, natRate = (double)made * 1000.0 / msNat;
    std::printf("  基线参考：本机旧实测 45.9 首/s（3000 首 65.4 s）\n");
    std::printf("  推算：3000 首 ffprobe %.1f s / native %.2f s ；15000 首 ffprobe %.1f s / native %.2f s\n",
                3000.0 / ffRate, 3000.0 / natRate, 15000.0 / ffRate, 15000.0 / natRate);
    ck(natRate > ffRate * 5.0, "原生通道首次全量扫描至少快 5 倍",
       std::to_string(ffRate) + " -> " + std::to_string(natRate) + " 首/s");
    ck(natRate > 500.0, "原生通道吞吐 > 500 首/s（基线 45.9 首/s）", std::to_string(natRate) + " 首/s");

    // 单首耗时（单线程直调 lib_probe_one）
    {
        std::vector<std::string> files;
        for (auto& de : fs::recursive_directory_iterator(work / "music", ec))
            if (de.is_regular_file(ec)) files.push_back(U8(de.path()));
        std::sort(files.begin(), files.end());
        if (files.size() > 120) files.resize(120);
        auto timeIt = [&](bool native) {
            setNative(native);
            const auto t0 = std::chrono::steady_clock::now();
            for (auto& f : files) { LibTrack x = rta::net::lib_probe_one(f); (void)x; }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        };
        const double ffMs = timeIt(false), natMs = timeIt(true);
        std::printf("  单首（单线程 %zu 个）：ffprobe %.3f ms/首，native %.4f ms/首（%.0fx）\n",
                    files.size(), ffMs / (double)files.size(), natMs / (double)files.size(),
                    ffMs / (natMs > 0 ? natMs : 1e-9));
        ck(natMs / (double)files.size() < 5.0, "单首原生解析 < 5 ms",
           std::to_string(natMs / (double)files.size()) + " ms");
        ck(ffMs / natMs > 10.0, "单首原生解析比 ffprobe 快 10 倍以上",
           std::to_string(ffMs / natMs) + "x");
    }

    // ---- [4] 回归：两次扫描的索引必须逐字节相同 ----
    std::printf("\n== [4] 回归：开关 RLPLAYER_NATIVE_TAGS 的两次全量扫描 ==\n");
    auto readAll = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const fs::path idxFf = work / "lib_ffprobe" / "rlplayer-index.tsv";
    const fs::path idxNat = work / "lib_native" / "rlplayer-index.tsv";
    const std::string a = readAll(idxFf), b = readAll(idxNat);
    const IndexCmp ic = cmpIndexWithPlaceholderImprove(a, b);
    ck(!a.empty(), "两次扫描都写出了索引（非空）", std::to_string(a.size()) + " / " + std::to_string(b.size()));
    ck(ic.same, "索引与 ffprobe 路径一致（允许「占位问号 -> 真名」的已解释差异）", ic.why);
    // 另扫一遍「关掉原生通道」的增量扫描：应 0 次探测（缓存命中）
    {
        const fs::path lib = work / "lib_ffprobe";
        setLibDir(lib);
        Library& L = Library::instance();
        L.load();
        setNative(false);
        L.startScan(false);
        L.joinScan();
        ck(L.lastScanProbed() == 0 && L.trackCount() == (size_t)made,
           "指纹未变时增量扫描 0 次探测（新旧都不该重探）",
           std::to_string(L.lastScanProbed()) + " probed / " + std::to_string(L.trackCount()) + " tracks");
    }
    (void)root;
    fs::remove_all(work, ec);
}

// ===========================================================================
// [5] WAV 专项：'id3 ' 块 / bext（261025：本库 52 个文件此前每个都要起一次 ffprobe 进程）
//   判据一（正确性）：MUSIC 的**每一个** WAV 都 A/B（ffprobe vs 原生，逐字段 + 时长 ±1 帧），0 差异。
//   判据二（覆盖）：这 71 个 WAV 全部原生命中。改动前是 19/71 —— 差异就来自这 52 个带
//                  'id3 ' 块（51）与 bext（1）的文件，本函数逐文件实测这两个数，不靠记忆。
//   判据三（回归）：把 MUSIC 整个曲库用 Library 全量扫两遍（RLPLAYER_NATIVE_TAGS=0/1），
//                  索引文件必须**逐字节相同**（字段、时长小数位、编码都要对得上）。
// ===========================================================================
// 读 RIFF 顶层块 ID（只读块头，用来把"带 id3 / bext 的"和"纯 LIST/INFO 的"分开统计）
static std::vector<std::string> wavChunkIds(const std::string& file) {
    std::vector<std::string> ids;
    std::ifstream f(fs::path(W(file)), std::ios::binary);
    if (!f) return ids;
    char head[12] = {0};
    f.read(head, 12);
    if (f.gcount() != 12 || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) return ids;
    for (;;) {
        char ch[8];
        f.read(ch, 8);
        if (f.gcount() != 8) break;
        const uint32_t len = (uint32_t)(unsigned char)ch[4] | ((uint32_t)(unsigned char)ch[5] << 8) |
                             ((uint32_t)(unsigned char)ch[6] << 16) | ((uint32_t)(unsigned char)ch[7] << 24);
        ids.push_back(std::string(ch, 4));
        f.seekg((std::streamoff)len + (std::streamoff)(len & 1u), std::ios::cur);
        if (!f) break;
    }
    return ids;
}

static void wavSpecial(const fs::path& root) {
    std::printf("\n== [5] WAV 专项：'id3 ' 块 / bext ==\n");
    const std::vector<std::string> wavs = collectAudio(root / "MUSIC", ".wav", (size_t)-1);
    if (wavs.empty()) {
        // 2026-10-25：本专项依赖仓库根的 MUSIC（用户真实曲库）。在没有 MUSIC 的检出（例如
        // 独立 worktree / CI 副本）里，它应该**明确跳过**而不是把门禁弄红 —— 之前就是这么红的。
        // 在真实仓库里仍然必须跑到（下面 71 个文件逐个 A/B）。
        std::printf("  [SKIP] 本检出没有 MUSIC/ 目录，WAV 专项跳过（在仓库根运行时必跑）\n");
        return;
    }
    int withId3 = 0, withBext = 0, diffs = 0, hit = 0;
    std::vector<std::string> fallbacks;
    for (const std::string& f : wavs) {
        const std::vector<std::string> ids = wavChunkIds(f);
        const bool hasId3Tag = std::find(ids.begin(), ids.end(), "id3 ") != ids.end();
        const bool hasBextChunk = std::find(ids.begin(), ids.end(), "bext") != ids.end();
        if (hasId3Tag) ++withId3;
        if (hasBextChunk) ++withBext;
        const AbResult r = abOne(f);
        if (r.hit) ++hit;
        else if (fallbacks.size() < 12) fallbacks.push_back(U8(fs::path(W(f)).filename()) + " ⟸ " + r.why);
        // 逐个打印（71 行）：这是「52 个带 id3 的文件逐个 A/B」这条判据的审计轨迹，
        // 不能只给一行汇总 —— 汇总绿了但某一行为 FBK 也能被一眼看到。
        std::printf("     %s %-46s tag=%s%s dur=%.3f->%.3f cover=%d/%d %s\n",
                    r.hit ? "NAT" : "FBK", U8(fs::path(W(f)).filename()).c_str(),
                    hasId3Tag ? "id3" : "-", hasBextChunk ? "+bext" : "",
                    r.ff.duration, r.nt.duration, r.ff.hasCover, r.nt.hasCover,
                    r.hit ? "" : r.why.c_str());
        if (!r.diffs.empty()) {
            ++diffs;
            std::printf("     [DIFF] %s\n", f.c_str());
            for (auto& x : r.diffs) std::printf("            %s\n", x.c_str());
        }
    }
    std::printf("  WAV 总数=%zu：带 'id3 ' 块 %d 个、带 bext %d 个；原生命中=%d、回退=%d\n",
                wavs.size(), withId3, withBext, hit, (int)wavs.size() - hit);
    for (auto& s : fallbacks) std::printf("  回退：%s\n", s.c_str());
    ck(diffs == 0, "MUSIC 全部 WAV 逐字段一致（含 cover/位深/声道/时长 ±1 帧）",
       std::to_string(diffs) + " 处不一致");
    if (wavs.size() == 71) {
        ck(withId3 == 51 && withBext == 1,
           "样本实况复核：71 个 WAV = 51 个带 'id3 ' 块 + 1 个带 bext（这 52 个在改动前必然回退）",
           std::to_string(withId3) + "/" + std::to_string(withBext));
    } else {
        std::printf("  [WARN] MUSIC 的 WAV 数不是 71（%zu），跳过'改动前 52 个回退'的实况复核\n", wavs.size());
    }
    ck(hit == (int)wavs.size(), "WAV 原生命中数 = 全部（改动前 19/71，差的 52 个就是 id3/bext）",
       std::to_string(hit) + "/" + std::to_string(wavs.size()));

    // ---- 判据三：整个 MUSIC 全量扫两遍，索引逐字节相同 ----
    std::error_code ec;
    const fs::path work = fs::temp_directory_path() / "rta_tags_wavspecial";
    fs::remove_all(work, ec);
    auto scanMUSIC = [&](bool native, const fs::path& lib) {
        fs::remove_all(lib, ec);
        setLibDir(lib);
        Library& L = Library::instance();
        for (auto r : L.roots()) L.removeRoot(r);
        L.addRootEx(U8(root / "MUSIC"));
        L.load();
        setNative(native);
        const auto t0 = std::chrono::steady_clock::now();
        L.startScan(true);                     // 与 /api/lib/scan?full=1 同一入口
        L.joinScan();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  %-8s %zu 首  %8.1f ms  %s\n", native ? "native" : "ffprobe", L.trackCount(), ms,
                    L.scanMsg().c_str());
        return L.trackCount();
    };
    const fs::path libFf = work / "lib_ffprobe", libNat = work / "lib_native";
    const size_t nFf = scanMUSIC(false, libFf);
    const size_t nNat = scanMUSIC(true, libNat);
    auto readAll = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const std::string a = readAll(libFf / "rlplayer-index.tsv");
    const std::string b = readAll(libNat / "rlplayer-index.tsv");
    ck(nFf == nNat && nFf > 0, "MUSIC 两次全量扫描的曲目数相同（>0）",
       std::to_string(nFf) + "/" + std::to_string(nNat));
    const IndexCmp ic2 = cmpIndexWithPlaceholderImprove(a, b);
    ck(!a.empty() && !b.empty(), "MUSIC 两次扫描都写出了索引",
       std::to_string(a.size()) + "/" + std::to_string(b.size()));
    ck(ic2.same, "MUSIC 索引与 ffprobe 路径一致（允许「占位问号 -> 真名」，其余逐字节相同）",
       std::to_string(ic2.improve) + " 处已解释差异 " + ic2.why);
    fs::remove_all(work, ec);
}

// ===========================================================================
// [6] 261026-02 #4「问号标签智能修复」：**合成夹具**逐条判据（铁律 3：每条规则都能被单独否定）
//
// 规则（都在 src/meta/tags_native.h，判据与实现一一对应）：
//   R1 同键择优（跨块）：cur 残缺、cand 不残缺、长度差 <= 3 倍 ⟹ 取 cand。
//   R1b 同键拼接：两来源码点数等长（或 <= 2 倍）时，把 cur 里的 '?' 用 cand 同位字符补上。
//   R2 只覆盖残缺值：正常值**一个字节都不改**（下面 6 组反向夹具点名叫板）。
//   R3 文件名兜底：只对残缺值生效；推不出来就保持原样；总开关 RLPLAYER_TAG_GUESS=0 整体关闭。
// 夹具形态覆盖：INFO 纯占位+ID3 真名 / INFO 混合占位+ID3 真名 / 两边都占位 / 两边都正常 /
//   MP3 占位+文件名正常 / FLAC 占位+文件名正常 / MP3+FLAC 两边都占位（不许编造）/
//   文件名六种形态（NN. Title / Artist - Title / 中文括号前缀 / 纯英文 / 日文 / 全角括号）。
// ===========================================================================
static size_t countQ(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '?') ++n;
        else if ((unsigned char)s[i] == 0xEF && i + 2 < s.size() &&
                 (unsigned char)s[i + 1] == 0xBC && (unsigned char)s[i + 2] == 0x9F) { ++n; i += 2; }
    }
    return n;
}
static void tagGuessFixtures(const fs::path& dir) {
    std::printf("\n== [6] 问号标签智能修复：合成夹具 ==\n");
    using rta::meta::native::detail::qm_guess_title_from_filename;
    using rta::meta::native::detail::qm_guess_artist_from_filename;
    using rta::meta::native::detail::tag_is_corrupt;
    using rta::meta::native::detail::tag_better_replaces;
    std::error_code ec;
    fs::create_directories(dir, ec);

    // ---- R1/R2 纯函数判据（不碰文件）----
    ck(tag_is_corrupt("??????????????"), "R1 纯占位判残缺");
    ck(tag_is_corrupt("????? (Chinese Tea)"), "R1 混合占位（CJK 段变问号 + ASCII 尾巴）判残缺");
    ck(tag_is_corrupt("Palace Memories NHK???????????????????????I"), "R1 长混合占位判残缺");
    ck(!tag_is_corrupt("U.N.\xE3\x82\xAA\xE3\x83\xBC\xE3\x82\xA8\xE3\x83\xB3\xE3\x81\xAF\xE5\xBD\xBC\xE5\xA5\xB3\xE3\x81\xAA\xE3\x81\xAE\xE3\x81\x8B\xEF\xBC\x9F"),
       "R2 正常日文标题（末尾一个全角问号）**不**判残缺");
    ck(!tag_is_corrupt("May I May I\xEF\xBC\x9F"), "R2 \"May I May I？\" 不判残缺");
    ck(!tag_is_corrupt("TV\xE3\x82\xA2\xE3\x83\x8B\xE3\x83\xA1"), "R2 正常日文不判残缺");
    ck(!tag_is_corrupt(""), "R2 空串不判残缺（空值另有口径）");
    ck(tag_better_replaces("???", "\xE4\xB8\x8D\xE7\x94\xB1\xE5\xB7\xB1"), "R1 残缺 -> 完好：可覆盖");
    ck(!tag_better_replaces("\xE8\x92\xBC\xE6\x96\x87", "\xE6\x81\x8B\xE6\x96\x87"), "R2 正常 -> 正常：**不许**覆盖");
    ck(!tag_better_replaces("??", std::string(200, 'x')), "R1 长度差 > 3 倍：不覆盖（防误配）");

    // ---- R3 文件名推导（六种形态）----
    struct G { std::string path, artist, albumArtist, want; };
    const G gs[] = {
        { "D:/m/01. Foo Bar.flac",                       "",      "", "Foo Bar" },                    // NN. Title
        { "D:/m/03. \xE3\x82\xB9\xE3\x82\xAB\xE3\x83\xBC.wav", "", "", "\xE3\x82\xB9\xE3\x82\xAB\xE3\x83\xBC" }, // NN. 日文
        // "Artist - Title"：**艺人未知时不去前缀**（" - " 两边谁是谁无法判定，去了就可能把艺人的名字
        // 当成曲名）。只有标签里的 artist 与该段一致时才去 —— 下一组用 artist="Artist" 验证。
        { "D:/m/Artist - My Song.mp3",                   "",      "", "Artist - My Song" },
        { "D:/m/Artist - My Song.mp3",                   "Artist", "", "My Song" },
        { "D:/m/\xE5\x99\xA8\xE4\xB9\x90\xEF\xBC\x88\xE9\x92\xA2\xE7\x90\xB4\xEF\xBC\x89 - Sonata (Live).wav", "", "",
          "Sonata (Live)" },                                                                             // 中文括号前缀
        { "D:/m/\xE5\x99\xA8\xE4\xB9\x90\xEF\xBC\x88\xE9\x92\xA2\xE7\x90\xB4\xEF\xBC\x89- Moonlight.wav", "", "",
          "Moonlight" },                                                                                 // 全角括号无空格
        { "D:/m/05. (Bonus) Track Two.wav",              "",      "", "(Bonus) Track Two" },         // 括号但**没有**连字符：不剥
        { "D:/m/01. 7PM.flac",                           "",      "", "7PM" },                       // 两码点标题可过
        { std::string("D:/m/") + "\xE9" "\x9F" "\xB3" "\xE8" "\xBD" "\xA8" "01.wav", "", "", "" },
    };
    for (const auto& g : gs) {
        const std::string got = qm_guess_title_from_filename(g.path, g.artist, g.albumArtist);
        ck(got == g.want, "R3 文件名推导：" + U8(fs::path(W(g.path)).filename()) + " ⟹ [" + g.want + "]",
           "得到 [" + got + "]");
    }
    ck(qm_guess_artist_from_filename("D:/m/Artist - My Song.mp3") == "Artist", "R3 艺术家推导：Artist - Title");
    ck(qm_guess_artist_from_filename("D:/m/01. \xE6\x9C\xAA\xE7\x9F\xA5\xE8\x89\xBA\xE6\x9C\xAF\xE5\xAE\xB6 - \xE9\x9F\xB3\xE8\xBD\xA8" "01.wav").empty(),
       "R3 艺术家推导：\"未知艺术家 - 音轨01\" **不**猜（占位词 + 曲目占位）");
    ck(qm_guess_artist_from_filename("D:/m/\xEF\xBC\x88\xE9\x92\xA2\xE7\x90\xB4\xEF\xBC\x89 - Sonata.wav").empty(),
       "R3 艺术家推导：以全角括号开头的段**不是**艺人");

    // ---- 真实容器夹具：WAV ----
    auto wavCase = [&](const char* name,
                       std::vector<std::pair<std::string, std::string>> info, bool id3,
                       std::string t0, std::string a0, std::string al0,
                       std::string wt, std::string wa, std::string wal,
                       bool expectNative) {
        WavSpec s;
        s.frames = 441;                       // 10 ms，够解析就行
        s.info = info;
        s.id3chunk = id3;
        s.id3Title = t0; s.id3Artist = a0; s.id3Album = al0;
        const fs::path p = dir / W(name);
        writeFile(p, buildWav(s));
        TagInfo t;
        std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok == expectNative, std::string("WAV 夹具解析状态：") + name, why);
        if (ok) ck(t.title == wt && t.artist == wa && t.album == wal,
                   std::string("WAV 夹具取值：") + name,
                   "title=[" + t.title + "] artist=[" + t.artist + "] album=[" + t.album + "]");
        return ok;
    };
    // W1 INFO 纯占位 + ID3 真名 ⟹ 取 ID3
    wavCase("qm_pure_then_id3.wav",
            { {"INAM", "?????????????????????"}, {"IART", "????"}, {"IPRD", "??????"} }, true,
            "\xE3\x81\x84\xE3\x81\xAE\xE3\x81\xA1\xE3\x80\x82", "\xE4\xB8\x8A\xE6\x9D\xBE", "\xE6\x9D\xB1\xE6\x96\xB9",
            "\xE3\x81\x84\xE3\x81\xAE\xE3\x81\xA1\xE3\x80\x82", "\xE4\xB8\x8A\xE6\x9D\xBE", "\xE6\x9D\xB1\xE6\x96\xB9", true);
    // W2 INFO 混合占位 + ID3 真名 ⟹ 取 ID3（已剔除的"纯占位"判据认不出这一类）
    wavCase("qm_mixed_then_id3.wav",
            { {"INAM", "????? (Chinese Tea)"}, {"IPRD", "?????"} }, true,
            "\xE4\xB8\x8A\xE6\xB5\xB7\xE7\xB4\x85\xE8\x8C\xB6\xE9\xA4\xA8 (Chinese Tea)", "", "\xE6\x9D\xB1\xE6\x96\xB9\xE5\xAD\x90\xE5\xAE\x88\xE5\x94\x84",
            "\xE4\xB8\x8A\xE6\xB5\xB7\xE7\xB4\x85\xE8\x8C\xB6\xE9\xA4\xA8 (Chinese Tea)", "", "\xE6\x9D\xB1\xE6\x96\xB9\xE5\xAD\x90\xE5\xAE\x88\xE5\x94\x84", true);
    // W3 两边都占位 ⟹ 保持占位（本例文件名 "qm_both_placeholder" 可读，会触发 R3 文件名兜底；
    // 所以这里额外用**文件名无可推导内容**的 "c.wav" 来单独验证"两边都占位时不编造"）
    {
        WavSpec s; s.frames = 441; s.info = { {"INAM", "??????"} }; s.id3chunk = true; s.id3Title = "??????";
        const fs::path p = dir / L"c.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok && t.title == "??????", "W3 两边都占位 + 文件名无信息 ⟹ 保持占位（不编造）", t.title + " " + why);
    }
    wavCase("7.wav",
            { {"INAM", "??????"} }, true, "??????", "", "",
            "??????", "", "", true);
    // W4 两边都正常且**不同** ⟹ 保持 INFO（先到先得口径不变，正常值一个字节都不改）
    wavCase("qm_both_good.wav",
            { {"INAM", "InfoTitle"}, {"IART", "InfoArtist"} }, true, "ID3Title", "ID3Artist", "",
            "InfoTitle", "InfoArtist", "", true);
    // W5 id3 在前、INFO 正常 ⟹ 仍是 ID3（先到先得），不许因为"新规则"被改
    {
        WavSpec s; s.frames = 441; s.id3chunk = true; s.id3First = true;
        s.id3Title = "FirstWins"; s.info = { {"INAM", "LaterInfo"} };
        const fs::path p = dir / L"qm_id3_first.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        ck(rta::meta::native::probe_file(U8(p), &t, &why) && t.title == "FirstWins",
           "R2 先到先得仍是先到先得（id3 在前、两边都正常 ⟹ 取 ID3）", t.title + " " + why);
    }
    // W6 残缺标题 + 文件名正常 ⟹ 文件名兜底（INFO/ID3 都残）
    {
        WavSpec s; s.frames = 441; s.info = { {"INAM", "??????"} };
        const fs::path p = dir / L"qm_name_fallback.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok && t.title == "qm_name_fallback", "R3 残缺标题 ⟹ 用文件名兜底", t.title + " " + why);
    }
    // W7 残缺标题 + 文件名推不出有效名字 ⟹ 保持原样（不编造）
    // 注：Windows 文件名里不可能出现 '?'，所以"文件名也占位"只能表现为"文件名没有信息"——
    // 这里用"标题被摘干净后只剩单码点"的文件名（"a.wav"），推出来不满足 >=2 码点 ⟹ 不采用。
    {
        WavSpec s; s.frames = 441; s.info = { {"INAM", "??????"} };
        const fs::path p = dir / L"a.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok && t.title == "??????", "R3 文件名推不出有效名字 ⟹ 不编造，保持原样", t.title + " " + why);
    }

    // ---- MP3 / FLAC：占位标题 + 正常文件名 / 都占位 ----
    {
        Mp3Spec s; s.title = "?????"; s.artist = "KnownArtist";
        const fs::path p = dir / L"06. Real Mp3 Title.mp3";
        writeFile(p, buildMp3(s));
        TagInfo t; std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok && t.title == "Real Mp3 Title" && t.artist == "KnownArtist",
           "R3 MP3 占位标题 + 正常文件名 ⟹ 文件名兜底（artist 不受影响）",
           "title=[" + t.title + "] artist=[" + t.artist + "] " + why);
    }
    {
        Mp3Spec s; s.title = "?????"; s.artist = "";
        const fs::path p = dir / L"b.mp3";                 // 文件名无可推导内容 ⟹ 保持占位
        writeFile(p, buildMp3(s));
        TagInfo t; std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok && t.title == "?????", "R3 MP3 占位标题 + 文件名无信息 ⟹ 保持原样", t.title + " " + why);
    }
    {
        FlacSpec s;
        s.tags = { {"TITLE", "?????"}, {"ARTIST", "SomeArtist"} };
        const fs::path p = dir / L"07. Real Flac Title.flac";
        writeFile(p, buildFlac(s));
        TagInfo t; std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok && t.title == "Real Flac Title", "R3 FLAC 占位标题 + 正常文件名 ⟹ 文件名兜底",
           "title=[" + t.title + "] " + why);
    }
    {
        FlacSpec s;
        s.tags = { {"TITLE", "Good Title"}, {"ARTIST", "Good Artist"} };
        const fs::path p = dir / L"08. Unrelated Name.flac";
        writeFile(p, buildFlac(s));
        TagInfo t; std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok && t.title == "Good Title" && t.artist == "Good Artist",
           "R2 正常标签**不许**被文件名改（FLAC）", t.title + "/" + t.artist);
    }

    // ---- 总开关：RLPLAYER_TAG_GUESS=0 ⟹ 文件名兜底与跨块择优全关，行为回到 261025-02 ----
    {
        _putenv_s("RLPLAYER_TAG_GUESS", "0");
        WavSpec s; s.frames = 441; s.info = { {"INAM", "??????"} };
        const fs::path p = dir / L"qm_switch_off.wav";
        writeFile(p, buildWav(s));
        TagInfo t; std::string why;
        const bool ok = rta::meta::native::probe_file(U8(p), &t, &why);
        ck(ok && t.title == "??????", "开关 OFF：文件名兜底关闭（保持占位原值）", t.title);
        WavSpec m; m.frames = 441; m.id3chunk = true;
        m.info = { {"INAM", "????? (Chinese Tea)"} };
        m.id3Title = "\xE4\xB8\x8A\xE6\xB5\xB7\xE7\xB4\x85\xE8\x8C\xB6\xE9\xA4\xA8 (Chinese Tea)";
        const fs::path p2 = dir / L"qm_switch_off2.wav";
        writeFile(p2, buildWav(m));
        TagInfo t2; std::string why2;
        const bool ok2 = rta::meta::native::probe_file(U8(p2), &t2, &why2);
        ck(ok2 && t2.title == "????? (Chinese Tea)", "开关 OFF：跨块择优关闭（INFO 的混合占位仍是先到先得）",
           t2.title);
        _putenv_s("RLPLAYER_TAG_GUESS", "1");
        TagInfo t3; std::string why3;
        ck(rta::meta::native::probe_file(U8(p2), &t3, &why3) &&
               t3.title == "\xE4\xB8\x8A\xE6\xB5\xB7\xE7\xB4\x85\xE8\x8C\xB6\xE9\xA4\xA8 (Chinese Tea)",
           "开关 ON：同一条夹具被修好（证明开关真的起作用）", t3.title);
    }

}


// ---------------------------------------------------------------------------
// [7] 261026-02 #4 真实库判据：MUSIC 全量逐文件跑**两条路径**
//   (a) RLPLAYER_TAG_GUESS=1（智能修复开）
//   (b) RLPLAYER_TAG_GUESS=0（关，= 261025-02 的行为）
// 判据：
//   1) **非占位标签零改动**：repair 里没有任何 '?' 的字段，开/关两次必须逐字节相同。
//      （这是"正常标签一个字节都不许改"的直接证据，不看汇总，逐字段比。）
//   2) 修前带 '?' 的行数 -> 修后行数，逐行打印「原值 -> 新值」。
//   3) 修复只减不增：修复后带 '?' 的行数必须 <= 修前。
// 只做只读探测（probe_file），不写库、不碰索引，不起 ffprobe。
// ---------------------------------------------------------------------------
static void tagGuessRealLibrary(const fs::path& root) {
    std::printf("\n== [7] 问号标签智能修复：真实库逐条对比 ==\n");
    const fs::path music = root / "MUSIC";
    std::error_code ec;
    std::vector<std::string> files;
    if (fs::is_directory(music, ec)) {
        for (auto& de : fs::recursive_directory_iterator(music, fs::directory_options::skip_permission_denied, ec)) {
            if (!de.is_regular_file(ec)) continue;
            const std::string e = rta::meta::native::detail::lower_ext(U8(de.path()));
            if (e == ".flac" || e == ".wav" || e == ".wave" || e == ".mp3" || e == ".ogg") files.push_back(U8(de.path()));
        }
    }
    if (files.empty()) {
        std::printf("  [WARN] 没有 MUSIC（或没有可解析文件），本节**未覆盖真实库**（铁律 4：不假装覆盖）\n");
        return;
    }
    struct Row { std::string file; TagInfo on, off; };
    std::vector<Row> rows;
    int zeroChangeViolations = 0, reparsedOn = 0;
    std::string firstViolation;
    for (const auto& f : files) {
        _putenv_s("RLPLAYER_TAG_GUESS", "0");
        Row r; r.file = f;
        const bool okOff = rta::meta::native::probe_file(f, &r.off, nullptr);
        _putenv_s("RLPLAYER_TAG_GUESS", "1");
        const bool okOn = rta::meta::native::probe_file(f, &r.on, nullptr);
        if (!okOff || !okOn) continue;            // 回退 ffprobe 的文件不参与（本节只测原生通道）
        ++reparsedOn;
        // 判据 1：repair 值里"没有任何 '?'"的字段，必须与关闭时逐字节相同
        auto cmp = [&](const char* name, const std::string& a, const std::string& b) {
            if (countQ(a) != 0) return;            // 只检查"非占位"字段
            if (a == b) return;
            ++zeroChangeViolations;
            if (firstViolation.empty())
                firstViolation = std::string(name) + " @ " + r.file + " off=[" + a + "] on=[" + b + "]";
        };
        cmp("title", r.off.title, r.on.title);
        cmp("artist", r.off.artist, r.on.artist);
        cmp("album", r.off.album, r.on.album);
        rows.push_back(std::move(r));
    }
    ck(zeroChangeViolations == 0, "非占位标签**零改动**（开/关两次逐字段相同）", firstViolation);
    ck(reparsedOn == (int)files.size(), "全部文件都能原生解析（本节覆盖 100% 文件）",
       std::to_string(reparsedOn) + "/" + std::to_string(files.size()));

    int rowsQBefore = 0, rowsQAfter = 0, fieldQBefore = 0, fieldQAfter = 0;
    for (const auto& r : rows) {
        const int qb = (int)(countQ(r.off.title) + countQ(r.off.artist) + countQ(r.off.album));
        const int qa = (int)(countQ(r.on.title) + countQ(r.on.artist) + countQ(r.on.album));
        if (qb) ++rowsQBefore;
        if (qa) ++rowsQAfter;
        fieldQBefore += qb;
        fieldQAfter  += qa;
        if (!qb) continue;                          // 只打印修前带问号的行（人工可读）
        std::printf("  %s\n", U8(fs::path(W(r.file)).filename()).c_str());
        auto line = [&](const char* label, const std::string& a, const std::string& b) {
            if (countQ(a) == 0 && a == b) return;
            std::printf("       %-6s 原[%s] -> 新[%s]%s\n", label, a.c_str(), b.c_str(),
                        countQ(b) ? "   <<仍含?" : "");
        };
        line("title", r.off.title, r.on.title);
        line("artist", r.off.artist, r.on.artist);
        line("album", r.off.album, r.on.album);
    }
    std::printf("  汇总：文件 %zu，带 '?' 的行 %d -> %d；带 '?' 的字段 %d 个 -> %d 个\n",
                rows.size(), rowsQBefore, rowsQAfter, fieldQBefore, fieldQAfter);
    ck(rowsQAfter <= rowsQBefore, "修复只减不增（带 '?' 的行数不上升）",
       std::to_string(rowsQBefore) + " -> " + std::to_string(rowsQAfter));
    ck(rowsQBefore == 0 || rowsQAfter < rowsQBefore, "至少修好一行（否则本规则在本库上没有收益，需复核）",
       std::to_string(rowsQBefore) + " -> " + std::to_string(rowsQAfter));
    _putenv_s("RLPLAYER_TAG_GUESS", "1");
}
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== 原生标签快速通道验证（FLAC/WAV/MP3/OGG） ==\n");
    const fs::path root = (argc > 1) ? fs::path(W(argv[1])) : findRepoRoot();
    std::printf("仓库根：%s\n", U8(root).c_str());
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / "rta_tags_native_test";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    setLibDir(tmp / "lib");

    // ffprobe 可用性（A/B 与回退都要它；缺了就没法证明任何事）
    const std::string ffprobe = rta::audio::find_tool("ffprobe");
    ck(!ffprobe.empty(), "ffprobe 可用（回退路径的前提）", ffprobe);

    unitTests(tmp);
    const int compared = abReal(root);
    std::printf("\n[INFO] A/B 比对文件数 = %d\n", compared);
    wavSpecial(root);
    tagGuessFixtures(tmp / "qmark");
    tagGuessRealLibrary(root);
    throughputAndRegression(root);

    fs::remove_all(tmp, ec);
    std::printf("\n%s（通过 %d 项，失败 %d 项）\n", g_fail == 0 ? "全部通过" : "存在失败",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
// 运行：ctest -R tags_native（已登记进 CMakeLists）；或单独编译：
//   & "C:\Program Files\AMD\ROCm\7.2\bin\clang++.exe" -std=c++23 -O2 -w -I src ^
//       tests\test_tags_native.cpp -o build-tags2\test_tags_native.exe
//   .\build-tags2\test_tags_native.exe           # 在仓库根运行；MUSIC 不存在时自动降级
//   $env:RL_TEST_TAGS_SCALE=3000                 # 可选：吞吐夹具规模（默认 1000）
