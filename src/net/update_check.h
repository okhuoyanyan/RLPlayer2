// ============================================================================
// ⚠️ 本文件移植自 BH2VOQ/RLPlayer-Mac 的 src/net/update_check.h（GPL-3.0-or-later），
//    2026-10-27 由 Windows 侧引入（用户 261027-02 #5：「MAC 那边的开发者已经发布代码，
//    你可以看看有什么可以合并进入主线的」）。纯逻辑层，平台无关，原样保留 167 条离线门禁的语义。
//
//    Windows 侧改动点（只有这三处，其余逐字未动）：
//     ① kDefaultManifestUrl：latest.json → latest-win.json（Windows 清单与 macOS 清单分开）
//     ② kRepoOwner/kRepoName/kRepoBranch：指向**清单所在**的公开仓库
//        （macOS 的清单仓同时承载 Windows 清单；我们主仓 okhuoyanyan/RLPlayer 是私有的，
//          匿名客户端读不到 raw，所以清单必须放在公开仓库，见 docs 里的说明）
//     ③ kManifestAssetName / kReleasesApiUrl 同步改成 Windows 侧的名字
//    增量更新（files[] / delta）的解析与计划在 src/net/update.h（本文件保持 MAC 的
//    "单文件全量清单"语义不变，避免破坏它原有的测试真值表）。
// ============================================================================// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 August Hu <BH2VOQ@163.com>
//
// 本文件是 RLPlayer 的一部分，以 GNU 通用公共许可证第 3 版（或更新版本）发布。
// 完整条款见仓库根目录的 LICENSE 文件。
//
// update_check.h — F-4 自动更新：**纯逻辑层**（无窗口、无 ObjC、可离线单测）
//
// ============================ 这一层为什么单独存在 ============================
// 「云端发版 → 提示 → 一键更新」里真正会出错、也必须被证明的部分**全是纯函数**：
//   ① 版本比较（1.0.5-beta.1 < 1.0.5，且 1.0.10 > 1.0.9 —— 不许退化成正比字符串比较）
//   ② sha256 校验（下载被截断 / 被投毒必须能判定，而且判据要用**官方标准向量**证明，
//      不能"自己算两遍一样就算对" —— 铁律 3）
//   ③ 清单解析（字段缺失/类型不对/版本号非法都要判非法，不是"读不到就当最新"）
//   ④ 下载 URL 白名单（清单被篡改成任意地址 → 必须拒绝）
//   ⑤ 失败分支（网络失败 / 鉴权失败 / 解析失败 **绝不**落到「已是最新」）
// 把它们放进原生壳的 ObjC++ 里就只能靠肉眼审查；抽到这里可以被 tests/test_update_check.cpp
// 逐条断言。真正的 IO（下载/挂载/替换 .app）仍在 tools/mac_host.mm。
//
// ============================ 安全边界（用户硬要求）============================
//   * 本文件**不含任何 token 字面量**，也不做鉴权 —— token 只从 .app 外的
//     update.conf 读进原生壳，作为 HTTP 头一次性使用，绝不进日志/错误信息/UI 文案。
//   * 下载 URL 必须 https 且主机**精确等于**白名单之一（见
//     update_download_url_allowed：拒绝 http、拒绝别的域、拒绝 user@host、拒绝端口变体）。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "net/json.h"

