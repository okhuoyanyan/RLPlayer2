// dsd_reader.h — DSF / DFF 原生读取 + 写出（ROADMAP P0-1 / P0-2 的底座）
//
// 为什么自己读：现在 DSD 文件交给 ffmpeg 先解成 PCM，等于「DSD 进、PCM 中转、DSD 出」，
// 量化链路叠加两次，而且拿不到原始 1-bit 流，DirectSDM 直通无从谈起。
// 原生读取后，1-bit 流可以：① 源速率 == 目标速率时逐位直通（DirectSDM）；
// ② 经我们自己的积分器（FIR/IIR/CIC）抽取成 PCM，再走原有管线。
//
// 格式要点（依据公开规范 + ffmpeg 的 dsf/dff demuxer 行为）：
//   DSF：小端；头 28 字节（"DSD " 4 + 块长 8 + 文件长 8 + 元数据指针 8）；
//        fmt 块 52 字节（含 12 字节块头）；data 按「每声道 blockSize 字节」分块交错；
//        **位序 LSB-first**（Sony 规范；ffmpeg 用 dsd_lsbf 解码器）。
//   DFF：DSDIFF，大端；FRM8("DSD ") → PROP("SND ") → FS / CHNL / CMPR；DSD 块按声道
//        独立连续存放；**位序 MSB-first**（ffmpeg 用 dsd_msbf）。DST 压缩块不支持。
//
// 统一内部表示：每声道一份 **MSB-first** 打包字节流（bit7 = 时间上更早的位）。
// 这样 DoP 打包时「每 16 位一组」正好等于两个相邻字节 (b0<<8)|b1，不需要再翻转位序。
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace rta::audio {

enum class DsdBitOrder { MsbFirst, LsbFirst };

inline const char* dsd_bit_order_name(DsdBitOrder o) {
    return (o == DsdBitOrder::LsbFirst) ? "LSB-first" : "MSB-first";
}

struct DsdStream {
    uint32_t rate = 0;                              // 1-bit 采样率（bits/s）
    int      channels = 0;
    std::vector<std::vector<uint8_t>> data;         // 每声道，MSB-first 打包
    std::vector<std::string> channelIds;            // "SLFT"/"SRGT"/…（DFF 取 CHNL，DSF 按声道数生成）
    uint64_t bits = 0;                              // 每声道有效位数
    DsdBitOrder fileOrder = DsdBitOrder::MsbFirst;  // 原文件位序（如实报告，不改变内部表示）
    std::string container;                          // "dsf" / "dff"
    int channelType = 0;                            // DSF 的 channel type 字段；DFF 为 0

    size_t bytesPerChannel() const { return data.empty() ? 0 : data[0].size(); }
    double seconds() const { return rate ? (double)bits / (double)rate : 0.0; }

    // MSB-first：n 号位在字节 n>>3 的 bit (7 - (n&7))
    bool bit(size_t ch, uint64_t n) const {
        if (ch >= data.size() || (n >> 3) >= data[ch].size()) return false;
        return ((data[ch][(size_t)(n >> 3)] >> (7 - (n & 7))) & 1u) != 0;
    }
    void setBit(size_t ch, uint64_t n, bool v) {
        const size_t byte = (size_t)(n >> 3);
        if (ch >= data.size() || byte >= data[ch].size()) return;
        const uint8_t mask = (uint8_t)(1u << (7 - (n & 7)));
        if (v) data[ch][byte] = (uint8_t)(data[ch][byte] | mask);
        else   data[ch][byte] = (uint8_t)(data[ch][byte] & (uint8_t)~mask);
    }

    // 互斥性自检：每个样本必须是 +1 或 -1（1-bit 流只有这两种取值）
    long long ones() const {
        long long n = 0;
        for (const auto& ch : data) for (uint8_t b : ch) n += __builtin_popcount(b);
        return n;
    }
};

namespace dsd_detail {

inline uint8_t reverse_bits8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

inline uint32_t rdl32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t rdl64(const uint8_t* p) {
    return (uint64_t)rdl32(p) | ((uint64_t)rdl32(p + 4) << 32);
}
inline uint16_t rdb16(const uint8_t* p) { return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]); }
inline uint32_t rdb32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
inline uint64_t rdb64(const uint8_t* p) {
    return ((uint64_t)rdb32(p) << 32) | (uint64_t)rdb32(p + 4);
}

