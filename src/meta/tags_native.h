// src/meta/tags_native.h — 原生标签快速通道（FLAC / WAV / MP3 / OGG）
//
// 为什么要有这个文件（261025-01 一.1 / 一.8「15000 首要等几分钟」）：
//   旧实现每次全量扫描对**每个文件**起一次 ffprobe 进程（library.h lib_probe_one），
//   实测 45.9 首/s（3000 首 65.4 s）。瓶颈不是解码，而是 CreateProcess + ffprobe 启动。
//   FLAC/WAV/MP3/OGG 四类容器的「标题/艺术家/专辑/碟号/流派/年份/采样率/位深/声道/时长/封面有无」
//   全都在文件头部几个 KB 里，本地解析 0.1~0.5 ms 就能拿到，不需要进程。
//
// 设计约束（铁律 3/4：判据要能单独否定，未解决项必须写明）：
//   1. **只做加法，不做假设**：任何结构异常/不支持的变体 ⟹ 返回 false，调用方原样回退 ffprobe。
//      绝不「猜一个值」—— 解析器宁可什么都不给，也不能给错（错值会进索引，之后按 mtime+size 复用）。
//   2. **与 ffprobe 的语义逐字段对齐**，因为回归判据是「同一份夹具下，开关 RLPLAYER_NATIVE_TAGS 的
//      扫描结果完全一致」。为此本文件刻意复刻了 ffprobe/ffmpeg 的若干行为（每条都有实测依据，见下）：
//        * FLAC 时长 = STREAMINFO.total_samples / sample_rate（不是按文件大小估）
//        * WAV 时长 = data 字节 / block_align / sample_rate（data 声明长度超出文件时按实际长度截断）
//        * MP3 有 Xing/Info：时长 = (frames*spf - LAME delay - LAME padding) / sr
//          （实测 10 个真实 MP3：只算 frames*spf 会偏 26~51 ms = 1~2 帧；减掉 LAME 头里的
//            delay/padding 后与 ffprobe **逐样本相等**）
//        * MP3 无 Xing（纯 CBR）：时长 = (文件字节 - ID3v2 长度) * 8 / 首帧码率
//          （实测 noxing 夹具与 ffprobe 相等；注意 ffmpeg **不减** ID3v1 的 128 字节，这里也不减）
//        * OGG 时长 = 末页 granulepos / 采样率（实测与 ffprobe 逐样本相等）
//        * 位深：ffprobe 的 bits_per_raw_sample 对 16-bit WAV / MP3 / OGG 是 **N/A**，旧代码于是
//          统一写成 16；原生解析必须复刻这一点（WAV 16-bit→16、24-bit→24，MP3/OGG→16）。
//   3. **整体关闭**：环境变量 RLPLAYER_NATIVE_TAGS=0（也认 off/false/no）⟹ 一律走 ffprobe。
//
// 能力与保底（每条都对应本文件里的 early-return）：
//   FLAC：STREAMINFO（采样率/位深/声道/总样本）+ VORBIS_COMMENT（TITLE/ARTIST/ALBUM/
//         ALBUMARTIST/DATE/GENRE/TRACKNUMBER/DISCNUMBER）+ PICTURE 块与 METADATA_BLOCK_PICTURE
//         （**只判 ffprobe 会不会建封面流**：MIME 必须是它认识的图片类型，见 picture_mime_accepted）；
//         跳过 ID3v2 前缀（ffprobe 对 FLAC 也只认 Vorbis comment，实测）；total_samples=0（流式 FLAC）⟹ 回退。
//   WAV ：RIFF/WAVE + fmt （PCM/float，含 WAVE_FORMAT_EXTENSIBLE）+ data 长度算时长
//         + LIST/INFO（INAM/IART/IPRD/IGNR/ICRD/ITRK/IPRT）
//         + "id3 " 块（ID3v2 in RIFF；帧解析与 MP3 **共用** parse_id3v2_tag）
//         + bext（origination_date → date，偏移 320 起 10 字节）。
//         三类标签合并成最终值，口径是**先到先得**（第一个非空值赢）—— 不是 ffmpeg 的语义，
//         而是 net::library.h lib_probe_one 的**实际**行为（段标记因 CRLF 匹配不上，见未解决项 8）.
//         APIC（id3 块里）的有效性判据与 MP3 相同：MIME 必须是 ffprobe 认识的图片类型才建封面流。
//         RF64/RIFX/非 PCM/iXML/axml ⟹ 回退（XML 元数据的字段映射未实测，见未解决项 5）。
//   MP3 ：ID3v2.3/2.4 文本帧（TIT2/TPE1/TALB/TPE2/TPOS/TCON/TDRC/TYER/TRCK）+ APIC
//         + 首帧 Xing/Info（含 LAME delay/padding）或 CBR 估算。ID3v2.2 / unsynchronisation /
//         帧标志位 / 数字流派引用（TCON='(17)'）⟹ 回退。
//         ⚠️ **ID3v1 故意不读**：实测 ffprobe 9.0 对「只有 ID3v1」的 MP3 返回**零标签**，
//            读了反而与 ffprobe 不一致（会把「没有标签」变成「有标签」）。
//         ⚠️ APIC 的**描述**会成为封面流的 title；ffprobe 先打印流、后打印 format，而 lib_probe_one
//            只取第一个非空值 ⟹ **描述压过 TIT2**（实测：TIT2=TitleFromTIT2 + 描述=DescFromAPIC
//            时 ffprobe 路径给的是 DescFromAPIC）。这里照做，否则 A/B 会红。
//   OGG ：第一页的 Vorbis identification（声道/采样率）+ 注释包（可跨页）
//         的 TITLE/ARTIST/ALBUM/ALBUMARTIST/DATE/GENRE/TRACKNUMBER/DISCNUMBER
//         + METADATA_BLOCK_PICTURE（base64 前缀解出 MIME 后按同一口径判）+ 末页 granulepos 算时长。
//         Opus/Speex/Theora/链式流/找不到末页 ⟹ 回退。
//
// 实测覆盖（MUSIC）：WAV 71/71 全部原生（原先 19/71 —— 52 个带 "id3 " 块/bext 的此前每次都要起
// ffprobe 进程）；四类容器合计 654 个文件，原生 654（100%）。build/testdata 98 个文件也是 100%。
// 扫描吞吐见 tests/test_tags_native.cpp 的 [3]，WAV 专项 A/B 见 [5]。
//
// ⚠️ 未解决项（铁律 4）：
//   1) ID3v2 文本帧 enc=0（ISO-8859-1）按**原字节透传**，与 ffmpeg 的行为一致（实测）；但如果将来
//      ffmpeg 改成转码 Latin-1，这里会与 ffprobe 有非 ASCII 差异（真实库的 MP3 全是 enc=1，A/B 未出现）。
//   2) TCON 数字流派引用（"(17)" / "17"）会回退 ffprobe —— 没抄 ffmpeg 的 192 条流派表，
//      抄不全就是错值，宁可慢一次。
//   3) WAV 的 LIST/INFO 值按原字节透传（GBK 中文标签会与 ffprobe 一样是非法 UTF-8 字节，行为一致）。
//   4) 只覆盖 4 类容器；ape/wv/m4a/dsf/dff 等一律回退 ffprobe。
//   5) WAV 的 iXML/axml 仍回退 ffprobe（XML 字段到 ffprobe 键的映射没实测过，本库 0 个样本）。
//   6) WAV 的 "id3 " 块里的 ID3v2.2 / unsynchronisation / 帧标志位同样回退（与 MP3 同口径）；
//      bext 长度 < 337 也回退 —— 实测 ffmpeg 9.0 对短 bext 会**整份文件报 invalid data**，
//      这时原生解析"成功"反而与 ffprobe 不一致。
//   7) 数字流派引用（TCON="(17)"）只有**真的入库**才回退：WAV 里如果前面的 LIST/INFO 已经占了
//      genre（先到先得），这个数字值根本进不了最终标签（实测），就不必多起一个 ffprobe。
//   8) ⚠️ **依赖 library.h 的一个既有缺陷**（不在本任务可改范围，记录在案）：lib_probe_one 用
//      `-of default=nw=0` 的段标记 [FORMAT]/[STREAM] 判断标签权威性，但 ffprobe 输出是 CRLF，
//      行末的 '\r' 让 `line == "[FORMAT]"` **永远为假** ⟹ inFormat 恒 false ⟹ put() 只在目标为空时
//      赋值。后果有两个：
//        a) 多个来源的同键标签（WAV 的 LIST + id3 块，ffprobe 会打出两条）取**第一条**，与 ffmpeg
//           的"后者覆盖"相反；本文件因此也按"先到先得"实现（否则 A/B 与索引逐字节两条判据全红）。
//        b) 流标签（APIC 描述 / OGG 的流内 Vorbis comment）会**压过** format 标签，与代码注释里
//           写的"format 优先、缺失回退 stream"不一致。
//      修 library.h 会让本条与本文件的合并口径一起失效，必须同步改（并重跑 [5] 的两条判据）。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include "platform/windows/win_text.h"   // rta::platform::detail::widen（UTF-8 -> UTF-16，含 windows.h）
#endif

