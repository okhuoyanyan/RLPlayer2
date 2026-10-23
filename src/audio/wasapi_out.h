// wasapi_out.h — WASAPI 独占 + 事件驱动输出后端
//
// 本文件严格照搬第 1 轮在真机上验证过的配方（每一条都是踩坑得来）：
//   1) hnsPeriodicity 必须等于 hnsBufferDuration（传 0 会报 AUDCLNT_E_BUFFER_SIZE_ERROR）
//   2) SetEventHandle 必须在 Initialize 成功【之后】调用
//   3) 开流前必须预填充整个缓冲，再 Start
//   4) 每个事件都必须被"响应"（写入），否则驱动判定饥饿并停止发送事件
//   5) AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED = 0x88890019，需用 GetBufferSize 反算对齐时长后重试
//   6) 计时必须用 QueryPerformanceCounter
#pragma once   // 2026-10-22：wasapi_shared.h 也会包含本文件（复用 hr_name 等），不能重复定义
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h 默认定义 min/max 宏，会破坏 std::min / std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "pcm_dump.h"
#include <stdexcept>

namespace rta::audio {

inline const char* hr_name(HRESULT hr) {
    switch ((unsigned)hr) {
        case 0x00000000: return "OK";
        case 0x00000001: return "S_FALSE";
        case 0x88890002: return "ALREADY_INITIALIZED";
        case 0x88890004: return "DEVICE_INVALIDATED";
        case 0x88890006: return "BUFFER_TOO_LARGE";
        case 0x88890008: return "UNSUPPORTED_FORMAT";
        case 0x8889000a: return "DEVICE_IN_USE";
        case 0x8889000e: return "EXCLUSIVE_MODE_NOT_ALLOWED";
        case 0x8889000f: return "ENDPOINT_CREATE_FAILED";
        case 0x88890013: return "BUFDURATION_PERIOD_NOT_EQUAL";
        case 0x88890014: return "EVENTHANDLE_NOT_SET";
        case 0x88890016: return "BUFFER_SIZE_ERROR";
        case 0x88890018: return "BUFFER_ERROR";
        case 0x88890019: return "BUFFER_SIZE_NOT_ALIGNED";
        case 0x88890020: return "INVALID_DEVICE_PERIOD";
        default: return "?";
    }
}

// 声道数 -> WAVEFORMATEXTENSIBLE 的 dwChannelMask。
// 独占模式下多声道必须把布局声明清楚，否则驱动会以 UNSUPPORTED_FORMAT 拒绝。
inline DWORD channel_mask_for(int channels) {
    switch (channels) {
        case 1:  return SPEAKER_FRONT_CENTER;
        case 2:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        case 4:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;   // 四声道
        case 6:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                        SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;                       // 5.1
        case 8:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                        SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                        SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;                                              // 7.1
        default: return 0;   // 不认识的布局交给驱动自己判断
    }
}

struct DeviceInfo {
    std::string name;
    std::string id;
    DWORD       state = 0;
    bool        isDefault = false;
};

// open() 失败时的结构化原因（HF-01/02 地基）：UNSUPPORTED_FORMAT 不再是唯一信息。
// 只在失败路径填充；open() 的出参指针默认为 nullptr，旧调用完全不变。
struct OpenFail {
    std::string request;         // 请求格式，如 "rate=44100 ch=2 bits=24 buf=100.0ms"
    std::string supportedList;   // 该 bits/channels 下独占支持的采样率，"44100/48000/…"；空=探测为空或端点不可用
    std::string hresultText;     // 如 "0x88890008 UNSUPPORTED_FORMAT"
    std::string deviceId;        // 实际使用的端点 id（空=默认端点）
};

// 结构化能力探测（HF-01/02 地基，只读）：16/24/32-bit 各自的独占支持率列表。
// 纯探测：不开流、不发声、不接管设备；不做任何回退/降级决策（决策在 rtaweb 接线）。
struct FormatProbe {
    std::vector<uint32_t> rates16, rates24, rates32;
    std::string deviceId;
    bool        endpointAlive = false;   // DEVICE_STATE_ACTIVE 才为 true
    std::string friendlyName;
};

// ---- 「写不进去」的成因分解（2026-10-22 用户真机反馈 20261022-02）----
// 旧日志只有 w0/wr，而「写不进去」至少有两条互斥成因，w0 把它们混成一个数：
//   ① 设备缓冲满    ⟹ pad == bufferFrames_ ⟹ room == 0
//   ② 手里数据不足一个周期，整周期量化后归零 ⟹ room > 0 但 avail < per
// 现场（96k/100ms，w0=205 wr=41，每次成功恰写 9600 帧整缓冲）区分不了二者，
// 而处置完全不同：①是设备/驱动问题，②是【我们的轮询粒度喂不上】。
// 独立成结构的原因（铁律 3）：判据本体要能被 mock 输入直接驱动，否则只能
// 「打开真设备再看日志」—— 那既违反铁律 1（不放声/不接管设备），也无法复现。
// WasapiOutput::write_available 用的就是这个结构，测试与生产走同一份代码。
struct ZeroAttrib {
    long long padFull = 0;   // ① 设备缓冲满
    long long starved = 0;   // ② 供料不足一个周期
    long long other   = 0;   // ③ 其它（可用空间不足一个周期等）
    int       last    = 0;   // 最近一次：0=写成功/未发生 1=满 2=供料不足 3=其它

