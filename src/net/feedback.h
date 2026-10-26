// feedback.h — 「一键反馈」：诊断快照 + 用户手打文字 → 单个 JSON → 上传到 GitHub 仓库
//
// 用户 261027-01 #1（2026-10-27）：把「设置-诊断」升级成「一键反馈报错 / 向我们提意见」。
// MAC 侧原本说已做完上报，但用户随后确认「MAC 那边的代码还没发上来，我们自己实现」⟹ 本文件从零写。
//
// 设计取舍（为什么是"一个 JSON 进仓库"而不是 issue）：
//   * 仓库私有 ⟹ 反馈不公开；一个目录 feedback/ 里堆 JSON，git log 就是时间线；
//   * JSON 自带 schema 版本 + 环境快照 + 日志尾巴 ⟹ 未来 DEBUG 时不用再问用户要日志；
//   * 不依赖任何第三方服务/账号（只用 GitHub Contents API），离线时退化成本地文件。
//
// ===========================================================================
// 【rtaweb.cpp 接入点】本文件不改 rtaweb.cpp（别的代理正在改它）。主人只需加 3 处：
//
// ① 文件顶部，其它 #include "net/..." 旁边（按字母序放在 net/emby.h 附近）：
//      #include "net/feedback.h"
//
// ② 副作用端点白名单 —— static bool is_side_effect_path() 里那张 kNeedPost[] 数组：
//    这个端点会打网络 + 往仓库写文件，必须只接受 POST（否则 <img src=...> 就能被页面触发）：
//      "/api/feedback",
//
// ③ HTTP 路由 —— handle 函数里 /api/diag 那个大 if 块**结束之后**、其它 /api/* 之前
//    （/api/feedback 与 /api/diag 无前缀冲突，但两者都必须排在"静态资源兜底"之前）：
//
//      if (rq.path.rfind("/api/feedback", 0) == 0) {
//          ct = "application/json; charset=utf-8";
//          rta::feedback::LiveState lv;
//          lv.source = "rtaweb";
//          {
//              std::lock_guard<std::mutex> lk(ps.mtx);      // ps = 现成的 g_ps，字段必须持锁读
//              lv.playing  = ps.playing.load();
//              lv.paused   = ps.paused.load();
//              lv.track    = ps.current;
//              lv.lastError= ps.error;
//              lv.outMode  = ps.outMode;
//              lv.outRate  = (long long)ps.outRate;
//              lv.outBits  = ps.outBits;
//          }
//          // 下面两行是**可选增强**：留空也照样有一份"conf 快照 + 空端点数组"，
//          // 但塞进来更利于 DEBUG（settings 会解析→逐字段脱敏→重新序列化）。
//          lv.settingsJson  = ...;   // 例如 /api/settings 的响应串
//          lv.endpointsJson = ...;   // 例如 /api/diag 响应里 "endpoints": [...] 那一段
//          return rta::feedback::web_handler(rq.body, lv);
//      }
//
//    （rq.body 形如 {"type":"bug","title":"…","body":"…","steps":"…"}；返回 JSON 里
//      uploaded / status / path / sha / local / error / message 七个字段可直接显示。）
//
// ④ UI 侧（归 UI 组，改 src/net/ui_script.h）：设置-诊断页那两个按钮改成
//    api('feedback', {type:'bug'|'idea'|'other', title, body, steps})，POST 即可。
//    无凭据时返回 uploaded=false + local=<本地文件绝对路径>，界面要把它显示出来。
// ===========================================================================
//
// 凭据（三级回退，任一命中即可上传）：
//   ① 环境变量 GITHUB_TOKEN / RLPLAYER_GH_TOKEN
//   ② exe 同目录 rlplayer-feedback.conf 里的 token= （用户自己填；值永不落盘/回显）
//   ③ 执行 gh auth token （本机装了 GitHub CLI 且已登录时可用）
//   全都没有 ⟹ 不上传，把 JSON 落到 exe 同目录 feedback-outbox\ 并明确告知路径。
//
// 纪律：
//   * 这里只做"打包 + 上传"，不采集音频、不开设备、不改系统状态；
//   * 任何 token 只在内存里过一手，绝不写进 payload / 日志 / 本地文件 / 返回值；
//   * 网络失败不阻塞超过 totalBudgetMs（默认 14 s，其中 gh 取 token ≤ 2.5 s、HTTP ≤ 11 s）。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <psapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crash_dump.h"          // rta::diag::os_version_string / cpu_brand / read_tester（复用，不重写）
#include "net/json.h"            // 只读 JSON 解析（解析调用方给的片段 + GitHub 响应）
#include "platform/path.h"       // module_dir_utf8 / join / from_utf8
#include "platform/process.h"    // run_hidden（调 gh auth token）
#include "version.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