inline void wrl16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back((uint8_t)(v & 0xFF)); o.push_back((uint8_t)(v >> 8));
}
inline void wrl32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
inline void wrl64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
inline void wrb32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 3; i >= 0; --i) o.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
inline void wrb64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 7; i >= 0; --i) o.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
inline void wrTag(std::vector<uint8_t>& o, const char* t) {
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t)t[i]);
}

inline std::vector<uint8_t> read_file_bytes(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("无法打开文件: " + path);
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); throw std::runtime_error("空文件或无法定长: " + path); }
    std::vector<uint8_t> b((size_t)n);
    const size_t got = std::fread(b.data(), 1, b.size(), f);
    std::fclose(f);
    if (got != b.size()) throw std::runtime_error("读取不完整: " + path);
    return b;
}

inline void write_file_bytes(const std::string& path, const std::vector<uint8_t>& b) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("无法写入: " + path);
    const size_t w = std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
    if (w != b.size()) throw std::runtime_error("写入不完整: " + path);
}

inline std::vector<std::string> dsf_channel_ids(int chNum) {
    switch (chNum) {
        case 1: return { "C   " };
        case 2: return { "SLFT", "SRGT" };
        case 3: return { "SLFT", "SRGT", "C   " };
        case 4: return { "SLFT", "SRGT", "LS  ", "RS  " };
        case 5: return { "SLFT", "SRGT", "C   ", "LS  ", "RS  " };
        case 6: return { "SLFT", "SRGT", "C   ", "LFE ", "LS  ", "RS  " };
        default: break;
    }
    std::vector<std::string> ids;
    for (int i = 0; i < chNum; ++i) ids.push_back("CH" + std::to_string(i + 1));
    return ids;
}

// DSF channel type：1=mono 2=stereo 3=3ch 4=quad 5=4ch 6=5ch 7=5.1
inline int dsf_channel_type(int chNum) {
    switch (chNum) {
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: return 6;
        case 6: return 7;
        default: return 0;
    }
}

} // namespace dsd_detail

