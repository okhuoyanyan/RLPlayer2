// test_retrieval.cpp — P3 资料层③ 检索层测试（离线为主，真实网络 opt-in）
//
// 覆盖任务书交付物 [3]：
//   1. filter_hits：阈值边界（1.49/1.5）、authority 边界、白名单命中/拒绝、去重、排序、上限、过滤原因统计
//   2. parse_search_response：正常/缺字段/空 results/畸形 JSON
//   3. parse_synthesis_reply：正常；每段无引用→拒绝；citation id 悬空→拒绝；url 不在允许集合→拒绝；
//      非 JSON→错误；只有部分段落合规→拒绝（不许部分接受）
//   4. build_synthesis_prompt：硬约束文本（「只依据材料」「JSON 输出」「每条引用」）、材料编号 [M1]/[M2]
//   5. 缓存：round-trip / TTL 判定 / 离线回退 / 损坏容错
//   6. 真实网络用例 opt-in（RTA_NET_TESTS=1 开启，默认 [SKIP] exit 0）
//   7. llm_chat_stream（2026-10-24，用户反馈 261024-05）：SSE 逐 delta / 中途 [DONE] /
//      非流式回退 / 半截断开必须报错 / 重试重新开流
//   8. 摘要生成进度回调：阶段（检索/生成/完成）+ 已生成字符数 + 部分文本；
//      重试清零重开；传与不传 ProgressFn 结果逐字段一致（向后兼容判据）
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "meta/retrieval.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using namespace rta::meta;
namespace fsx = std::filesystem;

static int g_fail = 0;
static void ck(bool cond, const std::string& what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what.c_str());
    else { std::printf("  [FAIL] %s  %s\n", what.c_str(), detail.c_str()); ++g_fail; }
}

// ---------------------------------------------------------------------------
// 夹具定义（写死报文）
// ---------------------------------------------------------------------------

static const char* kFixtureSearchNormal = R"json({
  "results": [
    {
      "title": "ZARD - 维基百科",
      "url": "https://zh.wikipedia.org/wiki/ZARD",
      "snippet": "ZARD是日本著名音乐团体，以坂井泉水为主唱。",
      "summary": "ZARD生平与作品概述",
      "date": "2024-01-01",
      "rerank_score": 2.15,
      "authority": 4
    },
    {
      "title": "ZARD Discography - Discogs",
      "url": "https://www.discogs.com/artist/ZARD",
      "snippet": "Explore ZARD releases and credits.",
      "summary": "唱片列表",
      "date": "2023-11-20",
      "rerank_score": 1.82,
      "authority": 3
    }
  ]
})json";

static const char* kFixtureSearchMissingFields = R"json({
  "results": [
    {
      "title": "Partial Hit",
      "url": "https://zh.wikipedia.org/wiki/Partial",
      "snippet": "Just snippet",
      "rerank_score": 1.60
    }
  ]
})json";

static const char* kFixtureSearchEmptyResults = R"json({
  "results": []
})json";

static const char* kFixtureSearchMalformed = R"json({
  "bad_root": 123
})json";

static const char* kFixtureSynthesisValid = R"json({
  "disclaimer": "AI 生成，仅依据下方来源材料综合；可能不完整，请以来源为准",
  "sections": [
    {
      "heading": "生平与创作背景",
      "paragraphs": [
        {
          "text": "ZARD是以主唱坂井泉水为核心的音乐团体，于1991年出道。",
          "citations": ["c1"]
        },
        {
          "text": "其代表作《负けないで》作为应援歌广为传唱，并在Oricon取得百万销量。",
          "citations": ["c1", "c2"]
        }
      ]
    }
  ],
  "citations": [
    {
      "id": "c1",
      "title": "ZARD - 维基百科",
      "url": "https://zh.wikipedia.org/wiki/ZARD",
      "source": "wikipedia-zh",
      "fetched_at": 1792690000
    },
    {
      "id": "c2",
      "title": "ZARD Discography - Discogs",
      "url": "https://www.discogs.com/artist/ZARD",
      "source": "discogs",
      "fetched_at": 1792690100
    }
  ]
})json";

// ---------------------------------------------------------------------------
// 测试主体
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 2026-10-24（用户反馈 261024-05「LLM 做流式比较好，不用枯等」）的离线夹具
// ---------------------------------------------------------------------------

static rta::net::HttpResponse resp_ok(const std::string& body, const char* ctype = "text/event-stream") {
    rta::net::HttpResponse r;
    r.ok = true;
    r.status = 200;
    r.contentType = ctype;
    r.body.assign(body.begin(), body.end());
    return r;
}

static rta::net::HttpResponse resp_status(int status, const std::string& body) {
    rta::net::HttpResponse r;
    r.ok = false;
    r.status = status;
    r.body.assign(body.begin(), body.end());
    return r;
}

// 一条 SSE 正文增量事件
static std::string sse_delta(const std::string& piece) {
    return "data: {\"id\":\"mock\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,"
           "\"delta\":{\"content\":\"" + llm_esc(piece) + "\"},\"finish_reason\":null}]}\n\n";
}

// 收尾事件（finish_reason + usage），DeepSeek/OpenAI 流式协议的最后一条正文事件
static std::string sse_stop(int totalTokens = 30) {
    return "data: {\"id\":\"mock\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
           "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":20,\"total_tokens\":" +
           std::to_string(totalTokens) + "}}\n\n";
}

// 普通（非流式）chat.completion 信封 —— 模拟"服务端忽略了 stream"
static std::string plain_chat_json(const std::string& content, int totalTokens = 30) {
    return std::string("{\"id\":\"mock\",\"object\":\"chat.completion\",\"choices\":[{\"index\":0,"
                       "\"message\":{\"role\":\"assistant\",\"content\":\"") +
           llm_esc(content) +
           "\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":20,"
           "\"total_tokens\":" + std::to_string(totalTokens) + "}}";
}

// 按字节切成 n 段（切点可能落在 UTF-8 中间 —— 专治"按字符切"的拼接 bug）
static std::vector<std::string> split_bytes(const std::string& s, size_t n) {
    std::vector<std::string> out;
    if (n == 0) n = 1;
    for (size_t i = 0; i < n; ++i) {
        const size_t b = s.size() * i / n;
        const size_t e = s.size() * (i + 1) / n;
        out.push_back(s.substr(b, e - b));
    }
    return out;
}

