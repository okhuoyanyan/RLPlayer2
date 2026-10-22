// wav.h — WAV (RIFF/WAVE) 读写
// 关键设计：内部保留【原始整数样本】，而不是只存归一化浮点。
// 这样"直通模式"可以做到逐位还原——浮点往返会引入不可逆的舍入，无法证明比特完美。
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <cmath>

namespace rta::audio {

struct AudioBuffer {
    uint32_t rate   = 44100;
    int      channels = 2;
    int      bits   = 16;          // 8 / 16 / 24 / 32
    bool     isFloat = false;      // true = IEEE float32
    std::vector<int32_t> raw;      // 交错原始整数样本；float 时存的是 float 的位模式

    size_t frames() const { return channels > 0 ? raw.size() / (size_t)channels : 0; }

    // 满幅标度：整数样本的满幅值（float 时为 1.0）
    double fullScale() const { return isFloat ? 1.0 : (double)(1u << (bits - 1)); }

    double sample(size_t frame, int ch) const {
        const int32_t v = raw[frame * (size_t)channels + (size_t)ch];
        if (isFloat) { float f; std::memcpy(&f, &v, 4); return (double)f; }
        return (double)v / fullScale();
    }
    void setSample(size_t frame, int ch, double x) {
        int32_t& v = raw[frame * (size_t)channels + (size_t)ch];
        if (isFloat) { float f = (float)x; std::memcpy(&v, &f, 4); return; }
        double s = x * fullScale();
        s = (s >= 0.0) ? std::floor(s + 0.5) : std::ceil(s - 0.5);
        const double lo = -(double)(1u << (bits - 1)), hi = (double)((1u << (bits - 1)) - 1u);
        if (s < lo) s = lo; if (s > hi) s = hi;
        v = (int32_t)s;
    }
};

// ---------------- 读取 ----------------
namespace detail {
inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

// F5（2026-10-22）：Windows 窄 fopen 按 ANSI 代码页（ACP，本机 936/GBK）解释路径，
// UTF-8 中文路径打不开（实证）。wav.h 不含 windows.h，用不了 MultiByteToWideChar，
// 这里自带一个纯标准库的 UTF-8 → UTF-16 转换（逐码点，>BMP 走代理对），配合 _wfopen。
// 非法 UTF-8 返回空串 → _wfopen 打不开 → 走既有的「无法打开文件: …」报错路径。
inline bool wav_utf8_next(const std::string& s, size_t& i, uint32_t& cp) {
    const unsigned char c = (unsigned char)s[i];
    int cont;
    if      (c < 0x80)                 { cp = c;        cont = 0; }
    else if (c >= 0xC2 && c <= 0xDF)   { cp = c & 0x1Fu;  cont = 1; }
    else if (c >= 0xE0 && c <= 0xEF)   { cp = c & 0x0Fu;  cont = 2; }
    else if (c >= 0xF0 && c <= 0xF4)   { cp = c & 0x07u;  cont = 3; }
    else return false;                                   // C0/C1、F5..FF、错位的续字节
    if (i + (size_t)cont >= s.size()) return false;      // 截断
    for (int k = 1; k <= cont; ++k) {
        const unsigned char cc = (unsigned char)s[i + (size_t)k];
        if ((cc & 0xC0) != 0x80) return false;           // 续字节必须 10xxxxxx
        cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
    }
    if ((cont == 1 && cp < 0x80) || (cont == 2 && cp < 0x800) || (cont == 3 && cp < 0x10000))
        return false;                                    // 过短编码
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;   // 越界/代理区
    i += (size_t)cont + 1;
    return true;
}
inline std::wstring wav_widen(const std::string& s) {
    std::wstring w; w.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        if (!wav_utf8_next(s, i, cp)) return std::wstring();             // 非法 UTF-8：交上层报错
        if (cp < 0x10000) w.push_back((wchar_t)cp);
        else {                                                           // 代理对
            const uint32_t t = cp - 0x10000;
            w.push_back((wchar_t)(0xD800 + (t >> 10)));
            w.push_back((wchar_t)(0xDC00 + (t & 0x3FF)));
        }
    }
    return w;
}
// 按平台打开文件：Windows 走宽字符（UTF-8 路径不再经 ANSI 代码页）；
// 非 Windows 窄路径行为不变。
inline FILE* wav_open(const std::string& path, const wchar_t* wmode, const char* nmode) {
#if defined(_WIN32)
    (void)nmode;
    return ::_wfopen(wav_widen(path).c_str(), wmode);
#else
    (void)wmode;
    return std::fopen(path.c_str(), nmode);
#endif
}
}

