// test_lyrics_ai.cpp — 歌词 AI 处理（翻译 / 音译 / 日语假名标注）离线单测
//
// 判据（铁律 3）：全程不联网。cfg.transport 钩子喂假 HTTP 响应，并记录调用次数与请求体，
// 因此"没有发起 HTTP"这类否定断言是可证的（计数器 == 0），而不是"看起来没报错"。
// 覆盖：正常翻译 / 行数不符 / 代码围栏 / 空行保留 / 缓存往返 / 损坏缓存自愈 /
//       无 key 快速失败 / 截断拒绝 / 解析失败摘要 / max_tokens 估算与封顶 / key 稳定性。
//
// 不碰用户配置：用量文件写到 %TEMP% 的本次测试目录，结束时整个删掉。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "meta/lyrics_ai.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace rta::meta;
namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

static void ck(bool cond, const std::string& what, const std::string& detail = std::string()) {
    if (cond) {
        ++g_pass;
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : "  |  ", detail.c_str());
    }
}

// 假传输：llm_chat 见到 cfg.transport 就不再走 winhttp。计数器是"没发请求"的唯一证据。
struct FakeLlm {
    int calls = 0;
    std::string lastUrl;
    std::string lastBody;
    std::vector<std::string> lastHeaders;
    std::string content;              // choices[0].message.content
    std::string finish = "stop";
    int status = 200;
    int totalTokens = 128;

    rta::net::HttpResponse operator()(const std::string& url, const std::string& body,
                                      const std::vector<std::string>& headers) {
        ++calls;
        lastUrl = url;
        lastBody = body;
        lastHeaders = headers;
        rta::net::HttpResponse r;
        if (status == 0) {
            r.status = 0;
            r.error = "模拟网络失败";
            return r;
        }
        r.status = status;
        r.ok = (status >= 200 && status < 300);
        std::string json;
        if (r.ok) {
            json = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"" + llm_esc(content) +
                   "\"},\"finish_reason\":\"" + finish + "\"}],\"usage\":{\"total_tokens\":" +
                   std::to_string(totalTokens) + "}}";
        } else {
            json = "{\"error\":{\"message\":\"boom\"}}";
        }
        r.body.assign(json.begin(), json.end());
        return r;
    }
};

static std::string json_array(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += '"';
        s += llm_esc(v[i]);
        s += '"';
    }
    s += ']';
    return s;
}

static LlmConfig make_cfg(FakeLlm& f, const std::string& tmp) {
    LlmConfig c;
    c.baseUrl = "http://127.0.0.1:9";        // 不会被真的访问：transport 已接管
    c.apiKey = "sk-test-offline";
    c.model = "deepseek-flash";
    c.usagePath = tmp + "/llm-usage.json";   // 别把用量写到仓库目录
    c.transport = [&f](const std::string& u, const std::string& b,
                       const std::vector<std::string>& h) { return f(u, b, h); };
    return c;
}

static std::string g_tmp;

// 8 行日文原文与对应的假译文（内容本身不重要，重要的是逐行对齐）
static const std::vector<std::string> kJa = {
    "夜に駆ける",
    "沈むように溶けてゆくように",
    "二人だけの空が広がる夜に",
    "「さよなら」だけだった",
    "その一言で全てが分かった",
    "日が沈み出した空と君の姿",
    "フェンス越しに重なっていた",
    "初めて会った日から"
};

