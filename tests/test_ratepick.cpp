// Regression tests: assertions encode the approved source-family policy, not old
// audible fallback behavior. Previous tests expected silent 44.1 -> 48 conversion;
// this is now an error unless Compatible was explicitly selected.
#include "audio/rate_pick.h"
#include <cstdio>
#include <string>
using namespace rta::audio;
static int total = 0, fails = 0;
static void ck(bool condition, const char* name) {
    ++total; if (!condition) { ++fails; std::printf("FAIL %s\n", name); }
}
int main() {
    auto p = pick_output_rate("96000", 44100, {48000});
    ck(p.rate == 96000 && p.decided, "explicit rate unchanged until capability check");
    ck(p.note.find("跨族") != std::string::npos && p.logTag == "RATE_CROSS_FAMILY", "explicit cross-family explained");
    p = pick_output_rate("96000", 44100, {96000}, 0, RateFamilyPolicy::Strict);
    ck(!p.decided && p.rate == 0, "strict blocks explicit cross-family");
    for (auto request : {"src", ""}) {
        p = pick_output_rate(request, 44100, {44100, 48000});
        ck(p.decided && p.rate == 44100 && p.note.empty(), "source selection stays exact");
        p = pick_output_rate(request, 44100, {48000, 96000});
        ck(!p.decided && p.rate == 0 && p.logTag == "RATE_FAMILY_BLOCKED", "source selection blocks cross-only device");
    }
    p = pick_output_rate("max", 44100, {44100,48000,88200,96000});
    ck(p.decided && p.rate == 88200, "max is highest source-family rate");
    p = pick_output_rate("max",44100,{96000,48000,96000});
    ck(!p.decided && p.note.find("compatible") != std::string::npos, "max cross-only explains compatible option");
    p = pick_output_rate("max",44100,{48000,96000},0,RateFamilyPolicy::Compatible);
    ck(p.decided && p.rate == 96000 && p.note.find("跨族") != std::string::npos, "compatible max explicit warning");
    for (auto caps : {std::vector<uint32_t>{}, std::vector<uint32_t>{0,0}}) {
        p = pick_output_rate("max",44100,caps);
        ck(!p.decided && p.rate == 44100 && p.logTag == "RATEPROBE_EMPTY", "empty probe is not successful source result");
    }
    p = pick_output_rate("max",0,{});
    ck(!p.decided && !p.rate, "missing source and capabilities fail safely");
    p = pick_output_rate("follow",44100,{44100,48000,88200,192000},48000);
    ck(p.decided && p.rate == 88200 && !p.note.empty(), "follow prefers source family at or above target");
    p = pick_output_rate("follow",44100,{44100,192000},44100);
    ck(p.rate == 44100, "follow exact source remains exact");
    p = pick_output_rate("follow",176400,{44100,88200,176400},44100);
    ck(p.rate == 176400, "follow does not needlessly downsample below source");
    p = pick_output_rate("follow",44100,{44100,88200},192000);
    ck(p.rate == 88200, "follow chooses lower same-family before foreign target");
    p = pick_output_rate("follow",44100,{44100,192000},0);
    ck(p.rate == 44100 && p.logTag == "SYSRATE_EMPTY" && !p.note.empty(), "missing system clock reported");
    p = pick_output_rate("follow",44100,{},48000);
    ck(!p.decided && !p.rate, "unprobed follow cannot silently cross source family");
    p = pick_output_rate("abc",44100,{44100});
    ck(p.rate == 44100 && p.note.find("无法解析") != std::string::npos, "invalid requested text explained");
    for (auto bad : {"0","-1","9999999999999999999999999999999","+"})
        ck(pick_output_rate(bad,44100,{44100}).rate == 44100, "invalid integer fallback is safe");
    ck(pick_supported_rate(44100,44100,{48000,96000}) == 0, "cross-only default fails");
    ck(pick_supported_rate(44100,44100,{48000,88200}) == 88200, "same-family higher wins");
    ck(pick_supported_rate(192000,44100,{44100,48000}) == 44100, "same-family lower wins");
    ck(pick_supported_rate(48000,44100,{44100,48000,96000}) == 44100, "exact foreign target does not override source family");
    ck(pick_supported_rate(44100,44100,{}) == 0, "empty list fails");
    ck(pick_supported_rate(44100,44100,{48000,96000},RateFamilyPolicy::Compatible) == 48000, "compatible cross fallback");
    ck(pick_supported_rate(32000,32000,{44100,96000}) == 96000, "32k to 96k is same-family");
    ck(pick_supported_rate(96000,44100,{0,88200,44100,88200}) == 88200, "unsorted duplicate zero normalized");
    uint32_t out=96000;
    auto d=decide_capability_failure(false,96000,{44100,48000},out,44100);
    ck(d.action == CapAction::HardFail && out == 96000 && !d.changed, "unsupported explicit rate stays unchanged and fails");
    ck(d.logTag == "EXPLICIT_REJECT" && d.userMessage.find("不会自动改成别的率") != std::string::npos, "explicit rejection explains no rate substitution");
    d=decide_capability_failure(true,44100,{48000,96000},out,44100);
    ck(d.action == CapAction::HardFail && out == 44100 && d.logTag == "RATE_FAMILY_BLOCKED", "capability fallback default cannot cross");
    d=decide_capability_failure(true,44100,{48000,96000},out,44100,RateFamilyPolicy::Compatible);
    ck(d.action == CapAction::FallbackRate && out == 48000 && d.changed, "compatible capability fallback crosses");
    ck(d.userMessage.find("跨族") != std::string::npos && d.logTag == "RATE_CROSS_FAMILY", "compatible capability fallback warns");
    d=decide_capability_failure(true,48000,{44100,48000},out,44100);
    ck(d.action == CapAction::FallbackRate && out == 44100, "matched foreign wanted still source checked");
    d=decide_capability_failure(false,48000,{44100,48000},out,44100);
    ck(d.action == CapAction::Matched && out == 48000 && !d.userMessage.empty(), "explicit preferred-policy cross matching warns");
    d=decide_capability_failure(false,48000,{44100,48000},out,44100,RateFamilyPolicy::Strict);
    ck(d.action == CapAction::HardFail, "strict matching foreign wanted still rejected");
    d=decide_capability_failure(true,44100,{},out,44100);
    ck(d.action == CapAction::RetryProbe && out == 44100 && !d.changed && d.logTag == "RATEPROBE_EMPTY", "empty capability retry preserved");
    d=decide_capability_failure(false,44100,{},out,44100);
    ck(d.action == CapAction::HardFail && d.userMessage.find("明确指定了 44100") != std::string::npos, "empty capability explicit fails and explains requested rate");
    d=decide_capability_failure(true,44100,{88200,48000,88200},out,44100);
    ck(d.action == CapAction::FallbackRate && out == 88200, "capability fallback same-family");
    std::printf("ratepick: %d checks, %d failures\n",total,fails);
    return fails ? 1 : 0;
}
