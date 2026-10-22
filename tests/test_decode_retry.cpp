// test_decode_retry.cpp — ffmpeg 解码失败归因 + 瞬时故障重试回归（反馈 20261022-01 现象 B）
//
// 现场（VM，1.0.7）：同一个 FLAC「ZARD - 02.翼を広げて.flac」（日文假名 + 空格）
//   18:22 连播两级都成功（日志 L5/L11 已用 ffmpeg 解码），18:44 起连续四次失败：
//   "ffmpeg 解码失败（退出码 -42 ... Error opening input: Illegal byte sequence)"。
// 即：同一文件、同一进程、间歇性失败 ⟹ 不是文件名编码，也不是文件损坏。
//
// 根因（本测试钉住的两条）：
//   ① **-42 = -EILSEQ 是 ffmpeg 的掩码，不是真实原因。** ffmpeg 打开输入失败后会回退到
//      ANSI 代码页重试；含非 ASCII 字符的路径在该回退里被判成「非法序列」，于是把
//      ENOENT / EACCES / 共享冲突统统报成 Illegal byte sequence。
//      实测对照：日文名文件存在 → exit 0；日文名 + 独占占用 → -42；
//                纯 ASCII 名 + 独占占用 → -13(Permission denied)。
//   ② 真实触发是**源文件此刻读不了**（标签写回的 rename 窗口 / 同步或杀毒扫描的短暂独占）。
//      这类状态是瞬时的，所以修法是：失败后问 Win32「到底能不能读、为什么」，
//      能重试就退避重试，并把 Win32 的真实原因写进错误文本。
//
// 判据设计（铁律 3）——为什么这样测才测得对：
//   * 只断言「抛异常」不够：修前修后都抛。必须断言**错误文本里出现 Win32 真实原因**
//     （"正被其它程序独占占用" / "此刻不存在"）且**不再让 -42 当成唯一解释**。
//   * 只测「成功路径」也不够：成功路径修前就是好的（现场 L5 证明）。必须构造
//     【文件读不了】这个状态，才能复现现场故障。
//   * 重试是否真的生效，用**恢复后能解码成功**来证明（锁释放 → 第 2 次尝试成功），
//     而不是数 Sleep 次数（那只是实现细节）。
//
// ⚠️ 未解决项（铁律 4）：
//   * 现场那四次的**具体占用方**无从取证（日志里没有句柄信息，用户的机器也不在本机）。
//     本测试证明「-42 ⟸ 源文件瞬时不可读」这条因果与修法有效，但不能证明用户机器上
//     那一刻是谁占的。若要定位占用方需在用户机器上加 handle 级遥测。
//   * 「标签写回的 rename 窗口」是**代码审查推断**（writeback.h:414/432 两次 MoveFileEx），
//     本机未在真实写回批次期间抓到该窗口（时序依赖，无稳定复现手段）。
//
// 素材全部落在 %TEMP% 沙箱（含日文名），绝不触碰 MUSIC 与用户数据（铁律 1 无关：不开设备）。
#include "audio/audio_loader.h"
#include "audio/wav.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

// 日文名素材：用 UTF-8 字节字面量，不受编译器 source/execution charset 影响。
// "翼を広げて" = E7 BF BC E3 82 92 E5 BA 83 E3 81 92 E3 81 A6
static const char* kCjkName = "ZARD - 02.\xE7\xBF\xBC\xE3\x82\x92\xE5\xBA\x83\xE3\x81\x92\xE3\x81\xA6.flac";

