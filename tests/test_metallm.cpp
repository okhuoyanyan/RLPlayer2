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
#include <filesystem>
#include <fstream>

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

    fs::current_path(oldCwd, ec);
    fs::remove_all(tmp, ec);

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
