// crash_dump.h — 崩溃转储 + 进程环境诊断（1.0.5 可观测性扩展共用件）
//
// 组成（2026-10-22，HF-04/可观测性）：
//   * rta::crash::install()   —— SetUnhandledExceptionFilter + MiniDumpWriteDump(dbghelp)。
//     dump 落 exe 同目录 dumps\，文件名含时间与 PID；filter 里【先写一行日志】再生成 dump，
//     然后返回 EXCEPTION_EXECUTE_HANDLER 让进程按 SEH 语义终止。rtaweb 的 main 与
//     launcher 的入口各安装一次。
//   * rta::diag::os_version_string() —— RtlGetVersion（ntdll，动态取）。
//     不用 GetVersionEx：它受 manifest 声明影响会说谎（声明了 compat 才给真版本）。
//   * rta::diag::cpu_brand()  —— CPUID 0x80000002..4 品牌串。
//   * rta::diag::read_tester()—— exe 目录 rlplayer-tester.txt 第一行（内测标识），
//     进启动横幅与 /api/diag。
//
// 纪律：全部只读探测，不开设备、不发声、不改系统状态；filter 里只用 WinAPI
//（CreateFile/WriteFile），不用可能已被破坏堆的 CRT 容器。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace rta::diag {

inline std::wstring exe_dir_w() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring d(buf);
    const size_t s = d.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring(L".") : d.substr(0, s);
}

// OS 版本（RtlGetVersion，manifest 无关）
inline std::string os_version_string() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return "未知";
    const auto rtl = (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion");
    if (!rtl) return "未知";
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (rtl(&v) != 0) return "未知";
    char b[96];
    std::snprintf(b, sizeof(b), "Windows %lu.%lu.%lu",
                  (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
                  (unsigned long)v.dwBuildNumber);
    return std::string(b);
}

// CPU 品牌串（CPUID 0x80000002..0x80000004；不支持扩展叶时如实返回"未知"）
inline std::string cpu_brand() {
    int ids[4] = {0, 0, 0, 0};
    __cpuid(ids, 0x80000000);
    if ((unsigned)ids[0] < 0x80000004u) return "未知";
    char b[49] = {0};
    for (int leaf = 0; leaf < 3; ++leaf) {
        int r[4];
        __cpuid(r, 0x80000002 + leaf);
        std::memcpy(b + leaf * 16, r, 16);
    }
    b[48] = 0;
    char* s = b;
    char* e = b + std::strlen(b);
    while (*s == ' ') ++s;
    while (e > s && e[-1] == ' ') --e;
    *e = 0;
    return std::string(s);
}

// exe 目录 rlplayer-tester.txt 第一行（trim 后；没有文件 / 只有空行 → 空串）
inline std::string read_tester() {
    const std::wstring wp = exe_dir_w() + L"\\rlplayer-tester.txt";
    const int n = WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string p((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, p.data(), n, nullptr, nullptr);
    if (!p.empty() && p.back() == 0) p.pop_back();
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = line.find_last_not_of(" \t");
        return line.substr(a, b - a + 1);
    }
    return "";
}

} // namespace rta::diag

namespace rta::crash {

// 未处理异常过滤器：先写一行日志 → 生成 minidump → 让进程终止。
// 写日志/生成 dump 都只用 WinAPI；重复进入（filter 里再崩）直接放行终止，绝不递归。
inline LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    static volatile LONG inFilter = 0;
    if (InterlockedExchange(&inFilter, 1)) return EXCEPTION_EXECUTE_HANDLER;

    const std::wstring dir = rta::diag::exe_dir_w() + L"\\dumps";
    CreateDirectoryW(dir.c_str(), nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t fname[80] = {0};
    _snwprintf(fname, 79,
               L"\\rlplayer-%04u%02u%02u-%02u%02u%02u-pid%lu.dmp",
               (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
               (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
               (unsigned long)GetCurrentProcessId());
    const std::wstring dumpPath = dir + fname;

    // ① 先写一行日志（崩溃第一现场：时间 / 异常码 / 地址 / dump 路径）
    {
        HANDLE lh = CreateFileW((dir + L"\\rlplayer-crash.log").c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lh != INVALID_HANDLE_VALUE) {
            const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
            char line[512];
            std::snprintf(line, sizeof(line),
                          "%04u-%02u-%02u %02u:%02u:%02u 未处理异常 code=0x%08lX addr=%p pid=%lu dump=%ls\r\n",
                          (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                          (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                          (unsigned long)(er ? er->ExceptionCode : 0),
                          (void*)(er ? er->ExceptionAddress : nullptr),
                          (unsigned long)GetCurrentProcessId(), fname + 1);
            DWORD written = 0;
            WriteFile(lh, line, (DWORD)std::strlen(line), &written, nullptr);
            CloseHandle(lh);
        }
    }

    // ② 生成 dump（MiniDumpNormal：线程栈 + 异常上下文，体积小、够定位崩溃点）
    {
        HANDLE fh = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), fh,
                              MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
            CloseHandle(fh);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;   // 让进程按默认语义终止
}

// 在进程入口调用一次（rtaweb main / launcher wWinMain）
inline void install() { SetUnhandledExceptionFilter(&unhandled_filter); }

} // namespace rta::crash
