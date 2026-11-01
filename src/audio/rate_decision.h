// Legacy decision API. PCM policy is delegated to the shared source-aware core.
#pragma once
#include "rate_family_policy.h"
namespace rta::audio {
struct RateDecision {
    bool ok;
    uint32_t rate;
    const char* reason;
};
// sourceRate was absent from the legacy API. Callers that know the real source
// MUST pass it; wanted is only retained as a backwards-compatible last resort.
inline RateDecision decide_rate(uint32_t wanted, const std::vector<uint32_t>& supported,
                                bool autoMode, bool isDsd, bool allowCrossFamily = false,
                                uint32_t sourceRate = 0,
                                RateFamilyPolicy policy = RateFamilyPolicy::Prefer) {
    const auto valid = normalized_rates(supported);
    // DSD / DoP has an exact transport clock. Never apply PCM family fallback.
    if (isDsd) {
        if (valid.empty()) return {false, 0, "设备支持列表为空"};
        if (wanted && std::binary_search(valid.begin(), valid.end(), wanted))
            return {true, wanted, "匹配DSD承载采样率"};
        return {false, 0, "设备不支持DSD承载采样率"};
    }
    if (allowCrossFamily && policy == RateFamilyPolicy::Prefer)
        policy = RateFamilyPolicy::Compatible;
    const auto d = decide_pcm_rate(sourceRate ? sourceRate : wanted, wanted, valid, autoMode, policy);
    return {d.ok, d.rate, d.reason};
}
} // namespace rta::audio
using rta::audio::RateDecision;
using rta::audio::decide_rate;
