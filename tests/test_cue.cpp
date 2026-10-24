// test_cue.cpp — CUE 整轨分轨回归（src/meta/cue.h 解析器 + src/net/library.h 的 lib_cue_expand）
//
// 判据纪律（铁律 3：测试「通过」不等于「测对了东西」）：
//   * **逐字段断言**，不写「不崩就算过」：每条用例都写出期望的专辑名 / 艺术家 / 轨名 / 偏移，
//     并且 GBK 与 UTF-8 两份解析结果**逐字段相等**，才证明编码回退真的走到了 GBK 分支。
//   * GBK 用例额外断言「那串字节不是合法 UTF-8」——否则它本来就是 UTF-8，回退分支根本没被执行
//     （这就是本仓反复踩的「判据没覆盖被测对象」）。
//   * TIME 换算断言**具体采样数**（1 帧 @44100 = 588、@48000 = 640），并单独测四舍五入
//     （1000Hz 下 2 帧 = 26.67 → 27；截断会得 26，这条能把「截断」和「四舍五入」区分开）。
//   * 时长断言「专辑总时长 = 整轨时长」，而不是「N 轨各自等于整轨时长」——后者正是分轨最容易写错的形态。
//   * 多文件 cue：这里选择**支持**（每条轨道自带所属文件）而不是拒绝；用例断言每轨归属文件正确，
//     且 FILE 指向不存在文件时**只跳过该文件的轨**，其余照常（降级，不丢歌）。
//
// ⚠️ 未覆盖（如实记录）：
//   1) 没有测 UTF-16 编码的 cue（实现支持 BOM 判定，但缺真实样本）；2) 没有测 4GB 以上整轨；
//   3) 播放侧偏移**尚未接入**（rtaweb.cpp 归另一条线），本测试只到「库/展开层能给出偏移」。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "meta/cue.h"
#include "net/library.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using rta::meta::CueSheet;
using rta::meta::CueTrack;

static int g_fail = 0;
static void ck(bool cond, const std::string& what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what.c_str());
    else { std::printf("  [FAIL] %s   %s\n", what.c_str(), detail.c_str()); ++g_fail; }
}
static void hdr(int no, const char* title) { std::printf("\n==== 用例 %d：%s ====\n", no, title); }

// UTF-8 -> GBK（CP936）：造「中文 cue 的真实编码」
static std::string u8_to_gbk(const std::string& u8) {
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return std::string();
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), w.data(), n);
    const int m = ::WideCharToMultiByte(936, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (m <= 0) return std::string();
    std::string s((size_t)m, '\0');
    ::WideCharToMultiByte(936, 0, w.c_str(), (int)w.size(), s.data(), m, nullptr, nullptr);
    return s;
}

static void write_bytes(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
}

// 造一份「已探测」的物理文件信息（等价于 ffprobe 的结果）
static rta::net::LibTrack mkprobe(const std::string& path, int sr, double dur,
                                  int bits = 16, int ch = 2, int cover = 0) {
    rta::net::LibTrack t;
    t.path = path; t.sampleRate = sr; t.duration = dur; t.bits = bits; t.channels = ch;
    t.hasCover = cover; t.mtime = 111; t.size = 222; t.codec = "flac";
    return t;
}

