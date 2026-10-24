// test_metacache.cpp — 实体资料缓存「共用 / 不重复付费 / 命中即秒回」的回归测试
//
// 覆盖用户反馈 261024-04 的 #6 与 #4（后端侧）：
//   #6「单曲页 AI 整理里的艺术家 与 主页艺术家页 是否共用一个缓存？」
//   #4「专辑页 AI 资料非常慢」（第二次进入必须命中缓存，不再等 LLM）
//
// 为什么需要它（铁律 3：测试要证明的是真事，不是形状）：
//   * 缓存键 = entity_id(kind, 归一化名字)，两条路径不可能靠"看起来一样"来共用 ——
//     必须证明**第二次请求没有发起任何 LLM/检索 HTTP**，而不只是"返回了内容"。
//     这里用 LlmConfig::transport 钩子做计数型 mock：一次调用都没发生 = 计数不变。
//   * 判据是"计数"而不是"耗时"：耗时会被机器抖动带偏，计数不会。
//   * 全离线：不碰网络（L2 材料直接写缓存文件；search/fetch/chat 全走 mock）。
//
// 顺带锁死 #4 的分阶段耗时可观测性：新生成结果的 diag 必须含 knowledge/search/fetch/llm
// 四个字段（没有它们，"为什么慢"就只能靠猜，261024-02 #1 就是为此加的）。
#include "meta/retrieval.h"
#include "meta/knowledge.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

using namespace rta::meta;
namespace fsx = std::filesystem;

static int g_fail = 0;
static void ck(bool cond, const std::string& what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s%s%s\n", what.c_str(), detail.empty() ? "" : "  ",
                          detail.c_str());
    else { std::printf("  [FAIL] %s  %s\n", what.c_str(), detail.c_str()); ++g_fail; }
}

static long long now_sec() { return (long long)std::time(nullptr); }

// JSON 字符串转义（只服务于 mock：把内层 JSON 塞进 content 字段）
static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// 计数型 mock：把 DeepSeek 的三个端点全部拦住并记账
// ---------------------------------------------------------------------------
struct MockLlm {
    int chat = 0, search = 0, fetch = 0;
    std::string materialUrl;    // 综合结果里引用的权威材料 URL
    std::string hitUrl;         // 检索命中页 URL（用来证明 allowed_urls 汇合正确）

    rta::net::HttpResponse reply(const std::string& s) const {
        rta::net::HttpResponse r;
        r.ok = true;
        r.status = 200;
        r.contentType = "application/json";
        r.body.assign(s.begin(), s.end());
        return r;
    }

    rta::net::HttpResponse operator()(const std::string& url, const std::string& body,
                                      const std::vector<std::string>&) {
        (void)body;
        if (url.find("/tools/search") != std::string::npos) {
            ++search;
            return reply(std::string(R"json({"results":[{"title":"检索命中页","url":")json") +
                         hitUrl + R"json(","snippet":"命中页摘要正文，用于证明检索阶段结果被合并进提示词。",
                         "summary":"摘要","date":"2025-01-01","rerank_score":2.4,"authority":3}]})json");
        }
        if (url.find("/tools/fetch") != std::string::npos) {
            ++fetch;
            return reply(R"json({"title":"检索命中页","content":"抓取到的正文（mock）。"})json");
        }
        ++chat;   // /chat/completions
        // 严格按 parse_synthesis_reply 的要求：每段至少 1 条引用，引用 id 必须已定义，
        // 引用 url 必须落在 allowed_urls 里。开启检索时**同时**引用材料与检索命中，
        // 于是"两阶段并行后 allowed_urls 是否残缺"这件事被这条判据钉住。
        const bool withHit = !hitUrl.empty();
        std::string cites = std::string(R"json({"id":"c1","title":"材料页","url":")json") + materialUrl +
                            R"json(","source":"wikipedia-zh","fetched_at":1792690000})json";
        std::string ids = "\"c1\"";
        if (withHit) {
            cites += std::string(R"json(,{"id":"c2","title":"检索命中页","url":")json") + hitUrl +
                     R"json(","source":"sogou","fetched_at":1792690100})json";
            ids += ",\"c2\"";
        }
        const std::string content = std::string(R"json({
  "disclaimer": "AI 生成，仅依据下方来源材料综合；可能不完整，请以来源为准",
  "sections": [
    {"heading": "简介", "paragraphs": [
      {"text": "（mock）依据权威材料与检索正文生成的一句话介绍。", "citations": [)json") + ids + R"json(]}
    ]}
  ],
  "citations": [)json" + cites + R"json(],
  "fields": {"风格": "（mock）合成器流行"}
})json";
        // llm_chat 解析的是 OpenAI 兼容信封：正文必须在 choices[0].message.content 里，
        // 而且是**转义后的字符串**。直接把裸 JSON 当响应体会被判「响应缺少 choices」
        // —— 这是 mock 的头号坑（第一次写这个测试就踩了）。
        return reply(std::string(R"json({"id":"mock","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":")json")
                     + json_escape(content) +
                     R"json("},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":20,"total_tokens":30}})json");
    }
};

