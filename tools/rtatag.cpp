// rtatag.cpp — 元数据工具 CLI（docs/元数据补全调研与规划.md §3 P0 的 ①/⑤ 两级 + LLM smoke）
//
// 子命令：
//   rtatag parse <文件...> [--llm]       ffprobe 现有标签 + 标签透传 + 文件名规则解析
//                                        （--llm 时走 DeepSeek 兜底；不带 --llm 零网络请求）
//   rtatag store show <文件>             打印 MetaStore 中该曲目的全部字段（value/source/ts/confidence）
//   rtatag store set <文件> <字段> <值>   以 source=user 合并字段，打印合并前后并原子落盘
//   rtatag write <文件...> [--apply] [--no-backup]
//                                        把 MetaStore 字段写回文件标签（ffmpeg remux +
//                                        PCM 逐位门禁）。默认 dry-run（全流程校验但不落盘）；
//                                        --apply 才真写；--no-backup 跳过备份。
//   rtatag write --library [--apply]      整库写回（有 MetaStore 记录的曲目），输出
//                                        written/skipped/failed 与失败队列文件路径
//   rtatag llm-smoke                     DeepSeek chat → search → fetch 三步连通性自检
//
// 纪律：
//   * 只读写文件与元数据，不打开任何音频设备（不发声）；
//   * api_key 只从 rlplayer-llm.conf 读、只进 Authorization 头，绝不打印、绝不写进任何文件；
//   * parse 不带 --llm 时除 ffprobe（本地进程）外零网络请求。
//
// 说明：用宽字符入口 wmain + wide_to_utf8() 把命令行参数统一转成项目约定的 UTF-8，
//       中文路径不经过 ANSI 代码页（GBK）。实测窄字符入口在本机（zh-CN 代码页 936）上
//       中文路径会变成 GBK 字节，下游按 UTF-8 解释时 MSVC 的 fs::path(u8string) 直接
//       抛 invalid_argument（未捕获异常，退出码 127）——因此这里必须用宽字符入口。
//       工具链证据：wv2_probe.cpp 同样用 wmain，本项目的 clang++/lld-link 能正确选择入口。
#include "meta/store.h"
#include "meta/parse.h"
#include "meta/llm.h"
#include "meta/enrich.h"
#include "meta/writeback.h"
#include "meta/knowledge.h"
#include "meta/retrieval.h"
#include "audio/media_info.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

using namespace rta::meta;
namespace fsx = std::filesystem;

static void usage() {
    printf("用法:\n"
           "  rtatag parse <文件...> [--llm]        现有标签 + 文件名规则解析（--llm 加 DeepSeek 兜底）\n"
           "  rtatag enrich <文件...> [--force] [--no-llm]\n"
           "                                        单曲元数据补全（搜索→匹配→融合→落盘,含封面/歌词）\n"
           "  rtatag store show <文件>              打印该曲目在 MetaStore 里的全部字段\n"
           "  rtatag store set <文件> <字段> <值>   以 user 来源合并字段（打印合并前后）\n"
           "  rtatag write <文件...> [--apply] [--no-backup]\n"
           "                                        MetaStore 字段写回文件标签（默认 dry-run;--apply 真写;\n"
           "                                        PCM 逐位门禁,不一致绝不动原文件;原文件进 <store>/backups）\n"
           "  rtatag write --library [--apply]      整库写回（打印 written/skipped/failed 与失败队列路径）\n"
           "  rtatag llm-smoke                      DeepSeek chat/search/fetch 连通性自检（需要 api_key）\n"
           "  rtatag knowledge --kind <kind> --name <name> [--lang zh] [--refresh] [--json] [--metabase <dir>]\n"
           "                                        查询实体权威文本材料（维基/萌娘百科 + 本地缓存/离线回退）\n"
           "  rtatag summary --kind <kind> --name <name> [--lang zh] [--refresh] [--json] [--metabase <dir>]\n"
           "                                        查询实体检索综合摘要（L2+L3检索综合 + 引用溯源纪律）\n");
}

