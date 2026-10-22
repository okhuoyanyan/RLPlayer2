// test_metamatch.cpp — src/meta/match.h：候选评分与选择（全离线，不发网络请求、不碰设备）
//
// 判据说明（铁律 3）：被测对象是纯打分函数，断言「输入 → 分数/排序/三档判定」的映射。
// 样本取自 docs/元数据补全调研与规划.md §2 的 smoke 真实样本（晴天/周杰伦/叶惠美、
// 澤田なつ《紅い瞳に映るセカイ》、天空/王菲）与任务规定的 6 组必过用例。
// 分数断言一律近似比较（1e-6，浮点路径不逐位断言）；排序断言断下标序列；判定断枚举值。
//
// 口径注记（均为 match.h 头注声明过的设计选择，非缺陷）：
//   * 编辑距离在规范化后的 UTF-8 字节上按单位代价计（CJK 一字差 = 3 次代价，两侧同口径）；
//   * 版本 +0.1 奖励仅在 title 基分 > 0 时叠加，封顶 1.0（封顶后经 why 的 "+ver" 观察）；
//   * duration_score 对 fileSec<=0 与 candMs<=0 同为中性 0.5；
//   * 「并列按 titleScore」：除全并列外，离散的分项取值（{0,0.5,0.6,0.75,0.8,...}×权重）
//     在浮点上构造不出「总分相同而 titleScore 不同」的稳定并列——逐位相等依赖计算路径，
//     不可靠（正是铁律 3 说的假判据），故只断言全并列时按下标稳定。
//
// ⚠️ 未解决项（铁律 4）：无。以本断言集作为 src/meta/match.h 的回归基线。
#include "meta/match.h"
#include <cstdio>
#include <string>
#include <vector>

