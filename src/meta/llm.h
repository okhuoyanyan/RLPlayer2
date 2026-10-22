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
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <iterator>

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
// 配置（rlplayer-llm.conf：纯文本 key=value，# 注释，空行忽略）
// ---------------------------------------------------------------------------

struct LlmConfig {
    std::string baseUrl = "https://api.deepseek.com";
    std::string apiKey;                    // 只进请求头；缺文件时为空
    std::string model   = "deepseek-flash";
};

// 缺文件 → 返回默认值（空 key）。key 名：base_url / api_key / model；其他键忽略。
inline LlmConfig llm_config_load(const std::string& path = "rlplayer-llm.conf") {
    LlmConfig c;
    std::ifstream f(path, std::ios::binary);
    if (!f) return c;
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
        } else {
            line += all[i];
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

// 合并写（界面保存 key 用）：只替换 base_url / api_key / model 三行，
// 其余行【原样保留】—— 尤其是 proxy=host:port（P1.5 的后备代理）与注释。
// 为什么不能用上面的 llm_config_save：它是整文件重写，界面保存一次 key 就会把
// 用户的 proxy 行冲掉（2026-10-22 功能4 的红线）。文件不存在时等价于新建。
inline bool llm_config_save_merge(const LlmConfig& c, const std::string& path = "rlplayer-llm.conf") {
    std::vector<std::string> out;
    bool wroteBase = false, wroteKey = false, wroteModel = false;
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
            } else {
                out.push_back(line);                     // 注释 / 空行 / proxy= / 未知键
            }
        }
    }
    if (!wroteBase) out.push_back("base_url=" + c.baseUrl);
    if (!wroteKey) out.push_back("api_key=" + c.apiKey);
    if (!wroteModel) out.push_back("model=" + c.model);
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
// chat completions
// ---------------------------------------------------------------------------

struct LlmMessage { std::string role, content; };
struct LlmReply {
    bool ok = false;
    std::string content, error;
    int totalTokens = 0;
};

// 完整请求体（含 model），llm_chat 用
inline std::string llm_build_chat_body(const std::string& model,
                                       const std::vector<LlmMessage>& msgs,
                                       int maxTokens, float temperature, bool jsonObject) {
    std::ostringstream o;
    o << "{";
    if (!model.empty()) o << "\"model\":\"" << llm_esc(model) << "\",";
    o << "\"messages\":[";
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (i) o << ",";
        o << "{\"role\":\"" << llm_esc(msgs[i].role)
          << "\",\"content\":\"" << llm_esc(msgs[i].content) << "\"}";
    }
    o << "],\"max_tokens\":" << maxTokens;
    char tb[32];
    std::snprintf(tb, sizeof(tb), "%.2f", (double)temperature);   // 保留 2 位小数
    o << ",\"temperature\":" << tb;
    if (jsonObject) o << ",\"response_format\":{\"type\":\"json_object\"}";
    o << "}";
    return o.str();
}

// 纯函数（可测）：不含 model 的请求体，model 由 llm_chat 注入
inline std::string llm_build_chat_body(const std::vector<LlmMessage>& msgs,
                                       int maxTokens = 800, float temperature = 0.2f,
                                       bool jsonObject = false) {
    return llm_build_chat_body(std::string(), msgs, maxTokens, temperature, jsonObject);
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
    const std::string body = llm_build_chat_body(cfg.model, msgs, maxTokens, temperature, jsonObject);
    const rta::net::HttpResponse resp = rta::net::http_post(
        llm_base_url(cfg.baseUrl) + "/chat/completions", body,
        { "Authorization: Bearer " + cfg.apiKey }, "application/json", 60000);
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
    if (!r.ok) r.error = llm_redact(cfg.apiKey, r.error);
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
    const rta::net::HttpResponse resp = rta::net::http_post(
        llm_base_url(cfg.baseUrl) + "/v1/tools/search",
        ds_build_search_body(query, count),
        { "Authorization: Bearer " + cfg.apiKey }, "application/json", 60000);
    if (resp.status == 0) {
        r.error = llm_redact(cfg.apiKey, "网络请求失败: " + resp.error);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    if (resp.status < 200 || resp.status >= 300) { r.error = ds_http_error(cfg.apiKey, resp.status, text); return r; }
    r = ds_parse_search_response(text);
    if (!r.ok) r.error = llm_redact(cfg.apiKey, r.error);
    return r;
}

// POST {base}/v1/tools/fetch，超时 60s，单次调用不重试
inline DsFetchReply ds_fetch(const LlmConfig& cfg, const std::string& url) {
    DsFetchReply r;
    if (cfg.apiKey.empty()) { r.error = "未配置 api_key（rlplayer-llm.conf）"; return r; }
    const rta::net::HttpResponse resp = rta::net::http_post(
        llm_base_url(cfg.baseUrl) + "/v1/tools/fetch",
        ds_build_fetch_body(url),
        { "Authorization: Bearer " + cfg.apiKey }, "application/json", 60000);
    if (resp.status == 0) {
        r.error = llm_redact(cfg.apiKey, "网络请求失败: " + resp.error);
        return r;
    }
    const std::string text(resp.body.begin(), resp.body.end());
    if (resp.status < 200 || resp.status >= 300) { r.error = ds_http_error(cfg.apiKey, resp.status, text); return r; }
    r = ds_parse_fetch_response(text);
    if (!r.ok) r.error = llm_redact(cfg.apiKey, r.error);
    return r;
}

} // namespace rta::meta
