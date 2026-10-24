// wasapi_capture.h — WASAPI 共享模式采集 / 回环录制核心 + 无锁 SPSC 环形缓冲
//
// 设计原则：
//   1) Header-only，仿 wasapi_out.h 风格；
//   2) 枚举采集端点（eCapture）与回环端点（eRender + AUDCLNT_STREAMFLAGS_LOOPBACK）；
//   3) 共享模式 IAudioClient + IAudioCaptureClient（事件驱动与轮询兼容）；
//   4) 格式转换：设备 mix 格式（16/24/32-bit PCM 或 float）转内部 float32 交错，
//      含 24-bit 紧凑 3 字节符号扩展及正负峰削波计数；
//   5) 无锁 SPSC 环形缓冲：生产者（采集回调/后台线程）与消费者（读取方）解耦，
//      支持溢出/下溢计数与实时水位；
//   6) 纯数字源（--src null / open_null）：不开设备、不发声，精确模拟 48k/2ch 数据流；
//   7) 离线信号分析：RMS / 峰值 / THD+N。
#pragma once
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <algorithm>
#include <complex>
#include "wasapi_out.h"
#include "audio/wav.h"
#include "dsp/fft.h"
#include "engine/pipeline.h"   // 监听走 DSP：复用播放同一份 Pipeline（PEQ/EQ/音量/重采样）
#include "dsp/eq_text.h"       // 文件 EQ（REW/AutoEq 文本）解析 —— pipeline.h 自己不 include 它

namespace rta::audio {

enum class CaptureFormat {
    Unknown,
    Float32,
    Pcm16,
    Pcm24_3Byte,
    Pcm24_In32,
    Pcm32
};

struct FormatDetails {
    CaptureFormat format = CaptureFormat::Unknown;
    uint32_t sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int validBits = 0;
};

struct ClipStats {
    long long posClips = 0;
    long long negClips = 0;
};

// ---------------- 格式转换纯函数（离线完全可测） ----------------

inline float pcm16_sample_to_f32(int16_t s, ClipStats& stats) {
    if (s >= 32767) ++stats.posClips;
    else if (s <= -32768) ++stats.negClips;
    return (float)((double)s / 32768.0);
}

inline float pcm24_3byte_sample_to_f32(const uint8_t* p, ClipStats& stats) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    // 符号扩展：若第 23 位为 1，高 8 位补 1
    int32_t s = (u & 0x800000) ? (int32_t)(u | 0xFF000000) : (int32_t)u;
    if (s >= 8388607) ++stats.posClips;
    else if (s <= -8388608) ++stats.negClips;
    return (float)((double)s / 8388608.0);
}

inline float pcm32_sample_to_f32(int32_t s, ClipStats& stats) {
    if (s >= 2147483647) ++stats.posClips;
    else if (s <= -2147483648) ++stats.negClips;
    return (float)((double)s / 2147483648.0);
}

inline float pcm24_in32_sample_to_f32(int32_t s, ClipStats& stats) {
    if (s >= 2147483392) ++stats.posClips;
    else if (s <= -2147483648) ++stats.negClips;
    return (float)((double)s / 2147483648.0);
}

inline float float32_sample_check_clip(float f, ClipStats& stats) {
    if (f >= 1.0f) ++stats.posClips;
    else if (f <= -1.0f) ++stats.negClips;
    return f;
}

inline void convert_to_float(const void* src, float* dst, size_t frames, int channels,
                            CaptureFormat fmt, ClipStats& stats) {
    if (!src || !dst || frames == 0 || channels <= 0) return;
    const size_t totalSamples = frames * (size_t)channels;

    switch (fmt) {
        case CaptureFormat::Float32: {
            const float* p = (const float*)src;
            for (size_t i = 0; i < totalSamples; ++i) {
                dst[i] = float32_sample_check_clip(p[i], stats);
            }
            break;
        }
        case CaptureFormat::Pcm16: {
            const int16_t* p = (const int16_t*)src;
            for (size_t i = 0; i < totalSamples; ++i) {
                dst[i] = pcm16_sample_to_f32(p[i], stats);
            }
            break;
        }
        case CaptureFormat::Pcm24_3Byte: {
            const uint8_t* p = (const uint8_t*)src;
            for (size_t i = 0; i < totalSamples; ++i) {
                dst[i] = pcm24_3byte_sample_to_f32(p + i * 3, stats);
            }
            break;
        }
        case CaptureFormat::Pcm24_In32: {
            const int32_t* p = (const int32_t*)src;
            for (size_t i = 0; i < totalSamples; ++i) {
                dst[i] = pcm24_in32_sample_to_f32(p[i], stats);
            }
            break;
        }
        case CaptureFormat::Pcm32: {
            const int32_t* p = (const int32_t*)src;
            for (size_t i = 0; i < totalSamples; ++i) {
                dst[i] = pcm32_sample_to_f32(p[i], stats);
            }
            break;
        }
        default: {
            std::memset(dst, 0, totalSamples * sizeof(float));
            break;
        }
    }
}

inline bool parse_mix_format(const WAVEFORMATEX* wfx, FormatDetails& details) {
    if (!wfx) return false;
    details.sampleRate = wfx->nSamplesPerSec;
    details.channels = wfx->nChannels;
    details.bitsPerSample = wfx->wBitsPerSample;
    details.validBits = wfx->wBitsPerSample;

    static const GUID SUB_FLOAT = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
    static const GUID SUB_PCM   = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        if (wfx->wBitsPerSample == 32) {
            details.format = CaptureFormat::Float32;
            return true;
        }
    } else if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        if (wfx->wBitsPerSample == 16) {
            details.format = CaptureFormat::Pcm16;
            return true;
        } else if (wfx->wBitsPerSample == 24) {
            details.format = CaptureFormat::Pcm24_3Byte;
            return true;
        } else if (wfx->wBitsPerSample == 32) {
            details.format = CaptureFormat::Pcm32;
            return true;
        }
    } else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto* ext = (const WAVEFORMATEXTENSIBLE*)wfx;
        details.validBits = ext->Samples.wValidBitsPerSample;
        if (ext->SubFormat == SUB_FLOAT) {
            if (wfx->wBitsPerSample == 32) {
                details.format = CaptureFormat::Float32;
                return true;
            }
        } else if (ext->SubFormat == SUB_PCM) {
            if (wfx->wBitsPerSample == 16) {
                details.format = CaptureFormat::Pcm16;
                return true;
            } else if (wfx->wBitsPerSample == 24) {
                details.format = CaptureFormat::Pcm24_3Byte;
                return true;
            } else if (wfx->wBitsPerSample == 32) {
                if (details.validBits == 24) {
                    details.format = CaptureFormat::Pcm24_In32;
                } else {
                    details.format = CaptureFormat::Pcm32;
                }
                return true;
            }
        }
    }
    return false;
}

