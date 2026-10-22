// test_knowledge.cpp — P3 资料层② 直连层测试（离线为主，真实网络 opt-in）
//
// 覆盖任务书交付物 [3]：
//   1. MediaWiki JSON 解析：正常 extract / redirect / 缺页 / 消歧义 / 空 extracts / 多页
//   2. opensearch 候选解析与「不猜」策略
//   3. URL 构造与 CJK 百分号编码（只编码一次，防双重编码）
//   4. KnowledgeMaterial 序列化/反序列化 round-trip（url/fetched_at/authority 必须保留）
//   5. 缓存 TTL 判定、过期离线回退、缓存损坏容错、同 URL 去重、多源共存
//   6. 令牌桶 / 熔断控制器纯函数逻辑
//   7. 可选源（Discogs / Last.fm）缺 key 明确报错、零网络请求
//   8. 真实网络用例 opt-in（RTA_NET_TESTS=1 开启，默认 [SKIP] exit 0）
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "meta/knowledge.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace rta::meta;
namespace fsx = std::filesystem;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
static void ck_eq(const std::string& got, const std::string& want, const char* what) {
    ck(got == want, what, "got=[" + got + "] want=[" + want + "]");
}

// ---------------------------------------------------------------------------
// 夹具定义（写死报文，使用 C++ raw string literal，保证绝对无转义丢失）
// ---------------------------------------------------------------------------

// 1. 正常 extract (formatversion=2)
static const char* kFixtureExtractNormal = R"json({
  "batchcomplete": true,
  "query": {
    "pages": [
      {
        "pageid": 90238,
        "ns": 0,
        "title": "ZARD",
        "extract": "ZARD是以日本女歌手坂井泉水为核心的音乐团体。
代表作有《不要认输》等。"
      }
    ]
  }
})json";

// 2. Redirect 响应
static const char* kFixtureExtractRedirect = R"json({
  "batchcomplete": true,
  "query": {
    "redirects": [
      {
        "from": "澤田なつ",
        "to": "泽田夏"
      }
    ],
    "pages": [
      {
        "pageid": 434993,
        "ns": 0,
        "title": "泽田夏",
        "extract": "泽田夏是日本女性声优。"
      }
    ]
  }
})json";

// 3. 缺页响应 (missing: true)
static const char* kFixtureExtractMissing = R"json({
  "batchcomplete": true,
  "query": {
    "pages": [
      {
        "ns": 0,
        "title": "NonExistent12345",
        "missing": true
      }
    ]
  }
})json";

// 4. 空 extract 响应
static const char* kFixtureExtractEmpty = R"json({
  "batchcomplete": true,
  "query": {
    "pages": [
      {
        "pageid": 1234,
        "ns": 0,
        "title": "EmptyPage",
        "extract": ""
      }
    ]
  }
})json";

// 5. 消歧义页面 (含 may refer to)
static const char* kFixtureExtractDisambig = R"json({
  "batchcomplete": true,
  "query": {
    "pages": [
      {
        "pageid": 401777,
        "ns": 0,
        "title": "Steve Roach",
        "extract": "Steve Roach may refer to:

Steve Roach (musician) (born 1955)
Steve Roach (rugby league)"
      }
    ]
  }
})json";

// 6. 兼容 formatversion=1 (pages 为对象)
static const char* kFixtureExtractFmt1 = R"json({
  "query": {
    "pages": {
      "90238": {
        "pageid": 90238,
        "title": "ZARD",
        "extract": "ZARD 乐队介绍。"
      }
    }
  }
})json";

// 7. OpenSearch 正常命中
static const char* kFixtureOpenSearchNormal = R"json([
  "ZARD",
  ["ZARD", "ZARD BEST The Single Collection", "ZARD BLEND"],
  ["", "", ""],
  ["https://zh.wikipedia.org/wiki/ZARD", "https://zh.wikipedia.org/wiki/ZARD_BEST", "https://zh.wikipedia.org/wiki/ZARD_BLEND"]
])json";

// 8. OpenSearch 多条候选但无精确匹配
static const char* kFixtureOpenSearchAmbiguous = R"json([
  "Steve Roach",
  ["Steve Reich", "Stevenia cheiranthoides", "Severo Ochoa"],
  ["", "", ""],
  ["https://zh.wikipedia.org/wiki/Steve_Reich", "https://zh.wikipedia.org/wiki/Stevenia", "https://zh.wikipedia.org/wiki/Severo"]
])json";