// ---------------- DSF 读取 ----------------
inline DsdStream read_dsf(const std::string& path) {
    using namespace dsd_detail;
    const std::vector<uint8_t> f = read_file_bytes(path);
    if (f.size() < 28 || std::memcmp(f.data(), "DSD ", 4) != 0)
        throw std::runtime_error("不是 DSF 文件（缺少 \"DSD \" 头）: " + path);

    const uint64_t hdrSize = rdl64(f.data() + 4);
    if (hdrSize != 28)
        throw std::runtime_error("DSF 头长度异常（" + std::to_string(hdrSize) + "，应为 28）");

    const size_t fmtOff = (size_t)hdrSize;
    if (fmtOff + 12 + 40 > f.size() || std::memcmp(f.data() + fmtOff, "fmt ", 4) != 0)
        throw std::runtime_error("DSF 缺少 fmt 块或文件被截断");

    const uint64_t fmtSize = rdl64(f.data() + fmtOff + 4);
    if (fmtSize < 52)
        throw std::runtime_error("DSF fmt 块长度异常（" + std::to_string(fmtSize) + "）");

    const uint8_t* b = f.data() + fmtOff + 12;
    const uint32_t fmtId     = rdl32(b + 4);
    const uint32_t chType    = rdl32(b + 8);
    const uint32_t chNum     = rdl32(b + 12);
    const uint32_t rate      = rdl32(b + 16);
    const uint32_t bitsPerSm = rdl32(b + 20);
    const uint64_t sampleCnt = rdl64(b + 24);
    const uint32_t blockSize = rdl32(b + 32);

    if (fmtId != 0)
        throw std::runtime_error("DSF format id = " + std::to_string(fmtId) + "（DST 压缩）暂不支持");
    if (bitsPerSm != 1)
        throw std::runtime_error("DSF bits per sample = " + std::to_string(bitsPerSm) + "（应为 1）");
    if (chNum < 1 || chNum > 8)
        throw std::runtime_error("DSF 声道数异常: " + std::to_string(chNum));
    if (rate < 44100)
        throw std::runtime_error("DSF 采样率异常: " + std::to_string(rate));
    if (blockSize == 0 || blockSize > (1u << 20))
        throw std::runtime_error("DSF 每声道块长异常: " + std::to_string(blockSize));
    if (sampleCnt == 0)
        throw std::runtime_error("DSF 样本数为 0");

    // 找 data 块：规范里紧跟 fmt（元数据指针在文件头单列），这里按块链表扫描更稳
    size_t off = fmtOff + (size_t)fmtSize;
    const uint8_t* dataPtr = nullptr;
    uint64_t dataSize = 0;
    while (off + 12 <= f.size()) {
        const uint64_t sz = rdl64(f.data() + off + 4);
        if (sz < 12 || off + sz > f.size()) break;
        if (std::memcmp(f.data() + off, "data", 4) == 0) {
            dataPtr = f.data() + off + 12;
            dataSize = sz - 12;
            break;
        }
        off += (size_t)sz;
    }
    if (!dataPtr) throw std::runtime_error("DSF 缺少 data 块");
    if (dataSize < (uint64_t)chNum * 8) throw std::runtime_error("DSF data 块过短");

    const uint64_t needBytes = (sampleCnt + 7) / 8;         // 每声道字节数
    const uint64_t fullBlocks = needBytes / blockSize;
    const uint64_t remBytes   = needBytes % blockSize;
    const uint64_t needData   = (fullBlocks + (remBytes ? 1 : 0)) * (uint64_t)chNum * blockSize;

    DsdStream s;
    s.rate = rate;
    s.channels = (int)chNum;
    s.container = "dsf";
    s.fileOrder = DsdBitOrder::LsbFirst;
    s.channelType = (int)chType;
    s.channelIds = dsf_channel_ids((int)chNum);
    s.bits = sampleCnt;
    s.data.assign((size_t)chNum, std::vector<uint8_t>((size_t)needBytes, 0));

    // 分块交错：块 b 内，声道 c 的偏移 = b*chNum*blockSize + c*blockSize。
    // 末块可能不满：若文件没有按整块补齐，则按「紧排」处理（每声道只剩 remBytes）。
    std::vector<uint64_t> written((size_t)chNum, 0);
    for (uint64_t blk = 0; blk <= fullBlocks; ++blk) {
        const uint64_t thisBlock = (blk < fullBlocks) ? blockSize : remBytes;
        if (thisBlock == 0) break;
        const uint64_t base = blk * (uint64_t)chNum * blockSize;
        const bool paddedLast = (blk == fullBlocks) && (dataSize >= needData);
        for (uint64_t c = 0; c < chNum; ++c) {
            const uint64_t src = paddedLast ? (base + c * blockSize) : (base + c * thisBlock);
            const uint64_t take = std::min<uint64_t>(thisBlock, needBytes - written[(size_t)c]);
            if (take == 0) continue;
            if (src + take > dataSize)
                throw std::runtime_error("DSF data 块长度不足（文件被截断）");
            for (uint64_t i = 0; i < take; ++i)
                s.data[(size_t)c][(size_t)(written[(size_t)c] + i)] = reverse_bits8(dataPtr[src + i]);
            written[(size_t)c] += take;
        }
    }
    for (int c = 0; c < (int)chNum; ++c)
        if (written[(size_t)c] < needBytes)
            throw std::runtime_error("DSF 声道 " + std::to_string(c) + " 数据不足");
    return s;
}