// ---------------- 无锁 SPSC 环形缓冲（FIFO） ----------------

template <typename T = float>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t capacityFrames = 65536, int channels = 2) {
        init(capacityFrames, channels);
    }

    void init(size_t capacityFrames, int channels) {
        channels_ = channels > 0 ? channels : 1;
        size_t cap = 64;
        while (cap < capacityFrames) cap <<= 1;
        capacity_ = cap;
        mask_ = capacity_ - 1;
        buffer_.assign(capacity_ * (size_t)channels_, T(0));
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
        overflow_count_.store(0, std::memory_order_relaxed);
        underflow_count_.store(0, std::memory_order_relaxed);
    }

    void reset() {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
        overflow_count_.store(0, std::memory_order_relaxed);
        underflow_count_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const { return capacity_; }
    int channels() const { return channels_; }

    size_t available_read() const {
        uint64_t w = write_pos_.load(std::memory_order_acquire);
        uint64_t r = read_pos_.load(std::memory_order_relaxed);
        return (size_t)(w - r);
    }

    size_t available_write() const {
        uint64_t w = write_pos_.load(std::memory_order_relaxed);
        uint64_t r = read_pos_.load(std::memory_order_acquire);
        size_t occupied = (size_t)(w - r);
        return capacity_ > occupied ? (capacity_ - occupied) : 0;
    }

    double water_level() const {
        return capacity_ > 0 ? ((double)available_read() / (double)capacity_) : 0.0;
    }

    uint64_t overflow_count() const {
        return overflow_count_.load(std::memory_order_relaxed);
    }

    uint64_t underflow_count() const {
        return underflow_count_.load(std::memory_order_relaxed);
    }

    // 生产者写入：空间不足时写入可用部分并丢弃超出帧，超出帧计入 overflow_count
    size_t write(const T* data, size_t frames) {
        if (!data || frames == 0) return 0;
        uint64_t w = write_pos_.load(std::memory_order_relaxed);
        uint64_t r = read_pos_.load(std::memory_order_acquire);
        size_t occupied = (size_t)(w - r);
        size_t space = capacity_ > occupied ? (capacity_ - occupied) : 0;
        size_t toWrite = std::min(frames, space);
        if (toWrite < frames) {
            overflow_count_.fetch_add(frames - toWrite, std::memory_order_relaxed);
        }
        if (toWrite == 0) return 0;

        size_t idx = (size_t)(w & mask_);
        size_t first = std::min(toWrite, capacity_ - idx);
        size_t second = toWrite - first;

        std::memcpy(buffer_.data() + idx * (size_t)channels_, data, first * (size_t)channels_ * sizeof(T));
        if (second > 0) {
            std::memcpy(buffer_.data(), data + first * (size_t)channels_, second * (size_t)channels_ * sizeof(T));
        }
        write_pos_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    // 消费者读出（非阻塞，只读当前可用部分，不计下溢）
    size_t read_available(T* dst, size_t maxFrames) {
        if (!dst || maxFrames == 0) return 0;
        uint64_t r = read_pos_.load(std::memory_order_relaxed);
        uint64_t w = write_pos_.load(std::memory_order_acquire);
        size_t avail = (size_t)(w - r);
        size_t toRead = std::min(maxFrames, avail);
        if (toRead == 0) return 0;

        size_t idx = (size_t)(r & mask_);
        size_t first = std::min(toRead, capacity_ - idx);
        size_t second = toRead - first;

        std::memcpy(dst, buffer_.data() + idx * (size_t)channels_, first * (size_t)channels_ * sizeof(T));
        if (second > 0) {
            std::memcpy(dst + first * (size_t)channels_, buffer_.data(), second * (size_t)channels_ * sizeof(T));
        }
        read_pos_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    // 消费者读取期望帧数：不足时将缺失帧计入 underflow_count
    size_t read(T* dst, size_t frames) {
        size_t got = read_available(dst, frames);
        if (got < frames) {
            underflow_count_.fetch_add(frames - got, std::memory_order_relaxed);
        }
        return got;
    }

private:
    size_t capacity_ = 0;
    size_t mask_ = 0;
    int channels_ = 1;
    std::vector<T> buffer_;

    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    alignas(64) std::atomic<uint64_t> overflow_count_{0};
    alignas(64) std::atomic<uint64_t> underflow_count_{0};
};

// ---------------- 信号客观测量（RMS / 峰值 / THD+N） ----------------

struct AnalysisResult {
    size_t frames = 0;
    int channels = 0;
    uint32_t sampleRate = 0;
    std::vector<double> rms;
    std::vector<double> rmsDb;
    double overallRms = 0.0;
    double overallRmsDb = -999.0;
    std::vector<double> peak;
    std::vector<double> peakDb;
    double overallPeak = 0.0;
    double overallPeakDb = -999.0;
    double fundamentalFreq = 0.0;
    double thdnDb = -999.0; // 基波功率 / (带内失真+噪声功率)，正 dB
    long long posClips = 0;
    long long negClips = 0;
};

inline AnalysisResult analyze_audio(const float* interleaved, size_t frames, int channels,
                                   uint32_t sampleRate, double expectedFreq = 1000.0) {
    AnalysisResult res;
    res.frames = frames;
    res.channels = channels;
    res.sampleRate = sampleRate;
    res.fundamentalFreq = expectedFreq;
    if (frames == 0 || channels <= 0) return res;

    res.rms.assign(channels, 0.0);
    res.rmsDb.assign(channels, -999.0);
    res.peak.assign(channels, 0.0);
    res.peakDb.assign(channels, -999.0);

    std::vector<double> sumSq(channels, 0.0);
    for (size_t n = 0; n < frames; ++n) {
        for (int ch = 0; ch < channels; ++ch) {
            double s = interleaved[n * (size_t)channels + ch];
            double absS = std::fabs(s);
            if (absS > res.peak[ch]) res.peak[ch] = absS;
            sumSq[ch] += s * s;
            if (s >= 1.0) res.posClips++;
            if (s <= -1.0) res.negClips++;
        }
    }

    double totalSumSq = 0.0;
    double maxPeak = 0.0;
    for (int ch = 0; ch < channels; ++ch) {
        double r = std::sqrt(sumSq[ch] / (double)frames);
        res.rms[ch] = r;
        res.rmsDb[ch] = 20.0 * std::log10(r > 1e-12 ? r : 1e-12);
        totalSumSq += sumSq[ch];
        if (res.peak[ch] > maxPeak) maxPeak = res.peak[ch];
        res.peakDb[ch] = 20.0 * std::log10(res.peak[ch] > 1e-12 ? res.peak[ch] : 1e-12);
    }
    res.overallRms = std::sqrt(totalSumSq / (double)(frames * (size_t)channels));
    res.overallRmsDb = 20.0 * std::log10(res.overallRms > 1e-12 ? res.overallRms : 1e-12);
    res.overallPeak = maxPeak;
    res.overallPeakDb = 20.0 * std::log10(maxPeak > 1e-12 ? maxPeak : 1e-12);

    // THD+N 分析（基于声道 0，采用最小二乘基波拟合 + 加窗残差 FFT 积分带内噪声）
    if (expectedFreq > 0.0 && frames >= 1024 && sampleRate > 0) {
        size_t N = 1024;
        while (N * 2 <= frames && N * 2 <= 65536) N *= 2;

        size_t offset = (frames > N + 1024) ? 1024 : 0;
        if (offset + N > frames) offset = frames - N;

        const double w0 = 2.0 * 3.14159265358979323846 * expectedFreq / (double)sampleRate;
        double cc = 0.0, ss = 0.0, cs = 0.0, xc = 0.0, xs = 0.0;
        for (size_t n = 0; n < N; ++n) {
            double val = interleaved[(offset + n) * (size_t)channels + 0];
            double c = std::cos(w0 * (double)n);
            double s = std::sin(w0 * (double)n);
            cc += c * c; ss += s * s; cs += c * s;
            xc += val * c; xs += val * s;
        }
        double det = cc * ss - cs * cs;
        if (std::fabs(det) > 1e-12) {
            double A = (xc * ss - xs * cs) / det;
            double B = (xs * cc - xc * cs) / det;
            double psig = (A * A + B * B) / 2.0;

            std::vector<std::complex<double>> xw(N);
            double sumW2 = 0.0;
            for (size_t n = 0; n < N; ++n) {
                double val = interleaved[(offset + n) * (size_t)channels + 0];
                double resid = val - (A * std::cos(w0 * (double)n) + B * std::sin(w0 * (double)n));
                double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double)n / (double)(N - 1));
                sumW2 += w * w;
                xw[n] = std::complex<double>(resid * w, 0.0);
            }
            rta::dsp::fft(xw);

            double binHz = (double)sampleRate / (double)N;
            double norm = (double)N * sumW2;
            size_t kLo = (size_t)std::ceil(20.0 / binHz);
            size_t kTop = (size_t)std::floor(20000.0 / binHz);
            if (kTop >= N / 2) kTop = N / 2 - 1;
            if (kLo < 1) kLo = 1;

            double pnoise = 0.0;
            for (size_t k = kLo; k <= kTop; ++k) {
                pnoise += std::norm(xw[k]) / norm;
            }

            if (pnoise > 0.0 && psig > 0.0) {
                res.thdnDb = 10.0 * std::log10(psig / (2.0 * pnoise + 1e-300));
            } else if (pnoise == 0.0 && psig > 0.0) {
                res.thdnDb = 180.0;
            }
        }
    }
    return res;
}

