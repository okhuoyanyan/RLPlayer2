// test_health_robust.cpp — 分析链路的健壮性回归（边界/畸形输入/确定性/JSON 合法性）
//
// 为什么单独一支测试（而不是塞进 test_track_health.cpp）：
//   test_track_health 证明的是「口径对不对」（数字与锚点对齐）；这一支证明的是
//   **「坏输入不会把链路打穿」**——即用户在真实曲库里会遇到的那些东西：
//   0 字节、只有文件头的截断、假扩展名、纯文本冒充音频、1 帧、全静音、8-bit/6 声道/32-bit、
//   含 & # % + 空格 全角括号 emoji 的文件名、200 字符文件名、中文目录、深层长路径。
//   再加上三条一致性判据：同一文件重复 5 次必须逐位一致；流式与整段必须逐位一致；
//   序列化结果里不允许出现 inf/nan（JSON 规范没有它们，客户端解析必崩）。
//
// 全部离线：只用本文件自己写出来的 WAV 夹具（+ 文本冒充音频），不依赖 ffmpeg、不读真实曲库、
// 不打开任何音频设备（铁律 1）。夹具落在当前工作目录的 test_health_robust_tmp\，跑完即删。
#include "analysis/track_health.h"
#include "analysis/health_stream.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fsx = std::filesystem;
using rta::analysis::TrackHealth;
using rta::analysis::HealthStreamOptions;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static fsx::path g_dir;

// ★ 必须用 u8string() 取路径字节：path::string() 在 Windows 上按 **ACP(GBK)** 转换，
//   含 emoji 的名字连转换都做不了（抛 "No mapping for the Unicode character exists..."），
//   含中文的名字则变成 GBK 字节 —— 那正是被分析链路的 valid_utf8() 挡下来的那类输入。
//   （本测试第一版就踩了：断言全红，其实是测试自己把路径编坏了。）
static std::string upath(const fsx::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// ---- WAV 夹具（只写头 + 可选样本；data 声明长度可与实际字节数不同，用来造截断/越界）----
static void write_wav(const fsx::path& p, int ch, int rate, int bits, int fmt,
                      unsigned long long frames, bool withSamples) {
    const int bytesPer = bits / 8;
    const unsigned long long dataBytes = frames * (unsigned long long)ch * (unsigned long long)bytesPer;
    std::vector<char> h;
    auto put32 = [&h](unsigned v) { for (int i = 0; i < 4; ++i) h.push_back((char)((v >> (8*i)) & 0xFF)); };
    auto put16 = [&h](unsigned v) { for (int i = 0; i < 2; ++i) h.push_back((char)((v >> (8*i)) & 0xFF)); };
    auto putS  = [&h](const char* s) { for (int i = 0; s[i]; ++i) h.push_back(s[i]); };
    putS("RIFF"); put32((unsigned)(36 + dataBytes)); putS("WAVEfmt "); put32(16);
    put16((unsigned)fmt); put16((unsigned)ch); put32((unsigned)rate);
    put32((unsigned)(rate * ch * bytesPer)); put16((unsigned)(ch * bytesPer)); put16((unsigned)bits);
    putS("data"); put32((unsigned)dataBytes);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(h.data(), (std::streamsize)h.size());
    if (withSamples) {
        // 全零（数字静音）：这是最容易把 log10(0) 打成 nan/inf 的输入
        std::vector<char> z(1 << 20, 0);
        unsigned long long left = dataBytes;
        while (left) {
            const size_t n = (size_t)std::min<unsigned long long>(left, z.size());
            f.write(z.data(), (std::streamsize)n);
            left -= n;
        }
    }
}

static void write_bytes(const fsx::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), (std::streamsize)s.size());
}