static std::string cue_samples(long long f75, int sr) { return std::to_string(rta::meta::cue_frames_to_samples(f75, sr)); }

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== CUE 整轨分轨回归（解析器 + 虚拟曲目展开）==\n");

    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / "rta_cue_test";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::string tmp8 = rta::meta::cue_path_utf8(tmp);

    // =====================================================================
    hdr(1, "标准单文件 3 TRACK：FILE/TRACK/INDEX 00|01/TITLE/PERFORMER/REM/CATALOG/ISRC 逐字段");
    // =====================================================================
    const std::string cue1 =
        "REM GENRE \"Jazz\"\r\n"
        "REM DATE 1996\r\n"
        "CATALOG 0075021033003\r\n"
        "PERFORMER \"Bill Evans Trio\"\r\n"
        "TITLE \"Sunday at the Village Vanguard\"\r\n"
        "FILE \"album.wav\" WAVE\r\n"
        "  TRACK 01 AUDIO\r\n"
        "    TITLE \"A1 Waltz for Debby\"\r\n"
        "    PERFORMER \"Bill Evans\"\r\n"
        "    ISRC USUM70601185\r\n"
        "    INDEX 01 00:00:00\r\n"
        "  TRACK 02 AUDIO\r\n"
        "    TITLE \"A2 My Foolish Heart\"\r\n"
        "    ISRC USUM70601186\r\n"
        "    INDEX 00 09:30:22\r\n"
        "    INDEX 01 09:32:35\r\n"
        "  TRACK 03 AUDIO\r\n"
        "    INDEX 01 15:00:00\r\n";
    {
        const fs::path cuePath = tmp / "album.cue";
        write_bytes(cuePath, cue1);
        const CueSheet sh = rta::meta::cue_parse_file(rta::meta::cue_path_utf8(cuePath));
        ck(sh.albumTitle == "Sunday at the Village Vanguard", "专辑名 = 全局 TITLE", sh.albumTitle);
        ck(sh.performer == "Bill Evans Trio", "专辑艺术家 = 全局 PERFORMER", sh.performer);
        ck(sh.genre == "Jazz", "REM GENRE 解析", sh.genre);
        ck(sh.date == "1996", "REM DATE 解析", sh.date);
        ck(sh.catalog == "0075021033003", "CATALOG 解析", sh.catalog);
        ck(sh.tracks.size() == 3, "3 条轨", std::to_string(sh.tracks.size()));
        ck(sh.errors.empty(), "无结构错误", sh.firstError());
        if (sh.tracks.size() == 3) {
            const auto& t0 = sh.tracks[0];
            const auto& t1 = sh.tracks[1];
            const auto& t2 = sh.tracks[2];
            ck(t0.number == 1 && t1.number == 2 && t2.number == 3, "TRACK 号 1/2/3");
            ck(t0.title == "A1 Waltz for Debby", "轨 1 TITLE", t0.title);
            ck(t0.performer == "Bill Evans", "轨 1 PERFORMER", t0.performer);
            ck(t0.isrc == "USUM70601185", "轨 1 ISRC", t0.isrc);
            ck(t0.index1 == 0 && !t0.hasIndex0(), "轨 1 INDEX 01 = 0、无 INDEX 00");
            ck(t1.index0 == (9LL * 60 + 30) * 75 + 22, "轨 2 INDEX 00 = 09:30:22（1/75 秒帧）",
               std::to_string(t1.index0));
            ck(t1.index1 == (9LL * 60 + 32) * 75 + 35, "轨 2 INDEX 01 = 09:32:35（1/75 秒帧）",
               std::to_string(t1.index1));
            ck(t1.isrc == "USUM70601186", "轨 2 ISRC（与轨 1 不串）", t1.isrc);
            ck(t2.title.empty(), "轨 3 没有 TITLE（保持空，回退交给展开层）");
            // FILE 相对路径按 cue 所在目录解析
            ck(fs::path(rta::meta::cue_u8path(t0.file)) == (tmp / "album.wav"),
               "FILE 相对路径按 cue 目录解析成绝对路径", t0.file);
            ck(t0.albumTitle == "Sunday at the Village Vanguard", "轨内已带全局专辑名快照", t0.albumTitle);
            ck(t0.type == "AUDIO" && t0.audio, "TRACK 类型 AUDIO");
            ck(t0.fileType == "WAVE", "FILE 类型 WAVE", t0.fileType);
        }
        ck(rta::meta::cue_path_utf8(cuePath) == sh.cuePath, "sheet.cuePath 回填");
    }

    // =====================================================================
    hdr(2, "UTF-8 BOM：带 BOM 的中文 cue 解码后字段与期望逐字相等，BOM 不进字段");
    // =====================================================================
    const std::string zh_cue =
        "REM GENRE \"Classical\"\r\n"
        "REM DATE 2005\r\n"
        "PERFORMER \"群星\"\r\n"
        "TITLE \"发烧友测试碟 · 中文样本\"\r\n"
        "FILE \"群星 - 发烧友测试碟.flac\" WAVE\r\n"
        "  TRACK 01 AUDIO\r\n"
        "    TITLE \"第一轨 夜曲\"\r\n"
        "    PERFORMER \"李云迪\"\r\n"
        "    INDEX 01 00:00:00\r\n"
        "  TRACK 02 AUDIO\r\n"
        "    TITLE \"第二轨 黄河颂\"\r\n"
        "    PERFORMER \"郎朗\"\r\n"
        "    INDEX 00 04:12:00\r\n"
        "    INDEX 01 04:14:00\r\n"
        "  TRACK 03 AUDIO\r\n"
        "    TITLE \"第三轨 梁祝（节选）\"\r\n"
        "    INDEX 01 08:00:00\r\n";
    {
        const fs::path p = tmp / "zh_utf8bom.cue";
        write_bytes(p, std::string("\xEF\xBB\xBF", 3) + zh_cue);
        const CueSheet sh = rta::meta::cue_parse_file(rta::meta::cue_path_utf8(p));
        ck(sh.albumTitle == "发烧友测试碟 · 中文样本", "BOM 文件：专辑名逐字相等", sh.albumTitle);
        ck(!sh.albumTitle.empty() && (unsigned char)sh.albumTitle[0] != 0xEF,
           "BOM 没有残留在第一个字段里");
        ck(sh.performer == "群星", "BOM 文件：专辑艺术家", sh.performer);
        ck(sh.tracks.size() == 3 && sh.tracks[0].title == "第一轨 夜曲", "BOM 文件：轨 1 标题",
           sh.tracks.empty() ? "" : sh.tracks[0].title);
        // 解码函数本身：BOM 必须被剥掉（UTF-8 BOM 三个字节 = EF BB BF）
        const std::string dec = rta::meta::cue_decode_text(std::string("\xEF\xBB\xBF", 3) + zh_cue);
        ck(!dec.empty() && (unsigned char)dec[0] == 'R', "cue_decode_text 剥掉 UTF-8 BOM");
    }

    // =====================================================================
    hdr(3, "GBK：同一份中文 cue 用 CP936 存盘，解析结果与 UTF-8 版逐字段相等");
    // =====================================================================
    {
        const std::string gbk = u8_to_gbk(zh_cue);
        ck(!gbk.empty(), "GBK 编码成功（非空）");
        ck(gbk != zh_cue, "GBK 字节序列与 UTF-8 不同（真的换了编码）");
        // 判据核心：这串字节**不是**合法 UTF-8，所以解析器只能走 GBK 回退分支
        ck(!rta::meta::cue_utf8_valid(gbk), "GBK 字节不是合法 UTF-8 ⇒ 必然走 GBK 回退分支");

        const fs::path p = tmp / "zh_gbk.cue";
        write_bytes(p, gbk);
        const CueSheet shG = rta::meta::cue_parse_file(rta::meta::cue_path_utf8(p));
        const CueSheet shU = rta::meta::cue_parse_text(zh_cue, rta::meta::cue_path_utf8(p));
        ck(shG.tracks.size() == 3, "GBK 文件解析出 3 条轨", std::to_string(shG.tracks.size()));
        ck(shG.albumTitle == shU.albumTitle && shG.albumTitle == "发烧友测试碟 · 中文样本",
           "GBK 专辑名与 UTF-8 版逐字节相等", shG.albumTitle);
        ck(shG.performer == shU.performer && shG.performer == "群星", "GBK 专辑艺术家一致", shG.performer);
        ck(shG.genre == shU.genre && shG.genre == "Classical", "GBK REM GENRE 一致", shG.genre);
        bool allEq = shG.tracks.size() == shU.tracks.size();
        std::string diff;
        for (size_t i = 0; allEq && i < shG.tracks.size(); ++i) {
            const auto& a = shG.tracks[i];
            const auto& b = shU.tracks[i];
            if (a.title != b.title || a.performer != b.performer || a.index1 != b.index1 ||
                a.number != b.number || a.file != b.file) { allEq = false; diff = "轨 " + std::to_string(i + 1); }
        }
        ck(allEq, "GBK 版 3 条轨的 标题/艺术家/偏移/文件 与 UTF-8 版全部一致", diff);
        ck(shG.tracks.size() == 3 && shG.tracks[2].title == "第三轨 梁祝（节选）",
           "GBK 轨 3 中文标题逐字相等（含全角括号）",
           shG.tracks.size() == 3 ? shG.tracks[2].title : "");
    }

    // =====================================================================
    hdr(4, "无 TITLE：三级回退 TITLE → PERFORMER → 「文件名 #NN」");
    // =====================================================================
    {
        const std::string cue =
            "TITLE \"合集\"\nPERFORMER \"合辑艺人\"\nFILE \"mix.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"有标题\"\n    PERFORMER \"甲\"\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    PERFORMER \"乙\"\n    INDEX 01 01:00:00\n"
            "  TRACK 03 AUDIO\n    INDEX 01 02:00:00\n";
        const CueSheet sh = rta::meta::cue_parse_text(cue, (tmp / "mix.cue").string());
        std::map<std::string, rta::net::LibTrack> probes;
        probes.emplace(sh.tracks[0].file, mkprobe(sh.tracks[0].file, 44100, 180.0));
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 3, "3 条虚拟轨", std::to_string(out.size()));
        if (out.size() == 3) {
            ck(out[0].title == "有标题", "轨 1：有 TITLE 就用 TITLE", out[0].title);
            ck(out[1].title == "乙", "轨 2：缺 TITLE → 回退轨 PERFORMER", out[1].title);
            ck(out[2].title == "mix #03", "轨 3：TITLE/PERFORMER 都缺 → 「文件名 #NN」", out[2].title);
            ck(out[0].artist == "甲" && out[1].artist == "乙" && out[2].artist == "合辑艺人",
               "艺术家：轨 PERFORMER 优先，缺则用全局 PERFORMER",
               out[2].artist);
            ck(out[0].album == "合集" && out[0].albumArtist == "合辑艺人", "专辑名/专辑艺术家来自 cue");
            ck(out[0].trackNo == 1 && out[1].trackNo == 2 && out[2].trackNo == 3, "曲目顺序 1/2/3");
        }
    }

    // =====================================================================
    hdr(5, "INDEX 00 与 INDEX 01 并存：起点用 INDEX 01，不是 INDEX 00");
    // =====================================================================
    {
        const std::string cue =
            "TITLE \"Gap\"\nPERFORMER \"P\"\nFILE \"gap.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    INDEX 00 09:30:22\n    INDEX 01 09:32:35\n";
        const CueSheet sh = rta::meta::cue_parse_text(cue, (tmp / "gap.cue").string());
        std::map<std::string, rta::net::LibTrack> probes;
        probes.emplace(sh.tracks[0].file, mkprobe(sh.tracks[0].file, 44100, 900.0));
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 2, "2 条虚拟轨", std::to_string(out.size()));
        if (out.size() == 2) {
            const long long want01 = rta::meta::cue_frames_to_samples((9LL * 60 + 32) * 75 + 35, 44100);
            const long long want00 = rta::meta::cue_frames_to_samples((9LL * 60 + 30) * 75 + 22, 44100);
            ck(out[1].cueOff == want01, "轨 2 起点 = INDEX 01 换算的采样数", std::to_string(out[1].cueOff));
            ck(out[1].cueOff != want00, "轨 2 起点**不是** INDEX 00（前导间隙不算进曲目）");
            ck(out[1].cueNo == 2 && out[1].cuePath.find("gap.cue") != std::string::npos,
               "cueNo / cuePath 回填正确");
            ck(out[0].cueLen == want01, "轨 1 长度 = 下一轨起点 - 本轨起点", std::to_string(out[0].cueLen));
            ck(sh.tracks[1].index0 == (9LL * 60 + 30) * 75 + 22, "INDEX 00 仍被记录（可查，不参与偏移）");
        }
    }

    // =====================================================================
    hdr(6, "TIME mm:ss:ff 换算边界：1 帧 = 588@44.1k / 640@48k、四舍五入、越界拒绝");
    // =====================================================================
    {
        long long f = -1;
        ck(rta::meta::cue_parse_mmssff("00:00:00", f) && f == 0, "00:00:00 → 0 帧");
        ck(rta::meta::cue_frames_to_samples(0, 44100) == 0, "0 帧 → 0 采样");
        ck(rta::meta::cue_parse_mmssff("00:00:01", f) && f == 1, "00:00:01 → 1 帧");
        ck(rta::meta::cue_frames_to_samples(1, 44100) == 588, "1 帧 @44100 = 588 采样（44.1k/75 整除）",
           cue_samples(1, 44100));
        ck(rta::meta::cue_frames_to_samples(1, 48000) == 640, "1 帧 @48000 = 640 采样", cue_samples(1, 48000));
        ck(rta::meta::cue_frames_to_samples(1, 96000) == 1280, "1 帧 @96000 = 1280 采样", cue_samples(1, 96000));
        ck(rta::meta::cue_frames_to_samples(2, 1000) == 27,
           "2 帧 @1000Hz = 26.67 → 27（四舍五入，截断会得 26）", cue_samples(2, 1000));
        ck(rta::meta::cue_parse_mmssff("01:00:00", f) && f == 4500, "01:00:00 → 4500 帧");
        ck(rta::meta::cue_frames_to_samples(4500, 44100) == 44100LL * 60, "01:00:00 @44100 = 60 秒整",
           cue_samples(4500, 44100));
        ck(rta::meta::cue_parse_mmssff("99:59:74", f) && f == 449999, "99:59:74 是合法上界（449999 帧）",
           std::to_string(f));
        ck(rta::meta::cue_frames_to_samples(445499, 44100) == 445499LL * 588,
           "上界换算 = 帧数 × 588（整数精确）");
        ck(!rta::meta::cue_parse_mmssff("00:00:75", f), "ff=75 越界 → 拒绝（75 帧已是下一秒）");
        ck(!rta::meta::cue_parse_mmssff("00:60:00", f), "ss=60 越界 → 拒绝");
        ck(!rta::meta::cue_parse_mmssff("00:00", f), "缺少 ff 段 → 拒绝");
        ck(!rta::meta::cue_parse_mmssff("abc", f), "非时间串 → 拒绝");
        ck(rta::meta::cue_parse_mmssff("00:00:74", f) && f == 74, "ff=74 是合法上界");
        ck(rta::meta::cue_frames_to_samples(-1, 44100) == -1, "负帧 → -1（偏移未知）");
        ck(rta::meta::cue_frames_to_samples(10, 0) == -1, "采样率 0 → -1（偏移未知）");
    }

    // =====================================================================
    hdr(7, "多 TRACK（12 轨）：顺序、逐轨偏移、专辑总时长 = 整轨时长（不是 N 倍）");
    // =====================================================================
    {
        std::string cue = "TITLE \"Long\"\nPERFORMER \"Band\"\nFILE \"long.wav\" WAVE\n";
        for (int i = 1; i <= 12; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "  TRACK %02d AUDIO\n    TITLE \"T%02d\"\n    INDEX 01 %02d:00:00\n",
                          i, i, (i - 1) * 3);
            cue += buf;
        }
        const CueSheet sh = rta::meta::cue_parse_text(cue, (tmp / "long.cue").string());
        std::map<std::string, rta::net::LibTrack> probes;
        probes.emplace(sh.tracks[0].file, mkprobe(sh.tracks[0].file, 44100, 2160.0));   // 36 分钟
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 12, "12 条虚拟轨", std::to_string(out.size()));
        if (out.size() == 12) {
            bool seqOk = true, offOk = true, lenOk = true;
            double total = 0.0;
            for (size_t i = 0; i < out.size(); ++i) {
                if (out[i].trackNo != (int)i + 1) seqOk = false;
                if (out[i].cueOff != (long long)i * 180 * 44100) offOk = false;
                if (out[i].cueLen != 180LL * 44100) lenOk = false;
                total += out[i].duration;
            }
            ck(seqOk, "trackNo 连续 1..12");
            ck(offOk, "每轨起点 = 3 分钟 × i × 44100", std::to_string(out[5].cueOff));
            ck(lenOk, "每轨长度 = 180 秒（末轨由文件总时长截住）", std::to_string(out[11].cueLen));
            ck(std::abs(total - 2160.0) < 0.001,
               "专辑总时长 = 2160 秒 = 整轨时长（不是 12×2160）", std::to_string(total));
        }
    }

    // =====================================================================
    hdr(8, "多 FILE cue：每轨带自己的文件，偏移各自从 0 计（选择「支持」而非拒绝）");
    // =====================================================================
    {
        const std::string cue =
            "TITLE \"Two Files\"\nPERFORMER \"P\"\n"
            "FILE \"a.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"A1\"\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    TITLE \"A2\"\n    INDEX 01 02:00:00\n"
            "FILE \"b.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"B1\"\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    TITLE \"B2\"\n    INDEX 01 02:00:00\n";
        const fs::path cuePath = tmp / "two.cue";
        write_bytes(cuePath, cue);
        const CueSheet sh = rta::meta::cue_parse_file(rta::meta::cue_path_utf8(cuePath));
        ck(sh.tracks.size() == 4, "4 条轨（两个 FILE 各 2 条）", std::to_string(sh.tracks.size()));
        std::map<std::string, rta::net::LibTrack> probes;
        probes.emplace(rta::meta::cue_resolve_path(sh.cuePath, "a.wav"), mkprobe(rta::meta::cue_resolve_path(sh.cuePath, "a.wav"), 44100, 300.0));
        probes.emplace(rta::meta::cue_resolve_path(sh.cuePath, "b.wav"), mkprobe(rta::meta::cue_resolve_path(sh.cuePath, "b.wav"), 44100, 240.0));
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 4, "4 条虚拟轨", std::to_string(out.size()));
        if (out.size() == 4) {
            ck(out[0].path != out[2].path, "前两轨属于 a.wav、后两轨属于 b.wav");
            ck(out[0].cueOff == 0 && out[2].cueOff == 0, "每个文件内偏移各自从 0 计");
            ck(out[0].cueLen == 120LL * 44100, "a.wav 轨 1 长度 = 120 秒", std::to_string(out[0].cueLen));
            ck(out[1].cueLen == 180LL * 44100, "a.wav 末轨长度 = 300-120 秒", std::to_string(out[1].cueLen));
            ck(out[3].cueLen == 120LL * 44100, "b.wav 末轨长度 = 240-120 秒", std::to_string(out[3].cueLen));
            ck(out[0].trackNo == 1 && out[1].trackNo == 2 && out[2].trackNo == 3 && out[3].trackNo == 4,
               "跨文件 trackNo 连续 1..4（同一张专辑里不重复）");
            ck(out[2].cueNo == 1 && out[3].cueNo == 2, "cueNo 保留 cue 里的原始 TRACK 号（可对回文本）");
        }
    }

    // =====================================================================
    hdr(9, "FILE 指向不存在的文件：只跳过那份文件的轨（降级，不丢其它文件）");
    // =====================================================================
    {
        const std::string cue =
            "TITLE \"Partial\"\nPERFORMER \"P\"\n"
            "FILE \"exists.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"E1\"\n    INDEX 01 00:00:00\n"
            "FILE \"missing.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"M1\"\n    INDEX 01 00:00:00\n";
        const fs::path cuePath = tmp / "partial.cue";
        write_bytes(cuePath, cue);
        const CueSheet sh = rta::meta::cue_parse_file(rta::meta::cue_path_utf8(cuePath));
        std::map<std::string, rta::net::LibTrack> probes;
        const std::string a = rta::meta::cue_resolve_path(sh.cuePath, "exists.wav");
        probes.emplace(a, mkprobe(a, 44100, 60.0));
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 1, "只展开存在的那个文件（1 条）", std::to_string(out.size()));
        ck(out.size() == 1 && out[0].title == "E1", "保留的轨来自存在的文件");
        // lib_cue_audio_exists：扫描「认领」整轨就靠这一条判据（不是靠扩展名）
        {
            const fs::path realWav = tmp / "exists.wav";
            write_bytes(realWav, "RIFF....");     // 内容不重要，这里只验存在性判定
            ck(rta::net::lib_cue_audio_exists(rta::meta::cue_path_utf8(realWav)),
               "lib_cue_audio_exists：真实存在的文件 → true");
            ck(!rta::net::lib_cue_audio_exists(rta::meta::cue_path_utf8(tmp / "missing.wav")),
               "lib_cue_audio_exists：不存在的文件 → false");
        }
        // 全部文件都不存在 → 展开为空（库侧因此退回「整轨曲目」，见 test_library.cpp）
        std::map<std::string, rta::net::LibTrack> none;
        ck(rta::net::lib_cue_expand(sh, none).empty(), "文件全不存在 → 展开为空（不产生幽灵曲目）");
    }

    // =====================================================================
    hdr(10, "大小写/缩进/CRLF/无引号 FILE/相对路径含 ..：解析结果一致");
    // =====================================================================
    {
        const std::string cueLower =
            "title \"Case\"\nperformer \"P\"\n"
            "file album.flac wave\n"
            "  track 01 audio\n    title \"One\"\n    index 01 00:00:00\n"
            "  track 02 audio\n    title \"Two\"\n    index 01 01:00:00\n";
        const CueSheet sh1 = rta::meta::cue_parse_text(cueLower, (tmp / "case.cue").string());
        ck(sh1.albumTitle == "Case" && sh1.tracks.size() == 2, "小写指令 + 无引号 FILE 可解析",
           std::to_string(sh1.tracks.size()));
        ck(sh1.tracks.size() == 2 && sh1.tracks[1].title == "Two", "小写 track/title/index 生效");
        ck(sh1.tracks.size() == 2 && fs::path(rta::meta::cue_u8path(sh1.tracks[0].file)).filename() == "album.flac",
           "无引号 FILE 名解析正确", sh1.tracks.empty() ? "" : sh1.tracks[0].file);

        // 相对路径 sub/../album.wav 归一到 cue 同目录
        const CueSheet sh2 = rta::meta::cue_parse_text(
            "FILE \"sub/../album.wav\" WAVE\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n",
            (tmp / "norm.cue").string());
        ck(!sh2.tracks.empty() && fs::path(rta::meta::cue_u8path(sh2.tracks[0].file)) == (tmp / "album.wav"),
           "FILE 相对路径里的 sub/.. 被归一化", sh2.tracks.empty() ? "" : sh2.tracks[0].file);

        // 制表符缩进 + 大量空白
        const CueSheet sh3 = rta::meta::cue_parse_text(
            "\tTITLE\t\"Tabbed\"\t\n\tFILE \"t.wav\" WAVE\n\tTRACK 01 AUDIO\n\tINDEX 01 00:00:00\n",
            (tmp / "tab.cue").string());
        ck(sh3.albumTitle == "Tabbed" && sh3.tracks.size() == 1, "制表符分隔/缩进可解析", sh3.albumTitle);
    }

    // =====================================================================
    hdr(11, "非 AUDIO 轨被跳过；PREGAP/POSTGAP 记录但不改偏移");
    // =====================================================================
    {
        const std::string cue =
            "TITLE \"Mixed\"\nPERFORMER \"P\"\nFILE \"mixed.bin\" BINARY\n"
            "  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    TITLE \"Song\"\n    PREGAP 00:02:00\n    INDEX 01 00:03:00\n"
            "    POSTGAP 00:01:00\n";
        const CueSheet sh = rta::meta::cue_parse_text(cue, (tmp / "mixed.cue").string());
        ck(sh.tracks.size() == 2, "解析保留 2 条轨（含数据轨）", std::to_string(sh.tracks.size()));
        ck(sh.tracks.size() == 2 && !sh.tracks[0].audio && sh.tracks[0].type == "MODE1/2352",
           "数据轨标记为 audio=false", sh.tracks.empty() ? "" : sh.tracks[0].type);
        ck(sh.tracks.size() == 2 && sh.tracks[1].audio, "音频轨 audio=true");
        ck(sh.tracks.size() == 2 && sh.tracks[1].pregap == 150, "PREGAP 00:02:00 = 150 帧（被记录）",
           sh.tracks.size() > 1 ? std::to_string(sh.tracks[1].pregap) : "");
        ck(sh.tracks.size() == 2 && sh.tracks[1].postgap == 75, "POSTGAP 00:01:00 = 75 帧（被记录）",
           sh.tracks.size() > 1 ? std::to_string(sh.tracks[1].postgap) : "");
        std::map<std::string, rta::net::LibTrack> probes;
        probes.emplace(sh.tracks[1].file, mkprobe(sh.tracks[1].file, 44100, 120.0));
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 1, "展开时数据轨被跳过（只 1 条音频轨）", std::to_string(out.size()));
        ck(out.size() == 1 && out[0].cueOff == 3LL * 44100,
           "PREGAP 不改偏移：起点仍是 INDEX 01 = 00:03:00", std::to_string(out.empty() ? -1 : out[0].cueOff));
    }

    // =====================================================================
    hdr(12, "坏输入不崩：无 FILE / 空文本 / 只有数据轨 / INDEX 时间越界");
    // =====================================================================
    {
        const CueSheet a = rta::meta::cue_parse_text("TRACK 01 AUDIO\nINDEX 01 00:00:00\n", (tmp / "nofile.cue").string());
        ck(a.tracks.empty(), "TRACK 出现在 FILE 之前 → 丢弃该轨（不产生无文件的曲目）");
        ck(!a.errors.empty(), "并记录结构错误", a.firstError());

        const CueSheet b = rta::meta::cue_parse_text("", (tmp / "empty.cue").string());
        ck(b.tracks.empty() && !b.errors.empty(), "空文本 → 0 轨 + 明确错误", b.firstError());

        const CueSheet c = rta::meta::cue_parse_text(
            "FILE \"d.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n", (tmp / "data.cue").string());
        ck(c.tracks.size() == 1 && !c.tracks[0].audio, "只有数据轨时解析成功但 audio=false");
        std::map<std::string, rta::net::LibTrack> probes;
        probes.emplace(c.tracks[0].file, mkprobe(c.tracks[0].file, 44100, 60.0));
        ck(rta::net::lib_cue_expand(c, probes).empty(), "只有数据轨 → 展开为空（库侧退回整轨）");

        const CueSheet d = rta::meta::cue_parse_text(
            "FILE \"x.wav\" WAVE\n TRACK 01 AUDIO\n INDEX 01 00:00:75\n", (tmp / "badtime.cue").string());
        ck(d.tracks.empty() && !d.errors.empty(), "INDEX 时间越界（ff=75）→ 该轨被丢弃并记错误",
           d.firstError());

        // 只有 INDEX 00（没有 INDEX 01）的方言：**降级**用 INDEX 00 当起点，并留一条警告 ——
        // 直接丢弃会让「整张专辑少一首」，而降级至少能播（起点可能含前导间隙）。
        const CueSheet e = rta::meta::cue_parse_text(
            "FILE \"y.wav\" WAVE\n TRACK 01 AUDIO\n INDEX 00 00:02:00\n", (tmp / "only00.cue").string());
        ck(e.tracks.size() == 1 && e.tracks[0].index1 == 150 && e.tracks[0].index1FromIndex0,
           "只有 INDEX 00 → 降级当起点（150 帧 = 2 秒）并打标记",
           e.tracks.empty() ? "" : std::to_string(e.tracks[0].index1));
        ck(!e.warnings.empty(), "降级处理留下警告（不静默）", e.warnings.empty() ? "" : e.warnings.front());
    }

    // =====================================================================
    hdr(13, "真实中文 cue（EAC 风格，含 DISCID）：逐字段 + 偏移换算");
    // =====================================================================
    {
        // 直接写 UTF-8 中文字面量（本仓源码是 UTF-8，CMake 给 MSVC 加了 /utf-8）。
        // 早先版本用 \xE4\xB8\xAD 这种手写字节，结果自己的期望值和字节表对不上（引子/弦子、
        // 闵惠明/闵惠芬），白丢了两条判据 —— 这也是「判据本身要可以被检验」的一例。
        const std::string zh =
            "REM GENRE \"民乐\"\r\n"
            "REM DATE 2003\r\n"
            "REM DISCID 8A0F0B0A\r\n"
            "PERFORMER \"中国广播民族乐团\"\r\n"
            "TITLE \"梁祝·黄河颂（原声复刻）\"\r\n"
            "FILE \"梁祝.wav\" WAVE\r\n"
            "  TRACK 01 AUDIO\r\n"
            "    TITLE \"第一章 引子与弦乐\"\r\n"
            "    PERFORMER \"闵惠芬\"\r\n"
            "    INDEX 00 00:00:00\r\n"
            "    INDEX 01 00:00:00\r\n"
            "  TRACK 02 AUDIO\r\n"
            "    TITLE \"第二章 二泉映月\"\r\n"
            "    INDEX 01 12:34:56\r\n";
        const fs::path p = tmp / "eac_zh.cue";
        write_bytes(p, zh);
        const CueSheet sh = rta::meta::cue_parse_file(rta::meta::cue_path_utf8(p));
        ck(sh.genre == "民乐", "REM GENRE 中文", sh.genre);
        ck(sh.date == "2003", "REM DATE", sh.date);
        ck(sh.discId == "8A0F0B0A", "REM DISCID 记录", sh.discId);
        ck(sh.albumTitle == "梁祝·黄河颂（原声复刻）", "中文专辑名逐字相等", sh.albumTitle);
        ck(sh.performer == "中国广播民族乐团", "中文专辑艺术家逐字相等", sh.performer);
        ck(sh.tracks.size() == 2, "2 条轨", std::to_string(sh.tracks.size()));
        if (sh.tracks.size() == 2) {
            ck(sh.tracks[0].title == "第一章 引子与弦乐", "轨 1 中文标题", sh.tracks[0].title);
            ck(sh.tracks[0].performer == "闵惠芬", "轨 1 中文艺术家", sh.tracks[0].performer);
            ck(sh.tracks[1].title == "第二章 二泉映月", "轨 2 中文标题", sh.tracks[1].title);
            ck(sh.tracks[1].performer.empty(), "轨 2 没有 PERFORMER（保持空）");
            const long long wantF = (12LL * 60 + 34) * 75 + 56;
            ck(sh.tracks[1].index1 == wantF, "12:34:56 帧数正确", std::to_string(sh.tracks[1].index1));
            std::map<std::string, rta::net::LibTrack> probes;
            probes.emplace(sh.tracks[0].file, mkprobe(sh.tracks[0].file, 44100, 1800.0));
            const auto out = rta::net::lib_cue_expand(sh, probes);
            ck(out.size() == 2 && out[1].cueOff == rta::meta::cue_frames_to_samples(wantF, 44100),
               "轨 2 采样偏移 = 12:34:56 @44100", out.size() > 1 ? std::to_string(out[1].cueOff) : "");
            ck(out.size() == 2 && out[1].artist == "中国广播民族乐团",
               "轨 2 缺 PERFORMER → 艺术家回退到专辑 PERFORMER", out.size() > 1 ? out[1].artist : "");
        }
    }

    // =====================================================================
    hdr(14, "虚拟轨字段透传：采样率/位深/声道/封面/mtime/size 来自物理文件，专辑名来自 cue");
    // =====================================================================
    {
        const std::string cue =
            "TITLE \"CueAlbum\"\nPERFORMER \"CueArtist\"\nREM GENRE \"Rock\"\nREM DATE 1999\n"
            "FILE \"x.flac\" WAVE\n TRACK 01 AUDIO\n TITLE \"T1\"\n INDEX 01 00:00:00\n";
        const fs::path cuePath = tmp / "fields.cue";
        write_bytes(cuePath, cue);
        const CueSheet sh = rta::meta::cue_parse_file(rta::meta::cue_path_utf8(cuePath));
        std::map<std::string, rta::net::LibTrack> probes;
        rta::net::LibTrack probe = mkprobe(sh.tracks[0].file, 96000, 100.0, 24, 2, 1);
        probe.album = "ffprobe 的专辑标签";      // 应被 cue 的 TITLE 覆盖
        probe.artist = "ffprobe 的艺术家标签";
        probe.genre = "Pop";                     // 应被 REM GENRE 覆盖
        probe.year = 2000;                       // 应被 REM DATE 覆盖
        probes.emplace(sh.tracks[0].file, probe);
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 1, "1 条虚拟轨", std::to_string(out.size()));
        if (out.size() == 1) {
            const auto& t = out[0];
            ck(t.sampleRate == 96000 && t.bits == 24 && t.channels == 2, "采样率/位深/声道透传");
            ck(t.hasCover == 1 && t.mtime == 111 && t.size == 222, "封面标记/mtime/size 透传");
            ck(t.album == "CueAlbum", "专辑名用 cue 的 TITLE（覆盖 ffprobe 标签）", t.album);
            ck(t.artist == "CueArtist" && t.albumArtist == "CueArtist", "艺术家用 cue 的 PERFORMER");
            ck(t.genre == "Rock" && t.year == 1999, "genre/year 用 REM 字段（覆盖探测值）",
               t.genre + "/" + std::to_string(t.year));
            ck(t.isCueTrack() && t.cueNo == 1 && t.cueOff == 0 && t.cueLen == 96000LL * 100,
               "isCueTrack / cueNo / cueOff / cueLen 就位");
            double s = -1, e = -1;
            ck(rta::net::lib_cue_range_sec(t, s, e) && s == 0.0 && std::abs(e - 100.0) < 1e-9,
               "lib_cue_range_sec 给出 0..100 秒", std::to_string(s) + ".." + std::to_string(e));
        }
    }

    // =====================================================================
    hdr(15, "没有 ffprobe（探测为空）时的降级：偏移未知但曲名/专辑/时长仍可用");
    // =====================================================================
    {
        const std::string cue =
            "TITLE \"NoProbe\"\nPERFORMER \"P\"\nFILE \"np.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"N1\"\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    TITLE \"N2\"\n    INDEX 01 01:00:00\n";
        const CueSheet sh = rta::meta::cue_parse_text(cue, (tmp / "np.cue").string());
        std::map<std::string, rta::net::LibTrack> probes;
        rta::net::LibTrack empty;                 // 探测失败时 lib_probe_one 返回的就是这个（sr=0/dur=0）
        empty.path = sh.tracks[0].file;
        probes.emplace(sh.tracks[0].file, empty);
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 2, "仍展开出 2 条轨（不因探测失败而丢轨）", std::to_string(out.size()));
        if (out.size() == 2) {
            ck(out[0].cueOff == -1, "采样率未知 → cueOff = -1（偏移未知，如实标记）",
               std::to_string(out[0].cueOff));
            ck(out[0].title == "N1" && out[0].album == "NoProbe", "曲名/专辑名不受影响");
            ck(std::abs(out[0].duration - 60.0) < 1e-9,
               "时长退回 cue 自带的差值 60 秒", std::to_string(out[0].duration));
            ck(out[1].duration == 0.0, "末轨且文件总时长未知 → 0（如实标记未知，不编数字）",
               std::to_string(out[1].duration));
            double s = 0, e = 0;
            ck(!rta::net::lib_cue_range_sec(out[0], s, e), "lib_cue_range_sec 返回 false（播放侧据此走整轨）");
        }
        // .cue 扩展名识别（大小写不敏感；.cue.txt 不算）
        ck(rta::net::lib_is_cue(fs::path("a.cue")) && rta::net::lib_is_cue(fs::path("A.CUE")),
           "lib_is_cue 大小写不敏感");
        ck(!rta::net::lib_is_cue(fs::path("a.cue.txt")) && !rta::net::lib_is_cue(fs::path("a.flac")),
           "lib_is_cue 不误判 .cue.txt / .flac");
    }

    // =====================================================================
    hdr(16, "cue 的 FILE 与磁盘真名大小写不一致（ALBUM.WAV vs album.wav）：仍能展开，path 用真名");
    // =====================================================================
    {
        const std::string cue =
            "TITLE \"Case\"\nPERFORMER \"P\"\nFILE \"ALBUM.WAV\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"c1\"\n    INDEX 01 00:00:00\n";
        const fs::path cuePath = tmp / "case16.cue";
        write_bytes(cuePath, cue);
        const CueSheet sh = rta::meta::cue_parse_file(rta::meta::cue_path_utf8(cuePath));
        ck(sh.tracks.size() == 1, "解析出 1 条轨", std::to_string(sh.tracks.size()));
        // 探测表里的键是**磁盘真名**（小写）
        const fs::path real = tmp / "album.wav";
        std::map<std::string, rta::net::LibTrack> probes;
        probes.emplace(rta::meta::cue_path_utf8(real), mkprobe(rta::meta::cue_path_utf8(real), 44100, 10.0));
        const auto out = rta::net::lib_cue_expand(sh, probes);
        ck(out.size() == 1, "大小写不一致仍能展开（Windows 文件系统不敏感）", std::to_string(out.size()));
        if (out.size() == 1) {
            ck(out[0].path == rta::meta::cue_path_utf8(real),
               "曲目 path = 磁盘真名（ALBUM.WAV 不会带进库/播放）", out[0].path);
            ck(out[0].cueLen == 441000, "长度按文件总时长 10 秒", std::to_string(out[0].cueLen));
            ck(rta::net::lib_path_key("D:\\A\\B.WAV") == rta::net::lib_path_key("d:\\a\\b.wav"),
               "lib_path_key 归一大小写");
        }
    }

    fs::remove_all(tmp, ec);
    std::printf("\n结果：%s（失败 %d 项）\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail ? 1 : 0;
}
