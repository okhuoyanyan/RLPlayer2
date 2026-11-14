// platform/windows/env_win32.h — env.h 的 Windows 实现
//
// 逐 1:1 包装 GetEnvironmentVariableW：缓冲大小、失败返回值、内容不保证的边界
// 全部按原生语义透传（调用方 meta/tags_native.h 的三个开关依赖这些细节）。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

#include "platform/windows/win_text.h"

namespace rta::platform {

inline unsigned get_env_wide(const char* name_utf8, unsigned cap, std::wstring& out) {
    // 与迁移前的 wchar_t buf[N] = {0} 同样的初始状态（失败时调用方可能仍读它）
    out.assign((size_t)cap, L'\0');
    if (!name_utf8 || !*name_utf8 || cap == 0) return 0;
    const std::wstring wname = detail::widen(std::string(name_utf8));
    if (wname.empty()) return 0;
    const DWORD n = ::GetEnvironmentVariableW(wname.c_str(), out.data(), (DWORD)cap);
    return (unsigned)n;
}

} // namespace rta::platform
