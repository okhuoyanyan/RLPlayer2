// PCM output-rate policy. Pure logic: no device, DSP or operating-system dependency.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace rta::audio {

enum class RateFamilyPolicy { Prefer, Strict, Compatible };
inline bool valid_rate_family_policy(const std::string& value) {
    return value == "prefer" || value == "strict" || value == "compatible";
}
inline RateFamilyPolicy parse_rate_family_policy(const std::string& value) {
    if (value == "strict") return RateFamilyPolicy::Strict;
    if (value == "compatible") return RateFamilyPolicy::Compatible;
    return RateFamilyPolicy::Prefer; // Invalid persisted input never enables cross-family fallback.
}
inline const char* rate_family_policy_name(RateFamilyPolicy policy) {
    switch (policy) {
    case RateFamilyPolicy::Strict: return "strict";
    case RateFamilyPolicy::Compatible: return "compatible";
    default: return "prefer";
    }
}

// Exact PCM clock series, rather than divisibility by 44100/48000: e.g. 7056000
// is divisible by BOTH, so this nonstandard PCM clock stays unknown.
// Standard 705600 belongs to 44.1k; 32k is the 48k family's 2/3 branch.
inline uint32_t rate_family(uint32_t rate) {
    if (!rate) return 0;
    for (uint64_t r = 11025; r <= UINT32_MAX; r *= 2)
        if (rate == r) return 44100;
    for (uint32_t base : {8000u, 12000u})
        for (uint64_t r = base; r <= UINT32_MAX; r *= 2)
            if (rate == r) return 48000;
    return 0;
}
inline std::vector<uint32_t> normalized_rates(const std::vector<uint32_t>& supported) {
    std::vector<uint32_t> result;
    for (auto r : supported) if (r) result.push_back(r);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct RateFamilyDecision {
    bool ok = false;
    uint32_t rate = 0;
    bool crossFamily = false;
    const char* reason = "";
    const char* logTag = "";
};

// Check the ACTUAL source and final PCM clock, including paths without a probed
// capability list (shared mode, offline export, rate changed after device open).
// DSD bits / DoP carrier clocks must not be passed to this PCM-only function.
inline RateFamilyDecision validate_pcm_rate(uint32_t sourceRate, uint32_t outputRate,
                                           bool automatic,
                                           RateFamilyPolicy policy = RateFamilyPolicy::Prefer) {
    if (!sourceRate || !outputRate)
        return {false, 0, false, "源或目标 PCM 采样率未知，无法确认采样率策略", "RATE_SOURCE_UNKNOWN"};
    if (sourceRate == outputRate)
        return {true, outputRate, false, "保持源采样率", ""};
    const auto sourceFamily = rate_family(sourceRate);
    const auto outputFamily = rate_family(outputRate);
    if (sourceFamily && sourceFamily == outputFamily)
        return {true, outputRate, false, "同族 PCM 重采样", ""};
    if (policy == RateFamilyPolicy::Strict)
        return {false, 0, true, "严格同族：禁止跨族 PCM 转换；未知采样率族仅允许保持源率", "RATE_FAMILY_BLOCKED"};
    if (policy == RateFamilyPolicy::Prefer && automatic)
        return {false, 0, true, "设备没有可用的源采样率同族档位；如需跨族转换，请明确切换为兼容转换（compatible）", "RATE_FAMILY_BLOCKED"};
    return {true, outputRate, true,
            automatic ? "兼容转换：已跨族 PCM 重采样，非 bit-perfect" : "用户明确指定跨族 PCM 转换，非 bit-perfect",
            "RATE_CROSS_FAMILY"};
}

inline RateFamilyDecision decide_pcm_rate(uint32_t sourceRate, uint32_t wanted,
                                         const std::vector<uint32_t>& supported,
                                         bool automatic,
                                         RateFamilyPolicy policy = RateFamilyPolicy::Prefer) {
    const auto valid = normalized_rates(supported);
    if (valid.empty())
        return {false, 0, false, "设备支持列表为空，必须重新探测采样率", "RATEPROBE_EMPTY"};
    if (!sourceRate || !wanted)
        return {false, 0, false, "源或目标 PCM 采样率未知，无法确认采样率策略", "RATE_SOURCE_UNKNOWN"};
    if (!automatic) {
        const auto policyCheck = validate_pcm_rate(sourceRate, wanted, false, policy);
        if (!policyCheck.ok) return policyCheck;
        if (!std::binary_search(valid.begin(), valid.end(), wanted))
            return {false, 0, policyCheck.crossFamily, "用户指定采样率不被设备支持", "EXPLICIT_REJECT"};
        return policyCheck;
    }
    // Source-clock identity is still safe when its family is unknown.
    const auto sourceFamily = rate_family(sourceRate);
    uint32_t sameMax = 0;
    const uint32_t floor = std::max(wanted, sourceRate);
    for (auto r : valid) {
        if (r != sourceRate && (!sourceFamily || rate_family(r) != sourceFamily)) continue;
        sameMax = r;
        if (r >= floor) return validate_pcm_rate(sourceRate, r, true, policy);
    }
    // Same-family output wins even below wanted. Only downsample below source
    // when the device offers no same-family clock >= source.
    if (sameMax) return validate_pcm_rate(sourceRate, sameMax, true, policy);
    if (policy != RateFamilyPolicy::Compatible) {
        if (sourceFamily)
            return {false, 0, true, "设备没有可用的源采样率同族档位；如需跨族转换，请明确切换为兼容转换（compatible）", "RATE_FAMILY_BLOCKED"};
        return {false, 0, true, "源采样率族未知；自动模式仅允许保持源率，跨率转换请明确切换为兼容转换（compatible）", "RATE_FAMILY_BLOCKED"};
    }
    for (auto r : valid)
        if (r >= floor) return validate_pcm_rate(sourceRate, r, true, policy);
    return validate_pcm_rate(sourceRate, valid.back(), true, policy);
}

} // namespace rta::audio
