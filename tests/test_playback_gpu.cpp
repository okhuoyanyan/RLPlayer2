// test_playback_gpu.cpp — /api/status 的 gpu / gpuUsed 组合语义（用户反馈 261024-04 #8）
//
// 背景（用户原话）：「我进入 bit-perfect，信息链路没看到绿色的点（就是没有 GPU），
// 但是主页右下角还是有 GPU 绿色图标亮着，是什么逻辑？是绿点错了还是什么意思？」
//
// 根因（代码级）：rtaweb.cpp 里 ps.gpu 只有渲染线程会置 true（rl.gpuOut = &ps.gpu），
// 而且**正常播放路径从不复位**（旧代码只有 DirectSDM 分支写过 ps.gpu=false）。
// 于是一旦本进程里有一首走过 GPU 升频，之后所有曲目（含不重采样的 bit-perfect 直通）
// 都对外报 gpu=true，右下角图标恒亮；信号链的绿点却由"是否发生重采样"决定 ⟹ 两边打架。
//
// 本测试锁死修好之后的语义（真值表见 src/meta/statusflags.h）：
//   gpu     = 可用性（与本次播放无关）
//   gpuUsed = 本次播放 GPU 真的参与了计算（升频 或 卷积）
// 判据是"字段组合"，不是"能编译"——3 个真实场景（可用未参与 / 参与 / 不可用）逐条对齐。
#include "meta/statusflags.h"

#include <cstdio>
#include <string>

using namespace rta::meta;

static int g_fail = 0;
static void ck(bool cond, const std::string& what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s%s%s\n", what.c_str(), detail.empty() ? "" : "  ", detail.c_str());
    else { std::printf("  [FAIL] %s  %s\n", what.c_str(), detail.c_str()); ++g_fail; }
}

static std::string flags_text(const PlaybackGpuFlags& f) {
    return std::string("gpu=") + (f.gpu ? "1" : "0") + " gpuUsed=" + (f.gpuUsed ? "1" : "0") +
           " gpuResample=" + (f.gpuResample ? "1" : "0") + " gpuConv=" + (f.gpuConv ? "1" : "0");
}

int main() {
    std::printf("== /api/status GPU 语义（gpu 可用性 vs gpuUsed 本次参与）==\n");

    // ---------------- 1) 不可用：没有 rtagpu.dll / kernel 跑不起来 ----------------
    std::printf("-- 1. 场景 A：GPU 不可用（全 CPU）--\n");
    {
        PlaybackGpuInput in;
        in.gpuAvailable = false;
        in.resampleGpu = false;
        in.convGpu = false;
        const PlaybackGpuFlags f = playback_gpu_flags(in);
        ck(!f.gpu, "gpu=false（不可用）");
        ck(!f.gpuUsed, "gpuUsed=false（没参与）");
        ck(!f.gpuResample && !f.gpuConv, "两条路径都未走 GPU", flags_text(f));
    }

    // ---------------- 2) 可用但未参与：这是用户 #8 撞到的组合 ----------------
    std::printf("-- 2. 场景 B：GPU 可用但本次没参与（bit-perfect 直通）--\n");
    {
        PlaybackGpuInput in;
        in.gpuAvailable = true;      // 机器有 rtagpu.dll
        in.resampleGpu = false;      // 不重采样（bit-perfect）
        in.convGpu = false;          // 没开 IR 卷积
        const PlaybackGpuFlags f = playback_gpu_flags(in);
        ck(f.gpu, "gpu=true（可用性）");
        ck(!f.gpuUsed, "gpuUsed=false —— 这一条就是 #8 的修法：右下角图标必须按 gpuUsed 点亮", flags_text(f));
        ck(!f.gpuResample && !f.gpuConv, "两条路径均未走 GPU");
        // 反面断言：把这个组合当成"在用 GPU"就是本次反馈的 bug
        ck(!(f.gpu && f.gpuUsed), "gpu 与 gpuUsed 是两件事，不许互相推导");
    }

    // ---------------- 3) 参与：升频 ----------------
    std::printf("-- 3. 场景 C：升频走 GPU --\n");
    {
        PlaybackGpuInput in;
        in.gpuAvailable = true;
        in.resampleGpu = true;
        in.convGpu = false;
        const PlaybackGpuFlags f = playback_gpu_flags(in);
        ck(f.gpu && f.gpuUsed && f.gpuResample && !f.gpuConv,
           "gpu=1 gpuUsed=1 gpuResample=1 gpuConv=0", flags_text(f));
    }

    // ---------------- 4) 参与：卷积 ----------------
    std::printf("-- 4. 场景 D：房间校正 IR 卷积走 GPU --\n");
    {
        PlaybackGpuInput in;
        in.gpuAvailable = true;
        in.resampleGpu = false;
        in.convGpu = true;
        const PlaybackGpuFlags f = playback_gpu_flags(in);
        ck(f.gpu && f.gpuUsed && !f.gpuResample && f.gpuConv,
           "gpu=1 gpuUsed=1 gpuResample=0 gpuConv=1", flags_text(f));
    }

    // ---------------- 5) 两条路径都走 GPU ----------------
    std::printf("-- 5. 场景 E：升频与卷积都走 GPU --\n");
    {
        PlaybackGpuInput in;
        in.gpuAvailable = true;
        in.resampleGpu = true;
        in.convGpu = true;
        const PlaybackGpuFlags f = playback_gpu_flags(in);
        ck(f.gpu && f.gpuUsed && f.gpuResample && f.gpuConv, "四项全 1", flags_text(f));
    }

    // ---------------- 6) 纯函数纪律：幂等 + 不依赖历史 ----------------
    std::printf("-- 6. 纯函数纪律（幂等 / 无状态）--\n");
    {
        PlaybackGpuInput in;
        in.gpuAvailable = true;
        in.resampleGpu = true;
        in.convGpu = false;
        const PlaybackGpuFlags a = playback_gpu_flags(in);
        const PlaybackGpuFlags b = playback_gpu_flags(in);
        ck(a.gpu == b.gpu && a.gpuUsed == b.gpuUsed && a.gpuResample == b.gpuResample &&
               a.gpuConv == b.gpuConv,
           "同输入两次调用结果完全一致（无内部状态，不会像 ps.gpu 那样粘住上一曲）");

        // 上一曲用了 GPU、这一曲没用 —— 只要每曲重新填输入，结果就是干净的
        PlaybackGpuInput next;
        next.gpuAvailable = true;
        next.resampleGpu = false;
        next.convGpu = false;
        const PlaybackGpuFlags c = playback_gpu_flags(next);
        ck(a.gpuUsed && !c.gpuUsed, "上一曲 gpuUsed=1、下一曲 gpuUsed=0（每曲复位是调用方的责任）");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
