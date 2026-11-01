// test_review2_json_cache.cpp -- 两轮审查 F28/F29/F30/F32 的独立回归门禁
//
// 全部离线：不联网、不打开音频设备、不读写用户真实配置（全部落在 %TEMP%）。
//
// F28 JSON 严格解析：截断/尾垃圾/非法 token 必须失败；合法 null 与解析失败要能区分。
// F29 代理对：\uD83D\uDE00 必须得到 F0 9F 98 80；孤立代理在严格模式报错。
// F30 Knowledge 缓存语言身份：zh 缓存不能冒充 en 命中；负缓存也按语言隔离。
// F32 HealthStore 反序列化：全字段往返（真峰/判定档/完整性/quality），旧缓存判失效。
#include "net/json.h"
#include "meta/knowledge.h"
#include "analysis/health_store.h"
#include "analysis/track_health.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <filesystem>

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static std::string temp_dir(const char* name) {
    char d[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, d);
    const std::string p = std::string(d) + name;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(p), ec);
    return p;
}

static std::string hex_of(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) { o += hx[c >> 4]; o += hx[c & 0xF]; }
    return o;
}

// ---------------- F28 / F29 ----------------
static void test_json() {
    printf("=== 1. JSON 严格解析（F28）===\n");
    {
        const char* good[] = { "{\"a\":1}", "[]", "null", "true", "\"x\"", "{\"a\":[1,2,{\"b\":null}]}" };
        for (const char* g : good) {
            rta::net::Json j;
            std::string e;
            check(rta::net::json_parse_strict(g, j, &e), std::string("合法 JSON 通过：") + g, e);
        }
    }
    {
        struct Bad { const char* text; const char* what; };
        const Bad bad[] = {
            { "{\"path\":\"track.wav\",\"fields\":{\"title\":{\"value\":\"PARTIAL", "对象内部截断" },
            { "[1,2,3", "数组截断" },
            { "{\"a\":1} trailing", "尾部垃圾" },
            { "{\"a\":tru}", "非法字面量 true" },
            { "{\"a\":}", "缺值" },
            { "\"unterminated", "字符串未闭引号" },
            { "", "空文档" },
        };
        for (const auto& b : bad) {
            rta::net::Json j;
            std::string e;
            const bool ok = rta::net::json_parse_strict(b.text, j, &e);
            check(!ok && !e.empty(), std::string("非法 JSON 被拒：") + b.what, e);
        }
    }
    {
        // 合法 null 必须与"解析失败"区分开
        rta::net::Json j;
        std::string e;
        const bool ok = rta::net::json_parse_strict("null", j, &e);
        check(ok && j.type == rta::net::Json::Null, "合法 null：解析成功且类型是 Null", e);
        rta::net::Json j2;
        const bool ok2 = rta::net::json_parse_strict("{", j2, &e);
        check(!ok2, "解析失败时返回 false（调用方据此区分）", e);
    }

    printf("=== 2. JSON 代理对与 UTF-8（F29）===\n");
    {
        rta::net::Json j;
        std::string e;
        const bool ok = rta::net::json_parse_strict("\"\\ud83d\\ude00\"", j, &e);
        check(ok, "emoji 代理对能解析", e);
        const std::string want = "F09F9880";           // U+1F600
        check(ok && hex_of(j.str) == want, "emoji 编码为 4 字节 UTF-8",
              ok ? hex_of(j.str) : e);
    }
    {
        rta::net::Json j;
        std::string e;
        const bool ok = rta::net::json_parse_strict("\"\\u4f60\\u597d\"", j, &e);
        check(ok && j.str == "你好", "BMP 汉字 \\uXXXX 正确", ok ? j.str : e);
    }
    {
        rta::net::Json j;
        std::string e;
        check(!rta::net::json_parse_strict("\"\\ud83d\"", j, &e), "孤立高代理被拒（严格）", e);
        check(!rta::net::json_parse_strict("\"\\ude00\"", j, &e), "孤立低代理被拒（严格）", e);
        check(!rta::net::json_parse_strict("\"\\u12\"", j, &e), "\\u 不足 4 位被拒（严格）", e);
        check(!rta::net::json_parse_strict("\"\\uZZZZ\"", j, &e), "非法十六进制被拒（严格）", e);
    }
    {
        // 宽松模式绝不产出非法 UTF-8（孤立代理 → U+FFFD）
        const rta::net::Json j = rta::net::json_parse("\"\\ud83d\"");
        check(hex_of(j.str) == "EFBFBD", "宽松模式孤立代理替换为 U+FFFD", hex_of(j.str));
    }
}

