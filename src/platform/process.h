// process.h — 子进程（平台层契约，跨平台）
//
// 用途：llm/ffmpeg/ffprobe 之类的【外部工具】调用。全是"跑一次、收退出码、收日志"，
// 不需要管道交互、不需要异步。
//
// 契约要点：
//   * 不弹窗、不接管标准输入、不继承控制台（Windows: CREATE_NO_WINDOW + SW_HIDE）
//   * 命令行是 UTF-8 单串（含引号规则已在调用方拼好）；平台层负责转原生编码
//   * capture=true 时合并捕获 stdout+stderr（原样字节，不转码、不裁剪）
//   * 返回 exit_code = -1 表示【启动失败】（进程根本没跑起来），与"退出码 -1"区分看 started
//   * 阻塞直到进程退出
//
// 为什么命令行不用 argv 数组：迁移前 writeback.h/audio_loader.h 拼的就是一整条命令行
//（CreateProcessW 语义），改成 argv 会重新引入引号/反斜杠转义差异的风险。
#pragma once
#include <string>

namespace rta::platform {

struct ProcessResult {
    int exit_code = -1;
    bool started = false;
    std::string output;
};

ProcessResult run_hidden(const std::string& command_line_utf8, bool capture_output = false);

// 找可执行文件：先看 exe 同目录（分发版把 ffmpeg.exe 放这里），再退回 PATH。
// 找不到返回空串（调用方据此报"找不到 ffmpeg"）。
// name 不带扩展名（如 "ffmpeg"）。
std::string find_executable(const char* name);

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/process_win32.h"
#else
#error "platform/process.h: 本平台尚无实现。Mac 组请按 PLATFORM.md §9（Mac 实现清单）用 posix_spawn 实现"
#endif