namespace rta::update {

// ---------------------------------------------------------------------------
// 0. 常量（唯一真相源）
// ---------------------------------------------------------------------------
// 2026-10-26（用户决定）：**通道从 Gitea 整体迁到 GitHub**，仓库 BH2VOQ/RLPlayer-Mac（公开）。
//   迁移原因：Gitea 私有仓库的 release 附件对 token 全线 403（见下方历史注释），
//   且匿名建 issue 不可用；GitHub 公开仓库的三条通道都能走通。
//
// 三条通道（本文件只用第 1 条；2/3 见 diag_feedback.h）：
//   ① 更新清单  raw.githubusercontent.com 匿名读（实测 200，**客户端零 token**）
//   ② 诊断上传  需 token（匿名建 issue 实测 401）
//   ③ 用户反馈  需 token（同上）
//
// 默认「公开更新清单」地址：**匿名可读**，客户端不需要任何 token。
//   ⚠️ 用 raw.githubusercontent.com 而不是 api.github.com：
//      · 走 raw 不需要 token，也不吃 API 速率限制（匿名 API 只有 60 次/小时/IP）；
//      · 实测 200 + 正确 JSON。
inline constexpr const char kDefaultManifestUrl[] =
    "https://raw.githubusercontent.com/BH2VOQ/RLPlayer-Mac/main/latest-win.json";

// 下载与取清单**唯一允许**的主机（精确匹配，不含端口）。
// 更新这条时**必须同步** update_download_url_allowed() 里的注释与测试用例：
// 白名单是防止"清单被篡改后变成任意地址下载"的最后一道闸门。
inline constexpr const char kAllowedHost[] = "raw.githubusercontent.com";

// 允许的**第二**个下载主机：release 附件真正的下载域名。
//   dmg 体积大，放 release assets 比放仓库文件合适（仓库文件每次改动都会进 git 历史）。
//   GitHub 的 release 下载链接 302 到 objects.githubusercontent.com，
//   但**清单里写的是** github.com/...（重定向由 curl/NSURLSession 自己跟随），
//   所以白名单只需要放行 github.com。
inline constexpr const char kAllowedDownloadHost[] = "github.com";

// 仓库标识（拼 API 路径用）
inline constexpr const char kRepoOwner[] = "BH2VOQ";
inline constexpr const char kRepoName[] = "RLPlayer-Mac";
inline constexpr const char kRepoBranch[] = "main";

// releases API（诊断/反馈上传走 issue，不走这里；保留给"从 release 读清单"的备选路径）
inline constexpr const char kReleasesApiUrl[] =
    "https://api.github.com/repos/BH2VOQ/RLPlayer-Mac/releases";

// 说明：旧版有一个 kPrivateRawPrefix = "https://api.github.com/repos/.../contents/"，
// 用于拼相对路径下载地址。**已删除** —— api.github.com 不在白名单里（白名单只放行
// raw.githubusercontent.com 与 github.com），用它拼出来的地址必然被拒。
// 相对路径现在按 raw.githubusercontent.com/<owner>/<repo>/<ref>/<path> 拼（见上）。

// 清单文件名（作为 release 附件时也用这个名字）。
inline constexpr const char kManifestAssetName[] = "latest-win.json";

// 私有路径下，清单与 dmg 所在的**仓库分支**（raw 端点的 ref 参数）。
// 用发布分支固定，不跟默认分支漂移（默认分支随时可能改名/推进）。
inline constexpr const char kPrivateManifestRef[] = "feat/macos-platform";

// ---------------------------------------------------------------------------
// 1. 版本比较（含 -beta / -rc 预发布语义）
//
// 语义对齐 SemVer 2.0.0 的**比较**部分（不涉及范围表达式）：
//   * 数字段逐段比较，缺段补 0（1.0 == 1.0.0）
//   * 有预发布标记 < 无预发布标记      ⟹ 1.0.5-beta.1 < 1.0.5
//   * 同为预发布：逐标识符比较
//       - 两边都是纯数字 → 按数值比（beta.2 > beta.10 吗？不！2 < 10，按数值）
//       - 一个是数字一个是字母 → 数字 < 字母（SemVer 规则）
//       - 都含字母 → 字典序（byte-wise，比 strcmp 稳定）
//       - 前缀相同时，标识符少的更低（1.0.5-beta < 1.0.5-beta.1）
//   * 容忍前导 'v'/'V'（Gitea tag 是 "v1.0.4"）
//   * 容忍 '+' 之后的构建元数据（比较时忽略）
//   * 非法输入**不崩**，返回 valid=false（调用方按"解析失败"，绝不能当成"更新"或"最新"）
// ---------------------------------------------------------------------------
struct Version {
    bool valid = false;
    std::vector<long long> nums;      // [1,0,5]
    std::vector<std::string> pre;     // ["beta","1"]；空 = 正式版
    std::string raw;                  // 原始串（回显用，不含敏感信息）

    bool has_pre() const { return !pre.empty(); }
};

namespace detail {


inline bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

// 去掉首尾空白（不修改入参）
inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

inline std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return o;
}

// 数字段：只接受十进制数字，溢出（> 9 位）判非法 —— 真实版本号不会这么大，
// 而 atoll 溢出会静默回绕成"比谁都大/小"，那是最危险的假判据。
inline bool parse_num_segment(const std::string& s, long long& out) {
    if (!all_digits(s) || s.size() > 9) return false;
    out = std::atoll(s.c_str());
    return true;
}

} // namespace detail

