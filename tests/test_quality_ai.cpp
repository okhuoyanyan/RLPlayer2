// test_quality_ai.cpp — 音频体检 B4：AI 解释层（离线，可注入 transport，绝不联网）
// 判据：
//   1) 无 key ⟹ 明确降级，确定性解释照常返回；
//   2) 请求体只含确定性指标、不含 key；key 只在 Authorization 头；
//   3) 服务端把 key 回显进错误时，返回文本里必须是 ****（脱敏）；
//   4) 同输入 ⟹ 请求体逐字节相同（确定性）。
#include "analysis/quality_ai.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

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

    fsx::remove_all(dir, ec);
    printf("\n%s（失败项 %d）\n", fails == 0 ? "全部通过" : "有失败项", fails);
    return fails == 0 ? 0 : 1;
}