// JSON 合法性：字符串外的 nan/inf 一律不允许（JSON 没有这两个 token）
static bool json_finite(const std::string& js, std::string* bad) {
    bool inStr = false, esc = false;
    for (size_t i = 0; i < js.size(); ++i) {
        const char c = js[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (js.compare(i, 3, "nan") == 0) { if (bad) *bad = "nan"; return false; }
        if (js.compare(i, 3, "inf") == 0) { if (bad) *bad = "inf"; return false; }
        if (c < 0x20) { if (bad) *bad = "raw control char"; return false; }
    }
    return true;
}

static std::string stable(const TrackHealth& h) {
    TrackHealth c = h;
    c.analyzedAtMs = 0;                       // 墙钟，天然每次不同；其余 19 项必须逐位一致
    return rta::analysis::track_health_json(c);
}

// 一次「分析 + 三条通用判据」：JSON 里不能有 inf/nan、不能有裸控制字符
static TrackHealth analyze_checked(const fsx::path& p, const std::string& label) {
    TrackHealth h = rta::analysis::analyze_file(upath(p));
    std::string bad;
    check(json_finite(stable(h), &bad), label + " JSON 无非有限数/裸控制字符", bad);
    return h;
}

static int run_all();

int main() {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);      // 崩溃时也要留下已打印的行（fuzz 工具的基本要求）
    try {
        return run_all();
    } catch (const std::exception& e) {
        std::printf("\nUNCAUGHT std::exception: %s\n", e.what());
    } catch (...) {
        std::printf("\nUNCAUGHT non-std exception\n");
    }
    return 3;
}