// ---------------- WAV 写出辅助 ----------------

inline AudioBuffer float_to_audio_buffer(const float* interleaved, size_t frames,
                                        int channels, uint32_t rate, int bits = 24) {
    AudioBuffer ab;
    ab.rate = rate;
    ab.channels = channels;
    ab.bits = bits;
    ab.isFloat = (bits == 32);
    const size_t total = frames * (size_t)channels;
    ab.raw.resize(total);

    if (ab.isFloat) {
        for (size_t i = 0; i < total; ++i) {
            float f = interleaved[i];
            uint32_t u; std::memcpy(&u, &f, 4);
            ab.raw[i] = (int32_t)u;
        }
    } else {
        const double scale = (double)(1u << (bits - 1));
        const double lo = -scale;
        const double hi = scale - 1.0;
        for (size_t i = 0; i < total; ++i) {
            double v = (double)interleaved[i] * scale;
            v = (v >= 0.0) ? std::floor(v + 0.5) : std::ceil(v - 0.5);
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            ab.raw[i] = (int32_t)v;
        }
    }
    return ab;
}

inline void write_capture_wav(const std::string& path, const float* interleaved,
                              size_t frames, int channels, uint32_t rate, int bits = 24) {
    AudioBuffer ab = float_to_audio_buffer(interleaved, frames, channels, rate, bits);
    write_wav(path, ab);
}

// =====================================================================================
// 监听走 DSP（monitor=dsp）：采集块 → 现有 DSP 管线 → 共享输出块
//
// 背景（2026-10-24）：旧直通监听把采集缓冲**原样**写进默认共享输出端点
// （tools/rtaweb.cpp 的 input_worker_func），不经过 PEQ / EQ / 音量 / 升频，
// 用户要求"监听也要能加 EQ/PEQ/音量"。
//
// 本段的职责边界：
//   * 输入 = WasapiCapture 已经归一化好的【交错 float32】（设备是 16/24/32-bit PCM 还是
//     float 都在 convert_to_float 里变成 float32 了）；输出 = 共享输出端点要的【交错 float32】。
//   * 中间就是**现有**的 rta::engine::Pipeline：PEQ → 文件 EQ / 每声道增益 → 重采样 → 音量，
//     与播放走同一份代码、同一套参数来源（rtaweb 侧从设置文件读同一批键）。
//   * **不碰任何设备**：打开端点 / 写端点都在调用方。正因如此，这一整段可以被
//     tests/test_monitor_dsp.cpp 完全离线断言（不打开音频设备、不发声，铁律 1 无涉）。
//
// 与播放链【刻意】的三处差异（必须留档，别被当成 bug）：
//   1) 重采样抽头按"每相位 N 抽头"给（monitor_resample_taps），不用播放的 26 万抽头：
//      监听要的是低延迟与实时余量，不是 -180 dB 阻带。群延迟量级 1~2 ms（见 resample_latency_ms）。
//   2) 不加载卷积 IR：房间校正 IR 会引入数十~数百 ms 延迟，而用户点名的是
//      PEQ / EQ / 音量 / matrix 四项。
//   3) 输出固定 32-bit 整数容器（精度等价 float），不做抖动/噪声整形 ——
//      后面紧接着就是混音引擎的 float/int16/int24 转换（wasapi_shared.h）。
// =====================================================================================

