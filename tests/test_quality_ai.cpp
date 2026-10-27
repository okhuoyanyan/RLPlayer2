// test_quality_ai.cpp — 音频体检 B4：AI 解释层（离线，可注入 transport，绝不联网）
// 判据：
//   1) 无 key ⟹ 明确降级，确定性解释照常返回；
//   2) 请求体只含确定性指标、不含 key；key 只在 Authorization 头；
//   3) 服务端把 key 回显进错误时，返回文本里必须是 ****（脱敏）；
//   4) 同输入 ⟹ 请求体逐字节相同（确定性）。
#include "analysis/quality_ai.h"
// 261027-06（用户 P1/P2）：模型列表解析与月度配额并发记账都在 meta/llm.h 里，
// 这里只测**纯函数 + 文件记账**，绝不发 HTTP（llm_models_fetch 的解析与请求是分开的）。
#include "meta/llm.h"
// 2026-10-27（用户 261027-06）：P1 分类提示词 / P2 不过滤解析 / P3 名词外链 / P4 单曲赏析。
// 这些全是纯函数（prompt_profiles.h 只依赖 net/json.h），所以能离线逐条断言。
// retrieval.h 只用到 summary_from_profile_doc / summary_to_json 这类纯函数（不发 HTTP）。
#include "meta/retrieval.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace rta::analysis;
namespace fsx = std::filesystem;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// 手工造一个"已评分"的对比结果（不依赖真实文件，专测提示词/降级/脱敏）
static VersionCompare make_fake_report() {
    VersionCompare r;
    r.ok = true;
    r.sameTrack = true;
    r.sameTrackCorr = 0.991;
    r.windowSec = 180.0;
    r.versions.resize(2);
    for (int i = 0; i < 2; ++i) {
        VersionData& v = r.versions[i];
        v.label = (i == 0) ? "V1" : "V2";
        v.path = (i == 0) ? "a.flac" : "b.flac";
        v.fs = 44100; v.channels = 2; v.bits = 16; v.durationSec = 180.0;
        v.lagSec = (i == 0) ? 0.0 : 0.12;
        v.analyzed = true;
    }
    r.versions[0].quality.snrDb = 92.4;
    r.versions[0].quality.noiseFloorDbfs = -95.1;
    r.versions[0].quality.nonTonalDb = -31.2;
    r.versions[0].quality.crestDb = 14.1;
    r.versions[0].quality.clippedPct = 0.0;
    r.versions[0].quality.peakDbfs = -0.8;
    r.versions[0].quality.stereo = true;
    r.versions[0].quality.monoLossDb = 0.4;
    r.versions[0].dynamicsTimes.drDb = 13.2;
    r.versions[0].dynamicsTimes.lra = 8.1;
    r.versions[0].dynamicsTimes.lufsIntegrated = -12.9;

    r.versions[1].quality.snrDb = 61.0;
    r.versions[1].quality.noiseFloorDbfs = -64.0;
    r.versions[1].quality.nonTonalDb = -18.7;
    r.versions[1].quality.crestDb = 9.2;
    r.versions[1].quality.clippedPct = 0.31;
    r.versions[1].quality.clippedRuns = 42;
    r.versions[1].quality.peakDbfs = -0.1;
    r.versions[1].quality.stereo = true;
    r.versions[1].quality.monoLossDb = 1.9;
    r.versions[1].dynamicsTimes.drDb = 7.4;
    r.versions[1].dynamicsTimes.lra = 4.0;
    r.versions[1].dynamicsTimes.lufsIntegrated = -9.6;
    score_versions(r);
    return r;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    fsx::path dir = fsx::temp_directory_path() / "rta_qai";
    std::error_code ec;
    fsx::create_directories(dir, ec);

    VersionCompare r = make_fake_report();

    printf("=== 1. 无 key：必须明确降级，指标与确定性解释照常可用 ===\n");
    {
        AiExplainOptions opt;
        opt.keyFile = (dir / "no_such_key.txt").string();
        opt.confPath = (dir / "no_such_conf.conf").string();
        AiExplain e = explain_versions(r, opt);
        printf("  ok=%d error=%s\n", (int)e.ok, e.error.c_str());
        check(!e.ok, "无 key ⟹ ok=false（不假装有解释）");
        check(e.error.find("未配置 API key") != std::string::npos, "错误原因是「未配置 API key」",
              e.error);
        check(!r.explanation.empty() && r.explanation.find("综合排序") != std::string::npos,
              "确定性解释仍然可用（降级路径有内容可显示）");
        const std::string fb = explain_or_fallback(r, opt);
        check(fb == r.explanation, "explain_or_fallback 回落到确定性解释");
    }

    printf("\n=== 2. 有 key：请求体只带指标，key 只在 Authorization 头 ===\n");
    const std::string fakeKey = "sk-test-0123456789abcdef0123456789abcdef";
    std::string capturedUrl, capturedBody;
    std::vector<std::string> capturedHeaders;
    {
        // 写一个临时 KEY.txt（内容与真实 key 无关，仅用于测试解析路径）
        const std::string kf = (dir / "KEY.txt").string();
        { std::ofstream f(kf, std::ios::binary | std::ios::trunc); f << fakeKey << "\n"; }
        AiExplainOptions opt;
        opt.keyFile = kf;
        opt.confPath = (dir / "no_such_conf.conf").string();
        opt.transport = [&](const std::string& url, const std::string& body,
                            const std::vector<std::string>& headers) {
            capturedUrl = url; capturedBody = body; capturedHeaders = headers;
            rta::net::HttpResponse resp;
            resp.status = 200;
            const std::string json =
                "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"V1 明显优于 V2：动态范围高 5.8 dB，"
                "削波从 0.31% 降到 0，信噪比高 31.4 dB。V2 的高频与噪声地板都更差，像低码率转制。\"},"
                "\"finish_reason\":\"stop\"}],\"usage\":{\"total_tokens\":321}}";
            resp.body.assign(json.begin(), json.end());
            return resp;
        };
        AiExplain e = explain_versions(r, opt);
        printf("  ok=%d source=%s model=%s tokens=%d\n", (int)e.ok, e.keySource.c_str(),
               e.model.c_str(), e.totalTokens);
        printf("  ai: %s\n", e.text.c_str());
        check(e.ok && e.usedAi, "注入 transport 后成功（离线可测）", e.error);
        check(e.keySource == "KEY.txt", "key 来源标为 KEY.txt", e.keySource);
        check(e.text.find("V1 明显优于") != std::string::npos, "解析出模型回复");
        check(capturedUrl.find("/chat/completions") != std::string::npos, "请求 URL 正确", capturedUrl);

        bool authHasKey = false, bodyHasKey = false;
        for (const auto& h : capturedHeaders)
            if (h == "Authorization: Bearer " + fakeKey) authHasKey = true;
        if (capturedBody.find(fakeKey) != std::string::npos) bodyHasKey = true;
        check(authHasKey, "key 在 Authorization 头里");
        check(!bodyHasKey, "请求体里【没有】key");
        check(capturedBody.find("非谐波残留") != std::string::npos
              && capturedBody.find("13.200") != std::string::npos,
              "请求体带确定性指标（指标名 + 数值）");
        check(capturedBody.find("\"role\":\"system\"") != std::string::npos
              && capturedBody.find("不得编造") != std::string::npos,
              "system 提示明确禁止编造");

    }

    printf("\n=== 3. 脱敏：服务端把 key 回显进错误 ⟹ 返回里必须是 **** ===\n");
    {
        const std::string kf = (dir / "KEY.txt").string();
        AiExplainOptions opt;
        opt.keyFile = kf;
        opt.confPath = (dir / "no_such_conf.conf").string();
        opt.transport = [&](const std::string&, const std::string&,
                            const std::vector<std::string>&) {
            rta::net::HttpResponse resp;
            resp.status = 401;
            const std::string json = "{\"error\":{\"message\":\"Invalid key: " + fakeKey + "\"}}";
            resp.body.assign(json.begin(), json.end());
            return resp;
        };
        AiExplain e = explain_versions(r, opt);
        printf("  error=%s\n", e.error.c_str());
        check(!e.ok, "HTTP 401 ⟹ ok=false");
        check(e.error.find(fakeKey) == std::string::npos, "错误文本里没有明文 key");
        check(e.error.find("****") != std::string::npos, "错误文本里 key 被替换成 ****");
    }

    printf("\n=== 4. 网络失败（无法连接）也必须降级而不是崩 ===\n");
    {
        const std::string kf = (dir / "KEY.txt").string();
        AiExplainOptions opt;
        opt.keyFile = kf;
        opt.confPath = (dir / "no_such_conf.conf").string();
        opt.transport = [&](const std::string&, const std::string&,
                            const std::vector<std::string>&) {
            rta::net::HttpResponse resp;   // status=0 ⟹ 传输层失败
            resp.status = 0;
            resp.error = "connection refused";
            return resp;
        };
        AiExplain e = explain_versions(r, opt);
        check(!e.ok && e.error.find("AI 解释失败") != std::string::npos,
              "传输失败 ⟹ 明确报 AI 解释失败并保留原因", e.error);
        check(explain_or_fallback(r, opt) == r.explanation, "仍回落确定性解释");
    }

    printf("\n=== 5. 确定性：同输入 ⟹ 请求体逐字节相同 ===\n");
    {
        const std::string kf = (dir / "KEY.txt").string();
        AiExplainOptions opt;
        opt.keyFile = kf;
        opt.confPath = (dir / "no_such_conf.conf").string();
        std::string b1, b2;
        opt.transport = [&](const std::string&, const std::string& body,
                            const std::vector<std::string>&) {
            b1 = body;
            rta::net::HttpResponse resp; resp.status = 200;
            const std::string json = "{\"choices\":[{\"message\":{\"content\":\"ok\"}}],\"usage\":{\"total_tokens\":1}}";
            resp.body.assign(json.begin(), json.end());
            return resp;
        };
        explain_versions(r, opt);
        opt.transport = [&](const std::string&, const std::string& body,
                            const std::vector<std::string>&) {
            b2 = body;
            rta::net::HttpResponse resp; resp.status = 200;
            const std::string json = "{\"choices\":[{\"message\":{\"content\":\"ok\"}}],\"usage\":{\"total_tokens\":1}}";
            resp.body.assign(json.begin(), json.end());
            return resp;
        };
        explain_versions(r, opt);
        check(!b1.empty() && b1 == b2, "两次请求体逐字节相同（含指标表与解释）");
        check(b1.find("综合排序") != std::string::npos, "请求体包含综合排序结论");
    }

    printf("\n=== 6. 未评分的结果必须拒绝（防止 AI 对着空数据编话） ===\n");
    {
        VersionCompare raw = r;
        raw.ranking.clear();
        raw.scores.clear();
        AiExplainOptions opt;
        opt.keyFile = (dir / "KEY.txt").string();
        opt.confPath = (dir / "no_such_conf.conf").string();
        AiExplain e = explain_versions(raw, opt);
        check(!e.ok && e.error.find("尚未评分") != std::string::npos, "未评分 ⟹ 拒绝生成解释",
              e.error);
    }

    // =====================================================================
    // 261027-06（用户 P1/P2）新增：模型列表解析 + 配额并发原子性 + 默认模型不变
    // 全部离线（铁律 3：解析是纯函数；配额只碰临时文件，不联网、不开音频设备）
    // =====================================================================
    printf("\n=== 7. 模型列表解析（GET {base}/models，OpenAI 兼容） ===\n");
    {
        namespace meta = rta::meta;
        // ① DeepSeek / OpenAI 标准形态：data[] + 每项 id/owned_by/created
        const std::string openai =
            "{\"object\":\"list\",\"data\":["
            "{\"id\":\"deepseek-chat\",\"object\":\"model\",\"owned_by\":\"deepseek\",\"created\":1700000000},"
            "{\"id\":\"deepseek-reasoner\",\"object\":\"model\",\"owned_by\":\"deepseek\"},"
            "{\"id\":\"deepseek-flash\",\"object\":\"model\"}]}";
        meta::LlmModelList a = meta::llm_parse_models_response(openai);
        check(a.ok, "标准 data[] 形态 ⟹ ok");
        check(a.count() == 3, "解析出 3 个模型", std::to_string(a.count()));
        check(a.models[0].id == "deepseek-chat" && a.models[2].id == "deepseek-flash",
              "顺序与报文一致（界面下拉不重排）");
        check(a.models[0].ownedBy == "deepseek" && a.models[0].created == 1700000000,
              "owned_by / created 一并解析");
        check(a.has("deepseek-reasoner") && !a.has("gpt-4o"),
              "has() 判据正确（界面据此提示「当前值不在列表里」）");

        // ② 网关变体 A：顶层键叫 models
        meta::LlmModelList b = meta::llm_parse_models_response(
            "{\"models\":[{\"id\":\"m1\"},{\"name\":\"m2\"}]}");
        check(b.ok && b.count() == 2 && b.models[1].id == "m2",
              "models[] 变体 + name 字段兜底", std::to_string(b.count()));

        // ③ 网关变体 B：纯字符串数组（有的代理只回 ["a","b"]）
        meta::LlmModelList c = meta::llm_parse_models_response("{\"data\":[\"x\",\"y\",\"x\"]}");
        check(c.ok && c.count() == 2, "字符串数组 + 去重", std::to_string(c.count()));

        // ④ 空列表是「合法成功」，不是错误（界面据此说"没有可用模型，可手填"）
        meta::LlmModelList d = meta::llm_parse_models_response("{\"object\":\"list\",\"data\":[]}");
        check(d.ok && d.count() == 0, "空列表 ⟹ ok 且 0 个");

        // ⑤ 服务端 error 对象
        meta::LlmModelList e = meta::llm_parse_models_response(
            "{\"error\":{\"message\":\"invalid api key\",\"type\":\"auth\"}}");
        check(!e.ok && e.error.find("invalid api key") != std::string::npos,
              "error.message 原样带回（界面显示原因）", e.error);

        // ⑥ 半截/非 JSON（断网代理常回 HTML）：必须 ok=false 而不是抛/崩
        meta::LlmModelList f = meta::llm_parse_models_response("<html>502 Bad Gateway</html>");
        check(!f.ok, "非 JSON ⟹ ok=false（优雅降级，绝不让设置页崩）", f.error);

        // ⑦ 没有 key 时 fetch 直接失败（不发请求）
        meta::LlmConfig nokey;
        meta::LlmModelList g = meta::llm_models_fetch(nokey, 1000, true, nullptr);
        check(!g.ok && g.error.find("api_key") != std::string::npos,
              "无 key ⟹ 明确报未配置（不发起 HTTP）", g.error);
    }

    printf("\n=== 8. 默认模型保持不变（P1：不强制改用户现有行为） ===\n");
    {
        namespace meta = rta::meta;
        meta::LlmConfig c0;
        check(c0.model == "deepseek-flash", "默认 model 仍是 deepseek-flash", c0.model);
        const std::string body = meta::llm_build_chat_body(
            "my-custom-model", { meta::LlmMessage{"user", "hi"} }, 16, 0.2f, false);
        check(body.find("\"model\":\"my-custom-model\"") != std::string::npos,
              "选中的模型真的进了请求体");
        const std::string empty = meta::llm_build_chat_body(
            std::vector<meta::LlmMessage>{ meta::LlmMessage{"user", "hi"} }, 16, 0.2f, false);
        check(empty.find("\"model\"") == std::string::npos,
              "不指定模型时不写 model 字段（旧行为）");
    }

    printf("\n=== 9. 月度配额并发原子性：10 线程各记一次 ⟹ 恰好 +10 ===\n");
    {
        namespace meta = rta::meta;
        const std::string usagePath = (dir / "llm-usage-concurrent.json").string();
        std::error_code ec2;
        fsx::remove(usagePath, ec2);
        fsx::remove(usagePath + ".bak", ec2);
        const std::string month = "202611";
        const int before = meta::llm_get_monthly_count(month, usagePath);
        check(before == 0, "起始计数为 0", std::to_string(before));

        std::vector<std::thread> th;
        for (int i = 0; i < 10; ++i) {
            th.emplace_back([&, i]() {
                meta::llm_record_usage(month, (i % 2 == 0) ? "chat" : "search", usagePath);
            });
        }
        for (auto& t : th) t.join();
        const meta::LlmMonthlyUsage u = meta::llm_get_usage(month, usagePath);
        check(u.total == 10, "并发 10 次 ⟹ total 恰好 10（不丢更新、不重复计）",
              std::to_string(u.total));
        check(u.chat + u.search == 10 && u.fetch == 0,
              "分桶合计等于 total（chat+search=10, fetch=0）",
              "chat=" + std::to_string(u.chat) + " search=" + std::to_string(u.search) +
              " fetch=" + std::to_string(u.fetch));
        // 账本必须仍是合法 JSON（并发写的第二类事故：半写文件）
        std::string text;
        {
            std::ifstream f(usagePath, std::ios::binary);
            text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        check(meta::llm_usage_json_complete(text), "并发写后台账仍是完整 JSON（无半写）",
              "bytes=" + std::to_string(text.size()));

        // 配额上限判据：满 10 后 check_cap 必须拒绝（且报出 10/10）
        meta::LlmConfig cap;
        cap.usagePath = usagePath;
        cap.monthlyCap = 10;
        cap.monthOverride = month;
        std::string capErr;
        check(!meta::llm_check_cap(cap, month, &capErr),
              "已达上限 ⟹ check_cap=false", capErr);
        cap.monthlyCap = 11;
        check(meta::llm_check_cap(cap, month, &capErr), "上限 +1 ⟹ 放行");

        // 第二个账本互不影响（usagePath 隔离）
        const std::string other = (dir / "llm-usage-other.json").string();
        fsx::remove(other, ec2);
        meta::llm_record_usage(month, "fetch", other);
        check(meta::llm_get_monthly_count(month, other) == 1 &&
              meta::llm_get_monthly_count(month, usagePath) == 10,
              "不同账本互不串账");
    }


    // =====================================================================
    // 2026-10-27（用户 261027-06）：AI 内容大修 —— 提示词策略与输出内容
    //   P1 两步走（先分类再按类定制提示词）/ P2 输出不过滤 / P3 名词超链接 / P4 单曲赏析
    // 全部离线纯函数（铁律 3：不开音频设备、不联网、不调真实 LLM）
    // =====================================================================
    printf("\n=== 10. P1 分类器：12 张不同风格专辑 → profile ===\n");
    {
        struct Case {
            const char* tag;
            const char* title;
            const char* artist;
            const char* genre;
            const char* material;
            rta::meta::MusicProfile want;
        };
        const Case cases[] = {
            {"古典", "Beethoven: Symphony No.5", "Herbert von Karajan / Berliner Philharmoniker", "古典",
             "流派与风格: Classical, Romantic\n指挥: Herbert von Karajan\n乐团: Berliner Philharmoniker\n"
             "厂牌: Deutsche Grammophon\n录音师: Gunter Hermanns\n版本对比: 卡拉扬 1963 vs 1977",
             rta::meta::MusicProfile::Classical},
            {"同人", "東方幻奏響 ～ Touhou Orchestral", "上海アリス幻樂団", "同人",
             "サークル: 上海アリス幻樂団\n会場: 例大祭\n原曲: U.N.オーエンは彼女なのか？\n特典: 譜面",
             rta::meta::MusicProfile::Doujin},
            {"流行", "叶惠美", "周杰伦", "华语流行",
             "发行时间: 2003-07-31\n厂牌: 阿尔发音乐\n榜单: 华语榜冠军\n语种: 国语",
             rta::meta::MusicProfile::Pop},
            {"爵士", "Kind of Blue", "Miles Davis", "Jazz",
             "厂牌: Columbia\n录音室: 1959-03-02\n乐手: John Coltrane (tenor sax)\n即兴: modal",
             rta::meta::MusicProfile::Jazz},
            {"原声", "明日方舟 原声带 Vol.2", "塞壬唱片-MSR", "",
             "Original Soundtrack / 游戏音乐\n发行商: Monster Siren Records\n配乐: 主题曲",
             rta::meta::MusicProfile::Soundtrack},
            {"金属", "Master of Puppets", "Metallica", "Thrash Metal",
             "厂牌: Elektra\n子流派: thrash metal\n成员: James Hetfield (guitar/vocal)",
             rta::meta::MusicProfile::Metal},
            {"摇滚", "OK Computer", "Radiohead", "Alternative Rock",
             "厂牌: Parlophone\n制作人: Nigel Godrich\n录音室: Abbey Road Studios",
             rta::meta::MusicProfile::Rock},
            {"民乐", "二胡名曲选", "闵惠芬", "民乐",
             "乐器: 二胡\n地域: 江南\n录音方式: 现场",
             rta::meta::MusicProfile::Folk},
            {"电子", "Discovery", "Daft Punk", "Electronic",
             "厂牌: Virgin\n风格: house / synthwave\n制作: 采样与合成器",
             rta::meta::MusicProfile::Electronic},
            {"虚拟歌手", "ミクの日 39", "初音ミク", "VOCALOID",
             "音源: Hatsune Miku V4X\nP主: 各种\n投稿: niconico",
             rta::meta::MusicProfile::Vocal},
            {"嘻哈", "Metaphorical Music", "Nujabes", "Hip-Hop",
             "厂牌: Hydeout\n制作: Nujabes\n风格: boom bap / 采样",
             rta::meta::MusicProfile::HipHop},
            {"氛围", "Ambient Works", "Brian Eno", "Ambient",
             "厂牌: Editions EG\n风格: ambient / drone\n录音: field recording",
             rta::meta::MusicProfile::Ambient},
        };
        int hit_count = 0;
        const int total = (int)(sizeof(cases) / sizeof(cases[0]));
        for (int i = 0; i < total; ++i) {
            const Case& c = cases[i];
            rta::meta::ProfileSignal s;
            s.title = c.title;
            s.artist = c.artist;
            if (c.genre && *c.genre) s.genres.push_back(c.genre);
            s.material_text = c.material;
            const rta::meta::ProfileDecision d = rta::meta::classify_profile(s);
            const bool okp = (d.profile == c.want);
            if (okp) ++hit_count;
            printf("  %-9s %-10s -> %-11s score=%-4d %s\n", c.tag, c.title,
                   rta::meta::music_profile_id(d.profile), d.score, okp ? "OK" : "MISMATCH");
            check(okp, std::string("分类正确：") + c.tag + " → " + rta::meta::music_profile_id(c.want),
                  std::string("得到 ") + rta::meta::music_profile_id(d.profile) + " / " + d.reason);
        }
        {
            rta::meta::ProfileSignal s;   // 完全无线索 → 必须 Generic，不许硬猜
            s.title = "Untitled";
            const rta::meta::ProfileDecision d = rta::meta::classify_profile(s);
            check(d.profile == rta::meta::MusicProfile::Generic, "无线索 ⟹ generic（不硬猜）",
                  rta::meta::music_profile_id(d.profile));
            rta::meta::ProfileSignal s2;
            s2.title = "X";
            s2.hint = "古典";
            check(rta::meta::classify_profile(s2).profile == rta::meta::MusicProfile::Classical,
                  "hint 直接定档（genre 标签写死时走这条路）");
        }
        printf("  分类命中 %d/%d\n", hit_count, total);
        check(hit_count == total, "12 类全部落到对应 profile");

        // 生产现实：rtaweb 现在**不传**本地流派标签（它只传 artist/entity_id），
        // 所以再跑一遍「把 genres 清空、只留标题+艺术家+材料正文」——分类器必须仍然分对，
        // 否则这套策略在真机上根本用不上。
        int hit_nogenre = 0;
        for (int i = 0; i < total; ++i) {
            const Case& c = cases[i];
            rta::meta::ProfileSignal s;
            s.title = c.title;
            s.artist = c.artist;
            s.material_text = c.material;   // 没有 genres
            const rta::meta::ProfileDecision d = rta::meta::classify_profile(s);
            if (d.profile == c.want) ++hit_nogenre;
            else printf("  [no-genre] %-9s -> %s (want %s)\n", c.tag,
                        rta::meta::music_profile_id(d.profile), rta::meta::music_profile_id(c.want));
        }
        printf("  无流派标签时命中 %d/%d\n", hit_nogenre, total);
        check(hit_nogenre >= total - 2, "清空流派标签后仍分类正确（>= 10/12，实测 " +
              std::to_string(hit_nogenre) + "/12）");
    }

    printf("\n=== 11. P1 每个 profile 的提示词模板含本类必查字段 ===\n");
    {
        struct Want { rta::meta::MusicProfile p; const char* tok[4]; };
        const Want wants[] = {
            {rta::meta::MusicProfile::Classical,  {"指挥", "乐团", "录音师", "版本对比"}},
            {rta::meta::MusicProfile::Doujin,     {"社团", "原曲出处", "售价", "特典"}},
            {rta::meta::MusicProfile::Pop,        {"制作人", "榜单", "语种", "总时长"}},
            {rta::meta::MusicProfile::Rock,       {"成员编制", "录音室", "混音", "母带"}},
            {rta::meta::MusicProfile::Metal,      {"子流派", "失真", "成员", "专辑名称"}},
            {rta::meta::MusicProfile::Jazz,       {"乐手", "录音日期", "目录号", "即兴"}},
            {rta::meta::MusicProfile::Folk,       {"乐器编制", "地域", "民歌", "录音方式"}},
            {rta::meta::MusicProfile::Electronic, {"BPM", "制作设备", "混音版本", "目录号"}},
            {rta::meta::MusicProfile::Soundtrack, {"作品归属", "主题曲", "OST", "曲目列表"}},
            {rta::meta::MusicProfile::Vocal,      {"音源", "P主", "投稿日", "播放量"}},
            {rta::meta::MusicProfile::HipHop,     {"Beatmaker", "客串", "采样来源", "flow"}},
            {rta::meta::MusicProfile::Ambient,    {"创作理念", "录音环境", "乐器", "声音空间"}},
            {rta::meta::MusicProfile::World,      {"地域", "乐器", "演奏传统", "专辑名称"}},
            {rta::meta::MusicProfile::Generic,    {"制作人员", "曲目列表", "碟数", "背景"}},
        };
        int ok_n = 0;
        for (const auto& w : wants) {
            rta::meta::ProfileDecision d;
            d.profile = w.p;
            d.reason = "unit";
            const rta::meta::ProfilePrompt pr =
                rta::meta::build_profile_prompt(d, "样本实体", "专辑", "[M1] 示例材料", {});
            std::string miss;
            for (const char* t : w.tok) {
                if (pr.system.find(t) == std::string::npos) miss += std::string(t) + " ";
            }
            const bool okp = miss.empty();
            if (okp) ++ok_n;
            printf("  %-11s %-4s 必查=%zu 字节  %s\n", rta::meta::music_profile_id(w.p), okp ? "OK" : "缺",
                   std::string(rta::meta::profile_spec(w.p).fields).size(),
                   okp ? "" : ("缺: " + miss).c_str());
            check(okp, std::string("prompt 含本类必查字段：") + rta::meta::music_profile_id(w.p), miss);
        }
        check(ok_n == (int)(sizeof(wants) / sizeof(wants[0])), "14 个 profile 的模板都带本类字段");

        // 通用纪律：不过滤 / 能确定的都写 / 保留分节 / 标签自由 / 值可多行
        rta::meta::ProfileDecision dg;
        dg.profile = rta::meta::MusicProfile::Classical;
        const rta::meta::ProfilePrompt pr = rta::meta::build_profile_prompt(dg, "样本", "专辑", "[M1] 材料", {});
        check(pr.system.find("不过滤") != std::string::npos, "system 含「不过滤」第一原则");
        check(pr.system.find("每一条信息都必须输出") != std::string::npos, "system 要求「能确证的每一条都输出」");
        check(pr.system.find("只有**不确定**的才不写") != std::string::npos, "system 要求「不确定的不写」（用户原话）");
        check(pr.system.find("📀 专辑基本信息") != std::string::npos &&
              pr.system.find("💿 补充背景与解读") != std::string::npos,
              "system 保留模型自己的分节结构（📀/🎵/💿）");
        check(pr.system.find("键名**自由**") != std::string::npos, "system 允许自由标签（不压成固定 schema）");
        check(pr.user.find("[M1]") != std::string::npos, "user 带材料编号 [M1]");
        printf("\n  ---- 古典模板 system 片段 ----\n%.420s\n", pr.system.c_str());

        // 单曲：必须要求「💿 补充背景与解读」
        const rta::meta::ProfilePrompt tr = rta::meta::build_profile_prompt(dg, "某曲", "歌曲", "[M1] 材料", {});
        check(tr.system.find("补充背景与解读") != std::string::npos, "单曲模板强制要求「补充背景与解读」");
        check(tr.system.find("曲名意象") != std::string::npos &&
              tr.system.find("编曲风格") != std::string::npos,
              "单曲模板点名「曲名意象 / 编曲风格」");
    }

    printf("\n=== 12. P2 不过滤：长输出（Static World 式）逐条进渲染 ===\n");
    {
        static const char* kRich = R"json({
  "disclaimer": "AI 整理，仅供参考",
  "fields": {
    "专辑名称": "Static World",
    "译名": "静止世界",
    "制作方": "Winged Step",
    "首发日期": "2024-01-09",
    "专辑类型": "EP（数字）",
    "分级": "全年龄",
    "碟数": "1",
    "音轨数": "6",
    "总时长": "24:13",
    "售价": "¥1,100（会场）/ ¥1,320（通贩含税）",
    "通贩渠道": "dizzylab / Booth",
    "社团": "Winged Step",
    "原曲出处": "东方妖妖梦 / 东方红魔乡",
    "会场": "C103",
    "特典": "谱面册（8P）",
    "编曲": "Iris / Nago",
    "母带": "Studio A",
    "曲目列表": "01 | Static World | 4:12 | Iris | 東方妖々夢\n02 | Frozen Light | 3:48 | Nago | 東方紅魔郷"
  },
  "sections": [
    {"heading": "📀 专辑基本信息", "paragraphs": ["Winged Step 在 C103 发布的第一张自主发行 EP。"]},
    {"heading": "🎵 曲目列表", "paragraphs": ["全 6 轨，其中 2 轨为纯器乐。"]},
    {"heading": "💿 补充背景与解读", "paragraphs": [
      "标题『Static World』指向静止而失真的世界，编曲刻意保留了磁带底噪。",
      "编曲以 90 年代 eurobeat 骨架叠加东方原曲旋律，是全篇最鲜明的特征。"]}
  ],
  "citations": [
    {"id": "M1", "title": "dizzylab 专辑页", "url": "https://dizzylab.net/x"},
    {"id": "M2", "title": "乱编的假链接", "url": "https://evil.example/x"}
  ]
})json";
        const std::vector<std::string> allowed = { "https://dizzylab.net/x" };
        const rta::meta::ProfileDoc doc = rta::meta::parse_profile_reply(kRich, allowed, true);
        check(doc.ok, "宽松解析成功", doc.error);
        check(doc.fields.size() == 18, "18 条标签全部保留（旧 schema 只会留 5~6 条）",
              std::to_string(doc.fields.size()));
        check(doc.sections.size() == 3, "3 个分节全部保留", std::to_string(doc.sections.size()));
        check(doc.citations.size() == 1 && doc.dropped_citations == 1,
              "只丢那条假引用，其余内容不受牵连",
              "kept=" + std::to_string(doc.citations.size()) + " dropped=" + std::to_string(doc.dropped_citations));

        rta::meta::ProfileDecision dd;
        dd.profile = rta::meta::MusicProfile::Doujin;
        const rta::meta::SummaryResult sr = rta::meta::summary_from_profile_doc(doc, dd);
        check(sr.ok, "转 SummaryResult 成功", sr.error);
        // 分节必须也能被当前前端渲染（renderSummaryContent 只在 fields 为空时才画 sections）
        // 18 条标签 + 3 分节镜像 − 1 条去重（模型已经把「曲目列表」当标签写了，
        // 镜像时归一化到同一个键名 ⟹ 不重复；前端在本地有曲目时也会 drop 掉这个键）
        check(sr.fields.size() == 20, "fields 20 条 = 18 标签 + 3 分节镜像 − 1 曲目列表去重",
              std::to_string(sr.fields.size()));
        bool all_in = true;
        std::string missing;
        for (const auto& kv : doc.fields) {
            bool found = false;
            for (const auto& kv2 : sr.fields) if (kv2.first == kv.first && kv2.second == kv.second) { found = true; break; }
            if (!found) { all_in = false; missing += kv.first + " "; }
        }
        check(all_in, "桩数据 18 条逐条进入渲染负载", missing);
        check(sr.profile == "doujin" && sr.profile_label == "同人", "结果带分类 id/标签",
              sr.profile + "/" + sr.profile_label);
        printf("  条目数：doc.items=%zu  summary.fields=%zu  sections=%zu  paragraphs=%zu\n",
               doc.item_count(), sr.fields.size(), sr.sections.size(),
               sr.sections.empty() ? (size_t)0 : sr.sections[2].paragraphs.size());

        // 缓存往返：分类与外链不能丢
        const std::string js = rta::meta::summary_to_json(sr);
        rta::meta::SummaryResult back;
        std::string jsErr;
        check(rta::meta::summary_from_json(js, back, &jsErr), "缓存往返可读", jsErr);
        check(back.fields.size() == sr.fields.size() && back.sections.size() == sr.sections.size(),
              "缓存往返：fields/sections 条数不变",
              std::to_string(back.fields.size()) + "/" + std::to_string(sr.fields.size()));
        check(back.profile == sr.profile && back.profile_label == sr.profile_label, "缓存往返：分类不丢");

        // 占位值丢弃（「未知」不占位），以及 JSON 坏掉时的按行兜底
        const rta::meta::ProfileDoc d2 = rta::meta::parse_profile_reply(
            "{\"fields\":{\"未知\":\"未知\",\"风格\":\"同人电子\"},"
            "\"sections\":[{\"heading\":\"简介\",\"paragraphs\":[\"有内容\"]}]}");
        check(d2.ok && d2.fields.size() == 1 && d2.dropped_placeholders >= 1,
              "「未知」占位被丢弃但仍保留真实标签", std::to_string(d2.fields.size()));
        const rta::meta::ProfileDoc d3 = rta::meta::parse_profile_reply(
            "📀 基本信息\n制作方：Winged Step\n\n💿 补充背景与解读\n标题意象是静止的世界。");
        check(d3.ok && d3.from_prose && d3.sections.size() == 2,
              "JSON 坏掉 ⟹ 按行兜底成 2 个分节（内容不丢）",
              "sections=" + std::to_string(d3.sections.size()));
    }

    printf("\n=== 13. P3 名词 → 权威链接（确定性匹配，20+ 探针） ===\n");
    {
        rta::meta::LinkIndex idx;
        idx.add_seed_table();
        const char* names[] = {
            "坂井泉水", "ZARD", "久石让", "坂本龙一", "初音未来", "卡拉扬",
            "柏林爱乐乐团", "维也纳爱乐乐团", "Deutsche Grammophon", "ECM Records",
            "Musikverein", "金色大厅", "上海アリス幻樂団", "幽閉サテライト", "东方Project",
            "東方紅魔郷", "Miles Davis", "Metallica", "塞壬唱片", "Abbey Road Studios",
            "不存在的某某音乐厅", "某位佚名演奏家", "国际爱乐协会"
        };
        const size_t n_names = sizeof(names) / sizeof(names[0]);
        const std::vector<rta::meta::LinkMatch> ms = rta::meta::probe_links(
            std::vector<std::string>(names, names + n_names), idx);
        int hits = 0;
        for (const auto& m : ms) {
            if (m.hit) ++hits;
            printf("  %-22s %-3s %s\n", m.name.c_str(), m.hit ? "HIT" : "MISS",
                   m.hit ? m.url.c_str() : "（纯文本：匹配不上就不连）");
        }
        printf("  命中 %d/%zu\n", hits, ms.size());
        check(hits >= 18, "至少 18 个名词命中权威链接（实测 " + std::to_string(hits) + "）");
        check(!ms[n_names - 1].hit && !ms[n_names - 2].hit, "生造/佚名名词一律不连（不瞎连）");
        bool prefix_ok = true;
        for (const auto& m : ms) {
            if (!m.hit) continue;
            if (m.url.rfind("https://", 0) != 0) prefix_ok = false;
        }
        check(prefix_ok, "命中的链接都是 https 且来自种子表/材料");

        // 正文内联：每名词只连一次，且不乱插
        std::vector<rta::meta::LinkMatch> rep;
        const std::string in = "坂井泉水 与 ZARD 都在这张里；坂井泉水 第二次出现不再重复连。";
        const std::string out = rta::meta::linkify_text(in, idx, 4, &rep);
        size_t url_cnt = 0;
        for (size_t p = out.find("https://"); p != std::string::npos; p = out.find("https://", p + 1)) ++url_cnt;
        check(url_cnt == 2, "内联只给每个名词挂 1 次链接（实测 " + std::to_string(url_cnt) + "）");
        check(out.find("坂井泉水（https://zh.wikipedia.org/wiki/坂井泉水）") != std::string::npos,
              "内联文本形如「名词（url）」——前端既有 linkifyEscaped 会把 url 变成可点外链");
        check(rep.size() == 2, "内联结果同时回报结构化 links[]");
    }

    printf("\n=== 14. P4 单曲：核心档案 + 💿 补充背景与解读 ===\n");
    {
        static const char* kTrack = R"json({
  "disclaimer": "AI 整理，仅供参考",
  "fields": {
    "曲名": "Static World",
    "时长": "4:12",
    "编曲": "Iris",
    "原曲": "U.N.オーエンは彼女なのか？",
    "原曲出处": "东方红魔乡",
    "所属专辑": "Static World (EP)",
    "演唱": "Nago"
  },
  "sections": [
    {"heading": "💿 补充背景与解读", "paragraphs": [
      "曲名把『静止』与『世界』并置，配合 4/4 的机械鼓点，营造出停摆的舞池感。",
      "编曲保留了原曲的小调动机，却把主旋律交给失真合成器，与 Wingsed Step 一贯的 eurobeat 取向一致。"]}
  ]
})json";
        rta::meta::ProfileSignal s;
        s.title = "U.N.オーエンは彼女なのか？ Static World";
        s.hint = "同人";
        const rta::meta::ProfileDecision d = rta::meta::classify_profile(s);
        rta::meta::LinkIndex idx;
        idx.add_seed_table();
        const rta::meta::SummaryResult sr = rta::meta::summary_build_from_profile_reply(kTrack, d, {}, idx);
        check(sr.ok, "单曲资料解析成功", sr.error);
        bool core_ok = true;
        for (const char* f : {"曲名", "时长", "编曲", "原曲", "原曲出处", "所属专辑"}) {
            bool found = false;
            for (const auto& kv : sr.fields) if (kv.first == f) { found = true; break; }
            if (!found) core_ok = false;
        }
        check(core_ok, "曲目核心档案齐全（曲名/时长/编曲/原曲/原曲出处/所属专辑）");
        std::string analysis;
        for (const auto& kv : sr.fields) {
            if (kv.first.find("补充背景与解读") != std::string::npos) analysis = kv.second;
        }
        check(!analysis.empty() && analysis.size() > 60,
              "「💿 补充背景与解读」段落进入渲染（" + std::to_string(analysis.size()) + " 字节）");
        printf("  赏析首句：%.90s…\n", analysis.c_str());
        check(sr.profile == "doujin", "单曲也被分到同人类", sr.profile);
    }


    printf("\n=== 15. 端到端（离线注入 transport）：retrieval_fetch 真走新路径 ===\n");
    {
        // 预置 L2 材料（不发网络请求），注入假 LLM：返回「Static World」式的长输出。
        // 判据：fields >= 18 条、分类 id 落到结果、diag 带 profile=、名词外链非空、赏析可分节渲染。
        namespace m = rta::meta;
        const std::string name = "Winged Step Static Probe";
        const std::string eid = m::entity_id(m::EntityKind::Person, name);
        const fsx::path base = dir / "e2e_meta";
        std::error_code ecE;
        fsx::create_directories(base, ecE);
        const std::string baseStr = rta::meta::meta_string_of(base);
        {
            m::KnowledgeMaterial km;
            km.source = "dizzylab";
            km.kind = "release";
            km.subject = name;
            km.title = "Static World";
            km.url = "https://dizzylab.net/staticworld";
            km.lang = "zh";
            km.fetched_at = (long long)std::time(nullptr);
            km.expires_at = km.fetched_at + 30 * 86400;
            km.authority = 4;
            km.text = "社团: Winged Step\n会场: 例大祭\n原曲: U.N.オーエンは彼女なのか？\n"
                      "特典: 谱面册\n售价: 1100 日元\n通贩: dizzylab";
            check(m::knowledge_save_cache(baseStr, eid, name, { km }), "15.0 预置 L2 材料（离线）");
        }
        static const char* kLong = R"json({
  "disclaimer": "AI 整理，仅供参考",
  "fields": {
    "专辑名称": "Static World", "译名": "静止世界", "制作方": "Winged Step",
    "首发日期": "2024-01-09", "专辑类型": "EP（数字）", "分级": "全年龄",
    "碟数": "1", "音轨数": "6", "总时长": "24:13", "售价": "¥1,100 / ¥1,320",
    "通贩渠道": "dizzylab / Booth", "社团": "Winged Step", "会场": "C103",
    "特典": "谱面册（8P）", "编曲": "Iris / Nago", "母带": "Studio A",
    "曲目列表": "01 | Static World | 4:12 | Iris\n02 | Frozen Light | 3:48 | Nago"
  },
  "sections": [
    {"heading": "📀 专辑基本信息", "paragraphs": ["Winged Step 自主发行的第一张 EP。"]},
    {"heading": "💿 补充背景与解读", "paragraphs": [
      "标题意象是静止而失真的世界；编曲把原曲动机交给失真合成器。",
      "与 上海アリス幻樂団 的原曲相比，这张更偏向 eurobeat 骨架。"]}
  ],
  "citations": [{"id": "M1", "title": "dizzylab", "url": "https://dizzylab.net/staticworld"}]
})json";
        m::LlmConfig cfg;
        cfg.apiKey = "test-key";
        cfg.model = "deepseek-flash";
        cfg.usagePath = rta::meta::meta_string_of(dir / "e2e-usage.json");
        int chat_calls = 0;
        cfg.transport = [&](const std::string&, const std::string& body,
                            const std::vector<std::string>&) {
            ++chat_calls;
            rta::net::HttpResponse resp;
            resp.status = 200;
            const bool streaming = body.find("\"stream\":true") != std::string::npos;
            std::string s;
            if (streaming) {
                s = "data: {\"choices\":[{\"delta\":{\"content\":\"{\"}}]}\n\n";
                s += "data: [DONE]\n\n";
            } else {
                s = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"";
                s += m::meta_jesc(kLong);          // meta_jesc 只转义、不补引号
                s += "\"}}],\"usage\":{\"total_tokens\":900}}";
            }
            resp.body.assign(s.begin(), s.end());
            return resp;
        };
        m::RetrievalOptions ro;
        ro.metabase = baseStr;
        ro.enable_search = false;      // 只走 L2 材料 + LLM（不打官方 search）
        ro.enable_web_tools = false;
        ro.llm_cfg = cfg;
        const m::SummaryResult sr = m::retrieval_fetch(m::EntityKind::Person, name, "zh", ro);
        check(sr.ok, "15.1 retrieval_fetch 离线成功", sr.error);
        check(sr.fields.size() >= 18, "15.2 长输出的标签全部保留（实测 " +
              std::to_string(sr.fields.size()) + " 条；旧路径只会有 5~6 条）");
        check(!sr.profile.empty(), "15.3 结果带分类 id（实测 " + sr.profile + "）");
        check(sr.diag.find("profile=") != std::string::npos,
              "15.4 diag 带 profile=（可核查分错类的实例）", sr.diag);
        check(!sr.links.empty(), "15.5 名词外链落到结果里（实测 " +
              std::to_string(sr.links.size()) + " 条）");
        bool has_analysis = false;
        for (const auto& kv : sr.fields) {
            if (kv.first.find("补充背景与解读") != std::string::npos) has_analysis = true;
        }
        check(has_analysis, "15.6 「💿 补充背景与解读」进入可选渲染的 fields");
        // 2026-10-27（真实调用反馈）：citations 有内容时 materials_used 不能空着（语义要对齐）
        check(!sr.materials_used.empty() && !sr.citations.empty(),
              "15.8 materials_used 与 citations 对齐（都非空）",
              "used=" + std::to_string(sr.materials_used.size()) +
              " cites=" + std::to_string(sr.citations.size()));
        check(chat_calls >= 1, "15.7 走了 LLM 通道（注入 transport，实测 " +
              std::to_string(chat_calls) + " 次）");
        printf("  e2e: fields=%zu links=%zu profile=%s sections=%zu\n",
               sr.fields.size(), sr.links.size(), sr.profile.c_str(), sr.sections.size());
        std::printf("  diag: %s\n", sr.diag.c_str());
    }


    printf("\n=== 16. 真实调用回归：Static World 会场限定CD（父代理实测失败的三点） ===\n");
    {
        namespace m = rta::meta;

        // ---- ① profile=generic 的真因：关键词表里没有「会场限定」这类现场颁布语汇 ----
        m::ProfileSignal s;
        s.title = "Static World 会场限定CD @ Static World";
        s.artist = "Static World";
        s.material_text = "VGMdb organization page\nSWCD-001 | Muoku Reion | Aug 04, 2012\n"
                          "SWCD-002 | mirage | Dec 30, 2012";
        const m::ProfileDecision d = m::classify_profile(s);
        check(d.profile == m::MusicProfile::Doujin, "会场限定 ⟹ doujin（真实失败案例）",
              std::string("得到 ") + m::music_profile_id(d.profile) + " / " + d.reason);

        // ---- ② 材料页类型：专辑页必须排组织页前面，且组织页要被明确标注「仅补充」 ----
        check(m::pp_page_kind_rank("https://vgmdb.net/album/12345", "X - VGMdb") >
              m::pp_page_kind_rank("https://vgmdb.net/org/5044", "Static World - VGMdb"),
              "专辑页(/album/) 优先级 > 组织页(/org/)");
        m::SearchHit hOrg;
        hOrg.url = "https://vgmdb.net/org/5044";
        hOrg.title = "Static World - VGMdb";
        hOrg.snippet = "SWCD-001 ...";
        const std::string blkOrg = m::summary_materials_block({}, { hOrg });
        check(blkOrg.find("组织/艺术家页") != std::string::npos &&
              blkOrg.find("不得把它的发行列表") != std::string::npos,
              "组织页材料被标注「仅补充，不得当本实体事实」");
        m::SearchHit hAlb;
        hAlb.url = "https://vgmdb.net/album/9";
        hAlb.title = "Static World - VGMdb";
        hAlb.snippet = "1. 曲名 4:12";
        check(m::summary_materials_block({}, { hAlb }).find("专辑/作品页") != std::string::npos,
              "专辑页材料被标注「主材料」");

        // ---- ③ 提示词：材料不足要用模型知识补 + 标注；禁止「材料未给出」/过程性标签/目录填充 ----
        m::ProfileDecision dd;
        dd.profile = m::MusicProfile::Doujin;
        const m::ProfilePrompt pr =
            m::build_profile_prompt(dd, "Static World 会场限定CD", "专辑", "[M1] 组织页", {});
        check(pr.system.find("（AI 知识）") != std::string::npos,
              "prompt 允许用模型知识补全并标注「（AI 知识）」");
        check(pr.system.find("材料未给出") != std::string::npos,
              "prompt 明确禁止「材料未给出」式回答");
        check(pr.system.find("匹配情况") != std::string::npos,
              "prompt 禁止「匹配情况」这类过程性标签");
        check(pr.system.find("目录列表") != std::string::npos,
              "prompt 禁止用同团体目录填本实体字段");
        check(pr.system.find("以专辑页为准") != std::string::npos,
              "prompt 规定「专辑页优先、组织页只作补充」");

        // ---- ④ links=[] 的真因：材料的页面标题没进索引 ----
        const m::LinkIndex idx = m::summary_link_index({}, { hOrg });
        std::vector<m::LinkMatch> rep;
        const std::string out = m::linkify_text("Static World 这张会场限定CD 收录于 VGMdb。", idx, 4, &rep);
        check(!rep.empty(), "真实场景 links 非空（页面标题 + VGMdb 站点）",
              "links=" + std::to_string(rep.size()));
        check(out.find("Static World（https://vgmdb.net/org/5044）") != std::string::npos,
              "页面标题 → 该页 URL（同源，确定性）");
        check(out.find("VGMdb（https://vgmdb.net）") != std::string::npos, "站点名 → 官网");
        printf("  linkify: %s\n", out.c_str());

        // ---- ⑤ 检索策略：按 profile 定制检索词 + 「只有组织页 ⟹ 补搜专辑页」的判据 ----
        const std::string qd = m::pp_search_query_suffix(m::MusicProfile::Doujin, "zh");
        check(qd.find("同人") != std::string::npos && qd.find("会场限定") != std::string::npos &&
              qd.find("曲目") != std::string::npos,
              "同人盘的检索词带「同人/会场限定/曲目」（真实失败点：只搜到组织页）", qd);
        check(m::pp_search_query_suffix(m::MusicProfile::Classical, "zh").find("指挥") != std::string::npos,
              "古典盘的检索词带「指挥」");
        check(!m::summary_has_primary_material({ hOrg }),
              "只有组织页 ⟹ 没有主材料（触发补搜专辑页）");
        check(m::summary_has_primary_material({ hOrg, hAlb }),
              "出现专辑页 ⟹ 有主材料（不再补搜，省一次搜索费）");

        // ---- ⑥ 必查字段不能默默消失：「确定没有」也要写出来 ----
        check(pr.system.find("必查字段逐条给结论") != std::string::npos,
              "prompt 要求必查字段逐条给结论（不许默默跳过）");
        check(pr.system.find("未见公开通贩记录") != std::string::npos,
              "prompt 给出「确定没有也要写」的示例（通贩渠道）");
    }

    fsx::remove_all(dir, ec);
    printf("\n%s（失败项 %d）\n", fails == 0 ? "全部通过" : "有失败项", fails);
    return fails == 0 ? 0 : 1;
}
