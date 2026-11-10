// feedback.h — 一键反馈：诊断快照、预览、脱敏、HTTPS 提交与本地 outbox。
//
// 上传只有 Windows 反馈服务一条路径，不需要用户凭据。同步 submit/web_handler
// 供 CLI 与离线测试使用；web_submit_start/web_submit_status 供本地 HTTP 层异步调用。
// 本文件不采集音频、不打开设备；所有离线测试都应注入 Options::transport。
#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

#include "crash_dump.h"
#include "net/http_authority.h"
#include "net/json.h"
#include "platform/http.h"
#include "platform/path.h"
#include "platform/process.h"
#include "platform/sysinfo.h"
#include "platform/time.h"
#include "version.h"

namespace rta::feedback {

constexpr int kSchema = 1;
inline constexpr const char* kServerUrlDefault =
    "https://rlplayer.rainlain.com/api/v1/feedback/windows";
inline constexpr size_t kAsyncMaxJobs = 16;
inline constexpr long long kAsyncResultRetentionSeconds = 15 * 60;

struct UserReport {
    std::string type;
    std::string title;
    std::string body;
    std::string steps;
};

struct LiveState {
    std::string source = "feedback_probe";
    bool playing = false;
    bool paused  = false;
    std::string track;
    std::string lastError;
    std::string backend;
    std::string device;
    std::string outMode;
    long long   outRate = 0;
    int         outBits = 0;
    std::string gpu;
    std::string settingsJson;
    std::string endpointsJson;
    std::string extraJson;
};

struct PutResult {
    bool ok = false;
    int status = 0;
    std::string error;
    std::string body;
};

struct ServerRequest {
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    int timeoutMs = 120000;
    int proxyMode = 0;
    std::string proxy;
    std::string userAgent;
};

struct Options {
    bool dryRun = false;
    std::string serverUrl;                 // 空 = conf 的 server，再没有则用 kServerUrlDefault
    std::string configPath;                // 空 = <exe>\rlplayer-feedback.conf
    std::string outboxDir;                 // 空 = <exe>\feedback-outbox
    std::vector<std::pair<std::string, std::string>> logFiles;
    size_t logLines = 200, logMaxBytes = 128 * 1024;
    size_t extraLogLines = 80, extraLogMaxBytes = 32 * 1024;
    int timeoutMs = 120000;                // 单次上传总超时；不自动重试
    std::string proxy;                     // "" = 系统代理；"none" = 直连
    std::string idempotencyKey;            // 空 = 本次提交生成；测试/手动补交可指定
    bool keepLocalCopy = false;
    std::function<PutResult(const ServerRequest&)> transport;
};

struct Result {
    bool uploaded = false;
    int status = 0;
    std::string id;
    std::string remotePath;
    std::string localPath;
    std::string error;
    std::string message;
    std::string json;
    size_t bytes = 0;
    long long elapsedMs = 0;
    bool diagStored = false;
    long long retryAfter = -1;
    std::string client;
    std::string idempotencyKey;
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

inline bool is_ident_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

// 非 UTF-8 字节 → U+FFFD（日志里混入 ANSI 代码页字节时，不至于让整份 JSON 变成非法 UTF-8）
inline std::string utf8_sanitize(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 16);
    const size_t n = in.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)in[i];
        size_t need = 0;
        if (c < 0x80) need = 0;
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else { o += "\xEF\xBF\xBD"; ++i; continue; }
        if (i + need >= n) { o += "\xEF\xBF\xBD"; ++i; continue; }
        bool good = true;
        for (size_t k = 1; k <= need; ++k)
            if (((unsigned char)in[i + k] & 0xC0) != 0x80) { good = false; break; }
        if (!good) { o += "\xEF\xBF\xBD"; ++i; continue; }
        o.append(in, i, need + 1);
        i += need + 1;
    }
    return o;
}