inline Version parse_version(const std::string& in) {
    Version v;
    v.raw = in;
    std::string s = detail::trim(in);
    if (s.empty()) return v;
    if (s[0] == 'v' || s[0] == 'V') s.erase(s.begin());      // v1.0.4 → 1.0.4
    if (s.empty()) return v;

    // 丢掉构建元数据（SemVer: '+' 之后不参与比较）
    const size_t plus = s.find('+');
    if (plus != std::string::npos) s = s.substr(0, plus);

    // 拆出预发布段
    std::string core = s;
    const size_t dash = s.find('-');
    if (dash != std::string::npos) {
        core = s.substr(0, dash);
        const std::string preStr = s.substr(dash + 1);
        if (preStr.empty()) return v;                        // "1.0.5-" 非法
        size_t pos = 0;
        while (pos <= preStr.size()) {
            const size_t dot = preStr.find('.', pos);
            const std::string tok = (dot == std::string::npos) ? preStr.substr(pos)
                                                              : preStr.substr(pos, dot - pos);
            if (tok.empty()) return v;                        // "1.0.5-beta." / "-beta..1" 非法
            for (unsigned char c : tok) {
                if (!std::isalnum(c) && c != '-') return v;    // 标识符只允许字母数字与连字符
            }
            v.pre.push_back(tok);
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
    }

    // 拆数字段（至少 1 段，至多 4 段：1 / 1.2 / 1.2.3 / 1.2.3.4）
    if (core.empty()) return v;
    size_t pos = 0;
    while (pos <= core.size()) {
        const size_t dot = core.find('.', pos);
        const std::string tok = (dot == std::string::npos) ? core.substr(pos) : core.substr(pos, dot - pos);
        long long n = 0;
        if (!detail::parse_num_segment(tok, n)) return v;
        v.nums.push_back(n);
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    if (v.nums.empty() || v.nums.size() > 4) return v;
    v.valid = true;
    return v;
}

enum class VersionCmp { Less = -1, Equal = 0, Greater = 1, Invalid = 2 };

inline VersionCmp compare_versions(const Version& a, const Version& b) {
    if (!a.valid || !b.valid) return VersionCmp::Invalid;

    // ---- 数字段：缺段补 0 ----
    const size_t n = std::max(a.nums.size(), b.nums.size());
    for (size_t i = 0; i < n; ++i) {
        const long long x = (i < a.nums.size()) ? a.nums[i] : 0;
        const long long y = (i < b.nums.size()) ? b.nums[i] : 0;
        if (x < y) return VersionCmp::Less;
        if (x > y) return VersionCmp::Greater;
    }

    // ---- 预发布：无标记的更高（1.0.5 > 1.0.5-beta.1）----
    if (a.pre.empty() && b.pre.empty()) return VersionCmp::Equal;
    if (a.pre.empty()) return VersionCmp::Greater;
    if (b.pre.empty()) return VersionCmp::Less;

    const size_t m = std::min(a.pre.size(), b.pre.size());
    for (size_t i = 0; i < m; ++i) {
        const std::string& x = a.pre[i];
        const std::string& y = b.pre[i];
        const bool xd = detail::all_digits(x);
        const bool yd = detail::all_digits(y);
        if (xd && yd) {
            // 按数值比（两位都 <= 9 位，atoll 安全；超长时退回按长度+字典序）
            const std::string xs = x;
            const std::string ys = y;
            if (xs.size() != ys.size() && xs.size() <= 9 && ys.size() <= 9) {
                const long long xn = std::atoll(xs.c_str());
                const long long yn = std::atoll(ys.c_str());
                if (xn < yn) return VersionCmp::Less;
                if (xn > yn) return VersionCmp::Greater;
            } else {
                if (xs.size() != ys.size()) return xs.size() < ys.size() ? VersionCmp::Less : VersionCmp::Greater;
                const int c = xs.compare(ys);
                if (c < 0) return VersionCmp::Less;
                if (c > 0) return VersionCmp::Greater;
            }
        } else if (xd != yd) {
            return xd ? VersionCmp::Less : VersionCmp::Greater;   // 数字 < 字母
        } else {
            const int c = x.compare(y);
            if (c < 0) return VersionCmp::Less;
            if (c > 0) return VersionCmp::Greater;
        }
    }
    if (a.pre.size() == b.pre.size()) return VersionCmp::Equal;
    return a.pre.size() < b.pre.size() ? VersionCmp::Less : VersionCmp::Greater;
}

// 便捷判定：remote 是否比 local 新。**任一非法都返回 false**（解析失败不是"有更新"）。
inline bool is_newer(const std::string& remote, const std::string& local) {
    return compare_versions(parse_version(remote), parse_version(local)) == VersionCmp::Greater;
}

// ---------------------------------------------------------------------------
// 2. SHA-256（自己实现，零外部依赖）
//
// 为什么不用系统库：CommonCrypto 需要额外 framework，OpenSSL 不在系统里，
// libcurl 内部虽有实现但不对外导出。自己写 ~60 行反而最省事、最可控。
// 判据不看"自洽"，只看 **FIPS 180-4 / NIST 官方标准向量**（tests 里有 4 组）。
// ---------------------------------------------------------------------------
namespace detail_sha256 {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void block(uint32_t h[8], const uint8_t p[64]) {
    static const uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

inline std::string hex_of(const uint32_t h[8]) {
    static const char* hx = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int j = 3; j >= 0; --j) {
            const uint8_t b = (uint8_t)((h[i] >> (8 * j)) & 0xFF);
            out.push_back(hx[b >> 4]);
            out.push_back(hx[b & 15]);
        }
    }
    return out;
}

inline void init(uint32_t h[8]) {
    h[0] = 0x6a09e667u; h[1] = 0xbb67ae85u; h[2] = 0x3c6ef372u; h[3] = 0xa54ff53au;
    h[4] = 0x510e527fu; h[5] = 0x9b05688cu; h[6] = 0x1f83d9abu; h[7] = 0x5be0cd19u;
}

} // namespace detail_sha256

inline std::string sha256_hex(const void* data, size_t len) {
    uint32_t h[8];
    detail_sha256::init(h);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = len;
    while (left >= 64) {
        detail_sha256::block(h, p);
        p += 64;
        left -= 64;
    }
    // 尾部：补齐 + 0x80 + 64 位大端长度
    uint8_t tail[128] = {0};
    if (left) std::memcpy(tail, p, left);
    tail[left] = 0x80;
    const size_t tailLen = (left < 56) ? 64 : 128;
    const uint64_t bits = (uint64_t)len * 8ULL;
    for (int i = 0; i < 8; ++i) {
        tail[tailLen - 1 - i] = (uint8_t)((bits >> (8 * i)) & 0xFF);   // 大端
    }
    detail_sha256::block(h, tail);
    if (tailLen == 128) detail_sha256::block(h, tail + 64);
    return detail_sha256::hex_of(h);
}

inline std::string sha256_hex(const std::string& s) { return sha256_hex(s.data(), s.size()); }

// 流式摘要一个文件（下载产物 7 MB 级，一次读完最简单也够快；
// 判据见 tests：写文件后用官方向量核对，证明"读文件这条路"本身没错）。
inline bool sha256_file(const std::string& path, std::string& outHex, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "无法打开文件"; return false; }
    std::string data;
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz < 0) { err = "无法读取文件大小"; return false; }
    f.seekg(0, std::ios::beg);
    if (sz > 0) {
        data.resize((size_t)sz);
        f.read(&data[0], sz);
        if (!f) { err = "读取文件失败"; return false; }
        data.resize((size_t)f.gcount());
    }
    outHex = sha256_hex(data.data(), data.size());
    return true;
}