// ---------------- DFF 读取 ----------------
inline DsdStream read_dff(const std::string& path) {
    using namespace dsd_detail;
    const std::vector<uint8_t> f = read_file_bytes(path);
    if (f.size() < 16 || std::memcmp(f.data(), "FRM8", 4) != 0)
        throw std::runtime_error("不是 DFF(DSDIFF) 文件（缺少 \"FRM8\" 头）: " + path);
    if (std::memcmp(f.data() + 12, "DSD ", 4) != 0)
        throw std::runtime_error("DFF 的 form type 不是 \"DSD \"（不支持 DST 或其它容器）");

    const uint64_t frm8Size = rdb64(f.data() + 4);
    const size_t end = std::min<size_t>(f.size(), (size_t)(12 + frm8Size));

    uint32_t rate = 0;
    uint32_t declaredCh = 0;
    std::vector<std::string> ids;
    std::string cmpType;
    const uint8_t* dataPtr = nullptr;
    uint64_t dataSize = 0;

    // PROP 里的子块
    const auto parseProp = [&](size_t p, uint64_t sz) {
        const size_t pe = std::min<size_t>(end, p + (size_t)sz);
        if (p + 4 > pe || std::memcmp(f.data() + p, "SND ", 4) != 0)
            throw std::runtime_error("DFF PROP 块不是 \"SND \"");
        size_t q = p + 4;
        while (q + 12 <= pe) {
            const uint64_t ss = rdb64(f.data() + q + 4);
            const uint8_t* body = f.data() + q + 12;
            if (q + 12 + ss > pe) break;
            if (std::memcmp(f.data() + q, "FS  ", 4) == 0 && ss >= 4) {
                rate = rdb32(body);
            } else if (std::memcmp(f.data() + q, "CHNL", 4) == 0 && ss >= 2) {
                declaredCh = rdb16(body);
                ids.clear();
                for (uint32_t i = 0; i < declaredCh; ++i) {
                    if (2 + (uint64_t)i * 4 + 4 <= ss)
                        ids.push_back(std::string((const char*)body + 2 + i * 4, 4));
                    else ids.push_back("CH" + std::to_string(i + 1));
                }
            } else if (std::memcmp(f.data() + q, "CMPR", 4) == 0 && ss >= 4) {
                cmpType.assign((const char*)body, 4);
            }
            q += 12 + (size_t)ss + (size_t)(ss & 1);
        }
    };

    size_t off = 16;
    while (off + 12 <= end) {
        const uint64_t sz = rdb64(f.data() + off + 4);
        const uint8_t* body = f.data() + off + 12;
        if (off + 12 + sz > end) break;
        if (std::memcmp(f.data() + off, "PROP", 4) == 0) {
            parseProp(off + 12, sz);
        } else if (std::memcmp(f.data() + off, "DSD ", 4) == 0) {
            dataPtr = body; dataSize = sz;
        } else if (std::memcmp(f.data() + off, "DST ", 4) == 0 ||
                   std::memcmp(f.data() + off, "DSTI", 4) == 0) {
            throw std::runtime_error("DFF 使用 DST 压缩（DST 块），暂不支持");
        }
        off += 12 + (size_t)sz + (size_t)(sz & 1);
    }

    if (!dataPtr) throw std::runtime_error("DFF 缺少 DSD 数据块");
    if (rate == 0) throw std::runtime_error("DFF 缺少 FS（采样率）子块");
    if (!cmpType.empty() && cmpType != "DSD ")
        throw std::runtime_error("DFF 压缩类型不是 \"DSD \"（为 \"" + cmpType + "\"）");

    const uint32_t ch = declaredCh ? declaredCh : 2;
    if (ch < 1 || ch > 8) throw std::runtime_error("DFF 声道数异常: " + std::to_string(ch));
    if (dataSize == 0 || dataSize % ch != 0)
        throw std::runtime_error("DFF DSD 数据长度 " + std::to_string(dataSize) +
                                 " 不能被声道数 " + std::to_string(ch) + " 整除");

    const uint64_t perCh = dataSize / ch;
    DsdStream s;
    s.rate = rate;
    s.channels = (int)ch;
    s.container = "dff";
    s.fileOrder = DsdBitOrder::MsbFirst;
    s.channelType = 0;
    s.channelIds = ids.size() == (size_t)ch ? ids : dsf_channel_ids((int)ch);
    s.bits = perCh * 8;
    s.data.assign((size_t)ch, {});
    for (uint32_t c = 0; c < ch; ++c)
        s.data[(size_t)c].assign(dataPtr + (size_t)c * perCh, dataPtr + (size_t)(c + 1) * perCh);
    return s;
}