namespace rta::feedback {

// 表单结构和语义变了就 +1。老 JSON 仍然可读（字段只增不删）。
constexpr int kSchema = 1;

// 缺省目标仓库（用户 261027-01：反馈存进"我们当前 github 仓库"的 feedback/ 目录）
constexpr const char* kRepoDefault   = "okhuoyanyan/RLPlayer";
constexpr const char* kBranchDefault = "main";
constexpr const char* kDirDefault    = "feedback";

// ---------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------

// 用户输入（UI 表单 / CLI 参数）
struct UserReport {
    std::string type;      // bug | idea | other（大小写、中文都容忍）
    std::string title;
    std::string body;
    std::string steps;     // 可选：复现步骤
};

// 调用方（rtaweb）能提供的现场状态。全部可空 —— CLI 里就是空的。
struct LiveState {
    std::string source = "feedback_probe";   // rtaweb | feedback_probe | 其他
    bool playing = false;
    bool paused  = false;
    std::string track;        // 当前曲目（路径或标题；打包时脱敏）
    std::string lastError;    // ps.error
    std::string backend;      // wasapi | wasapi-shared | asio | upnp
    std::string device;       // 输出端点名或 id
    std::string outMode;      // pcm | dop | native
    long long   outRate = 0;
    int         outBits = 0;
    std::string gpu;          // 例如 "可用(ROCm)"；空 = 本进程自己探
    std::string settingsJson;    // 可选：整段设置 JSON（会解析→脱敏→重新序列化）
    std::string endpointsJson;   // 可选：端点数组 JSON（同上）
    std::string extraJson;       // 可选：任意附加对象（同上）
};

struct Options {
    bool dryRun = false;                       // 只打包不上传
    std::string repo   = kRepoDefault;
    std::string branch = kBranchDefault;
    std::string outboxDir;                     // 空 = <exe>\feedback-outbox
    std::vector<std::pair<std::string, std::string>> logFiles;   // (显示名, 路径)；空 = 默认两份
    size_t logLines = 200, logMaxBytes = 128 * 1024;
    size_t extraLogLines = 80, extraLogMaxBytes = 32 * 1024;     // 第二份日志（launcher.log）
    int timeoutMs     = 11000;                 // 单次 HTTP 预算
    int totalBudgetMs = 14000;                 // 总预算（含取 token），硬上限 15 s
    int ghTimeoutMs   = 2500;                  // gh auth token 的预算
    std::string proxy;                         // "" = 自动（系统代理）；"none" = 强制直连
    bool skipTokenLookup = false;              // 测试用：模拟"三个来源都没有"（走降级路径）
    bool keepLocalCopy = false;                // 上传成功也留一份本地副本
};

struct Result {
    bool   uploaded = false;
    int    status   = 0;          // HTTP 状态码（0 = 没发出请求）
    std::string id;               // 20261027-024012-a1b2c3
    std::string remotePath;       // feedback/<id>.json
    std::string sha;              // 文件 blob sha
    std::string commitSha;
    std::string localPath;        // 降级（或 keepLocalCopy）时的本地绝对路径
    std::string error;            // 人话错误（空 = 没错）
    std::string message;          // 给用户看的一句话结论
    std::string tokenSource;      // env | conf | gh | ""（只报来源，不报值）
    std::string json;             // 实际打包的 payload
    size_t      bytes = 0;
    long long   elapsedMs = 0;
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
        while (e < s.size()) {
            const char c = s[e];
            if (c == '\r' || c == '\n' || c == '"' || c == ',' || c == '}' || c == ';') break;
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
               s[e] != ' ' && s[e] != '\r' && s[e] != '\n') ++e;
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
        const bool looksMail = (b > p + 2) && (dot != std::string::npos) && (dot < b) && (p - a >= 2);
        if (looksMail && s.compare(a, p - a, "<user>") != 0) {
            replace_range(s, a, p, "<user>");
            at = a + 7;
        } else {
            at = p + 1;
        }
    }
}