inline std::string jesc(const std::string& raw) {
    const std::string s = utf8_sanitize(raw);
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"' : o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

inline std::string base64_encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4 + 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = ((unsigned)(unsigned char)in[i] << 16) |
                           ((unsigned)(unsigned char)in[i + 1] << 8) |
                           (unsigned)(unsigned char)in[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
        i += 3;
    }
    const size_t rest = in.size() - i;
    if (rest == 1) {
        const unsigned v = (unsigned)(unsigned char)in[i] << 16;
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += "==";
    } else if (rest == 2) {
        const unsigned v = ((unsigned)(unsigned char)in[i] << 16) | ((unsigned)(unsigned char)in[i + 1] << 8);
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

// 不区分大小写的子串查找
inline bool ci_find(const std::string& s, const char* lit, size_t from, size_t& hit) {
    const size_t n = std::strlen(lit);
    if (n == 0 || s.size() < n) return false;
    for (size_t i = from; i + n <= s.size(); ++i) {
        size_t k = 0;
        for (; k < n; ++k)
            if (std::tolower((unsigned char)s[i + k]) != std::tolower((unsigned char)lit[k])) break;
        if (k == n) { hit = i; return true; }
    }
    return false;
}

inline void replace_range(std::string& s, size_t a, size_t b, const std::string& rep) {
    if (a >= b || b > s.size()) return;
    s.replace(a, b - a, rep);
}

// ---------------------------------------------------------------------------
// 脱敏（唯一入口：payload 里每个字符串都过它）
//
// 规则清单（也是 payload 里 redaction.rules 报给用户看的那几条）：
//   1) openai_key          —— "sk-" 开头的密钥（含 sk-proj- / sk-live- …）
//   2) github_token        —— ghp_ / gho_ / ghu_ / ghs_ / ghr_ / github_pat_ 开头的 token
//   3) aws_key / slack_token —— AKIA… / xoxb-… / xoxp-…
//   4) auth_header         —— Authorization: <值>、Proxy-Authorization:、X-Api-Key:、Cookie:
//   5) bearer_token        —— 裸的 "Bearer <token>"
//   6) credential_kv       —— api_key / apikey / token / access_token / password / secret /
//                             client_secret / auth / key / gh_token 后面跟的值
//                             （"k":"v" 与 k=v 两种写法都认）
//   7) windows_user_path   —— C:\Users\<用户名>\… 与 /Users/<用户名>/… 里的用户名 → <user>
//   8) email               —— 邮箱本地部分 → <user>，域名保留
// 被替换掉的一律写 <redacted>（用户目录名写 <user>）：前缀/键名保留，方便判断"当时配没配"。
//
// 已知的"宁多勿漏"取舍：规则 6 里的裸 key 会把日志中形如 key=<封面缓存键> 的普通键值
// （rtaweb 的 COVERBG 行）一并打码 —— 拒绝把"看起来像凭据"的东西发出去，代价是那一小段
// 调试信息丢失（同一行的 name= 仍保留，够定位是哪张封面）。
// ---------------------------------------------------------------------------

inline void mask_prefixed_tokens(std::string& s) {
    static const char* kPfx[] = {"sk-", "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_",
                                 "xoxb-", "xoxp-", "AKIA"};
    size_t scan = 0;
    for (;;) {
        size_t hit = std::string::npos, plen = 0;
        for (const char* pf : kPfx) {
            const size_t p = s.find(pf, scan);
            if (p != std::string::npos && (hit == std::string::npos || p < hit)) {
                hit = p; plen = std::strlen(pf);
            }
        }
        if (hit == std::string::npos) break;
        const bool leftOk = (hit == 0) || !is_ident_char(s[hit - 1]);
        size_t e = hit + plen;
        while (e < s.size() && (is_ident_char(s[e]) || s[e] == '-' || s[e] == '.')) ++e;
        const size_t vlen = e - (hit + plen);
        if (leftOk && vlen >= 6) { replace_range(s, hit, e, "<redacted>"); scan = hit + 10; }
        else scan = hit + plen;
    }
}

inline void mask_header_values(std::string& s) {
    static const char* kHdr[] = {"authorization", "proxy-authorization", "x-api-key", "api-key",
                                 "x-auth-token", "cookie"};
    size_t scan = 0;
    for (;;) {
        size_t hit = std::string::npos, hlen = 0;
        for (const char* h : kHdr) {
            size_t p = 0;
            if (!ci_find(s, h, scan, p)) continue;
            if (hit == std::string::npos || p < hit) { hit = p; hlen = std::strlen(h); }
        }
        if (hit == std::string::npos) break;
        if (hit > 0 && is_ident_char(s[hit - 1])) { scan = hit + hlen; continue; }
        size_t j = hit + hlen;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (j >= s.size() || s[j] != ':') { scan = hit + hlen; continue; }
        ++j;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        const size_t vstart = j;
        size_t e = j;
        size_t cookiePos = 0;
        const bool cookieHeader = ci_find(s, "cookie", hit, cookiePos) && cookiePos == hit;
        while (e < s.size()) {
            const char c = s[e];
            if (c == '\r' || c == '\n' || (!cookieHeader &&
                (c == '"' || c == ',' || c == '}' || c == ';'))) break;
            ++e;
        }
        size_t ee = e;
        while (ee > vstart && (s[ee - 1] == ' ' || s[ee - 1] == '\t')) --ee;
        if (ee > vstart && s.compare(vstart, ee - vstart, "<redacted>") != 0)
            replace_range(s, vstart, ee, "<redacted>");
        scan = vstart + 10;
    }
}

inline void mask_bearer(std::string& s) {
    size_t scan = 0;
    for (;;) {
        size_t hit = 0;
        if (!ci_find(s, "bearer", scan, hit)) break;
        if (hit > 0 && is_ident_char(s[hit - 1])) { scan = hit + 6; continue; }
        size_t j = hit + 6;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        size_t e = j;
        while (e < s.size() && (is_ident_char(s[e]) || s[e] == '-' || s[e] == '.' || s[e] == '+' ||
                                s[e] == '/' || s[e] == '=')) ++e;
        if (e - j >= 6) { replace_range(s, j, e, "<redacted>"); scan = j + 10; }
        else scan = hit + 6;
    }
}

inline void mask_kv(std::string& s, const char* keyLit) {
    const size_t klen = std::strlen(keyLit);
    size_t scan = 0;
    for (;;) {
        size_t hit = 0;
        if (!ci_find(s, keyLit, scan, hit)) break;
        // 左右边界：前后都不能是标识符字符（避免 hotkey / monkey 被误伤）
        const bool leftOk = (hit == 0) || !is_ident_char(s[hit - 1]);
        const size_t after = hit + klen;
        const bool rightOk = (after >= s.size()) || !(is_ident_char(s[after]) || s[after] == '-');
        if (!leftOk || !rightOk) { scan = after; continue; }
        size_t j = after;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (j < s.size() && (s[j] == '"' || s[j] == '\'')) ++j;   // "key": ...
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (j >= s.size() || (s[j] != '=' && s[j] != ':')) { scan = after; continue; }
        ++j;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        bool quoted = false;
        if (j < s.size() && (s[j] == '"' || s[j] == '\'')) { quoted = true; ++j; }
        const size_t vstart = j;
        size_t e = j;
        while (e < s.size()) {
            const char c = s[e];
            if (quoted) { if (c == '"' || c == '\'' || c == '\r' || c == '\n') break; }
            else if (c == '&' || c == '"' || c == '\'' || c == ',' || c == '}' || c == ';' ||
                     c == '\r' || c == '\n' || c == ' ' || c == '\t') break;
            ++e;
        }
        if (e > vstart && s.compare(vstart, e - vstart, "<redacted>") != 0)
            replace_range(s, vstart, e, "<redacted>");
        scan = vstart + 10;
    }
}

inline void mask_url_userinfo(std::string& s) {
    size_t scan = 0;
    for (;;) {
        const size_t scheme = s.find("://", scan);
        if (scheme == std::string::npos) break;
        const size_t begin = scheme + 3;
        const size_t delimiter = s.find_first_of("/\\?# \t\r\n\"'", begin);
        const size_t end = delimiter == std::string::npos ? s.size() : delimiter;
        const size_t at = end > begin ? s.rfind('@', end - 1) : std::string::npos;
        if (at != std::string::npos && at >= begin) {
            replace_range(s, begin, at, "<redacted>");
            scan = begin + 11; // marker and '@'; retain host/port/path
        } else scan = end;
    }
}

inline void mask_user_paths(std::string& s) {
    static const char* kRoots[] = {"\\users\\", "/users/", "\\documents and settings\\"};
    size_t scan = 0;
    for (;;) {
        size_t hit = std::string::npos, rlen = 0;
        for (const char* r : kRoots) {
            size_t p = 0;
            if (!ci_find(s, r, scan, p)) continue;
            if (hit == std::string::npos || p < hit) { hit = p; rlen = std::strlen(r); }
        }
        if (hit == std::string::npos) break;
        const size_t start = hit + rlen;
        size_t e = start;
        while (e < s.size() && s[e] != '\\' && s[e] != '/' && s[e] != '"' && s[e] != '\'' &&
               s[e] != '\r' && s[e] != '\n') ++e;
        if (e > start && s.compare(start, e - start, "<user>") != 0) {
            replace_range(s, start, e, "<user>");
            scan = start + 6;
        } else {
            scan = hit + rlen + 1;
        }
    }
}

inline void mask_emails(std::string& s) {
    size_t at = 0;
    for (;;) {
        const size_t p = s.find('@', at);
        if (p == std::string::npos) break;
        size_t a = p;
        while (a > 0 && (is_ident_char(s[a - 1]) || s[a - 1] == '.' || s[a - 1] == '-' ||
                         s[a - 1] == '+' || s[a - 1] == '%')) --a;
        size_t b = p + 1;
        while (b < s.size() && (is_ident_char(s[b]) || s[b] == '.' || s[b] == '-')) ++b;
        const size_t dot = s.find('.', p + 1);
        const bool looksMail = (b > p + 2) && (dot != std::string::npos) && (dot < b) && (p > a);
        if (looksMail && s.compare(a, p - a, "<user>") != 0) {
            replace_range(s, a, p, "<user>");
            at = a + 7;
        } else {
            at = p + 1;
        }
    }
}

// 敏感键名表（脱敏与结构化字段共用同一份，避免"文本里认、JSON 里不认"）
inline const char* const* sensitive_key_list(size_t& n) {
    static const char* kKeys[] = {"api_key", "apikey", "api-key", "x-api-key", "token",
                                  "access_token", "refresh_token", "password", "passwd",
                                  "secret", "client_secret", "auth", "key", "gh_token", "cookie"};
    n = sizeof(kKeys) / sizeof(kKeys[0]);
    return kKeys;
}

// 这个键名是不是"值必须整值脱敏"的敏感字段（大小写不敏感，含子串匹配，
// 与 mask_kv 的口径一致：access_token 命中 token、client_secret 命中 secret）。
inline bool sensitive_key(const std::string& k) {
    size_t n = 0;
    const char* const* keys = sensitive_key_list(n);
    std::string lk;
    lk.reserve(k.size());
    for (unsigned char c : k) lk += (char)std::tolower(c);
    for (size_t i = 0; i < n; ++i) {
        const std::string lit = keys[i];
        if (lk.find(lit) != std::string::npos) return true;
    }
    return false;
}

// 公开入口：任何要进 payload 的文本都必须过它
inline std::string redact(const std::string& in) {
    std::string s = in;
    mask_prefixed_tokens(s);
    mask_header_values(s);
    mask_bearer(s);
    size_t nk = 0;
    const char* const* keys = sensitive_key_list(nk);
    for (size_t i = 0; i < nk; ++i) mask_kv(s, keys[i]);
    mask_url_userinfo(s);
    mask_user_paths(s);
    mask_emails(s);
    return s;
}

// 脱敏 + JSON 转义 + 加引号。payload 里所有字符串都从这里出去。
inline std::string jq(const std::string& raw) {
    return std::string("\"") + jesc(redact(raw)) + std::string("\"");
}

// ---------------------------------------------------------------------------
// 调用方塞进来的 JSON 片段也走同一条脱敏路：解析 → 每个字符串重新 jq() → 序列化
// ---------------------------------------------------------------------------
namespace detail {

// ★ 2026-10-29（两轮审查 F13）：结构化 JSON 的**键名上下文**必须传下去。
//
// 缺陷：sanitize_json_fragment() 先解析、再对每个字符串单独调 jq()（= redact()），
//   于是 {"password":"ExampleDummy123"} 里的值被当成一段**没有键名**的普通文本 ——
//   redact() 认不出它是密码，原样保留；而同一串在 "password=ExampleDummy123"
//   这种纯文本形态里会被 mask_kv 打掉。同一个秘密，两种形态两种结果。
//
// 修法：emit_value 带 key 参数；键名敏感时**整值**替换成 <redacted>（不管值是
//   字符串、对象还是数组 —— 整值脱敏比逐字段猜更安全）。
inline void emit_value(const rta::net::Json& v, std::string& o,
                       const std::string& key = std::string()) {
    if (!key.empty() && sensitive_key(key)) { o += "\"<redacted>\""; return; }
    switch (v.type) {
        case rta::net::Json::Null: o += "null"; break;
        case rta::net::Json::Bool: o += (v.b ? "true" : "false"); break;
        case rta::net::Json::Num: {
            char b[40];
            if (v.num == (double)(long long)v.num) std::snprintf(b, sizeof(b), "%lld", (long long)v.num);
            else std::snprintf(b, sizeof(b), "%.6g", v.num);
            o += b;
            break;
        }
        case rta::net::Json::Str: o += jq(v.str); break;
        case rta::net::Json::Arr: {
            o += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) { if (i) o += ','; emit_value(v.arr[i], o); }
            o += ']';
            break;
        }
        case rta::net::Json::Obj: {
            o += '{';
            bool first = true;
            for (const auto& kv : v.obj) {
                if (!first) o += ',';
                first = false;
                o += jq(kv.first);
                o += ':';
                emit_value(kv.second, o, kv.first);   // ★ F13：键名上下文向下传
            }
            o += '}';
            break;
        }
    }
}

} // namespace detail

inline std::string sanitize_json_fragment(const std::string& raw) {
    if (raw.find_first_not_of(" \t\r\n") == std::string::npos) return "{}";
    rta::net::Json j = rta::net::json_parse(raw);
    if (j.type == rta::net::Json::Null) return std::string("{\"_unparsed\":") + jq(raw) + "}";
    std::string o;
    detail::emit_value(j, o);
    return o;
}

// ---------------------------------------------------------------------------
// 时间 / 环境
// ---------------------------------------------------------------------------
struct Stamp {
    std::string id;        // 20261027-024012-a1b2c3
    std::string iso;       // 2026-10-27T02:40:12+08:00
    long long   unixSec = 0;
};

inline unsigned rnd32() {
    static std::mutex mx;
    static unsigned long long st = 0;
    std::lock_guard<std::mutex> lk(mx);
    if (st == 0) {
        st = (unsigned long long)rta::platform::monotonic_ms() * 6364136223846793005ULL +
             (unsigned long long)rta::platform::process_id() * 1442695040888963407ULL;
        if (st == 0) st = 88172645463325252ULL;
    }
    st ^= st << 13; st ^= st >> 7; st ^= st << 17;
    return (unsigned)((st >> 16) & 0xFFFFFFFFull);
}

inline Stamp make_stamp() {
    Stamp s;
    // 平台层：本地时间 + 时区偏移（分钟，东为正）。Windows 实现 = GetLocalTime +
    // GetTimeZoneInformation（夏令时算 DaylightBias，否则 StandardBias）—— 与迁移前逐字一致。
    const rta::platform::LocalTime st = rta::platform::local_time_now();
    char b[64];
    std::snprintf(b, sizeof(b), "%04u%02u%02u-%02u%02u%02u", st.year, st.month,
                  st.day, st.hour, st.minute, st.second);
    char r[8];
    std::snprintf(r, sizeof(r), "%06x", rnd32() & 0xFFFFFFu);
    s.id = std::string(b) + "-" + r;
    std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02u", st.year, st.month,
                  st.day, st.hour, st.minute, st.second);
    const long east = (long)rta::platform::local_timezone_offset_minutes();   // 分钟，东为正
    const long ah = east >= 0 ? east / 60 : -((-east) / 60);
    const long am = east >= 0 ? (east % 60) : -((-east) % 60);
    char tzb[16];
    std::snprintf(tzb, sizeof(tzb), "%+03ld:%02ld", ah, (am < 0 ? -am : am));
    s.iso = std::string(b) + tzb;
    s.unixSec = (long long)std::time(nullptr);
    return s;
}