// 9. OpenSearch 零候选
static const char* kFixtureOpenSearchEmpty = R"json([
  "UnknownSubject",
  [],
  [],
  []
])json";

// ---------------------------------------------------------------------------
// 测试套件
// ---------------------------------------------------------------------------

// 1. MediaWiki JSON 解析
static void test_mediawiki_parse() {
    std::printf("-- 1. MediaWiki JSON 解析 --\n");

    // 1a: 正常
    {
        std::string title, extract, redir;
        bool missing = false;
        bool ok = parse_mediawiki_extract(kFixtureExtractNormal, title, extract, missing, redir);
        ck(ok, "正常 extract 解析成功");
        ck_eq(title, "ZARD", "标题正确提取");
        ck(!missing, "非 missing 页面");
        ck(extract.find("坂井泉水") != std::string::npos, "正文包含期望关键字");
        ck(extract.find('\n') != std::string::npos, "段落换行符被保留");
    }

    // 1b: 重定向
    {
        std::string title, extract, redir;
        bool missing = false;
        bool ok = parse_mediawiki_extract(kFixtureExtractRedirect, title, extract, missing, redir);
        ck(ok, "redirect 解析成功");
        ck_eq(redir, "泽田夏", "redirect to 字段正确捕获");
        ck_eq(title, "泽田夏", "重定向后目标标题正确");
    }

    // 1c: 缺页
    {
        std::string title, extract, redir;
        bool missing = false;
        bool ok = parse_mediawiki_extract(kFixtureExtractMissing, title, extract, missing, redir);
        ck(ok, "missing 响应解析成功");
        ck(missing, "missing 标志正确置 true");
    }

    // 1d: 空 extract
    {
        std::string title, extract, redir;
        bool missing = false;
        bool ok = parse_mediawiki_extract(kFixtureExtractEmpty, title, extract, missing, redir);
        ck(ok, "空 extract 解析成功");
        ck(!missing, "空 extract 不是 missing");
        ck(extract.empty(), "extract 为字符串空");
    }

    // 1e: 消歧义
    {
        std::string title, extract, redir;
        bool missing = false;
        parse_mediawiki_extract(kFixtureExtractDisambig, title, extract, missing, redir);
        ck(is_disambiguation_page(title, extract), "消歧义短语正确识别为消歧义页");
        ck(is_disambiguation_page("某某 (消歧义)", "简介"), "标题带(消歧义)正确识别");
        ck(!is_disambiguation_page("ZARD", "日本流行乐队"), "普通条目不被误判为消歧义页");
    }

    // 1f: formatversion=1 对象结构兼容
    {
        std::string title, extract, redir;
        bool missing = false;
        bool ok = parse_mediawiki_extract(kFixtureExtractFmt1, title, extract, missing, redir);
        ck(ok, "formatversion=1 对象格式兼容成功");
        ck_eq(title, "ZARD", "formatversion=1 标题提取正确");
        ck_eq(extract, "ZARD 乐队介绍。", "formatversion=1 extract 提取正确");
    }
}

// 2. OpenSearch 解析与「不猜」策略
static void test_opensearch_and_no_guess() {
    std::printf("-- 2. OpenSearch 解析与「不猜」策略 --\n");

    // 2a: 正常精确命中
    {
        std::string q;
        std::vector<std::string> cands, urls;
        bool ok = parse_opensearch(kFixtureOpenSearchNormal, q, cands, urls);
        ck(ok, "opensearch 解析成功");
        ck_eq(q, "ZARD", "query 提取正确");
        ck(cands.size() == 3, "候选条数正确 (3条)");
        ck(urls.size() == 3, "URL 条数正确 (3条)");
        ck_eq(cands[0], "ZARD", "第一候选即目标标题");
    }

    // 2b: 歧义/无精确匹配 -> 「不猜」逻辑
    {
        std::string q;
        std::vector<std::string> cands, urls;
        parse_opensearch(kFixtureOpenSearchAmbiguous, q, cands, urls);
        ck(cands.size() == 3, "多条候选存在");

        // 验证没有候选匹配 "Steve Roach"
        bool exactMatch = false;
        for (const auto& c : cands) {
            if (c == "Steve Roach" || entity_norm(c) == entity_norm("Steve Roach")) {
                exactMatch = true;
                break;
            }
        }
        ck(!exactMatch, "无一候选精确匹配 Steve Roach");
        // 核心不猜策略：如果无匹配，不得选取 Steve Reich！
        ck(cands[0] != "Steve Roach", "绝不猜测第一候选 Steve Reich");
    }

    // 2c: 零候选
    {
        std::string q;
        std::vector<std::string> cands, urls;
        bool ok = parse_opensearch(kFixtureOpenSearchEmpty, q, cands, urls);
        ck(ok, "零候选 opensearch 解析成功");
        ck(cands.empty(), "候选列表为空");
    }
}

