// test_tags_native.cpp — 原生标签快速通道（261025-01 一.1/一.8：首次全量扫描 15000 首要等几分钟）
//
// 判据（铁律 3：每条判据都能被单独否定）：
//   [1] 单元：**手工构造** 4 类容器的字节流（不依赖 ffmpeg/ffprobe），逐字段断言解析结果；
//       同时断言「该回退的必须回退」：RF64 / WAV 里的 "id3 " 块 / bext / ID3v2.2 / unsynchronisation /
//       数字流派引用 / Opus / FLAC total_samples=0 / OGG 末页缺失 …… 一条条点名，回退错了就是红。
//   [2] A/B（唯一能证明「开关变量不影响入库结果」的判据）：build/testdata 与 MUSIC 里各取真实文件
//       （FLAC/WAV/MP3/OGG 各 5 个），用 lib_probe_one 在 RLPLAYER_NATIVE_TAGS=1/0 下逐字段比对
//       （title/artist/album/albumArtist/genre/year/trackNo/discNo/sr/bits/ch/hasCover + 时长 ±1 帧）。
//   [3] 吞吐：N 个「1 秒静音 FLAC」的**首次全量扫描**（Library::startScan(true)，与 /api/lib/scan 同一入口）
//       对比「每个文件起一次 ffprobe」的基线，给出首/s 与单首耗时（附 3000/15000 首的推算）。
//   [4] 回归：同一份夹具，RLPLAYER_NATIVE_TAGS=0/1 各扫一次，**索引文件必须逐字节相同**
//       （列数/字段/时长小数位全都要对得上，只比字段不算数）。
//
// ⚠️ 未解决项（铁律 4）：
//   1) 真实 WAV 里带 "id3 " 块 / bext / iXML / axml 的，原生通道**主动回退** ffprobe（这些块的标签
//      语义与 INFO 不同、且与块顺序有关，实测本库 71 个 WAV 里 52 个属于这一类）⟹ 它们的扫描速度
//      仍是 ffprobe 水平。本测试的 A/B 覆盖它们（结果一致），吞吐夹具用的是纯 flac。
//   2) 本文件未登记进 CMakeLists（本任务允许的文件范围不含 CMakeLists.txt）。
//      编译/运行方式见文件末尾注释里的命令行。
//   3) MUSIC 不存在时 [2] 只用 build/testdata 的文件并如实打印「真实文件数不足」，不假装覆盖。
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

