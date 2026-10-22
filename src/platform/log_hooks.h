// log_hooks.h — 日志/追踪钩子的【类型契约】（平台层）
//
// 刻意【不】新建日志实现：项目里已经有两套现成钩子
//   * 引擎侧：rta::render::RenderHost::log / dlogFn（std::function<void(const std::string&)>，
//     见 src/audio/render_loop.h 与 tools/rtaweb.cpp 里把它们接到 play_log / dlog 的位置）
//   * 落盘侧：rtaweb 的 play_log（exe 同目录 rlplayer-playback.log，4 MB 轮转）
// 再造一套日志文件只会让"到底哪份日志是真的"变成新的坑。
//
// 本头只声明两端都认的【类型】，让 Mac 侧实现的平台后端能把日志接回同一条链路。
#pragma once
#include <functional>
#include <string>

namespace rta::platform {

// 与 rta::render::RenderHost::log / dlogFn 完全同型，可直接互赋
using LogSink = std::function<void(const std::string&)>;

// 一条日志通道的成对钩子（play = 用户可见的播放日志；debug = 仅诊断用的 dlog）
struct LogHooks {
    LogSink play;
    LogSink debug;
};

} // namespace rta::platform
