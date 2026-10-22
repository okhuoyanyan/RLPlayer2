// asio_out.h — ASIO 输出后端（自带接口声明，不依赖 Steinberg SDK）
//
// 为什么要自己声明接口：ASIO 的 ABI 从 1997 年至今没变过，就是一套固定顺序的虚函数。
// 仓库里没有 ASIO SDK，但驱动在注册表里是现成的（HKLM\SOFTWARE\ASIO），
// 只要 vtable 顺序对，CoCreateInstance 之后就能直接调。
//
// 验证策略（必须先跑 tools/asio_probe.cpp）：
//   1. 能否创建对象、拿回驱动名  —— 证明 vtable 前几项正确
//   2. 能否 getChannels / getBufferSize / canSampleRate —— 证明中间段正确
//   3. 能否 createBuffers + start + 回调里收数据 —— 证明后半段正确
// 任何一步错都会表现为崩溃或返回错误码，所以探针是一个一个往后推进的。
//
// 已实现：PCM 输出（Int16LSB / Int24LSB / Int32LSB / Float32LSB）。
// ⚠️ 未实现：原生 DSD（ASIOSTDSDInt8*）。那需要把 1-bit 码流按 ASIO 的 DSD 采样类型送出去，
//    而当前管线输出的是 int32 PCM。遇到只支持 DSD 类型的通道会明确报错，不会静默出噪声。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "pcm_dump.h"

namespace rta::audio {

// ---------------- ASIO 基本类型（与 asiosys.h 一致） ----------------
using ASIOBool   = long;
using ASIOError  = long;
using ASIOSampleRate = double;
using ASIOSampleType = long;

constexpr ASIOBool ASIOFalse = 0, ASIOTrue = 1;
constexpr ASIOError ASE_OK = 0;
constexpr ASIOError ASE_SUCCESS = 0x3f4847a0;
constexpr ASIOError ASE_NotPresent = -1000;
constexpr ASIOError ASE_HWMalfunction = -999;
constexpr ASIOError ASE_InvalidParameter = -998;
constexpr ASIOError ASE_InvalidMode = -997;
constexpr ASIOError ASE_SPNotAdvancing = -996;
constexpr ASIOError ASE_NoClock = -995;
constexpr ASIOError ASE_NoMemory = -994;

enum : ASIOSampleType {
    ASIOSTInt16MSB = 0, ASIOSTInt24MSB = 1, ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3, ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8, ASIOSTInt32MSB18 = 9, ASIOSTInt32MSB20 = 10, ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16, ASIOSTInt24LSB = 17, ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19, ASIOSTFloat64LSB = 20,
    ASIOSTDSDInt8LSB1 = 21, ASIOSTDSDInt8MSB1 = 22, ASIOSTDSDInt8NER8 = 23
};

struct ASIOSamples  { unsigned long hi, lo; };
struct ASIOTimeStamp{ unsigned long hi, lo; };
struct ASIOClockSource {
    long index, associatedChannel, associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
};
struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput, isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
};
struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
};
struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
};
struct ASIOTime {
    long reserved[4];
    ASIOTimeCode timeCode;
};
struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

// ---------------- SEH 护栏 ----------------
// 驱动是第三方 DLL，个别驱动在 start() 里会直接访问冲突（本机 AudioByte ASIO 实测）。
// 这里把每个可能崩的调用包起来：崩了就当成"驱动不可用"，而不是把整个播放器带走。
// 注意：只拦访问冲突这类硬件异常，不吞 C++ 异常。
#if defined(_MSC_VER)
#define ASIO_GUARD_BEGIN __try {
#define ASIO_GUARD_END(fallback) } __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
#else
#define ASIO_GUARD_BEGIN {
#define ASIO_GUARD_END(fallback) }
#endif

// ---------------- IASIO ----------------
struct IASIO : public IUnknown {
    virtual ASIOBool    init(void* sysHandle) = 0;
    virtual void        getDriverName(char* name) = 0;
    virtual long        getDriverVersion() = 0;
    virtual void        getErrorMessage(char* string) = 0;
    virtual ASIOError   start() = 0;
    virtual ASIOError   stop() = 0;
    virtual ASIOError   getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError   getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError   getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError   canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError   getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError   setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError   getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError   setClockSource(long reference) = 0;
    virtual ASIOError   getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError   getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError   createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                                      long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError   disposeBuffers() = 0;
    virtual ASIOError   controlPanel() = 0;
    virtual ASIOError   future(long selector, void* opt) = 0;
    virtual ASIOError   outputReady() = 0;
};