// ---------------- 小工具 ----------------

static double ms_since(const std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static long long now_ts() { return (long long)std::time(nullptr); }

// 取前 n 个字节，回退到 UTF-8 序列边界再截断（不切坏中文）
static std::string first_n(const std::string& s, const size_t n) {
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) --k;
    return s.substr(0, k) + "…";
}

// Unix 秒 -> 可读时间（UTC）；<=0 显示 -
static std::string fmt_ts(const long long ts) {
    if (ts <= 0) return "-";
    const std::time_t t = (std::time_t)ts;
    std::tm tmv {};
#ifdef _WIN32
    if (gmtime_s(&tmv, &t) != 0) return "-";
#else
    if (gmtime_r(&t, &tmv) == nullptr) return "-";
#endif
    char b[40];
    if (std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S UTC", &tmv) == 0) return "-";
    return b;
}

// 把换行/制表等变成可见转义，保证逐行打印的版式不被字段值打断
static std::string oneline(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:   o += (char)c;
        }
    }
    return o;
}

// ---------------- parse ----------------

static void print_parsed(const ParsedName& p, const char* indent) {
    printf("%sartist : %s\n", indent, p.artist.c_str());
    printf("%stitle  : %s\n", indent, p.title.c_str());
    printf("%sfeat   : %s\n", indent, p.feat.c_str());
    printf("%sversion: %s\n", indent, p.version.c_str());
}

static int cmd_parse(const int argc, char** argv) {
    std::vector<std::string> files;
    bool useLlm = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--llm") useLlm = true;
        else files.push_back(a);
    }
    if (files.empty()) { usage(); return 2; }

    // ffprobe 缺失时如实提示（probe_media 对空工具直接返回空结构，不报错）
    const bool haveProbe = !rta::audio::find_tool("ffprobe").empty();
    if (!haveProbe) printf("[提示] 找不到 ffprobe，跳过现有标签读取\n");

    LlmConfig cfg;
    if (useLlm) {
        cfg = llm_config_load();
        if (cfg.apiKey.empty())
            printf("[提示] rlplayer-llm.conf 没有配置 api_key，跳过 LLM 步骤（其余照常，无网络请求）\n");
    }

    int rc = 0;
    for (const std::string& file : files) {
        printf("== %s\n", file.c_str());

        // 1) ffprobe 现有标签
        if (haveProbe) {
            const rta::audio::MediaInfo mi = rta::audio::probe_media(file);
            printf("  [ffprobe] %s / %d Hz / %d bit / %d ch / %.1f 秒\n",
                   mi.codec.empty() ? "?" : mi.codec.c_str(),
                   mi.sampleRate, mi.bits, mi.channels, mi.duration);
            const bool anyTag = !mi.title.empty() || !mi.artist.empty() || !mi.album.empty() ||
                                !mi.date.empty()  || !mi.genre.empty()  || !mi.track.empty();
            if (anyTag) {
                printf("  [现有标签]\n");
                if (!mi.title.empty())  printf("    title : %s\n", mi.title.c_str());
                if (!mi.artist.empty()) printf("    artist: %s\n", mi.artist.c_str());
                if (!mi.album.empty())  printf("    album : %s\n", mi.album.c_str());
                if (!mi.date.empty())   printf("    date  : %s\n", mi.date.c_str());
                if (!mi.genre.empty())  printf("    genre : %s\n", mi.genre.c_str());
                if (!mi.track.empty())  printf("    track : %s\n", mi.track.c_str());
            } else {
                printf("  [现有标签] （无标签）\n");
            }

            // 2) 标签透传（有标签时才有意义）
            if (!mi.artist.empty() || !mi.title.empty()) {
                const ParsedName t = parse_from_tags(mi.artist, mi.title);
                printf("  [标签解析] method=%s confidence=%.2f\n", t.method.c_str(), t.confidence);
                print_parsed(t, "    ");
            }
        }

        // 3) 文件名规则解析（不含扩展名的文件名）
        const std::string stem = meta_string_of(meta_path_of(file).stem());
        const ParsedName n = parse_filename(stem);
        printf("  [文件名解析] stem=\"%s\" method=%s confidence=%.2f\n",
               stem.c_str(), n.method.c_str(), n.confidence);
        print_parsed(n, "    ");

        // 4) LLM 兜底（--llm 且有 key 才走；单次调用不重试）
        if (useLlm && !cfg.apiKey.empty()) {
            const std::string prompt = parse_llm_prompt(stem);
            const auto t0 = std::chrono::steady_clock::now();
            const LlmReply rep = llm_chat(cfg, { LlmMessage{ "user", prompt } });
            const double ms = ms_since(t0);
            if (!rep.ok) {
                printf("  [LLM 解析] 失败（%.0f ms）: %s\n", ms, rep.error.c_str());
                rc = 1;
            } else {
                const ParsedName p = parse_llm_parse(rep.content);
                printf("  [LLM 解析] method=%s confidence=%.2f（耗时 %.0f ms，tokens %d）\n",
                       p.method.c_str(), p.confidence, ms, rep.totalTokens);
                print_parsed(p, "    ");
                printf("    原文预览: %s\n", first_n(rep.content, 200).c_str());
            }
        }
    }
    return rc;
}