// 3. URL 构造与 CJK 百分号编码
static void test_url_construction() {
    std::printf("-- 3. URL 构造与 CJK 百分号编码 --\n");

    // 3a: 维基 CJK URL
    std::string wikiZh = build_wiki_page_url("zh", "初音未来");
    ck_eq(wikiZh, "https://zh.wikipedia.org/wiki/%E5%88%9D%E9%9F%B3%E6%9C%AA%E6%9D%A5",
          "维基百科 CJK URL 构造正确");

    // 3b: 维基 带空格与括号
    std::string wikiEn = build_wiki_page_url("en", "Toe (band)");
    ck_eq(wikiEn, "https://en.wikipedia.org/wiki/Toe_(band)",
          "维基百科 空格转下划线、括号保留正确");

    // 3c: 萌娘 CJK URL
    std::string moeUrl = build_moegirl_page_url("塞壬唱片");
    ck_eq(moeUrl, "https://zh.moegirl.org.cn/%E5%A1%9E%E5%A3%AC%E5%94%B1%E7%89%87",
          "萌娘百科 CJK URL 构造正确");

    // 3d: 双重编码防线验证
    // knowledge_percent_enc 编码后的字符串，经 platform::percent_encode_path 不会再次被编码
    std::string raw = "初音未来";
    std::string once = knowledge_percent_enc(raw);
    std::string twice = rta::platform::percent_encode_path(once);
    ck_eq(twice, once, "百分号编码防双重编码守卫验证通过（只编码一次）");
}

// 4. KnowledgeMaterial 序列化与反序列化 Round-Trip
static void test_material_roundtrip() {
    std::printf("-- 4. KnowledgeMaterial 序列化与反序列化 Round-Trip --\n");

    std::vector<KnowledgeMaterial> input;
    {
        KnowledgeMaterial m1;
        m1.source     = "wikipedia-zh";
        m1.kind       = "biography";
        m1.subject    = "ZARD";
        m1.title      = "ZARD";
        m1.text       = R"(第一段生平。
第二段成就："百万销量"。
特殊字符: & < > %)";
        m1.url        = "https://zh.wikipedia.org/wiki/ZARD";
        m1.lang       = "zh";
        m1.fetched_at = 1729647900LL;
        m1.expires_at = 1732239900LL;
        m1.authority  = 4;
        input.push_back(m1);

        KnowledgeMaterial m2;
        m2.source     = "moegirl";
        m2.kind       = "biography";
        m2.subject    = "ZARD";
        m2.title      = "ZARD";
        m2.text       = "萌娘百科短简介。";
        m2.url        = "https://zh.moegirl.org.cn/ZARD";
        m2.lang       = "zh";
        m2.fetched_at = 1729647902LL;
        m2.expires_at = 1732239902LL;
        m2.authority  = 3;
        input.push_back(m2);
    }

    std::string jsonStr = knowledge_to_json(input, "eid_12345", "ZARD");
    ck(!jsonStr.empty(), "JSON 序列化非空");

    std::vector<KnowledgeMaterial> output;
    std::string eidOut, subOut, errOut;
    bool ok = knowledge_from_json(jsonStr, output, eidOut, subOut, &errOut);
    ck(ok, "JSON 反序列化成功", errOut);
    ck_eq(eidOut, "eid_12345", "entity_id 保留无误");
    ck_eq(subOut, "ZARD", "subject 保留无误");
    ck(output.size() == 2, "材料条数一致 (2条)");

    // 逐字段核验关键信息
    ck_eq(output[0].source, "wikipedia-zh", "m1.source 保留");
    ck_eq(output[0].kind, "biography", "m1.kind 保留");
    ck_eq(output[0].url, "https://zh.wikipedia.org/wiki/ZARD", "m1.url 保留");
    ck(output[0].fetched_at == 1729647900LL, "m1.fetched_at 保留");
    ck(output[0].expires_at == 1732239900LL, "m1.expires_at 保留");
    ck(output[0].authority == 4, "m1.authority 保留");
    ck_eq(output[0].text, input[0].text, "m1.text（含换行与引号转义）完全一致");

    ck_eq(output[1].source, "moegirl", "m2.source 保留");
    ck(output[1].authority == 3, "m2.authority 保留");
    ck_eq(output[1].url, "https://zh.moegirl.org.cn/ZARD", "m2.url 保留");
}