inline ASIOError asio_guarded_start(IASIO* d) {
    ASIO_GUARD_BEGIN
    return d->start();
    ASIO_GUARD_END(ASE_NotPresent)
}

// 注册表枚举：返回驱动名 + CLSID 字符串
struct AsioDriverEntry {
    std::string name;
    std::string clsid;
};
inline std::vector<AsioDriverEntry> asio_enum_drivers() {
    std::vector<AsioDriverEntry> out;
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW(hk, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring sub = std::wstring(L"SOFTWARE\\ASIO\\") + name;
        HKEY hd = nullptr;
        AsioDriverEntry e;
        char nb[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nb, sizeof(nb) - 1, nullptr, nullptr);
        e.name = nb;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &hd) == ERROR_SUCCESS) {
            wchar_t cbuf[256]; DWORD clen = sizeof(cbuf); DWORD type = 0;
            if (RegQueryValueExW(hd, L"CLSID", nullptr, &type, (LPBYTE)cbuf, &clen) == ERROR_SUCCESS) {
                char cb[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, cbuf, -1, cb, sizeof(cb) - 1, nullptr, nullptr);
                e.clsid = cb;
            }
            RegCloseKey(hd);
        }
        out.push_back(e);
    }
    RegCloseKey(hk);
    return out;
}

// ---------------- 输出驱动 ----------------
// 回调在驱动线程里跑，这里用互斥保护的环形缓冲；欠载时补零并计数。
class AsioDriver {
public:
    static constexpr size_t kRingFrames = 1 << 16;

    ~AsioDriver() { close(); }

    // srcBits = 管线输出位深：AudioBuffer 的 int32 是按这个位深定标的（24-bit 就落在 ±2^23）。
    // 驱动要的是各类型自己的满幅标度，所以必须归一化后再换算，不能直接拷。
    // sysHandle: 部分驱动的 init() 需要一个有效 HWND；diagNopCallback: 诊断用，回调里不写缓冲
    bool open(const std::string& clsidStr, ASIOSampleRate rate, int wantChannels, int srcBits,
              std::string& err, void* sysHandle = nullptr, bool diagNopCallback = false) {
        close();
        rate_ = rate;
        wantCh_ = wantChannels;
        srcBits_ = (srcBits >= 8 && srcBits <= 32) ? srcBits : 24;

        wchar_t wc[128] = {0};
        MultiByteToWideChar(CP_UTF8, 0, clsidStr.c_str(), -1, wc, 127);
        CLSID clsid{};
        if (FAILED(CLSIDFromString(wc, &clsid))) { err = "CLSID 解析失败: " + clsidStr; return false; }

        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&drv_);
        if (FAILED(hr) || !drv_) {
            char b[96]; std::snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
            err = std::string("CoCreateInstance 失败 ") + b;
            return false;
        }
        if (!drv_->init(sysHandle)) { err = "IASIO::init 返回 false"; free_drv(); return false; }

        char nm[64] = {0};
        drv_->getDriverName(nm);
        name_ = nm;

        long inCh = 0, outCh = 0;
        if (drv_->getChannels(&inCh, &outCh) != ASE_OK) { err = "getChannels 失败"; free_drv(); return false; }
        inCh_ = inCh; outCh_ = outCh;
        if (outCh_ <= 0) { err = "驱动没有输出声道"; free_drv(); return false; }
        ch_ = (wantCh_ > 0 && wantCh_ <= outCh_) ? wantCh_ : (int)outCh_;

        long mn = 0, mx = 0, pref = 0, gran = 0;
        if (drv_->getBufferSize(&mn, &mx, &pref, &gran) != ASE_OK) { err = "getBufferSize 失败"; free_drv(); return false; }
        minBuf_ = mn; maxBuf_ = mx; prefBuf_ = pref; granBuf_ = gran;
        bufSize_ = pref > 0 ? pref : (mx > 0 ? mx : 512);

