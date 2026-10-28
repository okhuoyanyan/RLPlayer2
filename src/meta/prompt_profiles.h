// prompt_profiles.h — P1「两步走」提示词策略 + P2 不过滤解析 + P3 名词超链接
//
// 用户 261027-06 原话：
//   「你为不同种类的音乐，制定不同的提示词策略，然后不用过滤，直接把内容展示出来就行。
//     如果可行，你还可以超链接匹配名词（人名、音乐厅名字等，用权威链接：维基百科/百度百科/萌娘百科）。」
//   「你过滤掉的信息，比展现的信息还多，那这个 AI 搜索就没有意义。」
//   「这才是我想要的结果，歌曲赏析、背景解读，这才是我想要的东西！」
//
// 本文件只做**纯函数**（无网络、无文件、无线程），因此可以在离线单测里逐条断言；
// 依赖只有 net/json.h（纯头文件、纯 std）。真正的 LLM 调用与材料拼装在 retrieval.h。
//
// 四层职责：
//   1) classify_profile()         —— 先把专辑/单曲分到 14 个 profile 之一（古典/同人/流行/摇滚/金属/
//                                   爵士/民乐/电子/原声/虚拟歌手/嘻哈/氛围/世界/兜底）
//   2) build_profile_prompt()     —— 再按 profile 给一套专用提示词，要求「能确定的全部输出、不确定的不写」
//   3) parse_profile_reply()      —— 宽松解析：标签自由、段落自由、引用可选；JSON 坏了就按行兜底，
//                                   绝不因为「格式不合规」把模型已经写出来的内容丢掉
//   4) LinkIndex / linkify_text() —— 名词 → 权威链接的确定性匹配（核对过的种子表 + 材料 URL + 页面标题）
#pragma once

#include "net/json.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rta::meta {

// ===========================================================================
// 0. 小工具
// ===========================================================================

inline std::string pp_trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        const unsigned char c = (unsigned char)s[b];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++b;
        else break;
    }
    while (e > b) {
        const unsigned char c = (unsigned char)s[e - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') --e;
        else break;
    }
    return s.substr(b, e - b);
}

inline std::string pp_lower_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

inline bool pp_is_word_char(char c) {
    const unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9');
}

inline bool pp_is_ascii_only(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return false;
    return true;
}

inline std::string pp_join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

// ASCII 关键词按「词边界」匹配（避免 rock 命中 rocket、op. 命中 options）；
// 含非 ASCII 的关键词直接子串匹配（中文/日文没有空格分词）。
inline bool pp_token_in(const std::string& hay_lower, const std::string& needle_lower) {
    if (needle_lower.empty()) return false;
    size_t pos = 0;
    while ((pos = hay_lower.find(needle_lower, pos)) != std::string::npos) {
        const bool left_ok = (pos == 0) || !pp_is_word_char(hay_lower[pos - 1]);
        const size_t end = pos + needle_lower.size();
        const bool right_ok = (end >= hay_lower.size()) || !pp_is_word_char(hay_lower[end]);
        if (left_ok && right_ok) return true;
        pos = (end > pos) ? end : (pos + 1);
    }
    return false;
}

inline bool pp_kw_in(const std::string& hay, const std::string& kw) {
    if (hay.empty() || kw.empty()) return false;
    if (pp_is_ascii_only(kw)) return pp_token_in(pp_lower_ascii(hay), pp_lower_ascii(kw));
    return hay.find(kw) != std::string::npos;
}

// ===========================================================================
// 1. 分类：先把专辑分到 profile（用户要求「不要只做 3 种」）
// ===========================================================================

enum class MusicProfile : int {
    Classical = 0,
    Doujin,
    Pop,
    Rock,
    Metal,
    Jazz,
    Folk,
    Electronic,
    Soundtrack,
    Vocal,
    HipHop,
    Ambient,
    World,
    Generic
};

inline const char* music_profile_id(MusicProfile p) {
    switch (p) {
        case MusicProfile::Classical:  return "classical";
        case MusicProfile::Doujin:     return "doujin";
        case MusicProfile::Pop:        return "pop";
        case MusicProfile::Rock:       return "rock";
        case MusicProfile::Metal:      return "metal";
        case MusicProfile::Jazz:       return "jazz";
        case MusicProfile::Folk:       return "folk";
        case MusicProfile::Electronic: return "electronic";
        case MusicProfile::Soundtrack: return "soundtrack";
        case MusicProfile::Vocal:      return "vocal";
        case MusicProfile::HipHop:     return "hiphop";
        case MusicProfile::Ambient:    return "ambient";
        case MusicProfile::World:      return "world";
        case MusicProfile::Generic:    return "generic";
    }
    return "generic";
}

inline const char* music_profile_label(MusicProfile p) {
    switch (p) {
        case MusicProfile::Classical:  return "古典";
        case MusicProfile::Doujin:     return "同人";
        case MusicProfile::Pop:        return "流行";
        case MusicProfile::Rock:       return "摇滚";
        case MusicProfile::Metal:      return "金属";
        case MusicProfile::Jazz:       return "爵士";
        case MusicProfile::Folk:       return "民乐/民族";
        case MusicProfile::Electronic: return "电子";
        case MusicProfile::Soundtrack: return "原声";
        case MusicProfile::Vocal:      return "虚拟歌手/唱见";
        case MusicProfile::HipHop:     return "嘻哈";
        case MusicProfile::Ambient:    return "氛围/新世纪";
        case MusicProfile::World:      return "世界音乐";
        case MusicProfile::Generic:    return "通用";
    }
    return "通用";
}

inline MusicProfile music_profile_from_id(const std::string& id) {
    const std::string s = pp_lower_ascii(pp_trim(id));
    for (int i = 0; i <= (int)MusicProfile::Generic; ++i) {
        const MusicProfile p = (MusicProfile)i;
        if (s == music_profile_id(p)) return p;
    }
    return MusicProfile::Generic;
}

inline std::vector<MusicProfile> all_music_profiles() {
    std::vector<MusicProfile> v;
    for (int i = 0; i <= (int)MusicProfile::Generic; ++i) v.push_back((MusicProfile)i);
    return v;
}

// 分类输入：调用方能拿到的所有线索。全部可选（空串/空数组都会被忽略）。
struct ProfileSignal {
    std::string title;          // 专辑名
    std::string artist;         // 艺术家 / 社团
    std::string album_artist;
    std::string label;          // 厂牌
    std::string catalog;        // 目录号
    std::string hint;           // 调用方直接给的类型提示（如 '古典' / 'classical'）
    std::vector<std::string> genres;    // 文件标签里的流派
    std::vector<std::string> tracks;    // 曲目名（同人：原曲名会出现在这里）
    std::string material_text;          // L2 材料 + L3 检索正文（Discogs 的『流派与风格』等）
    int track_count = 0;
    double total_sec = 0.0;
};

struct ProfileScore {
    MusicProfile profile = MusicProfile::Generic;
    int score = 0;
    std::string hits;   // 命中的关键词，用于日志解释「为什么分到这一类」
};

struct ProfileDecision {
    MusicProfile profile = MusicProfile::Generic;
    int score = 0;
    std::string reason;                 // 一行人类可读的理由
    std::vector<ProfileScore> ranked;   // 按分数降序（前 3 个）
    std::string hint_source;            // "hint" / "keywords" / "fallback"
};