inline std::string exe_dir() { return rta::platform::module_dir_utf8(); }

inline void collect_gpu_adapters(std::string& out) {
    // 平台层：EnumDisplayDevicesW 枚举 + 去重 + 只取"已连接到桌面"的（语义与迁移前一致）
    const std::vector<std::string> names = rta::platform::display_adapter_names();
    out.clear();
    for (size_t i = 0; i < names.size(); ++i) { if (i) out += " / "; out += names[i]; }
}

// 进程已运行秒数。
// 坑：GetProcessTimes 的 lpExitTime 对**仍在运行**的进程不可用（实测拿到 0），
// 拿它减低 64 位会回绕成 1.8e12 秒。所以改成"现在（FILETIME 时基）- 创建时间"。
inline unsigned long long proc_uptime_sec() {
    return rta::platform::process_uptime_seconds();
}

struct MemInfo {
    unsigned long long wsMb = 0, peakMb = 0, privMb = 0, ramTotalMb = 0, ramAvailMb = 0;
    unsigned handles = 0;
};

inline MemInfo mem_info() {
    MemInfo m;
    // 平台层给字节，这里换成 MB（整数除法，与迁移前的 / (1024*1024) 同一算式）
    const rta::platform::MemoryInfo mi = rta::platform::process_memory();
    m.wsMb       = mi.working_set_bytes / (1024ull * 1024ull);
    m.peakMb     = mi.peak_working_set_bytes / (1024ull * 1024ull);
    m.privMb     = mi.private_bytes / (1024ull * 1024ull);
    m.handles    = mi.handle_count;
    m.ramTotalMb = mi.total_physical_bytes / (1024ull * 1024ull);
    m.ramAvailMb = mi.avail_physical_bytes / (1024ull * 1024ull);
    return m;
}

