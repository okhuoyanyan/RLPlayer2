// asio_probe.cpp — ASIO 后端的分步验证探针（不发声）
//
// 为什么要分步：ASIO 是一套固定顺序的虚函数表，一旦顺序错，调用会直接崩。
// 所以先只走前面几项（创建 / 取名 / 声道 / 缓冲 / 采样率），确认不崩、返回值合理，
// 再用 --stream 验证 createBuffers + start + 回调。全程送零样本，不产生可听声音。
//
// 用法：
//   asio_probe                列出驱动与能力（只读，不创建缓冲）
//   asio_probe --stream <n>   对第 n 个驱动做 createBuffers + start + 1 秒静音推送
#include "audio/asio_out.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>

using namespace rta::audio;

static const char* type_name(ASIOSampleType t) {
    switch (t) {
        case ASIOSTInt16LSB: return "Int16LSB";
        case ASIOSTInt24LSB: return "Int24LSB";
        case ASIOSTInt32LSB: return "Int32LSB";
        case ASIOSTFloat32LSB: return "Float32LSB";
        case ASIOSTFloat64LSB: return "Float64LSB";
        case ASIOSTInt16MSB: return "Int16MSB";
        case ASIOSTInt24MSB: return "Int24MSB";
        case ASIOSTInt32MSB: return "Int32MSB";
        case ASIOSTFloat32MSB: return "Float32MSB";
        case ASIOSTDSDInt8LSB1: return "DSDInt8LSB1";
        case ASIOSTDSDInt8MSB1: return "DSDInt8MSB1";
        case ASIOSTDSDInt8NER8: return "DSDInt8NER8";
        default: return "?";
    }
}

static bool probe_one(const AsioDriverEntry& e, int idx) {
    printf("\n[%d] %s\n     CLSID %s\n", idx, e.name.c_str(), e.clsid.c_str());
    wchar_t wc[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, e.clsid.c_str(), -1, wc, 127);
    CLSID clsid{};
    if (FAILED(CLSIDFromString(wc, &clsid))) { printf("     CLSID 解析失败\n"); return false; }

    IASIO* d = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&d);
    if (FAILED(hr) || !d) { printf("     CoCreateInstance 失败 0x%08lx\n", (unsigned long)hr); return false; }
    printf("     对象创建 OK（vtable 基址已拿到）\n");

    ASIOBool inited = d->init(nullptr);
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

    printf("     采样率支持：");
    const double rates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600};
    for (double r : rates) {
        if (d->canSampleRate(r) == ASE_OK) printf("%u ", (unsigned)r);
    }
    printf("\n");

    for (long c = 0; c < outCh && c < 8; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = c; ci.isInput = ASIOFalse;
        if (d->getChannelInfo(&ci) == ASE_OK)
            printf("     out ch%ld: %-14s type=%-12s group=%ld active=%ld\n",
                   c, std::string(ci.name, strnlen(ci.name, 32)).c_str(), type_name(ci.type),
                   ci.channelGroup, (long)ci.isActive);
    }
    d->Release();
    return true;
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
    printf("\n=== 推流测试（零样本，1 秒）%s @ %.0f Hz / %d ch ===\n", e.name.c_str(), rate, ch);
    AsioDriver drv;
    std::string err;
    if (!drv.open(e.clsid, rate, ch, 24, err, hwnd, nopCb)) { printf("open 失败: %s\n", err.c_str()); return 1; }
    printf("open OK: %s, 声道 %d（驱动输出 %d），缓冲 %ld 帧，outReady=%d\n",
           drv.name().c_str(), drv.channels(), drv.driver_out_channels(),
           drv.buffer_size(), (int)drv.out_ready_supported());
    for (int c = 0; c < drv.channels(); ++c)
        printf("   ch%d type=%s\n", c, type_name(drv.channel_type(c)));

    if (!drv.start(err)) { printf("start 失败: %s\n", err.c_str()); return 1; }
    std::vector<int32_t> zeros((size_t)drv.buffer_size() * (size_t)drv.channels(), 0);
    const size_t total = (size_t)(rate * (double)ms / 1000.0);
    size_t pushed = 0;
    while (pushed < total) {
        const size_t n = drv.buffer_size();
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
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)ci;

    auto list = asio_enum_drivers();
    printf("注册表里 ASIO 驱动 %zu 个：\n", list.size());
    for (size_t i = 0; i < list.size(); ++i) printf("  [%zu] %s\n", i, list[i].name.c_str());
    if (list.empty()) { printf("（没有 ASIO 驱动）\n"); CoUninitialize(); return 1; }

    int streamIdx = -1; double rate = 48000; int ch = 2;
    bool useHwnd = false, nopCb = false, checkOnly = false;
    std::string wantClsid;
    int ms = 1000;
    for (int i = 1; i < argc; ++i) {
        // 只有后面确实跟着数字才当序号吃掉，否则会误吞 --clsid 这类开关（本轮踩到）
        if (!std::strcmp(argv[i], "--stream") && i + 1 < argc && (isdigit((unsigned char)argv[i + 1][0]) || argv[i + 1][0] == '-'))
            streamIdx = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--ch") && i + 1 < argc) ch = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--check")) checkOnly = true;
        else if (!std::strcmp(argv[i], "--clsid") && i + 1 < argc) wantClsid = argv[++i];
        else if (!std::strcmp(argv[i], "--ms") && i + 1 < argc) ms = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--hwnd")) useHwnd = true;
        else if (!std::strcmp(argv[i], "--nop"))  nopCb = true;
    }
    if (!wantClsid.empty()) {
        streamIdx = -1;
        for (size_t i = 0; i < list.size(); ++i) if (list[i].clsid == wantClsid) streamIdx = (int)i;
        if (streamIdx < 0) { printf("找不到 CLSID %s\n", wantClsid.c_str()); return 4; }
    }
    if (checkOnly && streamIdx >= 0 && streamIdx < (int)list.size()) {
        const int rc = check_test(list[(size_t)streamIdx], rate, ch);
        CoUninitialize();
        return rc;
    }
    if (streamIdx >= 0 && streamIdx < (int)list.size()) {
        void* hw = useHwnd ? (void*)make_hidden_window(GetModuleHandleW(nullptr)) : nullptr;
        printf("sysHandle=%p nopCallback=%d\n", hw, (int)nopCb);
        const int rc = stream_test(list[(size_t)streamIdx], rate, ch, hw, nopCb, ms);
        CoUninitialize();
        return rc;
    }
    for (size_t i = 0; i < list.size(); ++i) probe_one(list[i], (int)i);
    printf("\n（只做了只读探测，没有创建缓冲、没有开流、没有声音）\n");
    CoUninitialize();
    return 0;
}