// ---------------- F30 ----------------
static void test_knowledge_lang() {
    printf("=== 3. Knowledge 缓存语言身份（F30）===\n");
    using namespace rta::meta;
    const long long now = (long long)std::time(nullptr);
    KnowledgeMaterial zh;
    zh.source = "wikipedia-zh"; zh.kind = "biography"; zh.subject = "测试实体";
    zh.title = "测试"; zh.text = "中文材料"; zh.url = "https://example.invalid/zh";
    zh.lang = "zh"; zh.fetched_at = now; zh.expires_at = now + 86400; zh.authority = 4;
    KnowledgeMaterial en = zh;
    en.source = "wikipedia-en"; en.text = "english material";
    en.url = "https://example.invalid/en"; en.lang = "en";
    std::vector<KnowledgeMaterial> all = { zh, en };

    check(knowledge_has_unexpired_for_lang(all, "zh", now), "zh 请求命中 zh 材料");
    check(knowledge_has_unexpired_for_lang(all, "en", now), "en 请求命中 en 材料");
    std::vector<KnowledgeMaterial> onlyZh = { zh };
    check(!knowledge_has_unexpired_for_lang(onlyZh, "en", now),
          "只有 zh 缓存时 en 请求判未命中（旧实现会误命中）");
    const auto f = knowledge_filter_by_lang(all, "en");
    check(f.size() == 1 && f[0].lang == "en", "按语言裁剪只返回 en 材料",
          std::to_string(f.size()) + " 条");
    check(knowledge_filter_by_lang(all, "").size() == 2, "lang 为空时不筛（老调用方语义不变）");

    // 负缓存按语言隔离
    const std::string base = temp_dir("rta_review2_meta");
    const std::string eid = "ent_test_lang";
    knowledge_mark_miss(base, eid, "测试实体", "no result", "zh");
    check(knowledge_recent_miss(base, eid, 6 * 3600, nullptr, "zh"), "zh 负缓存命中 zh 请求");
    check(!knowledge_recent_miss(base, eid, 6 * 3600, nullptr, "en"),
          "zh 负缓存不拦 en 请求（旧实现只按实体 ID）");
    knowledge_clear_miss(base, eid);
}