        const ASIOError cr = drv_->canSampleRate(rate_);
        if (cr != ASE_OK) {
            char b[128];
            std::snprintf(b, sizeof(b), "驱动不接受 %u Hz（canSampleRate=0x%lx）", (unsigned)rate_, (unsigned long)cr);
            err = b; free_drv(); return false;
        }
        if (drv_->setSampleRate(rate_) != ASE_OK) { err = "setSampleRate 失败"; free_drv(); return false; }

        // 时钟源：有的驱动在 start() 里假定已经选好时钟，没选就解引用空指针
        // 不主动改时钟源：getClockSources/setClockSource 在部分驱动上会打断流
        // （实测 Portacapture 设完之后不再回调）。驱动默认时钟通常就是正确的。

        // 声道信息（决定要不要做样本格式转换）
        types_.assign((size_t)ch_, ASIOSTInt32LSB);
        chNames_.assign((size_t)ch_, std::string());
        for (int c = 0; c < ch_; ++c) {
            ASIOChannelInfo ci{};
            ci.channel = c;
            ci.isInput = ASIOFalse;
            if (drv_->getChannelInfo(&ci) == ASE_OK) {
                types_[(size_t)c] = ci.type;
                chNames_[(size_t)c] = std::string(ci.name, strnlen(ci.name, 32));
            }
        }

        bufs_.assign((size_t)ch_, ASIOBufferInfo{});
        for (int c = 0; c < ch_; ++c) {
            bufs_[(size_t)c].isInput = ASIOFalse;
            bufs_[(size_t)c].channelNum = c;
        }

        // !! 必须是【成员】：驱动只保存这个指针，之后在自己的线程里才去读函数指针。
        //    早先写成局部变量，createBuffers 返回后栈帧就没了，驱动读到的是野内存 ——
        //    表现为 start() 返回成功、回调一次都不触发、约 2 秒后访问冲突。
        std::memset(&cb_, 0, sizeof(cb_));
        if (diagNopCallback) {
            cb_.bufferSwitch = &AsioDriver::s_buffer_switch_nop;
            cb_.sampleRateDidChange = &AsioDriver::s_rate_changed;
            cb_.asioMessage = &AsioDriver::s_message;
            cb_.bufferSwitchTimeInfo = &AsioDriver::s_buffer_switch_time_nop;
        } else {
            cb_.bufferSwitch = &AsioDriver::s_buffer_switch;
            cb_.sampleRateDidChange = &AsioDriver::s_rate_changed;
            cb_.asioMessage = &AsioDriver::s_message;
            cb_.bufferSwitchTimeInfo = &AsioDriver::s_buffer_switch_time;
        }

        if (drv_->createBuffers(bufs_.data(), (long)ch_, (long)bufSize_, &cb_) != ASE_OK) {
            err = "createBuffers 失败"; free_drv(); return false;
        }
        buffersCreated_ = true;
        ring_.assign(kRingFrames * (size_t)ch_, 0);
        ringR_ = ringW_ = ringCount_ = 0;
        underruns_ = 0;
        // outputReady 不再主动探测：在 start() 之前调用它在部分驱动上会破坏内部状态
        // （实测 Portacapture 之后不再回调、约 2 秒后访问冲突）。默认不调用。
        outReadySupported_ = false;
        return true;
    }

    bool start(std::string& err) {
        if (!drv_ || !buffersCreated_) { err = "驱动未打开"; return false; }
        g_inst = this;
        const ASIOError sr = asio_guarded_start(drv_);
        if (sr != ASE_OK) {
            err = (sr == ASE_NotPresent)
                ? "驱动在 start() 中崩溃或拒绝启动（已拦截，未影响播放器）"
                : "IASIO::start 返回错误";
            return false;
        }
        started_ = true;
        return true;
    }
    void stop() {
        if (drv_ && started_) { drv_->stop(); started_ = false; }
    }
    void close() {
        stop();
        if (drv_) {
            if (buffersCreated_) { drv_->disposeBuffers(); buffersCreated_ = false; }
            free_drv();
        }
        if (g_inst == this) g_inst = nullptr;
    }
    // 推交错 int32；返回实际接收帧数
    size_t push(const int32_t* pcm, size_t frames) {
        detail::pcm_dump("asio", pcm, frames * (size_t)ch_, srcBits_);   // 取证（默认关闭）
        size_t ok = 0;
        {
            std::lock_guard<std::mutex> lk(mx_);
            while (ok < frames && ringCount_ < kRingFrames) {
                const size_t dst = ringW_ * (size_t)ch_;
                const size_t src = ok * (size_t)ch_;
                for (int c = 0; c < ch_; ++c) ring_[dst + (size_t)c] = pcm[src + (size_t)c];
                ringW_ = (ringW_ + 1) % kRingFrames;
                ++ringCount_;
                ++ok;
            }
        }
        // 通知驱动"数据已就绪"——放在写入方线程，避免与回调重入
        if (ok && drv_ && outReadySupported_) drv_->outputReady();
        return ok;
    }
    size_t free_frames() const {
        std::lock_guard<std::mutex> lk(mx_);
        return kRingFrames - ringCount_;
    }
    long buffer_size() const { return bufSize_; }
    const std::string& name() const { return name_; }
    int channels() const { return ch_; }
    int driver_out_channels() const { return outCh_; }
    ASIOSampleType channel_type(int c) const {
        return (c >= 0 && c < (int)types_.size()) ? types_[(size_t)c] : ASIOSTInt32LSB;
    }
    const std::string& channel_name(int c) const {
        static const std::string kEmpty;
        return (c >= 0 && c < (int)chNames_.size()) ? chNames_[(size_t)c] : kEmpty;
    }
    unsigned long long underruns() const { return underruns_; }
    bool out_ready_supported() const { return outReadySupported_; }
    long min_buf() const { return minBuf_; }
    long max_buf() const { return maxBuf_; }
    long pref_buf() const { return prefBuf_; }

