// platform/windows/fs_win32.h — fs.h 的 Windows 实现（逐 1:1 包装 Win32，不改语义）
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <utility>
#include <vector>

#include "platform/windows/win_text.h"   // detail::narrow（目录项名 -> UTF-8）

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

// ---------------- 目录 / 属性 / 兜底搬移 ----------------

inline bool is_directory(const std::filesystem::path& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline long long file_size(const std::filesystem::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | (long long)d.nFileSizeLow;
}

inline bool create_directory(const std::filesystem::path& p) {
    return ::CreateDirectoryW(p.c_str(), nullptr) != 0;
}

inline bool remove_directory(const std::filesystem::path& p) {
    return ::RemoveDirectoryW(p.c_str()) != 0;
}

inline bool set_normal_attributes(const std::filesystem::path& p) {
    return ::SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL) != 0;
}

inline bool is_reparse_point(const std::filesystem::path& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

inline bool list_directory(const std::filesystem::path& dir, std::vector<DirEntry>& out) {
    out.clear();
    // 掩码 = dir + 分隔符 + "*"（与迁移前 join_path(dir, "*") 逐字符一致：
    // 末尾已有分隔符就不重复加，分隔符是 '\'）
    std::wstring mask = dir.native();
    if (!mask.empty() && mask.back() != L'\\' && mask.back() != L'/') mask += L'\\';
    mask += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const std::wstring nm(fd.cFileName);
        if (nm == L"." || nm == L"..") continue;
        DirEntry e;
        e.name = detail::narrow(nm);
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(std::move(e));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return true;
}

inline bool move_or_copy_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    // A completed copy is only half a move. Keep the copied backup on delete
    // failure so the caller can recover/retry, but never report the source gone
    // while Windows still retains it (for example, a sharing lock).
    ::SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    if (!::CopyFileW(from.c_str(), to.c_str(), FALSE)) return false;
    ::SetFileAttributesW(from.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (::DeleteFileW(from.c_str())) return true;
    const DWORD deletionError = ::GetLastError();
    // A concurrent remover or a filesystem driver may leave an absent source
    // despite a failed return. Only a definite not-found result proves success;
    // access denied and other attribute-query errors do not prove absence.
    if (::GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD queryError = ::GetLastError();
        if (queryError == ERROR_FILE_NOT_FOUND || queryError == ERROR_PATH_NOT_FOUND) return true;
    }
    ::SetLastError(deletionError);
    return false;
}

} // namespace rta::platform