// 读文件尾巴：最多 maxLines 行、最多 maxBytes 字节（先按字节截，再按行截）
inline bool read_tail_file(const std::string& path, size_t maxLines, size_t maxBytes,
                           std::string& out, bool& truncated) {
    out.clear();
    truncated = false;
    std::ifstream f(rta::platform::from_utf8(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff len = f.tellg();
    if (len < 0) return false;
    const std::streamoff start = (len > (std::streamoff)maxBytes) ? len - (std::streamoff)maxBytes : 0;
    f.seekg(start);
    std::string buf((size_t)(len - start), '\0');
    if (!buf.empty()) f.read(buf.data(), (std::streamsize)buf.size());
    if (start > 0) truncated = true;
    size_t lines = 0, cut = 0;
    for (size_t i = buf.size(); i > 0; --i) {
        if (buf[i - 1] == '\n') {
            ++lines;
            if (lines > maxLines) { cut = i; break; }
        }
    }
    if (cut) { buf.erase(0, cut); truncated = true; }
    // 去掉 UTF-8 BOM（记事本/PowerShell 5.1 写的日志常带；留着会让 JSON 里多一个不可见字符）
    if (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) buf.erase(0, 3);
    out = utf8_sanitize(buf);
    return true;
}

// 设置快照：conf 里所有 key=value（敏感键的值会被 redact 打掉；条数/长度都封顶）
inline std::string settings_snapshot(const std::string& path, bool& present) {
    present = false;
    std::ifstream f(rta::platform::from_utf8(path), std::ios::binary);
    if (!f) return "{}";
    present = true;
    std::string o = "{";
    bool first = true;
    std::string line;
    int n = 0;
    while (std::getline(f, line) && n < 200) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t e = line.find('=');
        if (e == std::string::npos) continue;
        const std::string k = line.substr(0, e);
        std::string v = line.substr(e + 1);
        if (v.size() > 300) v = v.substr(0, 300) + "...";
        if (!first) o += ",";
        first = false;
        // ★ F13：设置文件里的敏感键（password / token / api_key …）**整值**脱敏。
        //   旧代码对键和值分别调 jq()：值没有键名上下文，一个不带任何可识别前缀的
        //   密码（如 ExampleDummy123）会原样进诊断包。
        o += jq(k) + ":" + (sensitive_key(k) ? std::string("\"<redacted>\"") : jq(v));
        ++n;
    }
    o += "}";
    return o;
}

inline std::string strip_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline std::string ascii_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline bool valid_server_url(const std::string& url) {
    for (unsigned char c : url) if (c <= 0x20 || c == 0x7f) return false;
    if (url.rfind("https://", 0) == 0 && url.size() > 8) {
        const size_t hostEnd = url.find_first_of("/?#", 8);
        return hostEnd == std::string::npos || hostEnd > 8;
    }
    const char* local = std::getenv("RLPLAYER_FEEDBACK_ALLOW_LOCAL");
    if (!local || std::strcmp(local, "1") != 0 || url.rfind("http://", 0) != 0) return false;
    const size_t slash = url.find('/', 7);
    if (slash == std::string::npos || slash + 1 >= url.size()) return false;
    const auto authority = rta::net::parse_http_authority(url.substr(7, slash - 7), 80);
    return authority.valid && (authority.host == "127.0.0.1" || authority.host == "localhost");
}

inline bool valid_client_id(const std::string& client) {
    if (client.size() != 32) return false;
    for (unsigned char c : client) if (!std::isxdigit(c)) return false;
    return true;
}

inline std::string secure_random_hex(size_t bytes) {
    if (bytes == 0 || bytes > 1024) throw std::runtime_error("invalid random size");
    std::vector<unsigned char> random(bytes);
#ifdef _WIN32
    const NTSTATUS status = BCryptGenRandom(nullptr, random.data(), (ULONG)bytes,
                                             BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0)
        throw std::runtime_error("Windows system random source failed (status=" +
                                 std::to_string((long)status) + ")");
#else
    throw std::runtime_error("secure feedback randomness requires Windows");
#endif
    static const char hex[] = "0123456789abcdef";
    std::string out(bytes * 2, '0');
    for (size_t i = 0; i < bytes; ++i) {
        out[i * 2] = hex[random[i] >> 4];
        out[i * 2 + 1] = hex[random[i] & 15];
    }
    return out;
}

struct FeedbackConfig {
    std::string serverUrl = kServerUrlDefault;
    std::string client;
    bool disabled = false;
    std::string error;
};

inline FeedbackConfig parse_feedback_config(const std::string& text) {
    FeedbackConfig cfg;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = ascii_lower(strip_trim(line.substr(0, eq)));
        const std::string value = strip_trim(line.substr(eq + 1));
        if (key == "server") {
            if (ascii_lower(value) == "none") cfg.disabled = true;
            else if (valid_server_url(value)) cfg.serverUrl = value;
            else cfg.error = "反馈服务器地址必须是 https://... 或 none";
        } else if (key == "worker" || key == "worker_url" || key == "feedback_worker") {
            if (ascii_lower(value) == "none") cfg.disabled = true;
        } else if (key == "client" && cfg.client.empty() && valid_client_id(value)) {
            cfg.client = ascii_lower(value);
        }
    }
    return cfg;
}

inline std::string feedback_config_path(const Options& opt) {
    return opt.configPath.empty() ? rta::platform::join(exe_dir(), "rlplayer-feedback.conf") : opt.configPath;
}