// 本测试按 spec 不引入任何 Windows 头；只声明用到的唯一 C 接口
// （kernel32!SetConsoleOutputCP，x64 下 __stdcall 即默认调用约定）
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace rta::meta;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
static void ck_eq(const std::string& got, const std::string& want, const char* what) {
    ck(got == want, what, "got=[" + got + "] want=[" + want + "]");
}
static bool approx(double a, double b) { const double d = a - b; return d < 1e-6 && d > -1e-6; }
static void ck_d(double got, double want, const char* what, const std::string& detail = "") {
    ck(approx(got, want), what,
       "got=" + std::to_string(got) + " want=" + std::to_string(want) +
       (detail.empty() ? "" : "  [" + detail + "]"));
}
static Candidate cand(const char* title, const char* artist, long long ms) {
    Candidate c;
    c.title = title;
    c.artist = artist;
    c.durationMs = ms;
    return c;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== meta/match 候选评分与选择（离线）==\n");

    // ---- 例1（任务用例 1）：晴天/周杰伦/269s 四候选 → 排序、top=叶惠美、confident ----
    {
        std::printf("- 例1 晴天/周杰伦/269s 四候选排序\n");
        std::vector<Candidate> cs = {
            cand("晴天", "周杰伦", 269000),           // 叶惠美（smoke §2 #4 的精确命中形态）
            cand("晴天 (Live)", "周杰伦", 295000),
            cand("晴天 (深情版)", "周杰伦", 255000),
            cand("别的歌", "别的艺人", 300000),
        };
        cs[0].album = "叶惠美";   // 标注用；本轮评分不读 album
        const std::vector<size_t> rk = match_rank(cs, "晴天", "周杰伦", "", 269);
        ck(rk.size() == 4, "例1 返回 4 个下标");
        ck(rk[0] == 0, "例1 top=叶惠美版(269s)",
           "rk[0]=" + std::to_string(rk[0]));
        ck(rk[1] == 1 && rk[2] == 2 && rk[3] == 3, "例1 完整顺序 0,1,2,3",
           "rk=" + std::to_string(rk[1]) + std::to_string(rk[2]) + std::to_string(rk[3]));

        const std::vector<Scored> all = match_score_all(cs, "晴天", "周杰伦", "", 269);
        ck(all[0].idx == 0 && all[1].idx == 1 && all[2].idx == 2 && all[3].idx == 3,
           "例1 Scored.idx 与排序一致");
        ck_d(all[0].score, 1.0, "例1 top 总分 1.0（题/艺人/时长全中）");
        ck_d(all[1].score, 0.85, "例1 Live 版 0.85（括号豁免 title=1.0，26s 时长差=0）",
           all[1].why);
        ck_d(all[2].score, 0.85, "例1 深情版 0.85（14s 时长差=0）", all[2].why);
        ck(all[1].score == all[2].score, "例1 两版并列（浮点逐位相等）");
        ck_d(all[3].score, 0.0, "例1 无关曲 0 分");
        ck(match_confident(cs, "晴天", "周杰伦", "", 269) == true,
           "例1 confident=true（1.0 领先 0.15）");
        ck(match_verdict(cs, "晴天", "周杰伦", "", 269) == MatchVerdict::Confident,
           "例1 verdict=Confident");
    }

    // ---- 例2（任务用例 2）：零标签场景（同人盘：澤田なつ） ----
    {
        std::printf("- 例2 零标签：澤田なつ/紅い瞳に映るセカイ/336s\n");
        std::vector<Candidate> cs = {
            cand("紅い瞳に映るセカイ", "澤田なつ", 336000),
            cand("星屑クロニクル", "別の社団", 200000),
        };
        const std::vector<size_t> rk =
            match_rank(cs, "紅い瞳に映るセカイ", "澤田なつ (さわだ なつ)", "", 336);
        ck(rk.size() == 2 && rk[0] == 0, "例2 top=同曲",
           "rk[0]=" + std::to_string(rk[0]));
        const std::vector<Scored> all =
            match_score_all(cs, "紅い瞳に映るセカイ", "澤田なつ (さわだ なつ)", "", 336);
        ck_d(all[0].titleScore, 1.0, "例2 title 全等 1.0");
        ck_d(all[0].artistScore, 0.8, "例2 artist 含读音括号 → 规范化后包含 0.8", all[0].why);
        ck_d(all[0].score, 0.94, "例2 top 总分 0.94");
        ck_d(all[1].score, 0.0, "例2 无关曲 0 分");
        ck(match_confident(cs, "紅い瞳に映るセカイ", "澤田なつ (さわだ なつ)", "", 336) == true,
           "例2 confident=true");
    }

    // ---- 例3（任务用例 3）：版本豁免与 +0.1 奖励 ----
    {
        std::printf("- 例3 版本豁免与奖励\n");
        ck_d(title_score("天空", "天空 (Live)"), 1.0, "例3 候选 (Live) 剥离后全等 1.0（豁免不惩罚）");
        ck_d(title_score("天空 (Live)", "天空"), 1.0, "例3 查询侧带括号对称豁免");
        const Scored s = match_score_detail("天空", "", "", 0,
                                            cand("天空 (Live)", "王菲", 0), "Live");
        ck(s.titleScore == 1.0, "例3 version=Live 命中后 title 分量仍 1.0（封顶）",
           std::to_string(s.titleScore));
        ck(s.why.find("+ver") != std::string::npos, "例3 封顶下的奖励经 why 的 +ver 可观察",
           s.why);
        // 奖励在封顶之下可见：一字符 CJK 差基分 0.5（字节级 d=3/L=6），+0.1 → 0.6
        ck_d(title_score("晴田", "晴天 (Live)"), 0.5, "例3 无 version：一字符差基分 0.5");
        ck_d(title_score("晴田", "晴天 (Live)", "Live"), 0.6, "例3 version 命中 → 0.5+0.1");
        ck_d(title_score("晴田", "晴天 (Live)", "DJ版"), 0.5, "例3 version 不同 → 不加");
        ck_d(title_score("完全不同歌", "天空 (Live)", "Live"), 0.0,
             "例3 基分 0 时版本不给续命（设计取舍）");
    }

    // ---- 例4（任务用例 4）：中性路径与时长边界 ----
    {
        std::printf("- 例4 中性路径与时长边界\n");
        ck_d(artist_score("", "王菲", "周杰伦"), 0.5, "例4 qArtist 空 → 0.5 中性");
        ck_d(duration_score(269, 0), 0.5, "例4 candMs 缺失 → 0.5 中性");
        ck_d(duration_score(0, 269000), 0.5, "例4 fileSec 缺失 → 0.5（对称扩展，见头注）");
        ck_d(title_score("", ""), 0.5, "例4 双空 title → 0.5 中性");
        ck_d(title_score("晴天", ""), 0.0, "例4 单空 title → 0.0");
        ck_d(duration_score(269, 269000), 1.0, "例4 时长全等 1.0");
        ck_d(duration_score(269, 272000), 1.0, "例4 差 3s → 1.0（边界含）");
        ck_d(duration_score(269, 272001), 0.6, "例4 差 3001ms → 0.6");
        ck_d(duration_score(269, 279000), 0.6, "例4 差 10s → 0.6（边界含）");
        ck_d(duration_score(269, 279001), 0.0, "例4 差 10001ms → 0.0");
    }

    // ---- 例5（任务用例 5）：歧义区 ----
    {
        std::printf("- 例5 歧义区（top1≈0.79 领先 0.09）\n");
        std::vector<Candidate> cs = {
            cand("晴天", "周杰伦", 255000),   // title 1.0 + artist 0.8（查询多「乐队」二字，包含）+ 时长 14s→0 = 0.79
            cand("晴天", "林俊杰", 269000),   // title 1.0 + artist 0 + 时长 0 → 1.0 = 0.70
        };
        const std::vector<size_t> rk = match_rank(cs, "晴天", "周杰伦乐队", "", 269);
        ck(rk.size() == 2 && rk[0] == 0, "例5 top1 正确", "rk[0]=" + std::to_string(rk[0]));
        const std::vector<Scored> all = match_score_all(cs, "晴天", "周杰伦乐队", "", 269);
        ck_d(all[0].score, 0.79, "例5 top1 ≈ 0.79（< 0.80 不可信线）",
           all[0].why);
        ck_d(all[1].score, 0.70, "例5 第二名 0.70（领先 0.09 < 0.10）");
        ck(all[0].score >= kMatchWeakFloor, "例5 top1 ≥ 0.45（非「无可信」）");
        ck(match_confident(cs, "晴天", "周杰伦乐队", "", 269) == false, "例5 confident=false");
        ck(match_verdict(cs, "晴天", "周杰伦乐队", "", 269) == MatchVerdict::Ambiguous,
           "例5 verdict=Ambiguous（歧义区，上层决定）");
    }

    // ---- 例6（任务用例 6）：规范化（全角/大小写/标点变体） ----
    {
        std::printf("- 例6 规范化\n");
        ck_eq(match_norm("Ｃａｎｄｙ　ＳＴＡＲ！"), "candystar", "例6 全角字母/空格/标点折叠");
        ck_eq(match_norm("candy star"), "candystar", "例6 ASCII 空白去除后拼接");
        ck_eq(match_norm("Candy★"), "candy", "例6 装饰符号 ★ 剥离");
        ck_eq(match_norm("Candy☆"), match_norm("Candy★"), "例6 ★/☆ 变体相等");
        ck_eq(match_norm("ＡＢＣ１２３"), "abc123", "例6 全角数字字母转半角");
        ck_eq(match_norm("周杰伦《七里香》"), "周杰伦七里香", "例6 全角书名号剥离");
        ck_eq(match_norm("晴天（Live）"), "晴天live", "例6 全角括号剥离（parse.h §5.10 缺口不影响 match）");
        ck_eq(match_norm("AC/DC"), "acdc", "例6 ASCII 标点剥离");
        ck_d(title_score("Ｃａｎｄｙ　ＳＴＡＲ！", "candy star"), 1.0, "例6 全角变体规范化后全等");
        ck_d(title_score("AC/DC", "ACDC"), 1.0, "例6 标点变体全等");
        ck_d(title_score("Candy★", "candy star"), 0.75,
             "例6 「Candy★」vs「candy star」：剥 ★ 后包含 → 0.75");
    }

    // ---- 补充：artist 拆分 / feat / why 形态 ----
    {
        std::printf("- 补充：artist 拆分与 feat\n");
        ck_d(artist_score("周杰伦", "", "林俊杰 / 周杰伦 / 五月天"), 1.0, "拆 \" / \" 后任一相等=1.0");
        ck_d(artist_score("周杰伦", "", "周杰伦乐队"), 0.8, "相互包含（候选含查询）=0.8");
        ck_d(artist_score("周杰伦乐队", "", "周杰伦"), 0.8, "相互包含（查询含候选）=0.8");
        ck_d(artist_score("周杰伦", "王菲", "周杰伦"), 1.0, "feat 命中但已 1.0 封顶");
        ck_d(artist_score("周杰伦乐队", "王菲", "周杰伦 / 王菲"), 0.9, "feat 命中 0.8+0.1=0.9");
        ck_d(artist_score("周杰伦", "", "王菲"), 0.0, "完全不沾 → 0.0");
        ck_d(artist_score("周杰伦", "王菲", "王菲"), 0.0, "仅 feat 命中、主艺人不沾 → 仍 0.0（字面口径）");

        const Scored s = match_score_detail("晴天", "周杰伦", "王菲", 269,
                                            cand("晴天 (Live)", "周杰伦 / 王菲", 269000), "Live");
        ck(s.why.find("title:eq+ver") != std::string::npos, "why: title:eq+ver", s.why);
        ck(s.why.find("artist:eq+feat") != std::string::npos, "why: artist:eq+feat", s.why);
        ck(s.why.find("dur:ok:0s") != std::string::npos, "why: dur:ok:0s", s.why);
    }

    // ---- 补充：权重恒等 / 空表 / 单候选 / 全并列稳定 ----
    {
        std::printf("- 补充：权重、空表、单候选、并列稳定\n");
        const Candidate c = cand("晴天", "周杰伦", 269000);
        ck_d(match_score("晴天", "周杰伦", "", 269, c),
             0.55 * 1.0 + 0.30 * 1.0 + 0.15 * 1.0, "match_score 权重恒等 0.55/0.30/0.15");
        const Scored d = match_score_detail("晴天", "", "", 0, cand("晴天 (DJ版)", "", 0), "");
        ck_d(d.score, 0.55 * 1.0 + 0.30 * 0.5 + 0.15 * 0.5,
             "中性分量组合：title 豁免 1.0 + artist 空 0.5 + 时长缺 0.5", d.why);

        std::vector<Candidate> none;
        ck(match_rank(none, "晴天", "周杰伦", "", 269).empty(), "空候选表 → 空排名");
        ck(match_confident(none, "晴天", "周杰伦", "", 269) == false, "空候选表 → not confident");
        ck(match_verdict(none, "晴天", "周杰伦", "", 269) == MatchVerdict::Weak,
           "空候选表 → Weak");

        std::vector<Candidate> one = { cand("晴天", "周杰伦", 269000) };
        ck(match_verdict(one, "晴天", "周杰伦", "", 269) == MatchVerdict::Confident,
           "单候选高分 → Confident（无第二名视为领先成立）");
        std::vector<Candidate> oneweak = { cand("别的歌", "别人", 300000) };
        ck(match_verdict(oneweak, "晴天", "周杰伦", "", 269) == MatchVerdict::Weak,
           "单候选低分 → Weak");

        std::vector<Candidate> tie = { cand("晴天", "周杰伦", 269000),
                                       cand("晴天", "周杰伦", 269000) };
        const std::vector<size_t> trk = match_rank(tie, "晴天", "周杰伦", "", 269);
        ck(trk[0] == 0 && trk[1] == 1, "全并列按下标稳定（titleScore 次级规则在比较器内，见文件头注记）");
    }

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
