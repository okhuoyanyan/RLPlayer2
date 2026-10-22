// test_discogs.cpp — P3 资料层④b：Discogs L4 古典作品版本推荐单元测试
//
// 覆盖任务书要求：
//   1. search/versions JSON 解析（正常、缺字段、空数组、畸形）
//   2. 限频与熔断纯函数（令牌桶补充、消耗判定、熔断激活判定、429 保护）
//   3. 标题与演出者解析纯函数（去消歧义后缀、提取乐团与指挥、分离作曲家）
//   4. URL 编码构造（特殊字符转义、空格处理）
//   5. 缓存序列化 round-trip / TTL 判定 / 离线回退 / 损坏容错
//   6. complete 判定（>= 5 且全带 url 为 true；不足 5 条如实 false）
//   7. 真实网络用例 opt-in（RTA_NET_TESTS=1 开启，默认 [SKIP] exit 0）
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "meta/discogs.h"
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
// 夹具定义
// ---------------------------------------------------------------------------

static const char* kFixtureSearchNormal = R"json({
  "pagination": { "items": 10, "pages": 1 },
  "results": [
    {
      "id": 1820755,
      "type": "master",
      "title": "Maurice Ravel, Nederlands Philharmonisch Orkest (2), Gordan Nikolitch, Carlo Rizzi - Maurice Ravel: La Valse, Ma mere l'Oye, Tzigane, Bolero, Pavane",
      "year": "2013",
      "label": ["Tacet"],
      "format": ["SACD", "Hybrid", "Multichannel", "Stereo"],
      "country": "Germany",
      "uri": "/master/1820755"
    },
    {
      "id": 1025098,
      "type": "master",
      "title": "Leonard Bernstein And Orchestre National De France - Ravel: Bolero",
      "year": "1980",
      "label": ["CBS Masterworks"],
      "format": ["Vinyl", "LP"],
      "country": "US",
      "uri": "/master/1025098"
    },
    {
      "id": 915325,
      "type": "release",
      "title": "Maurice Ravel - Boléro",
      "year": "1985",
      "label": ["TELDEC"],
      "format": ["Vinyl 12""],
      "country": "Germany",
      "uri": "/release/915325"
    }
  ]
})json";

static const char* kFixtureSearchMissingFields = R"json({
  "results": [
    {
      "id": 123456,
      "title": "Just Title"
    }
  ]
})json";

static const char* kFixtureSearchEmpty = R"json({
  "results": []
})json";

static const char* kFixtureSearchMalformed = R"json({
  "error": "syntax"
})json";

static const char* kFixtureVersionsNormal = R"json({
  "pagination": { "items": 3 },
  "versions": [
    {
      "id": 18245671,
      "label": "Tacet",
      "country": "Germany",
      "title": "Maurice Ravel: La Valse, Ma mere l'Oye, Tzigane, Bolero, Pavane",
      "format": "Album, Stereo",
      "released": "2012"
    },
    {
      "id": 4310089,
      "label": "Tacet",
      "country": "Germany",
      "title": "oreloB",
      "format": "LP",
      "released": "2013"
    },
    {
      "id": 16049501,
      "label": ["Tacet", "Pallas"],
      "country": "Germany",
      "title": "Maurice Ravel: La Valse, Ma mere l'Oye, Tzigane, Bolero, Pavane",
      "format": ["Blu-ray Audio", "Album"],
      "released": "2013"
    }
  ]
})json";

static const char* kFixtureVersionsEmpty = R"json({
  "versions": []
})json";

