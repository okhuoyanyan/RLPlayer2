// asio_probe.cpp — ASIO 后端的分步验证探针（不发声）
//
// 为什么要分步：ASIO 是一套固定顺序的虚函数表，一旦顺序错，调用会直接崩。
// 所以先只走前面几项（创建 / 取名 / 声道 / 缓冲 / 采样率），确认不崩、返回值合理，
// 再用 --stream 验证 createBuffers + start + 回调。全程送零样本，不产生可听声音。
//
// 用法：
//   asio_probe                列出驱动与能力（只读，不创建缓冲）
//   asio_probe --dsd          同上，并追加【原生 DSD 取证】：DSD 采样率、通道类型码、
//                             ASIOFuture(kAsioGetIoFormat / kAsioCanDoIoFormat) 的原始返回
//   asio_probe --stream <n>   对第 n 个驱动做 createBuffers + start + 1 秒静音推送
//   asio_probe --dsd-create <n|--clsid X> [--rate 2822400] [--ch 2] [--ms 1000]
//                             真机：切 kASIODSDFormat → 设 DSD 位率 → createBuffers →
//                             start → 推【全零 DSD 字节】→ stop/dispose → 恢复 PCM。
//                             每一步都打印原始 ASE 码，绝不只是 OK/FAIL。
//
// ⚠️ 判据（铁律 3）：ASIO 的 "DSD 能力" 不是靠猜类型码，而是看三点同时成立：
//   (1) ASIOFuture(kAsioSetIoFormat, kASIODSDFormat) 返回 ASE_SUCCESS(0x3f4847a0) 且 formatType 仍是 1；
//   (2) 切完之后 getChannelInfo 报的 type 是 ASIOSTDSDInt8LSB1(32) 或 MSB1(33)；
//   (3) canSampleRate(DSD 位率) = ASE_OK 且 ASIOCreateBuffers 返回 ASE_OK。
//   只要有一条不成立，就不是原生 DSD（可能只是 DoP 伪装）。
#include "audio/asio_out.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <atomic>
#include <string>
#include <vector>

using namespace rta::audio;

static const char* type_str(ASIOSampleType t) {
    switch (t) {
        case ASIOSTDSDInt8LSB1: return "DSDInt8LSB1";
        case ASIOSTDSDInt8MSB1: return "DSDInt8MSB1";
        case ASIOSTDSDInt8NER8: return "DSDInt8NER8";
        default: return asio_sample_type_name(t);
    }
}

// 原始返回码一律双写（十进制 + 十六进制）：ASE_SUCCESS 是 0x3f4847a0，
// 只打十进制会被当成"某个大正数"，而只打 OK/FAIL 会掩盖 -1000 与 -998 的区别。
static std::string ase(long v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%ld (0x%08lx)", v, (unsigned long)v);
    return b;
}

// ---------------- 回调（探针自己用；回调里把缓冲填零 = 静音） ----------------
static std::atomic<long> g_cbCount{0};
static std::atomic<unsigned long long> g_firstCb{0}, g_lastCb{0};
static ASIOBufferInfo* g_bufs = nullptr;
static long g_ch = 0, g_bufUnits = 0, g_unitBytes = 1;

static long g_fillBytes = 0;    // >0 时覆盖"每次回调写多少字节"（诊断用）
static std::atomic<int> g_cbLog{0};
static void probe_fill(long idx) {
    const unsigned long long now = GetTickCount64();
    if (g_cbCount.load() == 0) g_firstCb = now;
    g_lastCb = now;
    const long n = ++g_cbCount;
    if (!g_bufs) {
        if (n <= 3) { printf("     [cb %ld] idx=%ld （nop：不写缓冲）\n", n, idx); fflush(stdout); }
        return;
    }
    if (g_cbLog.fetch_add(1) < 3)
        printf("     [cb %ld] idx=%ld ch=%ld buf0=%p buf1=%p 每声道写 %ld 字节\n",
               n, idx, g_ch, g_bufs[0].buffers[0], g_bufs[0].buffers[1],
               g_bufUnits * g_unitBytes);
    const size_t nb = g_fillBytes > 0 ? (size_t)g_fillBytes
                                     : (size_t)g_bufUnits * (size_t)g_unitBytes;
    for (long c = 0; c < g_ch; ++c) {
        void* p = g_bufs[c].buffers[idx];
        if (p) std::memset(p, 0, nb);
    }
}
static void probe_switch(long idx, ASIOBool) { probe_fill(idx); }
static ASIOTime* probe_switch_time(ASIOTime* t, long idx, ASIOBool) { probe_fill(idx); return t; }
static void probe_rate_changed(ASIOSampleRate) {}
static long probe_messages(long selector, long value, void*, double*) {
    enum { kSelSupported = 1, kEngineVersion = 2, kResetRequest = 3,
           kBufferSizeChange = 4, kResyncRequest = 5, kLatenciesChanged = 6,
           kSupportsTimeInfo = 7, kSupportsTimeCode = 8, kOverload = 15 };
    switch (selector) {
        case kSelSupported:
            switch (value) {
                case kEngineVersion: case kResetRequest: case kBufferSizeChange:
                case kResyncRequest: case kLatenciesChanged: case kOverload:
                    return ASIOTrue;
                default: return ASIOFalse;
            }
        case kEngineVersion:    return 2;
        case kSupportsTimeInfo: return ASIOFalse;
        case kSupportsTimeCode: return ASIOFalse;
        case kResetRequest:     return ASIOTrue;
        case kOverload:         return ASIOFalse;
        default:                return 0;
    }
}

