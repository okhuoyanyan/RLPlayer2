// test_health_completeness.cpp — BUG-3 回归：半截文件不许被当成完成品
//
// 背景（2026-10-27 fuzz 扫描抓到的契约问题）：
//   截断到 50% 的 FLAC 与「边写边分析」的文件都能正常解码，于是 status=ready、
//   duration_sec 只反映已有字节（32.046 s 的曲子报 14.942 s），**没有任何不完整提示**。
//   用户若在拷贝/下载过程中分析，会拿到一份"看起来正常但只有一半"的结论。
//
// 修法（判据即本测试）：
//   1. 从**容器头**取声明时长（FLAC STREAMINFO.total_samples / WAV data / MP3 Xing·VBRI / DSF sampleCount）；
//   2. 与**实际解码**时长比对，|差| > max(1 s, 2%) ⟹ status="incomplete" + note 人话；
//   3. 指标照给（ok 仍为 true），只是状态与 note 让人看得见。
//
// 本测试两段：
//   ① 解析器单测（完全离线）：手工构造四个容器的头，逐个断言"声明时长"读得对；
//      另测三种"必须判不出"的情形（total_samples=0 / dataSize=0xFFFFFFFF / 纯文本冒充），
//      以及**非 ASCII 路径**（中文全角括号）—— 这里踩过一个真坑，见下。
//   ② 端到端（可选，需要 ffmpeg + 一个真 FLAC）：把真 FLAC 拷成临时文件并截到 50%，
//      analyze_file 必须报 incomplete（声明 32.046 s / 实际 14.942 s）；
//      同一首**完整**文件必须仍是 ready（不误报）。
//
// ⚠️ 踩过的坑（必须留着当回归）：本测试第一版用 int main(char** argv) 传中文路径，
//    argv 被 CRT 按 ANSI(GBK) 收窄 ⟹ 解析器打不开文件、静默返回"未知"。
//    现在一律 wmain + WideCharToMultiByte(CP_UTF8)，非 ASCII 夹具直接写在源码里。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "analysis/track_health.h"
#include "analysis/container_duration.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fsx = std::filesystem;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static std::string u8(const wchar_t* w) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::string upath(const fsx::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
static void write_bytes(const fsx::path& p, const std::vector<unsigned char>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)b.data(), (std::streamsize)b.size());
}
static fsx::path dir() { return fsx::current_path() / "test_health_completeness_tmp"; }

// ---- 夹具：FLAC（"fLaC" + 一个 STREAMINFO 块，后面没有音频帧：这里只测**头解析**）----
static std::vector<unsigned char> flac_header(uint32_t rate, int ch, int bps, long long total) {
    std::vector<unsigned char> v;
    auto put = [&v](const char* s) { for (int i = 0; s[i]; ++i) v.push_back((unsigned char)s[i]); };
    put("fLaC");
    v.push_back(0x80);                                  // last-block=1, type=0(STREAMINFO)
    v.push_back(0); v.push_back(0); v.push_back(34);    // 长度 34
    std::vector<unsigned char> si(34, 0);
    si[0] = 0x12; si[1] = 0x00; si[2] = 0x12; si[3] = 0x00;   // min/max block size（值无关）
    si[10] = (unsigned char)((rate >> 12) & 0xFF);
    si[11] = (unsigned char)((rate >> 4) & 0xFF);
    si[12] = (unsigned char)(((rate & 0x0F) << 4) | (((ch - 1) & 0x7) << 1) | (((bps - 1) >> 4) & 0x1));
    si[13] = (unsigned char)((((bps - 1) & 0x0F) << 4) | ((total >> 32) & 0x0F));
    for (int i = 0; i < 4; ++i) si[14 + i] = (unsigned char)((total >> (8 * (3 - i))) & 0xFF);
    v.insert(v.end(), si.begin(), si.end());
    v.resize(v.size() + 64, 0);                          // 一点填充，模拟真实的文件体
    return v;
}

