// enrich.h — 单曲元数据补全编排(docs/元数据补全调研与规划.md §3 的 ①→⑤ 全链)
//
// 编排:parse(标签→文件名→LLM) → providers(qq/netease/itunes 搜索) → match(评分排序,
// 歧义时 LLM 裁决) → fuse(字段级融合写入) → store(MetaStore 落盘) + 封面下载 + 歌词链。
//
// 设计要点:
//   * 每步失败都降级继续,不抛异常:任何网络/解析失败只进 stats.note,流程不断;
//   * MetaStore::instance() 的 baseDir 由调用方设好(rtaweb=exe 目录,rtatag=当前工作目录);
//   * LLM 两处参与(都只在 opts.llm 且 rlplayer-llm.conf 有 api_key 时):
//       ③ 查询解析兜底 —— 标签/文件名解析置信度 <0.8 时让 deepseek-flash 拆文件名;
//       ⑤ 匹配裁决 —— 非 confident 且候选 ≥2 时给前三候选让模型挑,只回 {"pick":N};
//   * 融合写入全部走 MetaStore::mergeField(source=候选源,confidence=该候选总分,ts=当前秒),
//     因此 user/tag 的高优先级字段天然不被覆盖;
//   * 采纳门槛(P2 HF 2026-10-22):第 5 步之后、融合写入之前,低分且 LLM 未确认的 top1
//     不再自动写入 —— 采用条件 = match_confident || LLM裁决 pick>=0 || top1 总分 >= 0.80
//     (纯函数 enrich_adopt)。不满足则不写任何字段/不下载封面/不取歌词/不落盘,保持
//     无记录,下次播放可再试。背景:P1.5 二十首抽样人工核对出 4 例错配,全部是
//     「top1 0.32~0.49 + LLM 裁决=-1 仍被写入」(同人曲在网易的大量同社团错曲目诱饵)。
//   * fieldsFilled 只记「本次真正新写入(合并生效)」的字段:写前快照、写后比对;
//   * 网络纪律:搜索源之间 sleep ≥300ms;歌词依序 lrclib→netease→qq 取第一个 ok;
//     封面只在记录缺 coverRelPath 且首选候选有 coverUrl 时下载一次(>2KB 才收);
//   * 可离线测试面(铁律 3,不造假联网测试):字段兜底选择/封面扩展名判定/LLM pick 解析/
//     歌词句柄挑选/查询串拼装都抽成纯函数(enrich_detail),tests/test_enrich.cpp 最小构造覆盖;
//     编排本体(网络+落盘)由任务 F 的真实网络门禁验证,不做假离线测试。
//
// ⚠️ 未解决项(铁律 4):provider 各源单次超时 15s(provider.h 实现),极端全超时场景
//     enrich_track 可能超过 20s(上层 /api/enrich 以同步方式调用时的已知边界);
//     正常命中场景(任一源活)在数秒内返回。以任务 F 实测时延为回归基线。
//     采纳门槛的回归基线 = P1.5 同批 20 首在空 store 沙箱(p16sandbox)的实跑复验
//     (build/p16_sample_report.txt,2026-10-22):4 例错配曲目(#1/#6/#7/#10)应显示
//     「未采纳:…低于门槛」且无落盘记录;LLM 逐次裁决结果本身仍随模型/候选波动。
#pragma once
#include "meta/store.h"
#include "meta/parse.h"
#include "meta/llm.h"
#include "meta/provider.h"
#include "meta/match.h"
#include "meta/discogs.h"
#include "audio/media_info.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace rta::meta {

// ---------------------------------------------------------------------------
// 编排参数与结果
// ---------------------------------------------------------------------------

struct EnrichOpts {
    // P1.5 注册表化:源开关 id → 开关;未列出的源缺省开(新增数据源零接线)。
    std::map<std::string, bool> srcOn;
    bool llm = true;
    // 2026-10-22 功能4:llm 配置文件路径。空 = 沿用默认相对路径 "rlplayer-llm.conf"
    // (rtatag 这类"以 CWD 为家"的调用方保持旧行为);rtaweb 传 exe 同目录的绝对路径 ——
    // 这样【界面里保存的 key】与【补全时读的 key】永远是同一份文件,不受启动 CWD 影响。
    std::string llmConfPath;
    // 读取开关:map 里没有该源 = 开
    bool enabled(const std::string& id) const {
        const auto it = srcOn.find(id);
        return it == srcOn.end() ? true : it->second;
    }
};

