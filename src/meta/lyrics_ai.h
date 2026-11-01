// lyrics_ai.h — 歌词 AI 处理（翻译 / 音译 / 日语假名标注）
//
// 产品需求（对标 QQ 音乐的歌词能力）：一首歌的歌词要能翻译成目标语言、能音译成
// 拉丁字母、日语歌要给汉字标假名。三条路都走已有的 llm_chat（meta/llm.h），
// 结果必须落盘缓存 —— 同一首歌同一语言反复打开播放页时不能再花一次钱。
//
// 三条硬约束（tests/test_lyrics_ai.cpp 逐条锁死）：
//   ① 行数守恒：界面是按行号把结果贴回原文的，少一行就是整首歌错位；
//   ② 空行保留：模型几乎总会吞掉空行，所以只送非空行、回来按原下标插回；
//   ③ 截断不交付：finish_reason=="length" 时宁可报错，也不返回半首歌
//      （半首歌贴上去，用户会以为歌词本来就这么短，比报错更难发现）。
//
// 缓存目录不在这里拼：本模块不知道进程装在哪（rtaweb 与单测的工作目录不同），
// 目录一律由调用方传入（rtaweb 传 <exe 目录>\data\lyrics-cache），传空串 = 不缓存。
#pragma once
#include "meta/llm.h"    // LlmConfig / LlmMessage / LlmReply / llm_chat / llm_esc / llm_trim
#include "net/json.h"    // json_parse_strict

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace rta::meta {

// ---------------------------------------------------------------------------
// 任务与结果
// ---------------------------------------------------------------------------

// 枚举值参与缓存 key（见 lyrics_cache_key），只能往后追加：
// 改旧值等于让历史缓存全部对不上号，用户会白花一次钱重算。
enum class LyricsAiTask {
    Translate,      // 翻译成目标语言
    Transliterate,  // 音译（拉丁字母转写：中文→拼音、日文→罗马字、俄文→拉丁）
    Furigana,       // 日语假名标注：给汉字标读音，输出「漢字(かんじ)」这种行内形式
    // 2026-10-30（用户 261029-03 #5「显示繁体歌词」）：简体 → 繁体。
    // 为什么也走大模型而不是内置码表：一简对多繁（发/髮/發、干/乾/幹）要按词义选字，
    //   码表只能给最常见的那一个，歌词里「头发」「发展」会一起变成同一个字。
    // 追加在末尾而不是插在中间：枚举值参与缓存 key，改动旧值会让历史缓存全部失效。
    TradChinese
};

struct LyricsAiResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> lines;   // 与输入逐行对应（行数必须相等）
    int tokens = 0;
    bool fromCache = false;
};

// 每行约 40 token（译文通常比原文长一截）+ 200 余量。
// 上限 8000 是兼容端点的常见单次响应天花板，再大也只会在服务端被截断。
// 2026-10-30 实测修正：40/200 在 deepseek-flash 上 3 行就被截断（finish_reason=length，
// max_tokens=320）。真因是这类模型会先吐一段**推理/思考**再给 JSON，思考 token 也计入
// max_tokens ⟹ 预算必须按"每行译文 + 每行可能附带的思考"来估，不能只按译文长度。
// 150 是实测能稳定跑完 3~40 行的值；再大就撞 8000 上限（见 kLyricsAiMaxTokens）。
// 轻任务（翻译 / 繁体）：实测 3 行 575 token，但单行请求波动很大（1 行 650 仍被截断），
// 所以按 300/行 + 800 余量给；配合 rtaweb 的 20 行切块（20×300+800=6800 < 8000）。
inline constexpr int kLyricsAiTokensPerLine = 300;
inline constexpr int kLyricsAiTokenMargin = 800;
// 重任务（假名标注 / 音译）：模型要先逐字判断读音再改写，推理长度远超译文本身。
// 500/行 是实测 440 再留一成余量；8000 上限 ⟹ 单块最多 14 行（见 rtaweb 的切块）。
inline constexpr int kLyricsAiTokensPerLineHeavy = 500;
inline constexpr int kLyricsAiTokenMarginHeavy = 800;
inline constexpr int kLyricsAiMaxTokens = 8000;
// 缓存格式版本：改提示词或改解析规则时 +1。旧缓存是旧提示词的产物，
// 与当前语义不可互换，宁可删掉重算。
inline constexpr int kLyricsAiCacheVersion = 1;
// 解析失败时回显的原文长度（字节）。200 足够看出模型回了什么，又不至于把日志撑爆。
inline constexpr size_t kLyricsAiSnippetBytes = 200;
// Markdown 代码围栏（三个反引号）。写成常量是因为它本身含有反引号，
// 在源码里散落多处时很难一眼核对是不是三个。
inline constexpr const char* kLyricsAiFence = "```";

