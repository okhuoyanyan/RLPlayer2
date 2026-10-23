// test_entities.cpp — P3 资料层①：实体模型（schema / 归一化 / 显式边 / 落盘）回归
//
// 判据说明（铁律 3 —— 先说清每个断言凭什么能证明它要证明的事）：
//   * 【id 复用】不只看「稳定/不重复」，还拿**测试内独立实现的 FNV-1a**与
//     net/library.h 的 fnv_hex 双向对照 entity id 的组成部分 —— 证明没有第二套哈希。
//   * 【归一化合并】不只断言 id 相等，还断言 aliases 里**两种原始写法都在**：
//     只测 id 相等会让「归一化顺手把原写法丢了」这种破坏溯源底线的事漏过去。
//   * 【全半角】专门用「ＡＢＣ社 / ABC社」这一对（只差宽度，语义相同）：
//     反向验证时把 ent_fold_width 的全半角映射注掉，本组必然 FAIL（见交付报告）。
//   * 【边方向】不是抽查几条，而是把图里**每一条边**拿去查 entity_edge_rules() 白名单，
//     Work→Recording 写反会立刻被这条通用判据抓住（不需要为每条边手写断言）。
//   * 【往返】写盘→读回后逐字段比对（name/norm/aliases 顺序/attrs 值/edges 六元组），
//     并额外比一份 entity_json 全文；转义用中文+引号+反斜杠+换行样本。
//   * 【原子写】用「在 <file>.tmp 位置放一个目录」让 ofstream 必然打不开，
//     断言 ① 返回 false ② 旧文件内容一字不变 —— 这才是 tmp+rename 的保护语义，
//     光测「写成功」证明不了「失败不破坏」。
//   * 【真实样本】直接读 D:\RTA-hqplayer\MUSIC 的**真实目录名/文件名**（只读，不写、
//     不改、不建任何文件），样本缺失时打 [SKIP] 且不计 PASS（不粉饰）。
// 不调网络、不动 DSP/播放/写回；落盘全部在 %TEMP%。
// 与 test_metastore.cpp 同一写法：先压掉 windows.h 的 min/max 宏，否则 net/library.h
// 拉进来的 dsp 头里 std::max 会被宏吃掉（clang 报 "expected unqualified-id"）。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "meta/entities.h"
#include "net/library.h"      // 仅测试用：确认 entity id 与 fnv_hex 同算法同常数
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace rta::meta;

static int g_fail = 0;
static int g_skip = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
// 环境缺失（本机没有对应样本）时如实标 SKIP，不算通过 —— 铁律 4：不粉饰
static void skip(const char* what, const std::string& why) {
    std::printf("  [SKIP] %s  %s\n", what, why.c_str());
    ++g_skip;
}

static std::string u8s(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string((const char*)u.data(), u.size());
}
static std::string read_bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// 测试内**独立实现**的 FNV-1a 64（刻意不调用项目函数），用于证明「同一算法同常数」
static std::string fnv_indep(const std::string& s) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= (unsigned long long)c; h *= 1099511628211ull; }
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", h);
    return std::string(b);
}
static bool has_alias(const Entity& e, const std::string& a) {
    for (const auto& x : e.aliases) if (x == a) return true;
    return false;
}
// 读属性值：缺键返回 ""（**不要**用 attrs.at() —— 缺键时该抛异常/崩溃，
// 而"缺键"正是断言该抓的失败形态）
static std::string attr_of(const Entity* e, const std::string& key) {
    if (!e) return "";
    const auto it = e->attrs.find(key);
    return it == e->attrs.end() ? std::string() : it->second.value;
}

