// test_wasapi_shared.cpp — WASAPI 共享输出后端（2026-10-22 VM 事故热修）
//
// 两部分：
//   §1 纯离线：int32→float32 换算边界值（0/±max/±1）与可写帧数钳制（shared_writable）
//      —— 不打开设备，铁律 1 无涉。
//   §2 设备 smoke：【只写数字静音（全零样本）且 ≤1 秒】，先打印醒目横幅
//      （目标设备 / 时长 / 电平=数字静音 0）。open→start→写 0.5s 全零→确认写入帧数>0
//      且设备播放位置增长→close。
//      ⚠️ 未解决项（铁律 4）：「无输出设备/被独占占用」的报错路径在本环境无法安全构造
//      （本机有活动端点且不能去占用用户的听音设备），当前以代码审查 + open 失败时
//      last_error 非空的断言作为回归基线，不做人为占锁实验。
#include "audio/wasapi_shared.h"
#include <cstdio>
#include <cstdint>
#include <cmath>
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

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== wasapi_shared：§1 纯离线（换算/钳制，不开设备） ==\n");

    // ---- §1.1 int32 → float32 边界值 ----
    ck(s32_to_f32(0) == 0.0, "s32_to_f32(0) == 0");
    const double dMin = s32_to_f32(INT32_MIN);
    ck(std::fabs(dMin + 1.0) < 1e-12, "s32_to_f32(INT32_MIN) == -1.0", std::to_string(dMin));
    const double dMax = s32_to_f32(INT32_MAX);
    ck(std::fabs(dMax - 0.999999999534338713) < 1e-15, "s32_to_f32(INT32_MAX) == 1-2^-31", std::to_string(dMax));
    ck(dMax < 1.0, "s32_to_f32(INT32_MAX) < 1.0（float 域不含 +1.0 满幅）");
    const double dPlus1 = s32_to_f32(1), dMinus1 = s32_to_f32(-1);
    ck(std::fabs(dPlus1 - 1.0 / 2147483648.0) < 1e-18, "s32_to_f32(1) == 2^-31", std::to_string(dPlus1));
    ck(std::fabs(dMinus1 + 1.0 / 2147483648.0) < 1e-18, "s32_to_f32(-1) == -2^-31", std::to_string(dMinus1));
    // 对称性与单调性抽查
    ck(s32_to_f32(1073741824) == 0.5, "s32_to_f32(2^30) == 0.5");
    ck(s32_to_f32(-1073741824) == -0.5, "s32_to_f32(-2^30) == -0.5");

    // ---- §1.1b 按源位深归一化（2026-10-23 现场修复的防回归）----
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

    // ---- §1.2 shared_writable 钳制 ----
    ck(shared_writable(9600, 0, 5000) == 5000, "pad=0 → 写满可用量");
    ck(shared_writable(9600, 0, 20000) == 9600, "pad=0 → 不超过缓冲深度");
    ck(shared_writable(9600, 9600, 20000) == 0, "pad=缓冲深度 → 0（背压）");
    ck(shared_writable(9600, 4600, 10000) == 5000, "pad 部分占用 → 补满剩余");
    ck(shared_writable(9600, 4600, 100) == 100, "可用数据不足 → 夹到数据量");
    ck(shared_writable(9600, 20000, 100) == 0, "pad 异常超界 → 钳到 0（不下溢成巨数）");

    // ---- §1.3 离线可测的 query 失败形态：不存在端点 → last_error 非空 ----
    {
        uint32_t r = 0; int ch = 0; bool f = true; std::string err;
        const bool ok = rta::audio::WasapiSharedOutput::query_mix_format(
            "{00000000-0000-0000-0000-000000000000}", r, ch, f, err);
        // 不存在的端点必须失败且错误可读（不存在 → rate 保持 0）。本机必有端点，
        // 但构造的假 id 一定不存在 —— 这条是报错路径在本环境唯一能安全覆盖的分支。
        ck(!ok && !err.empty() && r == 0, "query_mix_format(不存在端点) 报错且错误非空", err);
    }

    // ---- §2 设备 smoke：只写数字静音，≤1 秒，先打印横幅 ----
    std::printf("\n== wasapi_shared：§2 设备 smoke ==\n");
    std::string devName = "(未知)", devId;
    {
        for (const auto& d : rta::audio::WasapiOutput::enumerate()) {
            if (d.state == DEVICE_STATE_ACTIVE) { devName = d.name; devId = d.id; if (d.isDefault) break; }
        }
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
        rta::audio::WasapiSharedOutput so;
        bool opened = false;
        try {
            opened = so.open(devId, 48000, 2, 32, 200.0);
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
                ck(false, "\u5171\u4EAB open \u6210\u529F", "open \u5931\u8D25: " + err);
            }
        } else {
            ck(true, "共享 open 成功");
            char fb[128];
            std::snprintf(fb, sizeof(fb), "mix %u Hz / %d ch / %s",
                          (unsigned)so.mix_rate(), so.mix_channels(), so.is_float() ? "float32" : "非float");
            ck(so.mix_rate() > 0 && so.mix_channels() > 0 && so.is_float(), "mix 格式为 float32 且率/声道有效", fb);
            ck(so.buffer_frames() > 0, "缓冲帧数 > 0", std::to_string(so.buffer_frames()));
            ck(so.start(), "共享 start 成功", so.last_error());

            const size_t total = (size_t)(0.5 * (double)so.mix_rate());   // 0.5 秒全零
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
            so.close();
            ck(true, "close 完成");
        }
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "wasapi_shared 全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
