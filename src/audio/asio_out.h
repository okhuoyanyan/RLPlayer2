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
// 已实现：原生 DSD（ASIOSTDSDInt8LSB1 / MSB1，走 ASIO 2.3 的 IoFormat 切换）。
//   规则（ASIO SDK 2.3 asio.h「DSD operation and buffer layout」）：1 字节 = 8 个 1-bit 样本，
//   bufferSize 的单位从「帧」变成「字节」；采样率要设成 DSD 位率（DSD64 = 2822400），
//   不是 DoP 的承载率（176400）。驱动不接受时返回带驱动名/类型码/ASE 码的明确错误，
//   由调用方决定是否回落 DoP —— 这里绝不静默降级。
//
// ⚠️ 采样类型数值以 SDK 为准：LSB1=32 / MSB1=33 / NER8=40（本文件旧版写的 21/22/23 是错的，
//    那是凭空猜的，驱动永远不会报这三个值）。
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
#include <cctype>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <functional>
#include "dsp/dop.h"
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
    ASIOSTInt32LSB16 = 24, ASIOSTInt32LSB18 = 25, ASIOSTInt32LSB20 = 26, ASIOSTInt32LSB24 = 27,
    // DSD 三种（asio.h 原文：LSB1 = 1 bit data, 8 samples per byte, first sample in LSB；
    // MSB1 同但 first sample in MSB；NER8 = 8 bit data, 1 sample per byte，不是 1-bit 打包）
    ASIOSTDSDInt8LSB1 = 32, ASIOSTDSDInt8MSB1 = 33, ASIOSTDSDInt8NER8 = 40
};

// ---------------- ASIO 2.3 IoFormat（原生 DSD 的总开关） ----------------
// 取值来自 asio.h：
//   kAsioSetIoFormat = 0x23111961 / kAsioGetIoFormat = 0x23111983 / kAsioCanDoIoFormat = 0x23112004
//   ASIOFuture 的成功返回是 ASE_SUCCESS(0x3f4847a0) —— SDK 原文写明「ASE_OK is *not* sufficient!」
//   驱动不支持时会把 ASIOIoFormat::formatType 改成 -1（kASIOFormatInvalid）
//   SDK 注释：格式切换必须发生在进入 "prepared" 状态之前，也就是 createBuffers 之前
constexpr long kAsioSetIoFormat   = 0x23111961L;
constexpr long kAsioGetIoFormat   = 0x23111983L;
constexpr long kAsioCanDoIoFormat = 0x23112004L;
constexpr long kASIOFormatInvalid = -1;
constexpr long kASIOPCMFormat     = 0;
constexpr long kASIODSDFormat     = 1;

struct ASIOIoFormat {
    long formatType;                 // ASIOIoFormatType = long int
    char future[512 - sizeof(long)]; // SDK 定义为 512 字节
};

// 驱动报的采样类型是不是我们认识的 1-bit DSD 打包类型
inline bool asio_is_dsd_1bit(ASIOSampleType t) {
    return t == ASIOSTDSDInt8LSB1 || t == ASIOSTDSDInt8MSB1;
}
inline const char* asio_sample_type_name(ASIOSampleType t) {
    switch (t) {
        case ASIOSTInt16MSB: return "Int16MSB";
        case ASIOSTInt24MSB: return "Int24MSB";
        case ASIOSTInt32MSB: return "Int32MSB";
        case ASIOSTFloat32MSB: return "Float32MSB";
        case ASIOSTFloat64MSB: return "Float64MSB";
        case ASIOSTInt32MSB16: return "Int32MSB16";
        case ASIOSTInt32MSB18: return "Int32MSB18";
        case ASIOSTInt32MSB20: return "Int32MSB20";
        case ASIOSTInt32MSB24: return "Int32MSB24";
        case ASIOSTInt16LSB: return "Int16LSB";
        case ASIOSTInt24LSB: return "Int24LSB";
        case ASIOSTInt32LSB: return "Int32LSB";
        case ASIOSTFloat32LSB: return "Float32LSB";
        case ASIOSTFloat64LSB: return "Float64LSB";
        case ASIOSTInt32LSB16: return "Int32LSB16";
        case ASIOSTInt32LSB18: return "Int32LSB18";
        case ASIOSTInt32LSB20: return "Int32LSB20";
        case ASIOSTInt32LSB24: return "Int32LSB24";
        case ASIOSTDSDInt8LSB1: return "DSDInt8LSB1";
        case ASIOSTDSDInt8MSB1: return "DSDInt8MSB1";
        case ASIOSTDSDInt8NER8: return "DSDInt8NER8";
        default: return "?";
    }
}
// ★ 每次回调能写的字节数：ASIO 的 bufferSize 单位是"样本"，DSD 的样本只有 1 bit，
//   8 个样本才占 1 字节 ⟹ 字节数 = bufferSize / 8。
//   本机 AudioByte 实测：DSD 模式报 32768，而双缓冲相距只有 4096 字节（VirtualQuery 与逐级试写都证实），
//   按 32768 字节写会立刻访问冲突 —— 这就是 StackOverflow 52774883 里 DSD "heap corruption" 的同一个坑。
inline long asio_dsd_bytes_per_buffer(long bufferSize) {
    const long b = bufferSize / 8;
    return b > 0 ? b : 1;
}

// 目标缓冲（ASIO 的 bufferSize，单位=样本；DSD 时 1 样本=1 bit）。
// 为什么需要：驱动报的 preferredSize 只保证「默认能用」，不代表「高码率不欠载」。
//   xDuoo USB Audio Device 在 DSD1024（45.1584 MHz）下 pref=8192 bit = 1024 字节/声道
//   = 0.18 ms，实测 2 秒欠载 94 次；改用驱动上限 131072 bit = 2.9 ms 后欠载 0。
// 规则：以用户缓冲毫秒为目标，不低于 pref、不超过 max，按 granularity 向上对齐。
inline long asio_pick_buffer_size(long mn, long mx, long pref, long gran,
                                  double rate, double ms) {
    long target = pref > 0 ? pref : (mx > 0 ? mx : 512);
    if (ms > 0.0 && rate > 0.0) {
        const double want = ms / 1000.0 * rate;
        if (want > (double)target) target = (long)std::llround(want);
    }
    if (mx > 0 && target > mx) target = mx;
    if (mn > 0 && target < mn) target = mn;
    if (gran > 1) target = ((target + gran - 1) / gran) * gran;
    if (mx > 0 && target > mx) target = mx;
    if (target < 8) target = 8;
    return target;
}

// A zero byte is full negative DC in packed 1-bit DSD, not silence.
inline uint8_t asio_silence_value(ASIOSampleType type, size_t sample = 0) {
    if (asio_is_dsd_1bit(type)) return 0x69; // Four ones / four zeros.
    if (type == ASIOSTDSDInt8NER8) return (uint8_t)(sample & 1); // One 0/1 sample per byte.
    return 0;
}

