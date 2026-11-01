// dop.h — DoP (DSD over PCM) v1.1 打包 / 解包
//
// 为什么先做 DoP 而不是 ASIO 原生 DSD：
//   DoP 可以复用已经验证过的 WASAPI 独占路径（24-bit 容器 + 交替标记），
//   不需要 ASIO SDK（其授权非自由许可），成本低得多。
//
// 原理（DoP v1.1 开放标准）：
//   每 16 个 DSD 位装进一个 24-bit PCM 样本：高 8 位放标记，低 16 位放 DSD 位
//   标记在 0x05 / 0xFA 之间逐样本交替 —— 接收端靠它识别出这是 DoP 而非普通 PCM
//   于是 DSD64(2.8224 MHz) -> 176400 Hz，DSD128 -> 352800 Hz，DSD256 -> 705600 Hz
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <cstddef>

namespace rta::dsp {

constexpr uint8_t DOP_MARKER_A = 0x05;
constexpr uint8_t DOP_MARKER_B = 0xFA;

// DSD 位数 -> DSD 采样率（1 个 DSD 位 = 1 个样本）
// baseRate 决定 44.1k 家族还是 48k 家族（48k 源的 DSD 是 2.304 MHz 的整数倍）
inline uint32_t dsd_rate(int dsdMultiple, uint32_t baseRate = 44100u) {
    return baseRate * (uint32_t)dsdMultiple;
}
// DSD 采样率 -> DoP 承载的 PCM 采样率
inline uint32_t dop_carrier_rate(int dsdMultiple, uint32_t baseRate = 44100u) {
    return dsd_rate(dsdMultiple, baseRate) / 16u;
}

// 分块打包时必须跨块保持"标记相位"，否则接收端会在块边界看到 A,A 而判定非法。
struct DopPackState {
    std::vector<char> phaseA;      // 每声道一个：true 表示本帧该用 0x05
};

// 流式版本：相位跨调用保持。frames 必须是 16 的整数倍，否则尾部会被丢掉。
inline void dop_pack(const double* const* sdm, int channels, size_t frames,
                     std::vector<int32_t>& out, DopPackState& st) {
    if ((int)st.phaseA.size() != channels) st.phaseA.assign((size_t)channels, 1);
    const size_t carrierFrames = frames / 16;
    out.assign(carrierFrames * (size_t)channels, 0);
    // 各声道独立维护标记相位（DoP 要求每个声道都交替）
    for (int c = 0; c < channels; ++c) {
        bool phaseA = st.phaseA[(size_t)c] != 0;
        for (size_t n = 0; n < carrierFrames; ++n) {
            uint32_t bits16 = 0;
            for (int b = 0; b < 16; ++b) {
                const size_t idx = n * 16 + (size_t)b;
                if (idx < frames && sdm[c][idx] > 0.0) bits16 |= (1u << (15 - b));
            }
            const uint32_t marker = (phaseA ? DOP_MARKER_A : DOP_MARKER_B) << 16;
            int32_t v = (int32_t)(marker | bits16);
            if (v & 0x800000) v |= (int32_t)0xFF000000;      // 24-bit 符号扩展
            out[n * (size_t)channels + (size_t)c] = v;
            phaseA = !phaseA;
        }
        st.phaseA[(size_t)c] = phaseA ? 1 : 0;
    }
}

// 一次性版本（相位从 A 开始），给离线转换用
inline void dop_pack(const double* const* sdm, int channels, size_t frames,
                     std::vector<int32_t>& out) {
    DopPackState st;
    dop_pack(sdm, channels, frames, out, st);
}

// ===========================================================================
// ★ 2026-10-29（两轮审查 F03）：1-bit 输出契约检查。
//
// 合法的 1-bit 调制器输出只有 +1.0 / −1.0 两个取值。任何其它值都说明上游把**多电平**
// 流送到了 1-bit 打包口。旧 dop_pack() 只做「> 0.0」判断 ⟹ 256 电平的 AHM 流被按符号
// 折成 1-bit：恒定输入 0.2 的平均值本来是 0.2，打包再解包后变成 1.0（直流 +1），
// 而 DoP marker 依然完全合法 —— 从输出侧根本发现不了。
//
// 因此：生产调用一律走 dop_pack_checked()，非二值输入必须【明确失败】，
// 由调用方把原因告诉用户（而不是"听起来像坏了"）。
// ===========================================================================
inline bool dop_check_binary(const double* const* sdm, int channels, size_t frames,
                             size_t* badIndex = nullptr, double* badValue = nullptr) {
    if (!sdm || channels <= 0) return true;
    for (int c = 0; c < channels; ++c) {
        const double* p = sdm[(size_t)c];
        if (!p) continue;
        for (size_t i = 0; i < frames; ++i) {
            const double v = p[i];
            if (v == 1.0 || v == -1.0) continue;
            if (badIndex) *badIndex = i;
            if (badValue) *badValue = v;
            return false;
        }
    }
    return true;
}

// 带契约检查的打包。返回 false = 输入不是 1-bit（多电平流混入），out 不被修改。
inline bool dop_pack_checked(const double* const* sdm, int channels, size_t frames,
                             std::vector<int32_t>& out, DopPackState& st,
                             std::string* why = nullptr) {
    size_t badI = 0;
    double badV = 0.0;
    if (!dop_check_binary(sdm, channels, frames, &badI, &badV)) {
        if (why) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "DoP 打包被拒绝：输入不是 1-bit 流（第 %llu 个样本 = %.9f，只有 +1/-1 合法）。"
                "这通常意味着多电平调制器（AHM 等）的输出被送进了 1-bit 通路。",
                (unsigned long long)badI, badV);
            *why = buf;
        }
        return false;
    }
    dop_pack(sdm, channels, frames, out, st);
    return true;
}

// 解包：校验标记并还原 1-bit 流。返回 false 表示标记序列不合法（不是有效 DoP）
inline bool dop_unpack(const int32_t* in, int channels, size_t carrierFrames,
                       std::vector<std::vector<double>>& out) {
    out.assign((size_t)channels, std::vector<double>(carrierFrames * 16, -1.0));
    for (int c = 0; c < channels; ++c) {
        bool phaseA = true;
        for (size_t n = 0; n < carrierFrames; ++n) {
            const uint32_t v = (uint32_t)in[n * (size_t)channels + (size_t)c] & 0xFFFFFFu;
            const uint32_t marker = (v >> 16) & 0xFFu;
            const uint32_t want = phaseA ? DOP_MARKER_A : DOP_MARKER_B;
            if (marker != want) return false;
            const uint32_t bits16 = v & 0xFFFFu;
            for (int b = 0; b < 16; ++b) {
                out[(size_t)c][n * 16 + (size_t)b] =
                    ((bits16 >> (15 - b)) & 1u) ? 1.0 : -1.0;
            }
            phaseA = !phaseA;
        }
    }
    return true;
}

} // namespace rta::dsp
