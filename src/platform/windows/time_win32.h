// platform/windows/time_win32.h — time.h 的 Windows 实现
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

namespace rta::platform {

// 语义 = GetTickCount64()（开机以来毫秒，不回退）
inline uint64_t monotonic_ms() { return (uint64_t)::GetTickCount64(); }

} // namespace rta::platform