// ---------------------------------------------------------------------------
// 任务 / 语言 的名字（提示词里给模型看的中文名）
// ---------------------------------------------------------------------------

inline std::string lyrics_ai_task_tag(LyricsAiTask t) {
    switch (t) {
        case LyricsAiTask::Translate:     return "translate";
        case LyricsAiTask::Transliterate: return "transliterate";
        case LyricsAiTask::Furigana:      return "furigana";
        case LyricsAiTask::TradChinese:   return "tradchinese";
    }
    return "unknown";
}

inline std::string lyrics_ai_task_name(LyricsAiTask t) {
    switch (t) {
        case LyricsAiTask::Translate:     return "翻译";
        case LyricsAiTask::Transliterate: return "音译";
        case LyricsAiTask::Furigana:      return "日语假名标注";
        case LyricsAiTask::TradChinese:   return "简体转繁体";
    }
    return "未知任务";
}

// 语言码归一化：只用于比较与拼 key（"ZH" 与 "zh" 是同一门语言，
// 不归一化会让同一首歌在两种写法下各存一份缓存、各花一次钱）。
inline std::string lyrics_ai_norm_lang(const std::string& s) {
    std::string o = llm_trim(s);
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return o;
}

inline std::string lyrics_ai_lang_name(const std::string& code) {
    const std::string c = lyrics_ai_norm_lang(code);
    if (c.empty()) return "自动判断";
    if (c == "zh" || c.rfind("zh-", 0) == 0) return "中文";
    if (c == "ja" || c.rfind("ja-", 0) == 0) return "日语";
    if (c == "en" || c.rfind("en-", 0) == 0) return "英语";
    if (c == "ko" || c.rfind("ko-", 0) == 0) return "韩语";
    if (c == "fr") return "法语";
    if (c == "de") return "德语";
    if (c == "ru") return "俄语";
    if (c == "es") return "西班牙语";
    if (c == "ar") return "阿拉伯语";
    if (c == "it") return "意大利语";
    if (c == "pt") return "葡萄牙语";
    // 表里没有的短码原样交给模型：BCP-47 的组合远多于这张表，模型自己认得。
    return code;
}

// 一行歌词里若混入换行（CUE 合并行、LRC 里塞了两句），编号列表会被拆散，
// 模型就对不上"第 i 行"了 —— 送模型前压成空格。
inline std::string lyrics_ai_flatten(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    return o;
}

// 按字节截断时退到 UTF-8 字符边界：截在半个汉字上，错误信息本身就是非法 UTF-8，
// 界面显示会变成乱码（这行错误是给人定位用的，不能先坏掉）。
inline std::string lyrics_ai_clip_utf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0u) == 0x80u) --n;
    return s.substr(0, n);
}

// 目录/文件名一律按 UTF-8 解释：rtaweb 的 path_in_exe_dir 返回 UTF-8，
// 安装目录含中文时窄字符 path 会按 ANSI 代码页解释而打不开
// （net/upnp.h 的取流 404 事故就是这个，后来改 _wfopen 才好）。
inline std::filesystem::path lyrics_ai_u8path(const std::string& s) {
#if defined(__cpp_lib_char8_t)
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::u8path(s);
#endif
}

