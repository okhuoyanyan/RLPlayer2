// llm.h — DeepSeek 客户端（chat / search / fetch）
//
// 用途（docs/元数据补全调研与规划.md §1.4 / §3）：
//   * POST {base}/chat/completions — 标签解析兜底、结构化输出（model 默认 deepseek-flash）
//   * POST {base}/v1/tools/search  — 联网检索（results[]: title/url/snippet/summary/
//                                    rerank_score/authority/date；过滤策略在调用方做）
//   * POST {base}/v1/tools/fetch   — 拉取网页正文（title + Markdown）
//
// 设计要点：
//   * api_key 只用于 Authorization 请求头。任何错误消息在返回前都过一遍 llm_redact()，
//     即使服务端把 key 回显进错误文本也会被替换成 ****（硬规则：错误里不得出现 key）
//   * JSON 解析只用 net/json.h；JSON 写出用 ostringstream 手工拼接 + 自带 llm_esc() 转义
//     （\ " \n \r \t 与 <0x20 → \u00xx；中文保持原样 UTF-8）
//   * 网络三个函数（llm_chat / ds_search / ds_fetch）内部各做一次调用、不重试；
//     重试 / 限频 / 熔断归上层（调研文档 §3「网络纪律」）
//   * 可测试性：body 构造与响应解析全部拆成纯函数，离线喂写死报文即可测（铁律 3）
#pragma once
// （原 WIN32_LEAN_AND_MEAN / NOMINMAX 前导已删：http_client.h -> platform/http.h 会在
//   windows.h 之前自己定义，行为不变。）
#include "net/http_client.h"
#include "net/json.h"
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <cstdlib>   // std::getenv（思考强度 A/B 开关）
#include <iterator>
#include <functional>
#include <ctime>
#include <chrono>
// 2026-10-25（并发记账 P0）：用量统计的读写需要进程内互斥 + 唯一 tmp 名 + 写失败重试
#include <mutex>
#include <thread>
#include <atomic>
#include <random>
#include <set>
#if defined(_WIN32)
#include <process.h>   // _getpid()
#else
#include <unistd.h>    // getpid()
#endif

namespace rta::meta {

// ---------------------------------------------------------------------------
// 小工具：trim / JSON 转义 / key 脱敏
// ---------------------------------------------------------------------------

inline std::string llm_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// JSON 字符串转义。注意：这里按本模块 spec 独立实现（\r 输出 \r 而不是像 rtaweb 的
// jesc 那样丢弃），中文等 >=0x80 的字节原样透传（UTF-8 本身就是合法 JSON 字符串内容）。
// 2026-10-24（261024-02 #1）：把非法 UTF-8 字节清成 '?'。
// 现象：艺术家资料偶发 "HTTP 400: 响应不是合法 JSON" —— 服务端收到非法 UTF-8 的 JSON body
// 时直接回纯文本 400，我们的解析器只能报"响应不是合法 JSON"。抓到的材料（网页正文）可能带
// GBK/截断字节，所以发请求前统一清洗一遍。
inline std::string llm_sanitize_utf8(const std::string& s) {
    std::string o; o.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        size_t len = 0;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else { o += '?'; ++i; continue; }
        if (i + len > s.size()) { o += '?'; ++i; continue; }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) { o += '?'; ++i; continue; }
        o.append(s, i, len);
        i += len;
    }
    return o;
}

inline std::string llm_esc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    char b[8];
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// 把 secret（api_key）在文本中的所有出现替换成 ****。
// 所有 error / 日志字符串在返回前都必须过一遍，保证 key 不外泄。
inline std::string llm_redact(const std::string& secret, const std::string& s) {
    if (secret.empty() || s.find(secret) == std::string::npos) return s;
    std::string out; out.reserve(s.size());
    size_t pos = 0;
    for (;;) {
        const size_t hit = s.find(secret, pos);
        if (hit == std::string::npos) { out += s.substr(pos); break; }
        out += s.substr(pos, hit - pos);
        out += "****";
        pos = hit + secret.size();
    }
    return out;
}

// 给界面看的掩码：只暴露前缀与末 4 位（形如 sk-****4872）。
// 界面【永不】回显明文：/api/llm 的 GET 只返回这个字符串，POST 也只写不读。
// 短 key 连前缀都不给 —— 否则 12 位里露出 7 位，等于泄了大半。
inline std::string llm_mask_key(const std::string& k) {
    if (k.empty()) return std::string();
    if (k.size() < 16) return "****";
    return k.substr(0, 3) + "****" + k.substr(k.size() - 4);
}

// baseUrl 规整：去首尾空白与结尾斜杠；空则回退默认端点
inline std::string llm_base_url(const std::string& raw) {
    std::string b = llm_trim(raw);
    if (b.empty()) b = "https://api.deepseek.com";
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    return b;
}

// ---------------------------------------------------------------------------
// 流式（SSE）输出的回调类型 —— 用户反馈 261024-05「LLM 做流式比较好，不用枯等」
//
//   LlmDeltaFn         : 模型增量回调。每个 SSE delta 调一次；**可能在网络线程上**（见下文）。
//   LlmChunkSink       : 传输层分块回调。每从 socket 读到一段原始字节调一次。
//   LlmStreamTransport : 支持「边收边吐」的 POST。比 transport 多一个 on_chunk 参数，
//                        status/error 语义与 transport 完全一致；body 只放未经 on_chunk 投递的
//                        剩余字节（通常为空）。为空时 llm_chat_stream 退回 transport/http_post。
//                        **硬约定**：on_chunk 只允许在 streamTransport 返回前调用 —— 返回后再调，
//                        接收者（retrieval 的 progress 对象）可能已经析构，属于违约用法。
// ---------------------------------------------------------------------------
using LlmDeltaFn = std::function<void(const std::string& delta)>;
using LlmChunkSink = std::function<void(const char* data, size_t n)>;
using LlmStreamTransport = std::function<rta::net::HttpResponse(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& headers,
    const LlmChunkSink& on_chunk)>;

// ---------------------------------------------------------------------------
// 配置（rlplayer-llm.conf：纯文本 key=value，# 注释，空行忽略）
// ---------------------------------------------------------------------------

struct LlmConfig {
    std::string baseUrl = "https://api.deepseek.com";
    std::string apiKey;                    // 只进请求头；缺文件时为空
    std::string model   = "deepseek-flash";
    int monthlyCap = 0;                    // 设置键 llmmonthlycap（0=不限，默认 0）
    std::string usagePath;                 // 用量文件路径，为空则自动定位
    std::string monthOverride;             // 测试用：模拟当前月份（如 "202611"）
    // mock transport 钩子（测试用，离线无网络）
    std::function<rta::net::HttpResponse(const std::string& url,
                                         const std::string& body,
                                         const std::vector<std::string>& headers)> transport;
    // 流式传输钩子（可选，真流式用）。为空时 llm_chat_stream 退回 transport / http_post：
    // 拿完整 body 再按 SSE 增量解析 —— 解析结果与回调时序完全一样，只是拿不到"边收边吐"
    // 带来的时延收益（整段响应到达后回调会一次性补齐）。接线见交付报告的「未验证项」。
    LlmStreamTransport streamTransport;
};

