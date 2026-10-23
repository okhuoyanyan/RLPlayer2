// test_media_duration.cpp — probe_media 的时长字段（2026-10-22 跨模块缺陷的回归守卫）
//
// 缺陷：src/audio/media_info.h 的 MediaInfo::duration 从未被赋值 —— ffprobe 只问了
// format_tags，没问 format=duration。后果链：
//   ① enrich.h 的 fileSec 恒 0
//   ② match.h 的 mf_duration 因 fileSec<=0 恒走「缺失 → 0.5 中性」
//   ③ 于是 docs/元数据补全调研与规划.md §3 写的「时长差 ±3s 强信号」从 P1 起从未生效
// 四源 + 未来 P3 资料层全部受影响。
//
// 判据设计（铁律 3）——「能证明以前是假的、现在是真的」：
//   * 已知时长的合成 WAV（44.1kHz / 2ch / 16bit / 帧数 N）：理论值 N/44100 由测试自己算，
//     不依赖 ffprobe 的四舍五入。断言 probe_media().duration 与理论值差 < 0.05s。
//     缺陷态（参数里没有 format=duration）下这里是 0 → 必 FAIL（反向验证见报告）。
//   * 端到端：拿真实 171.69s 的 FLAC 的 fileSec 喂给 rta::meta::match_detail::mf_duration，
//     打印「fileSec=0（旧）→ 0.5 中性 / fileSec=172（新）→ 1.0 实分」对照表 —— 证明
//     缺陷真实存在、修复真的把强信号接通了，而不只是「字段有值了」。
//
// 为什么单独一个文件而不并入 test_hotfix_guard.cpp：
//   test_hotfix_guard.cpp 的头注明确承诺「全部离线：不打开音频设备、不依赖 ffmpeg」，
//   它的合法 WAV 由 write_wav 生成、被测代码不碰外部工具。本测试的被测对象恰恰是
//   ffprobe 的调用与输出解析，必然依赖环境里的 ffprobe（与 test_writeback.cpp 同类，
//   属「工具缺失即失败」的环境门禁，不跳过）。把工具依赖塞进 hotfix_guard 会破坏它
//   已写明的离线契约，也让「hotfix_guard 红了」的含义变模糊。
//
// ⚠️ 未解决项（铁律 4）：
//   * 真实 FLAC 用例依赖开发机上的素材路径（可用环境变量 RTA_DURATION_FLAC 覆盖）。
//     素材不存在时该段打印 SKIP 并把「素材缺失」计入 note，不判失败 —— 否则这个
//     测试在别的机器上会因为「没有那首歌」而红，那是被判据误伤，不是缺陷。
//     ⚠️ 因此「真实 FLAC 那一段」在无素材的机器上等于未验证；回归基线以合成 WAV +
//     mf_duration 对照表为准（这两段恒执行、恒断言）。
//   * DSD（.dsf/.dff）时长口径（E-16，2026-10-23 已解决）：
//     优先用 dsd_reader 解析 header 真实样本数（DSF sampleCount / DFF dataSize），
//     不再依赖 ffprobe 的文件实际长度口径。含篡改 header 对照测试与容错断言。
// 抑制 CRT 的 getenv 弃用警告与 C++20 u8path 弃用警告（测试里都用得到，且语义正确）
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#endif
#include "audio/media_info.h"
#include "meta/match.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using rta::audio::MediaInfo;
using rta::audio::probe_media;

static int g_fail = 0;
static void ck(bool cond, const std::string& what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what.c_str());
    else { std::printf("  [FAIL] %s  %s\n", what.c_str(), detail.c_str()); ++g_fail; }
}