// 崩溃定位：ASIO 驱动在 DSD 会话里会访问冲突（本机 AudioByte 实测），
// SEH 护栏只挡得住主线程上被包住的那几次调用；驱动自己线程里的崩要另外看见。
// 这里挂一个向量化异常处理器，把异常码 / 出错地址 / 所属模块 / 线程序号打出来再放行。
static LONG WINAPI probe_veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363 || code == 0x406D1388) return EXCEPTION_CONTINUE_SEARCH;  // C++ 异常/线程命名
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {0};
    char modName[260] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod) {
        GetModuleFileNameW(mod, path, MAX_PATH);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, modName, sizeof(modName) - 1, nullptr, nullptr);
    }
    char b[512];
    std::snprintf(b, sizeof(b),
                  "\n!!! 异常 0x%08lx @ %p (%s) 线程=%lu 参数0=%llu\n",
                  (unsigned long)code, addr, modName, GetCurrentThreadId(),
                  (unsigned long long)(ep->ExceptionRecord->NumberParameters > 0
                                       ? ep->ExceptionRecord->ExceptionInformation[0] : 0));
    fputs(b, stdout);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

// 试写 n 字节：AV 就说明这个长度超过驱动实际给我们的缓冲（ASIO 没有 API 能问"缓冲多少字节"）
static int try_fill_bytes(void* p, size_t n) {
    ASIO_GUARD_BEGIN
    std::memset(p, 0, n);
    return 0;
    ASIO_GUARD_END(1)
}