namespace pp_detail {

struct Kw { const char* kw; int w; };

// 关键词表：w 越大越"决定性"。CJK 用子串，ASCII 用词边界。
inline std::vector<Kw> keywords_for(MusicProfile p) {
    switch (p) {
        case MusicProfile::Classical:
            return {
                {"古典", 4}, {"classical", 4}, {"交响", 3}, {"symphony", 3}, {"symphonic", 2},
                {"奏鸣曲", 3}, {"sonata", 3}, {"协奏曲", 3}, {"concerto", 3}, {"四重奏", 3},
                {"quartet", 3}, {"管弦", 3}, {"orchestra", 3}, {"philharmonic", 3},
                {"指挥", 2}, {"conductor", 2}, {"乐团", 2}, {"ensemble", 2},
                {"bwv", 3}, {"op.", 2}, {"变奏曲", 3}, {"variations", 2}, {"安魂曲", 3},
                {"requiem", 3}, {"歌剧", 3}, {"opera", 2}, {"咏叹调", 3}, {"aria", 1},
                {"巴洛克", 3}, {"baroque", 3}, {"浪漫派", 3}, {"romantic", 1},
                {"室内乐", 3}, {"chamber", 2}, {"赋格", 3}, {"fugue", 3}, {"弥撒", 3},
                {"古乐", 3}, {"period instrument", 3},
                {"deutsche grammophon", 4}, {"decca", 3}, {"chandos", 3}, {"hyperion", 3},
                {"harmonia mundi", 3}, {"rca red seal", 3}, {"bmg classics", 2},
                {"gramophone", 2}, {"留声机", 3}, {"企鹅唱片", 3}, {"classics today", 3},
                {"录音师", 2}, {"remaster", 1}, {"sacd", 1}, {"d大调", 2}, {"c小调", 2},
                {"弦乐", 2}, {"stabat mater", 3}, {"cantata", 3}, {"康塔塔", 3}
            };
        case MusicProfile::Doujin:
            return {
                {"同人", 4}, {"doujin", 4}, {"東方", 4}, {"东方", 4}, {"touhou", 4},
                {"例大祭", 4}, {"紅楼夢", 4}, {"红楼", 3}, {"コミケ", 4}, {"comiket", 4},
                {"m3", 2}, {"サークル", 3}, {"社团", 3}, {"circle", 1},
                {"arrange", 2}, {"アレンジ", 2}, {"原曲", 3}, {"幻想郷", 4}, {"幻想乡", 4},
                {"秘封", 4}, {"上海アリス幻樂団", 4}, {"幽閉サテライト", 4}, {"豚乙女", 4},
                {"iosys", 4}, {"alstroemeria records", 4}, {"sound holic", 4},
                {"東京アクティブneets", 4}, {"dizzylab", 4}, {"booth", 2},
                {"c89", 3}, {"c90", 3}, {"c91", 3}, {"c92", 3}, {"c93", 3}, {"c94", 3},
                {"c95", 3}, {"c96", 3}, {"c97", 3}, {"c98", 3}, {"c99", 3}, {"c100", 3},
                {"特典", 2}, {"通贩", 2}, {"通販", 2},
                // 2026-10-27（真实调用反馈，Static World 会场限定CD 落到 generic）：
                // 现场颁布/即卖会语境是同人的强信号，但材料里常常**没有**「例大祭」「东方」字样
                //（只有 VGMdb 组织页），所以这些词必须单独进表，否则一整类同人盘分不出来。
                {"会场限定", 4}, {"会場限定", 4}, {"限定cd", 4},
                {"即卖会", 4}, {"即売会", 4}, {"即売", 3},
                {"頒布", 3}, {"颁布", 3}, {"委託", 2},
                {"新谱", 2}, {"新譜", 2}, {"会場", 1}
            };
        case MusicProfile::Pop:
            return {
                {"流行", 4}, {"pop", 3}, {"j-pop", 3}, {"k-pop", 3}, {"华语", 3}, {"粤语", 3},
                {"国语", 2}, {"偶像", 3}, {"idol", 2}, {"ballad", 2}, {"抒情", 2},
                {"city pop", 4}, {"singer-songwriter", 2}, {"主打歌", 3}, {"榜单", 2},
                {"billboard", 2}, {"oricon", 3}, {"专辑主打", 3}, {"情歌", 2}, {"翻唱", 1}
            };
        case MusicProfile::Rock:
            return {
                {"摇滚", 4}, {"rock", 3}, {"punk", 3}, {"朋克", 3}, {"grunge", 3},
                {"indie", 2}, {"另类", 2}, {"alternative", 2}, {"hard rock", 4},
                {"classic rock", 4}, {"后摇", 4}, {"post-rock", 4}, {"shoegaze", 3},
                {"britpop", 3}, {"garage rock", 3}, {"rock band", 2},
                // 2026-10-27：乐队名单独给权重 —— 现实里材料常常只有「乐队 + 厂牌」而没有 genre 标签
                //（rtaweb 目前确实不传流派），只靠 "rock" 这个词会漏判。
                {"radiohead", 3}, {"nirvana", 3}, {"pink floyd", 3}, {"led zeppelin", 3},
                {"oasis", 3}, {"green day", 3}, {"coldplay", 2}
            };
        case MusicProfile::Metal:
            return {
                {"金属", 4}, {"metal", 3}, {"heavy metal", 4}, {"death metal", 4},
                {"black metal", 4}, {"thrash", 4}, {"doom", 3}, {"metalcore", 4},
                {"symphonic metal", 4}, {"power metal", 4}, {"djent", 4},
                {"metallica", 3}, {"megadeth", 3}, {"slayer", 3}, {"opeth", 3},
                {"双踩", 3}, {"blast beat", 3}
            };
        case MusicProfile::Jazz:
            return {
                {"爵士", 4}, {"jazz", 4}, {"blues", 3}, {"蓝调", 3}, {"swing", 2},
                {"bebop", 4}, {"bop", 2}, {"cool jazz", 4}, {"fusion", 2}, {"funk", 2},
                {"即兴", 3}, {"improv", 2}, {"blue note", 4}, {"ecm", 3}, {"verve", 2},
                {"impulse!", 3}, {"piano trio", 4}, {"big band", 4}, {"萨克斯", 2},
                {"saxophone", 2}, {"standards", 3}
            };
        case MusicProfile::Folk:
            return {
                {"民乐", 4}, {"民族", 2}, {"folk", 3}, {"民谣", 3}, {"古筝", 4}, {"二胡", 4},
                {"琵琶", 4}, {"笛子", 4}, {"箫", 3}, {"扬琴", 4}, {"中阮", 4}, {"唢呐", 4},
                {"马头琴", 4}, {"erhu", 4}, {"guzheng", 4}, {"pipa", 4}, {"dizi", 4},
                {"世界音乐", 3}, {"world music", 3}, {"celtic", 3}, {"凯尔特", 3},
                {"flamenco", 3}, {"tango", 2}, {"gamelan", 4}, {"山歌", 3}, {"原生态", 3},
                {"手风琴", 2}, {"accordion", 2}, {"管乐", 2}, {"吹打", 3}
            };
        case MusicProfile::Electronic:
            return {
                {"电子", 4}, {"electronic", 3}, {"electronica", 3}, {"techno", 4},
                {"house", 2}, {"trance", 4}, {"dubstep", 4}, {"drum and bass", 4},
                {"dnb", 3}, {"edm", 4}, {"synthwave", 4}, {"idm", 4}, {"breakbeat", 4},
                {"big beat", 4}, {"合成器", 3}, {"synthesizer", 2}, {"chiptune", 3},
                {"8bit", 2}, {"downtempo", 3}, {"trip hop", 4}, {"daft punk", 3},
                {"aphex twin", 3}, {"bpm", 1}
            };
        case MusicProfile::Soundtrack:
            return {
                {"原声", 4}, {"ost", 4}, {"original soundtrack", 4}, {"soundtrack", 4},
                {"サウンドトラック", 4}, {"剧伴", 4}, {"配乐", 4}, {"游戏音乐", 4},
                {"影视原声", 4}, {"动画原声", 4}, {"score", 2}, {"theme song", 3},
                {"主题曲", 2}, {"塞壬唱片", 4}, {"monster siren", 4}, {"arknights", 4},
                {"明日方舟", 4}, {"原声带", 4}, {"bgm", 2}, {"影视", 2}, {"剧集", 2}
            };
        case MusicProfile::Vocal:
            return {
                {"vocaloid", 4}, {"ボーカロイド", 4}, {"ボカロ", 4}, {"初音ミク", 4},
                {"初音未来", 4}, {"鏡音", 4}, {"巡音", 4}, {"gumi", 3},
                {"洛天依", 4}, {"utau", 4}, {"歌ってみた", 4}, {"唱见", 4}, {"唱見", 4},
                {"niconico", 3}, {"ニコニコ", 3}, {"虚拟歌手", 4}, {"音源", 2},
                {"cevio", 4}, {"synthesizer v", 4}
            };
        case MusicProfile::HipHop:
            return {
                {"嘻哈", 4}, {"说唱", 4}, {"hip-hop", 4}, {"hip hop", 4}, {"rap", 3},
                {"trap", 4}, {"boom bap", 4}, {"drill", 4}, {"mixtape", 4}, {"beatmaker", 4},
                {"采样", 2}, {"nujabes", 3}, {"押韵", 3}
            };
        case MusicProfile::Ambient:
            return {
                {"氛围", 4}, {"ambient", 4}, {"new age", 4}, {"新世纪", 4}, {"冥想", 4},
                {"drone", 4}, {"环境音乐", 4}, {"soundscape", 4}, {"field recording", 4},
                {"疗愈", 3}, {"binaural", 3}, {"brian eno", 3}, {"nils frahm", 3},
                {"钢琴独奏", 3}, {"solo piano", 3}
            };
        case MusicProfile::World:
            return {
                {"世界音乐", 4}, {"afrobeat", 4}, {"reggae", 4}, {"拉丁", 4},
                {"latin", 3}, {"bossa nova", 4}, {"samba", 3}, {"fado", 4}, {"ragas", 4},
                {"印度", 2}, {"中东", 2}, {"凯尔特", 3}, {"celtic", 3}, {"民族音乐", 3}
            };
        case MusicProfile::Generic:
            break;
    }
    return {};
}

} // namespace pp_detail

// 分类主函数：先在 hint 里找直接答案，再按关键词加权打分。
inline ProfileDecision classify_profile(const ProfileSignal& sig) {
    ProfileDecision d;
    d.hint_source = "fallback";

    // ① 调用方直接给的提示优先（文件标签里 genre 写死的情况最常见）
    const std::string hint = pp_trim(sig.hint);
    if (!hint.empty()) {
        const std::string hl = pp_lower_ascii(hint);
        for (int i = 0; i <= (int)MusicProfile::Generic; ++i) {
            const MusicProfile p = (MusicProfile)i;
            if (hl == music_profile_id(p) || hint == music_profile_label(p)) {
                d.profile = p;
                d.score = 100;
                d.hint_source = "hint";
                d.reason = std::string("hint=") + hint + " -> " + music_profile_id(p);
                d.ranked.push_back(ProfileScore{p, 100, "hint"});
                return d;
            }
        }
    }

    // ② 关键词打分（每个关键词只在权重最高的字段计一次，避免同一词在正文里刷分）
    const std::string title = sig.title;
    const std::string artist = !sig.album_artist.empty() ? (sig.album_artist + " " + sig.artist) : sig.artist;
    const std::string label = sig.label + " " + sig.catalog;
    const std::string genres = pp_join(sig.genres, " ");
    const std::string tracks = pp_join(sig.tracks, " ");
    const std::string material = sig.material_text;

    // 字段权重：流派标签 > 标题 > 曲目 > 艺术家 > 厂牌 > 正文
    struct FieldRef { const std::string* text; int weight; const char* name; };
    const FieldRef fields[] = {
        {&genres,   5, "genre"},
        {&title,    4, "title"},
        {&tracks,   3, "tracks"},
        {&artist,   3, "artist"},
        {&label,    2, "label"},
        {&material, 1, "material"}
    };

    std::vector<ProfileScore> all;
    for (int i = 0; i <= (int)MusicProfile::Generic; ++i) {
        const MusicProfile p = (MusicProfile)i;
        if (p == MusicProfile::Generic) continue;
        ProfileScore ps;
        ps.profile = p;
        std::vector<std::string> hit_words;
        for (const auto& kw : pp_detail::keywords_for(p)) {
            for (const auto& f : fields) {
                if (f.text->empty()) continue;
                if (!pp_kw_in(*f.text, kw.kw)) continue;
                ps.score += kw.w * f.weight;
                if (hit_words.size() < 5) hit_words.push_back(std::string(kw.kw) + "@" + f.name);
                break;   // 该关键词只在权重最高的字段计一次
            }
        }
        if (!hit_words.empty()) ps.hits = pp_join(hit_words, ",");
        all.push_back(std::move(ps));
    }

    std::stable_sort(all.begin(), all.end(), [](const ProfileScore& a, const ProfileScore& b) {
        if (a.score != b.score) return a.score > b.score;
        return (int)a.profile < (int)b.profile;
    });

    for (size_t i = 0; i < all.size() && i < 3; ++i) d.ranked.push_back(all[i]);
    if (!all.empty() && all[0].score >= 3) {
        d.profile = all[0].profile;
        d.score = all[0].score;
        d.hint_source = "keywords";
        d.reason = std::string("score=") + std::to_string(all[0].score) + " hits=" + all[0].hits;
    } else {
        d.profile = MusicProfile::Generic;
        d.score = all.empty() ? 0 : all[0].score;
        d.reason = "no decisive keyword -> generic";
        if (!all.empty() && !all[0].hits.empty()) {
            d.reason += std::string(" (best=") + music_profile_id(all[0].profile) + ":" + all[0].hits + ")";
        }
    }
    return d;
}

// 把信号里"已知的专辑标签"整理成 k/v，交给提示词（用户要求「把所有专辑标签都交给模型」）
inline std::vector<std::pair<std::string, std::string>> pp_signal_tags(const ProfileSignal& sig) {
    std::vector<std::pair<std::string, std::string>> out;
    auto put = [&out](const std::string& k, const std::string& v) {
        const std::string t = pp_trim(v);
        if (!t.empty()) out.emplace_back(k, t);
    };
    put("专辑名称", sig.title);
    put("艺术家/社团", sig.album_artist.empty() ? sig.artist : sig.album_artist);
    put("厂牌", sig.label);
    put("目录号", sig.catalog);
    put("流派标签", pp_join(sig.genres, " / "));
    if (sig.track_count > 0) put("曲目数", std::to_string(sig.track_count));
    if (sig.total_sec > 0.5) {
        const int sec = (int)(sig.total_sec + 0.5);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%02d", sec / 60, sec % 60);
        put("总时长", buf);
    }
    return out;
}

