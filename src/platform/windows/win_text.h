// platform/windows/win_text.h — UTF-8 <-> UTF-16 转换（Windows 实现内部用）
//
// 刻意用 MultiByteToWideChar/WideCharToMultiByte（CP_UTF8）而不是 fs::path::u8string：
// 迁移前的 http_client.h / audio_loader.h / writeback.h 用的就是这两个 API，
// 保持同一实现 ⟹ 转换结果逐字节等价（含非法序列的替换行为）。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace rta::platform::detail {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

inline std::string narrow(const wchar_t* w) {
    if (!w) return "";
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

inline std::string narrow(const std::wstring& w) { return narrow(w.c_str()); }

} // namespace rta::platform::detail
