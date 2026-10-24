// statusflags.h — 播放状态标志的**组合语义**（/api/status 与 /api/signal 的单一事实源）
//
// 为什么这个纯函数住在 meta/：
//   * 它必须能被离线单测锁定（tests/test_playback_gpu.cpp），而 rtaweb.cpp 有 main()、测试不能 include；
//   * 本工作包（BE）允许改动的范围只有 tools/rtaweb.cpp、src/meta/**、tests/**，不能动 src/dsp/**；
//   * meta/ 已经是本项目「平台无关纯函数」的存放地（entities.h / parse.h / year.h 同性质：
//     不含 windows.h，可直接被测试链接）。
//
// 背景（用户反馈 261024-04 #8）：
//   用户进 bit-perfect 直通后，信号链里没有绿点，但主页右下角仍有 GPU 绿图标。
//   根因是 /api/status 的 `gpu` 字段语义漂移：它实际表达的是「本次播放有没有用 GPU 升频」，
//   却在 UI 里被当成「GPU 图标 = 亮/暗」的布尔量，且 ps.gpu 从不在每次播放开始时复位
//   （见 rtaweb.cpp 的 PLAY 入口注释）。于是：
//     · 只要本次进程里**曾经**有一首走过 GPU 升频，之后所有曲目（包括 bit-perfect 直通）都亮；
//     · 信号链的绿点走的是另一条判据（是否发生了重采样 + UI 固定给该节点上色），两边天然不一致。
//
// 语义分工（本文件把它钉死，rtaweb 只负责填输入）：
//   gpu          = GPU **可用性**（rtagpu.dll 在不在、kernel 能不能跑）—— 与本次播放无关
//   gpuUsed      = 本次播放 GPU **真的参与了计算**（升频 或 卷积，任一为真）
//   gpuResample  = 本次播放的重采样真的跑在 GPU 上（DLL 缺失/失败时自动回落 CPU ⟹ false）
//   gpuConv      = 本次播放的 IR 卷积真的跑在 GPU 上（同上）
//
//   | 可用 | 升频走GPU | 卷积走GPU | gpu  | gpuUsed | 场景 |
//   | 否   | 否        | 否        | false| false   | 无 rtagpu.dll / kernel 不可用（全 CPU） |
//   | 是   | 否        | 否        | true | false   | **bit-perfect 直通：可用但未参与**（用户 #8） |
//   | 是   | 是        | 否        | true | true    | 升频被 GPU 接管 |
//   | 是   | 否        | 是        | true | true    | 房间校正 IR 卷积被 GPU 接管 |
//   | 是   | 是        | 是        | true | true    | 两者都走 GPU |
//
// 铁律 2（不撒谎）：gpuUsed 只由**实测**产生（PipelineStats::usedGpuConvolution /
// Resampler::gpu_active），不由"用户勾了 GPU 开关"推导 —— 勾了但 DLL 缺失就是 false。
#pragma once

namespace rta::meta {

// 输入：三路实测事实（全部来自运行期，不看设置项）
struct PlaybackGpuInput {
    bool gpuAvailable = false;   // GpuBridge::instance().available()
    bool resampleGpu  = false;   // 本次播放重采样真的是 GPU 算的
    bool convGpu      = false;   // 本次播放卷积真的是 GPU 算的
};

// 输出：给 HTTP 契约用的布尔组合（字段名与 /api/status 的 JSON 键一一对应）
struct PlaybackGpuFlags {
    bool gpu         = false;    // 可用性（保留旧字段名，语义收窄为"能用"）
    bool gpuUsed     = false;    // 本次播放真的用了 GPU
    bool gpuResample = false;    // 升频走 GPU
    bool gpuConv     = false;    // 卷积走 GPU
};

// 纯函数：无副作用、无 IO、可任意次调用（幂等）
inline PlaybackGpuFlags playback_gpu_flags(const PlaybackGpuInput& in) {
    PlaybackGpuFlags f;
    f.gpuResample = in.resampleGpu;
    f.gpuConv     = in.convGpu;
    // 参与判据只看实测的两条路径，与"可用"无关：不可用时两条实测必然为 false，
    // 但这里不依赖那个前提（万一将来有别的 GPU 路径，此处仍成立）。
    f.gpuUsed     = in.resampleGpu || in.convGpu;
    // 可用性是独立事实：bit-perfect 直通时 gpu=true 且 gpuUsed=false 是**正常**组合，
    // UI 不许再拿 gpu 当"这次用了 GPU"来点亮图标（见 rtaweb.cpp /api/status 注释）。
    f.gpu         = in.gpuAvailable;
    return f;
}

} // namespace rta::meta