// ---------------- F32 ----------------
static void test_health_store() {
    printf("=== 4. HealthStore 全字段往返（F32）===\n");
    using namespace rta::analysis;
    const std::string dir = temp_dir("rta_review2_health");
    const std::string path = dir + "\\health.json";
    DeleteFileA(path.c_str());

    TrackHealth h;
    h.drDb = 12.345; h.lufs = -13.5; h.lra = 7.25; h.peakDbfs = -0.5;
    h.truePeakDbtp = 2.5;                       // ★ 旧缓存丢失的字段
    h.losslessSuspected = true; h.cutoffHz = 16000.0; h.confidence = 0.87;
    h.reason = "高频陡沿"; h.fake24bit = true;
    h.fakeVerdict = 1; h.edgeDb = 41.5; h.windows = 64; h.windowFill = 0.98;
    h.durationSec = 213.5; h.fs = 44100; h.channels = 2; h.bits = 16;
    h.status = "ready"; h.error.clear();
    h.declaredSec = 213.6; h.incomplete = false; h.note.clear();
    h.analyzedAtMs = 1792000000000LL;
    h.hasQuality = true;
    h.quality.valid = true;
    h.quality.thdnDb = -80.5; h.quality.thdnPct = 0.0091; h.quality.thdnAvailable = true;
    h.quality.thdnMethod = "notch";
    h.quality.nonTonalDb = -70.25; h.quality.imdAvailable = false; h.quality.imdDb = 0.0;
    h.quality.noiseFloorDbfs = -96.5; h.quality.spectralMinDbfs = -120.5; h.quality.snrDb = 88.5;
    h.quality.centroidHz = 2400.5; h.quality.tiltDbPerOct = -3.5;
    h.quality.rolloff95Hz = 18000.0; h.quality.hfRatio10kDb = -9.5;
    h.quality.peakDbfs = -0.5; h.quality.rmsDbfs = -18.5;
    h.quality.clippedSamples = 12; h.quality.clippedPct = 0.0001;
    h.quality.clippedRuns = 3; h.quality.longestClipRun = 5; h.quality.crestDb = 18.0;
    h.quality.stereo = true; h.quality.channelCorr = 0.75;
    h.quality.monoLossDb = 1.5; h.quality.sideMidDb = -12.0;
    h.quality.dynP95P10Db = 14.0; h.quality.dynCentroidDbfs = -20.0;
    h.quality.quietP10Dbfs = -40.0; h.quality.silencePct = 2.5;
    h.quality.spectralWindows = 128; h.quality.note = "ok";

    {
        HealthStore st(path);
        check(st.loadError().empty(), "新建缓存无加载错误", st.loadError());
        st.upsert("D:\\music\\a.flac", 123456789, 987654, h);
        check(st.save(), "save() 成功");
    }
    {
        HealthStore st(path);
        check(st.loadError().empty(), "重载无错误", st.loadError());
        TrackHealth out;
        const bool hit = st.lookup("D:\\music\\a.flac", 123456789, 987654, out);
        check(hit, "lookup 命中", std::to_string(st.size()) + " 条");
        if (hit) {
            char b2[200];
            std::snprintf(b2, sizeof(b2), "(truePeak %.3f dBTP, verdict %d, windows %d, quality %d)",
                          out.truePeakDbtp, out.fakeVerdict, out.windows, out.hasQuality ? 1 : 0);
            check(std::fabs(out.truePeakDbtp - 2.5) < 1e-9 && out.fakeVerdict == 1 &&
                  out.windows == 64 && out.hasQuality,
                  "真峰 / 判定档 / 谱窗数 / quality 全部往返", b2);
            check(std::fabs(out.quality.thdnDb + 80.5) < 1e-9 &&
                  std::fabs(out.quality.centroidHz - 2400.5) < 1e-9 &&
                  out.quality.clippedSamples == 12 && out.quality.stereo,
                  "quality 段逐字段往返");
            check(std::fabs(out.declaredSec - 213.6) < 1e-9 && !out.incomplete,
                  "完整性字段往返");
        }
    }
    // 截断的缓存文件必须被判损坏（且拒绝回写），不能静默当空库
    {
        FILE* f = std::fopen(path.c_str(), "r+b");
        if (f) { std::fseek(f, 0, SEEK_END); const long n = std::ftell(f);
                 std::fseek(f, 0, SEEK_SET); std::fclose(f); (void)n; }
        std::string raw;
        { FILE* g = std::fopen(path.c_str(), "rb");
          if (g) { char buf[4096]; size_t got;
                   while ((got = std::fread(buf, 1, sizeof(buf), g)) > 0) raw.append(buf, got);
                   std::fclose(g); } }
        const std::string cut = raw.substr(0, raw.size() / 2);
        { FILE* g = std::fopen(path.c_str(), "wb");
          if (g) { std::fwrite(cut.data(), 1, cut.size(), g); std::fclose(g); } }
        HealthStore st(path);
        check(!st.loadError().empty(), "截断的缓存被判损坏（不再静默当空库）", st.loadError());
        check(!st.save(), "损坏缓存拒绝回写（不覆盖原件）");
    }
    DeleteFileA(path.c_str());
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_json();
    test_knowledge_lang();
    test_health_store();
    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}