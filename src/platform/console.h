// console.h — 控制台编码（平台层契约，跨平台）
//
// 现状：所有 CLI 入口（tools/*.cpp、tests/*.cpp）都自己写 SetConsoleOutputCP(CP_UTF8)，
// 有几处甚至为了避开 windows.h 手写 __declspec(dllimport) 声明。本接口是它们统一的目标，
// 本轮【不】批量改工具/测试（避免与 CI/测试分支撞车，见 PLATFORM.md §11 未做项）。
//
// 契约：把标准输出的编码切成 UTF-8（中文日志不乱码）；无控制台时安全失败（返回 false，不抛）。
#pragma once

namespace rta::platform {

bool enable_utf8_console();

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/console_win32.h"
#else
#error "platform/console.h: 本平台尚无实现（POSIX 下控制台本来就是 UTF-8，可直接 return true）"
#endif
