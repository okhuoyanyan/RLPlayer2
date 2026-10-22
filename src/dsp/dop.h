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
