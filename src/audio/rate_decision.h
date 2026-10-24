// src/audio/rate_decision.h — 播放侧设备采样率自适应决策（纯函数，零外部依赖）
#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>

namespace rta::audio {

struct RateDecision {
    bool ok;
    uint32_t rate;
    const char* reason;
};

// decide_rate: 采样率决策策略纯函数
//
// 背景（元数据文档 1.6.E #10）：
//   - 用户明确指定采样率（autoMode=false）时：若设备不支持则硬失败并报错，绝不偷偷换率；
//   - 用户选择自动换率（autoMode=true）时：按设备支持列表换档（优先同族，其次就近不降级，再无更大则选最大可用）；
//   - DSD 播放（isDsd=true）时：
//       【重要说明】wanted 必须由调用方换算成 DoP / PCM 承载率（如 DSD64 -> 176400 Hz，DSD128 -> 352800 Hz 等）。
//       由于 DoP 载波率是精确数学固定的（0x05/0xFA 标记与 16-bit 载波强依赖准确时钟），
//       若设备不支持该承载率，即使 autoMode=true 也必须硬失败，绝不允许自动换率破坏 DoP 数据流。
inline RateDecision decide_rate(uint32_t wanted, const std::vector<uint32_t>& supported, bool autoMode, bool isDsd) {
    // 1. 规范化设备支持列表（剔除 0、升序排列、去重）
    std::vector<uint32_t> valid;
    valid.reserve(supported.size());
    for (uint32_t r : supported) {
        if (r > 0) valid.push_back(r);
    }
    std::sort(valid.begin(), valid.end());
    valid.erase(std::unique(valid.begin(), valid.end()), valid.end());

    // 设备支持列表为空
    if (valid.empty()) {
        return {false, 0, "设备支持列表为空"};
    }

    // 2. 检查设备是否直接支持 wanted
    bool exactMatch = (wanted > 0 && std::binary_search(valid.begin(), valid.end(), wanted));
    if (exactMatch) {
        return {true, wanted, "匹配设备支持采样率"};
    }

    // 3. wanted 不在设备支持列表中的情况：
    // DSD 模式：承载率不可变，禁止自动换率
    if (isDsd) {
        return {false, 0, "设备不支持DSD承载采样率"};
    }

    // 非自动模式（用户明确指定采样率）：硬失败，绝不偷偷换率
    if (!autoMode) {
        return {false, 0, "用户指定采样率不被设备支持"};
    }

    // 4. 自动模式（autoMode=true）换档逻辑：
    // 确定 wanted 属于哪个采样率族：
    // 44.1k 族（44100, 88200, 176400, 352800, 705600 ...）
    // 48k 族（48000, 96000, 192000, 384000, 768000 ...）
    uint32_t family = 0;
    if (wanted > 0 && wanted % 44100u == 0) {
        family = 44100u;
    } else if (wanted > 0 && wanted % 48000u == 0) {
        family = 48000u;
    }

    // ① 同族优先：在同族中找 >= wanted 的最小可用
    if (family > 0) {
        for (uint32_t r : valid) {
            if (r % family == 0 && r >= wanted) {
                return {true, r, "自动换档：同族不小于目标率的最小可用"};
            }
        }
    }

    // ② 其次就近：跨族找 >= wanted 的最小可用（避免无谓降采样）
    for (uint32_t r : valid) {
        if (r >= wanted) {
            return {true, r, "自动换档：跨族不小于目标率的最小可用"};
        }
    }

    // ③ 没有更大的就选最大可用（降采样保底出声）
    return {true, valid.back(), "自动换档：无更大采样率，选取设备最大可用"};
}

} // namespace rta::audio

// 方便调用方在全局作用域直接使用
using rta::audio::RateDecision;
using rta::audio::decide_rate;
