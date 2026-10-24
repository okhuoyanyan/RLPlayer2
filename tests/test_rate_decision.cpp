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

    // Case 8: autoMode=true 跨族就近（同族完全无可用档，选 >= wanted 的最小可用）
    {
        auto d = decide_rate(44100, {48000, 96000}, true, false);
        ck(d.ok && d.rate == 48000, "Case 8: 无同族档 ⟹ 跨族就近选最小更高 48000（不盲目降级）");
    }

    // Case 9: autoMode=true 跨族只有更大档
    {
        auto d = decide_rate(44100, {96000, 192000}, true, false);
        ck(d.ok && d.rate == 96000, "Case 9: 跨族只有更大档 ⟹ 选取最小可用 96000");
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
        auto d = decide_rate(88200, {192000, 0, 96000, 0, 96000}, true, false);
        ck(d.ok && d.rate == 96000, "Case 16: 支持列表乱序/含0/含重复 ⟹ 健壮清洗并正确决策 96000");
    }

    std::printf("=== test_rate_decision: %d 用例, %d 失败 ===\n", 16, g_fails);
    return g_fails ? 1 : 0;
}