inline FeedbackConfig load_feedback_config(const Options& opt) {
    static std::mutex* mutex = new std::mutex();
    std::lock_guard<std::mutex> lock(*mutex);
    const std::string path = feedback_config_path(opt);
    std::string text;
    {
        std::ifstream file(rta::platform::from_utf8(path), std::ios::binary);
        if (file) { std::ostringstream buffer; buffer << file.rdbuf(); text = buffer.str(); }
    }
    FeedbackConfig cfg = parse_feedback_config(text);
    if (!opt.serverUrl.empty()) {
        if (ascii_lower(opt.serverUrl) == "none") cfg.disabled = true;
        else if (valid_server_url(opt.serverUrl)) cfg.serverUrl = opt.serverUrl;
        else cfg.error = "反馈服务器地址必须是 https://... 或 none";
    }
    if (!cfg.error.empty() || !cfg.client.empty()) return cfg;
    try {
        cfg.client = secure_random_hex(16);
    } catch (const std::exception& e) {
        cfg.error = e.what();
        return cfg;
    }
    std::ofstream file(rta::platform::from_utf8(path), std::ios::binary | std::ios::app);
    if (!file) {
        cfg.error = "无法写入反馈客户端配置：" + path;
        cfg.client.clear();
        return cfg;
    }
    if (!text.empty() && text.back() != '\n' && text.back() != '\r') file << '\n';
    file << "client=" << cfg.client << "\n";
    file.close();
    if (!file) {
        cfg.error = "无法保存反馈客户端标识：" + path;
        cfg.client.clear();
    }
    return cfg;
}

inline PutResult http_post_once(const ServerRequest& request) {
    rta::platform::HttpUploadRequest req;
    req.method = "POST";
    req.url = request.url;
    req.headers = request.headers;
    req.body = request.body;
    req.timeoutMs = request.timeoutMs;
    req.proxyMode = request.proxyMode;
    req.proxy = request.proxy;
    req.userAgent = request.userAgent;
    const rta::platform::HttpUploadResult r = rta::platform::http_upload(req);
    return {r.ok, r.status, r.error, r.body};
}

inline std::string http_status_hint(int status) {
    switch (status) {
        case 413: return "反馈或诊断包超过服务器限制（HTTP 413）";
        case 429: return "提交过于频繁，请稍后重试（HTTP 429）";
        case 503: return "反馈服务暂不可用（HTTP 503）";
        default: return "反馈服务返回 HTTP " + std::to_string(status);
    }
}

struct ServerAck {
    bool valid = false;
    std::string id;
    bool diagStored = false;
};

inline ServerAck parse_server_ack(int status, const std::string& body) {
    ServerAck ack;
    if (status != 200 && status != 201) return ack;
    rta::net::Json json;
    if (!rta::net::json_parse_strict(body, json) || json.type != rta::net::Json::Obj ||
        json["ok"].type != rta::net::Json::Bool || !json["ok"].b ||
        json["id"].type != rta::net::Json::Str || json["id"].str.empty() ||
        json["id"].str.size() > 256 || json["diagStored"].type != rta::net::Json::Bool) return ack;
    ack.valid = true;
    ack.id = json["id"].str;
    ack.diagStored = json["diagStored"].b;
    return ack;
}

struct ServerError {
    bool valid = false;
    std::string error;
    long long retryAfter = -1;
};

inline ServerError parse_server_error(const std::string& body) {
    ServerError out;
    rta::net::Json json;
    if (!rta::net::json_parse_strict(body, json) || json.type != rta::net::Json::Obj ||
        json["ok"].type != rta::net::Json::Bool || json["ok"].b ||
        json["error"].type != rta::net::Json::Str || json["error"].str.empty()) return out;
    out.valid = true;
    out.error = json["error"].str;
    if (json["retryAfter"].type == rta::net::Json::Num && json["retryAfter"].num >= 0 &&
        json["retryAfter"].num <= 31536000.0 && std::floor(json["retryAfter"].num) == json["retryAfter"].num)
        out.retryAfter = (long long)json["retryAfter"].num;
    return out;
}

// ---------------------------------------------------------------------------
// 打包
// ---------------------------------------------------------------------------
struct Payload {
    std::string id;
    std::string json;
};

inline std::string type_norm(const std::string& raw) {
    std::string t;
    for (char c : raw) t += (char)std::tolower((unsigned char)c);
    t = strip_trim(t);
    if (t.empty()) return "bug";
    if (t == "bug" || t == "error" || t == "错误" || t == "报错" || t == "崩溃") return "bug";
    if (t == "idea" || t == "suggest" || t == "suggestion" || t == "feature" ||
        t == "建议" || t == "意见" || t == "需求") return "idea";
    if (t == "other" || t == "其他" || t == "其它") return "other";
    return t;
}

inline std::string type_label(const std::string& t) {
    if (t == "bug")   return "BUG 报错";
    if (t == "idea")  return "功能建议";
    if (t == "other") return "其他";
    return t;
}

