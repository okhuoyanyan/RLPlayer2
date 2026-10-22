// direct_sdm.h — DirectSDM 直通（ROADMAP P0-3）：源 DSD 速率 == 目标速率时跳过调制
//
// 为什么值得单独做：现在 DSD 源一律「ffmpeg 解成 PCM → 升频 → 调制回 1-bit」，
// 即使源和目标都是 DSD64 也会多走两遍量化。DirectSDM 让 1-bit 流直接进 DoP 容器，
// 输出解包后与源逐位相同 —— 这是可判定的，不是「听起来一样」。
#pragma once
#include "audio/dsd_reader.h"
#include "dsp/dop.h"
#include <string>
#include <vector>

namespace rta::dsp {

struct DirectSdmDecision {
    bool enabled = false;
    std::string reason;
};

// 直通条件（与 HQPlayer 的 DirectSDM 语义一致）：
//   源 DSD 速率 == 目标 DSD 速率，且 PCM 域没有任何处理（EQ/卷积/重采样），音量 = 0 dB。
// 任何一项不满足都要老实走调制器，并在 UI 里说明原因。
inline DirectSdmDecision direct_sdm_decision(uint32_t srcRate, uint32_t targetRate,
                                             bool anyPcmProcessing, bool volumeActive) {
    DirectSdmDecision d;
    if (srcRate == 0 || targetRate == 0) { d.reason = "缺少源或目标 DSD 速率"; return d; }
    if (srcRate != targetRate) {
        d.reason = "源 DSD " + std::to_string(srcRate) + " Hz ≠ 目标 " +
                   std::to_string(targetRate) + " Hz：必须经调制器重采样";
        return d;
    }
    if (anyPcmProcessing) { d.reason = "存在 PCM 域处理（EQ / 卷积 / 重采样），不能直通"; return d; }
    if (volumeActive) { d.reason = "音量不是 0 dB（或渐变未结束），直通会失去逐位一致性"; return d; }
    d.enabled = true;
    d.reason = "源与目标同为 " + std::to_string(srcRate) + " Hz 且无 PCM 处理：跳过调制";
    return d;
}

// 1-bit 流 → DoP 24-bit 载波（每 16 位一个样本）。
// 内部表示是 MSB-first，第 n 组 16 位正好是两个相邻字节 (b0<<8)|b1，
// 与 dop_pack() 的「b=0 放 bit15」约定一致，因此不需要先转成 double 数组。
// 返回写出的载波帧数（每声道）。不足 16 位的尾巴被丢弃（DoP 帧必须整 16 位）。
inline size_t dsd_to_dop(const rta::audio::DsdStream& s, std::vector<int32_t>& out,
                         DopPackState& st) {
    const size_t ch = (size_t)s.channels;
    const size_t groups = (size_t)(s.bits / 16);
    if ((int)st.phaseA.size() != (int)ch) st.phaseA.assign(ch, 1);
    out.assign(groups * ch, 0);
    for (size_t c = 0; c < ch; ++c) {
        const std::vector<uint8_t>& buf = s.data[c];
        bool phaseA = st.phaseA[c] != 0;
        for (size_t n = 0; n < groups; ++n) {
            const size_t b = n * 2;
            const uint32_t bits16 = ((uint32_t)buf[b] << 8) | (uint32_t)buf[b + 1];
            const uint32_t marker = (uint32_t)(phaseA ? DOP_MARKER_A : DOP_MARKER_B) << 16;
            int32_t v = (int32_t)(marker | bits16);
            if (v & 0x800000) v |= (int32_t)0xFF000000;      // 24-bit 符号扩展
            out[n * ch + c] = v;
            phaseA = !phaseA;
        }
        st.phaseA[c] = phaseA ? 1 : 0;
    }
    return groups;
}

// DoP 载波 → 1-bit 流（位级还原，不经过 double）。
// 返回 false 表示标记序列不合法（不是有效 DoP）。
inline bool dop_to_dsd_bits(const int32_t* in, size_t carrierFrames, int channels,
                            rta::audio::DsdStream& out) {
    if (channels < 1) return false;
    out.channels = channels;
    out.bits = (uint64_t)carrierFrames * 16;
    out.data.assign((size_t)channels, std::vector<uint8_t>(carrierFrames * 2, 0));
    for (int c = 0; c < channels; ++c) {
        bool phaseA = true;
        for (size_t n = 0; n < carrierFrames; ++n) {
            const uint32_t v = (uint32_t)in[n * (size_t)channels + (size_t)c] & 0xFFFFFFu;
            const uint32_t marker = (v >> 16) & 0xFFu;
            const uint32_t want = phaseA ? DOP_MARKER_A : DOP_MARKER_B;
            if (marker != want) return false;
            const uint32_t bits16 = v & 0xFFFFu;
            out.data[(size_t)c][n * 2 + 0] = (uint8_t)(bits16 >> 8);
            out.data[(size_t)c][n * 2 + 1] = (uint8_t)(bits16 & 0xFFu);
            phaseA = !phaseA;
        }
    }
    return true;
}

} // namespace rta::dsp
