// wasapi_shared.h — WASAPI 共享模式输出后端（2026-10-22 VM 事故热修 / 2026-10-24 多格式）
//
// 背景：独占模式在 VM 板载 HD Audio 上 GetCurrentPadding 永远等于 bufferFrames
//（write_available 恒 0），200 次超时后渲染循环静默 break，END pos=0 error=""，
// 真机（TASCAM）正常、VM 全灭。共享模式经系统混音引擎，不需要独占接管，
// 是虚拟机 / 远程桌面 / 独占不可用场景的兜底路径（代价：非 bit-perfect）。
//
// ★ 2026-10-24 多格式支持（本版核心改动）
//   旧实现假定「混音引擎恒为 float32」：GetMixFormat 不是 float32 就直接报
//   「系统混音格式不是 float32，共享模式暂不支持；请使用 WASAPI 独占。」
//   （tools/rtaweb.cpp:1908 的门，判据来自本文件旧版的 describe_mix）。
//   但共享混音格式是**端点驱动 + 用户配置**决定的，实测存在 16/24/32-bit 整型：
//   蓝牙 HFP 端点、部分 USB DAC 驱动、老声卡控制面板里把默认格式设成 16-bit。
//   ⇒ 本版读 GetMixFormat 后**按它的格式写样本**，覆盖：
//        float32（Windows 默认）、int16、int24（24-in-32，低 8 位补零）、int32，
//        另外兼容「紧凑 24-bit（3 字节容器）」这类老驱动形态。
//   映射抽成纯函数 pick_shared_writer(WAVEFORMATEX) → SharedSampleFmt；
//   单样本字节写出抽成 put_shared_sample(fmt, double, bytes)；
//   两者都在 tests/test_wasapi_shared.cpp 离线断言（不开设备、不发声，铁律 1 无涉）。
//
// 与 WasapiOutput（独占）同接口风格，差异点：
//   * Initialize(AUDCLNT_SHAREMODE_SHARED, 0 flags, 200ms, periodicity=0, GetMixFormat)
//     —— 共享模式 periodicity 必须 0；格式由混音引擎裁决，请求的 rate/channels/bits
//        只是"期望"，实际以混音格式为准并回报给调用方（mix_rate() / mix_channels() /
//        mix_format() / mix_bits()），上层据此定 deviceRate 与管线声道。
//   * 写入：管线 int32（按 reqBits 右对齐，见 s32_to_f32_bits）→ 目标容器字节。
//   * 事件驱动不用于共享：wait_event 直接 Sleep —— 本类按"补满到 bufferFrames−pad、
//     20ms 轮询"的节奏写入（与独占版渲染循环同一骨架）。
//   * 浮点换算 / 格式选择 / 单样本装箱 / 可写帧数计算全部抽成纯函数，离线可测。
//
// ⚠️ 未验证项（铁律 4，必须保留）
//   1) int16 / int24(24-in-32) / int32 / 紧凑 24 四条写入分支**只有离线单测覆盖，
//      没有在真实整型混音端点上跑过**：本机 2026-10-24 只读探测了全部 6 个 ACTIVE 端点，
//      混音格式**全是 float32**（N300 96000/2ch/32f、Audiobyte USB 48000/2ch/32f、
//      VB-Matrix VAIO 48000/6ch/32f、AMD HD Audio 48000/2ch/32f ×2、TASCAM 48000/2ch/32f），
//      没有整型端点可测、也不去改用户端点的默认格式。当前回归基线 =
//      pick_shared_writer + put_shared_sample + shared_pack_int32 的离线单测
//      （生产 pack() 直接调用后两者，不是测试另抄一份）+ 整型分支的代码审查。
//   2) 直通监听（write_available_f32）在整型混音格式下会正确换算，但 tools/rtaweb.cpp:2758
//      的调用点用 is_float() 把关（非 float 即不监听）。is_float() 保持诚实语义
//      （混音格式真的是 float32），因此**整型混音端点下直通监听不工作**（不是写坏数据）。
//      修这个需要改 tools/rtaweb.cpp，不在本任务可改范围。
//   3) 「共享 = 非 bit-perfect」的提示仍由调用方负责（tools/rtaweb.cpp:1913/2461 的
//      appendNote / FALLBACK_SHARED，本次未改）。本文件没有打印通道，无法自行提示 —— 见
//      mix_valid_bits()：管线位深高于混音位深时（如 24-bit 管线 → int16 混音）实际分辨率
//      被降到混音位深，调用方应据此提示。**音质折中（如实记录）**：调用方的抖动/整形是按
//      请求位深（如 24-bit LNS15）做的，最后一级到混音位深（如 16-bit）的量化由本文件做
//      四舍五入、没有专门抖动 —— 约 −96 dBFS 量级、无噪声整形的量化误差。要彻底消除需
//      把 cfg.outBits 设成混音位深（改 tools/rtaweb.cpp），不在本任务可改范围。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "wasapi_out.h"   // 复用 COM 初始化模式 / hr_name / 端点定位