// ---- 夹具：WAV（fmt + data 声明大小，与实际字节数无关）----
static std::vector<unsigned char> wav_header(uint32_t byteRate, uint32_t dataSize) {
    std::vector<unsigned char> v;
    auto put = [&v](const char* s) { for (int i = 0; s[i]; ++i) v.push_back((unsigned char)s[i]); };
    auto le32 = [&v](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((unsigned char)((x >> (8*i)) & 0xFF)); };
    auto le16 = [&v](unsigned x) { for (int i = 0; i < 2; ++i) v.push_back((unsigned char)((x >> (8*i)) & 0xFF)); };
    put("RIFF"); le32(36 + dataSize); put("WAVEfmt "); le32(16);
    le16(1); le16(2); le32(44100); le32(byteRate); le16(4); le16(16);
    put("data"); le32(dataSize);
    v.resize(v.size() + 128, 0);                        // 一点真实字节（远少于声明值）
    return v;
}

// ---- 夹具：MP3（首帧头 + Xing/帧计数 [+ LAME delay/padding]）----
static std::vector<unsigned char> mp3_xing(uint32_t frames, long long delay, long long padding) {
    std::vector<unsigned char> v;
    v.push_back(0xFF); v.push_back(0xFB);               // MPEG1 Layer3，无 CRC
    v.push_back(0x90);                                   // bitrate idx 9(128k) / sr idx 0(44.1k)
    v.push_back(0x00);                                   // 立体声
    v.resize(4 + 32, 0);                                 // 边信息 32 字节（MPEG1 立体声）
    auto putBE32 = [&v](uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((unsigned char)((x >> (8*i)) & 0xFF)); };
    v.push_back('X'); v.push_back('i'); v.push_back('n'); v.push_back('g');
    putBE32(0x1);                                        // flags: FRAMES 位
    putBE32(frames);
    v.resize(4 + 32 + 120, 0);                           // Xing 头合计 120 字节
    if (delay >= 0) {
        v.push_back('L'); v.push_back('A'); v.push_back('M'); v.push_back('E');
        v.resize(v.size() + 17, 0);                      // 到 LAME 标签 +21
        v.push_back((unsigned char)((delay >> 4) & 0xFF));
        v.push_back((unsigned char)(((delay & 0x0F) << 4) | ((padding >> 8) & 0x0F)));
        v.push_back((unsigned char)(padding & 0xFF));
    }
    v.resize(v.size() + 256, 0);
    return v;
}

// ---- 夹具：DSF（"DSD " + fmt：samplingFreq / sampleCount）----
static std::vector<unsigned char> dsf_header(uint32_t rate, uint64_t count) {
    std::vector<unsigned char> v;
    auto put = [&v](const char* s) { for (int i = 0; s[i]; ++i) v.push_back((unsigned char)s[i]); };
    auto le32 = [&v](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((unsigned char)((x >> (8*i)) & 0xFF)); };
    auto le64 = [&v](uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((unsigned char)((x >> (8*i)) & 0xFF)); };
    put("DSD "); le64(28); le64(28 + 12 + 52 + 12 + 128); le64(0);      // 头 + fmt + 一点 data
    put("fmt "); le64(52);
    le32(1); le32(0); le32(2); le32(2);                  // 版本 / 格式ID / 声道类型 / 声道数
    le32(rate); le32(1); le64(count); le32(4096); le32(0);
    put("data"); le64(128);
    v.resize(v.size() + 128, 0);
    return v;
}

static bool probe(const fsx::path& p, double* sec, std::string* how) {
    rta::analysis::DeclaredDuration d;
    const bool ok = rta::analysis::probe_declared_duration(upath(p), &d);
    if (sec) *sec = d.seconds;
    if (how) *how = d.how;
    return ok;
}