// 统一的大小写不敏感 hex 比较：**长度不等直接 false**（截断必须判失败）。
inline bool hex_equals_ci(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

inline bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (unsigned char c : s) if (!std::isxdigit(c)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// 3. 下载 URL 白名单（防「清单被篡改 → 变成任意地址下载」）
//
// 规则（全部满足才放行）：
//   ① 必须以 "https://" 开头（大小写不敏感）—— http / file / data 一律拒绝
//   ② host 部分（到第一个 '/' 为止）必须**精确等于** kAllowedHost 或 kAllowedDownloadHost
//       ⟹ 天然拒绝 github.com.evil.com / evilgithub.com / github.com:8080 / user@github.com
//   ③ 必须有非空路径，且不得含空白与换行（防注入/请求拆分）
//   ④ 长度上限 4096
// ---------------------------------------------------------------------------
inline bool update_download_url_allowed(const std::string& url) {
    if (url.empty() || url.size() > 4096) return false;
    for (char c : url) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') return false;
    }
    static const char kPrefix[] = "https://";
    const size_t np = sizeof(kPrefix) - 1;
    if (url.size() <= np) return false;
    for (size_t i = 0; i < np; ++i) {
        const char a = (char)std::tolower((unsigned char)url[i]);
        if (a != kPrefix[i]) return false;                   // ← 只认 https（①）
    }
    const std::string rest = url.substr(np);
    const size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0) return false;      // 必须有主机名 + 路径
    const std::string host = detail::lower(rest.substr(0, slash));
    // ← 精确主机（②）。两个主机都要放行，但**都在编译期写死**，不接受运行时来源：
    //   · raw.githubusercontent.com = 清单文件（匿名读，不吃 API 速率限制）
    //   · github.com               = release 附件下载（dmg 走这里，体积大不进 git 历史）
    // 注意：GitHub 的 release 下载会 302 到 objects.githubusercontent.com，
    // 但**清单里写的是 github.com**，重定向由 NSURLSession 自己跟随，
    // 所以白名单只需要放行起始地址，不必把 objects.* 也加进来。
    if (host != kAllowedHost && host != kAllowedDownloadHost) return false;
    if (slash + 1 >= rest.size()) return false;                      // ③ 路径非空
    return true;
}