namespace rta::meta::native {

// ---------------------------------------------------------------------------
// 解析结果：字段与 net::LibTrack 一一对应（lib_probe_one 原样拷贝）
// ---------------------------------------------------------------------------
struct TagInfo {
    std::string title, artist, album, albumArtist, genre, date;
    int    trackNo = 0, discNo = 0, year = 0;
    int    sampleRate = 0, channels = 0, bits = 0;
    double duration = 0.0;
    bool   hasCover = false;
    std::string codec;        // ffprobe 的 codec_name（flac / pcm_s16le / mp3 / vorbis）
};

// 统计（无锁原子量）：调用方用它报告「原生命中 / 回退」比例。
struct SharedStats {
    std::atomic<unsigned long long> attempted{0};
    std::atomic<unsigned long long> parsed{0};
    std::atomic<unsigned long long> fellBack{0};
};
inline SharedStats& stats() { static SharedStats s; return s; }

// RLPLAYER_NATIVE_TAGS=0/off/false/no ⟹ 关闭原生通道（默认开）
//
// 为什么用 GetEnvironmentVariableW 而不是 _wgetenv：扫描是 8 线程并行，每个文件都会问一次开关；
// MS CRT 的 _wgetenv 返回的是**共享缓冲**（它自己被标记为 deprecated，建议 _wdupenv_s），
// 并发读有撕裂风险。GetEnvironmentVariableW 是线程安全的，且不用分配内存。
inline bool enabled() {
#ifdef _WIN32
    wchar_t v[64] = {0};
    const DWORD n = ::GetEnvironmentVariableW(L"RLPLAYER_NATIVE_TAGS", v, 64);
    if (n == 0 || v[0] == L'\0') return true;              // 未设置 = 开
    std::wstring s(v, (n > 64 ? 64 : (size_t)n));
    std::wstring low;
    for (wchar_t c : s) low.push_back((wchar_t)::towlower((wint_t)c));
    // 去首尾空白（用户可能写 " 0 "）
    const size_t a = low.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return true;
    const size_t b = low.find_last_not_of(L" \t\r\n");
    low = low.substr(a, b - a + 1);
    return !(low == L"0" || low == L"off" || low == L"false" || low == L"no");
#else
    const char* v = std::getenv("RLPLAYER_NATIVE_TAGS");
    if (!v || !*v) return true;
    std::string s(v);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return !(s == "0" || s == "off" || s == "false" || s == "no");
#endif
}

namespace detail {

// ---- 小工具 ----
inline uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
inline uint64_t be64(const unsigned char* p) { return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4); }
inline uint64_t le64s(const unsigned char* p) {   // granulepos 是有符号 64 位，-1 表示无
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
// ID3v2 的「syncsafe」整数（每字节只用 7 位）
inline uint32_t syncsafe28(const unsigned char* p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}
inline bool starts_with(const unsigned char* p, size_t n, const char* lit) {
    const size_t m = std::strlen(lit);
    return n >= m && std::memcmp(p, lit, m) == 0;
}
// 与 net/library.h lib_probe_one 的 put() 完全相同的尾部修剪（'\r' 与空格），再加 NUL 与制表语料的尾部清理。
// 为什么要在这里做：ffprobe 路径的标签值会经过 lib_probe_one 的 put() 修剪，原生路径必须给出同样的值，
// 否则「开关变量的 A/B」在带尾随空格的标签上会不一致。
inline std::string trim_tag_tail(std::string v) {
    // 尾部清理与 net/library.h lib_probe_one 的 put() **逐字符一致**：只砍 '\r' 与空格；
    // 额外砍 '\0'（ID3/INFO 的 C 字符串终止符在 ffprobe 的输出里本来就被截断了）。
    // 刻意不砍 '\t'/'\n'：put() 不砍，多砍就会与 ffprobe 路径产生差异。
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\0')) v.pop_back();
    return v;
}
inline std::string lower_ascii(std::string k) {
    for (auto& c : k) c = (char)std::tolower((unsigned char)c);
    return k;
}

// UTF-16（LE/BE）→ UTF-8。失败（非法代理对外的超长/截断）返回 false ⟹ 调用方回退。
inline bool utf16_to_utf8(const unsigned char* p, size_t n, bool big, std::string* out) {
    out->clear();
    if (n & 1u) return false;                       // 奇数长度：编码坏了
    if (n == 0) return true;
#ifdef _WIN32
    std::wstring w;
    w.reserve(n / 2);
    for (size_t i = 0; i + 1 < n; i += 2) {
        const uint16_t u = big ? (uint16_t)((p[i] << 8) | p[i + 1]) : (uint16_t)(p[i] | (p[i + 1] << 8));
        w.push_back((wchar_t)u);
    }
    const int m = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (m <= 0) return false;
    std::string s((size_t)m, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), m, nullptr, nullptr);
    *out = std::move(s);
    return true;
#else
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = big ? (uint32_t)((p[i] << 8) | p[i + 1]) : (uint32_t)(p[i] | (p[i + 1] << 8));
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 3 >= n) return false;
            const uint32_t lo = big ? (uint32_t)((p[i + 2] << 8) | p[i + 3])
                                    : (uint32_t)(p[i + 2] | (p[i + 3] << 8));
            if (lo < 0xDC00 || lo > 0xDFFF) return false;
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;
        }
        if (u < 0x80) out->push_back((char)u);
        else if (u < 0x800) { out->push_back((char)(0xC0 | (u >> 6))); out->push_back((char)(0x80 | (u & 0x3F))); }
        else if (u < 0x10000) {
            out->push_back((char)(0xE0 | (u >> 12)));
            out->push_back((char)(0x80 | ((u >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (u & 0x3F)));
        } else {
            out->push_back((char)(0xF0 | (u >> 18)));
            out->push_back((char)(0x80 | ((u >> 12) & 0x3F)));
            out->push_back((char)(0x80 | ((u >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (u & 0x3F)));
        }
    }
    return true;
#endif
}

// ---------------------------------------------------------------------------
// 只读随机访问的文件读取器（UTF-8 路径 → 宽字符 path；每线程一个实例）
// ---------------------------------------------------------------------------
class FileReader {
public:
    bool open(const std::string& utf8Path) {
        std::error_code ec;
        std::filesystem::path p = make_path(utf8Path);
        size_ = (long long)std::filesystem::file_size(p, ec);
        if (ec) { ok_ = false; return false; }
        f_.open(p, std::ios::binary);
        ok_ = (bool)f_ && size_ > 0;
        return ok_;
    }
    bool ok() const { return ok_; }
    long long size() const { return size_; }
    // 精确读 n 字节（越界/短读返回 false）
    bool read_at(long long off, void* dst, size_t n) const {
        if (off < 0 || n == 0 || off + (long long)n > size_) return false;
        f_.clear();
        f_.seekg((std::streamoff)off, std::ios::beg);
        f_.read((char*)dst, (std::streamsize)n);
        return (size_t)f_.gcount() == n;
    }
    size_t read_up_to(long long off, void* dst, size_t n) const {
        if (off < 0 || off >= size_) return 0;
        const size_t want = (size_t)std::min<long long>((long long)n, size_ - off);
        if (want == 0) return 0;
        f_.clear();
        f_.seekg((std::streamoff)off, std::ios::beg);
        f_.read((char*)dst, (std::streamsize)want);
        return (size_t)f_.gcount();
    }
private:
    static std::filesystem::path make_path(const std::string& utf8) {
#ifdef _WIN32
        return std::filesystem::path(rta::platform::detail::widen(utf8));   // 宽字符构造（见 library.h p8 注释）
#else
        return std::filesystem::path(std::u8string((const char8_t*)utf8.data(), utf8.size()));
#endif
    }
    mutable std::ifstream f_;
    long long size_ = 0;
    bool ok_ = false;
};

// ---------------------------------------------------------------------------
// Vorbis Comment（FLAC 元数据块与 OGG 注释包共用）：vendor_len + count + [len + "KEY=value"]*
// 键名大小写不敏感；重复键「后者覆盖前者」（与 ffmpeg 把每条都 av_dict_set 的行为一致）。
// 已知键到字段的映射刻意复刻 ffprobe 的 vorbiscomment metadata_conv：
//   ALBUMARTIST -> album_artist、TRACKNUMBER -> track、DISCNUMBER -> disc
// 但也接受 ALBUM_ARTIST/TRACK/DISC 这类本身就叫 album_artist/track/disc 的键 —— 因为
// lib_probe_one 是「键名小写后按 album_artist/track/disc 取值」，ffprobe 会把未知键原样打出来，
// 所以这两种拼写在 ffprobe 路径上是**同一个结果**。
// ---------------------------------------------------------------------------
struct VorbisCommentOut { bool hasPicture = false; bool hasCoverArt = false; };

// ---------------------------------------------------------------------------
// 内嵌封面「存在性」判定的唯一口径（2026-10-25 实测，必须与 ffprobe 一致）：
//   ffmpeg 只在 MIME 是它认识的图片类型时才真的**建一条 video 流**；其它情况（空 MIME / 未知类型 /
//   MIME 长度 > 64）它只是忽略（有的还会打一行 warning）。而 lib_probe_one 的 hasCover 判据就是
//   「有没有 codec_type=video 的流」——所以原生通道也必须按 MIME 判，而不是「见到 PICTURE 块就算」。
//   实测（本机 ffprobe 9.0）：
//     FLAC PICTURE：image/jpeg|jpg|png|gif|bmp|webp|tiff → 有 video 流；"" / "image/xyz" /
//                   "image/JPEG"（大写）/ 64~65 字节未知串 → 无 video 流。**大小写敏感**。
//     ID3 APIC   ：同一组类型都建流，且**大小写不敏感**（image/JPEG 也建流）。
//   真实库现状（MUSIC 572 个 FLAC）：333 个文件的 PICTURE 块 MIME 是**空串**（作者工具写坏了），
//   ffprobe 因此报 hasCover=0 —— 原生命中同样给 0，才与旧索引逐字段一致。
// ---------------------------------------------------------------------------
inline bool mime_is_known_image(const std::string& mime, bool caseInsensitive) {
    static const char* kOk[] = { "image/jpeg", "image/jpg", "image/png", "image/gif",
                                 "image/bmp", "image/webp", "image/tiff" };
    for (const char* k : kOk) {
        if (mime.size() != std::strlen(k)) continue;
        if (caseInsensitive) { if (_strnicmp(mime.c_str(), k, mime.size()) == 0) return true; }
        else if (mime == k) return true;
    }
    return false;
}
// p 指向 FLAC picture 结构起点：[type u32][mime_len u32][mime ...]
inline bool picture_mime_accepted(const unsigned char* p, size_t n, bool caseInsensitive) {
    if (n < 8) return false;
    const uint32_t mimeLen = be32(p + 4);
    if (mimeLen == 0 || mimeLen > 64) return false;
    if (n < 8 + (size_t)mimeLen) return false;
    return mime_is_known_image(std::string((const char*)p + 8, (size_t)mimeLen), caseInsensitive);
}
// ID3 APIC 帧体：encoding(1) + MIME(ISO-8859-1, NUL 结尾) + picture type(1) + description + data
inline bool apic_mime_accepted(const unsigned char* body, size_t n) {
    if (n < 3) return false;
    size_t i = 1;
    while (i < n && body[i] != 0) ++i;
    const size_t mimeLen = i - 1;
    if (mimeLen == 0 || mimeLen > 64) return false;
    return mime_is_known_image(std::string((const char*)body + 1, mimeLen), true);   // APIC 大小写不敏感（实测）
}

// base64 前缀解码（METADATA_BLOCK_PICTURE 是 base64 的 FLAC picture 结构；只要前 ~96 字节即可判 MIME）
inline int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
inline bool base64_prefix(const char* s, size_t n, size_t need, std::vector<unsigned char>& out) {
    out.clear();
    out.reserve(need);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n && out.size() < need; ++i) {
        const int v = b64val((unsigned char)s[i]);
        if (v < 0) return false;                 // '=' 或非法字符：前缀里不该出现
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((acc >> bits) & 0xFF));
        }
    }
    return out.size() >= need;
}
// 从 base64 的 METADATA_BLOCK_PICTURE 值判断「ffprobe 会不会建封面流」
inline bool comment_picture_has_cover(const std::string& b64) {
    std::vector<unsigned char> head;
    if (!base64_prefix(b64.data(), b64.size(), 8 + 64, head)) {
        // 长度不够 72 字节 → 图片结构必然读不全，ffmpeg 会失败 ⟹ 不算有封面
        return false;
    }
    return picture_mime_accepted(head.data(), head.size(), false);
}