inline void asio_write_silence(void* destination, size_t samples, ASIOSampleType type) {
    if (asio_is_dsd_1bit(type)) {
        std::memset(destination, asio_silence_value(type), samples);
    } else if (type == ASIOSTDSDInt8NER8) {
        auto* bytes = static_cast<uint8_t*>(destination);
        for (size_t i = 0; i < samples; ++i) bytes[i] = asio_silence_value(type, i);
    } else {
        size_t width = 4; // Preserve the existing unknown-type fallback.
        if (type == ASIOSTInt16MSB || type == ASIOSTInt16LSB) width = 2;
        else if (type == ASIOSTInt24MSB || type == ASIOSTInt24LSB) width = 3;
        else if (type == ASIOSTFloat64MSB || type == ASIOSTFloat64LSB) width = 8;
        std::memset(destination, 0, samples * width);
    }
}

// 位序转换：内部（DsdStream / 调制器输出）一律 MSB-first 打包，即字节 bit7 = 最早样本。
// 驱动要 LSB1（最早样本在 bit0）时逐字节反序；MSB1 直接拷。
inline uint8_t asio_reverse_bits8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

// ---------------- DSD 纯函数（离线可单测；tests/test_asio_native_dsd.cpp） ----------------
// 这些函数不碰驱动，只做打包/电平估计，所以可以在没有声卡的机器上验证判据本身对不对。
inline bool asio_dsd_bit(const uint8_t* plane, size_t n) {   // MSB-first：n 号位在 bit(7-(n&7))
    return ((plane[n >> 3] >> (7 - (n & 7))) & 1u) != 0;
}

// DoP 打包：从 1-bit 平面直接打 24-bit 承载样本（标记 0x05/0xFA 逐样本交替，每样本装 16 bit）。
// 不先展开成 double —— DSD256 整曲那样会吃掉几百 MB。
inline void asio_dop_pack_planes(const std::vector<const uint8_t*>& planes, size_t bitStart,
                                 size_t bits, std::vector<int32_t>& out, std::vector<char>& phaseA) {
    const size_t ch = planes.size();
    const size_t carrier = bits / 16;
    if (phaseA.size() != ch) phaseA.assign(ch, 1);
    out.assign(carrier * ch, 0);
    for (size_t c = 0; c < ch; ++c) {
        bool a = phaseA[c] != 0;
        for (size_t n = 0; n < carrier; ++n) {
            uint32_t bits16 = 0;
            for (int b = 0; b < 16; ++b)
                if (asio_dsd_bit(planes[c], bitStart + n * 16 + (size_t)b)) bits16 |= (1u << (15 - b));
            const uint32_t marker = (a ? 0x05u : 0xFAu) << 16;   // DOP_MARKER_A / DOP_MARKER_B
            int32_t v = (int32_t)(marker | bits16);
            if (v & 0x800000) v |= (int32_t)0xFF000000;          // 24-bit 符号扩展
            out[n * ch + c] = v;
            a = !a;
        }
        phaseA[c] = a ? 1 : 0;
    }
}

// DSD 电平估计（横幅/铁律 1 判据用）。
// 原理：1-bit 流的局部"1 的密度"就是瞬时电平，密度 0.5 = 0。先减掉整段流的直流密度，
// 再取短窗峰值 —— 否则全零流（恒定电平）会被误判成 0 dBFS。
// ⚠️ 这是【上界】：短窗（256 bit）里包含整形噪声，实测对 -60 dBFS 的 1 kHz 音给出约 -42 dBFS
//    （偏高约 18 dB）。偏高是安全的方向（只会把可疑素材拦下来，不会放过可闻素材）。
struct AsioDsdLevel { double peakDbfs = -999.0; double dcDbfs = -999.0; bool allZero = true; };
inline AsioDsdLevel asio_dsd_level_planes(const std::vector<const uint8_t*>& planes, size_t bytesPerPlane) {
    AsioDsdLevel r;
    double peak = 0.0, dc = 0.0;
    const size_t W = 32;   // 32 字节 = 256 个 1-bit 样本（DSD64 下 90 µs）
    for (const uint8_t* p : planes) {
        if (!p || bytesPerPlane == 0) continue;
        unsigned long long ones = 0;
        for (size_t i = 0; i < bytesPerPlane; ++i) ones += (unsigned)__builtin_popcount(p[i]);
        if (ones) r.allZero = false;
        const double mean = (double)ones / (8.0 * (double)bytesPerPlane);
        const double thisDc = std::fabs(2.0 * (mean - 0.5));
        if (thisDc > dc) dc = thisDc;
        for (size_t i = 0; i + W <= bytesPerPlane; i += W) {
            unsigned long long o = 0;
            for (size_t k = 0; k < W; ++k) o += (unsigned)__builtin_popcount(p[i + k]);
            const double d = (double)o / (double)(W * 8);
            const double dev = std::fabs(2.0 * (d - mean));
            if (dev > peak) peak = dev;
        }
    }
    r.peakDbfs = 20.0 * std::log10(peak + 1e-12);
    r.dcDbfs = 20.0 * std::log10(dc + 1e-12);
    return r;
}

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
    // ! 真实签名是 void* ASIOFuture(long, void*)，不是 ASIOError（见 asio.h）。
    //   返回类型写错在 x64 上虽然"能跑"（返回值都在 RAX），但语义上会丢掉指针高位，
    //   而且 ASE_SUCCESS 只对 ASIOFuture 有效 —— 这里按 SDK 原样声明。
    virtual void*       future(long selector, void* opt) = 0;
    virtual ASIOError   outputReady() = 0;
};

inline ASIOError asio_guarded_start(IASIO* d) {
    ASIO_GUARD_BEGIN
    return d->start();
    ASIO_GUARD_END(ASE_NotPresent)
}
// 实测：本机 AudioByte 在【原生 DSD 会话收尾】时会在 stop()/disposeBuffers() 里访问冲突
// （与 StackOverflow 52774883 报告的 DSD disposeBuffers 堆损坏同型）。收尾也必须挡，
// 否则播完就崩，比播不出来更糟。
inline ASIOError asio_guarded_stop(IASIO* d) {
    ASIO_GUARD_BEGIN
    return d->stop();
    ASIO_GUARD_END(ASE_NotPresent)
}
inline ASIOError asio_guarded_dispose(IASIO* d) {
    ASIO_GUARD_BEGIN
    return d->disposeBuffers();
    ASIO_GUARD_END(ASE_NotPresent)
}
// 同上护栏。ASIOFuture 会走各驱动的私有分支（DSD 切换就是其中一条），崩过一次就记 0。
inline void* asio_guarded_future(IASIO* d, long selector, void* params) {
    ASIO_GUARD_BEGIN
    return d->future(selector, params);
    ASIO_GUARD_END(nullptr)
}
inline ASIOBool asio_guarded_init(IASIO* d, void* sysHandle) {
    ASIO_GUARD_BEGIN
    return d->init(sysHandle);
    ASIO_GUARD_END(ASIOFalse)
}
inline ASIOError asio_guarded_create_buffers(IASIO* d, ASIOBufferInfo* infos, long nch,
                                             long bufSize, ASIOCallbacks* cb) {
    ASIO_GUARD_BEGIN
    return d->createBuffers(infos, nch, bufSize, cb);
    ASIO_GUARD_END(ASE_NotPresent)
}
inline ASIOError asio_guarded_channel_info(IASIO* d, ASIOChannelInfo* ci) {
    ASIO_GUARD_BEGIN
    return d->getChannelInfo(ci);
    ASIO_GUARD_END(ASE_NotPresent)
}