inline AudioBuffer read_wav(const std::string& path) {
    FILE* f = detail::wav_open(path, L"rb", "rb");   // F5：Windows 用宽字符打开（UTF-8 中文路径实证打不开窄 fopen）
    if (!f) throw std::runtime_error("无法打开文件: " + path);

    // HF-03：Windows 上 long 是 32 位。旧代码对 >2GB 的文件（长曲目/高码率解码的
    // 临时 WAV 可达）ftell 返回 -1，随后 buf.resize((size_t)-1) 直接抛裸的
    // "vector too long"（2.log 症状之一）。改用 64 位定位并显式检查，杜绝该路径。
    long long fsize = -1;
#if defined(_WIN32)
    ::_fseeki64(f, 0, SEEK_END);
    fsize = ::_ftelli64(f);
    ::_fseeki64(f, 0, SEEK_SET);
#else
    std::fseek(f, 0, SEEK_END);
    fsize = (long long)std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
#endif
    if (fsize < 0) { std::fclose(f); throw std::runtime_error("无法确定文件大小(文件=" + path + ")"); }
    if (fsize == 0) { std::fclose(f); throw std::runtime_error("文件为空(文件=" + path + ", 大小=0 字节)"); }
    constexpr long long kMaxFileBytes = 1LL << 32;   // 4 GiB 防御上限：真音乐文件到不了，
    if (fsize > kMaxFileBytes) {                     // 超过按异常处理，防止后续巨量分配
        std::fclose(f);
        throw std::runtime_error("文件超过防御上限(文件=" + path + ", 大小=" + std::to_string(fsize) +
                                 " 字节, 上限=" + std::to_string(kMaxFileBytes) + " 字节)");
    }
    std::vector<uint8_t> buf((size_t)fsize);
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fclose(f);
        throw std::runtime_error("读取失败(文件=" + path + ", 期望=" + std::to_string(fsize) + " 字节)");
    }
    std::fclose(f);

    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) || std::memcmp(buf.data() + 8, "WAVE", 4))
        throw std::runtime_error("不是合法的 WAV 文件(文件=" + path +
                                 ", 实际字节=" + std::to_string(buf.size()) + ", 需 RIFF/WAVE 头且≥44 字节)");

    AudioBuffer ab;
    uint16_t fmtTag = 0, ch = 0, bps = 0;
    uint32_t rate = 0;
    const uint8_t* dataPtr = nullptr;
    size_t dataLen = 0;
    bool     haveFmt  = false;       // 解析到了合法 fmt 块
    bool     haveData = false;
    int      fmtShortSz = -1;        // fmt 块存在但过短（sz<16）时记录其声明大小
    uint32_t truncDeclared = 0;      // data 块声明越界（截断）时的声明值
    size_t   truncAvail   = 0;       // 截断时 data 块体实际可用字节

    size_t off = 12;
    while (off + 8 <= buf.size()) {
        const uint8_t* id = buf.data() + off;
        const uint32_t sz = detail::rd32(buf.data() + off + 4);
        const uint8_t* body = buf.data() + off + 8;
        if (off + 8 + sz > buf.size()) {              // 块声明超出文件末尾：记录并停止
            if (!std::memcmp(id, "data", 4)) {
                truncDeclared = sz;
                truncAvail = buf.size() - (off + 8);
            }
            break;
        }
        if (!std::memcmp(id, "fmt ", 4)) {
            if (sz >= 16) {
                fmtTag = detail::rd16(body);
                ch     = detail::rd16(body + 2);
                rate   = detail::rd32(body + 4);
                bps    = detail::rd16(body + 14);
                if (fmtTag == 0xFFFE && sz >= 40) {       // WAVE_FORMAT_EXTENSIBLE
                    const uint16_t valid = detail::rd16(body + 18);
                    const uint16_t sub   = detail::rd16(body + 24);
                    fmtTag = sub;
                    (void)valid;
                }
                haveFmt = true;
            } else {
                fmtShortSz = (int)sz;
            }
        } else if (!std::memcmp(id, "data", 4)) {
            dataPtr = body; dataLen = sz; haveData = true;
        }
        off += 8 + sz + (sz & 1);
    }

    // HF-03：所有拒绝分支的错误文本都带 文件名 + fmt 头字段 + dataLen + 实际可用字节，
    // 让日志能直接定位问题文件（不再出现无上下文的失败）。
    auto ctx = [&](const std::string& head) {
        char b[768];
        std::snprintf(b, sizeof(b),
                      "%s(文件=%s, fmtTag=%u, ch=%u, bps=%u, rate=%u, dataLen=%zu, 实际文件字节=%zu)",
                      head.c_str(), path.c_str(),
                      (unsigned)fmtTag, (unsigned)ch, (unsigned)bps, (unsigned)rate,
                      dataLen, buf.size());
        return std::string(b);
    };

    if (!haveFmt) {
        if (fmtShortSz >= 0)
            throw std::runtime_error(ctx("WAV fmt 块过短(声明 sz=" + std::to_string(fmtShortSz) + " 字节, 需≥16)"));
        throw std::runtime_error(ctx("WAV 缺少 fmt 块"));
    }
    if (!haveData) {
        if (truncDeclared)
            throw std::runtime_error(ctx("WAV data 块被截断(声明 dataLen=" + std::to_string(truncDeclared) +
                                         " 字节, 块体实际可用=" + std::to_string(truncAvail) + " 字节)"));
        throw std::runtime_error(ctx("WAV 缺少 data 块"));
    }
    if (fmtTag != 1 && fmtTag != 3)
        throw std::runtime_error(ctx("WAV 编码不是线性 PCM(fmtTag 需 1=PCM 或 3=IEEE float)"));
    if (fmtTag == 3) {
        if (bps != 32 && bps != 64)
            throw std::runtime_error(ctx("float WAV 位深非法(允许 32 或 64)"));
        if (bps == 64)
            throw std::runtime_error(ctx("float64 WAV 不支持(读取管线按 float32 位型存储),请先转成 32-bit float 或整型 PCM"));
    } else if (bps != 8 && bps != 16 && bps != 24 && bps != 32) {
        throw std::runtime_error(ctx("WAV 位深非法(允许 8/16/24/32)"));
    }
    if (ch < 1 || ch > 32)
        throw std::runtime_error(ctx("WAV 声道数非法(允许 1..32)"));

    ab.rate = rate; ab.channels = ch; ab.bits = bps;
    ab.isFloat = (fmtTag == 3);
    const int bytesPer = bps / 8;
    if (bytesPer <= 0) throw std::runtime_error(ctx("WAV 位深换算字节数非法(bytesPer<=0)"));   // 防御（上面已挡）
    // dataLen 对照实际缓冲区边界（本函数把整个文件读进 buf，buf.size() 即总长上界）
    const size_t avail = buf.size() - (size_t)(dataPtr - buf.data());
    if (dataLen > avail)
        throw std::runtime_error(ctx("WAV data 块越界(dataLen=" + std::to_string(dataLen) +
                                     " > 块体实际可用=" + std::to_string(avail) + ")"));
    const unsigned long long nS = (unsigned long long)dataLen / (unsigned long long)bytesPer;
    constexpr unsigned long long kMaxSamples = 1ULL << 32;   // 防御上限：防溢出/巨量分配路径
    if (nS > kMaxSamples)
        throw std::runtime_error(ctx("样本数超过防御上限(" + std::to_string(nS) + " > 2^32, bytesPer=" +
                                     std::to_string(bytesPer) + ")"));
    const size_t nSamples = (size_t)nS;
    ab.raw.resize(nSamples);

    for (size_t i = 0; i < nSamples; ++i) {
        const uint8_t* p = dataPtr + i * (size_t)bytesPer;
        int32_t v = 0;
        if (ab.isFloat) {
            uint32_t u = detail::rd32(p); std::memcpy(&v, &u, 4);
        } else if (bps == 8) {
            v = (int32_t)p[0] - 128;                  // WAV 的 8-bit PCM 是无符号，中心 128
        } else if (bps == 16) {
            v = (int32_t)(int16_t)detail::rd16(p);
        } else if (bps == 24) {
            int32_t t = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (t & 0x800000) t |= (int32_t)0xFF000000;      // 符号扩展
            v = t;
        } else if (bps == 32) {
            v = (int32_t)detail::rd32(p);
        }
        ab.raw[i] = v;
    }
    return ab;
}