// ---- WAV ----
struct WavSpec {
    int sr = 44100, ch = 2, bits = 16, fmtTag = 1;
    long long frames = 44100;                      // 1.0 s
    long long dataDecl = -1;                       // -1 = 与真实字节一致；其它值用于「声明超长」
    std::vector<std::pair<std::string, std::string>> info;   // INAM/IART/IPRD/IGNR/ICRD/ITRK
    bool id3chunk = false, bext = false, rf64 = false, extensible = false;
};
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
    if (s.bext) {
        std::vector<unsigned char> b(602, 0);
        std::memcpy(b.data(), "desc", 4);
        putStr(chunks, "bext"); put32le(chunks, (uint32_t)b.size()); put(chunks, b.data(), b.size());
    }
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
    if (s.id3chunk) {
        std::vector<unsigned char> frame;
        putStr(frame, "TIT2");
        std::vector<unsigned char> body; body.push_back(0); putStr(body, "ID3T");
        put32be(frame, (uint32_t)body.size()); put16be(frame, 0); put(frame, body.data(), body.size());
        std::vector<unsigned char> tag;
        putStr(tag, "ID3"); tag.push_back(3); tag.push_back(0); tag.push_back(0);
        syncsafe32(tag, (uint32_t)frame.size());
        put(tag, frame.data(), frame.size());
        putStr(chunks, "id3 "); put32le(chunks, (uint32_t)tag.size()); put(chunks, tag.data(), tag.size());
        if (tag.size() & 1u) chunks.push_back(0);
    }
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
    // ---- WAV：必须回退的四种 ----
    {
        WavSpec r; r.rf64 = true;
        const fs::path p = dir / "rf64.wav";
        writeFile(p, buildWav(r));
        TagInfo t; std::string why;
        ck(!probeNative(p, t, why), "RF64 ⟹ 回退 ffprobe", why);

        WavSpec i; i.id3chunk = true;
        const fs::path q = dir / "id3chunk.wav";
        writeFile(q, buildWav(i));
        TagInfo t2; std::string why2;
        ck(!probeNative(q, t2, why2) && why2.find("id3") != std::string::npos,
           "WAV 含 'id3 ' 块 ⟹ 回退 ffprobe（那里的 ID3 标签我们不解）", why2);

        WavSpec b; b.bext = true;
        const fs::path rp = dir / "bext.wav";
        writeFile(rp, buildWav(b));
        TagInfo t3; std::string why3;
        ck(!probeNative(rp, t3, why3) && why3.find("bext") != std::string::npos,
           "WAV 含 bext ⟹ 回退 ffprobe（origination_date 会变成 year）", why3);
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
            setNative(false);
            const LibTrack ff = rta::net::lib_probe_one(f);
            setNative(true);
            const unsigned long long before = rta::meta::native::stats().parsed.load();
            const LibTrack nt = rta::net::lib_probe_one(f);
            const bool hit = rta::meta::native::stats().parsed.load() != before;
            std::string why;
            if (!hit) {
                TagInfo tmpTags;
                rta::meta::native::probe_file(f, &tmpTags, &why);    // 只为拿到回退原因（会重复解析一次，可忽略）
            }
            ++compared;
            std::vector<std::string> d;
            auto eq = [&](const char* n, const std::string& a, const std::string& b) {
                if (a != b) d.push_back(std::string(n) + " ff=[" + a + "] nat=[" + b + "]");
            };
            eq("title", ff.title, nt.title);
            eq("artist", ff.artist, nt.artist);
            eq("album", ff.album, nt.album);
            eq("albumArtist", ff.albumArtist, nt.albumArtist);
            eq("genre", ff.genre, nt.genre);
            if (ff.trackNo != nt.trackNo) d.push_back("track " + std::to_string(ff.trackNo) + "/" + std::to_string(nt.trackNo));
            if (ff.discNo != nt.discNo) d.push_back("disc " + std::to_string(ff.discNo) + "/" + std::to_string(nt.discNo));
            if (ff.year != nt.year) d.push_back("year " + std::to_string(ff.year) + "/" + std::to_string(nt.year));
            if (ff.sampleRate != nt.sampleRate) d.push_back("sr " + std::to_string(ff.sampleRate) + "/" + std::to_string(nt.sampleRate));
            if (ff.bits != nt.bits) d.push_back("bits " + std::to_string(ff.bits) + "/" + std::to_string(nt.bits));
            if (ff.channels != nt.channels) d.push_back("ch " + std::to_string(ff.channels) + "/" + std::to_string(nt.channels));
            if (ff.hasCover != nt.hasCover) d.push_back("cover " + std::to_string(ff.hasCover) + "/" + std::to_string(nt.hasCover));
            const double frame = ff.sampleRate > 0 ? 1.0 / ff.sampleRate : 1e-6;
            if (!(std::fabs(ff.duration - nt.duration) <= frame))
                d.push_back("dur " + std::to_string(ff.duration) + "/" + std::to_string(nt.duration));
            std::string base = U8(fs::path(W(f)).filename());
            std::printf("     %s %-28s dur=%.3f->%.3f cover=%d/%d %s\n",
                        hit ? "NAT" : "FBK", base.c_str(), ff.duration, nt.duration, ff.hasCover, nt.hasCover,
                        hit ? "" : why.c_str());
            if (!d.empty()) {
                ++diffs;
                std::printf("       [DIFF] %s\n", f.c_str());
                for (auto& x : d) std::printf("              %s\n", x.c_str());
            }
        }
    }
    ck(diffs == 0, "A/B：全部真实文件逐字段一致（含 hasCover / 位深 / 时长 ±1 帧）",
       std::to_string(diffs) + " 处不一致");
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
    ck(!a.empty() && a.size() == b.size(), "两次扫描的索引文件大小相同",
       std::to_string(a.size()) + " / " + std::to_string(b.size()));
    ck(a == b, "索引逐字节相同（曲目数/专辑数/所有字段/时长小数位）",
       a == b ? "" : "首个差异位置 " + std::to_string(a.find_first_not_of(b)));
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
    throughputAndRegression(root);

    fs::remove_all(tmp, ec);
    std::printf("\n%s（通过 %d 项，失败 %d 项）\n", g_fail == 0 ? "全部通过" : "存在失败",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
// 编译/运行（本文件未登记进 CMakeLists，见文件头「未解决项 2」）：
//   & "C:\Program Files\AMD\ROCm\7.2\bin\clang++.exe" -std=c++23 -O2 -w -I src ^
//       tests\test_tags_native.cpp -o build-tags\test_tags_native.exe
//   .\build-tags\test_tags_native.exe            # 在仓库根运行；MUSIC 不存在时自动降级
//   $env:RL_TEST_TAGS_SCALE=3000                 # 可选：吞吐夹具规模（默认 1000）
