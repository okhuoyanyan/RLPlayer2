// zipstore.h — 最小 ZIP 打包（只做 STORE，不压缩；纯 C++，零外部依赖）
//
// 为什么自己写（2026-10-22 功能3 的选型理由，必须写清楚）：
//   * 用户要「一个压缩包」，不能继续让他单独去翻 diag.json / playback.log / launcher.log。
//   * 候选方案一：调 PowerShell 的 Compress-Archive。
//     ✗ 否决。这是【分发给用户的程序】，用户机器上不保证有 pwsh / powershell 可用
//       （精简系统、Server Core、组策略禁用、或 PATH 被裁掉都可能没有）。
//       把功能建立在"用户机器恰好有某个 shell"上，等于给自己埋一个只在别人机器上炸的雷。
//   * 候选方案二：引入 miniz / libzip / zlib。
//     ✗ 否决。项目至今零压缩库依赖（verify 的 check_compat 也在守"只导入系统 DLL"），
//       为一个诊断包引入新三方库不划算，还要过静态 CRT/兼容门禁。
//   * 候选方案三（选它）：STORE（不压缩）ZIP。
//     ✓ 格式简单到可以逐字节写出来：本地文件头 + 数据 + 中央目录 + EOCD，
//       每个文件带 CRC32。Windows 资源管理器、7-Zip、tar 都能正常解开（实测见分析文档）。
//     ✓ 诊断包本来就是几个 KB~几 MB 的文本，不压缩的代价可以忽略。
//     ✓ 只依赖 <cstdio>/<cstdint>/<string>/<vector>，与 DSP 完全隔离。
//
// 判据（怎么证明它对）：
//   * 结构自检 zif_selftest()：把内存里的 ZIP 反解一遍，核对每个条目的
//     名字/长度/CRC32 与写入时一致 —— 不依赖任何外部解压器就能进 ctest。
//   * 现实验证：Expand-Archive 必须能解开（见分析文档里的命令行输出）。
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace rta::zip {

// ---- CRC32（IEEE 802.3 多项式，ZIP 用的就是这个）----
inline uint32_t crc32_update(uint32_t crc, const void* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    const unsigned char* p = (const unsigned char*)data;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct Entry {
    std::string name;                 // ZIP 内路径（用 '/'）
    std::string data;
    uint32_t crc = 0;
    uint32_t offset = 0;              // 本地头在文件里的偏移
};

namespace detail {

inline void put16(std::vector<unsigned char>& v, uint16_t x) {
    v.push_back((unsigned char)(x & 0xFF));
    v.push_back((unsigned char)((x >> 8) & 0xFF));
}
inline void put32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back((unsigned char)(x & 0xFF));
    v.push_back((unsigned char)((x >> 8) & 0xFF));
    v.push_back((unsigned char)((x >> 16) & 0xFF));
    v.push_back((unsigned char)((x >> 24) & 0xFF));
}

// DOS 时间/日期（ZIP 头里必须写；用固定值也能被所有解压器接受，
// 但写真实时间更便于用户对照"这是什么时候导出的"）
inline void dos_time_date(uint16_t& dosTime, uint16_t& dosDate) {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    lt = *std::localtime(&t);
#endif
    if (lt.tm_year < 80) { dosTime = 0; dosDate = 0x21; return; }   // 1980-01-01
    dosTime = (uint16_t)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
    dosDate = (uint16_t)(((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
}

}  // namespace detail

// 打包：返回完整 ZIP 字节流。entry 名一律用 '/' 分隔（Windows 资源管理器也认）。
inline std::string build(const std::vector<Entry>& entries) {
    std::vector<unsigned char> out;
    out.reserve(4096);
    std::vector<Entry> done;
    done.reserve(entries.size());

    uint16_t dosTime = 0, dosDate = 0;
    detail::dos_time_date(dosTime, dosDate);

    for (const Entry& e : entries) {
        Entry d = e;
        d.crc = crc32_update(0, e.data.data(), e.data.size());
        d.offset = (uint32_t)out.size();
        // ---- 本地文件头 ----
        detail::put32(out, 0x04034B50u);      // signature "PK\3\4"
        detail::put16(out, 20);               // version needed (2.0 = store 足够)
        detail::put16(out, 0x0800);           // flags: bit11 = 文件名是 UTF-8
        detail::put16(out, 0);                // method 0 = store
        detail::put16(out, dosTime);
        detail::put16(out, dosDate);
        detail::put32(out, d.crc);
        detail::put32(out, (uint32_t)d.data.size());   // compressed size == raw (store)
        detail::put32(out, (uint32_t)d.data.size());
        detail::put16(out, (uint16_t)d.name.size());
        detail::put16(out, 0);                // extra len
        out.insert(out.end(), d.name.begin(), d.name.end());
        out.insert(out.end(), d.data.begin(), d.data.end());
        done.push_back(d);
    }

    const uint32_t cdStart = (uint32_t)out.size();
    for (const Entry& d : done) {
        detail::put32(out, 0x02014B50u);      // "PK\1\2"
        detail::put16(out, 20);               // version made by
        detail::put16(out, 20);               // version needed
        detail::put16(out, 0x0800);           // UTF-8 文件名
        detail::put16(out, 0);
        detail::put16(out, dosTime);
        detail::put16(out, dosDate);
        detail::put32(out, d.crc);
        detail::put32(out, (uint32_t)d.data.size());
        detail::put32(out, (uint32_t)d.data.size());
        detail::put16(out, (uint16_t)d.name.size());
        detail::put16(out, 0);                // extra
        detail::put16(out, 0);                // comment
        detail::put16(out, 0);                // disk number
        detail::put16(out, 0);                // internal attrs
        detail::put32(out, 0);                // external attrs
        detail::put32(out, d.offset);
        out.insert(out.end(), d.name.begin(), d.name.end());
    }
    const uint32_t cdSize = (uint32_t)out.size() - cdStart;

    // ---- EOCD ----
    detail::put32(out, 0x06054B50u);          // "PK\5\6"
    detail::put16(out, 0);                    // disk
    detail::put16(out, 0);                    // cd start disk
    detail::put16(out, (uint16_t)done.size());
    detail::put16(out, (uint16_t)done.size());
    detail::put32(out, cdSize);
    detail::put32(out, cdStart);
    detail::put16(out, 0);                    // comment len

    return std::string((const char*)out.data(), out.size());
}

// 自检用：从 ZIP 字节流里反解条目清单（只认我们自己写出去的 STORE 形态）。
// 返回 false 表示结构不对 —— 这是不依赖外部解压器的判据（ctest 用它）。
struct Parsed {
    std::string name;
    uint32_t crc = 0;
    uint32_t size = 0;
    std::string data;
};
inline bool parse(const std::string& zip, std::vector<Parsed>& out, std::string& err) {
    out.clear();
    const unsigned char* b = (const unsigned char*)zip.data();
    const size_t n = zip.size();
    if (n < 22) { err = "太短"; return false; }
    // 找 EOCD（从尾部往前找签名）
    size_t eocd = std::string::npos;
    for (size_t i = n >= 22 ? n - 22 + 1 : 0; i-- > 0;) {
        if (b[i] == 0x50 && b[i + 1] == 0x4B && b[i + 2] == 0x05 && b[i + 3] == 0x06) {
            eocd = i; break;
        }
    }
    if (eocd == std::string::npos) { err = "找不到 EOCD"; return false; }
    auto rd16 = [&](size_t p) -> uint16_t { return (uint16_t)(b[p] | (b[p + 1] << 8)); };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (uint32_t)b[p] | ((uint32_t)b[p + 1] << 8) | ((uint32_t)b[p + 2] << 16) |
               ((uint32_t)b[p + 3] << 24);
    };
    const uint16_t count = rd16(eocd + 10);
    const uint32_t cdSize = rd32(eocd + 12);
    const uint32_t cdStart = rd32(eocd + 16);
    if ((size_t)cdStart + cdSize > n) { err = "中央目录越界"; return false; }
    size_t p = cdStart;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > n || rd32(p) != 0x02014B50u) { err = "中央目录头签名错"; return false; }
        const uint32_t crc = rd32(p + 16);
        const uint32_t csize = rd32(p + 20);
        const uint32_t usize = rd32(p + 24);
        const uint16_t nlen = rd16(p + 28);
        const uint16_t elen = rd16(p + 30);
        const uint16_t clen = rd16(p + 32);
        const uint32_t lho = rd32(p + 42);
        if (p + 46 + nlen > n) { err = "中央目录名越界"; return false; }
        Parsed e;
        e.name.assign((const char*)b + p + 46, nlen);
        e.crc = crc;
        e.size = usize;
        if (csize != usize) { err = "非 STORE 条目"; return false; }
        // 读本地头拿数据
        if ((size_t)lho + 30 > n || rd32(lho) != 0x04034B50u) { err = "本地头签名错"; return false; }
        const uint16_t lnlen = rd16(lho + 26);
        const uint16_t lelen = rd16(lho + 28);
        const size_t dataAt = (size_t)lho + 30 + lnlen + lelen;
        if (dataAt + usize > n) { err = "数据越界"; return false; }
        e.data.assign((const char*)b + dataAt, usize);
        if (crc32_update(0, e.data.data(), e.data.size()) != crc) { err = "CRC 不符: " + e.name; return false; }
        out.push_back(std::move(e));
        p += 46 + nlen + elen + clen;
    }
    return true;
}

}  // namespace rta::zip