inline void apply_vorbis_tag(TagInfo& t, const std::string& key, const std::string& val) {
    const std::string v = trim_tag_tail(val);
    if (key == "title")                     t.title = v;
    else if (key == "artist")               t.artist = v;
    else if (key == "album")                t.album = v;
    else if (key == "albumartist" || key == "album_artist") t.albumArtist = v;
    else if (key == "date")                 t.date = v;
    else if (key == "genre")                t.genre = v;
    else if (key == "track" || key == "tracknumber") t.trackNo = std::atoi(v.c_str());
    else if (key == "disc" || key == "discnumber")   t.discNo = std::atoi(v.c_str());
}

inline bool parse_vorbis_comment(const unsigned char* p, size_t n, TagInfo& t, VorbisCommentOut& vo,
                                 std::string& why) {
    if (n < 8) { why = "Vorbis comment 过短"; return false; }
    size_t q = 0;
    const uint32_t vendor = le32(p + q);
    q += 4;
    if (vendor > n - q) { why = "Vorbis vendor 长度越界"; return false; }
    q += vendor;
    if (q + 4 > n) { why = "Vorbis comment 计数缺失"; return false; }
    const uint32_t cnt = le32(p + q);
    q += 4;
    if (cnt > 100000u) { why = "Vorbis comment 条数异常"; return false; }
    for (uint32_t i = 0; i < cnt; ++i) {
        if (q + 4 > n) { why = "Vorbis comment 条目长度缺失"; return false; }
        const uint32_t len = le32(p + q);
        q += 4;
        if (len > n - q) { why = "Vorbis comment 条目越界"; return false; }
        const char* s = (const char*)(p + q);
        const char* eq = (const char*)std::memchr(s, '=', len);
        if (eq) {
            std::string key = lower_ascii(std::string(s, (size_t)(eq - s)));
            std::string val((const char*)(eq + 1), len - (size_t)(eq - s) - 1);
            if (key == "metadata_block_picture") vo.hasPicture = vo.hasPicture || comment_picture_has_cover(val);
            else if (key == "coverart")          vo.hasCoverArt = true;   // 非标准键：留给 ffprobe 判定
            apply_vorbis_tag(t, key, val);
        }
        q += len;
    }
    return true;
}

// ---------------------------------------------------------------------------
// FLAC
// ---------------------------------------------------------------------------
inline bool parse_flac(FileReader& r, TagInfo& t, std::string& why) {
    long long pos = 0;
    unsigned char magic[4];
    if (!r.read_at(0, magic, 4)) { why = "FLAC: 读不到文件头"; return false; }
    if (std::memcmp(magic, "ID3", 3) == 0) {
        // 前置 ID3v2：ffprobe 对 FLAC 只认 Vorbis comment（实测带 ID3 前缀的夹具仍报 Vorbis 标签），
        // 所以这里也只是跳过，不解析。
        unsigned char h[10];
        if (!r.read_at(0, h, 10)) { why = "FLAC: ID3 头读不到"; return false; }
        const long long tagLen = 10 + (long long)syncsafe28(h + 6) + ((h[5] & 0x10) ? 10 : 0);
        if (tagLen <= 0 || tagLen + 4 > r.size()) { why = "FLAC: ID3 前缀长度越界"; return false; }
        pos = tagLen;
        if (!r.read_at(pos, magic, 4)) { why = "FLAC: fLaC 标记读不到"; return false; }
    }
    if (std::memcmp(magic, "fLaC", 4) != 0) { why = "FLAC: 不是 fLaC 容器"; return false; }

    long long p = pos + 4;
    bool gotSI = false, gotVC = false;
    long long totalSamples = -1;
    std::string vcWhy;
    for (;;) {
        unsigned char b[4];
        if (!r.read_at(p, b, 4)) { why = "FLAC: 元数据块头越界"; return false; }
        const bool last = (b[0] & 0x80) != 0;
        const int type = b[0] & 0x7f;
        const long long len = ((long long)b[1] << 16) | ((long long)b[2] << 8) | (long long)b[3];
        p += 4;
        if (len < 0 || p + len > r.size()) { why = "FLAC: 元数据块长度越界"; return false; }
        if (type == 0) {                       // STREAMINFO
            if (len < 34) { why = "FLAC: STREAMINFO 过短"; return false; }
            unsigned char si[34];
            if (!r.read_at(p, si, 34)) { why = "FLAC: STREAMINFO 读失败"; return false; }
            t.sampleRate = ((int)si[10] << 12) | ((int)si[11] << 4) | ((int)si[12] >> 4);
            t.channels   = (int)(((si[12] >> 1) & 0x7)) + 1;
            t.bits       = (int)(((((int)si[12] & 0x1) << 4) | ((int)si[13] >> 4))) + 1;
            totalSamples = ((long long)(si[13] & 0x0f) << 32) | (long long)be32(si + 14);
            gotSI = true;
        } else if (type == 4) {                // VORBIS_COMMENT
            if (gotVC) { why = "FLAC: 多个 VORBIS_COMMENT 块"; return false; }
            if (len > (8 << 20)) { why = "FLAC: VORBIS_COMMENT 过大"; return false; }
            if (len > 0) {
                std::vector<unsigned char> buf((size_t)len);
                if (!r.read_at(p, buf.data(), (size_t)len)) { why = "FLAC: VORBIS_COMMENT 读失败"; return false; }
                VorbisCommentOut vo;
                if (!parse_vorbis_comment(buf.data(), buf.size(), t, vo, vcWhy)) { why = "FLAC: " + vcWhy; return false; }
                if (vo.hasPicture) t.hasCover = true;
                if (vo.hasCoverArt) { why = "FLAC: 非标准 COVERART 键（交给 ffprobe）"; return false; }
            }
            gotVC = true;
        } else if (type == 6) {                // PICTURE：只读 MIME 前缀，判 ffprobe 会不会建封面流
            const size_t want = (size_t)std::min<long long>(len, 8 + 64);
            unsigned char ph[8 + 64] = {0};
            if (want > 0 && r.read_at(p, ph, want) && picture_mime_accepted(ph, want, false))
                t.hasCover = true;
        } else if (type == 127) {
            why = "FLAC: 非法元数据块类型 127";
            return false;
        }
        p += len;
        if (last) break;
        if (p + 4 > r.size()) { why = "FLAC: 元数据块链越界"; return false; }
    }
    if (!gotSI) { why = "FLAC: 缺 STREAMINFO"; return false; }
    if (t.sampleRate <= 0 || t.channels <= 0 || t.bits <= 0) { why = "FLAC: STREAMINFO 参数无效"; return false; }
    if (totalSamples <= 0) { why = "FLAC: 总样本数未知（流式编码）"; return false; }
    t.duration = (double)totalSamples / (double)t.sampleRate;
    t.codec = "flac";
    return true;
}