// ---------------------------------------------------------------------------
// 提示词
//
// 格式纪律写进 system、内容与编号列表写进 user：
// 模型对"最后一段用户消息"的服从度最高，把任务描述放在那里，任务相关的偏差最少；
// 而"只输出 JSON"是跨任务的不变量，放 system 才不会被任务描述挤掉。
// ---------------------------------------------------------------------------

inline std::string lyrics_ai_system_prompt(LyricsAiTask task, int n) {
    std::ostringstream o;
    o << "你是歌词处理引擎，只做逐行的机械改写，不创作、不解释。\n"
      << "输出格式（必须严格遵守）：\n"
      << "- 只输出一个 JSON 数组，元素个数必须等于 " << n << "；\n"
      << "- 不要输出任何解释、前言或结语，不要给元素编号，不要使用 Markdown 代码围栏；\n"
      << "- 数组第 i 个元素对应输入的第 i 行，不要合并、拆分、增删或调换顺序；\n"
      << "- 每个元素必须是字符串；输入已剔除空行，所以不要输出空字符串。\n"
      << "任务：" << lyrics_ai_task_name(task) << "。";
    return o.str();
}

inline std::string lyrics_ai_task_instruction(LyricsAiTask task, const std::string& targetLang) {
    switch (task) {
        case LyricsAiTask::Translate:
            return "把每一行歌词翻译成" + lyrics_ai_lang_name(targetLang) +
                   "：保持原意、语气与分行，不要合并或拆分句子，不要添加原文没有的内容。";
        case LyricsAiTask::Transliterate:
            return "把每一行歌词转写成拉丁字母（音译，不翻译语义）：中文转汉语拼音，"
                   "日文转罗马字（Hepburn），俄文转拉丁字母；保留原有标点与语气词。";
        case LyricsAiTask::Furigana:
            return "给每一行歌词里的日语汉字标注平假名读音，写成「漢字(かんじ)」的行内形式；"
                   "假名与拉丁字母原样保留，不要翻译成中文或英文，不要加注释。";
        case LyricsAiTask::TradChinese:
            return "把每一行歌词从简体中文转成繁体中文（台湾/香港通行写法）：只换字形，"
                   "不改词句、不翻译、不改标点；一简对多繁按词义选字（头发→頭髮、发展→發展、"
                   "干净→乾淨）。本来就是繁体的行原样返回。";
    }
    return std::string();
}

inline std::string lyrics_ai_target_desc(LyricsAiTask task, const std::string& targetLang) {
    switch (task) {
        case LyricsAiTask::Translate: {
            const std::string name = lyrics_ai_lang_name(targetLang);
            const std::string code = lyrics_ai_norm_lang(targetLang);
            return (code.empty() || code == name) ? name : name + "（" + code + "）";
        }
        case LyricsAiTask::Transliterate: return "拉丁字母转写（拼音 / 罗马字 / 拉丁化）";
        case LyricsAiTask::Furigana:      return "日语假名（平假名行内注音）";
        case LyricsAiTask::TradChinese:   return "繁体中文（正体字）";
    }
    return targetLang;
}

inline std::string lyrics_ai_user_prompt(LyricsAiTask task, const std::vector<std::string>& payload,
                                         const std::string& sourceLang, const std::string& targetLang) {
    std::ostringstream o;
    o << "任务：" << lyrics_ai_task_instruction(task, targetLang) << "\n";
    o << "源语言：" << lyrics_ai_lang_name(sourceLang) << "\n";
    o << "目标语言：" << lyrics_ai_target_desc(task, targetLang) << "\n";
    o << "共 " << payload.size() << " 行（编号只为对齐，输出里不要带编号）：\n";
    for (size_t i = 0; i < payload.size(); ++i) {
        o << (i + 1) << ". " << lyrics_ai_flatten(payload[i]) << "\n";
    }
    o << "请输出 JSON 数组，共 " << payload.size() << " 个元素。";
    return o.str();
}

// 逐字段赋值而不是 {"role", "content"} 聚合初始化：LlmMessage 还有
// toolCallsJson / toolCallId 两个字段，聚合写法会触发 -Wmissing-field-initializers。
inline LlmMessage lyrics_ai_message(const char* role, const std::string& content) {
    LlmMessage m;
    m.role = role;
    m.content = content;
    return m;
}