// ---------------- 只读探测 ----------------
// withDsd=true 时追加 DSD 取证。全部只读：不 setSampleRate、不 createBuffers、不开流。
static bool probe_one(const AsioDriverEntry& e, int idx, bool withDsd) {
    printf("\n[%d] %s\n     CLSID %s\n", idx, e.name.c_str(), e.clsid.c_str());
    wchar_t wc[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, e.clsid.c_str(), -1, wc, 127);
    CLSID clsid{};
    if (FAILED(CLSIDFromString(wc, &clsid))) { printf("     CLSID 解析失败\n"); return false; }

    IASIO* d = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&d);
    if (FAILED(hr) || !d) { printf("     CoCreateInstance 失败 0x%08lx\n", (unsigned long)hr); return false; }
    printf("     对象创建 OK（vtable 基址已拿到）\n");

    ASIOBool inited = asio_guarded_init(d, nullptr);
    printf("     init -> %ld\n", (long)inited);
    if (!inited) { d->Release(); return false; }

    char nm[64] = {0};
    d->getDriverName(nm);
    printf("     getDriverName -> \"%s\"  (getDriverVersion -> %ld)\n", nm, d->getDriverVersion());

    long inCh = 0, outCh = 0;
    const ASIOError gc = d->getChannels(&inCh, &outCh);
    printf("     getChannels -> err=%ld  in=%ld out=%ld\n", (long)gc, inCh, outCh);
    if (gc != ASE_OK) { d->Release(); return false; }

    long mn = 0, mx = 0, pref = 0, gran = 0;
    const ASIOError gb = d->getBufferSize(&mn, &mx, &pref, &gran);
    printf("     getBufferSize -> err=%ld  min=%ld max=%ld pref=%ld gran=%ld\n",
           (long)gb, mn, mx, pref, gran);

    long latIn = 0, latOut = 0;
    if (d->getLatencies(&latIn, &latOut) == ASE_OK)
        printf("     getLatencies -> in=%ld out=%ld\n", latIn, latOut);

    printf("     采样率支持（PCM）：");
    const double rates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600};
    for (double r : rates) {
        if (d->canSampleRate(r) == ASE_OK) printf("%u ", (unsigned)r);
    }
    printf("\n");

    for (long c = 0; c < outCh && c < 8; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = c; ci.isInput = ASIOFalse;
        if (asio_guarded_channel_info(d, &ci) == ASE_OK)
            printf("     out ch%ld: %-14s type=%-2ld %-12s group=%ld active=%ld\n",
                   c, std::string(ci.name, strnlen(ci.name, 32)).c_str(), (long)ci.type,
                   type_str(ci.type), ci.channelGroup, (long)ci.isActive);
    }

    if (withDsd) {
        // ---- DSD 采样率（只读）----
        printf("     DSD 采样率支持（canSampleRate 原始返回）：\n");
        const double dsdRates[] = {2822400, 5644800, 11289600, 22579200, 45158400};
        for (double r : dsdRates) {
            const ASIOError cr = d->canSampleRate(r);
            printf("       %9.0f Hz (%3.0f x 44.1k) -> %s%s\n", r, r / 44100.0, ase(cr).c_str(),
                   cr == ASE_OK ? "  <= 接受" : "");
        }
        // ---- IoFormat（只读查询）----
        ASIOIoFormat g{};
        g.formatType = kASIOFormatInvalid;
        const void* rg = asio_guarded_future(d, kAsioGetIoFormat, &g);
        printf("     ASIOFuture(kAsioGetIoFormat=0x%08lx) -> raw=%p(%s) formatType=%ld\n",
               (unsigned long)kAsioGetIoFormat, rg, ase((long)(intptr_t)rg).c_str(), g.formatType);

        ASIOIoFormat cdf{};
        cdf.formatType = kASIODSDFormat;
        const void* rc = asio_guarded_future(d, kAsioCanDoIoFormat, &cdf);
        printf("     ASIOFuture(kAsioCanDoIoFormat=0x%08lx, {DSD=1}) -> raw=%p(%s) formatType=%ld%s\n",
               (unsigned long)kAsioCanDoIoFormat, rc, ase((long)(intptr_t)rc).c_str(), cdf.formatType,
               ((long)(intptr_t)rc == ASE_SUCCESS && cdf.formatType == kASIODSDFormat)
                   ? "  <= 支持 DSD" : "  (不支持/未实现)");
        printf("     （只读取证：没有切格式、没有建缓冲、没有开流）\n");
    }
    d->Release();
    return true;
}

// ---------------- 真机 DSD 全流程（原始调用 + 原始返回码） ----------------
struct DsdTestOpts {
    double dsdRate = 2822400.0;
    int ch = 2;
    int ms = 1000;
    long bufBytes = 0;      // 0 = 用驱动报的 pref
    bool rateFirst = false; // 先设采样率再切 IoFormat（SDK 顺序是先切格式）
    bool outReady = false;  // 每次回调后从主线程补 ASIOOutputReady()
    bool nopCb = false;     // 回调里不写缓冲（排查"写缓冲本身把驱动搞挂"）
    bool allCh = false;     // 建全部输出声道
    bool spin = false;      // 主线程自旋（诊断"是不是被流线程饿死"）
    bool probeBuf = false;  // 试写探测真实缓冲字节数
    long fillBytes = 0;     // >0 时覆盖每次回调写入的字节数
    void* hwnd = nullptr;
};

