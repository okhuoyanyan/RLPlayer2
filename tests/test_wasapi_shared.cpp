// test_wasapi_shared.cpp — WASAPI 共享输出后端
//   2026-10-22：VM 事故热修（共享模式兜底）
//   2026-10-24：共享模式多混音格式（float32 / int16 / int24(24-in-32) / int32 / 紧凑 24）
//
// 三部分：
//   §1 纯离线（本文件的主体）：不打开设备、不发声，铁律 1 无涉。
//      §1.0 mix format → 写入路径的映射（pick_shared_writer，4 种必需格式 + 拒绝项）
//      §1.1 单样本字节写出（put_shared_sample：0.5 / ±满幅 / 越界饱和 / NaN）
//      §1.2 数值换算（f64_to_s16 / s24 / s24_in_32 / s32；含 int16 全域往返扫描）
//      §1.3 管线 int32 → 字节（put_shared_sample_from_s32：位深归一化 + 装箱整链）
//      §1.3b 整块交错装箱（shared_pack_int32 / shared_pack_f32 —— 生产 pack() 用的就是它们）
//      §1.3c 独占容器装箱（ex_pack_int32 —— 生产 WasapiOutput::pack() 用的就是它；
//            2026-10-25 P0-A：16→float32 / 24→EXT32-int 的 −48 dBFS 量化错位，
//            含 16/24-bit 全域逐样本扫描 + 与共享路径逐字节对照）
//      §1.4 老回归：int32→float32 换算边界（s32_to_f32 / s32_to_f32_bits）
//      §1.5 老回归：shared_writable 钳制
//   §2 设备 smoke：【只写数字静音（全零样本）且 ≤1 秒】，先打印醒目横幅
//      （目标设备 / 时长 / 电平=数字静音 0）。open→start→写 0.5s 全零→确认写入帧数>0
//      且设备播放位置增长→close。
//
// 判据设计（铁律 3：这个指标真能证明我要证明的事吗？）
//   · §1.0 证明「格式识别」，§1.1 证明「字节装箱」，§1.3 证明「位深归一化 + 装箱串联」。
//     三者串起来覆盖的是 open() 之外的全部链路；open() 里唯一剩下的动作是
//     GetMixFormat + Initialize（原样交指针）—— 那只能用真机 smoke 覆盖，见下。
//   · 满幅/越界用的是 ±1.0 与 ±2.0（不是 −60 dBFS）：量化器的溢出/回绕只在满幅附近暴露
//     （铁律 3 第三坑）。±32767/−32768、0x7FFFFF/−0x800000、INT32_MAX/INT32_MIN 全部断言。
//
// ⚠️ 未解决项（铁律 4）
//   1) 整型混音格式（int16 / int24 / int32）**只有本文件的离线单测**，没有真机端点验证：
//      2026-10-24 只读探测本机 6 个 ACTIVE 输出端点，混音格式全是 float32
//      （N300 96000/2ch/32f、Audiobyte USB 48000/2ch/32f、VB-Matrix VAIO 48000/6ch/32f、
//       AMD HD Audio×2 / TASCAM 48000/2ch/32f），没有整型端点可测。
//      当前以「pick_shared_writer / put_shared_sample / shared_pack_int32 单测」为回归基线。
//   2) §2 device smoke 覆盖的仍是 float32 端点（默认 Audiobyte 48k/32f；
//      也可指定端点：test_wasapi_shared.exe N300 → 已实测 96000/2ch/32f 全链路通过）。
//      整型端点的 smoke 无从执行（同上）。
//   3) 「无输出设备/被独占占用」的报错路径在本环境无法安全构造（本机有活动端点且不能去
//      占用用户的听音设备），当前以 open 失败时 last_error 非空的断言作为回归基线，
//      不做人为占锁实验。
#include "audio/wasapi_shared.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using rta::audio::s32_to_f32;
using rta::audio::shared_writable;
using rta::audio::SharedSampleFmt;
using rta::audio::pick_shared_writer;
using rta::audio::put_shared_sample;
using rta::audio::put_shared_sample_from_s32;
using rta::audio::shared_pack_int32;
using rta::audio::shared_pack_f32;
using rta::audio::shared_fmt_bytes;
using rta::audio::shared_fmt_name;
using rta::audio::shared_fmt_valid_bits;
using rta::audio::f64_to_s16;
using rta::audio::f64_to_s24;
using rta::audio::f64_to_s24_in_32;
using rta::audio::f64_to_s32;
using rta::audio::shared_subfmt_float;
using rta::audio::shared_subfmt_pcm;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

static std::string hexs(const unsigned char* p, int n) {
    static const char* H = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < n; ++i) {
        if (i) s += ' ';
        s += H[(p[i] >> 4) & 0xF];
        s += H[p[i] & 0xF];
    }
    return s;
}

// 逐字节比对期望的小端字节序列
static bool bytes_eq(const unsigned char* p, std::initializer_list<int> exp) {
    int i = 0;
    for (int v : exp) { if (p[i] != (unsigned char)v) return false; ++i; }
    return true;
}

// ---- WAVEFORMATEX / WAVEFORMATEXTENSIBLE 构造器（模拟 GetMixFormat 的返回值）----
static WAVEFORMATEX mk_plain(WORD tag, WORD ch, DWORD rate, WORD bits, WORD blockAlign) {
    WAVEFORMATEX w;
    std::memset(&w, 0, sizeof(w));
    w.wFormatTag = tag;
    w.nChannels = ch;
    w.nSamplesPerSec = rate;
    w.wBitsPerSample = bits;
    w.nBlockAlign = blockAlign;
    w.nAvgBytesPerSec = rate * blockAlign;
    return w;
}

// containerBits = wBitsPerSample（容器宽度），validBits = wValidBitsPerSample（有效位深）
static WAVEFORMATEXTENSIBLE mk_ext(WORD ch, DWORD rate, WORD containerBits, WORD validBits,
                                   bool flt, WORD blockAlign) {
    WAVEFORMATEXTENSIBLE w;
    std::memset(&w, 0, sizeof(w));
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = ch;
    w.Format.nSamplesPerSec = rate;
    w.Format.wBitsPerSample = containerBits;
    w.Format.nBlockAlign = blockAlign;
    w.Format.nAvgBytesPerSec = rate * blockAlign;
    w.Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    w.Samples.wValidBitsPerSample = validBits;
    w.dwChannelMask = 0x3;
    w.SubFormat = flt ? shared_subfmt_float() : shared_subfmt_pcm();
    return w;
}

