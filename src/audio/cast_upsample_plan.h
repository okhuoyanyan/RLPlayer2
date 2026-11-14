// SPDX-FileCopyrightText: 2026 rainlain <https://github.com/okhuoyanyan>
// SPDX-FileCopyrightText: 2026 BH2VOQ <https://github.com/BH2VOQ>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// 本文件是 RLPlayer 的一部分，以 GNU 通用公共许可证第 3 版（或更新版本）发布。
// 完整条款见仓库根目录的 LICENSE 文件。
//
// cast_upsample_plan.h —— UPnP 投放升频的「目标采样率与位深决策」（纯函数、无 IO、无全局状态）
//
// 设计原则与性质：
//   * 纯函数（Pure function）：无文件 IO、无网络、无全局状态、无静态可变变量、无平台依赖头文件；
//   * 规则来源：RLPlayer 1.1.3 UPnP 投放升频决策规范（ROADMAP-1.1.x.md / docs/UPnP投放升频-计划书.md §3.2）；
//   * 族约束：44.1k 族（44100/88200/176400/352800）与 48k 族（48000/96000/192000/384000/768000）不互转，
//     实现上严格要求目标采样率必须是源采样率的整数倍（target > source && target % source == 0）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <initializer_list>

namespace rta::audio {

struct CastRateDecision {
    uint32_t    targetRate = 0;   // 最终供源采样率
    bool        upsample   = false;
    bool        capped     = false;  // 请求值被 ceiling/渲染器能力压低过
    bool        undeclared = false;  // 渲染器未声明任何速率
    bool        forced     = false;  // 强制升频：目标率未经渲染器声明验证（用户显式勾选）
    std::string reason;              // 机器可读枚举，见下表
    std::string noteZh;              // 用户可见中文真相（可为空：off/src 时也给一句话更好）
    std::string noteEn;              // 同位英文（非空、不含中文）
};

inline CastRateDecision decide_cast_target_rate(uint32_t sourceRate,
                                                const std::string& requested,
                                                const std::vector<uint32_t>& rendererRates,
                                                uint32_t ceiling,
                                                bool applyUpsample,
                                                bool forceRate = false);

struct CastBitDecision {
    int servedBits = 0;
    bool reduced = false;
    std::string noteZh;
    std::string noteEn;
};

inline CastBitDecision decide_cast_served_bits(int sourceBits, int rendererMaxBits);

// 兼容别名（主控与 B 包对接可用）
inline CastBitDecision decide_served_bits(int sourceBits, int rendererMaxBits);

namespace cast_plan_detail {

inline bool has_cjk(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0xE3) return true; // UTF-8 三字节首字节：CJK汉字/标点
    }
    return false;
}