// 按扩展名分派；不认识的抛异常（调用方据此回退 ffmpeg）
inline DsdStream read_dsd(const std::string& path) {
    std::string ext;
    const size_t d = path.find_last_of('.');
    if (d != std::string::npos) {
        ext = path.substr(d);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
    }
    if (ext == ".dsf") return read_dsf(path);
    if (ext == ".dff") return read_dff(path);
    throw std::runtime_error("不是 DSD 文件（按扩展名判断）: " + path);
}


// ---------------- 只读文件头（DirectSDM 判定 / 库信息） ----------------
// 不载入数据，只读前 64 KB。DirectSDM 要在解码整曲之前判断"源速率 == 目标速率"，
// 所以不能先 read_dsd（DSD256 整曲几百 MB）。
struct DsdInfo {
    uint32_t rate = 0;
    int      channels = 0;
    uint64_t bits = 0;          // 每声道位数；DFF 没读到 DSD 块头时为 0（未知时长）
    std::string container;      // "dsf" / "dff"
    DsdBitOrder fileOrder = DsdBitOrder::MsbFirst;
};

inline DsdInfo probe_dsd(const std::string& path) {
    using namespace dsd_detail;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) throw std::runtime_error("无法打开文件: " + path);
    std::fseek(fp, 0, SEEK_END);
    const long total = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    const size_t want = (size_t)std::min<long>(total, 65536);
    std::vector<uint8_t> b(want);
    const size_t got = std::fread(b.data(), 1, want, fp);
    std::fclose(fp);
    b.resize(got);
    if (b.size() < 16) throw std::runtime_error("文件过短: " + path);

    if (std::memcmp(b.data(), "DSD ", 4) == 0) {
        if (b.size() < 80 || std::memcmp(b.data() + 28, "fmt ", 4) != 0)
            throw std::runtime_error("DSF 头不完整: " + path);
        const uint8_t* fmt = b.data() + 40;
        DsdInfo i;
        i.container = "dsf";
        i.fileOrder = DsdBitOrder::LsbFirst;
        i.channels = (int)rdl32(fmt + 12);
        i.rate = rdl32(fmt + 16);
        i.bits = rdl64(fmt + 24);
        if (rdl32(fmt + 20) != 1)
            throw std::runtime_error("DSF bits per sample 不是 1（DST 压缩？）");
        if (rdl32(fmt + 4) != 0)
            throw std::runtime_error("DSF format id 不是 0（DST 压缩？）");
        if (i.channels < 1 || i.channels > 8 || i.rate < 44100)
            throw std::runtime_error("DSF 头字段异常");
        return i;
    }

    if (std::memcmp(b.data(), "FRM8", 4) == 0) {
        if (std::memcmp(b.data() + 12, "DSD ", 4) != 0)
            throw std::runtime_error("DFF 的 form type 不是 \"DSD \"");
        DsdInfo i;
        i.container = "dff";
        i.fileOrder = DsdBitOrder::MsbFirst;
        uint32_t declaredCh = 0;
        uint64_t dsdSize = 0;
        size_t off = 16;
        while (off + 12 <= b.size()) {
            const uint64_t sz = rdb64(b.data() + off + 4);
            if (std::memcmp(b.data() + off, "PROP", 4) == 0 && off + 12 + sz <= b.size()) {
                size_t p = off + 16;                       // 跳过 "SND "
                const size_t pe = off + 12 + (size_t)sz;
                while (p + 12 <= pe) {
                    const uint64_t ss = rdb64(b.data() + p + 4);
                    if (p + 12 + ss > pe) break;
                    if (std::memcmp(b.data() + p, "FS  ", 4) == 0 && ss >= 4)
                        i.rate = rdb32(b.data() + p + 12);
                    else if (std::memcmp(b.data() + p, "CHNL", 4) == 0 && ss >= 2)
                        declaredCh = rdb16(b.data() + p + 12);
                    p += 12 + (size_t)ss + (size_t)(ss & 1);
                }
            } else if (std::memcmp(b.data() + off, "DSD ", 4) == 0) {
                dsdSize = sz;
            } else if (std::memcmp(b.data() + off, "DST ", 4) == 0 ||
                       std::memcmp(b.data() + off, "DSTI", 4) == 0) {
                throw std::runtime_error("DFF 使用 DST 压缩，暂不支持");
            }
            if (off + 12 + sz > b.size()) break;           // 下一个块在缓冲之外，停
            off += 12 + (size_t)sz + (size_t)(sz & 1);
        }
        if (i.rate == 0) throw std::runtime_error("DFF 缺少 FS 子块");
        i.channels = (int)declaredCh;
        if (i.channels < 1 || i.channels > 8) throw std::runtime_error("DFF 缺少/非法 CHNL");
        if (dsdSize && dsdSize % (uint64_t)i.channels == 0)
            i.bits = dsdSize / (uint64_t)i.channels * 8;
        return i;
    }
    throw std::runtime_error("不是 DSF/DFF 文件: " + path);
}