// 缺文件 → 返回默认值（空 key）。key 名：base_url / api_key / model / llmmonthlycap；其他键忽略。
inline LlmConfig llm_config_load(const std::string& path = "rlplayer-llm.conf") {
    LlmConfig c;
    std::ifstream f(path, std::ios::binary);
    if (f) {
        const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string line;
        for (size_t i = 0; i <= all.size(); ++i) {
            if (i == all.size() || all[i] == '\n') {
                while (!line.empty() && line.back() == '\r') line.pop_back();
                const std::string t = llm_trim(line);
                line.clear();
                if (t.empty() || t[0] == '#') continue;      // 注释与空行
                const size_t eq = t.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = llm_trim(t.substr(0, eq));
                const std::string v = llm_trim(t.substr(eq + 1));   // 值做 trim
                if      (k == "base_url") c.baseUrl = v;
                else if (k == "api_key")  c.apiKey  = v;
                else if (k == "model")    c.model   = v;
                else if (k == "llmmonthlycap" || k == "monthly_cap" || k == "monthlycap") c.monthlyCap = atoi(v.c_str());
                else if (k == "usage_path") c.usagePath = v;
            } else {
                line += all[i];
            }
        }
    }
    // 若 conf 里未写 monthlyCap，尝试从同级或全局 rlplayer-settings.conf 补充
    if (c.monthlyCap == 0) {
        std::error_code ec;
        std::filesystem::path p(path);
        std::string sp = (p.parent_path() / "rlplayer-settings.conf").string();
        if (sp == "rlplayer-settings.conf" || sp.empty()) sp = "rlplayer-settings.conf";
        std::ifstream sf(sp, std::ios::binary);
        if (sf) {
            std::string sline;
            while (std::getline(sf, sline)) {
                while (!sline.empty() && sline.back() == '\r') sline.pop_back();
                const size_t eq = sline.find('=');
                if (eq != std::string::npos) {
                    const std::string sk = llm_trim(sline.substr(0, eq));
                    const std::string sv = llm_trim(sline.substr(eq + 1));
                    if (sk == "llmmonthlycap" || sk == "monthly_cap") {
                        c.monthlyCap = atoi(sv.c_str());
                        break;
                    }
                }
            }
        }
    }
    return c;
}