// ---------------------------------------------------------------------------
// ID3v2（MP3 的标签前缀 与 WAV 的 "id3 " 块共用）
//   * decode_id3_text_enc / apic_desc_text：文本编码与 APIC 结构；
//   * TagValues / TagMerger：合并口径（见下面那段实测说明，别改成「后者覆盖前者」）；
//   * parse_id3v2_tag：两种容器共用的帧解析（按字节来源抽象读取，见 ByteSource）。
// 紧跟其后的 parse_mp3 只是把结果落到 TagInfo，并补首帧/Xing 的时长口径。
// ---------------------------------------------------------------------------
inline bool decode_id3_text_enc(unsigned char enc, const unsigned char* p, size_t len, std::string& out,
                                std::string& why) {
    out.clear();
    if (enc == 0) {
        // ISO-8859-1：ffmpeg 原字节透传（实测），这里同样透传；只砍到第一个 NUL。
        size_t k = 0;
        while (k < len && p[k] != '\0') ++k;
        out.assign((const char*)p, k);
        return true;
    }
    if (enc == 3) {                                  // UTF-8
        size_t k = 0;
        while (k < len && p[k] != '\0') ++k;
        out.assign((const char*)p, k);
        return true;
    }
    if (enc == 1 || enc == 2) {                      // UTF-16（带 BOM / 指定 BE）
        bool big = (enc == 2);
        if (enc == 1 && len >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE) { big = false; p += 2; len -= 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { big = true; p += 2; len -= 2; }
        }
        // 去掉尾部 UTF-16 NUL（2 字节）
        while (len >= 2) {
            const uint16_t u = big ? (uint16_t)((p[len - 2] << 8) | p[len - 1]) : (uint16_t)(p[len - 2] | (p[len - 1] << 8));
            if (u != 0) break;
            len -= 2;
        }
        // 也接受奇数长度的坏标签：多余的尾字节丢掉（ffmpeg 同样容错）
        if (len & 1u) --len;
        if (!utf16_to_utf8(p, len, big, &out)) { why = "ID3 UTF-16 解码失败"; return false; }
        // 值内可能还含内嵌 NUL 分隔（ID3v2.4 多值）：与 ffmpeg 一致，截到第一个 NUL。
        const size_t z = out.find('\0');
        if (z != std::string::npos) out.resize(z);
        return true;
    }
    why = "ID3 文本编码不支持";
    return false;
}

inline bool decode_id3_text(const unsigned char* body, size_t n, std::string& out, std::string& why) {
    if (n < 1) { why = "ID3 文本帧为空"; return false; }
    return decode_id3_text_enc(body[0], body + 1, n - 1, out, why);
}

// APIC 帧体：encoding(1) + MIME(NUL) + picture type(1) + description(按 encoding，NUL 结尾) + data
// 为什么要读描述：ffmpeg 把 APIC 的 description 放到**封面流的 title** 上，而 lib_probe_one 的
// 「stream tags 补缺」会把第一个非空的流标题当成曲名（实测：只有 APIC、没有 TIT2 的 MP3，
// ffprobe 的 title = APIC 描述）。要逐字段对齐就只能照做。
inline bool apic_desc_text(const unsigned char* body, size_t n, std::string& out, std::string& why) {
    out.clear();
    if (n < 4) return true;                        // 没描述（结构太短）：不算失败
    const unsigned char enc = body[0];
    size_t i = 1;
    while (i < n && body[i] != 0) ++i;              // MIME
    if (i >= n) return true;
    ++i;                                            // 跳过 MIME 的 NUL
    if (i >= n) return true;
    ++i;                                            // picture type
    const size_t start = i;
    if (enc == 1 || enc == 2) {
        while (i + 1 < n && !(body[i] == 0 && body[i + 1] == 0)) i += 2;
        const size_t stop = std::min(i, n);
        if (stop <= start) return true;
        return decode_id3_text_enc(enc, body + start, stop - start, out, why);
    }
    while (i < n && body[i] != 0) ++i;
    return decode_id3_text_enc(enc, body + start, i - start, out, why);
}

// ---------------------------------------------------------------------------
// 标签值的合并口径（**先到先得**）。这一段是 A/B 与「索引逐字节」两条判据能不能过的关键，
// 不要按直觉改成「后者覆盖前者」。实测（本机 ffprobe 9.0 + 本仓库 net/library.h 的实际结果；
// 夹具 + 71 个真实 WAV）：
//   * 不同的块之间，ffmpeg 是**追加**（ID3 走 AV_DICT_MULTIKEY）⟹ ffprobe 会打出两条同键 tag：
//     LIST→id3 的文件先打 [TAG:title=INFO值] 再打 [TAG:title=ID3值]，id3→LIST 反过来。
//   * 同一个 LIST/INFO 块内重复键：ffmpeg 用覆盖 ⟹ **后者**赢（实测 INAM×2 → 取第二个）。
//   * 同一个 ID3 标签内重复帧：ffmpeg 用 DONT_OVERWRITE ⟹ **前者**赢（实测 TIT2×2 → 取第一个）。
//   * 而 lib_probe_one 按 -of default=nw=0 的段标记解析，比的却是 "[FORMAT]"/"[STREAM]"；
//     ffprobe 的输出是 CRLF（行尾带 '\r'）⟹ **段标记永远匹配不上**，inFormat 恒为 false
//     ⟹ put() 只在目标为空时赋值 ⟹ 最终入库的是**第一个非空值**（流标签先打印，所以
//     APIC 描述会压过 TIT2 —— 实测 x_id3_apic_desc_title.wav: FF=DescFromAPIC）。
// 结论：每个键取「第一个非空值」；「块内重复」先按 ffmpeg 的行为压成一条，再参与跨块合并。
// （library.h 的 CRLF 缺陷不在本文件可改范围，见本文件顶部未解决项 8。）
// ---------------------------------------------------------------------------
struct TagValues {
    std::string title, artist, album, albumArtist, genre, date, track, disc;
    bool hasTitle = false, hasArtist = false, hasAlbum = false, hasAlbumArtist = false;
    bool hasGenre = false, hasDate = false, hasTrack = false, hasDisc = false;
    // 直接覆盖 = ffmpeg 在**同一个 INFO 块内**的行为（后一条 av_dict_set 覆盖前一条）
    static void overwrite(std::string& dst, bool& has, const std::string& v) { dst = v; has = true; }
    // 只在槽还空着时写 = ffmpeg 在**同一个 ID3 标签内**的行为（DONT_OVERWRITE）；空值不占位
    // （lib_probe_one 的 put() 对空值同样不会锁住字段）
    static bool putFirst(std::string& dst, bool& has, const std::string& v) {
        if (has || v.empty()) return false;
        dst = v; has = true; return true;
    }
};

// 跨块合并：把一块里读到的值「先到先得」地吃进去（槽空着且值非空才吃）。
struct TagMerger {
    TagValues acc;                 // 已入库的（每个键的第一个非空值）
    bool genreNumeric = false;     // 已入库的 genre 来自 ID3 的数字流派引用 ⟹ 需要回退
    void absorb(const TagValues& v, bool vGenreNumeric = false) {
        bool tookGenre = false;
        if (!acc.hasTitle && !v.title.empty())             { acc.title = v.title; acc.hasTitle = true; }
        if (!acc.hasArtist && !v.artist.empty())           { acc.artist = v.artist; acc.hasArtist = true; }
        if (!acc.hasAlbum && !v.album.empty())             { acc.album = v.album; acc.hasAlbum = true; }
        if (!acc.hasAlbumArtist && !v.albumArtist.empty()) { acc.albumArtist = v.albumArtist; acc.hasAlbumArtist = true; }
        if (!acc.hasGenre && !v.genre.empty())             { acc.genre = v.genre; acc.hasGenre = true; tookGenre = true; }
        if (!acc.hasDate && !v.date.empty())               { acc.date = v.date; acc.hasDate = true; }
        if (!acc.hasTrack && !v.track.empty())             { acc.track = v.track; acc.hasTrack = true; }
        if (!acc.hasDisc && !v.disc.empty())               { acc.disc = v.disc; acc.hasDisc = true; }
        if (tookGenre) genreNumeric = vGenreNumeric;
    }
};