// 注册表枚举：返回驱动名 + CLSID 字符串
//
// ⚠️ 2026-10-26（用户 261025-02 #4）：**必须两个注册表视图都枚举**，旧代码只读了默认视图。
//   ASIO 驱动把自己的键同时写进 HKLM\SOFTWARE\ASIO（64 位视图）与
//   HKLM\SOFTWARE\WOW6432Node\ASIO（32 位视图）。64 位宿主进程用默认 KEY_READ 枚举时
//   只看得到 64 位视图；只在 32 位视图注册的驱动（32 位驱动 / 32 位安装器）会被漏掉 ——
//   那种机器上列表就是空的，界面只能显示兜底项「（第一个可用驱动）」。
//   去重键 = CLSID（同名同 CLSID 只保留 64 位视图那一份；没有 CLSID 的键按名字去重）。
//   x86 = true 表示只在 32 位视图里注册：64 位宿主 CoCreateInstance 会失败，
//   界面可以据此把它标灰（而不是让用户选了一个永远开不起来的驱动）。
struct AsioDriverEntry {
    std::string name;
    std::string clsid;
    bool x86 = false;
};

inline bool asio_same_str_ci(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

// 枚举单个视图（viewFlag = KEY_WOW64_64KEY / KEY_WOW64_32KEY），结果追加进 out（已去重）。
inline void asio_enum_one_view(DWORD viewFlag, bool x86, std::vector<AsioDriverEntry>& out) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                      KEY_READ | viewFlag, &hk) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW(hk, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        AsioDriverEntry e;
        e.x86 = x86;
        char nb[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nb, sizeof(nb) - 1, nullptr, nullptr);
        e.name = nb;
        const std::wstring sub = std::wstring(L"SOFTWARE\\ASIO\\") + name;
        HKEY hd = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ | viewFlag, &hd) == ERROR_SUCCESS) {
            wchar_t cbuf[256] = {0}; DWORD clen = sizeof(cbuf) - sizeof(wchar_t); DWORD type = 0;
            if (RegQueryValueExW(hd, L"CLSID", nullptr, &type, (LPBYTE)cbuf, &clen) == ERROR_SUCCESS) {
                char cb[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, cbuf, -1, cb, sizeof(cb) - 1, nullptr, nullptr);
                e.clsid = cb;
            }
            RegCloseKey(hd);
        }
        bool dup = false;
        for (const auto& x : out) {
            if (!e.clsid.empty() && !x.clsid.empty()) {
                if (asio_same_str_ci(x.clsid, e.clsid)) { dup = true; break; }
            } else if (e.clsid.empty() && x.clsid.empty() && asio_same_str_ci(x.name, e.name)) {
                dup = true; break;
            }
        }
        if (!dup) out.push_back(e);
    }
    RegCloseKey(hk);
}

inline std::vector<AsioDriverEntry> asio_enum_drivers() {
    std::vector<AsioDriverEntry> out;
    asio_enum_one_view(KEY_WOW64_64KEY, false, out);   // 主视图（64 位宿主可直接 CoCreate）
    asio_enum_one_view(KEY_WOW64_32KEY, true, out);    // WOW6432Node（32 位驱动）
    return out;
}

// ---------------- 输出驱动 ----------------
// 回调在驱动线程里跑，这里用互斥保护的环形缓冲；欠载按格式补静音并计数。
class AsioDriver {
public:
    static constexpr size_t kRingFrames = 1 << 16;

    ~AsioDriver() { close(); }

    // srcBits = 管线输出位深：AudioBuffer 的 int32 是按这个位深定标的（24-bit 就落在 ±2^23）。
    // 驱动采用各类型的 2^(bits-1) 标度；同位深 unity 必须逐位复制，展宽只补零。
    // sysHandle: 部分驱动的 init() 需要一个有效 HWND；diagNopCallback: 诊断用，回调里不写缓冲
    bool open(const std::string& clsidStr, ASIOSampleRate rate, int wantChannels, int srcBits,
              std::string& err, void* sysHandle = nullptr, bool diagNopCallback = false,
              double bufferMs = 100.0) {
        close();
        rate_ = rate;
        wantCh_ = wantChannels;
        bufferMs_ = bufferMs > 0.0 ? bufferMs : 100.0;
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
        bufSize_ = asio_pick_buffer_size(mn, mx, pref, gran, rate_, bufferMs_);

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

        {
            std::string attempts;
            const long used = create_buffers_resilient(bufSize_, attempts);
            if (used == 0) {
                err = "createBuffers 失败（尝试 " + attempts + "）";
                free_drv();
                return false;
            }
        }
        buffersCreated_ = true;
        // 环形缓冲必须装得下至少 4 个驱动周期：高码率下 100 ms 的 ASIO 缓冲可能远大于
        // 旧的固定 65536 帧，否则「回调一次要 262144 字节、环里只有 65536」会永远喂不满。
        ringFrames_ = std::max<size_t>(kRingFrames, (size_t)bufSize_ * 4);
        ring_.assign(ringFrames_ * (size_t)ch_, 0);
        ringR_ = ringW_ = ringCount_ = 0;
        underruns_ = 0;
        reset_completion_state();
        // outputReady 不再主动探测：在 start() 之前调用它在部分驱动上会破坏内部状态
        // （实测 Portacapture 之后不再回调、约 2 秒后访问冲突）。默认不调用。
        outReadySupported_ = false;
        return true;
    }

    // ================= 原生 DSD（ASIO 2.3 IoFormat = kASIODSDFormat） =================
    // 现场记录：每个可能失败的调用都留原始返回码，错误信息里逐项写出来（不许只说"失败"）。
    struct AsioDsdInfo {
        std::string name;
        long canDoIo = 0,   canDoTypeAfter = 0;   // ASIOFuture(kAsioCanDoIoFormat)
        long setIo = 0,     setIoTypeAfter = 0;   // ASIOFuture(kAsioSetIoFormat)
        long getIo = 0,     getIoType = 0;        // ASIOFuture(kAsioGetIoFormat)
        long canRate = 0,   setRate = 0;          // canSampleRate / setSampleRate
        double rateReadback = 0.0;                // getSampleRate 读回
        ASIOSampleType type = -1;                 // getChannelInfo 报的采样类型
        long bufferBytes = 0, bufferMin = 0, bufferMax = 0;  // ASIO 报的 bufferSize（单位=DSD 样本/bit）
        long writeBytes = 0;                      // 实际每次回调能写的字节 = bufferSize/8
        long createBuffers = ASE_NotPresent;
        std::string bufferAttempts;   // createBuffers 的阶梯尝试与原始 ASE 码
        int channels = 0;
        double dsdRate = 0.0;
    };