// ---------------- store show ----------------

static int cmd_store_show(const std::string& file) {
    MetaStore& ms = MetaStore::instance();
    printf("MetaStore base=%s  曲目=%s\n  hash=%s（%s/tracks/%s.json）\n",
           ms.baseDir().c_str(), file.c_str(),
           MetaStore::hashOf(file).c_str(), ms.baseDir().c_str(), MetaStore::hashOf(file).c_str());
    TrackMeta tm;
    if (!ms.load(file, tm)) { printf("无记录\n"); return 0; }
    printf("  path : %s\n", tm.path.c_str());
    printf("  cover: %s\n", tm.coverRelPath.empty() ? "（无）" : tm.coverRelPath.c_str());
    if (tm.fields.empty()) { printf("  （记录存在但没有任何字段）\n"); return 0; }
    for (const auto& kv : tm.fields) {
        const MetaValue& v = kv.second;
        printf("  [%s]\n    value=%s\n    source=%s ts=%lld（%s）confidence=%.2f\n",
               kv.first.c_str(), oneline(v.value).c_str(), v.source.c_str(),
               v.ts, fmt_ts(v.ts).c_str(), v.confidence);
    }
    return 0;
}

// ---------------- store set ----------------

static int cmd_store_set(const std::string& file, const std::string& field, const std::string& value) {
    // 常规键集合（store.h 的约定）；非常规键照写，但提示一句
    static const char* kKnown[] = { "title", "artist", "albumArtist", "album", "year", "genre",
                                    "trackNo", "discNo", "composer", "conductor", "label", "lyrics" };
    bool known = false;
    for (const char* k : kKnown) if (field == k) { known = true; break; }
    if (!known)
        printf("[提示] 字段 \"%s\" 不是常规键（title/artist/albumArtist/album/year/genre/"
               "trackNo/discNo/composer/conductor/label/lyrics），仍按原样写入\n", field.c_str());

    MetaStore& ms = MetaStore::instance();
    TrackMeta tm;
    const bool had = ms.load(file, tm);
    if (!had) tm.path = file;                    // 无记录：新建一条
    printf("store set: %s\n  字段=%s hash=%s（%s）\n",
           file.c_str(), field.c_str(), MetaStore::hashOf(file).c_str(), had ? "已有记录" : "新建记录");

    const auto it = tm.fields.find(field);
    if (it == tm.fields.end())
        printf("  合并前: （无该字段）\n");
    else
        printf("  合并前: value=\"%s\" source=%s ts=%lld confidence=%.2f\n",
               oneline(it->second.value).c_str(), it->second.source.c_str(),
               it->second.ts, it->second.confidence);

    MetaValue v;
    v.value      = value;
    v.source     = "user";
    v.ts         = now_ts();
    v.confidence = 1.0f;
    MetaStore::mergeField(tm, field, v);

    const MetaValue& after = tm.fields[field];
    printf("  合并后: value=\"%s\" source=%s ts=%lld confidence=%.2f\n",
           oneline(after.value).c_str(), after.source.c_str(), after.ts, after.confidence);
    if (after.source != v.source || after.ts != v.ts || after.value != v.value)
        printf("  （合并未生效：按 store.h 的合并规则保留了原字段——优先级/置信度更高，"
               "或同级且时间戳不早于本次）\n");

    ms.save(tm);
    printf("  已保存 %s/tracks/%s.json\n", ms.baseDir().c_str(), MetaStore::hashOf(file).c_str());
    return 0;
}