// 把清单里的相对路径拼成 raw 绝对 URL，并过白名单。
// 清单里给的若是 "path/to/RLPlayer.dmg"（相对），就走这里；给绝对 https URL 则原样用。
//
// 2026-10-26 迁到 GitHub 时的行为变化（实测过）：
//   GitHub 的 raw **相对路径**要用 raw.githubusercontent.com/<owner>/<repo>/<ref>/<path>，
//   ref 是**路径段**而不是查询参数 —— 这一点和 Gitea 的 ?ref= 不同。
//   旧实现拼的是 api.github.com/.../contents/<path>?ref=...，那个域名不在白名单里，
//   结果就是相对路径永远返回空串（本次测试 3 条红正是这个原因，不是断言写错了）。
inline std::string resolve_private_download_url(const std::string& manifestValue,
                                               const std::string& ref) {
    if (manifestValue.empty()) return "";
    // 已经是 https:// 绝对地址：直接按白名单校验
    if (manifestValue.size() > 8 && manifestValue.compare(0, 8, "https://") == 0) {
        return update_download_url_allowed(manifestValue) ? manifestValue : std::string();
    }
    // 相对路径：去掉前导 '/'，拼成 raw.githubusercontent.com/<owner>/<repo>/<ref>/<path>
    std::string rel = manifestValue;
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    if (rel.empty()) return "";
    std::string url = "https://raw.githubusercontent.com/";
    url += kRepoOwner;
    url += '/';
    url += kRepoName;
    url += '/';
    url += ref.empty() ? kRepoBranch : ref;
    url += '/';
    url += rel;
    return update_download_url_allowed(url) ? url : std::string();
}


// ---------------------------------------------------------------------------
// 4. 清单（latest.json）
//
// 格式（唯一格式；私有仓库把它作为 release 附件 `latest.json` 上传，公开仓库直接当文件放）：
//   {
//     "version":      "1.0.5",                    // 必填，允许 "v" 前缀
//     "download_url": "https://github.com/.../RLPlayer-1.0.5.dmg",  // 必填，过白名单
//     "sha256":       "<64 位 hex>",               // 必填，dmg 的 sha256
//     "size":         7139420,                     // 选填；>0 时下载后必须一致
//     "notes":        "更新说明（纯文本）"           // 选填
//   }
// 任何必填项缺失/类型不对/格式非法 ⟹ valid=false（调用方判「解析失败」，绝不判「最新」）。
// ---------------------------------------------------------------------------
// 安全相对路径：只允许字母数字与 . _ - /，且不得含 ".."、反斜杠、协议头。
// 用途：私有仓库路径下清单给的是**仓库内相对路径**（如 dist/RLPlayer-1.0.5.dmg），
// 由 resolve_private_download_url 拼到 raw 前缀上。这里必须先挡掉路径穿越。
inline bool is_safe_repo_relative_path(const std::string& p) {
    if (p.empty() || p.size() > 1024) return false;
    if (p.find("://") != std::string::npos) return false;
    if (p.find("..") != std::string::npos) return false;
    if (p.find('\\') != std::string::npos) return false;
    for (unsigned char c : p) {
        if (std::isalnum(c)) continue;
        if (c == '.' || c == '_' || c == '-' || c == '/') continue;
        return false;
    }
    return true;
}

struct Manifest {
    bool valid = false;
    std::string error;          // 失败原因（**不含**任何凭据）
    std::string version;
    std::string downloadUrl;
    bool downloadUrlIsRelative = false;   // true = 仓库内相对路径（私有路径走 raw 端点）
    std::string sha256;         // 统一转小写
    std::string notes;
    long long size = 0;         // 0 = 清单没声明，不校验
};

inline Manifest parse_update_manifest(const std::string& json) {
    Manifest m;
    const rta::net::Json j = rta::net::json_parse(json);
    if (j.type != rta::net::Json::Obj) { m.error = "清单不是 JSON 对象"; return m; }

    const std::string ver = j["version"].s();
    if (ver.empty()) { m.error = "清单缺少 version"; return m; }
    if (!parse_version(ver).valid) { m.error = "清单 version 非法"; return m; }
    m.version = ver;

    const std::string url = j["download_url"].s();
    if (url.empty()) { m.error = "清单缺少 download_url"; return m; }
    // 两种合法形态：
    //   (a) 绝对 URL —— 必须过白名单（公开仓库路径）
    //   (b) 仓库内相对路径 —— 私有仓库路径，拼到 raw 前缀后再过白名单
    if (update_download_url_allowed(url)) {
        m.downloadUrl = url;
        m.downloadUrlIsRelative = false;
    } else if (is_safe_repo_relative_path(url)) {
        m.downloadUrl = url;
        m.downloadUrlIsRelative = true;
    } else {
        m.error = "清单 download_url 不在白名单（必须是 https://raw.githubusercontent.com/ 或 https://github.com/，或安全的仓库内相对路径）";
        return m;
    }

    const std::string sha = detail::lower(j["sha256"].s());
    if (!is_hex64(sha)) { m.error = "清单 sha256 缺失或不是 64 位 hex"; return m; }
    m.sha256 = sha;

    if (j.has("size") && j["size"].type == rta::net::Json::Num) {
        const double sz = j["size"].n();
        if (sz > 0) m.size = (long long)sz;
    }
    m.notes = j["notes"].s();
    m.valid = true;
    return m;
}

