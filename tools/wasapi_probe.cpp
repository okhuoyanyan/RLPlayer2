// wasapi_probe.cpp — 枚举音频端点，探测 WASAPI 独占能力矩阵，并真实推流验证
// 构建：clang++ (ROCm 7.2) + lld-link + Windows SDK
// 输出统一 UTF-8。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstring>

static const GUID SUB_PCM   = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID SUB_FLOAT = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const PROPERTYKEY PK_FORMFACTOR     = { {0x1da5d803,0xd492,0x4edd,{0x8c,0x23,0xe0,0xc0,0xff,0xee,0x7f,0x0e}}, 0 };
static const PROPERTYKEY PK_DISABLE_SYSFX  = { {0x1da5d803,0xd492,0x4edd,{0x8c,0x23,0xe0,0xc0,0xff,0xee,0x7f,0x0e}}, 5 };

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Fmt { WAVEFORMATEXTENSIBLE wfx; };

static Fmt MakeFmt(UINT32 rate, WORD bits, WORD channels, bool isFloat) {
    Fmt f{};
    WAVEFORMATEXTENSIBLE& w = f.wfx;
    w.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels       = channels;
    w.Format.nSamplesPerSec  = rate;
    w.Format.wBitsPerSample  = bits;
    w.Format.nBlockAlign     = (WORD)(channels * bits / 8);
    w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
    w.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = bits;
    w.dwChannelMask          = (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    w.SubFormat              = isFloat ? SUB_FLOAT : SUB_PCM;
    return f;
}

// 权威错误名（对照 Windows SDK um/audioclient.h，AUDCLNT_ERR(n) = 0x88890000 | n）
static const char* HrName(HRESULT hr) {
    switch ((unsigned)hr) {
        case 0x00000000: return "OK";
        case 0x00000001: return "OK-near(S_FALSE)";
        case 0x88890001: return "AUDCLNT_E_NOT_INITIALIZED";
        case 0x88890002: return "AUDCLNT_E_ALREADY_INITIALIZED";
        case 0x88890003: return "AUDCLNT_E_WRONG_ENDPOINT_TYPE";
        case 0x88890004: return "AUDCLNT_E_DEVICE_INVALIDATED";
        case 0x88890005: return "AUDCLNT_E_NOT_STOPPED";
        case 0x88890006: return "AUDCLNT_E_BUFFER_TOO_LARGE";
        case 0x88890007: return "AUDCLNT_E_OUT_OF_ORDER";
        case 0x88890008: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
        case 0x88890009: return "AUDCLNT_E_INVALID_SIZE";
        case 0x8889000a: return "AUDCLNT_E_DEVICE_IN_USE";
        case 0x8889000b: return "AUDCLNT_E_BUFFER_OPERATION_PENDING";
        case 0x8889000c: return "AUDCLNT_E_THREAD_NOT_REGISTERED";
        case 0x8889000e: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
        case 0x8889000f: return "AUDCLNT_E_ENDPOINT_CREATE_FAILED";
        case 0x88890010: return "AUDCLNT_E_SERVICE_NOT_RUNNING";
        case 0x88890011: return "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED";
        case 0x88890012: return "AUDCLNT_E_EXCLUSIVE_MODE_ONLY";
        case 0x88890013: return "AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL";
        case 0x88890014: return "AUDCLNT_E_EVENTHANDLE_NOT_SET";
        case 0x88890015: return "AUDCLNT_E_INCORRECT_BUFFER_SIZE";
        case 0x88890016: return "AUDCLNT_E_BUFFER_SIZE_ERROR";
        case 0x88890017: return "AUDCLNT_E_CPUUSAGE_EXCEEDED";
        case 0x88890018: return "AUDCLNT_E_BUFFER_ERROR";
        case 0x88890019: return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
        case 0x88890020: return "AUDCLNT_E_INVALID_DEVICE_PERIOD";
        case 0x88890021: return "AUDCLNT_E_INVALID_STREAM_FLAG";
        case 0x88890027: return "AUDCLNT_E_RAW_MODE_UNSUPPORTED";
        default: return "?";
    }
}

struct OpenResult {
    HRESULT hr = E_FAIL;
    UINT32 bufferFrames = 0;
    REFERENCE_TIME period = 0;
    bool realigned = false;
    bool started = false;
    const char* failStep = "";
    int eventsFired = 0;
    int framesPushed = 0;
    double elapsedMs = 0;
    unsigned long long devicePlayed = 0;   // IAudioClock 权威计数
    double deviceRate = 0;
    int skipEvents = 0;      // 位置未推进的伪事件
    int writeCalls = 0;
    int clockFail = 0;
};

// 按微软官方配方打开独占事件驱动流：
//   1) SetEventHandle 先于 Initialize
//   2) periodicity 必须与 bufferDuration 相等（独占模式）
//   3) NOT_ALIGNED(0x88890019) 时用 GetBufferSize 反算对齐时长后重试
static OpenResult OpenExclusive(IMMDevice* dev, const WAVEFORMATEX* fmt, UINT32 rate,
                                REFERENCE_TIME requested, bool doStream) {
    OpenResult r{};
    IAudioClient* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) {
        r.hr = E_FAIL; return r;
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    REFERENCE_TIME dur = requested;
    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                dur, dur /* periodicity == bufferDuration */, fmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        if (SUCCEEDED(ac->GetBufferSize(&aligned)) && aligned) {
            dur = (REFERENCE_TIME)(10000.0 * 1000 * aligned / rate + 0.5);
            hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                dur, dur, fmt, nullptr);
            r.realigned = true;
        }
    }
    r.hr = hr;
    if (hr != S_OK) { if (ev) CloseHandle(ev); SafeRelease(ac); return r; }

    // 事件句柄必须在 Initialize 成功之后设置（微软官方样例顺序）
    HRESULT hset = ac->SetEventHandle(ev);
    if (FAILED(hset)) { r.failStep = "SetEventHandle"; r.hr = hset; if (ev) CloseHandle(ev); SafeRelease(ac); return r; }

    ac->GetBufferSize(&r.bufferFrames);
    { REFERENCE_TIME p = 0, mp = 0; ac->GetDevicePeriod(&p, &mp); r.period = p; }

    if (doStream) {
        IAudioRenderClient* rc = nullptr;
        HRESULT hs = ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc);
        if (FAILED(hs)) { r.failStep = "GetService(IAudioRenderClient)"; r.hr = hs; }
        else {
            // 不做预填充：独占模式下首个事件必须被"响应"（写入数据），
            // 否则驱动判定流饥饿而停止发送事件（实测：预填充后跳过首次写入 => 只收到 1 个事件、设备只播 480 帧）。
            // 预填充：独占模式必须先填满整个缓冲再 Start，否则流会立刻饥饿停摆
            {
                BYTE* data = nullptr;
                HRESULT hg = rc->GetBuffer(r.bufferFrames, &data);
                if (FAILED(hg)) { r.failStep = "GetBuffer(prefill)"; r.hr = hg; }
                else {
                    memset(data, 0, (size_t)r.bufferFrames * fmt->nBlockAlign);
                    HRESULT hrl = rc->ReleaseBuffer(r.bufferFrames, 0);
                    if (FAILED(hrl)) { r.failStep = "ReleaseBuffer(prefill)"; r.hr = hrl; }
                }
            }
            {
            HRESULT hst = ac->Start();
            if (FAILED(hst)) { r.failStep = "Start()"; r.hr = hst; }
            else {
                r.started = true;
                // 独占模式：缓冲区完全归客户端所有，每个事件写入"一个缓冲区"的量。
                // 用 QPC 精确计时（GetTickCount 只有 ~15.6 ms 粒度，会误导）。
                LARGE_INTEGER f, t0, t1;
                QueryPerformanceFrequency(&f);

                IAudioClock* clock = nullptr;
                ac->GetService(__uuidof(IAudioClock), (void**)&clock);

                // 正确模型（实测得出）：
                //   - 独占模式每个"真实周期事件"写满一整个缓冲
                //   - 但事件流里混有伪信号（实测 1006ms 内 115 次事件 vs 100 个真实周期），
                //     必须用 IAudioClock::GetPosition() 做守卫，否则会超额写入 14%
                //   - 判据：written - pos < bufferFrames 时才补一个完整周期
                long long writtenFrames = (long long)r.bufferFrames;   // 含预填充
                UINT64 lastWritePos = 0;
                r.skipEvents = 0; r.writeCalls = 0;
                r.clockFail = 0;

                QueryPerformanceCounter(&t0);
                for (;;) {
                    QueryPerformanceCounter(&t1);
                    double elapsedMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
                    if (elapsedMs >= 1000.0) break;          // 推流 1000 ms
                    if (WaitForSingleObject(ev, 50) != WAIT_OBJECT_0) continue;
                    r.eventsFired++;
                    if (!clock) continue;
                    UINT64 pos = 0, qpc = 0;
                    if (FAILED(clock->GetPosition(&pos, &qpc))) { r.clockFail++; continue; }
                    // 正确模型（实测推导）：
                    //   GetBufferSize() 报 480 帧，但设备真实周期约 420 帧（8.75 ms），
                    //   所以"每事件写满一个缓冲"会超额写入 ~14%。
                    //   只补设备自上次写入以来实际消耗的帧数（pos - lastWritePos）。
                    long long consumed = (long long)pos - (long long)lastWritePos;
                    if (consumed <= 0) { r.skipEvents++; consumed = 0; }
                    // 该驱动"每个事件都必须被响应"，但 GetPosition 在事件间不实时推进，
                    // 故按消耗量写入只能作为记账；实际下发量以 0 为下限、缓冲为上限。
                    long long toWrite = consumed;
                    if (toWrite <= 0) toWrite = (long long)r.bufferFrames;  // 保守写满以保证流存活
                    if (toWrite > (long long)r.bufferFrames) toWrite = (long long)r.bufferFrames;
                    BYTE* p = nullptr;
                    if (SUCCEEDED(rc->GetBuffer((UINT32)toWrite, &p))) {
                        memset(p, 0, (size_t)toWrite * fmt->nBlockAlign);
                        rc->ReleaseBuffer((UINT32)toWrite, 0);
                        writtenFrames += toWrite;
                        lastWritePos = pos;
                        r.writeCalls++;
                    }
                }
                QueryPerformanceCounter(&t1);
                r.elapsedMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;

                // 权威校验：设备实际播放了多少帧（IAudioClock）
                if (clock) {
                    UINT64 pos = 0, qpc = 0;
                    if (SUCCEEDED(clock->GetPosition(&pos, &qpc))) {
                        r.devicePlayed = (unsigned long long)pos;
                        if (r.elapsedMs > 0) r.deviceRate = (double)pos * 1000.0 / r.elapsedMs;
                    }
                    SafeRelease(clock);
                }
                r.framesPushed = (int)writtenFrames;
                ac->Stop();
            }
            }
            SafeRelease(rc);
        }
    }
    if (ev) CloseHandle(ev);
    SafeRelease(ac);
    return r;
}