    // 打开原生 DSD 流。顺序按 SDK 要求：先切 IoFormat，再设采样率，最后 createBuffers。
    // 失败时 err 里带上：驱动名 / 请求的 DSD 类型 / 原始 ASE 码。
    bool open_dsd(const std::string& clsidStr, ASIOSampleRate dsdRate, int wantChannels,
                  std::string& err, void* sysHandle = nullptr, double bufferMs = 100.0) {
        close();
        dsd_ = AsioDsdInfo{};
        dsd_.dsdRate = (double)dsdRate;
        rate_ = dsdRate;
        wantCh_ = wantChannels;
        dsdRate_ = dsdRate;
        bufferMs_ = bufferMs > 0.0 ? bufferMs : 100.0;

        wchar_t wc[128] = {0};
        MultiByteToWideChar(CP_UTF8, 0, clsidStr.c_str(), -1, wc, 127);
        CLSID clsid{};
        if (FAILED(CLSIDFromString(wc, &clsid))) { err = "CLSID 解析失败: " + clsidStr; return false; }
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&drv_);
        if (FAILED(hr) || !drv_) {
            char b[96]; std::snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
            err = std::string("CoCreateInstance 失败 ") + b;
            drv_ = nullptr;
            return false;
        }
        if (!asio_guarded_init(drv_, sysHandle)) {
            err = "IASIO::init 返回 false（原生 DSD 路径）"; free_drv(); return false;
        }
        char nm[64] = {0};
        drv_->getDriverName(nm);
        name_ = nm;
        dsd_.name = name_;

        char eb[512];
        // 1) 问 + 切 DSD 格式（必须在 createBuffers 之前）
        ASIOIoFormat f{};
        f.formatType = kASIODSDFormat;
        dsd_.canDoIo = (long)(intptr_t)asio_guarded_future(drv_, kAsioCanDoIoFormat, &f);
        dsd_.canDoTypeAfter = f.formatType;
        f.formatType = kASIODSDFormat;
        dsd_.setIo = (long)(intptr_t)asio_guarded_future(drv_, kAsioSetIoFormat, &f);
        dsd_.setIoTypeAfter = f.formatType;
        ASIOIoFormat got{};
        got.formatType = kASIOFormatInvalid;
        dsd_.getIo = (long)(intptr_t)asio_guarded_future(drv_, kAsioGetIoFormat, &got);
        dsd_.getIoType = got.formatType;

        if (dsd_.setIo != ASE_SUCCESS || f.formatType != kASIODSDFormat) {
            std::snprintf(eb, sizeof(eb),
                "驱动「%s」不接受原生 DSD：kAsioSetIoFormat 返回 0x%08lx（成功应为 ASE_SUCCESS=0x%08lx），"
                "ASIOIoFormat.formatType=%ld（-1 = 驱动把请求改成了 Invalid）；"
                "kAsioCanDoIoFormat=0x%08lx（formatType=%ld），kAsioGetIoFormat=0x%08lx（formatType=%ld）",
                name_.c_str(), (unsigned long)dsd_.setIo, (unsigned long)ASE_SUCCESS,
                f.formatType, (unsigned long)dsd_.canDoIo, dsd_.canDoTypeAfter,
                (unsigned long)dsd_.getIo, dsd_.getIoType);
            err = eb;
            restore_pcm_format();
            free_drv();
            return false;
        }

        // 2) 声道 / 缓冲（DSD 模式下 bufferSize 的单位是字节 = 8 个 1-bit 样本）
        long inCh = 0, outCh = 0;
        if (drv_->getChannels(&inCh, &outCh) != ASE_OK) {
            err = "驱动「" + name_ + "」getChannels 失败（原生 DSD）"; restore_pcm_format(); free_drv(); return false;
        }
        inCh_ = inCh; outCh_ = outCh;
        if (outCh_ <= 0) {
            err = "驱动「" + name_ + "」在 DSD 模式下没有输出声道"; restore_pcm_format(); free_drv(); return false;
        }
        ch_ = (wantCh_ > 0 && wantCh_ <= outCh_) ? wantCh_ : (int)outCh_;
        dsd_.channels = ch_;

        long mn = 0, mx = 0, pref = 0, gran = 0;
        if (drv_->getBufferSize(&mn, &mx, &pref, &gran) != ASE_OK) {
            err = "驱动「" + name_ + "」getBufferSize 失败（原生 DSD）"; restore_pcm_format(); free_drv(); return false;
        }
        minBuf_ = mn; maxBuf_ = mx; prefBuf_ = pref; granBuf_ = gran;
        // DSD 的 bufferSize 单位是 1-bit 样本：按毫秒目标选，默认 100 ms 在 DSD1024 下会被
        // 驱动上限 131072 bit（2.9 ms）截断；这已经比 pref=8192 bit（0.18 ms）稳得多。
        bufSize_ = asio_pick_buffer_size(mn, mx, pref, gran, dsdRate_, bufferMs_);
        dsd_.bufferBytes = bufSize_; dsd_.bufferMin = mn; dsd_.bufferMax = mx;
        // ★ ASIO 的 bufferSize 单位是"样本"；DSD 的 1 个样本只有 1 bit，8 个样本才占 1 字节。
        //   所以每次回调能写的字节数 = bufferSize / 8。
        //   本机 AudioByte 实测：DSD 模式 getBufferSize 报 32768，而双缓冲相距只有 4096 字节
        //   （VirtualQuery + 逐级试写都证实）；按 32768 字节写会立刻 AV —— 这正是
        //   StackOverflow 52774883 里"DSD 播一半 heap corruption"的那个坑。
        dsdBufBytes_ = asio_dsd_bytes_per_buffer(bufSize_);
        dsd_.writeBytes = dsdBufBytes_;

        // 3) 采样率 = DSD 位率（DSD64 2822400），不是 DoP 承载率
        dsd_.canRate = drv_->canSampleRate(dsdRate_);
        if (dsd_.canRate != ASE_OK) {
            std::snprintf(eb, sizeof(eb),
                "驱动「%s」在 DSD 模式下不接受采样率 %.0f Hz（canSampleRate=0x%08lx）；"
                "请求类型 ASIOSTDSDInt8LSB1(%d)/MSB1(%d)，此时 formatType=%ld",
                name_.c_str(), dsdRate_, (unsigned long)dsd_.canRate,
                (int)ASIOSTDSDInt8LSB1, (int)ASIOSTDSDInt8MSB1, f.formatType);
            err = eb; restore_pcm_format(); free_drv(); return false;
        }
        dsd_.setRate = drv_->setSampleRate(dsdRate_);
        if (dsd_.setRate != ASE_OK) {
            std::snprintf(eb, sizeof(eb), "驱动「%s」setSampleRate(%.0f) 返回 0x%08lx（原生 DSD）",
                          name_.c_str(), dsdRate_, (unsigned long)dsd_.setRate);
            err = eb; restore_pcm_format(); free_drv(); return false;
        }
        ASIOSampleRate rb = 0.0;
        if (drv_->getSampleRate(&rb) == ASE_OK) dsd_.rateReadback = (double)rb;