static const std::vector<std::string> kZh = {
    "向着夜晚奔跑",
    "像要沉没般融化而去",
    "只属于两人的天空在夜色中铺展",
    "只剩下「再见」这句话",
    "仅凭那一句我就全都明白了",
    "日落时分的天空与你的身影",
    "隔着围栏重叠在一起",
    "从第一次相遇的那天起"
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    const fs::path tmpPath = fs::temp_directory_path() /
        ("rta-test-lyrics-ai-" + std::to_string((unsigned long long)::GetCurrentProcessId()));
    {
        const std::u8string u8 = tmpPath.u8string();
        g_tmp.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    std::error_code ec;
    fs::remove_all(tmpPath, ec);
    fs::create_directories(tmpPath, ec);
    std::printf("临时目录：%s\n", g_tmp.c_str());

    FakeLlm f;
    const LlmConfig cfg = make_cfg(f, g_tmp);

    // ---------------- 1) 正常翻译 ----------------
    std::printf("\n-- 1) 正常翻译：8 行进、8 行出\n");
    {
        f.content = json_array(kZh);
        f.calls = 0;
        const LyricsAiResult r = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(r.ok, "ok=true", r.error);
        ck(r.lines.size() == kJa.size(), "输出行数 == 输入行数",
           std::to_string(r.lines.size()) + " vs " + std::to_string(kJa.size()));
        ck(r.lines == kZh, "内容逐行对应");
        ck(r.tokens == 128, "tokens 透传", std::to_string(r.tokens));
        ck(!r.fromCache, "fromCache=false（首次调用）");
        ck(f.calls == 1, "只发一次请求", std::to_string(f.calls));
        ck(f.lastUrl.find("/chat/completions") != std::string::npos, "打到 /chat/completions", f.lastUrl);
        ck(!f.lastHeaders.empty() && f.lastHeaders[0].find("Bearer sk-test-offline") != std::string::npos,
           "Authorization 头带上 key");
        // 2026-10-30 实测改判：json_object 要求顶层是对象，而 system 要求"只输出数组"，
        //   两者冲突时 deepseek-flash 会一直推理到 finish_reason=length（3 行烧掉 950 token）。
        //   现在**不开** response_format，靠解析侧剥围栏兜底 ⟹ 断言反过来。
        ck(f.lastBody.find("\"response_format\"") == std::string::npos,
           "不开 response_format（与「只输出数组」冲突，实测会截断）");
        ck(f.lastBody.find("\"max_tokens\":3200") != std::string::npos,
           "max_tokens = 300*8+800 = 3200（轻任务）");
        ck(f.lastBody.find("元素个数必须等于 8") != std::string::npos,
           "system 写明「元素个数必须等于 N」");
        ck(f.lastBody.find("不要输出任何解释") != std::string::npos &&
           f.lastBody.find("Markdown 代码围栏") != std::string::npos,
           "system 写明「不要解释 / 不要围栏」");
        ck(f.lastBody.find("1. 夜に駆ける") != std::string::npos &&
           f.lastBody.find("8. 初めて会った日から") != std::string::npos,
           "user 里是编号列表");
        ck(f.lastBody.find("目标语言") != std::string::npos &&
           f.lastBody.find("中文（zh）") != std::string::npos,
           "user 里给出目标语言");
    }

    // ---------------- 2) 行数不符 ----------------
    std::printf("\n-- 2) 行数不符：模型回 7 行\n");
    {
        const std::vector<std::string> seven(kZh.begin(), kZh.end() - 1);
        f.content = json_array(seven);
        f.calls = 0;
        const LyricsAiResult r = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(!r.ok, "ok=false");
        ck(r.error.find("行数") != std::string::npos, "error 里含「行数」", r.error);
        ck(r.lines.empty(), "失败时不返回半份结果", std::to_string(r.lines.size()));
    }

    // ---------------- 3) 代码围栏 / 前后闲话 ----------------
    std::printf("\n-- 3) 代码围栏与前后闲话仍能解析\n");
    {
        const std::string fence = kLyricsAiFence;
        f.content = fence + "json\n" + json_array(kZh) + "\n" + fence;
        const LyricsAiResult r1 = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(r1.ok, "围栏响应 ok", r1.error);
        ck(r1.lines == kZh, "围栏响应内容正确");

        f.content = "好的，这是结果：\n" + json_array(kZh) + "\n希望对你有帮助。";
        const LyricsAiResult r2 = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(r2.ok, "前后带闲话仍能解析", r2.error);
        ck(r2.lines == kZh, "闲话响应内容正确");
    }

    // ---------------- 4) 空行保留 ----------------
    std::printf("\n-- 4) 空行保留：2 个空行 + 2 个纯空白行\n");
    {
        const std::vector<std::string> src = {
            "夜に駆ける", "", "沈むように", "   ", "二人だけの空", "\t", "さよなら", ""
        };
        const std::vector<std::string> out = {"向着夜晚奔跑", "像要沉没般", "只属于两人的天空", "再见"};
        f.content = json_array(out);
        f.calls = 0;
        const LyricsAiResult r = lyrics_ai_process(cfg, LyricsAiTask::Translate, src, "ja", "zh");
        ck(r.ok, "ok=true", r.error);
        ck(r.lines.size() == src.size(), "行数守恒（8）", std::to_string(r.lines.size()));
        ck(r.lines[1].empty() && r.lines[3].empty() && r.lines[5].empty() && r.lines[7].empty(),
           "4 个空行位置仍是空行");
        ck(r.lines[0] == out[0] && r.lines[2] == out[1] && r.lines[4] == out[2] && r.lines[6] == out[3],
           "非空行按原下标插回");
        ck(f.lastBody.find("\"max_tokens\":2000") != std::string::npos,
           "只送 4 个非空行（max_tokens=300*4+800=2000）");
    }

    // ---------------- 5) 缓存往返 ----------------
    std::printf("\n-- 5) 缓存：save -> load 命中；process 第二次不再请求\n");
    {
        const std::string dir = g_tmp + "/cache";
        f.content = json_array(kZh);
        f.calls = 0;
        const LyricsAiResult r1 = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh", dir);
        ck(r1.ok && !r1.fromCache, "第一次走网络", r1.error);
        ck(f.calls == 1, "请求 1 次", std::to_string(f.calls));

        const std::string key = lyrics_cache_key(LyricsAiTask::Translate, "ja", "zh", kJa);
        const fs::path file = lyrics_ai_cache_file(dir, key);
        ck(fs::exists(file, ec), "缓存文件已落盘", dir + "/" + key + ".json");
        ck(!fs::exists(lyrics_ai_u8path(dir + "/" + key + ".json.part"), ec), "没有残留 .part");

        LyricsAiResult hit;
        ck(lyrics_cache_load(dir, key, hit), "load 命中");
        ck(hit.ok && hit.fromCache, "hit.ok && fromCache");
        ck(hit.lines == r1.lines, "内容一致");
        ck(hit.tokens == r1.tokens, "tokens 一致");

        const int before = f.calls;
        const LyricsAiResult r2 = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh", dir);
        ck(r2.ok && r2.fromCache, "第二次 process 命中缓存", r2.error);
        ck(f.calls == before, "命中缓存不发 HTTP", std::to_string(f.calls - before));
        ck(r2.lines == r1.lines, "缓存结果与首次一致");

        LyricsAiResult dummy;
        ck(!lyrics_cache_load("", key, dummy), "dir 为空：load 返回 false");
        ck(!lyrics_cache_save("", key, r1), "dir 为空：save 返回 false");

        LyricsAiResult failed;
        failed.error = "网络失败";
        ck(!lyrics_cache_save(dir, "deadbeefdeadbeef", failed), "失败结果不写缓存");
        ck(!fs::exists(lyrics_ai_cache_file(dir, "deadbeefdeadbeef"), ec), "确实没写文件");

        // 引号/反斜杠/制表符必须逐字节往返：缓存是自己写的 JSON，
        // 转义与严格解析只要有一边不对称，歌词里的 " 就会吃掉半行。
        LyricsAiResult tricky;
        tricky.ok = true;
        tricky.tokens = 7;
        tricky.lines = {"he said \"hi\"", "back\\slash", "tab\there", ""};
        ck(lyrics_cache_save(dir, "aaaaaaaaaaaaaaaa", tricky), "特殊字符结果可写缓存");
        LyricsAiResult tgot;
        ck(lyrics_cache_load(dir, "aaaaaaaaaaaaaaaa", tgot), "特殊字符结果可读回");
        ck(tgot.lines == tricky.lines, "引号/反斜杠/制表符/空串逐字节一致");
    }

    // ---------------- 6) 损坏缓存 ----------------
    std::printf("\n-- 6) 损坏缓存：load=false 且文件被删除\n");
    {
        const std::string dir = g_tmp + "/cache-bad";
        fs::create_directories(lyrics_ai_u8path(dir), ec);
        const std::string key = "0123456789abcdef";
        const fs::path file = lyrics_ai_cache_file(dir, key);

        {
            std::ofstream o(file, std::ios::binary | std::ios::trunc);
            o << "{\"v\":1,\"lines\":[\"a\",\"b\"";
        }
        LyricsAiResult out;
        ck(!lyrics_cache_load(dir, key, out), "截断文件 load=false");
        ck(!fs::exists(file, ec), "截断文件已被删除");

        {
            std::ofstream o(file, std::ios::binary | std::ios::trunc);
            o << "{\"v\":1,\"lines\":\"不是数组\"}";
        }
        ck(!lyrics_cache_load(dir, key, out), "结构不对 load=false");
        ck(!fs::exists(file, ec), "结构不对的文件也被删除");

        {
            std::ofstream o(file, std::ios::binary | std::ios::trunc);
            o << "{\"v\":999,\"lines\":[\"a\"]}";
        }
        ck(!lyrics_cache_load(dir, key, out), "版本不符 load=false");
        ck(!fs::exists(file, ec), "版本不符的文件也被删除");
    }

    // ---------------- 7) 无 apiKey ----------------
    std::printf("\n-- 7) 无 apiKey：快速失败且不发 HTTP\n");
    {
        LlmConfig c = cfg;
        c.apiKey.clear();
        f.calls = 0;
        const LyricsAiResult r = lyrics_ai_process(c, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(!r.ok, "ok=false");
        ck(r.error == "未配置 api_key", "error=\"未配置 api_key\"", r.error);
        ck(f.calls == 0, "transport 调用次数 = 0", std::to_string(f.calls));
        ck(r.lines.empty(), "没有结果行");
    }

    // ---------------- 8) 截断 ----------------
    std::printf("\n-- 8) finish_reason=length：拒绝半首歌\n");
    {
        f.finish = "length";
        f.content = json_array(std::vector<std::string>(kZh.begin(), kZh.begin() + 5));
        const LyricsAiResult r = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(!r.ok, "ok=false");
        ck(r.error.find("截断") != std::string::npos, "error 里含「截断」", r.error);
        ck(r.lines.empty(), "不返回半份结果");
        f.finish = "stop";
    }

    // ---------------- 9) 非 JSON 响应 ----------------
    std::printf("\n-- 9) 非 JSON 响应：error 带原文摘要\n");
    {
        f.content = "抱歉，我无法完成这个请求。";
        const LyricsAiResult r1 = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(!r1.ok, "ok=false");
        ck(r1.error.find("抱歉") != std::string::npos, "error 里有原文摘要", r1.error);

        f.content = std::string(400, 'x');
        const LyricsAiResult r2 = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(!r2.ok, "ok=false");
        ck(r2.error.size() < 400, "摘要被截到 200 字符以内", std::to_string(r2.error.size()));
    }

    // ---------------- 10) max_tokens 封顶 ----------------
    std::printf("\n-- 10) max_tokens 上限 8000\n");
    {
        const std::vector<std::string> many(300, "x");
        f.content = json_array(many);
        const LyricsAiResult r = lyrics_ai_process(cfg, LyricsAiTask::Translate, many, "ja", "zh");
        ck(r.ok, "300 行 ok", r.error);
        ck(f.lastBody.find("\"max_tokens\":8000") != std::string::npos, "max_tokens 封顶 8000");
        ck(r.lines.size() == 300, "行数守恒（300）", std::to_string(r.lines.size()));
    }

    // ---------------- 11) key 稳定性 ----------------
    std::printf("\n-- 11) 缓存 key：稳定、区分任务/语言/内容\n");
    {
        const std::string k1 = lyrics_cache_key(LyricsAiTask::Translate, "ja", "zh", kJa);
        ck(k1 == lyrics_cache_key(LyricsAiTask::Translate, "ja", "zh", kJa), "同输入同 key");
        ck(k1.size() == 16, "16 位十六进制", k1);
        bool hex = true;
        for (char ch : k1) {
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) hex = false;
        }
        ck(hex, "只含 0-9a-f", k1);
        ck(lyrics_cache_key(LyricsAiTask::Furigana, "ja", "zh", kJa) != k1, "任务不同 key 不同");
        ck(lyrics_cache_key(LyricsAiTask::Translate, "ja", "en", kJa) != k1, "目标语言不同 key 不同");
        ck(lyrics_cache_key(LyricsAiTask::Translate, "en", "zh", kJa) != k1, "源语言不同 key 不同");
        std::vector<std::string> changed = kJa;
        changed[3] = "改一行";
        ck(lyrics_cache_key(LyricsAiTask::Translate, "ja", "zh", changed) != k1, "内容不同 key 不同");
        ck(lyrics_cache_key(LyricsAiTask::Translate, "JA", "ZH", kJa) == k1, "语言码大小写归一");

        const std::vector<std::string> a1 = {"ab", "c"};
        const std::vector<std::string> a2 = {"a", "bc"};
        ck(lyrics_cache_key(LyricsAiTask::Translate, "ja", "zh", a1) !=
           lyrics_cache_key(LyricsAiTask::Translate, "ja", "zh", a2), "行边界不歧义");
    }

    // ---------------- 12) 全空行 ----------------
    std::printf("\n-- 12) 全空行输入：不调模型\n");
    {
        const std::vector<std::string> blanks = {"", "   ", "\t"};
        f.calls = 0;
        const LyricsAiResult r = lyrics_ai_process(cfg, LyricsAiTask::Translate, blanks, "ja", "zh");
        ck(r.ok, "ok=true", r.error);
        ck(r.lines.size() == blanks.size() && r.lines[0].empty() && r.lines[2].empty(), "输出仍是空行");
        ck(f.calls == 0, "没有发起 HTTP", std::to_string(f.calls));
    }

    // ---------------- 13) HTTP 错误 ----------------
    std::printf("\n-- 13) HTTP 500 / 网络失败：明确报错\n");
    {
        f.content = json_array(kZh);
        f.status = 500;
        const LyricsAiResult r1 = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(!r1.ok, "ok=false");
        ck(r1.error.find("500") != std::string::npos, "error 里有状态码", r1.error);

        f.status = 0;
        const LyricsAiResult r2 = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "zh");
        ck(!r2.ok && !r2.error.empty(), "网络失败有错误信息", r2.error);
        f.status = 200;
    }

    // ---------------- 14) 翻译缺目标语言 ----------------
    std::printf("\n-- 14) 翻译缺目标语言：不发请求\n");
    {
        f.calls = 0;
        const LyricsAiResult r = lyrics_ai_process(cfg, LyricsAiTask::Translate, kJa, "ja", "");
        ck(!r.ok, "ok=false");
        ck(r.error.find("目标语言") != std::string::npos, "error 提到目标语言", r.error);
        ck(f.calls == 0, "没有发起 HTTP", std::to_string(f.calls));
    }

    // ---------------- 15) 音译 / 假名标注 ----------------
    std::printf("\n-- 15) 音译 / 假名标注：任务描述进 user\n");
    {
        const std::vector<std::string> eight = {"a", "b", "c", "d", "e", "f", "g", "h"};
        f.content = json_array(eight);
        const LyricsAiResult rt = lyrics_ai_process(cfg, LyricsAiTask::Transliterate, kJa, "ja", "");
        ck(rt.ok, "音译 ok", rt.error);
        ck(f.lastBody.find("拉丁字母") != std::string::npos, "音译提示词含「拉丁字母」");

        f.content = json_array(eight);
        const LyricsAiResult rf = lyrics_ai_process(cfg, LyricsAiTask::Furigana, kJa, "ja", "");
        ck(rf.ok, "假名标注 ok", rf.error);
        ck(f.lastBody.find("平假名") != std::string::npos, "假名提示词含「平假名」");
    }

    fs::remove_all(tmpPath, ec);
    std::printf("\n== 结束：%s（断言 %d 项，PASS %d / FAIL %d）==\n",
                g_fail == 0 ? "全部通过" : "有失败", g_pass + g_fail, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
