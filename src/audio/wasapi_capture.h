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