        // 4) 驱动在 DSD 模式下必须报 1-bit DSD 类型，否则说明它根本没进 DSD
        types_.assign((size_t)ch_, ASIOSTInt32LSB);
        chNames_.assign((size_t)ch_, std::string());
        for (int c = 0; c < ch_; ++c) {
            ASIOChannelInfo ci{};
            ci.channel = c;
            ci.isInput = ASIOFalse;
            if (asio_guarded_channel_info(drv_, &ci) == ASE_OK) {
                types_[(size_t)c] = ci.type;
                chNames_[(size_t)c] = std::string(ci.name, strnlen(ci.name, 32));
            }
        }
        dsd_.type = types_[0];
        for (int c = 0; c < ch_; ++c) {
            if (!asio_is_dsd_1bit(types_[(size_t)c])) {
                std::snprintf(eb, sizeof(eb),
                    "驱动「%s」切到 DSD 后，输出 ch%d 的采样类型是 %ld（%s），"
                    "不是 ASIOSTDSDInt8LSB1(%d)/MSB1(%d)；请求类型=DSD，setIoFormat=0x%08lx，"
                    "canSampleRate(%.0f)=0x%08lx",
                    name_.c_str(), c, (long)types_[(size_t)c], asio_sample_type_name(types_[(size_t)c]),
                    (int)ASIOSTDSDInt8LSB1, (int)ASIOSTDSDInt8MSB1,
                    (unsigned long)dsd_.setIo, dsdRate_, (unsigned long)dsd_.canRate);
                err = eb; restore_pcm_format(); free_drv(); return false;
            }
        }
        packing_ = (types_[0] == ASIOSTDSDInt8LSB1) ? DsdPack::Lsb1 : DsdPack::Msb1;

        // 5) 建缓冲
        bufs_.assign((size_t)ch_, ASIOBufferInfo{});
        for (int c = 0; c < ch_; ++c) {
            bufs_[(size_t)c].isInput = ASIOFalse;
            bufs_[(size_t)c].channelNum = c;
        }
        std::memset(&cb_, 0, sizeof(cb_));
        cb_.bufferSwitch = &AsioDriver::s_buffer_switch;
        cb_.sampleRateDidChange = &AsioDriver::s_rate_changed;
        cb_.asioMessage = &AsioDriver::s_message;
        cb_.bufferSwitchTimeInfo = &AsioDriver::s_buffer_switch_time;
        {
            std::string attempts;
            const long used = create_buffers_resilient(bufSize_, attempts);
            dsd_.bufferAttempts = attempts;
            dsd_.createBuffers = used ? ASE_OK : (long)ASE_NotPresent;
            if (used == 0) {
                std::snprintf(eb, sizeof(eb),
                    "驱动「%s」原生 DSD 下 ASIOCreateBuffers 全部失败（ASE_OK 才是成功）："
                    "%d 声道，类型 %ld（%s），bufferSize 尝试 [%s]（单位=1-bit 样本）",
                    name_.c_str(), ch_, (long)dsd_.type, asio_sample_type_name(dsd_.type),
                    attempts.c_str());
                err = eb; restore_pcm_format(); free_drv(); return false;
            }
            dsdBufBytes_ = asio_dsd_bytes_per_buffer(bufSize_);
            dsd_.writeBytes = dsdBufBytes_;
        }
        buffersCreated_ = true;
        dsdMode_ = true;