// ---------------- enrich ----------------

// 打印补全后 MetaStore 关键字段表（值 + 溯源）
static void print_store_brief(const std::string& file) {
    MetaStore& ms = MetaStore::instance();
    TrackMeta tm;
    if (!ms.load(file, tm)) { printf("  [MetaStore] （无记录）\n"); return; }
    printf("  [MetaStore] base=%s hash=%s cover=%s\n",
           ms.baseDir().c_str(), MetaStore::hashOf(file).c_str(),
           tm.coverRelPath.empty() ? "（无）" : tm.coverRelPath.c_str());
    static const char* kKeys[] = { "title", "artist", "albumArtist", "album",
                                   "year", "genre", "lyrics" };
    for (const char* k : kKeys) {
        const auto it = tm.fields.find(k);
        if (it == tm.fields.end() || it->second.value.empty()) {
            printf("    %-11s = （空）\n", k);
            continue;
        }
        const MetaValue& v = it->second;
        // 歌词只打首行与长度，不整段刷屏
        std::string shown = (k == std::string("lyrics"))
            ? first_n(oneline(v.value), 60) + "（" + std::to_string(v.value.size()) + "B）"
            : oneline(v.value);
        printf("    %-11s = %s   [src=%s conf=%.2f]\n", k, shown.c_str(),
               v.source.c_str(), v.confidence);
    }
}

static int cmd_enrich(const int argc, char** argv) {
    std::vector<std::string> files;
    bool force = false;
    EnrichOpts opts;                            // 默认五源全开
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--force")  force = true;
        else if (a == "--no-llm") opts.llm = false;
        else files.push_back(a);
    }
    if (files.empty()) { usage(); return 2; }

    // baseDir 不显式设置：MetaStore 默认 "rlplayer-meta"（相对当前工作目录，
    // 与既有 store show/set 完全一致 —— 跟随 CWD）。
    if (opts.llm && llm_config_load().apiKey.empty())
        printf("[提示] rlplayer-llm.conf 没有配置 api_key，LLM 解析/裁决将自动跳过\n");

    int nFail = 0;
    for (const std::string& file : files) {
        printf("== enrich %s%s\n", file.c_str(), force ? "  (--force)" : "");
        const auto t0 = std::chrono::steady_clock::now();
        const EnrichStats st = enrich_track(file, force, opts);
        printf("  耗时 %.1f s\n", ms_since(t0) / 1000.0);
        if (st.skipped) {
            printf("  [跳过] %s\n", st.note.c_str());
            continue;                            // skip 不是失败
        }
        if (!st.ok) {
            printf("  [失败] %s\n", st.note.c_str());
            ++nFail;
            print_store_brief(file);
            continue;
        }
        printf("  [结果] 候选=%d best=%.2f src=%s confident=%s 新写入=%zu\n",
               st.nCand, st.bestScore, st.bestSource.c_str(),
               st.confident ? "是" : "否", st.fieldsFilled.size());
        printf("  [新写入] %s\n",
               st.fieldsFilled.empty() ? "（无 —— 既有字段优先级/置信度更高，合并未生效）"
                                       : "");
        for (const std::string& f : st.fieldsFilled) printf("    - %s\n", f.c_str());
        printf("  [note] %s\n", st.note.c_str());
        print_store_brief(file);
    }
    return (nFail == (int)files.size()) ? 1 : 0;   // 全部失败 → 1，否则 0
}