static int run_all() {
    g_dir = fsx::current_path() / "test_health_robust_tmp";
    std::error_code ec;
    fsx::remove_all(g_dir, ec);
    fsx::create_directories(g_dir, ec);

    std::printf("== 1. 极端文件名 / 路径 ==\n");
    {
        const char* names[] = {
            "A&B - 测试.wav", "A#B.wav", "A%20B.wav", "A+B.wav", "A B.wav",
            "曲名（Live）.wav", "🎵测试.wav",
        };
        for (const char* n : names) {
            const fsx::path p = g_dir / fsx::u8path(n);
            write_wav(p, 2, 44100, 16, 1, 4410, true);      // 0.1 s
            TrackHealth h = analyze_checked(p, n);
            check(h.status == "ready" && std::fabs(h.durationSec - 0.1) < 1e-6,
                  std::string("可分析：") + n, "status=" + h.status + " dur=" + std::to_string(h.durationSec));
        }
        // 200 字符文件名（= 200 字节 ASCII，NTFS 上限 255）
        std::string longName(200, 'x');
        longName += ".wav";
        const fsx::path lp = g_dir / longName;
        write_wav(lp, 2, 44100, 16, 1, 4410, true);
        check(analyze_checked(lp, "超长文件名").status == "ready", "200 字符文件名可分析");
        // 中文目录 + 中文名
        const fsx::path cd = g_dir / fsx::u8path("专辑-原创&同人") / fsx::u8path("曲名.wav");
        fsx::create_directories(cd.parent_path(), ec);
        write_wav(cd, 2, 44100, 16, 1, 4410, true);
        check(analyze_checked(cd, "中文目录+中文名").status == "ready", "中文目录 + 中文名可分析");
    }

    std::printf("== 2. 畸形 / 边界输入（每种都必须给明确结论，不许崩）==\n");
    {
        // 0 字节
        const fsx::path z = g_dir / "empty.wav";
        write_bytes(z, "");
        TrackHealth h = analyze_checked(z, "0 字节");
        check(h.status == "error" && !h.error.empty(), "0 字节 → 明确 error", h.error.substr(0, 60));

        // 只有 100 字节（不够 44 字节头都不一定够）
        const fsx::path t = g_dir / "trunc_head.wav";
        write_bytes(t, std::string(100, '\0'));
        h = analyze_checked(t, "100 字节");
        check(h.status == "error", "只有文件头 → error", h.error.substr(0, 60));

        // data 声明 10 MB，实际只有 1 MB（下载不完整）
        const fsx::path s = g_dir / "declared_more.wav";
        {
            std::vector<char> hd;
            auto put32 = [&hd](unsigned v) { for (int i = 0; i < 4; ++i) hd.push_back((char)((v >> (8*i)) & 0xFF)); };
            auto put16 = [&hd](unsigned v) { for (int i = 0; i < 2; ++i) hd.push_back((char)((v >> (8*i)) & 0xFF)); };
            auto putS  = [&hd](const char* q) { for (int i = 0; q[i]; ++i) hd.push_back(q[i]); };
            putS("RIFF"); put32(36 + 10485760); putS("WAVEfmt "); put32(16);
            put16(1); put16(2); put32(44100); put32(44100*4); put16(4); put16(16);
            putS("data"); put32(10485760);
            std::ofstream f(s, std::ios::binary | std::ios::trunc);
            f.write(hd.data(), (std::streamsize)hd.size());
            std::vector<char> pad(1 << 20, 0);
            f.write(pad.data(), (std::streamsize)pad.size());
        }
        h = analyze_checked(s, "声明多于实际");
        check(h.status == "error", "data 声明 10 MB / 实际 1 MB → error（不是半截 ready）", h.error.substr(0, 80));

        // 0 帧 / 1 帧
        const fsx::path f0 = g_dir / "frames0.wav";
        write_wav(f0, 2, 44100, 16, 1, 0, false);
        h = analyze_checked(f0, "0 帧");
        check(h.status == "error", "0 帧 → error", h.error.substr(0, 60));

        const fsx::path f1 = g_dir / "frames1.wav";
        write_wav(f1, 2, 44100, 16, 1, 1, true);
        h = analyze_checked(f1, "1 帧");
        check(h.status == "ready", "1 帧 → ready（不崩、不除零）", "dur=" + std::to_string(h.durationSec));

        // 假扩展名：WAV 内容叫 .flac（内容嗅探必须赢过扩展名）
        const fsx::path fake = g_dir / "real_wav_named.flac";
        write_wav(fake, 2, 44100, 16, 1, 4410, true);
        h = analyze_checked(fake, "WAV 内容 .flac 扩展名");
        check(h.status == "ready", "假扩展名（WAV→.flac）→ ready", "fs=" + std::to_string(h.fs));
    }

    std::printf("== 3. 格式矩阵（bit depth / 声道 / 采样率）==\n");
    {
        struct F { const char* n; int ch, rate, bits, fmt; };
        const F fs[] = {
            { "wav_8bit_mono.wav", 1, 22050, 8, 1 },
            { "wav_16bit_stereo.wav", 2, 44100, 16, 1 },
            { "wav_24bit_6ch.wav", 6, 44100, 24, 1 },
            { "wav_32bit_int.wav", 2, 96000, 32, 1 },
            { "wav_32bit_float.wav", 2, 48000, 32, 3 },
            { "wav_rate8k.wav", 2, 8000, 16, 1 },
            { "wav_rate192k.wav", 2, 192000, 24, 1 },
        };
        for (const F& x : fs) {
            const fsx::path p = g_dir / x.n;
            write_wav(p, x.ch, x.rate, x.bits, x.fmt, (unsigned long long)x.rate / 10, true);
            TrackHealth h = analyze_checked(p, x.n);
            check(h.status == "ready" && h.channels == x.ch && (int)h.fs == x.rate,
                  std::string("格式矩阵：") + x.n,
                  "status=" + h.status + " ch=" + std::to_string(h.channels) + " fs=" + std::to_string(h.fs));
        }
    }

    std::printf("== 4. 全静音（log10(0) 的经典雷区）==\n");
    {
        const fsx::path p = g_dir / "silence_30s.wav";
        write_wav(p, 2, 44100, 16, 1, 44100 * 30, true);
        TrackHealth h = analyze_checked(p, "30 s 静音");
        check(h.status == "ready", "全静音 → ready（不崩）", "lufs=" + std::to_string(h.lufs));
        check(h.lufs <= -100.0, "全静音 LUFS 落到「测不出」区间", "lufs=" + std::to_string(h.lufs));
        check(h.quality.silencePct > 99.0, "silence_pct 判为几乎全静音",
              "pct=" + std::to_string(h.quality.silencePct));
    }

    std::printf("== 5. 确定性：同一文件 5 次逐位一致 ==\n");
    {
        const fsx::path p = g_dir / "det.wav";
        write_wav(p, 2, 44100, 16, 1, 44100 * 3, true);
        const std::string base = stable(rta::analysis::analyze_file(upath(p), HealthStreamOptions{}));
        int diff = 0;
        for (int i = 0; i < 5; ++i) {
            const std::string s = stable(rta::analysis::analyze_file(upath(p), HealthStreamOptions{}));
            if (s != base) ++diff;
        }
        check(diff == 0, "5 次重跑逐位一致", "diffs=" + std::to_string(diff));
    }

    std::printf("== 6. 流式 vs 整段逐位一致 ==\n");
    {
        const fsx::path p = g_dir / "stream_vs_whole.wav";
        write_wav(p, 2, 44100, 16, 1, 44100 * 5, true);
        TrackHealth a = rta::analysis::analyze_file(upath(p), HealthStreamOptions{});
        TrackHealth b = rta::analysis::analyze_file_whole(upath(p));
        check(a.status == "ready" && a.streamed, "WAV 走流式分块路径");
        check(stable(a) == stable(b), "流式与整段结果逐位一致");
    }

    std::printf("== 7. 不存在的文件 / 目录（必须明确报错，不许 500/崩）==\n");
    {
        TrackHealth h = analyze_checked(g_dir / "no-such-file.wav", "不存在");
        check(h.status == "error" && !h.error.empty(), "不存在的文件 → error", h.error.substr(0, 50));
        h = analyze_checked(g_dir, "目录");
        check(h.status == "error" && !h.error.empty(), "传目录 → error", h.error.substr(0, 50));
        // 表达式求值顺序无关性：析构/异常都不许把进程带走
    }

    std::printf("== 8. JSON 转义（引号 / 反斜杠）==\n");
    {
        // 注意：Windows 的文件名**不允许**双引号，所以「带引号的文件」只能以「不存在的路径」形式
        // 进入错误信息 —— 那正好是转义最容易漏的地方（error 里会原样带上用户给的路径）。
        TrackHealth e = analyze_checked(g_dir / "no\"such\tfile.wav", "不存在的带引号/制表符名");
        const std::string js = stable(e);
        check(json_finite(js, nullptr), "错误信息里的引号/控制字符已转义（JSON 仍合法）");
        check(js.find("no\"such") == std::string::npos, "错误信息里没有未转义的裸引号");
        check(js.find("\tfile") == std::string::npos || js.find("\\tfile") != std::string::npos,
              "错误信息里的制表符已转义成 \\t");
        // 反斜杠：Windows 路径天然带 \，必须成对转义（否则 JSON 解析器会吃掉下一个字符）
        check(js.find("D:\\\\") != std::string::npos || js.find("D:\\\\RTA") != std::string::npos,
              "路径反斜杠已转义成双反斜杠");
        // 合法但少见的文件名：单引号 / 方括号
        const fsx::path p = g_dir / "a'b[1].wav";
        write_wav(p, 2, 44100, 16, 1, 441, true);
        TrackHealth h = analyze_checked(p, "单引号+方括号文件名");
        check(h.status == "ready", "单引号/方括号文件名 → ready", h.status);
    }

    std::error_code ec2;
    fsx::remove_all(g_dir, ec2);
    std::printf("\n%s  fails=%d\n", fails == 0 ? "ALL PASS" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}