// ===========================================================================
// 2. 每个 profile 的专用提示词素材（用户要求的「第二步：按类定制」）
// ===========================================================================

struct ProfileSpec {
    MusicProfile profile = MusicProfile::Generic;
    const char* id = "generic";
    const char* label = "通用";
    const char* fields = "";        // 本类必查信息（推荐标签名，可自由扩展）
    const char* angles = "";        // 本类解读角度
    const char* rules = "";         // 本类特有规矩
    const char* track_fields = "";  // 单曲页核心档案
};

inline const std::vector<ProfileSpec>& all_profile_specs() {
    static const std::vector<ProfileSpec> k = {
        {
            MusicProfile::Classical, "classical", "古典",
            "专辑名称、译名、作曲家、作品编号（Op./BWV/K./Hob.）、演奏者、独奏家、乐团、指挥、"
            "声部首席（首席小提琴/大提琴/长笛等）、合唱团、录音师、制作人、录音地点、录音日期、"
            "录音技术（ADD/DDD、母带、采样率）、录音水平与音质评价、发行时间、厂牌、目录号、"
            "介质（CD/SACD/LP/高解析）、碟数、音轨数、总时长、版本对比（同时期或同曲目的其他名盘）、"
            "媒体评价（留声机/企鹅/ClassicsToday/AllMusic）、曲目列表（乐章级：音轨/乐章名/时长/速度标记）",
            "演绎诠释（速度、分句、声部平衡、厅堂感）、与作曲家原谱的关系（原版/校订版/删改）、"
            "历史录音价值、同曲目名盘对比（如卡拉扬 vs 伯恩斯坦）、录音与制作水准",
            "作曲家与演奏者必须分开写，绝不能把作曲家当艺术家；作品编号能确定就必须写；"
            "乐章要逐条列出并尽量带时长；『古典』不要写成『流行』。",
            "曲名（乐章名）、调性与作品编号、时长、作曲家、演奏/指挥/乐团、录音年份、录音地点"
        },
        {
            MusicProfile::Doujin, "doujin", "同人",
            "专辑名称、译名、社团（サークル）、主催/作者、原曲出处（东方 Project 具体曲名 + 出自哪一作，"
            "如『U.N.オーエンは彼女なのか？』出自东方红魔乡；或其他原作）、编曲者、作词、演唱、演奏、"
            "会场（例大祭/Comiket/M3/紅楼夢）与摊位号、首发日期、通贩渠道（dizzylab/Booth/とらのあな/"
            "メロンブックス）与上架时间、售价（会场价 vs 通贩价、含税与否）、特典（特典CD/挂轴/谱面/明信片）、"
            "系列归属（第几作/系列名）、碟数、音轨数、总时长、介质、"
            "曲目列表（音轨/曲名/时长/编曲/原曲出处）",
            "原曲的重构方式（保旋律/改调/换拍/器乐化）、社团一贯风格（交响/摇滚/电子/爵士改编）、"
            "该曲在东方同人谱系里的位置（哪些社团也改编过）、编曲手法与音色取向",
            "原曲必须写具体曲名与出处作品，不确定就不写，绝不写『东方某曲』；社团名用官方写法"
            "（如『上海アリス幻樂団』『幽閉サテライト』）；售价与会场信息只有确定才写。",
            "曲名、时长、编曲、原曲、原曲出处（作品名）、演唱、所属专辑"
        },
        {
            MusicProfile::Pop, "pop", "流行",
            "专辑名称、译名、艺术家、发行时间、厂牌、制作人、专辑类型（录音室/EP/单曲/精选/现场）、"
            "语种、曲目列表（音轨/曲名/时长/作词/作曲/编曲）、碟数、音轨数、总时长、榜单成绩、销量、"
            "获奖、关联单曲与巡演、版本（初回/通常/限定）",
            "词曲主题与时代语境、制作取向与编曲、在歌手生涯中的位置、与同时代作品的比较",
            "区分专辑与单曲/EP；榜单与销量只有确定才写。",
            "曲名、时长、作词、作曲、编曲、所属专辑"
        },
        {
            MusicProfile::Rock, "rock", "摇滚",
            "专辑名称、艺术家/乐队、成员编制（主唱/吉他/贝斯/鼓/键盘）、发行时间、厂牌、制作人、"
            "录音室、混音、母带、专辑类型、曲目列表（音轨/曲名/时长/作曲）、碟数、音轨数、总时长、"
            "风格标签、乐评、巡演、版本",
            "音响取向与制作（干/湿、失真、动态）、作曲结构与 riff 组织、乐队阵容变化、在流派中的位置",
            "成员写成『人名（乐器）』；不要把制作人写成乐队成员。",
            "曲名、时长、作曲、编曲、演唱"
        },
        {
            MusicProfile::Metal, "metal", "金属",
            "专辑名称、乐队、成员编制（主唱/吉他/贝斯/鼓/键盘）、发行时间、厂牌、制作人、录音室、"
            "混音、母带、子流派（death/black/thrash/doom/symphonic…）、曲目列表（含时长）、碟数、"
            "音轨数、总时长、乐评、巡演、版本",
            "失真与音色设计、riff 与节奏结构、极端唱腔与编曲密度、乐队阵容与录音条件",
            "子流派要写准；成员写成『人名（乐器）』。",
            "曲名、时长、作曲、编曲、演唱/主唱"
        },
        {
            MusicProfile::Jazz, "jazz", "爵士",
            "专辑名称、领队与乐手（人名 + 乐器）、发行时间、厂牌、目录号、录音日期、录音室、制作人、"
            "曲目列表（含作曲者与时长）、碟数、音轨数、总时长、版本（首版/复刻/重发）、乐评",
            "即兴结构与独奏顺序、编制与互动、录音年代与厂牌审美、同曲目其他版本的比较",
            "标准曲要写作曲者（如『Autumn Leaves (Kosma)』）；乐手一律『人名（乐器）』。",
            "曲名、时长、作曲者、编曲、演奏编制"
        },
        {
            MusicProfile::Folk, "folk", "民乐/民族",
            "专辑名称、演奏者/乐团、乐器编制、地域与流派、发行时间、厂牌、录音方式与地点、"
            "曲目列表（含原曲/民歌出处）、碟数、音轨数、总时长、出版信息",
            "采风来源与改编程度、乐器与音色、传统与现代的取舍",
            "民歌/古曲要写出处（如『《梅花三弄》古曲』）。",
            "曲名、时长、乐器、编曲、原曲出处"
        },
        {
            MusicProfile::Electronic, "electronic", "电子",
            "专辑名称、制作人/DJ、发行时间、厂牌、目录号、风格（techno/house/ambient/idm…）、"
            "BPM 与调性（若确定）、制作设备与软件、曲目列表（含时长与混音版本）、碟数、音轨数、"
            "总时长、介质与版本",
            "声音设计与织体、节奏与结构、与舞池或聆听场景的关系",
            "BPM/调性只有确定才写；不要把 remix 当原曲。",
            "曲名、时长、制作人、混音版本"
        },
        {
            MusicProfile::Soundtrack, "soundtrack", "原声",
            "专辑名称、作品归属（游戏/动画/影视 + 第几季第几部）、作曲、编曲、演奏乐团、"
            "主题曲（曲名 + 演唱）、发行商/厂牌、发行时间、专辑类型（OST/arrange/演唱会/角色歌）、"
            "曲目列表（含对应场景或剧集，若确定）、碟数、音轨数、总时长、版本",
            "配乐与画面的对应、主题动机的使用、与同系列其他原声的关系",
            "必须区分 OST / arrange / 演唱会专辑；不要把同人改编当官方原声。",
            "曲名、时长、作曲、编曲、对应场景/剧集、演唱"
        },
        {
            MusicProfile::Vocal, "vocal", "虚拟歌手/唱见",
            "专辑名称、歌手/角色、音源（Vocaloid 型号、CeVIO、UTAU、Synthesizer V）、P主/制作人、"
            "作词、作曲、编曲、插画、动画（PV）、投稿日、播放量（若确定）、所属系列/专辑、"
            "曲目列表、总时长",
            "歌词叙事与世界观、调声与音源特性、P主风格、与同系列曲目的关联",
            "『音源』与『歌手』要分开写；播放量只有确定才写。",
            "曲名、时长、P主、音源、作词/作曲/编曲、投稿日"
        },
        {
            MusicProfile::HipHop, "hiphop", "嘻哈",
            "专辑名称、艺人、制作人/Beatmaker、客串、发行时间、厂牌、采样来源、"
            "曲目列表（含时长）、碟数、音轨数、总时长、版本（mixtape/专辑）",
            "flow 与押韵、beat 与采样、叙事主题",
            "采样来源只有确定才写。",
            "曲名、时长、制作人、客串、采样来源"
        },
        {
            MusicProfile::Ambient, "ambient", "氛围/新世纪",
            "专辑名称、艺术家、发行时间、厂牌、创作理念、录音环境、使用乐器与设备、"
            "曲目列表（含时长）、总时长",
            "声音空间与时间感、聆听方式、与氛围/新世纪流派的关系",
            "不要把氛围专辑写成流行专辑；时长与结构描述尽量具体。",
            "曲名、时长、乐器/设备、录音环境"
        },
        {
            MusicProfile::World, "world", "世界音乐",
            "专辑名称、演奏者、地域与文化来源、乐器、发行时间、厂牌、曲目列表、总时长",
            "地域音乐语汇、演奏传统、录音方式",
            "地域写具体（如『西非马里』而非『非洲』）。",
            "曲名、时长、乐器、地域来源"
        },
        {
            MusicProfile::Generic, "generic", "通用",
            "专辑名称、译名、艺术家、发行时间、厂牌、专辑类型、语种、制作人员、"
            "曲目列表（音轨/曲名/时长/编曲）、碟数、音轨数、总时长、关联、背景",
            "创作背景、风格与制作、评价与影响",
            "",
            "曲名、时长、编曲、作词/作曲、所属专辑"
        },
    };
    return k;
}

inline const ProfileSpec& profile_spec(MusicProfile p) {
    const auto& all = all_profile_specs();
    for (const auto& s : all) if (s.profile == p) return s;
    return all.back();
}

// ===========================================================================
// 3. 提示词构建
// ===========================================================================

struct ProfilePrompt {
    std::string system;
    std::string user;
};

inline std::string pp_kind_word(const std::string& kind) {
    if (kind == "歌曲" || kind == "track") return "歌曲";
    if (kind == "艺术家" || kind == "person") return "艺术家";
    return "专辑";
}