// ---------------- write ----------------

// 打印单个写回结果；返回是否算失败（skip 不是失败）
static bool print_writeback_result(const WritebackResult& r) {
    if (r.skipped) {
        printf("  [跳过] %s\n", r.note.c_str());
        return false;
    }
    if (!r.ok) {
        printf("  [失败] %s\n", r.err.c_str());
        return true;
    }
    printf("  [%s] %s\n", r.dryRun ? "dry-run" : "已写回", r.note.c_str());
    if (!r.dryRun)
        printf("  [PCM] 逐字节一致（pcm_s32le 解码比对）\n");
    return false;
}

static int cmd_write(const int argc, char** argv) {
    bool apply = false, library = false;
    WritebackOpts opts;                          // 默认 dry-run + 备份
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--apply")     apply = true;
        else if (a == "--no-backup") opts.backup = false;
        else if (a == "--library")   library = true;
        else files.push_back(a);
    }
    opts.dryRun = !apply;

    if (!apply)
        printf("[dry-run] 默认只校验不写盘；确认输出无误后加 --apply 真写\n");

    // ffmpeg 缺失时如实提示（writeback_file 内部也会报，这里提前说清）
    if (rta::audio::find_tool("ffmpeg").empty())
        printf("[提示] 找不到 ffmpeg：所有需要 remux/PCM 校验的操作都会失败\n");

    MetaStore& ms = MetaStore::instance();

    if (library) {
        std::string errOut;
        const auto t0 = std::chrono::steady_clock::now();
        const BatchResult br = writeback_library(opts, &errOut);
        if (!errOut.empty()) {
            printf("[失败] %s\n", errOut.c_str());
            return 1;
        }
        printf("== 整库写回%s：total=%d written=%d skipped=%d failed=%d（耗时 %.1f s）\n",
               apply ? "（--apply）" : "（dry-run）",
               br.total, br.written, br.skipped, br.failed, ms_since(t0) / 1000.0);
        for (const std::string& f : br.failures)
            printf("  [失败队列] %s\n", f.c_str());
        printf("  [失败队列文件] %s/writeback-failures.json\n", ms.baseDir().c_str());
        return br.failed > 0 ? 1 : 0;
    }

    if (files.empty()) { usage(); return 2; }

    int nFail = 0;
    for (const std::string& file : files) {
        printf("== write %s\n", file.c_str());
        TrackMeta tm;
        if (!ms.load(file, tm) || tm.fields.empty()) {
            printf("  [跳过] MetaStore 无记录或无字段（先 enrich 或 store set）\n");
            continue;
        }
        printf("  [store] base=%s hash=%s 字段=%zu\n",
               ms.baseDir().c_str(), MetaStore::hashOf(file).c_str(), tm.fields.size());
        const WritebackResult r = writeback_file(file, tm, opts);
        if (print_writeback_result(r)) ++nFail;
    }
    return nFail > 0 ? 1 : 0;
}

// ---------------- llm-smoke ----------------

