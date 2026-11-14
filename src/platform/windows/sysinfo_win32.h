// platform/windows/sysinfo_win32.h — sysinfo.h 的 Windows 实现
//
// 全部逐行搬自迁移前的 net/feedback.h（make_stamp / collect_gpu_adapters /
// proc_uptime_sec / mem_info / GetSystemInfo）与 net/i18n.h（GetUserDefaultUILanguage），
// **错误处理与兜底值一个都没改**（例如 GetProcessTimes 的 lpExitTime 坑：
// 对仍在运行的进程不可用，所以改成"现在（FILETIME 时基）- 创建时间"）。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "platform/windows/win_text.h"

// 迁移前这两条 pragma 写在 net/feedback.h 里；平台层收走实现，链接指令跟着实现走
// （否则只 include 本头的目标会缺符号）。user32 用于 EnumDisplayDevicesW。
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

namespace rta::platform {

inline LocalTime local_time_now() {
    LocalTime t;
    SYSTEMTIME st;
    GetLocalTime(&st);
    t.year = (unsigned)st.wYear;  t.month  = (unsigned)st.wMonth;
    t.day  = (unsigned)st.wDay;   t.hour   = (unsigned)st.wHour;
    t.minute = (unsigned)st.wMinute; t.second = (unsigned)st.wSecond;
    return t;
}

inline int local_timezone_offset_minutes() {
    TIME_ZONE_INFORMATION tzi{};
    const DWORD tz = ::GetTimeZoneInformation(&tzi);
    const long bias = tzi.Bias + (tz == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : tzi.StandardBias);
    return (int)(-bias);                       // 分钟，东为正
}

inline unsigned long long process_uptime_seconds() {
    FILETIME c, e, k, u;
    if (!::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    FILETIME now;
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER cur, born;
    cur.LowPart = now.dwLowDateTime; cur.HighPart = now.dwHighDateTime;
    born.LowPart = c.dwLowDateTime;  born.HighPart = c.dwHighDateTime;
    if (cur.QuadPart <= born.QuadPart) return 0;
    return (unsigned long long)((cur.QuadPart - born.QuadPart) / 10000000ULL);
}

inline unsigned long long process_id() { return (unsigned long long)::GetCurrentProcessId(); }

inline MemoryInfo process_memory() {
    MemoryInfo m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        m.working_set_bytes      = (unsigned long long)pmc.WorkingSetSize;
        m.peak_working_set_bytes = (unsigned long long)pmc.PeakWorkingSetSize;
        m.private_bytes          = (unsigned long long)pmc.PrivateUsage;
    }
    DWORD h = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &h)) m.handle_count = (unsigned)h;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms)) {
        m.total_physical_bytes = (unsigned long long)ms.ullTotalPhys;
        m.avail_physical_bytes = (unsigned long long)ms.ullAvailPhys;
    }
    return m;
}

inline unsigned cpu_thread_count() {
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    return (unsigned)si.dwNumberOfProcessors;
}

inline std::vector<std::string> display_adapter_names() {
    std::vector<std::string> names;
    for (DWORD i = 0; i < 8; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!::EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        char u8[256] = {0};
        ::WideCharToMultiByte(CP_UTF8, 0, dd.DeviceString, -1, u8, (int)sizeof(u8) - 1, nullptr, nullptr);
        const std::string n(u8);
        if (n.empty()) continue;
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    }
    return names;
}

inline unsigned user_ui_language_id() {
    const LANGID id = ::GetUserDefaultUILanguage();
    return (unsigned)id;
}

} // namespace rta::platform
