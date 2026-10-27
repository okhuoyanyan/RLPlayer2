// container_duration.h — 容器声明的时长（BUG-3「文件可能不完整」判据的分子）
//
// 用途：/api/health 的 status 只有 ready/error 两态时，**半截文件**（正在拷贝/下载/边写边分析，
//       或下载中断的残件）会被当成完成品：duration_sec 只反映「已有字节」的长度，
//       指标看起来完全正常。本头文件负责回答一个独立的问题：
//       **这个文件的容器头声称它有多长？**
//       把它与实际解码出的时长（TrackHealth::durationSec）比对，差异过大 ⟹ incomplete。
//
// 为什么必须由容器头来回答，而不是「按文件大小估」：
//   * FLAC/WAV/MP3(Xing)/DSF 的头里都写着**总样本数/总字节数**，这与文件当前有多少字节无关；
//   * 它们都是**前面写、后面追加**的格式：截掉尾部 ⟹ 头仍然有效、还能解码出前一半，
//     于是"能解码 + 时长正常 + 无警告"三者同时成立 —— 正是要被识破的那个假象。
//
// 纪律（宁可漏报、不许误报）：
//   1. 只认**权威且精确**的字段：FLAC STREAMINFO.total_samples、WAV fmt/data 的 byteRate+dataSize、
//      MP3 的 Xing/Info/VBRI **帧计数**、DSF fmt 的 sampleCount。
//   2. 估出来的时长一律**不用**：MP3 无 Xing 时的 CBR 估算（文件字节×8/码率）对 VBR、对带
//      padding 的 CBR 都会偏，用它判"不完整"就是制造误报；ALAC/M4A/OGG/AIFF/DFF 等没有统一
//      精确字段的容器同样不判（未覆盖，返回 unknown）。
//   3. 任何解析失败/字段为 0/越界/异常 → 返回 unknown（调用方据此**不做任何判定**）。
//      这一条比"多抓几个不完整文件"重要：误报会让每一个正常文件都顶一条黄字。
//
// 只读文件头（最多读几十 KB），不打开音频设备，不依赖 ffmpeg。
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace rta::analysis {

// 容器声明时长。seconds<=0 表示"未知/不判"（调用方必须当成没有结论，而不是 0 秒）。
struct DeclaredDuration {
    double      seconds = 0.0;
    std::string how;        // 诊断用："FLAC STREAMINFO 32.046 s"；进 health.note 供人核对
};

namespace cdetail {

// UTF-8 → fs::path。
// 这里**不能**用 std::filesystem::path(std::u8string)：MSVC STL 的 char8_t 路径会被转进当前
// ANSI 代码页，遇到「曲名（Live）.flac」「🎵测试.flac」这类名字直接抛 std::system_error
// ("No mapping for the Unicode character exists…")（本机实测：进程以 0xC0000409 退出）。
// 项目里其它地方一律走平台层 widen()；本头文件不依赖它，自己走 MultiByteToWideChar(CP_UTF8)，
// 与 win32 的宽字符路径完全等价。
inline std::filesystem::path to_fs_path(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty() || utf8.size() > 0x7FFFFFFFu) return std::filesystem::path();
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                        (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::filesystem::path();
    std::wstring w((size_t)n, wchar_t(0));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(),
                              w.data(), n) != n) return std::filesystem::path();
    return std::filesystem::path(w);
#else
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
#endif
}

// 只读随机访问（UTF-8 路径）。任何失败都返回 false，绝不把异常抛给调用方。
class RFile {
public:
    bool open(const std::string& utf8Path) {
        try {
            p_ = to_fs_path(utf8Path);
            if (p_.empty()) return false;
            std::error_code ec;
            size_ = (long long)std::filesystem::file_size(p_, ec);
            if (ec || size_ <= 0) return false;
            f_.open(p_, std::ios::binary);
            return (bool)f_;
        } catch (...) {
            return false;                 // 路径打不开/编码不支持 ⟹ 当成"未知"，不做判定
        }
    }
    long long size() const { return size_; }
    // 越界/短读返回 false（不抛异常）
    bool read_at(long long off, void* dst, size_t n) {
        if (off < 0 || n == 0 || off + (long long)n > size_) return false;
        f_.clear();
        f_.seekg((std::streamoff)off, std::ios::beg);
        f_.read((char*)dst, (std::streamsize)n);
        return (size_t)f_.gcount() == n;
    }
    size_t read_up_to(long long off, void* dst, size_t n) {
        if (off < 0 || off >= size_) return 0;
        const size_t want = (size_t)std::min<long long>((long long)n, size_ - off);
        if (want == 0) return 0;
        f_.clear();
        f_.seekg((std::streamoff)off, std::ios::beg);
        f_.read((char*)dst, (std::streamsize)want);
        return (size_t)f_.gcount();
    }
private:
    std::filesystem::path p_;
    std::ifstream f_;
    long long size_ = 0;
};