// 把一个 ID3v2 文本帧写进 TagValues（**同一个标签内先到先得**，见上）。
// genreNumeric：TCON 是数字流派引用（"(17)" / "(17)Rock" / "17"）——ffmpeg 会用 192 条表
// 转成名字，本地没抄那张表。**不在这里直接失败**：这个值可能根本进不了最终标签
// （前面的 INFO 已经占了 genre，或者后面还有别的块），由调用方按「它是否真的入库」决定回退。
inline bool apply_id3_text_frame(const std::string& id, const unsigned char* body, size_t n, TagValues& tv,
                                 bool* genreNumeric, std::string& why) {
    std::string v;
    if (!decode_id3_text(body, n, v, why)) return false;
    v = trim_tag_tail(v);
    if (id == "TIT2")      TagValues::putFirst(tv.title, tv.hasTitle, v);
    else if (id == "TPE1") TagValues::putFirst(tv.artist, tv.hasArtist, v);
    else if (id == "TALB") TagValues::putFirst(tv.album, tv.hasAlbum, v);
    else if (id == "TPE2") TagValues::putFirst(tv.albumArtist, tv.hasAlbumArtist, v);
    else if (id == "TPOS") TagValues::putFirst(tv.disc, tv.hasDisc, v);
    else if (id == "TRCK") TagValues::putFirst(tv.track, tv.hasTrack, v);
    else if (id == "TDRC" || id == "TYER") TagValues::putFirst(tv.date, tv.hasDate, v);
    else if (id == "TCON") {
        const bool numeric = !v.empty() &&
            (v[0] == '(' || (v.find_first_not_of("0123456789") == std::string::npos &&
                             v.find_first_of("0123456789") != std::string::npos));
        if (TagValues::putFirst(tv.genre, tv.hasGenre, v) && genreNumeric) *genreNumeric = numeric;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ID3v2 帧解析（MP3 的 ID3v2 前缀 与 WAV 的 "id3 " 块共用这一份实现）
//
// 为什么抽出来：RIFF 里的 "id3 " 块装的就是**同一个 ID3v2 字节流**，ffmpeg 也是用同一个
// ff_id3v2_parse 处理的（实测：同一套帧 ID / 编码 / APIC 规则在两种容器里表现一致）。
// 复制两份必然漂移（铁律 3），所以只留一份，两种容器各给一个「字节来源」。
//
// 字节来源抽象：MP3 的标签里可能有几百 KB 的 APIC，**不能整块读进内存**（真实库里有 460 KB 的；
// 15000 首全量扫会白白多读几 GB），所以走 FileByteSource 按需读；WAV 的 "id3 " 块本来就要
// 按块长度整块读（和 LIST 一样），走 MemoryByteSource。
// ---------------------------------------------------------------------------
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual bool readAt(long long off, void* dst, size_t n) const = 0;
};
class FileByteSource final : public ByteSource {
public:
    explicit FileByteSource(FileReader& r) : r_(r) {}
    bool readAt(long long off, void* dst, size_t n) const override { return r_.read_at(off, dst, n); }
private:
    FileReader& r_;
};
class MemoryByteSource final : public ByteSource {
public:
    MemoryByteSource(const unsigned char* p, size_t n) : p_(p), n_(n) {}
    bool readAt(long long off, void* dst, size_t n) const override {
        if (off < 0 || n == 0 || (size_t)off + n > n_) return false;
        std::memcpy(dst, p_ + (size_t)off, n);
        return true;
    }
private:
    const unsigned char* p_;
    size_t n_;
};

// ID3v2 解析的附加结果（TagValues 之外、WAV 合并与回退判断要用到的东西）
struct Id3Out {
    bool hasCover = false;          // 有「ffprobe 会真的建封面流」的 APIC（判据同 apic_mime_accepted）
    std::string firstApicDesc;      // 第一个这类 APIC 的描述：ffmpeg 把它放到封面流的 title 上
    bool genreNumeric = false;      // 入库的 genre 来自数字流派引用（"(17)" / "17"）
    long long totalLen = 0;         // ID3v2 标签总字节数（含 10 字节头/footer）；MP3 用它算音频起点
};

// 解析 off 处的一个完整 ID3v2 标签。帧字段写进 tv（**同一个标签内先到先得**，与 ffmpeg 的
// DONT_OVERWRITE 一致，见上面 TagValues 的实测说明）。avail = 从 off 起可用的字节数。
inline bool parse_id3v2_tag(const ByteSource& src, long long off, long long avail, TagValues& tv, Id3Out& out,
                            std::string& why) {
    out = Id3Out();
    unsigned char h[10];
    if (avail < 10 || !src.readAt(off, h, 10)) { why = "ID3v2 头读不到"; return false; }
    if (std::memcmp(h, "ID3", 3) != 0) { why = "ID3v2 幻数不符"; return false; }
    const int ver = h[3];
    if (ver != 3 && ver != 4) { why = "ID3v2 版本不是 2.3/2.4"; return false; }
    if (h[5] & 0x80) { why = "ID3v2 使用了 unsynchronisation"; return false; }
    const long long tagLen = 10 + (long long)syncsafe28(h + 6) + ((h[5] & 0x10) ? 10 : 0);
    if (tagLen > avail) { why = "ID3v2 长度越界"; return false; }
    out.totalLen = tagLen;
    long long fp = 10;                                  // 相对 off
    if (h[5] & 0x40) {                                  // 扩展头
        unsigned char e[4];
        if (!src.readAt(off + fp, e, 4)) { why = "ID3 扩展头读不到"; return false; }
        const long long ext = (ver == 4) ? (long long)syncsafe28(e) : (long long)le32(e);
        if (ext < 0 || fp + ext > tagLen) { why = "ID3 扩展头越界"; return false; }
        fp += ext;
    }
    while (fp + 10 <= tagLen) {
        unsigned char fh[10];
        if (!src.readAt(off + fp, fh, 10)) { why = "ID3 帧头读不到"; return false; }
        if (fh[0] == 0 && fh[1] == 0 && fh[2] == 0 && fh[3] == 0) break;   // padding
        for (int i = 0; i < 4; ++i) {
            const unsigned char c = fh[i];
            if (!(std::isalnum(c) || c == '_' || c == '-')) { why = "ID3 帧 ID 非法"; return false; }
        }
        const long long fsz = (ver == 4) ? (long long)syncsafe28(fh + 4) : (long long)be32(fh + 4);
        if (fh[8] || fh[9]) { why = "ID3 帧带标志（压缩/加密/分组）"; return false; }
        if (fsz < 0 || fp + 10 + fsz > tagLen) { why = "ID3 帧长度越界"; return false; }
        const std::string id((const char*)fh, 4);
        if (id == "APIC" || id == "PIC") {
            // 只读前 512 字节（MIME/描述就在最前面）：ffprobe 只在 MIME 是已知图片类型时才建封面流，
            // 空/未知 MIME 的 APIC 它直接忽略（实测）⟹ 我们也只按 MIME 判，不按帧存在判。
            // 同时取「第一个非空描述」：ffmpeg 把它放到封面流的 title 上，会成为补缺曲名。
            const size_t want = (size_t)std::min<long long>(fsz, 512);
            if (want > 0) {
                std::vector<unsigned char> ab(want);
                if (!src.readAt(off + fp + 10, ab.data(), ab.size())) { why = "ID3 APIC 读失败"; return false; }
                if (apic_mime_accepted(ab.data(), ab.size())) {
                    out.hasCover = true;
                    // 只有「真的建了封面流」的 APIC，它的描述才会成为流标题 ⟹ 只有这时才收集描述。
                    if (out.firstApicDesc.empty()) {
                        std::string d, dwhy;
                        if (apic_desc_text(ab.data(), ab.size(), d, dwhy)) out.firstApicDesc = trim_tag_tail(d);
                    }
                }
            }
        } else if (id == "TIT2" || id == "TPE1" || id == "TALB" || id == "TPE2" || id == "TPOS" ||
                   id == "TRCK" || id == "TDRC" || id == "TYER" || id == "TCON") {
            if (fsz > 0) {
                std::vector<unsigned char> body((size_t)fsz);
                if (!src.readAt(off + fp + 10, body.data(), body.size())) { why = "ID3 帧内容读失败"; return false; }
                if (!apply_id3_text_frame(id, body.data(), body.size(), tv, &out.genreNumeric, why))
                    return false;
            }
        }
        fp += 10 + fsz;
    }
    return true;
}

inline bool parse_mp3(FileReader& r, TagInfo& t, std::string& why) {
    const long long fsize = r.size();
    long long audio = 0;
    unsigned char h[10];
    if (fsize >= 10 && r.read_at(0, h, 10) && std::memcmp(h, "ID3", 3) == 0) {
        // 帧解析与 WAV 的 "id3 " 块共用（parse_id3v2_tag）；这里只管把结果落进 TagInfo，
        // 外加两条 MP3 侧的口径：数字流派引用要回退、封面流标题补缺。
        FileByteSource src(r);
        TagValues tv;
        Id3Out io;
        std::string iw;
        if (!parse_id3v2_tag(src, 0, fsize, tv, io, iw)) { why = "MP3: " + iw; return false; }
        if (io.genreNumeric) { why = "MP3: TCON 是数字流派引用（交给 ffprobe）"; return false; }
        if (io.hasCover) t.hasCover = true;   // APIC 的 MIME 是 ffprobe 认识的图片类型 ⟹ 它会建封面流
        // 与 lib_probe_one 的实际结果一致：封面流的 title（APIC 描述）先打印、先入库，
        // 所以它压过 TIT2（实测 MP3 夹具：TIT2=TitleFromTIT2 + APIC 描述=DescFromAPIC ⟹ FF 取后者）。
        t.title       = io.firstApicDesc.empty() ? tv.title : io.firstApicDesc;
        t.artist      = tv.artist;
        t.album       = tv.album;
        t.albumArtist = tv.albumArtist;
        t.genre       = tv.genre;
        t.date        = tv.date;
        t.trackNo     = std::atoi(tv.track.c_str());
        t.discNo      = std::atoi(tv.disc.c_str());
        audio = io.totalLen;
    }

    // ---- 首帧 ----
    const long long winLen = std::min<long long>(fsize - audio, 1 << 20);
    if (winLen < 4) { why = "MP3: 音频数据过短"; return false; }
    std::vector<unsigned char> win((size_t)winLen);
    if (r.read_up_to(audio, win.data(), win.size()) != win.size()) { why = "MP3: 音频窗口读失败"; return false; }
    long long off = -1;
    for (long long i = 0; i + 4 <= winLen; ++i) {
        const unsigned char b1 = win[(size_t)i + 1];
        if (win[(size_t)i] != 0xFF || (b1 & 0xE0) != 0xE0) continue;
        const unsigned char b2 = win[(size_t)i + 2];
        const int verBits = (b1 >> 3) & 0x3;        // 3=MPEG1 2=MPEG2 0=MPEG2.5 1=保留
        const int layer   = (b1 >> 1) & 0x3;        // 1=Layer III
        const int brIdx   = (b2 >> 4) & 0xF;
        const int srIdx   = (b2 >> 2) & 0x3;
        if (verBits == 1 || layer != 1) continue;
        if (brIdx == 0 || brIdx == 0xF || srIdx == 3) continue;
        off = i;
        break;
    }
    if (off < 0) { why = "MP3: 找不到合法的 MPEG 帧"; return false; }
    const unsigned char* f = win.data() + off;
    const unsigned char b1 = f[1], b2 = f[2], b3 = f[3];
    const int verBits = (b1 >> 3) & 0x3;
    const int brIdx = (b2 >> 4) & 0xF;
    const int srIdx = (b2 >> 2) & 0x3;
    const int pad = (b2 >> 1) & 0x1;
    const int mode = (b3 >> 6) & 0x3;           // 3 = 单声道
    static const int kSr[3] = { 44100, 48000, 32000 };
    static const int kBrMpeg1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    int sr = kSr[srIdx];
    int brKbps = kBrMpeg1[brIdx];
    if (verBits != 3) {                          // MPEG2 / MPEG2.5：表不同
        static const int kBrMpeg2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
        static const int kSr2[3] = { 22050, 24000, 16000 };
        static const int kSr25[3] = { 11025, 12000, 8000 };
        brKbps = kBrMpeg2[brIdx];
        sr = (verBits == 2) ? kSr2[srIdx] : kSr25[srIdx];
    }
    if (sr <= 0 || brKbps <= 0) { why = "MP3: 首帧参数无效"; return false; }
    const int spf = (verBits == 3) ? 1152 : 576;
    const int channels = (mode == 3) ? 1 : 2;
    const int sideInfo = (verBits == 3) ? ((mode == 3) ? 17 : 32) : ((mode == 3) ? 9 : 17);
    const long long frameLen = (verBits == 3 ? 144LL : 72LL) * brKbps * 1000LL / sr + pad;
    const long long frameEnd = off + frameLen;
    t.sampleRate = sr;
    t.channels = channels;
    t.bits = 0;                                   // ffprobe 对 MP3 报 N/A ⟹ 由调用方退化成 16（与旧路径一致）
    t.codec = "mp3";

    // ---- Xing/Info ----
    const long long xoff = off + 4 + sideInfo;
    bool gotFrames = false;
    long long frames = 0;
    if (xoff + 8 <= (long long)win.size() && frameEnd <= (long long)win.size() &&
        (std::memcmp(win.data() + xoff, "Xing", 4) == 0 || std::memcmp(win.data() + xoff, "Info", 4) == 0)) {
        const long long xflags = (long long)be32(win.data() + xoff + 4);
        if (xflags & 0x1) {
            frames = (long long)be32(win.data() + xoff + 8);
            gotFrames = frames > 0;
        }
        if (gotFrames) {
            // LAME 标签固定在 Xing 头之后 120 字节处（'Xing'/'Info' + 全部可选字段 = 120 字节）；
            // 里面第 21..23 字节是「编码器延迟(12bit) + 填充(12bit)」，ffmpeg 会从时长里减掉它们。
            // 实测：不减会偏 1~2 帧（26~51 ms）；减掉后与 ffprobe **逐样本相等**。
            // 用编码器字符串前 4 字节识别 LAME 系标签（真实库全是 "LAME"；ffmpeg 新写的为 "Lavc"/"Lavf"）。
            long long delay = 0, padding = 0;
            const long long ltag = xoff + 120;
            if (ltag + 24 <= (long long)win.size() &&
                (std::memcmp(win.data() + ltag, "LAME", 4) == 0 || std::memcmp(win.data() + ltag, "Lavc", 4) == 0 ||
                 std::memcmp(win.data() + ltag, "Lavf", 4) == 0 || std::memcmp(win.data() + ltag, "GOGO", 4) == 0)) {
                const unsigned char* d = win.data() + ltag + 21;
                delay   = ((long long)d[0] << 4) | (d[1] >> 4);
                padding = (((long long)d[1] & 0x0F) << 8) | d[2];
                if (delay + padding < 0 || delay + padding >= frames * spf) { delay = padding = 0; }
            }
            const long long samples = frames * spf - delay - padding;
            if (samples > 0) {
                t.duration = (double)samples / (double)sr;
                return true;
            }
        }
    }
    // ---- 无 Xing / Xing 无帧数：CBR 估算（与 ffmpeg 同式：(文件字节 - ID3v2 长度) * 8 / 码率）----
    t.duration = (double)(fsize - audio) * 8.0 / ((double)brKbps * 1000.0);
    if (!(t.duration > 0.0)) { why = "MP3: 时长算不出来"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// WAV（RIFF/WAVE，PCM 或 IEEE float）
// ---------------------------------------------------------------------------
// 一个 LIST/INFO 块里的条目 → TagValues。**块内是后者覆盖前者**（ffmpeg 的 ff_read_riff_info
// 对该块逐条 av_dict_set 覆盖；实测 INAM×2 时 ffprobe 只打第二条，lib_probe_one 也只能拿到它）。
inline void apply_wav_info_tag(TagValues& tv, const std::string& id, const char* v, size_t n) {
    // 值以 NUL 结尾（长度字段常含 NUL 与填充），先取到第一个 NUL（与 C 字符串语义一致）
    size_t len = 0;
    while (len < n && v[len] != '\0') ++len;
    const std::string s = trim_tag_tail(std::string(v, len));
    if      (id == "INAM") TagValues::overwrite(tv.title, tv.hasTitle, s);
    else if (id == "IART") TagValues::overwrite(tv.artist, tv.hasArtist, s);
    else if (id == "IPRD") TagValues::overwrite(tv.album, tv.hasAlbum, s);
    else if (id == "IGNR") TagValues::overwrite(tv.genre, tv.hasGenre, s);
    else if (id == "ICRD") TagValues::overwrite(tv.date, tv.hasDate, s);
    else if (id == "ITRK" || id == "IPRT") TagValues::overwrite(tv.track, tv.hasTrack, s);
}

inline bool parse_wav(FileReader& r, TagInfo& t, std::string& why) {
    unsigned char h[12];
    if (!r.read_at(0, h, 12)) { why = "WAV: 文件头过短"; return false; }
    if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) {
        why = "WAV: 不是 RIFF/WAVE（RF64/RIFX 不支持）";
        return false;
    }
    const long long fsize = r.size();
    long long p = 12;
    long long dataOff = -1, dataLen = 0;
    bool gotFmt = false;
    int  fmtTag = 0, bits = 0, blockAlign = 0;
    // ---- 标签合并（LIST/INFO、id3 块、bext）----
    // 口径见上面 TagValues/TagMerger 的实测说明：ffprobe 会把不同块的同键标签**都**打出来，
    // 而 lib_probe_one 实际只取**第一个非空值**（段标记因 CRLF 匹配不上）。所以：
    //   块与块之间 = 先到先得（LIST→id3 时 INFO 赢、id3→LIST 时 ID3 赢，与「后者覆盖」正好相反）；
    //   同一个 INFO 块内 = 后者覆盖前者（ffmpeg 自己就覆盖了）；
    //   同一个 ID3 标签内 = 先到先得（ffmpeg 用 DONT_OVERWRITE）。
    TagMerger mg;                     // 跨块合并（先到先得）
    std::string firstApicDesc;        // 封面流 title（APIC 描述）：流标签先打印，所以它优先于 format
    while (p + 8 <= fsize) {
        unsigned char ch[8];
        if (!r.read_at(p, ch, 8)) { why = "WAV: 块头越界"; return false; }
        const long long len = (long long)le32(ch + 4);
        const long long body = p + 8;
        // data 块声明长度可能大于实际文件（截断/流式录制）：ffmpeg 按实际可读长度截断，
        // 所以这里也只对「非 data 块」要求严格越界检查。
        const bool isData = std::memcmp(ch, "data", 4) == 0;
        if (len < 0 || (!isData && body + len > fsize)) { why = "WAV: 块长度越界"; return false; }
        if (body > fsize) { why = "WAV: 块头越界"; return false; }
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            if (len < 16) { why = "WAV: fmt 块过短"; return false; }
            unsigned char f[40] = {0};
            const size_t want = (size_t)std::min<long long>(len, 40);
            if (!r.read_at(body, f, want)) { why = "WAV: fmt 读失败"; return false; }
            fmtTag      = (int)le16(f);
            t.channels  = (int)le16(f + 2);
            t.sampleRate= (int)le32(f + 4);
            blockAlign  = (int)le16(f + 12);
            bits        = (int)le16(f + 14);
            if (fmtTag == 0xFFFE) {                       // WAVE_FORMAT_EXTENSIBLE
                if (len < 40) { why = "WAV: extensible fmt 过短"; return false; }
                const int validBits = (int)le16(f + 18);
                const uint16_t sub = le16(f + 24);
                if (validBits != bits) { why = "WAV: 有效位深与容器位深不同（交给 ffprobe）"; return false; }
                if (sub != 1 && sub != 3) { why = "WAV: extensible 子格式非 PCM/float"; return false; }
                fmtTag = (int)sub;
            }
            if (fmtTag != 1 && fmtTag != 3) { why = "WAV: 非 PCM/float 编码"; return false; }
            gotFmt = true;
        } else if (isData) {
            if (dataOff < 0) { dataOff = body; dataLen = std::min(len, fsize - body); }
        } else if (std::memcmp(ch, "LIST", 4) == 0) {
            if (len >= 4 && len <= (1 << 20)) {
                std::vector<unsigned char> buf((size_t)len);
                if (!r.read_at(body, buf.data(), buf.size())) { why = "WAV: LIST 读失败"; return false; }
                if (std::memcmp(buf.data(), "INFO", 4) == 0) {
                    TagValues iv;                       // 块内：后者覆盖前者（ffmpeg 的行为）
                    size_t q = 4;
                    while (q + 8 <= buf.size()) {
                        const char* sid = (const char*)buf.data() + q;
                        const long long sl = (long long)le32(buf.data() + q + 4);
                        q += 8;
                        if (sl < 0 || q + (size_t)sl > buf.size()) break;
                        apply_wav_info_tag(iv, std::string(sid, 4), (const char*)buf.data() + q, (size_t)sl);
                        q += (size_t)sl + ((size_t)sl & 1u);
                    }
                    mg.absorb(iv);                      // 跨块：先到先得
                }
            }
        } else if (std::memcmp(ch, "id3 ", 4) == 0 || std::memcmp(ch, "ID3 ", 4) == 0) {
            // RIFF 里的 ID3v2（"ID3v2 in RIFF"）：ffmpeg 的 wav demuxer 就地解析这个块，标签并进
            // format tags。跨块口径是**先到先得**（见 TagValues/TagMerger 的实测说明）：
            // 实测本库 51 个真实文件 —— LIST→id3 的 30 个最终取 INFO 的值，id3→LIST 的 19 个取 ID3 的值。
            // 块里的 APIC 会让 ffmpeg 建一条 video 流（封面流），其描述成为该流的 title
            // ⟹ lib_probe_one 的 stream-tags 补缺会把它当曲名（与 MP3 同一条坑），这里同样收集。
            // 帧级解析与 MP3 共用 parse_id3v2_tag（同一种字节流，不复制第二份）。
            if (len > (32 << 20)) { why = "WAV: 'id3 ' 块过大"; return false; }
            std::vector<unsigned char> id3buf((size_t)len);
            if (len > 0 && !r.read_at(body, id3buf.data(), id3buf.size())) {
                why = "WAV: 'id3 ' 块读失败";
                return false;
            }
            if (id3buf.size() < 3 || std::memcmp(id3buf.data(), "ID3", 3) != 0) {
                // ffmpeg 的 ff_id3v2_read 找不到幻数时只打一行 debug 日志、**不加标签也不报错**
                // （实测夹具：块内是垃圾字节 ⟹ ffprobe 只报 INFO 的标签）。这里同样跳过，
                // 不要把它当解析失败——否则这类文件会白白多起一个 ffprobe 进程。
            } else if (id3buf.size() < 10) {
                why = "WAV: 'id3 ' 块过短";
                return false;
            } else {
                MemoryByteSource ms(id3buf.data(), id3buf.size());
                TagValues dv;
                Id3Out io;
                std::string iw;
                if (!parse_id3v2_tag(ms, 0, (long long)id3buf.size(), dv, io, iw)) {
                    why = "WAV: " + iw;
                    return false;
                }
                if (io.hasCover) t.hasCover = true;
                if (!io.firstApicDesc.empty() && firstApicDesc.empty()) firstApicDesc = io.firstApicDesc;
                mg.absorb(dv, io.genreNumeric);
            }
        } else if (std::memcmp(ch, "bext", 4) == 0) {
            // Broadcast Wave 的 bext：ffmpeg 把 origination_date（偏移 320，10 字节）写成 **date**，
            // originator → encoded_by、origination_time → creation_time（后两者 lib_probe_one 不取）。
            // 实测（本机 ffprobe 9.0，逐字节造块扫 330..346）：
            //   * 块长 < 337 ⟹ ffmpeg **整份文件报 invalid data**（连流都不给）⟹ 这里也必须回退，
            //     否则我们会比 ffprobe "多解析"出一个文件（真出现这种文件就是 A/B 红）；
            //   * 块长 >= 337 ⟹ date 照常读出；origination_date 为空 ⟹ ffmpeg 不写 date 键
            //     （所以不能把前面 LIST 的 ICRD 清成空）。
            if (len < 337) { why = "WAV: bext 块过短（ffprobe 会拒绝整个文件）"; return false; }
            unsigned char bx[330];
            if (!r.read_at(body, bx, sizeof(bx))) { why = "WAV: bext 读失败"; return false; }
            size_t dn = 0;
            while (dn < 10 && bx[320 + dn] != '\0') ++dn;
            TagValues bv;
            const std::string d = trim_tag_tail(std::string((const char*)bx + 320, dn));
            if (!d.empty()) TagValues::overwrite(bv.date, bv.hasDate, d);
            mg.absorb(bv);
        } else if (std::memcmp(ch, "iXML", 4) == 0 || std::memcmp(ch, "axml", 4) == 0) {
            // iXML/axml 是 XML 元数据，ffmpeg 会解析出若干键（本库 71 个 WAV 里 0 个）。
            // 没实测过它们的字段映射 ⟹ 照旧交给 ffprobe（保底不丢标签，铁律 3）。
            why = "WAV: 含 iXML/axml 元数据块（交给 ffprobe）";
            return false;
        }
        if (isData && body + len > fsize) break;      // 声明的 data 超出文件：已截断，后面没有块了
        p = body + len + (len & 1);
    }
    // 数字流派引用（"(17)"）要靠 ffmpeg 的 192 条表才能变成名字；只有它**真的入库**才需要回退。
    // （如果前面的 INFO 已经占了 genre，它的值根本不会出现在最终标签里 ⟹ 不必回退，实测验证过。）
    if (mg.genreNumeric) { why = "WAV: 'id3 ' 块的 TCON 是数字流派引用（交给 ffprobe）"; return false; }
    // 落位：封面流的 title（APIC 描述）先打印、先入库，所以它有最高优先级；没有才用 format 的 title。
    t.title       = firstApicDesc.empty() ? mg.acc.title : firstApicDesc;
    t.artist      = mg.acc.artist;
    t.album       = mg.acc.album;
    t.albumArtist = mg.acc.albumArtist;
    t.genre       = mg.acc.genre;
    t.date        = mg.acc.date;
    t.trackNo     = std::atoi(mg.acc.track.c_str());
    t.discNo      = std::atoi(mg.acc.disc.c_str());
    if (!gotFmt) { why = "WAV: 缺 fmt 块"; return false; }
    if (dataOff < 0) { why = "WAV: 缺 data 块"; return false; }
    if (t.sampleRate <= 0 || t.channels <= 0 || blockAlign <= 0 || bits <= 0) {
        why = "WAV: fmt 参数无效";
        return false;
    }
    // data 声明长度可能大于实际文件（截断/流式录制）：ffmpeg 按实际可读长度截断，这里同样处理。
    long long remain = fsize - dataOff;
    if (remain <= 0) { why = "WAV: data 块为空"; return false; }
    const long long used = std::min(dataLen > 0 ? dataLen : remain, remain);
    t.duration = (double)used / ((double)blockAlign * (double)t.sampleRate);
    if (!(t.duration > 0.0)) { why = "WAV: 时长算不出来"; return false; }
    // 位深口径必须与 ffprobe 的 bits_per_raw_sample 一致（实测本机 ffprobe 9.0）：
    //   pcm_s16le → N/A、pcm_u8 → N/A、pcm_f32le/f64le → N/A  ⟹ 旧代码把它们统一写成 16；
    //   pcm_s24le → 24、pcm_s32le → 32。
    // 所以这里只在 24/32 位整数 PCM 时报真值，其余情形报 0，由 lib_probe_one 的 upsell 逻辑写成 16。
    t.bits = (fmtTag == 1 && (bits == 24 || bits == 32)) ? bits : 0;
    if (fmtTag == 3)      t.codec = (bits == 64) ? "pcm_f64le" : "pcm_f32le";
    else if (bits == 8)   t.codec = "pcm_u8";
    else if (bits == 16)  t.codec = "pcm_s16le";
    else if (bits == 24)  t.codec = "pcm_s24le";
    else if (bits == 32)  t.codec = "pcm_s32le";
    else                  t.codec = "pcm";
    return true;
}

// ---------------------------------------------------------------------------
// OGG（Vorbis）
// ---------------------------------------------------------------------------
// 读第 i 个页头；成功时给出 body 偏移、段表、是否结束页。
struct OggPage {
    long long bodyOff = 0;
    long long bodyLen = 0;
    std::vector<int> segs;
    bool bos = false, eos = false;
    long long serial = 0;
    long long granule = 0;
};

inline bool ogg_read_page(FileReader& r, long long off, OggPage& pg, long long* nextOff) {
    unsigned char h[27];
    if (!r.read_at(off, h, 27)) return false;
    if (std::memcmp(h, "OggS", 4) != 0) return false;
    if (h[4] != 0) return false;                        // 流版本必须是 0
    pg.bos = (h[5] & 0x02) != 0;
    pg.eos = (h[5] & 0x04) != 0;
    pg.granule = (long long)le64s(h + 6);
    pg.serial = (long long)((uint32_t)le32(h + 14));
    const int nseg = h[26];
    unsigned char seg[255];
    if (nseg > 0 && !r.read_at(off + 27, seg, (size_t)nseg)) return false;
    pg.segs.assign(seg, seg + nseg);
    long long body = 0;
    for (int s : pg.segs) body += s;
    pg.bodyOff = off + 27 + nseg;
    pg.bodyLen = body;
    if (pg.bodyOff + body > r.size()) return false;
    if (nextOff) *nextOff = pg.bodyOff + body;
    return true;
}

inline bool parse_ogg(FileReader& r, TagInfo& t, std::string& why) {
    OggPage p0;
    long long next = 0;
    if (!ogg_read_page(r, 0, p0, &next)) { why = "OGG: 第一页不合法"; return false; }
    if (!p0.bos) { why = "OGG: 第一页不是 BOS"; return false; }
    if (p0.bodyLen < 30) { why = "OGG: 第一页过短"; return false; }
    unsigned char id[30];
    if (!r.read_at(p0.bodyOff, id, 30)) { why = "OGG: 识别头读不到"; return false; }
    if (!(id[0] == 0x01 && std::memcmp(id + 1, "vorbis", 6) == 0)) {
        why = "OGG: 不是 Vorbis（Opus/Speex/Theora 交给 ffprobe）";
        return false;
    }
    const int channels = id[11];
    const uint32_t rate = le32(id + 12);
    if (channels <= 0 || rate == 0) { why = "OGG: Vorbis 识别头参数无效"; return false; }
    t.channels = channels;
    t.sampleRate = (int)rate;
    t.bits = 0;                                    // ffprobe 对 Vorbis 报 N/A ⟹ 调用方退化成 16
    t.codec = "vorbis";

    // ---- 找注释包（第 2 个包；大封面会让它跨很多页）----
    std::vector<unsigned char> acc;
    bool packetDone = false;
    long long off = next;
    int pages = 0;
    while (!packetDone && pages < 4096 && off + 27 <= r.size()) {
        OggPage pg;
        long long nxt = 0;
        if (!ogg_read_page(r, off, pg, &nxt)) { why = "OGG: 页链断裂（注释包）"; return false; }
        ++pages;
        if (pg.serial != p0.serial) { why = "OGG: 出现链式逻辑流"; return false; }
        std::vector<unsigned char> data((size_t)pg.bodyLen);
        if (pg.bodyLen > 0 && !r.read_at(pg.bodyOff, data.data(), data.size())) { why = "OGG: 页数据读失败"; return false; }
        size_t pos = 0;
        for (size_t i = 0; i < pg.segs.size(); ++i) {
            const int s = pg.segs[i];
            if (pos + (size_t)s > data.size()) { why = "OGG: 段表越界"; return false; }
            acc.insert(acc.end(), data.begin() + pos, data.begin() + pos + (size_t)s);
            pos += (size_t)s;
            if (s < 255) { packetDone = true; break; }     // 包结束
        }
        if (acc.size() > (8u << 20)) { why = "OGG: 注释包过大"; return false; }
        off = nxt;
        if (packetDone) break;
    }
    if (!packetDone) { why = "OGG: 找不到完整的注释包"; return false; }
    if (acc.size() < 8 || !(acc[0] == 0x03 && std::memcmp(acc.data() + 1, "vorbis", 6) == 0)) {
        why = "OGG: 第二个包不是 Vorbis 注释头";
        return false;
    }
    VorbisCommentOut vo;
    std::string vcWhy;
    if (!parse_vorbis_comment(acc.data() + 7, acc.size() - 7, t, vo, vcWhy)) { why = "OGG: " + vcWhy; return false; }
    if (vo.hasPicture) t.hasCover = true;
    if (vo.hasCoverArt) { why = "OGG: 非标准 COVERART 键（交给 ffprobe）"; return false; }

    // ---- 末页 granulepos 算时长 ----
    // 强校验：候选页必须**恰好结束在文件末尾**（结构对得上才算数；音频里偶然出现的 "OggS" 会被排除）。
    const long long tailLen = std::min<long long>(r.size(), 1 << 16);
    std::vector<unsigned char> tail((size_t)tailLen);
    if (r.read_up_to(r.size() - tailLen, tail.data(), tail.size()) != tail.size()) {
        why = "OGG: 末页读取失败";
        return false;
    }
    bool gotLast = false;
    OggPage last;
    for (long long i = (long long)tail.size() - 27; i >= 0; --i) {
        if (!(tail[(size_t)i] == 'O' && tail[(size_t)i + 1] == 'g' && tail[(size_t)i + 2] == 'g' &&
              tail[(size_t)i + 3] == 'S'))
            continue;
        if (tail[(size_t)i + 4] != 0) continue;
        const int nseg = tail[(size_t)i + 26];
        if ((long long)i + 27 + nseg > (long long)tail.size()) continue;
        long long body = 0;
        for (int s = 0; s < nseg; ++s) body += tail[(size_t)i + 27 + s];
        if ((long long)i + 27 + nseg + body != (long long)tail.size()) continue;   // 必须正好到 EOF
        last.bos = (tail[(size_t)i + 5] & 0x02) != 0;
        last.eos = (tail[(size_t)i + 5] & 0x04) != 0;
        last.granule = (long long)le64s(tail.data() + i + 6);
        last.serial = (long long)((uint32_t)le32(tail.data() + i + 14));
        gotLast = true;
        break;
    }
    if (!gotLast) { why = "OGG: 找不到合法的末页"; return false; }
    if (last.serial != p0.serial) { why = "OGG: 末页不属于同一逻辑流"; return false; }
    if (last.granule <= 0) { why = "OGG: 末页 granulepos 无效"; return false; }
    t.duration = (double)last.granule / (double)t.sampleRate;
    if (!(t.duration > 0.0)) { why = "OGG: 时长算不出来"; return false; }
    return true;
}

inline std::string lower_ext(const std::string& path) {
    const size_t d = path.find_last_of("./\\");
    if (d == std::string::npos || path[d] != '.') return "";
    std::string e = path.substr(d);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

} // namespace detail

// ---------------------------------------------------------------------------
// 对外入口：解析一个文件。
//   返回 true  ⟹ out 里的字段**就是** ffprobe 会给的那一份（可直接入库）。
//   返回 false ⟹ 调用方必须回退 ffprobe（why 给出人话原因，仅供测试/日志）。
// year 在这里已经按 ffprobe 路径的老规矩算好（atoi(date)），bits 未做 16 兜底（由调用方与旧路径同样处理）。
// ---------------------------------------------------------------------------
inline bool probe_file(const std::string& path, TagInfo* out, std::string* why) {
    if (!out) return false;
    *out = TagInfo();
    if (why) why->clear();
    stats().attempted.fetch_add(1, std::memory_order_relaxed);
    const std::string ext = detail::lower_ext(path);
    if (ext != ".flac" && ext != ".wav" && ext != ".wave" && ext != ".mp3" && ext != ".ogg") {
        if (why) *why = "扩展名不在原生通道覆盖范围";
        stats().fellBack.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    detail::FileReader r;
    if (!r.open(path)) {
        if (why) *why = "文件打不开/为空";
        stats().fellBack.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    TagInfo t;
    std::string my;
    bool ok = false;
    if (ext == ".flac") ok = detail::parse_flac(r, t, my);
    else if (ext == ".wav" || ext == ".wave") ok = detail::parse_wav(r, t, my);
    else if (ext == ".mp3") ok = detail::parse_mp3(r, t, my);
    else ok = detail::parse_ogg(r, t, my);
    if (!ok) {
        if (why) *why = my.empty() ? std::string("解析失败") : my;
        stats().fellBack.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    t.year = std::atoi(t.date.c_str());
    *out = std::move(t);
    stats().parsed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace rta::meta::native
