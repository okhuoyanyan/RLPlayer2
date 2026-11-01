// tests/test_rate_decision.cpp — rateAuto 采样率自适应决策单测（纯离线，不开设备）
#include "audio/rate_decision.h"
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

static int g_fails = 0;
static void ck(bool cond, const char* desc, const std::string& detail = "") {
    if (cond) {
        std::printf("  [PASS] %s\n", desc);
    } else {
        std::printf("  [FAIL] %s %s\n", desc, detail.c_str());
        ++g_fails;
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("=== test_rate_decision: rateAuto 决策纯函数回归单测 ===\n");

    // Case 1: 设备支持列表为空 + autoMode=false
    {
        auto d = decide_rate(44100, {}, false, false);
        ck(!d.ok && d.rate == 0, "Case 1: 空列表 + autoMode=false ⟹ 失败");
    }

    // Case 2: 设备支持列表为空 + autoMode=true
    {
        auto d = decide_rate(44100, {}, true, false);
        ck(!d.ok && d.rate == 0, "Case 2: 空列表 + autoMode=true ⟹ 失败");
    }

    // Case 3: wanted 在设备支持列表中 + autoMode=false
    {
        auto d = decide_rate(48000, {44100, 48000, 96000}, false, false);
        ck(d.ok && d.rate == 48000, "Case 3: wanted 在支持列表 + autoMode=false ⟹ 成功命中 48000");
    }

    // Case 4: wanted 在设备支持列表中 + autoMode=true
    {
        auto d = decide_rate(96000, {44100, 48000, 96000}, true, false);
        ck(d.ok && d.rate == 96000, "Case 4: wanted 在支持列表 + autoMode=true ⟹ 成功命中 96000");
    }

    // Case 5: wanted 不在支持列表 + autoMode=false (硬失败并报清楚)
    {
        auto d = decide_rate(88200, {44100, 48000, 96000}, false, false);
        ck(!d.ok && d.rate == 0 && std::string(d.reason) == "用户指定采样率不被设备支持",
           "Case 5: wanted 不在支持列表 + autoMode=false ⟹ 硬失败且报清楚");
    }

    // Case 6: autoMode=true 同族优先（44.1k 族）选不小于 wanted 的最小可用
    {
        auto d = decide_rate(44100, {48000, 88200, 176400}, true, false);
        ck(d.ok && d.rate == 88200, "Case 6: 44.1k 族同族优先 ⟹ 选最小可用 88200（不选 48k 族）");
    }

    // Case 7: autoMode=true 同族优先（48k 族）选不小于 wanted 的最小可用
    {
        auto d = decide_rate(48000, {44100, 88200, 96000, 192000}, true, false);
        ck(d.ok && d.rate == 96000, "Case 7: 48k 族同族优先 ⟹ 选最小可用 96000（不选 44.1k 族）");
    }

    // Case 8（261030-01 D-12 改写）: 无同族档 ⟹ 跨族被禁止，必须硬失败
    {
        auto d = decide_rate(44100, {48000, 96000}, true, false);
        ck(!d.ok && d.rate == 0 && std::string(d.reason).find("compatible") != std::string::npos,
           "Case 8: 无同族档 ⟹ 跨族禁止，硬失败（D-12）");
    }

    // Case 8b: 逃生口 allowCrossFamily=true ⟹ 恢复旧行为（兼容重采样，非 HIFI）
    {
        auto d = decide_rate(44100, {48000, 96000}, true, false, true);
        ck(d.ok && d.rate == 48000, "Case 8b: 显式逃生口 ⟹ 跨族就近选 48000（兼容重采样）");
    }

    // Case 9（D-12 改写）: 跨族只有更大档 ⟹ 依然硬失败
    {
        auto d = decide_rate(44100, {96000, 192000}, true, false);
        ck(!d.ok && d.rate == 0, "Case 9: 跨族只有更大档 ⟹ 硬失败（D-12）");
    }

    // Case 9b: 同族只有更大档（44.1k 族 88200）⟹ 允许，仍选同族
    {
        auto d = decide_rate(44100, {88200, 96000}, true, false);
        ck(d.ok && d.rate == 88200, "Case 9b: 同族有更大档 ⟹ 选 88200，不碰 48k 族");
    }

    // Case 10: autoMode=true 只有更小档（没有更大档时选最大可用）
    {
        auto d = decide_rate(192000, {44100, 48000, 96000}, true, false);
        ck(d.ok && d.rate == 96000, "Case 10: 只有更小档 ⟹ 选设备支持的最大可用 96000");
    }

    // Case 11: autoMode=true 同族只有更小档
    {
        auto d = decide_rate(88200, {44100}, true, false);
        ck(d.ok && d.rate == 44100, "Case 11: 同族只有更小档 ⟹ 选最大可用 44100");
    }

    // Case 12: DSD 承载率命中
    {
        auto d = decide_rate(176400, {44100, 48000, 176400}, false, true);
        ck(d.ok && d.rate == 176400, "Case 12: DSD 承载率直接命中 176400 ⟹ 成功");
    }

    // Case 13: DSD 承载率不在列表 + autoMode=false ⟹ 硬失败
    {
        auto d = decide_rate(176400, {44100, 48000, 96000}, false, true);
        ck(!d.ok && d.rate == 0, "Case 13: DSD 承载率不在列表 + autoMode=false ⟹ 硬失败");
    }

    // Case 14: DSD 承载率不在列表 + autoMode=true ⟹ 依然硬失败（DSD 载波固定禁止自动换率）
    {
        auto d = decide_rate(176400, {44100, 48000, 96000}, true, true);
        ck(!d.ok && d.rate == 0, "Case 14: DSD 承载率不在列表 + autoMode=true ⟹ 绝不自动换率（硬失败）");
    }

    // Case 15: DSD 模式设备仅有其他 DSD 承载率（如 DSD64 目标 176400，设备只支持 352800）
    {
        auto d = decide_rate(176400, {352800}, true, true);
        ck(!d.ok && d.rate == 0, "Case 15: DSD 不得任意升档承载率 ⟹ 拒绝开流（硬失败）");
    }

    // Case 16: 支持列表无序、含 0、含重复时的健壮性
    {
        auto d = decide_rate(88200, {176400, 0, 44100, 0, 44100}, true, false);
        ck(d.ok && d.rate == 176400, "Case 16: 支持列表乱序/含0/含重复 ⟹ 健壮清洗并正确决策 176400（同族）");
    }

    // Real source must drive the family even when wanted already matches hardware.
    {
        auto d = decide_rate(96000, {44100, 96000}, true, false, false, 44100);
        ck(d.ok && d.rate == 44100, "Case 17: real 44.1 source beats exact foreign wanted");
        d = decide_rate(96000, {44100, 96000}, false, false, false, 44100,
                        rta::audio::RateFamilyPolicy::Strict);
        ck(!d.ok, "Case 18: strict rejects explicit foreign clock despite exact hardware support");
        d = decide_rate(176400, {176400, 352800}, true, true, false, 48000,
                        rta::audio::RateFamilyPolicy::Strict);
        ck(d.ok && d.rate == 176400, "Case 19: DoP carrier bypasses PCM family policy");
        d = decide_rate(176400, {352800}, true, true, true, 44100,
                        rta::audio::RateFamilyPolicy::Compatible);
        ck(!d.ok, "Case 20: compatible never changes unsupported DoP carrier");
    }
    std::printf("=== test_rate_decision: 22 checks, %d failures ===\n", g_fails);
    return g_fails ? 1 : 0;
}