static int run_all(const std::string& realFlac) {
    std::error_code ec;
    const fsx::path d = dir();
    fsx::remove_all(d, ec);
    fsx::create_directories(d, ec);

    std::printf("== 1. 容器声明时长：四个容器逐项 ==\n");
    {
        const fsx::path f = d / "a.flac";
        write_bytes(f, flac_header(44100, 2, 16, 1413239));
        double s = 0; std::string how;
        check(probe(f, &s, &how) && std::fabs(s - 32.0462) < 0.001,
              "FLAC STREAMINFO 1413239 @44100 = 32.046 s", how);

        const fsx::path w = d / "a.wav";
        write_bytes(w, wav_header(176400, 10485760));
        check(probe(w, &s, &how) && std::fabs(s - 59.4435) < 0.001,
              "WAV data 10485760 B / byteRate 176400 = 59.443 s", how);

        const fsx::path m = d / "a.mp3";
        write_bytes(m, mp3_xing(4098, -1, 0));
        check(probe(m, &s, &how) && std::fabs(s - (4098.0 * 1152) / 44100.0) < 0.001,
              "MP3 Xing 4098 帧 ×1152 @44100 = 107.050 s", how);

        const fsx::path m2 = d / "b.mp3";
        write_bytes(m2, mp3_xing(4098, 576, 1000));
        check(probe(m2, &s, &how) && std::fabs(s - (4098.0 * 1152 - 1576) / 44100.0) < 0.001,
              "MP3 Xing 扣掉 LAME delay/padding", how);

        const fsx::path ds = d / "a.dsf";
        write_bytes(ds, dsf_header(2822400, 1083768832ULL));
        check(probe(ds, &s, &how) && std::fabs(s - 383.9884) < 0.001,
              "DSF sampleCount 1083768832 @2822400 = 383.988 s", how);
    }

    std::printf("== 2. 必须判「未知」的四种情形（宁可漏报，不许误报）==\n");
    {
        double s = 0; std::string how;
        const fsx::path f0 = d / "stream.flac";
        write_bytes(f0, flac_header(44100, 2, 16, 0));    // total_samples=0 = 流式编码
        check(!probe(f0, &s, &how), "FLAC total_samples=0 → 未知（不判）", how);

        const fsx::path wf = d / "rf64.wav";
        write_bytes(wf, wav_header(176400, 0xFFFFFFFFu)); // RF64/流式占位
        check(!probe(wf, &s, &how), "WAV dataSize=0xFFFFFFFF → 未知（不判）", how);

        const fsx::path mp3cbr = d / "cbr.mp3";            // 有帧、无 Xing
        {
            std::vector<unsigned char> v;
            v.push_back(0xFF); v.push_back(0xFB); v.push_back(0x90); v.push_back(0x00);
            v.resize(4 + 32 + 512, 0);
            write_bytes(mp3cbr, v);
        }
        check(!probe(mp3cbr, &s, &how), "MP3 无 Xing/VBRI → 未知（不估，避免 VBR 误报）", how);

        const fsx::path txt = d / "text.flac";
        write_bytes(txt, std::vector<unsigned char>(256, 'A'));
        check(!probe(txt, &s, &how), "纯文本冒充音频 → 未知（不判）", how);

        const fsx::path none = d / "no-such-file.flac";
        check(!probe(none, &s, &how), "不存在的文件 → 未知（不抛异常、不判）", how);
    }

    std::printf("== 3. 非 ASCII 路径（曾导致进程 0xC0000409 退出的回归）==\n");
    {
        const fsx::path sub = d / fsx::u8path("专辑-原创&同人");
        fsx::create_directories(sub, ec);
        const fsx::path p = sub / fsx::u8path("曲名（Live）🎵.flac");
        write_bytes(p, flac_header(44100, 2, 16, 1413239));
        double s = 0; std::string how;
        check(probe(p, &s, &how) && std::fabs(s - 32.0462) < 0.001,
              "中文全角括号 + emoji 路径可解析", how);
    }

    std::printf("== 4. 判定门槛（apply_completeness 的两种结局）==\n");
    {
        // 完整：声明=实际 ⟹ 不标 incomplete
        rta::analysis::TrackHealth h;
        h.status = "ready";
        h.durationSec = 32.0462;
        const fsx::path f = d / "full.flac";
        write_bytes(f, flac_header(44100, 2, 16, 1413239));
        rta::analysis::detail::apply_completeness(h, upath(f));
        check(h.status == "ready" && !h.incomplete && h.note.empty(),
              "声明=实际 → 保持 ready（不误报）", "decl=" + std::to_string(h.declaredSec));
        check(std::fabs(h.declaredSec - 32.0462) < 0.001, "declared_sec 已填（诊断用）",
              std::to_string(h.declaredSec));

        // 截断：声明 32.046 / 实际 14.942 ⟹ incomplete + note
        rta::analysis::TrackHealth t;
        t.status = "ready";
        t.durationSec = 14.942;
        rta::analysis::detail::apply_completeness(t, upath(f));
        check(t.status == "incomplete" && t.incomplete && !t.note.empty(),
              "声明 32.046 / 实际 14.942 → incomplete", t.note.substr(0, 60));
        check(t.note.find("32.046") != std::string::npos && t.note.find("14.942") != std::string::npos,
              "note 里同时给了声明值与实际值");

        // 未知容器（纯文本）：无论差多少都不判
        rta::analysis::TrackHealth u;
        u.status = "ready";
        u.durationSec = 1.0;
        const fsx::path txt = d / "unknown.flac";
        write_bytes(txt, std::vector<unsigned char>(128, 'Z'));
        rta::analysis::detail::apply_completeness(u, upath(txt));
        check(u.status == "ready" && !u.incomplete, "容器时长未知 → 不判（宁可漏报）");

        // 1 s 下限：差 0.5 s 的长曲不标（容器字段的正常粒度）
        rta::analysis::TrackHealth v;
        v.status = "ready";
        v.durationSec = 200.0 - 0.5;
        const fsx::path g = d / "200s.wav";
        write_bytes(g, wav_header(176400, 200 * 176400));
        rta::analysis::detail::apply_completeness(v, upath(g));
        check(v.status == "ready" && !v.incomplete, "长曲差 0.5 s（<1 s 下限）→ 不标",
              "decl=200 act=199.5");

        // JSON 契约：三个新键必须在
        const std::string js = rta::analysis::track_health_json(t);
        check(js.find("\"declared_sec\":") != std::string::npos &&
              js.find("\"incomplete\":true") != std::string::npos &&
              js.find("\"note\":") != std::string::npos,
              "JSON 含 declared_sec / incomplete / note");
    }

    std::printf("== 5. 端到端：真 FLAC 截断 50%%（需要 ffmpeg + 样本文件）==\n");
    {
        if (realFlac.empty() || !fsx::exists(fsx::u8path(realFlac))) {
            std::printf("  [SKIP] 没给真 FLAC 样本（argv[1] 或 RLPLAYER_TEST_FLAC），本段不跑\n");
        } else {
            // 载入源文件，按 50% 字节数写一个"半截"副本
            std::ifstream in(fsx::u8path(realFlac), std::ios::binary);
            std::vector<char> all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const fsx::path half = d / "half.flac";
            {
                std::ofstream o(half, std::ios::binary | std::ios::trunc);
                o.write(all.data(), (std::streamsize)(all.size() / 2));
            }
            const rta::analysis::TrackHealth h = rta::analysis::analyze_file(upath(half));
            std::printf("  half: status=%s dur=%.3f declared=%.3f\n", h.status.c_str(),
                        h.durationSec, h.declaredSec);
            check(h.status == "incomplete" && h.incomplete,
                  "截断 50% 的 FLAC → incomplete", h.status);
            check(h.declaredSec > h.durationSec * 1.5,
                  "声明时长明显大于实际（说明头没被截掉）",
                  std::to_string(h.declaredSec) + " vs " + std::to_string(h.durationSec));

            const rta::analysis::TrackHealth full = rta::analysis::analyze_file(realFlac);
            check(full.status == "ready" && !full.incomplete,
                  "同一首完整文件 → ready（不误报）", full.status);
            check(std::fabs(full.declaredSec - full.durationSec) < 1.0,
                  "完整文件 声明≈实际", std::to_string(full.declaredSec) + " / " +
                  std::to_string(full.durationSec));
        }
    }

    std::error_code ec2;
    fsx::remove_all(d, ec2);
    std::printf("\n%s  fails=%d\n", fails == 0 ? "ALL PASS" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string real;
    if (argc > 1) real = u8(argv[1]);
    else {
        const char* envp = std::getenv("RLPLAYER_TEST_FLAC");
        if (envp) real = envp;
    }
    try {
        return run_all(real);
    } catch (const std::exception& e) {
        std::printf("\nUNCAUGHT std::exception: %s\n", e.what());
    } catch (...) {
        std::printf("\nUNCAUGHT non-std exception\n");
    }
    return 3;
}