inline Payload build_payload(const UserReport& u, const LiveState& live, const Options& opt) {
    Payload p;
    const Stamp st = make_stamp();
    p.id = st.id;
    const MemInfo mi = mem_info();

    std::string gpu = live.gpu;
    if (gpu.empty()) collect_gpu_adapters(gpu);

    const std::string dir = exe_dir();
    const std::string type = type_norm(u.type);

    std::string o;
    o.reserve(32 * 1024);
    o += "{";
    o += "\"schema\":" + std::to_string(kSchema);
    o += ",\"kind\":\"rlplayer-feedback\"";
    o += ",\"id\":" + jq(p.id);
    o += ",\"created_at\":" + jq(st.iso);
    o += ",\"created_at_unix\":" + std::to_string(st.unixSec);
    // 未来 DEBUG 的人先读这一段：字段怎么用、哪些东西被拿掉了
    o += ",\"schema_doc\":" + jq(
        "schema=1。report=用户填写；app/system/process/playback/settings/endpoints/logs=自动采集；"
        "redaction=脱敏说明（被替换的原文一律是 <redacted>，用户目录名是 <user>）；"
        "全部字符串都已脱敏，可以随反馈提交给开发团队");

    o += ",\"app\":{";
    o += "\"name\":\"RLPlayer\"";
    o += ",\"version\":" + jq(std::string(RLPLAYER_VERSION));
    o += ",\"build\":" + jq(std::string(__DATE__) + " " + __TIME__);
    o += ",\"source\":" + jq(live.source.empty() ? "unknown" : live.source);
    o += ",\"tester\":" + jq(rta::diag::read_tester());
    o += ",\"arch\":" + jq(sizeof(void*) == 8 ? "x64" : "x86");
    o += "}";

    o += ",\"report\":{";
    o += "\"type\":" + jq(type);
    o += ",\"type_label\":" + jq(type_label(type));
    o += ",\"title\":" + jq(u.title);
    o += ",\"body\":" + jq(u.body);
    if (!u.steps.empty()) o += ",\"steps\":" + jq(u.steps);
    o += "}";

    o += ",\"system\":{";
    o += "\"os\":" + jq(rta::diag::os_version_string());
    o += ",\"cpu\":" + jq(rta::diag::cpu_brand());
    o += ",\"cpu_threads\":" + std::to_string((unsigned long long)rta::platform::cpu_thread_count());
    o += ",\"gpu\":" + jq(gpu);
    o += ",\"ram_total_mb\":" + std::to_string(mi.ramTotalMb);
    o += ",\"ram_avail_mb\":" + std::to_string(mi.ramAvailMb);
    o += ",\"exe_dir\":" + jq(dir);
    o += "}";

    o += ",\"process\":{";
    o += "\"pid\":" + std::to_string(rta::platform::process_id());
    o += ",\"uptime_sec\":" + std::to_string(proc_uptime_sec());
    o += ",\"mem_working_set_mb\":" + std::to_string(mi.wsMb);
    o += ",\"mem_peak_mb\":" + std::to_string(mi.peakMb);
    o += ",\"mem_private_mb\":" + std::to_string(mi.privMb);
    o += ",\"handles\":" + std::to_string(mi.handles);
    o += "}";

    o += ",\"playback\":{";
    o += "\"playing\":" + std::string(live.playing ? "true" : "false");
    o += ",\"paused\":" + std::string(live.paused ? "true" : "false");
    o += ",\"track\":" + jq(live.track);
    o += ",\"last_error\":" + jq(live.lastError);
    o += ",\"backend\":" + jq(live.backend);
    o += ",\"device\":" + jq(live.device);
    o += ",\"out_mode\":" + jq(live.outMode);
    o += ",\"out_rate\":" + std::to_string(live.outRate);
    o += ",\"out_bits\":" + std::to_string(live.outBits);
    o += "}";

    {
        bool present = false;
        const std::string conf = settings_snapshot(rta::platform::join(dir, "rlplayer-settings.conf"), present);
        o += ",\"settings_present\":" + std::string(present ? "true" : "false");
        o += ",\"settings\":";
        o += live.settingsJson.empty() ? conf : sanitize_json_fragment(live.settingsJson);
    }
    o += ",\"endpoints\":";
    o += live.endpointsJson.empty() ? std::string("[]") : sanitize_json_fragment(live.endpointsJson);

    // 日志尾巴：默认播放日志（200 行 / 128 KB）+ launcher.log（80 行 / 32 KB）
    o += ",\"logs\":[";
    {
        std::vector<std::pair<std::string, std::string>> files = opt.logFiles;
        if (files.empty()) {
            files.push_back(std::make_pair(std::string("rlplayer-playback.log"),
                                           rta::platform::join(dir, "rlplayer-playback.log")));
            files.push_back(std::make_pair(std::string("launcher.log"),
                                           rta::platform::join(dir, "launcher.log")));
        }
        bool first = true;
        for (size_t i = 0; i < files.size(); ++i) {
            const bool primary = (i == 0);
            const size_t lines = primary ? opt.logLines : opt.extraLogLines;
            const size_t bytes = primary ? opt.logMaxBytes : opt.extraLogMaxBytes;
            std::string text;
            bool trunc = false;
            const bool ok = read_tail_file(files[i].second, lines, bytes, text, trunc);
            if (!first) o += ",";
            first = false;
            o += "{";
            o += "\"name\":" + jq(files[i].first);
            o += ",\"path\":" + jq(files[i].second);
            o += ",\"present\":" + std::string(ok ? "true" : "false");
            o += ",\"max_lines\":" + std::to_string(lines);
            o += ",\"truncated\":" + std::string(trunc ? "true" : "false");
            o += ",\"text\":" + jq(text);
            o += "}";
        }
    }
    o += "]";

    o += ",\"extra\":";
    o += live.extraJson.empty() ? std::string("{}") : sanitize_json_fragment(live.extraJson);

    o += ",\"redaction\":{";
    o += "\"applied\":true";
    o += ",\"rules\":[\"openai_key\",\"github_token\",\"aws_key\",\"slack_token\",\"auth_header\","
         "\"bearer_token\",\"credential_kv\",\"windows_user_path\",\"email\"]";
    o += ",\"marker\":\"<redacted>\"";
    o += ",\"user_marker\":\"<user>\"";
    o += ",\"excluded_files\":[\"KEY.txt\",\"rlplayer-llm.conf\",\"rlplayer-feedback.conf\"]";
    o += ",\"note\":" + jq("上面三个文件的内容从不读取、从不打包；本包里所有字符串都过了同一条脱敏函数");
    o += "}";
    o += "}";

    p.json = o;
    return p;
}

// ---------------------------------------------------------------------------
// 本地兜底
// ---------------------------------------------------------------------------
inline std::string outbox_write(const std::string& id, const std::string& json,
                                const std::string& outboxDir) {
    const std::string dir = outboxDir.empty() ? rta::platform::join(exe_dir(), "feedback-outbox") : outboxDir;
    std::error_code ec;
    std::filesystem::create_directories(rta::platform::from_utf8(dir), ec);
    if (ec) return "";
    const std::string path = rta::platform::join(dir, id + ".json");
    std::ofstream f(rta::platform::from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!f) return "";
    f.write(json.data(), (std::streamsize)json.size());
    f.close();
    return f ? path : std::string();
}

inline ServerRequest make_server_request(const Payload& payload, const UserReport& user,
                                         const LiveState& live, const Options& opt,
                                         const std::string& serverUrl, const std::string& client,
                                         const std::string& idempotencyKey) {
    std::string text = user.title.empty() ? user.body : (user.title + "\n\n" + user.body);
    if (!user.steps.empty()) text += "\n\n复现步骤：\n" + user.steps;
    if (text.size() > 16000) text = text.substr(0, 16000) + "\n…（已截断）";
    const std::string userAgent = std::string("RLPlayer/") + RLPLAYER_VERSION;
    ServerRequest request;
    request.url = serverUrl;
    request.timeoutMs = std::max(1, opt.timeoutMs);
    request.proxyMode = opt.proxy.empty() ? 0 : (opt.proxy == "none" ? 1 : 2);
    request.proxy = opt.proxy;
    request.userAgent = userAgent;
    request.headers = {
        "Content-Type: application/json; charset=utf-8",
        "Accept: application/json",
        "User-Agent: " + userAgent,
        "X-RLPlayer-Client: " + client,
        "Idempotency-Key: " + idempotencyKey
    };
    request.body = "{";
    request.body += "\"text\":" + jq(text);
    request.body += ",\"version\":" + jq(std::string(RLPLAYER_VERSION));
    request.body += ",\"os\":" + jq(std::string("Windows · ") +
        (live.source.empty() ? std::string("unknown") : live.source));
    request.body += ",\"diag\":" + jq(base64_encode(payload.json));
    request.body += ",\"client\":" + jq(client);
    request.body += ",\"idempotency_key\":" + jq(idempotencyKey);
    request.body += "}";
    return request;
}

inline void finish_failure(Result& result, const Payload& payload, const Options& opt,
                           unsigned long long started) {
    result.localPath = outbox_write(payload.id, payload.json, opt.outboxDir);
    result.message = result.localPath.empty()
        ? (result.error + "；本地兜底也没写成，请检查 exe 目录是否可写")
        : ("没上传成功（" + result.error + "）。反馈已保存到本地：" + result.localPath);
    result.elapsedMs = (long long)(rta::platform::monotonic_ms() - started);
}