int main() {
    std::printf("== 实体资料缓存（共用 / 不重复付费 / 命中秒回）回归测试 ==\n");

    const fsx::path base = fsx::temp_directory_path() / "rta_be4_metacache";
    std::error_code ec;
    fsx::remove_all(base, ec);
    fsx::create_directories(base, ec);
    const std::string baseDir = meta_string_of(base);

    // ---------------- 1) 准备：L2 权威材料直接落缓存（不联网） ----------------
    std::printf("-- 1. 预置 L2 材料（离线），确认 knowledge_get 走缓存且不联网 --\n");
    const std::string person = "Ivoris";              // 艺术家页用的写法
    const std::string personPadded = "  ivoris  ";    // 单曲页常见的写法差异（大小写+空格）
    const std::string eid = entity_id(EntityKind::Person, person);
    {
        KnowledgeMaterial m;
        m.source = "wikipedia-zh";
        m.kind = "biography";
        m.subject = person;
        m.title = person;
        m.text = "（预置材料）Ivoris 是一位音乐人。";
        m.url = "https://zh.wikipedia.org/wiki/Ivoris";
        m.lang = "zh";
        m.fetched_at = now_sec();
        m.expires_at = m.fetched_at + 30 * 86400;
        m.authority = 4;
        ck(knowledge_save_cache(baseDir, eid, person, { m }), "写入 L2 材料缓存 knowledge.json");

        KnowledgeOpts kopts;
        kopts.metabase = baseDir;
        const auto t0 = std::chrono::steady_clock::now();
        const KnowledgeResult kr = knowledge_fetch(EntityKind::Person, person, kopts);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ck(!kr.materials.empty() && kr.from_cache, "knowledge_fetch 命中 L2 缓存（from_cache=1）");
        ck(ms < 500, "L2 命中耗时 < 500ms（实测 " + std::to_string(ms) + "ms）");
        ck(kr.materials.size() == 1 && kr.materials[0].url == m.url, "缓存里的来源 URL 原样保留");
    }

    // ---------------- 2) #6：第二次请求不发起任何 LLM / 检索调用 ----------------
    std::printf("-- 2. #6 艺术家摘要：同一实体第二次请求必须零调用 --\n");
    {
        MockLlm mock;
        mock.materialUrl = "https://zh.wikipedia.org/wiki/Ivoris";
        mock.hitUrl = "https://zh.wikipedia.org/wiki/Ivoris_discography";

        LlmConfig cfg;
        cfg.apiKey = "test-key";
        cfg.model = "deepseek-flash";
        cfg.usagePath = meta_string_of(base / "llm-usage.json");   // 不污染构建目录
        cfg.transport = [&mock](const std::string& u, const std::string& b,
                                const std::vector<std::string>& h) { return mock(u, b, h); };

        RetrievalOptions ro;
        ro.metabase = baseDir;
        ro.enable_search = true;       // 两阶段全开：验证并行后的汇合
        ro.max_fetch_pages = 1;
        ro.llm_cfg = cfg;

        const auto t0 = std::chrono::steady_clock::now();
        const SummaryResult first = retrieval_fetch(EntityKind::Person, person, "zh", ro);
        const long long firstMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        ck(first.ok, "首次生成成功（ok=1）", first.error);
        ck(first.sections.size() == 1 && first.sections[0].paragraphs.size() == 1,
           "综合结果结构完整（sections/paragraphs）");
        ck(first.citations.size() == 2, "两条引用都在（材料 + 检索命中）——证明 allowed_urls 汇合完整");
        ck(mock.chat == 1 && mock.search == 1 && mock.fetch == 1,
           "首次：chat=1 search=1 fetch=1（实测 " + std::to_string(mock.chat) + "/" +
               std::to_string(mock.search) + "/" + std::to_string(mock.fetch) + "）");
        // #4：分阶段耗时必须齐全，否则"为什么慢"无法归因
        ck(first.diag.find("knowledge=") != std::string::npos &&
               first.diag.find("search=") != std::string::npos &&
               first.diag.find("fetch=") != std::string::npos &&
               first.diag.find("llm=") != std::string::npos,
           "新生成结果带分阶段耗时 diag", first.diag);

        const fsx::path cacheFile = summary_cache_path(baseDir, eid, "zh");
        ck(fsx::exists(cacheFile, ec), "摘要已落盘 " + meta_string_of(cacheFile));
        std::printf("     首次生成耗时 %lld ms（mock 无网络，线上实测为数十秒）\n", firstMs);

        // 第二次：换一种写法（大小写 + 首尾空格），模拟"单曲页的艺术家 chip"与"艺术家页"的差异
        const int chatBefore = mock.chat, searchBefore = mock.search, fetchBefore = mock.fetch;
        const auto t1 = std::chrono::steady_clock::now();
        const SummaryResult second = retrieval_fetch(EntityKind::Person, personPadded, "zh", ro);
        const long long hitMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t1).count();

        ck(second.ok && second.from_cache, "第二次命中磁盘缓存（from_cache=1）");
        ck(mock.chat == chatBefore, "第二次**没有**发起 LLM 调用（chat 计数不变）",
           "chat=" + std::to_string(mock.chat));
        ck(mock.search == searchBefore && mock.fetch == fetchBefore,
           "第二次也没有发起 search/fetch 调用");
        ck(second.sections.size() == first.sections.size() &&
               second.citations.size() == first.citations.size(),
           "缓存内容与首次一致（同一份数据）");
        std::printf("     第二次命中耗时 %lld ms（判据 ≤300ms）\n", hitMs);
        ck(hitMs <= 300, "第二次进入 ≤300ms（实测 " + std::to_string(hitMs) + "ms）");

        // 缓存键必须是"归一化后同一个 id"：这就是共用的根据
        const std::string eidPadded = entity_id(EntityKind::Person, personPadded);
        ck(eidPadded == eid, "两种写法归一到同一个 person 实体 id", eid + " vs " + eidPadded);
        ck(fsx::exists(summary_cache_path(baseDir, eidPadded, "zh"), ec),
           "两种写法指向同一个缓存文件（单曲页 / 艺术家页共用一份）");

        // 而"歌曲"页签的 track 摘要按 (artist,title) 归键 —— 本来就是另一个实体，
        // 这里显式记下来，避免以后有人把它当成"没共用"的 bug 去错修。
        const std::string eidTrack = entity_make_id(
            "track", entity_norm(person) + "\x1f" + entity_norm("Winged Step"));
        ck(eidTrack != eid, "track 级摘要与 person 级摘要是两个实体（设计如此，不是 bug）",
           "track=" + eidTrack + " person=" + eid);
    }

    // ---------------- 3) 缓存过期后必须重新生成（TTL 不是永不过期） ----------------
    std::printf("-- 3. 过期缓存不得冒充命中 --\n");
    {
        SummaryResult old;
        old.ok = true;
        old.generated_at = now_sec() - 30 * 86400;
        old.expires_at = old.generated_at + 7 * 86400;   // 早已过期
        ck(summary_is_expired(old), "30 天前的摘要判定为过期（TTL 7 天）");
        ck(summary_save_cache(baseDir, eid, "zh", old), "写入过期缓存");

        MockLlm mock;
        mock.materialUrl = "https://zh.wikipedia.org/wiki/Ivoris";
        mock.hitUrl = "";                 // 本节关闭检索：mock 只引用权威材料
        LlmConfig cfg;
        cfg.apiKey = "test-key";
        cfg.usagePath = meta_string_of(base / "llm-usage.json");
        cfg.transport = [&mock](const std::string& u, const std::string& b,
                                const std::vector<std::string>& h) { return mock(u, b, h); };
        RetrievalOptions ro;
        ro.metabase = baseDir;
        ro.enable_search = false;
        ro.llm_cfg = cfg;
        const SummaryResult again = retrieval_fetch(EntityKind::Person, person, "zh", ro);
        ck(again.ok && !again.from_cache && mock.chat == 1,
           "过期缓存触发一次重新生成（chat=1，from_cache=0）",
           "ok=" + std::string(again.ok ? "1" : "0") + " cached=" +
               std::string(again.from_cache ? "1" : "0") + " chat=" + std::to_string(mock.chat) +
               " err=" + again.error);
    }

    // ---------------- 4) 无源可查时必须快速返回（并行改造不得卡住） ----------------
    std::printf("-- 4. 两个源都关闭时 knowledge_get 不得卡住 --\n");
    {
        KnowledgeOpts kopts;
        kopts.metabase = baseDir;
        kopts.include_wikipedia = false;
        kopts.include_moegirl = false;
        const auto t0 = std::chrono::steady_clock::now();
        const KnowledgeResult kr = knowledge_fetch(EntityKind::Person, "NobodyKnowsThisName-xyz", kopts);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ck(kr.materials.empty(), "无源时材料为空");
        ck(!kr.error.empty(), "无源时给出明确错误文案", kr.error);
        ck(ms < 800, "无源路径不发起网络、立即返回（实测 " + std::to_string(ms) + "ms）");
    }

    fsx::remove_all(base, ec);
    std::printf("\n%s（失败 %d 项）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