// ---------------- 写出（P0-2 测试素材生成器用） ----------------
// DSF：blockBytes 默认 4096（规范默认值）；末块按整块补零。
inline void write_dsf(const std::string& path, const DsdStream& s, uint32_t blockBytes = 4096) {
    using namespace dsd_detail;
    if (s.channels < 1 || s.channels > 8) throw std::runtime_error("write_dsf: 声道数非法");
    if (s.rate == 0) throw std::runtime_error("write_dsf: 采样率为 0");
    if (s.bits == 0) throw std::runtime_error("write_dsf: 位数为 0");
    const uint64_t bytesPerCh = (s.bits + 7) / 8;
    for (int c = 0; c < s.channels; ++c)
        if (s.data[(size_t)c].size() < bytesPerCh)
            throw std::runtime_error("write_dsf: 声道 " + std::to_string(c) + " 数据不足");

    const uint64_t blocks = (bytesPerCh + blockBytes - 1) / blockBytes;
    const uint64_t payload = blocks * (uint64_t)s.channels * blockBytes;
    const uint64_t fileSize = 28 + 52 + 12 + payload;

    std::vector<uint8_t> o;
    wrTag(o, "DSD ");
    wrl64(o, 28);
    wrl64(o, fileSize);
    wrl64(o, 0);                                  // 无元数据
    wrTag(o, "fmt ");
    wrl64(o, 52);
    wrl32(o, 1);                                  // format version
    wrl32(o, 0);                                  // format id: DSD raw
    wrl32(o, (uint32_t)dsf_channel_type(s.channels));
    wrl32(o, (uint32_t)s.channels);
    wrl32(o, s.rate);
    wrl32(o, 1);                                  // bits per sample
    wrl64(o, s.bits);                             // 每声道样本数（位）
    wrl32(o, blockBytes);
    wrl32(o, 0);
    wrTag(o, "data");
    wrl64(o, 12 + payload);

    for (uint64_t blk = 0; blk < blocks; ++blk) {
        for (int c = 0; c < s.channels; ++c) {
            for (uint32_t i = 0; i < blockBytes; ++i) {
                const uint64_t idx = blk * blockBytes + i;
                const uint8_t v = (idx < bytesPerCh) ? reverse_bits8(s.data[(size_t)c][(size_t)idx]) : 0;
                o.push_back(v);
            }
        }
    }
    write_file_bytes(path, o);
}