// ---------------------------------------------------------------------------
// 测试主体
// ---------------------------------------------------------------------------

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== meta/discogs.h Discogs L4 版本推荐单元测试 ==\n");

    // 1. search/versions JSON 解析
    std::printf("-- 1. search/versions JSON 解析 --\n");
    {
        std::vector<SearchResultItem> hits;
        std::string err;
        ck(parse_discogs_search_json(kFixtureSearchNormal, hits, &err), "正常 search JSON 解析成功");
        ck(hits.size() == 3, "search 命中 3 条", "实际 " + std::to_string(hits.size()));
        ck(hits[0].id == 1820755 && hits[0].type == "master", "第 1 条为 master 类型且 id 正确");
        ck(hits[0].year == 2013, "年份解析正确", "year=" + std::to_string(hits[0].year));
        ck(hits[0].label == "Tacet", "厂牌数组第一项正确提取");
        ck(hits[0].format.find("SACD") != std::string::npos, "格式数组正确连接");

        std::vector<SearchResultItem> hitsPartial;
        ck(parse_discogs_search_json(kFixtureSearchMissingFields, hitsPartial), "缺失非必填字段容错成功");
        ck(hitsPartial.size() == 1 && hitsPartial[0].year == 0 && hitsPartial[0].label.empty(), "缺失字段默认值安全");

        std::vector<SearchResultItem> hitsEmpty;
        ck(parse_discogs_search_json(kFixtureSearchEmpty, hitsEmpty) && hitsEmpty.empty(), "空 results 数组解析正常");

        std::vector<SearchResultItem> hitsMal;
        std::string malErr;
        ck(!parse_discogs_search_json(kFixtureSearchMalformed, hitsMal, &malErr), "缺少 results 字段报错");

        std::vector<MasterVersionItem> vItems;
        ck(parse_discogs_versions_json(kFixtureVersionsNormal, vItems), "正常 versions JSON 解析成功");
        ck(vItems.size() == 3, "versions 条目数为 3");
        ck(vItems[0].id == 18245671 && vItems[0].year == 2012, "version id 与年份正确");
        ck(vItems[2].format.find("Blu-ray Audio") != std::string::npos, "version format 数组正确处理");

        std::vector<MasterVersionItem> vEmpty;
        ck(parse_discogs_versions_json(kFixtureVersionsEmpty, vEmpty) && vEmpty.empty(), "空 versions 数组解析正常");
    }

    // 2. 标题与演出者解析纯函数
    std::printf("-- 2. 标题与演出者解析纯函数 --\n");
    {
        ck(clean_discogs_artist("Mahler*") == "Mahler", "清理尾部星号");
        ck(clean_discogs_artist("Nederlands Philharmonisch Orkest (2)") == "Nederlands Philharmonisch Orkest", "清理尾部序号 (2)");
        ck(clean_discogs_artist("  Herbert von Karajan*  ") == "Herbert von Karajan", "清理首尾空格与星号");

        std::string p, c, o, wt;
        parse_classical_title(
            "Maurice Ravel, Nederlands Philharmonisch Orkest (2), Gordan Nikolitch, Carlo Rizzi - Maurice Ravel: La Valse, Ma mere l'Oye, Tzigane, Bolero, Pavane",
            "Maurice Ravel", p, c, o, wt);
        ck(o == "Nederlands Philharmonisch Orkest", "正确识别乐团: " + o);
        ck(c == "Gordan Nikolitch" || c == "Carlo Rizzi", "正确识别指挥/独奏: " + c);
        ck(p.find("Nederlands Philharmonisch Orkest") != std::string::npos, "演出者列表包含乐团");
        ck(p.find("Maurice Ravel") == std::string::npos, "演出者中剥除作曲家本人");

        parse_classical_title(
            "Sir Neville Marriner, Staatskapelle Dresden - Ravel - Bolero",
            "Maurice Ravel", p, c, o, wt);
        ck(o == "Staatskapelle Dresden", "Staatskapelle 识别为乐团: " + o);
        ck(c == "Sir Neville Marriner", "Sir Neville Marriner 识别为指挥: " + c);
    }

    // 3. 限频与熔断纯函数
    std::printf("-- 3. 限频与熔断纯函数 --\n");
    {
        // 令牌桶计算
        double r1 = DiscogsRateLimiter::calc_refilled_tokens(1.0, 2.4, 25.0 / 60.0, 5.0); // 2.4s * (25/60) = 1.0 token
        ck(std::abs(r1 - 2.0) < 0.05, "2.4秒补充约 1.0 个令牌 (总计 2.0)", "actual=" + std::to_string(r1));

        double r2 = DiscogsRateLimiter::calc_refilled_tokens(4.0, 100.0, 25.0 / 60.0, 5.0);
        ck(r2 == 5.0, "令牌补充严格不超过容量上限 5.0");

        ck(DiscogsRateLimiter::can_consume(1.0), "1.0 令牌允许消费 1.0");
        ck(!DiscogsRateLimiter::can_consume(0.9), "0.9 令牌禁止消费 1.0");

        // 熔断器激活判定
        ck(DiscogsRateLimiter::circuit_breaker_active(true, 30000, 60000), "熔断器激活中 (30s < 60s)");
        ck(!DiscogsRateLimiter::circuit_breaker_active(true, 60001, 60000), "熔断期满自动失效 (60.001s >= 60s)");
        ck(!DiscogsRateLimiter::circuit_breaker_active(false, 100, 60000), "未开熔断器不生效");

        // 限流器实例状态流转
        auto& lim = DiscogsRateLimiter::instance();
        lim.reset_circuit();
        ck(!lim.is_circuit_open(), "初始熔断器关闭");
        lim.trip_circuit(2); // 熔断 2 秒
        ck(lim.is_circuit_open(), "触发熔断后处于开启状态");
        ck(!lim.acquire(100), "熔断状态下 acquire 立即失败");
        lim.reset_circuit();
        ck(!lim.is_circuit_open(), "手动重置后熔断恢复");
    }

    // 4. URL 编码构造
    std::printf("-- 4. URL 编码构造 --\n");
    {
        ck(discogs_url_encode("Bolero") == "Bolero", "普通英文字符不编码");
        ck(discogs_url_encode("Boléro") == "Bol%C3%A9ro", "法语带重音字符 UTF-8 百分号编码");
        ck(discogs_url_encode("Maurice Ravel") == "Maurice+Ravel", "空格转为 + 号");
    }

    // 5. 缓存序列化 round-trip、TTL 判定与离线回退
    std::printf("-- 5. 缓存序列化 round-trip、TTL 与离线回退 --\n");
    {
        WorkVersionsResult orig;
        orig.ok = true;
        orig.work.id = entity_id_keyed(EntityKind::Work, {"Maurice Ravel", "Boléro"});
        orig.work.title = "Boléro";
        orig.work.composer = "Maurice Ravel";
        orig.complete = true;
        orig.generated_at = 1792690000;
        orig.expires_at = 1792690000 + 7 * 86400;

        DiscogsPerformance dp1;
        dp1.id = "discogs-release:915325";
        dp1.title = "Boléro";
        dp1.performers = "Sir Neville Marriner, Staatskapelle Dresden";
        dp1.conductor = "Sir Neville Marriner";
        dp1.orchestra = "Staatskapelle Dresden";
        dp1.year = 1985;
        dp1.label = "TELDEC";
        dp1.format = R"(Vinyl 12")";
        dp1.country = "Germany";
        dp1.url = "https://www.discogs.com/release/915325";
        dp1.source = "discogs";
        dp1.fetched_at = 1792690000;
        orig.performances.push_back(dp1);

        const std::string jsonStr = work_versions_to_json(orig);
        WorkVersionsResult loaded;
        std::string loadErr;
        ck(work_versions_from_json(jsonStr, loaded, &loadErr), "work_versions_to_json -> work_versions_from_json round-trip 成功");
        ck(loaded.work.title == "Boléro" && loaded.work.composer == "Maurice Ravel", "作品名称与作曲家一致");
        ck(loaded.complete == true, "complete 标记无损往返");
        ck(loaded.performances.size() == 1, "演出版本列表数量一致");
        ck(loaded.performances[0].url == "https://www.discogs.com/release/915325", "版本 URL 一致");

        // TTL
        ck(!versions_is_expired(orig, 1792690000 + 3 * 86400), "3天内缓存判定未过期");
        ck(versions_is_expired(orig, 1792690000 + 8 * 86400), "8天缓存判定已过期");

        // 原子落盘与读取
        const std::string tmpDir = "build/test-discogs-cache";
        fsx::create_directories(tmpDir);
        ck(versions_save_cache(tmpDir, orig.work.id, orig), "versions_save_cache 原子落盘成功");

        WorkVersionsResult diskLoaded;
        ck(versions_load_cache(tmpDir, orig.work.id, diskLoaded), "versions_load_cache 读回成功");
        ck(diskLoaded.performances.size() == 1 && diskLoaded.performances[0].id == dp1.id, "落盘读回内容完整");

        // 离线模式调用
        DiscogsOptions offOpts;
        offOpts.metabase = tmpDir;
        offOpts.offline = true;
        WorkVersionsResult offRes = discogs_versions("Boléro", "Maurice Ravel", offOpts);
        ck(offRes.ok && offRes.from_cache && offRes.offline, "离线模式成功回退至本地缓存 (offline=true, from_cache=true)");

        // 离线模式无缓存测试
        WorkVersionsResult noCacheOff = discogs_versions("NonExistentWork", "UnknownComposer", offOpts);
        ck(!noCacheOff.ok && noCacheOff.offline, "离线且无缓存返回 ok=false");

        fsx::remove_all(tmpDir);
    }

    // 6. complete 判定规则
    std::printf("-- 6. complete 判定规则 --\n");
    {
        WorkVersionsResult r;
        r.complete = false;
        for (int i = 0; i < 4; ++i) {
            DiscogsPerformance p;
            p.id = "p" + std::to_string(i);
            p.url = "https://www.discogs.com/release/" + std::to_string(i);
            r.performances.push_back(p);
        }
        ck(r.performances.size() < 5, "不足 5 条演绎");

        // 补充至 5 条
        DiscogsPerformance p5;
        p5.id = "p5";
        p5.url = "https://www.discogs.com/release/5";
        r.performances.push_back(p5);
        bool allOk = true;
        for (const auto& it : r.performances) if (it.url.empty()) allOk = false;
        r.complete = (r.performances.size() >= 5) && allOk;
        ck(r.complete == true, "满 5 条且均有 url 时 complete 为 true");

        // 某条 url 为空
        r.performances[0].url.clear();
        allOk = true;
        for (const auto& it : r.performances) if (it.url.empty()) allOk = false;
        r.complete = (r.performances.size() >= 5) && allOk;
        ck(r.complete == false, "虽然条数 >= 5，但有缺少 url 的条目时 complete 为 false (硬门生效)");
    }

    // 7. 真实网络用例 opt-in
    std::printf("-- 7. 真实网络用例 opt-in --\n");
    {
        char* net_env = nullptr;
        size_t len = 0;
        _dupenv_s(&net_env, &len, "RTA_NET_TESTS");
        if (!net_env || std::string(net_env) != "1") {
            std::printf("  [SKIP] RTA_NET_TESTS!=1（网络用例默认跳过，零网络请求保证离线绿色）\n");
        } else {
            std::printf("  [INFO] RTA_NET_TESTS=1，执行真实 Discogs API 抓取 Boléro\n");
            DiscogsOptions nOpts;
            nOpts.refresh = true;
            nOpts.timeout_ms = 15000;
            WorkVersionsResult liveRes = discogs_versions("Boléro", "Maurice Ravel", nOpts);
            ck(liveRes.ok, "真实 Discogs 请求成功");
            ck(liveRes.performances.size() >= 5, "Boléro 演绎版本 >= 5 条", "条数=" + std::to_string(liveRes.performances.size()));
            ck(liveRes.complete == true, "complete 判定为 true");
            if (!liveRes.performances.empty()) {
                ck(!liveRes.performances[0].url.empty(), "第 1 条版本有真实 url: " + liveRes.performances[0].url);
            }
        }
        if (net_env) free(net_env);
    }

    std::printf("\n测试结果: %s (失败数: %d)\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
