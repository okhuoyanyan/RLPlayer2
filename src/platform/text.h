// text.h — 文本编码转换（平台层契约，跨平台）
//
// 为什么它属于平台层：UTF-8 <-> UTF-16 的取舍（用哪个 API、哪个 flags、-1 还是显式长度、
// 是否保留结尾 NUL、非法序列怎么替换）在各平台**并不等价**，而 net/meta 里的标签解析、
// CUE 解码、URL/环境变量读取全都依赖这些细节。把它们收在这里，内核才可能跨平台。
//
// 契约（Windows 实现 = MultiByteToWideChar / WideCharToMultiByte，CP_UTF8）：
//   * utf8_to_wide(s)   ：CP_UTF8, dwFlags=0, **显式长度 (int)s.size()**（不要求 NUL 结尾、
//                         不产出 NUL）。空串 -> 空串；转换失败 -> 空串。
//   * wide_to_utf8(w)   ：CP_UTF8, dwFlags=0, **显式长度 (int)w.size()**，不追加 NUL。
//                         空串 -> 空串；转换失败 -> 空串。
//   * wide_to_utf8_checked(w, out)：同上，但用返回值区分"失败"，给需要区分的调用方
//                         （tags_native 的 UTF-16 标签解码：失败要回退 ffprobe，不能当空串）。
//   * gbk_to_utf8(in, out)：CP936 解码。先严格探测（MB_ERR_INVALID_CHARS），失败再宽松探测，
//                         最后仍用**宽松** flags 做真正转换（与迁移前的 cue.h 逐步一致）。
//                         返回 false = 连宽松解码都失败（调用方保留原始字节）。
//
// ⚠️ 刻意不提供"猜编码"：那是 meta/cue.h 的策略（UTF-8 严格校验 + 回退），不属于平台层。
#pragma once
#include <string>

namespace rta::platform {

std::wstring utf8_to_wide(const std::string& s);
std::string  wide_to_utf8(const std::wstring& w);
bool         wide_to_utf8_checked(const std::wstring& w, std::string& out);
bool         gbk_to_utf8(const std::string& in, std::string& out);

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/text_win32.h"
#else
#error "platform/text.h: 本平台尚无实现。Mac 组请按 PLATFORM.md §9 用 CoreFoundation/iconv 实现（UTF-8 与 UTF-16 互转；CP936 用 iconv）"
#endif