// 旧字段形态的兼容便利构造(metaqq/metanetease/metaitunes/metalrclib/metallm 逐个布尔)。
// 显式给出的值原样写入 srcOn —— 只有 false 才真正关闭对应源。
inline EnrichOpts make_enrich_opts(bool qq, bool netease, bool itunes, bool lrclib, bool llm) {
    EnrichOpts o;
    o.srcOn["qq"]      = qq;
    o.srcOn["netease"] = netease;
    o.srcOn["itunes"]  = itunes;
    o.srcOn["lrclib"]  = lrclib;
    o.llm = llm;
    return o;
}

struct EnrichStats {
    bool ok = false;                         // 本次实际补到了内容并过采纳门槛(未采纳/skipped/失败均 false)
    bool skipped = false;                    // 已有完整标签记录且未 force → 未动
    bool confident = false;                  // match_confident(top1) 的判定(与是否 LLM 裁决无关)
    int nCand = 0;                           // 汇总候选数
    double bestScore = 0;                    // 采用候选的 match 总分
    std::string bestSource;                  // 采用候选的来源 "qq"/"netease"/"itunes"
    std::string note;                        // 一句话:候选/置信/LLM 是否参与/各源错误摘要
    std::vector<std::string> fieldsFilled;   // 本次实际新写入(合并生效)的字段名(含 "cover")
};