// ---------------------------------------------------------------------------
// 5. 结果语义（五种失败必须彼此独立，且**都不等于**「已是最新」）
// ---------------------------------------------------------------------------
enum class Decision {
    UpdateAvailable,   // 远端更新
    UpToDate,          // 确实检查成功且不更新 —— 只有这一条能写「已是最新」
    FailedNetwork,     // 传输层失败（DNS/TLS/超时/连不上），status==0
    FailedAuth,        // 401/403（token 缺失/无效/权限不对）
    FailedHttp,        // 其它非 2xx（404 = 清单地址不对/仓库还没建）
    FailedParse,       // 清单 JSON/字段/版本号/sha256 格式非法
    FailedBadUrl       // 清单给的下载地址不在白名单
};

inline const char* decision_code(Decision d) {
    switch (d) {
        case Decision::UpdateAvailable: return "update-available";
        case Decision::UpToDate:        return "up-to-date";
        case Decision::FailedNetwork:   return "network-error";
        case Decision::FailedAuth:      return "auth-failed";
        case Decision::FailedHttp:      return "http-error";
        case Decision::FailedParse:     return "parse-error";
        case Decision::FailedBadUrl:    return "bad-url";
    }
    return "unknown";
}

struct CheckResult {
    Decision decision = Decision::FailedNetwork;
    std::string remoteVersion;
    std::string downloadUrl;
    std::string sha256;
    std::string notes;
    long long   size = 0;
    std::string error;                    // 人类可读（**不含凭据**）
    int         status = 0;

    const char* code() const { return decision_code(decision); }
    bool up_to_date() const { return decision == Decision::UpToDate; }
    bool available()  const { return decision == Decision::UpdateAvailable; }
    // 「有没有可能真的最新」——只有 UpToDate 成立。UI 侧只允许对它显示「已是最新」。
    bool ok() const { return decision == Decision::UpToDate || decision == Decision::UpdateAvailable; }
};

// 把「HTTP 传输结果 + 本地版本」映射成决策。**这是整个检查流程的唯一判定入口**，
// 原生壳只负责把 http 结果塞进来，不自己写 if —— 保证判定可离线测。
inline CheckResult evaluate_update_check(const std::string& localVersion,
                                        bool httpOk, int httpStatus,
                                        const std::string& body) {
    CheckResult r;
    r.status = httpStatus;

    // ① 传输层失败（status==0 表示连 DNS/TLS/连接都没成）
    if (!httpOk && httpStatus == 0) {
        r.decision = Decision::FailedNetwork;
        r.error = "网络不可用（无法连接更新服务器）";
        return r;
    }
    // ② 鉴权（必须先于普通 http 错误判断：401/403 是"权限"不是"网络"）
    if (httpStatus == 401 || httpStatus == 403) {
        r.decision = Decision::FailedAuth;
        // ⚠️ 这里绝不能回显 token，也不回显它的长度/前缀。
        r.error = "鉴权失败（检查 update.conf 里的只读 token）";
        return r;
    }
    // ③ 其它 HTTP 错误（含 404：清单地址不对，或公开更新仓库还没建）
    if (!httpOk || httpStatus < 200 || httpStatus >= 300) {
        r.decision = Decision::FailedHttp;
        r.error = "更新服务器返回 HTTP " + std::to_string(httpStatus);
        return r;
    }
    // ④ 解析
    const Manifest m = parse_update_manifest(body);
    if (!m.valid) {
        r.decision = Decision::FailedParse;
        r.error = m.error;
        return r;
    }
    // 白名单在解析里已经查过；这里再判一次是为了让"清单合法但地址非法"有独立码。
    if (!update_download_url_allowed(m.downloadUrl)) {
        r.decision = Decision::FailedBadUrl;
        r.error = "清单下载地址不在白名单";
        return r;
    }
    r.remoteVersion = m.version;
    r.downloadUrl   = m.downloadUrl;
    r.sha256        = m.sha256;
    r.notes         = m.notes;
    r.size          = m.size;

    const Version remote = parse_version(m.version);
    const Version local  = parse_version(localVersion);
    if (!local.valid) {
        // 本地版本都解析不出来时，绝不能声称"已是最新"，也不能乱装：判解析失败。
        r.decision = Decision::FailedParse;
        r.error = "本地版本号无法解析";
        return r;
    }
    r.decision = (compare_versions(remote, local) == VersionCmp::Greater)
                     ? Decision::UpdateAvailable
                     : Decision::UpToDate;
    return r;
}