static int cmd_llm_smoke() {
    const LlmConfig cfg = llm_config_load();
    if (cfg.apiKey.empty()) {
        printf("[提示] 未配置 api_key：在 rlplayer-llm.conf 写入 api_key=...（key 只存本地配置，"
               "绝不进源码），跳过 smoke\n");
        return 2;
    }
    printf("端点: %s  模型: %s\n", cfg.baseUrl.c_str(), cfg.model.c_str());
    int rc = 0;
    auto t0 = std::chrono::steady_clock::now();
    double ms = 0;

    // 1) chat
    printf("[1/3] llm_chat「只回复:OK」\n");
    const LlmReply c = llm_chat(cfg, { LlmMessage{ "user", "只回复:OK" } });
    ms = ms_since(t0);
    if (c.ok)
        printf("      ok=1 耗时=%.0f ms tokens=%d 前200字符: %s\n",
               ms, c.totalTokens, first_n(c.content, 200).c_str());
    else { printf("      ok=0 耗时=%.0f ms 错误: %s\n", ms, c.error.c_str()); rc = 1; }

    // 2) search
    printf("[2/3] ds_search「贝多芬 第九交响曲 首演 年份」\n");
    t0 = std::chrono::steady_clock::now();
    const DsSearchReply s = ds_search(cfg, "贝多芬 第九交响曲 首演 年份");
    ms = ms_since(t0);
    if (s.ok) {
        std::string sum;
        for (const DsHit& h : s.hits) {
            if (!sum.empty()) sum += " ;; ";
            sum += h.title + " | " + h.url + " | " + h.snippet;
        }
        if (sum.empty()) sum = "（0 条结果）";
        printf("      ok=1 耗时=%.0f ms 命中=%zu 前200字符: %s\n",
               ms, s.hits.size(), first_n(sum, 200).c_str());
    } else { printf("      ok=0 耗时=%.0f ms 错误: %s\n", ms, s.error.c_str()); rc = 1; }

    // 3) fetch
    printf("[3/3] ds_fetch https://example.com\n");
    t0 = std::chrono::steady_clock::now();
    const DsFetchReply f = ds_fetch(cfg, "https://example.com");
    ms = ms_since(t0);
    if (f.ok)
        printf("      ok=1 耗时=%.0f ms 前200字符: %s\n",
               ms, first_n(f.title + " | " + f.content, 200).c_str());
    else { printf("      ok=0 耗时=%.0f ms 错误: %s\n", ms, f.error.c_str()); rc = 1; }

    return rc;
}

// ---------------- knowledge ----------------