// 原子写（tmp + rename，仿 net/library.h 的 writeFile）
inline bool llm_config_save(const LlmConfig& c, const std::string& path = "rlplayer-llm.conf") {
    std::ostringstream o;
    o << "# RLPlayer DeepSeek 配置（key=value，# 开头为注释）\n";
    o << "base_url=" << c.baseUrl << "\n";
    o << "api_key=" << c.apiKey << "\n";
    o << "model=" << c.model << "\n";
    if (c.monthlyCap > 0) o << "llmmonthlycap=" << c.monthlyCap << "\n";
    if (!c.usagePath.empty()) o << "usage_path=" << c.usagePath << "\n";
    const std::string s = o.str();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(s.data(), (std::streamsize)s.size());
        if (!f.good()) { return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

// 合并写（界面保存 key 用）：替换 base_url / api_key / model / llmmonthlycap 行，
// 其余行【原样保留】—— 尤其是 proxy=host:port（P1.5 的后备代理）与注释。
// 为什么不能用上面的 llm_config_save：它是整文件重写，界面保存一次 key 就会把
// 用户的 proxy 行冲掉（2026-10-22 功能4 的红线）。文件不存在时等价于新建。
inline bool llm_config_save_merge(const LlmConfig& c, const std::string& path = "rlplayer-llm.conf") {
    std::vector<std::string> out;
    bool wroteBase = false, wroteKey = false, wroteModel = false, wroteCap = false, wroteUsage = false;
    {
        std::ifstream f(path, std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string t = llm_trim(line);
            std::string k;
            if (!t.empty() && t[0] != '#') {
                const size_t eq = t.find('=');
                if (eq != std::string::npos) k = llm_trim(t.substr(0, eq));
            }
            if (k == "base_url") {
                if (wroteBase) continue;                 // 重复键只留第一条
                out.push_back("base_url=" + c.baseUrl); wroteBase = true;
            } else if (k == "api_key") {
                if (wroteKey) continue;
                out.push_back("api_key=" + c.apiKey); wroteKey = true;
            } else if (k == "model") {
                if (wroteModel) continue;
                out.push_back("model=" + c.model); wroteModel = true;
            } else if (k == "llmmonthlycap" || k == "monthly_cap") {
                if (wroteCap) continue;
                if (c.monthlyCap > 0) out.push_back("llmmonthlycap=" + std::to_string(c.monthlyCap));
                wroteCap = true;
            } else if (k == "usage_path") {
                if (wroteUsage) continue;
                if (!c.usagePath.empty()) out.push_back("usage_path=" + c.usagePath);
                wroteUsage = true;
            } else {
                out.push_back(line);                     // 注释 / 空行 / proxy= / 未知键
            }
        }
    }
    if (!wroteBase) out.push_back("base_url=" + c.baseUrl);
    if (!wroteKey) out.push_back("api_key=" + c.apiKey);
    if (!wroteModel) out.push_back("model=" + c.model);
    if (!wroteCap && c.monthlyCap > 0) out.push_back("llmmonthlycap=" + std::to_string(c.monthlyCap));
    if (!wroteUsage && !c.usagePath.empty()) out.push_back("usage_path=" + c.usagePath);
    std::string s;
    for (const std::string& l : out) { s += l; s += "\n"; }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(s.data(), (std::streamsize)s.size());
        if (!f.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

// ---------------------------------------------------------------------------
// 月度调用配额与用量统计（E-8）
// ---------------------------------------------------------------------------

struct LlmMonthlyUsage {
    int total = 0;
    int chat = 0;
    int search = 0;
    int fetch = 0;
};

inline std::string llm_usage_path(const std::string& customPath = "") {
    if (!customPath.empty()) return customPath;
    std::error_code ec;
    if (std::filesystem::is_directory("build", ec)) {
        return "build/rlplayer-llm-usage.json";
    }
    if (std::filesystem::is_directory("rlplayer-meta", ec)) {
        return "rlplayer-meta/llm-usage.json";
    }
    return "rlplayer-llm-usage.json";
}

// 2026-10-25（并发记账 P0）：std::localtime 返回的是**进程级静态缓冲**，多线程同时调用会
// 读到别的线程刚覆盖的结构（甚至撕裂）—— 4 路并行 fetch 每路都要取一次当前月份。
// 改用线程安全的 localtime_s（Windows）/ localtime_r（其它）。
inline std::string llm_format_month(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0) return "197001";
#else
    if (!localtime_r(&t, &tm)) return "197001";
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d", tm.tm_year + 1900, tm.tm_mon + 1);
    return std::string(buf);
}

inline std::string llm_current_month() {
    return llm_format_month(std::time(nullptr));
}

// ---------------------------------------------------------------------------
// 并发安全（2026-10-25 外部评审 P0 修复）
//
// 旧实现是「读文件 → ++ → 写固定 p+".tmp" → rename」的无锁 read-modify-write。
// retrieval.h 的 4 路并行 ds_fetch 每条都记一次 fetch 用量，于是同时发生：
//   ① 丢更新：A/B 都读到 total=7，各自 +1 写回 8，实际两次调用只加 1；
//   ② 互踩 tmp：两条线共用一个 "<p>.tmp"，A 写出后 B 截断重写，rename 出去的
//      可能是别人写了一半的内容 → 半写文件；
//   ③ 半写文件下次 json_parse 得到"非对象"或残缺对象，被静默当成 0 ——
//      用量与月度配额（llm_check_cap）一起失真，用户永远不会知道。
//
// 现在：整段读改写持一把进程内互斥锁；tmp 名带 pid/线程/序号/随机盐；
// 写失败退避重试；上一份 good 留成 "<p>.bak"，损坏时回退并打 WARN（不再静默按 0）。
// ---------------------------------------------------------------------------

// 进程内唯一锁：inline 函数里的函数级 static，跨 TU 也只有一个实例。
// 只保护用量文件的读改写（几毫秒），不参与网络 I/O，属于"静音锁"。
inline std::mutex& llm_usage_lock() {
    static std::mutex m;
    return m;
}

inline unsigned long long llm_process_id() {
#if defined(_WIN32)
    return (unsigned long long)_getpid();
#else
    return (unsigned long long)getpid();
#endif
}

// 唯一临时文件名：<path>.<tag>.<pid>.<线程/随机><序号>.tmp
inline std::string llm_unique_tmp_path(const std::string& path, const char* tag) {
    static std::atomic<unsigned long long> seq{0};
    static const unsigned long long salt = (unsigned long long)std::random_device{}();
    const unsigned long long th =
        (unsigned long long)std::hash<std::thread::id>{}(std::this_thread::get_id());
    char b[160];
    std::snprintf(b, sizeof(b), ".%s.%llu.%06llx.%llu.tmp",
                  tag, llm_process_id(), (th ^ salt) & 0xFFFFFFull, seq.fetch_add(1));
    return path + b;
}

// 原子写：写唯一 tmp → 关闭句柄 → rename 覆盖。失败退避重试（attempts 次）。
// 任何失败路径都不留 .tmp 垃圾。
inline bool llm_write_file_atomic(const std::string& path, const std::string& data,
                                  int attempts, const char* tag) {
    if (attempts < 1) attempts = 1;
    for (int i = 0; i < attempts; ++i) {
        const std::string tmp = llm_unique_tmp_path(path, tag);
        bool written = false;
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (f) {
                f.write(data.data(), (std::streamsize)data.size());
                f.flush();
                written = f.good();
            }
        }   // 句柄必须先关：Windows 上目标被打开时 rename 会失败（这也是要重试的原因）
        if (written) {
            std::error_code rec;
            std::filesystem::rename(tmp, path, rec);
            if (!rec) return true;
        }
        std::error_code rec2;
        std::filesystem::remove(tmp, rec2);
        std::this_thread::sleep_for(std::chrono::milliseconds(2 * (i + 1)));
    }
    return false;
}

inline std::atomic<unsigned>& llm_usage_warn_counter() {
    static std::atomic<unsigned> n{0};
    return n;
}

// WARN 落盘：<usage>.warn.log（64 KB 上限，超限重开保留最近的）。
// 为什么必须落盘：rtaweb 是 GUI 进程，stderr 往往被启动器丢弃 —— 只打 stderr 等于
// "用户永远不会知道"。这条路径只在异常时发生，正常记账不产生任何额外 IO。
inline void llm_usage_warn_log_locked(const std::string& usagePath, const std::string& msg) {
    const std::string lp = usagePath + ".warn.log";
    std::error_code sz;
    const auto n = std::filesystem::file_size(lp, sz);
    const bool rotate = (!sz && n > 64 * 1024);      // 超限重开，只留最近 64 KB
    std::ofstream f(lp, std::ios::binary | (rotate ? std::ios::trunc : std::ios::app));
    if (!f) return;
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    const bool tmOk = localtime_s(&tm, &now) == 0;
#else
    const bool tmOk = localtime_r(&now, &tm) != nullptr;
#endif
    char ts[24] = {0};
    if (tmOk) {
        std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    std::string line = std::string(ts) + " [WARN] " + msg + "\n";
    f.write(line.data(), (std::streamsize)line.size());
    f.flush();
}

// WARN 文本 + 计数（测试用 llm_usage_warn_count() 断言"确实报过"）。
// 同一条消息每进程只打一次，避免文件长期损坏时每次调用都刷日志。
// 调用方必须持 llm_usage_lock()。
inline void llm_usage_warn_locked(const std::string& usagePath, const std::string& msg) {
    static std::set<std::string> seen;
    if (seen.size() < 64 && !seen.insert(usagePath + "|" + msg).second) return;
    llm_usage_warn_counter().fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[WARN] llm-usage: %s\n", msg.c_str());
    std::fflush(stderr);
    llm_usage_warn_log_locked(usagePath, msg);
}

inline unsigned llm_usage_warn_count() {
    return llm_usage_warn_counter().load(std::memory_order_relaxed);
}

inline std::string llm_usage_backup_path(const std::string& p) { return p + ".bak"; }

inline bool llm_read_text_file(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// 结构完整性：括号/引号配平，且顶层值结束后没有残留垃圾。
// 注意与文件后半段 llm_json_looks_complete()（流式 SSE 用）区分：那个只判"配平 +
// 末字符是 }/]"；这里额外要求**顶层值闭合后不再有任何残留**，因为"两个文档拼接"
// 恰恰是并发互踩 tmp 的典型产物，而它和合法 JSON 一样配平。
// 为什么需要它：net/json.h 的解析器很宽容，"{\"202611\":{\"total\":" 这种半写文件
// 也会返回 Obj，只看 type 根本判不出损坏（这正是旧代码静默按 0 的原因）。
inline bool llm_usage_json_complete(const std::string& t) {
    int depth = 0;
    bool inStr = false, esc = false, topClosed = false;
    for (const char c : t) {
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (topClosed) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            return false;                      // 顶层结束后还有东西 → 拼接/踩踏痕迹
        }
        if (c == '"') inStr = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            --depth;
            if (depth < 0) return false;
            if (depth == 0) topClosed = true;
        }
    }
    return !inStr && !esc && depth == 0 && topClosed;
}

// 严格解析用量 JSON（结构 + 分桶字段类型）。失败时 err 给出人可读原因。
inline bool llm_usage_parse_strict(const std::string& text,
                                   std::map<std::string, LlmMonthlyUsage>& out,
                                   std::string* err) {
    if (!llm_usage_json_complete(text)) {
        if (err) *err = "结构不完整（括号/引号不配平，疑似半写或并发踩踏）";
        return false;
    }
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) {
        if (err) *err = "顶层不是 JSON 对象";
        return false;
    }
    std::map<std::string, LlmMonthlyUsage> tmp;
    for (const auto& [mKey, mVal] : j.obj) {
        if (mVal.type != rta::net::Json::Obj) {
            if (err) *err = "分桶 \"" + mKey + "\" 不是对象";
            return false;
        }
        for (const char* f : {"total", "chat", "search", "fetch"}) {
            if (mVal.has(f) && mVal[f].type != rta::net::Json::Num) {
                if (err) *err = "分桶 \"" + mKey + "\" 的 " + f + " 不是数字";
                return false;
            }
        }
        LlmMonthlyUsage u;
        if (mVal.has("total"))  u.total  = (int)mVal["total"].n();
        if (mVal.has("chat"))   u.chat   = (int)mVal["chat"].n();
        if (mVal.has("search")) u.search = (int)mVal["search"].n();
        if (mVal.has("fetch"))  u.fetch  = (int)mVal["fetch"].n();
        tmp[mKey] = u;
    }
    out.swap(tmp);
    return true;
}

// 读盘结果（内部用）
struct LlmUsageLoad {
    bool ok = false;          // 有可用数据（主文件或 .bak；主文件不存在且无 .bak 时为 false=全新账本）
    bool corrupt = false;     // 主文件存在但不可解析
    bool fromBackup = false;  // 实际数据来自 .bak
    std::string error;        // 人可读错误（正常读成功时为空）
    std::string raw;          // 主文件原文（仅当主文件本身可解析时非空 → 可写进 .bak）
    std::map<std::string, LlmMonthlyUsage> all;
};

// 读 + 校验 + 回退 .bak。调用方必须持 llm_usage_lock()。
inline LlmUsageLoad llm_load_usage_locked(const std::string& p) {
    LlmUsageLoad r;
    std::string mainText;
    const bool hasMain = llm_read_text_file(p, mainText);
    if (hasMain && mainText.empty()) { r.ok = true; return r; }   // 0 字节：全新账本，不算损坏
    std::string err;
    if (hasMain && llm_usage_parse_strict(mainText, r.all, &err)) {
        r.ok = true;
        r.raw = mainText;
        return r;
    }
    if (hasMain) {
        r.corrupt = true;
        r.error = "用量文件损坏：" + (err.empty() ? std::string("无法解析") : err);
        llm_usage_warn_locked(p, r.error);
    }
    // 回退上一份 good
    std::string bakText, berr;
    if (llm_read_text_file(llm_usage_backup_path(p), bakText) &&
        llm_usage_parse_strict(bakText, r.all, &berr)) {
        r.ok = true;
        r.fromBackup = true;
        r.error += (r.error.empty() ? "" : "；") + std::string("已回退 .bak");
        llm_usage_warn_locked(p, "已回退备份 " + llm_usage_backup_path(p) +
                                (r.corrupt ? "（主文件损坏）" : "（主文件缺失）"));
        return r;
    }
    if (r.corrupt) {
        r.error += "；.bak 不可用";
        llm_usage_warn_locked(p, "主文件与 .bak 均不可用，本次按 0 计");
    }
    r.all.clear();
    return r;
}

// 读取侧诊断：损坏时不再静默 —— 调用方（以及测试）能拿到"为什么读不出来"。
struct LlmUsageRead {
    bool ok = true;          // 读到的数据可用（主文件或 .bak）
    bool corrupt = false;    // 主文件损坏
    bool fromBackup = false; // 数据来自 .bak
    std::string error;       // 人可读错误说明
};

inline LlmMonthlyUsage llm_get_usage(const std::string& month, const std::string& customPath = "",
                                     LlmUsageRead* outDiag = nullptr) {
    const std::string p = llm_usage_path(customPath);
    std::lock_guard<std::mutex> lk(llm_usage_lock());
    const LlmUsageLoad ld = llm_load_usage_locked(p);
    if (outDiag) {
        outDiag->ok = ld.ok;
        outDiag->corrupt = ld.corrupt;
        outDiag->fromBackup = ld.fromBackup;
        outDiag->error = ld.error;
    }
    const auto it = ld.all.find(month);
    return it == ld.all.end() ? LlmMonthlyUsage{} : it->second;
}

inline int llm_get_monthly_count(const std::string& month, const std::string& customPath = "") {
    return llm_get_usage(month, customPath).total;
}

// 用量 JSON 序列化（纯函数，便于测试；顺序固定 = 月键字典序，便于 diff）
inline std::string llm_format_usage_json(const std::map<std::string, LlmMonthlyUsage>& all) {
    std::ostringstream o;
    o << "{\n";
    bool firstMonth = true;
    for (const auto& [mKey, u] : all) {
        if (!firstMonth) o << ",\n";
        firstMonth = false;
        o << "  \"" << llm_esc(mKey) << "\": {\n"
          << "    \"total\": " << u.total << ",\n"
          << "    \"chat\": " << u.chat << ",\n"
          << "    \"search\": " << u.search << ",\n"
          << "    \"fetch\": " << u.fetch << "\n"
          << "  }";
    }
    o << "\n}\n";
    return o.str();
}

// 记录调用次数。
//
// ★ 线程安全（2026-10-25 P0）：整段「读 → 累加 → 备份 → 原子写」都在 llm_usage_lock() 里，
//   8 线程 × 100 次并发记账后 total 必须精确等于 800（不许丢更新）。
// ★ 落盘：唯一 tmp 名 + rename；失败退避重试 5 次；上一份 good 另存 "<p>.bak"。
// ★ 损坏：读不出来不再静默按 0 —— 打 WARN、回退 .bak、并以 .bak 为基准继续累加
//   （这样一次半写事故不会把整月用量清零）。
// ★ 格式纯 JSON，绝不记录 key 等敏感信息。
inline bool llm_record_usage(const std::string& month, const std::string& callType, const std::string& customPath = "") {
    const std::string p = llm_usage_path(customPath);
    std::error_code ec;
    const auto parent = std::filesystem::path(p).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::lock_guard<std::mutex> lk(llm_usage_lock());

    const LlmUsageLoad ld = llm_load_usage_locked(p);   // 已含校验 + .bak 回退 + WARN
    std::map<std::string, LlmMonthlyUsage> all = ld.all;

    LlmMonthlyUsage& cur = all[month];
    cur.total++;
    if (callType == "chat") cur.chat++;
    else if (callType == "search") cur.search++;
    else if (callType == "fetch") cur.fetch++;

    const std::string s = llm_format_usage_json(all);

    // 先把"上一份 good"落成 .bak，再覆盖主文件。主文件本身损坏时（ld.raw 为空）
    // 绝不拿坏内容覆盖 .bak —— 否则唯一能救的数据就没了。
    if (!ld.raw.empty()) {
        if (!llm_write_file_atomic(llm_usage_backup_path(p), ld.raw, 2, "bak")) {
            llm_usage_warn_locked(p, "备份 .bak 写入失败（主账本不受影响）：" + llm_usage_backup_path(p));
        }
    }

    if (!llm_write_file_atomic(p, s, 5, "usage")) {
        llm_usage_warn_locked(p, "用量写盘失败（已重试 5 次，本次计数丢失）");
        return false;
    }
    return true;
}

// 检查是否已达月度调用上限（cfg.monthlyCap == 0 为不限，默认 0）
inline bool llm_check_cap(const LlmConfig& cfg, const std::string& month = "", std::string* outError = nullptr) {
    if (cfg.monthlyCap <= 0) return true; // 0=不限
    const std::string m = month.empty() ? (cfg.monthOverride.empty() ? llm_current_month() : cfg.monthOverride) : month;
    const int current = llm_get_monthly_count(m, cfg.usagePath);
    if (current >= cfg.monthlyCap) {
        if (outError) {
            *outError = "本月 AI 调用已达上限 (" + std::to_string(current) + "/" + std::to_string(cfg.monthlyCap) + ")";
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// chat completions
// ---------------------------------------------------------------------------

struct LlmMessage { std::string role, content; };
struct LlmReply {
    bool ok = false;
    std::string content, error;
    int totalTokens = 0;
};

// 完整请求体（含 model），llm_chat / llm_chat_stream 共用。
// stream=true 追加 "stream":true（流式）；deepseek 模型再加 stream_options.include_usage，
// 让最后一个 chunk 带上 usage —— 该参数可用 RLPLAYER_LLM_STREAM_USAGE=0 关掉
//（未联网验证：若某兼容端点因此 400，这一行是唯一要回退的地方）。
inline std::string llm_build_chat_body(const std::string& model,
                                       const std::vector<LlmMessage>& msgs,
                                       int maxTokens, float temperature, bool jsonObject,
                                       bool stream = false) {
    std::ostringstream o;
    o << "{";
    if (!model.empty()) o << "\"model\":\"" << llm_esc(model) << "\",";
    o << "\"messages\":[";
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (i) o << ",";
        o << "{\"role\":\"" << llm_esc(msgs[i].role)
          << "\",\"content\":\"" << llm_esc(llm_sanitize_utf8(msgs[i].content)) << "\"}";
    }
    o << "],\"max_tokens\":" << maxTokens;
    // 2026-10-24（用户 261024-01 #1）：思考强度设 LOW 以提升速度。
    // DeepSeek 侧接受 reasoning_effort 参数（实测 200；不支持的兼容端点可能 400，
    // 所以只在模型名含 deepseek 时追加，避免影响其它 OpenAI 兼容后端）。
    // 2026-10-24（261024-02 #7）：思考强度做成可调，便于 A/B 实测（off / low / default）。
    // RLPLAYER_LLM_REASONING 环境变量优先，其次 conf 里 llm_reasoning 键（rtaweb 会写进 env）。
    const std::string lm = model;
    if (lm.find("deepseek") != std::string::npos) {
        const char* rv = std::getenv("RLPLAYER_LLM_REASONING");
        const std::string mode = rv ? std::string(rv) : std::string("low");
        if (mode == "off") o << ",\"thinking\":{\"type\":\"disabled\"}";
        else if (mode == "default" || mode == "auto") { /* 不追加参数 */ }
        else o << ",\"reasoning_effort\":\"low\"";
    }
    char tb[32];
    std::snprintf(tb, sizeof(tb), "%.2f", (double)temperature);   // 保留 2 位小数
    o << ",\"temperature\":" << tb;
    if (jsonObject) o << ",\"response_format\":{\"type\":\"json_object\"}";
    if (stream) {
        o << ",\"stream\":true";
        if (model.find("deepseek") != std::string::npos) {
            const char* uv = std::getenv("RLPLAYER_LLM_STREAM_USAGE");
            const bool wantUsage = !(uv && std::string(uv) == "0");
            if (wantUsage) o << ",\"stream_options\":{\"include_usage\":true}";
        }
    }
    o << "}";
    return o.str();
}

// 纯函数（可测）：不含 model 的请求体，model 由 llm_chat 注入
inline std::string llm_build_chat_body(const std::vector<LlmMessage>& msgs,
                                       int maxTokens = 800, float temperature = 0.2f,
                                       bool jsonObject = false, bool stream = false) {
    return llm_build_chat_body(std::string(), msgs, maxTokens, temperature, jsonObject, stream);
}

// 纯函数（可测）：解析 /chat/completions 响应。错误分三类：非法 JSON / 服务端 error / 缺 choices
inline LlmReply llm_parse_chat_response(const std::string& text) {
    LlmReply r;
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) { r.error = "响应不是合法 JSON"; return r; }
    if (j.has("error")) {
        const rta::net::Json& e = j["error"];
        std::string msg = e.has("message") ? e["message"].s() : e.s();
        if (msg.empty()) msg = "服务端未给出 message";
        r.error = "服务端错误: " + msg;
        return r;
    }
    if (!j.has("choices") || j["choices"].size() == 0) { r.error = "响应缺少 choices"; return r; }
    r.content = j["choices"].at(0)["message"]["content"].s();
    if (j.has("usage")) r.totalTokens = (int)j["usage"]["total_tokens"].n();
    r.ok = true;
    return r;
}

// POST {base}/chat/completions，超时 60s，单次调用不重试
inline LlmReply llm_chat(const LlmConfig& cfg, const std::vector<LlmMessage>& msgs,
                         int maxTokens = 800, float temperature = 0.2f, bool jsonObject = false) {
    LlmReply r;
    if (cfg.apiKey.empty()) { r.error = "未配置 api_key（rlplayer-llm.conf）"; return r; }

    // E-8: 月度调用上限与超限降级（超限不发起 HTTP）
    std::string capErr;
    if (!llm_check_cap(cfg, "", &capErr)) {
        r.error = capErr;
        return r;
    }

    const std::string body = llm_build_chat_body(cfg.model, msgs, maxTokens, temperature, jsonObject);
    const std::string url = llm_base_url(cfg.baseUrl) + "/chat/completions";
    const std::vector<std::string> headers = { "Authorization: Bearer " + cfg.apiKey };

    rta::net::HttpResponse resp;
    if (cfg.transport) {
        resp = cfg.transport(url, body, headers);
    } else {
        resp = rta::net::http_post(url, body, headers, "application/json", 60000);
    }

    if (resp.status == 0) {
        r.error = llm_redact(cfg.apiKey, "网络请求失败: " + resp.error);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    if (resp.status < 200 || resp.status >= 300) {
        const LlmReply p = llm_parse_chat_response(text);   // 借它提取服务端 error.message
        r.error = llm_redact(cfg.apiKey,
            "HTTP " + std::to_string(resp.status) + (p.error.empty() ? "" : ": " + p.error));
        return r;
    }
    r = llm_parse_chat_response(text);
    if (!r.ok) {
        r.error = llm_redact(cfg.apiKey, r.error);
    } else {
        // E-8: 成功响应计费口径（按实际成功计）
        const std::string m = cfg.monthOverride.empty() ? llm_current_month() : cfg.monthOverride;
        llm_record_usage(m, "chat", cfg.usagePath);
    }
    return r;
}

// ---------------------------------------------------------------------------
// 流式输出（SSE）— 用户反馈 261024-05「LLM 做流式比较好，不用枯等」
//
// 协议：POST {base}/chat/completions，body 带 "stream":true；服务端逐行回
//   data: {"choices":[{"index":0,"delta":{"content":"…"}}]}
//   data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{...}}
//   data: [DONE]
// 解析只认 "data:" 行；": 心跳"、event:/id:/retry:/空行一律忽略。
//
// 三种服务端行为都已覆盖（离线测试锁死）：
//   ① 正常 SSE            → 逐 delta 回调；[DONE] 或 finish_reason 判"完整"
//   ② 忽略 stream 回 JSON → 走 llm_parse_chat_response，全文作为【单个】delta 投递一次
//   ③ 半截断开            → errorKind="truncated"、ok=false —— 绝不把半截当成功
//
// 线程：delta 回调在"解析线程"上执行 —— 走 streamTransport 时那是传输层的网络线程，
// 走 transport/http_post 时是调用 llm_chat_stream 的线程。调用方必须自己保证回调安全
//（retrieval.h 的 SummaryProgressEmitter 就是为此用 mutex + 快照）。
// ---------------------------------------------------------------------------

struct LlmStreamReply {
    bool ok = false;
    std::string content;    // 完整正文（失败时可能是半截 —— 只有 ok==true 才能用）
    std::string error;      // 给人看的错误（已过 llm_redact）
    std::string errorKind;  // 机器判据：config/cap/transport/http/server/truncated/json/empty
    int  totalTokens = 0;   // usage.total_tokens（服务端不给就是 0）
    int  deltaCount = 0;    // 增量回调次数（非流式回退固定 1 次）
    int  events = 0;        // 收到的 data: 事件数（含 [DONE]）
    bool sawDone = false;   // 收到 data: [DONE]
    bool sawFinish = false; // 收到非空 finish_reason
    bool streamed = false;  // 真的走了 SSE（false = 服务端忽略了 stream，回的是普通 JSON）
    bool truncated = false; // 半截断开（errorKind=truncated 时为 true）
    long long firstDeltaMs = -1;  // 首个 delta 距发请求的毫秒数（-1 = 没有）
    long long totalMs = 0;
};

struct LlmSseState {
    std::string lineBuf;   // 尚未遇到 '\n' 的残行（可能跨多个 feed 边界）
    std::string content;   // 累计正文
    std::string error;     // 首个错误（非空即失败）
    std::string errorKind;
    int totalTokens = 0;
    int deltaCount = 0;
    int events = 0;
    bool sawDone = false;
    bool sawFinish = false;
    bool sawData = false;
};

// 纯函数（可测）：粗判一段 JSON 文本是否完整（括号配平、引号闭合、最后一个有效字符是 } 或 ]）。
// 为什么必须有：net/json.h 的解析器是"宽进"的 —— 半截 JSON 也能解析出部分字段，
// 不先判完整性就会把半截 delta 当成正常增量（这正是 6.3 回归的点）。
inline bool llm_json_looks_complete(const std::string& s) {
    int depth = 0;
    bool in_str = false, esc = false;
    char last = 0;
    for (char c : s) {
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { --depth; if (depth < 0) return false; }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') last = c;
    }
    return depth == 0 && !in_str && (last == '}' || last == ']');
}

// 纯函数（可测）：处理一条 data: 的 payload。
inline void llm_sse_apply_payload(LlmSseState& st, const std::string& raw, const LlmDeltaFn& on_delta) {
    if (!st.error.empty() || st.sawDone) return;
    const std::string payload = llm_trim(raw);
    if (payload.empty()) return;                      // "data:" 空行 = 心跳
    st.sawData = true;
    if (payload == "[DONE]") { st.sawDone = true; return; }
    if (!llm_json_looks_complete(payload)) {
        // 半截断开时最后一行几乎必然停在这里：判"截断"，绝不当成功
        st.errorKind = "truncated";
        const std::string head = payload.size() > 120 ? payload.substr(0, 120) + "…" : payload;
        st.error = "流式分片不完整（括号/引号未闭合，连接多半中途断开）: " + head;
        return;
    }
    const rta::net::Json j = rta::net::json_parse(payload);
    if (j.type != rta::net::Json::Obj) {
        st.errorKind = "malformed";
        const std::string head = payload.size() > 120 ? payload.substr(0, 120) + "…" : payload;
        st.error = "流式分片不是合法 JSON 对象: " + head;
        return;
    }
    if (j.has("error")) {
        const rta::net::Json& e = j["error"];
        std::string msg = e.has("message") ? e["message"].s() : e.s();
        if (msg.empty()) msg = "服务端未给出 message";
        st.errorKind = "server";
        st.error = "服务端错误: " + msg;
        return;
    }
    if (j.has("usage")) st.totalTokens = (int)j["usage"]["total_tokens"].n();
    if (!j.has("choices")) return;                    // 有些端点只回一个 usage 尾包
    const rta::net::Json& ch = j["choices"];
    if (ch.type != rta::net::Json::Arr || ch.size() == 0) return;
    const rta::net::Json& c0 = ch.at(0);
    const std::string finish = c0["finish_reason"].s();
    if (!finish.empty()) st.sawFinish = true;
    std::string piece;
    if (c0.has("delta")) piece = c0["delta"]["content"].s();
    else if (c0.has("message")) piece = c0["message"]["content"].s();  // 个别端点在 SSE 里回非流式结构
    if (piece.empty()) return;                        // role / finish 包没有正文
    st.content += piece;
    ++st.deltaCount;
    if (on_delta) on_delta(piece);
}

// 纯函数（可测）：处理一整行（已去掉 '\n'；"\r\n" 的 '\r' 在这里剥掉）。
inline void llm_sse_handle_line(LlmSseState& st, std::string line, const LlmDeltaFn& on_delta) {
    while (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return;
    if (line[0] == ':') return;                       // SSE 注释 / keep-alive
    if (line.size() >= 5 && line.compare(0, 5, "data:") == 0) {
        ++st.events;
        llm_sse_apply_payload(st, line.substr(5), on_delta);
        return;
    }
    // event: / id: / retry: / 裸行：SSE 里合法但不带正文，忽略
}

// 喂入一段原始字节（可分任意次、任意切点调用）；完整行会被立即处理。
inline void llm_sse_feed(LlmSseState& st, const char* data, size_t n, const LlmDeltaFn& on_delta) {
    if (!data || n == 0) return;
    st.lineBuf.append(data, n);
    size_t consumed = 0;
    for (;;) {
        const size_t nl = st.lineBuf.find('\n', consumed);
        if (nl == std::string::npos) break;
        llm_sse_handle_line(st, st.lineBuf.substr(consumed, nl - consumed), on_delta);
        consumed = nl + 1;
    }
    if (consumed > 0) st.lineBuf.erase(0, consumed);
}

// 流结束：处理没有结尾换行的最后一行（[DONE] 允许不带换行），并判定"是否完整"。
// 返回 false 时 st.error/st.errorKind 已填好；**半截流一律返回 false**。
inline bool llm_sse_finish(LlmSseState& st, const LlmDeltaFn& on_delta) {
    if (!st.lineBuf.empty()) {
        std::string tail;
        tail.swap(st.lineBuf);
        llm_sse_handle_line(st, tail, on_delta);
    }
    if (!st.error.empty()) return false;
    if (st.sawDone || st.sawFinish) return true;
    st.errorKind = "truncated";
    st.error = st.sawData
        ? "流式响应中断：没收到 [DONE] 或 finish_reason（连接提前断开）"
        : "流式响应为空：没有任何 data: 事件";
    return false;
}

// 响应体类型判定（首字节路由）：流式钩子可能只先给几十字节，还不能确定是 SSE 还是普通 JSON，
// 所以先缓冲头部，看到首个非空白字符再定：'{' '[' → JSON；其余 → SSE。
// 一次性拿到完整 body 的路径（transport/http_post）也走同一个路由，保证两条路完全同构。
struct LlmBodyRouter {
    enum Mode { Unknown = 0, Sse, Json };
    Mode mode = Unknown;
    std::string head;      // Unknown 阶段缓冲的字节
    std::string jsonText;  // Json 模式累积

    void feed(const char* data, size_t n, LlmSseState& st, const LlmDeltaFn& on_delta) {
        if (!data || n == 0) return;
        if (mode == Sse) { llm_sse_feed(st, data, n, on_delta); return; }
        if (mode == Json) { jsonText.append(data, n); return; }
        head.append(data, n);
        for (size_t i = 0; i < head.size(); ++i) {
            const char c = head[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            mode = (c == '{' || c == '[') ? Json : Sse;
            break;
        }
        if (mode == Unknown && head.size() >= 64) mode = Sse;   // 64 字节还全是空白：按 SSE 处理（最后判空）
        if (mode == Json) {
            jsonText.swap(head);
        } else if (mode == Sse) {
            llm_sse_feed(st, head.data(), head.size(), on_delta);
            head.clear();
        }
    }

    void finish(LlmSseState& st, const LlmDeltaFn& on_delta) {
        if (mode == Sse) { llm_sse_finish(st, on_delta); return; }
        if (mode == Unknown && !head.empty()) jsonText.swap(head);
    }
};

// 传输层失败（status==0）的错误细分：超时归 timeout，其余归 transport。
// 判据只能是错误文本 —— 平台层把 WinHTTP 的 ERROR_WINHTTP_TIMEOUT 变成 message，
// 这里保持"只做分类、不改写原文"（原文仍完整保留在 error 里）。
inline std::string llm_transport_error_kind(const std::string& err) {
    std::string low;
    low.reserve(err.size());
    for (char c : err) low += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    if (low.find("timeout") != std::string::npos || low.find("timed out") != std::string::npos ||
        err.find("超时") != std::string::npos) {
        return "timeout";
    }
    return "transport";
}

// POST {base}/chat/completions 的流式版。超时/重试/计费口径与 llm_chat 一致：
//   * 超时只作用在传输层（streamTransport 自己管；http_post 走 timeout_ms）
//   * 单次调用不重试；重试归上层（retrieval.h 的 attempt 循环，每次重试都重新开一条流）
//   * 成功（ok）才记一次 chat 用量；半截/失败不记账（与 llm_chat 的失败口径相同）
inline LlmStreamReply llm_chat_stream(const LlmConfig& cfg, const std::vector<LlmMessage>& msgs,
                                      const LlmDeltaFn& on_delta = LlmDeltaFn(),
                                      int maxTokens = 800, float temperature = 0.2f,
                                      bool jsonObject = false, int timeout_ms = 60000) {
    LlmStreamReply r;
    const auto t0 = std::chrono::steady_clock::now();
    auto ms_since = [&t0]() {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    };

    if (cfg.apiKey.empty()) { r.errorKind = "config"; r.error = "未配置 api_key（rlplayer-llm.conf）"; return r; }
    std::string capErr;
    if (!llm_check_cap(cfg, "", &capErr)) { r.errorKind = "cap"; r.error = capErr; return r; }

    const std::string body = llm_build_chat_body(cfg.model, msgs, maxTokens, temperature, jsonObject, true);
    const std::string url = llm_base_url(cfg.baseUrl) + "/chat/completions";
    const std::vector<std::string> headers = { "Authorization: Bearer " + cfg.apiKey };

    LlmSseState st;
    LlmBodyRouter router;
    // 回调包装：记首个 delta 时刻；用户回调抛异常不得把网络线程带崩（内容照常返回）
    const LlmDeltaFn emit = [&](const std::string& d) {
        if (r.firstDeltaMs < 0) r.firstDeltaMs = ms_since();
        if (!on_delta) return;
        try { on_delta(d); } catch (...) { }
    };

    rta::net::HttpResponse resp;
    if (cfg.streamTransport) {
        const LlmChunkSink sink = [&](const char* d, size_t n) { router.feed(d, n, st, emit); };
        resp = cfg.streamTransport(url, body, headers, sink);
    } else if (cfg.transport) {
        resp = cfg.transport(url, body, headers);
    } else {
        // 261024-05：默认就用**真流式** POST（以前这里是 http_post，要等整段 body 到齐，
        // 等于没有任何"不用枯等"的收益）。onChunk 只有 http_post_stream 会调；
        // 不支持分块的传输实现（平台的其它后端）只管返回完整 body，语义不变。
        const LlmChunkSink sink = [&](const char* d, size_t n) { router.feed(d, n, st, emit); };
        resp = rta::net::http_post_stream(url, body, headers,
                                          [&](const char* d, size_t n) { sink(d, n); },
                                          "application/json", timeout_ms);
    }
    r.totalMs = ms_since();

    if (resp.status == 0) {
        r.errorKind = llm_transport_error_kind(resp.error);
        r.truncated = st.sawData;   // 已经吐过半个流才算截断，否则就是根本没连上
        r.error = llm_redact(cfg.apiKey, "网络请求失败: " + resp.error);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    if (resp.status < 200 || resp.status >= 300) {
        const LlmReply p = llm_parse_chat_response(text);   // 借它提取服务端 error.message
        r.errorKind = "http";
        r.error = llm_redact(cfg.apiKey,
            "HTTP " + std::to_string(resp.status) + (p.error.empty() ? "" : ": " + p.error));
        return r;
    }

    // 传输层已经投递过的字节不会再出现在 body 里（约定），这里只补剩余部分
    if (!text.empty()) router.feed(text.data(), text.size(), st, emit);
    router.finish(st, emit);

    if (router.mode == LlmBodyRouter::Json) {
        // 服务端忽略了 stream：按普通 JSON 解析，全文作为单个 delta 投递（调用方只有一条代码路径）
        const LlmReply p = llm_parse_chat_response(router.jsonText);
        if (!p.ok) {
            r.errorKind = "json";
            r.error = llm_redact(cfg.apiKey, p.error);
            return r;
        }
        r.content = p.content;
        r.totalTokens = p.totalTokens;
        r.streamed = false;
        r.deltaCount = 1;
        if (on_delta) emit(r.content);
    } else if (router.mode == LlmBodyRouter::Sse) {
        r.content = st.content;
        r.totalTokens = st.totalTokens;
        r.deltaCount = st.deltaCount;
        r.events = st.events;
        r.sawDone = st.sawDone;
        r.sawFinish = st.sawFinish;
        r.streamed = true;
        if (!st.error.empty() || !(st.sawDone || st.sawFinish)) {
            r.truncated = true;
            r.errorKind = st.errorKind.empty() ? "truncated" : st.errorKind;
            r.error = llm_redact(cfg.apiKey, st.error.empty() ? "流式响应中断" : st.error);
            return r;
        }
    } else {
        r.errorKind = "empty";
        r.error = "流式响应为空（既不是 SSE 也不是 JSON）";
        return r;
    }

    // E-8：成功响应计费口径（与 llm_chat 一致：只在 ok 时记一次 chat）
    const std::string m = cfg.monthOverride.empty() ? llm_current_month() : cfg.monthOverride;
    llm_record_usage(m, "chat", cfg.usagePath);
    r.ok = true;
    r.totalMs = ms_since();
    return r;
}

// ---------------------------------------------------------------------------
// DeepSeek Search / Fetch tools（docs §1.4：Bearer 认证，与 chat 同域）
// ---------------------------------------------------------------------------

struct DsHit {
    std::string title, url, snippet, summary;
    double rerankScore = 0;
    int authority = -1;
    std::string date;
};
struct DsSearchReply { bool ok = false; std::string error; std::vector<DsHit> hits; };
struct DsFetchReply  { bool ok = false; std::string title, content, error; };

inline std::string ds_build_search_body(const std::string& query, int count) {
    std::ostringstream o;
    o << "{\"query\":\"" << llm_esc(query) << "\",\"count\":" << count << "}";
    return o.str();
}

inline std::string ds_build_fetch_body(const std::string& url) {
    std::ostringstream o;
    o << "{\"url\":\"" << llm_esc(url) << "\"}";
    return o.str();
}

// 纯函数（可测）：解析 /v1/tools/search 响应的 results[]
inline DsSearchReply ds_parse_search_response(const std::string& text) {
    DsSearchReply r;
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) { r.error = "响应不是合法 JSON"; return r; }
    if (j.has("error")) {
        const rta::net::Json& e = j["error"];
        std::string msg = e.has("message") ? e["message"].s() : e.s();
        if (msg.empty()) msg = "服务端未给出 message";
        r.error = "服务端错误: " + msg;
        return r;
    }
    if (!j.has("results")) { r.error = "响应缺少 results"; return r; }
    const rta::net::Json& arr = j["results"];
    r.hits.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        const rta::net::Json& h = arr.at(i);
        DsHit hit;
        hit.title       = h["title"].s();
        hit.url         = h["url"].s();
        hit.snippet     = h["snippet"].s();
        hit.summary     = h["summary"].s();
        hit.rerankScore = h["rerank_score"].n();
        hit.authority   = h["authority"].ok() ? (int)h["authority"].n() : -1;
        hit.date        = h["date"].s();
        r.hits.push_back(std::move(hit));
    }
    r.ok = true;
    return r;
}

// 纯函数（可测）：解析 /v1/tools/fetch 响应的 title/content
inline DsFetchReply ds_parse_fetch_response(const std::string& text) {
    DsFetchReply r;
    const rta::net::Json j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj) { r.error = "响应不是合法 JSON"; return r; }
    if (j.has("error")) {
        const rta::net::Json& e = j["error"];
        std::string msg = e.has("message") ? e["message"].s() : e.s();
        if (msg.empty()) msg = "服务端未给出 message";
        r.error = "服务端错误: " + msg;
        return r;
    }
    if (!j.has("title") && !j.has("content")) { r.error = "响应缺少 title/content"; return r; }
    r.title   = j["title"].s();
    r.content = j["content"].s();
    r.ok = true;
    return r;
}

// 三个网络函数共用的「非 2xx」错误拼装（顺带把 key 从服务端回显里抹掉）
inline std::string ds_http_error(const std::string& apiKey, int status, const std::string& text) {
    const DsFetchReply p = ds_parse_fetch_response(text);   // 借它提取 error.message
    return llm_redact(apiKey,
        "HTTP " + std::to_string(status) + (p.error.empty() ? "" : ": " + p.error));
}

// POST {base}/v1/tools/search，超时 60s，单次调用不重试
inline DsSearchReply ds_search(const LlmConfig& cfg, const std::string& query, int count = 5) {
    DsSearchReply r;
    if (cfg.apiKey.empty()) { r.error = "未配置 api_key（rlplayer-llm.conf）"; return r; }

    // E-8: 月度调用上限与超限降级（超限不发起 HTTP）
    std::string capErr;
    if (!llm_check_cap(cfg, "", &capErr)) {
        r.error = capErr;
        return r;
    }

    const std::string body = ds_build_search_body(query, count);
    const std::string url = llm_base_url(cfg.baseUrl) + "/v1/tools/search";
    const std::vector<std::string> headers = { "Authorization: Bearer " + cfg.apiKey };

    rta::net::HttpResponse resp;
    if (cfg.transport) {
        resp = cfg.transport(url, body, headers);
    } else {
        resp = rta::net::http_post(url, body, headers, "application/json", 60000);
    }

    if (resp.status == 0) {
        r.error = llm_redact(cfg.apiKey, "网络请求失败: " + resp.error);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    if (resp.status < 200 || resp.status >= 300) { r.error = ds_http_error(cfg.apiKey, resp.status, text); return r; }
    r = ds_parse_search_response(text);
    if (!r.ok) {
        r.error = llm_redact(cfg.apiKey, r.error);
    } else {
        // E-8: 成功响应计费口径（按实际成功计）
        const std::string m = cfg.monthOverride.empty() ? llm_current_month() : cfg.monthOverride;
        llm_record_usage(m, "search", cfg.usagePath);
    }
    return r;
}

// POST {base}/v1/tools/fetch，单次调用不重试。timeout_ms 默认 60s；
// 2026-10-24（261024-02 #1 速度）：检索层改成 4 路并行 + 单页 12s 超时，
// 慢页面不再把整个资料整理拖到 20s+（实测最慢一次 fetch 阶段 10.9s）。
inline DsFetchReply ds_fetch(const LlmConfig& cfg, const std::string& url, int timeout_ms = 60000) {
    DsFetchReply r;
    if (cfg.apiKey.empty()) { r.error = "未配置 api_key（rlplayer-llm.conf）"; return r; }

    // E-8: 月度调用上限与超限降级（超限不发起 HTTP）
    std::string capErr;
    if (!llm_check_cap(cfg, "", &capErr)) {
        r.error = capErr;
        return r;
    }

    const std::string body = ds_build_fetch_body(url);
    const std::string reqUrl = llm_base_url(cfg.baseUrl) + "/v1/tools/fetch";
    const std::vector<std::string> headers = { "Authorization: Bearer " + cfg.apiKey };

    rta::net::HttpResponse resp;
    if (cfg.transport) {
        resp = cfg.transport(reqUrl, body, headers);
    } else {
        resp = rta::net::http_post(reqUrl, body, headers, "application/json", timeout_ms);
    }

    if (resp.status == 0) {
        r.error = llm_redact(cfg.apiKey, "网络请求失败: " + resp.error);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    if (resp.status < 200 || resp.status >= 300) { r.error = ds_http_error(cfg.apiKey, resp.status, text); return r; }
    r = ds_parse_fetch_response(text);
    if (!r.ok) {
        r.error = llm_redact(cfg.apiKey, r.error);
    } else {
        // E-8: 成功响应计费口径（按实际成功计）
        const std::string m = cfg.monthOverride.empty() ? llm_current_month() : cfg.monthOverride;
        llm_record_usage(m, "fetch", cfg.usagePath);
    }
    return r;
}

} // namespace rta::meta