inline uint32_t rd_be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
inline uint32_t rd_le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t rd_le64(const unsigned char* p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}
inline uint32_t rd_syncsafe32(const unsigned char* p) {   // ID3v2 的 7-bit 大端
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  | (uint32_t)(p[3] & 0x7F);
}

// ID3v2 跳过：很多 FLAC/MP3 开头挂着 ID3v2（FLAC 规范不允许，但现实里存在；FFmpeg 也照跳）。
inline long long id3v2_len(RFile& f) {
    unsigned char h[10] = {0};
    if (!f.read_at(0, h, 10)) return 0;
    if (!(h[0] == 'I' && h[1] == 'D' && h[2] == '3')) return 0;
    const uint32_t sz = rd_syncsafe32(h + 6);
    const bool footer = (h[5] & 0x10) != 0;
    return (long long)10 + (long long)sz + (footer ? 10 : 0);
}

// ---------------- FLAC：STREAMINFO.total_samples / sample_rate ----------------
// 语义（RFC 9639 §8.2）：total_samples 是**整段音频**的样本数（每声道），由编码器写在文件头，
// 与文件当前有多少字节无关 ⟹ 正是截断检测要用的字段。total_samples=0 表示"未知"（流式编码）。
inline bool probe_flac(RFile& f, long long off, DeclaredDuration* out) {
    unsigned char b[4] = {0};
    if (!f.read_at(off, b, 4) || std::memcmp(b, "fLaC", 4) != 0) return false;
    long long p = off + 4;
    for (int guard = 0; guard < 64; ++guard) {           // 元数据块最多几十个，防病态文件死循环
        unsigned char hd[4] = {0};
        if (!f.read_at(p, hd, 4)) return false;
        const bool last = (hd[0] & 0x80) != 0;
        const int  type = hd[0] & 0x7F;
        const long long len = ((long long)hd[1] << 16) | ((long long)hd[2] << 8) | (long long)hd[3];
        if (type == 0) {                                  // STREAMINFO
            if (len < 34) return false;
            unsigned char si[34] = {0};
            if (!f.read_at(p + 4, si, 34)) return false;
            const uint32_t rate = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | ((uint32_t)si[12] >> 4);
            const long long total = ((long long)(si[13] & 0x0F) << 32) | (long long)rd_be32(si + 14);
            if (rate == 0 || total <= 0) return false;     // 未知：不判
            out->seconds = (double)total / (double)rate;
            out->how = "FLAC STREAMINFO total_samples=" + std::to_string(total) +
                       " @" + std::to_string(rate) + " Hz";
            return out->seconds > 0.0;
        }
        if (last) return false;                            // 没有 STREAMINFO（不合规）
        p += 4 + len;
    }
    return false;
}

// ---------------- WAV（RIFF/WAVE）：fmt 的 byteRate + data 块声明大小 ----------------
// data 块头里的 size 是编码器写的**总字节数**，截断的文件仍然写着完整值 ⟹ 可比。
// 0xFFFFFFFF 是 RF64/流式写法的占位（真实长度在 ds64 里），这里不猜，直接不判。
inline bool probe_wav(RFile& f, long long off, DeclaredDuration* out) {
    unsigned char h[12] = {0};
    if (!f.read_at(off, h, 12)) return false;
    const bool riff = std::memcmp(h, "RIFF", 4) == 0;
    const bool rf64 = std::memcmp(h, "RF64", 4) == 0;
    if (!(riff || rf64) || std::memcmp(h + 8, "WAVE", 4) != 0) return false;
    long long p = off + 12;
    uint32_t byteRate = 0, dataSize = 0;
    bool hasFmt = false, hasData = false;
    for (int guard = 0; guard < 256; ++guard) {
        unsigned char ch[8] = {0};
        if (!f.read_at(p, ch, 8)) break;
        const uint32_t len = rd_le32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[16] = {0};
            if (len >= 16 && f.read_at(p + 8, fmt, 16)) {
                byteRate = rd_le32(fmt + 8);               // 每秒字节数（含声道/位深）
                hasFmt = (byteRate > 0);
            }
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataSize = len;
            hasData = true;
            break;                                          // 只需要第一个 data
        }
        if (len == 0xFFFFFFFFu) break;                       // RF64 的巨型块：不猜
        p += 8 + (long long)len + (long long)(len & 1);       // 块按 2 字节对齐
    }
    if (!hasFmt || !hasData) return false;
    if (dataSize == 0 || dataSize == 0xFFFFFFFFu) return false;
    out->seconds = (double)dataSize / (double)byteRate;
    out->how = "WAV data=" + std::to_string((unsigned long long)dataSize) +
               " B / byteRate=" + std::to_string((unsigned long long)byteRate);
    return out->seconds > 0.0;
}