namespace rta::audio {

// ---- 纯函数（离线测试边界值，不碰设备）----

// int32 → float32 换算：满幅 2^31 映射到 ±1.0。
// INT32_MIN → 恰好 −1.0；INT32_MAX → 0.99999999953（共享引擎的 float 域本就如此）。
inline double s32_to_f32(int32_t v) {
    return (double)v / 2147483648.0;
}

// ★ 按【源位深】归一化（2026-10-23 现场修复）。
// 管线的 int32 是按 bits 右对齐的（AudioBuffer::fullScale = 2^(bits-1)，见 src/audio/wav.h:25），
// 而共享混音引擎要吃 ±1.0 的浮点。旧实现一律 s32_to_f32（÷2^31），于是
//   24-bit 源 → 1/256 = **−48.16 dB**，16-bit 源 → 1/65536 = −96.3 dB。
// 症状是"确实没断，但电平极低"——而内部所有判据（peak/clip/isp 都按位深归一化）
// 全部显示正常，唯有真正送进混音引擎那一步少了位深换算（铁律 3 的教科书案例）。
// 32-bit 与未知位深保持原路径；结果 clamp 到 ±1.0，防上游 overshoot 溢出。
inline double s32_to_f32_bits(int32_t v, int bits) {
    double x;
    if (bits <= 0 || bits >= 32) x = (double)v / 2147483648.0;
    else                        x = (double)v / (double)(1LL << (bits - 1));
    if (x >  1.0) return  1.0;
    if (x < -1.0) return -1.0;
    return x;
}

// 可写帧数 = 缓冲深度 − 当前积压，再夹到可用数据量之内。
// 抽出来是为了离线验证钳制逻辑（pad 超过 bufferFrames 时不得下溢）。
inline size_t shared_writable(UINT32 bufferFrames, UINT32 pad, size_t availFrames) {
    if (pad > bufferFrames) pad = bufferFrames;              // 驱动异常时防御，别让 size_t 下溢
    size_t toWrite = (size_t)bufferFrames - (size_t)pad;
    if (toWrite > availFrames) toWrite = availFrames;
    return toWrite;
}

// =====================================================================================
// 混音格式识别与样本装箱（2026-10-24：共享模式不再只支持 float32）
// =====================================================================================

// 混音格式 → 写入路径。顺序即"支持的格式清单"。
enum class SharedSampleFmt : int {
    Unsupported = 0,   // 8-bit / µ-law / A-law / 64-bit float / 未知 SubFormat …
    Float32     = 1,   // IEEE float 32（Windows 默认混音格式）
    Int16       = 2,   // PCM 16（蓝牙 HFP、老声卡"16 位"配置）
    Int24In32   = 3,   // PCM 32 位容器 + 24 位有效（低 8 位必须补零）
    Int32       = 4,   // PCM 32
    Int24       = 5,   // PCM 紧凑 24（每样本 3 字节，blockAlign = ch×3）
};

inline const char* shared_fmt_name(SharedSampleFmt f) {
    switch (f) {
        case SharedSampleFmt::Float32:   return "float32";
        case SharedSampleFmt::Int16:     return "int16";
        case SharedSampleFmt::Int24In32: return "int24(24-in-32)";
        case SharedSampleFmt::Int32:     return "int32";
        case SharedSampleFmt::Int24:     return "int24(compact-3)";
        default:                         return "unsupported";
    }
}

// 每样本容器字节数（决定交错步长；0 = 不支持）
inline int shared_fmt_bytes(SharedSampleFmt f) {
    switch (f) {
        case SharedSampleFmt::Int16:     return 2;
        case SharedSampleFmt::Int24:     return 3;
        case SharedSampleFmt::Float32:
        case SharedSampleFmt::Int24In32:
        case SharedSampleFmt::Int32:     return 4;
        default:                         return 0;
    }
}

// 每个样本的【有效位深】（= 管线真正能表达的分辨率；float32 记 32 位有效）
inline int shared_fmt_valid_bits(SharedSampleFmt f) {
    switch (f) {
        case SharedSampleFmt::Int16:     return 16;
        case SharedSampleFmt::Int24:     return 24;
        case SharedSampleFmt::Int24In32: return 24;
        case SharedSampleFmt::Float32:
        case SharedSampleFmt::Int32:     return 32;
        default:                         return 0;
    }
}

// EXTENSIBLE 的 SubFormat GUID（本地定义，不依赖 mmreg.h 的 extern 常量 / uuid 库）
inline const GUID& shared_subfmt_pcm() {
    static const GUID g = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
    return g;
}
inline const GUID& shared_subfmt_float() {
    static const GUID g = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
    return g;
}
inline bool shared_guid_equal(const GUID& a, const GUID& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::memcmp(a.Data4, b.Data4, 8) == 0;
}

// 每样本【容器】字节数。
// 优先用 nBlockAlign/nChannels —— 缓冲区的交错步长实际由 blockAlign 决定，
// 位深字段（wBitsPerSample）在某些驱动上与容器宽度不一致（24-bit 值放 32 位容器）。
inline int wfx_container_bytes(const WAVEFORMATEX* w) {
    if (!w || w->nChannels == 0) return 0;
    const int ch = (int)w->nChannels;
    if (w->nBlockAlign > 0 && (int)w->nBlockAlign % ch == 0) {
        const int b = (int)(w->nBlockAlign / ch);
        if (b >= 1 && b <= 4) return b;
    }
    const int b = (int)w->wBitsPerSample / 8;
    return (b >= 1 && b <= 4) ? b : 0;
}

// 每样本【有效位深】。EXTENSIBLE 取 wValidBitsPerSample（0 表示"等于容器位深"）。
inline int wfx_valid_bits(const WAVEFORMATEX* w) {
    if (!w) return 0;
    int valid = (int)w->wBitsPerSample;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const auto* e = (const WAVEFORMATEXTENSIBLE*)w;
        const int v = (int)e->Samples.wValidBitsPerSample;
        if (v > 0) valid = v;
    }
    if (valid <= 0) valid = (int)w->wBitsPerSample;
    return valid;
}

