// platform/windows/text_win32.h — text.h 的 Windows 实现
//
// 逐行搬自迁移前的三处实现（flags 与长度参数**一点没改**，因为它们是行为的一部分）：
//   * net/feedback.h 的 utf8_to_wide   —— CP_UTF8, 0, (int)s.size()
//   * meta/cue.h 的 cue_from_wstring / cue_widen_utf8 / cue_gbk_to_utf8
//   * meta/tags_native.h 的 utf16_to_utf8（CP_UTF8, 0, (int)w.size()）
//
// 注意：这里**不用** platform/windows/win_text.h 的 widen()/narrow() —— 那两个是
// "-1 长度 + 去掉结尾 NUL" 的语义（给路径/命令行用的），与这里的显式长度语义不同。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace rta::platform {

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

inline bool wide_to_utf8_checked(const std::wstring& w, std::string& out) {
    out.clear();
    if (w.empty()) return true;
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    out = std::move(s);
    return true;
}

inline std::string wide_to_utf8(const std::wstring& w) {
    std::string out;
    wide_to_utf8_checked(w, out);
    return out;
}

// GBK（CP936）-> UTF-8：严格探测 -> 宽松探测 -> 宽松转换（与迁移前 cue_gbk_to_utf8 同序同 flags）
inline bool gbk_to_utf8(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty()) return true;
    int n = ::MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, in.c_str(), (int)in.size(), nullptr, 0);
    if (n <= 0) n = ::MultiByteToWideChar(936, 0, in.c_str(), (int)in.size(), nullptr, 0);
    if (n <= 0) return false;                       // 连宽松解码都失败：调用方保留原字节
    std::wstring w((size_t)n, L'\0');
    if (::MultiByteToWideChar(936, 0, in.c_str(), (int)in.size(), w.data(), n) <= 0) return false;
    return wide_to_utf8_checked(w, out);
}

} // namespace rta::platform
