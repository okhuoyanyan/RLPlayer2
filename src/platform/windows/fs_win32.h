// platform/windows/fs_win32.h — fs.h 的 Windows 实现（逐 1:1 包装 Win32，不改语义）
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rta::platform {

inline bool remove_file(const std::filesystem::path& p) {
    return ::DeleteFileW(p.c_str()) != 0;
}

inline bool move_file(const std::filesystem::path& from, const std::filesystem::path& to,
                      bool replace_existing) {
    const DWORD flags = replace_existing ? MOVEFILE_REPLACE_EXISTING : 0;
    return ::MoveFileExW(from.c_str(), to.c_str(), flags) != 0;
}

inline bool copy_file(const std::filesystem::path& from, const std::filesystem::path& to,
                      bool fail_if_exists) {
    return ::CopyFileW(from.c_str(), to.c_str(), fail_if_exists ? TRUE : FALSE) != 0;
}

inline bool file_exists(const std::filesystem::path& p) {
    return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline int last_error_code() { return (int)::GetLastError(); }

} // namespace rta::platform
