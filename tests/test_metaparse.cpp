// test_metaparse.cpp — src/meta/parse.h：文件名/标签/LLM 回包解析
//
// 判据说明（铁律 3）：被测对象是纯字符串规则，因此逐字段断言输入→输出的映射，
// 样例全部来自任务规定必过的 7 个真实文件名 + docs/元数据补全调研与规划.md 的
// 曲库样本（澤田なつ / 塞壬唱片-MSR / 贝多芬交响曲）。不依赖网络、不打开音频设备、
// 不读写磁盘（纯函数，无临时目录需求）；confidence 只断言浮点近似值。
//
// ⚠️ 未解决项（铁律 4）：无。当前以本文件的断言集作为 src/meta/parse.h 的回归基线。
// 2026-10-21 追加「缺陷修复回归」6 组：尾部 (feat.x) 括号错进 version、
// 闭括号后紧跟连字符漏切分（均来自曲库 MUSIC 真实文件名形态）。
// 2026-10-21 再追加「全角 （） 摘组回归」3 组（调研文档 §5 未解决项 10 的修复，
// MUSIC 实测 10 首受影响）：全角括号组照常摘除/分类，artist 段不误伤。
#include "meta/parse.h"
#include <cstdio>
#include <string>

// 本测试按 spec 不引入任何 Windows 头；只声明用到的唯一 C 接口
// （kernel32!SetConsoleOutputCP，x64 下 __stdcall 即默认调用约定）
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using rta::meta::ParsedName;
using rta::meta::parse_filename;
using rta::meta::parse_from_tags;
using rta::meta::parse_llm_parse;
using rta::meta::parse_llm_prompt;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
static void ck_eq(const std::string& got, const std::string& want, const char* what) {
    ck(got == want, what, "got=[" + got + "] want=[" + want + "]");
}
static bool approx(float a, float b) { return (a - b) < 0.005f && (b - a) < 0.005f; }
static void ck_conf(float got, float want, const char* what) {
    ck(approx(got, want), what, "got=" + std::to_string(got) + " want=" + std::to_string(want));
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== meta/parse 文件名/标签解析验证 ==\n");

    // ---- 必过样例 1~7（逐字段断言） ----
    {
        std::printf("- 例1 纯 artist - title\n");
        const ParsedName p = parse_filename("Isobel Campbell - Loving Hannah");
        ck_eq(p.artist, "Isobel Campbell", "例1 artist");
        ck_eq(p.title,  "Loving Hannah",   "例1 title");
        ck_eq(p.feat,   "",                "例1 feat 为空");
        ck_eq(p.version, "",               "例1 version 为空");
        ck_eq(p.method, "rule",            "例1 method=rule");
        ck_conf(p.confidence, 0.8f,        "例1 confidence=0.8");
    }
    {
        std::printf("- 例2 括号别名 artist + feat_ + 尾括号 version\n");
        const ParsedName p =
            parse_filename("澤田なつ (さわだ なつ) - 紅い瞳に映るセカイ feat_二階堂真紅 (Single Version)");
        ck_eq(p.artist, "澤田なつ (さわだ なつ)", "例2 artist（括号别名整体保留）");
        ck_eq(p.title,  "紅い瞳に映るセカイ",     "例2 title");
        ck_eq(p.feat,   "二階堂真紅",             "例2 feat");
        ck_eq(p.version, "Single Version",        "例2 version");
        ck_conf(p.confidence, 0.9f,               "例2 confidence=0.9（feat+version 封顶）");
    }
    {
        std::printf("- 例3 「 _ 」不是分隔符\n");
        const ParsedName p = parse_filename("塞壬唱片-MSR _ Ivoris - Winged Step");
        ck_eq(p.artist, "塞壬唱片-MSR _ Ivoris", "例3 artist（artist 段里的 - 和 _ 不拆）");
        ck_eq(p.title,  "Winged Step",           "例3 title");
        ck_eq(p.feat,   "",                      "例3 feat 为空");
        ck_eq(p.version, "",                     "例3 version 为空");
        ck_conf(p.confidence, 0.8f,              "例3 confidence=0.8");
    }
    {
        std::printf("- 例4 尾部单个版本括号\n");
        const ParsedName p = parse_filename("谭晶 - 游京 (长征版)");
        ck_eq(p.artist, "谭晶",    "例4 artist");
        ck_eq(p.title,  "游京",    "例4 title");
        ck_eq(p.feat,   "",        "例4 feat 为空");
        ck_eq(p.version, "长征版", "例4 version");
        ck_conf(p.confidence, 0.85f, "例4 confidence=0.85");
    }
    {
        std::printf("- 例5 曲号剥离 + title 内的 \" - \" 保留\n");
        const ParsedName p =
            parse_filename("01. Beethoven - Symphony No. 9 in D minor, Op. 125 - II. Molto vivace");
        ck_eq(p.artist, "Beethoven", "例5 artist");
        ck_eq(p.title,  "Symphony No. 9 in D minor, Op. 125 - II. Molto vivace",
               "例5 title（第一个 \" - \" 切分，后面的 \" - II.\" 留在 title）");
        ck_eq(p.feat,   "", "例5 feat 为空");
        ck_eq(p.version, "", "例5 version 为空");
        ck_conf(p.confidence, 0.8f, "例5 confidence=0.8");
    }
    {
        std::printf("- 例6 无分隔符\n");
        const ParsedName p = parse_filename("Loving Hannah");
        ck_eq(p.artist, "",             "例6 artist 为空");
        ck_eq(p.title,  "Loving Hannah", "例6 title=全文");
        ck(p.confidence < 0.5f,         "例6 confidence<0.5", std::to_string(p.confidence));
        ck_eq(p.method, "rule",         "例6 method=rule");
    }
    {
        std::printf("- 例7 连续两组尾括号\n");
        const ParsedName p = parse_filename("群星 - 晴天 (DJ版) (Live)");
        ck_eq(p.artist, "群星",         "例7 artist");
        ck_eq(p.title,  "晴天",         "例7 title");
        ck_eq(p.version, "DJ版 / Live", "例7 version（按原文从左到右连接）");
        ck_eq(p.feat,   "",             "例7 feat 为空");
        ck_conf(p.confidence, 0.85f,    "例7 confidence=0.85");
    }

    // ---- parse_llm_parse 三例 ----
    {
        std::printf("- LLM1 正常 JSON\n");
        const ParsedName p = parse_llm_parse(
            "{\"artist\": \"A\", \"title\": \"T\", \"feat\": \"F\", \"version\": \"V\"}");
        ck_eq(p.method, "llm", "LLM1 method=llm");
        ck_eq(p.artist, "A",   "LLM1 artist");
        ck_eq(p.title,  "T",   "LLM1 title");
        ck_eq(p.feat,   "F",   "LLM1 feat");
        ck_eq(p.version, "V",  "LLM1 version");
        ck_conf(p.confidence, 0.7f, "LLM1 confidence=0.7");
    }
    {
        std::printf("- LLM2 ```json 围栏\n");
        const ParsedName p = parse_llm_parse(
            "```json\n"
            "{\"artist\": \"澤田なつ\", \"title\": \"紅い瞳に映るセカイ\", \"feat\": \"\", \"version\": \"Single Version\"}\n"
            "```");
        ck_eq(p.method, "llm", "LLM2 method=llm");
        ck_eq(p.artist, "澤田なつ",               "LLM2 artist");
        ck_eq(p.title,  "紅い瞳に映るセカイ",     "LLM2 title");
        ck_eq(p.feat,   "",                       "LLM2 feat 为空");
        ck_eq(p.version, "Single Version",        "LLM2 version");
        ck_conf(p.confidence, 0.7f,               "LLM2 confidence=0.7");
    }
    {
        std::printf("- LLM3 垃圾文本\n");
        const ParsedName p = parse_llm_parse("抱歉，这段文件名我无法解析。");
        ck(p.artist.empty() && p.feat.empty() && p.version.empty(),
           "LLM3 垃圾文本 → 其余字段全空");
        ck_eq(p.method, "llm", "LLM3 method=llm");
        ck(p.title.empty(), "LLM3 title 留空");
        ck(p.confidence == 0.f, "LLM3 confidence=0", std::to_string(p.confidence));
    }

    // ---- parse_llm_prompt ----
    {
        std::printf("- prompt 生成\n");
        const std::string stem = "01. Beethoven - Symphony No. 9 in D minor, Op. 125 - II. Molto vivace";
        const std::string pr = parse_llm_prompt(stem);
        ck(pr.find(stem) != std::string::npos, "prompt 含 stem 原文");
        ck(pr.find("JSON") != std::string::npos, "prompt 含 \"JSON\" 字样");
        ck(pr.find("\"artist\"") != std::string::npos && pr.find("\"title\"") != std::string::npos &&
           pr.find("\"feat\"") != std::string::npos && pr.find("\"version\"") != std::string::npos,
           "prompt 含四个键名");
    }

    // ---- 补充边界（曲号变体 / feat 边界 / 标签透传 / 空输入） ----
    {
        std::printf("- 补充边界\n");
        ParsedName p = parse_filename("[07] Aimee Mann - Save Me");
        ck_eq(p.artist, "Aimee Mann", "曲号 [07] 剥除");
        p = parse_filename("(03) 坂本龙一 - Merry Christmas Mr. Lawrence");
        ck_eq(p.artist, "坂本龙一", "曲号 (03) 剥除");
        p = parse_filename("07 - Under Pressure");
        ck_eq(p.title, "Under Pressure", "曲号 \"07 - \" 剥除后不再误切");
        ck_eq(p.artist, "",              "剥完无分隔符 → artist 空");
        p = parse_filename("99Luftballons - Nena");
        ck_eq(p.artist, "99Luftballons", "纯数字开头后跟文字 → 不当曲号");
        p = parse_filename("5.1 Mix - Tonality");
        ck_eq(p.artist, "5.1 Mix", "\"5.1\" 小数开头不当曲号");
        p = parse_filename("Queen - Under Pressure featuring David Bowie");
        ck_eq(p.title, "Under Pressure",  "featuring 切 title");
        ck_eq(p.feat,  "David Bowie",     "featuring 提 feat");
        p = parse_filename("Artist - Soft. Rock");
        ck_eq(p.title, "Soft. Rock", "\"Soft.\" 的 ft. 无前边界不误切");
        p = parse_filename("Loving Hannah (Live)");
        ck_eq(p.title, "Loving Hannah", "无分隔符路径也摘尾括号");
        ck_eq(p.version, "Live",        "无分隔符路径 version 照提");
        ck(p.confidence < 0.5f,         "无分隔符路径 confidence 仍 0.3", std::to_string(p.confidence));
        p = parse_filename("");
        ck(p.title.empty() && p.artist.empty() && p.method == "rule" && p.confidence == 0.f,
           "空 stem → 全空 method=rule confidence=0");

        p = parse_from_tags("五条人", "阿珍爱上了阿强");
        ck_eq(p.method, "tag", "标签 method=tag");
        ck_eq(p.artist, "五条人",       "标签 artist 透传");
        ck_eq(p.title,  "阿珍爱上了阿强", "标签 title 透传");
        ck_conf(p.confidence, 1.0f,     "标签齐全 confidence=1");
        p = parse_from_tags("五条人", "");
        ck_conf(p.confidence, 0.5f,     "标签缺 title confidence=0.5");
        p = parse_from_tags("", "");
        ck_conf(p.confidence, 0.5f,     "标签全空 confidence=0.5");
        ck_eq(p.method, "tag",          "标签全空仍 method=tag");
    }

    // ---- 缺陷修复回归（2026-10-21，曲库真实形态） ----
    // 缺陷 1：尾部 (feat.x) 括号曾被错摘进 version；缺陷 2：闭括号后紧跟连字符
    //（全角 ）无空格）曾漏切分。以下 6 组为规定的必过断言。
    {
        std::printf("- 修1 尾部 (feat.x) 不再错进 version\n");
        const ParsedName p = parse_filename("未来永劫 (feat.可不)");
        ck_eq(p.artist, "",        "修1 artist 为空");
        ck_eq(p.title,  "未来永劫", "修1 title");
        ck_eq(p.feat,   "可不",     "修1 feat");
        ck_eq(p.version, "",       "修1 version 为空");
    }
    {
        std::printf("- 修2 曲号 + 尾部 (feat.x)\n");
        const ParsedName p = parse_filename("2 未来永劫 (feat.可不)");
        ck_eq(p.artist, "",        "修2 artist 为空");
        ck_eq(p.title,  "未来永劫", "修2 title（曲号剥掉后同修1）");
        ck_eq(p.feat,   "可不",     "修2 feat");
        ck_eq(p.version, "",       "修2 version 为空");
    }
    {
        std::printf("- 修3 闭括号后紧跟连字符的回退切分\n");
        const ParsedName p = parse_filename("人声音质（甜系女声）- 恋文");
        ck_eq(p.artist, "人声音质（甜系女声）", "修3 artist（含全角括号整体保留）");
        ck_eq(p.title,  "恋文",                "修3 title");
        ck_conf(p.confidence, 0.8f,            "修3 confidence=0.8（与标准切分同基准）");
    }
    {
        std::printf("- 修4 回退切分 + 双空格 trim + title 内连字符保留\n");
        const ParsedName p = parse_filename(
            "器乐音质（综合）-  The Heroic Weather-Conditions of the Universe, Part 7_ After The Storm");
        ck_eq(p.artist, "器乐音质（综合）", "修4 artist");
        ck_eq(p.title,
              "The Heroic Weather-Conditions of the Universe, Part 7_ After The Storm",
              "修4 title（双空格 trim；-Conditions 原样保留）");
        ck_conf(p.confidence, 0.8f,       "修4 confidence=0.8");
    }
    {
        std::printf("- 修5 (inst.) 无 feat 标记仍进 version\n");
        const ParsedName p = parse_filename("9 未来永劫 (inst.)");
        ck_eq(p.artist, "",        "修5 artist 为空");
        ck_eq(p.title,  "未来永劫", "修5 title");
        ck_eq(p.version, "inst.",  "修5 version");
        ck_eq(p.feat,   "",        "修5 feat 为空");
    }
    {
        std::printf("- 修6 (feat. x) 与 (Live) 混排\n");
        const ParsedName p = parse_filename("群星 - 天空 (feat. 王菲) (Live)");
        ck_eq(p.artist,  "群星",   "修6 artist");
        ck_eq(p.title,   "天空",   "修6 title");
        ck_eq(p.feat,    "王菲",   "修6 feat");
        ck_eq(p.version, "Live",   "修6 version（feat 组不再挤占 version）");
    }

    // ---- 全角 （） 摘组回归（2026-10-21，调研文档 §5 未解决项 10） ----
    // pd_strip_trailing_groups 的括号族补入全角 （(EF BC 88)/）(EF BC 89)（各三字节），
    // 与 【】 同型：深度配对逻辑不变。以下 3 组为规定的必过断言。
    {
        std::printf("- 全1 全角括号 feat 组（曲号 + 无分隔符路径）\n");
        const ParsedName p = parse_filename("1 不夜城 Night Fall（feat.冬名）");
        ck_eq(p.artist, "",                  "全1 artist 为空（无分隔符路径）");
        ck_eq(p.title,  "不夜城 Night Fall", "全1 title（曲号剥掉、尾部全角组摘除）");
        ck_eq(p.feat,   "冬名",              "全1 feat（组内 feat. 标记同样分类）");
        ck_eq(p.version, "",                 "全1 version 为空");
        ck(p.confidence < 0.5f,              "全1 confidence 弱路径（无分隔符不加成）",
           std::to_string(p.confidence));
    }
    {
        std::printf("- 全2 全角括号副题进 version\n");
        const ParsedName p = parse_filename("砂糖星辰（Song for ANK & Kumako）");
        ck_eq(p.artist, "",                       "全2 artist 为空（无分隔符路径）");
        ck_eq(p.title,  "砂糖星辰",               "全2 title（尾部全角组摘除）");
        ck_eq(p.version, "Song for ANK & Kumako", "全2 version（组内无 feat 标记 → 进 version）");
        ck_eq(p.feat,   "",                       "全2 feat 为空");
        ck(p.confidence < 0.5f,                   "全2 confidence 弱路径（无分隔符不加成）",
           std::to_string(p.confidence));
    }
    {
        std::printf("- 全3 回归：artist 段的全角括号组不摘\n");
        const ParsedName p = parse_filename("人声音质（甜系女声）- 恋文");
        ck_eq(p.artist, "人声音质（甜系女声）", "全3 artist 整体保留（artist 段不做尾部摘组）");
        ck_eq(p.title,  "恋文",                "全3 title");
        ck_conf(p.confidence, 0.8f,            "全3 confidence=0.8（回退切分同基准）");
    }

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