inline Result submit(const UserReport& u, const LiveState& live, const Options& opt) {
    Result r;
    const unsigned long long started = rta::platform::monotonic_ms();
    const Payload payload = build_payload(u, live, opt);
    r.id = payload.id;
    r.json = payload.json;
    r.bytes = payload.json.size();
    if (opt.dryRun) {
        r.message = "dry-run：只生成 JSON，未上传";
        r.elapsedMs = (long long)(rta::platform::monotonic_ms() - started);
        return r;
    }
    if (u.title.empty() && u.body.empty()) {
        r.error = "标题和正文不能都为空";
        r.message = r.error;
        r.elapsedMs = (long long)(rta::platform::monotonic_ms() - started);
        return r;
    }

    const FeedbackConfig cfg = load_feedback_config(opt);
    r.client = cfg.client;
    if (!cfg.error.empty()) {
        r.error = cfg.error;
        finish_failure(r, payload, opt, started);
        return r;
    }
    if (cfg.disabled) {
        r.error = "上传已禁用（反馈配置为 none）";
        finish_failure(r, payload, opt, started);
        return r;
    }
    r.idempotencyKey = opt.idempotencyKey;
    if (r.idempotencyKey.empty()) {
        try {
            r.idempotencyKey = secure_random_hex(16);
        } catch (const std::exception& e) {
            r.error = e.what();
            finish_failure(r, payload, opt, started);
            return r;
        }
    }
    if (r.idempotencyKey.size() > 256 || r.idempotencyKey.find_first_of("\r\n") != std::string::npos) {
        r.error = "幂等键无效";
        finish_failure(r, payload, opt, started);
        return r;
    }

    const ServerRequest request = make_server_request(payload, u, live, opt, cfg.serverUrl,
                                                       r.client, r.idempotencyKey);
    PutResult response;
    try {
        response = opt.transport ? opt.transport(request) : http_post_once(request);
    } catch (const std::exception& e) {
        response.error = e.what();
    } catch (...) {
        response.error = "未知传输错误";
    }
    r.status = response.status;
    const ServerAck ack = parse_server_ack(response.status, response.body);
    if (response.ok && ack.valid) {
        r.uploaded = true;
        r.id = ack.id;
        r.remotePath = ack.id;
        r.diagStored = ack.diagStored;
        r.message = "已提交，感谢反馈！工单 " + r.id +
                    (r.diagStored ? "（诊断已存储）" : "（服务器未存储诊断）");
        if (opt.keepLocalCopy) r.localPath = outbox_write(payload.id, payload.json, opt.outboxDir);
        r.elapsedMs = (long long)(rta::platform::monotonic_ms() - started);
        return r;
    }

    if (response.status == 200 || response.status == 201) {
        r.error = "反馈服务响应无效，无法确认已提交";
    } else if (response.status != 0) {
        const ServerError serverError = parse_server_error(response.body);
        if (serverError.valid) {
            r.error = serverError.error;
            r.retryAfter = serverError.retryAfter;
        } else {
            r.error = http_status_hint(response.status);
        }
    } else {
        r.error = "网络请求失败：" + (response.error.empty() ? std::string("未知错误") : response.error);
    }
    finish_failure(r, payload, opt, started);
    return r;
}

inline std::string result_json(const Result& r) {
    std::string o = "{";
    o += "\"ok\":" + std::string(r.uploaded ? "true" : "false");
    o += ",\"uploaded\":" + std::string(r.uploaded ? "true" : "false");
    o += ",\"status\":" + std::to_string(r.status);
    o += ",\"id\":" + jq(r.id);
    o += ",\"path\":" + jq(r.remotePath);
    o += ",\"local\":" + jq(r.localPath);
    o += ",\"error\":" + jq(r.error);
    o += ",\"message\":" + jq(r.message);
    o += ",\"bytes\":" + std::to_string((unsigned long long)r.bytes);
    o += ",\"elapsed_ms\":" + std::to_string(r.elapsedMs);
    o += ",\"diag_stored\":" + std::string(r.diagStored ? "true" : "false");
    o += ",\"retry_after\":" + std::to_string(r.retryAfter);
    o += ",\"client\":" + jq(r.client);
    o += ",\"idempotency_key\":" + jq(r.idempotencyKey);
    o += "}";
    return o;
}

// body 形如 {"type":"bug","title":"…","body":"…","steps":"…"}（都容忍缺失）
// 2026-10-28（用户：让终端用户直接通过软件提交反馈）：
//   /api/feedback/preview —— **不联网、不落盘**，只回报"这一条会带出去哪些东西"。
//   实现上直接调用 build_payload()，与真提交是**同一个打包函数**：
//   预览里报的日志行数、设置项数、脱敏规则数，就是提交时的真实值，不存在两套口径。
//   只回摘要，不把整包 JSON 给前端 —— 已脱敏也没必要让浏览器持有。
inline std::string preview_handler(const std::string& body, const LiveState& live) {
    const rta::net::Json j = rta::net::json_parse(body);
    UserReport u;
    u.type  = j["type"].s();
    u.title = j["title"].s();
    u.body  = j["body"].s();
    u.steps = j["steps"].s();

    LiveState lv = live;
    if (lv.source.empty()) lv.source = "rtaweb";

    // 真打一遍包（含全部脱敏）。失败如实回报，不给空壳让用户误判。
    // 用默认 Options：与 web_handler 真提交时**完全一致**（否则预览的日志行数会跟实际不同）
    const Payload p = build_payload(u, lv, Options{});
    if (p.json.empty()) return std::string("{\"ok\":false,\"error\":\"打包失败（内部错误）\"}");

    const rta::net::Json pj = rta::net::json_parse(p.json);
    if (!pj.has("app") || !pj.has("logs"))
        return std::string("{\"ok\":false,\"error\":\"预览解析失败\"}");

    // 日志：日志段是数组，取各文件的行数/字节数求和
    long long logLines = 0, logBytes = 0;
    if (pj["logs"].type == rta::net::Json::Arr) {
        for (const auto& f : pj["logs"].arr) {
            const std::string txt = f.has("text") ? f["text"].s() : std::string();
            // 行数按实际换行统计（比 max_lines 更诚实：文件可能更短）
            long long n = 0;
            for (char c : txt) if (c == '\n') ++n;
            if (!txt.empty() && txt.back() != '\n') ++n;
            logLines += n;
            logBytes += (long long)txt.size();
        }
    }

    // 设置项数
    int settingsKeys = 0;
    if (pj.has("settings") && pj["settings"].type == rta::net::Json::Obj)
        settingsKeys = (int)pj["settings"].obj.size();

    // 脱敏规则条数（取 payload 自己报的清单长度，不硬编码）
    int redactRules = 0;
    if (pj.has("redaction")) {
        const rta::net::Json& r = pj["redaction"];
        if (r.has("rules") && r["rules"].type == rta::net::Json::Arr)
            redactRules = (int)r["rules"].arr.size();
    }

    const std::string osStr  = pj["system"].has("os")  ? pj["system"]["os"].s()  : std::string();
    const std::string cpuStr = pj["system"].has("cpu") ? pj["system"]["cpu"].s() : std::string();
    long long threads = 0;
    if (pj["system"].has("cpu_threads") && pj["system"]["cpu_threads"].type == rta::net::Json::Num)
        threads = (long long)pj["system"]["cpu_threads"].n();
    long long ramTotal = 0;
    if (pj["system"].has("ram_total_mb") && pj["system"]["ram_total_mb"].type == rta::net::Json::Num)
        ramTotal = (long long)pj["system"]["ram_total_mb"].n();
    const std::string gpuStr = pj["system"].has("gpu") ? pj["system"]["gpu"].s() : std::string();

    // 设备名可能含用户名路径，用脱敏后的值
    std::string dev;
    if (pj.has("playback")) {
        const rta::net::Json& pb = pj["playback"];
        if (pb.has("device")) dev = pb["device"].s();
    }
    std::string backend;
    if (pj.has("playback") && pj["playback"].has("backend")) backend = pj["playback"]["backend"].s();
    bool playing = false;
    if (pj.has("playback") && pj["playback"].has("playing"))
        playing = pj["playback"]["playing"].type == rta::net::Json::Bool && pj["playback"]["playing"].b;

    std::ostringstream o;
    o << "{\"ok\":true";
    o << ",\"type_label\":" << jq(type_label(type_norm(u.type)));
    o << ",\"version\":" << jq(pj["app"].has("version") ? pj["app"]["version"].s() : std::string());
    o << ",\"build\":" << jq(pj["app"].has("build") ? pj["app"]["build"].s() : std::string());
    o << ",\"os\":" << jq(osStr);
    o << ",\"cpu\":" << jq(cpuStr);
    o << ",\"cpu_threads\":" << threads;
    o << ",\"gpu\":" << jq(gpuStr);
    o << ",\"ram_total_mb\":" << ramTotal;
    o << ",\"playing\":" << (playing ? "true" : "false");
    o << ",\"backend\":" << jq(backend);
    o << ",\"device\":" << jq(dev);
    o << ",\"settings_keys\":" << settingsKeys;
    o << ",\"log_lines\":" << logLines;
    o << ",\"log_kb\":" << ((logBytes + 1023) / 1024);
    o << ",\"redaction_rules\":" << redactRules;
    o << "}";
    return o.str();
}

