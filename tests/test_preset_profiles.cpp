// tests/test_preset_profiles.cpp — 「三档官方推荐预设」决策纯函数的离线单测
//
// 判据（铁律 3）：这里只测**决策**，不测设备。探测（WASAPI IsFormatSupported / ASIO
// canSampleRate、kAsioCanDoIoFormat）在 rtaweb 里，验证靠 GET /api/preset/caps 的实测输出。
// 因此本文件不需要声卡、不开流、不出声，任何机器都能跑。
//
// 覆盖的四类硬要求（用户 2026-10-25）：
//   1) 无 DSD 能力的设备选 dsd 档必须**降级并给 note**（不是静默假装支持）；
//   2) maxRate 只有 96k 时 pcm 档**不许写 384**；
//   3) 源 DSD 与目标同速率 ⟹ directSdm=1（不同速率必须 0）；
//   4) bitperfect 档在任何设备上都必须满足 bit-perfect 七条件（且负向对照能证明判据不是摆设）。
#include "engine/preset_profiles.h"
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace rta::engine;

static int g_fails = 0;
static void ck(bool cond, const char* desc, const std::string& detail = "") {
    if (cond) {
        std::printf("  [PASS] %s\n", desc);
    } else {
        std::printf("  [FAIL] %s %s\n", desc, detail.c_str());
        ++g_fails;
    }
}