// ---------------------------------------------------------------------------
// 响应解析（容错但不将就）
// ---------------------------------------------------------------------------

// 从 JSON 里取"字符串数组"。顶层数组直接用；顶层对象则认 lines 字段，
// 或者唯一的一个数组字段 —— json_object 模式下部分兼容端点只允许顶层对象，
// 坚持"非数组即失败"会把本来完全可用的结果判死。
inline bool lyrics_ai_json_to_lines(const rta::net::Json& j, std::vector<std::string>& out,
                                    std::string* err) {
    const rta::net::Json* arr = nullptr;
    if (j.type == rta::net::Json::Arr) {
        arr = &j;
    } else if (j.type == rta::net::Json::Obj) {
        if (j.has("lines") && j["lines"].type == rta::net::Json::Arr) {
            arr = &j["lines"];
        } else {
            const rta::net::Json* only = nullptr;
            size_t count = 0;
            for (const auto& kv : j.obj) {
                if (kv.second.type != rta::net::Json::Arr) continue;
                only = &kv.second;
                ++count;
            }
            if (count == 1) arr = only;
        }
    }
    if (!arr) {
        if (err) *err = "顶层既不是数组，也找不到唯一的歌词数组字段";
        return false;
    }
    std::vector<std::string> tmp;
    tmp.reserve(arr->size());
    for (size_t i = 0; i < arr->size(); ++i) {
        const rta::net::Json& e = arr->at(i);
        if (e.type != rta::net::Json::Str) {
            if (err) *err = "第 " + std::to_string(i + 1) + " 个元素不是字符串";
            return false;
        }
        tmp.push_back(e.str);
    }
    out.swap(tmp);
    return true;
}

// 三层解析：原文 → 剥 Markdown 代码围栏 → 取第一个 [ 到最后一个 ]。
// 为什么必须容错：模型经常把数组套在代码围栏里，或在数组前后写"这是结果："；
// 直接按纯 JSON 解析会把完全正确的译文判成失败，用户看到的是"AI 出错了"。
inline bool lyrics_ai_extract_lines(const std::string& raw, std::vector<std::string>& out,
                                    std::string* err) {
    const std::string text = llm_trim(raw);
    rta::net::Json j;
    std::string why;
    std::string perr;

    // ① 原文本身就是纯 JSON（json_object 模式下的正常返回）。
    if (rta::net::json_parse_strict(text, j, &perr)) {
        std::string serr;
        if (lyrics_ai_json_to_lines(j, out, &serr)) return true;
        why = serr;
    } else {
        why = perr;
    }

    // ② 剥围栏：从第一行围栏之后取到最后一个围栏。
    const std::string fence = kLyricsAiFence;
    const size_t at = text.find(fence);
    if (at != std::string::npos) {
        const size_t nl = text.find('\n', at);
        if (nl != std::string::npos) {
            std::string body = text.substr(nl + 1);
            const size_t close = body.rfind(fence);
            if (close != std::string::npos) body = body.substr(0, close);
            body = llm_trim(body);
            if (!body.empty() && rta::net::json_parse_strict(body, j, &perr)) {
                std::string serr;
                if (lyrics_ai_json_to_lines(j, out, &serr)) return true;
                why = serr;
            } else {
                why = perr;
            }
        }
    }

    // ③ 取第一个 [ 到最后一个 ]：前后夹闲话时这是最后一招。
    const size_t lb = text.find('[');
    const size_t rb = text.rfind(']');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
        const std::string sub = text.substr(lb, rb - lb + 1);
        if (rta::net::json_parse_strict(sub, j, &perr)) {
            std::string serr;
            if (lyrics_ai_json_to_lines(j, out, &serr)) return true;
            why = serr;
        } else {
            why = perr;
        }
    }

    if (err) {
        const std::string snip = lyrics_ai_flatten(lyrics_ai_clip_utf8(text, kLyricsAiSnippetBytes));
        *err = "模型返回的内容不是合法 JSON 数组" +
               (why.empty() ? std::string() : "（" + why + "）") +
               "；原文摘要（最多 " + std::to_string(kLyricsAiSnippetBytes) + " 字符）：" + snip;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 缓存 key / 读写
// ---------------------------------------------------------------------------

// FNV-1a 64（offset basis 0xcbf29ce484222325、prime 0x100000001b3）。
// 为什么自己写：key 只需要"同输入必同 key、不同输入几乎不同 key"，不需要密码学强度；
// 为一次哈希引入新依赖（SHA/xxhash）不值得，项目里也没有现成的。
inline unsigned long long lyrics_ai_fnv1a64(const std::string& s) {
    unsigned long long h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= (unsigned long long)c;
        h *= 1099511628211ull;
    }
    return h;
}

