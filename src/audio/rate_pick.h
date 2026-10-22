// rate_pick.h — 输出采样率选择（纯逻辑，零设备依赖，可离线单测）
//
// 为什么单独抽出来（2026-10-22 现场事故）：
//   同一端点、同一设置，第一首 44.1k FLAC 按 rate=max 探到 96000 并正常播放；紧接着播第二首，
//   rate=max 的 exclusive_rates() 瞬时返回【空列表】，旧代码是
//       want = rr.empty() ? 0 : rr.back();
//   于是 want=0 在下面被解释成「跟随源率」→ 按 44100 去开独占流 → 端点只支持 48k/96k 族 →
//   在写 PLAY 日志之前就报错退出（日志里连 PLAY 都没有）。
//   这条路径违反了 HF-01 自己定的规矩（rtaweb.cpp 能力检查处注释：rates.empty() 一律当
//   「不支持/端点异常」，绝不静默放行）——「探测为空」不是结论，绝不能被当成「探测结果是源率」。
//   判据必须能被离线测试钉住，所以把它做成纯函数：不 include windows.h、不打开任何设备。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace rta::audio {

// 采样率选择结果
struct RatePick {
    uint32_t    rate = 0;        // 最终要打开的设备采样率（0 = 无法决定，调用方必须报错）
    bool        decided = false; // true = 有确定结论（含「跟随源率」这种正当结论）
    std::string note;            // 人话说明（进 note / ps.info / 信号链 JSON）
    std::string logTag;          // "RATEPROBE_EMPTY" / "RATEPROBE_STALE" / ""，供 play_log 归因
};

namespace ratepick_detail {

// 规范化支持率列表：剔除 0（0 Hz 不是设备能力）、升序、去重。
// 为什么容忍乱序/重复：WASAPI 枚举顺序不作保证，调用方不该被迫先排序。
inline std::vector<uint32_t> normalized(const std::vector<uint32_t>& supported) {
    std::vector<uint32_t> v;
    v.reserve(supported.size());
    for (uint32_t r : supported) if (r > 0) v.push_back(r);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// 手写十进制解析（只为了不引入 <cstdlib>，本头文件保持只依赖 <cstdint>/<string>/<vector>/<algorithm>）
// 返回 0 = 不是合法正整数（含 "abc"、"0"、"-1"、溢出）
inline uint32_t parse_rate(const std::string& s) {
    size_t i = 0;
    if (i < s.size() && s[i] == '+') ++i;
    if (i >= s.size()) return 0;
    uint64_t acc = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        acc = acc * 10u + (uint64_t)(s[i] - '0');
        if (acc > 40000000ull) return 0;      // 40 MHz 以上不可能是采样率（防溢出）
    }
    return (uint32_t)acc;
}

}  // namespace ratepick_detail