// 输出格式说明（专辑/单曲/艺术家共用；单曲多一句「赏析」）
inline std::string pp_format_block(const std::string& kind_word) {
    std::string s;
    s += "【输出格式】只输出严格合法的 JSON 对象（不要 ```json 围栏、不要任何解释文字）：\n";
    s += "{\n";
    s += "  \"disclaimer\": \"AI 整理，仅供参考\",\n";
    s += "  \"fields\": { \"标签名\": \"值\" },\n";
    s += "  \"sections\": [ { \"heading\": \"📀 基本信息\", \"paragraphs\": [\"……\", \"……\"] } ],\n";
    s += "  \"citations\": [ { \"id\": \"M1\", \"title\": \"……\", \"url\": \"https://……\" } ]\n";
    s += "}\n";
    s += "说明：\n";
    s += "1. fields 的键名**自由**（中文优先），不限于上面列的清单；条数不设上限，能确定几条就写几条。\n";
    s += "2. fields 的值可以是多行文本：用 \\n 分行；要做表格就用 | 分列（例：\"曲目列表\": "
         "\"01 | 曲名 | 4:12 | 编曲者 | 原曲出处\\n02 | ……\"）。\n";
    s += "3. sections 放叙述与解读，heading 保留你习惯的分节标题（emoji 可以保留）；\n";
    s += "   paragraphs 可以是字符串数组（推荐），也可以是 {\"text\":\"…\",\"citations\":[\"M1\"]} 对象数组。\n";
    s += "4. citations 可选：只在确实引用了给定材料时给出，url 必须从材料里原样复制。\n";
    if (kind_word == "歌曲") {
        s += "5. 单曲必须至少包含一节「💿 补充背景与解读」：曲名意象、编曲风格、社团/作者风格、"
             "与其他作品的关联（能给几条给几条）。\n";
    }
    return s;
}

// 主入口：按 profile 生成提示词。
//   dec         classify_profile 的结果
//   entity      实体名（专辑名 / 曲名 / 艺术家名）
//   kind        "专辑" / "歌曲" / "艺术家"
//   material    已渲染好的材料块（[M1] … 形式）
//   known_tags  已知标签（pp_signal_tags 或调用方给的元数据）
inline ProfilePrompt build_profile_prompt(
    const ProfileDecision& dec,
    const std::string& entity,
    const std::string& kind,
    const std::string& material,
    const std::vector<std::pair<std::string, std::string>>& known_tags = {}) {
    const ProfileSpec& spec = profile_spec(dec.profile);
    const std::string kind_word = pp_kind_word(kind);

    std::string sys;
    sys += std::string("你是资深唱片资料编辑、乐评人与考据党，负责为【") + spec.label + "】类音乐整理资料。\n\n";
    sys += "【第一原则：不过滤】\n";
    sys += "1. 你能确证的每一条信息都必须输出：名称与译名、日期、编号、厂牌、人员、渠道、售价、时长、"
           "逐条曲目、版本、评价……不要因为『格式不统一』『篇幅太长』而省略。你省略的每一条，用户都拿不到。\n";
    sys += "2. 只有**不确定**的才不写。判断标准：说不出确切名称/日期/数字/人名/出处 ⟹ 不写"
           "（不要写『可能』『据说』『约』『暂无』『未知』）。\n";
    sys += "3. 允许自由标签：fields 的键名可以自拟（中文优先），不限于清单；值可以很长、可以多行、可以做表格。\n";
    sys += "4. 保留你自己的分节结构（如「📀 专辑基本信息」「🎵 曲目列表」「💿 补充背景与解读」），"
           "标题原样放进 sections[].heading，emoji 可以保留。\n";
    sys += "5. 不要元话语（『根据资料』『材料显示』『以上为』）；直接陈述事实。\n";
    sys += "6. 严禁编造 URL；引用只能从给定材料的链接里原样复制。\n";
    // 2026-10-27（真实调用反馈）：旧提示词只说「材料没说的不要写」，于是模型把「材料未给出售价」当结论，
    // 用户拿到的是一堆「没有」。正确做法是：材料说了就用材料，材料没说就用**模型自己的确证知识**并标注。
    sys += "7. 【材料不足时怎么办】这是常态，必须按下面五条处理：\n";
    sys += "   a) 材料只能回答一部分字段时，**其余必查字段用你自己确证的知识补全**，并在该值末尾标注"
           "「（AI 知识）」；例：\"首发日期\": \"2012-08-04（AI 知识）\"。\n";
    sys += "   b) 材料与你的知识冲突 ⟹ 以材料为准（不加标注）。\n";
    sys += "   c) 两边都确定不了的字段：**整条不要输出**；绝不写「材料未给出」「无法确认」「暂无」「未找到」"
           "这类句子 —— 它们对用户没有价值。\n";
    sys += "   d) 严禁拿「同团体/同厂牌的其他发行」「目录列表」「相似条目」「搜索结果」填充本实体的字段："
           "只有当条目标题就是本实体本身时，才算本实体的材料。\n";
    sys += "   e) 禁止过程性标签（如「匹配情况」「是否找到」「检索结果」）：标签必须是专辑的属性。\n";
    // 2026-10-27（真实调用反馈第 2 轮）：必查字段不能"默默消失"。
    // 「确定没有」也是确定信息（如会场限定盘没有公开通贩），要写出来；只有「不知道」才省略。
    sys += "   f) 【必查字段逐条给结论】上面列出的必查字段，一条都不要默默跳过：\n";
    sys += "      · 有确定值 ⟹ 直接写；\n";
    sys += "      · 能确定「没有/不适用」⟹ 也要写，并在值里说明，例如 "
           "\"通贩渠道\": \"仅会场贩售，未见公开通贩记录（AI 知识）\"；\n";
    sys += "      · 只有「完全不知道」的字段才省略。\n";
    sys += "8. 【材料页类型】材料里同时有「专辑/作品页」与「组织/艺术家页」时，以专辑页为准；"
           "组织页只能用来确认团体名、目录编号前缀这类外围信息。\n\n";
    // 2026-10-28（用户真机实测抓到的**真事故**）：
    //   资源为「Static World 会场限定CD / 02. 19：30.wav」（WAV 无任何标签，只有目录名与文件名），
    //   模型拿曲名 "19:30" 去搜网易云，搜到一个完全不相干的专辑《火花》（id=148064894，
    //   歌手「啵啵啵啵菜」，只收录 1 首 02:06），却断言
    //     「所属专辑：网易云音乐专辑页 id=148064894 收录本曲（专辑名与艺术家未能确证）」
    //   —— **它自己都写了"未能确证"，却仍然下了"收录本曲"的结论**。
    //   用户点开那个链接看到的是《火花》，与《19:30》毫无关系。
    // 这类错误比"没查到"更有害：它给出了一个可点击、可被信任的错误归属。
    sys += "9. 【引用外部页面必须先核对归属】这是本提示词里最容易出错的一条，务必逐字执行：\n";
    sys += "   a) 你可以引用材料里给出的音乐平台页面（网易云/QQ音乐/dizzylab 等），"
           "但**只有当该页面的内容能与本实体对上时**，才能说它「收录了」本实体。\n";
    sys += "   b) 「对得上」的判据（**至少满足两项**，缺一不可）：\n";
    sys += "      · 专辑名一致或明显是同一张（含译名、缩写、全半角差异）；\n";
    sys += "      · 曲目列表里能找到本曲（曲名一致，或曲目数与序号能对应）；\n";
    sys += "      · 时长一致（允许 ±3 秒）。\n";
    sys += "   c) **只对上曲名不算对得上** —— 曲名常常重名（「19:30」「Intro」「Theme」这类尤其如此）。\n";
    sys += "   d) 对不上时，**绝对不要写「收录本曲」「收录于」「所属专辑：XX 平台 id=...」这类断言**。"
           "要么整条不输出（按规则 7c），要么只写你确证的部分，**不带那个页面**。\n";
    sys += "   e) **禁止**出现「XX 专辑页收录本曲（专辑名与艺术家未能确证）」这种自相矛盾的表述："
           "既然未能确证专辑名，就没有依据断言收录关系。\n";
    sys += "   f) 你自己知道的发行信息（「这张碟是同人社团在某某展会上发的无料盘」）可以照写并标"
           "「（AI 知识）」，但**不要把不确定的搜索结果伪装成已核实的事实**。\n\n";
    // 2026-10-28（真机实测抓到的观感问题）：
    //   模型把 URL **直接写进字段值**，于是专辑名被劈成两半：
    //     "专辑名称": "Static World（https://vgmdb.net/org/5044） 会場限定CD"
    //     "译名":     "Static World（https://vgmdb.net/org/5044） 限定作"
    //   每一个出现社团名的地方都被插了一遍，读起来支离破碎。
    //   而**程序本身有 linkify_text()**：它会把核对过的名词（维基/萌娘/VGMdb/THBWiki 等）
    //   自动变成超链接。模型手写 URL 不但多余，还会把短语切断。
    sys += "10. 【字段值里不要写 URL】\n";
    sys += "   a) 所有字段值一律写**纯文本**：不要出现 http://、https://、www.，也不要用「名词（链接）」这种形式。\n";
    sys += "   b) 需要给出来源时，把链接放进 citations[]（并从材料里原样复制），"
           "**不要塞进 fields 的值里面**。\n";
    sys += "   c) **名词超链接由程序自动完成**（人名、团体、会场、专辑、作品、音乐厅等，"
           "程序会匹配维基百科 / 萌娘百科 / VGMdb / THBWiki 等权威站点）。"
           "你只要把名词写对，链接会自己挂上去。\n";
    sys += "   d) 反例（不要这样写）：\"专辑名称\": \"Static World（https://vgmdb.net/org/5044） 会場限定CD\"\n";
    sys += "      正例：\"专辑名称\": \"Static World 会場限定CD\"\n\n";
    sys += std::string("【本类必查信息】") + spec.fields + "\n";
    sys += std::string("【本类解读角度】") + spec.angles + "\n";
    if (spec.rules && *spec.rules) sys += std::string("【本类特有规矩】") + spec.rules + "\n";
    if (kind_word == "歌曲") {
        sys += std::string("【单曲核心档案】") + spec.track_fields + "\n";
        sys += "【单曲必写】曲目核心档案 + 「💿 补充背景与解读」（曲名意象、编曲风格、社团/作者风格、与其他作品的关联）。\n";
    }
    sys += "\n";
    sys += pp_format_block(kind_word);

    std::string user;
    user += std::string("实体：【") + entity + "】\n";
    user += std::string("类型：") + kind_word + "（分类：" + spec.label + "，依据：" + dec.reason + "）\n";
    if (!known_tags.empty()) {
        user += "\n【已知标签】（来自本地文件标签/数据库，可直接采用并补全）：\n";
        for (const auto& kv : known_tags) {
            if (kv.second.empty()) continue;
            user += "  " + kv.first + ": " + kv.second + "\n";
        }
    }
    if (!material.empty()) {
        user += "\n【参考材料】（编号 [M1] [M2] …，引用时只复制这里的链接）：\n";
        user += material;
        user += "\n";
    } else {
        user += "\n【参考材料】无（没有任何联网材料）：只写你有把握的共识信息，"
                "具体日期/编号/人名/售价一律不写。\n";
    }
    user += "\n请按上面的规范输出 JSON：先把所有能确定的标签写进 fields（越多越好），再写解读分节。";

    ProfilePrompt out;
    out.system = sys;
    out.user = user;
    return out;
}