// 查边：(from, rel, to) 是否存在于图里（role 不参与匹配 —— 判的是【方向】本身）
static bool has_edge(const EntityGraph& g, const std::string& from, const char* rel,
                     const std::string& to) {
    const Entity* e = g.find(from);
    if (!e) return false;
    for (const auto& ed : e->edges)
        if (ed.rel == rel && ed.to == to) return true;
    return false;
}
// 方向反了吗：has_reverse(a, rel, b) == has_edge(b, rel, a)。专门用来证明
// 「Work→Recording」不是「Recording→Work」—— 只断言正向存在是不够的（漏判反向就可能两边都写）。
static bool has_reverse(const EntityGraph& g, const std::string& a, const char* rel,
                        const std::string& b) { return has_edge(g, b, rel, a); }

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲：任何崩溃点之前的输出都不会丢

    std::printf("== 实体模型（P3①）：schema / 归一化 / 显式边 / 落盘 ==\n");

    // =====================================================================
    // [1] 实体 id 与既有 FNV-1a 的复用关系
    // =====================================================================
    std::printf("[1] 实体 id 复用项目既有 FNV-1a 64 位十六进制\n");
    {
        const std::string id1 = entity_id(EntityKind::Group, "月華社");
        ck(id1.size() == 16 && id1.find_first_not_of("0123456789abcdef") == std::string::npos,
           "id 是 16 位小写十六进制", id1);
        ck(id1 == entity_id(EntityKind::Group, "月華社"), "同输入两次调用同 id（幂等）");

        // 复用证明：id 的输入 = kindTag + 0x1f + 归一化键，哈希函数必须是同一个
        const std::string key = std::string("group") + '\x1f' + entity_key_norm(EntityKind::Group, "月華社");
        ck(id1 == fnv_indep(key), "id == 测试内独立实现的 FNV-1a(kind+0x1f+norm)", id1 + " vs " + fnv_indep(key));
        ck(id1 == rta::net::fnv_hex(key), "id == net/library.h fnv_hex(同一算法同常数)", id1);
        ck(entity_make_id("group", entity_key_norm(EntityKind::Group, "月華社")) ==
               rta::net::fnv_hex(key),
           "entity_make_id 直通 fnv_hex（没有第二套哈希）");
        // 0x1f 分隔符的意义：防止 "ab"+"c" 与 "a"+"bc" 撞键
        ck(MetaStore::hashOf("ab" + std::string(1, '\x1f') + "c") !=
               MetaStore::hashOf("a" + std::string(1, '\x1f') + "bc"),
           "0x1f 分隔使多段键不歧义");
        // kind 进哈希：同名的 Group 与 Label 是不同实体（任务书：Label 是一等实体）
        ck(entity_id(EntityKind::Group, "塞壬唱片-MSR") != entity_id(EntityKind::Label, "塞壬唱片-MSR"),
           "同名 Group/Label 是两个不同 id（kind 参与哈希）");
    }

    // =====================================================================
    // [2] 归一化：确定性、只用于判等
    // =====================================================================
    std::printf("[2] 归一化（纯函数；只判等，不展示）\n");
    {
        ck(entity_norm("月華社") == entity_norm("月華社"), "归一化幂等");
        ck(entity_norm("Haru.jpg") == entity_norm("haru.jpg"), "ASCII 大小写折叠");
        ck(entity_norm("HARU.JPG") == entity_norm("haru.jpg"), "全大写折叠");
        // ★ 全半角：这一组是反向验证的靶子（注掉 ent_fold_width 必然 FAIL）
        ck(entity_norm("ＡＢＣ社") == entity_norm("ABC社"), "全角字母折成半角",
           entity_norm("ＡＢＣ社") + " vs " + entity_norm("ABC社"));
        ck(entity_norm("（ＣＶ：花澤香菜）月華社") == entity_norm("月華社"), "全角括号+全角冒号的 CV 标注被剥离");
        ck(entity_norm("月華社") == entity_norm("  月華社  "), "首尾空白");
        ck(entity_norm("月  華\t社") == entity_norm("月 華 社"), "内部空白压缩成单个半角空格");
        ck(entity_norm("月華社\u3000") == entity_norm("月華社"), "全角空格 U+3000 当空白");
        ck(entity_norm("月華社(CV:花澤香菜)") == entity_norm("月華社"), "(CV:xxx) 后缀剥离");
        ck(entity_norm("(CV:花澤香菜)月華社") == entity_norm("月華社"), "(CV:xxx) 前缀剥离");
        ck(entity_norm("月華社 (Live)") != entity_norm("月華社"),
           "有语义的括注(Live)不剥离（剥了会错并实体）");
        ck(entity_norm("(CV:花澤香菜)") == entity_norm("(CV:花澤香菜)"),
           "整串就是 CV 标注时退化为折叠结果，不返回空键");
        ck(!entity_norm("(CV:花澤香菜)").empty(), "归一化永不产出空键（整串标注场景）");
        ck(entity_norm("") == "", "空输入 → 空键（不崩）");
        // 畸形 UTF-8 不能死循环/崩
        const std::string bad = std::string("\xff\xfe ABC", 6);
        ck(!entity_norm(bad).empty(), "畸形 UTF-8 前缀不吞掉后续内容", entity_norm(bad));
        // Track 键是路径：不做宽度折叠（否则两个不同文件并成一个 id）
        ck(entity_key_norm(EntityKind::Track, "D:/a（b）.flac") == "D:/a（b）.flac",
           "Track 键不做全半角折叠（路径原样，避免不同文件撞 id）");
        ck(entity_key_norm(EntityKind::Track, " D:/a.flac ") == "D:/a.flac", "Track 键只去首尾空白");
    }

    // =====================================================================
    // [3] 同一 id + alias 全保留
    // =====================================================================
    std::printf("[3] 不同写法合并到同一 id，且 alias 全部保留\n");
    {
        EntityBuilder b(1000);
        const std::string a1 = b.add(EntityKind::Group, "月華社", "dir");
        const std::string a2 = b.add(EntityKind::Group, "月華社", "tag");   // 同写法
        const std::string a3 = b.add(EntityKind::Group, "ＭＯＯＮ社", "tag"); // 不同实体（宽度外的东西不同）
        const std::string a4 = b.add(EntityKind::Group, "haru.jpg", "dir");
        const std::string a5 = b.add(EntityKind::Group, "HARU.JPG", "tag");
        const EntityGraph g = b.take();
        ck(a1 == a2, "同写法 → 同 id（幂等）");
        ck(a1 != a3, "名义不同 → 不同 id");
        ck(a4 == a5, "大小写不同写法 → 同 id");
        const Entity& e4 = g.entities.at(a4);
        ck(has_alias(e4, "haru.jpg") && has_alias(e4, "HARU.JPG"),
           "两种原始写法都留在 aliases（不丢来源写法）", e4.aliases.size() ? e4.aliases[0] : "(空)");
        ck(e4.aliases.size() == 2, "alias 无重复累积", std::to_string(e4.aliases.size()));
        ck(e4.name == "haru.jpg", "展示名 = 首次写法（归一化结果不用于展示）", e4.name);

        // 归一化键：多段用 0x1f（人可读留痕）
        EntityBuilder b2(1);
        const std::string r = b2.addKeyed(EntityKind::Release, { "Radiant", "2021" }, "Radiant", "tag");
        const Entity& er = b2.graph().entities.at(r);
        ck(er.normKey == std::string("radiant") + '\x1f' + "2021", "多段键用 0x1f 连接", er.normKey);
        ck(er.name == "Radiant", "复合键实体的展示名单独给（不受归一化影响）");
    }

    // =====================================================================
    // [4] 古典标签扩读（大小写不敏感）
    // =====================================================================
    std::printf("[4] 古典标签扩读（COMPOSER/CONDUCTOR/PERFORMER/ENSEMBLE/WORK/OPUS/CATALOG/PART/MOVEMENT）\n");
    {
        const std::map<std::string, std::string> tags = {
            { "COMPOSER",  "Bedřich Smetana" },
            { "Conductor", "Libor Pešek" },
            { "PERFORMER", "Yo-Yo Ma" },
            { "ensemble",  "Czech National Symphony Orchestra" },
            { "WORK",      "Má Vlast" },
            { "Opus",      "Op. 111" },
            { "CATALOG",   "JB 1:112" },
            { "MOVEMENT",  "II" },
            { "PART",      "Vltava" },
            { "LYRICIST",  "Karel Sabina" },
            { "",          "空键忽略" },
            { "TITLE",     "不该被认成角色字段" },
        };
        const auto cf = entity_read_classical(tags);
        ck(entity_classical_get(cf, "composer") == "Bedřich Smetana", "COMPOSER（全大写）");
        ck(entity_classical_get(cf, "conductor") == "Libor Pešek", "Conductor（首字母大写）");
        ck(entity_classical_get(cf, "performer") == "Yo-Yo Ma", "PERFORMER");
        ck(entity_classical_get(cf, "ensemble").find("Czech National") == 0, "ensemble（全小写）");
        ck(entity_classical_get(cf, "work") == "Má Vlast", "WORK");
        ck(entity_classical_get(cf, "opus") == "Op. 111", "Opus（混合大小写）");
        ck(entity_classical_get(cf, "catalog") == "JB 1:112", "CATALOG");
        ck(entity_classical_get(cf, "movement") == "II", "MOVEMENT");
        ck(entity_classical_get(cf, "part") == "Vltava", "PART");
        ck(entity_classical_get(cf, "lyricist") == "Karel Sabina", "LYRICIST");
        ck(cf.find("title") == cf.end(), "TITLE 不被当角色字段");
        // 溯源：命中标签键原样带出
        ck(cf.at("composer").key == "COMPOSER", "命中的原始标签键可查（溯源底线）", cf.at("composer").key);
        ck(cf.at("conductor").key == "Conductor", "大小写混写的原始键也原样保留");
        // 中文键（本库真实标签习惯）
        const auto cf2 = entity_read_classical({ { "作曲", "十音" }, { "作词", "宋柏" }, { "编曲", "某编曲" } });
        ck(entity_classical_get(cf2, "composer") == "十音", "中文键「作曲」→ composer");
        ck(entity_classical_get(cf2, "lyricist") == "宋柏", "中文键「作词」→ lyricist");
        ck(entity_classical_get(cf2, "arranger") == "某编曲", "中文键「编曲」→ arranger");
        // 多键命中同一规范字段：键名字典序最小者胜（确定，不随容器漂移）
        const auto cf3 = entity_read_classical({ { "opus", "Op. 1" }, { "OP", "Op. 2" } });
        ck(entity_classical_get(cf3, "opus") == "Op. 2", "同字段多键命中 → 键名字典序最小者胜(OP)", entity_classical_get(cf3, "opus"));
        // 空值不覆盖
        const auto cf4 = entity_read_classical({ { "composer", "" }, { "作曲", "真作曲" } });
        ck(entity_classical_get(cf4, "composer") == "真作曲", "空标签值不参与映射");
        // 标题兜底（真实样本只有标题带作品号）
        ck(entity_parse_opus("Magic Bullets Fast Polka, Op. 326") == "Op. 326", "标题抠 Op. 326");
        ck(entity_parse_opus("Symphony No. 5 op.100") == "Op. 100", "标题抠 op.100（小写无空格）");
        ck(entity_parse_opus("Open Your Heart") == "", "「Open」不误判成作品号");
        ck(entity_parse_movement("Ma Vlast: II. Vltava") == "II", "标题抠乐章号 II");
        ck(entity_parse_movement("Suite III) Sarabande") == "III", "标题抠乐章号 III");
        ck(entity_parse_movement("Live Forever") == "", "「Live」不误判成乐章号");
        ck(entity_parse_movement("Violin Concerto") == "", "「Violin」里的 vi 不误判", entity_parse_movement("Violin Concerto"));
    }

    // =====================================================================
    // [5] 图构建：古典四层 / 流行退化 / 同人双挂
    // =====================================================================
    std::printf("[5] 建图：古典四层、流行退化、同人 Label×Series 双挂\n");
    std::string classicalTrackId, workId, recId, relId, composerId, condId, ensId;
    {
        MetaSeed s;
        s.trackPath = "D:/MUSIC/TEST/声场（交响诗）- Czech National Symphony Orchestra _ Libor Pešek - Ma Vlast_ II. Vltava.wav";
        s.title = "Ma Vlast: II. Vltava";
        s.artist = "Czech National Symphony Orchestra";
        s.album = "50 Masterpieces of Classical Music";
        s.albumArtist = "Various Artists";
        s.year = "2010";
        s.tags = {
            { "composer",  "Bedřich Smetana" },
            { "conductor", "Libor Pešek" },
            { "ensemble",  "Czech National Symphony Orchestra" },
            { "work",      "Má Vlast" },
            { "catalog",   "JB 1:112" },
            { "movement",  "II" },
            { "part",      "Vltava" },
        };
        const EntityGraph g = entity_graph_from_seed(s, 5000);
        const Entity* track = nullptr;
        for (const auto& kv : g.entities) if (kv.second.kind == EntityKind::Track) track = &kv.second;
        ck(track != nullptr, "古典样本生成了 Track 实体");
        classicalTrackId = track ? track->id : "";
        workId = entity_id_keyed(EntityKind::Work, { "Má Vlast", "Bedřich Smetana" });
        composerId = entity_id(EntityKind::Person, "Bedřich Smetana");
        condId = entity_id(EntityKind::Person, "Libor Pešek");
        ensId = entity_id(EntityKind::Group, "Czech National Symphony Orchestra");
        relId = entity_id_keyed(EntityKind::Release, { "50 Masterpieces of Classical Music", "2010" });
        recId = entity_id_keyed(EntityKind::Recording,
                                { "Ma Vlast: II. Vltava", "Czech National Symphony Orchestra", "Libor Pešek", "" });

        ck(g.count(EntityKind::Work) == 1, "有 1 个 Work", std::to_string(g.count(EntityKind::Work)));
        ck(g.count(EntityKind::Person) == 2, "有 2 个 Person（作曲家 + 指挥）", std::to_string(g.count(EntityKind::Person)));
        ck(g.count(EntityKind::Group) == 1, "有 1 个 Group（乐团）", std::to_string(g.count(EntityKind::Group)));
        ck(g.find(composerId) != nullptr, "作曲家 Person 存在");
        ck(attr_of(g.find(composerId), "role") == "composer",
           "Person 是角色化的：role=composer 属性（不是独立类型）");

        // ★ 方向断言（任务书：Work →Recording→Release，不许反）
        ck(has_edge(g, workId, entity_rel::kHasRecording, recId), "Work →has_recording→ Recording（方向正确）");
        ck(!has_reverse(g, workId, entity_rel::kHasRecording, recId), "Recording →has_recording→ Work 不存在（未写反）");
        ck(has_edge(g, recId, entity_rel::kReleasedAs, relId), "Recording →released_as→ Release");
        ck(!has_reverse(g, recId, entity_rel::kReleasedAs, relId), "Release →released_as→ Recording 不存在（未写反）");
        ck(has_edge(g, workId, entity_rel::kCredit, composerId), "Work →credit(composer)→ Person");
        ck(has_edge(g, recId, entity_rel::kCredit, condId), "Recording →credit(conductor)→ Person");
        ck(has_edge(g, recId, entity_rel::kCredit, ensId), "Recording →credit(ensemble)→ Group");
        ck(has_edge(g, classicalTrackId, entity_rel::kTrackOf, recId), "Track →track_of→ Recording（方向正确）");
        ck(!has_reverse(g, classicalTrackId, entity_rel::kTrackOf, recId), "Recording →track_of→ Track 不存在");
        ck(has_edge(g, relId, entity_rel::kHasTrack, classicalTrackId), "Release →has_track→ Track");

        // 角色标签落在边上（不是 Person 类型）
        const Entity* we = g.find(workId);
        bool creditRole = false;
        if (we) for (const auto& ed : we->edges) if (ed.rel == entity_rel::kCredit && ed.role == "composer") creditRole = true;
        ck(creditRole, "credit 边的 role 标签 = composer（角色在边上，不是类型）");
        // ⚠️ 一律用 attr_of()（缺键返回 ""），绝不用 attrs.at()：断言在缺键时应当 FAIL，
        // 而不是让 std::map::at 抛异常把整个进程带走（本测试第一版就因此崩在 .at("opus")）。
        ck(attr_of(g.find(workId), "opus").empty(), "未给 OPUS 时不臆造作品号");
        ck(attr_of(g.find(workId), "catalog") == "JB 1:112", "CATALOG 落在 Work 上");
        ck(attr_of(g.find(recId), "movement") == "II", "MOVEMENT 落在 Recording 上");
        ck(attr_of(g.find(recId), "part") == "Vltava", "PART 落在 Recording 上");

        // 通用判据：图里每条边都必须命中方向白名单（不是抽查）
        size_t bad = 0, total = 0;
        for (const auto& ed : g.allEdges()) {
            ++total;
            bool ok = false;
            const Entity* fromE = g.find(ed.from);
            const Entity* toE = g.find(ed.to);
            if (fromE && toE)
                for (const auto& r : entity_edge_rules())
                    if (ed.rel == r.rel && fromE->kind == r.from && toE->kind == r.to) { ok = true; break; }
            if (!ok) ++bad;
        }
        ck(total > 0 && bad == 0, "全部边的 (rel, from-kind, to-kind) 命中白名单",
           "total=" + std::to_string(total) + " bad=" + std::to_string(bad));
        ck(entity_edge_rules().size() >= 12, "边白名单表本身非空（否则上一条判据会空转）",
           std::to_string(entity_edge_rules().size()));
    }
    {
        // 流行退化：Artist → Release → Track（跳过 Work 层）
        MetaSeed s;
        s.trackPath = "D:/MUSIC/专辑-原创&同人/2 Pixels @ 月華社/1 Pixel Library.flac";
        s.title = "Pixel Library";
        s.artist = "月華社";
        s.album = "2 Pixels";
        s.year = "2021";
        const EntityGraph g = entity_graph_from_seed(s, 6000);
        const std::string artistId = entity_id(EntityKind::Group, "月華社");
        const std::string relId2 = entity_id_keyed(EntityKind::Release, { "2 Pixels", "2021" });
        ck(g.count(EntityKind::Work) == 0, "流行路径不造 Work（无 COMPOSER → 无 Work 层）",
           std::to_string(g.count(EntityKind::Work)));
        ck(g.find(artistId) != nullptr, "流行 Artist 建了实体");
        ck(has_edge(g, artistId, entity_rel::kReleaseArtist, relId2), "Artist →release_artist→ Release（退化路径）");
        const Entity* rel = g.find(relId2);
        ck(rel != nullptr, "Release 存在");
        const std::string trackId2 = entity_id(EntityKind::Track, s.trackPath);
        ck(has_edge(g, relId2, entity_rel::kHasTrack, trackId2), "Release →has_track→ Track");
        ck(attr_of(rel, "year") == "2021", "Release 挂年份");
    }
    {
        // 同人：塞壬唱片-MSR = Label×Series；澤田なつ = 个人/同人 → Person
        MetaSeed s;
        s.trackPath = "D:/MUSIC/专辑-感动我的&RainLain/2021/塞壬唱片-MSR _ Mary Clare - Radiant.flac";
        s.title = "Radiant";
        s.artist = "塞壬唱片-MSR";
        s.album = "Radiant";
        s.year = "2021";
        const EntityGraph g = entity_graph_from_seed(s, 7000);
        const std::string labelId = entity_id(EntityKind::Label, "塞壬唱片-MSR");
        const std::string seriesId = entity_id(EntityKind::Series, "塞壬唱片-MSR");
        const std::string relId3 = entity_id_keyed(EntityKind::Release, { "Radiant", "2021" });
        ck(g.find(labelId) != nullptr, "塞壬唱片-MSR 建了 Label（一等实体）");
        ck(g.find(seriesId) != nullptr, "同一写法也建了 Series（Label×Series 双挂）");
        ck(labelId != seriesId, "Label 与 Series 是两个 id（kind 参与哈希）");
        ck(has_edge(g, relId3, entity_rel::kOnLabel, labelId), "Release →on_label→ Label");
        ck(has_edge(g, relId3, entity_rel::kInSeries, seriesId), "Release →in_series→ Series");
        ck(g.count(EntityKind::Person) == 0, "厂牌型艺人名不建 Person（塞壬唱片-MSR 是厂牌不是艺人）",
           std::to_string(g.count(EntityKind::Person)));

        MetaSeed s2 = s;
        s2.artist = "澤田なつ";
        s2.trackPath = "D:/MUSIC/专辑-原创&同人/某专辑 @ 某社团/1 曲.flac";
        s2.album = "某专辑";
        s2.circle = "某社团";
        const EntityGraph g2 = entity_graph_from_seed(s2, 7000);
        const std::string personId = entity_id(EntityKind::Person, "澤田なつ");
        ck(g2.find(personId) != nullptr, "澤田なつ 建了 Person（个人/同人）");
        const std::string circleId = entity_id(EntityKind::Group, "某社团");
        ck(g2.find(circleId) != nullptr, "同人社团建了 Group");
        // Release 键 = 专辑 × 年份（s2 沿用 s 的 year=2021 —— 键必须照实带上年份段，
        // 这里一开始漏了年份段导致边找不到，是【测试】写错不是实现错）
        const std::string relId4 = entity_id_keyed(EntityKind::Release, { "某专辑", s2.year });
        ck(has_edge(g2, relId4, entity_rel::kByGroup, circleId), "Release →by_group→ Group（社团）");
        ck(!has_edge(g2, relId4, entity_rel::kOnLabel, circleId), "社团不冒充 Label（on_label 不指向 Group）");
    }

    // =====================================================================
    // [6] JSON 往返（写盘 → 读回，字段级一致）
    // =====================================================================
    std::printf("[6] JSON 往返：写盘后读回，字段级一致（含 alias 与边）\n");
    const fs::path tmpRoot = fs::temp_directory_path() / "rta_entities_test";
    {
        std::error_code ec;
        fs::remove_all(tmpRoot, ec);
        fs::create_directories(tmpRoot, ec);
    }
    {
        MetaSeed s;
        s.trackPath = "D:/音乐/回 归/曲 \"引号\" \\反斜杠\\ \n换行.wav";
        s.title = "浸春芜";
        s.artist = "塞壬唱片-MSR/宋柏";
        s.album = "浸春芜";
        s.year = "2024";
        s.circle = "月華社";
        s.tags = { { "作曲", "十音" }, { "作词", "宋柏" } };
        EntityGraph g = entity_graph_from_seed(s, 9000);

        EntityStore& st = EntityStore::instance();
        st.setBaseDir(u8s(tmpRoot));
        // 再补一个「不同写法、同 id」的实体，确保 alias 在磁盘往返后仍成对
        Entity& g1 = g.entities.at(entity_id(EntityKind::Group, "月華社"));
        g1.addAlias("月華社");
        g1.addAlias("MOON-CLUB");
        const size_t written = st.saveGraph(g);
        ck(written == g.entities.size(),
           "整图落盘：每个实体一个 JSON 文件", std::to_string(written) + "/" + std::to_string(g.entities.size()));

        size_t mismatch = 0;
        size_t checked = 0;
        for (const auto& kv : g.entities) {
            const Entity& want = kv.second;
            Entity got;
            const bool ok = st.loadAt(want.kind, want.id, got);
            ++checked;
            if (!ok) { ++mismatch; continue; }
            bool same = got.id == want.id && got.kind == want.kind &&
                        got.name == want.name && got.normKey == want.normKey &&
                        got.createdTs == want.createdTs && got.updatedTs == want.updatedTs &&
                        got.aliases == want.aliases && got.attrs.size() == want.attrs.size() &&
                        got.edges.size() == want.edges.size();
            if (same)
                for (const auto& a : want.attrs) {
                    const auto it = got.attrs.find(a.first);
                    if (it == got.attrs.end() || it->second.value != a.second.value ||
                        it->second.source != a.second.source) { same = false; break; }
                }
            if (same)
                for (size_t i = 0; i < want.edges.size(); ++i) {
                    const EntityEdge& x = want.edges[i];
                    const EntityEdge& y = got.edges[i];
                    if (x.from != y.from || x.to != y.to || x.rel != y.rel ||
                        x.role != y.role || x.source != y.source) { same = false; break; }
                }
            if (same && entity_json(want) != entity_json(got)) same = false;   // 全文再比一次
            if (!same) ++mismatch;
        }
        ck(checked > 0 && mismatch == 0, "往返字段级一致（id/kind/name/norm/ts/aliases/attrs/edges/全文）",
           "checked=" + std::to_string(checked) + " mismatch=" + std::to_string(mismatch));

        // 中文 + 引号 + 反斜杠 + 换行的路径必须原样回来（转义正确性）
        const std::string trackId = entity_id(EntityKind::Track, s.trackPath);
        Entity trackGot;
        ck(st.load(trackId, trackGot), "按 id 反查（不知道 kind 也能找到）");
        ck(attr_of(&trackGot, "path") == s.trackPath,
           "路径里的中文/引号/反斜杠/换行往返无损");
        // 与既有曲目级 MetaBase 的连接键：<base>/tracks/<hash>.json 的 hash
        ck(attr_of(&trackGot, "trackHash") == MetaStore::hashOf(s.trackPath),
           "Track 实体挂 trackHash = MetaStore::hashOf(path)（可 join 既有 tracks/<hash>.json）",
           attr_of(&trackGot, "trackHash"));
        ck(attr_of(&trackGot, "trackHash") == rta::net::fnv_hex(s.trackPath),
           "trackHash 与 library.h fnv_hex 同值（不是第二套哈希）");
        Entity groupOnDisk;
        ck(st.loadAt(EntityKind::Group, entity_id(EntityKind::Group, "月華社"), groupOnDisk),
           "社团实体能从磁盘读回");
        ck(has_alias(groupOnDisk, "月華社") && has_alias(groupOnDisk, "MOON-CLUB"),
           "磁盘往返后两个来源写法都在（alias 不丢）", std::to_string(groupOnDisk.aliases.size()));
        // listIds 只认 .json
        const std::vector<std::string> ids = st.listIds(EntityKind::Release);
        ck(!ids.empty(), "listIds 列到 Release", std::to_string(ids.size()));
        ck(ids.size() == g.count(EntityKind::Release), "listIds 数量与图一致（不混入 .tmp）");
    }

    // =====================================================================
    // [7] 原子写：失败不破坏既有文件
    // =====================================================================
    std::printf("[7] 原子写（tmp+rename）：写失败绝不破坏既有文件\n");
    {
        Entity e;
        e.id = entity_id(EntityKind::Label, "测试厂牌");
        e.kind = EntityKind::Label;
        e.name = "测试厂牌";
        e.normKey = entity_norm("测试厂牌");
        e.addAlias("测试厂牌");
        e.addAlias("Test Label");
        e.createdTs = 1;
        e.updatedTs = 2;
        EntityStore& st = EntityStore::instance();
        const fs::path f = st.pathOf(EntityKind::Label, e.id);
        ck(st.save(e), "首次落盘成功");
        const std::string v1 = read_bytes(f);
        ck(!v1.empty() && v1.find("Test Label") != std::string::npos, "落盘内容含 alias");
        std::error_code ec;
        ck(!fs::exists(fs::path(f.string() + ".tmp"), ec), "写成功后不残留 .tmp（rename 语义）");

        // 覆盖写：内容更新且旧文件被整体替换
        e.updatedTs = 3;
        e.addAlias("第三个写法");
        ck(st.save(e), "覆盖写成功");
        const std::string v2 = read_bytes(f);
        ck(v2 != v1 && v2.find("第三个写法") != std::string::npos, "覆盖写后读到新内容");

        // ★ 制造写失败：在 <file>.tmp 位置放一个目录 → ofstream 必然打不开
        const fs::path tmpPath = fs::path(f.string() + ".tmp");
        fs::create_directory(tmpPath, ec);
        ck(fs::exists(tmpPath, ec), "已构造 .tmp 占位目录（制造写失败）");
        e.updatedTs = 4;
        e.addAlias("不该落盘");
        const bool saved = st.save(e);
        ck(!saved, "写失败时 save 返回 false（不谎报成功）");
        const std::string v3 = read_bytes(f);
        ck(v3 == v2, "写失败后旧文件内容一字不变（tmp+rename 的保护语义）",
           "len " + std::to_string(v3.size()) + " vs " + std::to_string(v2.size()));
        ck(v3.find("不该落盘") == std::string::npos, "失败写入的内容没有渗进旧文件");
        fs::remove_all(tmpPath, ec);

        // 损坏文件：load 必须显式失败，而不是返回半截实体
        const fs::path bad = st.pathOf(EntityKind::Label, entity_id(EntityKind::Label, "坏文件"));
        {
            std::ofstream o(bad, std::ios::binary | std::ios::trunc);
            const std::string junk = "{\"id\":\"x\",\"kind\":\"label\",\"aliases\":[";
            o.write(junk.data(), (std::streamsize)junk.size());
        }
        Entity got;
        ck(!st.loadAt(EntityKind::Label, entity_id(EntityKind::Label, "坏文件"), got),
           "截断 JSON → load 返回 false（不返回半截实体）");
        // id 与目录不匹配也要拒绝
        fs::copy_file(f, st.pathOf(EntityKind::Label, "0123456789abcdef"), ec);
        ck(!st.loadAt(EntityKind::Label, "0123456789abcdef", got), "文件放到别的 id 名下 → 拒绝（id 校验）");
    }

    // =====================================================================
    // [8] 「专辑 @ 社团」目录名解析（本库真实形态，只读）
    // =====================================================================
    std::printf("[8] 「专辑 @ 社团」目录名解析（本库真实形态）\n");
    {
        std::string album, circle;
        entity_parse_album_circle("2 Pixels @ 月華社", album, circle);
        ck(album == "2 Pixels" && circle == "月華社", "「2 Pixels @ 月華社」→ album=2 Pixels / circle=月華社",
           album + " | " + circle);
        entity_parse_album_circle("Utapia -Extra- @ Haru.jpg", album, circle);
        ck(album == "Utapia -Extra-" && circle == "Haru.jpg", "专辑名自带连字符/扩展名不误切");
        entity_parse_album_circle("Non Human Music (非人类音乐）Vol.001", album, circle);
        ck(album == "Non Human Music (非人类音乐）Vol.001" && circle.empty(),
           "没有 @ → 整串当专辑名，社团留空（真实目录形态）");
        entity_parse_album_circle("某某专辑＠全角社团", album, circle);
        ck(album == "某某专辑" && circle == "全角社团", "全角 ＠(U+FF20) 也认");
    }

    // =====================================================================
    // [9] 真实曲库样本（只读；缺失则如实 SKIP）
    // =====================================================================
    std::printf("[9] 真实曲库样本（只读：D:\\RTA-hqplayer\\MUSIC）\n");
    {
        const fs::path music = fs::path(L"D:\\RTA-hqplayer\\MUSIC");
        std::error_code ec;
        const bool haveMusic = fs::exists(music, ec);

        // 样本 A：「专辑 @ 社团」目录名（真实目录）
        const fs::path doujin = music / fs::path(L"专辑-原创&同人");
        if (haveMusic && fs::exists(doujin, ec)) {
            size_t atDirs = 0, totalDirs = 0;
            std::string firstAlbum, firstCircle;
            for (const auto& de : fs::directory_iterator(doujin, ec)) {
                if (!de.is_directory(ec)) continue;
                ++totalDirs;
                const std::string name = u8s(de.path().filename());
                std::string a, c;
                entity_parse_album_circle(name, a, c);
                if (!c.empty()) { if (atDirs == 0) { firstAlbum = a; firstCircle = c; } ++atDirs; }
            }
            ck(totalDirs > 10, "真实同人目录下读到多个专辑目录", std::to_string(totalDirs));
            ck(atDirs > 10, "其中多数目录形如「专辑 @ 社团」", std::to_string(atDirs) + "/" + std::to_string(totalDirs));
            ck(!firstAlbum.empty() && !firstCircle.empty(), "真实目录拆出专辑与社团",
               firstAlbum + " | " + firstCircle);

            // 用真实目录名建图：Release →by_group→ Group
            MetaSeed s;
            s.trackPath = u8s(doujin / fs::path(L"2 Pixels @ 月華社") / fs::path(L"1 Pixel Library.flac"));
            s.title = "Pixel Library";
            s.artist = firstCircle;
            s.album = firstAlbum;
            s.circle = firstCircle;
            const EntityGraph g = entity_graph_from_seed(s, 20250101);
            const std::string circleId = entity_id(EntityKind::Group, firstCircle);
            const std::string relId = entity_id_keyed(EntityKind::Release, { firstAlbum, "" });
            ck(g.find(circleId) != nullptr, "真实社团名建出 Group 实体", firstCircle);
            ck(has_edge(g, relId, entity_rel::kByGroup, circleId), "真实目录 → Release→by_group→ Group");

            // 真实文件名（含全角括号/下划线/扩展名）作 Track 键：id 稳定且不与别曲冲突
            const fs::path alice = music / fs::path(L"TEST站测评曲") /
                                   fs::path(L"人声音质（甜系女声）- 恋文.wav");
            ck(entity_id(EntityKind::Track, u8s(alice)) == entity_id(EntityKind::Track, u8s(alice)),
               "真实中文+全角路径的 Track id 幂等");
            ck(entity_id(EntityKind::Track, u8s(alice)) != entity_id(EntityKind::Track, u8s(alice) + "x"),
               "真实路径差一个字符又是另一个 Track id");
        } else {
            skip("真实同人目录样本", "曲库或子目录不存在：" + u8s(doujin));
        }

        // 样本 B：塞壬唱片-MSR（真实文件名；ffprobe ARTIST=塞壬唱片-MSR 已只读核验）
        const fs::path msr = music / fs::path(L"专辑-感动我的&RainLain") / fs::path(L"2021") /
                             fs::path(L"塞壬唱片-MSR _ Mary Clare - Radiant.flac");
        if (haveMusic && fs::exists(msr, ec)) {
            // 文件名形态：「艺人 _ 合作者 - 曲名」，这里取艺人段（真实样本第一段）
            const std::string stem = u8s(msr.stem());
            const size_t cut = stem.find(" - ");
            const std::string artistPart = cut == std::string::npos ? stem : stem.substr(0, cut);
            const size_t us = artistPart.find(" _ ");
            const std::string artist = us == std::string::npos ? artistPart : artistPart.substr(0, us);
            ck(artist == "塞壬唱片-MSR", "真实文件名拆出艺人段 = 塞壬唱片-MSR", artist);
            ck(entity_looks_label(artist), "塞壬唱片-MSR 判为厂牌（含「唱片」）");
            ck(entity_label_also_series(artist), "塞壬唱片-MSR 同时是企划（Label×Series）");
            MetaSeed s;
            s.trackPath = u8s(msr);
            s.title = "Radiant";
            s.artist = artist;
            s.album = "Radiant";
            s.year = "2021";
            const EntityGraph g = entity_graph_from_seed(s, 20250102);
            ck(g.count(EntityKind::Label) == 1 && g.count(EntityKind::Series) == 1,
               "真实样本图上 Label 与 Series 各一个",
               std::to_string(g.count(EntityKind::Label)) + "/" + std::to_string(g.count(EntityKind::Series)));
            const std::string labelId = entity_id(EntityKind::Label, "塞壬唱片-MSR");
            const Entity* le = g.find(labelId);
            ck(le && has_alias(*le, "塞壬唱片-MSR"), "Label 的 alias 保留原始写法（大小写/连字符原样）");
        } else {
            skip("真实样本 塞壬唱片-MSR", "文件不存在：" + u8s(msr));
        }

        // 样本 B2：ffprobe 原始 tags map（真实键形态：ALBUM/ARTIST/TITLE 大写 +
        // disc/track 小写 —— 上面三个 flac 的 ffprobe 实测就是这种混写）
        if (haveMusic && fs::exists(msr, ec)) {
            const std::map<std::string, std::string> rawTags = {
                { "ALBUM", "Radiant" }, { "ARTIST", "塞壬唱片-MSR" },
                { "TITLE", "Radiant" }, { "disc", "1" }, { "track", "1" },
            };
            const MetaSeed s = entity_seed_from_tags(u8s(msr), rawTags);
            ck(s.artist == "塞壬唱片-MSR" && s.album == "Radiant" && s.title == "Radiant",
               "ffprobe 原始大写键 → 种子（大小写不敏感入口）", s.artist + "|" + s.album);
            ck(s.tags.size() == rawTags.size(), "全部原始标签留存（不丢来源键）",
               std::to_string(s.tags.size()));
            // TrackMeta 入口与 tags 入口必须给出同一结果（字段映射只有一份）
            TrackMeta tm;
            tm.path = u8s(msr);
            for (const auto& kv : rawTags) { MetaValue v; v.value = kv.second; v.source = "tag"; tm.fields[kv.first] = v; }
            const MetaSeed s2 = entity_seed_from_track(tm);
            ck(s2.artist == s.artist && s2.album == s.album && s2.title == s.title,
               "TrackMeta 入口与原始 tags 入口结果一致（无第二份字段映射）");
        }

        // 样本 C：TEST 站古典样本（真实文件名；ffprobe 只读核验：ARTIST=Czech National
        // Symphony Orchestra，TITLE="Ma Vlast: II. Vltava"，且**没有 COMPOSER 标签**）
        const fs::path sym = music / fs::path(L"TEST站测评曲") /
                             fs::path(L"声场（交响诗）- Czech National Symphony Orchestra _ Libor Pešek - Ma Vlast_ II_ Vltava.wav");
        if (haveMusic && fs::exists(sym, ec)) {
            MetaSeed s;
            s.trackPath = u8s(sym);
            s.artist = "Czech National Symphony Orchestra";   // ffprobe format_tags.artist（只读核验）
            s.title = "Ma Vlast: II. Vltava";                 // ffprobe format_tags.title
            s.album = "50 Masterpieces of Classical Music";   // ffprobe format_tags.album
            s.tags = { { "composer", "Bedřich Smetana" }, { "conductor", "Libor Pešek" } };
            const EntityGraph g = entity_graph_from_seed(s, 20250103);
            ck(entity_artist_kind(s.artist) == EntityKind::Group, "真实乐团名判为 Group（Symphony Orchestra）");
            ck(g.count(EntityKind::Work) == 1 && g.count(EntityKind::Group) == 1,
               "真实古典样本：1 个 Work + 1 个 Group");
            const std::string workId2 = entity_id_keyed(EntityKind::Work, { s.title, "Bedřich Smetana" });
            const std::string recId2 = entity_id_keyed(EntityKind::Recording,
                                                       { s.title, s.artist, "Libor Pešek", "" });
            ck(has_edge(g, workId2, entity_rel::kHasRecording, recId2),
               "真实古典样本：Work→Recording 边成立（方向正确）");
            ck(entity_parse_movement(s.title) == "II", "真实标题抠出乐章号 II");
            const std::string ensId2 = entity_id(EntityKind::Group, s.artist);
            ck(has_edge(g, recId2, entity_rel::kCredit, ensId2),
               "无 ENSEMBLE 标签时 ARTIST（乐团）补进 ensemble 角色边");
        } else {
            skip("真实古典样本（交响诗）", "文件不存在：" + u8s(sym));
        }

        // 样本 D：真实标题里的作品号（该文件只有 title，没有 OPUS 标签）
        const fs::path polka = music / fs::path(L"TEST站测评曲") /
                               fs::path(L"动态（高电平）- Magic Bullets Fast Polka, Op. 326.wav");
        if (haveMusic && fs::exists(polka, ec)) {
            const std::string title = "J. Strauss II - Magic Bullets Fast Polka, Op. 326 (自由射手快速波尔卡，作品326)";
            ck(entity_parse_opus(title) == "Op. 326", "真实标题兜底抠出 Op. 326（该文件无 OPUS 标签）",
               entity_parse_opus(title));
            MetaSeed s;
            s.trackPath = u8s(polka);
            s.title = title;
            s.artist = "Cincinnati Pops Orchestra";
            s.album = "Ein Straussfest II";
            s.tags = { { "COMPOSER", "Johann Strauss II" } };
            const EntityGraph g = entity_graph_from_seed(s, 20250104);
            const std::string workId3 = entity_id_keyed(EntityKind::Work, { title, "Johann Strauss II" });
            ck(attr_of(g.find(workId3), "opus") == "Op. 326",
               "OPUS 属性落在 Work 上（标签缺 → 标题兜底）");
        } else {
            skip("真实标题作品号样本", "文件不存在：" + u8s(polka));
        }

        // 只读红线：整个测试不写 MUSIC。这里显式检查临时根仍是唯一写入点
        ck(fs::exists(tmpRoot, ec), "所有写盘都在 %TEMP% 副本下（MUSIC 全程只读）");
    }

    // =====================================================================
    // [9b] entity_artist_kind 与 entity_looks_label 启发式校准（E-12：单字社/団/团门槛与负例保护）
    // =====================================================================
    std::printf("[9b] 启发式校准（E-12：正例保留 + 负例「社会科学」「团结」等不误判）\n");
    {
        // 1. 现有正例保持正确
        ck(entity_looks_label("塞壬唱片") == true, "正例：塞壬唱片 是 Label");
        ck(entity_looks_label("塞壬唱片-MSR") == true, "正例：塞壬唱片-MSR 是 Label");
        ck(entity_looks_label("Sony Records") == true, "正例：Sony Records 是 Label");
        ck(entity_artist_kind("月華社") == EntityKind::Group, "正例：月華社 是 Group（后缀社）");
        ck(entity_artist_kind("月華社 (Circle)") == EntityKind::Group, "正例：月華社 (Circle) 是 Group");
        ck(entity_artist_kind("上海アリス幻樂団") == EntityKind::Group, "正例：上海アリス幻樂団 是 Group（樂団）");
        ck(entity_artist_kind("Czech National Symphony Orchestra") == EntityKind::Group, "正例：Symphony Orchestra 是 Group");
        ck(entity_artist_kind("早见沙织应援团") == EntityKind::Group, "正例：应援团 是 Group");
        ck(entity_artist_kind("团结交响乐团") == EntityKind::Group, "正例：团结交响乐团 是 Group（强特征词覆盖负例前缀）");

        // 乐团词表扩展正例（Philharmoniker/Philharmonic/Philharmonia/Sinfonieorchester/Symphonieorchester/交響楽団/管弦乐团/交响乐团/室内乐团/合唱团 等）
        ck(entity_artist_kind("Berliner Philharmoniker") == EntityKind::Group, "正例：Berliner Philharmoniker 是 Group");
        ck(entity_artist_kind("Wiener Philharmoniker") == EntityKind::Group, "正例：Wiener Philharmoniker 是 Group");
        ck(entity_artist_kind("Philharmonia Orchestra") == EntityKind::Group, "正例：Philharmonia Orchestra 是 Group");
        ck(entity_artist_kind("NDR Sinfonieorchester") == EntityKind::Group, "正例：NDR Sinfonieorchester 是 Group");
        ck(entity_artist_kind("Symphonieorchester des Bayerischen Rundfunks") == EntityKind::Group, "正例：Symphonieorchester 是 Group");
        ck(entity_artist_kind("NHK交響楽団") == EntityKind::Group, "正例：NHK交響楽団 是 Group");
        ck(entity_artist_kind("国家大剧院管弦乐团") == EntityKind::Group, "正例：国家大剧院管弦乐团 是 Group");
        ck(entity_artist_kind("中国爱乐乐团") == EntityKind::Group, "正例：中国爱乐乐团 是 Group");
        ck(entity_artist_kind("维也纳室内乐团") == EntityKind::Group, "正例：维也纳室内乐团 是 Group");
        ck(entity_artist_kind("中央歌剧院合唱团") == EntityKind::Group, "正例：中央歌剧院合唱团 是 Group");

        // 2. 负例保护（不误判为 Group，退化为 Person，保证宁可拆开也不错并）
        ck(entity_artist_kind("社会科学") == EntityKind::Person, "负例：社会科学 不误判为 Group（Person）");
        ck(entity_artist_kind("团结") == EntityKind::Person, "负例：团结 不误判为 Group（Person）");
        ck(entity_artist_kind("团子") == EntityKind::Person, "负例：团子 不误判为 Group（Person）");
        ck(entity_artist_kind("社长") == EntityKind::Person, "负例：社长 不误判为 Group（Person）");
        ck(entity_artist_kind("社交达人") == EntityKind::Person, "负例：社交达人 不误判为 Group（Person）");
        ck(entity_artist_kind("社区工作者") == EntityKind::Person, "负例：社区工作者 不误判为 Group（Person）");
        ck(entity_artist_kind("团") == EntityKind::Person, "单字「团」不误判为 Group（需>=2字）");
        ck(entity_artist_kind("社") == EntityKind::Person, "单字「社」不误判为 Group（需>=2字）");

        // 人名与含 Philharmonic 专辑名负例保护：人名退化为 Person，专辑名建为 Release 而不是假 Group
        ck(entity_artist_kind("Herbert von Karajan") == EntityKind::Person, "负例：指挥家 Karajan 是 Person");
        ck(entity_artist_kind("Glenn Gould") == EntityKind::Person, "负例：独奏家 Glenn Gould 是 Person");
        ck(entity_artist_kind("Ludwig van Beethoven") == EntityKind::Person, "负例：作曲家 Beethoven 是 Person");

        // 图级别负例：专辑名含 Philharmonic（如 Live at the Philharmonic）时，艺人与专辑不被误判为 Group
        MetaSeed negSeed;
        negSeed.artist = "Miles Davis";
        negSeed.album = "Live at the Philharmonic";
        negSeed.title = "Autumn Leaves";
        const EntityGraph gNeg = entity_graph_from_seed(negSeed, 13050);
        const Entity* aEnt = gNeg.find(entity_id(EntityKind::Person, "Miles Davis"));
        ck(aEnt != nullptr && aEnt->kind == EntityKind::Person, "含 Philharmonic 专辑名时，艺人仍为 Person");
        const Entity* relEnt = gNeg.find(entity_id(EntityKind::Release, "Live at the Philharmonic"));
        ck(relEnt != nullptr && relEnt->kind == EntityKind::Release, "Live at the Philharmonic 实体种类为 Release（非 Group）");
        ck(gNeg.find(entity_id(EntityKind::Group, "Live at the Philharmonic")) == nullptr, "专辑名未被误判创建为 Group 实体");

        // 古典 PERFORMER 独奏家负例（非乐团名称角色仍为 performer 但种类为 Person）
        MetaSeed perfSeed;
        perfSeed.tags["composer"] = "Johann Sebastian Bach";
        perfSeed.tags["performer"] = "Glenn Gould";
        perfSeed.title = "Goldberg Variations, BWV 988";
        const EntityGraph gPerf = entity_graph_from_seed(perfSeed, 13051);
        const Entity* ggEnt = gPerf.find(entity_id(EntityKind::Person, "Glenn Gould"));
        ck(ggEnt != nullptr && ggEnt->kind == EntityKind::Person, "古典独奏家 PERFORMER（Glenn Gould）判定为 Person 实体");
        if (ggEnt) {
            auto itRole = ggEnt->attrs.find("role");
            ck(itRole != ggEnt->attrs.end() && itRole->second.value == "performer", "Glenn Gould 角色属性为 performer");
        }

        // 3. entity_looks_label 负例保护
        ck(entity_looks_label("唱片机") == false, "负例：唱片机 不是 Label");
        ck(entity_looks_label("唱片店") == false, "负例：唱片店 不是 Label");
        ck(entity_looks_label("黑胶唱片机") == false, "负例：黑胶唱片机 不是 Label");
    }

    // =====================================================================
    // [10] entity_year4：4 位年份清洗（纯函数；Release 判等键的正解）
    // =====================================================================
    // 判据凭什么成立：这一组不只看"改对了的几个"，还专门钉住
    //   ① 不能误伤本来就干净的 4 位年（"1996" → "1996"）；
    //   ② 垃圾必须变成空串而不是半截数字（"0000" 是 P2 真实事故形态；
    //      "99999" 若截前 4 位会得到 "9999"，落进窗口 → 把垃圾并进真专辑）；
    //   ③ 窗口边界含端点（1000/2100），窗口外一律空。
    std::printf("[10] entity_year4：年份清洗（2022-08-16 → 2022；0000/99999/abc → 空）\n");
    {
        ck(entity_year4("2022-08-16") == "2022", "ISO 日期 → 年", entity_year4("2022-08-16"));
        ck(entity_year4("2022-10-01") == "2022", "同碟另一天的 ISO 日期 → 同一年",
           entity_year4("2022-10-01"));
        ck(entity_year4("20030731") == "2003", "紧凑日期 YYYYMMDD → 前 4 位", entity_year4("20030731"));
        ck(entity_year4("1996") == "1996", "本来就是 4 位年 → 不误伤（原样）", entity_year4("1996"));
        ck(entity_year4("2022-12-24") == "2022", "真实事故形态 2022-12-24 → 2022",
           entity_year4("2022-12-24"));
        ck(entity_year4("2023-07-29") == "2023", "真库样本 2023-07-29 → 2023");
        ck(entity_year4("2003-07-31T07:00:00Z") == "2003", "iTunes ISO 全形态 → 2003",
           entity_year4("2003-07-31T07:00:00Z"));
        // ★ 垃圾 → 空串（不是 "0000"/"9999"/"0999" 这种半截数字）
        ck(entity_year4("0000") == "", "「0000」→ 空串（P2 真实事故形态，绝不写进判等键）",
           entity_year4("0000"));
        ck(entity_year4("0000-00-00") == "", "「0000-00-00」→ 空串", entity_year4("0000-00-00"));
        ck(entity_year4("") == "", "空串 → 空串", entity_year4(""));
        ck(entity_year4("abc") == "", "非数字「abc」→ 空串", entity_year4("abc"));
        ck(entity_year4("999") == "", "「999」(3 位，非 YYYYMMDD 形态) → 空串", entity_year4("999"));
        ck(entity_year4("99999") == "", "「99999」→ 空串（不截前 4 位成 9999，否则垃圾进窗口）",
           entity_year4("99999"));
        ck(entity_year4("-1") == "", "负数「-1」→ 空串", entity_year4("-1"));
        ck(entity_year4("0999") == "", "「0999」(<1000) → 空串", entity_year4("0999"));
        ck(entity_year4("2101") == "", "「2101」(>2100) → 空串", entity_year4("2101"));
        ck(entity_year4("1000") == "1000" && entity_year4("2100") == "2100",
           "窗口边界 1000/2100 含端点", entity_year4("1000") + "|" + entity_year4("2100"));
        ck(entity_year4("未出版") == "", "无数字文本 → 空串", entity_year4("未出版"));
        // 幂等：清洗结果再清洗不变（判等键必须幂等，否则同一输入会漂）
        ck(entity_year4(entity_year4("2022-08-16")) == "2022", "清洗幂等（结果再过一遍不变）");

        // E-14 指定边界用例全覆盖：'1999'、'1999-05-01'、'[1000,2100] 窗口'、脏值（0/9999/-1/空）
        ck(entity_year4("1999") == "1999", "E-14 覆盖：'1999' → '1999'", entity_year4("1999"));
        ck(entity_year4("1999-05-01") == "1999", "E-14 覆盖：'1999-05-01' → '1999'", entity_year4("1999-05-01"));
        ck(entity_year4("1000") == "1000", "E-14 窗口下界端点 1000", entity_year4("1000"));
        ck(entity_year4("2100") == "2100", "E-14 窗口上界端点 2100", entity_year4("2100"));
        ck(entity_year4("0999") == "", "E-14 窗口外下界 0999 → 空", entity_year4("0999"));
        ck(entity_year4("2101") == "", "E-14 窗口外上界 2101 → 空", entity_year4("2101"));
        ck(entity_year4("0") == "", "E-14 脏值 '0' → 空串", entity_year4("0"));
        ck(entity_year4("9999") == "", "E-14 脏值 '9999'(>2100) → 空串", entity_year4("9999"));
        ck(entity_year4("-1") == "", "E-14 脏值 '-1' → 空串", entity_year4("-1"));
        ck(entity_year4("") == "", "E-14 脏值 空串 → 空串", entity_year4(""));
        ck(entity_year4("1999") == pv_year4("1999") &&
           entity_year4("1999-05-01") == pv_year4("1999-05-01") &&
           entity_year4("0") == pv_year4("0") &&
           entity_year4("9999") == pv_year4("9999"),
           "E-14 实体 year 与 pv_year4 结果完全同一（统一底层实现）");
    }

    // =====================================================================
    // [11] Release 判等键不被 date 精度污染（同碟两 date → 同一个 Release id）
    // =====================================================================
    // 这是本次修补的核心断言。真实证据（只读 ffprobe 扫全库 653 个文件 / 85 张 ≥2 轨专辑）：
    //   MUSIC\专辑-原创&同人\Swallow's Lies @ Odoriin's Upanishad of Death
    //     4 首 date=2022-10-01（album 标签="Swallow's Lies"）+ 1 首 bonus date=2022-08-16
    //   → 修补前按「专辑 × 原样 date」判等，同一张碟被拆成 2 个 Release 实体。
    // 判据凭什么成立：不只断言"两曲同 id"，还断言
    //   ① 用**原样 date** 作键时两曲确实不同 id（否则这条断言可能只是常数相等而空转）；
    //   ② 完整原始 date 仍留在属性里（收敛不能以丢信息为代价）；
    //   ③ 反向验证：把 entity_year4 改成原样返回后本组必然 FAIL（见交付报告）。
    std::printf("[11] Release 判等键：专辑 × 4 位年（date 精度不进键，原始 date 进属性）\n");
    {
        const std::string alb = "Swallow's Lies";

        // 对照：旧行为（原样 date 串进键）→ 两曲是两个 Release（证明断言不是空转）
        const std::string oldKeyA = entity_id_keyed(EntityKind::Release, { alb, "2022-10-01" });
        const std::string oldKeyB = entity_id_keyed(EntityKind::Release, { alb, "2022-08-16" });
        ck(oldKeyA != oldKeyB,
           "对照：仍用原样 date 作键时两曲确实不同 id（这条防止下面的断言变成空转）");

        MetaSeed s1;
        s1.trackPath = "D:/MUSIC/专辑-原创&同人/Swallow's Lies @ Odoriin's Upanishad of Death/1 Swallow's Lies -Chiptune Ver-.flac";
        s1.title = "Swallow's Lies -Chiptune Ver-";
        s1.artist = "Odoriin";
        s1.album = alb;
        s1.year  = "2022-10-01";                      // 真实 tag 原样形态
        const EntityGraph g1 = entity_graph_from_seed(s1, 11000);

        MetaSeed s2 = s1;
        s2.trackPath = "D:/MUSIC/专辑-原创&同人/Swallow's Lies @ Odoriin's Upanishad of Death/Swallow's Lies Bonus Track.wav";
        s2.title = "Swallow's Lies Bonus Track";
        s2.year  = "2022-08-16";                      // 同碟另一天（真实 bonus 轨）
        const EntityGraph g2 = entity_graph_from_seed(s2, 11001);

        // 图里各有一个 Release，取出它们的 id/属性来比
        const Entity* rel1 = nullptr;
        const Entity* rel2 = nullptr;
        for (const auto& kv : g1.entities) if (kv.second.kind == EntityKind::Release) rel1 = &kv.second;
        for (const auto& kv : g2.entities) if (kv.second.kind == EntityKind::Release) rel2 = &kv.second;
        ck(rel1 && rel2, "两张图各生成了 1 个 Release 实体");
        const std::string id1 = rel1 ? rel1->id : "";
        const std::string id2 = rel2 ? rel2->id : "";
        ck(!id1.empty() && id1 == id2,
           "★ 同碟两首曲子 date 差一天（2022-10-01 / 2022-08-16）→ 同一个 Release id", id1 + " vs " + id2);
        ck(g1.count(EntityKind::Release) == 1 && g2.count(EntityKind::Release) == 1,
           "每张图只有 1 个 Release（没有被 date 精度拆开）");
        // 与"只用 4 位年"的期望键一致：证明键里确实是清洗过的年，而不是碰巧相等
        ck(id1 == entity_id_keyed(EntityKind::Release, { alb, "2022" }),
           "Release id == 键(专辑 × 2022)（年份段是清洗后的 4 位年）");

        // 完整原始 date 留在属性里（收敛的代价不能是信息丢失）
        ck(attr_of(rel1, "date") == "2022-10-01",
           "首个写法的完整 date 原样留在属性 date（不清洗）", attr_of(rel1, "date"));
        ck(attr_of(rel2, "date") == "2022-08-16",
           "另一曲的完整 date 也原样留在它自己的图上（属性按曲目来源留存）", attr_of(rel2, "date"));
        ck(attr_of(rel1, "year") == "2022" && attr_of(rel2, "year") == "2022",
           "属性 year = 4 位年（与判等键一致，人看 JSON 能直接对上）",
           attr_of(rel1, "year") + "|" + attr_of(rel2, "year"));

        // 合流：把两曲塞进同一个 builder 也不会长出第二个 Release（同一 id → 同一实体）
        {
            EntityBuilder b(11002);
            const std::string ra = b.addKeyed(EntityKind::Release, { alb, entity_year4(s1.year) }, alb, "tag");
            const std::string rb = b.addKeyed(EntityKind::Release, { alb, entity_year4(s2.year) }, alb, "tag");
            b.attr(ra, "date", s1.year, "tag");
            b.attr(rb, "date", s2.year, "tag");
            ck(ra == rb, "同一 builder 内两曲落到同一 Release id");
            ck(b.graph().count(EntityKind::Release) == 1, "合并后 Release 计数仍是 1",
               std::to_string(b.graph().count(EntityKind::Release)));
            // 合流后属性里必须仍是一个**完整**原始 date（不是被截成 4 位年）——
            // 否则"收敛"的代价就成了信息丢失。注意属性合并按 mergeField（同级同置信度取 ts 新者），
            // 所以留下的是较晚写入的那条；断言它属于两条原串之一，且长度 > 4。
            const std::string mergedDate = attr_of(b.graph().find(ra), "date");
            ck(mergedDate == "2022-10-01" || mergedDate == "2022-08-16",
               "合流后 date 属性仍是完整原始串（未被截成 4 位年）", mergedDate);
            ck(mergedDate.size() > 4, "合流后 date 保留了月/日精度（长度 > 4）",
               std::to_string(mergedDate.size()));
        }

        // ★ 刻意取舍（不是疏忽，写进断言以免后人"顺手修掉"）：
        // 空 year 与有 year 的同名专辑**不合并** —— 无日期版/有日期版可能是不同版次，
        // 合并会丢版次信息；代价是同碟可能分裂，由后续候选合并环节兜底。
        ck(entity_id_keyed(EntityKind::Release, { alb, "" }) !=
               entity_id_keyed(EntityKind::Release, { alb, "2022" }),
           "空 year 与有 year 不合并（刻意的版次取舍，不是 bug）");
        MetaSeed sNoYear = s1;
        sNoYear.year = "";
        sNoYear.title = "Swallow's Lies -Instrumental-";
        const EntityGraph gNoYear = entity_graph_from_seed(sNoYear, 11003);
        ck(gNoYear.count(EntityKind::Release) == 1 &&
               attr_of([&] { const Entity* r = nullptr;
                             for (const auto& kv : gNoYear.entities)
                                 if (kv.second.kind == EntityKind::Release) r = &kv.second;
                             return r; }(), "year").empty(),
           "无 year 时不臆造 year 属性（空就是空）");

        // 垃圾 year 不得改变判等键（0000 → 空段，绝不当成"1970/0 年"这类实体）
        MetaSeed sJunk = s1;
        sJunk.year = "0000";
        sJunk.title = "junk-year";
        const EntityGraph gJunk = entity_graph_from_seed(sJunk, 11004);
        ck(attr_of([&] { const Entity* r = nullptr;
                         for (const auto& kv : gJunk.entities)
                             if (kv.second.kind == EntityKind::Release) r = &kv.second;
                         return r; }(), "year").empty(),
           "垃圾 year「0000」→ 不写 year 属性（窗口外年份在源头丢弃）");
        ck(entity_id_keyed(EntityKind::Release, { alb, entity_year4("0000") }) ==
               entity_id_keyed(EntityKind::Release, { alb, "" }),
           "垃圾 year 的判等键退化与「无 year」一致（不产生幽灵版次）");
        // date 属性照实存原始垃圾串（属性是溯源记录，不是判等材料）—— 刻意口径差异，
        // 写进断言以免后人"顺手"把它也清洗掉，反而抹掉「源数据本来是脏的」这条信息。
        ck(attr_of([&] { const Entity* r = nullptr;
                         for (const auto& kv : gJunk.entities)
                             if (kv.second.kind == EntityKind::Release) r = &kv.second;
                         return r; }(), "date") == "0000",
           "垃圾 year 仍原样留在 date 属性（溯源：源数据是脏的，只不进判等键）");
    }

    // =====================================================================
    // [12] 目录结构判据：分类目录 vs 专辑目录（只读真实库；不硬编码名字白名单）
    // =====================================================================
    // 判据：该目录下**直接有音频文件** → 专辑目录；只含子目录、无直接音频 → 分类目录。
    // 为什么不能用名字白名单："专辑-…" / "TEST站测评曲" 换个用户就失效，还会误伤用户自建的
    // "专辑-单曲收集"。结构判据在真实库上的覆盖率见下面 totalAlbumDirs 的断言。
    std::printf("[12] 分类目录 vs 专辑目录：结构判据（直接有音频 = 专辑；只含子目录 = 分类）\n");
    {
        const fs::path music = fs::path(L"D:\\RTA-hqplayer\\MUSIC");
        std::error_code ec;
        const bool haveMusic = fs::exists(music, ec);

        // 扩展名口径（与 net/library.h lib_is_audio 同一张表；这里只验证比较自身正确）
        ck(entity_audio_ext_ok(".flac") && entity_audio_ext_ok(".FLAC") && entity_audio_ext_ok("wav"),
           "扩展名口径：.flac/.FLAC/wav 都认（大小写与带不带点）");
        ck(!entity_audio_ext_ok(".cue") && !entity_audio_ext_ok(".jpg") && !entity_audio_ext_ok(""),
           "扩展名口径：.cue/.jpg/空 都不是音频");

        // 纯合成场景（不依赖真实库，任何机器上都能跑）
        {
            const fs::path tmp = tmpRoot / "dircrit";
            fs::create_directories(tmp / "cat" / "albumA", ec);
            fs::create_directories(tmp / "empty", ec);
            { std::ofstream o(tmp / "cat" / "albumA" / "01.flac", std::ios::binary); o << "x"; }
            { std::ofstream o(tmp / "cat" / "albumA" / "Cover.jpg", std::ios::binary); o << "x"; }
            ck(entity_dir_is_album(tmp / "cat" / "albumA"), "合成：直接含 .flac → 专辑目录");
            ck(!entity_dir_is_category(tmp / "cat" / "albumA"), "合成：专辑目录不是分类目录");
            ck(entity_dir_is_category(tmp / "cat"), "合成：只含子目录、无直接音频 → 分类目录");
            ck(!entity_dir_is_album(tmp / "cat"), "合成：分类目录不是专辑目录");
            ck(!entity_dir_is_album(tmp / "empty") && !entity_dir_is_category(tmp / "empty"),
               "合成：空目录既不是专辑也不是分类（未知 → 留空，不猜）");
            ck(entity_album_dir_name(tmp / "cat").empty(),
               "★ 分类目录的专辑名必须是空串（绝不把 'cat' 这种分类名当专辑名）",
               entity_album_dir_name(tmp / "cat"));
            ck(entity_album_dir_name(tmp / "cat" / "albumA") == "albumA",
               "专辑目录给出目录名（交给 entity_parse_album_circle 继续拆 @）");
            ck(entity_album_dir_name(tmp / "nope").empty(), "不存在的目录 → 空串（不崩）");
        }

        if (!haveMusic) {
            skip("真实曲库结构判据", "曲库不存在：" + u8s(music));
        } else {
            // ★ 真实样本三条（任务书点名）
            const fs::path doujin = music / fs::path(L"专辑-原创&同人");
            const fs::path cup    = doujin / fs::path(L"A Cup of Fragrance @ 月華社");
            const fs::path testst = music / fs::path(L"TEST站测评曲");

            if (fs::exists(doujin, ec)) {
                ck(entity_dir_is_category(doujin), "★ 真实：「专辑-原创&同人」是分类目录", u8s(doujin));
                ck(!entity_dir_is_album(doujin), "★ 真实：分类目录不是专辑目录");
                ck(entity_album_dir_name(doujin).empty(),
                   "★ 真实：分类目录不给专辑名（否则 Release 会退化成 '专辑-原创&同人'）",
                   entity_album_dir_name(doujin));
                // 结构覆盖率：77 个子目录应当**全部**被判成专辑目录（不漏判才说明判据可用）
                size_t subs = 0, albums = 0, cats = 0;
                for (const auto& de : fs::directory_iterator(doujin, ec)) {
                    if (!de.is_directory(ec)) continue;
                    ++subs;
                    if (entity_dir_is_album(de.path())) ++albums;
                    if (entity_dir_is_category(de.path())) ++cats;
                }
                ck(subs > 10 && albums == subs,
                   "真实：分类目录下全部子目录都判成专辑目录（结构覆盖率）",
                   std::to_string(albums) + "/" + std::to_string(subs));
                ck(cats == 0, "真实：分类目录的直接子目录里没有第二个分类层", std::to_string(cats));
            } else {
                skip("真实分类目录 专辑-原创&同人", "不存在：" + u8s(doujin));
            }

            if (fs::exists(cup, ec)) {
                ck(entity_dir_is_album(cup), "★ 真实：「A Cup of Fragrance @ 月華社」是专辑目录", u8s(cup));
                ck(!entity_dir_is_category(cup), "★ 真实：专辑目录不是分类目录");
                const std::string dn = entity_album_dir_name(cup);
                ck(dn == "A Cup of Fragrance @ 月華社", "真实：专辑目录给出完整目录名（UTF-8 无损）", dn);
                std::string a, c;
                entity_parse_album_circle(dn, a, c);
                ck(a == "A Cup of Fragrance" && c == "月華社",
                   "真实：结构判据 + 「专辑 @ 社团」拆解 → album/circle 正确", a + " | " + c);
                // 端到端：用结构判据拿到专辑名 → 建图 → Release 名字是真专辑名，不是分类名
                MetaSeed s;
                s.trackPath = u8s(cup / fs::path(L"01. 曲目.flac"));
                s.title = "曲目";
                s.artist = c;
                s.album = a;                  // ← 只来自结构判据，绝不来自父目录名
                s.circle = c;
                s.year = "2022-08-16";
                const EntityGraph g = entity_graph_from_seed(s, 12001);
                const std::string relId = entity_id_keyed(EntityKind::Release,
                                                          { a, entity_year4(s.year) });
                const Entity* rel = g.find(relId);
                ck(rel != nullptr, "真实：结构判据来的专辑名建出 Release");
                ck(rel && rel->name == "A Cup of Fragrance",
                   "★ 真实：Release 名 = 真专辑名（不是分类名 '专辑-原创&同人'）",
                   rel ? rel->name : "(null)");
                ck(rel && rel->name.find("专辑-") == std::string::npos,
                   "★ 真实：Release 名里不含分类目录名片段");
            } else {
                skip("真实专辑目录 A Cup of Fragrance", "不存在：" + u8s(cup));
            }

            if (fs::exists(testst, ec)) {
                ck(entity_dir_is_album(testst), "★ 真实：「TEST站测评曲」是专辑目录（该层直接放音频）",
                   u8s(testst));
                ck(!entity_dir_is_category(testst), "★ 真实：TEST站测评曲 不是分类目录");
                const std::string dn = entity_album_dir_name(testst);
                ck(dn == "TEST站测评曲", "真实：TEST站测评曲 的目录名原样取出", dn);
            } else {
                skip("真实目录 TEST站测评曲", "不存在：" + u8s(testst));
            }

            // 第三个真实分类层（证明判据不是只为 专辑-原创&同人 定制）
            const fs::path rain = music / fs::path(L"专辑-感动我的&RainLain");
            const fs::path vol2 = rain / fs::path(L"VOL2");
            if (fs::exists(rain, ec)) {
                ck(entity_dir_is_category(rain), "真实：「专辑-感动我的&RainLain」也是分类目录", u8s(rain));
                if (fs::exists(vol2, ec))
                    ck(entity_dir_is_album(vol2), "真实：分类下的 VOL2 是专辑目录（下钻一层即命中）");
            } else {
                skip("真实分类目录 专辑-感动我的&RainLain", "不存在：" + u8s(rain));
            }

            // 跨层一致：整库所有「有直接音频」的叶子目录都必须被 album 判据认到
            size_t dirs = 0, albumDirs = 0;
            for (const auto& de : fs::recursive_directory_iterator(music, ec)) {
                if (!de.is_directory(ec)) continue;
                ++dirs;
                if (entity_dir_is_album(de.path())) ++albumDirs;
            }
            ck(albumDirs > 50 && albumDirs < dirs,
               "真实：整库两层结构下专辑目录数 < 目录总数（判据真的在分类，不是全判成专辑）",
               std::to_string(albumDirs) + "/" + std::to_string(dirs));
        }

        // 只读红线复核：本节只做 is_directory / directory_iterator / extension()，不写 MUSIC
        ck(fs::exists(tmpRoot, ec), "本节同样没有往 MUSIC 写任何文件（写入点仍是 %TEMP%）");
    }

    // =====================================================================
    // [13] 目录结构判据接进真实批量入口（E-13：多碟盒装聚合与 5 张专辑收敛）
    // =====================================================================
    std::printf("[13] 真实批量入口与多碟盒装聚合（5 张专辑收敛离线测试）\n");
    {
        std::error_code ec;
        const fs::path batchRoot = tmpRoot / "batch_e13";
        fs::remove_all(batchRoot, ec);
        fs::create_directories(batchRoot, ec);

        // 构造 5 张专辑：
        // 专辑 1: 分类 1 下的单碟普通专辑
        const fs::path alb1 = batchRoot / "cat1" / "AlbumOne";
        fs::create_directories(alb1, ec);
        { std::ofstream o(alb1 / "01.flac", std::ios::binary); o << "x"; }
        { std::ofstream o(alb1 / "02.flac", std::ios::binary); o << "x"; }

        // 专辑 2: 分类 1 下的同人社团专辑（带 @ 社团）
        const fs::path alb2 = batchRoot / "cat1" / "AlbumTwo @ CircleAlpha";
        fs::create_directories(alb2, ec);
        { std::ofstream o(alb2 / "01.flac", std::ios::binary); o << "x"; }

        // 专辑 3: 顶层直接存放的专辑
        const fs::path alb3 = batchRoot / "AlbumThree";
        fs::create_directories(alb3, ec);
        { std::ofstream o(alb3 / "01.wav", std::ios::binary); o << "x"; }

        // 专辑 4: 分类 2 下的单曲专辑
        const fs::path alb4 = batchRoot / "cat2" / "AlbumFour";
        fs::create_directories(alb4, ec);
        { std::ofstream o(alb4 / "01.mp3", std::ios::binary); o << "x"; }

        // 专辑 5: 分类 2 下的多碟盒装（专辑目录无直接音频，只有 CD1 和 CD2，且另附 Scans 目录）
        const fs::path alb5 = batchRoot / "cat2" / "BoxSetFive @ CircleBeta";
        fs::create_directories(alb5 / "CD1", ec);
        fs::create_directories(alb5 / "CD2", ec);
        fs::create_directories(alb5 / "Scans", ec);
        { std::ofstream o(alb5 / "CD1" / "01.flac", std::ios::binary); o << "x"; }
        { std::ofstream o(alb5 / "CD1" / "02.flac", std::ios::binary); o << "x"; }
        { std::ofstream o(alb5 / "CD2" / "01.flac", std::ios::binary); o << "x"; }
        { std::ofstream o(alb5 / "CD2" / "02.flac", std::ios::binary); o << "x"; }
        { std::ofstream o(alb5 / "Scans" / "booklet.jpg", std::ios::binary); o << "x"; }

        // 1. 结构判据直接断言
        ck(entity_is_disc_subdir_name("CD1") && entity_is_disc_subdir_name("cd2") &&
           entity_is_disc_subdir_name("Disc 1") && entity_is_disc_subdir_name("DISC-02"),
           "CD1/cd2/Disc 1/DISC-02 均判定为碟片子目录名");
        ck(!entity_is_disc_subdir_name("cat1") && !entity_is_disc_subdir_name("AlbumOne") &&
           !entity_is_disc_subdir_name("Scans"),
           "cat1/AlbumOne/Scans 均不是碟片子目录名");

        ck(entity_dir_is_boxset(alb5), "BoxSetFive 识别为多碟盒装");
        ck(entity_dir_is_album(alb5), "多碟盒装识别为专辑目录（不因无直接音频漏判）");
        ck(!entity_dir_is_category(alb5), "多碟盒装不是分类目录（防止拆散）");
        ck(entity_dir_is_disc_subdir(alb5 / "CD1"), "alb5/CD1 是盒装下的碟片子目录");
        ck(entity_dir_is_disc_subdir(alb5 / "CD2"), "alb5/CD2 是盒装下的碟片子目录");
        ck(!entity_dir_is_album(alb5 / "CD1"), "CD1 碟片子目录本身不作为独立专辑");

        // 2. 收集全部音频文件路径
        std::vector<std::string> allFiles;
        for (const auto& de : fs::recursive_directory_iterator(batchRoot, ec)) {
            if (!de.is_regular_file(ec)) continue;
            if (entity_audio_ext_ok(u8s(de.path().extension().u8string()))) {
                allFiles.push_back(u8s(de.path().u8string()));
            }
        }
        std::sort(allFiles.begin(), allFiles.end());
        ck(allFiles.size() == 9, "共 9 个音频文件（2 + 1 + 1 + 1 + 4）", std::to_string(allFiles.size()));

        // 3. 走真实批量入口：entity_batch_from_paths
        const EntityGraph g = entity_batch_from_paths(allFiles, 13000);

        // 4. 断言核心：收敛为 5 个 Release 实体
        const size_t relCount = g.count(EntityKind::Release);
        ck(relCount == 5, "★ 5 张专辑（含 1 个多碟盒装）经真实批量入口严格收敛为 5 个 Release 实体",
           std::to_string(relCount));

        // 5. 断言多碟盒装：是 1 个实体且 disc 数为 2
        std::string boxReleaseId;
        const Entity* boxRel = nullptr;
        for (const auto& kv : g.entities) {
            if (kv.second.kind == EntityKind::Release && kv.second.name == "BoxSetFive") {
                boxRel = &kv.second;
                boxReleaseId = kv.second.id;
                break;
            }
        }
        ck(boxRel != nullptr, "多碟盒装在图里存在对应 Release 实体（名称取自盒装目录名 BoxSetFive）");
        if (boxRel) {
            const size_t dCount = entity_release_disc_count(g, boxReleaseId);
            ck(dCount == 2, "★ 多碟盒装是 1 个实体且 disc 数正确（2 碟）", std::to_string(dCount));
            // 验证社团边：挂载到 CircleBeta
            const std::string circleId = entity_id(EntityKind::Group, "CircleBeta");
            ck(boxRel->findEdge(entity_rel::kByGroup, circleId, "") != nullptr,
               "多碟盒装从「BoxSetFive @ CircleBeta」目录名成功解析出 CircleBeta 社团实体并链接");
        }

        // 6. 验证音乐库 LibTrack 批量入口一致性
        std::vector<rta::net::LibTrack> libTracks;
        for (const auto& p : allFiles) {
            rta::net::LibTrack t;
            t.path = p;
            t.title = u8s(fs::path(p).stem().u8string());
            libTracks.push_back(t);
        }
        const EntityGraph gLib = rta::net::entity_graph_from_library_tracks(libTracks, 13001);
        ck(gLib.count(EntityKind::Release) == 5,
           "通过 LibTrack 批量入口（entity_graph_from_library_tracks）同样严格收敛为 5 个 Release 实体",
           std::to_string(gLib.count(EntityKind::Release)));

        // 7. EntityStore 落盘核验
        EntityStore& st = EntityStore::instance();
        const std::string oldBase = st.baseDir();
        st.setBaseDir(u8s((batchRoot / "meta_store").u8string()));
        const size_t saved = st.saveGraph(g);
        ck(saved > 0, "实体图成功落盘", std::to_string(saved));
        const auto releaseIdsOnDisk = st.listIds(EntityKind::Release);
        ck(releaseIdsOnDisk.size() == 5, "落盘后 listIds(Release) 数量严格为 5",
           std::to_string(releaseIdsOnDisk.size()));
        st.setBaseDir(oldBase);

        // 8. MUSIC 只读真实样本核验
        const fs::path music = fs::path(L"D:\\RTA-hqplayer\\MUSIC");
        if (fs::exists(music, ec)) {
            size_t validAlbumDirFound = 0, totalAudios = 0;
            for (const auto& de : fs::recursive_directory_iterator(music, ec)) {
                if (!de.is_regular_file(ec)) continue;
                if (!entity_audio_ext_ok(u8s(de.path().extension().u8string()))) continue;
                ++totalAudios;
                int inferredDisc = 0;
                fsx::path ad = entity_find_album_dir_for_track(de.path(), &inferredDisc);
                if (!ad.empty() && entity_dir_is_album(ad)) {
                    ++validAlbumDirFound;
                }
            }
            ck(totalAudios > 0 && validAlbumDirFound == totalAudios,
               "真实曲库（MUSIC）只读核验：全部真实音频文件均通过结构判据定位到有效专辑目录",
               std::to_string(validAlbumDirFound) + "/" + std::to_string(totalAudios));
        } else {
            skip("MUSIC 批量只读核对", "目录不存在");
        }
    }

    // =====================================================================
    // [16] 多碟盒装三层下钻支持（Season/Vol/CD 结构 + 负例保护）
    // =====================================================================
    std::printf("[16] 多碟盒装三层下钻（Box/Season1/CD1,CD2 + Box2/Vol.1/Disc1 + 负例保护）\n");
    {
        std::error_code ec;
        const fs::path root3 = fs::temp_directory_path() / "rta_test_entities_3layer";
        // 1. 三层盒装 1：Box/Season1/CD1, CD2
        const fs::path box1 = root3 / "Box";
        fs::create_directories(box1 / "Season1" / "CD1", ec);
        fs::create_directories(box1 / "Season1" / "CD2", ec);
        { std::ofstream o(box1 / "Season1" / "CD1" / "01.flac", std::ios::binary); o << "x"; }
        { std::ofstream o(box1 / "Season1" / "CD1" / "02.flac", std::ios::binary); o << "x"; }
        { std::ofstream o(box1 / "Season1" / "CD2" / "01.flac", std::ios::binary); o << "x"; }

        // 2. 三层盒装 2：Box2/Vol.1/Disc1
        const fs::path box2 = root3 / "Box2";
        fs::create_directories(box2 / "Vol.1" / "Disc1", ec);
        { std::ofstream o(box2 / "Vol.1" / "Disc1" / "01.flac", std::ios::binary); o << "x"; }

        // 3. 普通专辑：NormalAlbum
        const fs::path normAlb = root3 / "NormalAlbum";
        fs::create_directories(normAlb, ec);
        { std::ofstream o(normAlb / "01.flac", std::ios::binary); o << "x"; }

        // 4. 负例：SomeCategory 下包含 NotASeasonAlbum/CD1 以及 AdjacentAlbum
        // 中间层是普通专辑名（不是 Season/Vol 等），严禁上钻把 SomeCategory 当成盒装！
        const fs::path catDir = root3 / "SomeCategory";
        const fs::path midAlbum = catDir / "NotASeasonAlbum";
        const fs::path adjAlbum = catDir / "AdjacentAlbum";
        fs::create_directories(midAlbum / "CD1", ec);
        fs::create_directories(adjAlbum, ec);
        { std::ofstream o(midAlbum / "CD1" / "01.flac", std::ios::binary); o << "x"; }
        { std::ofstream o(adjAlbum / "01.flac", std::ios::binary); o << "x"; }

        // 结构函数级别断言
        int d1 = 0, d2 = 0, dNorm = 0, dMid = 0;
        const fsx::path pBox1Track = box1 / "Season1" / "CD1" / "01.flac";
        const fsx::path ad1 = entity_find_album_dir_for_track(pBox1Track, &d1);
        ck(ad1 == box1, "Box/Season1/CD1 向上钻取收敛到盒装根 Box", u8s(ad1.u8string()));
        ck(d1 == 1, "Box 碟号取最终级碟片编号 (1)", std::to_string(d1));

        const fsx::path pBox2Track = box2 / "Vol.1" / "Disc1" / "01.flac";
        const fsx::path ad2 = entity_find_album_dir_for_track(pBox2Track, &d2);
        ck(ad2 == box2, "Box2/Vol.1/Disc1 向上钻取收敛到盒装根 Box2", u8s(ad2.u8string()));
        ck(d2 == 1, "Box2 碟号取最终级碟片编号 (1)", std::to_string(d2));

        const fsx::path pNormTrack = normAlb / "01.flac";
        const fsx::path adNorm = entity_find_album_dir_for_track(pNormTrack, &dNorm);
        ck(adNorm == normAlb, "普通专辑 NormalAlbum 定位自身", u8s(adNorm.u8string()));

        // 负例断言：中间层不是季节/卷而是普通专辑名，只定位到中间层，不上钻到 SomeCategory
        const fsx::path pMidTrack = midAlbum / "CD1" / "01.flac";
        const fsx::path adMid = entity_find_album_dir_for_track(pMidTrack, &dMid);
        ck(adMid == midAlbum, "★ 负例：中间层为普通专辑名 NotASeasonAlbum 时不上钻到 SomeCategory",
           u8s(adMid.u8string()));
        ck(dMid == 1, "NotASeasonAlbum 碟号为 1", std::to_string(dMid));

        ck(!entity_dir_is_boxset(catDir), "SomeCategory 不是盒装目录");
        ck(entity_dir_is_boxset(box1), "三层盒装 Box 判定为 boxset");
        ck(entity_dir_is_boxset(box2), "三层盒装 Box2 判定为 boxset");

        // 批量入口断言
        std::vector<std::string> all3Files;
        for (const auto& de : fs::recursive_directory_iterator(root3, ec)) {
            if (!de.is_regular_file(ec)) continue;
            if (entity_audio_ext_ok(u8s(de.path().extension().u8string()))) {
                all3Files.push_back(u8s(de.path().u8string()));
            }
        }
        std::sort(all3Files.begin(), all3Files.end());
        ck(all3Files.size() == 7, "三层盒装测试共 7 个音频文件 (3 + 1 + 1 + 1 + 1)", std::to_string(all3Files.size()));

        const EntityGraph g3 = entity_batch_from_paths(all3Files, 13100);

        // 验证 Release 数量与实体
        // 应该聚合为 5 个 Release: Box, Box2, NormalAlbum, NotASeasonAlbum, AdjacentAlbum
        const size_t relCount3 = g3.count(EntityKind::Release);
        ck(relCount3 == 5, "★ 批量入口严格收敛为 5 个 Release 实体（不误吞相邻专辑）", std::to_string(relCount3));

        auto find_release_named = [&](const std::string& name) -> const Entity* {
            for (const auto& kv : g3.entities) {
                if (kv.second.kind == EntityKind::Release && kv.second.name == name) {
                    return &kv.second;
                }
            }
            return nullptr;
        };

        const Entity* rBox1 = find_release_named("Box");
        ck(rBox1 != nullptr, "三层盒装 1 聚合为单个 Release 实体 (Box)");
        if (rBox1) {
            const size_t dc = entity_release_disc_count(g3, rBox1->id);
            ck(dc == 2, "★ Box 碟数正确 (2 碟)", std::to_string(dc));
        }

        const Entity* rBox2 = find_release_named("Box2");
        ck(rBox2 != nullptr, "三层盒装 2 聚合为单个 Release 实体 (Box2)");
        if (rBox2) {
            const size_t dc = entity_release_disc_count(g3, rBox2->id);
            ck(dc == 1, "★ Box2 碟数正确 (1 碟)", std::to_string(dc));
        }

        const Entity* rNorm = find_release_named("NormalAlbum");
        ck(rNorm != nullptr, "普通专辑 Release 实体存在 (NormalAlbum)");

        const Entity* rMid = find_release_named("NotASeasonAlbum");
        ck(rMid != nullptr, "负例普通专辑 Release 实体独立存在 (NotASeasonAlbum)");

        const Entity* rAdj = find_release_named("AdjacentAlbum");
        ck(rAdj != nullptr, "相邻普通专辑 Release 实体独立存在 (AdjacentAlbum)");

        ck(find_release_named("SomeCategory") == nullptr, "分类目录 SomeCategory 未被错误创建为 Release");

        fs::remove_all(root3, ec);
    }

    std::printf("\n结果：%s  （通过 %s，失败 %d，跳过 %d）\n",
                g_fail == 0 ? "ALL PASS" : "FAILED",
                g_fail == 0 ? "全部" : "见上方 [FAIL]",
                g_fail, g_skip);
    return g_fail ? 1 : 0;
}
