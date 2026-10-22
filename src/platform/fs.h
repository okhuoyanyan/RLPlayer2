// fs.h — 文件操作（平台层契约，跨平台）
//
// 只抽"语义在各平台不等价、必须由平台保证"的那几个操作：
//   * 覆盖式重命名（原子替换）—— WAV/写回的安全红线全靠它
//   * 非覆盖式重命名（备份搬移）、跨盘复制
//   * 删除可能不存在的文件
//   * 最近一次系统错误码（用户可见文案里要带数字，便于现场诊断）
//
// 不抽：读/写文件内容（std::ifstream/ofstream 已经足够，且行为一致）、
//       目录遍历（std::filesystem 的 directory_iterator 可用）。
#pragma once
#include <filesystem>
#include <string>

namespace rta::platform {

// 删除文件。文件不存在也返回 false（与 DeleteFileW 一致，调用方历来忽略返回值）
bool remove_file(const std::filesystem::path& p);

// 重命名/搬移。replace_existing=true 时允许覆盖目标（原子替换）；
// false 时目标已存在即失败（备份场景：绝不覆盖最早的原始备份）。
// 跨卷时按平台语义失败（Windows: MoveFileEx 返回 FALSE + ERROR_NOT_SAME_DEVICE）。
bool move_file(const std::filesystem::path& from, const std::filesystem::path& to,
               bool replace_existing);

// 复制文件。fail_if_exists=true 时目标已存在即失败。
bool copy_file(const std::filesystem::path& from, const std::filesystem::path& to,
               bool fail_if_exists);

// 文件是否存在（目录也返回 true，语义 = GetFileAttributesW != INVALID）
bool file_exists(const std::filesystem::path& p);

// 最近一次系统错误码的数字值（Windows: GetLastError()；POSIX: errno）。
// 用户可见错误文案必须带上它 —— 不许只说"失败了"。
int last_error_code();

// 原子替换 = 覆盖式重命名（语义糖，写明意图）
inline bool atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
    return move_file(from, to, true);
}

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/fs_win32.h"
#else
#error "platform/fs.h: 本平台尚无实现。Mac 组请按 PLATFORM.md §9（Mac 实现清单）实现 src/platform/posix/fs_posix.h"
#endif