// ===========================================================================
// 4. 宽松解析（不过滤）：模型给了什么就收什么
// ===========================================================================

struct ProfileCitation {
    std::string id;
    std::string title;
    std::string url;
    std::string source;
    // 2026-10-27（合并后回归 8.10）：模型给了 fetched_at 就必须**原样保留**。
    // 旧实现在转换层用 std::time(nullptr) 覆盖 ⟹ 同一份回包转两次结果不同，
    // 「传/不传 ProgressFn 逐字段相同」就会跨秒随机失败（实测 a=1793108892 b=1793108894）。
    // 纪律：解析/转换必须是**回包的纯函数**，不许读时钟。
    long long fetched_at = 0;
};

struct ProfileParagraph {
    std::string text;
    std::vector<std::string> citations;
};

struct ProfileSection {
    std::string heading;
    std::vector<ProfileParagraph> paragraphs;
};

struct ProfileDoc {
    bool ok = false;
    std::string error;
    std::string disclaimer;
    std::vector<std::pair<std::string, std::string>> fields;   // 保序；键名自由
    std::vector<ProfileSection> sections;
    std::vector<ProfileCitation> citations;
    bool from_prose = false;       // JSON 解析失败时按行兜底（内容仍完整展示）
    int dropped_citations = 0;     // 丢弃的无效引用（空 url / 不在白名单）
    int dropped_placeholders = 0;  // 丢弃的「未知/暂无」占位值

    size_t item_count() const {
        size_t n = fields.size();
        for (const auto& s : sections) {
            n += 1 + s.paragraphs.size();   // 分节标题也算一条
        }
        return n;
    }
};

namespace pp_detail {

inline bool is_placeholder(const std::string& v) {
    static const char* kBad[] = { "未知", "暂无", "无", "—", "-", "N/A", "n/a", "None", "null", "待补", "不详" };
    for (const char* b : kBad) if (v == b) return true;
    return false;
}

inline std::string json_scalar_to_text(const rta::net::Json& v);

// 对象 -> "k: v · k2: v2"（用于表格行/嵌套对象）
inline std::string json_obj_to_text(const rta::net::Json& v) {
    std::string out;
    bool first = true;
    for (const auto& kv : v.obj) {
        std::string val = json_scalar_to_text(kv.second);
        if (val.empty()) continue;
        if (!first) out += " · ";
        first = false;
        out += kv.first + ": " + val;
    }
    return out;
}

inline std::string json_scalar_to_text(const rta::net::Json& v) {
    switch (v.type) {
        case rta::net::Json::Str: {
            return pp_trim(v.str);
        }
        case rta::net::Json::Num: {
            if (v.num == (double)(long long)v.num) return std::to_string((long long)v.num);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.3f", v.num);
            return std::string(buf);
        }
        case rta::net::Json::Bool:
            return v.b ? "是" : "否";
        case rta::net::Json::Arr: {
            std::vector<std::string> rows;
            for (size_t i = 0; i < v.arr.size(); ++i) {
                const rta::net::Json& e = v.arr[i];
                std::string row = (e.type == rta::net::Json::Obj) ? json_obj_to_text(e) : json_scalar_to_text(e);
                row = pp_trim(row);
                if (!row.empty()) rows.push_back(row);
            }
            // 表格：多行；短列表：一行顿号 —— 让前端的「多行=独占整行」规则自然生效
            if (rows.size() <= 2) return pp_join(rows, "、");
            return pp_join(rows, "\n");
        }
        case rta::net::Json::Obj:
            return json_obj_to_text(v);
        default:
            return std::string();
    }
}

inline std::string strip_fence(const std::string& text) {
    std::string s = pp_trim(text);
    if (s.rfind("```json", 0) == 0) s = s.substr(7);
    else if (s.rfind("```", 0) == 0) s = s.substr(3);
    if (s.size() >= 3 && s.substr(s.size() - 3) == "```") s = s.substr(0, s.size() - 3);
    s = pp_trim(s);
    // 有的模型会在 JSON 前后各写一句话：截取第一个 { 到最后一个 }
    if (!s.empty() && s[0] != '{') {
        const size_t b = s.find('{');
        const size_t e = s.rfind('}');
        if (b != std::string::npos && e != std::string::npos && e > b) s = s.substr(b, e - b + 1);
    }
    return s;
}

// 段落数组：接受 ["a","b"] / [{"text":"a"}] / "a" / {"text":"a"}
inline void append_paragraphs(const rta::net::Json& node, ProfileSection& sec) {
    if (node.type == rta::net::Json::Str) {
        const std::string t = pp_trim(node.str);
        if (!t.empty()) {
            ProfileParagraph p;
            p.text = t;
            sec.paragraphs.push_back(std::move(p));
        }
        return;
    }
    if (node.type == rta::net::Json::Obj) {
        std::string t = pp_trim(node["text"].s());
        if (t.empty()) t = pp_trim(node["content"].s());
        if (t.empty()) t = json_obj_to_text(node);
        if (!t.empty()) {
            ProfileParagraph para;
            para.text = t;
            const rta::net::Json& ca = node["citations"];
            if (ca.type == rta::net::Json::Arr) {
                for (size_t i = 0; i < ca.arr.size(); ++i) {
                    const std::string id = pp_trim(ca.arr[i].s());
                    if (!id.empty()) para.citations.push_back(id);
                }
            }
            sec.paragraphs.push_back(std::move(para));
        }
        return;
    }
    if (node.type != rta::net::Json::Arr) return;
    for (size_t i = 0; i < node.arr.size(); ++i) append_paragraphs(node.arr[i], sec);
}

// 键值：接受 {"k":"v"} / {"k":["v1","v2"]} / [{"label":"k","value":"v"}] / [{"name":..,"value":..}]
inline void append_fields(const rta::net::Json& node,
                          std::vector<std::pair<std::string, std::string>>& out,
                          int* dropped_placeholders) {
    auto push = [&](std::string k, std::string v) {
        k = pp_trim(k);
        v = pp_trim(v);
        if (k.empty() || v.empty()) return;
        if (is_placeholder(v)) { if (dropped_placeholders) ++(*dropped_placeholders); return; }
        if (v.size() > 4000) v = v.substr(0, 4000);   // 只为保护缓存文件，不再按长度丢内容
        out.emplace_back(std::move(k), std::move(v));
    };
    if (node.type == rta::net::Json::Obj) {
        for (const auto& kv : node.obj) push(kv.first, json_scalar_to_text(kv.second));
        return;
    }
    if (node.type != rta::net::Json::Arr) return;
    for (size_t i = 0; i < node.arr.size(); ++i) {
        const rta::net::Json& e = node.arr[i];
        if (e.type == rta::net::Json::Obj) {
            std::string k = e["label"].s();
            if (k.empty()) k = e["name"].s();
            if (k.empty()) k = e["key"].s();
            std::string v = json_scalar_to_text(e["value"]);
            if (v.empty()) v = json_scalar_to_text(e["text"]);
            push(k, v);
        } else if (e.type == rta::net::Json::Arr && e.arr.size() >= 2) {
            push(e.arr[0].s(), json_scalar_to_text(e.arr[1]));
        }
    }
}

// JSON 不可解析时的兜底：按行切，标题行（📀/##/【】/加粗）当分节，其余当段落；绝不丢内容
inline void parse_prose(const std::string& text, ProfileDoc& doc) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    bool have_sec = false;
    size_t sec_i = 0;
    auto new_section = [&doc, &have_sec, &sec_i](const std::string& h) {
        doc.sections.push_back(ProfileSection{ h.empty() ? std::string("整理") : h, {} });
        sec_i = doc.sections.size() - 1;
        have_sec = true;
    };
    std::string body;
    auto flush = [&]() {
        const std::string t = pp_trim(body);
        body.clear();
        if (t.empty()) return;
        if (!have_sec) new_section("整理");
        ProfileParagraph p;
        p.text = t;
        doc.sections[sec_i].paragraphs.push_back(std::move(p));
    };

    for (const auto& raw : lines) {
        const std::string line = pp_trim(raw);
        if (line.empty()) { body += "\n"; continue; }
        bool is_heading = false;
        std::string h = line;
        if (line.size() >= 4 && line.compare(0, 4, "####") == 0) { is_heading = true; h = pp_trim(line.substr(4)); }
        else if (line.size() >= 3 && line.compare(0, 3, "###") == 0) { is_heading = true; h = pp_trim(line.substr(3)); }
        else if (line.size() >= 2 && line.compare(0, 2, "##") == 0) { is_heading = true; h = pp_trim(line.substr(2)); }
        else if (line.size() >= 1 && line[0] == '#') { is_heading = true; h = pp_trim(line.substr(1)); }
        else if (line.size() > 6 && line.compare(0, 2, "**") == 0 && line.rfind("**") == line.size() - 2) {
            is_heading = true;
            h = pp_trim(line.substr(2, line.size() - 4));
        } else if ((unsigned char)line[0] >= 0xE0 &&
                   (line.compare(0, 4, "📀") == 0 || line.compare(0, 4, "🎵") == 0 ||
                    line.compare(0, 4, "💿") == 0 || line.compare(0, 4, "🎼") == 0 ||
                    line.compare(0, 4, "📝") == 0)) {
            is_heading = true;
        }
        if (is_heading) {
            flush();
            new_section(h);
            continue;
        }
        if (!body.empty() && body.back() != '\n') body += "\n";
        body += line;
    }
    flush();
    if (doc.sections.empty() && !pp_trim(text).empty()) {
        new_section("整理");
        ProfileParagraph p;
        p.text = pp_trim(text);
        doc.sections[sec_i].paragraphs.push_back(std::move(p));
    }
    doc.from_prose = true;
}

} // namespace pp_detail