    // toWrite     量化后的写入帧数（0 = 这次写不进去）
    // room        设备侧可用空间（整周期量化之前）
    // availFrames 手里待写帧数
    // per         设备周期帧数（0 = 未知，不做整周期量化）
    void classify(size_t toWrite, size_t room, size_t availFrames, size_t per) {
        if (toWrite != 0) { last = 0; return; }
        if (room == 0) { ++padFull; last = 1; }
        else if (per > 0 && availFrames < per) { ++starved; last = 2; }
        else { ++other; last = 3; }
    }
    // 最近一次「写不进去」的成因，一行文字，供 STAT / RENDERFAIL 直接打印
    std::string reason() const {
        if (last == 1) return "pad=full";
        if (last == 2) return "avail<period";
        if (last == 3) return "other";
        return "-";
    }
};

class WasapiOutput {
public:
    WasapiOutput() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInit_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        if (!comInit_) throw std::runtime_error("CoInitializeEx 失败");
    }
    ~WasapiOutput() { close(); if (comInit_) CoUninitialize(); }

    WasapiOutput(const WasapiOutput&) = delete;
    WasapiOutput& operator=(const WasapiOutput&) = delete;

    // 探测某设备在【独占模式】下支持的采样率。
    // 只调用 IsFormatSupported —— 不开流、不发声、不接管设备，完全只读。
    // 用途：设备不支持目标采样率时给出可用的替代值，而不是只报一个错误码。
    static std::vector<uint32_t> exclusive_rates(const std::string& deviceId, int bits = 24, int channels = 2) {
        std::vector<uint32_t> ok;
        HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool own = SUCCEEDED(ci);
        if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) return ok;

