// platform/windows/console_win32.h — console.h 的 Windows 实现
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rta::platform {

// 只切【输出】代码页（CP_UTF8），不改输入代码页 —— 与迁移前 tools/tests 的做法一致
inline bool enable_utf8_console() { return ::SetConsoleOutputCP(CP_UTF8) != 0; }

} // namespace rta::platform