static fs::path u8path_of(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// 用包内/环境 ffmpeg 现场生成一个真 FLAC（不依赖 MUSIC）
static bool make_flac(const std::string& ffmpeg, const std::string& dst) {
    const std::wstring cmd = L"\"" + rta::audio::widen(ffmpeg) +
        L"\" -v error -y -f lavfi -i \"anullsrc=r=44100:cl=stereo\" -t 1 -c:a flac \"" +
        rta::audio::widen(dst) + L"\"";
    return rta::audio::run_hidden(cmd) == 0;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== ffmpeg 解码失败归因 / 瞬时故障重试（反馈 20261022-01 现象 B）==\n");

    const std::string ffmpeg = rta::audio::find_tool("ffmpeg");
    ck(!ffmpeg.empty(), "环境有 ffmpeg（门禁前提）", "find_tool(\"ffmpeg\") 为空");
    if (ffmpeg.empty()) { std::printf("缺 ffmpeg，门禁无法执行\n"); return 1; }

    // ---- 沙箱目录（%TEMP% 下，进程唯一，绝不碰 MUSIC / 用户数据）----
    wchar_t tmpW[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmpW);
    const std::string sandbox =
        rta::audio::wide_to_utf8(tmpW) + "rta_decode_retry_" + std::to_string((unsigned long)GetCurrentProcessId());
    std::error_code ec0;
    fs::create_directories(u8path_of(sandbox), ec0);
    std::printf("  沙箱: %s\n", sandbox.c_str());

    const std::string cjk = sandbox + "\\" + kCjkName;
    const std::string ascii = sandbox + "\\ascii_probe.flac";

    // ================= 1) 前提：日文名素材真能生成且真能解码（不是「文件名有问题」） =========
    std::printf("- 1 日文名素材：生成 + 正常解码（排除「文件名编码」这条线）\n");
    {
        const bool a = make_flac(ffmpeg, ascii);
        ck(a, "ffmpeg 生成 ASCII 名 FLAC（模板素材）");
        // 用 filesystem 宽路径复制到日文名 —— 保证落的是【真名】，不经 ACP
        std::error_code ec;
        fs::copy_file(u8path_of(ascii), u8path_of(cjk), fs::copy_options::overwrite_existing, ec);
        ck(!ec && fs::file_size(u8path_of(cjk), ec) > 44, "日文名 FLAC 按真名落盘",
           "size=" + std::to_string((long long)fs::file_size(u8path_of(cjk), ec)));
        try {
            rta::audio::LoadNote note;
            const rta::audio::AudioBuffer b = rta::audio::read_any(cjk, &note);
            ck(b.rate == 44100 && b.channels == 2 && b.frames() > 0,
               "read_any 能解码日文名 FLAC（成功路径不该被误杀）",
               "rate=" + std::to_string(b.rate) + " ch=" + std::to_string(b.channels) +
               " frames=" + std::to_string(b.frames()) + " note=" + note.text);
        } catch (const std::exception& e) {
            ck(false, "read_any 能解码日文名 FLAC（成功路径不该被误杀）", e.what());
        }
    }

    // ================= 2) 核心：源文件被独占占用时，错误必须说真原因（不是 -42 掩码） =========
    // 这一步直接复现现场故障形态：文件读不了 → ffmpeg 报 -42 → 修前错误文本只有
    // "Illegal byte sequence"，修后必须带出 Win32 的 ERROR_SHARING_VIOLATION(32)。
    std::printf("- 2 源文件被独占占用：错误文本必须带 Win32 真实原因（-42 是掩码）\n");
    {
        HANDLE lock = CreateFileW(rta::audio::widen(cjk).c_str(), GENERIC_READ, 0 /* 不共享 */,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ck(lock != INVALID_HANDLE_VALUE, "已对日文名源文件加独占锁（构造故障态）");
        if (lock != INVALID_HANDLE_VALUE) {
            std::string msg;
            try {
                rta::audio::LoadNote note;
                (void)rta::audio::read_any(cjk, &note);
                msg = "(未抛异常——说明锁没生效或读到了缓存，本用例无效)";
            } catch (const std::exception& e) { msg = e.what(); }
            CloseHandle(lock);

            std::printf("  [信息] 修复后的错误文本:\n    %s\n", msg.c_str());
            ck(!msg.empty(), "源文件读不了时 read_any 必须失败（不许静默返回空继续播）");
            // 关键判据：真实原因必须出现，且是「占用」这一类，而不是只有 EILSEQ 的复述
            ck(msg.find("独占占用") != std::string::npos || msg.find("32") != std::string::npos,
               "错误文本带出 Win32 真实原因（共享冲突），不再只有 Illegal byte sequence",
               "got=" + msg);
            // 原始退出码必须如实带出：
            //   zh-CN (ACP 936) 下日文名回退 ANSI 会触发 EILSEQ(-42)；
            //   en-US (ACP 1252，如 CI 云机) 下日文名回退 ANSI 会变乱码找不到路径，触发 ENOENT(-2)。
            // 两者都是 ffmpeg 内部行为，判据断言「原始负数退出码被保留」而不写死某一 locale 的特定数值。
            ck(msg.find("退出码 -") != std::string::npos,
               "错误文本保留 ffmpeg 原始负数退出码供排查（如 zh-CN 下的 -42、en-US 下的 -2）",
               "got=" + msg);
            // 真实原因与假原因区分：
            //   若是 -42 则文本明示「不代表编码坏了」；若非 -42 则说明未触发 EILSEQ，由前一条 Win32 原因断言保证真因定性
            if (msg.find("-42") != std::string::npos) {
                ck(msg.find("不代表文件真有编码问题") != std::string::npos ||
                   msg.find("掩码") != std::string::npos ||
                   msg.find("回退到 ANSI") != std::string::npos,
                   "在报 -42 时错误文本明示「不代表文件编码坏了」（避免把假原因当结论）", "got=" + msg);
            } else {
                ck(msg.find("源文件此刻读不了") != std::string::npos,
                   "在非 -42 退出码时错误文本前置 Win32 真实原因（非 -42 路径分支）", "got=" + msg);
            }
        }
    }

    // ================= 3) 核心：瞬时故障恢复后必须自动成功（重试真的起作用） =========
    // 手法：起一个线程在 150ms 后释放锁；read_any 第 1 次必然失败，重试应该在退避后成功。
    // 判据是「最终成功」，不是「重试了几次」——后者是实现细节（铁律 3）。
    std::printf("- 3 瞬时占用恢复后应自动解码成功（退避重试验证）\n");
    {
        HANDLE lock = CreateFileW(rta::audio::widen(cjk).c_str(), GENERIC_READ, 0,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ck(lock != INVALID_HANDLE_VALUE, "已加独占锁（本轮由后台线程稍后释放）");
        if (lock != INVALID_HANDLE_VALUE) {
            std::thread releaser([lock] {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                CloseHandle(lock);          // 模拟占用方松手（写在回/扫描结束/改名完成）
            });
            bool ok = false;
            std::string err;
            try {
                rta::audio::LoadNote note;
                const rta::audio::AudioBuffer b = rta::audio::read_any(cjk, &note);
                ok = (b.rate == 44100 && b.frames() > 0);
            } catch (const std::exception& e) { err = e.what(); }
            releaser.join();
            ck(ok, "占用释放后 read_any 自动解码成功（现场那种「重试一下就好」的故障）", err);
        }
    }

    // ================= 4) 反向验证：不是「什么都重试」 =================
    // 文件根本不存在时，源文件探针应立刻给出「不存在」，且不做无意义的三次长重试。
    std::printf("- 4 反向：文件确实不存在时报「不存在」（不是无脑重试/不是编码问题）\n");
    {
        const std::string missing = sandbox + "\\" + kCjkName + ".does-not-exist";
        std::string msg;
        try {
            rta::audio::LoadNote note;
            (void)rta::audio::read_any(missing, &note);
        } catch (const std::exception& e) { msg = e.what(); }
        ck(!msg.empty(), "不存在的文件必须失败");
        ck(msg.find("不存在") != std::string::npos,
           "错误文本说「不存在」（子串）", "got=" + msg);
    }

    // ================= 5) 并发压力：同一日文名文件 2 线程 × N 轮，不得失败 =================
    // 现场怀疑过「预解码线程与播放线程同时解同一文件」。这里两线程各解 25 轮（共 50）。
    // 铁律 3：断言的是【全部成功】，不是「偶尔成功也算过」。
    std::printf("- 5 并发压力：2 线程 × 25 轮同解日文名文件（现场怀疑的并发线索）\n");
    {
        auto hammer = [&](int rounds, int* okCount, std::string* firstErr) {
            for (int i = 0; i < rounds; ++i) {
                try {
                    rta::audio::LoadNote note;
                    const rta::audio::AudioBuffer b = rta::audio::read_any(cjk, &note);
                    if (b.rate == 44100 && b.frames() > 0) ++(*okCount);
                    else if (firstErr->empty()) *firstErr = "解码结果为空";
                } catch (const std::exception& e) {
                    if (firstErr->empty()) *firstErr = e.what();
                }
            }
        };
        int ok1 = 0, ok2 = 0;
        std::string e1, e2;
        const auto t0 = std::chrono::steady_clock::now();
        std::thread th1(hammer, 25, &ok1, &e1);
        std::thread th2(hammer, 25, &ok2, &e2);
        th1.join(); th2.join();
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const int total = ok1 + ok2;
        std::printf("  并发结果: ok=%d/50 (%.1fs)  err1=%s  err2=%s\n",
                    total, sec, e1.empty() ? "-" : e1.c_str(), e2.empty() ? "-" : e2.c_str());
        ck(total == 50, "并发 50 次全部成功（并发抢 tmp 名/抢源文件均不成立）",
           "ok=" + std::to_string(total) + "/50 err=" + (e1.empty() ? e2 : e1));
    }

    // ================= 6) 长路径 + 日文 + 空格：连续 30 次顺序解码（稳定性） =================
    // 现场临时 WAV 落在 C:\Users\XINCHE~1\AppData\Local\Temp\（8.3 短名），源文件含日文与空格。
    // 这里把这两点都放大：深层嵌套目录 + 日文目录名 + 文件名带空格与假名，连解 30 次。
    // 判据：30 次全部成功、结果一致（不是「偶尔成功」）。也不许因路径长而误报编码问题。
    std::printf("- 6 长路径/日文/空格：连续 30 次顺序解码\n");
    {
        // 逐层加长（每层都是日文名），最终路径远超常见 100 字符，但仍在 Win32 长路径配合下可用
        std::string deep = sandbox;
        for (int i = 0; i < 6; ++i) {
            deep += "\\";
            deep += "\xE3\x83\x87\xE3\x82\xA3\xE3\x83\xAC\xE3\x82\xAF\xE3\x83\x88\xE3\x83\xAA"  // ディレクトリ
                    "\xE5\x90\x8D\xE9\x95\xB7\xE3\x81\x84";                                            // 名長い
            deep += std::to_string(i);
        }
        std::error_code ec;
        fs::create_directories(u8path_of(deep), ec);
        const std::string longFile = deep + "\\ZARD - 02.\xE7\xBF\xBC\xE3\x82\x92\xE5\xBA\x83"
                                          "\xE3\x81\x92\xE3\x81\xA6 (long path).flac";
        std::printf("  路径长度: %zu 字符\n", longFile.size());
        fs::copy_file(u8path_of(ascii), u8path_of(longFile), fs::copy_options::overwrite_existing, ec);
        ck(!ec, "长路径日文素材落盘", ec ? ec.message() : "");

        int ok = 0;
        std::string firstErr;
        for (int i = 0; i < 30; ++i) {
            try {
                rta::audio::LoadNote note;
                const rta::audio::AudioBuffer b = rta::audio::read_any(longFile, &note);
                if (b.rate == 44100 && b.frames() > 0) ++ok;
                else if (firstErr.empty()) firstErr = "解码结果为空";
            } catch (const std::exception& e) {
                if (firstErr.empty()) firstErr = e.what();
            }
        }
        std::printf("  顺序解码: %d/30 成功  firstErr=%s\n", ok, firstErr.empty() ? "-" : firstErr.c_str());
        ck(ok == 30, "长路径+日文+空格 连续 30 次全部解码成功（稳定）",
           "ok=" + std::to_string(ok) + "/30 err=" + firstErr);
    }

    // ---- 清理沙箱（只删自己建的 %TEMP% 目录）----
    {
        std::error_code ec;
        fs::remove_all(u8path_of(sandbox), ec);
    }

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