        IMMDeviceEnumerator* en = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), (void**)&en)) && en) {
            IMMDevice* dev = nullptr;
            if (deviceId.empty()) {
                en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
            } else {
                IMMDeviceCollection* col = nullptr;
                if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
                    UINT32 n = 0; col->GetCount(&n);
                    for (UINT32 i = 0; i < n && !dev; ++i) {
                        IMMDevice* d = nullptr;
                        if (FAILED(col->Item(i, &d)) || !d) continue;
                        LPWSTR id = nullptr;
                        if (SUCCEEDED(d->GetId(&id)) && id) {
                            char buf[1024] = {0};
                            WideCharToMultiByte(CP_UTF8, 0, id, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
                            if (deviceId == buf) dev = d; else d->Release();
                            CoTaskMemFree(id);
                        } else d->Release();
                    }
                    col->Release();
                }
            }
            if (dev) {
                IAudioClient* ac = nullptr;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) && ac) {
                    static const uint32_t cand[] = { 44100, 48000, 88200, 96000, 176400, 192000,
                                                     352800, 384000, 705600, 768000, 1536000 };
                    WAVEFORMATEXTENSIBLE wfx{};
                    // ★ 2026-10-23 实测（wasapi_ex_probe 矩阵）：UltraLite-mk5 的独占只接受
                    // 「32 位容器 + 24 位有效」（wBitsPerSample=32 / wValidBitsPerSample=24 /
                    // blockAlign=ch*4）。紧凑 24 位（blockAlign=ch*3）会被 IsFormatSupported
                    // 判成 AUDCLNT_E_UNSUPPORTED_FORMAT —— 这就是"独占全位深都不支持"的真因。
                    const int cont = (bits <= 16) ? 2 : 4;          // 每样本容器字节
                    const int valid = bits;                          // 有效位深
                    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                    wfx.Format.nChannels = (WORD)channels;
                    wfx.Format.nSamplesPerSec = 48000;
                    wfx.Format.wBitsPerSample = (WORD)(cont * 8);
                    wfx.Format.nBlockAlign = (WORD)(channels * cont);
                    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
                    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                    wfx.Samples.wValidBitsPerSample = (WORD)valid;
                    wfx.dwChannelMask = channel_mask_for(channels);
                    wfx.SubFormat = (bits == 32) ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
                    for (uint32_t r : cand) {
                        wfx.Format.nSamplesPerSec = r;
                        wfx.Format.nAvgBytesPerSec = r * wfx.Format.nBlockAlign;
                        if (ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                  (WAVEFORMATEX*)&wfx, nullptr) == S_OK) ok.push_back(r);
                    }
                    ac->Release();
                }
                dev->Release();
            }
            en->Release();
        }
        if (own) CoUninitialize();
        return ok;
    }

    // 结构化格式探测（HF-01/02）：按端点 id 或友好名找设备，检查 DEVICE_STATE，
    // 再对 16/24/32-bit 各跑一遍独占支持率列表（内部复用 exclusive_rates 的只读探测）。
    // 找不到端点时 endpointAlive=false 且三个列表为空，如实返回；不做任何回退决策。
    static FormatProbe probe_formats(const std::string& deviceFriendlyNameOrId) {
        FormatProbe p;
        const auto devs = enumerate(eRender);   // 含 ACTIVE/DISABLED/NOTPRESENT/UNPLUGGED 全状态
        const DeviceInfo* found = nullptr;
        for (const auto& d : devs) {
            if ((!deviceFriendlyNameOrId.empty() && d.id == deviceFriendlyNameOrId) ||
                (!d.name.empty() && d.name == deviceFriendlyNameOrId)) { found = &d; break; }
        }
        if (!found) return p;                    // 找不到：如实返回空探测结果
        p.deviceId     = found->id;
        p.friendlyName = found->name;
        p.endpointAlive = (found->state == DEVICE_STATE_ACTIVE);
        if (!p.endpointAlive) return p;          // 禁用/拔出：支持列表无从谈起
        p.rates16 = exclusive_rates(p.deviceId, 16, 2);
        p.rates24 = exclusive_rates(p.deviceId, 24, 2);
        p.rates32 = exclusive_rates(p.deviceId, 32, 2);
        return p;
    }

    // 输入端点（线路输入 / 麦克风 / 回环）
    static std::vector<DeviceInfo> enumerate_capture() { return enumerate(eCapture); }

    // flow = eRender（默认，输出端点）或 eCapture（输入端点）
    static std::vector<DeviceInfo> enumerate(EDataFlow flow = eRender) {
        std::vector<DeviceInfo> out;
        // 静态方法不会走构造函数，必须自己初始化 COM（曾因此列出 0 个端点）
        HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool own = SUCCEEDED(ci);
        if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) return out;

        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en))) return out;
        IMMDevice* def = nullptr;
        en->GetDefaultAudioEndpoint(flow, eConsole, &def);
        LPWSTR defId = nullptr;
        if (def) def->GetId(&defId);

        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(flow,
                DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED, &col))) {
            UINT32 n = 0; col->GetCount(&n);
            for (UINT32 i = 0; i < n; ++i) {
                IMMDevice* d = nullptr;
                if (FAILED(col->Item(i, &d)) || !d) continue;
                DeviceInfo di;
                d->GetState(&di.state);
                LPWSTR id = nullptr;
                if (SUCCEEDED(d->GetId(&id)) && id) {
                    char buf[1024] = {0};
                    WideCharToMultiByte(CP_UTF8, 0, id, -1, buf, sizeof(buf), nullptr, nullptr);
                    di.id = buf;
                    if (defId && wcscmp(id, defId) == 0) di.isDefault = true;
                }
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                    PROPVARIANT v; PropVariantInit(&v);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                        char buf[512] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, buf, sizeof(buf), nullptr, nullptr);
                        di.name = buf;
                    }
                    PropVariantClear(&v);
                    ps->Release();
                }
                out.push_back(std::move(di));
                if (id) CoTaskMemFree(id);
                d->Release();
            }
            col->Release();
        }
        if (defId) CoTaskMemFree(defId);
        if (def) def->Release();
        en->Release();
        if (own) CoUninitialize();
        return out;
    }

    // bits: 16 / 24 / 32（24 为紧凑 3 字节；设备普遍只接受 24 或 32）
    // bufferMs: 缓冲时长；独占模式下必须与周期相等，内部按此实现
    // fail（HF-01/02，可选出参，默认 nullptr）: 失败路径带回请求格式、支持列表、
    //       HRESULT 文本与端点 id —— 错误不再只剩一个 UNSUPPORTED_FORMAT。
    bool open(const std::string& deviceId, uint32_t rate, int channels, int bits, double bufferMs = 100.0,
              OpenFail* fail = nullptr) {
        close();
        rate_ = rate; channels_ = channels; bits_ = bits;

        auto fillFail = [&](const std::string& hrTxt) {
            if (!fail) return;
            char rb[128];
            std::snprintf(rb, sizeof(rb), "rate=%u ch=%d bits=%d buf=%.1fms",
                          (unsigned)rate, channels, bits, bufferMs);
            fail->request = rb;
            fail->hresultText = hrTxt;
            fail->deviceId = deviceId;
            std::string lst;
            for (uint32_t r : exclusive_rates(deviceId, bits, channels)) {
                if (!lst.empty()) lst += "/";
                lst += std::to_string(r);
            }
            fail->supportedList = lst;   // 空 = 该位深下没有独占支持率，或端点不可用
        };

        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en))) {
            lastErr_ = "枚举器创建失败"; fillFail(lastErr_); return false;
        }
        IMMDevice* dev = nullptr;
        HRESULT hr;
        if (deviceId.empty()) {
            hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        } else {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
            std::wstring wid((size_t)wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, wid.data(), wlen);
            hr = en->GetDevice(wid.c_str(), &dev);
        }
        en->Release();
        if (FAILED(hr) || !dev) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s(GetDevice/GetDefaultAudioEndpoint)",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = "打开端点失败"; fillFail(hb); return false;
        }

        // 用 RAII 持有设备指针：对齐重试（配方 5）需要拿 dev 重新 Activate 一个新 client，
        // 所以不能在 Activate 之后立刻 Release（原实现在这里就释放了，导致那条分支永远无法真正重试）。
        struct DevRel { IMMDevice* d; ~DevRel() { if (d) d->Release(); } } devRel{ dev };
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_);
        if (FAILED(hr) || !ac_) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s(Activate)",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = "Activate(IAudioClient) 失败"; fillFail(hb); return false;
        }

        // 构造 WAVEFORMATEXTENSIBLE
        // ★ 2026-10-23：容器宽度与有效位深分开。UltraLite-mk5 独占只认「32 位容器 + 24 位有效」
        //（实测：紧凑 24 → AUDCLNT_E_UNSUPPORTED_FORMAT；32/24 → IsFormatSupported OK 且
        // Initialize 只返回 BUFFER_SIZE_NOT_ALIGNED，即可按对齐重试成功）。
        static const GUID SUB_PCM = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
        const int contBytes = (bits <= 16) ? 2 : 4;   // 每样本容器字节数
        std::memset(&wfx_, 0, sizeof(wfx_));
        wfx_.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        wfx_.Format.nChannels       = (WORD)channels;
        wfx_.Format.nSamplesPerSec  = rate;
        wfx_.Format.wBitsPerSample  = (WORD)(contBytes * 8);
        wfx_.Format.nBlockAlign     = (WORD)(channels * contBytes);
        wfx_.Format.nAvgBytesPerSec = rate * wfx_.Format.nBlockAlign;
        wfx_.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx_.Samples.wValidBitsPerSample = (WORD)bits;   // 有效位深（16 或 24）
        wfx_.dwChannelMask          = channel_mask_for(channels);
        wfx_.SubFormat              = SUB_PCM;

        // 多配方尝试（2026-10-23）：不同驱动对「独占 + 事件驱动」的参数组合脾气不同，
        // 单一配方失败就判死会把可用的设备挡在外面（现场：同机别的播放器独占正常）。
        // 每个配方都记 HRESULT，失败时整串带进 lastErr_，从此不用猜。
        ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        REFERENCE_TIME dur = (REFERENCE_TIME)(bufferMs * 10000.0);
        struct Try { REFERENCE_TIME buf; REFERENCE_TIME per; DWORD flags; const char* why; };
        const Try tries[] = {
            { dur, dur,       AUDCLNT_STREAMFLAGS_EVENTCALLBACK, "buffer=period=100ms,event" },
            { dur, 0,         AUDCLNT_STREAMFLAGS_EVENTCALLBACK, "buffer=100ms,period=驱动默认,event" },
            { 0,   0,         AUDCLNT_STREAMFLAGS_EVENTCALLBACK, "buffer/period 全默认,event" },
            { dur, 0,         0,                                 "buffer=100ms,period默认,轮询" },
            { 0,   0,         0,                                 "全默认,轮询" },
        };
        std::string trialLog;
        hr = E_FAIL;
        for (const auto& t : tries) {
            hr = ac_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, t.flags, t.buf, t.per,
                                 (WAVEFORMATEX*)&wfx_, nullptr);
            {
                char b[160];
                std::snprintf(b, sizeof(b), "[%s -> 0x%08lX %s] ",
                              t.why, (unsigned long)(unsigned)hr, hr_name(hr));
                trialLog += b;
            }
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {        // 配方 5：按实际缓冲对齐后重试
                // ★ 必须换一个**新的** IAudioClient：微软文档要求 Release 后重新 Activate，
                // 复用同一个 client 重试只会得到 AUDCLNT_E_BUFFER_SIZE_ERROR（0x88890016）。
                // 2026-10-23 实测：对齐后 periodicity 仍取对齐时长（不是 0），返回 S_OK。
                UINT32 aligned = 0;
                if (SUCCEEDED(ac_->GetBufferSize(&aligned)) && aligned) {
                    const REFERENCE_TIME d2 =
                        (REFERENCE_TIME)(10000.0 * 1000.0 * (double)aligned / (double)rate + 0.5);
                    ac_->Release();
                    ac_ = nullptr;
                    HRESULT hr2 = E_FAIL;
                    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_)) && ac_) {
                        hr2 = ac_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, t.flags, d2, d2,
                                              (WAVEFORMATEX*)&wfx_, nullptr);
                    }
                    {
                        char b[160];
                        std::snprintf(b, sizeof(b), "[对齐 %u 帧(%lldus),新client -> 0x%08lX %s] ",
                                      (unsigned)aligned, (long long)d2,
                                      (unsigned long)(unsigned)hr2, hr_name(hr2));
                        trialLog += b;
                    }
                    if (hr2 == S_OK) { realigned_ = true; hr = S_OK; break; }
                    hr = hr2;
                }
            }
            if (hr == S_OK) break;
        }
        if (hr != S_OK) {
            lastErr_ = std::string("Initialize 失败（全部配方）：") + trialLog;
            fillFail(trialLog.c_str());   // HF-01：把请求格式与该位深的实际独占支持列表带出去
            close(); return false;
        }

        // 配方 2：SetEventHandle 在 Initialize 之后
        hr = ac_->SetEventHandle(ev_);
        if (FAILED(hr)) {
            char hb[64]; std::snprintf(hb, sizeof(hb), "0x%08lX %s",
                                       (unsigned long)(unsigned)hr, hr_name(hr));
            lastErr_ = std::string("SetEventHandle 失败: ") + hr_name(hr);
            fillFail(hb); close(); return false;
        }

        ac_->GetBufferSize(&bufferFrames_);
        { REFERENCE_TIME p = 0, mp = 0; ac_->GetDevicePeriod(&p, &mp); period_ = p; }
        if (FAILED(ac_->GetService(__uuidof(IAudioRenderClient), (void**)&rc_)) || !rc_) {
            lastErr_ = "GetService(IAudioRenderClient) 失败"; fillFail(lastErr_); close(); return false;
        }
        ac_->GetService(__uuidof(IAudioClock), (void**)&clock_);   // 可选，用于统计
        return true;
    }

    bool start() {
        if (!ac_ || !rc_) return false;
        // 不做预填充：改用 GetCurrentPadding 驱动写入后，首事件必须能写入数据
        // （预填充会让首事件 padding==bufferFrames ⟹ 可用空间为 0 ⟹ 跳过 ⟹ 流饥饿停摆）
        written_ = 0;
        HRESULT hr = ac_->Start();
        if (FAILED(hr)) { lastErr_ = std::string("Start 失败: ") + hr_name(hr); return false; }
        LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
        qpcFreq_ = f.QuadPart; qpcStart_ = t.QuadPart;
        running_ = true;
        return true;
    }

    // 写入交错整数样本（每样本 int32，按 bits_ 决定取值域）。
    // 返回真正写入的帧数。必须由调用方循环喂满。
    size_t write(const int32_t* interleaved, size_t frames) {
        if (!running_ || frames == 0) return 0;
        size_t done = 0;
        while (done < frames) {
            const size_t chunk = std::min(frames - done, (size_t)bufferFrames_);
            BYTE* data = nullptr;
            if (FAILED(rc_->GetBuffer((UINT32)chunk, &data))) break;
            pack(data, interleaved + done * (size_t)channels_, chunk);
            if (FAILED(rc_->ReleaseBuffer((UINT32)chunk, 0))) break;
            written_ += chunk;
            done += chunk;
            if (chunk == (size_t)bufferFrames_) break;   // 一次事件写一个缓冲
        }
        return done;
    }

    // 暂停 / 继续：用 IAudioClient::Stop / Start。
    // 这与「跳过事件不写」完全不同 —— 后者会让驱动永久停止发事件（Round 6 实测），
    // 而 Stop/Start 是 WASAPI 的合法暂停路径。
    bool pause() { return ac_ && ac_->Stop() == S_OK; }
    bool resume() { return ac_ && ac_->Start() == S_OK; }
    // 清空设备内部缓冲 —— 跳转(seek)前必须先 Reset，否则旧数据会继续播出来
    bool reset() { return ac_ && ac_->Reset() == S_OK; }

    // 实时音量微调（相对"播放起点"的差值，单位 dB）。
    // 之所以做成「差值」而不是绝对值：绝对音量在 DSP 里已经带抖动做过一次（音质正确的那条路），
    // 这里只负责拖动滑块时的即时响应。差值为 0 时 gain_=1.0，pack 完全走原路径，逐位不变。
    void set_live_gain_db(double db) {
        if (db <= -144.0) { gain_ = 0.0; return; }
        gain_ = (db == 0.0) ? 1.0 : std::pow(10.0, db / 20.0);
    }

    // 等待下一次"需要数据"的信号
    bool wait_event(int timeoutMs = 500) {
        if (!ev_) return false;
        return WaitForSingleObject(ev_, (DWORD)timeoutMs) == WAIT_OBJECT_0;
    }

    // ★ 正确模型：按设备【实际消耗】写入，而不是"每个事件写满一个缓冲"。
    //   实测：本机事件间隔约 95 ms，而缓冲时长 100 ms——按事件写满会超额写入约 9%，
    //   多出的数据会被驱动丢弃（等于丢样/跳音）。
    //   这里以 IAudioClock 位置为准：保持"已写入"恰好领先"已播放"一个缓冲深度。
    //   返回本次写入的帧数（0 表示设备尚未消耗、本次无需写入）。
    size_t pump_consumed(const int32_t* src, size_t totalFrames, size_t& cursor) {
        if (!running_) return 0;
        // 用 GetCurrentPadding 求"缓冲里还有多少未被消耗"，据此算出可用空间。
        // 这比 IAudioClock 位置可靠：实测位置计数被量化到缓冲边界，
        // 在事件之间根本不推进，用它做节流会把每个事件都判成"未消耗"而饿死流。
        UINT32 pad = 0;
        if (FAILED(ac_->GetCurrentPadding(&pad))) pad = 0;
        lastPad_ = (long long)pad;
        if (pad > bufferFrames_) pad = bufferFrames_;
        long long toWrite = (long long)bufferFrames_ - (long long)pad;
        if (toWrite <= 0) { skips_++; return 0; }
        if (cursor < totalFrames) {
            const size_t avail = totalFrames - cursor;
            if ((size_t)toWrite > avail) toWrite = (long long)avail;
        }
        if (toWrite <= 0) return 0;

        BYTE* data = nullptr;
        lastTw_ = (long long)toWrite; lastTotal_ = (long long)totalFrames; lastCursor_ = (long long)cursor;
        HRESULT gb = rc_->GetBuffer((UINT32)toWrite, &data);
        lastGbHr_ = (long long)gb;
        if (FAILED(gb)) return 0;
        pack(data, src + cursor * (size_t)channels_, (size_t)toWrite);
        if (FAILED(rc_->ReleaseBuffer((UINT32)toWrite, 0))) return 0;
        written_ += (size_t)toWrite;
        cursor += (size_t)toWrite;
        return (size_t)toWrite;
    }

    // 严格按"一个周期"写。独占 + 事件回调模式下 GetBuffer 的帧数【必须是周期长度的整数倍】，
    // 否则返回 AUDCLNT_E_BUFFER_SIZE_ERROR(0x88890016) 且一个字节都不写 ——
    // 表现为"播放中但进度永远 0 秒"（本轮踩到）。旧版侥幸没触发，是因为它每次正好有整周期数据。
    size_t write_one_period(const int32_t* src, size_t availFrames) {
        if (!running_) return 0;
        UINT32 pad = 0;
        if (FAILED(ac_->GetCurrentPadding(&pad))) pad = 0;
        lastPad_ = (long long)pad;
        if (pad != 0) return 0;                       // 设备还没把上一周期播完
        if (availFrames < (size_t)bufferFrames_) return 0;
        BYTE* data = nullptr;
        const HRESULT gb = rc_->GetBuffer(bufferFrames_, &data);
        lastGbHr_ = (long long)gb;
        if (FAILED(gb) || !data) return 0;
        pack(data, src, (size_t)bufferFrames_);
        if (FAILED(rc_->ReleaseBuffer(bufferFrames_, 0))) return 0;
        written_ += (size_t)bufferFrames_;
        return (size_t)bufferFrames_;
    }

    // 按"设备实际可用空间"写入（write_one_period 只在 pad==0 时写，遇到
    // 设备周期 < 缓冲时长的驱动会频繁写不进去，实测 w0 高达 1000+）。
    // 每次事件尽量把缓冲补满，但保持周期整数倍；返回实际写入帧数。
    size_t write_available(const int32_t* src, size_t availFrames) {
        if (!running_ || !ac_ || !rc_ || availFrames == 0) return 0;
        UINT32 pad = 0;
        if (FAILED(ac_->GetCurrentPadding(&pad))) return 0;
        lastPad_ = (long long)pad;
        if (pad > bufferFrames_) pad = bufferFrames_;
        size_t toWrite = (size_t)bufferFrames_ - (size_t)pad;
        // ★ 2026-10-23 真 bug 修复：某些驱动（MOTU）在 Start 之后会报告 pad == bufferFrames，
        // 此时按 pad 算可用空间恒为 0 → 永远不写 → 流饿死（现场表现：wr 不涨、4 秒后
        // RENDERFAIL、最后回退共享）。首次写入时直接试满缓冲：GetBuffer 若真的没空间
        // 就会失败并返回 0，对正常路径无害。
        if (toWrite == 0 && written_ == 0) toWrite = (size_t)bufferFrames_;
        const size_t room = toWrite;          // 设备侧可用空间（量化之前）
        if (toWrite > availFrames) toWrite = availFrames;
        // 独占模式的 GetBuffer 要求周期整数倍；向下取整，宁可少写也不报错。
        size_t per = 0;
        if (period_ > 0 && rate_ > 0) {
            per = (size_t)((double)period_ * (double)rate_ / 10000000.0 + 0.5);
            if (per > 1 && per <= bufferFrames_ && (toWrite % per) != 0)
                toWrite = (toWrite / per) * per;
        }
        // ---- 归因计数（2026-10-22 用户真机反馈 20261022-02）：只观测，不改变行为 ----
        // 判定本体在 ZeroAttrib::classify（见本文件上方）；这里只把三个观测量喂进去。
        // 独立成结构是为了让 tests/test_renderfail.cpp 能用 mock 输入直接驱动它
        //（真开 WASAPI 才能测的话，既违反铁律 1，也不可复现）。
        zero_.classify(toWrite, room, availFrames, per);
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

    // 写不进去的三类成因计数（2026-10-22 反馈 20261022-02：w0 把它们混成一个数，无法定案）
    //   ① 设备缓冲恒满（pad == bufferFrames_）   ② 供料不足一个周期 + 整周期量化归零
    //   ③ 其它（GetCurrentPadding 失败等）
    long long zero_pad_full() const { return zero_.padFull; }
    long long zero_starved()  const { return zero_.starved; }
    long long zero_other()    const { return zero_.other; }
    // 最近一次「写不进去」的成因，一行文字，供 STAT / RENDERFAIL 直接打印
    std::string last_zero_reason() const { return zero_.reason(); }
    long long skips() const { return skips_; }   // 因"设备未消耗"而跳过的事件数
    long long last_pad() const { return lastPad_; }
    long long last_gb_hr() const { return lastGbHr_; }
    std::string last_gb_name() const {
        char b[64];
        std::snprintf(b, sizeof(b), "%08lX %s", (unsigned long)(unsigned)lastGbHr_, hr_name((HRESULT)lastGbHr_));
        return std::string(b);
    }
    long long last_tw() const { return lastTw_; }
    long long last_total() const { return lastTotal_; }
    long long last_cursor() const { return lastCursor_; }


    // 缓冲积压 = 已写入 - 设备已渲染。稳态下应稳定在一个缓冲深度附近。
    // 若它随时间【持续增长】，说明写入快于渲染 ⟹ 驱动只能丢弃 ⟹ 真实丢样。
    // 若它【收敛到常数】，说明只是位置计数量化 ⟹ 无丢样。
    long long backlog() const {
        if (!clock_) return -1;
        unsigned long long pos = 0;
        if (!device_position(pos)) return -1;
        return (long long)written_ - (long long)pos;
    }

    size_t buffer_frames() const { return bufferFrames_; }
    size_t frames_written() const { return written_; }
    double elapsed_seconds() const {
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return (double)(t.QuadPart - qpcStart_) / (double)qpcFreq_;
    }
    bool device_position(unsigned long long& pos) const {
        if (!clock_) return false;
        UINT64 p = 0, q = 0;
        if (FAILED(clock_->GetPosition(&p, &q))) return false;
        pos = (unsigned long long)p; return true;
    }
    uint32_t rate() const { return rate_; }
    int bits() const { return bits_; }
    int channels() const { return channels_; }
    bool realigned() const { return realigned_; }
    const std::string& last_error() const { return lastErr_; }

    void close() {
        if (ac_ && running_) { ac_->Stop(); running_ = false; }
        if (clock_) { clock_->Release(); clock_ = nullptr; }
        if (rc_)    { rc_->Release();    rc_ = nullptr; }
        if (ac_)    { ac_->Release();    ac_ = nullptr; }
        if (ev_)    { CloseHandle(ev_);  ev_ = nullptr; }
    }

private:
    // 应用微调增益：四舍五入，并按位深夹紧（越界会让 DAC 听到削波）
    int32_t scaled(int32_t s) const {
        const double v = (double)s * gain_;
        const double lim = (double)((int64_t)1 << (bits_ - 1));
        if (v >  lim - 1.0) return (int32_t)(int64_t)(lim - 1.0);
        if (v < -lim)       return (int32_t)(int64_t)(-lim);
        return (int32_t)(v >= 0.0 ? (v + 0.5) : (v - 0.5));
    }

    void pack(BYTE* dst, const int32_t* src, size_t frames) {
        detail::pcm_dump("wasapi-ex", src, frames * (size_t)channels_, bits_);   // 取证（默认关闭）
        const size_t n = frames * (size_t)channels_;
        if (gain_ != 1.0) {
            // 慢路径：只用于用户拖动音量滑块的场合
            if (bits_ == 16) {
                auto* p = (int16_t*)dst;
                for (size_t i = 0; i < n; ++i) p[i] = (int16_t)scaled(src[i]);
            } else {
                // 24 位也走 32 位容器（wBitsPerSample=32 / valid=24）：把 ±2^23 的值直接放
                // int32 低位。驱动实测只认这个容器宽度（紧凑 3 字节会被判 UNSUPPORTED_FORMAT）。
                auto* p = (int32_t*)dst;
                for (size_t i = 0; i < n; ++i) p[i] = scaled(src[i]);
            }
            return;
        }
        if (bits_ == 16) {
            auto* p = (int16_t*)dst;
            for (size_t i = 0; i < n; ++i) p[i] = (int16_t)src[i];
        } else {
            // 同上：24 位 = 32 位容器 + 24 位有效，写 int32
            auto* p = (int32_t*)dst;
            for (size_t i = 0; i < n; ++i) p[i] = src[i];
        }
    }

    bool comInit_ = false, running_ = false, realigned_ = false;
    IAudioClient* ac_ = nullptr;
    IAudioRenderClient* rc_ = nullptr;
    IAudioClock* clock_ = nullptr;
    HANDLE ev_ = nullptr;
    WAVEFORMATEXTENSIBLE wfx_{};
    UINT32 bufferFrames_ = 0;
    REFERENCE_TIME period_ = 0;
    uint32_t rate_ = 0;
    int channels_ = 0, bits_ = 0;
    size_t written_ = 0;
    double gain_ = 1.0;          // 1.0 = 未启用微调（逐位原样）
    long long lastPad_ = -1, lastGbHr_ = -999, lastTw_ = -1, lastTotal_ = -1, lastCursor_ = -1;
    long long skips_ = 0;
    // 「写不进去」的成因分解（见文件上方的 ZeroAttrib；分类与计数都在该结构里）
    ZeroAttrib zero_;
    long long qpcFreq_ = 1, qpcStart_ = 0;
    std::string lastErr_;
};

} // namespace rta::audio