        // 原生 DSD 的环要更深：DSD64 一个回调周期约 46 ms，4 周期只有 185 ms，
        // 系统一次 200 ms 的调度抖动就会让回调抓到空环（实测全曲 527 次欠载）。
        // 8 周期（约 370 ms）配合"按剩余空间部分填充"把欠载压到 0。
        ringFrames_ = std::max<size_t>(kRingFrames, (size_t)dsdBufBytes_ * 32);
        ring_.assign(ringFrames_ * (size_t)ch_, 0);
        ringR_ = ringW_ = ringCount_ = 0;
        underruns_ = 0;
        dsdPushed_ = 0;
        callbacks_ = 0;
        reset_completion_state();
        outReadySupported_ = false;
        return true;
    }

    // 原生 DSD 环形缓冲的空闲字节数（每声道）；供 sink 在写之前等待，避免
    // push_dsd 返回 0 被上层记成 WFAIL。
    size_t free_dsd_bytes() const {
        std::lock_guard<std::mutex> lk(mx_);
        return ringFrames_ > ringCount_ ? ringFrames_ - ringCount_ : 0;
    }

    // 推交错 DSD 字节（每声道 1 字节 = 8 个 1-bit 样本）；返回实际接收的字节数（每声道）
    size_t push_dsd(const uint8_t* bytes, size_t nBytes, bool final = false) {
        if (!dsdMode_ || !drv_) return 0;
        size_t ok = 0;
        {
            std::lock_guard<std::mutex> lk(mx_);
            if(inputFinished_)return 0;
            while (ok < nBytes && ringCount_ < ringFrames_) {
                const size_t dst = ringW_ * (size_t)ch_;
                const size_t src = ok * (size_t)ch_;
                for (int c = 0; c < ch_; ++c) ring_[dst + (size_t)c] = (int32_t)bytes[src + (size_t)c];
                ringW_ = (ringW_ + 1) % ringFrames_;
                ++ringCount_;
                ++ok;
            }
            if(final&&ok==nBytes)inputFinished_=true;
        }
        dsdPushed_ += ok;
        return ok;
    }

    bool dsd_mode() const { return dsdMode_; }
    const AsioDsdInfo& dsd_info() const { return dsd_; }
    // Driver observation only. Do not substitute the requested rate when the
    // device cannot report it: callers must distinguish unknown from verified.
    double sample_rate_readback() const {
        ASIOSampleRate actual = 0.0;
        if (!drv_ || drv_->getSampleRate(&actual) != ASE_OK || !std::isfinite(actual) || actual <= 0.0)
            return 0.0;
        return actual;
    }
    // 每个回调周期写入的字节数（= 驱动 bufferSize/8）；rtaplay 按它切片
    long dsd_buffer_bytes() const { return dsdBufBytes_; }
    // 实时音量微调（与 WASAPI 同语义：相对播放起点的差值 dB，0 = 逐位原路径）。
    // ★ F24：以前 OutSink::set_live_gain_db 在 ASIO 上是空实现 ⟹ 拖动滑块被静默忽略。
    //   DSD（原生）模式下本函数不产生任何效果（on_buffer 的 DSD 分支不碰 gain_）。
    void set_live_gain_db(double db) {
        if (db <= -144.0) { gain_.store(0.0, std::memory_order_relaxed); return; }
        gain_.store(db == 0.0 ? 1.0 : std::pow(10.0, db / 20.0), std::memory_order_relaxed);
    }
    double live_gain() const { return gain_.load(std::memory_order_relaxed); }

    const char* dsd_pack_name() const {
        if (!dsdMode_) return "PCM";
        return packing_ == DsdPack::Lsb1 ? "ASIOSTDSDInt8LSB1（bit0 最早）" : "ASIOSTDSDInt8MSB1（bit7 最早）";
    }
    // 驱动自己的样本位置计数：用它算"每秒消耗多少"，才能证明驱动真的按 DSD 位率在跑
    bool sample_position(unsigned long long& pos) {
        if (!drv_) return false;
        ASIOSamples s{};
        ASIOTimeStamp t{};
        if (drv_->getSamplePosition(&s, &t) != ASE_OK) return false;
        pos = ((unsigned long long)s.hi << 32) | (unsigned long long)s.lo;
        return true;
    }
    unsigned long long dsd_pushed_bytes() const { return dsdPushed_; }
    unsigned long long callbacks() const { return callbacks_; }

    // 驱动对 bufferSize 的接受度不一致：先按「缓冲毫秒 → 样本数」算目标值（不低于 pref、
    // 不超过 max），createBuffers 失败时按阶梯重试（max → pref → 目标折半…），
    // 每一次的原始 ASE 码都记进 attempts。返回 0 = 全部失败。
    long create_buffers_resilient(long target, std::string& attempts) {
        std::vector<long> tries;
        auto add = [&](long v) {
            if (v <= 0) return;
            if (minBuf_ > 0 && v < minBuf_) v = minBuf_;
            if (maxBuf_ > 0 && v > maxBuf_) v = maxBuf_;
            if (v < 8) v = 8;
            if (std::find(tries.begin(), tries.end(), v) == tries.end()) tries.push_back(v);
        };
        add(target);
        add(maxBuf_);
        add(prefBuf_);
        for (long v = target / 2; v >= 512; v /= 2) add(v);
        add(minBuf_);
        attempts.clear();
        for (long v : tries) {
            for (auto& b : bufs_) b.isInput = ASIOFalse;
            const ASIOError e = asio_guarded_create_buffers(drv_, bufs_.data(), (long)ch_, v, &cb_);
            if (!attempts.empty()) attempts += ", ";
            attempts += std::to_string(v) + ":" + std::to_string((long)e);
            if (e == ASE_OK) { bufSize_ = v; return v; }
        }
        return 0;
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
        if (drv_ && started_) { asio_guarded_stop(drv_); started_ = false; }
    }
    void close() {
        stop();
        if (drv_) {
            if (buffersCreated_) { asio_guarded_dispose(drv_); buffersCreated_ = false; }
            // 不能把设备留在 DSD 模式里：恢复 PCM，否则别的程序（和下一次 open）会拿到怪状态。
            if (dsdMode_) restore_pcm_format();
            free_drv();
        }
        dsdMode_ = false;
        if (g_inst == this) g_inst = nullptr;
    }
    // 推交错 int32；返回实际接收帧数
    size_t push(const int32_t* pcm, size_t frames, bool final = false) {
        detail::pcm_dump("asio", pcm, frames * (size_t)ch_, srcBits_);   // 取证（默认关闭）
        size_t ok = 0;
        {
            std::lock_guard<std::mutex> lk(mx_);
            if(inputFinished_)return 0;
            while (ok < frames && ringCount_ < ringFrames_) {
                const size_t dst = ringW_ * (size_t)ch_;
                const size_t src = ok * (size_t)ch_;
                for (int c = 0; c < ch_; ++c) ring_[dst + (size_t)c] = pcm[src + (size_t)c];
                ringW_ = (ringW_ + 1) % ringFrames_;
                ++ringCount_;
                ++ok;
            }
            if(final&&ok==frames)inputFinished_=true;
        }
        // 通知驱动"数据已就绪"——放在写入方线程，避免与回调重入
        if (ok && drv_ && outReadySupported_) drv_->outputReady();
        return ok;
    }
    size_t free_frames() const {
        std::lock_guard<std::mutex> lk(mx_);
        return ringFrames_ - ringCount_;
    }
    // 环里还压着多少（DSD 模式单位是字节）——收尾时要按它等驱动把尾巴吃完，否则统计里会冒出假欠载
    size_t buffered() const {
        std::lock_guard<std::mutex> lk(mx_);
        return ringCount_;
    }
    // An explicit EOF is a promise that no more source data will arrive.
    // The final push can publish it under the same lock as its partial tail.
    void finish_input() {std::lock_guard<std::mutex> lock(mx_);inputFinished_=true;}
    bool input_finished() const {std::lock_guard<std::mutex> lock(mx_);return inputFinished_;}
    bool drain_complete() const {
        std::lock_guard<std::mutex> lock(mx_);
        return inputFinished_&&ringCount_==0&&inFlightUnits_[0]==0&&inFlightUnits_[1]==0;
    }
    unsigned long long eof_silence_callbacks() const {std::lock_guard<std::mutex> lock(mx_);return eofSilenceCallbacks_;}
    unsigned long long eof_silence_units() const {std::lock_guard<std::mutex> lock(mx_);return eofSilenceUnits_;}
    unsigned long long played_units() const {std::lock_guard<std::mutex> lock(mx_);return playedUnits_;}
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
    friend struct AsioDriverTestAccess; // Offline callback fixture; never opens a driver.
    // DSD 打包方式（由驱动 getChannelInfo 报的类型决定，不是我们挑的）
    enum class DsdPack { Msb1, Lsb1 };

    void free_drv() {
        if (drv_) { drv_->Release(); drv_ = nullptr; }
    }
    // 把驱动切回 PCM：DSD 是"一次性"状态，留着会让下一次 open/别的宿主拿到意外格式
    void restore_pcm_format() {
        if (!drv_) return;
        ASIOIoFormat f{};
        f.formatType = kASIOPCMFormat;
        (void)asio_guarded_future(drv_, kAsioSetIoFormat, &f);
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
        ++callbacks_;

        // 每个回调周期要送的"单位数"：PCM 是帧，DSD 是字节（1 字节 = 8 个 1-bit 样本）
        const size_t period = dsdMode_ ? (size_t)dsdBufBytes_ : (size_t)bufSize_;
        std::vector<int32_t> tmp;
        {
            std::lock_guard<std::mutex> lk(mx_);
            // ASIO returns the now inactive buffer index. Its previous content
            // has been consumed; merely copying the last ring frame into this
            // buffer is not playback completion (there are two driver buffers).
            playedUnits_+=inFlightUnits_[(size_t)idx];
            inFlightUnits_[(size_t)idx]=0;
            tmp.resize(period * (size_t)ch_);
            const size_t take = ringCount_ < period ? ringCount_ : period;
            for (size_t i = 0; i < take; ++i) {
                const size_t s = ringR_ * (size_t)ch_;
                for (int c = 0; c < ch_; ++c) tmp[i * (size_t)ch_ + (size_t)c] = ring_[s + (size_t)c];
                ringR_ = (ringR_ + 1) % ringFrames_;
            }
            ringCount_ -= take;
            inFlightUnits_[(size_t)idx]=take;
            if (take < period) {
                if(inputFinished_){++eofSilenceCallbacks_;eofSilenceUnits_+=period-take;}
                else ++underruns_;
                const int32_t silence = dsdMode_ ? asio_silence_value(ASIOSTDSDInt8MSB1) : 0;
                for (size_t i = take * (size_t)ch_; i < tmp.size(); ++i) tmp[i] = silence;
            }
        }
        if (dsdMode_) {
            // 原生 DSD：每声道 bufSize_ 个【字节】，每字节 8 个 1-bit 样本。
            // 内部一律 MSB-first（DsdStream 的表示）；驱动要 LSB1 就逐字节反序。
            for (int c = 0; c < ch_; ++c) {
                void* dst = bufs_[(size_t)c].buffers[idx];
                if (!dst) continue;
                uint8_t* d = (uint8_t*)dst;
                const int32_t* src = tmp.data() + (size_t)c;
                const size_t stride = (size_t)ch_;
                if (packing_ == DsdPack::Msb1) {
                    for (size_t i = 0; i < period; ++i)
                        d[i] = (uint8_t)(src[i * stride] & 0xFF);
                } else {
                    for (size_t i = 0; i < period; ++i)
                        d[i] = asio_reverse_bits8((uint8_t)(src[i * stride] & 0xFF));
                }
            }
            return;
        }
        // ★ 2026-10-29（两轮审查 F24）：实时音量。旧实现 AsioSink 根本没有覆盖
        //   OutSink::set_live_gain_db，基类默认是空函数 ⟹ 播放中拖动音量滑块
        //   在 ASIO 后端上被静默忽略（"看起来成功了"，声音一点没变）。
        //   这里在【装箱阶段】施加，与 WASAPI 独占同一语义：0 dB ⟹ gain==1.0 ⟹
        //   乘法精确、逐位不变。DSD 模式绝不走这条（1-bit 流被 PCM 增益缩放会毁掉
        //   每一位，见 F04），所以下面 dsdMode_ 分支直接 return，碰不到 gain。
        const double g = gain_.load(std::memory_order_relaxed);
        for (int c = 0; c < ch_; ++c) {
            void* dst = bufs_[(size_t)c].buffers[idx];
            if (!dst) continue;
            const int32_t* src = tmp.data() + (size_t)c;
            const size_t stride = (size_t)ch_;
            write_channel(dst, src, stride, (size_t)bufSize_, types_[(size_t)c], srcBits_, g);
        }
        // outputReady() 只在"写入方"线程调用（见 push 之后），
        // 绝不能在驱动回调里调：会重入驱动（实测 2 秒左右访问冲突）。
    }

    // 2026-11-12：音量允许 +20 dB 后，实时增益 > 1。ASIO 整型分支以前直接 llround 后强转，
    // 超满幅会溢出回绕（正峰变负峰，听感是爆裂）。这里统一先饱和再转，float 分支也夹到 ±1。
    static double asio_saturate(double v, double lo, double hi) {
        if (!std::isfinite(v)) return 0.0;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    void reset_completion_state() {
        inputFinished_=false;inFlightUnits_[0]=inFlightUnits_[1]=0;
        playedUnits_=eofSilenceCallbacks_=eofSilenceUnits_=0;callbacks_=0;
    }
    static int32_t pcm_to_bits(int32_t sample, int srcBits, int dstBits, double gain) {
        const int64_t lim = int64_t(1) << (dstBits - 1);
        if (gain == 1.0) {
            // Integer unity never normalizes through 2^N-1: that scale used to
            // change every sample above half scale by one bit, including the
            // deliberately zero low bits of an 18/20/24-bit DAC in a 32-bit box.
            int64_t value = sample;
            if (dstBits >= srcBits) value *= int64_t(1) << (dstBits - srcBits);
            else {
                const int shift = srcBits - dstBits;
                const int64_t half = int64_t(1) << (shift - 1);
                value = value < 0 ? -((-value + half) >> shift) : (value + half) >> shift;
            }
            return (int32_t)std::clamp(value, -lim, lim - 1);
        }
        // Non-unity gain is a genuine requantization. Use the same asymmetric
        // signed rails as PCM/WASAPI, including the representable -full scale.
        const double value = std::ldexp((double)sample * gain, dstBits - srcBits);
        return (int32_t)std::llround(asio_saturate(value, -(double)lim, (double)(lim - 1)));
    }
    static void write_channel(void* dst, const int32_t* src, size_t stride, size_t n,
                              ASIOSampleType t, int srcBits, double gain = 1.0) {
        // 归一化因子：管线 int32 = srcBits 位定标
        const double inv = 1.0 / (double)(1LL << (srcBits - 1));
        const bool unity = (gain == 1.0);
        switch (t) {
            case ASIOSTInt32LSB: {
                int32_t* d = (int32_t*)dst;
                for (size_t i = 0; i < n; ++i) d[i] = pcm_to_bits(src[i * stride], srcBits, 32, gain);
                break;
            }
            case ASIOSTInt24LSB: {
                unsigned char* d = (unsigned char*)dst;
                for (size_t i = 0; i < n; ++i) {
                    // DoP marker is the high byte; payload and marker stay exact.
                    const int32_t q = pcm_to_bits(src[i * stride], srcBits, 24, gain);
                    const uint32_t u = (uint32_t)q;
                    d[i * 3 + 0] = (unsigned char)(u & 0xFF);
                    d[i * 3 + 1] = (unsigned char)((u >> 8) & 0xFF);
                    d[i * 3 + 2] = (unsigned char)((u >> 16) & 0xFF);
                }
                break;
            }
            case ASIOSTInt16LSB: {
                int16_t* d = (int16_t*)dst;
                for (size_t i = 0; i < n; ++i) d[i] = (int16_t)pcm_to_bits(src[i * stride], srcBits, 16, gain);
                break;
            }
            case ASIOSTFloat32LSB: {
                float* d = (float*)dst;
                for (size_t i = 0; i < n; ++i) {
                    const double v = (double)src[i * stride] * inv;
                    d[i] = (float)asio_saturate(unity ? v : v * gain, -1.0, 1.0);
                }
                break;
            }
            default: {
                // Never reinterpret PCM as DSD. Use the actual format's silence code.
                asio_write_silence(dst, n, t);
                break;
            }
        }
    }

    // ★ F24：实时音量（相对播放起点的差值 dB；0 ⟹ gain 1.0 ⟹ 逐位不变）。
    //   跨线程：渲染线程写、驱动回调线程读 ⟹ 必须是原子量。
    std::atomic<double> gain_{1.0};
    IASIO* drv_ = nullptr;
    std::string name_;
    ASIOSampleRate rate_ = 48000.0;
    ASIOSampleRate dsdRate_ = 0.0;
    int wantCh_ = 2, ch_ = 2, srcBits_ = 24;
    bool dsdMode_ = false;
    long dsdBufBytes_ = 0;   // DSD 每次回调的字节数 = 驱动 bufferSize/8
    DsdPack packing_ = DsdPack::Msb1;
    AsioDsdInfo dsd_{};
    unsigned long long dsdPushed_ = 0;
    std::atomic<unsigned long long> callbacks_{0};
    long inCh_ = 0, outCh_ = 0;
    long minBuf_ = 0, maxBuf_ = 0, prefBuf_ = 0, granBuf_ = 0, bufSize_ = 512;
    double bufferMs_ = 100.0;
    ASIOCallbacks cb_{};
    std::vector<ASIOBufferInfo> bufs_;
    std::vector<ASIOSampleType> types_;
    std::vector<std::string> chNames_;
    bool buffersCreated_ = false, started_ = false, outReadySupported_ = false;

    mutable std::mutex mx_;
    std::vector<int32_t> ring_;
    size_t ringFrames_ = kRingFrames;
    size_t ringR_ = 0, ringW_ = 0, ringCount_ = 0;
    bool inputFinished_=false;
    size_t inFlightUnits_[2]={0,0};
    unsigned long long playedUnits_=0,eofSilenceCallbacks_=0,eofSilenceUnits_=0;
    std::atomic<unsigned long long> underruns_{0};
};