// 采样率查表（MPEG1 / MPEG2 / MPEG2.5）
inline int mp3_rate(int ver, int srIdx) {
    static const int kRate[4]   = {44100, 48000, 32000, 0};
    static const int kRate2[4]  = {22050, 24000, 16000, 0};
    static const int kRate25[4] = {11025, 12000, 8000, 0};
    if (srIdx < 0 || srIdx > 3) return 0;
    if (ver == 3) return kRate[srIdx];
    if (ver == 2) return kRate2[srIdx];
    return kRate25[srIdx];
}

// ---------------- MP3：Xing/Info/VBRI 的**帧计数** ----------------
// 只用帧计数（乘每帧样本数），不做「文件字节×8/码率」估算：后者对 VBR 与带 padding 的 CBR
// 都会偏几个百分点，用来判"不完整"= 制造误报。没有 Xing/VBRI 的文件一律不判。
inline bool probe_mp3(RFile& f, long long off, DeclaredDuration* out) {
    unsigned char buf[4] = {0};
    // 找首个帧同步（最多扫 1 MB，挡掉垃圾前缀）
    long long found = -1;
    for (long long i = off; i < off + (1 << 20); ++i) {
        if (!f.read_at(i, buf, 4)) break;
        if (buf[0] != 0xFF || (buf[1] & 0xE0) != 0xE0) continue;
        const int ver = (buf[1] >> 3) & 0x3;        // 0=MPEG2.5 1=保留 2=MPEG2 3=MPEG1
        const int lay = (buf[1] >> 1) & 0x3;        // 1=Layer3 2=Layer2 3=Layer1
        if (ver == 1 || lay == 0) continue;
        const int brIdx = (buf[2] >> 4) & 0xF;
        const int srIdx = (buf[2] >> 2) & 0x3;
        if (brIdx == 0xF || srIdx == 0x3) continue;
        found = i;
        break;
    }
    if (found < 0) return false;
    unsigned char hdr[4] = {0};
    if (!f.read_at(found, hdr, 4)) return false;
    const int ver = (hdr[1] >> 3) & 0x3;
    const int lay = (hdr[1] >> 1) & 0x3;
    const bool mpeg1 = (ver == 3);
    const int mode = (hdr[3] >> 6) & 0x3;
    const int channels = (mode == 3) ? 1 : 2;
    // 每帧样本数：Layer1=384；Layer2=1152；Layer3 MPEG1=1152、MPEG2/2.5=576
    const long long spf = (lay == 3) ? 384 : (lay == 2 ? 1152 : (mpeg1 ? 1152 : 576));
    // 边信息长度（Layer3）：MPEG1 单声道 17 / 立体声 32；MPEG2/2.5 单声道 9 / 立体声 17
    const long long side = (lay == 1) ? (mpeg1 ? (channels == 1 ? 17 : 32) : (channels == 1 ? 9 : 17)) : 0;
    const int srIdx = (hdr[2] >> 2) & 0x3;
    const int rate = mp3_rate(ver, srIdx);
    if (rate <= 0) return false;
    const long long xingOff = found + 4 + side;
    unsigned char xh[16] = {0};
    if (f.read_at(xingOff, xh, 16) &&
        (std::memcmp(xh, "Xing", 4) == 0 || std::memcmp(xh, "Info", 4) == 0)) {
        const uint32_t flags = rd_be32(xh + 4);
        if (flags & 0x1) {
            const uint32_t frames = rd_be32(xh + 8);
            if (frames == 0) return false;
            long long total = (long long)frames * spf;
            // LAME/Lavf 的 gapless 信息：Xing 之后固定 120 字节处是 LAME 标签，
            // delay/padding 在 LAME 标签 +21 起的 3 字节（12+12 bit）。
            // ffmpeg 默认按它裁掉编码器延迟与尾部填充，所以声明时长也要扣掉同样的量
            // （与 src/meta/tags_native.h 的 MP3 时长口径一致：不减会偏 1~2 帧）。
            unsigned char ltag[4] = {0};
            if (f.read_at(xingOff + 120, ltag, 4) &&
                (std::memcmp(ltag, "LAME", 4) == 0 || std::memcmp(ltag, "Lavc", 4) == 0 ||
                 std::memcmp(ltag, "Lavf", 4) == 0 || std::memcmp(ltag, "GOGO", 4) == 0)) {
                unsigned char lame[3] = {0};
                if (f.read_at(xingOff + 120 + 21, lame, 3)) {
                    const long long delay   = ((long long)lame[0] << 4) | ((long long)lame[1] >> 4);
                    const long long padding = ((long long)(lame[1] & 0x0F) << 8) | (long long)lame[2];
                    if (delay + padding > 0 && delay + padding < total) total -= (delay + padding);
                }
            }
            out->seconds = (double)total / (double)rate;
            out->how = std::string(std::memcmp(xh, "Xing", 4) == 0 ? "MP3 Xing" : "MP3 Info") +
                       " frames=" + std::to_string(frames) + " spf=" + std::to_string(spf) +
                       " @" + std::to_string(rate) + " Hz";
            return out->seconds > 0.0;
        }
        return false;                                     // Xing 存在但没帧数：不判
    }
    // VBRI（Fraunhofer）：固定在首帧 +4+32
    unsigned char vb[18] = {0};
    const long long vbOff = found + 4 + 32;
    if (f.read_at(vbOff, vb, 18) && std::memcmp(vb, "VBRI", 4) == 0) {
        const uint32_t frames = rd_be32(vb + 14);
        if (frames == 0) return false;
        out->seconds = (double)((long long)frames * spf) / (double)rate;
        out->how = "MP3 VBRI frames=" + std::to_string(frames) + " @" + std::to_string(rate) + " Hz";
        return out->seconds > 0.0;
    }
    return false;
}