// ---------------------------------------------------------------------------
// 5b. 安装前门禁：verify_downloaded_file —— **篡改/截断必失败**
//
// 这是整条更新链上唯一决定"要不要覆盖 /Applications 里的 .app"的函数。
// 原生壳必须先拿到 ok==true，**才允许**挂载 dmg 与替换；ok==false 一律丢弃临时文件。
// 三种失败彼此独立（理由要能区分：是网络/IO 问题，还是文件被换过）：
//   * MissingExpectedSha —— 清单没给摘要（缺省即**拒绝**，不是缺省即放行）
//   * OpenFailed         —— 文件打不开/读失败（下载没落盘）
//   * SizeMismatch       —— 声明了 size 且不符（截断）
//   * Sha256Mismatch     —— 摘要不符（投毒/内容被换）
// ⚠️ 顺序上先比 size 再比 sha：size 是零成本前置，且能把"截断"与"投毒"分开报。
// ---------------------------------------------------------------------------
enum class VerifyFailReason { None, MissingExpectedSha, OpenFailed, SizeMismatch, Sha256Mismatch };

struct VerifyResult {
    bool ok = false;
    VerifyFailReason reason = VerifyFailReason::None;
    std::string error;        // 人类可读、**不含**任何凭据
    std::string actualSha;    // 实际摘要（便于报告，不是秘密）
};

inline VerifyResult verify_downloaded_file(const std::string& path,
                                           const std::string& expectedSha256,
                                           long long expectedSize) {
    VerifyResult r;
    // ① 期望摘要必须存在且是 64 位 hex —— 清单漏填就拒绝，绝不"没摘要也算过"
    if (!is_hex64(expectedSha256)) {
        r.reason = VerifyFailReason::MissingExpectedSha;
        r.error = "清单未提供合法的 sha256，拒绝安装";
        return r;
    }
    // ② 文件必须存在且可读
    {
        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            r.reason = VerifyFailReason::OpenFailed;
            r.error = "下载文件不存在或无法打开";
            return r;
        }
    }
    // ③ 大小（>0 才校验；0 表示清单没声明）
    if (expectedSize > 0) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            r.reason = VerifyFailReason::OpenFailed;
            r.error = "无法读取下载文件大小";
            return r;
        }
        const std::streamoff sz = f.tellg();
        if (sz < 0) {
            r.reason = VerifyFailReason::OpenFailed;
            r.error = "无法读取下载文件大小";
            return r;
        }
        if ((long long)sz != expectedSize) {
            r.reason = VerifyFailReason::SizeMismatch;
            r.error = "文件大小不符（期望 " + std::to_string(expectedSize) +
                      "，实际 " + std::to_string((long long)sz) + "）";
            return r;
        }
    }
    // ④ sha256
    std::string hex, err;
    if (!sha256_file(path, hex, err)) {
        r.reason = VerifyFailReason::OpenFailed;
        r.error = err.empty() ? "读取下载文件失败" : err;
        return r;
    }
    r.actualSha = hex;
    if (!hex_equals_ci(hex, expectedSha256)) {
        r.reason = VerifyFailReason::Sha256Mismatch;
        r.error = "sha256 校验失败（文件与清单不符，已丢弃）";
        return r;
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// 6. 私有仓库路径：从 Gitea releases API 响应里挑出最新 release 的 latest.json 附件
//
// 为什么不是直接用 API 里的 tag_name + 附件 URL：
//   Gitea 1.27 的 release asset 对象**不带 sha256**，而 sha256 是安装前的硬闸门。
//   所以约定：release 必须额外上传一个 `latest.json` 附件（内含 sha256），
//   客户端先问 API 拿到该附件的 URL，再取它 —— 两种渠道（公开/私有）最终都用同一份清单格式。
// ---------------------------------------------------------------------------
struct ReleaseAssetLookup {
    bool found = false;
    std::string url;        // 附件的绝对下载 URL（已过白名单）
    std::string tag;        // 用于日志/回显
    std::string error;
};

inline ReleaseAssetLookup find_manifest_asset_in_releases(const std::string& json) {
    ReleaseAssetLookup out;
    const rta::net::Json arr = rta::net::json_parse(json);
    if (arr.type != rta::net::Json::Arr) { out.error = "releases 响应不是数组"; return out; }

    const rta::net::Json* best = nullptr;
    Version bestVer;
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& rel = arr.at(i);
        if (rel.type != rta::net::Json::Obj) continue;
        if (rel["draft"].type == rta::net::Json::Bool && rel["draft"].b) continue;   // 草稿不算
        const Version v = parse_version(rel["tag_name"].s());
        if (!v.valid) continue;
        if (!best || compare_versions(v, bestVer) == VersionCmp::Greater) {
            best = &rel;
            bestVer = v;
        }
    }
    if (!best) { out.error = "没有可用的 release（全部是草稿或 tag 非法）"; return out; }

    const rta::net::Json& assets = (*best)["assets"];
    for (size_t i = 0; i < assets.size(); ++i) {
        const rta::net::Json& a = assets.at(i);
        const std::string name = detail::lower(a["name"].s());
        if (name != detail::lower(kManifestAssetName)) continue;
        const std::string url = a["browser_download_url"].s();
        if (!update_download_url_allowed(url)) {
            out.error = "latest.json 附件地址不在白名单";
            return out;
        }
        out.found = true;
        out.url = url;
        out.tag = (*best)["tag_name"].s();
        return out;
    }
    out.error = "该 release 没有 latest.json 附件";
    out.tag = (*best)["tag_name"].s();
    return out;
}