inline AsioDriver* AsioDriver::g_inst = nullptr;

// The Web renderer supplies arbitrary-sized PCM/DoP blocks. Keep at most one
// such block here, submit complete callback periods, and prime before start.
// Explicit EOF and its partial last push are atomic in AsioDriver; normal EOF
// silence is distinct from, and never clears, a mid-stream underrun.
class AsioBufferedStream {
public:
    explicit AsioBufferedStream(AsioDriver& driver):driver_(driver){}
    void configure(bool nativeDsd,uint32_t carrierRate,int channels,const std::atomic<bool>* stop=nullptr){
        native_=nativeDsd;rate_=carrierRate;channels_=channels;stop_=stop;
        period_=native_?std::max<size_t>(1,(size_t)driver_.dsd_buffer_bytes()/2):std::max<size_t>(1,(size_t)driver_.buffer_size());
        pending_.clear();pendingOffset_=0;bytes_.clear();armed_=started_=fatal_=false;accepted_=0;err_.clear();drainState_="not-requested";
    }
    bool start(){armed_=true;return true;}
    size_t write(const int32_t* words,size_t frames){
        if(!frames||fatal_||cancelled()||driver_.input_finished())return 0;
        if(!flush(false))return 0;
        if(pendingOffset_){pending_.erase(pending_.begin(),pending_.begin()+(long long)pendingOffset_);pendingOffset_=0;}
        pending_.insert(pending_.end(),words,words+frames*(size_t)channels_);
        accepted_+=frames;
        // These frames are now owned by pending_, even if a later device wait
        // fails. Reporting zero here would duplicate them on a retry.
        flush(false);
        return frames;
    }
    bool drain(){
        drainState_="draining";
        if(cancelled()){drainState_="cancelled";return true;}
        if(!flush(true)){drainState_=cancelled()?"cancelled":"write-failed";return cancelled();}
        driver_.finish_input();
        if(!ensure_started(true)){drainState_="start-failed";return false;}
        if(!started_){drainState_="complete";return true;}
        const double queuedFrames=(double)driver_.buffered()/(native_?2.:1.);
        const double deadline=now_ms()+std::max(500.,1000.*(queuedFrames+3.*period_)/std::max<uint32_t>(1,rate_)+500.);
        while(!driver_.drain_complete()){
            if(cancelled()){drainState_="cancelled";return true;}
            if(now_ms()>=deadline){err_="ASIO drain timeout before both driver buffers completed";drainState_="timeout";fatal_=true;return false;}
            wait_ms(1);
        }
        drainState_="complete";return true;
    }
    bool failed()const{return fatal_;}
    const std::string& last_error()const{return err_;}
    const std::string& drain_state()const{return drainState_;}
    size_t accepted_frames()const{return accepted_;}
    size_t pending_frames()const{return (pending_.size()-pendingOffset_)/(size_t)std::max(1,channels_);}
private:
    friend struct AsioBufferedStreamTestAccess;
    bool cancelled()const{return stop_&&stop_->load();}
    double now_ms()const{return clockHook_?clockHook_():(double)GetTickCount64();}
    void wait_ms(int ms){if(waitHook_)waitHook_(ms);else Sleep((DWORD)ms);}
    bool ensure_started(bool force){
        if(started_)return true;
        const size_t buffered=driver_.buffered();
        if(!buffered||(!force&&buffered<2*period_*(native_?2u:1u)))return true;
        if(!armed_){err_="ASIO stream was not armed";fatal_=true;return false;}
        if(!(startHook_?startHook_(err_):driver_.start(err_))){fatal_=true;return false;}
        started_=true;return true;
    }
    bool flush(bool final){
        while(pending_frames()>=period_||(final&&pending_frames()>0)){
            const size_t take=std::min(period_,pending_frames());
            const size_t units=take*(native_?2u:1u);
            const double deadline=now_ms()+std::max(200.,3000.*period_/std::max<uint32_t>(1,rate_)+100.);
            while(driver_.free_frames()<units){
                if(cancelled())return false;
                if(!ensure_started(false))return false;
                if(now_ms()>=deadline){err_="ASIO ring did not accept a complete callback period";fatal_=true;return false;}
                wait_ms(1);
            }
            const bool last=final&&take==pending_frames();
            size_t got=0;
            if(native_){
                bytes_.resize(units*(size_t)channels_);
                for(size_t n=0;n<take;++n)for(int c=0;c<channels_;++c){
                    uint8_t hi=0,lo=0;rta::dsp::dop_word_to_dsd_bytes((uint32_t)pending_[pendingOffset_+n*(size_t)channels_+(size_t)c],hi,lo);
                    bytes_[(2*n)*(size_t)channels_+(size_t)c]=hi;bytes_[(2*n+1)*(size_t)channels_+(size_t)c]=lo;
                }
                got=driver_.push_dsd(bytes_.data(),units,last)/2;
            }else got=driver_.push(pending_.data()+pendingOffset_,take,last);
            if(got!=take){err_="ASIO ring rejected a reserved callback period";fatal_=true;return false;}
            pendingOffset_+=take*(size_t)channels_;
            if(pendingOffset_==pending_.size()){pending_.clear();pendingOffset_=0;}
            if(!ensure_started(false))return false;
        }
        return true;
    }
    AsioDriver& driver_;
    bool native_=false,armed_=false,started_=false,fatal_=false;
    uint32_t rate_=48000;int channels_=2;size_t period_=512,accepted_=0;
    const std::atomic<bool>* stop_=nullptr;
    std::vector<int32_t> pending_;std::vector<uint8_t> bytes_;
    size_t pendingOffset_=0;
    std::string err_,drainState_="not-requested";
    std::function<bool(std::string&)> startHook_;
    std::function<void(int)> waitHook_;
    std::function<double()> clockHook_;
};

} // namespace rta::audio