inline Options request_options(const rta::net::Json& json, const Options& input) {
    Options opt = input;
    if (opt.idempotencyKey.empty()) opt.idempotencyKey = strip_trim(json["idempotency_key"].s());
    return opt;
}

inline std::string web_handler(const std::string& body, const LiveState& live,
                               const Options& optIn = Options{}) {
    const rta::net::Json j = rta::net::json_parse(body);
    UserReport u;
    u.type  = j["type"].s();
    u.title = j["title"].s();
    u.body  = j["body"].s();
    u.steps = j["steps"].s();
    if (u.title.empty() && u.body.empty())
        return std::string("{\"ok\":false,\"uploaded\":false,\"error\":\"标题和正文不能都为空\","
                           "\"message\":\"标题和正文不能都为空\"}");
    LiveState lv = live;
    if (lv.source.empty()) lv.source = "rtaweb";
    return result_json(submit(u, lv, request_options(j, optIn)));
}

namespace detail {

struct AsyncJob {
    bool done = false;
    std::string result;
    std::chrono::steady_clock::time_point completed;
};

struct AsyncRegistry {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<AsyncJob>> jobs;
};

inline AsyncRegistry& async_registry() {
    // Detached workers may finish during process teardown; the registry has no static destructor.
    static AsyncRegistry* registry = new AsyncRegistry();
    return *registry;
}

inline void prune_async_jobs_locked(AsyncRegistry& registry,
                                    std::chrono::steady_clock::time_point now) {
    for (auto it = registry.jobs.begin(); it != registry.jobs.end();) {
        const auto& job = *it->second;
        if (job.done && now - job.completed >= std::chrono::seconds(kAsyncResultRetentionSeconds))
            it = registry.jobs.erase(it);
        else
            ++it;
    }
}

inline std::string async_exception_result() {
    return "{\"ok\":false,\"uploaded\":false,\"status\":0,"
           "\"error\":\"后台提交失败\",\"message\":\"后台提交失败\"}";
}

} // namespace detail

// 本地 HTTP 层应把 accepted=true 映射为 202，并用 job 轮询 status。
inline std::string web_submit_start(const std::string& body, const LiveState& live,
                                    const Options& optIn = Options{}) {
    const rta::net::Json json = rta::net::json_parse(body);
    UserReport user;
    user.type = json["type"].s();
    user.title = json["title"].s();
    user.body = json["body"].s();
    user.steps = json["steps"].s();
    if (user.title.empty() && user.body.empty())
        return "{\"ok\":false,\"accepted\":false,\"error\":\"标题和正文不能都为空\"}";
    LiveState snapshot = live;
    if (snapshot.source.empty()) snapshot.source = "rtaweb";
    const Options options = request_options(json, optIn);
    auto& registry = detail::async_registry();
    auto job = std::make_shared<detail::AsyncJob>();
    std::string id;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        detail::prune_async_jobs_locked(registry, std::chrono::steady_clock::now());
        if (registry.jobs.size() >= kAsyncMaxJobs)
            return "{\"ok\":false,\"accepted\":false,\"error\":\"反馈提交队列已满，请稍后重试\"}";
        try {
            do { id = "job-" + secure_random_hex(8); } while (registry.jobs.count(id));
        } catch (...) {
            return "{\"ok\":false,\"accepted\":false,\"error\":\"无法创建反馈任务\"}";
        }
        registry.jobs.emplace(id, job);
    }
    try {
        std::thread worker([&registry, id, job, user, snapshot, options]() {
            std::string result;
            try {
                result = result_json(submit(user, snapshot, options));
            } catch (...) {
                result = detail::async_exception_result();
            }
            std::lock_guard<std::mutex> lock(registry.mutex);
            job->result = std::move(result);
            job->completed = std::chrono::steady_clock::now();
            job->done = true;
        });
        worker.detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.jobs.erase(id);
        return "{\"ok\":false,\"accepted\":false,\"error\":\"无法启动反馈任务\"}";
    }
    return std::string("{\"ok\":true,\"accepted\":true,\"job\":") + jq(id) + "}";
}

inline std::string web_submit_status(const std::string& id) {
    auto& registry = detail::async_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    detail::prune_async_jobs_locked(registry, std::chrono::steady_clock::now());
    const auto it = registry.jobs.find(id);
    if (it == registry.jobs.end())
        return "{\"ok\":false,\"done\":true,\"error\":\"job 不存在或已过期\"}";
    if (!it->second->done) return "{\"ok\":true,\"done\":false}";
    return std::string("{\"ok\":true,\"done\":true,\"result\":") + it->second->result + "}";
}

} // namespace rta::feedback