// 解析入口：宽松到「只要模型写了东西就一定能渲染」。
//   allowed_urls / enforce：给了就只保留白名单内的引用（不再因为一条坏引用否掉整份资料）
inline ProfileDoc parse_profile_reply(const std::string& text,
                                      const std::vector<std::string>& allowed_urls = {},
                                      bool enforce = false) {
    ProfileDoc doc;
    const std::string clean = pp_detail::strip_fence(text);
    const rta::net::Json j = rta::net::json_parse(clean);

    if (j.type != rta::net::Json::Obj) {
        // JSON 不合法 → 按行兜底（"不过滤"的底线：模型写了什么就展示什么）
        pp_detail::parse_prose(clean.empty() ? text : clean, doc);
        if (doc.sections.empty() && doc.fields.empty()) {
            doc.ok = false;
            doc.error = "模型输出既不是 JSON 也没有可展示文本";
            return doc;
        }
        doc.ok = true;
        doc.disclaimer = "AI 整理，仅供参考";
        return doc;
    }

    if (j.has("error")) {
        doc.ok = false;
        doc.error = "服务端报错: " + (j["error"].has("message") ? j["error"]["message"].s() : j["error"].s());
        return doc;
    }

    doc.disclaimer = pp_trim(j["disclaimer"].s());
    if (doc.disclaimer.empty()) doc.disclaimer = "AI 整理，仅供参考";

    // fields（键名自由、不限条数）
    if (j.has("fields")) pp_detail::append_fields(j["fields"], doc.fields, &doc.dropped_placeholders);
    else if (j.has("标签")) pp_detail::append_fields(j["标签"], doc.fields, &doc.dropped_placeholders);

    // sections（标题保留原文；段落接受字符串或对象）
    const rta::net::Json* sarr = nullptr;
    if (j.has("sections")) sarr = &j["sections"];
    else if (j.has("分节")) sarr = &j["分节"];
    if (sarr && sarr->type == rta::net::Json::Arr) {
        for (size_t i = 0; i < sarr->arr.size(); ++i) {
            const rta::net::Json& sitem = sarr->arr[i];
            if (sitem.type != rta::net::Json::Obj) continue;
            ProfileSection sec;
            sec.heading = pp_trim(sitem["heading"].s());
            if (sec.heading.empty()) sec.heading = pp_trim(sitem["title"].s());
            if (sec.heading.empty()) sec.heading = pp_trim(sitem["标题"].s());
            if (sec.heading.empty()) sec.heading = "正文";
            if (sitem.has("paragraphs")) pp_detail::append_paragraphs(sitem["paragraphs"], sec);
            else if (sitem.has("paragraph")) pp_detail::append_paragraphs(sitem["paragraph"], sec);
            else if (sitem.has("text")) pp_detail::append_paragraphs(sitem["text"], sec);
            else if (sitem.has("content")) pp_detail::append_paragraphs(sitem["content"], sec);
            if (!sec.paragraphs.empty()) doc.sections.push_back(std::move(sec));
        }
    }

    // citations：可选；有 url 且（未启用白名单 or 命中白名单）就保留，否则只丢那一条，不否整份
    const rta::net::Json& carr = j["citations"];
    if (carr.type == rta::net::Json::Arr) {
        for (size_t i = 0; i < carr.arr.size(); ++i) {
            const rta::net::Json& item = carr.arr[i];
            if (item.type != rta::net::Json::Obj) continue;
            ProfileCitation c;
            c.id = pp_trim(item["id"].s());
            c.title = pp_trim(item["title"].s());
            c.url = pp_trim(item["url"].s());
            c.source = pp_trim(item["source"].s());
            c.fetched_at = (long long)item["fetched_at"].n();   // 没给就是 0（未知），绝不读时钟
            if (c.id.empty()) c.id = "M" + std::to_string(i + 1);
            if (c.url.empty()) {
                int midx = 0;
                if (c.id.size() > 1 && (c.id[0] == 'M' || c.id[0] == 'm')) midx = std::atoi(c.id.c_str() + 1);
                if (midx >= 1 && (size_t)midx <= allowed_urls.size()) c.url = allowed_urls[(size_t)midx - 1];
            }
            if (c.url.empty()) { ++doc.dropped_citations; continue; }
            if (enforce) {
                bool hit = false;
                for (const auto& u : allowed_urls) {
                    if (!u.empty() && (u == c.url || u.find(c.url) != std::string::npos ||
                                       c.url.find(u) != std::string::npos)) {
                        hit = true;
                        break;
                    }
                }
                if (!hit) { ++doc.dropped_citations; continue; }
            }
            doc.citations.push_back(std::move(c));
        }
    }

    if (doc.fields.empty() && doc.sections.empty()) {
        // 结构化字段全空：再试一次按行兜底（模型可能把内容写在约定字段之外）
        pp_detail::parse_prose(clean, doc);
        if (doc.sections.empty() && doc.fields.empty()) {
            doc.ok = false;
            doc.error = "模型输出里没有可展示的内容";
            return doc;
        }
        doc.from_prose = true;
    }

    doc.ok = true;
    return doc;
}

// ===========================================================================
// 5. 名词 → 权威链接（只用确定性匹配，匹配不上就纯文本）
// ===========================================================================

struct LinkEntry {
    std::string name;    // 文本里出现的写法
    std::string url;     // 权威链接
    std::string source;  // wikipedia-zh / wikipedia-ja / wikipedia-en / moegirl / baike / discogs / musicbrainz / official
    std::string kind;    // 人物 / 团体 / 场地 / 厂牌 / 作品 / 活动 / 站点
};

// 语义上"不该当名词链接"的词（避免把『专辑』『编曲』连到某个百科条目）
inline const std::vector<std::string>& pp_link_stopwords() {
    static const std::vector<std::string> k = {
        "音乐", "专辑", "歌曲", "作品", "简介", "背景", "评价", "收录曲", "原曲", "制作人",
        "作曲", "编曲", "作词", "演唱", "乐团", "管弦乐团", "交响曲", "钢琴", "吉他",
        "album", "single", "music", "soundtrack", "track", "artist", "release"
    };
    return k;
}