// ★ 核心映射（纯函数）：mix format → 写入路径。
// 判据顺序：容器字节数（blockAlign/ch）+ 有效位深 + SubFormat。
//   float32     ：tag=IEEE_FLOAT（或 EXTENSIBLE+SubFormat=float）且容器 4 字节 / 32 位
//   int16       ：PCM 容器 2 字节
//   int24       ：PCM 容器 3 字节（紧凑）
//   int24-in-32 ：PCM 容器 4 字节且有效位 ≤ 24（低 8 位补零）
//   int32       ：PCM 容器 4 字节且有效位 25..32
inline SharedSampleFmt pick_shared_writer(const WAVEFORMATEX* w) {
    if (!w) return SharedSampleFmt::Unsupported;
    const int bytes = wfx_container_bytes(w);
    const int valid = wfx_valid_bits(w);
    if (bytes <= 0 || valid <= 0) return SharedSampleFmt::Unsupported;

    bool isFloat = false, isPcm = false;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (w->wFormatTag == WAVE_FORMAT_PCM) {
        isPcm = true;
    } else if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const auto* e = (const WAVEFORMATEXTENSIBLE*)w;
        if (shared_guid_equal(e->SubFormat, shared_subfmt_float()))      isFloat = true;
        else if (shared_guid_equal(e->SubFormat, shared_subfmt_pcm()))   isPcm = true;
        else return SharedSampleFmt::Unsupported;   // 未知 SubFormat（Dolby/DTS…）不猜
    } else {
        return SharedSampleFmt::Unsupported;        // µ-law / A-law / ADPCM 等
    }
    if (isFloat) {
        return (bytes == 4 && w->wBitsPerSample == 32) ? SharedSampleFmt::Float32
                                                       : SharedSampleFmt::Unsupported;
    }
    if (!isPcm) return SharedSampleFmt::Unsupported;
    if (bytes == 2) return SharedSampleFmt::Int16;
    if (bytes == 3) return SharedSampleFmt::Int24;
    if (bytes == 4) return (valid <= 24) ? SharedSampleFmt::Int24In32 : SharedSampleFmt::Int32;
    return SharedSampleFmt::Unsupported;
}

// 一行文字描述 WAVEFORMATEX（错误信息/日志用；不含 SubFormat 以免刷屏）
inline std::string wfx_describe(const WAVEFORMATEX* w) {
    if (!w) return "(null)";
    char b[192];
    std::snprintf(b, sizeof(b), "tag=0x%04X bits=%u valid=%d blockAlign=%u ch=%u rate=%u",
                  (unsigned)w->wFormatTag, (unsigned)w->wBitsPerSample, wfx_valid_bits(w),
                  (unsigned)w->nBlockAlign, (unsigned)w->nChannels, (unsigned)w->nSamplesPerSec);
    return std::string(b);
}