// 监听重采样：给定"每相位抽头数"换算成 PipelineConfig::taps（内插域总抽头数）。
// 为什么不能直接给总抽头数：T = totalTaps / L，而 L = outRate/gcd(inRate,outRate)。
// 44.1k→48k 的 L=160，若照搬播放的 262144 总抽头，T=1638 —— 每输出样本 1638 次乘加，
// 2 声道 48k 下就是 1.6 亿次/秒，实时余量会被吃光；反过来 48k→96k 的 L=2，
// 262144 总抽头 = 每相位 131072 抽头，更荒谬。按相位给就与采样率无关了。
inline uint32_t monitor_gcd(uint32_t a, uint32_t b) {
    while (b != 0) {
        const uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

inline int monitor_resample_taps(uint32_t inRate, uint32_t outRate, int tapsPerPhase = 256) {
    if (inRate == 0 || outRate == 0) return 0;
    const uint32_t g = monitor_gcd(inRate, outRate);
    const uint64_t L = (uint64_t)(outRate / (g ? g : 1));
    if (tapsPerPhase < 8) tapsPerPhase = 8;
    uint64_t total = L * (uint64_t)tapsPerPhase;
    if (total < 256) total = 256;            // 抽头太少时阻带会塌（Kaiser 设计需要足够长度）
    if (total > 262144) total = 262144;      // 上限：与播放同量级，避免病态比率把内存吃光
    return (int)total;
}

// 采集声道数 ≠ 输出声道数时的默认路由。
// 空 = 交给管线走单位路由（前 min(in,out) 个声道一一对应，其余补零），
// 这正是播放的语义；唯一需要显式展开的是【单声道 → 多声道复制】
// （旧 raw 直通就是复制到左右两个声道，用户听到的是"两边都有声"，不能因为走 DSP 就变单边）。
inline std::vector<rta::dsp::MatrixRoute> monitor_default_matrix(int inCh, int outCh) {
    std::vector<rta::dsp::MatrixRoute> r;
    if (inCh <= 0 || outCh <= 0 || inCh == outCh) return r;
    if (inCh == 1) {
        for (int c = 0; c < outCh; ++c) {
            rta::dsp::MatrixRoute rt;
            rt.src = 0; rt.dst = c; rt.gain = 1.0;
            r.push_back(rt);
        }
    }
    return r;
}

// 端到端延迟估计（纯数据，无设备）。单位 ms。
// 口径写死在这里，避免 rtaweb 每次自己拼数字（两处不一致就又是一次"判据错误"）：
//   totalMs = 采集端点缓冲 + 采集 FIFO 实测积压 + 重采样群延迟 + DSP 块耗时 + 共享输出缓冲深度
// 用的是【缓冲深度】而不是"当前占用"：待播帧数在运行中会随设备节奏起伏（实测常接近 0），
// 拿它当端到端延迟会随采样时刻乱跳，也不能回答"最坏情况多少毫秒"。
// outputFillMs 是【实测待播】（GetCurrentPadding + 我们待写队列），单独回报供现场看真实占用 ——
// 它不参与 totalMs 求和，否则同一个缓冲会被算两遍。
// ⚠️ 这是"缓冲深度口径"的估计，不是硬件往返实测：真实声卡还有驱动/引擎内部延迟，
//    未做环路实测（铁律 4：没测过的就不许写成测过）。
struct MonitorLatency {
    double captureBufMs  = 0.0;   // 采集端点缓冲时长（WASAPI 分配的缓冲；null 源为 0）
    double fifoMs        = 0.0;   // 采集环形缓冲实测积压
    double resampleMs    = 0.0;   // 重采样线性相位群延迟（无重采样 = 0）
    double dspMs         = 0.0;   // 一块 DSP 的处理耗时（EMA 实测）
    double outputFillMs  = 0.0;   // 共享输出端点当前待播（实测，仅供参考）
    double outputBufMs   = 0.0;   // 共享输出端点缓冲深度
    double totalMs       = 0.0;   // 缓冲深度口径（最坏情况，见上）
    // 实测占用口径：把 outputBufMs 换成【当前真的排队着】的 outputFillMs。
    // 稳态下监听是"来多少写多少"，端点缓冲里往往只有几毫秒 ⟹ liveMs 通常远小于 totalMs。
    // 两个都给，读者才不会把"最坏情况 300ms"误当成"现在就是 300ms"。
    double liveMs        = 0.0;
};

inline MonitorLatency monitor_latency_estimate(double captureBufMs, double fifoMs, double resampleMs,
                                               double dspMs, double outputFillMs, double outputBufMs) {
    MonitorLatency l;
    l.captureBufMs = captureBufMs > 0.0 ? captureBufMs : 0.0;
    l.fifoMs       = fifoMs > 0.0 ? fifoMs : 0.0;
    l.resampleMs   = resampleMs > 0.0 ? resampleMs : 0.0;
    l.dspMs        = dspMs > 0.0 ? dspMs : 0.0;
    l.outputBufMs  = outputBufMs > 0.0 ? outputBufMs : 0.0;
    l.outputFillMs = outputFillMs > 0.0 ? outputFillMs : 0.0;
    if (l.outputFillMs > l.outputBufMs) l.outputFillMs = l.outputBufMs;
    const double base = l.captureBufMs + l.fifoMs + l.resampleMs + l.dspMs;
    l.totalMs = base + l.outputBufMs;
    l.liveMs  = base + l.outputFillMs;
    return l;
}

// 监听 DSP 配置：参数来源与播放是同一套设置；重采样参数是监听专用的（见上）。
struct MonitorDspConfig {
    uint32_t inRate      = 48000;
    int      inChannels  = 2;
    uint32_t outRate     = 48000;
    int      outChannels = 2;

    bool     volumeEnabled = true;
    double   volumeDb      = 0.0;

    // 20 段 PEQ：调用方持有引擎（与播放同一个 PeqEngine 类型），已按 inRate prepare。
    // 传 nullptr = 不用 PEQ（等价于全 0 dB / bypass）。
    std::shared_ptr<rta::dsp::PeqEngine> peq;

    // 文件 EQ（REW / AutoEq 文本）。解析失败不会静默吞掉：notes() 里留一行说明。
    std::string eqText;

    // 路由：空 = monitor_default_matrix()
    std::vector<rta::dsp::MatrixRoute> matrix;

    // 每输出声道微调 dB（与播放的 trim 同语义，0 = 不动）
    double trimDb[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    int      tapsPerPhase   = 256;    // 监听用短滤波器
    double   attenDb        = 100.0;  // 阻带目标（不追 -180 dB）
    double   transition     = 0.05;   // 过渡带占比
    bool     useGpuResample = false;  // 监听不依赖 rtagpu.dll（CPU 足够，少一次 DLL 探测）

    // 起流前"预跑"的静音长度（ms）。必须 > 管线的 volumeRampMs（本链固定 50ms）。
    // 作用见 configure() 里的注释：把音量渐变跑完，避免开麦头 50ms 以 0 dB 漏音。
    // 0 = 不预跑 —— 只给测试做"判据自证"用，生产永远用默认值。
    double   primeMs = 60.0;
};

// 采集块 → DSP → 输出块的纯处理单元（不碰设备；跨调用保持滤波器/重采样相位状态）。
class MonitorDspChain {
public:
    MonitorDspChain() = default;
    MonitorDspChain(const MonitorDspChain&) = delete;
    MonitorDspChain& operator=(const MonitorDspChain&) = delete;

    bool configure(const MonitorDspConfig& cfg, std::string* err = nullptr) {
        auto fail = [&](const std::string& m) {
            lastErr_ = m;
            if (err) *err = m;
            return false;
        };
        pipe_.reset();
        cfg_ = cfg;
        lastErr_.clear();
        notes_.clear();
        inFramesTotal_ = 0;
        outFramesTotal_ = 0;
        resampleMs_ = 0.0;
        eqLoaded_ = false;
        if (cfg_.inRate == 0 || cfg_.outRate == 0) {
            return fail("监听 DSP 配置非法: 采样率必须 > 0 (采集=" + std::to_string(cfg_.inRate) +
                        ", 输出=" + std::to_string(cfg_.outRate) + ")");
        }
        if (cfg_.inChannels <= 0 || cfg_.inChannels > 32 ||
            cfg_.outChannels <= 0 || cfg_.outChannels > 32) {
            return fail("监听 DSP 配置非法: 声道数必须在 1..32 (采集=" + std::to_string(cfg_.inChannels) +
                        ", 输出=" + std::to_string(cfg_.outChannels) + ")");
        }
        if (cfg_.tapsPerPhase <= 0) cfg_.tapsPerPhase = 256;

        rta::engine::PipelineConfig pc;
        pc.outRate        = cfg_.outRate;
        pc.outBits        = 32;                     // 输出统一 32-bit 整数容器（精度等价 float）
        pc.outChannels    = cfg_.outChannels;
        pc.shaper         = rta::dsp::Shaper::None; // 监听不做噪声整形
        pc.volumeEnabled  = cfg_.volumeEnabled;
        pc.volumeDb       = cfg_.volumeDb;
        pc.volumeRampMs   = 50.0;                   // 监听比播放(200ms)短：调音量要跟手
        pc.peq            = cfg_.peq;
        pc.taps           = monitor_resample_taps(cfg_.inRate, cfg_.outRate, cfg_.tapsPerPhase);
        pc.attenDb        = cfg_.attenDb;
        pc.transition     = cfg_.transition;
        pc.twoStage       = false;                  // 监听比率通常不需要两段式
        pc.gpuResample    = cfg_.useGpuResample;
        pc.matrix         = cfg_.matrix.empty() ? monitor_default_matrix(cfg_.inChannels, cfg_.outChannels)
                                                : cfg_.matrix;

        // 文件 EQ：与播放同一条路径（解析器 + 在每个输出声道上同一套双二阶）
        if (!cfg_.eqText.empty()) {
            try {
                const rta::dsp::EqSpec spec = rta::dsp::parse_eq_text(cfg_.eqText);
                const std::vector<rta::dsp::Biquad> eq = rta::dsp::build_eq_filters(spec, (double)cfg_.inRate);
                if (!eq.empty() || spec.preampDb != 0.0) {
                    if (pc.channel.size() < (size_t)cfg_.outChannels) pc.channel.resize((size_t)cfg_.outChannels);
                    for (auto& cp : pc.channel) {
                        cp.eq = eq;
                        if (spec.preampDb != 0.0) cp.gainDb += spec.preampDb;
                    }
                    eqLoaded_ = true;
                } else {
                    eqLoaded_ = false;
                    notes_ += (notes_.empty() ? "" : "；") + std::string("文件 EQ 没有有效滤波器（已忽略）");
                }
            } catch (...) {
                eqLoaded_ = false;   // 与播放一致：坏文件当没配，但要留痕
                notes_ += (notes_.empty() ? "" : "；") + std::string("文件 EQ 解析失败（已忽略）");
            }
        }

        // 每声道微调
        if (pc.channel.size() < (size_t)cfg_.outChannels) pc.channel.resize((size_t)cfg_.outChannels);
        for (int c = 0; c < cfg_.outChannels && c < 8; ++c) {
            if (cfg_.trimDb[c] != 0.0) pc.channel[(size_t)c].gainDb += cfg_.trimDb[c];
        }

        try {
            pipe_ = std::make_unique<rta::engine::Pipeline>(pc);
        } catch (const std::exception& e) {
            return fail(std::string("监听 DSP 管线构造失败: ") + e.what());
        }

        // ---- 起流防爆音：把音量渐变先跑完 ----
        // VolumeControl 的 current_ 初值是 1.0（0 dB），set_gain_db() 只会把 target_ 设为
        // 用户音量并安排一段渐变。监听是"开麦即出声"：若直接开始，头 50ms 会以接近 0 dB
        // 把采集到的声音送出去 —— 用户听到的是开麦瞬间的一记漏音/爆音。音量设得越低越明显
        //（增益从 1.0 掉到目标值）。
        // 这里先喂一段【数字静音】把渐变跑完，输出丢弃：喂的是 0，等价于流从 t=0 开始，
        // 重采样相位与滤波器历史照常建立，不改变之后的任何样本。
        if (cfg_.primeMs > 0.0) {
            const size_t primeFrames =
                (size_t)std::max(1.0, std::ceil(cfg_.primeMs * (double)cfg_.inRate / 1000.0));
            std::vector<float> zeros(primeFrames * (size_t)cfg_.inChannels, 0.0f);
            std::vector<float> sink;
            process(zeros.data(), primeFrames, sink);
            inFramesTotal_ = 0;
            outFramesTotal_ = 0;
        }
        if (!lastErr_.empty()) return fail(lastErr_);
        return true;
    }

    // 处理一块采集数据：in = 交错 float32（cfg.inChannels），把结果【追加】到
    // outInterleaved（交错 float32，cfg.outChannels）。返回本次产生的输出帧数。
    size_t process(const float* in, size_t inFrames, std::vector<float>& outInterleaved) {
        if (!pipe_ || !in || inFrames == 0 || cfg_.inChannels <= 0) return 0;
        try {
            const int inCh = cfg_.inChannels;
            rta::audio::AudioBuffer ib;
            ib.rate = cfg_.inRate;
            ib.channels = inCh;
            ib.bits = 32;
            ib.isFloat = false;
            ib.raw.resize(inFrames * (size_t)inCh);
            for (size_t f = 0; f < inFrames; ++f) {
                for (int c = 0; c < inCh; ++c) {
                    double x = (double)in[f * (size_t)inCh + (size_t)c];
                    if (!std::isfinite(x)) x = 0.0;     // 上游 bug 不该变成满幅直流/爆音
                    ib.setSample(f, c, x);              // ±1.0 → 2^31 标度，内部自动钳位
                }
            }

            rta::engine::PipelineStats st;
            rta::audio::AudioBuffer ob = pipe_->process(ib, &st);
            const size_t nOut = ob.frames();
            const int    oc   = ob.channels;
            if (nOut == 0 || oc <= 0) return 0;

            const double fscale = ob.fullScale();       // 32-bit → 2^31
            const int    outCh  = cfg_.outChannels;
            const size_t base   = outInterleaved.size();
            outInterleaved.resize(base + nOut * (size_t)outCh);
            for (size_t f = 0; f < nOut; ++f) {
                for (int c = 0; c < outCh; ++c) {
                    const int sc = (c < oc) ? c : (oc - 1);
                    const double v = (double)ob.raw[f * (size_t)oc + (size_t)sc] / fscale;
                    outInterleaved[base + f * (size_t)outCh + (size_t)c] = (float)v;
                }
            }
            resampleMs_ = (st.usedResample && cfg_.inRate > 0)
                              ? st.latencyInSamples * 1000.0 / (double)cfg_.inRate
                              : 0.0;
            bitPerfect_ = st.bitPerfect;
            usedPeq_   = st.usedPeq;
            usedEq_    = st.usedEq;
            usedVol_   = st.usedVolume;
            usedRs_    = st.usedResample;
            inFramesTotal_  += inFrames;
            outFramesTotal_ += nOut;
            return nOut;
        } catch (const std::exception& e) {
            lastErr_ = std::string("监听 DSP 处理失败: ") + e.what();
            return 0;
        }
    }

    // 清滤波器/重采样历史（停流后重新起流用它；正常停流直接重建对象亦可）
    void reset() { if (pipe_) pipe_->seek_reset(); }

    bool   ready() const { return pipe_ != nullptr; }
    uint32_t in_rate() const { return cfg_.inRate; }
    uint32_t out_rate() const { return cfg_.outRate; }
    int    in_channels() const { return cfg_.inChannels; }
    int    out_channels() const { return cfg_.outChannels; }
    int    taps_per_phase() const { return cfg_.tapsPerPhase; }
    int    taps_total() const { return monitor_resample_taps(cfg_.inRate, cfg_.outRate, cfg_.tapsPerPhase); }
    double resample_latency_ms() const { return resampleMs_; }
    uint64_t in_frames_total() const { return inFramesTotal_; }
    uint64_t out_frames_total() const { return outFramesTotal_; }
    bool   bit_perfect() const { return bitPerfect_; }
    bool   used_peq() const { return usedPeq_; }
    bool   used_eq() const { return usedEq_; }
    bool   used_volume() const { return usedVol_; }
    bool   used_resample() const { return usedRs_; }
    bool   eq_loaded() const { return eqLoaded_; }
    const std::string& notes() const { return notes_; }
    const std::string& last_error() const { return lastErr_; }

private:
    MonitorDspConfig cfg_;
    std::unique_ptr<rta::engine::Pipeline> pipe_;
    double   resampleMs_ = 0.0;
    bool     bitPerfect_ = false, usedPeq_ = false, usedEq_ = false, usedVol_ = false, usedRs_ = false;
    bool     eqLoaded_ = false;
    uint64_t inFramesTotal_ = 0, outFramesTotal_ = 0;
    std::string notes_;
    std::string lastErr_;
};

// ---------------- WASAPI 采集设备描述与核心类 ----------------

struct CaptureDeviceInfo {
    std::string name;
    std::string id;
    DWORD       state = 0;
    bool        isDefault = false;
    bool        isLoopback = false;
    uint32_t    mixRate = 0;
    int         mixChannels = 0;
    int         mixBits = 0;
};

struct CaptureConfig {
    std::string deviceId;          // 端点 ID 或名称子串；空 = 默认
    bool        loopback = false;  // AUDCLNT_STREAMFLAGS_LOOPBACK（在渲染端点上采系统混音）
    bool        eventDriven = true; // 事件驱动 vs 轮询
    double      bufferMs = 100.0;  // 缓冲时长
    bool        srcNull = false;   // 模拟源（48k/2ch，不开设备，自动化门禁用）
    double      injectFreq = 1000.0;
    double      injectAmp = 0.001; // 约 -60 dBFS
    size_t      targetFrames = 0;  // 目标帧数（0 = 持续推流直到手动 stop）
    bool        paced = true;      // 是否按实时节奏（10ms 包）推送
};

class WasapiCapture {
public:
    WasapiCapture() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInit_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }

    ~WasapiCapture() {
        close();
        if (comInit_) CoUninitialize();
    }

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    // 枚举采集端点（可附带输出端点作为 loopback 源）
    static std::vector<CaptureDeviceInfo> enumerate_capture(bool includeLoopback = true) {
        std::vector<CaptureDeviceInfo> out;
        HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool own = SUCCEEDED(ci);
        if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) return out;

        IMMDeviceEnumerator* en = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), (void**)&en)) && en) {
            auto probeFlow = [&](EDataFlow flow, bool isLb) {
                IMMDevice* defDev = nullptr;
                en->GetDefaultAudioEndpoint(flow, eConsole, &defDev);
                LPWSTR defId = nullptr;
                if (defDev) defDev->GetId(&defId);

                IMMDeviceCollection* col = nullptr;
                if (SUCCEEDED(en->EnumAudioEndpoints(flow,
                        DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED,
                        &col)) && col) {
                    UINT32 n = 0; col->GetCount(&n);
                    for (UINT32 i = 0; i < n; ++i) {
                        IMMDevice* d = nullptr;
                        if (FAILED(col->Item(i, &d)) || !d) continue;
                        CaptureDeviceInfo cdi;
                        cdi.isLoopback = isLb;
                        d->GetState(&cdi.state);
                        LPWSTR id = nullptr;
                        if (SUCCEEDED(d->GetId(&id)) && id) {
                            char buf[1024] = {0};
                            WideCharToMultiByte(CP_UTF8, 0, id, -1, buf, sizeof(buf), nullptr, nullptr);
                            cdi.id = buf;
                            if (defId && wcscmp(id, defId) == 0) cdi.isDefault = true;
                        }
                        IPropertyStore* ps = nullptr;
                        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                            PROPVARIANT v; PropVariantInit(&v);
                            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                                char buf[512] = {0};
                                WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, buf, sizeof(buf), nullptr, nullptr);
                                cdi.name = buf;
                            }
                            PropVariantClear(&v);
                            ps->Release();
                        }
                        if (isLb) cdi.name = "[Loopback] " + cdi.name;

                        // 若端点处于活动状态，探测混音格式
                        if (cdi.state == DEVICE_STATE_ACTIVE) {
                            IAudioClient* ac = nullptr;
                            if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) && ac) {
                                WAVEFORMATEX* mix = nullptr;
                                if (SUCCEEDED(ac->GetMixFormat(&mix)) && mix) {
                                    cdi.mixRate = mix->nSamplesPerSec;
                                    cdi.mixChannels = mix->nChannels;
                                    cdi.mixBits = mix->wBitsPerSample;
                                    CoTaskMemFree(mix);
                                }
                                ac->Release();
                            }
                        }
                        out.push_back(std::move(cdi));
                        if (id) CoTaskMemFree(id);
                        d->Release();
                    }
                    col->Release();
                }
                if (defId) CoTaskMemFree(defId);
                if (defDev) defDev->Release();
            };

            // 先枚举麦克风/输入
            probeFlow(eCapture, false);
            // 再枚举扬声器/输出作为 loopback
            if (includeLoopback) {
                probeFlow(eRender, true);
            }
            en->Release();
        }
        if (own) CoUninitialize();
        return out;
    }

    bool open(const CaptureConfig& cfg) {
        close();
        cfg_ = cfg;
        lastErr_.clear();
        clipStats_ = ClipStats{};
        totalFramesCaptured_ = 0;

        if (cfg.srcNull) {
            return open_null(48000, 2, cfg.injectFreq, cfg.injectAmp, cfg.targetFrames, cfg.paced);
        }

        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en))) {
            lastErr_ = "枚举器创建失败"; return false;
        }

        EDataFlow flow = cfg.loopback ? eRender : eCapture;
        IMMDevice* dev = nullptr;
        HRESULT hr = S_OK;

        if (cfg.deviceId.empty()) {
            hr = en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
        } else {
            // 支持精确 ID 或友好名子串匹配
            std::vector<CaptureDeviceInfo> devs = enumerate_capture(cfg.loopback);
            std::string matchedId;
            for (const auto& d : devs) {
                if (d.isLoopback != cfg.loopback) continue;
                if (d.id == cfg.deviceId || d.name.find(cfg.deviceId) != std::string::npos) {
                    matchedId = d.id;
                    break;
                }
            }
            if (!matchedId.empty()) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, matchedId.c_str(), -1, nullptr, 0);
                std::wstring wid((size_t)wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, matchedId.c_str(), -1, wid.data(), wlen);
                hr = en->GetDevice(wid.c_str(), &dev);
            } else {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, cfg.deviceId.c_str(), -1, nullptr, 0);
                std::wstring wid((size_t)wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, cfg.deviceId.c_str(), -1, wid.data(), wlen);
                hr = en->GetDevice(wid.c_str(), &dev);
            }
        }
        en->Release();

        if (FAILED(hr) || !dev) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s", (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = "打开采集端点失败: " + std::string(hb);
            return false;
        }

        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_);
        dev->Release();
        if (FAILED(hr) || !ac_) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s", (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = "Activate(IAudioClient) 失败: " + std::string(hb);
            return false;
        }

        WAVEFORMATEX* mix = nullptr;
        hr = ac_->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s", (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = "GetMixFormat 失败: " + std::string(hb);
            close(); return false;
        }

        if (!parse_mix_format(mix, formatDetails_)) {
            lastErr_ = "不支持的采集设备混音格式";
            CoTaskMemFree(mix);
            close(); return false;
        }

        DWORD flags = 0;
        if (cfg.loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        if (cfg.eventDriven) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        REFERENCE_TIME dur = (REFERENCE_TIME)(cfg.bufferMs * 10000.0);
        hr = ac_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, mix, nullptr);
        CoTaskMemFree(mix);

        if (FAILED(hr)) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s", (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = "Initialize 失败: " + std::string(hb);
            close(); return false;
        }

        eventDriven_ = cfg.eventDriven;
        if (eventDriven_) {
            hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            hr = ac_->SetEventHandle(hEvent_);
            if (FAILED(hr)) {
                char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s", (unsigned long)(unsigned)hr, hr_name(hr));
                lastErr_ = "SetEventHandle 失败: " + std::string(hb);
                close(); return false;
            }
        }

        hr = ac_->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient_);
        if (FAILED(hr) || !captureClient_) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s", (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = "GetService(IAudioCaptureClient) 失败: " + std::string(hb);
            close(); return false;
        }

        ac_->GetBufferSize(&bufferFrames_);
        fifo_.init(65536, formatDetails_.channels);
        opened_ = true;
        return true;
    }

    bool open_null(uint32_t rate = 48000, int channels = 2, double injectFreq = 1000.0,
                   double injectAmp = 0.001, size_t targetFrames = 0, bool paced = true) {
        close();
        isNull_ = true;
        formatDetails_.format = CaptureFormat::Float32;
        formatDetails_.sampleRate = rate;
        formatDetails_.channels = channels;
        formatDetails_.bitsPerSample = 32;
        formatDetails_.validBits = 32;
        injectFreq_ = injectFreq;
        injectAmp_ = injectAmp;
        targetFrames_ = targetFrames;
        paced_ = paced;
        totalFramesCaptured_ = 0;
        clipStats_ = ClipStats{};
        fifo_.init(65536, channels);
        opened_ = true;
        return true;
    }

    bool start() {
        if (!opened_ || running_) return false;
        running_ = true;
        if (isNull_) {
            worker_ = std::thread(&WasapiCapture::null_worker, this);
            return true;
        }
        HRESULT hr = ac_->Start();
        if (FAILED(hr)) {
            running_ = false;
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s", (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = "Start 失败: " + std::string(hb);
            return false;
        }
        worker_ = std::thread(&WasapiCapture::capture_worker, this);
        return true;
    }

    bool stop() {
        if (!running_) return false;
        running_ = false;
        if (hEvent_) SetEvent(hEvent_);
        if (worker_.joinable()) worker_.join();
        if (ac_) ac_->Stop();
        return true;
    }

    void close() {
        stop();
        if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
        if (ac_) { ac_->Release(); ac_ = nullptr; }
        if (hEvent_) { CloseHandle(hEvent_); hEvent_ = nullptr; }
        opened_ = false;
        isNull_ = false;
    }

    size_t read(float* dst, size_t frames) {
        return fifo_.read(dst, frames);
    }

    size_t read_available(float* dst, size_t maxFrames) {
        return fifo_.read_available(dst, maxFrames);
    }

    uint32_t rate() const { return formatDetails_.sampleRate; }
    int channels() const { return formatDetails_.channels; }
    int bits() const { return formatDetails_.bitsPerSample; }
    bool is_null() const { return isNull_; }
    bool is_running() const { return running_; }
    double water_level() const { return fifo_.water_level(); }
    // 监听走 DSP 的延迟估计要用：FIFO 里现有多少帧、以及采集端点缓冲多少帧。
    // （水位数是 0..1 的比例，换算成毫秒还得知道容量 —— 直接给帧数更不容易算错。）
    size_t buffered_frames() const { return fifo_.available_read(); }
    size_t fifo_capacity_frames() const { return fifo_.capacity(); }
    size_t device_buffer_frames() const { return (size_t)bufferFrames_; }
    uint64_t overflow_count() const { return fifo_.overflow_count(); }
    uint64_t underflow_count() const { return fifo_.underflow_count(); }
    long long positive_clips() const { return clipStats_.posClips; }
    long long negative_clips() const { return clipStats_.negClips; }
    size_t total_captured_frames() const { return totalFramesCaptured_; }
    const std::string& last_error() const { return lastErr_; }