// ---------------------------------------------------------------------------
// "follow" = 首次打开时锁定【声卡当前设置的采样率】（用户 2026-10-22 功能1）
//
// 为什么需要这一档：默认 rate=max 会挑设备最高支持率（例如 384k），而声卡在
// Windows 里当前配置的往往是 48k。独占模式按 384k 开流时，某些设备/驱动组合
// 直接不出声（用户原话"这样直接播放通常是不出声的"）。改成"跟随系统"后，
// 第一次就能响。
//
// 与 bit-perfect 的关系（必须讲清楚，不能含糊）：
//   * 跟随系统 = 用 GetMixFormat 报的混音率。它不是"源率"，所以 44.1k 源在
//     48k 混音率的机器上必然要重采样一次 ⟹ 这一档【不是】bit-perfect。
//   * 想要 bit-perfect 仍要选「保持源采样率」(src)；本档只是"首次默认"，
//     不是"唯一选项"，设置里三档都还在。
//   * 独占模式下 deviceRate 由我们决定，系统混音率只是【参考值】：所以这一档
//     的正确语义是"按系统当前配置开流"，而不是"经系统混音"。
// ---------------------------------------------------------------------------
// 选「最终要打开的设备采样率」。语义（逐条，测试逐条打）：
//   1) requested 是具体数值（"96000"）：rate = 该值，decided = true；
//      【不管是否在 supported 里】—— 明确选择要尊重，能力检查在调用方照旧会报错。note 留空。
//   2) requested == "src" 或空：rate = sourceRate，decided = true，note 留空（跟随源是正当选择）。
//   3) requested == "max"：
//        supported 非空 → rate = 最大值，decided = true；
//        supported 为空 → decided = false、logTag = "RATEPROBE_EMPTY"、note 说明「探测为空」，
//        但 rate 仍设为 sourceRate 以便调用方【可以继续尝试】——既给出可继续的率，又用
//        decided=false 明确「这不是探测结论」，调用方要能看到 logTag 去重试/记录/沿用旧结果。
inline RatePick pick_output_rate(const std::string& requested, uint32_t sourceRate,
                                 const std::vector<uint32_t>& supported,
                                 uint32_t systemRate = 0) {
    RatePick out;

    if (requested == "max") {
        const auto v = ratepick_detail::normalized(supported);
        if (!v.empty()) {
            out.rate = v.back();
            out.decided = true;
            return out;
        }
        // 探测为空：绝不当成「探测结果是源率」。给可继续的 rate，但同时把结论标成「未确定」。
        out.rate    = sourceRate;
        out.decided = false;
        out.logTag  = "RATEPROBE_EMPTY";
        out.note    = "设备独占支持率探测为空（端点可能正在重配置）；按源采样率 " +
                      std::to_string(sourceRate) + " Hz 尝试";
        return out;
    }

    if (requested.empty() || requested == "src") {
        out.rate    = sourceRate;      // 跟随源
        out.decided = true;
        return out;                    // note 留空
    }

    // "follow" = 跟随系统（声卡当前配置的采样率），由调用方把系统混音率作为
    // systemRate 传进来。取不到（0）时【不猜】—— 退回"跟随源"并留话，因为
    // 悄悄换成设备最高率正是用户抱怨的那件事。
    if (requested == "follow") {
        if (systemRate > 0) {
            out.rate    = systemRate;
            out.decided = true;
            return out;                // 正常结论，note 留空
        }
        out.rate    = sourceRate;
        out.decided = true;
        out.logTag  = "SYSRATE_EMPTY";
        out.note    = "读不到系统混音采样率（GetMixFormat 失败）；按源采样率 " +
                      std::to_string(sourceRate) + " Hz 尝试";
        return out;
    }

    // 具体数值：明确选择优先，即使不在 supported 里也不改（能力检查会如实报错）
    const uint32_t v = ratepick_detail::parse_rate(requested);
    if (v > 0) {
        out.rate    = v;
        out.decided = true;
        return out;
    }

    // 解析不出合法数值（"abc" / "0" / 负数）：按跟随源处理，但必须留话，不能悄悄改行为
    out.rate    = sourceRate;
    out.decided = true;
    out.note    = "采样率参数 \"" + requested + "\" 无法解析，按源采样率 " +
                  std::to_string(sourceRate) + " Hz";
    return out;
}

// 目标率不被设备接受时「挑一个能用的」。supported 非空（空 → 返回 0 = 无可挑）。
// 优先级：
//   ① 与 want 完全相等；
//   ② 同族且不低于 want —— 44.1k 源不能换到 48k 族（会引入非整数比重采样）。
//      判据：want % 44100 == 0 视为 44.1k 族，在 supported 里找 % 44100 == 0 且 >= want 的最小值；
//            want % 48000 == 0 视为 48k 族，同理 % 48000 == 0；
//            want 本身不在任何族（被人手改成怪值）时，改用 sourceRate 的族。
//   ③ 不小于 want 的最小值（绝不无谓降采样）；
//   ④ 最大值（全低于 want 时，宁可降一点也不能不出声）。
// 注意 ② 里「同族」只解决「换族」问题，不解决「更高」问题：同族里没有 >= want 的，仍落到 ③/④。
inline uint32_t pick_supported_rate(uint32_t want, uint32_t sourceRate,
                                    const std::vector<uint32_t>& supported) {
    const auto v = ratepick_detail::normalized(supported);
    if (v.empty()) return 0;                       // 无可挑：调用方必须报错
    if (want == 0) return v.front();               // 目标未知：取最低可用（不猜源率）

    // ① 完全相等 —— 最理想，直接返回
    if (std::binary_search(v.begin(), v.end(), want)) return want;

    // ② 同族且 >= want
    uint32_t family = 0;
    if (want % 44100u == 0)       family = 44100u;
    else if (want % 48000u == 0)  family = 48000u;
    else if (sourceRate && sourceRate % 44100u == 0) family = 44100u;
    else if (sourceRate && sourceRate % 48000u == 0) family = 48000u;
    if (family) {
        // v 已升序 → 第一个满足条件的即最小
        for (uint32_t r : v) if (r % family == 0 && r >= want) return r;
    }

    // ③ 不小于 want 的最小值
    for (uint32_t r : v) if (r >= want) return r;

    // ④ 都低于 want：取最大
    return v.back();
}

}  // namespace rta::audio