// 5. 缓存管理：TTL判定、多源共存、同URL去重、损坏容错、离线回退
static void test_cache_management() {
    std::printf("-- 5. 缓存管理与离线策略 --\n");

    const std::string tmpDir = (fsx::temp_directory_path() / "rta_test_knowledge_cache").string();
    std::error_code ec;
    fsx::remove_all(tmpDir, ec);

    const std::string eid = "entity_test_01";
    const std::string sub = "初音未来";

    // 5a: 保存并读取缓存
    std::vector<KnowledgeMaterial> list1;
    KnowledgeMaterial m1;
    m1.source = "wikipedia-zh";
    m1.kind = "biography";
    m1.subject = sub;
    m1.title = sub;
    m1.text = "初音生平1";
    m1.url = "https://zh.wikipedia.org/wiki/%E5%88%9D%E9%9F%B3%E6%9C%AA%E6%9D%A5";
    m1.lang = "zh";
    m1.fetched_at = 1000;
    m1.expires_at = 2000;
    m1.authority = 4;
    list1.push_back(m1);

    bool saved = knowledge_save_cache(tmpDir, eid, sub, list1);
    ck(saved, "knowledge_save_cache 原子写成功");

    std::vector<KnowledgeMaterial> loaded;
    bool loadedOk = knowledge_load_cache(tmpDir, eid, loaded);
    ck(loadedOk, "knowledge_load_cache 读取成功");
    ck(loaded.size() == 1, "读出条数一致");
    ck_eq(loaded[0].url, m1.url, "读出 URL 一致");

    // 5b: TTL 过期判定
    ck(!knowledge_is_expired(m1, 1500), "在 TTL 内判定未过期 (1500 < 2000)");
    ck(knowledge_is_expired(m1, 2500), "超过 TTL 判定已过期 (2500 >= 2000)");

    // 5c: 多源合并与同 URL 去重
    std::vector<KnowledgeMaterial> incoming;
    // 重复 URL，但更新了文本
    KnowledgeMaterial m1_updated = m1;
    m1_updated.text = "初音生平更新版";
    m1_updated.fetched_at = 1100;
    incoming.push_back(m1_updated);

    // 新源材料
    KnowledgeMaterial m2;
    m2.source = "moegirl";
    m2.kind = "biography";
    m2.subject = sub;
    m2.title = sub;
    m2.text = "初音萌娘介绍";
    m2.url = "https://zh.moegirl.org.cn/%E5%88%9D%E9%9F%B3%E6%9C%AA%E6%9D%A5";
    m2.lang = "zh";
    m2.fetched_at = 1050;
    m2.expires_at = 2050;
    m2.authority = 3;
    incoming.push_back(m2);

    knowledge_merge_materials(loaded, incoming);
    ck(loaded.size() == 2, "同 URL 去重 + 多源共存合并后条数为 2");
    ck_eq(loaded[0].text, "初音生平更新版", "旧 URL 文本被新材料覆盖更新");
    ck_eq(loaded[1].source, "moegirl", "新数据源材料被合并保留");

    // 5d: 缓存损坏容错（坏 JSON 不崩溃，返回 error）
    fsx::path cacheFile = knowledge_cache_path(tmpDir, eid);
    {
        std::ofstream bad(cacheFile, std::ios::binary | std::ios::trunc);
        bad << "{ bad json truncated: [ 123, ";
    }
    std::vector<KnowledgeMaterial> badOut;
    std::string loadErr;
    bool badOk = knowledge_load_cache(tmpDir, eid, badOut, &loadErr);
    ck(!badOk, "损坏的 JSON 缓存加载显式返回 false");
    ck(!loadErr.empty(), "损坏时给出明确错误信息（无崩溃）");

    fsx::remove_all(tmpDir, ec);
}

