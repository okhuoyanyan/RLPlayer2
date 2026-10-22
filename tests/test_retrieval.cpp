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
#include <string>
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

    // ---------------- 6) 网络用例 opt-in (RTA_NET_TESTS=1) ----------------
    std::printf("-- 6. 真实网络用例 opt-in --\n");
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