inline std::string trim_ascii(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// 只接受纯十进制正整数；"0"、负数、带小数、超 uint32 一律判非法（回退源率，不猜）
inline bool parse_rate(const std::string& raw, uint32_t& out) {
    const std::string t = trim_ascii(raw);
    if (t.empty() || t.size() > 10) return false;
    for (char c : t) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(t.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    if (v == 0 || v > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

// 忽略 0（0 = 不设该项）后取最小值；全 0 返回 0
inline uint32_t min_non_zero(uint32_t a, uint32_t b) {
    if (a == 0) return b;
    if (b == 0) return a;
    return std::min(a, b);
}

inline uint32_t min_non_zero(uint32_t a, uint32_t b, uint32_t c) {
    return min_non_zero(min_non_zero(a, b), c);
}

inline std::string join_rates(const std::vector<uint32_t>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += "/";
        s += std::to_string(v[i]);
    }
    return s;
}

inline std::string family_label(uint32_t rate) {
    if (rate != 0 && rate % 44100u == 0) return "44.1 kHz";
    if (rate != 0 && rate % 48000u == 0) return "48 kHz";
    return std::to_string(rate) + " Hz";
}

} // namespace cast_plan_detail

inline CastRateDecision decide_cast_target_rate(uint32_t sourceRate,
                                                const std::string& requested,
                                                const std::vector<uint32_t>& rendererRates,
                                                uint32_t ceiling,
                                                bool applyUpsample,
                                                bool forceRate) {
    CastRateDecision d;
    d.targetRate=sourceRate;

    // 1. applyUpsample == false → target=sourceRate, upsample=false, reason="off"
    if (!applyUpsample) {
        d.targetRate = sourceRate;
        d.upsample = false;
        d.capped = false;
        d.undeclared = false;
        d.reason = "off";
        d.noteZh = "投放升频未开启，按源采样率直供";
        d.noteEn = "Cast upsampling is disabled; serving at source sample rate.";
        return d;
    }

    // 2. sourceRate == 0 → target=0, upsample=false, reason="invalid-source"
    if (sourceRate == 0) {
        d.targetRate = 0;
        d.upsample = false;
        d.capped = false;
        d.undeclared = false;
        d.reason = "invalid-source";
        d.noteZh = "源采样率无效（0 Hz）";
        d.noteEn = "Invalid source sample rate (0 Hz).";
        return d;
    }

    // 3. requested ∈ {"", "src", "follow"} → target=sourceRate, upsample=false, reason="src"
    const std::string trimmed = cast_plan_detail::trim_ascii(requested);
    const std::string lowerReq = cast_plan_detail::lower_ascii(trimmed);
    if (lowerReq.empty() || lowerReq == "src" || lowerReq == "follow") {
        d.targetRate = sourceRate;
        d.upsample = false;
        d.capped = false;
        d.undeclared = false;
        d.reason = "src";
        d.noteZh = "跟随源采样率直供（" + std::to_string(sourceRate) + " Hz）";
        d.noteEn = "Following source sample rate (" + std::to_string(sourceRate) + " Hz); direct streaming.";
        return d;
    }

    uint32_t rendererMax = 0;
    for (uint32_t r : rendererRates) {
        if (r > rendererMax) rendererMax = r;
    }

    uint32_t cap = 0;
    bool isNumeric = false;
    uint32_t reqNum = 0;

    // 4. requested == "max"
    if (lowerReq == "max") {
        if (rendererRates.empty()) {
            d.undeclared = true;
            cap = ceiling;
        } else {
            cap = cast_plan_detail::min_non_zero(ceiling, rendererMax);
        }
        // 9. 'max' 时：若 rendererMax > 0 && ceiling > 0 && cap < ceiling → capped = true
        if (rendererMax > 0 && ceiling > 0 && cap < ceiling) {
            d.capped = true;
        }
    } else {
        // 5. requested 是数字
        if (!cast_plan_detail::parse_rate(trimmed, reqNum)) {
            d.targetRate = sourceRate;
            d.upsample = false;
            d.capped = false;
            d.undeclared = false;
            d.reason = "invalid-request";
            d.noteZh = "请求采样率无效（\"" + requested + "\"），按源采样率直供";
            d.noteEn = "Invalid requested sample rate (\"" + requested + "\"); serving at source sample rate.";
            return d;
        }
        isNumeric = true;
        if (rendererRates.empty()) {
            d.undeclared = true;
        }
        cap = cast_plan_detail::min_non_zero(reqNum, ceiling, rendererMax);
        if (cap < reqNum) {
            d.capped = true;
        }
    }

    // 5b. 强制目标率（用户显式勾选）：忽略"必须被声明"这一条，但**同族整数倍与上限仍然成立**。
    //     为什么要它：实测 Rygel（MR4 音响）只声明 audio/l16;rate=44100，却实际能收 96k WAV；
    //     声明只是它写得保守，不该由我们替用户判定"设备不行"。
    //     代价必须写在 note 里：目标率没有声明背书，可能被拒播 —— 绝不与普通升频混为一谈。
    if (forceRate) {
        const bool isMaxReq = (lowerReq == "max");
        // 强制只受"本机上限"约束：既然明确忽略渲染器声明，就不能再拿声明去当上限，
        // 否则"强制"会退化成"看声明脸色"。上限没设就明确拒绝，不猜一个目标出来。
        const uint32_t fcap = ceiling;
        if (fcap == 0) {
            d.reason = "no-same-family";
            d.noteZh = "已启用强制目标率，但没有设置投放升频上限，无法确定目标，按源直供";
            d.noteEn = "Forced target rate is on, but no cast upsampling ceiling is set; "
                       "cannot determine a target, serving at source rate.";
            return d;
        }
        uint32_t best = 0;
        bool cappedHere = false;
        if (isMaxReq) {
            if (fcap > sourceRate) best = (fcap / sourceRate) * sourceRate;   // 源率整数倍里最大的
        } else if (reqNum > sourceRate && (reqNum % sourceRate) == 0 && reqNum <= fcap) {
            best = reqNum;
        } else if (reqNum > fcap) {
            cappedHere = true;
            if (fcap > sourceRate) best = (fcap / sourceRate) * sourceRate;
        }
        if (best > sourceRate) {
            const bool declared = std::find(rendererRates.begin(), rendererRates.end(), best) != rendererRates.end();
            d.targetRate = best;
            d.upsample = true;
            d.forced = !declared;
            d.capped = d.capped || cappedHere;
            d.reason = "ok";
            const std::string srcStr = std::to_string(sourceRate);
            const std::string dstStr = std::to_string(best);
            const std::string declStr = rendererRates.empty() ? std::string("(未声明)")
                                                              : cast_plan_detail::join_rates(rendererRates);
            if (declared) {
                d.noteZh = "源采样率 " + srcStr + " Hz 升频至 " + dstStr + " Hz（渲染器声明：" + declStr + " Hz）";
                d.noteEn = "Source rate " + srcStr + " Hz upsampled to " + dstStr +
                           " Hz (renderer declared: " + declStr + " Hz).";
            } else {
                d.noteZh = "已强制升频 " + srcStr + " Hz → " + dstStr + " Hz（渲染器声明：" + declStr +
                           " Hz，未包含 " + dstStr + " Hz；此速率未经设备声明验证，可能被拒播）";
                d.noteEn = "Forced upsampling " + srcStr + " Hz -> " + dstStr + " Hz (renderer declared: " +
                           declStr + " Hz, which does not include " + dstStr +
                           " Hz; this rate is not backed by the device declaration and may be refused).";
            }
            return d;
        }
        // 强制也没算出合法目标 → 按"用户请求"本身给原因（不能用声明算出的 cap 去解释，
        // 否则用户看到的是"降采样拒绝"，而真实原因是"你填的不是源率的整数倍"）。
        if (!isMaxReq) {
            if (reqNum == sourceRate) {
                d.reason = "already";
                d.noteZh = "源采样率已是目标速率 " + std::to_string(sourceRate) + " Hz，无需升频，按源直供";
                d.noteEn = "Source sample rate is already at target rate " + std::to_string(sourceRate) +
                           " Hz; serving at source rate.";
            } else if (reqNum < sourceRate) {
                d.reason = "no-down";
                d.noteZh = "强制目标率 " + std::to_string(reqNum) + " Hz 低于源 " +
                           std::to_string(sourceRate) + " Hz，本版只升不降";
                d.noteEn = "Forced target " + std::to_string(reqNum) + " Hz is below the source " +
                           std::to_string(sourceRate) + " Hz; this version only upsamples.";
            } else {
                d.reason = "no-same-family";
                d.noteZh = "强制目标率 " + std::to_string(reqNum) + " Hz 不是源采样率 " +
                           std::to_string(sourceRate) + " Hz 的整数倍（同族规则不被强制绕过），按源直供";
                d.noteEn = "Forced target " + std::to_string(reqNum) + " Hz is not an integer multiple of the source " +
                           std::to_string(sourceRate) + " Hz (forcing does not bypass the same-family rule); serving at source rate.";
            }
            d.targetRate = sourceRate;
            d.upsample = false;
            return d;
        }
    }

    // 6. 候选集：
    // - 已声明（rendererRates 非空）：所有满足 r > sourceRate && r % sourceRate == 0 && r <= cap 的 r；
    // - 未声明（rendererRates 为空）：若 cap > sourceRate && cap % sourceRate == 0 则候选集 = {cap}。
    std::vector<uint32_t> candidates;
    if (!rendererRates.empty()) {
        for (uint32_t r : rendererRates) {
            if (r > sourceRate && (r % sourceRate == 0) && r <= cap) {
                candidates.push_back(r);
            }
        }
    } else {
        if (cap > sourceRate && (cap % sourceRate == 0)) {
            candidates.push_back(cap);
        }
    }

    // 7. 候选集非空 → targetRate = max(候选), upsample = true, reason="ok"
    if (!candidates.empty()) {
        uint32_t best = *std::max_element(candidates.begin(), candidates.end());
        d.targetRate = best;
        d.upsample = true;
        d.reason = "ok";
        std::string capZh, capEn;
        if (d.capped) {
            if (lowerReq == "max") {
                capZh = "（受上限/渲染器能力约束，被压至 " + std::to_string(cap) + " Hz）";
                capEn = " (capped to " + std::to_string(cap) + " Hz by ceiling or renderer capability)";
            } else {
                capZh = "（请求 " + std::to_string(reqNum) + " Hz 受上限/渲染器能力压至 " + std::to_string(cap) + " Hz）";
                capEn = " (requested " + std::to_string(reqNum) + " Hz capped to " + std::to_string(cap) + " Hz by ceiling or renderer capability)";
            }
        }
        if (d.undeclared) {
            d.noteZh = "源采样率 " + std::to_string(sourceRate) + " Hz 升频至 " + std::to_string(best) +
                       " Hz（渲染器未声明接收格式，按本机上限策略）" + capZh;
            d.noteEn = "Source rate " + std::to_string(sourceRate) + " Hz upsampled to " + std::to_string(best) +
                       " Hz (renderer undeclared, following ceiling policy)" + capEn + ".";
        } else {
            d.noteZh = "源采样率 " + std::to_string(sourceRate) + " Hz 升频至 " + std::to_string(best) +
                       " Hz（渲染器声明：" + cast_plan_detail::join_rates(rendererRates) + " Hz）" + capZh;
            d.noteEn = "Source rate " + std::to_string(sourceRate) + " Hz upsampled to " + std::to_string(best) +
                       " Hz (renderer declared: " + cast_plan_detail::join_rates(rendererRates) + " Hz)" + capEn + ".";
        }
        return d;
    }

    // 8. 候选集为空 → 按顺序判定 reason：
    //    - cap == sourceRate（或 numeric 时 requested == sourceRate）→ reason="already"（源已是目标率）；
    //    - cap < sourceRate → reason="no-down"（本功能只升不降）；
    //    - 否则（cap > source 但不是整数倍 / 渲染器没声明同族速率）→ reason="no-same-family"。
    //    - 以上三种一律 targetRate = sourceRate, upsample = false。
    d.targetRate = sourceRate;
    d.upsample = false;

    // 2026-11-05（真实设备实测暴露）：`cap == sourceRate` 有两种完全不同的成因，不能一律说
    // 「源已是目标率」——当渲染器**只声明到源率**（实测 Rygel 只声明 audio/l16;rate=44100）
    // 时，真实原因是"它没有更高的同族速率"，说成"无需升频"会让用户以为是自己设置的问题。
    // 只有用户**明确点名要源率**（numeric == sourceRate）才算 already。
    if (isNumeric && reqNum == sourceRate) {
        d.reason = "already";
        d.noteZh = "源采样率已是目标速率 " + std::to_string(sourceRate) + " Hz，无需升频，按源直供";
        d.noteEn = "Source sample rate is already at target rate " + std::to_string(sourceRate) + " Hz; serving at source rate.";
    } else if (cap < sourceRate) {
        d.reason = "no-down";
        d.noteZh = "目标采样率上限 " + std::to_string(cap) + " Hz 低于源采样率 " + std::to_string(sourceRate) + " Hz，投放升频只升不降，按源直供";
        d.noteEn = "Target rate cap " + std::to_string(cap) + " Hz is lower than source rate " + std::to_string(sourceRate) + " Hz; cast upsampling only increases rate, serving at source rate.";
    } else {
        d.reason = "no-same-family";
        const std::string fam = cast_plan_detail::family_label(sourceRate);
        if (!rendererRates.empty()) {
            const std::string ratesStr = cast_plan_detail::join_rates(rendererRates);
            d.noteZh = "渲染器未声明 " + fam + " 族的更高速率（声明：" + ratesStr + " Hz），已保持源直供";
            d.noteEn = "Renderer does not declare higher " + fam + " family rates (declared: " + ratesStr + " Hz); serving at source rate.";
        } else {
            d.noteZh = "渲染器未声明速率且上限 " + std::to_string(cap) + " Hz 非源采样率 " + std::to_string(sourceRate) + " Hz 的整数倍（跨族），已保持源直供";
            d.noteEn = "Renderer rates undeclared and cap " + std::to_string(cap) + " Hz is not an integer multiple of source rate " + std::to_string(sourceRate) + " Hz (cross-family); serving at source rate.";
        }
    }

    return d;
}

inline CastBitDecision decide_cast_served_bits(int sourceBits, int rendererMaxBits) {
    CastBitDecision dec;
    // sourceBits <= 0 → {0, false, "", ""}
    if (sourceBits <= 0) {
        dec.servedBits = 0;
        dec.reduced = false;
        dec.noteZh = "";
        dec.noteEn = "";
        return dec;
    }

    // rendererMaxBits <= 0 或 rendererMaxBits >= sourceBits → {sourceBits, false, "", ""}
    if (rendererMaxBits <= 0 || rendererMaxBits >= sourceBits) {
        dec.servedBits = sourceBits;
        dec.reduced = false;
        dec.noteZh = "";
        dec.noteEn = "";
        return dec;
    }

    // 否则 servedBits = max(16, rendererMaxBits)，reduced = true，
    // note 写明「渲染器最高声明 X bit，输出位深由 Y 降到 Z，已加 TPDF 抖动」。
    dec.servedBits = std::max(16, rendererMaxBits);
    dec.reduced = true;
    dec.noteZh = "渲染器最高声明 " + std::to_string(rendererMaxBits) + " bit，输出位深由 " +
                 std::to_string(sourceBits) + " 降到 " + std::to_string(dec.servedBits) + "，已加 TPDF 抖动";
    dec.noteEn = "Renderer declares maximum " + std::to_string(rendererMaxBits) + " bit; output bit depth reduced from " +
                 std::to_string(sourceBits) + " to " + std::to_string(dec.servedBits) + ", TPDF dither applied.";
    return dec;
}

inline CastBitDecision decide_served_bits(int sourceBits, int rendererMaxBits) {
    return decide_cast_served_bits(sourceBits, rendererMaxBits);
}

} // namespace rta::audio