static int dsd_create_test(const AsioDriverEntry& e, const DsdTestOpts& o) {
    const double dsdRate = o.dsdRate;
    printf("\n=== 原生 DSD 全流程取证：%s ===\n", e.name.c_str());
    printf("    目标：kAsioSetIoFormat(kASIODSDFormat) → 采样率 %.0f Hz → createBuffers → start\n", dsdRate);
    printf("    参数：ch=%d%s buf=%ld(0=驱动 pref) rateFirst=%d outReady=%d nopCb=%d ms=%d\n",
           o.ch, o.allCh ? "(全部)" : "", o.bufBytes, (int)o.rateFirst, (int)o.outReady, (int)o.nopCb, o.ms);
    printf("    ⚠️ 会打开该驱动并推【全零 DSD 字节】%d ms（全零 = 恒定电平 = 数字静音，不产生可听信号）\n", o.ms);
    printf("    ⚠️ ASIO 独占：运行期间系统其他声音会被该设备接管\n");

    wchar_t wc[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, e.clsid.c_str(), -1, wc, 127);
    CLSID clsid{};
    if (FAILED(CLSIDFromString(wc, &clsid))) { printf("     CLSID 解析失败\n"); return 1; }
    IASIO* d = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&d);
    if (FAILED(hr) || !d) { printf("     CoCreateInstance 失败 0x%08lx\n", (unsigned long)hr); return 1; }

    const ASIOBool inited = asio_guarded_init(d, o.hwnd);
    printf("[1] init(hwnd=%p) -> %ld\n", o.hwnd, (long)inited);
    if (!inited) { d->Release(); return 1; }
    char nm[64] = {0};
    d->getDriverName(nm);
    printf("    getDriverName -> \"%s\" version=%ld\n", nm, d->getDriverVersion());

    // PCM 现状（对照）
    long inCh = 0, outCh = 0, mn = 0, mx = 0, pref = 0, gran = 0;
    printf("[2] getChannels -> err=%ld  ", (long)d->getChannels(&inCh, &outCh));
    printf("in=%ld out=%ld\n", inCh, outCh);
    printf("    getBufferSize -> err=%ld min=%ld max=%ld pref=%ld gran=%ld\n",
           (long)d->getBufferSize(&mn, &mx, &pref, &gran), mn, mx, pref, gran);
    ASIOSampleRate cur = 0.0;
    printf("    getSampleRate -> err=%ld rate=%.0f\n", (long)d->getSampleRate(&cur), (double)cur);

    ASIOIoFormat f{};
    void* rr = nullptr;
    if (o.rateFirst) {
        const ASIOError cr0 = d->canSampleRate(dsdRate);
        const ASIOError sr0 = d->setSampleRate(dsdRate);
        ASIOSampleRate rb0 = 0.0;
        const ASIOError gr0 = d->getSampleRate(&rb0);
        printf("[2b] rateFirst：canSampleRate(%.0f)=%s setSampleRate=%s getSampleRate=%s rate=%.0f\n",
               dsdRate, ase((long)cr0).c_str(), ase((long)sr0).c_str(), ase((long)gr0).c_str(), (double)rb0);
    }
    // DSD 切换
    f.formatType = kASIODSDFormat;
    rr = (void*)asio_guarded_future(d, kAsioCanDoIoFormat, &f);
    printf("[3] ASIOFuture(kAsioCanDoIoFormat, {DSD=1}) -> raw=%p %s  formatType=%ld\n",
           rr, ase((long)(intptr_t)rr).c_str(), f.formatType);
    f.formatType = kASIODSDFormat;
    rr = (void*)asio_guarded_future(d, kAsioSetIoFormat, &f);
    printf("[4] ASIOFuture(kAsioSetIoFormat,   {DSD=1}) -> raw=%p %s  formatType=%ld%s\n",
           rr, ase((long)(intptr_t)rr).c_str(), f.formatType,
           (f.formatType == kASIOFormatInvalid) ? "  <= 驱动把请求改成 Invalid（拒绝 DSD）" : "");
    ASIOIoFormat g{};
    g.formatType = kASIOFormatInvalid;
    rr = (void*)asio_guarded_future(d, kAsioGetIoFormat, &g);
    printf("[5] ASIOFuture(kAsioGetIoFormat)              -> raw=%p %s  formatType=%ld\n",
           rr, ase((long)(intptr_t)rr).c_str(), g.formatType);

    // 切换后的声道 / 缓冲
    long inCh2 = 0, outCh2 = 0;
    const ASIOError gc2 = d->getChannels(&inCh2, &outCh2);
    printf("[6] getChannels(DSD 模式) -> err=%ld in=%ld out=%ld\n", (long)gc2, inCh2, outCh2);
    long mn2 = 0, mx2 = 0, pref2 = 0, gran2 = 0;
    const ASIOError gb2 = d->getBufferSize(&mn2, &mx2, &pref2, &gran2);
    printf("    getBufferSize(DSD 模式) -> err=%ld min=%ld max=%ld pref=%ld gran=%ld\n",
           (long)gb2, mn2, mx2, pref2, gran2);

    // DSD 位率
    if (!o.rateFirst) {
        const ASIOError cr = d->canSampleRate(dsdRate);
        const ASIOError sr = d->setSampleRate(dsdRate);
        ASIOSampleRate rb = 0.0;
        const ASIOError gr = d->getSampleRate(&rb);
        printf("[7] canSampleRate(%.0f) -> %s\n", dsdRate, ase((long)cr).c_str());
        printf("    setSampleRate(%.0f) -> %s\n", dsdRate, ase((long)sr).c_str());
        printf("    getSampleRate       -> %s  rate=%.0f\n", ase((long)gr).c_str(), (double)rb);
    }

    int ch = (o.ch > 0 && o.ch <= outCh2) ? o.ch : (int)outCh2;
    if (o.allCh) ch = (int)outCh2;
    const long bufBytes = o.bufBytes > 0 ? o.bufBytes : (pref2 > 0 ? pref2 : (mx2 > 0 ? mx2 : 512));
    const long writeBytes = bufBytes / 8 > 0 ? bufBytes / 8 : 1;
    printf("[8] 采用：%d 声道；getBufferSize 报 %ld（单位=1-bit 样本）⟹ 每回调实际写 %ld 字节/声道"
           "（%.1f ms @ %.0f Hz）\n",
           ch, bufBytes, writeBytes, (double)writeBytes * 8.0 * 1000.0 / dsdRate, dsdRate);
    if (ch <= 0 || ch > 32) { printf("    !! 声道数不合理，中止\n"); d->Release(); return 2; }

    ASIOBufferInfo bufs[32];
    for (int c = 0; c < ch; ++c) {
        bufs[c].isInput = ASIOFalse;
        bufs[c].channelNum = c;
        bufs[c].buffers[0] = bufs[c].buffers[1] = nullptr;
    }
    ASIOCallbacks cb{};
    cb.bufferSwitch = &probe_switch;
    cb.sampleRateDidChange = &probe_rate_changed;
    cb.asioMessage = &probe_messages;
    cb.bufferSwitchTimeInfo = &probe_switch_time;

    const ASIOError cberr = asio_guarded_create_buffers(d, bufs, ch, bufBytes, &cb);
    printf("[9] ASIOCreateBuffers(%d ch, %ld bytes, cb) -> %s%s\n",
           ch, bufBytes, ase((long)cberr).c_str(), cberr == ASE_OK ? "  <= ASE_OK" : "  !! 不是 ASE_OK");
    if (cberr != ASE_OK) {
        ASIOIoFormat pf{}; pf.formatType = kASIOPCMFormat;
        asio_guarded_future(d, kAsioSetIoFormat, &pf);
        d->Release();
        return 3;
    }

    // createBuffers 之后重查类型（关键判据：必须是 DSD 类型）
    printf("[10] createBuffers 之后 getChannelInfo：\n");
    bool allDsd = true;
    ASIOSampleType firstType = -1;
    for (int c = 0; c < ch; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = c; ci.isInput = ASIOFalse;
        const ASIOError err = asio_guarded_channel_info(d, &ci);
        if (c == 0) firstType = ci.type;
        if (!asio_is_dsd_1bit(ci.type)) allDsd = false;
        if (c < 4 || !asio_is_dsd_1bit(ci.type))
            printf("     ch%-2d getChannelInfo -> err=%ld type=%-2ld %-12s name=\"%s\" active=%ld\n",
                   c, (long)err, (long)ci.type, type_str(ci.type),
                   std::string(ci.name, strnlen(ci.name, 32)).c_str(), (long)ci.isActive);
    }
    printf("     => 全部声道都是 1-bit DSD 类型：%s（类型码 %ld；LSB1=%d / MSB1=%d）\n",
           allDsd ? "是" : "否", (long)firstType,
           (int)ASIOSTDSDInt8LSB1, (int)ASIOSTDSDInt8MSB1);

    // ---- 到底能写多少字节？ASIO 没有 API 能问，只能实测 ----
    // 本机 AudioByte 的关键坑：DSD 模式下 getBufferSize 报 32768，但按字节直接写 32768 会 AV。
    // 先看驱动给的两块内存各自的虚拟内存区域，再逐个长度试写，边界就是真实可写范围。
    for (int c = 0; c < ch && c < 2; ++c) {
        for (int b = 0; b < 2; ++b) {
            void* p = bufs[c].buffers[b];
            if (!p) { printf("     [内存] ch%d buf%d = null\n", c, b); continue; }
            MEMORY_BASIC_INFORMATION mbi{};
            const SIZE_T q = VirtualQuery(p, &mbi, sizeof(mbi));
            printf("     [内存] ch%d buf%d=%p VirtualQuery=%llu base=%p region=%llu state=0x%lx protect=0x%lx\n",
                   c, b, p, (unsigned long long)q, mbi.AllocationBase,
                   (unsigned long long)mbi.RegionSize, (unsigned long)mbi.State, (unsigned long)mbi.Protect);
        }
        if (bufs[c].buffers[0] && bufs[c].buffers[1])
            printf("     [内存] ch%d 两个 double-buffer 相距 %lld 字节\n", c,
                   (long long)((char*)bufs[c].buffers[1] - (char*)bufs[c].buffers[0]));
    }
    if (o.probeBuf && bufs[0].buffers[0]) {
        const size_t cand[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536};
        printf("     [试写] 从 ch0 buf0 起逐级 memset（AV = 超过驱动实际缓冲）：\n");
        for (size_t n : cand) {
            const int bad = try_fill_bytes(bufs[0].buffers[0], n);
            printf("       %6zu 字节 -> %s\n", n, bad ? "AV（越界）" : "OK");
            if (bad) break;
        }
    }

    // 开流 + 推全零 DSD
    g_cbCount = 0; g_firstCb = 0; g_lastCb = 0;
    g_bufs = o.nopCb ? nullptr : bufs;
    g_ch = ch; g_bufUnits = bufBytes; g_unitBytes = 1;   // DSD：1 字节 = 8 样本
    g_fillBytes = o.fillBytes;
    ASIOSamples s0{}; ASIOTimeStamp t0{};
    const ASIOError sp0 = d->getSamplePosition(&s0, &t0);
    const unsigned long long pos0 = ((unsigned long long)s0.hi << 32) | s0.lo;
    const ASIOError orErr = d->outputReady();
    printf("[10b] ASIOOutputReady(启动前探测) -> %s（ASE_OK 表示驱动希望宿主每块后调用）\n",
           ase((long)orErr).c_str());

    const ASIOError st = asio_guarded_start(d);
    printf("[11] ASIOStart -> %s%s\n", ase((long)st).c_str(),
           st == ASE_OK ? "" : "  !! 启动失败（驱动可能崩过，已被 SEH 拦下）");
    if (st != ASE_OK) {
        printf("     stop/dispose...\n");
        asio_guarded_stop(d);
        asio_guarded_dispose(d);
        ASIOIoFormat pf{}; pf.formatType = kASIOPCMFormat;
        asio_guarded_future(d, kAsioSetIoFormat, &pf);
        d->Release();
        return 4;
    }

    // 诊断：ASIO 驱动（尤其是 DSD 路径）常把流线程提到 TIME_CRITICAL。
    // 若主线程被饿死，Sleep 会远超预期 —— 所以这两种计时都要打出来。
    printf("     [诊断] 逻辑 CPU=%u，进程优先级类=%lu（0x%lx）\n",
           (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
           (unsigned long)GetPriorityClass(GetCurrentProcess()),
           (unsigned long)GetPriorityClass(GetCurrentProcess()));
    const ULONGLONG tStart = GetTickCount64();
    LARGE_INTEGER fq{}, q0{}, q1{};
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&q0);
    int nextReport = 250;
    long iters = 0;
    while (GetTickCount64() - tStart < (ULONGLONG)o.ms) {
        if (o.spin) {
            for (int k = 0; k < 2000; ++k) { QueryPerformanceCounter(&q1); ++iters; }
        } else {
            Sleep(10);
            ++iters;
        }
        if (o.outReady) d->outputReady();
        const int el = (int)(GetTickCount64() - tStart);
        if (el >= nextReport) {
            QueryPerformanceCounter(&q1);
            printf("     t=%4d ms (QPC %.0f ms)  轮次=%ld  callbacks=%ld  流优先级类=%lu\n",
                   el, (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart,
                   iters, (long)g_cbCount, (unsigned long)GetPriorityClass(GetCurrentProcess()));
            nextReport += 250;
        }
    }
    const double wall = (double)(GetTickCount64() - tStart) / 1000.0;
    ASIOSamples s1{}; ASIOTimeStamp t1{};
    const ASIOError sp1 = d->getSamplePosition(&s1, &t1);
    const unsigned long long pos1 = ((unsigned long long)s1.hi << 32) | s1.lo;

    printf("[12] 运行 %.2f 秒：回调 %ld 次，首/末回调 tick=%llu/%llu，位置 %llu -> %llu（Δ=%lld）\n",
           wall, (long)g_cbCount, (unsigned long long)g_firstCb, (unsigned long long)g_lastCb,
           pos0, pos1, (long long)(pos1 - pos0));
    if (sp1 == ASE_OK && pos1 > pos0 && wall > 0.0) {
        const double perSec = (double)(pos1 - pos0) / wall;
        printf("     每秒位置增量 %.0f；按 DSD 位率应为 %.0f，按 DoP 承载率应为 %.0f\n",
               perSec, dsdRate, dsdRate / 16.0);
    }
    // 回调间隔（判据：DSD 下每块 32768 字节 = 93 ms；若只有 1 次回调，说明流根本没跑起来）
    if (g_cbCount > 1 && g_lastCb > g_firstCb) {
        const double avgMs = (double)(g_lastCb - g_firstCb) / (double)(g_cbCount - 1);
        printf("     平均回调间隔 %.1f ms；理论（每回调 %ld 字节 = %ld bit / %.0f）= %.1f ms\n",
               avgMs, writeBytes, writeBytes * 8, dsdRate,
               (double)writeBytes * 8.0 * 1000.0 / dsdRate);
    }

    printf("[13] ASIOStop ...\n");
    const ASIOError sr2 = asio_guarded_stop(d);
    printf("     ASIOStop -> %s\n", ase((long)sr2).c_str());
    printf("[14] ASIODisposeBuffers ...\n");
    const ASIOError dr2 = asio_guarded_dispose(d);
    printf("     ASIODisposeBuffers -> %s\n", ase((long)dr2).c_str());
    ASIOIoFormat pf{};
    pf.formatType = kASIOPCMFormat;
    rr = (void*)asio_guarded_future(d, kAsioSetIoFormat, &pf);
    printf("[15] 恢复 PCM：kAsioSetIoFormat({PCM=0}) -> raw=%p %s formatType=%ld\n",
           rr, ase((long)(intptr_t)rr).c_str(), pf.formatType);
    g_bufs = nullptr;
    printf("[16] Release ...\n");
    d->Release();
    printf("[17] 完成，没有崩溃\n");
    return 0;
}