// 种子表：2026-10-27 逐条 HTTP 抓取核对（全部返回 200）后写死。
// 纪律：只放**核对过 URL** 的条目；新增条目必须先验证 200，否则不准加 —— 绝不"瞎连"。
inline const std::vector<LinkEntry>& pp_seed_links() {
    static const std::vector<LinkEntry> k = {
        // ---- 人物 ----
        {"坂井泉水", "https://zh.wikipedia.org/wiki/坂井泉水", "wikipedia-zh", "人物"},
        {"大黑摩季", "https://zh.wikipedia.org/wiki/大黑摩季", "wikipedia-zh", "人物"},
        {"久石让", "https://zh.wikipedia.org/wiki/久石讓", "wikipedia-zh", "人物"},
        {"久石譲", "https://ja.wikipedia.org/wiki/久石譲", "wikipedia-ja", "人物"},
        {"坂本龙一", "https://zh.wikipedia.org/wiki/坂本龍一", "wikipedia-zh", "人物"},
        {"坂本龍一", "https://ja.wikipedia.org/wiki/坂本龍一", "wikipedia-ja", "人物"},
        {"中岛美雪", "https://zh.wikipedia.org/wiki/中島美雪", "wikipedia-zh", "人物"},
        {"中島みゆき", "https://ja.wikipedia.org/wiki/中島みゆき", "wikipedia-ja", "人物"},
        {"宇多田光", "https://zh.wikipedia.org/wiki/宇多田光", "wikipedia-zh", "人物"},
        {"椎名林檎", "https://zh.wikipedia.org/wiki/椎名林檎", "wikipedia-zh", "人物"},
        {"王菲", "https://zh.wikipedia.org/wiki/王菲", "wikipedia-zh", "人物"},
        {"邓丽君", "https://zh.wikipedia.org/wiki/邓丽君", "wikipedia-zh", "人物"},
        {"周杰伦", "https://zh.wikipedia.org/wiki/周杰伦", "wikipedia-zh", "人物"},
        {"李宗盛", "https://zh.wikipedia.org/wiki/李宗盛", "wikipedia-zh", "人物"},
        {"初音未来", "https://zh.wikipedia.org/wiki/初音未来", "wikipedia-zh", "人物"},
        {"Hatsune Miku", "https://en.wikipedia.org/wiki/Hatsune_Miku", "wikipedia-en", "人物"},
        {"菅野洋子", "https://zh.wikipedia.org/wiki/菅野洋子", "wikipedia-zh", "人物"},
        {"菅野よう子", "https://ja.wikipedia.org/wiki/菅野よう子", "wikipedia-ja", "人物"},
        {"Yoko Kanno", "https://en.wikipedia.org/wiki/Yoko_Kanno", "wikipedia-en", "人物"},
        {"梶浦由记", "https://zh.wikipedia.org/wiki/梶浦由記", "wikipedia-zh", "人物"},
        {"梶浦由記", "https://ja.wikipedia.org/wiki/梶浦由記", "wikipedia-ja", "人物"},
        {"Yuki Kajiura", "https://en.wikipedia.org/wiki/Yuki_Kajiura", "wikipedia-en", "人物"},
        {"泽野弘之", "https://zh.wikipedia.org/wiki/澤野弘之", "wikipedia-zh", "人物"},
        {"澤野弘之", "https://ja.wikipedia.org/wiki/澤野弘之", "wikipedia-ja", "人物"},
        {"Hiroyuki Sawano", "https://en.wikipedia.org/wiki/Hiroyuki_Sawano", "wikipedia-en", "人物"},
        {"Ryuichi Sakamoto", "https://en.wikipedia.org/wiki/Ryuichi_Sakamoto", "wikipedia-en", "人物"},
        {"Joe Hisaishi", "https://en.wikipedia.org/wiki/Joe_Hisaishi", "wikipedia-en", "人物"},
        {"巴赫", "https://zh.wikipedia.org/wiki/约翰·塞巴斯蒂安·巴赫", "wikipedia-zh", "人物"},
        {"贝多芬", "https://zh.wikipedia.org/wiki/路德维希·范·贝多芬", "wikipedia-zh", "人物"},
        {"莫扎特", "https://zh.wikipedia.org/wiki/沃尔夫冈·阿马德乌斯·莫扎特", "wikipedia-zh", "人物"},
        {"马勒", "https://zh.wikipedia.org/wiki/古斯塔夫·马勒", "wikipedia-zh", "人物"},
        {"卡拉扬", "https://zh.wikipedia.org/wiki/赫伯特·冯·卡拉扬", "wikipedia-zh", "人物"},
        {"Herbert von Karajan", "https://en.wikipedia.org/wiki/Herbert_von_Karajan", "wikipedia-en", "人物"},
        {"伯恩斯坦", "https://zh.wikipedia.org/wiki/伦纳德·伯恩斯坦", "wikipedia-zh", "人物"},
        {"Miles Davis", "https://en.wikipedia.org/wiki/Miles_Davis", "wikipedia-en", "人物"},
        {"迈尔斯·戴维斯", "https://zh.wikipedia.org/wiki/迈尔斯·戴维斯", "wikipedia-zh", "人物"},
        {"John Coltrane", "https://en.wikipedia.org/wiki/John_Coltrane", "wikipedia-en", "人物"},
        {"Bill Evans", "https://en.wikipedia.org/wiki/Bill_Evans", "wikipedia-en", "人物"},
        {"Brian Eno", "https://en.wikipedia.org/wiki/Brian_Eno", "wikipedia-en", "人物"},
        {"Nils Frahm", "https://en.wikipedia.org/wiki/Nils_Frahm", "wikipedia-en", "人物"},
        {"Nujabes", "https://en.wikipedia.org/wiki/Nujabes", "wikipedia-en", "人物"},
        {"ZUN", "https://ja.wikipedia.org/wiki/ZUN", "wikipedia-ja", "人物"},

        // ---- 团体 / 社团 / 乐团 / 乐队 ----
        {"ZARD", "https://zh.wikipedia.org/wiki/ZARD", "wikipedia-zh", "团体"},
        {"东京事变", "https://zh.wikipedia.org/wiki/東京事變", "wikipedia-zh", "团体"},
        {"東京事変", "https://ja.wikipedia.org/wiki/東京事変", "wikipedia-ja", "团体"},
        {"维也纳爱乐乐团", "https://zh.wikipedia.org/wiki/维也纳爱乐乐团", "wikipedia-zh", "团体"},
        {"柏林爱乐乐团", "https://zh.wikipedia.org/wiki/柏林爱乐乐团", "wikipedia-zh", "团体"},
        {"Berliner Philharmoniker", "https://en.wikipedia.org/wiki/Berliner_Philharmoniker", "wikipedia-en", "团体"},
        {"Vienna Philharmonic", "https://en.wikipedia.org/wiki/Vienna_Philharmonic", "wikipedia-en", "团体"},
        {"上海アリス幻樂団", "https://ja.wikipedia.org/wiki/上海アリス幻樂団", "wikipedia-ja", "团体"},
        {"上海爱丽丝幻乐团", "https://zh.moegirl.org.cn/上海爱丽丝幻乐团", "moegirl", "团体"},
        {"幽閉サテライト", "https://ja.wikipedia.org/wiki/幽閉サテライト", "wikipedia-ja", "团体"},
        {"豚乙女", "https://ja.wikipedia.org/wiki/豚乙女", "wikipedia-ja", "团体"},
        {"IOSYS", "https://ja.wikipedia.org/wiki/IOSYS", "wikipedia-ja", "团体"},
        {"Alstroemeria Records", "https://ja.wikipedia.org/wiki/Alstroemeria_Records", "wikipedia-ja", "团体"},
        {"ZUNTATA", "https://ja.wikipedia.org/wiki/ZUNTATA", "wikipedia-ja", "团体"},
        {"少女病", "https://ja.wikipedia.org/wiki/少女病", "wikipedia-ja", "团体"},
        {"妖精帝國", "https://ja.wikipedia.org/wiki/妖精帝國", "wikipedia-ja", "团体"},
        {"東京フィルハーモニー交響楽団", "https://ja.wikipedia.org/wiki/東京フィルハーモニー交響楽団", "wikipedia-ja", "团体"},
        {"ベルリン・フィルハーモニー管弦楽団", "https://ja.wikipedia.org/wiki/ベルリン・フィルハーモニー管弦楽団", "wikipedia-ja", "团体"},
        {"ウィーン・フィルハーモニー管弦楽団", "https://ja.wikipedia.org/wiki/ウィーン・フィルハーモニー管弦楽団", "wikipedia-ja", "团体"},
        {"Metallica", "https://en.wikipedia.org/wiki/Metallica", "wikipedia-en", "团体"},
        {"Opeth", "https://en.wikipedia.org/wiki/Opeth", "wikipedia-en", "团体"},
        {"Radiohead", "https://en.wikipedia.org/wiki/Radiohead", "wikipedia-en", "团体"},
        {"Daft Punk", "https://en.wikipedia.org/wiki/Daft_Punk", "wikipedia-en", "团体"},
        {"Aphex Twin", "https://en.wikipedia.org/wiki/Aphex_Twin", "wikipedia-en", "团体"},
        {"五月天", "https://zh.wikipedia.org/wiki/五月天", "wikipedia-zh", "团体"},

        // ---- 场地 / 音乐厅 ----
        {"Musikverein", "https://en.wikipedia.org/wiki/Musikverein", "wikipedia-en", "场地"},
        {"维也纳音乐协会大楼", "https://zh.wikipedia.org/wiki/維也納音樂協會大樓", "wikipedia-zh", "场地"},
        {"金色大厅", "https://en.wikipedia.org/wiki/Musikverein", "wikipedia-en", "场地"},
        {"Suntory Hall", "https://en.wikipedia.org/wiki/Suntory_Hall", "wikipedia-en", "场地"},
        {"サントリーホール", "https://ja.wikipedia.org/wiki/サントリーホール", "wikipedia-ja", "场地"},
        {"Abbey Road Studios", "https://en.wikipedia.org/wiki/Abbey_Road_Studios", "wikipedia-en", "场地"},

        // ---- 厂牌 ----
        {"Deutsche Grammophon", "https://en.wikipedia.org/wiki/Deutsche_Grammophon", "wikipedia-en", "厂牌"},
        {"ECM Records", "https://en.wikipedia.org/wiki/ECM_Records", "wikipedia-en", "厂牌"},
        {"Blue Note Records", "https://en.wikipedia.org/wiki/Blue_Note_Records", "wikipedia-en", "厂牌"},

        // ---- 作品 / 系列 / 活动 ----
        {"东方Project", "https://zh.moegirl.org.cn/东方Project", "moegirl", "作品"},
        {"東方Project", "https://ja.wikipedia.org/wiki/東方Project", "wikipedia-ja", "作品"},
        {"Touhou Project", "https://en.wikipedia.org/wiki/Touhou_Project", "wikipedia-en", "作品"},
        {"东方红魔乡", "https://zh.wikipedia.org/wiki/東方紅魔鄉", "wikipedia-zh", "作品"},
        {"東方紅魔郷", "https://ja.wikipedia.org/wiki/東方紅魔郷", "wikipedia-ja", "作品"},
        {"博麗神社例大祭", "https://ja.wikipedia.org/wiki/博麗神社例大祭", "wikipedia-ja", "活动"},
        {"コミックマーケット", "https://ja.wikipedia.org/wiki/コミックマーケット", "wikipedia-ja", "活动"},
        {"塞壬唱片", "https://zh.moegirl.org.cn/塞壬唱片", "moegirl", "厂牌"},
        {"明日方舟", "https://zh.moegirl.org.cn/明日方舟", "moegirl", "作品"},

        // ---- 数据库 / 发行平台（站点本身；条目页由材料标题动态生成）----
        // 注：vgmdb / discogs / booth 对脚本 UA 返回 403（反爬），但浏览器与本项目管线都能正常访问
        //（真实调用里 DeepSeek fetch 成功取回 vgmdb.net/org/5044）；dizzylab 实测 200。
        {"VGMdb", "https://vgmdb.net", "official", "站点"},
        {"vgmdb.net", "https://vgmdb.net", "official", "站点"},
        {"Discogs", "https://www.discogs.com", "official", "站点"},
        {"discogs.com", "https://www.discogs.com", "official", "站点"},
        {"MusicBrainz", "https://musicbrainz.org", "official", "站点"},
        {"musicbrainz.org", "https://musicbrainz.org", "official", "站点"},
        {"BOOTH", "https://booth.pm", "official", "站点"},
        {"booth.pm", "https://booth.pm", "official", "站点"},
        {"dizzylab", "https://dizzylab.net", "official", "站点"},
        {"dizzylab.net", "https://dizzylab.net", "official", "站点"},
    };
    return k;
}

inline std::string pp_url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += (char)((hi << 4) | lo); i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}

inline std::string pp_url_host(const std::string& url) {
    const size_t p = url.find("://");
    const size_t b = (p == std::string::npos) ? 0 : p + 3;
    size_t e = url.find('/', b);
    if (e == std::string::npos) e = url.size();
    return pp_lower_ascii(url.substr(b, e - b));
}

inline std::string pp_strip_source_suffix(const std::string& title) {
    std::string t = pp_trim(title);
    static const char* kSuffix[] = {
        " - 维基百科，自由的百科全书", " - 维基百科", " - Wikipedia", " – Wikipedia",
        " - 萌娘百科", " - 百度百科", " | Discogs", " - Discogs", " - MusicBrainz",
        "｜Discogs", " — 萌娘百科",
        // 2026-10-27（真实调用反馈）：VGMdb / BOOTH / dizzylab 的检索标题也带站点后缀，
        // 不去掉就会把「Static World - VGMdb」整体当成名词，永远匹配不上正文。
        " - VGMdb", " | VGMdb", "｜VGMdb", " - VGMdb.info", " | VGMdb.info",
        " - BOOTH", " | BOOTH", " - dizzylab", " | dizzylab"
    };
    for (const char* suf : kSuffix) {
        const size_t n = std::string(suf).size();
        if (t.size() > n && t.compare(t.size() - n, n, suf) == 0) {
            t = pp_trim(t.substr(0, t.size() - n));
            break;
        }
    }
    return t;
}

// 按 profile 定制检索词尾（2026-10-27 真实调用反馈：同人盘只搜到 VGMdb 组织页）。
// 目的不是"多搜"，而是把搜索引到**专辑页**（曲目/售价/颁布日期都在专辑页上）。
// 2026-10-28（用户真机对比：DeepSeek 网页能搜到准确资料，软件里是垃圾）：
//   除了「artist 与 name 重复」之外，还有第二层重复 —— suffix 里的判别词与 base 里的词撞车：
//     base   = "Static World 会场限定CD"
//     suffix = " 同人 東方 会场限定 颁布 曲目 社团 售价"
//                                ↑ 又一遍「会场限定」
//   重复词会拉低搜索引擎的相关性排序。这里按**空格分词 + 子串判定**去掉已经出现过的词
//   （中文没有分词器，所以用"这个词是不是 base 的子串"来判断，对「会场限定 ⊂ 会场限定CD」这种
//    情形恰好成立，也不会丢信息 —— base 里本来就有）。
//   同时给后缀长度设了软上限：超过 6 个词就把尾部（通常是次要判别词）丢掉，避免稀释。
inline std::string pp_query_merge_suffix(const std::string& base, const std::string& suffix) {
    // 拆 base（用于子串判定）与 suffix（用于逐词过滤）
    std::vector<std::string> sw;
    {
        std::string cur;
        for (char ch : suffix) {
            if (ch == ' ' || ch == '\t') { if (!cur.empty()) { sw.push_back(cur); cur.clear(); } }
            else cur.push_back(ch);
        }
        if (!cur.empty()) sw.push_back(cur);
    }
    std::string out = base;
    int kept = 0;
    for (const auto& w : sw) {
        if (w.empty()) continue;
        if (base.find(w) != std::string::npos) continue;   // base 里已有 ⟹ 跳过
        if (kept >= 6) break;                              // 软上限：避免 query 被稀释
        out += ' ';
        out += w;
        ++kept;
    }
    return out;
}