// ---------------- 构造器 ----------------
static DeviceCaps wasapi_caps(std::vector<uint32_t> rates, int bits = 24, int ch = 2) {
    DeviceCaps c;
    c.backend = "wasapi";
    c.rates = std::move(rates);
    c.maxRate = c.rates.empty() ? 0u : c.rates.back();
    c.exclusive = !c.rates.empty();
    c.bits = bits;
    c.channels = ch;
    c.deviceName = "USB DAC (mock)";
    c.dopMax = preset_dop_max_from_rates(c.rates);
    c.dop = !c.dopMax.empty();
    return c;
}
static DeviceCaps asio_caps(std::vector<uint32_t> rates, const std::string& nativeMax,
                            int bits = 24, int ch = 2) {
    DeviceCaps c = wasapi_caps(std::move(rates), bits, ch);
    c.backend = "asio";
    c.exclusive = true;
    c.dsdNative = !nativeMax.empty();
    c.dsdNativeMax = nativeMax;
    c.deviceName = "AudioByte ASIO (mock)";
    return c;
}
static SourceInfo pcm_src(uint32_t rate, int bits) {
    SourceInfo s;
    s.isDsd = false;
    s.srcRate = rate;
    s.srcBits = bits;
    return s;
}
static SourceInfo dsd_src(int level) {
    SourceInfo s;
    s.isDsd = true;
    s.srcRate = (uint32_t)(44100 * level);
    s.srcBits = 1;
    return s;
}
static bool has_note(const PresetSettings& s, const char* needle) {
    for (const auto& n : s.notes) if (n.find(needle) != std::string::npos) return true;
    return false;
}
static uint32_t rate_num(const std::string& r) {
    if (r.empty()) return 0;
    uint32_t v = 0;
    for (char c : r) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (uint32_t)(c - '0');
    }
    return v;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_preset_profiles: 三档官方推荐预设的决策纯函数 ===\n");

    const std::vector<uint32_t> kFull = {44100, 48000, 88200, 96000, 176400, 192000,
                                         352800, 384000, 705600};
    const std::vector<uint32_t> k96k = {44100, 48000, 96000};
    const std::vector<uint32_t> k192k = {44100, 48000, 88200, 96000, 176400, 192000};

    // ---- ① 原音直通：PCM 源必须满足七条件 ----
    {
        const DeviceCaps c = wasapi_caps(kFull);
        const SourceInfo src = pcm_src(44100, 16);
        const PresetSettings s = plan_preset(PresetProfile::BitPerfect, c, src);
        ck(s.rate == "follow", "Case 1: bitperfect/PCM ⟹ rate=follow（不重采样）");
        ck(s.dsd == 0, "Case 1: bitperfect/PCM ⟹ dsd=0（不调制）");
        ck(s.volumeDb == 0.0, "Case 1: bitperfect/PCM ⟹ 音量 0 dB");
        ck(s.shaper == "none", "Case 1: bitperfect/PCM ⟹ shaper=none（无抖动/整形）");
        ck(s.clearEqIr, "Case 1: bitperfect/PCM ⟹ 清空 EQ/IR/预衰减");
        ck(s.directSdm == 0, "Case 1: bitperfect/PCM ⟹ directSdm=0");
        const BitPerfectCheck bp = check_bit_perfect(s, c, src);
        ck(bp.ok && bp.violations.empty(), "Case 1: bitperfect/PCM ⟹ 七条件全过",
           bp.violations.empty() ? std::string() : bp.violations[0]);
    }

    // ---- ② 原音直通：设备只有 96k 也照样满足七条件（follow 不需要支持列表） ----
    {
        const DeviceCaps c = wasapi_caps(k96k);
        const SourceInfo src = pcm_src(44100, 24);
        const PresetSettings s = plan_preset(PresetProfile::BitPerfect, c, src);
        const BitPerfectCheck bp = check_bit_perfect(s, c, src);
        ck(s.rate == "follow" && bp.ok, "Case 2: bitperfect/96k 设备 ⟹ 仍是 follow 且七条件全过");
    }

    // ---- ③ 负向对照：位深降低必须被判出来（证明七条件不是摆设） ----
    {
        const DeviceCaps c = wasapi_caps(kFull, 16);
        const SourceInfo src = pcm_src(96000, 24);
        const PresetSettings s = plan_preset(PresetProfile::BitPerfect, c, src);
        const BitPerfectCheck bp = check_bit_perfect(s, c, src);
        ck(!bp.ok, "Case 3: 24-bit 源 + 16-bit 输出 ⟹ 七条件判不过（负向对照）");
        ck(!bp.violations.empty() && bp.violations[0].find("⑦") != std::string::npos,
           "Case 3: 违例条目点名条件⑦（位深降低）",
           bp.violations.empty() ? std::string() : bp.violations[0]);
    }

    // ---- ④ 原音直通：DSD 源 + 原生能力 ⟹ 同速率 DirectSDM ----
    {
        const DeviceCaps c = asio_caps(kFull, "dsd256");
        const SourceInfo src = dsd_src(64);
        const PresetSettings s = plan_preset(PresetProfile::BitPerfect, c, src);
        ck(s.dsd == 64, "Case 4: bitperfect/DSD64 ⟹ dsd=64（目标是源位率本身）");
        ck(s.directSdm == 1, "Case 4: bitperfect/DSD64 ⟹ directSdm=1（同速率逐位直通）");
        ck(has_note(s, "DirectSDM"), "Case 4: note 写明 DirectSDM 1-bit 直通");
        ck(has_note(s, "原生"), "Case 4: note 写明设备原生支持（可 1-bit 直送）");
        const BitPerfectCheck bp = check_bit_perfect(s, c, src);
        ck(bp.ok, "Case 4: bitperfect/DSD64 ⟹ 七条件全过（⑥按 1-bit 直通判）");
    }

    // ---- ⑤ 原音直通：无原生但 DoP 够 ⟹ 仍按 1-bit 直通 ----
    {
        const DeviceCaps c = wasapi_caps(k192k);      // 176400 载波 ⟹ dopMax=dsd64
        const SourceInfo src = dsd_src(64);
        const PresetSettings s = plan_preset(PresetProfile::BitPerfect, c, src);
        ck(s.dsd == 64 && s.directSdm == 1, "Case 5: bitperfect/DSD64 + DoP ⟹ dsd=64 且 directSdm=1");
        ck(has_note(s, "DoP"), "Case 5: note 写明按 DoP 1-bit 直通");
    }

    // ---- ⑥ 原音直通：源位率超出设备承载 ⟹ 降级 + note，且必须不再声称 bit-perfect ----
    {
        const DeviceCaps c = wasapi_caps(k192k);      // DoP 只到 dsd64
        const SourceInfo src = dsd_src(256);
        const PresetSettings s = plan_preset(PresetProfile::BitPerfect, c, src);
        ck(s.dsd == 64, "Case 6: bitperfect/DSD256 超能力 ⟹ 降级到 DSD64");
        ck(s.directSdm == 0, "Case 6: 降级后 directSdm=0（不假装直通）");
        ck(has_note(s, "降级"), "Case 6: note 明确写「已降级」");
        const BitPerfectCheck bp = check_bit_perfect(s, c, src);
        ck(!bp.ok, "Case 6: 降级后七条件当然不成立（不许静默宣称 bit-perfect）");
    }

    // ---- ⑦ 原音直通：能力未知（探测为空）⟹ 保守不承诺直通 ----
    {
        const DeviceCaps c = wasapi_caps({});
        const SourceInfo src = dsd_src(64);
        const PresetSettings s = plan_preset(PresetProfile::BitPerfect, c, src);
        ck(s.dsd == 64 && s.directSdm == 0, "Case 7: bitperfect/能力未知 ⟹ dsd=64 但 directSdm=0（保守）");
        ck(has_note(s, "能力未知"), "Case 7: note 写明能力未知");
    }

    // ---- ⑧ PCM 升频：96k 设备不许写 384000 ----
    {
        const DeviceCaps c = wasapi_caps(k96k);
        const PresetSettings s = plan_preset(PresetProfile::Pcm, c, pcm_src(44100, 16));
        ck(s.rate == "96000", "Case 8: pcm/96k 设备 ⟹ rate=96000（不写 384000）", s.rate);
        ck(s.rate != "384000", "Case 8: pcm/96k 设备 ⟹ rate 不是 384000");
        ck(s.filter == "poly-sinc-tight-lp", "Case 8: pcm ⟹ filter=poly-sinc-tight-lp");
        ck(s.minTapsPerInput == 512, "Case 8: pcm ⟹ minTapsPerInput=512（高倍率下 20 kHz 的保命线）");
        ck(s.shaper == "auto", "Case 8: pcm ⟹ shaper=auto");
        ck(s.dsd == 0, "Case 8: pcm ⟹ dsd=0");
        ck(s.modFamily.empty() && s.modOrder == 0, "Case 8: pcm ⟹ 不动 modfamily/modorder");
        ck(s.directSdm == 0 && s.volumeDb == 0.0, "Case 8: pcm ⟹ directsdm=0、volume=0");
    }

    // ---- ⑨ PCM 升频：设备有 705.6k 也要按 384k 封顶并说明 ----
    {
        const DeviceCaps c = wasapi_caps(kFull);
        const PresetSettings s = plan_preset(PresetProfile::Pcm, c, pcm_src(44100, 16));
        ck(s.rate == "384000", "Case 9: pcm/705.6k 设备 ⟹ rate 封顶 384000", s.rate);
        ck(has_note(s, "封顶"), "Case 9: note 写明封顶（不是偷偷降档）");
        ck(rate_num(s.rate) <= kPresetRateCap, "Case 9: rate 数值 ≤ 384000");
    }

    // ---- ⑩ PCM 升频：探测为空 ⟹ follow + note（不假装支持） ----
    {
        const DeviceCaps c = wasapi_caps({});
        const PresetSettings s = plan_preset(PresetProfile::Pcm, c, pcm_src(44100, 16));
        ck(s.rate == "follow", "Case 10: pcm/探测为空 ⟹ rate=follow", s.rate);
        ck(has_note(s, "探测为空"), "Case 10: note 写明探测为空、已降级");
    }

    // ---- ⑪ 任何一组设备列表都不许把 rate 写成超过 384000 ----
    {
        bool allOk = true;
        std::string bad;
        const std::vector<std::vector<uint32_t>> sets = {
            kFull, k96k, k192k, {44100}, {48000, 96000}, {44100, 768000}, {768000}, {}
        };
        for (const auto& rs : sets) {
            const DeviceCaps c = wasapi_caps(rs);
            const PresetSettings s = plan_preset(PresetProfile::Pcm, c, pcm_src(44100, 16));
            const uint32_t v = rate_num(s.rate);
            if (v > kPresetRateCap) { allOk = false; bad = s.rate; }
        }
        ck(allOk, "Case 11: 8 组设备列表 ⟹ pcm 档 rate 从不超过 384000", bad);
    }

    // ---- ⑫ DSD 升频：设备没有 DSD 通路 ⟹ 降级为 PCM 升频 + note ----
    {
        DeviceCaps noDsd = wasapi_caps(k192k);
        noDsd.dop = false;
        noDsd.dopMax.clear();
        const PresetSettings s = plan_preset(PresetProfile::Dsd, noDsd, pcm_src(44100, 16));
        ck(s.dsd == 0, "Case 12: dsd/无 DSD 能力 ⟹ dsd=0（降级成 PCM）");
        ck(s.rate == "192000", "Case 12: dsd/无 DSD 能力 ⟹ rate=设备 PCM 最高档 192000", s.rate);
        ck(s.shaper == "auto", "Case 12: 降级后 shaper=auto（PCM 路径）");
        ck(s.modFamily.empty() && s.modOrder == 0, "Case 12: 降级后不动调制器");
        ck(has_note(s, "降级为 PCM"), "Case 12: note 写明已降级为 PCM 升频");
        ck(s.filter == "poly-sinc-tight-lp" && s.minTapsPerInput == 512,
           "Case 12: 降级后仍保留 poly-sinc-tight-lp + 512 taps");
    }

    // ---- ⑬ DSD 升频：原生最高档命中 + 调制器 ec/7 ----
    {
        const DeviceCaps c = asio_caps(kFull, "dsd256");
        const PresetSettings s = plan_preset(PresetProfile::Dsd, c, pcm_src(44100, 16));
        ck(s.dsd == 256, "Case 13: dsd/原生 dsd256 ⟹ 目标 DSD256");
        ck(s.modFamily == "ec" && s.modOrder == 7, "Case 13: dsd ⟹ modfamily=ec / modorder=7");
        ck(s.filter == "poly-sinc-tight-lp" && s.minTapsPerInput == 512,
           "Case 13: dsd ⟹ poly-sinc-tight-lp + 512 taps");
        ck(s.rate == "follow", "Case 13: dsd ⟹ rate=follow");
        ck(s.shaper == "none", "Case 13: dsd ⟹ shaper=none（DoP 强制无抖动）");
        ck(s.directSdm == 0, "Case 13: 源是 PCM ⟹ directSdm=0");
        ck(has_note(s, "原生"), "Case 13: note 写明走原生");
    }

    // ---- ⑭ DSD 升频：没有原生 ⟹ 退 DoP 最高档 ----
    {
        const DeviceCaps c = wasapi_caps(kFull);      // 705600 载波 ⟹ dopMax=dsd256
        const PresetSettings s = plan_preset(PresetProfile::Dsd, c, pcm_src(44100, 16));
        ck(s.dsd == 256, "Case 14: dsd/DoP ⟹ 目标 DSD256（705600/16）");
        ck(has_note(s, "DoP"), "Case 14: note 写明退 DoP");
    }

    // ---- ⑮ DSD 升频：源 DSD 与目标同速率 ⟹ directSdm=1；不同速率 ⟹ 0 ----
    {
        const DeviceCaps c = asio_caps(kFull, "dsd256");
        const PresetSettings same = plan_preset(PresetProfile::Dsd, c, dsd_src(256));
        ck(same.dsd == 256 && same.directSdm == 1,
           "Case 15: 源 DSD256 = 目标 DSD256 ⟹ directSdm=1");
        ck(has_note(same, "DirectSDM"), "Case 15: note 写明 DirectSDM 1-bit 直通");
        const PresetSettings diff = plan_preset(PresetProfile::Dsd, c, dsd_src(64));
        ck(diff.dsd == 256 && diff.directSdm == 0,
           "Case 15: 源 DSD64 ≠ 目标 DSD256 ⟹ directSdm=0（要重调制）");
        ck(has_note(diff, "重调制"), "Case 15: note 写明需要重调制");
    }

    // ---- ⑯ DSD 升频：驱动声称 DSD1024 ⟹ 封顶 DSD512 + note ----
    {
        const DeviceCaps c = asio_caps(kFull, "dsd1024");
        const PresetSettings s = plan_preset(PresetProfile::Dsd, c, pcm_src(44100, 16));
        ck(s.dsd == 512, "Case 16: 驱动声称 dsd1024 ⟹ 目标封顶 DSD512", std::to_string(s.dsd));
        ck(has_note(s, "封顶"), "Case 16: note 写明按 DSD512 封顶");
    }

    // ---- ⑰ bitperfect：多设备 × 多源矩阵都必须满足七条件（PCM 源） ----
    {
        struct Case { const char* name; DeviceCaps c; };
        std::vector<Case> cases;
        cases.push_back({ "wasapi-full", wasapi_caps(kFull) });
        cases.push_back({ "wasapi-96k", wasapi_caps(k96k) });
        cases.push_back({ "wasapi-192k", wasapi_caps(k192k) });
        cases.push_back({ "wasapi-empty", wasapi_caps({}) });
        cases.push_back({ "asio-native-dsd512", asio_caps(kFull, "dsd512") });
        DeviceCaps shared = wasapi_caps(kFull);
        shared.exclusive = false;
        cases.push_back({ "shared-non-exclusive", shared });
        std::vector<SourceInfo> srcs;
        srcs.push_back(pcm_src(44100, 16));
        srcs.push_back(pcm_src(44100, 24));
        srcs.push_back(pcm_src(96000, 24));
        srcs.push_back(pcm_src(192000, 32));
        int matrixFail = 0;
        std::string firstBad;
        for (const auto& cs : cases) {
            for (const auto& src : srcs) {
                DeviceCaps dc = cs.c;
                if (src.srcBits == 32) dc.bits = 32;    // 32-bit 源需要 32-bit 输出才不降位深
                const PresetSettings s = plan_preset(PresetProfile::BitPerfect, dc, src);
                const BitPerfectCheck bp = check_bit_perfect(s, dc, src);
                if (!bp.ok) {
                    ++matrixFail;
                    if (firstBad.empty()) firstBad = std::string(cs.name) + ": " + bp.violations[0];
                }
            }
        }
        ck(matrixFail == 0, "Case 17: 6 设备 × 4 源 ⟹ bitperfect 档七条件全部成立", firstBad);
    }

    // ---- ⑱ 档位名 / 解析 / DoP 载波换算 ----
    {
        PresetProfile p = PresetProfile::Pcm;
        ck(preset_profile_parse("bitperfect", p) && p == PresetProfile::BitPerfect,
           "Case 18: profile=bitperfect 解析正确");
        ck(preset_profile_parse("PCM", p) && p == PresetProfile::Pcm, "Case 18: profile=PCM 大小写不敏感");
        ck(preset_profile_parse("dsd", p) && p == PresetProfile::Dsd, "Case 18: profile=dsd 解析正确");
        ck(!preset_profile_parse("dsd128", p), "Case 18: 未知 profile ⟹ 解析失败（由 API 回 400）");
        ck(std::string(preset_profile_name(PresetProfile::BitPerfect)) == "bitperfect" &&
           std::string(preset_profile_name(PresetProfile::Dsd)) == "dsd",
           "Case 18: 规范名 bitperfect/dsd");
        ck(dop_carrier_for_level(64) == 176400 && dop_carrier_for_level(256) == 705600 &&
           dop_carrier_for_level(512) == 1411200,
           "Case 18: DoP 载波 64/256/512 = 176400/705600/1411200");
        ck(dsd_level_from_name("dsd128") == 128 && dsd_level_from_name("512") == 512 &&
           dsd_level_from_name("dsd999") == 0, "Case 18: 档位名解析（含非法值）");
        ck(dsd_level_of_bitrate(2822400) == 64 && dsd_level_of_bitrate(11289600) == 256 &&
           dsd_level_of_bitrate(3072000) == 64, "Case 18: 位率→档位（44.1k 与 48k 族）");
        ck(preset_dop_max_from_rates(kFull) == "dsd256" && preset_dop_max_from_rates(k96k).empty(),
           "Case 18: DoP 最高档由 PCM 载波率推得（96k 设备没有 DoP）");
    }

    if (g_fails == 0) std::printf("\n[ALL PASS] 三档预设决策单测全部通过\n");
    else               std::printf("\n[FAILED] %d 项未通过\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