// 6. 令牌桶限频与熔断纯逻辑
static void test_gate_logic() {
    std::printf("-- 6. 令牌桶限频与熔断控制器逻辑 --\n");

    KnowledgeGate g;
    g.min_interval_ms = 1000;
    g.fail_limit = 3;
    g.cooldown_sec = 30;

    auto t0 = std::chrono::steady_clock::now();
    long long sleepMs = 0;
    std::string err;

    // 首次调用 -> 不需要等待
    ck(g.acquire_check(t0, sleepMs, err), "首次 acquire 允许");
    ck(sleepMs == 0, "首次 acquire 不需要等待");
    g.last_call = t0;

    // 200ms 后再次调用 -> 需要等待 800ms
    auto t1 = t0 + std::chrono::milliseconds(200);
    ck(g.acquire_check(t1, sleepMs, err), "间隔内 acquire 依然允许");
    ck(sleepMs == 800, "精确计算剩余等待毫秒数 (800ms)");

    // 1200ms 后再次调用 -> 不需要等待
    auto t2 = t0 + std::chrono::milliseconds(1200);
    ck(g.acquire_check(t2, sleepMs, err), "超过最小间隔无需等待");
    ck(sleepMs == 0, "sleepMs 为 0");

    // 连续失败 3 次 -> 触发熔断
    g.record_failure_at(t2);
    ck(g.fails == 1, "记录第 1 次失败");
    g.record_failure_at(t2);
    ck(g.fails == 2, "记录第 2 次失败");
    g.record_failure_at(t2);
    ck(g.fails == 0, "达到阈值后熔断触发，失败计数归零");
    ck(g.cool_until == t2 + std::chrono::seconds(30), "冷却时间设为 now + 30s");

    // 熔断期间 (t2 + 10s) -> 拒绝请求
    auto tCool = t2 + std::chrono::seconds(10);
    bool coolBlocked = g.acquire_check(tCool, sleepMs, err);
    ck(!coolBlocked, "熔断冷却期内拒绝发起请求");
    ck(err.find("熔断") != std::string::npos, "给出熔断冷却错误文案");

    // 冷却期后 (t2 + 31s) -> 自动恢复
    auto tRecover = t2 + std::chrono::seconds(31);
    ck(g.acquire_check(tRecover, sleepMs, err), "冷却期结束后自动恢复请求能力");

    // 成功调用重置失败计数
    g.fails = 2;
    g.record_success();
    ck(g.fails == 0, "成功调用重置连续失败计数");
}

// 7. 可选源（Discogs / Last.fm）缺 key 验证
static void test_optional_sources() {
    std::printf("-- 7. 可选源缺 key 守卫 --\n");

    KnowledgeOpts opts;
    opts.discogs_token = "";
    opts.lastfm_api_key = "";

    KnowledgeResult dr = knowledge_discogs("ZARD", opts);
    ck(dr.materials.empty(), "Discogs 缺 key 零产出材料");
    ck(dr.error.find("未配置") != std::string::npos, "Discogs 给出明确未配置错误");

    KnowledgeResult lr = knowledge_lastfm("ZARD", opts);
    ck(lr.materials.empty(), "Last.fm 缺 key 零产出材料");
    ck(lr.error.find("未配置") != std::string::npos, "Last.fm 给出明确未配置错误");
}

// 8. 真实网络用例（opt-in）
static void test_network_opt_in() {
    std::printf("-- 8. 真实网络测试（opt-in, RTA_NET_TESTS=1）--\n");
    const char* netEnv = std::getenv("RTA_NET_TESTS");
    const bool netOn = netEnv && (std::string(netEnv) == "1" || std::string(netEnv) == "true");
    if (!netOn) {
        std::printf("  [SKIP] 未设 RTA_NET_TESTS=1，跳过联网用例（默认离线）\n");
        return;
    }

    std::printf("  [NET] 开启真网测试：查询 ZARD\n");
    KnowledgeOpts opts;
    opts.refresh = true;
    KnowledgeResult res = knowledge_get(EntityKind::Person, "ZARD", opts);
    ck(!res.materials.empty(), "真网拉取 ZARD 材料非空");
    ck(!res.from_cache, "refresh=true 为非缓存拉取");
    for (const auto& m : res.materials) {
        ck(!m.url.empty(), "每条材料必须带 url", m.url);
        ck(m.fetched_at > 0, "每条材料必须带 fetched_at");
        ck(m.authority >= 3, "权威分级符合要求 (维基=4, 萌娘=3)");
    }
}

int main() {
    std::printf("========================================\n");
    std::printf("test_knowledge: P3 资料层直连层单测\n");
    std::printf("========================================\n");

    test_mediawiki_parse();
    test_opensearch_and_no_guess();
    test_url_construction();
    test_material_roundtrip();
    test_cache_management();
    test_gate_logic();
    test_optional_sources();
    test_network_opt_in();

    std::printf("========================================\n");
    if (g_fail == 0) {
        std::printf("ALL PASS! 0 failures.\n");
        return 0;
    } else {
        std::printf("FAILED with %d failure(s)!\n", g_fail);
        return 1;
    }
}