// ---- 归一化浮点 ±1.0 → 有符号整数（四舍五入、饱和，绝不溢出）----
// 输入先钳到 [−1, 1]，量化后再夹到 [−2^(bits−1), 2^(bits−1)−1]。
// bits=32 时 +1.0 → 2147483647、−1.0 → −2147483648（容器内可表示值，转换安全）。
// NaN/Inf → 0（上游 bug 不该变成满幅直流或爆音）。
inline int32_t f64_to_signed_bits(double x, int bits) {
    if (bits <= 0 || bits > 32) return 0;
    if (!std::isfinite(x)) return 0;
    if (x >  1.0) x =  1.0;
    if (x < -1.0) x = -1.0;
    const double lim = std::ldexp(1.0, bits - 1);                 // 2^(bits−1)
    double v = x * lim;
    v = (v >= 0.0) ? std::floor(v + 0.5) : std::ceil(v - 0.5);    // 四舍五入（远离零）
    if (v >  lim - 1.0) v =  lim - 1.0;
    if (v < -lim)       v = -lim;
    return (int32_t)v;                                            // 已保证在 int32 值域内
}

// 0.5 → 16384；+满幅 → 32767；−满幅 → −32768（饱和，不溢出）
inline int16_t f64_to_s16(double x) { return (int16_t)f64_to_signed_bits(x, 16); }
// 0.5 → 0x400000；+满幅 → 0x7FFFFF；−满幅 → −0x800000
inline int32_t f64_to_s24(double x) { return f64_to_signed_bits(x, 24); }
// 24-in-32：24 位有效值**左移 8 位**放进 32 位容器（低 8 位恒 0）。
// 0.5 → 0x40000000 (1073741824)；+满幅 → 0x7FFFFF00；−满幅 → 0x80000000 (INT32_MIN)
inline int32_t f64_to_s24_in_32(double x) {
    return (int32_t)((int64_t)f64_to_s24(x) * 256);   // 用乘法代替左移：负值左移在 C++17 是 UB
}
// 0.5 → 1073741824；+满幅 → 2147483647；−满幅 → −2147483648
inline int32_t f64_to_s32(double x) { return f64_to_signed_bits(x, 32); }

// 浮点 → float32（钳到 ±1.0；非有限值 → 0）
inline float f64_to_f32_clamped(double x) {
    if (!std::isfinite(x)) return 0.0f;
    if (x >  1.0) x =  1.0;
    if (x < -1.0) x = -1.0;
    return (float)x;
}

// 小端写出 n 字节（PCM 容器一律小端，WASAPI 共享模式不认 big-endian）
inline void store_le(unsigned char* dst, int32_t v, int bytes) {
    const uint32_t u = (uint32_t)v;
    for (int i = 0; i < bytes; ++i) dst[i] = (unsigned char)((u >> (8 * i)) & 0xFFu);
}

// ★ 单样本写出（纯函数，生产的整型写入路径逐样本调用它 —— 测的就是跑的那份）。
// dst 至少 shared_fmt_bytes(fmt) 字节。fmt=Unsupported 时不写（open 已拒绝该情形）。
inline void put_shared_sample(SharedSampleFmt fmt, double x, unsigned char* dst) {
    switch (fmt) {
        case SharedSampleFmt::Float32: {
            const float f = f64_to_f32_clamped(x);
            std::memcpy(dst, &f, sizeof(f));
            return;
        }
        case SharedSampleFmt::Int16:     store_le(dst, (int32_t)f64_to_s16(x),     2); return;
        case SharedSampleFmt::Int24:     store_le(dst, f64_to_s24(x),              3); return;
        case SharedSampleFmt::Int24In32: store_le(dst, f64_to_s24_in_32(x),        4); return;
        case SharedSampleFmt::Int32:     store_le(dst, f64_to_s32(x),              4); return;
        default: return;
    }
}

// 管线 int32（按 srcBits 右对齐）→ 混音格式字节。生产 pack() 与此同一定标器。
inline void put_shared_sample_from_s32(SharedSampleFmt fmt, int32_t v, int srcBits,
                                       unsigned char* dst) {
    put_shared_sample(fmt, s32_to_f32_bits(v, srcBits), dst);
}

// ★ 整块交错装箱（纯函数；生产的整型写入路径**就是**它，不是"另抄一份"）。
// 输入：交错 int32，frames×(mixCh) 个样本，按 srcBits 右对齐；输出：交错目标容器字节。
// 同时返回更新后的峰值（归一化域，post-gain、pre-quantize，与旧 pack() 口径一致）。
// 抽成自由函数的原因（铁律 3）：交错步长/声道交织只能在"多帧多声道"上验，
// 单样本函数验不到；而这条路径在真机整型混音端点上没法跑（本机只有 float32），
// 只有离线覆盖这一条路。
inline double shared_pack_int32(SharedSampleFmt fmt, int mixCh, int srcBits, double gain,
                                unsigned char* dst, const int32_t* src, size_t frames,
                                double peak0) {
    const int bytes = shared_fmt_bytes(fmt);
    if (bytes <= 0 || mixCh <= 0 || !dst || !src) return peak0;
    const size_t n = frames * (size_t)mixCh;
    double pk = peak0;
    for (size_t i = 0; i < n; ++i) {
        const double v = s32_to_f32_bits(src[i], srcBits) * gain;
        if (std::fabs(v) > pk) pk = std::fabs(v);
        put_shared_sample(fmt, v, dst + i * (size_t)bytes);
    }
    return pk;
}

