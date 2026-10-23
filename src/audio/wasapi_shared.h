// wasapi_shared.h — WASAPI 共享模式输出后端（2026-10-22 VM 事故热修）
//
// 背景：独占模式在 VM 板载 HD Audio 上 GetCurrentPadding 永远等于 bufferFrames
//（write_available 恒 0），200 次超时后渲染循环静默 break，END pos=0 error=""，
// 真机（TASCAM）正常、VM 全灭。共享模式经系统混音引擎，不需要独占接管，
// 是虚拟机 / 远程桌面 / 独占不可用场景的兜底路径（代价：非 bit-perfect）。
//
// 与 WasapiOutput（独占）同接口风格，差异点：
//   * Initialize(AUDCLNT_SHAREMODE_SHARED, 0 flags, 200ms, periodicity=0, GetMixFormat)
//     —— 共享模式 periodicity 必须 0；格式用 GetMixFormat（通常 float32 / 48k / 2ch），
//        请求的 rate/channels/bits 只是"期望"，实际以混音格式为准并回报给调用方
//        （mix_rate() / mix_channels() / is_float()），上层据此定 deviceRate。
//   * 写入：int32 输入 → float32（除以 2^31），无独占的"周期整数倍"约束。
//   * 事件驱动不用于共享：wait_event 直接 Sleep —— 本类按"补满到 bufferFrames−pad、
//     20ms 轮询"的节奏写入（与独占版渲染循环同一骨架）。
//   * 浮点换算与可写帧数计算抽成纯函数（s32_to_f32 / shared_writable），离线可测。
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
#include <string>
#include <cstring>
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
// 而共享混音引擎要吃 ±1.0 的 float32。旧实现一律 s32_to_f32（÷2^31），于是
//   24-bit 源 → 1/256 = **−48.16 dB**，16-bit 源 → 1/65536 = −96.3 dB。
// 症状是"确实没断，但电平极低"——而内部所有判据（peak/clip/isp 都按位深归一化）
// 全部显示正常，唯有真正送进混音引擎那一步少了位深换算（铁律 3 的教科书案例）。
// 32-bit 与未知位深保持原路径；结果 clamp 到 ±1.0，防上游 overshoot 溢出 float 域。
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
    // 静态方法不走构造函数 —— 必须自己初始化 COM（wasapi_out.h enumerate 的老教训）。
    static bool query_mix_format(const std::string& deviceId, uint32_t& rate, int& channels,
                                 bool& isFloat, std::string& err) {
        rate = 0; channels = 0; isFloat = false;
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
        const bool okFmt = describe_mix(wfx, rate, channels, isFloat);
        CoTaskMemFree(wfx);
        ac->Release();
        if (!okFmt) { err = "系统混音格式无法识别"; if (own) CoUninitialize(); return false; }
        if (own) CoUninitialize();
        return true;
    }

    // rate/channels/bits 是"期望值"；实际开流格式 = GetMixFormat。
    // 若与期望不同（通常 48k/float），调用方必须改用 mix_rate()/mix_channels() 重排管线，
    // open 本身不因此失败 —— 差异通过 query_mix_format / 访问器回报。
    bool open(const std::string& deviceId, uint32_t rate, int channels, int bits, double bufferMs = 200.0) {
        (void)bits;   // 共享模式位深由混音引擎决定（float32），请求值仅记录
        close();
        reqRate_ = rate; reqCh_ = channels; reqBits_ = bits;
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
        uint32_t mr = 0; int mch = 0; bool mf = false;
        const bool okFmt = describe_mix(mix, mr, mch, mf);
        if (!okFmt || !mf) {
            char b[128];
            std::snprintf(b, sizeof(b), "rate=%u ch=%d bits=%d tag=0x%04X",
                          (unsigned)mix->nSamplesPerSec, (int)mix->nChannels,
                          (int)mix->wBitsPerSample, (unsigned)mix->wFormatTag);
            lastErr_ = std::string("系统混音格式不是 float32（实际 ") + (okFmt ? "" : "无法识别: ") + b +
                       "），共享后端暂不支持；请使用独占模式";
            CoTaskMemFree(mix);
            close(); return false;
        }
        mixRate_ = mr; mixCh_ = mch; mixFloat_ = true;
        deviceId_ = deviceId;

        // 共享模式：flags=0、periodicity 必须 0（与独占"配方 1"相反）、缓冲给足 200ms。
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

    // 写入交错 float32 样本（用于采集直通监听等浮点通路）
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

    // 写入交错 int32 样本（按 mix 格式换算 float32）。返回实际写入帧数，调用方循环补满。
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
    bool     is_float() const { return mixFloat_; }
    // 实际写进混音引擎的最大幅度（float 域，0..1）。与管线侧 peak（位深域）互为对照：
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
    // 识别 GetMixFormat：float32 直接认；EXTENSIBLE 按 SubFormat 判。
    static bool describe_mix(const WAVEFORMATEX* wfx, uint32_t& rate, int& channels, bool& isFloat) {
        if (!wfx) return false;
        rate = wfx->nSamplesPerSec;
        channels = wfx->nChannels;
        isFloat = false;
        static const GUID SUB_FLOAT = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
        if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = (wfx->wBitsPerSample == 32);
            return isFloat;
        }
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
            const auto* e = (const WAVEFORMATEXTENSIBLE*)wfx;
            if (e->SubFormat == SUB_FLOAT) { isFloat = (wfx->wBitsPerSample == 32); return isFloat; }
            return false;   // 混音引擎理论上恒为 float32；遇到整数格式如实拒绝
        }
        return false;
    }

    // float32 直通写出（含实时增益；gain=1 时直接 memcpy）
    void pack_f32(BYTE* dst, const float* src, size_t frames) {
        const size_t n = frames * (size_t)mixCh_;
        float* p = (float*)dst;
        if (gain_ != 1.0) {
            for (size_t i = 0; i < n; ++i) p[i] = (float)(src[i] * gain_);
        } else {
            std::memcpy(p, src, n * sizeof(float));
        }
    }

    // int32 → float32（含实时增益；gain=1 时走纯换算）
    // 必须用 reqBits_ 归一化：混音引擎的 float32 是 ±1.0 满幅，而管线交来的是
    // "按源位深右对齐"的 int32（24-bit 源只在 ±2^23 量级）。见 s32_to_f32_bits 的注释。
    void pack(BYTE* dst, const int32_t* src, size_t frames) {
        const size_t n = frames * (size_t)mixCh_;
        float* p = (float*)dst;
        const int b = (int)reqBits_;
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
        outPeak_ = pk;   // 真实写出峰值（float 域），供 /api/status 的 fpeak 读
    }

    bool comInit_ = false, running_ = false;
    IAudioClient* ac_ = nullptr;
    IAudioRenderClient* rc_ = nullptr;
    IAudioClock* clock_ = nullptr;
    UINT32 bufferFrames_ = 0;
    uint32_t mixRate_ = 0, reqRate_ = 0, reqBits_ = 0;
    int mixCh_ = 0, reqCh_ = 0;
    bool mixFloat_ = false;
    std::string deviceId_;
    size_t written_ = 0;
    double gain_ = 1.0;
    double outPeak_ = 0.0;      // 实际写进混音引擎的最大幅度（float 域），见 out_peak()
    long long lastPad_ = -1, lastGbHr_ = -999;
    long long qpcFreq_ = 1, qpcStart_ = 0;
    std::string lastErr_;
};

} // namespace rta::audio