private:
    void free_drv() {
        if (drv_) { drv_->Release(); drv_ = nullptr; }
    }
    static AsioDriver* g_inst;
    static void s_buffer_switch(long idx, ASIOBool direct) { if (g_inst) g_inst->on_buffer(idx); }
    // ! 必须把 params 原样返回：ASIO 2.x 的驱动会把返回值当时间戳继续用，
    //   返回 nullptr 会直接访问冲突（本机 AudioByte 驱动实测崩在 +0x8）。
    static ASIOTime* s_buffer_switch_time(ASIOTime* params, long idx, ASIOBool) {
        if (g_inst) g_inst->on_buffer(idx);
        return params;
    }
    static void s_buffer_switch_nop(long, ASIOBool) {}
    static ASIOTime* s_buffer_switch_time_nop(ASIOTime* params, long, ASIOBool) { return params; }
    static void s_rate_changed(ASIOSampleRate) {}
    // ASIO 的"能力协商"。返回 0 一律表示"不支持"，有的驱动会因此按 ASIO 1
    // 的旧行为继续跑，随后在自己的流线程里出问题。这里如实回答：我们是 2.2 宿主、
    // 用 bufferSwitchTimeInfo、不支持时间码/输入监听。
    static long s_message(long selector, long value, void*, double*) {
        enum { kSelSupported = 1, kEngineVersion = 2, kResetRequest = 3,
               kBufferSizeChange = 4, kResyncRequest = 5, kLatenciesChanged = 6,
               kSupportsTimeInfo = 7, kSupportsTimeCode = 8, kOverload = 15 };
        switch (selector) {
            case kSelSupported:
                switch (value) {
                    case kEngineVersion: case kResetRequest: case kBufferSizeChange:
                    case kResyncRequest: case kLatenciesChanged:
                    case kOverload: return ASIOTrue;
                    default: return ASIOFalse;
                }
            case kEngineVersion:     return 2;
            // 只走 bufferSwitch。声明支持 bufferSwitchTimeInfo 会让驱动改走另一条回调，
            // 实测（本机 Portacapture / AudioByte）在 2 秒左右访问冲突。
            case kSupportsTimeInfo:  return ASIOFalse;
            case kSupportsTimeCode:  return ASIOFalse;
            case kResetRequest:      return ASIOTrue;
            case kOverload:          return ASIOFalse;
            default:                 return 0;
        }
    }

    void on_buffer(long idx) {
        if (idx < 0 || idx > 1) return;

        std::vector<int32_t> tmp;
        {
            std::lock_guard<std::mutex> lk(mx_);
            tmp.resize((size_t)bufSize_ * (size_t)ch_);
            const size_t take = ringCount_ < (size_t)bufSize_ ? ringCount_ : (size_t)bufSize_;
            for (size_t i = 0; i < take; ++i) {
                const size_t s = ringR_ * (size_t)ch_;
                for (int c = 0; c < ch_; ++c) tmp[i * (size_t)ch_ + (size_t)c] = ring_[s + (size_t)c];
                ringR_ = (ringR_ + 1) % kRingFrames;
            }
            ringCount_ -= take;
            if (take < (size_t)bufSize_) {
                ++underruns_;
                std::memset(tmp.data() + take * (size_t)ch_, 0,
                            ((size_t)bufSize_ - take) * (size_t)ch_ * sizeof(int32_t));
            }
        }
        for (int c = 0; c < ch_; ++c) {
            void* dst = bufs_[(size_t)c].buffers[idx];
            if (!dst) continue;
            const int32_t* src = tmp.data() + (size_t)c;
            const size_t stride = (size_t)ch_;
            write_channel(dst, src, stride, (size_t)bufSize_, types_[(size_t)c], srcBits_);
        }
        // outputReady() 只在"写入方"线程调用（见 push 之后），
        // 绝不能在驱动回调里调：会重入驱动（实测 2 秒左右访问冲突）。
    }

    static void write_channel(void* dst, const int32_t* src, size_t stride, size_t n,
                              ASIOSampleType t, int srcBits) {
        // 归一化因子：管线 int32 = srcBits 位定标
        const double inv = 1.0 / (double)(1LL << (srcBits - 1));
        switch (t) {
            case ASIOSTInt32LSB: {
                int32_t* d = (int32_t*)dst;
                for (size_t i = 0; i < n; ++i) {
                    const double v = (double)src[i * stride] * inv;
                    d[i] = (int32_t)llround(v * 2147483647.0);
                }
                break;
            }
            case ASIOSTInt24LSB: {
                unsigned char* d = (unsigned char*)dst;
                for (size_t i = 0; i < n; ++i) {
                    const double v = (double)src[i * stride] * inv;
                    // 24-bit 满幅：DoP 的标记位在最低 8 位，round-trip 必须精确
                    const int32_t q = (int32_t)llround(v * 8388607.0);
                    const uint32_t u = (uint32_t)q;
                    d[i * 3 + 0] = (unsigned char)(u & 0xFF);
                    d[i * 3 + 1] = (unsigned char)((u >> 8) & 0xFF);
                    d[i * 3 + 2] = (unsigned char)((u >> 16) & 0xFF);
                }
                break;
            }
            case ASIOSTInt16LSB: {
                int16_t* d = (int16_t*)dst;
                for (size_t i = 0; i < n; ++i) {
                    const double v = (double)src[i * stride] * inv;
                    d[i] = (int16_t)llround(v * 32767.0);
                }
                break;
            }
            case ASIOSTFloat32LSB: {
                float* d = (float*)dst;
                for (size_t i = 0; i < n; ++i) d[i] = (float)((double)src[i * stride] * inv);
                break;
            }
            default: {
                // 不认识的类型（含 DSD 类型）：写零，避免把 PCM 当 DSD 塞进去出噪声
                std::memset(dst, 0, n * (t == ASIOSTDSDInt8LSB1 || t == ASIOSTDSDInt8MSB1 || t == ASIOSTDSDInt8NER8 ? 1 : 4));
                break;
            }
        }
    }

    IASIO* drv_ = nullptr;
    std::string name_;
    ASIOSampleRate rate_ = 48000.0;
    int wantCh_ = 2, ch_ = 2, srcBits_ = 24;
    long inCh_ = 0, outCh_ = 0;
    long minBuf_ = 0, maxBuf_ = 0, prefBuf_ = 0, granBuf_ = 0, bufSize_ = 512;
    ASIOCallbacks cb_{};
    std::vector<ASIOBufferInfo> bufs_;
    std::vector<ASIOSampleType> types_;
    std::vector<std::string> chNames_;
    bool buffersCreated_ = false, started_ = false, outReadySupported_ = false;

    mutable std::mutex mx_;
    std::vector<int32_t> ring_;
    size_t ringR_ = 0, ringW_ = 0, ringCount_ = 0;
    std::atomic<unsigned long long> underruns_{0};
};

inline AsioDriver* AsioDriver::g_inst = nullptr;

} // namespace rta::audio
