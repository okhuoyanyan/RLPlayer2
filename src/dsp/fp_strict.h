// src/dsp/fp_strict.h — DSP 运算次序守卫（不是完整 IEEE 严格模式）
//
// 背景（2026-10-31 审计）：发布构建用的是 `-ffast-math`，它包含
//   -fassociative-math（允许重排加法）、-freciprocal-math（把 x/y 换成 x*(1/y)）、
//   -fno-signed-zeros。这三条都会让**同样的输入算出不同的结果**。
//
// 实测（build/bitexact.cpp，528 抽头 × 1000 次点积）：
//   strict : -0x1.f2a68e3d039b1p+1
//   fast   : -0x1.f2a68e3d039dep+1     ⟸ 尾数最后几位不同（约 1e-16 相对 = −320 dB）
// 性能差 3.2 倍（2.99 vs 9.57 GMAC/s）。
//
// 结论：这是「为了性能牺牲数学完美」的典型例子，与本项目「听感 + 数学双完美」的目标冲突。
// 处理：此 pragma 关闭重排、FMA 收缩与倒数改写。它不会恢复 signed-zero
// 或取消所有 fast-math 子选项，不能将本文件称为完整 IEEE 严格模式。
// 用 pragma 而不是改构建标志，是因为 DSP 是 header-only：改标志要覆盖每一个包含它的 TU，
// 而 pragma 写在头里，谁包含谁生效。
//
// ⚠ **作用域是「到本 TU 末尾」，不是「到本头结束」**（2026-10-31 DEBUG-DSP 实测）。
//   clang 的 `#pragma clang fp` 没有 push/pop，一旦某个 TU 包含过 DSP 头，
//   该 TU 后续解析的代码（HTTP/JSON/LLM/UI）也受这些浮点选项约束。
//   实测：tools/rtaweb.cpp 第一个项目 include 就是 engine/pipeline.h。
//   若要精确控制需在每个非 DSP 头里反向 push/pop，收益不值得。
//
// 2026-11-01 实测补充：若平台头先解析了 SIMD intrinsic，其内联函数体可保留
// reassoc/arcp；调用处的 pragma 不会追溯清除。SIMD 热核必须在自己的作用域
// 使用原生向量算术和局部 pragma，并以正式编译参数及两种 include 顺序回归。
// 当前正式参数的 IR 仍可有 nsz/afn；验收结论以实际构建与输出证据为准。
//
// 历史标量探针：在 `-ffast-math` 下加本 pragma，与严格构建逐位相同。
// 这是该探针的结论，不代表所有内联函数或所有浮点环境都具有相同语义。
//
// 代价：DSP 热循环约慢 3.2 倍。DSD1024 从 5.8x 实时降到约 1.8x 实时 —— 仍然实时，
//       而算力从来不是本项目的约束（用户原话：算力不是首要问题，质量才是）。
#pragma once

#if defined(__clang__)
#  pragma clang fp reassociate(off) contract(off) reciprocal(off)
#endif