// 逐字段比较（排除随时间的字段：generated_at / expires_at / diag / cache_ver）
static bool summary_core_equal(const SummaryResult& a, const SummaryResult& b) {
    if (a.ok != b.ok || a.ai != b.ai || a.model != b.model || a.disclaimer != b.disclaimer) return false;
    if (a.validation_ok != b.validation_ok || a.unverified != b.unverified) return false;
    if (a.offline != b.offline || a.from_cache != b.from_cache || a.error != b.error) return false;
    if (a.sections.size() != b.sections.size() || a.citations.size() != b.citations.size()) return false;
    for (size_t i = 0; i < a.sections.size(); ++i) {
        if (a.sections[i].heading != b.sections[i].heading) return false;
        if (a.sections[i].paragraphs.size() != b.sections[i].paragraphs.size()) return false;
        for (size_t k = 0; k < a.sections[i].paragraphs.size(); ++k) {
            if (a.sections[i].paragraphs[k].text != b.sections[i].paragraphs[k].text) return false;
            if (a.sections[i].paragraphs[k].citations != b.sections[i].paragraphs[k].citations) return false;
        }
    }
    for (size_t i = 0; i < a.citations.size(); ++i) {
        if (a.citations[i].id != b.citations[i].id || a.citations[i].title != b.citations[i].title) return false;
        if (a.citations[i].url != b.citations[i].url || a.citations[i].source != b.citations[i].source) return false;
        if (a.citations[i].fetched_at != b.citations[i].fetched_at) return false;
    }
    if (a.materials_used != b.materials_used) return false;
    if (a.fields != b.fields) return false;
    return true;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== meta/retrieval.h 检索层单元测试 ==\n");

    // ---------------- 1) filter_hits 过滤纯函数 ----------------
    std::printf("-- 1. filter_hits 阈值、白名单、去重与统计 --\n");
    {
        // 1.1 阈值边界（1.49 / 1.5）
        SearchHit h_low_rerank;
        h_low_rerank.title = "Low Rerank";
        h_low_rerank.url = "https://zh.wikipedia.org/wiki/Low";
        h_low_rerank.rerankScore = 1.49;
        h_low_rerank.authority = 2;

        SearchHit h_exact_rerank;
        h_exact_rerank.title = "Exact Rerank";
        h_exact_rerank.url = "https://zh.wikipedia.org/wiki/Exact";
        h_exact_rerank.rerankScore = 1.50;
        h_exact_rerank.authority = 2;

        FilterOptions f_opts;
        f_opts.min_rerank_score = 1.5;
        f_opts.min_authority = 2;

        FilterResult r1 = filter_hits({ h_low_rerank }, f_opts);
        ck(r1.hits.empty() && r1.stats.dropped_rerank == 1, "rerank=1.49 严格拒绝 (dropped_rerank=1)");

        FilterResult r2 = filter_hits({ h_exact_rerank }, f_opts);
        ck(r2.hits.size() == 1 && r2.stats.total_passed == 1, "rerank=1.50 命中通过 (total_passed=1)");

        // 1.2 authority 边界（1 / 2）
        SearchHit h_low_auth;
        h_low_auth.title = "Low Auth";
        h_low_auth.url = "https://zh.wikipedia.org/wiki/LowAuth";
        h_low_auth.rerankScore = 2.0;
        h_low_auth.authority = 1;

        SearchHit h_exact_auth;
        h_exact_auth.title = "Exact Auth";
        h_exact_auth.url = "https://zh.wikipedia.org/wiki/ExactAuth";
        h_exact_auth.rerankScore = 2.0;
        h_exact_auth.authority = 2;

        FilterResult r3 = filter_hits({ h_low_auth }, f_opts);
        ck(r3.hits.empty() && r3.stats.dropped_authority == 1, "authority=1 严格拒绝 (dropped_authority=1)");

        FilterResult r4 = filter_hits({ h_exact_auth }, f_opts);
        ck(r4.hits.size() == 1 && r4.stats.total_passed == 1, "authority=2 命中通过 (total_passed=1)");

        // 1.3 白名单命中与拒绝（子域匹配规则）
        std::vector<std::string> valid_urls = {
            "https://zh.wikipedia.org/wiki/ZARD",
            "https://wikipedia.org/wiki/ZARD",
            "https://zh.moegirl.org.cn/ZARD",
            "https://musicbrainz.org/artist/123",
            "https://www.discogs.com/artist/456",
            "https://www.allmusic.com/artist/789",
            "https://last.fm/music/ZARD",
            "https://pitchfork.com/reviews/101",
            "https://www.gramophone.co.uk/review/102",
            "https://www.bach-cantatas.com/BWV1.htm"
        };
        std::vector<SearchHit> wl_hits;
        for (size_t i = 0; i < valid_urls.size(); ++i) {
            SearchHit h;
            h.title = "Hit" + std::to_string(i);
            h.url = valid_urls[i];
            h.rerankScore = 2.0;
            h.authority = 3;
            wl_hits.push_back(h);
        }
        FilterOptions wl_opts = f_opts;
        wl_opts.max_hits = 20; // 调大上限以检验全部通过
        FilterResult r_wl = filter_hits(wl_hits, wl_opts);
        ck(r_wl.hits.size() == valid_urls.size(), "契约全部白名单域名及其子域均正确放行",
           "passed=" + std::to_string(r_wl.hits.size()) + " want=" + std::to_string(valid_urls.size()));

        // 白名单拒绝样例（仿冒、非白名单域名、同名前缀欺骗）
        std::vector<std::string> rejected_urls = {
            "https://fake-wikipedia.org/wiki/ZARD",
            "https://wikipedia.org.evil.com/fake",
            "https://baidu.com/s?wd=ZARD",
            "https://moegirl.org/ZARD",
            "https://notlast.fm/music"
        };
        std::vector<SearchHit> rej_hits;
        for (const auto& u : rejected_urls) {
            SearchHit h; h.url = u; h.rerankScore = 3.0; h.authority = 4;
            rej_hits.push_back(h);
        }
        FilterResult r_rej = filter_hits(rej_hits, wl_opts);
        ck(r_rej.hits.empty() && r_rej.stats.dropped_whitelist == rejected_urls.size(),
           "仿冒/非白名单域名全量拒绝 (dropped_whitelist 计数准确)");

        // 1.4 去重与排序
        SearchHit d1; d1.url = "https://zh.wikipedia.org/wiki/ZARD"; d1.rerankScore = 1.6; d1.authority = 2;
        SearchHit d2; d2.url = "https://zh.wikipedia.org/wiki/ZARD"; d2.rerankScore = 2.5; d2.authority = 4;
        SearchHit d3; d3.url = "https://www.discogs.com/artist/ZARD"; d3.rerankScore = 1.9; d3.authority = 3;
        FilterResult r_dedup = filter_hits({ d1, d2, d3 }, f_opts);
        ck(r_dedup.hits.size() == 2 && r_dedup.stats.dropped_duplicate == 1, "重复 URL 正确去重 (dropped_duplicate=1)");
        ck(r_dedup.hits[0].rerankScore == 2.5 && r_dedup.hits[1].rerankScore == 1.9,
           "过滤结果按 (rerankScore, authority) 降序排列且优先保留高分项");

        // 1.5 上限条数截断（默认 8）
        std::vector<SearchHit> ten_hits;
        for (int i = 0; i < 10; ++i) {
            SearchHit h;
            h.url = "https://zh.wikipedia.org/wiki/Item" + std::to_string(i);
            h.rerankScore = 3.0 - i * 0.1;
            h.authority = 3;
            ten_hits.push_back(h);
        }
        FilterOptions f_limit = f_opts;
        f_limit.max_hits = 8;
        FilterResult r_limit = filter_hits(ten_hits, f_limit);
        ck(r_limit.hits.size() == 8 && r_limit.stats.dropped_limit == 2, "上限条数严格截断至 8 条 (dropped_limit=2)");

        // 1.6 综合过滤原因统计
        std::vector<SearchHit> mixed_hits = {
            h_low_rerank,    // dropped_rerank
            h_low_auth,      // dropped_authority
            rej_hits[0],     // dropped_whitelist
            d1,              // duplicate of d2
            d2               // passes
        };
        FilterResult r_mix = filter_hits(mixed_hits, f_opts);
        ck(r_mix.stats.total_in == 5 &&
           r_mix.stats.dropped_rerank == 1 &&
           r_mix.stats.dropped_authority == 1 &&
           r_mix.stats.dropped_whitelist == 1 &&
           r_mix.stats.dropped_duplicate == 1 &&
           r_mix.stats.total_passed == 1, "综合列表各项拒绝原因精确统计");
    }

    // ---------------- 2) parse_search_response ----------------
    std::printf("-- 2. parse_search_response 响应解析 --\n");
    {
        // 2.1 正常解析
        SearchResponse s1 = parse_search_response(kFixtureSearchNormal);
        ck(s1.ok && s1.hits.size() == 2, "正常搜索响应解析成功");
        ck(s1.hits[0].title == "ZARD - 维基百科" && s1.hits[0].rerankScore == 2.15 && s1.hits[0].authority == 4,
           "搜索命中项字段解析完整");

        // 2.2 缺非必填字段容错
        SearchResponse s2 = parse_search_response(kFixtureSearchMissingFields);
        ck(s2.ok && s2.hits.size() == 1, "缺失非必填字段容错成功");
        ck(s2.hits[0].authority == -1 && s2.hits[0].summary.empty(), "缺失字段赋默认值安全");

        // 2.3 空 results
        SearchResponse s3 = parse_search_response(kFixtureSearchEmptyResults);
        ck(s3.ok && s3.hits.empty(), "空 results 数组合法且返回 0 条");

        // 2.4 畸形 JSON
        SearchResponse s4 = parse_search_response(kFixtureSearchMalformed);
        ck(!s4.ok && !s4.error.empty(), "缺少 results 字段正确报错");

        SearchResponse s5 = parse_search_response("not a json at all {");
        ck(!s5.ok && !s5.error.empty(), "非法 JSON 语法正确报错");
    }

    // ---------------- 3) parse_synthesis_reply ----------------
    std::printf("-- 3. parse_synthesis_reply 与引用硬约束校验 --\n");
    {
        std::vector<std::string> allowed_urls = {
            "https://zh.wikipedia.org/wiki/ZARD",
            "https://www.discogs.com/artist/ZARD"
        };

        // 3.1 正常响应
        SummaryResult sr1 = parse_synthesis_reply(kFixtureSynthesisValid, allowed_urls);
        ck(sr1.ok && sr1.validation_ok, "正常综合响应解析与引用校验通过");
        ck(sr1.sections.size() == 1 && sr1.citations.size() == 2, "sections 与 citations 数量正确");
        ck(sr1.materials_used.size() == 2, "materials_used 提取正确");

        // 3.2 每段无引用 -> 拒绝
        const char* kNoCitationJson = R"json({
          "disclaimer": "AI",
          "sections": [
            {
              "heading": "生平",
              "paragraphs": [
                { "text": "无引用的段落", "citations": [] }
              ]
            }
          ],
          "citations": [
            { "id": "c1", "title": "t", "url": "https://zh.wikipedia.org/wiki/ZARD", "source": "w", "fetched_at": 100 }
          ]
        })json";
        SummaryResult sr2 = parse_synthesis_reply(kNoCitationJson, allowed_urls);
        ck(!sr2.ok, "段落 citations 为空严格拒绝 (硬门生效)");

        // 3.3 citation id 悬空 -> 拒绝
        const char* kDanglingCitationJson = R"json({
          "disclaimer": "AI",
          "sections": [
            {
              "heading": "生平",
              "paragraphs": [
                { "text": "引用了不存在的c99", "citations": ["c99"] }
              ]
            }
          ],
          "citations": [
            { "id": "c1", "title": "t", "url": "https://zh.wikipedia.org/wiki/ZARD", "source": "w", "fetched_at": 100 }
          ]
        })json";
        SummaryResult sr3 = parse_synthesis_reply(kDanglingCitationJson, allowed_urls);
        ck(!sr3.ok, "引用 ID 悬空严格拒绝");

        // 3.4 url 不在允许集合 -> 拒绝（防幻觉硬门）
        const char* kUnauthorizedUrlJson = R"json({
          "disclaimer": "AI",
          "sections": [
            {
              "heading": "生平",
              "paragraphs": [
                { "text": "引用了未通过过滤的假URL", "citations": ["c1"] }
              ]
            }
          ],
          "citations": [
            { "id": "c1", "title": "t", "url": "https://attacker.org/hallucinated", "source": "w", "fetched_at": 100 }
          ]
        })json";
        SummaryResult sr4 = parse_synthesis_reply(kUnauthorizedUrlJson, allowed_urls);
        ck(!sr4.ok, "引用 URL 不在允许来源集合严格拒绝 (防幻觉硬门生效)");

        // 3.5 citation url 为空 -> 拒绝
        const char* kEmptyUrlJson = R"json({
          "disclaimer": "AI",
          "sections": [
            {
              "heading": "生平",
              "paragraphs": [
                { "text": "URL为空的引用", "citations": ["c1"] }
              ]
            }
          ],
          "citations": [
            { "id": "c1", "title": "t", "url": "", "source": "w", "fetched_at": 100 }
          ]
        })json";
        SummaryResult sr5 = parse_synthesis_reply(kEmptyUrlJson, allowed_urls);
        ck(!sr5.ok, "引用 URL 为空严格拒绝");

        // 3.6 非 JSON -> 错误
        SummaryResult sr6 = parse_synthesis_reply("Raw markdown text without JSON", allowed_urls);
        ck(!sr6.ok && !sr6.error.empty(), "裸文本非JSON格式严格报错，不把裸文本塞入页面");

        // 3.7 只有部分段落合规 -> 拒绝（不许部分接受）
        const char* kPartialCompliantJson = R"json({
          "disclaimer": "AI",
          "sections": [
            {
              "heading": "第一节（合规）",
              "paragraphs": [
                { "text": "这节有引用", "citations": ["c1"] }
              ]
            },
            {
              "heading": "第二节（违规）",
              "paragraphs": [
                { "text": "这节偷懒没带引用", "citations": [] }
              ]
            }
          ],
          "citations": [
            { "id": "c1", "title": "t", "url": "https://zh.wikipedia.org/wiki/ZARD", "source": "w", "fetched_at": 100 }
          ]
        })json";
        SummaryResult sr7 = parse_synthesis_reply(kPartialCompliantJson, allowed_urls);
        ck(!sr7.ok, "部分段落违规时整份结果严格判失败 (不许部分接受)");
    }

    // ---------------- 4) build_synthesis_prompt ----------------
    std::printf("-- 4. build_synthesis_prompt 硬约束文本与编号 --\n");
    {
        KnowledgeMaterial m1;
        m1.source = "wikipedia-zh";
        m1.title = "ZARD";
        m1.url = "https://zh.wikipedia.org/wiki/ZARD";
        m1.text = "ZARD是以日本女歌手坂井泉水为核心的音乐团体。";
        m1.fetched_at = 1792690000;

        SearchHit h1;
        h1.title = "ZARD 评价";
        h1.url = "https://www.discogs.com/artist/ZARD";
        h1.snippet = "90年代J-POP代表性摇滚乐团。";
        h1.date = "2023";

        std::vector<LlmMessage> msgs = build_synthesis_prompt("ZARD", { m1 }, { h1 });
        ck(msgs.size() == 2, "提示词生成 system 与 user 消息对");
        const std::string& sys = msgs[0].content;
        const std::string& usr = msgs[1].content;

        ck(sys.find("只依据材料") != std::string::npos, "system 包含硬约束「只依据材料」");
        ck(sys.find("JSON 输出") != std::string::npos, "system 包含硬约束「JSON 输出」");
        ck(sys.find("每条引用") != std::string::npos, "system 包含硬约束「每条引用」");
        ck(sys.find("每段至少 1 条引用") != std::string::npos, "system 包含硬约束「每段至少 1 条引用」");
        ck(usr.find("[M1]") != std::string::npos && usr.find("[M2]") != std::string::npos,
           "user 消息中包含完整的材料编号 [M1] 和 [M2]");
        ck(usr.find(m1.url) != std::string::npos && usr.find(h1.url) != std::string::npos,
           "user 消息中保留原始 URL");
    }

    // ---------------- 5) 缓存：round-trip / TTL / 离线回退 / 损坏容错 ----------------
    std::printf("-- 5. 缓存序列化、TTL、离线回退与损坏容错 --\n");
    {
        const fsx::path tmp_base = fsx::temp_directory_path() / "rta_test_retrieval_meta";
        std::error_code ec;
        fsx::remove_all(tmp_base, ec);
        fsx::create_directories(tmp_base, ec);
        const std::string base_dir = meta_string_of(tmp_base);

        // 5.1 round-trip
        SummaryResult orig;
        orig.ok = true;
        orig.model = "deepseek-flash";
        orig.generated_at = 1792690000;
        orig.expires_at = orig.generated_at + 7 * 86400;
        orig.disclaimer = "AI生成免责声明";
        SummarySection sec;
        sec.heading = "主要成就";
        SummaryParagraph p;
        p.text = "单曲销量突破千万。";
        p.citations = { "c1" };
        sec.paragraphs.push_back(p);
        orig.sections.push_back(sec);
        SummaryCitation c;
        c.id = "c1"; c.title = "Title"; c.url = "https://zh.wikipedia.org/wiki/ZARD"; c.source = "w"; c.fetched_at = 1792690000;
        orig.citations.push_back(c);
        orig.materials_used = { c.url };

        const std::string json_str = summary_to_json(orig);
        SummaryResult loaded;
        ck(summary_from_json(json_str, loaded), "summary_to_json -> summary_from_json round-trip 成功");
        ck(loaded.sections.size() == 1 && loaded.sections[0].paragraphs[0].citations[0] == "c1",
           "round-trip 后 sections 与 citations 结构无损");
        ck(loaded.citations[0].url == orig.citations[0].url, "round-trip 后 URL 保持原样");

        // 5.2 TTL 判定
        const long long now_sec = (long long)std::time(nullptr);
        SummaryResult unexpired_item = orig;
        unexpired_item.generated_at = now_sec - 100;
        unexpired_item.expires_at = unexpired_item.generated_at + 7 * 86400;
        ck(!summary_is_expired(unexpired_item, now_sec), "7天内缓存判定未过期");

        SummaryResult expired_item = orig;
        expired_item.generated_at = now_sec - 8 * 86400;
        expired_item.expires_at = expired_item.generated_at + 7 * 86400;
        ck(summary_is_expired(expired_item, now_sec), "超过7天缓存判定已过期");

        // 5.3 离线回退
        const std::string eid = entity_id(EntityKind::Person, "ZARD");
        ck(summary_save_cache(base_dir, eid, "zh", unexpired_item), "原子落盘保存 summary 缓存");

        RetrievalOptions off_opts;
        off_opts.metabase = base_dir;
        off_opts.offline = true;
        SummaryResult off_res = retrieval_fetch(EntityKind::Person, "ZARD", "zh", off_opts);
        ck(off_res.ok && off_res.from_cache && off_res.offline, "离线模式成功回退至本地缓存 (offline=true, from_cache=true)");

        // 5.4 损坏容错
        const fsx::path p_cache = summary_cache_path(base_dir, eid, "zh");
        {
            std::ofstream f(p_cache, std::ios::binary | std::ios::trunc);
            f << "{ corrupt garbage json ...";
        }
        SummaryResult corrupt_load;
        std::string corrupt_err;
        ck(!summary_load_cache(base_dir, eid, "zh", corrupt_load, &corrupt_err), "损坏的缓存文件安全返回 false 不崩溃");
        ck(!corrupt_err.empty(), "损坏时给出明确错误提示");

        fsx::remove_all(tmp_base, ec);
    }

    // ---------------- 6) llm_chat_stream：SSE / 非流式回退 / 半截截断（离线 mock） ----------------
    std::printf("-- 6. llm_chat_stream：SSE 解析、非流式回退、半截必须报错 --\n");
    const fsx::path stream_tmp = fsx::temp_directory_path() / "rta_test_llm_stream";
    std::error_code ecs;
    fsx::remove_all(stream_tmp, ecs);
    fsx::create_directories(stream_tmp, ecs);
    const std::string stream_usage = meta_string_of(stream_tmp / "llm-usage.json");
    {
        const std::string full = "流式摘要：ZARD 是以坂井泉水为核心的摇滚乐队。第一段。第二段。结束。";
        const std::vector<std::string> pieces = split_bytes(full, 5);
        std::string sse;
        for (const auto& p : pieces) sse += sse_delta(p);
        sse += sse_stop(30);
        sse += "data: [DONE]\n\n";

        LlmConfig cfg;
        cfg.apiKey = "test-key";
        cfg.model = "deepseek-flash";
        cfg.usagePath = stream_usage;
        std::string sentBody;
        cfg.transport = [&](const std::string&, const std::string& b, const std::vector<std::string>&) {
            sentBody = b;
            return resp_ok(sse);
        };

        std::vector<std::string> deltas;
        std::string joined;
        const LlmStreamReply r = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } },
            [&](const std::string& d) { deltas.push_back(d); joined += d; });

        ck(r.ok, "6.1 正常 SSE：ok", r.error);
        ck(deltas.size() >= 5, "6.1 增量回调 >= 5 次（实测 " + std::to_string(deltas.size()) + " 次）");
        ck(joined == full, "6.1 回调累计文本 == 完整文本");
        ck(r.content == full, "6.1 返回正文 == 完整文本");
        ck(r.streamed && r.sawDone && !r.truncated, "6.1 判定为流式、收到 [DONE]、非截断");
        ck(r.totalTokens == 30, "6.1 usage 透传 total_tokens=30（实测 " + std::to_string(r.totalTokens) + "）");
        ck(sentBody.find("\"stream\":true") != std::string::npos, "6.1 请求体带 \"stream\":true");

        const LlmStreamReply rn = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } }, LlmDeltaFn());
        ck(rn.ok && rn.deltaCount >= 5, "6.1 不传回调也能拿到完整正文与 deltaCount");

        // 6.2 [DONE] 出现在中途：后面的字节必须被忽略（不能拼进正文、也不许报错）
        std::string sse_mid = sse_delta("A") + sse_delta("B") + "data: [DONE]\n\n" +
                              sse_delta("不该出现") + "data: 坏数据\n";
        cfg.transport = [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            return resp_ok(sse_mid);
        };
        const LlmStreamReply rm = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } });
        ck(rm.ok && rm.content == "AB" && rm.deltaCount == 2,
           "6.2 [DONE] 之后的字节全部忽略（正文=\"" + rm.content + "\"）", rm.error);

        // 6.3 半截断开：最后一个 data: 行是半个 JSON，且没有 [DONE]
        const std::string sse_cut = sse_delta("前半段") +
            "data: {\"choices\":[{\"delta\":{\"content\":\"被截断";
        cfg.transport = [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            return resp_ok(sse_cut);
        };
        const LlmStreamReply rc = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } });
        ck(!rc.ok, "6.3 chunked 但半截断开：必须返回错误，不能把半截当成功", rc.error);
        ck(rc.truncated && rc.errorKind == "truncated",
           "6.3 错误分类 = truncated（实测 " + rc.errorKind + "）");
        ck(rc.content == "前半段", "6.3 半截正文只作诊断保留，不冒充成功");

        // 6.4 事件都完整，但流没给 [DONE]/finish_reason → 仍判截断
        const std::string sse_nodone = sse_delta("A") + sse_delta("B");
        cfg.transport = [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            return resp_ok(sse_nodone);
        };
        const LlmStreamReply rnd = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } });
        ck(!rnd.ok && rnd.errorKind == "truncated",
           "6.4 没有 [DONE]/finish_reason 的流判截断（不猜它已经讲完）");

        // 6.5 真·分块投递：每 3 字节喂一次（切点会落在 JSON/UTF-8 中间）
        cfg.streamTransport = [&](const std::string&, const std::string&, const std::vector<std::string>&,
                                  const LlmChunkSink& sink) {
            for (size_t i = 0; i < sse.size(); i += 3) {
                const size_t n = (i + 3 <= sse.size()) ? 3 : (sse.size() - i);
                sink(sse.data() + i, n);
            }
            return resp_ok("");   // 全部字节都经 sink 投递，body 为空
        };
        std::string joined2;
        int calls2 = 0;
        const LlmStreamReply rs = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } },
            [&](const std::string& d) { ++calls2; joined2 += d; });
        ck(rs.ok && rs.content == full, "6.5 分块到达（3 字节/次）仍得到完整正文", rs.error);
        ck(calls2 >= 5 && joined2 == full, "6.5 跨分块的增量回调 >= 5 次且拼接无误");
        cfg.streamTransport = nullptr;

        // 6.6 服务端忽略 stream，只回一个完整 JSON
        const std::string plain = plain_chat_json("服务端忽略了 stream，一次性返回全文", 77);
        cfg.transport = [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            return resp_ok(plain, "application/json");
        };
        int plain_calls = 0;
        std::string plain_got;
        const LlmStreamReply rp = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } },
            [&](const std::string& d) { ++plain_calls; plain_got = d; });
        ck(rp.ok && rp.content == "服务端忽略了 stream，一次性返回全文",
           "6.6 非流式回退：正常返回全文", rp.error);
        ck(!rp.streamed && rp.deltaCount == 1 && plain_calls == 1,
           "6.6 非流式回退：全文作为单个 delta 投递一次（deltaCount=" + std::to_string(rp.deltaCount) + "）");
        ck(rp.totalTokens == 77, "6.6 非流式回退：usage 透传");

        // 6.7 HTTP 错误 / 传输层失败要能区分
        cfg.transport = [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            return resp_status(500, "{\"error\":{\"message\":\"insufficient balance\"}}");
        };
        const LlmStreamReply rh = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } });
        ck(!rh.ok && rh.errorKind == "http" && rh.error.find("HTTP 500") != std::string::npos,
           "6.7 HTTP 500 判为 http 错误", rh.error);
        ck(rh.error.find("insufficient balance") != std::string::npos, "6.7 服务端 message 透传到错误里");

        cfg.transport = [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            rta::net::HttpResponse rr;
            rr.status = 0;
            rr.error = "timeout after 60000ms";
            return rr;
        };
        const LlmStreamReply rt = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } });
        ck(!rt.ok && rt.errorKind == "timeout" && rt.error.find("timeout") != std::string::npos,
           "6.7 超时判为 timeout 错误（与普通网络失败区分，原文保留）", rt.error);

        cfg.transport = [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            rta::net::HttpResponse rr;
            rr.status = 0;
            rr.error = "WinHTTP connect failed: 10061";
            return rr;
        };
        const LlmStreamReply rc2 = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } });
        ck(!rc2.ok && rc2.errorKind == "transport",
           "6.7 非超时的连接失败判为 transport（实测 " + rc2.errorKind + "）", rc2.error);
        // 6.8 请求体：stream 开关；stream_options 只在 deepseek 模型上出现
        const std::vector<LlmMessage> one{ LlmMessage{ "user", "hi" } };
        const std::string b_stream = llm_build_chat_body(std::string("deepseek-flash"), one, 100, 0.2f, true, true);
        const std::string b_plain  = llm_build_chat_body(std::string("deepseek-flash"), one, 100, 0.2f, true, false);
        const std::string b_other  = llm_build_chat_body(std::string("gpt-4o"), one, 100, 0.2f, true, true);
        ck(b_stream.find("\"stream\":true") != std::string::npos, "6.8 stream=true 追加 \"stream\":true");
        ck(b_plain.find("\"stream\"") == std::string::npos,
           "6.8 非流式请求体不含 stream 字段（现有 llm_chat 调用零变化）");
        ck(b_stream.find("\"stream_options\"") != std::string::npos,
           "6.8 deepseek 流式请求带 stream_options.include_usage（拿 usage 用）");
        ck(b_other.find("\"stream_options\"") == std::string::npos,
           "6.8 非 deepseek 模型不追加 stream_options（避免兼容端点 400）");
    }

    // ---------------- 7) 重试必须重新开流（不能接在半截后面） ----------------
    std::printf("-- 7. 重试从头开流：第二次的正文不含第一次的半截 --\n");
    {
        const std::string good = "重试后的完整正文：只有这一份，绝无上一轮的半截前缀。";
        int call_no = 0;
        LlmConfig cfg;
        cfg.apiKey = "test-key";
        cfg.model = "deepseek-flash";
        cfg.usagePath = stream_usage;
        cfg.transport = [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            ++call_no;
            if (call_no == 1) {
                return resp_ok(sse_delta("上一轮的半截") +
                               "data: {\"choices\":[{\"delta\":{\"content\":\"断了");
            }
            std::string s;
            for (const auto& p : split_bytes(good, 4)) s += sse_delta(p);
            s += sse_stop(40);
            s += "data: [DONE]\n\n";
            return resp_ok(s);
        };
        const LlmStreamReply r1 = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } });
        ck(!r1.ok && r1.truncated, "7.1 第一次流半截 → 失败（truncated）");
        const LlmStreamReply r2 = llm_chat_stream(cfg, { LlmMessage{ "user", "hi" } });
        ck(r2.ok, "7.2 重试成功", r2.error);
        ck(r2.content == good, "7.3 重试正文 == 第二次的完整文本（没有拼上第一次的半截）");
    }

    // ---------------- 8) retrieval_fetch 进度回调：阶段 / 字符数 / 部分文本 ----------------
    std::printf("-- 8. 摘要生成进度回调（阶段 + 已生成字符数 + 部分文本）--\n");
    const fsx::path prog_tmp = fsx::temp_directory_path() / "rta_test_stream_progress";
    std::error_code ecp;
    fsx::remove_all(prog_tmp, ecp);
    fsx::create_directories(prog_tmp, ecp);
    const std::string progBase = meta_string_of(prog_tmp);
    const std::string kSynthesis = R"json({
  "disclaimer": "仅依据下方来源材料综合整理；可能不完整，请以来源为准",
  "sections": [{"heading": "简介", "paragraphs": [{"text": "（mock 流式）StreamProbe 是一支测试用乐队。", "citations": ["c1"]}]}],
  "citations": [{"id": "c1", "title": "StreamProbe", "url": "https://zh.wikipedia.org/wiki/StreamProbe", "source": "wikipedia-zh", "fetched_at": 1792690000}],
  "fields": {"风格": "（mock）测试"}
})json";
    {
        const std::string name = "StreamProbe";
        const std::string eid = entity_id(EntityKind::Person, name);
        {
            KnowledgeMaterial m;
            m.source = "wikipedia-zh";
            m.kind = "biography";
            m.subject = name;
            m.title = name;
            m.text = "（预置材料）StreamProbe 是一支测试用乐队。";
            m.url = "https://zh.wikipedia.org/wiki/StreamProbe";
            m.lang = "zh";
            m.fetched_at = (long long)std::time(nullptr);
            m.expires_at = m.fetched_at + 30 * 86400;
            m.authority = 4;
            ck(knowledge_save_cache(progBase, eid, name, { m }), "8.0 预置 L2 材料（离线，不联网）");
        }

        std::string sse;
        for (const auto& p : split_bytes(kSynthesis, 5)) sse += sse_delta(p);
        sse += sse_stop(120);
        sse += "data: [DONE]\n\n";

        int stream_calls = 0, json_calls = 0;
        LlmConfig cfg;
        cfg.apiKey = "test-key";
        cfg.model = "deepseek-flash";
        cfg.usagePath = meta_string_of(prog_tmp / "llm-usage.json");
        cfg.transport = [&](const std::string&, const std::string& b, const std::vector<std::string>&) {
            if (b.find("\"stream\":true") != std::string::npos) {
                ++stream_calls;
                return resp_ok(sse);
            }
            ++json_calls;
            return resp_ok(plain_chat_json(kSynthesis, 120), "application/json");
        };
        RetrievalOptions ro;
        ro.metabase = progBase;
        ro.enable_search = false;   // 只测 LLM 段（检索段另有覆盖）
        ro.llm_cfg = cfg;

        std::mutex pv_mtx;
        std::vector<SummaryProgress> evs;
        auto collect = [&](const SummaryProgress& p) {
            std::lock_guard<std::mutex> lk(pv_mtx);
            evs.push_back(p);
        };
        const SummaryResult a = retrieval_fetch(EntityKind::Person, name, "zh", ro, collect);
        ck(a.ok, "8.1 带 ProgressFn 生成成功", a.error);
        ck(a.sections.size() == 1 && a.citations.size() == 1, "8.1 结果结构完整");

        int n_retrieval = 0, n_delta = 0;
        size_t max_chars = 0;
        std::string last_partial;
        for (const auto& p : evs) {
            if (p.phase == SummaryPhase::Retrieving) ++n_retrieval;
            if (p.chars > 0) ++n_delta;
            if (p.chars >= max_chars) { max_chars = p.chars; last_partial = p.partial; }
        }
        ck(n_retrieval >= 1, "8.2 收到检索阶段事件（retrieval）");
        ck(n_delta >= 5, "8.3 生成阶段带正文的事件 >= 5 次（实测 " + std::to_string(n_delta) + " 次）");
        ck(max_chars == kSynthesis.size(),
           "8.4 最终已生成字节数 == 服务端流出全文长度（" + std::to_string(max_chars) + "/" +
               std::to_string(kSynthesis.size()) + "）");
        ck(last_partial == kSynthesis, "8.5 部分文本拼接 == 完整流式正文");
        ck(!evs.empty() && evs.back().phase == SummaryPhase::Done, "8.6 最后一个是 Done");
        ck(!evs.empty() && evs.back().attempt == 0, "8.6 首次生成 attempt=0（没有多余重试）");
        ck(stream_calls == 1 && json_calls == 0, "8.7 传 ProgressFn → 走流式（stream=1 json=0）");

        // 不传 ProgressFn：逐字段与流式结果相同（向后兼容）
        fsx::remove(summary_cache_path(progBase, eid, "zh"), ecp);
        const SummaryResult b2 = retrieval_fetch(EntityKind::Person, name, "zh", ro);
        ck(b2.ok, "8.8 不传 ProgressFn 生成成功", b2.error);
        ck(json_calls == 1 && stream_calls == 1, "8.9 不传 ProgressFn → 走原来的非流式 llm_chat");
        ck(summary_core_equal(a, b2), "8.10 传/不传 ProgressFn：结果逐字段相同");
    }

    // ---------------- 8b) 生成失败重试：进度从 chars=0 重新开始 ----------------
    std::printf("-- 8b. 生成失败重试：进度清零重开，不接半截 --\n");
    {
        const std::string name = "StreamRetry";
        const std::string eid = entity_id(EntityKind::Person, name);
        {
            KnowledgeMaterial m;
            m.source = "wikipedia-zh";
            m.kind = "biography";
            m.subject = name;
            m.title = name;
            m.text = "（预置材料）StreamRetry 是一支测试用乐队。";
            m.url = "https://zh.wikipedia.org/wiki/StreamRetry";
            m.lang = "zh";
            m.fetched_at = (long long)std::time(nullptr);
            m.expires_at = m.fetched_at + 30 * 86400;
            m.authority = 4;
            ck(knowledge_save_cache(progBase, eid, name, { m }), "8b.0 预置 L2 材料");
        }

        const std::string synthesis2 = R"json({
  "disclaimer": "仅依据下方来源材料综合整理；可能不完整，请以来源为准",
  "sections": [{"heading": "简介", "paragraphs": [{"text": "（mock 重试）StreamRetry 是一支测试用乐队。", "citations": ["c1"]}]}],
  "citations": [{"id": "c1", "title": "StreamRetry", "url": "https://zh.wikipedia.org/wiki/StreamRetry", "source": "wikipedia-zh", "fetched_at": 1792690000}],
  "fields": {"风格": "（mock）测试"}
})json";
        int attempts = 0;
        LlmConfig cfg;
        cfg.apiKey = "test-key";
        cfg.model = "deepseek-flash";
        cfg.usagePath = meta_string_of(prog_tmp / "llm-usage.json");
        cfg.transport = [&](const std::string&, const std::string& b, const std::vector<std::string>&) {
            if (b.find("\"stream\":true") != std::string::npos) {
                ++attempts;
                if (attempts == 1) {
                    // 第一次：两个完整 chunk 后断开（半截 JSON、没有 [DONE]）
                    return resp_ok(sse_delta("坏") + sse_delta("掉") +
                                   "data: {\"choices\":[{\"delta\":{\"content\":\"断");
                }
                std::string s;
                for (const auto& p : split_bytes(synthesis2, 4)) s += sse_delta(p);
                s += sse_stop(120);
                s += "data: [DONE]\n\n";
                return resp_ok(s);
            }
            return resp_ok(plain_chat_json(synthesis2, 120), "application/json");
        };
        RetrievalOptions ro;
        ro.metabase = progBase;
        ro.enable_search = false;
        ro.llm_cfg = cfg;

        std::vector<SummaryProgress> evs2;
        auto collect2 = [&](const SummaryProgress& p) { evs2.push_back(p); };
        const SummaryResult r = retrieval_fetch(EntityKind::Person, name, "zh", ro, collect2);
        ck(r.ok, "8b.1 第一次流断开后自动重试成功", r.error);
        ck(attempts == 2, "8b.2 恰好重试一次（2 次流式请求，实测 " + std::to_string(attempts) + "）");

        size_t chars_at_attempt1 = (size_t)-1;
        size_t max_chars2 = 0;
        std::string last_partial2;
        for (const auto& p : evs2) {
            if (p.attempt == 1 && chars_at_attempt1 == (size_t)-1) chars_at_attempt1 = p.chars;
            if (p.phase == SummaryPhase::Generating && p.chars > max_chars2) {
                max_chars2 = p.chars;
                last_partial2 = p.partial;
            }
        }
        ck(chars_at_attempt1 == 0, "8b.3 attempt=1 的首个事件 chars=0（进度清零重开）");
        ck(max_chars2 == synthesis2.size(),
           "8b.4 最终已生成字节数 == 全文长度（" + std::to_string(max_chars2) + "/" +
               std::to_string(synthesis2.size()) + "）");
        ck(last_partial2 == synthesis2, "8b.5 最终部分文本 == 第二次的完整正文（没有拼接两轮）");
    }

    // ---------------- 8c) 回调在传输线程上：跨线程进度只能靠锁 ----------------
    std::printf("-- 8c. 流式回调发生在传输线程（跨线程进度）--\n");
    {
        const std::string name = "StreamThread";
        const std::string eid = entity_id(EntityKind::Person, name);
        {
            KnowledgeMaterial m;
            m.source = "wikipedia-zh";
            m.kind = "biography";
            m.subject = name;
            m.title = name;
            m.text = "（预置材料）StreamThread 是一支测试用乐队。";
            m.url = "https://zh.wikipedia.org/wiki/StreamThread";
            m.lang = "zh";
            m.fetched_at = (long long)std::time(nullptr);
            m.expires_at = m.fetched_at + 30 * 86400;
            m.authority = 4;
            ck(knowledge_save_cache(progBase, eid, name, { m }), "8c.0 预置 L2 材料");
        }
        const std::string synthesis3 = R"json({
  "disclaimer": "仅依据下方来源材料综合整理；可能不完整，请以来源为准",
  "sections": [{"heading": "简介", "paragraphs": [{"text": "（mock 跨线程）StreamThread 是一支测试用乐队。", "citations": ["c1"]}]}],
  "citations": [{"id": "c1", "title": "StreamThread", "url": "https://zh.wikipedia.org/wiki/StreamThread", "source": "wikipedia-zh", "fetched_at": 1792690000}]
})json";
        std::string sse;
        for (const auto& p : split_bytes(synthesis3, 5)) sse += sse_delta(p);
        sse += sse_stop(90);
        sse += "data: [DONE]\n\n";

        LlmConfig cfg;
        cfg.apiKey = "test-key";
        cfg.model = "deepseek-flash";
        cfg.usagePath = meta_string_of(prog_tmp / "llm-usage.json");
        cfg.streamTransport = [&](const std::string&, const std::string&, const std::vector<std::string>&,
                                  const LlmChunkSink& sink) {
            std::thread th([&]() {
                for (size_t i = 0; i < sse.size(); i += 11) {
                    const size_t n = (i + 11 <= sse.size()) ? 11 : (sse.size() - i);
                    sink(sse.data() + i, n);
                }
            });
            th.join();
            return resp_ok("");   // 全部字节经 sink 投递
        };
        RetrievalOptions ro;
        ro.metabase = progBase;
        ro.enable_search = false;
        ro.llm_cfg = cfg;

        std::mutex m2;
        std::vector<SummaryProgress> evs3;
        std::thread::id cb_thread;
        const SummaryResult r = retrieval_fetch(EntityKind::Person, name, "zh", ro,
            [&](const SummaryProgress& p) {
                std::lock_guard<std::mutex> lk(m2);
                evs3.push_back(p);
                // 只看"增量正文"事件（Done 事件的快照里 chars 也会 >0，但那是调用线程补发的）
                if (p.phase == SummaryPhase::Generating && p.chars > 0) cb_thread = std::this_thread::get_id();
            });
        ck(r.ok, "8c.1 跨线程分块投递仍生成成功", r.error);
        size_t mx = 0;
        std::string lp;
        for (const auto& p : evs3) {
            if (p.chars > mx) { mx = p.chars; lp = p.partial; }
        }
        ck(mx == synthesis3.size() && lp == synthesis3,
           "8c.2 跨线程增量拼接完整（" + std::to_string(mx) + "/" + std::to_string(synthesis3.size()) + "）");
        ck(cb_thread != std::thread::id() && cb_thread != std::this_thread::get_id(),
           "8c.3 正文回调确实发生在传输线程（不是调用线程）");
    }

    // ---------------- 9) 网络用例 opt-in (RTA_NET_TESTS=1) ----------------
    std::printf("-- 9. 真实网络用例 opt-in --\n");
    const char* net_env = std::getenv("RTA_NET_TESTS");
    if (!net_env || std::string(net_env) != "1") {
        std::printf("  [SKIP] RTA_NET_TESTS!=1（网络用例默认跳过，零网络请求保证离线绿色）\n");
        return g_fail ? 1 : 0;
    }

    std::printf("  [NET] RTA_NET_TESTS=1，开始对 ZARD 进行真实 retrieval_fetch 测试...\n");
    RetrievalOptions net_opts;
    net_opts.refresh = true;
    const auto t0 = std::chrono::steady_clock::now();
    const SummaryResult net_res = retrieval_fetch(EntityKind::Person, "ZARD", "zh", net_opts);
    const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    ck(net_res.ok, "真实网络下 ZARD retrieval_fetch 成功 (耗时: " + std::to_string(elapsed_s) + "s)");
    ck(net_res.validation_ok, "真实综合响应引用校验通过");
    ck(net_res.sections.size() > 0, "包含有效 sections");
    ck(net_res.citations.size() > 0, "包含有效 citations");
    for (const auto& sec : net_res.sections) {
        for (const auto& p : sec.paragraphs) {
            ck(p.citations.size() >= 1, "网络结果每个段落均有引用支撑");
        }
    }
    for (const auto& c : net_res.citations) {
        ck(!c.url.empty(), "网络结果每条引用均包含有效 url (" + c.url + ")");
    }

    return g_fail ? 1 : 0;
}
