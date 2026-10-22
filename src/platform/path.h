// path.h — 路径与编码（平台层契约，跨平台）
//
// 规则（详见 PLATFORM.md §6）：
//   * 内核里流转的路径一律是 UTF-8 窄字符串；只有本层能把它们变成【原生路径】
//   * 原生路径类型是 std::filesystem::path（Windows = wchar_t，POSIX = char）
//   * 本文件【不得】出现任何 Windows 类型（HANDLE/LPWSTR/DWORD...）
//
// 为什么 UTF-8 -> path 用 u8string 而不是 MultiByteToWideChar：
//   meta/store.h 的 meta_path_of/meta_string_of 早已这么做（那里刻意不拉 windows.h），
//   这套转换是既有行为的一部分，本层只是把它变成唯一入口。
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace rta::platform {

using NativePath = std::filesystem::path;

// UTF-8 窄串 -> 原生路径（Windows 上按 UTF-8 解释，不经 ANSI 代码页）
inline NativePath from_utf8(const std::string& s) {
    return NativePath(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
// 原生路径 -> UTF-8 窄串
inline std::string to_utf8(const NativePath& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// 平台路径分隔符（Windows '\\'，POSIX '/'）
inline char native_separator() {
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

// UTF-8 路径拼接：dir 末尾已有分隔符则不重复加
inline std::string join(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    if (leaf.empty()) return dir;
    const char last = dir.back();
    if (last == '\\' || last == '/') return dir + leaf;
    return dir + native_separator() + leaf;
}

// 小写扩展名（含点）。纯字符串操作，与 audio/audio_loader.h 的 lowerExt 逐字符一致
//（写回模块要用它，但不能再拉 audio_loader.h —— 那会把 windows.h 带进来）。
inline std::string lower_extension(const std::string& p) {
    const size_t d = p.find_last_of('.');
    if (d == std::string::npos) return "";
    std::string e = p.substr(d);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

// ---------------- 以下四个由平台实现提供 ----------------

// 可执行文件所在目录（UTF-8，无尾分隔符；失败返回空串）
std::string module_dir_utf8();
// 系统临时目录（UTF-8，【带尾分隔符】，语义与 Win32 GetTempPathW 一致）
std::string temp_dir_utf8();
// 配置目录（放 *.conf）。Windows 现行规则 = exe 同目录（便携式分发）
std::string config_dir_utf8();
// 数据目录（缓存 / meta 库 / 日志）。Windows 现行规则 = exe 同目录
std::string data_dir_utf8();

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/path_win32.h"
#else
#error "platform/path.h: 本平台尚无实现。Mac 组请按 PLATFORM.md §9（Mac 实现清单）实现 src/platform/posix/path_posix.h"
#endif
