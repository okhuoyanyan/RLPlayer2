// test_metallm.cpp — DeepSeek 客户端（meta/llm.h）：全部离线，不发任何网络请求
//
// 判据说明（铁律 3）：
//   * 网络函数不做联网验证——这里验证的是「body 构造」与「响应解析」两条纯函数链路，
//     用写死的报文直接喂解析函数；llm_chat/ds_search/ds_fetch 的真实网络行为留给
//     有 key 时的 smoke（见 docs/元数据补全调研与规划.md §2），本测试只覆盖其离线
//     快速失败路径（空 key 不发请求）。
//   * body 断言用整串精确匹配，而不是 find 子串——格式回归（字段顺序/小数位）也要被抓到。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "meta/llm.h"
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>     // 7/8) 并发记账
#include <atomic>     // 7) 写成功计数
#include <ctime>      // 10) 月份格式化

namespace fs = std::filesystem;
using namespace rta::meta;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== meta/llm.h DeepSeek 客户端（离线）==\n");

    // 临时目录隔离：配置文件默认是相对路径，切过去测
    const fs::path tmp = fs::temp_directory_path() / "rta_metallm_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::string oldCwd = fs::current_path().string();
    fs::current_path(tmp, ec);

    // ---------------- 1) 配置往返 ----------------
    std::printf("-- 配置 --\n");
    {
        LlmConfig c1;
        c1.baseUrl = "https://api.deepseek.example";
        c1.apiKey  = "sk-roundtrip-demo";   // 假 key，仅测试占位，非真实凭据
        c1.model   = "deepseek-flash";
        ck(llm_config_save(c1, "rlplayer-llm.conf"), "config save 成功");
        ck(fs::exists("rlplayer-llm.conf") && !fs::exists("rlplayer-llm.conf.tmp"),
           "原子写：tmp 已 rename、无 .tmp 残留");

        LlmConfig c2 = llm_config_load("rlplayer-llm.conf");
        ck(c2.baseUrl == c1.baseUrl && c2.apiKey == c1.apiKey && c2.model == c1.model,
           "save→load 字段一致",
           c2.baseUrl + " | " + c2.apiKey + " | " + c2.model);

        LlmConfig c3 = llm_config_load("no-such-llm.conf");
        ck(c3.baseUrl == "https://api.deepseek.com" && c3.apiKey.empty() && c3.model == "deepseek-flash",
           "缺文件返回默认值（空 key）", c3.baseUrl + " | " + c3.model);

        // 注释 / 空行 / 缩进注释 / trim / 未知键 / 无等号行 / CRLF
        {
            std::ofstream f("hand.conf", std::ios::binary);
            f << "# 顶部注释\r\n"
              << "\r\n"
              << "   # 缩进注释也算注释\r\n"
              << "base_url = https://api.custom/v1/  \r\n"
              << "api_key=  sk-hand-123  \r\n"
              << "model =deepseek-flash\r\n"
              << "unknown_key=ignored\r\n"
              << "没有等号的行\r\n";
        }
        LlmConfig c4 = llm_config_load("hand.conf");
        ck(c4.baseUrl == "https://api.custom/v1/", "注释/空行被忽略，值 trim（base_url）", c4.baseUrl);
        ck(c4.apiKey == "sk-hand-123", "值 trim（api_key）", c4.apiKey);
        ck(c4.model == "deepseek-flash", "键名容错空白（model）", c4.model);

        // 值中间的 # 不是注释
        {
            std::ofstream f("hash.conf", std::ios::binary);
            f << "api_key=sk-with#hash\n";
        }
        LlmConfig c5 = llm_config_load("hash.conf");
        ck(c5.apiKey == "sk-with#hash", "值内 '#' 不当注释处理", c5.apiKey);
    }

    // ---------------- 1b) 界面掩码 + 合并写（2026-10-22 功能4 的红线） ----------------
    // 为什么这两条必须单独测：
    //   * 掩码是「界面永不回显明文」的唯一屏障 —— 掩码里若带整段 key，等于没做；
    //   * 合并写是「保存 key 不冲掉用户 proxy 行」的唯一保障 —— llm_config_save 是
    //     整文件重写，用它保存一次就把 proxy= 冲没了（本测试用真实文件内容做回归）。
    std::printf("-- key 掩码 / 合并写 --\n");
    {
        ck(llm_mask_key("").empty(), "空 key → 空掩码");
        ck(llm_mask_key("sk-short") == "****", "短 key → 只有 ****（连前缀都不露）",
           llm_mask_key("sk-short"));
        const std::string sk = "sk-" + std::string(32, 'a');   // 35 位，真实 DeepSeek 形态
        const std::string masked = llm_mask_key(sk);
        ck(masked == "sk-****aaaa", "35 位 key → 前缀+****+末 4 位", masked);
        ck(masked.find(sk) == std::string::npos && masked.size() <= 12,
           "掩码不含完整 key 且长度受限", masked);

        // 真实形态的 conf：注释 + proxy + 三个已知键 + 一个未知键 + CRLF
        {
            std::ofstream f("merge.conf", std::ios::binary);
            f << "# RLPlayer LLM 配置（本机运行配置，不入库不分发）\r\n"
              << "proxy=http://127.0.0.1:7897\r\n"
              << "base_url=https://api.deepseek.com\r\n"
              << "api_key=sk-old-placeholder\r\n"
              << "model=deepseek-flash\r\n"
              << "weird_key=keep-me\r\n";
        }
        LlmConfig m;
        m.baseUrl = "https://api.deepseek.com";
        m.apiKey  = "sk-new-placeholder";
        m.model   = "deepseek-chat";
        ck(llm_config_save_merge(m, "merge.conf"), "merge save 成功");
        ck(!fs::exists("merge.conf.tmp"), "合并写也是原子写（无 .tmp 残留）");
        std::string txt;
        {
            std::ifstream f("merge.conf", std::ios::binary);
            txt.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        ck(txt.find("proxy=http://127.0.0.1:7897") != std::string::npos,
           "★ proxy= 行原样保留（保存 key 不许冲掉用户的代理）");
        ck(txt.find("# RLPlayer LLM 配置") != std::string::npos, "注释行保留");
        ck(txt.find("weird_key=keep-me") != std::string::npos, "未知键保留");
        ck(txt.find("api_key=sk-new-placeholder") != std::string::npos &&
           txt.find("sk-old-placeholder") == std::string::npos,
           "api_key 被替换且旧值不残留");
        ck(txt.find("model=deepseek-chat") != std::string::npos, "model 被替换");
        auto countSub = [](const std::string& hay, const std::string& needle) {
            size_t n = 0, p = 0;
            while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
            return n;
        };
        ck(countSub(txt, "api_key=") == 1, "api_key 只剩一行（重复键去重）",
           std::to_string(countSub(txt, "api_key=")));
        const LlmConfig rt = llm_config_load("merge.conf");
        ck(rt.apiKey == "sk-new-placeholder" && rt.model == "deepseek-chat" &&
           rt.baseUrl == "https://api.deepseek.com",
           "合并写后 load 读回新值（字段名与 LlmConfig 对齐）",
           rt.baseUrl + " | " + rt.apiKey + " | " + rt.model);

        // 文件不存在 → 等价于新建（三行齐全）
        ck(llm_config_save_merge(m, "fresh.conf"), "merge save 到不存在的文件");
        const LlmConfig fresh = llm_config_load("fresh.conf");
        ck(fresh.apiKey == m.apiKey && fresh.model == m.model && fresh.baseUrl == m.baseUrl,
           "新建文件三键齐全", fresh.baseUrl + " | " + fresh.apiKey + " | " + fresh.model);

        // 原文件里重复的 api_key（手工编辑过的现场）→ 只留第一条
        {
            std::ofstream f("dup.conf", std::ios::binary);
            f << "api_key=first\napi_key=second\nmodel=x\n";
        }
        LlmConfig d; d.apiKey = "third"; d.model = "x";
        ck(llm_config_save_merge(d, "dup.conf"), "merge save 处理重复键");
        std::string dtxt;
        { std::ifstream f("dup.conf", std::ios::binary);
          dtxt.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
        ck(countSub(dtxt, "api_key=") == 1 && dtxt.find("third") != std::string::npos &&
           dtxt.find("second") == std::string::npos,
           "重复 api_key 去重为一行", dtxt);
    }

    // ---------------- 2) llm_build_chat_body 精确匹配 ----------------
    std::printf("-- chat body --\n");
    {
        std::vector<LlmMessage> msgs;
        msgs.push_back({"system", "你是音乐元数据助手"});
        msgs.push_back({"user", "歌手:\"周杰伦\"《七里香》\n专辑\t2004"});

        const std::string body = llm_build_chat_body(msgs, 800, 0.2f, false);
        const std::string expect =
            "{\"messages\":["
            "{\"role\":\"system\",\"content\":\"你是音乐元数据助手\"},"
            "{\"role\":\"user\",\"content\":\"歌手:\\\"周杰伦\\\"《七里香》\\n专辑\\t2004\"}"
            "],\"max_tokens\":800,\"temperature\":0.20}";
        ck(body == expect, "chat body 精确匹配（中文原样 + 引号/换行/制表转义）", body);

        const std::string body2 = llm_build_chat_body(msgs, 800, 0.2f, true);
        const std::string expect2 = expect.substr(0, expect.size() - 1) +
            ",\"response_format\":{\"type\":\"json_object\"}}";
        ck(body2 == expect2, "jsonObject=true 追加 response_format", body2);

        // \r 与 <0x20 控制字符、temperature 保留 2 位小数
        const std::string body3 =
            llm_build_chat_body({{"user", "line1\r\nline2" "\x01" "b"}}, 64, 1.0f, false);
        const std::string expect3 =
            "{\"messages\":[{\"role\":\"user\",\"content\":\"line1\\r\\nline2\\u0001b\"}],"
            "\"max_tokens\":64,\"temperature\":1.00}";
        ck(body3 == expect3, "\\r 与控制字符 \\u00xx 转义 + temperature 两位小数", body3);

        // 转义输出必须是合法 JSON 且能无损还原（escape↔parse 互逆）
        const std::string weird = "引号\"反斜杠\\换行\n制表\t" "\x01" "控制字";
        const rta::net::Json j = rta::net::json_parse("{\"v\":\"" + llm_esc(weird) + "\"}");
        ck(j.type == rta::net::Json::Obj && j["v"].str == weird,
           "llm_esc 输出可被 net/json.h 无损还原", j["v"].str);
    }

    // ---------------- 3) search/fetch body 精确匹配 ----------------
    std::printf("-- search/fetch body --\n");
    {
        const std::string sb = ds_build_search_body("周杰伦 出道年份", 5);
        ck(sb == "{\"query\":\"周杰伦 出道年份\",\"count\":5}", "search body 精确匹配", sb);

        const std::string fb = ds_build_fetch_body("https://example.com/a?b=1");
        ck(fb == "{\"url\":\"https://example.com/a?b=1\"}", "fetch body 精确匹配", fb);
    }

    // ---------------- 4) 响应解析 ----------------
    std::printf("-- chat 解析 --\n");
    {
        const std::string chatResp =
            "{\n  \"id\": \"chatcmpl-1\",\n  \"choices\": [\n"
            "    {\"index\": 0, \"message\": {\"role\": \"assistant\","
            " \"content\": \"《晴天》收录于周杰伦 2003 年专辑《叶惠美》。\"},"
            " \"finish_reason\": \"stop\"}\n  ],\n"
            "  \"usage\": {\"prompt_tokens\": 12, \"completion_tokens\": 34, \"total_tokens\": 46}\n}";
        const LlmReply r = llm_parse_chat_response(chatResp);
        ck(r.ok, "chat 解析 ok", r.error);
        ck(r.content == "《晴天》收录于周杰伦 2003 年专辑《叶惠美》。",
           "content 中文原样提取", r.content);
        ck(r.totalTokens == 46, "usage.total_tokens = 46", std::to_string(r.totalTokens));

        const LlmReply ru = llm_parse_chat_response(
            "{\"choices\":[{\"message\":{\"content\":\"\\u6674\\u5929\"}}],"
            "\"usage\":{\"total_tokens\":3}}");
        ck(ru.ok && ru.content == "晴天", "\\uXXXX 转义还原为 UTF-8", ru.content);

        const LlmReply re = llm_parse_chat_response(
            "{\"error\":{\"message\":\"Authentication Fails\",\"type\":\"authentication_error\"}}");
        ck(!re.ok && re.error.find("Authentication Fails") != std::string::npos,
           "error 对象 → ok=false 且带 message", re.error);

        const LlmReply rg = llm_parse_chat_response("<html>502 Bad Gateway</html>");
        ck(!rg.ok && rg.error.find("JSON") != std::string::npos, "非法 HTML → 解析失败", rg.error);

        const LlmReply rn = llm_parse_chat_response("{}");
        ck(!rn.ok, "空对象 → 缺 choices", rn.error);
    }

    std::printf("-- search 解析 --\n");
    {
        const std::string searchResp =
            "{\n  \"results\": [\n"
            "    {\"title\": \"周杰伦\", \"url\": \"https://zh.wikipedia.org/wiki/%E5%91%A8%E6%9D%B0%E4%BC%A6\","
            " \"snippet\": \"台湾创作歌手\", \"summary\": \"周杰伦，1979 年生于台北……\","
            " \"rerank_score\": 2.5, \"authority\": 3, \"date\": \"2024-06-01\"},\n"
            "    {\"title\": \"叶惠美\", \"url\": \"https://baike.baidu.com/item/叶惠美\","
            " \"snippet\": \"音乐专辑\", \"summary\": \"2003 年发行\", \"rerank_score\": 1.2, \"authority\": 2}\n"
            "  ]}";
        const DsSearchReply s = ds_parse_search_response(searchResp);
        ck(s.ok && s.hits.size() == 2, "search 解析 ok、2 条命中",
           "ok=" + std::to_string(s.ok) + " n=" + std::to_string(s.hits.size()) + " " + s.error);
        if (s.hits.size() == 2) {
            const DsHit& h0 = s.hits[0];
            ck(h0.title == "周杰伦" && h0.snippet == "台湾创作歌手" &&
               h0.rerankScore == 2.5 && h0.authority == 3 && h0.date == "2024-06-01",
               "第 1 条字段完整（rerank/authority/date）",
               h0.title + " " + std::to_string(h0.rerankScore) + " " + std::to_string(h0.authority));
            const DsHit& h1 = s.hits[1];
            ck(h1.summary == "2003 年发行" && h1.authority == 2 && h1.date.empty(),
               "第 2 条缺 date → 空", h1.summary + " | " + h1.date);
        }

        const DsSearchReply s2 = ds_parse_search_response(
            "{\"results\":[{\"title\":\"t\",\"url\":\"u\"}]}");
        ck(s2.ok && s2.hits.size() == 1 &&
           s2.hits[0].authority == -1 && s2.hits[0].rerankScore == 0.0,
           "缺省字段 → authority=-1, rerank=0",
           std::to_string(s2.hits.empty() ? 99 : s2.hits[0].authority));

        const DsSearchReply se = ds_parse_search_response(
            "{\"error\":{\"message\":\"rate limited\"}}");
        ck(!se.ok && se.error.find("rate limited") != std::string::npos, "search error 对象", se.error);

        const DsSearchReply sg = ds_parse_search_response("not json at all");
        ck(!sg.ok && sg.error.find("JSON") != std::string::npos, "search 非法 JSON", sg.error);
    }

    std::printf("-- fetch 解析 --\n");
    {
        const DsFetchReply f = ds_parse_fetch_response(
            "{\"title\": \"Example Domain\", \"content\": \"# Example\\n正文第一段\\n\"}");
        ck(f.ok && f.title == "Example Domain" && f.content == "# Example\n正文第一段\n",
           "fetch title/content 提取（含 \\n 还原）", f.title + " | " + f.content);

        const DsFetchReply fe = ds_parse_fetch_response(
            "{\"error\":{\"message\":\"fetch blocked\"}}");
        ck(!fe.ok && fe.error.find("fetch blocked") != std::string::npos, "fetch error 对象", fe.error);
    }

    // ---------------- 5) key 不外泄 + 空 key 快速失败（不发网络） ----------------
    std::printf("-- key 安全与离线失败路径 --\n");
    {
        ck(llm_redact("sk-test-key", "HTTP 401: your key is sk-test-key, bye") ==
               "HTTP 401: your key is ****, bye", "redact 替换 key 出现");
        ck(llm_redact("k", "kak") == "****a****", "redact 多次出现全替换", llm_redact("k", "kak"));
        ck(llm_redact("", "abc") == "abc", "redact 空 secret 原样返回");

        const LlmConfig noKey;   // 默认构造：apiKey 为空
        const LlmReply nf = llm_chat(noKey, {{"user", "hi"}});
        ck(!nf.ok && !nf.error.empty() && nf.error.find("api_key") != std::string::npos,
           "空 key → chat 快速失败（不发网络请求）", nf.error);
        const DsSearchReply nsf = ds_search(noKey, "query");
        ck(!nsf.ok && !nsf.error.empty(), "空 key → search 快速失败", nsf.error);
        const DsFetchReply nff = ds_fetch(noKey, "https://example.com");
        ck(!nff.ok && !nff.error.empty(), "空 key → fetch 快速失败", nff.error);

        // 假 key 灌进错误文本再 redact：错误里不得出现 key 全文
        const std::string guarded = llm_redact("sk-demo-42", "echoed key: sk-demo-42 end");
        ck(guarded.find("sk-demo-42") == std::string::npos, "错误文本经 redact 后不含 key", guarded);
    }

    // ---------------- 6) E-8: DeepSeek 月度调用上限与超限降级（离线 mock transport） ----------------
    std::printf("-- E-8: 月度调用上限与超限降级（mock transport 离线测试）--\n");
    {
        int httpCalls = 0;
        std::string lastReqUrl;
        std::string lastReqBody;
        int nextStatus = 200;
        std::string nextResponseBody;

        auto mockTransport = [&](const std::string& url, const std::string& body, const std::vector<std::string>& headers) -> rta::net::HttpResponse {
            httpCalls++;
            lastReqUrl = url;
            lastReqBody = body;
            rta::net::HttpResponse resp;
            resp.status = nextStatus;
            if (nextStatus != 200) {
                resp.error = "Mock HTTP Error";
            }
            resp.body.assign(nextResponseBody.begin(), nextResponseBody.end());
            return resp;
        };

        const std::string testUsagePath = (tmp / "test-usage.json").string();

        LlmConfig cfg;
        cfg.apiKey = "sk-mock-key-secret-123456789";
        cfg.baseUrl = "https://api.deepseek.mock";
        cfg.model = "deepseek-flash";
        cfg.monthlyCap = 2; // 上限 2 次
        cfg.usagePath = testUsagePath;
        cfg.monthOverride = "202610";
        cfg.transport = mockTransport;

        // 6.1 cap=2 时调用 1 与调用 2 成功，调用 3 不发请求直接拦截
        nextStatus = 200;
        nextResponseBody = "{\"choices\":[{\"message\":{\"content\":\"OK 1\"}}]}";
        const LlmReply r1 = llm_chat(cfg, {{"user", "q1"}});
        ck(r1.ok && r1.content == "OK 1", "第 1 次 chat 成功");
        ck(httpCalls == 1, "第 1 次 chat 真实发起 HTTP (httpCalls=1)");
        ck(llm_get_monthly_count("202610", testUsagePath) == 1, "202610 计数为 1");

        nextResponseBody = "{\"results\":[{\"title\":\"res1\",\"url\":\"https://example.com/1\"}]}";
        const DsSearchReply r2 = ds_search(cfg, "query2");
        ck(r2.ok && r2.hits.size() == 1, "第 2 次 search 成功");
        ck(httpCalls == 2, "第 2 次 search 真实发起 HTTP (httpCalls=2)");
        ck(llm_get_monthly_count("202610", testUsagePath) == 2, "202610 计数为 2");

        // 第 3 次调用 (fetch)：已达 cap=2，必须不发请求且返回明确错误
        nextResponseBody = "{\"title\":\"t\",\"content\":\"c\"}";
        const DsFetchReply r3 = ds_fetch(cfg, "https://example.com/3");
        ck(!r3.ok, "第 3 次 fetch 超限被拦截（ok=false）");
        ck(r3.error.find("本月 AI 调用已达上限") != std::string::npos,
           "超限返回明确错误「本月 AI 调用已达上限」", r3.error);
        ck(httpCalls == 2, "★ 关键断言：超限调用不发起 HTTP 请求 (httpCalls 仍为 2)",
           std::to_string(httpCalls));
        ck(llm_get_monthly_count("202610", testUsagePath) == 2, "计数未被超限调用污染 (仍为 2)");

        // chat 同样被超限拦截
        const LlmReply r3_chat = llm_chat(cfg, {{"user", "q3"}});
        ck(!r3_chat.ok && r3_chat.error.find("本月 AI 调用已达上限") != std::string::npos,
           "超限后 chat 同样被拦截", r3_chat.error);
        ck(httpCalls == 2, "chat 被拦截不发 HTTP (httpCalls 仍为 2)");

        // 6.2 换月后恢复（202611 分桶从 0 开始，自动清零恢复）
        cfg.monthOverride = "202611";
        nextResponseBody = "{\"title\":\"Doc Nov\",\"content\":\"Nov Content\"}";
        const DsFetchReply r4 = ds_fetch(cfg, "https://example.com/nov");
        ck(r4.ok && r4.title == "Doc Nov", "换月到 202611 后自动恢复可用");
        ck(httpCalls == 3, "换月后调用发起 HTTP 请求 (httpCalls 增加到 3)");
        ck(llm_get_monthly_count("202611", testUsagePath) == 1, "202611 计数为 1");
        ck(llm_get_monthly_count("202610", testUsagePath) == 2, "旧月份 202610 计数保持 2 不变（分桶独立）");

        // 6.3 cap=0 不受限（默认行为）
        cfg.monthlyCap = 0; // 不限
        cfg.monthOverride = "202610"; // 哪怕在已经有 2 次调用的 202610 月份
        nextResponseBody = "{\"choices\":[{\"message\":{\"content\":\"Unlimited 1\"}}]}";
        const LlmReply r5 = llm_chat(cfg, {{"user", "q5"}});
        ck(r5.ok && r5.content == "Unlimited 1", "cap=0 不受限，在旧月份依然可调用");
        ck(httpCalls == 4, "cap=0 正常发起 HTTP (httpCalls=4)");
        ck(llm_get_monthly_count("202610", testUsagePath) == 3, "cap=0 依然正确累加用量统计 (3)");

        // 6.4 计费口径：失败/空响应不计（按实际成功计）
        cfg.monthlyCap = 5;
        const int countBeforeFail = llm_get_monthly_count("202610", testUsagePath);
        // a) HTTP 500 服务端报错
        nextStatus = 500;
        nextResponseBody = "{\"error\":{\"message\":\"Internal Server Error\"}}";
        const LlmReply rFail1 = llm_chat(cfg, {{"user", "fail1"}});
        ck(!rFail1.ok, "HTTP 500 返回失败");
        ck(llm_get_monthly_count("202610", testUsagePath) == countBeforeFail,
           "口径验证：HTTP 500 失败调用不计入月度配额",
           std::to_string(llm_get_monthly_count("202610", testUsagePath)));

        // b) 网络断开 (status=0)
        nextStatus = 0;
        nextResponseBody = "";
        const DsSearchReply rFail2 = ds_search(cfg, "netfail");
        ck(!rFail2.ok, "网络错误 status=0 返回失败");
        ck(llm_get_monthly_count("202610", testUsagePath) == countBeforeFail,
           "口径验证：网络失败调用不计入月度配额");

        // c) 非法/空响应
        nextStatus = 200;
        nextResponseBody = "invalid json";
        const DsFetchReply rFail3 = ds_fetch(cfg, "https://example.com/badjson");
        ck(!rFail3.ok, "非法响应解析失败");
        ck(llm_get_monthly_count("202610", testUsagePath) == countBeforeFail,
           "口径验证：非法/空响应不计入月度配额");

        // 6.5 持久化与安全性：检查用量 JSON 文件结构，断言无 key 泄露
        ck(fs::exists(testUsagePath), "用量文件 test-usage.json 存在");
        std::string usageContent;
        {
            std::ifstream uf(testUsagePath, std::ios::binary);
            usageContent.assign((std::istreambuf_iterator<char>(uf)), std::istreambuf_iterator<char>());
        }
        ck(usageContent.find("202610") != std::string::npos && usageContent.find("202611") != std::string::npos,
           "用量文件包含 202610 与 202611 分桶");
        ck(usageContent.find("total") != std::string::npos && usageContent.find("chat") != std::string::npos,
           "用量文件包含 total, chat, search, fetch 字段");
        ck(usageContent.find("sk-mock-key") == std::string::npos,
           "★ 红线断言：用量持久化文件中绝无 API Key 泄露");

        // 6.6 配置项往返：llmmonthlycap
        LlmConfig capCfg;
        capCfg.monthlyCap = 500;
        capCfg.apiKey = "sk-cap-save-test";
        ck(llm_config_save_merge(capCfg, "cap_test.conf"), "save_merge 带 llmmonthlycap");
        LlmConfig loadedCap = llm_config_load("cap_test.conf");
        ck(loadedCap.monthlyCap == 500, "load 读回 llmmonthlycap=500", std::to_string(loadedCap.monthlyCap));
    }

    // ---------------- 7) 并发记账：8 线程 × 100 次，总量必须 800（不许丢更新） ----------------
    // 判据（铁律 3）：为什么要这条断言 —— 旧实现是「读 JSON → +1 → 写固定 .tmp → rename」的
    // 无锁 read-modify-write。两条线程只要读到同一个旧值，最终值就必然 < 800，且无法"运气凑回"。
    // 因此 total==800 对无锁实现是致命判据，对加锁实现是充分判据。
    std::printf("-- 7) 并发记账 8 线程 × 100 次 --\n");
    {
        const std::string racePath = (tmp / "race-usage.json").string();
        const std::string month = "202612";
        std::error_code rec;
        fs::remove(racePath, rec);
        fs::remove(racePath + ".bak", rec);
        ck(llm_get_monthly_count(month, racePath) == 0, "并发测试起点计数为 0");

        const int kThreads = 8, kPer = 100;
        std::atomic<int> okWrites{0}, badWrites{0};
        std::vector<std::thread> ths;
        ths.reserve((size_t)kThreads);
        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&, t]() {
                static const char* kTypes[3] = {"chat", "search", "fetch"};
                for (int i = 0; i < kPer; ++i) {
                    const char* ty = kTypes[(t + i) % 3];
                    if (llm_record_usage(month, ty, racePath)) okWrites.fetch_add(1);
                    else badWrites.fetch_add(1);
                }
            });
        }
        for (auto& th : ths) th.join();

        ck(okWrites.load() == kThreads * kPer, "800 次 record 全部返回成功",
           std::to_string(okWrites.load()));
        ck(badWrites.load() == 0, "0 次写失败（写失败会自动重试 5 次）", std::to_string(badWrites.load()));

        LlmUsageRead d7;
        const LlmMonthlyUsage u7 = llm_get_usage(month, racePath, &d7);
        ck(u7.total == 800, "★ 8×100 并发记账后 total == 800（丢更新会 <800）",
           std::to_string(u7.total));
        ck(u7.chat == 267 && u7.search == 267 && u7.fetch == 266,
           "分类型计数精确 267/267/266（按 (线程+序号) mod 3 轮转的期望值）",
           std::to_string(u7.chat) + "/" + std::to_string(u7.search) + "/" + std::to_string(u7.fetch));
        ck(u7.chat + u7.search + u7.fetch == u7.total, "chat+search+fetch == total（无类别漏计）");
        ck(!d7.corrupt && !d7.fromBackup && d7.error.empty(),
           "并发写后主文件干净（未触发 .bak 回退）", d7.error);

        // 文件层面（不经 API）：固定 tmp 名互踩的表现就是这里读到半截文档
        std::string raw;
        bool hasRaw = false;
        {
            std::ifstream f(racePath, std::ios::binary);
            if (f) { raw.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); hasRaw = true; }
        }
        ck(hasRaw && llm_usage_json_complete(raw), "并发 800 次后主文件结构完整（括号/引号配平）",
           hasRaw ? (raw.size() > 80 ? raw.substr(0, 60) + "..." : raw) : std::string("<missing>"));
        std::map<std::string, LlmMonthlyUsage> parsed7;
        std::string perr7;
        ck(llm_usage_parse_strict(raw, parsed7, &perr7) &&
           parsed7.count(month) == 1 && parsed7[month].total == 800,
           "严格解析：JSON 可解析且 total==800", perr7);

        int leftover = 0;
        for (const auto& de : fs::directory_iterator(tmp)) {
            const std::string nm = de.path().filename().string();
            if (nm.rfind("race-usage.json", 0) == 0 && nm.size() > 4 &&
                nm.compare(nm.size() - 4, 4, ".tmp") == 0) ++leftover;
        }
        ck(leftover == 0, "无残留 .tmp（唯一 tmp 名 + 失败清理）", std::to_string(leftover));
        ck(fs::exists(racePath + ".bak"), ".bak 已生成（上一份 good 备份）");
    }

    // ---------------- 7b) 4 路并行 ds_fetch：retrieval.h:1748 的真实调用形状 ----------------
    // 上面的单元断言之外，再走一遍真实链路（mock transport，不联网）：
    // 4 线程 → ds_fetch → llm_check_cap / llm_record_usage，正是外部评审点名的并发形状。
    std::printf("-- 7b) 4 路并行 ds_fetch（retrieval.h 并行 fetch 的真实形状）--\n");
    {
        const std::string p4 = (tmp / "parallel-fetch-usage.json").string();
        const std::string m4 = "202614";
        std::error_code rec;
        fs::remove(p4, rec);
        fs::remove(p4 + ".bak", rec);

        std::atomic<int> hits{0};
        LlmConfig cfg4;
        cfg4.apiKey = "sk-mock-parallel";
        cfg4.baseUrl = "https://api.deepseek.mock";
        cfg4.model = "deepseek-flash";
        cfg4.monthlyCap = 0;          // 本用例只测记账，不测熔断
        cfg4.monthOverride = m4;
        cfg4.usagePath = p4;
        cfg4.transport = [&hits](const std::string&, const std::string&,
                                 const std::vector<std::string>&) -> rta::net::HttpResponse {
            hits.fetch_add(1);
            rta::net::HttpResponse resp;
            resp.status = 200;
            const std::string body = "{\"title\":\"T\",\"content\":\"正文\"}";
            resp.body.assign(body.begin(), body.end());
            return resp;
        };

        std::atomic<int> oks{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; ++t)
            ths.emplace_back([&cfg4, &oks, t]() {
                if (ds_fetch(cfg4, "https://example.com/p" + std::to_string(t)).ok) oks.fetch_add(1);
            });
        for (auto& th : ths) th.join();

        ck(hits.load() == 4, "4 个 fetch 都真实发起（mock transport 计数=4）", std::to_string(hits.load()));
        ck(oks.load() == 4, "4 个 fetch 都返回成功", std::to_string(oks.load()));
        const LlmMonthlyUsage u4 = llm_get_usage(m4, p4);
        ck(u4.total == 4 && u4.fetch == 4,
           "★ 4 路并行 fetch 记满 4 次（旧实现会 <4，丢的那次配额就白给了）",
           std::to_string(u4.total) + " total / " + std::to_string(u4.fetch) + " fetch");
    }

    // ---------------- 8) 并发写 200 次后 JSON 仍可解析 ----------------
    std::printf("-- 8) 并发写 200 次后 JSON 仍可解析 --\n");
    {
        const std::string p8 = (tmp / "burst-usage.json").string();
        const std::string m8 = "202613";
        std::error_code rec;
        fs::remove(p8, rec);
        fs::remove(p8 + ".bak", rec);

        std::vector<std::thread> ths;
        for (int t = 0; t < 8; ++t)
            ths.emplace_back([&p8, &m8]() {
                for (int i = 0; i < 25; ++i) llm_record_usage(m8, "fetch", p8);
            });
        for (auto& th : ths) th.join();

        std::string raw;
        bool has = false;
        {
            std::ifstream f(p8, std::ios::binary);
            if (f) { raw.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); has = true; }
        }
        ck(has && llm_usage_json_complete(raw),
           "200 次并发写后磁盘上仍是一个完整 JSON 文档（无拼接/截断/半写）");
        std::map<std::string, LlmMonthlyUsage> parsed8;
        std::string perr8;
        ck(llm_usage_parse_strict(raw, parsed8, &perr8), "严格解析通过（可被读取侧接受）", perr8);
        ck(parsed8.count(m8) == 1 && parsed8[m8].total == 200,
           "total == 200（8 线程 × 25 次，一次不漏）",
           parsed8.count(m8) ? std::to_string(parsed8[m8].total) : std::string("missing"));
        ck(parsed8.count(m8) == 1 && parsed8[m8].fetch == 200, "fetch == 200",
           parsed8.count(m8) ? std::to_string(parsed8[m8].fetch) : std::string("missing"));
    }

    // ---------------- 9) 半写坏文件：报可读错误 + 自动回退 .bak ----------------
    // 判据：不能只断言"能读出来"——必须同时满足
    //   ① 读取侧明确报错（corrupt/error 非空，且 WARN 计数增加）② 数据不是 0 而是 .bak 的值
    //   ③ 后续记账以 .bak 为基准继续累加（不是从 0 重来）④ 坏内容没有覆盖 .bak
    std::printf("-- 9) 半写坏文件：可读错误 + .bak 回退 --\n");
    {
        const std::string p9 = (tmp / "corrupt-usage.json").string();
        const std::string m9 = "202610";
        std::error_code rec;
        fs::remove(p9, rec);
        fs::remove(p9 + ".bak", rec);

        for (int i = 0; i < 5; ++i) llm_record_usage(m9, "chat", p9);
        ck(llm_get_monthly_count(m9, p9) == 5, "正常记账 5 次后 total=5");
        ck(fs::exists(p9 + ".bak"), ".bak 已存在");
        ck(llm_get_monthly_count(m9, p9 + ".bak") == 4, ".bak 是上一份 good（total=4）",
           std::to_string(llm_get_monthly_count(m9, p9 + ".bak")));

        // 模拟半写：只落了前半截字节（并发踩 tmp / 断电都长这样）
        {
            std::ofstream f(p9, std::ios::binary | std::ios::trunc);
            const std::string half =
                "{\n  \"202610\": {\n    \"total\": 5,\n    \"chat\": 5,\n"
                "    \"search\": 0,\n    \"fetch\": 0";
            f.write(half.data(), (std::streamsize)half.size());
        }
        const unsigned warnBefore = llm_usage_warn_count();
        LlmUsageRead d9;
        const LlmMonthlyUsage u9 = llm_get_usage(m9, p9, &d9);
        ck(d9.corrupt, "★ 半写文件被判为损坏（旧实现会静默按 0）", d9.error);
        ck(!d9.error.empty(), "报出可读错误文本", d9.error);
        ck(d9.ok && d9.fromBackup, "自动回退到 .bak", d9.error);
        ck(u9.total == 4, "回退后读到的是 .bak 的 total=4（不是 0）", std::to_string(u9.total));
        ck(llm_usage_warn_count() > warnBefore, "损坏时确实留下 WARN（计数增加）",
           std::to_string(llm_usage_warn_count() - warnBefore));
        // 只打 stderr 在 GUI 里等于没打（启动器丢弃），WARN 必须同时落盘
        const std::string warnLog = p9 + ".warn.log";
        std::string wl;
        {
            std::ifstream f(warnLog, std::ios::binary);
            if (f) wl.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        ck(fs::exists(warnLog) && wl.find("[WARN]") != std::string::npos &&
           wl.find("损坏") != std::string::npos,
           "WARN 已落盘 <usage>.warn.log（GUI 丢弃 stderr 也留痕）",
           wl.size() > 100 ? wl.substr(0, 100) : wl);

        // 继续记账 → 以 .bak(4) 为基准 +1 = 5；若"按 0 重算"这里会是 1
        ck(llm_record_usage(m9, "chat", p9), "损坏后继续记账成功");
        const LlmMonthlyUsage u9b = llm_get_usage(m9, p9);
        ck(u9b.total == 5, "★ 恢复：total = .bak(4)+1 = 5（按 0 重算会是 1）", std::to_string(u9b.total));
        ck(u9b.chat == 5, "chat 一并恢复为 5", std::to_string(u9b.chat));
        ck(fs::exists(p9 + ".bak") && llm_get_monthly_count(m9, p9 + ".bak") == 4,
           "坏内容没有覆盖 .bak（仍可读回 4）");

        // 另一种损坏形态：两个文档拼接（并发互踩 tmp 的典型产物）
        {
            std::ofstream f(p9, std::ios::binary | std::ios::trunc);
            f << "{\"202610\": {\"total\": 5, \"chat\": 5, \"search\": 0, \"fetch\": 0}}\n"
              << "{\"202610\": {\"total\": 7, \"chat\": 7, \"search\": 0, \"fetch\": 0}}\n";
        }
        ck(!llm_usage_json_complete("{\"a\":1}{\"b\":2}"), "结构校验：拼接文档判为不完整");
        LlmUsageRead d9c;
        const LlmMonthlyUsage u9c = llm_get_usage(m9, p9, &d9c);
        ck(d9c.corrupt && d9c.fromBackup && u9c.total == 4,
           "拼接损坏同样走 .bak 回退（total=4）", d9c.error);

        // 0 字节文件（rename 前崩溃的残留）不是损坏，是"全新账本"
        const std::string pz = (tmp / "empty-usage.json").string();
        { std::ofstream f(pz, std::ios::binary | std::ios::trunc); }
        LlmUsageRead dz;
        const LlmMonthlyUsage uz = llm_get_usage("202610", pz, &dz);
        ck(uz.total == 0 && !dz.corrupt && dz.error.empty(), "0 字节文件按全新账本处理（不误报损坏）",
           dz.error);
    }

    // ---------------- 10) 月份格式化线程安全（std::localtime 静态缓冲 → localtime_s） ----------------
    std::printf("-- 10) 月份格式化 8 线程并发 --\n");
    {
        const std::time_t fixed = (std::time_t)1790000000;   // 固定时间戳 = 单线程基准
        const std::string expect = llm_format_month(fixed);
        std::atomic<int> bad{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 8; ++t)
            ths.emplace_back([&]() {
                for (int i = 0; i < 2000; ++i) {
                    if (llm_format_month(fixed) != expect) bad.fetch_add(1);
                }
            });
        for (auto& th : ths) th.join();
        ck(bad.load() == 0, "8 线程 × 2000 次格式化：零撕裂（localtime_s）",
           std::to_string(bad.load()));
        bool digits = expect.size() == 6;
        for (const char c : expect) if (c < '0' || c > '9') digits = false;
        ck(digits, "月份格式为 6 位 YYYYMM", expect);
    }

    // ---------------- 11) 写失败重试与清理：目标被独占打开 → rename 必然失败 ----------------
    // 判据：失败必须"诚实"（返回 false、不假装成功）、必须留 WARN、必须不留 .tmp 垃圾；
    // 并且句柄释放后能立刻恢复正常记账。这条路径不测就等于没写重试逻辑。
    std::printf("-- 11) 写失败重试与 .tmp 清理（目标被独占）--\n");
    {
        const std::string p11 = (tmp / "locked-usage.json").string();
        std::error_code rec;
        fs::remove(p11, rec);
        fs::remove(p11 + ".bak", rec);
        ck(llm_record_usage("202610", "chat", p11), "首次记账成功（建出文件）");

        HANDLE h = CreateFileW(fs::path(p11).c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        ck(h != INVALID_HANDLE_VALUE, "以不给 FILE_SHARE_DELETE 的方式独占打开目标文件");
        const unsigned warnBefore11 = llm_usage_warn_count();
        const bool ok11 = llm_record_usage("202610", "chat", p11);
        ck(!ok11, "★ 目标不可替换时返回 false（不假装记账成功）", ok11 ? "true" : "false");
        ck(llm_usage_warn_count() > warnBefore11, "写失败留 WARN",
           std::to_string(llm_usage_warn_count() - warnBefore11));
        int tmpLeft = 0;
        for (const auto& de : fs::directory_iterator(tmp)) {
            const std::string nm = de.path().filename().string();
            if (nm.rfind("locked-usage.json", 0) == 0 && nm.size() > 4 &&
                nm.compare(nm.size() - 4, 4, ".tmp") == 0) ++tmpLeft;
        }
        ck(tmpLeft == 0, "5 次重试全部失败后没有残留 .tmp", std::to_string(tmpLeft));
        ck(llm_get_monthly_count("202610", p11) == 1, "磁盘上的账本没有被写坏（仍为 1）",
           std::to_string(llm_get_monthly_count("202610", p11)));

        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        ck(llm_record_usage("202610", "chat", p11), "句柄释放后恢复正常记账");
        ck(llm_get_monthly_count("202610", p11) == 2, "恢复后计数 1→2", 
           std::to_string(llm_get_monthly_count("202610", p11)));
    }

    fs::current_path(oldCwd, ec);
    fs::remove_all(tmp, ec);

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
