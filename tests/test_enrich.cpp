// test_enrich.cpp — src/meta/enrich.h 的可离线纯函数(全离线,不发任何网络请求)
//
// 判据说明(铁律 3):enrich 的编排本体(搜索→评分→融合→落盘)是网络+文件系统流程,
// 无法也不应伪造「离线编排测试」——它的真实验证是任务 F 的真实网络门禁(3 首实跑)。
// 本测试只覆盖 enrich_detail 里抽成纯函数的「融合兜底/封面判定/LLM pick 解析/歌词
// 句柄挑选/查询串拼装/采纳门槛」六件可离线逻辑,用最小构造断言「输入 → 选择」的映射。
//
// ⚠️ 未解决项(铁律 4):编排本体不在本测试覆盖内(不造假);以任务 F 实跑为回归基线。
#include "meta/enrich.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace rta::meta;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
static void ck_eq(const std::string& got, const std::string& want, const char* what) {
    ck(got == want, what, "got=[" + got + "] want=[" + want + "]");
}

// 最小构造:下标即分数序(idx0 最优)
static Scored sc(size_t idx, double score) {
    Scored s;
    s.idx = idx;
    s.score = score;
    return s;
}
static Candidate cd(const char* source, const char* genre, const char* year,
                    const char* album, const char* songId) {
    Candidate c;
    c.source = source;
    c.genre = genre;
    c.year = year;
    c.album = album;
    c.songId = songId;
    return c;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== meta/enrich 可离线纯函数(编排本体由真实网络门禁验证,见文件头)==\n");

    // ---- ed_query_string ----
    std::printf("-- ed_query_string --\n");
    ck_eq(enrich_detail::ed_query_string("周杰伦", "晴天"), "周杰伦 晴天", "artist+title → 中间一个空格");
    ck_eq(enrich_detail::ed_query_string("", "晴天"), "晴天", "artist 空 → 只有 title");
    ck_eq(enrich_detail::ed_query_string("澤田なつ", ""), "澤田なつ ", "title 空 → artist+空格(spec 字面)");
    ck_eq(enrich_detail::ed_query_string("", ""), "", "双空 → 空串");

    // ---- ed_fallback_pick ----
    std::printf("-- ed_fallback_pick --\n");
    {
        std::vector<Candidate> cs = {
            cd("qq", "", "", "叶惠美", ""),        // 0:首选(无 genre/year)
            cd("netease", "", "", "", ""),          // 1:全空
            cd("itunes", "華語流行樂", "2003", "JAY", ""),  // 2:有 genre/year/album
            cd("qq", "", "1999", "", ""),           // 3:只有 year
        };
        std::vector<Scored> sorted = { sc(0, 0.95), sc(1, 0.70), sc(2, 0.60), sc(3, 0.50) };
        ck(enrich_detail::ed_fallback_pick("genre", sorted, cs, 0) == 2,
           "genre 兜底 → 第一个有 genre 的是候选2(itunes)");
        ck(enrich_detail::ed_fallback_pick("year", sorted, cs, 0) == 2,
           "year 兜底 → 按分数序第一个有 year 的是候选2");
        ck(enrich_detail::ed_fallback_pick("album", sorted, cs, 0) == 2,
           "album 兜底 → 候选0 被跳过(首选),取候选2");
        // 首选缺 album、第二名也无 → 跳到下一个有值的
        ck(enrich_detail::ed_fallback_pick("year", sorted, cs, 2) == 3,
           "skip=候选2 时 year 兜底 → 候选3(qq 1999)");
        std::vector<Candidate> none = { cd("qq", "", "", "", ""), cd("itunes", "", "", "", "") };
        std::vector<Scored> snone = { sc(0, 0.9), sc(1, 0.5) };
        ck(enrich_detail::ed_fallback_pick("genre", snone, none, 0) == (size_t)-1,
           "全部无值 → npos(不写入)");
        // 下标越界防御:sorted 指向不存在候选 → 跳过不崩
        std::vector<Scored> sbad = { sc(9, 0.9), sc(2, 0.6) };
        ck(enrich_detail::ed_fallback_pick("genre", sbad, cs, 0) == 2,
           "sorted 含越界下标 → 跳过,取合法的有值候选");
        // 未知字段名 → 一律 npos
        ck(enrich_detail::ed_fallback_pick("title", sorted, cs, 0) == (size_t)-1,
           "未支持字段名 → npos(防御)");
        // 2026-10-22 HF:分数下限(默认 0.45)—— 修「同人曲被 0.08 置信候选填流派」的噪声
        {
            std::vector<Candidate> low = { cd("qq", "", "", "", ""), cd("itunes", "同人音楽", "", "", "") };
            std::vector<Scored> slow  = { sc(0, 0.90), sc(1, 0.08) };
            ck(enrich_detail::ed_fallback_pick("genre", slow, low, 0) == (size_t)-1,
               "低分候选(0.08 < 0.45)不兜底 → npos(同人曲噪声修复)");
            std::vector<Scored> shigh = { sc(0, 0.90), sc(1, 0.60) };
            ck(enrich_detail::ed_fallback_pick("genre", shigh, low, 0) == 1,
               "高分候选(0.60 ≥ 0.45)兜底 → 候选1(itunes)");
            ck(enrich_detail::ed_fallback_pick("genre", slow, low, 0, 0.05) == 1,
               "显式放宽下限(0.05)→ 0.08 候选可用(下限参数真实生效)");
            // 下限附近的确定性:恰好等于下限 → 收(≥ 语义);略低 → 拒
            std::vector<Scored> seq  = { sc(0, 0.90), sc(1, 0.45) };
            ck(enrich_detail::ed_fallback_pick("genre", seq, low, 0) == 1,
               "分数恰等于下限(0.45)→ 兜底(≥ 语义)");
        }
    }

    // ---- ed_cover_ext ----
    std::printf("-- ed_cover_ext --\n");
    ck_eq(enrich_detail::ed_cover_ext(
              "https://is1-ssl.mzstatic.com/image/thumb/Music/JAY.jpg/1200x1200bb.jpg"),
          "jpg", "mzstatic jpg URL → jpg");
    ck_eq(enrich_detail::ed_cover_ext(
              "https://y.gtimg.cn/music/photo_new/T002R800x800M000000MkMni19ClKG.jpg"),
          "jpg", "QQ 封面(无扩展名标记)→ 默认 jpg");
    ck_eq(enrich_detail::ed_cover_ext("https://p1.music.126.net/abc/18875.png"), "png", "netease png → png");
    ck_eq(enrich_detail::ed_cover_ext("https://example.com/IMG.PNG?param=90y"), "png", "大写 PNG + 查询串 → png");
    ck_eq(enrich_detail::ed_cover_ext("https://example.com/img.png"), "png", "小写 .png → png");

    // ---- ed_llm_pick ----
    std::printf("-- ed_llm_pick --\n");
    ck(enrich_detail::ed_llm_pick("{\"pick\":1}", 3) == 1, "干净 JSON pick=1");
    ck(enrich_detail::ed_llm_pick("{\"pick\":0}", 3) == 0, "pick=0 合法");
    ck(enrich_detail::ed_llm_pick("{\"pick\":-1}", 3) == -1, "pick=-1(都不对)合法");
    ck(enrich_detail::ed_llm_pick("```json\n{\"pick\":2}\n```", 3) == 2, "带围栏 → 剥后解析");
    ck(enrich_detail::ed_llm_pick("{\"pick\":2}", 2) == -1, "pick 超出候选数(nShown=2)→ -1");
    ck(enrich_detail::ed_llm_pick("{\"pick\":5}", 3) == -1, "pick 越界 → -1");
    ck(enrich_detail::ed_llm_pick("模型拒绝回答", 3) == -1, "无 JSON → -1");
    ck(enrich_detail::ed_llm_pick("{\"pick\":\"1\"}", 3) == 1, "pick 为字符串数字 → 1");
    ck(enrich_detail::ed_llm_pick("{\"result\":1}", 3) == -1, "缺 pick 键 → -1");

    // ---- ed_lyric_song_id ----
    std::printf("-- ed_lyric_song_id --\n");
    {
        std::vector<Candidate> cs = {
            cd("netease", "", "", "", ""),                    // 0:netease 但无 id
            cd("qq", "", "", "", "0039MnYb0qxYhV"),           // 1:qq 有 mid
            cd("netease", "", "", "", "186016"),              // 2:netease 有数字 id
            cd("netease", "", "", "", "999"),                 // 3:netease 分更低
        };
        std::vector<Scored> sorted = { sc(0, 0.95), sc(1, 0.80), sc(2, 0.60), sc(3, 0.40) };
        ck_eq(enrich_detail::ed_lyric_song_id(sorted, cs, "netease"), "186016",
              "netease → 跳过无 id 的候选0,取分数最高的有 id 者(候选2)");
        ck_eq(enrich_detail::ed_lyric_song_id(sorted, cs, "qq"), "0039MnYb0qxYhV",
              "qq → 唯一有 mid 的候选1");
        ck_eq(enrich_detail::ed_lyric_song_id(sorted, cs, "itunes"), "",
              "无 itunes 候选 → 空串");
        std::vector<Candidate> empty = { cd("qq", "", "", "", "") };
        std::vector<Scored> sone = { sc(0, 0.9) };
        ck_eq(enrich_detail::ed_lyric_song_id(sone, empty, "qq"), "", "songId 全空 → 空串");
    }

    // ---- 铁律3 自查:match 侧排序下喂 ed_fallback_pick 的顺序性(分数降序语义) ----
    std::printf("-- 与 match_score_all 的集成(纯函数级) --\n");
    {
        // 真实排序输出喂兜底选择:分高者先被看 —— 用 match_score_all 而非手写 sorted
        std::vector<Candidate> cs = {
            cd("qq", "", "2003", "", ""),           // 匹配最好但无 genre
            cd("itunes", "Mandopop", "2003", "叶惠美", ""),
        };
        cs[0].title = "晴天"; cs[0].artist = "周杰伦";
        cs[1].title = "晴天"; cs[1].artist = "周杰伦";
        const std::vector<Scored> r = match_score_all(cs, "晴天", "周杰伦", "", 269);
        const size_t pick = enrich_detail::ed_fallback_pick("genre", r, cs, r.front().idx);
        ck(r.front().idx == 0 && pick == 1,
           "match 排序 + genre 兜底 → 首选无 genre,兜底落到 itunes 候选",
           "top=" + std::to_string(r.front().idx) + " pick=" + std::to_string(pick));
    }

    // ---- enrich_adopt(2026-10-22 P2 HF:采纳门槛) ----
    // 背景:P1.5 抽样人工核对出 4 例错配(#1/#6/#7/#10),全部是「低分 top1(0.32~0.49)
    // + LLM 裁决=-1(或单候选没触发裁决)仍被采纳写入」。这里把门槛本身当纯函数测:
    // 采用条件(满足其一)= confident || llmPick>=0 || topScore>=0.80;llmPick=-9 表示未运行。
    std::printf("-- enrich_adopt(采纳门槛;对应 P1.5 样本形态) --\n");
    // 1) confident → 采纳(LLM 与分数均无关)
    ck(enrich_detail::enrich_adopt(true, -1, true, 0.49),
       "1a confident=true、llmPick=-1、低分 → 采纳");
    ck(enrich_detail::enrich_adopt(true, -9, false, 0.30),
       "1b confident=true、LLM 未跑、很低分 → 仍采纳");
    // 2) LLM 确认(pick>=0)→ 采纳
    ck(enrich_detail::enrich_adopt(false, 1, true, 0.32),
       "2a confident=false、llmPick=1、低分 → 采纳(LLM 背书)");
    ck(enrich_detail::enrich_adopt(false, 0, true, 0.49),
       "2b confident=false、llmPick=0 → 采纳");
    // 3) 低分 + LLM 裁决 -1 → 不采纳(P1.5 样本 #1 top=0.4875 / #7 top=0.41875 形态)
    ck(!enrich_detail::enrich_adopt(false, -1, true, 0.49),
       "3a confident=false、llmPick=-1、top=0.49 → 不采纳(样本 #1 形态)");
    ck(!enrich_detail::enrich_adopt(false, -1, true, 0.42),
       "3b confident=false、llmPick=-1、top=0.42 → 不采纳(样本 #7 形态)");
    // 4) LLM 未跑(-9)+ 低分 → 不采纳(P1.5 样本 #6 形态:单候选没触发裁决,top=0.375)
    ck(!enrich_detail::enrich_adopt(false, -9, false, 0.38),
       "4a confident=false、llm未跑、top=0.38 → 不采纳(样本 #6/#10 分数形态)");
    ck(!enrich_detail::enrich_adopt(false, -9, false, 0.79),
       "4b confident=false、llm未跑、top 差一点到门槛(0.79)→ 不采纳");
    // 5) LLM 未跑 + 高分 → 采纳(LLM 接口故障时,高分无歧义形态仍能补全;样本 #8/#9 形态)
    ck(enrich_detail::enrich_adopt(false, -9, false, 0.93),
       "5a confident=false、llm未跑、top=0.93 → 采纳(高分豁免)");
    // 边界与防御
    ck(enrich_detail::enrich_adopt(false, -9, false, 0.80),
       "6a top 恰等于 0.80 → 采纳(≥ 语义)");
    ck(!enrich_detail::enrich_adopt(false, -1, true, 0.7875),
       "6b top=0.7875(<0.80)+ llmPick=-1 → 不采纳(样本 #19 形态:0.79 分也被拦)");
    ck(!enrich_detail::enrich_adopt(false, 1, false, 0.40),
       "6c llmRan=false 时忽略传入的 pick(以 llmRan 为准,防御)");

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