// 公开入口：任何要进 payload 的文本都必须过它
inline std::string redact(const std::string& in) {
    std::string s = in;
    mask_prefixed_tokens(s);
    mask_header_values(s);
    mask_bearer(s);
    static const char* kKeys[] = {"api_key", "apikey", "api-key", "x-api-key", "token",
                                  "access_token", "refresh_token", "password", "passwd",
                                  "secret", "client_secret", "auth", "key", "gh_token"};
    for (const char* k : kKeys) mask_kv(s, k);
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

inline void emit_value(const rta::net::Json& v, std::string& o) {
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
                emit_value(kv.second, o);
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
        st = (unsigned long long)GetTickCount64() * 6364136223846793005ULL +
             (unsigned long long)GetCurrentProcessId() * 1442695040888963407ULL;
        if (st == 0) st = 88172645463325252ULL;
    }
    st ^= st << 13; st ^= st >> 7; st ^= st << 17;
    return (unsigned)((st >> 16) & 0xFFFFFFFFull);
}

inline Stamp make_stamp() {
    Stamp s;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[64];
    std::snprintf(b, sizeof(b), "%04u%02u%02u-%02u%02u%02u", (unsigned)st.wYear, (unsigned)st.wMonth,
                  (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    char r[8];
    std::snprintf(r, sizeof(r), "%06x", rnd32() & 0xFFFFFFu);
    s.id = std::string(b) + "-" + r;
    std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02u", (unsigned)st.wYear, (unsigned)st.wMonth,
                  (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    TIME_ZONE_INFORMATION tzi{};
    const DWORD tz = GetTimeZoneInformation(&tzi);
    const long bias = tzi.Bias + (tz == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : tzi.StandardBias);
    const long east = -bias;                       // 分钟，东为正
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
    std::vector<std::string> names;
    for (DWORD i = 0; i < 8; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        char u8[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, dd.DeviceString, -1, u8, (int)sizeof(u8) - 1, nullptr, nullptr);
        const std::string n(u8);
        if (n.empty()) continue;
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    }
    out.clear();
    for (size_t i = 0; i < names.size(); ++i) { if (i) out += " / "; out += names[i]; }
}

// 进程已运行秒数。
// 坑：GetProcessTimes 的 lpExitTime 对**仍在运行**的进程不可用（实测拿到 0），
// 拿它减低 64 位会回绕成 1.8e12 秒。所以改成"现在（FILETIME 时基）- 创建时间"。
inline unsigned long long proc_uptime_sec() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER cur, born;
    cur.LowPart = now.dwLowDateTime; cur.HighPart = now.dwHighDateTime;
    born.LowPart = c.dwLowDateTime;  born.HighPart = c.dwHighDateTime;
    if (cur.QuadPart <= born.QuadPart) return 0;
    return (unsigned long long)((cur.QuadPart - born.QuadPart) / 10000000ULL);
}

struct MemInfo {
    unsigned long long wsMb = 0, peakMb = 0, privMb = 0, ramTotalMb = 0, ramAvailMb = 0;
    unsigned handles = 0;
};

inline MemInfo mem_info() {
    MemInfo m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        m.wsMb   = (unsigned long long)pmc.WorkingSetSize / (1024ull * 1024ull);
        m.peakMb = (unsigned long long)pmc.PeakWorkingSetSize / (1024ull * 1024ull);
        m.privMb = (unsigned long long)pmc.PrivateUsage / (1024ull * 1024ull);
    }
    DWORD h = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &h)) m.handles = (unsigned)h;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.ramTotalMb = (unsigned long long)ms.ullTotalPhys / (1024ull * 1024ull);
        m.ramAvailMb = (unsigned long long)ms.ullAvailPhys / (1024ull * 1024ull);
    }
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
        o += jq(k) + ":" + jq(v);
        ++n;
    }
    o += "}";
    return o;
}