// DFF：大端，按声道连续存放，MSB-first 直通。
inline void write_dff(const std::string& path, const DsdStream& s) {
    using namespace dsd_detail;
    if (s.channels < 1 || s.channels > 8) throw std::runtime_error("write_dff: 声道数非法");
    if (s.rate == 0) throw std::runtime_error("write_dff: 采样率为 0");
    const uint64_t bytesPerCh = (s.bits + 7) / 8;
    for (int c = 0; c < s.channels; ++c)
        if (s.data[(size_t)c].size() < bytesPerCh)
            throw std::runtime_error("write_dff: 声道 " + std::to_string(c) + " 数据不足");

    auto chunk = [](const char* id, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> c;
        wrTag(c, id);
        wrb64(c, body.size());
        c.insert(c.end(), body.begin(), body.end());
        if (body.size() & 1) c.push_back(0);
        return c;
    };

    std::vector<uint8_t> fver;
    wrb32(fver, 0x01050000u);

    std::vector<uint8_t> fs;
    wrb32(fs, s.rate);

    std::vector<uint8_t> chnl;
    {
        std::vector<uint8_t> b;
        b.push_back((uint8_t)((s.channels >> 8) & 0xFF));
        b.push_back((uint8_t)(s.channels & 0xFF));
        for (int c = 0; c < s.channels; ++c) {
            std::string id = (c < (int)s.channelIds.size() && s.channelIds[(size_t)c].size() == 4)
                                 ? s.channelIds[(size_t)c] : "CH" + std::to_string(c + 1);
            for (int i = 0; i < 4; ++i) b.push_back((uint8_t)id[(size_t)i]);
        }
        if (b.size() & 1) b.push_back(0);
        chnl = chunk("CHNL", b);
    }
    std::vector<uint8_t> cmp;

    std::vector<uint8_t> propFmt;
    wrTag(propFmt, "SND ");
    const auto fsChunk = chunk("FS  ", fs);
    propFmt.insert(propFmt.end(), fsChunk.begin(), fsChunk.end());
    propFmt.insert(propFmt.end(), chnl.begin(), chnl.end());

    // CMPR：类型 "DSD " + 名称 "not compressed"（补空到偶数）
    std::vector<uint8_t> cmprBody = { 'D', 'S', 'D', ' ' };
    const char* name = "not compressed";
    for (size_t i = 0; name[i]; ++i) cmprBody.push_back((uint8_t)name[i]);
    if (cmprBody.size() & 1) cmprBody.push_back(0);
    cmp = chunk("CMPR", cmprBody);
    propFmt.insert(propFmt.end(), cmp.begin(), cmp.end());

    const auto propChunk = chunk("PROP", propFmt);
    std::vector<uint8_t> dsd;
    dsd.reserve((size_t)(bytesPerCh * (uint64_t)s.channels));
    for (int c = 0; c < s.channels; ++c)
        dsd.insert(dsd.end(), s.data[(size_t)c].begin(), s.data[(size_t)c].begin() + (long long)bytesPerCh);
    const auto dsdChunk = chunk("DSD ", dsd);

    std::vector<uint8_t> out;
    wrTag(out, "FRM8");
    const uint64_t bodySize = 4 + fver.size() + 12 + propChunk.size() + dsdChunk.size();
    wrb64(out, bodySize);
    wrTag(out, "DSD ");
    const auto fverChunk = chunk("FVER", fver);
    out.insert(out.end(), fverChunk.begin(), fverChunk.end());
    out.insert(out.end(), propChunk.begin(), propChunk.end());
    out.insert(out.end(), dsdChunk.begin(), dsdChunk.end());
    write_file_bytes(path, out);
}

} // namespace rta::audio
