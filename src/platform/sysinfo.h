// sysinfo.h — 系统 / 进程信息（平台层契约，跨平台）
//
// 用途：一键反馈（net/feedback.h）要把"用户机器长什么样"带进诊断包；界面多语言
// （net/i18n.h）要问系统界面语言。这些全都只有平台 API 能回答，但**内核不该知道**
// SYSTEMTIME / PROCESS_MEMORY_COUNTERS / LANGID 这些类型 —— 所以在这里定义一份
// "只有数据、没有类型"的结构。
//
// 纪律：
//   * 每个字段的单位写清楚（字节 / 秒 / 分钟），内核负责换成 MB、时区字符串等展示形式；
//   * 失败一律返回 0 / 空，不抛异常（诊断代码绝不能因为取不到信息而让主流程失败）；
//   * 语义与 Windows 迁移前**逐字一致**（见 windows/sysinfo_win32.h 的注释）。
#pragma once
#include <string>
#include <vector>

namespace rta::platform {

// 本地时间（本地时区、取整到秒）。year 是四位年。
struct LocalTime {
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};
LocalTime local_time_now();

// 本地时区相对 UTC 的偏移，单位**分钟，东为正**（北京 = +480）。
// Windows = -(Bias + (当前是否夏令时 ? DaylightBias : StandardBias))。
int local_timezone_offset_minutes();

// 当前进程已运行秒数（取整）。取不到返回 0。
unsigned long long process_uptime_seconds();

// 当前进程 ID。
unsigned long long process_id();

// 进程内存与系统内存（单位 = 字节；handle_count 是句柄数）
struct MemoryInfo {
    unsigned long long working_set_bytes = 0;
    unsigned long long peak_working_set_bytes = 0;
    unsigned long long private_bytes = 0;
    unsigned long long total_physical_bytes = 0;
    unsigned long long avail_physical_bytes = 0;
    unsigned handle_count = 0;
};
MemoryInfo process_memory();

// 逻辑处理器数（Windows = SYSTEM_INFO.dwNumberOfProcessors）
unsigned cpu_thread_count();

// 已连接到桌面的显示适配器名字（UTF-8，去重，保持枚举顺序；取不到返回空）
std::vector<std::string> display_adapter_names();

// 当前登录用户的界面语言 ID（Windows = GetUserDefaultUILanguage 的原生值，
// 低 10 位是主语言；取不到返回 0）。为什么是"用户"而不是"系统"：用户改了显示语言
// 要立刻生效，GetSystemDefaultUILanguage 返回的是安装时的系统语言。
unsigned user_ui_language_id();

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/sysinfo_win32.h"
#else
#error "platform/sysinfo.h: 本平台尚无实现。Mac 组请按 PLATFORM.md §9 用 NSProcessInfo / sysctl / NSTimeZone 实现"
#endif