// ---------------- DSF（DSD Stream File）：fmt 的 sampleCount / samplingFreq ----------------
// DSF 的 fmt 块里 sampleCount 是**每声道**的 1-bit 样本总数；截断的 DSF 头仍是完整值。
// 这是库里最容易被"边拷边分析"的格式（258 MB 起），所以必须覆盖。
inline bool probe_dsf(RFile& f, long long off, DeclaredDuration* out) {
    unsigned char h[4] = {0};
    if (!f.read_at(off, h, 4) || std::memcmp(h, "DSD ", 4) != 0) return false;
    long long p = off + 28;                                // "DSD " + chunkSize(8) + fileSize(8) + metaOff(8)
    for (int guard = 0; guard < 8; ++guard) {
        unsigned char ch[12] = {0};
        if (!f.read_at(p, ch, 12)) return false;            // id(4) + size(8)
        const uint64_t len = rd_le64(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            // fmt 数据布局（DSF 规范，偏移从 fmt 数据起点 0 算）：
            //   +0 版本 +4 格式ID +8 声道类型 +12 声道数 +16 samplingFreq(4)
            //   +20 位深 +24 sampleCount(8) +32 块大小 +36 保留  （块总长 52）
            unsigned char fm[32] = {0};
            if (len < 32 || !f.read_at(p + 12, fm, 32)) return false;
            const uint32_t rate  = rd_le32(fm + 16);        // samplingFreq
            const uint64_t count = rd_le64(fm + 24);        // sampleCount
            if (rate == 0 || count == 0) return false;
            out->seconds = (double)count / (double)rate;
            out->how = "DSF sampleCount=" + std::to_string((unsigned long long)count) +
                       " @" + std::to_string(rate) + " Hz";
            return out->seconds > 0.0;
        }
        if (std::memcmp(ch, "data", 4) == 0) return false;   // data 在前 ⟹ 没有 fmt，不判
        if (len == 0 || len > (uint64_t)f.size()) return false;
        p += 12 + (long long)len;
    }
    return false;
}

}  // namespace cdetail

// 统一入口：返回 true 表示拿到了**可信的**容器声明时长（*seconds>0）。
// 返回 false 表示未知/不支持 ⟹ 调用方不得据此判定"不完整"（宁可漏报）。
inline bool probe_declared_duration(const std::string& path, DeclaredDuration* out) {
    if (!out) return false;
    *out = DeclaredDuration{};
    try {
        cdetail::RFile f;
        if (!f.open(path)) return false;
        DeclaredDuration d;
        // 容器嗅探顺序：内容优先、扩展名不参与（与 read_any 的口径一致）
        if (cdetail::probe_flac(f, 0, &d)) { *out = d; return true; }
        const long long id3 = cdetail::id3v2_len(f);
        if (id3 > 0 && cdetail::probe_flac(f, id3, &d)) { *out = d; return true; }
        if (cdetail::probe_wav(f, 0, &d)) { *out = d; return true; }
        if (cdetail::probe_dsf(f, 0, &d)) { *out = d; return true; }
        if (cdetail::probe_mp3(f, id3 > 0 ? id3 : 0, &d)) { *out = d; return true; }
    } catch (...) {
        *out = DeclaredDuration{};        // 任何异常都退化成"未知"，绝不影响 /api/health
    }
    return false;
}

}  // namespace rta::analysis