// ---------------------------------------------------------------------------
// 凭据（三级回退）。返回值只有"来源名"，token 本体不离开本函数的作用域。
// ---------------------------------------------------------------------------
struct TokenPick {
    std::string token;
    std::string source;   // env | conf | gh
};

inline std::string strip_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline bool looks_like_token(const std::string& t) {
    if (t.size() < 20 || t.size() > 512) return false;
    for (unsigned char c : t)
        if (c <= 0x20 || c >= 0x7F) return false;      // token 一定是可见 ASCII
    return true;
}

// 走 GetEnvironmentVariableW 而不是 std::getenv：后者在 MSVC/clang-cl 上是 deprecated，
// 而且只看得懂 ANSI 代码页。token 是纯 ASCII，但统一走宽 API 更省事。
inline std::string token_from_env() {
    const wchar_t* names[2] = {L"GITHUB_TOKEN", L"RLPLAYER_GH_TOKEN"};
    for (const wchar_t* k : names) {
        wchar_t buf[1024];
        const DWORD n = ::GetEnvironmentVariableW(k, buf, 1024);
        if (n == 0 || n >= 1024) continue;
        const int m = ::WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
        if (m <= 0) continue;
        std::string t((size_t)m, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, t.data(), m, nullptr, nullptr);
        t = strip_trim(t);
        if (looks_like_token(t)) return t;
    }
    return "";
}

// rlplayer-feedback.conf：纯文本 key=value；认 token= / gh_token= / github_token=
inline std::string token_from_conf(std::string& where) {
    where = rta::platform::join(exe_dir(), "rlplayer-feedback.conf");
    std::ifstream f(rta::platform::from_utf8(where), std::ios::binary);
    if (!f) return "";
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t e = line.find('=');
        if (e == std::string::npos) continue;
        const std::string k = strip_trim(line.substr(0, e));
        if (k != "token" && k != "gh_token" && k != "github_token") continue;
        const std::string v = strip_trim(line.substr(e + 1));
        if (looks_like_token(v)) return v;
    }
    return "";
}