// ---------------------------------------------------------------------------
// 可离线测试的纯函数(ed_ = enrich detail)
// ---------------------------------------------------------------------------
namespace enrich_detail {

// 查询串 = (artist 非空 ? artist+" " : "") + title(任务 spec 步 3 原文)
inline std::string ed_query_string(const std::string& artist, const std::string& title) {
    return (artist.empty() ? std::string() : artist + " ") + title;
}

// 字段兜底选择(任务 spec 步 6;2026-10-22 HF:增加分数下限):按分数从高到低遍历其余候选
// (skipIdx 是已写入的首选),返回第一个 field(genre/year/album)取值非空【且该候选总分
// ≥ minScore(默认 0.45)】的下标;全空/全低于下限返回 npos。
// 为什么要下限:同作曲目的 0.08 置信候选也能借来填流派,是元数据噪声的实测来源 ——
// 兜底本来就是「借别人的字段」,借的对象自身得先像样(分数下限与 confident 阈值同量级)。
inline size_t ed_fallback_pick(const std::string& field, const std::vector<Scored>& sortedDesc,
                               const std::vector<Candidate>& cands, size_t skipIdx,
                               double minScore = 0.45) {
    for (const Scored& s : sortedDesc) {
        if (s.idx == skipIdx) continue;
        if (s.idx >= cands.size()) continue;
        if (s.score < minScore) continue;   // 低分候选不兜底(同人曲 0.08 置信候选填流派的噪声)
        const std::string v =
            (field == "genre") ? cands[s.idx].genre :
            (field == "year")  ? cands[s.idx].year  :
            (field == "album") ? cands[s.idx].album : std::string();
        if (!v.empty()) return s.idx;
    }
    return (size_t)-1;
}

// 封面扩展名判定(任务 spec 步 7):URL(忽略查询串)里出现 ".png"(大小写不敏感)→ png,
// 否则一律 jpg(saveCover 只认 jpg/png;iTunes/mzstatic 与 QQ 封面均为 jpg)。
inline std::string ed_cover_ext(const std::string& url) {
    std::string low = url;
    for (char& c : low) c = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    const size_t q = low.find('?');
    if (q != std::string::npos) low = low.substr(0, q);
    return low.find(".png") != std::string::npos ? "png" : "jpg";
}

// LLM 裁决回包解析:剥围栏/找 {…} 后取 "pick";合法范围 [-1, nShown-1],越界/解析失败 → -1。
inline int ed_llm_pick(const std::string& text, int nShown) {
    const std::string body = parse_llm_strip_fence(text);
    const size_t b1 = body.find('{');
    if (b1 == std::string::npos) return -1;
    const size_t b2 = body.rfind('}');
    if (b2 == std::string::npos || b2 <= b1) return -1;
    const rta::net::Json j = rta::net::json_parse(body.substr(b1, b2 - b1 + 1));
    if (j.type != rta::net::Json::Obj || !j.has("pick")) return -1;
    int pick = (int)j["pick"].n();                       // Num 直取;Str 经 atof
    if (pick < -1) pick = -1;
    if (pick > nShown - 1) pick = -1;
    return pick;
}

// 歌词句柄挑选:按分数从高到低找第一个来自 source 且 songId 非空的候选,返回其 songId。
inline std::string ed_lyric_song_id(const std::vector<Scored>& sortedDesc,
                                    const std::vector<Candidate>& cands, const char* source) {
    for (const Scored& s : sortedDesc) {
        if (s.idx >= cands.size()) continue;
        if (cands[s.idx].source == source && !cands[s.idx].songId.empty())
            return cands[s.idx].songId;
    }
    return "";
}

// note 碎片拼一句("; " 连接)
inline std::string ed_join_notes(const std::vector<std::string>& notes) {
    std::string all;
    for (const std::string& n : notes) { if (!all.empty()) all += "; "; all += n; }
    return all;
}

// 采纳门槛(2026-10-22 P2 HF,纯函数供离线测试):融合写入前的最后一道闸。
// 采用条件(满足其一):match_confident;或 LLM 裁决 pick>=0;或 top1 总分 >= kEnrichAdoptTop。
// llmPick == -9 表示「LLM 未运行」(llmRan=false 时 pick 一律按 -9 处理,以 llmRan 为准);
// 此时只剩 confident || 高分两条路。
// 背景:P1.5 二十首抽样人工核对出 4 例错配(#1/#6/#7/#10),全部是「top1 总分 0.32~0.49
// + LLM 裁决=-1(或候选只有 1 条没触发裁决)仍被采纳写入」——同人曲在网易存在大量
// 同社团错曲目诱饵,分数低于门槛又没有 LLM 背书的 top1 不得直接落盘。
// 高分豁免保留(>=0.80,与 match_confident 的 top1 阈值同量级):LLM 接口故障时,
// 无歧义的高分命中(P1.5 样本 #2/#8/#9/#11/#12/#18/#20 形态)仍能正确补全。
inline constexpr double kEnrichAdoptTop = 0.80;
inline bool enrich_adopt(bool confident, int llmPick, bool llmRan, double topScore) {
    if (confident) return true;
    const int pick = llmRan ? llmPick : -9;      // 未运行 LLM → 恒按 -9(未确认)
    if (pick >= 0) return true;                  // LLM 明确点名某候选 → 背书成立
    return topScore >= kEnrichAdoptTop;          // 无背书时只放行高分
}

} // namespace enrich_detail

// ---------------------------------------------------------------------------
// 编排本体
// ---------------------------------------------------------------------------

