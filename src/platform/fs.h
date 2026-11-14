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
#include <vector>

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

// ---------------- 目录 / 属性 / 兜底搬移（261031：net/update.h、net/update_apply.h 的 IO 收口） ----------------
// 这些操作的语义在各平台**不等价**（属性位、重解析点、跨卷 rename），所以必须由平台保证。

// 目录是否存在（Windows = GetFileAttributesW 且带 FILE_ATTRIBUTE_DIRECTORY）
bool is_directory(const std::filesystem::path& p);

// 文件大小（字节）。取不到返回 -1（Windows = GetFileAttributesExW 失败）
long long file_size(const std::filesystem::path& p);

// 创建**一级**目录（父目录必须已存在）。已存在返回 false —— 与 CreateDirectoryW 一致，
// 递归是调用方的事（它要处理"父目录不存在就逐级建"的策略与报错文案）。
bool create_directory(const std::filesystem::path& p);

// 删除**空**目录（Windows = RemoveDirectoryW）
bool remove_directory(const std::filesystem::path& p);

// 清掉只读等属性（Windows = SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL)）。
// 返回值只是"这次调用成功了没有"，调用方历来忽略（失败时后续的删除/改名自己会报错）。
bool set_normal_attributes(const std::filesystem::path& p);

// 是否是重解析点（junction / symlink / mount point）。路径不存在返回 false。
// 用途：更新落地前拒绝穿越链接（防止把文件写到安装目录之外）。
bool is_reparse_point(const std::filesystem::path& p);

// 目录项：只给"叶子名 + 是不是目录"，内核用不到其它属性
struct DirEntry {
    std::string name;      // UTF-8 叶子名
    bool is_dir = false;
};

// 枚举目录（Windows = FindFirstFileW(dir + "\*")，"." ".." 已过滤）。
// 返回 false = 打不开目录（不存在/无权限）。**不排序**：保持原生枚举顺序，
// 因为调用方（增量安装清单）依赖"同一目录两次枚举顺序一致"。
bool list_directory(const std::filesystem::path& dir, std::vector<DirEntry>& out);

// 搬家（"必须搬过去"的语义）：同卷 MoveFileEx（覆盖已存在目标）优先；失败（跨卷等）
// 退回"复制 + 删除源"。逐字等价于迁移前 net/update_apply.h 的 move_file()。
bool move_or_copy_file(const std::filesystem::path& from, const std::filesystem::path& to);

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/fs_win32.h"
#else
#error "platform/fs.h: 本平台尚无实现。Mac 组请按 PLATFORM.md §9（Mac 实现清单）实现 src/platform/posix/fs_posix.h"
#endif
