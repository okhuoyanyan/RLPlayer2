// PCM output selection. Device-free; all family decisions share rate_family_policy.h.
#pragma once
#include "rate_family_policy.h"

namespace rta::audio {
struct RatePick {
    uint32_t rate = 0;
    bool decided = false;
    std::string note;
    std::string logTag;
};
namespace ratepick_detail {
inline std::vector<uint32_t> normalized(const std::vector<uint32_t>& supported) {
    return normalized_rates(supported);
}
inline uint32_t parse_rate(const std::string& s) {
    size_t i = !s.empty() && s[0] == '+' ? 1 : 0;
    if (i >= s.size()) return 0;
    uint64_t result = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        result = result * 10 + static_cast<unsigned>(s[i] - '0');
        if (result > 40000000ull) return 0;
    }
    return static_cast<uint32_t>(result);
}
}

// Numeric selections are explicit: preserve the requested clock for the later
// capability check. Automatic src/max/follow all prefer the ACTUAL source family.
// Empty capability evidence is never interpreted as a successful max probe.
inline RatePick pick_output_rate(const std::string& requested, uint32_t sourceRate,
                                 const std::vector<uint32_t>& supported,
                                 uint32_t systemRate = 0,
                                 RateFamilyPolicy policy = RateFamilyPolicy::Prefer) {
    const uint32_t explicitRate = ratepick_detail::parse_rate(requested);
    const bool automatic = !explicitRate;
    uint32_t wanted = explicitRate ? explicitRate : sourceRate;
    std::string preface;
    std::string tag;
    const auto valid = normalized_rates(supported);
    if (requested == "max") {
        if (valid.empty())
            return {sourceRate, false,
                    "设备独占支持率探测为空；源采样率 " + std::to_string(sourceRate) + " Hz 仅供重新探测，不能当成可用结论",
                    "RATEPROBE_EMPTY"};
        wanted = valid.back();
    } else if (requested == "follow") {
        if (systemRate) wanted = systemRate;
        else {
            preface = "读不到系统混音采样率；按源采样率 " + std::to_string(sourceRate) + " Hz 选择。";
            tag = "SYSRATE_EMPTY";
        }
    } else if (automatic && !requested.empty() && requested != "src") {
        preface = "采样率参数 \"" + requested + "\" 无法解析，按源采样率选择。";
    }
    const auto decision = automatic && !valid.empty()
        ? decide_pcm_rate(sourceRate, wanted, valid, true, policy)
        : validate_pcm_rate(sourceRate, wanted, automatic, policy);
    if (!decision.ok)
        return {0, false, preface + decision.reason, decision.logTag};
    // Ordinary unchanged / same-family choices need no extra UI noise. Cross-family
    // choices always carry their explicit user-facing explanation.
    std::string note = preface;
    if (decision.crossFamily) note += decision.reason;
    else if (decision.rate != wanted)
        note += "按源采样率同族选择 " + std::to_string(decision.rate) + " Hz（目标 " + std::to_string(wanted) + " Hz）";
    return {decision.rate, true, note, decision.crossFamily ? decision.logTag : tag};
}

inline uint32_t pick_supported_rate(uint32_t want, uint32_t sourceRate,
                                    const std::vector<uint32_t>& supported,
                                    RateFamilyPolicy policy = RateFamilyPolicy::Prefer) {
    return decide_pcm_rate(sourceRate ? sourceRate : want, want, supported, true, policy).rate;
}

enum class CapAction { Matched, FallbackRate, RetryProbe, HardFail };
struct CapDecision {
    CapAction action = CapAction::HardFail;
    uint32_t outRate = 0;
    bool changed = false;
    std::string userMessage;
    std::string logTag;
};

inline CapDecision decide_capability_failure(bool rateAuto, uint32_t want,
                                            const std::vector<uint32_t>& supported,
                                            uint32_t& outRate,
                                            uint32_t sourceRate = 0,
                                            RateFamilyPolicy policy = RateFamilyPolicy::Prefer) {
    const auto valid = normalized_rates(supported);
    if (rateAuto && valid.empty()) {
        outRate = want;
        return {CapAction::RetryProbe, want, false,
                "设备独占支持率探测为空，建议探测重试或保持现有采样率", "RATEPROBE_EMPTY"};
    }
    if (!rateAuto && valid.empty()) {
        outRate = want;
        return {CapAction::HardFail, want, false,
                "你在设置里明确指定了 " + std::to_string(want) +
                " Hz，但设备独占支持率探测为空，尚无法确认是否支持；不会自动改成别的率",
                "RATEPROBE_EMPTY"};
    }
    const auto d = decide_pcm_rate(sourceRate ? sourceRate : want, want, valid, rateAuto, policy);
    if (!d.ok) {
        outRate = want;
        std::string message = d.reason;
        if (!rateAuto && std::string(d.logTag) == "EXPLICIT_REJECT")
            message = "你在设置里明确指定了 " + std::to_string(want) +
                      " Hz，设备独占模式做不了这个采样率；不会自动改成别的率";
        return {CapAction::HardFail, want, false, message, d.logTag};
    }
    outRate = d.rate;
    const bool changed = d.rate != want;
    std::string message = d.crossFamily ? d.reason : "";
    if (changed)
        message = "目标 " + std::to_string(want) + " Hz，按源 " + std::to_string(sourceRate ? sourceRate : want) +
                  " Hz 策略已用 " + std::to_string(d.rate) + " Hz。" + d.reason;
    return {changed ? CapAction::FallbackRate : CapAction::Matched, d.rate, changed,
            message, d.crossFamily ? d.logTag : (changed ? "RATEFALLBACK" : "")};
}
inline CapDecision decide_capability_failure(bool rateAuto, uint32_t want,
                                            const std::vector<uint32_t>& supported) {
    uint32_t ignored = want;
    return decide_capability_failure(rateAuto, want, supported, ignored);
}
} // namespace rta::audio