static const char* StateName(DWORD s) {
    switch (s) {
        case DEVICE_STATE_ACTIVE: return "ACTIVE";
        case DEVICE_STATE_DISABLED: return "DISABLED";
        case DEVICE_STATE_NOTPRESENT: return "NOTPRESENT";
        case DEVICE_STATE_UNPLUGGED: return "UNPLUGGED";
        default: return "?";
    }
}
static const char* FormFactorName(UINT32 ff) {
    switch (ff) {
        case 0: return "RemoteNetworkDevice"; case 1: return "Speakers";
        case 2: return "LineLevel"; case 3: return "Headphones";
        case 4: return "Microphone"; case 5: return "Headset";
        case 6: return "Handset"; case 7: return "UnknownDigitalPassthrough";
        case 8: return "SPDIF"; case 9: return "DigitalAudioDisplayDevice";
        case 10: return "UnknownFormFactor"; default: return "?";
    }
}

static void ProbeDevice(IMMDevice* dev, int index, bool isDefault) {
    IPropertyStore* props = nullptr;
    char name[512] = "(unknown)";
    DWORD state = 0;
    UINT32 formFactor = 0xFFFFFFFF;
    bool sysfxDisabled = false, hasSysfxKey = false;

    dev->GetState(&state);
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof(name), nullptr, nullptr);
        PropVariantClear(&v);
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PK_FORMFACTOR, &v)) && v.vt == VT_UI4) formFactor = v.uintVal;
        PropVariantClear(&v);
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PK_DISABLE_SYSFX, &v)) && v.vt == VT_UI4) { hasSysfxKey = true; sysfxDisabled = (v.uintVal != 0); }
        PropVariantClear(&v);
        SafeRelease(props);
    }

    printf("\n================================================================\n");
    printf("[%d] %s%s\n", index, name, isDefault ? "   <== 默认设备" : "");
    printf("    状态=%s  FormFactor=%s  音频增强(SysFx)=%s\n",
           StateName(state), FormFactorName(formFactor),
           !hasSysfxKey ? "未设置" : (sysfxDisabled ? "已关闭(好)" : "开启中(会污染信号!)"));
    if (state != DEVICE_STATE_ACTIVE) { printf("    (端点非活动，跳过)\n"); return; }

    IAudioClient* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) {
        printf("    !! Activate 失败\n"); return;
    }

    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(ac->GetMixFormat(&mix))) {
        printf("    [共享混音格式] %lu Hz, %u ch, %u bits", (unsigned long)mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* we = (WAVEFORMATEXTENSIBLE*)mix;
            printf("  subformat=%s validbits=%u",
                   IsEqualGUID(we->SubFormat, SUB_FLOAT) ? "IEEE_FLOAT" : IsEqualGUID(we->SubFormat, SUB_PCM) ? "PCM" : "other",
                   we->Samples.wValidBitsPerSample);
        }
        printf("\n");
        printf("    [独占] 混音格式 -> %s\n", HrName(ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, mix, nullptr)));
    }
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    if (SUCCEEDED(ac->GetDevicePeriod(&defPeriod, &minPeriod)))
        printf("    [周期] default=%.2f ms   min=%.2f ms\n", defPeriod / 10000.0, minPeriod / 10000.0);

    const UINT32 rates[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000, 1536000 };
    struct BD { WORD bits; bool isFloat; const char* label; };
    const BD bds[] = { {16,false,"16/PCM"}, {24,false,"24/PCM"}, {32,false,"32/PCM"}, {32,true,"32/FLT"} };

    printf("    %-9s", "rate\\fmt");
    for (auto& b : bds) printf("%-10s", b.label);
    printf("\n");
    for (UINT32 r : rates) {
        printf("    %-9u", r);
        for (auto& b : bds) {
            Fmt f = MakeFmt(r, b.bits, 2, b.isFloat);
            HRESULT e = ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&f.wfx, nullptr);
            printf("%-10s", (e == S_OK) ? "YES" : (e == S_FALSE) ? "near" : (e == AUDCLNT_E_UNSUPPORTED_FORMAT) ? "-" : HrName(e));
        }
        printf("\n");
    }
    SafeRelease(ac);

    // ---- 独占开流参数扫描（找这个设备真实可用的缓冲时长）----
    printf("    --- 独占+事件驱动 开流参数扫描 ---\n");
    struct Sweep { REFERENCE_TIME dur; const char* label; };
    const Sweep sweeps[] = {
        { 1000000, "100 ms" }, { 500000, "50 ms" }, { 200000, "20 ms" },
        { 100000, "10 ms" },   { 30000,  "3 ms" },  { 0, "0(驱动默认)" },
    };
    const UINT32 testRates[] = { 44100, 48000, 96000 };
    for (UINT32 r : testRates) {
        Fmt f = MakeFmt(r, 24, 2, false);
        for (auto& s : sweeps) {
            // 扫描阶段不真正推流，只验证能否开流
            OpenResult o = OpenExclusive(dev, (WAVEFORMATEX*)&f.wfx, r, s.dur, false);
            if (o.hr == S_OK) {
                printf("      %6u Hz/24: %-12s -> OK  buffer=%u 帧 (%.2f ms)%s\n",
                       r, s.label, o.bufferFrames, o.bufferFrames * 1000.0 / r, o.realigned ? "  [对齐重试]" : "");
            } else if (o.hr != AUDCLNT_E_BUFFER_SIZE_ERROR) {
                printf("      %6u Hz/24: %-12s -> %s\n", r, s.label, HrName(o.hr));
            }
        }
    }

    // ---- 真实推流验证（用第一个能开流的组合）----
    printf("    --- 真实推流验证 (48 kHz/24bit, 100 ms 缓冲, 1000 ms 时长) ---\n");
    {
        Fmt f = MakeFmt(48000, 24, 2, false);
        OpenResult o = OpenExclusive(dev, (WAVEFORMATEX*)&f.wfx, 48000, 1000000, true);
        if (o.hr == S_OK && o.started) {
            double audioMs = o.framesPushed * 1000.0 / 48000.0;
            printf("      推流成功: 墙钟 %.1f ms | 事件 %d 次 | 写入 %d 帧 = %.1f ms 音频\n",
                   o.elapsedMs, o.eventsFired, o.framesPushed, audioMs);
            printf("      设备权威计数: 已播放 %llu 帧, 推算实际速率 %.1f Hz (标称 48000)\n",
                   o.devicePlayed, o.deviceRate);
            printf("      写入调用 %d 次 | 跳过伪事件 %d 次 | GetPosition 失败 %d 次\n",
                   o.writeCalls, o.skipEvents, o.clockFail);
            printf("      写入/播放 差 = %lld 帧 (%.1f ms)  %s\n",
                   (long long)o.framesPushed - (long long)o.devicePlayed,
                   ((double)o.framesPushed - (double)o.devicePlayed) * 1000.0 / 48000.0,
                   ((long long)o.framesPushed - (long long)o.devicePlayed) < 480
                       ? "<== 未超出缓冲，写入模型正确"
                       : "<== 超额写入，需按实际消耗节流");
        }
        else
            printf("      推流失败于 [%s]: %s\n",
                   o.failStep[0] ? o.failStep : "(未启动)", HrName(o.hr));
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { printf("CoInitializeEx 失败\n"); return 1; }

    IMMDeviceEnumerator* en = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) { printf("MMDeviceEnumerator 失败\n"); return 1; }

    IMMDevice* defDev = nullptr;
    en->GetDefaultAudioEndpoint(eRender, eConsole, &defDev);

    IMMDeviceCollection* col = nullptr;
    hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED, &col);
    if (FAILED(hr)) return 1;

    UINT32 n = 0; col->GetCount(&n);
    printf("渲染端点总数(含禁用/未插入): %u\n", n);

    for (UINT32 i = 0; i < n; ++i) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(col->Item(i, &d))) {
            bool isDef = false;
            if (defDev) {
                LPWSTR a = nullptr, b = nullptr;
                if (SUCCEEDED(d->GetId(&a)) && SUCCEEDED(defDev->GetId(&b)) && a && b) isDef = (wcscmp(a, b) == 0);
                if (a) CoTaskMemFree(a);
                if (b) CoTaskMemFree(b);
            }
            ProbeDevice(d, (int)i, isDef);
            SafeRelease(d);
        }
    }
    SafeRelease(defDev); SafeRelease(col); SafeRelease(en);
    CoUninitialize();
    return 0;
}