// 同上，但输入是交错 float32（采集直通监听用的那条路）。不跟踪峰值（旧行为不变）。
inline void shared_pack_f32(SharedSampleFmt fmt, int mixCh, double gain,
                            unsigned char* dst, const float* src, size_t frames) {
    const int bytes = shared_fmt_bytes(fmt);
    if (bytes <= 0 || mixCh <= 0 || !dst || !src) return;
    const size_t n = frames * (size_t)mixCh;
    for (size_t i = 0; i < n; ++i) {
        put_shared_sample(fmt, (double)src[i] * gain, dst + i * (size_t)bytes);
    }
}

class WasapiSharedOutput {
public:
    WasapiSharedOutput() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInit_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        if (!comInit_) throw std::runtime_error("CoInitializeEx 失败");
    }
    ~WasapiSharedOutput() { close(); if (comInit_) CoUninitialize(); }
    WasapiSharedOutput(const WasapiSharedOutput&) = delete;
    WasapiSharedOutput& operator=(const WasapiSharedOutput&) = delete;

    // 只读探测系统混音格式（不开流、不出声）。调用方在开管线之前用它定 deviceRate，
    // 避免 DSP 升到用户率之后又被系统引擎重采样一次。
    //
    // ★★ isFloat 出参的语义（2026-10-24 多格式扩展，改之前务必读完）
    //   tools/rtaweb.cpp:1906 的 if (!mixFloat) 是「共享后端能不能开」的**唯一总门**，
    //   而它把判据压成了一个 bool。本任务不允许改 tools/rtaweb.cpp，所以这个出参必须
    //   表示「混音格式是否**受支持/可写**」，不能是「格式是不是 float32」——否则
    //   int16/int24/int32 混音系统仍然会被拒在那道门外（正是本次要修的 bug）。
    //   ⇒ isFloat = (pick_shared_writer(wfx) != Unsupported)：
    //        float32 / int16 / int24-in-32 / int32 / 紧凑 24 → true
    //        8-bit / µ-law / 64-bit float / 未知 SubFormat   → 查询返回 false + 可读原因
    //   想知道**真实**格式：用 outFmt 出参（可选，默认 nullptr，旧调用不变），
    //   或构造对象后读 mix_format()；类上的 is_float() 仍是诚实的「混音格式 == float32」。
    static bool query_mix_format(const std::string& deviceId, uint32_t& rate, int& channels,
                                 bool& isFloat, std::string& err,
                                 SharedSampleFmt* outFmt = nullptr) {
        rate = 0; channels = 0; isFloat = false;
        if (outFmt) *outFmt = SharedSampleFmt::Unsupported;
        const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool own = SUCCEEDED(ci);   // RPC_E_CHANGED_MODE = 线程已是另一种套间，照样能用
        if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX", (unsigned long)(unsigned)ci);
            err = std::string("CoInitializeEx 失败: ") + hb; return false;
        }
        IMMDeviceEnumerator* en = nullptr;
        HRESULT hrC = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), (void**)&en);
        if (FAILED(hrC) || !en) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX", (unsigned long)(unsigned)hrC);
            err = std::string("枚举器创建失败: ") + hb;
            if (own) CoUninitialize();
            return false;
        }
        IMMDevice* dev = nullptr;
        HRESULT hr;
        if (deviceId.empty()) {
            hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        } else {
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
            std::wstring wid((size_t)wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, wid.data(), wlen);
            hr = en->GetDevice(wid.c_str(), &dev);
        }
        en->Release();
        if (FAILED(hr) || !dev) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s(GetDevice/GetDefaultAudioEndpoint)",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            err = "打开端点失败: "; err += hb;
            if (own) CoUninitialize();
            return false;
        }
        IAudioClient* ac = nullptr;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        dev->Release();
        if (FAILED(hr) || !ac) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s(Activate)",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            err = "Activate(IAudioClient) 失败: "; err += hb;
            if (own) CoUninitialize();
            return false;
        }
        WAVEFORMATEX* wfx = nullptr;
        hr = ac->GetMixFormat(&wfx);
        if (FAILED(hr) || !wfx) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s(GetMixFormat)",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            err = "GetMixFormat 失败: "; err += hb;
            ac->Release();
            if (own) CoUninitialize();
            return false;
        }
        const SharedSampleFmt f = pick_shared_writer(wfx);
        rate = wfx->nSamplesPerSec;
        channels = (int)wfx->nChannels;
        isFloat = (f != SharedSampleFmt::Unsupported);   // 见上方注释：语义 = 受支持/可写
        if (outFmt) *outFmt = f;
        std::string desc = wfx_describe(wfx);
        CoTaskMemFree(wfx);
        ac->Release();
        if (own) CoUninitialize();
        if (f == SharedSampleFmt::Unsupported) {
            err = "系统混音格式不受支持（" + desc +
                  "）：共享模式支持 float32 / int16 / int24(24-in-32) / int32 / 紧凑 24；"
                  "请改用 WASAPI 独占，或把该端点默认格式改为 32-bit float";
            return false;
        }
        return true;
    }

    // rate/channels/bits 是"期望值"；实际开流格式 = GetMixFormat（必须原样交给 Initialize）。
    // 若与期望不同（通常 48k/float32，也可能是 int16/int24/int32），调用方必须改用
    // mix_rate()/mix_channels() 重排管线，open 本身不因此失败 —— 差异通过
    // query_mix_format / 访问器回报。
    bool open(const std::string& deviceId, uint32_t rate, int channels, int bits, double bufferMs = 200.0) {
        close();
        reqRate_ = rate; reqCh_ = channels; reqBits_ = bits;   // reqBits_ 是【管线位深】，非混音位深
        lastErr_.clear();

        IMMDeviceEnumerator* en = nullptr;
        const HRESULT hrCo = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                              __uuidof(IMMDeviceEnumerator), (void**)&en);
        if (FAILED(hrCo) || !en) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "枚举器创建失败: 0x%08lX",
                                       (unsigned long)(unsigned)hrCo);
            lastErr_ = hb; return false;
        }
        IMMDevice* dev = nullptr;
        HRESULT hr;
        if (deviceId.empty()) {
            hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        } else {
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
            std::wstring wid((size_t)wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, wid.data(), wlen);
            hr = en->GetDevice(wid.c_str(), &dev);
        }
        en->Release();
        if (FAILED(hr) || !dev) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s(GetDevice/GetDefaultAudioEndpoint)",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = std::string("打开端点失败: ") + hb; return false;
        }
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_);
        dev->Release();
        if (FAILED(hr) || !ac_) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s(Activate)",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = std::string("Activate(IAudioClient) 失败: ") + hb; return false;
        }

        WAVEFORMATEX* mix = nullptr;
        hr = ac_->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s(GetMixFormat)",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = std::string("GetMixFormat 失败: ") + hb;
            close(); return false;
        }
        // ★ 2026-10-24：不再要求 float32 —— 按混音格式选写入路径（int16/int24/int32 都写）。
        const SharedSampleFmt fmt = pick_shared_writer(mix);
        if (fmt == SharedSampleFmt::Unsupported) {
            lastErr_ = "系统混音格式不受支持（" + wfx_describe(mix) +
                       "）：共享模式支持 float32 / int16 / int24(24-in-32) / int32 / 紧凑 24；"
                       "请改用 WASAPI 独占，或把该端点默认格式改为 32-bit float";
            CoTaskMemFree(mix);
            close(); return false;
        }
        mixFmt_   = fmt;
        mixBytes_ = shared_fmt_bytes(fmt);
        mixValid_ = shared_fmt_valid_bits(fmt);
        mixRate_  = mix->nSamplesPerSec;
        mixCh_    = (int)mix->nChannels;
        mixFloat_ = (fmt == SharedSampleFmt::Float32);
        deviceId_ = deviceId;

        // 共享模式：flags=0、periodicity 必须 0（与独占"配方 1"相反）、缓冲给足 200ms。
        // 第三个格式参数必须是 GetMixFormat 原样指针 —— 共享模式不接受自造格式。
        REFERENCE_TIME dur = (REFERENCE_TIME)(bufferMs * 10000.0);
        hr = ac_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, dur, 0, mix, nullptr);
        if (FAILED(hr)) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = std::string("Initialize 失败(共享): ") + hr_name(hr) + "（被独占占用时为 DEVICE_IN_USE）";
            CoTaskMemFree(mix);
            close(); return false;
        }
        CoTaskMemFree(mix);
        ac_->GetBufferSize(&bufferFrames_);
        if (FAILED(ac_->GetService(__uuidof(IAudioRenderClient), (void**)&rc_)) || !rc_) {
            lastErr_ = "GetService(IAudioRenderClient) 失败";
            close(); return false;
        }
        ac_->GetService(__uuidof(IAudioClock), (void**)&clock_);   // 可选：played 增长统计
        return true;
    }

    bool start() {
        if (!ac_ || !rc_) return false;
        written_ = 0;
        const HRESULT hr = ac_->Start();
        if (FAILED(hr)) { lastErr_ = std::string("Start 失败: ") + hr_name(hr); return false; }
        LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
        qpcFreq_ = f.QuadPart; qpcStart_ = t.QuadPart;
        running_ = true;
        return true;
    }

    // 写入交错 float32 样本（采集直通监听等浮点通路）。
    // ★ 目标容器由混音格式决定：float32 直接拷，其余格式走 put_shared_sample 换算
    //   （调用方 tools/rtaweb.cpp:2758 目前只在 is_float() 时进来；换算分支是整型混音
    //    端点下"不写坏数据"的保险，见文件头 ⚠️ 未验证项 2）。
    size_t write_available_f32(const float* src, size_t availFrames) {
        if (!running_ || !ac_ || !rc_ || availFrames == 0) return 0;
        UINT32 pad = 0;
        if (FAILED(ac_->GetCurrentPadding(&pad))) return 0;
        lastPad_ = (long long)pad;
        const size_t toWrite = shared_writable(bufferFrames_, pad, availFrames);
        if (toWrite == 0) return 0;
        BYTE* data = nullptr;
        const HRESULT gb = rc_->GetBuffer((UINT32)toWrite, &data);
        lastGbHr_ = (long long)gb;
        if (FAILED(gb) || !data) return 0;
        pack_f32(data, src, toWrite);
        if (FAILED(rc_->ReleaseBuffer((UINT32)toWrite, 0))) return 0;
        written_ += toWrite;
        return toWrite;
    }

    // 写入交错 int32 样本（按混音格式换算/装箱）。返回实际写入帧数，调用方循环补满。
    size_t write_available(const int32_t* src, size_t availFrames) {
        if (!running_ || !ac_ || !rc_ || availFrames == 0) return 0;
        UINT32 pad = 0;
        if (FAILED(ac_->GetCurrentPadding(&pad))) return 0;
        lastPad_ = (long long)pad;
        const size_t toWrite = shared_writable(bufferFrames_, pad, availFrames);
        if (toWrite == 0) return 0;
        BYTE* data = nullptr;
        const HRESULT gb = rc_->GetBuffer((UINT32)toWrite, &data);
        lastGbHr_ = (long long)gb;
        if (FAILED(gb) || !data) return 0;
        pack(data, src, toWrite);
        if (FAILED(rc_->ReleaseBuffer((UINT32)toWrite, 0))) return 0;
        written_ += toWrite;
        return toWrite;
    }

    bool pause()  { return ac_ && ac_->Stop()  == S_OK; }
    bool resume() { return ac_ && ac_->Start() == S_OK; }
    bool reset()  { return ac_ && ac_->Reset() == S_OK; }

    // 实时音量微调（与独占版同语义：相对播放起点的差值 dB；0 = 逐位原路径）
    void set_live_gain_db(double db) {
        if (db <= -144.0) { gain_ = 0.0; return; }
        gain_ = (db == 0.0) ? 1.0 : std::pow(10.0, db / 20.0);
    }

    // 共享模式不用事件：按调用方的节奏睡等（渲染循环 20ms 轮询 padding）
    bool wait_event(int timeoutMs) {
        if (timeoutMs > 0) Sleep((DWORD)timeoutMs);
        return false;
    }

    long long last_pad() const { return lastPad_; }
    long long last_gb_hr() const { return lastGbHr_; }
    std::string last_gb_name() const {
        char b[64];
        std::snprintf(b, sizeof(b), "%08lX %s", (unsigned long)(unsigned)lastGbHr_, hr_name((HRESULT)lastGbHr_));
        return std::string(b);
    }
    size_t buffer_frames() const { return bufferFrames_; }
    size_t frames_written() const { return written_; }
    bool device_position(unsigned long long& pos) const {
        if (!clock_) return false;
        UINT64 p = 0, q = 0;
        if (FAILED(clock_->GetPosition(&p, &q))) return false;
        pos = (unsigned long long)p; return true;
    }
    double elapsed_seconds() const {
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return (double)(t.QuadPart - qpcStart_) / (double)qpcFreq_;
    }
    uint32_t mix_rate() const { return mixRate_; }
    int      mix_channels() const { return mixCh_; }
    // 诚实语义：混音格式**真的是** IEEE float32 才为 true（直通监听的判据用它）。
    // 「共享后端能不能开」请看 query_mix_format 的 isFloat 出参 / mix_supported()。
    bool     is_float() const { return mixFloat_; }
    SharedSampleFmt mix_format() const { return mixFmt_; }
    const char* mix_format_name() const { return shared_fmt_name(mixFmt_); }
    int      mix_bytes() const { return mixBytes_; }        // 每样本容器字节数
    int      mix_valid_bits() const { return mixValid_; }   // 每样本有效位深（实际分辨率）
    bool     mix_supported() const { return mixFmt_ != SharedSampleFmt::Unsupported; }
    // 实际写进混音引擎的最大幅度（归一化域，0..1）。与管线侧 peak（位深域）互为对照：
    // 两者应当一致；不一致就说明换算或位深基准有问题（2026-10-23 现场就是靠它定论的）。
    double   out_peak() const { return outPeak_; }
    uint32_t req_rate() const { return reqRate_; }
    int      req_channels() const { return reqCh_; }
    const std::string& device_id() const { return deviceId_; }
    const std::string& last_error() const { return lastErr_; }

    void close() {
        if (ac_ && running_) { ac_->Stop(); running_ = false; }
        if (clock_) { clock_->Release(); clock_ = nullptr; }
        if (rc_)    { rc_->Release();    rc_ = nullptr; }
        if (ac_)    { ac_->Release();    ac_ = nullptr; }
        bufferFrames_ = 0;
        lastPad_ = -1; lastGbHr_ = -999;
    }