static int cmd_knowledge(const int argc, char** argv) {
    std::string kindStr = "person";
    std::string name;
    std::string lang = "zh";
    bool refresh = false;
    bool json_output = false;
    std::string metabase;
    KnowledgeOpts opts;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--kind" && i + 1 < argc) {
            kindStr = argv[++i];
        } else if (a == "--name" && i + 1 < argc) {
            name = argv[++i];
        } else if (a == "--lang" && i + 1 < argc) {
            lang = argv[++i];
        } else if (a == "--refresh") {
            refresh = true;
        } else if (a == "--json") {
            json_output = true;
        } else if (a == "--metabase" && i + 1 < argc) {
            metabase = argv[++i];
        } else if (a == "--wiki-base" && i + 1 < argc) {
            opts.wiki_api_base = argv[++i];
        } else if (a == "--moegirl-base" && i + 1 < argc) {
            opts.moegirl_api_base = argv[++i];
        } else if (a == "--offline") {
            // 构造离线场景：将端点指向不可达地址并强制 refresh，以验证缓存离线回退
            opts.wiki_api_base = "http://127.0.0.1:8919/unreachable_wiki";
            opts.moegirl_api_base = "http://127.0.0.1:8919/unreachable_moe";
            refresh = true;
            opts.timeout_ms = 1500;
        } else if (name.empty() && a.rfind("--", 0) != 0) {
            name = a; // 容许直接传名：rtatag knowledge ZARD
        }
    }

    if (name.empty()) {
        printf("用法:\n  rtatag knowledge --kind <kind> --name <name> [--lang zh] [--refresh] [--json] [--metabase <dir>]\n");
        return 2;
    }

    EntityKind k = EntityKind::Person;
    if (!kindStr.empty()) {
        if (!entity_kind_from_tag(kindStr, k)) {
            printf("[错误] 未知实体种类: %s（支持 person / group / label / series / work / recording / release / track）\n",
                   kindStr.c_str());
            return 2;
        }
    }

    opts.lang = lang;
    opts.refresh = refresh;
    opts.metabase = metabase;

    const auto t0 = std::chrono::steady_clock::now();
    const KnowledgeResult res = knowledge_get(k, name, opts);
    const double elapsed_s = ms_since(t0) / 1000.0;

    if (json_output) {
        printf("{\n  \"ok\": %s,\n  \"from_cache\": %s,\n  \"offline\": %s,\n  \"materials\": [\n",
               res.materials.empty() ? "false" : "true",
               res.from_cache ? "true" : "false",
               res.offline ? "true" : "false");
        for (size_t i = 0; i < res.materials.size(); ++i) {
            const auto& m = res.materials[i];
            printf("    {\n"
                   "      \"source\": \"%s\",\n"
                   "      \"kind\": \"%s\",\n"
                   "      \"subject\": \"%s\",\n"
                   "      \"title\": \"%s\",\n"
                   "      \"url\": \"%s\",\n"
                   "      \"lang\": \"%s\",\n"
                   "      \"fetched_at\": %lld,\n"
                   "      \"expires_at\": %lld,\n"
                   "      \"authority\": %d,\n"
                   "      \"text\": \"%s\"\n"
                   "    }%s\n",
                   meta_jesc(m.source).c_str(),
                   meta_jesc(m.kind).c_str(),
                   meta_jesc(m.subject).c_str(),
                   meta_jesc(m.title).c_str(),
                   meta_jesc(m.url).c_str(),
                   meta_jesc(m.lang).c_str(),
                   m.fetched_at,
                   m.expires_at,
                   m.authority,
                   meta_jesc(m.text).c_str(),
                   (i + 1 < res.materials.size() ? "," : ""));
        }
        printf("  ],\n  \"error\": \"%s\"\n}\n", meta_jesc(res.error).c_str());
        return res.materials.empty() ? 1 : 0;
    }

    const std::string eid = entity_id(k, name);
    printf("== knowledge: kind=%s name=\"%s\" id=%s  (耗时 %.2f s)\n",
           entity_kind_tag(k), name.c_str(), eid.c_str(), elapsed_s);
    printf("  [状态] from_cache=%s  offline=%s  材料数=%zu\n",
           res.from_cache ? "是" : "否",
           res.offline ? "是" : "否",
           res.materials.size());
    if (!res.error.empty()) {
        printf("  [说明] %s\n", res.error.c_str());
    }
    for (size_t i = 0; i < res.materials.size(); ++i) {
        const auto& m = res.materials[i];
        printf("  [%zu] source=%-14s kind=%-14s authority=%d lang=%s\n",
               i + 1, m.source.c_str(), m.kind.c_str(), m.authority, m.lang.c_str());
        printf("      title: %s\n", m.title.c_str());
        printf("      url:   %s\n", m.url.c_str());
        printf("      time:  %lld (%s)  expires: %lld (%s)\n",
               m.fetched_at, fmt_ts(m.fetched_at).c_str(),
               m.expires_at, fmt_ts(m.expires_at).c_str());
        printf("      text:  %s\n", first_n(m.text, 200).c_str());
    }
    return res.materials.empty() ? 1 : 0;
}

// ---------------- summary ----------------