// 单样本写出 + 断言（先填 0xEE 毒值，确保写出长度与字节序都被检查到）
static void ck_sample(SharedSampleFmt fmt, double x, std::initializer_list<int> exp,
                      const char* what) {
    unsigned char buf[8];
    std::memset(buf, 0xEE, sizeof(buf));
    put_shared_sample(fmt, x, buf);
    const int n = shared_fmt_bytes(fmt);
    const bool ok = bytes_eq(buf, exp);
    // 毒值检查：第 n 字节之后必须没被写
    bool tailOk = true;
    for (int i = n; i < 8; ++i) if (buf[i] != 0xEE) tailOk = false;
    ck(ok && tailOk, what, "实际 " + hexs(buf, n));
}

// §2 可选：用第一个参数（或环境变量 RTA_SHARED_DEV）指定端点 —— 名字子串或完整 id。
// 用途：本机默认端点是 Audiobyte（48k/32f），而任务关注的 N300 是 96000/32f；
// 不指定时行为与旧版完全一致（默认端点）。
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string wantDev;
    // 2026-10-25：新增 --offline（或 RTA_NO_DEVICE=1）——只跑 §1 纯离线部分，
    // **完全不开音频设备**（铁律 1 的最保守形态）。默认行为不变（ctest 仍跑 §2 静音 smoke）。
    bool offlineOnly = false;
    for (int i = 1; i < argc && argv[i]; ++i) {
        if (std::strcmp(argv[i], "--offline") == 0) { offlineOnly = true; continue; }
        if (argv[i][0] == '\0') continue;
        if (argv[i][0] == '-') { std::printf("  [WARN] 未知选项 %s（已忽略）\n", argv[i]); continue; }
        wantDev = argv[i];   // 兼容旧用法：第一个非选项参数 = 端点名/子串
    }
    if (const char* e = std::getenv("RTA_SHARED_DEV")) { if (wantDev.empty()) wantDev = e; }
    if (const char* e = std::getenv("RTA_NO_DEVICE")) { if (std::string(e) == "1") offlineOnly = true; }
    std::printf("== wasapi_shared：§1 纯离线（格式映射/换算/钳制，不开设备） ==\n");

    // ============================ §1.0 格式映射 ============================
    // 真实世界形态：Windows 默认 float32；蓝牙 HFP / 老声卡 16-bit；
    // 24-bit 值放 32 位容器（24-in-32）；32-bit int；紧凑 24（老驱动）。
    std::printf("\n-- §1.0 pick_shared_writer：mix format → 写入路径 --\n");
    {
        // 本机 N300 之外的这台机器实测：96000 Hz / 32-bit float（EXTENSIBLE）
        const auto w96f = mk_ext(2, 96000, 32, 32, true, 8);
        ck(pick_shared_writer((const WAVEFORMATEX*)&w96f) == SharedSampleFmt::Float32,
           "96000/32f EXTENSIBLE → float32（真机混音格式）",
           shared_fmt_name(pick_shared_writer((const WAVEFORMATEX*)&w96f)));

        const auto wf48 = mk_plain(WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 32, 8);
        ck(pick_shared_writer(&wf48) == SharedSampleFmt::Float32, "48000/32f IEEE_FLOAT → float32");

        // ① int16
        const auto w16_441 = mk_plain(WAVE_FORMAT_PCM, 2, 44100, 16, 4);
        ck(pick_shared_writer(&w16_441) == SharedSampleFmt::Int16, "44100/16 PCM → int16",
           shared_fmt_name(pick_shared_writer(&w16_441)));
        const auto w16_ext = mk_ext(2, 48000, 16, 16, false, 4);
        ck(pick_shared_writer((const WAVEFORMATEX*)&w16_ext) == SharedSampleFmt::Int16,
           "48000/16 EXTENSIBLE PCM → int16");

        // ② int24（24-in-32）：32 位容器 + 24 位有效
        const auto w24in32 = mk_ext(2, 48000, 32, 24, false, 8);
        ck(pick_shared_writer((const WAVEFORMATEX*)&w24in32) == SharedSampleFmt::Int24In32,
           "48000/32容器+24有效 EXTENSIBLE → int24(24-in-32)",
           shared_fmt_name(pick_shared_writer((const WAVEFORMATEX*)&w24in32)));
        // 同一格式的"裸"写法：wBitsPerSample=24 但 blockAlign/ch=4 —— 容器以 blockAlign 为准
        const auto w24in32b = mk_plain(WAVE_FORMAT_PCM, 2, 96000, 24, 8);
        ck(pick_shared_writer(&w24in32b) == SharedSampleFmt::Int24In32,
           "96000/24 bits + blockAlign=4B/sample → int24(24-in-32)（容器信 blockAlign）");

        // ③ int32
        const auto w32_ext = mk_ext(2, 48000, 32, 32, false, 8);
        ck(pick_shared_writer((const WAVEFORMATEX*)&w32_ext) == SharedSampleFmt::Int32,
           "48000/32 EXTENSIBLE PCM → int32");
        const auto w32_plain = mk_plain(WAVE_FORMAT_PCM, 1, 48000, 32, 4);
        ck(pick_shared_writer(&w32_plain) == SharedSampleFmt::Int32, "48000/32 PCM 单声道 → int32");

        // ④ 紧凑 24（3 字节容器）—— 额外兼容项
        const auto w24c = mk_plain(WAVE_FORMAT_PCM, 2, 44100, 24, 6);
        ck(pick_shared_writer(&w24c) == SharedSampleFmt::Int24, "44100/24 紧凑(3B) → int24(compact-3)");

        // 拒绝项：不能"猜"着写，必须如实 Unsupported
        const auto w8 = mk_plain(WAVE_FORMAT_PCM, 2, 44100, 8, 2);
        ck(pick_shared_writer(&w8) == SharedSampleFmt::Unsupported, "8-bit PCM → unsupported");
        const auto w64f = mk_plain(WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 64, 16);
        ck(pick_shared_writer(&w64f) == SharedSampleFmt::Unsupported, "64-bit float → unsupported");
        const auto wulaw = mk_plain(0x0007 /* WAVE_FORMAT_MULAW */, 1, 8000, 8, 1);
        ck(pick_shared_writer(&wulaw) == SharedSampleFmt::Unsupported, "µ-law → unsupported");
        auto wbad = mk_ext(2, 48000, 32, 32, false, 8);
        std::memset(&wbad.SubFormat, 0, sizeof(GUID));
        ck(pick_shared_writer((const WAVEFORMATEX*)&wbad) == SharedSampleFmt::Unsupported,
           "EXTENSIBLE + 未知 SubFormat → unsupported");
        auto wshort = mk_plain(WAVE_FORMAT_EXTENSIBLE, 2, 48000, 32, 8);
        wshort.cbSize = 0;   // 畸形：EXTENSIBLE 却缺扩展块
        ck(pick_shared_writer(&wshort) == SharedSampleFmt::Unsupported,
           "EXTENSIBLE(cbSize=0) 畸形 → unsupported");
        ck(pick_shared_writer(nullptr) == SharedSampleFmt::Unsupported, "nullptr → unsupported");

        // 元数据一致性：字节数 / 有效位深与格式一一对应
        ck(shared_fmt_bytes(SharedSampleFmt::Float32) == 4 && shared_fmt_bytes(SharedSampleFmt::Int16) == 2 &&
           shared_fmt_bytes(SharedSampleFmt::Int24In32) == 4 && shared_fmt_bytes(SharedSampleFmt::Int32) == 4 &&
           shared_fmt_bytes(SharedSampleFmt::Int24) == 3,
           "shared_fmt_bytes：4/2/4/4/3");
        ck(shared_fmt_valid_bits(SharedSampleFmt::Int16) == 16 &&
           shared_fmt_valid_bits(SharedSampleFmt::Int24In32) == 24 &&
           shared_fmt_valid_bits(SharedSampleFmt::Int32) == 32,
           "shared_fmt_valid_bits：16/24/32");
    }

    // ============================ §1.1 单样本字节 ============================
    std::printf("\n-- §1.1 put_shared_sample：浮点 → 目标容器字节（小端） --\n");
    {
        // int16：0.5 → 16384 = 0x4000 → 00 40
        ck(f64_to_s16(0.5) == 16384, "f64_to_s16(0.5) == 16384",
           std::to_string((int)f64_to_s16(0.5)));
        ck_sample(SharedSampleFmt::Int16,  0.5,  {0x00, 0x40},             "int16  0.5  → 00 40");
        ck_sample(SharedSampleFmt::Int16,  1.0,  {0xFF, 0x7F},             "int16  +满幅 → FF 7F (32767)");
        ck_sample(SharedSampleFmt::Int16, -1.0,  {0x00, 0x80},             "int16  −满幅 → 00 80 (-32768)");
        ck_sample(SharedSampleFmt::Int16,  2.0,  {0xFF, 0x7F},             "int16  +2.0 越界 → 饱和 32767");
        ck_sample(SharedSampleFmt::Int16, -2.0,  {0x00, 0x80},             "int16  −2.0 越界 → 饱和 -32768");
        ck_sample(SharedSampleFmt::Int16,  0.0,  {0x00, 0x00},             "int16  0.0  → 00 00");
        ck_sample(SharedSampleFmt::Int16, -0.5,  {0x00, 0xC0},             "int16  -0.5 → 00 C0 (-16384)");

        // int24 in 32：24 位有效值左移 8 位，低字节恒 0
        ck(f64_to_s24_in_32(0.5) == 1073741824, "f64_to_s24_in_32(0.5) == 1073741824 (0x40000000)",
           std::to_string(f64_to_s24_in_32(0.5)));
        ck(f64_to_s24_in_32(0.5) == (int32_t)((int64_t)f64_to_s24(0.5) * 256),
           "24-in-32 == s24 值左移 8 位（移位语义）");
        ck_sample(SharedSampleFmt::Int24In32,  0.5, {0x00, 0x00, 0x00, 0x40}, "int24in32  0.5  → 00 00 00 40");
        ck_sample(SharedSampleFmt::Int24In32,  1.0, {0x00, 0xFF, 0xFF, 0x7F}, "int24in32 +满幅 → 00 FF FF 7F");
        ck_sample(SharedSampleFmt::Int24In32, -1.0, {0x00, 0x00, 0x00, 0x80}, "int24in32 −满幅 → 00 00 00 80");
        ck_sample(SharedSampleFmt::Int24In32,  2.0, {0x00, 0xFF, 0xFF, 0x7F}, "int24in32 +2.0 越界 → 饱和");
        ck_sample(SharedSampleFmt::Int24In32, -2.0, {0x00, 0x00, 0x00, 0x80}, "int24in32 −2.0 越界 → 饱和");
        {   // 低 8 位必须是 0（24-in-32 的全部意义所在）
            bool lowZero = true;
            unsigned char b[4];
            for (int i = -1000; i <= 1000; ++i) {
                put_shared_sample(SharedSampleFmt::Int24In32, i / 1000.0, b);
                if (b[0] != 0) { lowZero = false; break; }
            }
            ck(lowZero, "int24in32 任意值低 8 位恒 0（2001 个样本扫描）");
        }

        // int32
        ck(f64_to_s32(0.5) == 1073741824, "f64_to_s32(0.5) == 1073741824");
        ck(f64_to_s32(1.0) == 2147483647, "f64_to_s32(+1.0) == INT32_MAX（不溢出）");
        ck(f64_to_s32(-1.0) == std::numeric_limits<int32_t>::min(), "f64_to_s32(−1.0) == INT32_MIN");
        ck_sample(SharedSampleFmt::Int32,  0.5, {0x00, 0x00, 0x00, 0x40}, "int32  0.5  → 00 00 00 40");
        ck_sample(SharedSampleFmt::Int32,  1.0, {0xFF, 0xFF, 0xFF, 0x7F}, "int32  +满幅 → FF FF FF 7F");
        ck_sample(SharedSampleFmt::Int32, -1.0, {0x00, 0x00, 0x00, 0x80}, "int32  −满幅 → 00 00 00 80");

        // 紧凑 24
        ck_sample(SharedSampleFmt::Int24,  0.5, {0x00, 0x00, 0x40}, "int24(3B)  0.5  → 00 00 40");
        ck_sample(SharedSampleFmt::Int24,  1.0, {0xFF, 0xFF, 0x7F}, "int24(3B)  +满幅 → FF FF 7F");
        ck_sample(SharedSampleFmt::Int24, -1.0, {0x00, 0x00, 0x80}, "int24(3B)  −满幅 → 00 00 80");

        // float32
        ck_sample(SharedSampleFmt::Float32, 0.5, {0x00, 0x00, 0x00, 0x3F}, "float32 0.5 → 00 00 00 3F");
        ck_sample(SharedSampleFmt::Float32, 1.0, {0x00, 0x00, 0x80, 0x3F}, "float32 +1.0 → 00 00 80 3F");
        ck_sample(SharedSampleFmt::Float32, -1.0, {0x00, 0x00, 0x80, 0xBF}, "float32 −1.0 → 00 00 80 BF");
        ck_sample(SharedSampleFmt::Float32, 1.5, {0x00, 0x00, 0x80, 0x3F}, "float32 +1.5 越界 → 钳到 1.0");

        // NaN / Inf → 0（上游 bug 不能变成满幅直流）
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        ck(f64_to_s16(nan) == 0 && f64_to_s32(nan) == 0 && f64_to_s24_in_32(inf) == 0,
           "NaN/Inf → 0（int16/int32/int24in32）");
        ck_sample(SharedSampleFmt::Float32, nan, {0x00, 0x00, 0x00, 0x00}, "float32 NaN → 0.0");
        // 非有限值统一当 0（与整型路径同口径）：Inf 不是"非常满幅"，写满幅会把上游 bug
        // 变成直流/爆音。这条与 f64_to_signed_bits 的 !isfinite 分支保持一致。
        ck_sample(SharedSampleFmt::Float32, -inf, {0x00, 0x00, 0x00, 0x00}, "float32 −Inf → 0.0");
        ck_sample(SharedSampleFmt::Float32, inf, {0x00, 0x00, 0x00, 0x00}, "float32 +Inf → 0.0");
    }

    // ============================ §1.2 数值换算 ============================
    std::printf("\n-- §1.2 量化器饱和与往返精度 --\n");
    {
        // int16 全域往返：x = i/32768 → f64_to_s16(x) 必须精确还原 i（无 off-by-one）
        bool exact = true;
        for (int32_t i = -32768; i <= 32767; ++i) {
            if ((int32_t)f64_to_s16((double)i / 32768.0) != i) { exact = false; break; }
        }
        ck(exact, "int16 全域 65536 点往返精确（x=i/32768 → i）");

        // int24 抽样往返（步长取质数，覆盖正负与边界）
        bool exact24 = true;
        for (int32_t i = -8388608; i <= 8388607; i += 32749) {
            const double x = (double)i / 8388608.0;
            if (f64_to_s24(x) != i || f64_to_s24_in_32(x) != (int32_t)((int64_t)i * 256)) {
                exact24 = false; break;
            }
        }
        ck(exact24, "int24 抽样往返精确（含 24-in-32 移位）");

        // 边界：量化后绝不越界（-2^31 的转换是安全的，不依赖 UB）
        ck(f64_to_s32(-1.0000001) == std::numeric_limits<int32_t>::min(), "−1.0000001 → INT32_MIN（不 UB）");
        ck(f64_to_s32(1.0000001) == std::numeric_limits<int32_t>::max(), "+1.0000001 → INT32_MAX");
        ck(f64_to_s16(-32768.0 / 32768.0) == -32768 && f64_to_s16(32767.0 / 32768.0) == 32767,
           "−32768/32768 与 32767/32768 各自还原");
    }

    // ==================== §1.3 管线 int32 → 字节（整链） ====================
    std::printf("\n-- §1.3 put_shared_sample_from_s32：位深归一化 + 装箱 --\n");
    {
        // 16-bit 管线：0.5 = 2^14
        unsigned char b[8];
        std::memset(b, 0xEE, sizeof(b));
        put_shared_sample_from_s32(SharedSampleFmt::Int16, 1 << 14, 16, b);
        ck(bytes_eq(b, {0x00, 0x40}), "16-bit 管线 0.5 → int16 16384", hexs(b, 2));
        // 24-bit 管线：0.5 = 2^22 —— 这一条防的正是 2026-10-23 那个 −48.16 dB 缺陷
        std::memset(b, 0xEE, sizeof(b));
        put_shared_sample_from_s32(SharedSampleFmt::Int24In32, 1 << 22, 24, b);
        ck(bytes_eq(b, {0x00, 0x00, 0x00, 0x40}), "24-bit 管线 0.5 → int24in32 0x40000000", hexs(b, 4));
        // 32-bit 管线：0.5 = 2^30
        std::memset(b, 0xEE, sizeof(b));
        put_shared_sample_from_s32(SharedSampleFmt::Int32, 1 << 30, 32, b);
        ck(bytes_eq(b, {0x00, 0x00, 0x00, 0x40}), "32-bit 管线 0.5 → int32 1073741824", hexs(b, 4));
        // 24-bit 管线满幅 → int16 饱和
        std::memset(b, 0xEE, sizeof(b));
        put_shared_sample_from_s32(SharedSampleFmt::Int16, (1 << 23) - 1, 24, b);
        ck(bytes_eq(b, {0xFF, 0x7F}), "24-bit 管线满幅 → int16 饱和 32767", hexs(b, 2));
        // 24-bit 管线负满幅 → int16 −32768
        std::memset(b, 0xEE, sizeof(b));
        put_shared_sample_from_s32(SharedSampleFmt::Int16, -(1 << 23), 24, b);
        ck(bytes_eq(b, {0x00, 0x80}), "24-bit 管线负满幅 → int16 -32768", hexs(b, 2));
        // 管线 overshoot（> 满幅）→ 饱和而不是回绕
        std::memset(b, 0xEE, sizeof(b));
        put_shared_sample_from_s32(SharedSampleFmt::Int24In32, (1 << 23) + 12345, 24, b);
        ck(bytes_eq(b, {0x00, 0xFF, 0xFF, 0x7F}), "管线 overshoot → int24in32 饱和（不回绕）", hexs(b, 4));
    }

    // ---- §1.3b 整块交错装箱（生产 pack() 走的正是这个函数，不是"测试另抄一份"）----
    std::printf("\n-- §1.3b shared_pack_int32 / shared_pack_f32：交错步长与声道交织 --\n");
    {
        unsigned char b[32];
        // 16-bit 管线、2 声道 2 帧：{−16384, +16384, +16384, −16384}
        const int32_t f16[4] = { -16384, 16384, 16384, -16384 };
        std::memset(b, 0xEE, sizeof(b));
        const double pk16 = shared_pack_int32(SharedSampleFmt::Int16, 2, 16, 1.0, b, f16, 2, 0.0);
        ck(bytes_eq(b, {0x00, 0xC0, 0x00, 0x40, 0x00, 0x40, 0x00, 0xC0}),
           "int16 2帧2声道交错 → 8 字节", hexs(b, 8));
        ck(std::fabs(pk16 - 0.5) < 1e-12, "int16 块装箱峰值 = 0.5（归一化域 post-gain）",
           std::to_string(pk16));
        ck(b[8] == 0xEE, "块装箱不越界写（第 9 字节仍是毒值）");

        // int24-in-32：24-bit 管线，2 声道 2 帧（0.5 / −0.5 / 0 / +满幅）
        const int32_t f24[4] = { 1 << 22, -(1 << 22), 0, (1 << 23) - 1 };
        std::memset(b, 0xEE, sizeof(b));
        shared_pack_int32(SharedSampleFmt::Int24In32, 2, 24, 1.0, b, f24, 2, 0.0);
        ck(bytes_eq(b, {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F}),
           "int24in32 2帧2声道交错 → 16 字节", hexs(b, 16));

        // int32：32-bit 管线，单声道 2 帧
        const int32_t f32v[2] = { 1 << 30, -(1 << 30) };
        std::memset(b, 0xEE, sizeof(b));
        shared_pack_int32(SharedSampleFmt::Int32, 1, 32, 1.0, b, f32v, 2, 0.0);
        ck(bytes_eq(b, {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0}),
           "int32 2帧单声道交错 → 8 字节", hexs(b, 8));

        // 增益 +6 dB：0.5 × 2 = 1.0 ⟹ 饱和（不是回绕）
        const int32_t half[1] = { 16384 };
        std::memset(b, 0xEE, sizeof(b));
        shared_pack_int32(SharedSampleFmt::Int16, 1, 16, 2.0, b, half, 1, 0.0);
        ck(bytes_eq(b, {0xFF, 0x7F}), "int16 增益 +6dB 后 1.0 → 饱和 32767", hexs(b, 2));

        // float32 输入（直通监听那条路）：0.5 / −0.5 / 0.25 / −0.25
        const float ff[4] = { 0.5f, -0.5f, 0.25f, -0.25f };
        std::memset(b, 0xEE, sizeof(b));
        shared_pack_f32(SharedSampleFmt::Int16, 2, 1.0, b, ff, 2);
        ck(bytes_eq(b, {0x00, 0x40, 0x00, 0xC0, 0x00, 0x20, 0x00, 0xE0}),
           "float32 输入 → int16 2帧2声道", hexs(b, 8));
        const float hf[1] = { 0.5f };
        std::memset(b, 0xEE, sizeof(b));
        shared_pack_f32(SharedSampleFmt::Int24In32, 1, 1.0, b, hf, 1);
        ck(bytes_eq(b, {0x00, 0x00, 0x00, 0x40}), "float32 输入 → int24in32 0.5", hexs(b, 4));
    }

    // ---- §1.3c 独占容器装箱（2026-10-25 外部评审 P0-A：电平 −48 dBFS）----
    // 被测对象 = 生产 WasapiOutput::pack() 直接调用的 ex_pack_int32 / ex_put_sample，
    // 不是测试另抄一份。判据设计（铁律 3）：
    //   · 每条都断言「0.5 输入 ⟹ 0.5 的样本值」，并显式算出电平（dBFS），
    //     因为旧实现的错误恰好是"结构完全正常、只是电平少了 48 dB"，肉眼/结构断言抓不到；
    //   · 同时断言旧口径（×1/8388608、原样搬 src）会给出什么值 —— 防回归也防判据误写；
    //   · 逐样本扫描（16-bit 全域 65536、24-bit 全域 16777216）而不是抽样；
    //   · 末尾与共享路径 shared_pack_int32 逐字节对照：两份实现不许悄悄漂移。
    std::printf("\n-- §1.3c 独占容器装箱 ex_pack_int32：请求位深 × 容器有效位 --\n");
    {
        using rta::audio::ExContainer;
        using rta::audio::ex_pack_int32;
        using rta::audio::ex_put_sample;
        using rta::audio::ex_normalize;

        const ExContainer cF32  {4, 32, true };    // EXT32/32 float（TASCAM 的 16-bit 兜底容器）
        const ExContainer cPcm16{2, 16, false};    // PCM16 / EXT16
        const ExContainer cC24  {3, 24, false};    // EXT24/24 紧凑（3 字节）
        const ExContainer cI24  {4, 24, false};    // EXT32容器/24有效（24-in-32，左对齐）
        const ExContainer cI32  {4, 32, false};    // EXT32/32 int
        unsigned char b[32];

        auto f32_at = [](const unsigned char* p) { float f; std::memcpy(&f, p, 4); return f; };
        auto i32_at = [](const unsigned char* p) { uint32_t u; std::memcpy(&u, p, 4); return (int32_t)u; };
        auto dbfs = [](double x) { return 20.0 * std::log10(x > 0.0 ? x : 1e-30); };

        // ---- (a) 16-bit 请求 → float32 容器（TASCAM 实测路径；旧实现 −48.16 dBFS）----
        const int32_t s16_half[1] = { 16384 };                 // 16-bit 域的 0.5
        const int32_t s16_full[1] = { 32767 };
        const int32_t s16_min[1]  = { -32768 };
        std::memset(b, 0xEE, sizeof(b));
        ex_pack_int32(cF32, 1, 16, 1.0, b, s16_half, 1);
        ck(f32_at(b) == 0.5f, "16-bit 0.5 → float32 容器 = 0.5（不是 0.5/32768）",
           std::to_string(f32_at(b)));
        ck(f32_at(b) != 16384.0f / 8388608.0f, "16-bit → float32 不再是旧口径 ×1/8388608",
           std::to_string(f32_at(b)) + " vs 旧 " + std::to_string(16384.0f / 8388608.0f));
        ck(b[4] == 0xEE, "float32 容器不越界写（第 5 字节仍是毒值）");

        ex_pack_int32(cF32, 1, 16, 1.0, b, s16_full, 1);
        const double db16 = dbfs((double)f32_at(b));
        ck(std::fabs(db16) < 0.001, "16-bit 满幅 → 0.00 dBFS（旧实现为 −48.16 dBFS）",
           "实测 " + std::to_string(db16) + " dBFS / 样本 " + std::to_string(f32_at(b)));
        ck(std::fabs(db16 - (-48.16)) > 5.0, "16-bit 满幅不再落在 −48 dBFS 附近",
           std::to_string(db16) + " dBFS");

        ex_pack_int32(cF32, 1, 16, 1.0, b, s16_min, 1);
        ck(f32_at(b) == -1.0f, "16-bit 负满幅 → float32 = −1.0（满幅不塌）", std::to_string(f32_at(b)));

        // 负向对照（判据本体）：把旧口径的电平也算出来钉死，证明"改掉的正是 −48.16 dBFS"，
        // 而不是"随便断言了一个数"。旧公式 = src × 1/8388608（24-bit 定标）。
        const double oldDb16 = dbfs(32767.0 / 8388608.0);
        ck(std::fabs(oldDb16 - (-48.1648)) < 0.01,
           "旧口径 16-bit 满幅 = −48.16 dBFS（缺陷量级对照）", std::to_string(oldDb16) + " dBFS");
        const double oldDb24 = dbfs(8388607.0 / 2147483648.0);
        ck(std::fabs(oldDb24 - (-48.1648)) < 0.01,
           "旧口径 24-bit 写进 32 位容器 = −48.16 dBFS（缺陷量级对照）", std::to_string(oldDb24) + " dBFS");

        // ---- (b) 24-bit 请求 → EXT32容器/24有效 与 EXT32/32 int（同为 4 字节，定标差 8 位）----
        const int32_t s24_half[1] = { 4194304 };               // 24-bit 域的 0.5 = 2^22
        const int32_t s24_full[1] = { 8388607 };
        const int32_t s24_min[1]  = { -8388608 };
        ex_pack_int32(cI24, 1, 24, 1.0, b, s24_half, 1);
        ck(i32_at(b) == 1073741824, "24-bit 0.5 → EXT32容器/24有效 = 0x40000000（左移 8 位）",
           hexs(b, 4));
        ck(i32_at(b) != 4194304, "24-bit → 24-in-32 不再是旧口径原样搬（0x00400000）", hexs(b, 4));
        ex_pack_int32(cI32, 1, 24, 1.0, b, s24_half, 1);
        ck(i32_at(b) == 1073741824, "24-bit 0.5 → EXT32/32 int = 0x40000000（占满 32 位域）",
           hexs(b, 4));
        ex_pack_int32(cI32, 1, 24, 1.0, b, s24_full, 1);
        ck(i32_at(b) == 2147483392 && (i32_at(b) & 0xFF) == 0,
           "24-bit 满幅 → 0x7FFFFF00（低 8 位恒 0）", hexs(b, 4));
        ex_pack_int32(cI24, 1, 24, 1.0, b, s24_full, 1);
        ck(i32_at(b) == 2147483392, "24-in-32 满幅 → 0x7FFFFF00（同一左对齐规则）", hexs(b, 4));
        ex_pack_int32(cI24, 1, 24, 1.0, b, s24_min, 1);
        ck(i32_at(b) == INT32_MIN, "24-bit 负满幅 → 0x80000000", hexs(b, 4));

        // ---- (c) 32-bit 请求 → float32 容器（旧实现同样 −48.16 dBFS）----
        const int32_t s32_half[1] = { 1073741824 };
        ex_pack_int32(cF32, 1, 32, 1.0, b, s32_half, 1);
        ck(f32_at(b) == 0.5f, "32-bit 0.5 → float32 容器 = 0.5", std::to_string(f32_at(b)));

        // ---- (d) 位深与容器一致时，输出必须与旧实现【逐位相同】（不能修出回归）----
        const int32_t s16_neg[1] = { -16384 };
        ex_pack_int32(cPcm16, 1, 16, 1.0, b, s16_neg, 1);
        ck(bytes_eq(b, {0x00, 0xC0}), "16-bit −0.5 → PCM16 = 0xC000（与旧实现逐位一致）", hexs(b, 2));
        ex_pack_int32(cC24, 1, 24, 1.0, b, s24_half, 1);
        ck(bytes_eq(b, {0x00, 0x00, 0x40}), "24-bit 0.5 → 紧凑 24 = 00 00 40（3 字节）", hexs(b, 3));
        ex_pack_int32(cI32, 1, 32, 1.0, b, s32_half, 1);
        ck(i32_at(b) == 1073741824, "32-bit 0.5 → EXT32/32 int 原样", hexs(b, 4));

        // ---- (e) 交错步长：2 声道 2 帧（旧实现的快速/慢速两条分支都要按容器宽度步进）----
        {
            const int32_t inter[4] = { 4194304, -4194304, 0, 8388607 };
            std::memset(b, 0xEE, sizeof(b));
            ex_pack_int32(cI24, 2, 24, 1.0, b, inter, 2);
            ck(bytes_eq(b, {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F}),
               "24→24-in-32 2帧2声道交错 → 16 字节", hexs(b, 16));
            ck(b[16] == 0xEE, "块装箱不越界写（第 17 字节仍是毒值）");
        }

        // ---- (f) 增益：乘法域必须与容器无关（旧实现慢路径的 float32 定标同样写死）----
        ex_pack_int32(cF32, 1, 16, 2.0, b, s16_half, 1);
        ck(f32_at(b) == 1.0f, "增益 ×2：16-bit 0.5 → float32 1.0", std::to_string(f32_at(b)));
        ex_pack_int32(cF32, 1, 16, 4.0, b, s16_half, 1);
        ck(f32_at(b) == 1.0f, "增益 ×4：0.5 → 饱和 1.0（不回绕）", std::to_string(f32_at(b)));
        ex_pack_int32(cI24, 1, 24, 4.0, b, s24_half, 1);
        ck(i32_at(b) == 2147483392, "增益 ×4：24-bit 0.5 → 饱和 0x7FFFFF00", hexs(b, 4));

        // ---- (g) 逐样本扫描（不是抽样）----
        {
            int bad = 0;
            for (int v = -32768; v <= 32767; ++v) {
                unsigned char t[4];
                ex_put_sample(cF32, ex_normalize(v, 16, 1.0), t);
                if (f32_at(t) != (float)((double)v / 32768.0)) ++bad;
            }
            ck(bad == 0, "16-bit 全域 65536 码值 → float32 逐样本相等",
               "不符 " + std::to_string(bad));
        }
        {
            int bad = 0;
            for (int64_t v = -8388608; v <= 8388607; ++v) {
                unsigned char t[4];
                ex_put_sample(cI24, ex_normalize((int32_t)v, 24, 1.0), t);
                if (i32_at(t) != (int32_t)(v * 256)) ++bad;
            }
            ck(bad == 0, "24-bit 全域 16777216 码值 → EXT32/24有效 逐样本相等",
               "不符 " + std::to_string(bad));
        }

        // 取证汇总（现场实测值，不是把断言里的常数再打印一遍）
        {
            unsigned char m[4];
            ex_pack_int32(cF32, 1, 16, 1.0, m, s16_half, 1);
            const double m16half = (double)f32_at(m);
            ex_pack_int32(cF32, 1, 16, 1.0, m, s16_full, 1);
            const double m16fullDb = dbfs((double)f32_at(m));
            ex_pack_int32(cI24, 1, 24, 1.0, m, s24_half, 1);
            const int32_t m24half = i32_at(m);
            ex_pack_int32(cI32, 1, 24, 1.0, m, s24_full, 1);
            const int32_t m24full = i32_at(m);
            std::printf("  [INFO] 16→float32 : 0.5 → %.9f（旧源码实测 0.001953125）；满幅 → %.2f dBFS（旧 −48.17）\n",
                        m16half, m16fullDb);
            std::printf("  [INFO] 24→EXT32int: 0.5 → 0x%08X（旧源码实测 0x%08X）；满幅 → 0x%08X\n",
                        (unsigned)m24half, 4194304u, (unsigned)m24full);
        }

        // ---- (h) 与共享路径逐字节对照（两份实现的"同一份口径"由这里钉住）----
        {
            struct Pair { ExContainer ex; SharedSampleFmt sh; const char* name; };
            const Pair pairs[] = {
                { {4, 32, true },  SharedSampleFmt::Float32,   "float32" },
                { {2, 16, false},  SharedSampleFmt::Int16,     "int16" },
                { {3, 24, false},  SharedSampleFmt::Int24,     "int24 紧凑" },
                { {4, 24, false},  SharedSampleFmt::Int24In32, "24-in-32" },
                { {4, 32, false},  SharedSampleFmt::Int32,     "int32" },
            };
            const int bitsSet[3] = { 16, 24, 32 };
            const double gains[3] = { 1.0, 0.5, 2.0 };
            int pairsChecked = 0, mismatches = 0;
            std::string firstBad;
            for (const auto& pr : pairs) {
                for (int bi = 0; bi < 3; ++bi) {
                    const int sb = bitsSet[bi];
                    const double denom = (sb >= 32) ? 2147483648.0 : (double)(1LL << (sb - 1));
                    int32_t src[64];
                    for (int i = 0; i < 64; ++i) {
                        const double x = (i - 32) / 31.0;                  // ≈ ±1.03（含越界样本）
                        src[i] = (int32_t)(x > 1.0 ? 1.0 : (x < -1.0 ? -1.0 : x) * denom);
                    }
                    for (double g : gains) {
                        unsigned char ba[256], bb[256];
                        std::memset(ba, 0xEE, sizeof(ba));
                        std::memset(bb, 0xEE, sizeof(bb));
                        ex_pack_int32(pr.ex, 2, sb, g, ba, src, 32);
                        shared_pack_int32(pr.sh, 2, sb, g, bb, src, 32, 0.0);
                        const int nb = shared_fmt_bytes(pr.sh) * 64;
                        ++pairsChecked;
                        if (std::memcmp(ba, bb, (size_t)nb) != 0) {
                            ++mismatches;
                            if (firstBad.empty())
                                firstBad = std::string(pr.name) + " srcBits=" + std::to_string(sb) +
                                           " gain=" + std::to_string(g) + " 独占=" + hexs(ba, nb) +
                                           " 共享=" + hexs(bb, nb);
                        }
                    }
                }
            }
            ck(mismatches == 0,
               "独占 ex_pack_int32 与共享 shared_pack_int32 逐字节一致（5 容器 × 3 源位深 × 3 增益）",
               "对照 " + std::to_string(pairsChecked) + " 组，不符 " + std::to_string(mismatches) +
               (firstBad.empty() ? "" : "；首例 " + firstBad));
        }
    }

    // ==================== §1.4 老回归：int32 → float32 ====================
    std::printf("\n-- §1.4 老回归：s32_to_f32 / s32_to_f32_bits --\n");
    ck(s32_to_f32(0) == 0.0, "s32_to_f32(0) == 0");
    const double dMin = s32_to_f32(INT32_MIN);
    ck(std::fabs(dMin + 1.0) < 1e-12, "s32_to_f32(INT32_MIN) == -1.0", std::to_string(dMin));
    const double dMax = s32_to_f32(INT32_MAX);
    ck(std::fabs(dMax - 0.999999999534338713) < 1e-15, "s32_to_f32(INT32_MAX) == 1-2^-31", std::to_string(dMax));
    ck(dMax < 1.0, "s32_to_f32(INT32_MAX) < 1.0（float 域不含 +1.0 满幅）");
    const double dPlus1 = s32_to_f32(1), dMinus1 = s32_to_f32(-1);
    ck(std::fabs(dPlus1 - 1.0 / 2147483648.0) < 1e-18, "s32_to_f32(1) == 2^-31", std::to_string(dPlus1));
    ck(std::fabs(dMinus1 + 1.0 / 2147483648.0) < 1e-18, "s32_to_f32(-1) == -2^-31", std::to_string(dMinus1));
    ck(s32_to_f32(1073741824) == 0.5, "s32_to_f32(2^30) == 0.5");
    ck(s32_to_f32(-1073741824) == -0.5, "s32_to_f32(-2^30) == -0.5");

    // 按源位深归一化（2026-10-23 现场修复的防回归）
    // 缺陷形态：管线交来的是「按 bits 右对齐」的 int32（24-bit 满幅 = ±2^23），
    // 旧实现一律 ÷2^31 ⟹ 输出只有 1/256 = −48.16 dB。用户听到"能放但极小声"，
    // 而内部 peak/clip 判据全按位深归一化、全部显示正常（判据掩盖缺陷）。
    {
        using rta::audio::s32_to_f32_bits;
        const int32_t p24 = (1 << 23) - 1;
        ck(std::fabs(s32_to_f32_bits(p24, 24) - 1.0) < 1e-6, "24-bit 满幅 → ≈1.0");
        ck(std::fabs(s32_to_f32_bits(-(1 << 23), 24) + 1.0) < 1e-12, "24-bit 负满幅 → −1.0");
        ck(s32_to_f32(p24) < 0.004, "旧路径 ÷2^31 会把 24-bit 满幅压到 0.0039（−48.16 dB）");
        const int32_t p16 = (1 << 15) - 1;
        ck(std::fabs(s32_to_f32_bits(p16, 16) - 1.0) < 1e-4, "16-bit 满幅 → ≈1.0");
        ck(s32_to_f32_bits(INT32_MAX, 32) == s32_to_f32(INT32_MAX), "32-bit 路径保持原语义");
        ck(s32_to_f32_bits(0, 24) == 0.0, "0 恒为 0");
        ck(s32_to_f32_bits((1 << 23) + 1000, 24) == 1.0, "24-bit overshoot → clamp 到 1.0");
        ck(s32_to_f32_bits(-(1 << 23) - 1000, 24) == -1.0, "24-bit 负 overshoot → clamp 到 −1.0");
    }

    // ============================ §1.5 可写帧数钳制 ============================
    std::printf("\n-- §1.5 shared_writable 钳制 --\n");
    ck(shared_writable(9600, 0, 5000) == 5000, "pad=0 → 写满可用量");
    ck(shared_writable(9600, 0, 20000) == 9600, "pad=0 → 不超过缓冲深度");
    ck(shared_writable(9600, 9600, 20000) == 0, "pad=缓冲深度 → 0（背压）");
    ck(shared_writable(9600, 4600, 10000) == 5000, "pad 部分占用 → 补满剩余");
    ck(shared_writable(9600, 4600, 100) == 100, "可用数据不足 → 夹到数据量");
    ck(shared_writable(9600, 20000, 100) == 0, "pad 异常超界 → 钳到 0（不下溢成巨数）");

    // ---- §1.6 离线可测的 query 失败形态：不存在端点 → 错误非空 ----
    {
        uint32_t r = 0; int ch = 0; bool f = true; std::string err;
        SharedSampleFmt qf = SharedSampleFmt::Float32;
        const bool ok = rta::audio::WasapiSharedOutput::query_mix_format(
            "{00000000-0000-0000-0000-000000000000}", r, ch, f, err, &qf);
        // 不存在的端点必须失败且错误可读（不存在 → rate 保持 0）。本机必有端点，
        // 但构造的假 id 一定不存在 —— 这条是报错路径在本环境唯一能安全覆盖的分支。
        ck(!ok && !err.empty() && r == 0, "query_mix_format(不存在端点) 报错且错误非空", err);
        ck(qf == SharedSampleFmt::Unsupported, "query_mix_format 失败时 outFmt 复位为 unsupported");
    }

    // ---- §2 设备 smoke：只写数字静音，≤1 秒，先打印横幅 ----
    if (offlineOnly) {
        std::printf("\n== wasapi_shared：§2 设备 smoke ==\n");
        std::printf("  [SKIP] --offline / RTA_NO_DEVICE=1：只跑 §1 离线部分，不打开任何音频设备\n");
        std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "wasapi_shared 离线部分全部通过" : "存在失败", g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    std::printf("\n== wasapi_shared：§2 设备 smoke ==\n");
    std::string devName = "(未知)", devId;
    bool foundWanted = wantDev.empty();
    {
        for (const auto& d : rta::audio::WasapiOutput::enumerate()) {
            if (d.state != DEVICE_STATE_ACTIVE) continue;
            if (!wantDev.empty()) {
                // 显式指定端点：名字子串或完整 id 命中即用（找不到就判失败，不静默退回默认设备 ——
                // 否则"我验证的是 N300"这句话就成了假证据）
                if (d.name.find(wantDev) != std::string::npos || d.id == wantDev) {
                    devName = d.name; devId = d.id; foundWanted = true; break;
                }
                continue;
            }
            devName = d.name; devId = d.id;
            if (d.isDefault) break;
        }
    }
    if (!foundWanted) {
        std::printf("  [FAIL] 未找到指定端点：%s（可用端点见 wasapi_probe / mixdump）\n", wantDev.c_str());
        ++g_fail;
        devName = "(未找到)"; devId.clear();
    } else if (!wantDev.empty()) {
        std::printf("  [INFO] 显式指定端点：%s\n", wantDev.c_str());
    }
    std::printf("***************************************************************\n");
    std::printf("** 听感提示：本测试即将向音频设备写入 0.5 秒【数字静音】      **\n");
    std::printf("**   目标设备: %s\n", devName.c_str());
    std::printf("**   时长    : ≤1 秒（写 0.5 s 静音 + ≤0.9 s 观察）           **\n");
    std::printf("**   电平    : 全零样本（数字静音，−∞ dBFS，不可能听到声音）  **\n");
    std::printf("***************************************************************\n");

    if (devId.empty() && devName == "(未知)") {
        // 保守判定：枚举不到任何 ACTIVE 端点。按项目门禁哲学（缺环境=失败，不静默跳过），
        // 但给出明确原因。
        ck(false, "存在 ACTIVE 输出端点", "未枚举到任何输出端点，无法做设备 smoke");
    } else {
        // 先只读探测一次混音格式（不开流），并与 open 之后的对象对照 ——
        // 两条路（query_mix_format / open）必须给出同一个格式，否则说明判据分叉了。
        uint32_t qr = 0; int qch = 0; bool qf = false; std::string qerr;
        SharedSampleFmt qfmt = SharedSampleFmt::Unsupported;
        const bool qok = rta::audio::WasapiSharedOutput::query_mix_format(devId, qr, qch, qf, qerr, &qfmt);
        ck(qok, "query_mix_format 成功", qerr);
        std::printf("  [INFO] 本机系统混音格式 = %u Hz / %d ch / %s（%d bit，%d 字节/样本）\n",
                    (unsigned)qr, qch, rta::audio::shared_fmt_name(qfmt),
                    rta::audio::shared_fmt_valid_bits(qfmt), rta::audio::shared_fmt_bytes(qfmt));
        ck(qok && qf, "isFloat 出参 = 受支持（多格式扩展后的语义）", qerr);

        rta::audio::WasapiSharedOutput so;
        bool opened = false;
        try {
            opened = so.open(devId, qr ? qr : 48000, qch ? qch : 2, 32, 200.0);
        } catch (const std::exception& e) {
            ck(false, "open 不抛异常", e.what());
        }
        if (!opened) {
            // 2026-10-23：设备被别的程序占用（用户正在放歌）时 DEVICE_IN_USE 属**环境状态**，不是产品缺陷；
            // 判 FAIL 会让「机器有人在用」变成假红（本仓库已多次踩过这类假红）。
            // 处理：DEVICE_IN_USE / 端点缺失 → SKIP 并打印原因；其它错误仍判失败。
            const std::string err = so.last_error();
            const bool envBusy = (err.find("DEVICE_IN_USE") != std::string::npos ||
                                  err.find("\u7AEF\u70B9") != std::string::npos);
            if (envBusy) {
                std::printf("  [SKIP] \u5171\u4EAB open \u672A\u6267\u884C\uFF08\u8BBE\u5907\u88AB\u5360\u7528\uFF09: %s\n", err.c_str());
            } else {
                ck(false, "\u5171\u4EAB open \u6210\u529F", "open 失败: " + err);
            }
        } else {
            ck(true, "共享 open 成功");
            char fb[192];
            std::snprintf(fb, sizeof(fb), "mix %u Hz / %d ch / %s（%d bit，%d 字节/样本）",
                          (unsigned)so.mix_rate(), so.mix_channels(), so.mix_format_name(),
                          so.mix_valid_bits(), so.mix_bytes());
            ck(so.mix_rate() > 0 && so.mix_channels() > 0 && so.mix_supported(),
               "mix 格式受支持且率/声道有效", fb);
            ck(so.mix_format() == qfmt, "open 的格式与 query_mix_format 一致", fb);
            ck(so.buffer_frames() > 0, "缓冲帧数 > 0", std::to_string(so.buffer_frames()));
            ck(so.start(), "共享 start 成功", so.last_error());

            // 0.5 秒全零（数字静音）；写入走 pack()，即被测的格式自适应路径
            const size_t total = (size_t)(0.5 * (double)so.mix_rate());
            std::vector<int32_t> zeros(total * (size_t)so.mix_channels(), 0);
            size_t written = 0;
            unsigned long long pos0 = 0, pos1 = 0;
            bool havePos = so.device_position(pos0);
            // ≤0.9 s 墙钟：写满 200ms 缓冲后靠设备消耗腾位置，同时观察位置增长
            while (written < total) {
                written += so.write_available(zeros.data() + written * (size_t)so.mix_channels(),
                                              total - written);
                if (so.elapsed_seconds() > 0.9) break;
                Sleep(10);
            }
            so.device_position(pos1);
            ck(written > 0, "设备接受了静音数据（w > 0）", "written=" + std::to_string(written));
            ck(written >= total / 4, "写入量 ≥ 0.125 s（200ms 缓冲 + 消耗推进）",
               "written=" + std::to_string(written) + "/" + std::to_string(total));
            ck(!havePos || pos1 > pos0, "设备播放位置增长（played 增长）",
               "pos " + std::to_string(pos0) + " → " + std::to_string(pos1));
            ck(so.last_pad() >= 0, "GetCurrentPadding 可读（共享模式不再恒满）",
               "last_pad=" + std::to_string(so.last_pad()));
            ck(so.out_peak() == 0.0, "静音写入后 out_peak == 0", std::to_string(so.out_peak()));
            so.close();
            ck(true, "close 完成");
        }
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "wasapi_shared 全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