private:
    // float32 混音格式的直通写出（含实时增益；gain=1 时直接 memcpy）
    // 非 float32 混音格式：逐样本换算（生产只在这条路上用 put_shared_sample）
    void pack_f32(BYTE* dst, const float* src, size_t frames) {
        const size_t n = frames * (size_t)mixCh_;
        if (mixFmt_ == SharedSampleFmt::Float32) {
            float* p = (float*)dst;
            if (gain_ != 1.0) {
                for (size_t i = 0; i < n; ++i) p[i] = (float)(src[i] * gain_);
            } else {
                std::memcpy(p, src, n * sizeof(float));
            }
            return;
        }
        // 非 float32 容器：换算后装箱（与离线单测同一份 shared_pack_f32）
        shared_pack_f32(mixFmt_, mixCh_, gain_, (unsigned char*)dst, src, frames);
    }

    // 管线 int32 → 混音容器。
    // 必须用 reqBits_ 归一化：混音引擎的满幅是 ±1.0，而管线交来的是
    // "按源位深右对齐"的 int32（24-bit 源只在 ±2^23 量级）。见 s32_to_f32_bits 的注释。
    void pack(BYTE* dst, const int32_t* src, size_t frames) {
        const size_t n = frames * (size_t)mixCh_;
        const int b = (int)reqBits_;
        if (mixFmt_ == SharedSampleFmt::Float32) {
            float* p = (float*)dst;
            double pk = outPeak_;
            if (gain_ != 1.0) {
                for (size_t i = 0; i < n; ++i) {
                    const double v = s32_to_f32_bits(src[i], b) * gain_;
                    if (std::fabs(v) > pk) pk = std::fabs(v);
                    p[i] = (float)v;
                }
            } else {
                for (size_t i = 0; i < n; ++i) {
                    const double v = s32_to_f32_bits(src[i], b);
                    if (std::fabs(v) > pk) pk = std::fabs(v);
                    p[i] = (float)v;
                }
            }
            outPeak_ = pk;   // 真实写出峰值（归一化域），供 /api/status 的 fpeak 读
            return;
        }
        // 整型混音格式（int16 / 紧凑 24 / 24-in-32 / int32）：整块装箱。
        // ★ 直接调 shared_pack_int32 —— 离线单测断言的就是这个函数（含交错步长），
        //   不是"测试里另写一份、生产里再抄一份"。
        outPeak_ = shared_pack_int32(mixFmt_, mixCh_, b, gain_, (unsigned char*)dst, src,
                                     frames, outPeak_);
    }

    bool comInit_ = false, running_ = false;
    IAudioClient* ac_ = nullptr;
    IAudioRenderClient* rc_ = nullptr;
    IAudioClock* clock_ = nullptr;
    UINT32 bufferFrames_ = 0;
    uint32_t mixRate_ = 0, reqRate_ = 0, reqBits_ = 0;
    int mixCh_ = 0, reqCh_ = 0;
    bool mixFloat_ = false;
    // 2026-10-24：混音格式对应的写入路径（open() 里由 pick_shared_writer 决定）
    SharedSampleFmt mixFmt_ = SharedSampleFmt::Unsupported;
    int mixBytes_ = 0;    // 每样本容器字节数（2/3/4）
    int mixValid_ = 0;    // 每样本有效位深（16/24/32）
    std::string deviceId_;
    size_t written_ = 0;
    double gain_ = 1.0;
    double outPeak_ = 0.0;      // 实际写进混音引擎的最大幅度（归一化域），见 out_peak()
    long long lastPad_ = -1, lastGbHr_ = -999;
    long long qpcFreq_ = 1, qpcStart_ = 0;
    std::string lastErr_;
};

} // namespace rta::audio