// gh auth token：run_hidden 会 INFINITE 等待，所以放到一次性线程里、只等 ghTimeoutMs。
// 超时就把线程 detach 掉（绝不 kill 用户进程），本次当"没有凭据"处理。
inline std::string token_from_gh(int timeoutMs) {
    const std::string exe = rta::platform::find_executable("gh");
    if (exe.empty()) return "";
    struct Cookie {
        std::mutex mx;
        std::string out;
        bool done = false;
    };
    auto ck = std::make_shared<Cookie>();
    std::thread th([ck, exe]() {
        const std::string cmd = std::string("\"") + exe + "\" auth token";
        const rta::platform::ProcessResult pr = rta::platform::run_hidden(cmd, true);
        std::string o = (pr.exit_code == 0) ? strip_trim(pr.output) : std::string();
        std::lock_guard<std::mutex> lk(ck->mx);
        ck->out = o;
        ck->done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(ck->mx);
            if (ck->done) {
                th.join();
                return looks_like_token(ck->out) ? ck->out : std::string();
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) { th.detach(); return ""; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

inline TokenPick pick_token(const Options& opt) {
    TokenPick t;
    if (opt.skipTokenLookup) return t;
    const std::string fromEnv = token_from_env();
    if (!fromEnv.empty()) { t.token = fromEnv; t.source = "env"; return t; }
    std::string where;
    const std::string fromConf = token_from_conf(where);
    if (!fromConf.empty()) { t.token = fromConf; t.source = "conf"; return t; }
    const std::string fromGh = token_from_gh(opt.ghTimeoutMs);
    if (!fromGh.empty()) { t.token = fromGh; t.source = "gh"; return t; }
    return t;
}

// ---------------------------------------------------------------------------
// GitHub Contents API PUT
//
// 为什么要自己走 WinHTTP：平台层 src/platform/http.h 的 method 只有 GET/POST 两档
// （那是另一个文件的契约，本工作包不改它），而 /contents 只接受 PUT。
// 预算：先试系统代理（AUTOMATIC_PROXY），失败再试直连；两次共享 opt.timeoutMs。
// ---------------------------------------------------------------------------
struct PutResult {
    bool ok = false;
    int  status = 0;
    std::string error;     // 人话
    std::string body;
};

inline std::string winhttp_reason(DWORD e) {
    switch (e) {
        case 12001: return "内存/缓冲区不足";
        case 12002: return "请求超时（网络太慢或被墙）";
        case 12005: return "URL 不合法";
        case 12006: return "网络不可用";
        case 12007: return "无法解析主机名（DNS 失败，检查网络）";
        case 12008: return "代理或服务器地址被拒绝";
        case 12017: return "操作被取消";
        case 12019: return "连接被服务器断开";
        case 12029: return "无法连接服务器（防火墙/代理/断网）";
        case 12030: return "连接被拒绝或中断";
        case 12152: return "服务器返回了无法解析的响应";
        case 12175: return "TLS/证书校验失败";
        default:     return "WinHTTP 错误 " + std::to_string((unsigned long)e);
    }
}

struct UrlParts {
    bool https = false;
    std::wstring host;
    int port = 0;
    std::wstring path;
    bool valid = false;
};

inline std::wstring utf8_to_wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

inline UrlParts parse_url(const std::string& url) {
    UrlParts u;
    size_t p = 0;
    if (url.rfind("https://", 0) == 0) { u.https = true; p = 8; }
    else if (url.rfind("http://", 0) == 0) { u.https = false; p = 7; }
    else return u;
    const size_t slash = url.find('/', p);
    const std::string hp = (slash == std::string::npos) ? url.substr(p) : url.substr(p, slash - p);
    const std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);
    std::string host = hp;
    int port = u.https ? 443 : 80;
    const size_t colon = hp.rfind(':');
    if (colon != std::string::npos) {
        host = hp.substr(0, colon);
        port = std::atoi(hp.c_str() + colon + 1);
    }
    if (host.empty() || port <= 0) return u;
    u.host = utf8_to_wide(host);
    u.path = utf8_to_wide(path);
    u.port = port;
    u.valid = !u.host.empty() && !u.path.empty();
    return u;
}

// proxyMode: 0 = 自动（系统代理）、1 = 直连、2 = 具名代理
inline PutResult http_put_once(const std::string& url, const std::vector<std::string>& headers,
                               const std::string& body, int timeoutMs, int proxyMode,
                               const std::string& proxy) {
    PutResult r;
    const UrlParts u = parse_url(url);
    if (!u.valid) { r.error = "URL 解析失败"; return r; }

    const std::wstring wproxy = (proxyMode == 2) ? utf8_to_wide(proxy) : std::wstring();
    const DWORD access = (proxyMode == 1) ? WINHTTP_ACCESS_TYPE_NO_PROXY
                       : (proxyMode == 2) ? WINHTTP_ACCESS_TYPE_NAMED_PROXY
                                          : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    HINTERNET ses = ::WinHttpOpen(L"RLPlayer-feedback/1.0", access,
                                  (proxyMode == 2) ? wproxy.c_str() : WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { r.error = winhttp_reason(::GetLastError()); return r; }
    ::WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET con = ::WinHttpConnect(ses, u.host.c_str(), (INTERNET_PORT)u.port, 0);
    if (!con) { r.error = winhttp_reason(::GetLastError()); ::WinHttpCloseHandle(ses); return r; }

    HINTERNET hreq = ::WinHttpOpenRequest(con, L"PUT", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hreq) {
        r.error = winhttp_reason(::GetLastError());
        ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
        return r;
    }
    for (const auto& h : headers) {
        const std::wstring w = utf8_to_wide(h);
        if (!w.empty()) ::WinHttpAddRequestHeaders(hreq, w.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    const bool sent = ::WinHttpSendRequest(hreq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           (LPVOID)body.data(), (DWORD)body.size(),
                                           (DWORD)body.size(), 0) != 0 &&
                      ::WinHttpReceiveResponse(hreq, nullptr) != 0;
    if (!sent) {
        r.error = winhttp_reason(::GetLastError());
        ::WinHttpCloseHandle(hreq); ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
        return r;
    }
    DWORD code = 0, len = sizeof(code);
    ::WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)code;
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(hreq, &avail) || avail == 0) break;
        std::vector<char> buf((size_t)avail);
        DWORD got = 0;
        if (!::WinHttpReadData(hreq, buf.data(), avail, &got) || got == 0) break;
        r.body.append(buf.data(), (size_t)got);
    }
    ::WinHttpCloseHandle(hreq); ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    return r;
}

inline std::string http_status_hint(int st) {
    switch (st) {
        case 401: return "凭据无效或已过期（在 GitHub 上重新生成 token，写进 rlplayer-feedback.conf）";
        case 403: return "被拒绝：token 缺 repo 写权限，或触发了 GitHub 限流";
        case 404: return "仓库/分支不存在，或这个 token 看不到该私有仓库";
        case 409: return "冲突：同名文件已存在（重试即可，文件名带随机后缀）";
        case 422: return "GitHub 认为内容不合法（文件过大或文件名非法）";
        default:  return "GitHub 返回 HTTP " + std::to_string(st);
    }
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
        "全部字符串都已脱敏，可以直接贴到 issue 里");

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
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    o += ",\"cpu_threads\":" + std::to_string((unsigned long long)si.dwNumberOfProcessors);
    o += ",\"gpu\":" + jq(gpu);
    o += ",\"ram_total_mb\":" + std::to_string(mi.ramTotalMb);
    o += ",\"ram_avail_mb\":" + std::to_string(mi.ramAvailMb);
    o += ",\"exe_dir\":" + jq(dir);
    o += "}";

    o += ",\"process\":{";
    o += "\"pid\":" + std::to_string((unsigned long long)GetCurrentProcessId());
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

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
inline Result submit(const UserReport& u, const LiveState& live, const Options& opt) {
    Result r;
    const ULONGLONG t0 = GetTickCount64();
    const Payload pl = build_payload(u, live, opt);
    r.id = pl.id;
    r.json = pl.json;
    r.bytes = pl.json.size();
    r.remotePath = std::string(kDirDefault) + "/" + pl.id + ".json";

    if (opt.dryRun) {
        r.message = "dry-run：只生成 JSON，未上传";
        r.elapsedMs = (long long)(GetTickCount64() - t0);
        return r;
    }
    if (u.title.empty() && u.body.empty()) {
        r.error = "标题和正文不能都为空";
        r.message = r.error;
        r.elapsedMs = (long long)(GetTickCount64() - t0);
        return r;
    }

    // ① 凭据
    const TokenPick tp = pick_token(opt);
    r.tokenSource = tp.source;
    if (tp.token.empty()) {
        r.localPath = outbox_write(pl.id, pl.json, opt.outboxDir);
        r.error = "没有可用的上传凭据（环境变量 GITHUB_TOKEN / exe 同目录 rlplayer-feedback.conf 的 "
                  "token / 已登录的 gh CLI，都没找到）";
        r.message = r.localPath.empty()
            ? (r.error + "；本地兜底也没写成，请检查 exe 目录是否可写")
            : ("没上传（缺凭据）。反馈已保存到本地：" + r.localPath);
        r.elapsedMs = (long long)(GetTickCount64() - t0);
        return r;
    }

    // ② 组装请求
    std::string commitMsg = std::string("feedback(") + RLPLAYER_VERSION + "): " +
                            type_label(type_norm(u.type)) + " - ";
    {
        std::string t = u.title.empty() ? u.body : u.title;
        for (size_t i = 0; i < t.size(); ++i)
            if (t[i] == '\r' || t[i] == '\n' || t[i] == '\t') t[i] = ' ';
        if (t.size() > 70) t = t.substr(0, 70) + "…";
        commitMsg += t;
    }
    commitMsg += " [" + pl.id + "]";

    const std::string url = "https://api.github.com/repos/" + opt.repo + "/contents/" +
                            std::string(kDirDefault) + "/" + pl.id + ".json";
    const std::string body = "{\"message\":" + jq(commitMsg) + ",\"content\":\"" +
                             base64_encode(pl.json) + "\",\"branch\":" + jq(opt.branch) + "}";
    std::vector<std::string> hdrs;
    hdrs.push_back("Authorization: Bearer " + tp.token);
    hdrs.push_back("Accept: application/vnd.github+json");
    hdrs.push_back("X-GitHub-Api-Version: 2022-11-28");
    hdrs.push_back("User-Agent: RLPlayer-feedback/1.0");
    hdrs.push_back("Content-Type: application/json; charset=utf-8");

    // ③ PUT：先系统代理、失败再直连（用户的 Clash 挂着或没挂都能通）
    //
    // 预算分配：取 token 已经花掉的时间要扣掉，剩下的分给两次尝试 —— 这样"总耗时不超过
    // totalBudgetMs（默认 14 s）"是算出来的，不是口号。下限 3 s：给慢网留活路，
    // 此时最坏 totalBudgetMs + 3 s，仍 < 15 s。
    int mode = 2;
    if (opt.proxy.empty()) mode = 0;
    else if (opt.proxy == "none") mode = 1;
    const long long spentMs = (long long)(GetTickCount64() - t0);
    const long long leftMs = (long long)opt.totalBudgetMs - spentMs;
    const int perTry = (int)std::max<long long>(3000, std::min<long long>(opt.timeoutMs, leftMs) / 2);
    PutResult pr = http_put_once(url, hdrs, body, perTry, mode, opt.proxy);
    if (!pr.ok && opt.proxy.empty()) {
        const PutResult pr2 = http_put_once(url, hdrs, body, perTry, 1, "");
        if (pr2.ok || pr2.status != 0) {
            if (!pr2.ok && pr2.status != 0) pr = pr2;
        } else if (pr.status == 0) {
            pr.error += "（直连也不行：" + pr2.error + "）";
        }
    }
    r.status = pr.status;

    if (!pr.ok) {
        r.error = (pr.status != 0) ? http_status_hint(pr.status) : ("网络请求失败：" + pr.error);
        r.localPath = outbox_write(pl.id, pl.json, opt.outboxDir);
        r.message = r.localPath.empty()
            ? (r.error + "；本地兜底也没写成，请检查 exe 目录是否可写")
            : ("没上传成功（" + r.error + "）。反馈已保存到本地：" + r.localPath);
        r.elapsedMs = (long long)(GetTickCount64() - t0);
        return r;
    }

    // ④ 解析 GitHub 响应（content.sha / commit.sha）
    {
        const rta::net::Json j = rta::net::json_parse(pr.body);
        const std::string path = j["content"]["path"].s();
        const std::string sha = j["content"]["sha"].s();
        const std::string csha = j["commit"]["sha"].s();
        if (!path.empty()) r.remotePath = path;
        r.sha = sha;
        r.commitSha = csha;
    }
    r.uploaded = true;
    r.message = "已上传，感谢反馈！文件：" + r.remotePath;
    if (opt.keepLocalCopy) r.localPath = outbox_write(pl.id, pl.json, opt.outboxDir);
    r.elapsedMs = (long long)(GetTickCount64() - t0);
    return r;
}

// ---------------------------------------------------------------------------
// 给 rtaweb 的 HTTP 端点实现（3 行接入，见文件头）
// ---------------------------------------------------------------------------
inline std::string result_json(const Result& r) {
    std::string o = "{";
    o += "\"ok\":" + std::string(r.uploaded ? "true" : "false");
    o += ",\"uploaded\":" + std::string(r.uploaded ? "true" : "false");
    o += ",\"status\":" + std::to_string(r.status);
    o += ",\"id\":" + jq(r.id);
    o += ",\"path\":" + jq(r.remotePath);
    o += ",\"sha\":" + jq(r.sha);
    o += ",\"commit\":" + jq(r.commitSha);
    o += ",\"local\":" + jq(r.localPath);
    o += ",\"error\":" + jq(r.error);
    o += ",\"message\":" + jq(r.message);
    o += ",\"bytes\":" + std::to_string((unsigned long long)r.bytes);
    o += ",\"elapsed_ms\":" + std::to_string(r.elapsedMs);
    o += ",\"token_source\":" + jq(r.tokenSource);
    o += "}";
    return o;
}

// body 形如 {"type":"bug","title":"…","body":"…","steps":"…"}（都容忍缺失）
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
    return result_json(submit(u, lv, optIn));
}

} // namespace rta::feedback
