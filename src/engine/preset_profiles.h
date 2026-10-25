// src/engine/preset_profiles.h — 「三档官方推荐预设」的纯决策层
//
// 用户 2026-10-25 钦定：三档官方推荐（原音直通 / PCM 升频 / DSD 升频）都要**自动探测硬件能力**，
// 一键把设置调到对应目标。本文件只做「能力 + 源信息 → 设置」的纯函数决策：
//   * 不碰设备、不读文件、不做 IO ⟹ 可以在没有声卡的机器上离线单测（tests/test_preset_profiles.cpp）；
//   * 探测本身在 tools/rtaweb.cpp（WASAPI IsFormatSupported / ASIO canSampleRate 与
//     kAsioCanDoIoFormat 都是**查询**，不开流、不建缓冲、不抢占设备）；
//   * 探测失败/未知一律**保守降级**，并把原因写进 notes —— 绝不静默假装支持。
//
// 三档口径（字段名就是 /api/play 的请求键名，也是 /api/preset/apply 的 applied 字段）：
//   bitperfect（原音直通）：完全不动数据
//       rate=follow（跟随源，不重采样）· volume=0 dB · shaper=none · 无 EQ/IR/等响度/每声道增益/ISP 预衰减
//       源是 PCM ⟹ dsd=0；源是 DSD 且设备能承载 ⟹ dsd=<源位率>，同速率 ⟹ directSdm=1（1-bit 直通）
//   pcm（PCM 升频）：
//       rate = 设备当前后端支持的最高档（**不超过 384000**）· filter=poly-sinc-tight-lp · tapsPerInput=512
//       shaper=auto · dsd=0
//       为什么必须 minTapsPerInput=512：高倍率下抽头总预算被摊到每个相位后只剩几个抽头，
//       20 kHz 处会掉 5 dB（见 docs/PCM回放质量-实测与改进.md §4.1）—— 这是本档的关键，
//       不能靠"预设自带 taps"隐式满足。
//   dsd（DSD 升频）：
//       dsd = 设备原生支持的最高 DSD 档（没有原生则退 DoP 的最高档）· modfamily=ec · modorder=7
//       filter=poly-sinc-tight-lp · tapsPerInput=512 · rate=follow · shaper=none（DoP 强制无抖动）
//       源 DSD 与目标同速率 ⟹ directSdm=1
//
// 为什么 shaper 在 dsd 档写 none：DoP 路径在 rtaweb 里是 `cfg.shaper = Shaper::None`
// （抖动交给调制器之后的 1-bit 量化），写 auto 只是好看、实际不生效 —— 宁可写真实值。
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace rta::engine {

// 升频档的采样率上限：官方推荐不超过 384 kHz（705.6k/768k 属于"展示指标"，实测听感与
// 稳定性都不划算，且吃 CPU）。设备支持更高时按 384000 封顶并在 notes 里说明。
constexpr uint32_t kPresetRateCap = 384000u;

// DSD 档的位率上限：本项目的调制器/输出链最远只到 DSD512（见 docs/SDM-调制器纪要.md 的
// SNR 表）。驱动若连 DSD1024 都报"接受"（本机 AudioByte 实测：canSampleRate(45158400)=OK），
// 只能说明它对采样率不做校验，不能当成真能力 —— 一律按 DSD512 封顶并记 note。
constexpr int kPresetDsdMaxLevel = 512;

enum class PresetProfile { BitPerfect, Pcm, Dsd };

inline const char* preset_profile_name(PresetProfile p) {
    switch (p) {
        case PresetProfile::BitPerfect: return "bitperfect";
        case PresetProfile::Pcm:        return "pcm";
        case PresetProfile::Dsd:        return "dsd";
    }
    return "pcm";
}

// 解析 /api/preset/apply?profile=… 的取值。接受几个等价写法（bit-perfect / bp / sdmod），
// 但输出的规范名永远是 preset_profile_name() 的三个之一 —— 界面上只显示规范名。
inline bool preset_profile_parse(const std::string& s, PresetProfile& out) {
    std::string v;
    v.reserve(s.size());
    for (char c : s) v.push_back((char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    if (v == "bitperfect" || v == "bit-perfect" || v == "bp") { out = PresetProfile::BitPerfect; return true; }
    if (v == "pcm" || v == "pcm-upsample" || v == "upsample") { out = PresetProfile::Pcm; return true; }
    if (v == "dsd" || v == "sdmod" || v == "dsd-upsample") { out = PresetProfile::Dsd; return true; }
    return false;
}

// ---------------- DSD 档位换算（纯算术，与 dsp/dop.h 同口径） ----------------
// DSD 位率 = 基率 × 倍率：44.1k 族 DSD64 = 2822400，48k 族 DSD64 = 3072000（两者都认）。
inline const char* dsd_level_name(int mult) {
    switch (mult) {
        case 64:   return "dsd64";
        case 128:  return "dsd128";
        case 256:  return "dsd256";
        case 512:  return "dsd512";
        // 1024 只用于"识别驱动声称的能力"（日志/封顶提示），不是可交付的档位：
        // 本项目的调制器只到 DSD512，见 kPresetDsdMaxLevel。
        case 1024: return "dsd1024";
        default:   return "";
    }
}

// "dsd256" / "256" / "DSD256" → 256；无法识别 → 0（0 = 无 DSD 能力，调用方必须按降级处理）
inline int dsd_level_from_name(const std::string& s) {
    int mult = 0;
    size_t i = 0;
    while (i < s.size() && (s[i] == 'd' || s[i] == 'D' || s[i] == 's' || s[i] == 'S')) ++i;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') break;
        mult = mult * 10 + (int)(s[i] - '0');
    }
    for (int ok : {64, 128, 256, 512, 1024}) if (mult == ok) return mult;
    return 0;
}

// DSD 位率 → 档位（44.1k 族与 48k 族都认；认不出返回 0）
inline int dsd_level_of_bitrate(uint32_t bits) {
    if (bits == 0) return 0;
    for (int m : {64, 128, 256, 512}) {
        if (bits == (uint32_t)(44100 * m) || bits == (uint32_t)(48000 * m)) return m;
    }
    return 0;
}

// DoP 载波 = DSD 位率 / 16（DSD64→176400 / DSD128→352800 / DSD256→705600 / DSD512→1411200）
inline uint32_t dop_carrier_for_level(int mult) {
    if (mult <= 0) return 0;
    return (uint32_t)(44100 * mult) / 16u;
}

// ---------------- 数据契约 ----------------
// 硬件能力：由 rtaweb 的只读探测填（字段名与 /api/preset/caps 的 device 对象一致）
struct DeviceCaps {
    std::string backend;                 // "asio" | "wasapi"
    std::vector<uint32_t> rates;         // PCM 采样率（升序；空 = 探测为空/未知）
    uint32_t maxRate = 0;                // 设备实际最高档（不封顶；0 = 未知）
    bool dsdNative = false;              // ASIO 2.3 IoFormat DSD 且 DSD 位率被接受
    std::string dsdNativeMax;            // "dsd64"…"dsd512"；空 = 无原生 DSD
    bool dop = false;                    // 有 ≥176400 的 24-bit 承载率才可能 DoP
    std::string dopMax;                  // DoP 能承载的最高档
    bool exclusive = false;              // 独占/直连（WASAPI 独占或 ASIO）
    int bits = 24;
    int channels = 2;
    // 以下不进 API 契约，只给日志/notes 用
    std::string deviceName;
    std::string probeNote;               // 非空 = 探测没拿到可信结果的原因
};

struct SourceInfo {
    bool isDsd = false;                  // 源是 .dsf/.dff（1-bit）
    uint32_t srcRate = 44100;            // PCM: 采样率；DSD: 1-bit 位率（如 2822400）
    int srcBits = 16;
    bool isStream = false;
};

// 设置：字段名与 /api/play 的请求键名同义（applied JSON 由 rtaweb 从这里生成）
struct PresetSettings {
    std::string rate = "follow";         // "follow" | "src" | "max" | "384000"（数字串）
    int dsd = 0;                         // 0 = PCM；64/128/256/512 = DSD 输出档
    std::string modFamily;               // "" = 不改（默认 dsd）；dsd 档写 "ec"
    int modOrder = 0;                    // 0 = 不改；dsd 档写 7
    std::string filter;                  // "" = 用默认 Kaiser；升频档写 poly-sinc-tight-lp
    int minTapsPerInput = 0;             // 0 = 用预设自带；升频档写 512
    std::string shaper = "none";         // "none" | "auto" | 具体型号
    double volumeDb = 0.0;
    int directSdm = 0;                   // 1 = 源 DSD 与目标同速率逐位直通
    bool clearEqIr = false;              // true = 清空 EQ/IR/等响度/每声道增益/ISP 预衰减
    // 不属于设置项：决策过程中的降级/事实说明（界面"为什么这样"用）
    std::vector<std::string> notes;
};

// ---------------- 小工具 ----------------
// 设备 PCM 采样率里不超过上限的最高档（0 = 没有可用档，调用方必须降级）
inline uint32_t preset_max_pcm_rate(const std::vector<uint32_t>& rates, uint32_t cap = kPresetRateCap) {
    uint32_t best = 0;
    for (uint32_t r : rates) {
        if (r == 0 || r > cap) continue;
        if (r > best) best = r;
    }
    return best;
}

// DoP 承载能力：PCM 采样率里每个都可能是载波，取最高档（必须 ≥176400 才算"能 DoP"）
inline std::string preset_dop_max_from_rates(const std::vector<uint32_t>& rates) {
    int best = 0;
    for (uint32_t r : rates) {
        for (int m : {64, 128, 256, 512}) {
            if (r == dop_carrier_for_level(m) && m > best) best = m;
            // 48k 族 DSD 的载波（230400/460800/921600/1843200）也认：源是 48k 族时用得上
            const uint32_t c48 = (uint32_t)(48000 * m) / 16u;
            if (r == c48 && m > best) best = m;
        }
    }
    return best > 0 ? std::string(dsd_level_name(best)) : std::string();
}

namespace detail {
inline std::string num(uint32_t v) { return std::to_string((unsigned long long)v); }
inline std::string numi(int v) { return std::to_string(v); }
}  // namespace detail

// ---------------- 决策主体 ----------------
// 三档共同点：
//   · 音量 0 dB —— 升频档不该靠"砍音量"换稳定（预衰减交给 DSP 的 ISP/调制器余量）。
//   · clearEqIr=true —— 一键预设必须是"官方推荐链路"：用户 EQ/IR 留在链路里，
//     "我明明选了原音直通却过了 EQ"就会变成一句谎话。apply 会清掉这些键并在 notes 写明。
//   · 能力未知时**不猜**：该写的降级与 note 一条不少。
inline PresetSettings plan_preset(PresetProfile prof, const DeviceCaps& caps, const SourceInfo& src) {
    PresetSettings s;
    s.volumeDb = 0.0;
    s.shaper = "none";
    s.clearEqIr = true;

    // 源 DSD 位率：认不出就按 DSD64 处理并如实写 note（宁可少升、不可乱升）
    int srcLevel = src.isDsd ? dsd_level_of_bitrate(src.srcRate) : 0;
    if (src.isDsd && srcLevel == 0) {
        srcLevel = 64;
        s.notes.push_back("源 DSD 位率未识别（srcRate=" + detail::num(src.srcRate) + "）：按 DSD64 处理");
    }
    const int nativeLvl = dsd_level_from_name(caps.dsdNativeMax);
    const int dopLvl = dsd_level_from_name(caps.dopMax);
    // 设备实际能承载的最高 DSD 档（原生优先，其次 DoP）
    int capLevel = 0;
    if (caps.dsdNative && nativeLvl > 0) capLevel = nativeLvl;
    if (caps.dop && dopLvl > capLevel) capLevel = dopLvl;
    if (capLevel > kPresetDsdMaxLevel) capLevel = kPresetDsdMaxLevel;

    // 探测没拿到可信结果（WASAPI 独占列表为空且无 DSD 信息 / ASIO 没探测成功）
    const bool capsUnknown = caps.rates.empty() && !caps.dsdNative && !caps.dop;

    if (!caps.probeNote.empty()) s.notes.push_back(caps.probeNote);

    // ---------------- ① 原音直通 ----------------
    if (prof == PresetProfile::BitPerfect) {
        s.rate = "follow";
        s.filter.clear();
        s.minTapsPerInput = 0;
        s.dsd = 0;
        s.directSdm = 0;
        if (!src.isDsd) {
            s.notes.push_back("源是 PCM ⟹ dsd=0：不调制、不重采样、不动数据");
            if (!capsUnknown && !caps.rates.empty()) {
                s.notes.push_back("设备 PCM 支持列表最高 " + detail::num(caps.maxRate) +
                                  " Hz；直通按源采样率开流，不升频");
            }
            return s;
        }
        // 源是 DSD：目标就是源位率本身（逐位直通才有意义）
        const bool nativeOk = caps.dsdNative && nativeLvl >= srcLevel;
        const bool dopOk = caps.dop && dopLvl >= srcLevel;
        if (capsUnknown) {
            s.dsd = srcLevel;
            s.directSdm = 0;
            s.notes.push_back("设备能力未知（探测为空）：按 DSD" + detail::numi(srcLevel) +
                              " 设置目标位率，但不启用 DirectSDM（保守：不承诺直通）");
            return s;
        }
        if (nativeOk || dopOk) {
            s.dsd = srcLevel;
            s.directSdm = 1;
            s.notes.push_back("源 DSD" + detail::numi(srcLevel) +
                              " 与目标同速率 ⟹ DirectSDM 1-bit 直通（跳过 PCM 与调制）");
            if (nativeOk) {
                s.notes.push_back("设备原生支持 DSD（最高 " + caps.dsdNativeMax +
                                  "）：封装选「原生」时 1-bit 直送 ASIO；DoP 载波 " +
                                  detail::num(dop_carrier_for_level(srcLevel)) + " Hz 同样逐位直通");
            } else {
                s.notes.push_back("设备没有原生 DSD（DoP 最高 " + caps.dopMax +
                                  "）：按 DoP 1-bit 直通，码流逐位一致（DoP 只是承载容器）");
            }
            return s;
        }
        // 源位率超出设备承载能力：直通做不到，降级到设备最高档并明说"这不是 bit-perfect"
        s.dsd = capLevel;
        s.directSdm = 0;
        s.notes.push_back("✗ 设备无法承载 DSD" + detail::numi(srcLevel) + "（原生最高 " +
                          (caps.dsdNativeMax.empty() ? std::string("无") : caps.dsdNativeMax) +
                          "，DoP 最高 " + (caps.dopMax.empty() ? std::string("无") : caps.dopMax) +
                          "）：已降级为 DSD" + detail::numi(capLevel) +
                          " 重调制 —— 这已不是原音直通，且不再满足 bit-perfect 条件⑥");
        return s;
    }

    // ---------------- ② PCM 升频 ----------------
    if (prof == PresetProfile::Pcm) {
        s.filter = "poly-sinc-tight-lp";
        s.minTapsPerInput = 512;      // 关键：高倍率下保住每相位的抽头数（20 kHz 不掉 5 dB）
        s.shaper = "auto";
        s.dsd = 0;
        s.directSdm = 0;
        const uint32_t devBest = caps.rates.empty() ? 0u : caps.rates.back();
        const uint32_t target = preset_max_pcm_rate(caps.rates, kPresetRateCap);
        if (target == 0) {
            s.rate = "follow";
            s.notes.push_back("设备能力探测为空（未能确认独占支持的采样率）：升频档降级为「跟随源」，"
                              "不写死 384000（不假装支持）");
            return s;
        }
        s.rate = detail::num(target);
        s.notes.push_back("检测到设备最高支持 " + detail::num(target) + " Hz（PCM）：本档按它升频");
        if (devBest > kPresetRateCap) {
            s.notes.push_back("设备还支持 " + detail::num(devBest) + " Hz，但升频档按官方推荐封顶 " +
                              detail::num(kPresetRateCap) + " Hz（更高档 CPU/稳定性不划算）");
        }
        if (!src.isDsd && src.srcRate > 0 && target <= src.srcRate) {
            s.notes.push_back("设备最高 " + detail::num(target) + " Hz 不高于本源 " +
                              detail::num(src.srcRate) + " Hz：本档不会升频" +
                              (target < src.srcRate ? "（会降采样）" : ""));
        }
        if (src.isDsd) {
            s.notes.push_back("源是 DSD：本档先把 DSD 解成 PCM 再升频（要 1-bit 直通请用原音直通档）");
        }
        s.notes.push_back("已清空 EQ/IR/等响度/每声道增益（官方推荐链路的基线）");
        return s;
    }

    // ---------------- ③ DSD 升频 ----------------
    s.filter = "poly-sinc-tight-lp";
    s.minTapsPerInput = 512;
    s.shaper = "none";                // DoP 路径强制无抖动（抖动交给 1-bit 量化）
    s.rate = "follow";                // 原生 DSD 要求跟随源；DoP 忽略 rate（载波由 dsd 档决定）
    if (capLevel == 0) {
        // 设备完全没有 DSD 通路：保守降级为 PCM 升频（让声音出来），并把事实写清
        const uint32_t target = preset_max_pcm_rate(caps.rates, kPresetRateCap);
        s.dsd = 0;
        s.directSdm = 0;
        s.modFamily.clear();
        s.modOrder = 0;
        if (target == 0) {
            s.rate = "follow";
        } else {
            s.rate = detail::num(target);
            s.shaper = "auto";
        }
        s.notes.push_back("✗ 设备没有 DSD 通路（原生 " +
                          (caps.dsdNativeMax.empty() ? std::string("无") : caps.dsdNativeMax) +
                          "、DoP " + (caps.dopMax.empty() ? std::string("无") : caps.dopMax) +
                          "）：DSD 升频档无法兑现，已降级为 PCM 升频（" +
                          (target == 0 ? std::string("跟随源") : (detail::num(target) + " Hz")) + "）");
        return s;
    }
    s.dsd = capLevel;
    s.modFamily = "ec";
    s.modOrder = 7;
    s.directSdm = (src.isDsd && srcLevel == capLevel) ? 1 : 0;
    if (caps.dsdNative && nativeLvl >= capLevel) {
        s.notes.push_back("设备原生支持 DSD，最高 " + caps.dsdNativeMax + " ⟹ 目标 DSD" +
                          detail::numi(capLevel) + "（封装选「原生」可 1-bit 直送）");
    } else {
        s.notes.push_back("设备无原生 DSD：退 DoP，最高 " + caps.dopMax + " ⟹ 目标 DSD" +
                          detail::numi(capLevel) + "（载波 " +
                          detail::num(dop_carrier_for_level(capLevel)) + " Hz）");
    }
    if (nativeLvl > kPresetDsdMaxLevel || dopLvl > kPresetDsdMaxLevel) {
        s.notes.push_back("驱动声称支持高于 DSD512 的位率：本档按 DSD" +
                          detail::numi(kPresetDsdMaxLevel) + " 封顶（不对超规格档位打包票）");
    }
    if (s.directSdm) {
        s.notes.push_back("源 DSD" + detail::numi(srcLevel) + " 与目标同速率 ⟹ DirectSDM 1-bit 直通（跳过调制）");
    } else if (src.isDsd && srcLevel > 0) {
        s.notes.push_back("源 DSD" + detail::numi(srcLevel) + " ≠ 目标 DSD" + detail::numi(capLevel) +
                          "：需要重调制（DirectSDM 仅在同速率时才逐位直通）");
    }
    s.notes.push_back("EC-7 调制器（实测 DSD64/128/256 稳定；预衰减由 DSP 的调制器余量自动给）");
    s.notes.push_back("已清空 EQ/IR/等响度/每声道增益（官方推荐链路的基线）");
    return s;
}

// ---------------- bit-perfect 七条件自检 ----------------
// 判据与 rtaweb 信号链的 bitPerfect 结论（rtaweb.cpp 的「三态 direct/yes/no」）同口径：
//   ① 不重采样（rate 跟随源）        ② 音量 0 dB
//   ③ 无抖动/噪声整形                ④ 无预衰减（ISP/调制器余量）
//   ⑤ 无 EQ / IR 卷积                ⑥ 非 DoP 载波（DSD 源必须 1-bit 直通）
//   ⑦ 位深未降（源真实位深 ≤ 输出位深）
// ⑥ 的写法说明：DSD 源若不直通，1-bit 流会被解成 PCM 再调制 —— 那已经不逐位了；
// 所以这里要求 directSdm=1（DirectSDM 与原生 DSD 都算），与 rtaweb 的 bpState="direct" 同义。
struct BitPerfectCheck {
    bool ok = true;
    std::vector<std::string> violations;
};

inline BitPerfectCheck check_bit_perfect(const PresetSettings& s, const DeviceCaps& caps,
                                         const SourceInfo& src) {
    BitPerfectCheck r;
    auto bad = [&r](const std::string& why) { r.ok = false; r.violations.push_back(why); };
    if (s.rate != "follow") bad("①重采样：rate=" + (s.rate.empty() ? std::string("(空)") : s.rate));
    if (std::fabs(s.volumeDb) > 1e-9) bad("②音量非 0 dB");
    if (s.shaper != "none") bad("③抖动/整形：" + s.shaper);
    if (!s.clearEqIr) bad("④⑤EQ/IR/预衰减未清空");
    if (!src.isDsd) {
        if (s.dsd != 0) bad("⑥PCM 源却开了 DSD/DoP 输出（dsd=" + detail::numi(s.dsd) + "）");
    } else if (s.directSdm != 1) {
        bad("⑥DSD 源未走 1-bit 直通（会被解成 PCM 再调制）");
    }
    if (!src.isDsd && src.srcBits > 0) {
        const int outBits = caps.bits > 0 ? caps.bits : 24;
        if (outBits < src.srcBits) {
            bad("⑦位深降低：" + detail::numi(outBits) + " bit < 源 " + detail::numi(src.srcBits) + " bit");
        }
    }
    return r;
}

}  // namespace rta::engine