// key = 任务 + 目标语言 + 源语言 + 全部行的内容（题目约定的口径）。
inline std::string lyrics_cache_key(LyricsAiTask task, const std::string& srcLang,
                                    const std::string& dstLang, const std::vector<std::string>& lines) {
    std::string m;
    m.reserve(64 + lines.size() * 24);
    m += "rta-lyrics-ai|";
    m += std::to_string(kLyricsAiCacheVersion);
    m += '|';
    m += lyrics_ai_task_tag(task);
    m += '|';
    m += lyrics_ai_norm_lang(srcLang);
    m += '|';
    m += lyrics_ai_norm_lang(dstLang);
    // 每行带长度前缀：否则 ["ab","c"] 与 ["a","bc"] 会拼成同一串，命中别人的译文。
    for (const std::string& l : lines) {
        m += '\x1f';
        m += std::to_string(l.size());
        m += ':';
        m += l;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", lyrics_ai_fnv1a64(m));
    return std::string(buf);
}

// 同一进程里 rtaweb 的多个请求线程可能同时处理同一首歌（用户连点两次"翻译"）：
// 序列化读写，避免两条线程同时写同一个 .part 把文件写成半截。
inline std::mutex& lyrics_ai_cache_mutex() {
    static std::mutex m;
    return m;
}

inline std::filesystem::path lyrics_ai_cache_file(const std::string& dir, const std::string& key) {
    return lyrics_ai_u8path(dir) / (key + ".json");
}

inline std::string lyrics_ai_cache_json(const LyricsAiResult& r) {
    std::ostringstream o;
    o << "{\"v\":" << kLyricsAiCacheVersion
      << ",\"tokens\":" << r.tokens
      << ",\"lines\":[";
    for (size_t i = 0; i < r.lines.size(); ++i) {
        if (i) o << ',';
        o << '"' << llm_esc(r.lines[i]) << '"';
    }
    o << "]}";
    return o.str();
}

// 先写 .part 再 rename：进程被杀/断电时最多留下一个 .part，绝不会让半截 JSON
// 变成正式缓存 —— 半截文件有时仍是"合法 JSON"，比读不出来更危险。
inline bool lyrics_cache_save(const std::string& dir, const std::string& key, const LyricsAiResult& r) {
    // 失败结果不入缓存：断网/超限/解析失败都是临时状态，缓存下来会让用户
    // 永远看不到正确歌词（而界面上并没有"清缓存重算"这个入口）。
    if (dir.empty() || key.empty() || !r.ok) return false;

    const std::string json = lyrics_ai_cache_json(r);
    std::lock_guard<std::mutex> lk(lyrics_ai_cache_mutex());

    std::error_code ec;
    const std::filesystem::path d = lyrics_ai_u8path(dir);
    std::filesystem::create_directories(d, ec);      // 首次调用时目录通常还不存在

    const std::filesystem::path target = d / (key + ".json");
    const std::filesystem::path part = d / (key + ".json.part");
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(json.data(), (std::streamsize)json.size());
        f.flush();
        if (!f.good()) {
            f.close();
            std::error_code rm;
            std::filesystem::remove(part, rm);
            return false;
        }
    }   // 句柄必须先关：Windows 上目标被打开时 rename 会失败
    std::filesystem::rename(part, target, ec);
    if (ec) {
        // 有些平台/文件系统上 rename 不覆盖已存在文件，删掉目标再试一次。
        std::error_code ec2;
        std::filesystem::remove(target, ec2);
        std::filesystem::rename(part, target, ec2);
        if (ec2) {
            std::error_code rm;
            std::filesystem::remove(part, rm);
            return false;
        }
    }
    return true;
}

