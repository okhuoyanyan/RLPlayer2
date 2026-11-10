// env.h — 环境变量读取（平台层契约，跨平台）
//
// 为什么不用 std::getenv / _wgetenv：
//   * MSVC/clang-cl 的 getenv 已 deprecated，而且只看得懂 ANSI 代码页（中文用户名会失真）；
//   * _wgetenv 返回的是**共享缓冲**，而标签扫描是 8 线程并行、每个文件都问一次开关 ——
//     并发读有撕裂风险（meta/tags_native.h 的注释里记着这条）。
// 所以内核统一走本层：Windows 实现 = GetEnvironmentVariableW（线程安全、不分配内存）。
//
// 契约（**刻意保留原生缓冲语义**，因为调用方依赖它把"太长"当"没设置"）：
//   返回 0        ⟹ 变量不存在（out 保持全 NUL，长度 = cap，与栈上 wchar_t buf[cap] = {0} 一致）
//   返回 <  cap   ⟹ 值 = out.substr(0, 返回值)（不含结尾 NUL）
//   返回 >= cap   ⟹ 缓冲区不够（GetEnvironmentVariableW 的原生返回：需要的字符数，含结尾 NUL）。
//                    原生 API 在这种情况下不保证 buf 内容；调用方历来按"越界就当没设置"处理，
//                    本层不做任何额外判断（不许顺手改语义）。
//   out 恒为 cap 个字符，内容 = 原生 API 写进缓冲的内容（不足部分保持 NUL）。
//
// 名字用 UTF-8 窄串（原实现的名字全是 ASCII 常量，逐字节等价）。
#pragma once
#include <string>

namespace rta::platform {

unsigned get_env_wide(const char* name_utf8, unsigned cap, std::wstring& out);

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/env_win32.h"
#else
#error "platform/env.h: 本平台尚无实现。Mac 组请按 PLATFORM.md §9 用 getenv_r / _NSGetEnviron + UTF-8 实现"
#endif