inline EnrichStats enrich_track(const std::string& file, bool force, const EnrichOpts& opts) {
    EnrichStats st;
    MetaStore& ms = MetaStore::instance();
    const long long now = (long long)std::time(nullptr);
    std::vector<std::string> notes;              // note 碎片,最后拼一句

    // ---- 1) 已有完整记录且未 force → skipped ----
    TrackMeta tm;
    const bool had = ms.load(file, tm);
    if (!had) tm.path = file;                    // 无记录:新建(路径决定 hash)
    if (had && !force) {
        const auto itT = tm.fields.find("title");
        const auto itA = tm.fields.find("artist");
        const auto itAl = tm.fields.find("album");
        const std::string t = (itT == tm.fields.end()) ? "" : itT->second.value;
        const std::string a = (itA == tm.fields.end()) ? "" : itA->second.value;
        const std::string al = (itAl == tm.fields.end()) ? "" : itAl->second.value;
        if (!t.empty() && (!a.empty() || !al.empty())) {
            st.skipped = true;
            st.note = "已有完整标签记录(title + artist/album 非空),未动(可用 force 强制)";
            return st;
        }
    }

    // 写字段并跟踪「合并是否真生效」(写前快照、写后比对;fieldsFilled 去重)
    auto mergeTracked = [&](const char* field, const MetaValue& v) {
        const bool existed = tm.fields.count(field) != 0;
        const MetaValue before = existed ? tm.fields[field] : MetaValue();
        MetaStore::mergeField(tm, field, v);
        const MetaValue& after = tm.fields[field];
        const bool changed = !existed || after.value != before.value ||
                             after.source != before.source || after.ts != before.ts ||
                             after.confidence != before.confidence;
        if (changed) {
            for (const std::string& f : st.fieldsFilled) if (f == field) return;
            st.fieldsFilled.push_back(field);
        }
    };

    // ---- 2) 本地标签与时长;父目录名只入 note(同人盘「专辑 @ 社团」形态,不参与查询) ----
    const rta::audio::MediaInfo mi = rta::audio::probe_media(file);
    // 时长 → 整秒（match 的「差 ≤3s」判据用它）。转换里的溢出/NaN 防御放在
    // audio::media_duration_sec（见 src/audio/media_info.h 的注释）：这里原来是
    // (int)(mi.duration + 0.5)，duration 一旦因损坏的头而巨大，越界转 int 是未定义
    // 行为（本机实测 1e10 → -4），负秒会让 match 静默退回中性分而缺陷无声。
    const int fileSec = rta::audio::media_duration_sec(mi.duration);
    const std::string parentName = meta_string_of(meta_path_of(file).parent_path().filename());
    std::string dirAlbum, dirCircle;
    if (parentName.find(" @ ") != std::string::npos) {
        const size_t at = parentName.find(" @ ");
        dirAlbum = parentName.substr(0, at);
        dirCircle = parentName.substr(at + 3);
        notes.push_back("父目录「" + parentName + "」(同人盘形态,参与扩展查询)");
    }

    // ---- 3) 查询侧 ParsedName:标签优先 → 文件名规则 → LLM 兜底 ----
    const std::string stem = meta_string_of(meta_path_of(file).stem());
    LlmConfig cfg;
    if (opts.llm) cfg = opts.llmConfPath.empty() ? llm_config_load() : llm_config_load(opts.llmConfPath);
    const bool llmReady = opts.llm && !cfg.apiKey.empty();

    ParsedName p;
    bool llmParsed = false;                      // 本次 LLM 是否实际参与(解析或裁决,进 note)
    if (!mi.artist.empty() && !mi.title.empty()) {
        p = parse_from_tags(mi.artist, mi.title);
    } else {
        p = parse_filename(stem);
        if (p.confidence < 0.8f && llmReady) {
            const LlmReply rep = llm_chat(cfg, { LlmMessage{ "user", parse_llm_prompt(stem) } },
                                          300, 0.2f, true);
            if (rep.ok) {
                const ParsedName lp = parse_llm_parse(rep.content);
                if (!lp.title.empty()) { p = lp; llmParsed = true; }
                else notes.push_back("LLM解析未给出标题(仍用规则结果)");
            } else {
                notes.push_back("LLM解析失败: " + rep.error);
            }
        }
    }
    const std::string query = enrich_detail::ed_query_string(p.artist, p.title);
    if (p.title.empty()) {                       // 无标题无法搜索(编排到此为止,如实报告)
        notes.push_back("无可用的标题查询串(标签与文件名都拆不出 title)");
        st.note = "失败: " + enrich_detail::ed_join_notes(notes);
        return st;
    }

    // ---- 4) 搜索(遍历注册表,canSearch 且启用的源逐个调;调用间 ≥300ms;错误记 note 继续) ----
    // 注册表顺序 = 搜索优先级(dizzylab → qq → netease → itunes)。
    // E-17 扩展：构造 ProviderQuery 传递 album/circle/dirName 供 dizzylab 优先命中，
    // 其它源无 searchQuery 指针，继续沿用 query（默认忽略新参数）。
    ProviderQuery pq;
    pq.query   = query;
    pq.title   = p.title;
    pq.artist  = p.artist;
    pq.album   = !mi.album.empty() ? mi.album : dirAlbum;
    pq.circle  = dirCircle;
    pq.dirName = parentName;

    std::vector<Candidate> cands;
    std::vector<std::string> srcErrs;
    bool firstCall = true;
    for (const ProviderDesc& pd : provider_registry()) {
        if (!pd.canSearch) continue;
        if (!opts.enabled(pd.id)) continue;
        if (!firstCall) std::this_thread::sleep_for(std::chrono::milliseconds(300));
        firstCall = false;
        std::string err;
        std::vector<Candidate> got;
        if (pd.searchQuery) {
            got = pd.searchQuery(pq, err);
        } else if (pd.search) {
            got = pd.search(pq.query, err);
        }
        if (!err.empty()) srcErrs.push_back(std::string(pd.id) + ": " + err);
        for (Candidate& c : got) cands.push_back(std::move(c));
    }
    st.nCand = (int)cands.size();
    if (!srcErrs.empty()) {
        std::string all;
        for (const std::string& e : srcErrs) { if (!all.empty()) all += " | "; all += e; }
        notes.push_back("源错误: " + all);
    }
    if (cands.empty()) {
        notes.push_back("无候选(query=\"" + query + "\")");
        st.note = "失败: " + enrich_detail::ed_join_notes(notes);
        return st;
    }

    // ---- 5) 评分排序 + (必要时)LLM 裁决 ----
    const std::vector<Scored> sorted = match_score_all(cands, p.title, p.artist, p.feat, fileSec, p.version);
    st.confident = match_confident(sorted);
    size_t bestIdx = sorted.front().idx;
    st.bestScore = sorted.front().score;
    st.bestSource = cands[bestIdx].source;
    int llmPick = -9;                            // LLM 裁决结果;-9 = 裁决未运行/失败(语义见 enrich_adopt)
    bool llmAdjudicated = false;                 // 裁决这一步是否真实运行(与解析是否用 LLM 无关)
    if (!st.confident && llmReady && sorted.size() >= 2) {
        const int nShow = (int)std::min<size_t>(3, sorted.size());
        std::ostringstream pr;
        pr << "你是音乐元数据匹配裁决助手。根据查询信息,从候选中选出正确匹配;都不对选 -1。\n"
              "查询: artist=" << p.artist << " | title=" << p.title;
        if (!p.version.empty()) pr << " | version=" << p.version;
        pr << "\n候选:\n";
        for (int i = 0; i < nShow; ++i) {
            const Candidate& c = cands[sorted[(size_t)i].idx];
            pr << i << ". [" << c.source << "] title=" << c.title << " | artist=" << c.artist
               << " | album=" << c.album << " | year=" << c.year << " | duration="
               << (c.durationMs > 0 ? std::to_string(c.durationMs / 1000) + "s" : std::string("未知"))
               << "\n";
        }
        pr << "只输出一个 JSON 对象:{\"pick\":0|1|2|-1},不要输出任何其他文字。";
        const LlmReply rep = llm_chat(cfg, { LlmMessage{ "user", pr.str() } }, 200, 0.0f, true);
        llmParsed = true;
        llmAdjudicated = true;
        if (rep.ok) {
            llmPick = enrich_detail::ed_llm_pick(rep.content, nShow);
            if (llmPick >= 0) {
                bestIdx = sorted[(size_t)llmPick].idx;
                st.bestScore = sorted[(size_t)llmPick].score;
                st.bestSource = cands[bestIdx].source;
                notes.push_back("LLM裁决=候选" + std::to_string(llmPick) + "(" + st.bestSource + ")");
            } else {
                notes.push_back("LLM裁决=-1");   // 是否维持 top1 由 5.5 采纳门槛决定,不在此断言
            }
        } else {
            notes.push_back("LLM裁决失败: " + rep.error);
        }
    }

    // note 汇总(两个出口共用:未采纳→门槛出口直接收尾;采纳→流程尾)
    auto finishNote = [&]() {
        std::ostringstream ns;
        ns << "候选" << st.nCand << ", best=" << st.bestSource << " " << st.bestScore
           << (st.confident ? " confident" : " 非confident")
           << (llmParsed ? ", LLM参与" : ", LLM未参与");
        notes.insert(notes.begin(), ns.str());
        std::string all;
        for (const std::string& n : notes) { if (!all.empty()) all += "; "; all += n; }
        st.note = all;
    };

    // ---- 5.5) 采纳门槛(2026-10-22 P2 HF):写入前最后一道闸 ----
    // P1.5 抽样人工核对:4 例错配全部是「top1 0.32~0.49 + LLM 裁决=-1(或单候选没触发
    // 裁决)仍被采纳写入」。不满足采用条件 → 本次不采纳:不写任何字段、不下载封面、
    // 不取歌词、不落盘(MetaStore 保持无记录,下次播放可再试);st.ok 保持 false。
    if (!enrich_detail::enrich_adopt(st.confident, llmPick, llmAdjudicated, sorted.front().score)) {
        std::ostringstream ub;
        ub << "未采纳:最高分 " << sorted.front().score << " 低于门槛且 LLM 未确认";
        notes.push_back(ub.str());
        finishNote();
        return st;
    }

    const Candidate& best = cands[bestIdx];
    st.ok = true;                                // 过了采纳门槛,后续步骤只增不减

    // ---- 6) 融合写入:首选候选六键 + genre/year/album 逐字段兜底(只补空) ----
    MetaValue mv; mv.ts = now; mv.confidence = (float)st.bestScore; mv.source = best.source;
    if (!best.title.empty()) { mv.value = best.title;  mergeTracked("title", mv); }
    if (!best.artist.empty()) { mv.value = best.artist; mergeTracked("artist", mv); }
    if (!best.album.empty())  { mv.value = best.album;  mergeTracked("album", mv); }
    if (!best.artist.empty()) { mv.value = best.artist; mergeTracked("albumArtist", mv); }
    if (!best.year.empty())   { mv.value = best.year;   mergeTracked("year", mv); }
    if (!best.genre.empty())  { mv.value = best.genre;  mergeTracked("genre", mv); }
    // E-15: 本地古典标签保留写入（若本地标签含 COMPOSER/WORK/CONDUCTOR/PERFORMER）
    if (!mi.composer.empty())  { MetaValue v; v.value = mi.composer;  v.source = "tag"; v.confidence = 1.0f; v.ts = now; mergeTracked("composer", v); }
    if (!mi.work.empty())      { MetaValue v; v.value = mi.work;      v.source = "tag"; v.confidence = 1.0f; v.ts = now; mergeTracked("work", v); }
    if (!mi.conductor.empty()) { MetaValue v; v.value = mi.conductor; v.source = "tag"; v.confidence = 1.0f; v.ts = now; mergeTracked("conductor", v); }
    if (!mi.performer.empty()) { MetaValue v; v.value = mi.performer; v.source = "tag"; v.confidence = 1.0f; v.ts = now; mergeTracked("performer", v); }
    auto fieldStillEmpty = [&](const char* f) {
        const auto it = tm.fields.find(f);
        return it == tm.fields.end() || it->second.value.empty();
    };
    for (const char* f : { "genre", "year", "album" }) {
        if (!fieldStillEmpty(f)) continue;
        const size_t j = enrich_detail::ed_fallback_pick(f, sorted, cands, bestIdx);
        if (j == (size_t)-1) continue;
        double scoreJ = 0;                        // 兜底写入按 spec 用「该候选总分」
        for (const Scored& s : sorted)
            if (s.idx == j) { scoreJ = s.score; break; }
        MetaValue fv;
        fv.value      = (std::string(f) == "genre") ? cands[j].genre :
                        (std::string(f) == "year")  ? cands[j].year  : cands[j].album;
        fv.source     = cands[j].source;
        fv.ts         = now;
        fv.confidence = (float)scoreJ;
        mergeTracked(f, fv);
    }

    // ---- 6.5) E-5: genre 多源（Discogs styles 接入作为 backfill 来源）----
    // 保持采纳门槛语义不变（在此之前采纳门槛已裁决并过门），保留 iTunes 优先（仅当 genre 仍为空时尝试 Discogs）
    if (fieldStillEmpty("genre") && opts.enabled("discogs")) {
        std::string dErr;
        const std::string alb = !best.album.empty() ? best.album : mi.album;
        const std::string dg = discogs_lookup_genre(p.artist, p.title, alb, dErr);
        if (!dg.empty()) {
            MetaValue gv;
            gv.value = dg;
            gv.source = "discogs";
            gv.ts = now;
            gv.confidence = (float)st.bestScore; // 按事实源规则，随采纳置信度
            mergeTracked("genre", gv);
            notes.push_back("genre=discogs(" + dg + ")");
        }
    }

    // ---- 7) 封面:记录缺封面且首选候选有 URL → 下载(>2KB 才收) ----
    if (tm.coverRelPath.empty() && !best.coverUrl.empty()) {
        const rta::net::HttpResponse resp = rta::net::http_get(best.coverUrl, {}, 15000);
        if (resp.status == 200 && resp.body.size() > 2048) {
            const std::string rel = ms.saveCover(file, resp.body.data(), resp.body.size(),
                                                 enrich_detail::ed_cover_ext(best.coverUrl));
            if (!rel.empty()) {
                tm.coverRelPath = rel;
                st.fieldsFilled.push_back("cover");
                notes.push_back("封面已缓存(" + rel + ")");
            } else notes.push_back("封面保存失败");
        } else {
            notes.push_back("封面未收(" + best.coverUrl.substr(0, 40) + "... HTTP " +
                            std::to_string(resp.status) + " " + std::to_string(resp.body.size()) + "B)");
        }
    }

    // ---- 8) 歌词:lyrics 为空时按注册表【逆序】(canLyric 过滤)= lrclib → netease → qq,
    //         第一个 ok 写入。注册表正向是搜索优先级,逆序恰是 P1 的歌词优先级,顺序不变;
    //         源内句柄经 LyricRequest.cands(按分数从高到低)带入,免重复搜索。 ----
    {
        const auto itL = tm.fields.find("lyrics");
        const bool needLyrics = itL == tm.fields.end() || itL->second.value.empty();
        if (needLyrics) {
            LyricRequest lrq;
            lrq.title  = p.title;
            lrq.artist = p.artist;
            lrq.album  = !best.album.empty() ? best.album : mi.album;
            lrq.durationSec = fileSec;
            lrq.cands.reserve(sorted.size());
            for (const Scored& s : sorted)           // 句柄挑选按分数从高到低(与 P1 一致)
                if (s.idx < cands.size()) lrq.cands.push_back(cands[s.idx]);
            LyricResult lr;
            std::vector<std::string> lyrErrs;        // 各参与源的真实失败(P1 只留最后一个)
            for (auto it = provider_registry().rbegin(); it != provider_registry().rend(); ++it) {
                if (!it->canLyric || !it->lyric) continue;
                if (!opts.enabled(it->id)) continue;
                std::string lerr;
                LyricResult t = it->lyric(lrq, lerr);
                if (t.ok) { lr = std::move(t); break; }
                if (!t.err.empty()) lyrErrs.push_back(t.err);
            }
            if (lr.ok && !lr.lrc.empty()) {
                MetaValue lv;
                lv.value = lr.lrc; lv.source = lr.source; lv.ts = now;
                lv.confidence = (float)st.bestScore;      // 歌词无独立评分,随匹配置信
                mergeTracked("lyrics", lv);
                notes.push_back("歌词=" + lr.source);
            } else if (!lyrErrs.empty()) {
                notes.push_back("歌词未取(" + enrich_detail::ed_join_notes(lyrErrs) + ")");
            } else {
                notes.push_back("歌词未取(各源无词/未启用)");
            }
        }
    }

    // ---- 9) 落盘 + note ----
    if (!tm.fields.empty() || !tm.coverRelPath.empty()) ms.save(tm);

    finishNote();
    return st;
}

} // namespace rta::meta