static int cmd_summary(const int argc, char** argv) {
    std::string kindStr = "person";
    std::string name;
    std::string lang = "zh";
    bool refresh = false;
    bool json_output = false;
    std::string metabase;
    RetrievalOptions opts;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--kind" && i + 1 < argc) {
            kindStr = argv[++i];
        } else if (a == "--name" && i + 1 < argc) {
            name = argv[++i];
        } else if (a == "--lang" && i + 1 < argc) {
            lang = argv[++i];
        } else if (a == "--refresh") {
            refresh = true;
        } else if (a == "--json") {
            json_output = true;
        } else if (a == "--metabase" && i + 1 < argc) {
            metabase = argv[++i];
        } else if (a == "--offline") {
            opts.offline = true;
        } else if (name.empty() && a.rfind("--", 0) != 0) {
            name = a; // 容许直接传名：rtatag summary ZARD
        }
    }

    if (name.empty()) {
        printf("用法:\n  rtatag summary --kind <kind> --name <name> [--lang zh] [--refresh] [--json] [--metabase <dir>]\n");
        return 2;
    }

    EntityKind k = EntityKind::Person;
    if (!kindStr.empty()) {
        if (!entity_kind_from_tag(kindStr, k)) {
            printf("[错误] 未知实体种类: %s（支持 person / group / label / series / work / recording / release / track）\n",
                   kindStr.c_str());
            return 2;
        }
    }

    opts.refresh = refresh;
    opts.metabase = metabase;

    const auto t0 = std::chrono::steady_clock::now();
    const SummaryResult res = retrieval_fetch(k, name, lang, opts);
    const double elapsed_s = ms_since(t0) / 1000.0;

    if (json_output) {
        printf("%s\n", summary_to_json(res).c_str());
        return res.ok ? 0 : 1;
    }

    const std::string eid = entity_id(k, name);
    printf("== summary: kind=%s name=\"%s\" id=%s  (耗时 %.2f s)\n",
           entity_kind_tag(k), name.c_str(), eid.c_str(), elapsed_s);
    printf("  [状态] from_cache=%s  offline=%s  引用校验=%s\n",
           res.from_cache ? "是" : "否",
           res.offline ? "是" : "否",
           (res.ok && res.validation_ok) ? "通过" : "失败");
    if (!res.disclaimer.empty()) {
        printf("  [免责声明] %s\n", res.disclaimer.c_str());
    }
    if (!res.error.empty()) {
        printf("  [说明/错误] %s\n", res.error.c_str());
    }

    printf("  [章节概览] (共 %zu 节)\n", res.sections.size());
    for (size_t i = 0; i < res.sections.size(); ++i) {
        const auto& sec = res.sections[i];
        printf("    §%zu %s (%zu 个段落)\n", i + 1, sec.heading.c_str(), sec.paragraphs.size());
        for (size_t j = 0; j < sec.paragraphs.size(); ++j) {
            const auto& p = sec.paragraphs[j];
            std::string cits;
            for (size_t c = 0; c < p.citations.size(); ++c) {
                if (c) cits += ",";
                cits += p.citations[c];
            }
            printf("       P%zu: %s [%s]\n", j + 1, first_n(p.text, 140).c_str(), cits.c_str());
        }
    }

    printf("  [引用来源] (共 %zu 条)\n", res.citations.size());
    for (size_t i = 0; i < res.citations.size(); ++i) {
        const auto& c = res.citations[i];
        printf("    [%s] 标题: %s\n", c.id.c_str(), c.title.c_str());
        printf("         来源: %s | 取回: %lld (%s)\n",
               c.source.c_str(), c.fetched_at, fmt_ts(c.fetched_at).c_str());
        printf("         链接: %s\n", c.url.c_str());
    }

    return res.ok ? 0 : 1;
}

// ---------------- main ----------------

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    // 命令行参数：宽字符 -> UTF-8（不经 ANSI 代码页），之后全部按 UTF-8 字节串处理
    std::vector<std::string> u8arg((size_t)argc);
    std::vector<char*>       argp((size_t)argc);
    for (int i = 0; i < argc; ++i) {
        u8arg[(size_t)i] = rta::audio::wide_to_utf8(argv[i]);
        argp[(size_t)i]  = u8arg[(size_t)i].data();
    }
    char** u8argv = argp.data();

    if (argc < 2) { usage(); return 2; }
    const std::string cmd = u8argv[1];
    if (cmd == "parse")     return cmd_parse(argc, u8argv);
    if (cmd == "enrich")    return cmd_enrich(argc, u8argv);
    if (cmd == "write")     return cmd_write(argc, u8argv);
    if (cmd == "llm-smoke") return cmd_llm_smoke();
    if (cmd == "knowledge") return cmd_knowledge(argc, u8argv);
    if (cmd == "summary")   return cmd_summary(argc, u8argv);
    if (cmd == "store") {
        if (argc >= 3 && std::string(u8argv[2]) == "show") {
            if (argc < 4) { usage(); return 2; }
            return cmd_store_show(u8argv[3]);
        }
        if (argc >= 3 && std::string(u8argv[2]) == "set") {
            if (argc < 6) { usage(); return 2; }
            return cmd_store_set(u8argv[3], u8argv[4], u8argv[5]);
        }
        usage();
        return 2;
    }
    usage();
    return 2;
}