inline bool lyrics_cache_load(const std::string& dir, const std::string& key, LyricsAiResult& out) {
    if (dir.empty() || key.empty()) return false;

    std::lock_guard<std::mutex> lk(lyrics_ai_cache_mutex());
    const std::filesystem::path target = lyrics_ai_cache_file(dir, key);
    std::error_code ec;
    if (!std::filesystem::exists(target, ec) || ec) return false;

    std::string text;
    {
        std::ifstream f(target, std::ios::binary);
        if (!f) return false;
        text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    rta::net::Json j;
    std::string perr;
    std::vector<std::string> lines;
    bool good = rta::net::json_parse_strict(text, j, &perr) && j.type == rta::net::Json::Obj;
    if (good && (!j.has("v") || (int)j["v"].n() != kLyricsAiCacheVersion)) good = false;
    if (good && (!j.has("lines") || j["lines"].type != rta::net::Json::Arr)) good = false;
    if (good) {
        for (size_t i = 0; i < j["lines"].size(); ++i) {
            const rta::net::Json& e = j["lines"].at(i);
            if (e.type != rta::net::Json::Str) { good = false; break; }
            lines.push_back(e.str);
        }
    }
    if (!good) {
        // 损坏文件必须删掉：留着会让每次打开播放页都读到同一份坏数据，
        // 而"删掉重算"是本模块唯一能自愈的路径（上层没有清缓存的入口）。
        std::error_code rm;
        std::filesystem::remove(target, rm);
        return false;
    }

    out.ok = true;
    out.fromCache = true;
    out.error.clear();
    out.lines.swap(lines);
    // tokens 保留当初的实际消耗（fromCache 说明本次没花钱）：
    // 统计"这首歌的 AI 歌词总共花了多少"时才有依据，累加时要先看 fromCache。
    out.tokens = (int)j["tokens"].n();
    return true;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
//
// cacheDir：缓存目录（UTF-8）。空串 = 不缓存。主控在 rtaweb 里传
// <exe 目录>\data\lyrics-cache；测试传临时目录。做成参数而不是全局变量：
// 同一进程里可能有多个数据目录（便携版/测试），全局状态会让它们互相串味。
inline LyricsAiResult lyrics_ai_process(const LlmConfig& cfg, LyricsAiTask task,
                                        const std::vector<std::string>& lines,
                                        const std::string& sourceLang,
                                        const std::string& targetLang,
                                        const std::string& cacheDir = std::string()) {
    LyricsAiResult r;

    // 没 key 就不发请求：未配置时必须快速失败，界面才好提示"去设置里填 key"。
    if (cfg.apiKey.empty()) {
        r.error = "未配置 api_key";
        return r;
    }
    // 目标语言是翻译的唯一必需参数；空着发出去只会让模型自己猜，白花钱。
    if (task == LyricsAiTask::Translate && llm_trim(targetLang).empty()) {
        r.error = "翻译任务缺少目标语言（targetLang 为空）";
        return r;
    }

    const std::string key = lyrics_cache_key(task, sourceLang, targetLang, lines);
    if (!cacheDir.empty() && lyrics_cache_load(cacheDir, key, r)) {
        r.fromCache = true;      // 命中就不发请求：这正是"同一首歌不重复花钱"的实现
        return r;
    }

    // 只把非空行送模型，并记下它们原来的下标。模型几乎总会把空行吞掉，
    // 直接送会让返回行数少于输入行数，界面就整首歌错位。
    std::vector<size_t> slots;
    std::vector<std::string> payload;
    slots.reserve(lines.size());
    payload.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (llm_trim(lines[i]).empty()) continue;   // 纯空白也算空行
        slots.push_back(i);
        payload.push_back(lines[i]);
    }

    // 全是空行（纯间奏/纯器乐）：没有要处理的内容，不值得花一次调用。
    if (payload.empty()) {
        r.ok = true;
        r.lines.assign(lines.size(), std::string());
        if (!cacheDir.empty() && !lines.empty()) lyrics_cache_save(cacheDir, key, r);
        return r;
    }

    // 按行数估 max_tokens：估小了长歌词会被截断（下面的 length 检查会报错，
    // 但用户白等一次）；估大了只是浪费上限，所以宁可宽一点。
    // 2026-10-30 实测：不同任务的"思考开销"差一个数量级 ——
    //   翻译/繁体：3 行 575~1041 token（含推理），每行约 150 够用；
    //   假名标注：3 行实测 completion=1328（推理 3631 字符），max_tokens=2000 仍被截断，
    //            要 4000 才收尾 ⟹ 每行约 440。
    //   所以预算必须按任务给，否则假名/音译永远 length。
    const bool heavy = (task == LyricsAiTask::Furigana || task == LyricsAiTask::Transliterate);
    const int perLine = heavy ? kLyricsAiTokensPerLineHeavy : kLyricsAiTokensPerLine;
    const int margin  = heavy ? kLyricsAiTokenMarginHeavy : kLyricsAiTokenMargin;
    const long long est = (long long)margin + (long long)perLine * (long long)payload.size();
    const int maxTokens = (int)std::min<long long>(kLyricsAiMaxTokens, est);

    std::vector<LlmMessage> msgs;
    msgs.push_back(lyrics_ai_message("system", lyrics_ai_system_prompt(task, (int)payload.size())));
    msgs.push_back(lyrics_ai_message("user", lyrics_ai_user_prompt(task, payload, sourceLang, targetLang)));

    // 2026-10-30 实测（deepseek-flash）：jsonObject=true（response_format=json_object）与
    //   system 里"只输出 JSON **数组**"冲突 —— json_object 要求顶层是对象，模型在两种约束下
    //   反复权衡，3 行歌词就烧掉 950 token 并 finish_reason=length（实测两次都截断）。
    //   直接调同样的提示词、不开 response_format，215 token 就正常收尾。
    //   解析侧本来就容错（剥围栏 + 取首个 '[' 到末个 ']'），所以关掉它零损失。
    const LlmReply reply = llm_chat(cfg, msgs, maxTokens, 0.2f, /*jsonObject=*/false);
    if (!reply.ok) {
        r.error = reply.error.empty() ? std::string("模型调用失败（未给出原因）") : reply.error;
        return r;
    }
    if (reply.finishReason == "length") {
        r.error = "模型输出被截断（finish_reason=length，max_tokens=" + std::to_string(maxTokens) +
                  "）：为避免只返回半首歌，本次结果丢弃";
        return r;
    }
    if (llm_trim(reply.content).empty()) {
        r.error = "模型返回了空内容（finish_reason=" +
                  (reply.finishReason.empty() ? std::string("空") : reply.finishReason) + "）";
        return r;
    }

    std::vector<std::string> got;
    std::string perr;
    if (!lyrics_ai_extract_lines(reply.content, got, &perr)) {
        r.error = perr;
        return r;
    }
    if (got.size() != payload.size()) {
        // 不"凑合对齐"：少一行就整首歌错位，多一行则后面全串。
        r.error = "模型返回的行数与输入不符：送检 " + std::to_string(payload.size()) +
                  " 行（输入共 " + std::to_string(lines.size()) + " 行，空行已剔除），返回 " +
                  std::to_string(got.size()) + " 行；为避免界面歌词错行，本次结果丢弃";
        return r;
    }

    r.ok = true;
    r.tokens = reply.totalTokens;
    r.lines.assign(lines.size(), std::string());     // 空行位置保持空串
    for (size_t k = 0; k < got.size(); ++k) r.lines[slots[k]] = got[k];

    if (!cacheDir.empty()) lyrics_cache_save(cacheDir, key, r);   // 写缓存失败不影响本次结果
    return r;
}

}  // namespace rta::meta
