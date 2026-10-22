// wasapi_ex_probe.cpp — 独占模式「格式 × 配方」矩阵探测（只 Initialize，不 Start，不出声）
//
// 为什么需要它：`exclusive_rates()` 在能力检查阶段就用 IsFormatSupported 把 MOTU
// 判成"独占支持 16/24/32-bit 全无"，于是永远走不到 Initialize —— 而同一台机器上
// 别的播放器独占正常（用户实测）。所以必须一次弄清：到底哪种格式结构被接受。
// 本探针把「格式结构」与「Initialize 配方」拆成两个维度分别打，只读不出声。
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstring>
#include <string>

static const GUID SUB_PCM = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID SUB_FLT = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

static std::string name_of(IMMDevice* d) {
    IPropertyStore* ps = nullptr;
    std::string out = "(?)";
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
            char b[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, b, sizeof(b) - 1, nullptr, nullptr);
            out = b;
        }
        PropVariantClear(&v); ps->Release();
    }
    return out;
}

struct Spec {
    const char* tag;
    bool ext;        // true = WAVEFORMATEXTENSIBLE
    int  container;  // wBitsPerSample
    int  valid;      // wValidBitsPerSample（仅 ext）
    bool flt;        // SubFormat = float
};

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const char* want = (argc > 1) ? argv[1] : "UltraLite";
    const bool streamTest = (argc > 2 && !std::strcmp(argv[2], "--stream"));
    const bool pollMode = (argc > 3 && !std::strcmp(argv[3], "--poll"));
    bool perZero = false;   // true = 对齐重试时 periodicity 传 0（驱动默认周期）
    for (int i = 3; i < argc; ++i) if (!std::strcmp(argv[i], "--per0")) perZero = true;

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) {
        printf("CoCreateInstance(MMDeviceEnumerator) 失败\n"); return 2;
    }
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) || !col) {
        printf("EnumAudioEndpoints 失败\n"); return 2;
    }
    UINT n = 0; col->GetCount(&n);
    IMMDevice* dev = nullptr;
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d)) || !d) continue;
        const std::string nm = name_of(d);
        if (nm.find(want) != std::string::npos) { dev = d; printf("端点: %s\n", nm.c_str()); break; }
        d->Release();
    }
    if (!dev) { printf("没找到名字含 \"%s\" 的活动端点\n", want); return 2; }

    // ===== 流测试：用 RLPlayer 实际使用的参数走完 Initialize→SetEventHandle→Start→写静音，
    //       观察 GetCurrentPadding 是否下降。全程写 0（数字静音），不产生可听声音。=====
    if (streamTest) {
        uint32_t r = 44100;
        uint32_t bufMs = 20;
        for (int i = 3; i < argc; ++i) {
            const int v = std::atoi(argv[i]);
            if (v <= 0) continue;
            if (v > 1000) r = (uint32_t)v;    // >1000 视为采样率
            else          bufMs = (uint32_t)v; // 否则视为缓冲毫秒
        }
        printf("流测试: %u Hz, 请求缓冲 %u ms\n", (unsigned)r, (unsigned)bufMs);
        const int ch = 2, cont = 4, valid = 24;
        WAVEFORMATEXTENSIBLE wfx{};
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels = (WORD)ch;
        wfx.Format.nSamplesPerSec = r;
        wfx.Format.wBitsPerSample = (WORD)(cont * 8);
        wfx.Format.nBlockAlign = (WORD)(ch * cont);
        wfx.Format.nAvgBytesPerSec = r * wfx.Format.nBlockAlign;
        wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = (WORD)valid;
        wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfx.SubFormat = SUB_PCM;

        IAudioClient* ac = nullptr;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || !ac) { printf("Activate 失败\n"); return 2; }
        REFERENCE_TIME dur = (REFERENCE_TIME)bufMs * 10000;   // 毫秒 → 100ns 单位
        const DWORD flags = pollMode ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        printf("模式: %s\n", pollMode ? "轮询（无 EVENTCALLBACK）" : "事件驱动（EVENTCALLBACK）");
        HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                                    dur, dur, (WAVEFORMATEX*)&wfx, nullptr);
        printf("Initialize(20ms,per=dur) -> 0x%08lX\n", (unsigned long)(unsigned)hr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 frames = 0;
            if (SUCCEEDED(ac->GetBufferSize(&frames)) && frames) {
                REFERENCE_TIME d2 = (REFERENCE_TIME)(10000.0 * 1000.0 * (double)frames / (double)r + 0.5);
                ac->Release(); ac = nullptr;
                if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || !ac) { printf("重新 Activate 失败\n"); return 2; }
                hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                                    d2, perZero ? 0 : d2, (WAVEFORMATEX*)&wfx, nullptr);
                printf("对齐 %u 帧后 Initialize(periodicity=%s) -> 0x%08lX\n",
                       (unsigned)frames, perZero ? "0" : "对齐值", (unsigned long)(unsigned)hr);
            }
        }
        if (hr != S_OK) { printf("Initialize 判定失败，终止\n"); return 2; }

        // ★ 2026-10-23 用户观察：独占播放时声卡面板显示 44.1，中断后回到 48。
        // 说明硬件**确实被切到了 44.1**（"硬件锁 48k"的推断被证伪）。那么不消费的原因
        // 很可能是**切率时序**：Initialize 期间驱动切时钟（MOTU 要重锁 PLL），
        // 而我们在切率尚未稳定时就 Start。这里插一段等待并重读缓冲/周期来验证。
        { UINT32 b0 = 0; ac->GetBufferSize(&b0);
          REFERENCE_TIME p0 = 0, m0 = 0; ac->GetDevicePeriod(&p0, &m0);
          printf("Initialize 后立即: 缓冲=%u 帧, 周期=%.2f ms\n", (unsigned)b0, p0 / 10000.0); }
        Sleep(600);
        { UINT32 b1 = 0; ac->GetBufferSize(&b1);
          REFERENCE_TIME p1 = 0, m1 = 0; ac->GetDevicePeriod(&p1, &m1);
          printf("等待 600ms 后: 缓冲=%u 帧, 周期=%.2f ms  ← 若这里数值变了，就是切率时序问题\n",
                 (unsigned)b1, p1 / 10000.0); }

        UINT32 bufFrames = 0; ac->GetBufferSize(&bufFrames);
        { REFERENCE_TIME p = 0, mp = 0; ac->GetDevicePeriod(&p, &mp);
          printf("缓冲=%u 帧，设备周期=%.2f ms / 最小 %.2f ms\n", (unsigned)bufFrames,
                 p / 10000.0, mp / 10000.0); }

        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!pollMode) {
            hr = ac->SetEventHandle(ev);
            printf("SetEventHandle -> 0x%08lX\n", (unsigned long)(unsigned)hr);
        }
        IAudioRenderClient* rc = nullptr;
        if (FAILED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc)) || !rc) { printf("GetService(RenderClient) 失败\n"); return 2; }

        UINT32 pad0 = 0; ac->GetCurrentPadding(&pad0);
        printf("Start 前 pad=%u\n", (unsigned)pad0);
        hr = ac->Start();
        printf("Start -> 0x%08lX\n", (unsigned long)(unsigned)hr);

        for (int i = 0; i < 8; ++i) {
            UINT32 pad = 0; ac->GetCurrentPadding(&pad);
            const DWORD w = WaitForSingleObject(ev, 50);
            UINT32 pad2 = 0; ac->GetCurrentPadding(&pad2);
            UINT32 room = (pad2 < bufFrames) ? (bufFrames - pad2) : 0;
            // 关键探针：首次即使 pad==bufferFrames 也强制试一次 GetBuffer。
            // 如果成功，说明「Start 后 pad 报满」是驱动语义（不是真的满），
            // 我们原代码用 pad 判断首写就会永远不写 —— 这正是独占"无响应"的机制。
            if (room == 0 && i == 0) { room = bufFrames; printf("  [首次强制试 GetBuffer(%u)]\n", (unsigned)room); }
            size_t wrote = 0;
            if (room > 0) {
                BYTE* data = nullptr;
                if (SUCCEEDED(rc->GetBuffer(room, &data)) && data) {
                    std::memset(data, 0, (size_t)room * ch * cont);   // 数字静音
                    if (SUCCEEDED(rc->ReleaseBuffer(room, 0))) wrote = room;
                }
            }
            printf("  轮 %d: pad=%u -> 等待(0x%lX) -> pad=%u 写=%zu\n",
                   i, (unsigned)pad, (unsigned long)w, (unsigned)pad2, wrote);
            Sleep(20);
        }
        ac->Stop(); ac->Release(); if (rc) rc->Release(); CloseHandle(ev);
        printf("（全程写的是全零静音，没有任何可听声音）\n");
        dev->Release(); col->Release(); en->Release(); CoUninitialize();
        return 0;
    }

    const Spec specs[] = {
        { "PCM16(WAVEFORMATEX)",        false, 16, 16, false },
        { "EXT16/16",                   true,  16, 16, false },
        { "EXT24/24(紧凑3字节)",         true,  24, 24, false },
        { "EXT32容器/24有效",            true,  32, 24, false },
        { "EXT32/32(int)",              true,  32, 32, false },
        { "EXT32/32(float)",            true,  32, 32, true  },
    };
    const uint32_t rates[] = { 44100, 48000 };
    const int ch = 2;
    const DWORD mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    const REFERENCE_TIME dur = 1000000;   // 100 ms

    printf("\n%-24s %6s  %-14s %-14s %-14s\n", "格式", "采样率",
           "IsFormatSupported", "Init(per=dur)", "Init(per=0)");
    printf("--------------------------------------------------------------------------------\n");
    for (const auto& s : specs) {
        for (uint32_t r : rates) {
            WAVEFORMATEXTENSIBLE wfx{};
            const int ba = ch * s.container / 8;
            if (s.ext) {
                wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
                wfx.Format.nChannels       = (WORD)ch;
                wfx.Format.nSamplesPerSec  = r;
                wfx.Format.wBitsPerSample  = (WORD)s.container;
                wfx.Format.nBlockAlign     = (WORD)ba;
                wfx.Format.nAvgBytesPerSec = r * (uint32_t)ba;
                wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                wfx.Samples.wValidBitsPerSample = (WORD)s.valid;
                wfx.dwChannelMask          = mask;
                wfx.SubFormat              = s.flt ? SUB_FLT : SUB_PCM;
            } else {
                wfx.Format.wFormatTag      = WAVE_FORMAT_PCM;
                wfx.Format.nChannels       = (WORD)ch;
                wfx.Format.nSamplesPerSec  = r;
                wfx.Format.wBitsPerSample  = (WORD)s.container;
                wfx.Format.nBlockAlign     = (WORD)ba;
                wfx.Format.nAvgBytesPerSec = r * (uint32_t)ba;
                wfx.Format.cbSize          = 0;
            }
            IAudioClient* ac = nullptr;
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || !ac) {
                printf("%-24s %6u  Activate 失败\n", s.tag, r); continue;
            }
            const HRESULT h1 = ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&wfx, nullptr);

            // 配方 1：periodicity = bufferDuration
            HRESULT h2 = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        dur, dur, (WAVEFORMATEX*)&wfx, nullptr);
            // 配方 5（关键）：BUFFER_SIZE_NOT_ALIGNED → 取实际缓冲帧数重算时长，
            // 并且**必须 Release 旧 client、重新 Activate**（微软文档的强制要求；
            // 复用同一个 IAudioClient 重试会返回 AUDCLNT_E_BUFFER_SIZE_ERROR 0x88890016，
            // 这正是 wasapi_out.h 里那条"配方 5"从来没生效过的原因）。
            HRESULT h5 = E_FAIL;
            REFERENCE_TIME dur2 = 0;
            if (h2 == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 frames = 0;
                if (SUCCEEDED(ac->GetBufferSize(&frames)) && frames) {
                    dur2 = (REFERENCE_TIME)(10000.0 * 1000.0 * (double)frames / (double)r + 0.5);
                    ac->Release();
                    ac = nullptr;
                    IAudioClient* ac2 = nullptr;
                    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac2)) && ac2) {
                        h5 = ac2->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             dur2, dur2, (WAVEFORMATEX*)&wfx, nullptr);
                        ac2->Release();
                    }
                }
            }
            if (h2 == S_OK) { ac->Release(); ac = nullptr;
                dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac); }
            // 配方 2：periodicity = 0（驱动默认周期）
            HRESULT h3 = E_FAIL;
            if (ac) h3 = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        dur, 0, (WAVEFORMATEX*)&wfx, nullptr);
            printf("%-24s %6u  0x%08lX %-6s 0x%08lX %-6s 0x%08lX %-6s | 对齐重试 0x%08lX %s\n",
                   s.tag, r,
                   (unsigned long)(unsigned)h1, h1 == S_OK ? "OK" : "",
                   (unsigned long)(unsigned)h2, h2 == S_OK ? "OK" : "",
                   (unsigned long)(unsigned)h3, h3 == S_OK ? "OK" : "",
                   (unsigned long)(unsigned)h5, h5 == S_OK ? "OK ← 可用" : "");
            if (ac) ac->Release();
        }
    }
    dev->Release(); col->Release(); en->Release();
    CoUninitialize();
    printf("\n（只做了 Initialize，没有 Start、没有写数据、没有声音）\n");
    return 0;
}