// UTF-8 字节串 → 文件系统路径（恒按 UTF-8 解释，不受本机 ACP=936 影响）
static std::filesystem::path u8p(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// ---------------- 临时文件 ----------------
static std::string temp_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec);
    if (ec || p.empty()) p = fs::path(".");
    return p.string();
}
static std::string temp_file(const char* name) {
    namespace fs = std::filesystem;
    std::string p = (fs::path(temp_dir()) / name).string();
    return p;
}
static bool write_bytes(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(u8p(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!b.empty()) f.write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
    f.close();
    return true;
}
static std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream f(u8p(path), std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> b(sz);
    if (sz > 0) f.read(reinterpret_cast<char*>(b.data()), (std::streamsize)sz);
    return b;
}
static void remove_file(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(u8p(path), ec);
}

// ---------------- 合成素材 ----------------
// 已知帧数的 WAV：用仓库既有的 write_wav（不自己重写头，避免测试与实现同时错）
static bool make_wav(const std::string& path, uint32_t rate, int channels, int bits, size_t frames) {
    rta::audio::AudioBuffer a;
    a.rate = rate; a.channels = channels; a.bits = bits; a.isFloat = false;
    a.raw.assign(frames * (size_t)channels, 0);      // 全零样本（静音；不出声、不碰设备）
    try { rta::audio::write_wav(path, a); } catch (const std::exception&) { return false; }
    return true;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== probe_media 时长字段：format=duration 接线回归（2026-10-22）==\n");

    // ================= 0) 工具前置：本测试的被测对象就是 ffprobe 调用 =================
    std::printf("- 0 环境前置\n");
    {
        const std::string ff = rta::audio::find_tool("ffprobe");
        ck(!ff.empty(), "环境有 ffprobe（门禁前提，缺失即失败而不是跳过）",
           "find_tool(\"ffprobe\") 为空");
        if (ff.empty()) { std::printf("\n缺 ffprobe，门禁无法执行\n"); return 1; }
        std::printf("    ffprobe = %s\n", ff.c_str());
    }

    // ================= 1) 解析容错（纯函数，不碰 ffprobe） =================
    // 为什么单独测：duration 行在播放链路上被解析，「N/A → 抛异常」会让整进程死；
    // 把 NaN/inf 交给 match 的加权和会让所有候选分数变 NaN，比不取时长更坏。
    std::printf("- 1 duration 文本解析容错（N/A / 空 / 非数字 / 负 / NaN / inf）\n");
    {
        auto eq = [&](const char* text, double want, const char* what) {
            const double got = rta::audio::probe_parse_duration(text);
            ck(got == want, what,
               "probe_parse_duration(\"" + std::string(text) + "\") = " + std::to_string(got) +
               "，期望 " + std::to_string(want));
        };
        eq("N/A",          0.0, "字面 N/A（ffprobe 判不出时长的标准输出）→ 0");
        eq("",             0.0, "空串 → 0");
        eq("   ",          0.0, "全空白 → 0");
        eq("abc",          0.0, "非数字 → 0");
        eq("-3.5",         0.0, "负数 → 0");
        eq("0",            0.0, "0 → 0（0 即未知）");
        eq("nan",          0.0, "nan → 0");
        eq("inf",          0.0, "inf → 0");
        eq("1e400",        0.0, "1e400（strtod 溢出成 inf）→ 0");
        eq("171.6x",       0.0, "尾部残留（不可信值）→ 0");
        eq("171.692290", 171.692290, "正常值原样保留");
        eq("  12.5  \r\n", 12.5, "前后空白被容忍");
        // 最关键的一条：全程不许抛（std::stod 在 "N/A" 上会抛）
        bool threw = false;
        try { (void)rta::audio::probe_parse_duration("N/A"); } catch (...) { threw = true; }
        ck(!threw, "解析不抛异常（这是播放链路，抛 = 整进程死）");
    }

    // ================= 2) 已知时长的合成 WAV：duration 必须与理论值一致 =================
    // 判据不依赖 ffprobe 的四舍五入：理论值 = frames / rate，由测试自己算。
    std::printf("- 2 已知时长合成素材（44.1kHz / 2ch / 16bit）\n");
    const uint32_t kRate = 44100;
    const size_t   kFrames = 155337;                       // 刻意不是整数秒，避免「整秒巧合」蒙对
    const double   kTheory = (double)kFrames / (double)kRate;
    const std::string wav = temp_file("rta_dur_known.wav");
    MediaInfo known;
    {
        ck(make_wav(wav, kRate, 2, 16, kFrames), "合成 WAV 落盘（write_wav，%TEMP%）",
           "写 "+wav+" 失败");
        known = probe_media(wav);
        std::printf("    理论时长 = %zu / %u = %.6f s；probe_media().duration = %.6f s\n",
                    kFrames, kRate, kTheory, known.duration);
        // 缺陷态下这里 duration==0 → |0-theory| = 3.52 > 0.05 → FAIL（反向验证会贴出来）
        ck(std::fabs(known.duration - kTheory) < 0.05,
           "duration 与理论值误差 < 0.05s（缺陷态是 0，必 FAIL）",
           "duration=" + std::to_string(known.duration) + " theory=" + std::to_string(kTheory));
        ck(known.duration > 0.0, "duration > 0（不再是「字段恒 0」）",
           "duration=" + std::to_string(known.duration));
        ck(std::isfinite(known.duration), "duration 是有限值（无 NaN/inf）");
        // 顺带确认标签没被 format=duration 这条改动带坏（同一个解析循环）
        ck(known.sampleRate == (int)kRate && known.channels == 2,
           "同一命令里流级参数/标签解析未被破坏",
           "rate=" + std::to_string(known.sampleRate) + " ch=" + std::to_string(known.channels));
    }

    // ================= 3) 损坏 / 空 / 判不出时长的文件 → duration == 0，不 NaN、不抛 =================
    std::printf("- 3 损坏/空/无时长文件 → 0（不 NaN、不抛）\n");
    {
        auto expect_zero = [&](const char* name, const std::vector<uint8_t>& bytes, const char* what) {
            const std::string p = temp_file(name);
            ck(write_bytes(p, bytes), std::string("构造 ") + name, "写盘失败");
            bool threw = false;
            MediaInfo mi;
            try { mi = probe_media(p); } catch (...) { threw = true; }
            ck(!threw, std::string(what) + "：不抛异常");
            ck(mi.duration == 0.0, std::string(what) + "：duration == 0",
               "duration=" + std::to_string(mi.duration));
            ck(std::isfinite(mi.duration), std::string(what) + "：duration 有限");
            remove_file(p);
        };
        expect_zero("rta_dur_empty.wav", {}, "空文件（ffprobe 非 0 退出）");
        expect_zero("rta_dur_junk.bin", std::vector<uint8_t>(512, 0x5A), "垃圾字节（ffprobe 非 0 退出）");

        // 1x1 PNG：ffprobe 退出码 0，但输出字面量 "duration=N/A" —— 专测 N/A 分支
        // （空文件走的是「非 0 退出码」分支，测不到 N/A；两者都要覆盖）
        static const uint8_t png[] = {
            0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0,0,0,0x0D,0x49,0x48,0x44,0x52,
            0,0,0,1,0,0,0,1,8,6,0,0,0,0x1F,0x15,0xC4,0x89,0,0,0,0x0A,0x49,0x44,0x41,
            0x54,0x78,0x9C,0x63,0,1,0,0,5,0,1,0x0D,0x0A,0x2D,0xB4,0,0,0,0,0x49,
            0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
        };
        expect_zero("rta_dur_px.png", std::vector<uint8_t>(png, png + sizeof(png)),
                    "1x1 PNG（ffprobe 输出 duration=N/A）");

        // 0 帧的合法 WAV：同样走 N/A
        const std::string zeroWav = temp_file("rta_dur_zero.wav");
        if (make_wav(zeroWav, 44100, 2, 16, 0)) {
            MediaInfo mi;
            bool threw = false;
            try { mi = probe_media(zeroWav); } catch (...) { threw = true; }
            ck(!threw && mi.duration == 0.0, "0 帧合法 WAV：duration == 0 且不抛",
               "duration=" + std::to_string(mi.duration));
            remove_file(zeroWav);
        } else {
            ck(false, "0 帧合法 WAV：构造失败（write_wav 不接受空缓冲？）");
        }
        // 不存在的文件：不得抛（播放链路里路径可能刚被移走）
        {
            bool threw = false;
            MediaInfo mi;
            try { mi = probe_media(temp_file("rta_dur_no_such_file.flac")); } catch (...) { threw = true; }
            ck(!threw && mi.duration == 0.0, "不存在的文件：duration == 0 且不抛",
               "threw=" + std::to_string((int)threw));
        }
    }

    // ================= 4) enrich 侧派生：整秒转换（含溢出防御） =================
    std::printf("- 4 enrich 侧派生 (int)(duration+0.5) 与溢出防御\n");
    {
        using rta::audio::media_duration_sec;
        const int secTheory = (int)(kTheory + 0.5);      // 3.522... → 4
        ck(media_duration_sec(known.duration) == secTheory,
           "合成素材 → fileSec 等于理论整秒（四舍五入）",
           "got=" + std::to_string(media_duration_sec(known.duration)) +
           " want=" + std::to_string(secTheory));
        ck(media_duration_sec(171.692290) == 172, "171.692290 → 172（真实 FLAC 的值）",
           "got=" + std::to_string(media_duration_sec(171.692290)));
        ck(media_duration_sec(0.0) == 0 && media_duration_sec(-1.0) == 0,
           "0 / 负数 → 0（未知）");
        ck(media_duration_sec(std::nan("")) == 0, "NaN → 0");
        ck(media_duration_sec(HUGE_VAL) == 0 && media_duration_sec(-HUGE_VAL) == 0, "±inf → 0");
        ck(media_duration_sec(86400.0) == 86400, "恰好 24 小时 → 保留 86400（上限本身不算越界）",
           "got=" + std::to_string(media_duration_sec(86400.0)));
        ck(media_duration_sec(86401.0) == 0, "超过 24 小时 → 判为头损坏，退回 0（未知）",
           "got=" + std::to_string(media_duration_sec(86401.0)));
        // 这条是「溢出真的会发生」的证据：旧公式 (int)(d+0.5) 在 d+0.5 > INT_MAX 时是
        // 未定义行为（本机 clang/x86-64 实测 1e10 → -4）。测试不重演 UB，只断言新转换
        // 在这段区间给 0，并打印旧公式的越界区间。
        ck((double)2147483647 > kTheory, "理论值远在 int 可表示范围内（正常素材不受上限影响）");
        ck(media_duration_sec(1e10) == 0,
           "1e10 秒（> INT_MAX 秒）→ 0，不产生负数",
           "got=" + std::to_string(media_duration_sec(1e10)));
        std::printf("    旧公式越界区间: d+0.5 > 2147483647 ⇔ d > 2147483646.5 秒（≈68 年）；"
                    "新转换在此区间返回 0 = 未知\n");
    }

    // ================= 5) 端到端证据：时长强信号真的接通了 =================
    // 这是本缺陷的核心判据：不是「字段有值」，而是「match 的分数从恒中性变成实分」。
    std::printf("- 5 端到端：mf_duration / match_score_detail 的旧/新对照\n");
    {
        using namespace rta::meta;
        // 真实文件 171.692290s → 旧 fileSec=0 / 新 fileSec=172；候选按 171s 报（常见平台口径）
        const int candMs = 171000;
        const int oldSec = 0;                       // 缺陷态：probe_media 不取时长 → enrich 恒 0
        const int newSec = rta::audio::media_duration_sec(171.692290);   // = 172

        const match_detail::mf_tagged dOld = match_detail::mf_duration(oldSec, candMs);
        const match_detail::mf_tagged dNew = match_detail::mf_duration(newSec, candMs);
        std::printf("    | fileSec | candMs | durScore | dur tag | 语义                 |\n");
        std::printf("    | %7d | %6d | %8.2f | %-7s | 缺失 → 中性降级      |\n",
                    oldSec, candMs, dOld.score, dOld.tag.c_str());
        std::printf("    | %7d | %6d | %8.2f | %-7s | 差 1s → 强信号命中   |\n",
                    newSec, candMs, dNew.score, dNew.tag.c_str());

        ck(oldSec == 0, "缺陷态 fileSec == 0（旧行为）");
        ck(dOld.score == 0.5 && dOld.tag == "na", "旧：fileSec=0 → 0.5 中性 / tag=na",
           "score=" + std::to_string(dOld.score) + " tag=" + dOld.tag);
        ck(newSec == 172, "新：171.692290s → fileSec=172",
           "got=" + std::to_string(newSec));
        ck(dNew.score == 1.0 && dNew.tag == "ok:1s", "新：|172000-171000|=1000ms ≤ 3s → 1.0 / tag=ok:1s",
           "score=" + std::to_string(dNew.score) + " tag=" + dNew.tag);
        ck(dNew.score > dOld.score, "强信号确实从「中性」变成「命中」（这就是缺陷的实体）");

        // 错候选对照：差 > 10s 时新口径直接判 far=0，中性则放过 —— 说明修好之后既
        // 会加分、也会减分（这才是有判别力的信号）
        const int wrongMs = 200000;                 // 200s，与 172s 差 28s
        const match_detail::mf_tagged wOld = match_detail::mf_duration(oldSec, wrongMs);
        const match_detail::mf_tagged wNew = match_detail::mf_duration(newSec, wrongMs);
        std::printf("    | %7d | %6d | %8.2f | %-7s | 错候选（差 28s）中性放过 |\n",
                    oldSec, wrongMs, wOld.score, wOld.tag.c_str());
        std::printf("    | %7d | %6d | %8.2f | %-7s | 错候选被时长否掉          |\n",
                    newSec, wrongMs, wNew.score, wNew.tag.c_str());
        ck(wOld.score == 0.5 && wNew.score == 0.0 && wNew.tag == "far",
           "新口径会把错候选判 far=0（旧口径恒 0.5，无判别力）",
           "old=" + std::to_string(wOld.score) + " new=" + std::to_string(wNew.score) +
           " tag=" + wNew.tag);

        // 总分层面（0.55*title + 0.30*artist + 0.15*duration）：title/artist 都对时，
        // 时长信号把总分从 0.925 抬到 1.000
        Candidate c;
        c.title = "T"; c.artist = "A"; c.album = ""; c.durationMs = candMs;
        const Scored so = match_score_detail("T", "A", "", oldSec, c);
        const Scored sn = match_score_detail("T", "A", "", newSec, c);
        std::printf("    match_score_detail: 旧 total=%.3f (%s) / 新 total=%.3f (%s)\n",
                    so.score, so.why.c_str(), sn.score, sn.why.c_str());
        ck(so.score == 0.925 && sn.score == 1.0,
           "总分 0.55+0.30+0.15：旧 0.925 → 新 1.000",
           "old=" + std::to_string(so.score) + " new=" + std::to_string(sn.score));
        ck(so.why.find("dur:na") != std::string::npos && sn.why.find("dur:ok:") != std::string::npos,
           "why 串如实反映 dur 分项（na → ok:1s）", sn.why);
    }

    // ================= 6) 真实 FLAC（素材存在时） =================
    // 路径用 UTF-8 字节拼（不受编译器 source/execution charset 影响）；可用环境变量覆盖。
    std::printf("- 6 真实 FLAC 时长（ffprobe 端到端；素材缺失则如实标注未验证）\n");
    {
        // D:\RTA-hqplayer\MUSIC\专辑-原创&同人\22号咖啡馆的平安夜 @ 熊青嵐\1 姜饼游行.flac
        const std::string u8path = std::string(
            "\x44\x3A\x5C\x52\x54\x41\x2D\x68\x71\x70\x6C\x61\x79\x65\x72\x5C\x4D\x55\x53\x49\x43"
            "\x5C\xE4\xB8\x93\xE8\xBE\x91\x2D\xE5\x8E\x9F\xE5\x88\x9B\x26\xE5\x90\x8C\xE4\xBA\xBA"
            "\x5C\x32\x32\xE5\x8F\xB7\xE5\x92\x96\xE5\x95\xA1\xE9\xA6\x86\xE7\x9A\x84\xE5\xB9\xB3"
            "\xE5\xAE\x89\xE5\xA4\x9C\x20\x40\x20\xE7\x86\x8A\xE9\x9D\x92\xE5\xB5\x90\x5C\x31\x20"
            "\xE5\xA7\x9C\xE9\xA5\xBC\xE6\xB8\xB8\xE8\xA1\x8C\x2E\x66\x6C\x61\x63");
        std::string path = u8path;
        if (const char* env = std::getenv("RTA_DURATION_FLAC")) path = env;

        std::error_code ec;
        const bool exists = std::filesystem::exists(u8p(path), ec) && !ec;
        if (!exists) {
            std::printf("    [SKIP] 素材不存在，此段未验证（铁律 4 如实标注）：%s\n", path.c_str());
            std::printf("    可用环境变量 RTA_DURATION_FLAC=<flac 路径> 指定素材后复跑。\n");
        } else {
            const MediaInfo mi = probe_media(path);
            std::printf("    真实 FLAC duration = %.6f s，fileSec = %d\n",
                        mi.duration, rta::audio::media_duration_sec(mi.duration));
            ck(mi.duration > 0.0, "真实 FLAC 能取到时长（旧代码这里是 0）",
               "duration=" + std::to_string(mi.duration));
            ck(std::fabs(mi.duration - 171.692290) < 1.0,
               "真实 FLAC 时长 ≈ 171.69s（ffprobe 实测 171.692290）",
               "duration=" + std::to_string(mi.duration));
            ck(rta::audio::media_duration_sec(mi.duration) == 172,
               "真实 FLAC → fileSec = 172（喂给 match 的就是这个值）",
               "fileSec=" + std::to_string(rta::audio::media_duration_sec(mi.duration)));
            // 标签没被这条改动带坏（同一个解析循环，回归守卫）
            ck(!mi.title.empty() && !mi.artist.empty() && !mi.album.empty(),
               "真实 FLAC 的 title/artist/album 照旧解析出来",
               "title=[" + mi.title + "] artist=[" + mi.artist + "] album=[" + mi.album + "]");
        }
    }

    // ================= 7) E-16：DSD 时长口径（真实 header 样本数） =================
    std::printf("- 7 DSD 时长口径（真实 header 样本数，E-16）\n");
    {
        const std::string ff = rta::audio::find_tool("ffprobe");
        auto read_ffprobe_raw_duration = [&](const std::string& path) -> double {
            if (ff.empty()) return 0.0;
            const std::wstring cmd = L"\"" + rta::audio::widen(ff) +
                L"\" -v error -show_entries format=duration -of default=nw=1 \"" +
                rta::audio::widen(path) + L"\"";
            std::string out;
            if (rta::audio::run_hidden(cmd, &out) == 0) {
                const size_t e = out.find('=');
                if (e != std::string::npos) {
                    return rta::audio::probe_parse_duration(out.substr(e + 1));
                }
            }
            return 0.0;
        };

        auto make_dsd_file = [&](const std::string& path, uint32_t rateDsd, const char* kind) -> bool {
            const std::string makeDsd = rta::audio::wide_to_utf8(rta::audio::exe_dir_w()) + "\\make_dsd.exe";
            const std::wstring cmd = L"\"" + rta::audio::widen(makeDsd) + L"\" --out \"" +
                rta::audio::widen(path) + L"\" --rate " + std::to_wstring(rateDsd) +
                L" --seconds 1 --kind " + rta::audio::widen(kind);
            if (rta::audio::run_hidden(cmd) == 0 && std::filesystem::exists(u8p(path))) {
                return true;
            }
            // 回退直接写
            const uint32_t rateHz = rateDsd * 44100;
            const uint64_t bits = (uint64_t)rateHz;
            rta::audio::DsdStream s;
            s.rate = rateHz;
            s.channels = 2;
            s.bits = bits;
            s.container = kind;
            s.data.assign(2, std::vector<uint8_t>((size_t)((bits + 7) / 8), 0x55));
            try {
                if (std::string(kind) == "dsf") rta::audio::write_dsf(path, s);
                else rta::audio::write_dff(path, s);
                return true;
            } catch (...) { return false; }
        };

        // 7.1 DSD64 / 128 / 256 各 1 秒：duration 误差 < 5ms
        std::printf("  7.1 DSF/DFF 的 DSD64/128/256 各 1s：误差 < 5ms\n");
        for (const char* kind : { "dsf", "dff" }) {
            for (uint32_t r : { 64u, 128u, 256u }) {
                const std::string p = temp_file(("rta_dur_" + std::string(kind) + "_" + std::to_string(r) + "." + kind).c_str());
                ck(make_dsd_file(p, r, kind), std::string("生成 1s DSD") + std::to_string(r) + " (" + kind + ")");
                const MediaInfo mi = probe_media(p);
                const double errMs = std::fabs(mi.duration - 1.0) * 1000.0;
                std::printf("      [%s] DSD%u: duration = %.6f s (误差 %.3f ms), rate = %d, ch = %d, bits = %d\n",
                            kind, r, mi.duration, errMs, mi.sampleRate, mi.channels, mi.bits);
                ck(errMs < 5.0, std::string(kind) + " DSD" + std::to_string(r) + "：duration 误差 < 5ms",
                   "duration=" + std::to_string(mi.duration) + " errMs=" + std::to_string(errMs));
                ck(mi.sampleRate == (int)(r * 44100) && mi.channels == 2 && mi.bits == 1,
                   std::string(kind) + " DSD" + std::to_string(r) + "：流参数正确");
                ck(rta::audio::media_duration_sec(mi.duration) == 1,
                   std::string(kind) + " DSD" + std::to_string(r) + "：整秒转换 = 1");
                remove_file(p);
            }
        }

        // 7.2 DSF 篡改 header 样本数对照测试
        std::printf("  7.2 DSF 篡改 header sampleCount 对照（证明跟随 header 而非看文件长度）\n");
        {
            const std::string origDsf = temp_file("rta_dur_orig.dsf");
            const std::string tamperedDsf = temp_file("rta_dur_tampered.dsf");
            ck(make_dsd_file(origDsf, 64, "dsf"), "生成 1s DSD64 DSF 基准文件");
            std::vector<uint8_t> bytes = read_bytes(origDsf);
            ck(bytes.size() >= 80, "DSF 基准文件读取成功");
            if (bytes.size() >= 80) {
                // sampleCount 在 fmt 块（offset 28）+ 24 = offset 64 处的 uint64_t
                uint64_t origSampleCount = 0;
                for (int i = 0; i < 8; ++i) origSampleCount |= ((uint64_t)bytes[64 + i]) << (8 * i);
                ck(origSampleCount == 2822400, "原始 sampleCount == 2822400 (1.0s)");

                // 篡改 sampleCount 为 7056000 (2.5s)
                const uint64_t tamperedCount = 7056000;
                for (int i = 0; i < 8; ++i) bytes[64 + i] = (uint8_t)((tamperedCount >> (8 * i)) & 0xFF);
                ck(write_bytes(tamperedDsf, bytes), "写出篡改 header 样本数后的 DSF");

                const MediaInfo miOrig = probe_media(origDsf);
                const MediaInfo miTampered = probe_media(tamperedDsf);
                const double ffOrig = read_ffprobe_raw_duration(origDsf);
                const double ffTampered = read_ffprobe_raw_duration(tamperedDsf);

                std::printf("    | 口径        | 篡改前（1s 样本数） | 篡改后（2.5s 样本数） | 结论                     |\n");
                std::printf("    | ffprobe     | %15.6f s | %16.6f s | 恒 ~1.01s（看文件长度）  |\n",
                            ffOrig, ffTampered);
                std::printf("    | probe_media | %15.6f s | %16.6f s | 跟随 header 样本数      |\n",
                            miOrig.duration, miTampered.duration);

                ck(std::fabs(miTampered.duration - 2.5) < 0.005,
                   "probe_media(tampered DSF)：duration 严格跟随 header 样本数 2.500s",
                   "duration=" + std::to_string(miTampered.duration));
                ck(std::fabs(ffTampered - ffOrig) < 0.005,
                   "对照：ffprobe 在 header 篡改后输出不变（证明 ffprobe 只看数据长度）",
                   "ffTampered=" + std::to_string(ffTampered) + " ffOrig=" + std::to_string(ffOrig));
                ck(rta::audio::media_duration_sec(miTampered.duration) == 3,
                   "篡改后 fileSec = 3 (四舍五入(2.5+0.5)=3)");
            }
            remove_file(origDsf);
            remove_file(tamperedDsf);
        }

        // 7.3 DFF 篡改 header DSD chunk 大小对照测试
        std::printf("  7.3 DFF 篡改 header DSD chunk 大小对照\n");
        {
            const std::string origDff = temp_file("rta_dur_orig.dff");
            const std::string tamperedDff = temp_file("rta_dur_tampered.dff");
            ck(make_dsd_file(origDff, 64, "dff"), "生成 1s DSD64 DFF 基准文件");
            std::vector<uint8_t> bytes = read_bytes(origDff);
            ck(bytes.size() >= 128, "DFF 基准文件读取成功");
            if (bytes.size() >= 128) {
                // 寻找 DSD chunk：大端 sz
                size_t dsdChunkOff = 0;
                for (size_t i = 16; i + 12 <= bytes.size(); ++i) {
                    if (std::memcmp(bytes.data() + i, "DSD ", 4) == 0) {
                        uint64_t sz = 0;
                        for (int j = 0; j < 8; ++j) sz = (sz << 8) | bytes[i + 4 + j];
                        if (sz == 705600) { dsdChunkOff = i; break; }
                    }
                }
                ck(dsdChunkOff > 0, "找到原始 DFF 的 DSD chunk (705600 字节)");
                if (dsdChunkOff > 0) {
                    // 篡改为 1411200 字节 (2.0s)
                    const uint64_t newSz = 1411200;
                    for (int j = 0; j < 8; ++j) bytes[dsdChunkOff + 4 + j] = (uint8_t)((newSz >> (8 * (7 - j))) & 0xFF);
                    ck(write_bytes(tamperedDff, bytes), "写出篡改 DSD chunk 大小后的 DFF");

                    const MediaInfo miOrig = probe_media(origDff);
                    const MediaInfo miTampered = probe_media(tamperedDff);
                    const double ffOrig = read_ffprobe_raw_duration(origDff);
                    const double ffTampered = read_ffprobe_raw_duration(tamperedDff);

                    std::printf("    | 口径        | 篡改前（705600 字节）| 篡改后（1411200 字节）| 结论                     |\n");
                    std::printf("    | ffprobe     | %15.6f s | %16.6f s | 恒 1.000s（看文件长度）  |\n",
                                ffOrig, ffTampered);
                    std::printf("    | probe_media | %15.6f s | %16.6f s | 跟随 header chunk 大小  |\n",
                                miOrig.duration, miTampered.duration);

                    ck(std::fabs(miTampered.duration - 2.0) < 0.005,
                       "probe_media(tampered DFF)：duration 严格跟随 header chunk 大小 2.000s",
                       "duration=" + std::to_string(miTampered.duration));
                    ck(std::fabs(ffTampered - 1.0) < 0.01,
                       "对照：ffprobe 对篡改后 DFF 仍报 1.000s",
                       "ffTampered=" + std::to_string(ffTampered));
                    ck(rta::audio::media_duration_sec(miTampered.duration) == 2,
                       "篡改后 fileSec = 2");
                }
            }
            remove_file(origDff);
            remove_file(tamperedDff);
        }

        // 7.4 非法 / 截断 header 容错：不抛异常、duration == 0、回退安全
        std::printf("  7.4 非法/截断 DSD header 容错\n");
        {
            auto test_bad = [&](const char* name, const std::vector<uint8_t>& b, const char* desc) {
                const std::string p = temp_file(name);
                write_bytes(p, b);
                bool threw = false;
                MediaInfo mi;
                try { mi = probe_media(p); } catch (...) { threw = true; }
                ck(!threw, std::string(desc) + "：不抛异常");
                ck(mi.duration == 0.0, std::string(desc) + "：duration == 0",
                   "duration=" + std::to_string(mi.duration));
                ck(rta::audio::media_duration_sec(mi.duration) == 0,
                   std::string(desc) + "：fileSec == 0（中性降级）");
                remove_file(p);
            };

            // DSF 截断与非法
            test_bad("bad_trunc1.dsf", { 'D', 'S', 'D', ' ' }, "DSF 仅 4 字节（截断）");
            test_bad("bad_trunc2.dsf", std::vector<uint8_t>(50, 0), "DSF 50 字节（缺少完整 fmt 块）");
            test_bad("bad_magic.dsf", std::vector<uint8_t>(90, 'X'), "DSF 错误魔数");

            // DFF 截断与非法
            test_bad("bad_trunc1.dff", { 'F', 'R', 'M', '8' }, "DFF 仅 4 字节（截断）");
            test_bad("bad_magic.dff", std::vector<uint8_t>(90, 'Y'), "DFF 错误魔数");
        }
    }

    // ================= 8) E-4：音频流标签合并（ogg/opus 元数据缺口） =================
    std::printf("- 8 音频流标签合并（E-4：merge_tags 纯函数 + ffprobe JSON 夹具）\n");
    {
        using rta::audio::merge_tags;
        using rta::audio::probe_parse_json;

        // 8.1 纯函数 merge_tags 单测：冲突 / 大小写 / 空值 / 独有键
        std::printf("  8.1 纯函数 merge_tags 单测\n");
        {
            // 冲突：format 优先
            std::map<std::string, std::string> fmt1 = { {"title", "FormatTitle"} };
            std::map<std::string, std::string> stm1 = { {"title", "StreamTitle"} };
            auto m1 = merge_tags(fmt1, stm1);
            ck(m1["title"] == "FormatTitle", "merge_tags 冲突：format 优先覆盖 stream");

            // 大小写不敏感：format Title vs stream title / TAG:title
            std::map<std::string, std::string> fmt2 = { {"TITLE", "UpperFormat"} };
            std::map<std::string, std::string> stm2 = { {"title", "lowerStream"}, {"TAG:ARTIST", "StreamArtist"} };
            auto m2 = merge_tags(fmt2, stm2);
            ck(m2["title"] == "UpperFormat", "merge_tags 大小写不敏感：TITLE 覆盖 title");
            ck(m2["artist"] == "StreamArtist", "merge_tags 剥除 TAG: 前缀并归一化键名");

            // 空值补缺：format 为空串或全空白，stream 有非空值
            std::map<std::string, std::string> fmt3 = { {"title", "FmtTitle"}, {"artist", ""}, {"album", "   "} };
            std::map<std::string, std::string> stm3 = { {"title", "StmTitle"}, {"artist", "StmArtist"}, {"album", "StmAlbum"} };
            auto m3 = merge_tags(fmt3, stm3);
            ck(m3["title"] == "FmtTitle", "format 非空 title 优先");
            ck(m3["artist"] == "StmArtist", "format 为空串时 stream 补缺 artist");
            ck(m3["album"] == "StmAlbum", "format 为全空白时 stream 补缺 album");

            // 独有键与全空值
            std::map<std::string, std::string> fmt4 = { {"comment", "NiceSong"}, {"empty1", ""} };
            std::map<std::string, std::string> stm4 = { {"year", "2024"}, {"empty1", ""}, {"empty2", "  "} };
            auto m4 = merge_tags(fmt4, stm4);
            ck(m4["comment"] == "NiceSong", "format 独有键保留");
            ck(m4["year"] == "2024", "stream 独有键补全");
            ck(m4.find("empty1") == m4.end() && m4.find("empty2") == m4.end(), "两边皆空/空白的键不注入结果");
        }

        // 8.2 内置 ffprobe JSON 夹具断言
        std::printf("  8.2 内置 ffprobe JSON 夹具（离线断言）\n");
        {
            // 夹具 1：真实 OGG Vorbis（标签全在 streams[0].tags）
            const std::string kOggFixture = R"({
                "streams": [
                    {
                        "index": 0,
                        "codec_name": "vorbis",
                        "codec_type": "audio",
                        "sample_rate": "44100",
                        "channels": 2,
                        "bits_per_raw_sample": "N/A",
                        "tags": {
                            "TITLE": "Bad Apple!!",
                            "ARTIST": "nomico",
                            "ALBUM": "Lovable",
                            "DATE": "2007",
                            "GENRE": "Touhou",
                            "TRACK": "1",
                            "encoder": "Lavc libvorbis"
                        }
                    }
                ],
                "format": {
                    "filename": "bad_apple.ogg",
                    "duration": "208.520000"
                }
            })";
            MediaInfo miOgg = probe_parse_json(kOggFixture);
            ck(miOgg.title == "Bad Apple!!", "OGG 夹具：提取 stream tags title");
            ck(miOgg.artist == "nomico", "OGG 夹具：提取 stream tags artist");
            ck(miOgg.album == "Lovable", "OGG 夹具：提取 stream tags album");
            ck(miOgg.date == "2007", "OGG 夹具：提取 stream tags date");
            ck(miOgg.genre == "Touhou", "OGG 夹具：提取 stream tags genre");
            ck(miOgg.track == "1", "OGG 夹具：提取 stream tags track");
            ck(miOgg.codec == "vorbis", "OGG 夹具：codec 为 vorbis");
            ck(miOgg.sampleRate == 44100 && miOgg.channels == 2, "OGG 夹具：流参数正确");
            ck(std::fabs(miOgg.duration - 208.52) < 0.001, "OGG 夹具：duration 正确");

            // 夹具 2：真实 Opus（标签在 streams[0].tags）
            const std::string kOpusFixture = R"({
                "streams": [
                    {
                        "index": 0,
                        "codec_name": "opus",
                        "codec_type": "audio",
                        "sample_rate": "48000",
                        "channels": 2,
                        "tags": {
                            "title": "Scarlet Zone",
                            "artist": "tatsh",
                            "album": "Material",
                            "date": "2010"
                        }
                    }
                ],
                "format": {
                    "filename": "scarlet.opus",
                    "duration": "154.200000"
                }
            })";
            MediaInfo miOpus = probe_parse_json(kOpusFixture);
            ck(miOpus.title == "Scarlet Zone", "Opus 夹具：提取 stream tags title");
            ck(miOpus.artist == "tatsh", "Opus 夹具：提取 stream tags artist");
            ck(miOpus.album == "Material", "Opus 夹具：提取 stream tags album");
            ck(miOpus.date == "2010", "Opus 夹具：提取 stream tags date");
            ck(miOpus.codec == "opus", "Opus 夹具：codec 为 opus");
            ck(miOpus.sampleRate == 48000 && miOpus.channels == 2, "Opus 夹具：流参数正确");
            ck(std::fabs(miOpus.duration - 154.20) < 0.001, "Opus 夹具：duration 正确");

            // 夹具 3：format 与 stream 标签混合冲突与补缺
            const std::string kMixedFixture = R"({
                "streams": [
                    {
                        "codec_name": "vorbis",
                        "codec_type": "audio",
                        "sample_rate": "44100",
                        "channels": 2,
                        "tags": {
                            "TITLE": "StreamTitle",
                            "ARTIST": "StreamArtist",
                            "ALBUM": "StreamAlbum"
                        }
                    }
                ],
                "format": {
                    "duration": "100.000000",
                    "tags": {
                        "title": "FormatTitle",
                        "artist": "",
                        "date": "2023"
                    }
                }
            })";
            MediaInfo miMixed = probe_parse_json(kMixedFixture);
            ck(miMixed.title == "FormatTitle", "混合夹具：format title 优先于 stream title");
            ck(miMixed.artist == "StreamArtist", "混合夹具：format artist 为空由 stream artist 补缺");
            ck(miMixed.album == "StreamAlbum", "混合夹具：format 缺失 album 由 stream album 补全");
            ck(miMixed.date == "2023", "混合夹具：format 独有 date 保留");

            // 夹具 4：多流环境（video 封面流 + audio 流），只提取 audio 流的 tags
            const std::string kMultiStreamFixture = R"({
                "streams": [
                    {
                        "codec_name": "png",
                        "codec_type": "video",
                        "tags": {
                            "title": "Cover Image Title",
                            "comment": "Video stream comment"
                        }
                    },
                    {
                        "codec_name": "flac",
                        "codec_type": "audio",
                        "sample_rate": "96000",
                        "channels": 2,
                        "bits_per_raw_sample": "24",
                        "tags": {
                            "title": "Audio Stream Title",
                            "artist": "Audio Artist"
                        }
                    }
                ],
                "format": {
                    "duration": "60.000000"
                }
            })";
            MediaInfo miMulti = probe_parse_json(kMultiStreamFixture);
            ck(miMulti.title == "Audio Stream Title", "多流夹具：正确认 audio 流标签，忽略 video 流 tags");
            ck(miMulti.artist == "Audio Artist", "多流夹具：提取 audio 流 artist");
            ck(miMulti.codec == "flac", "多流夹具：正确提取 audio 流 codec");
            ck(miMulti.sampleRate == 96000 && miMulti.bits == 24, "多流夹具：提取 audio 流采样率与位深");
        }

        // 8.3 真文件 opt-in（RTA_NET_TESTS=1）
        std::printf("  8.3 真文件测试（opt-in：RTA_NET_TESTS=1）\n");
        {
            const char* opt = std::getenv("RTA_NET_TESTS");
            if (opt && std::string(opt) == "1") {
                const std::string ffmpeg = rta::audio::find_tool("ffmpeg");
                if (ffmpeg.empty()) {
                    std::printf("    [SKIP] 未找到 ffmpeg，跳过合成真文件测试\n");
                } else {
                    const std::string oggPath = temp_file("rta_synth_stream.ogg");
                    const std::string opusPath = temp_file("rta_synth_stream.opus");

                    // 用 ffmpeg 合成带 stream 标签的 ogg
                    const std::wstring cmdOgg = L"\"" + rta::audio::widen(ffmpeg) +
                        L"\" -v error -y -f lavfi -i \"sine=frequency=1000:duration=1\" "
                        L"-metadata:s:a:0 title=\"SynthOggTitle\" -metadata:s:a:0 artist=\"SynthOggArtist\" \"" +
                        rta::audio::widen(oggPath) + L"\"";
                    if (rta::audio::run_hidden(cmdOgg) == 0 && std::filesystem::exists(u8p(oggPath))) {
                        const MediaInfo mi = probe_media(oggPath);
                        ck(mi.title == "SynthOggTitle", "真合成 OGG：probe_media 读取 stream title 成功", "got=" + mi.title);
                        ck(mi.artist == "SynthOggArtist", "真合成 OGG：probe_media 读取 stream artist 成功", "got=" + mi.artist);
                        remove_file(oggPath);
                    }

                    // 用 ffmpeg 合成带 stream 标签的 opus
                    const std::wstring cmdOpus = L"\"" + rta::audio::widen(ffmpeg) +
                        L"\" -v error -y -f lavfi -i \"sine=frequency=1000:duration=1\" -c:a libopus "
                        L"-metadata:s:a:0 title=\"SynthOpusTitle\" -metadata:s:a:0 artist=\"SynthOpusArtist\" \"" +
                        rta::audio::widen(opusPath) + L"\"";
                    if (rta::audio::run_hidden(cmdOpus) == 0 && std::filesystem::exists(u8p(opusPath))) {
                        const MediaInfo mi = probe_media(opusPath);
                        ck(mi.title == "SynthOpusTitle", "真合成 Opus：probe_media 读取 stream title 成功", "got=" + mi.title);
                        ck(mi.artist == "SynthOpusArtist", "真合成 Opus：probe_media 读取 stream artist 成功", "got=" + mi.artist);
                        remove_file(opusPath);
                    }
                }
            } else {
                std::printf("    [SKIP] RTA_NET_TESTS 未设为 1，跳过 ffmpeg 合成文件测试（设置 $env:RTA_NET_TESTS=1 可启用）\n");
            }
        }
    }

    remove_file(wav);

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