// ---------------- 写入 ----------------
namespace detail {
inline void wr32(std::vector<uint8_t>& o, uint32_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 24)); }
inline void wr16(std::vector<uint8_t>& o, uint16_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); }
}

inline void write_wav(const std::string& path, const AudioBuffer& ab) {
    const int bytesPer = ab.bits / 8;
    const uint32_t dataBytes = (uint32_t)(ab.raw.size() * (size_t)bytesPer);

    std::vector<uint8_t> o;
    o.insert(o.end(), {'R','I','F','F'});
    detail::wr32(o, 36 + dataBytes);
    o.insert(o.end(), {'W','A','V','E'});
    o.insert(o.end(), {'f','m','t',' '});
    detail::wr32(o, 16);
    detail::wr16(o, ab.isFloat ? (uint16_t)3 : (uint16_t)1);
    detail::wr16(o, (uint16_t)ab.channels);
    detail::wr32(o, ab.rate);
    detail::wr32(o, ab.rate * (uint32_t)ab.channels * (uint32_t)bytesPer);
    detail::wr16(o, (uint16_t)(ab.channels * bytesPer));
    detail::wr16(o, (uint16_t)ab.bits);
    o.insert(o.end(), {'d','a','t','a'});
    detail::wr32(o, dataBytes);

    for (int32_t v : ab.raw) {
        if (ab.isFloat) {
            uint32_t u; std::memcpy(&u, &v, 4);
            detail::wr32(o, u);
        } else if (ab.bits == 8) {
            o.push_back((uint8_t)((int)v + 128));    // 无符号 8-bit，中心 128（与 read_wav 对称）
        } else if (ab.bits == 16) {
            detail::wr16(o, (uint16_t)(int16_t)v);
        } else if (ab.bits == 24) {
            o.push_back((uint8_t)(v & 0xFF));
            o.push_back((uint8_t)((v >> 8) & 0xFF));
            o.push_back((uint8_t)((v >> 16) & 0xFF));
        } else {
            detail::wr32(o, (uint32_t)v);
        }
    }
    FILE* f = detail::wav_open(path, L"wb", "wb");   // F5：与 read_wav 同一宽字符路径
    if (!f) throw std::runtime_error("无法写入: " + path);
    const size_t w = std::fwrite(o.data(), 1, o.size(), f);
    std::fclose(f);
    if (w != o.size()) throw std::runtime_error("写入不完整: " + path);
}

} // namespace rta::audio