inline std::string pp_search_query_suffix(MusicProfile p, const std::string& lang) {
    const bool ja = (lang == "ja");
    const bool en = (lang == "en");
    switch (p) {
        case MusicProfile::Doujin:
            if (en) return " doujin touhou arrange album tracklist circle release";
            if (ja) return " 同人 東方 アレンジ 会場限定 頒布 収録曲 サークル";
            return " 同人 東方 会场限定 颁布 曲目 社团 售价";
        case MusicProfile::Classical:
            if (en) return " classical album conductor orchestra recording tracklist";
            if (ja) return " クラシック 指揮 管弦楽団 録音 収録曲";
            return " 古典 指挥 乐团 录音 曲目 版本";
        case MusicProfile::Jazz:
            if (en) return " jazz album lineup recording date label tracklist";
            if (ja) return " ジャズ 録音 メンバー 収録曲";
            return " 爵士 乐手 录音 曲目 厂牌";
        case MusicProfile::Soundtrack:
            if (en) return " original soundtrack tracklist composer release date";
            if (ja) return " サウンドトラック 収録曲 作曲 発売日";
            return " 原声 配乐 曲目 作曲 发行";
        case MusicProfile::Vocal:
            if (en) return " vocaloid album tracklist producer release";
            if (ja) return " ボカロ 収録曲 P主 投稿日";
            return " 虚拟歌手 曲目 P主 投稿";
        case MusicProfile::Metal:
        case MusicProfile::Rock:
            if (en) return " album tracklist lineup producer release date";
            return " 专辑 曲目 成员 制作 发行";
        case MusicProfile::Electronic:
            if (en) return " electronic album tracklist label release";
            return " 电子 专辑 曲目 厂牌 发行";
        case MusicProfile::Folk:
            if (en) return " traditional album instruments tracklist";
            return " 民乐 乐器 曲目 演奏";
        case MusicProfile::Ambient:
            if (en) return " ambient album tracklist release label";
            return " 氛围 专辑 曲目 厂牌";
        case MusicProfile::HipHop:
            if (en) return " hip hop album tracklist producer release";
            return " 嘻哈 专辑 曲目 制作";
        case MusicProfile::World:
            if (en) return " world music album tracklist region";
            return " 世界音乐 专辑 曲目 地域";
        case MusicProfile::Pop:
            if (en) return " album tracklist release date label";
            return " 专辑 曲目 发行 厂牌";
        case MusicProfile::Generic:
            break;
    }
    if (en) return " album tracklist release date label credits";
    if (ja) return " アルバム 収録曲 発売日 レーベル";
    return " 专辑 曲目 发行时间 曲目列表 制作";
}

// ---------------------------------------------------------------------------
// 页面类型优先级（2026-10-27 真实调用反馈：材料抓到了 VGMdb **组织页** /org/5044，
// 模型只能看见「同团体 22 条目录」这种与专辑无关的内容 ⟹ 字段全是目录列表）。
// 单位：越大越该当主材料。专辑/作品页 > 通用 > 组织/艺术家/厂牌页（只作补充）。
// ---------------------------------------------------------------------------
inline int pp_page_kind_rank(const std::string& url, const std::string& title = std::string()) {
    const std::string u = pp_lower_ascii(url);
    int rank = 0;
    // 专辑 / 发行 / 作品页
    static const char* kGood[] = { "/album/", "/albums/", "/release/", "/releases/", "/master/",
                                   "/item/", "/song/", "/songs/", "/disc/", "/music/", "/product/" };
    for (const char* g : kGood) if (u.find(g) != std::string::npos) { rank += 3; break; }
    // 组织 / 艺术家 / 厂牌页（同团体目录、艺人作品表 —— 只能当补充）
    static const char* kWeak[] = { "/org/", "/artist/", "/artists/", "/label/", "/labels/",
                                   "/company/", "/band/", "/person/" };
    for (const char* w : kWeak) if (u.find(w) != std::string::npos) { rank -= 2; break; }
    const std::string t = pp_lower_ascii(title);
    if (!t.empty() && (t.find("album") != std::string::npos || t.find("专辑") != std::string::npos ||
                       t.find("release") != std::string::npos || t.find("作品") != std::string::npos)) {
        rank += 1;
    }
    return rank;
}

inline const char* pp_page_kind_name(int rank) {
    if (rank >= 2) return "专辑/作品页（主材料）";
    if (rank <= -1) return "组织/艺术家页（仅补充，不能当本实体事实）";
    return "通用页面";
}

// 链接索引：种子表 + 材料 URL + 页面标题三种来源，全部按**最长优先**匹配。
class LinkIndex {
public:
    void add(const std::string& name, const std::string& url, const std::string& source,
             const std::string& kind = std::string()) {
        const std::string n = pp_trim(name);
        const std::string u = pp_trim(url);
        if (n.empty() || u.empty()) return;
        const bool cjk = !pp_is_ascii_only(n);
        if (cjk ? (n.size() < 2) : (n.size() < 3)) return;   // 太短的名字不连（防误连）
        for (const auto& bad : pp_link_stopwords()) if (n == bad) return;
        if (u.rfind("http", 0) != 0) return;
        for (const auto& e : entries_) if (e.name == n) return;   // 先到先得（种子表先加）
        entries_.push_back(LinkEntry{ n, u, source, kind.empty() ? std::string("名词") : kind });
        sorted_ready_ = false;
    }

    void add_seed_table() {
        for (const auto& e : pp_seed_links()) add(e.name, e.url, e.source, e.kind);
    }

    // 从百科类 URL 反推条目名：https://zh.wikipedia.org/wiki/坂井泉水 → 坂井泉水
    void add_from_url(const std::string& url, const std::string& page_title = std::string()) {
        const std::string host = pp_url_host(url);
        std::string source, kind, name;
        if (host.find("wikipedia.org") != std::string::npos) {
            if (host.rfind("zh.", 0) == 0) source = "wikipedia-zh";
            else if (host.rfind("ja.", 0) == 0) source = "wikipedia-ja";
            else source = "wikipedia-en";
            const size_t w = url.find("/wiki/");
            if (w == std::string::npos) return;
            std::string slug = url.substr(w + 6);
            const size_t q = slug.find_first_of("?#");
            if (q != std::string::npos) slug = slug.substr(0, q);
            name = pp_url_decode(slug);
            for (char& c : name) if (c == '_') c = ' ';
            name = pp_trim(name);
            if (name.find(':') != std::string::npos) return;   // File:/Category:/Wikipedia:
            kind = "名词";
        } else if (host.find("moegirl.org.cn") != std::string::npos ||
                   host.find("baike.baidu.com") != std::string::npos) {
            source = (host.find("moegirl") != std::string::npos) ? "moegirl" : "baike";
            const size_t p = url.find("://");
            size_t b = (p == std::string::npos) ? 0 : url.find('/', p + 3);
            if (b == std::string::npos) return;
            std::string path = url.substr(b + 1);
            const size_t q = path.find_first_of("?#");
            if (q != std::string::npos) path = path.substr(0, q);
            if (!path.empty() && path.back() == '/') path.pop_back();
            const size_t slash = path.rfind('/');
            name = pp_url_decode((slash == std::string::npos) ? path : path.substr(slash + 1));
            kind = "名词";
        } else {
            // 其它站点（VGMdb / Discogs / MusicBrainz / BOOTH / dizzylab / 官方站）：
            // URL 里不含条目名，只能靠**页面标题**。标题与 URL 来自同一条检索结果，
            // 所以「标题 → 这个 URL」是确定关系，不是瞎连。
            const std::string t = pp_strip_source_suffix(page_title);
            if (t.empty()) return;
            if (t.size() > 60) return;                      // 太长的是整句标题，不是名词
            if (t.find('\n') != std::string::npos) return;
            add(t, url, host, "名词");
            return;
        }
        if (name.empty()) return;
        // 页面标题更像"这篇讲的是什么"：同文种时优先用标题当条目名
        const std::string t = pp_strip_source_suffix(page_title);
        if (!t.empty() && pp_is_ascii_only(t) == pp_is_ascii_only(name)) add(t, url, source, kind);
        add(name, url, source, kind);
    }

    bool empty() const { return entries_.empty(); }
    const std::vector<LinkEntry>& entries() const { return entries_; }

    // 在 text[pos] 处尝试匹配（最长优先）
    const LinkEntry* match_at(const std::string& text, size_t pos, size_t* out_len) const {
        ensure_sorted();
        for (const auto& e : sorted_) {
            if (pos + e.name.size() > text.size()) continue;
            if (text.compare(pos, e.name.size(), e.name) != 0) continue;
            if (pp_is_ascii_only(e.name)) {
                const bool left_ok = (pos == 0) || !pp_is_word_char(text[pos - 1]);
                const size_t end = pos + e.name.size();
                const bool right_ok = (end >= text.size()) || !pp_is_word_char(text[end]);
                if (!left_ok || !right_ok) continue;
            }
            if (out_len) *out_len = e.name.size();
            return &e;
        }
        return nullptr;
    }

private:
    void ensure_sorted() const {
        if (sorted_ready_) return;
        sorted_ = entries_;
        std::stable_sort(sorted_.begin(), sorted_.end(), [](const LinkEntry& a, const LinkEntry& b) {
            return a.name.size() > b.name.size();
        });
        sorted_ready_ = true;
    }

    std::vector<LinkEntry> entries_;
    mutable std::vector<LinkEntry> sorted_;
    mutable bool sorted_ready_ = true;
};

struct LinkMatch {
    std::string name;
    bool hit = false;
    std::string url;
    std::string source;
    std::string kind;
};

// 把文本里的名词替换成「名词（权威链接）」——前端已有的 linkifyEscaped 会把裸 URL 变成可点外链，
// 因此这条路径**不需要改前端**就能点开；同时 links[] 也回给前端做结构化外链。
inline std::string linkify_text(const std::string& text, const LinkIndex& idx, int max_links,
                                std::vector<LinkMatch>* report = nullptr) {
    if (text.empty() || idx.empty() || max_links <= 0) return text;
    std::string out;
    out.reserve(text.size() + 64);
    std::vector<std::string> linked;
    int count = 0;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char uc = (unsigned char)text[i];
        const bool try_match = ((uc & 0xC0) != 0x80);   // 只在 UTF-8 字符起始字节处尝试
        if (try_match && count < max_links) {
            size_t len = 0;
            const LinkEntry* e = idx.match_at(text, i, &len);
            if (e) {
                bool dup = false;
                for (const auto& n : linked) if (n == e->name) { dup = true; break; }
                if (!dup) {
                    out += text.substr(i, len);
                    out += "（";
                    out += e->url;
                    out += "）";
                    linked.push_back(e->name);
                    if (report) report->push_back(LinkMatch{ e->name, true, e->url, e->source, e->kind });
                    ++count;
                    i += len;
                    continue;
                }
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

// 探针：给一批名词，逐个报告命中/未命中（判据 #5 用）
inline std::vector<LinkMatch> probe_links(const std::vector<std::string>& names, const LinkIndex& idx) {
    std::vector<LinkMatch> out;
    for (const auto& n : names) {
        LinkMatch m;
        m.name = n;
        size_t len = 0;
        const LinkEntry* e = idx.match_at(n, 0, &len);
        if (e && len == n.size()) {
            m.hit = true;
            m.url = e->url;
            m.source = e->source;
            m.kind = e->kind;
        }
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace rta::meta
