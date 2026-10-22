// platform/windows/path_win32.h — path.h 的 Windows 实现
//
// ⚠️ 本目录（src/platform/windows/）是全项目【唯一】允许 #include <windows.h> 的地方。
// 内核目录（dsp/engine/net/meta）由 tools/check_platform_boundary.ps1 把门。
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

// 与迁移前的 GetModuleFileNameW(nullptr,...) + 截到最后一个分隔符 逐字符一致。
// 转换用 WideCharToMultiByte(CP_UTF8)，与迁移前 source_mgr/audio_loader 的实现同一个 API。
inline std::string module_dir_utf8() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return "";
    const std::wstring s(buf, n);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return "";
    return detail::narrow(s.substr(0, slash));
}

// 语义 = GetTempPathW(MAX_PATH)：返回值【带尾分隔符】，超长按 MAX_PATH 截断
inline std::string temp_dir_utf8() {
    wchar_t tmp[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, tmp);
    return detail::narrow(tmp);
}

// Windows 分发是"便携式"：配置与数据全部落在 exe 同目录（rlplayer-*.conf / rlplayer-meta /
// remote-cache / webview2 / logs...）。Mac 组必须提供等价规则，见 PLATFORM.md §6。
inline std::string config_dir_utf8() { return module_dir_utf8(); }
inline std::string data_dir_utf8()   { return module_dir_utf8(); }

} // namespace rta::platform