// ---------------------------------------------------------------------------
// 7. 配置文件（update.conf）解析 —— **缺省即安全**
//
//   * 文件不存在 / 没有 token ⟹ 匿名走公开清单（默认行为，不报错不弹窗）
//   * 有 token ⟹ 走私有 releases API 找 latest.json 附件
//   * token 只允许出现在这个 .app 外的文件里（0600），**绝不硬编码**，
//     绝不写日志 / 错误信息 / UI 文案（本结构体的 error 与回显字段都不含 token）
//
// 文件格式：`key=value`，'#' 开头为注释，键名固定 token / manifest_url。
// ---------------------------------------------------------------------------
struct Config {
    bool  fileExists = false;
    bool  hasToken = false;
    std::string token;          // ⚠️ 只在内存里活一次请求，用完即弃；绝不外传
    std::string manifestUrl = kDefaultManifestUrl;
    bool  manifestUrlValid = true;   // 给了但不在白名单 → false（拒绝，不静默回退）
    std::string warning;        // 配置层面的提示（不含 token）
};

inline Config parse_update_config(const std::string& text) {
    Config c;
    c.fileExists = true;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string line = detail::trim((nl == std::string::npos) ? text.substr(pos)
                                                                      : text.substr(pos, nl - pos));
        if (!line.empty() && line[0] != '#') {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                const std::string k = detail::lower(detail::trim(line.substr(0, eq)));
                const std::string v = detail::trim(line.substr(eq + 1));
                if (k == "token") {
                    // 只认"看起来像 token"的值；过长/含空白直接丢弃并给提示（不回显内容）
                    if (!v.empty() && v.size() <= 512) {
                        bool clean = true;
                        for (unsigned char ch : v) if (std::isspace(ch)) clean = false;
                        if (clean) { c.token = v; c.hasToken = true; }
                        else c.warning = "update.conf: token 含空白，已忽略";
                    }
                } else if (k == "manifest_url") {
                    if (v.empty()) {
                        c.manifestUrl = kDefaultManifestUrl;
                    } else if (update_download_url_allowed(v)) {
                        c.manifestUrl = v;
                    } else {
                        c.manifestUrlValid = false;
                        c.manifestUrl = v;
                        c.warning = "update.conf: manifest_url 不在白名单（必须 https://raw.githubusercontent.com/）";
                    }
                }
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return c;
}

// 首次运行时写出的模板（用户把自己的**只读** token 填进 token= 即可）。
// 注意：模板里 token= 后面**必须为空** —— 这是"缺省即安全"的落地形式。
inline std::string default_update_conf_template() {
    std::string s;
    s += "# RLPlayer 更新检查配置（权限 0600；此文件在 .app 之外，绝不打包进发布版）\n";
    s += "#\n";
    s += "# token：Gitea **只读** token（read:repository 范围即可）。\n";
    s += "#        留空 = 匿名拉公开清单（推荐，绝大多数用户就是这样）。\n";
    s += "#        填了 = 走私有 releases API，从 release 的 latest.json 附件取清单。\n";
    s += "#        ⚠️ 绝不要填有写权限的 token；本文件一旦泄露等于泄露该 token。\n";
    s += "token=\n";
    s += "#\n";
    s += "# manifest_url：公开更新清单地址（必须 https://raw.githubusercontent.com/ 前缀）\n";
    s += "manifest_url=";
    s += kDefaultManifestUrl;
    s += "\n";
    return s;
}

} // namespace rta::update