// 隐藏窗口：部分 ASIO 驱动的 init() 需要有效 HWND
static HWND make_hidden_window(HINSTANCE hInst) {
    static const wchar_t* kClass = L"AsioProbeWnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    return CreateWindowExW(0, kClass, L"asio-probe", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
}

// 预检：只验证"能不能 open + start"，不推数据。崩溃会带走这个子进程，父进程据此判定。
static int check_test(const AsioDriverEntry& e, double rate, int ch) {
    AsioDriver drv;
    std::string err;
    if (!drv.open(e.clsid, rate, ch, 24, err, nullptr, true)) { printf("open 失败: %s\n", err.c_str()); return 2; }
    if (!drv.start(err)) { printf("start 失败: %s\n", err.c_str()); return 3; }
    drv.stop();
    drv.close();
    printf("OK\n");
    return 0;
}

static int stream_test(const AsioDriverEntry& e, double rate, int ch, void* hwnd, bool nopCb, int ms) {
    printf("\n=== 推流测试（零样本，%d ms）%s @ %.0f Hz / %d ch ===\n", ms, e.name.c_str(), rate, ch);
    AsioDriver drv;
    std::string err;
    if (!drv.open(e.clsid, rate, ch, 24, err, hwnd, nopCb)) { printf("open 失败: %s\n", err.c_str()); return 1; }
    printf("open OK: %s, 声道 %d（驱动输出 %d），缓冲 %ld 帧，outReady=%d\n",
           drv.name().c_str(), drv.channels(), drv.driver_out_channels(),
           drv.buffer_size(), (int)drv.out_ready_supported());
    for (int c = 0; c < drv.channels(); ++c)
        printf("   ch%d type=%ld %s\n", c, (long)drv.channel_type(c), type_str(drv.channel_type(c)));

    if (!drv.start(err)) { printf("start 失败: %s\n", err.c_str()); return 1; }
    std::vector<int32_t> zeros((size_t)drv.buffer_size() * (size_t)drv.channels(), 0);
    const size_t total = (size_t)(rate * (double)ms / 1000.0);
    size_t pushed = 0;
    while (pushed < total) {
        const size_t n = (size_t)drv.buffer_size();
        if (drv.free_frames() < n) { Sleep(2); continue; }
        const size_t before = pushed;
        pushed += drv.push(zeros.data(), n);
        if (before / 16384 != pushed / 16384) printf("  pushed %zu\n", pushed);
    }
    Sleep(300);
    drv.stop();
    printf("推送 %zu 帧，欠载 %llu 次\n", pushed, drv.underruns());
    drv.close();
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);   // 崩溃时也要看到崩溃前打印到哪一步
    AddVectoredExceptionHandler(1, &probe_veh);
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)ci;

    auto list = asio_enum_drivers();
    printf("注册表里 ASIO 驱动 %zu 个：\n", list.size());
    for (size_t i = 0; i < list.size(); ++i) printf("  [%zu] %s\n", i, list[i].name.c_str());
    if (list.empty()) { printf("（没有 ASIO 驱动）\n"); CoUninitialize(); return 1; }

    int streamIdx = -1, dsdIdx = -1;
    double rate = 48000, dsdRate = 2822400.0;
    int ch = 2;
    bool useHwnd = false, nopCb = false, checkOnly = false, withDsd = false;
    bool rateFirst = false, outReady = false, allCh = false, spin = false;
    bool probeBuf = false;
    long bufBytes = 0, fillBytes = 0;
    std::string wantClsid;
    int ms = 1000;
    for (int i = 1; i < argc; ++i) {
        // 只有后面确实跟着数字才当序号吃掉，否则会误吞 --clsid 这类开关（本轮踩到）
        if (!std::strcmp(argv[i], "--stream") && i + 1 < argc && (isdigit((unsigned char)argv[i + 1][0]) || argv[i + 1][0] == '-'))
            streamIdx = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dsd-create") && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0]))
            dsdIdx = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dsd")) withDsd = true;
        else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--dsd-rate") && i + 1 < argc) dsdRate = atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--ch") && i + 1 < argc) ch = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--check")) checkOnly = true;
        else if (!std::strcmp(argv[i], "--clsid") && i + 1 < argc) wantClsid = argv[++i];
        else if (!std::strcmp(argv[i], "--ms") && i + 1 < argc) ms = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--buf") && i + 1 < argc) bufBytes = atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--rate-first")) rateFirst = true;
        else if (!std::strcmp(argv[i], "--outready"))  outReady = true;
        else if (!std::strcmp(argv[i], "--allch"))     allCh = true;
        else if (!std::strcmp(argv[i], "--spin"))      spin = true;
        else if (!std::strcmp(argv[i], "--probe-buf"))  probeBuf = true;
        else if (!std::strcmp(argv[i], "--bytes") && i + 1 < argc) fillBytes = atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--hwnd")) useHwnd = true;
        else if (!std::strcmp(argv[i], "--nop"))  nopCb = true;
    }
    if (!wantClsid.empty()) {
        streamIdx = -1; dsdIdx = -1;
        for (size_t i = 0; i < list.size(); ++i) if (list[i].clsid == wantClsid) { streamIdx = (int)i; }
        if (streamIdx < 0) { printf("找不到 CLSID %s\n", wantClsid.c_str()); return 4; }
        dsdIdx = streamIdx;
    }
    if (ms < 50) ms = 50;
    if (ms > 5000) ms = 5000;      // 探针只做短时取证；长时验证交给 rtaplay
    void* hw = useHwnd ? (void*)make_hidden_window(GetModuleHandleW(nullptr)) : nullptr;
    if (useHwnd) printf("sysHandle=%p\n", hw);

    if (dsdIdx >= 0 && dsdIdx < (int)list.size()) {
        DsdTestOpts o;
        o.dsdRate = dsdRate;
        o.ch = ch;
        o.ms = ms;
        o.bufBytes = bufBytes;
        o.rateFirst = rateFirst;
        o.outReady = outReady;
        o.nopCb = nopCb;
        o.allCh = allCh;
        o.spin = spin;
        o.probeBuf = probeBuf;
        o.fillBytes = fillBytes;
        o.hwnd = hw;
        const int rc = dsd_create_test(list[(size_t)dsdIdx], o);
        CoUninitialize();
        return rc;
    }
    if (checkOnly && streamIdx >= 0 && streamIdx < (int)list.size()) {
        const int rc = check_test(list[(size_t)streamIdx], rate, ch);
        CoUninitialize();
        return rc;
    }
    if (streamIdx >= 0 && streamIdx < (int)list.size()) {
        printf("sysHandle=%p nopCallback=%d\n", hw, (int)nopCb);
        const int rc = stream_test(list[(size_t)streamIdx], rate, ch, hw, nopCb, ms);
        CoUninitialize();
        return rc;
    }
    for (size_t i = 0; i < list.size(); ++i) probe_one(list[i], (int)i, withDsd);
    printf("\n（只做了只读探测，没有创建缓冲、没有开流、没有声音）\n");
    CoUninitialize();
    return 0;
}