private:
    void capture_worker() {
        HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ownCom = SUCCEEDED(ci);
        std::vector<float> convertBuf;

        while (running_) {
            if (eventDriven_ && hEvent_) {
                WaitForSingleObject(hEvent_, 20);
            } else {
                Sleep(10);
            }
            if (!running_) break;

            UINT32 packetFrames = 0;
            HRESULT hr = captureClient_->GetNextPacketSize(&packetFrames);
            while (SUCCEEDED(hr) && packetFrames > 0 && running_) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                UINT64 devPos = 0, qpcPos = 0;
                hr = captureClient_->GetBuffer(&pData, &numFrames, &flags, &devPos, &qpcPos);
                if (SUCCEEDED(hr) && numFrames > 0) {
                    convertBuf.resize(numFrames * (size_t)formatDetails_.channels);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        std::memset(convertBuf.data(), 0, convertBuf.size() * sizeof(float));
                    } else if (pData) {
                        convert_to_float(pData, convertBuf.data(), numFrames,
                                         formatDetails_.channels, formatDetails_.format, clipStats_);
                    }
                    fifo_.write(convertBuf.data(), numFrames);
                    captureClient_->ReleaseBuffer(numFrames);
                    totalFramesCaptured_ += numFrames;
                } else {
                    break;
                }
                hr = captureClient_->GetNextPacketSize(&packetFrames);
            }
        }
        if (ownCom) CoUninitialize();
    }

    void null_worker() {
        const size_t chunkSize = 480;
        const int chs = formatDetails_.channels;
        std::vector<float> chunk(chunkSize * (size_t)chs);
        size_t frameIndex = 0;

        while (running_) {
            if (targetFrames_ > 0 && frameIndex >= targetFrames_) {
                break;
            }
            size_t cur = chunkSize;
            if (targetFrames_ > 0 && frameIndex + cur > targetFrames_) {
                cur = targetFrames_ - frameIndex;
            }
            for (size_t i = 0; i < cur; ++i) {
                double t = (double)(frameIndex + i) / (double)formatDetails_.sampleRate;
                float s = 0.0f;
                if (injectFreq_ > 0.0 && injectAmp_ > 0.0) {
                    s = (float)(injectAmp_ * std::sin(2.0 * 3.14159265358979323846 * injectFreq_ * t));
                }
                if (s >= 1.0f) ++clipStats_.posClips;
                if (s <= -1.0f) ++clipStats_.negClips;
                for (int ch = 0; ch < chs; ++ch) {
                    chunk[i * (size_t)chs + ch] = s;
                }
            }

            while (running_ && fifo_.available_write() < cur) {
                std::this_thread::yield();
            }
            if (!running_) break;

            fifo_.write(chunk.data(), cur);
            frameIndex += cur;
            totalFramesCaptured_ += cur;

            if (paced_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    bool comInit_ = false;
    bool opened_ = false;
    std::atomic<bool> running_{false};
    bool isNull_ = false;
    bool paced_ = true;
    bool eventDriven_ = true;
    double injectFreq_ = 1000.0;
    double injectAmp_ = 0.001;
    size_t targetFrames_ = 0;
    std::atomic<size_t> totalFramesCaptured_{0};

    CaptureConfig cfg_;
    FormatDetails formatDetails_;
    ClipStats clipStats_;
    SpscRingBuffer<float> fifo_;

    IAudioClient* ac_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    HANDLE hEvent_ = nullptr;
    UINT32 bufferFrames_ = 0;
    std::thread worker_;
    std::string lastErr_;
};

} // namespace rta::audio
