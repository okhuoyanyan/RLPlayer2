# GPU 卷积纪要（Round 15）

代码：[tools/gpu_conv.cpp](tools/gpu_conv.cpp)　构建：`hipcc --offload-arch=gfx1201 -O3 -Isrc tools/gpu_conv.cpp`

## 1. 动机（最初计划里的差异化点）

HQPlayer 的 GPU offload **只支持 NVIDIA CUDA**——安装目录里只有 `cufft64_12.dll`，
零 HIP/OpenCL/Vulkan 痕迹。所以本机的 RX 9070 XT 在**官方路径下完全闲置**。
我们用 ROCm/HIP 把它用起来。

## 2. 分工

```
CPU  每块的 FFT / IFFT（块多但每块小，传输可批量摊薄）
GPU  P 个 IR 分区的频域乘累加 —— 长 IR 卷积的主要开销
```

kernel: `Y[b][k] = Σ_{p=0..min(P-1,b)} H[p][k] · X[b-p][k]`

## 3. 实测结果

条件：块 1024，**IR 262144 抽头**，信号 48000 帧（1 秒 @48k），分区 P=256，块数 Nb=47。

| 项 | 耗时 |
|---|---|
| CPU 直接卷积 | **6156.7 ms** |
| GPU 传输 (H2D) | 2.6 ms |
| GPU kernel | **0.2 ms** |
| GPU 端到端（kernel + IFFT + 重叠相加） | **2.0 ms** |

**最大偏差 8.212e−06（相对 1.394e−07）** ⟹ 与 CPU 参考一致（float32 累加的预期精度）。

小案例（P=1, IR 1024, N 4096）：最大偏差 1.605e−07（相对 4.489e−08）。

## 4. ⚠️ 关键教训：kernel 静默不执行

**症状**：输出缓冲全零，但 `hipDeviceSynchronize()` 返回 `no error`。
**真因**：`hipGetLastError()` 返回 **`device kernel image is invalid`** ——
编译架构是 `gfx1200`（Navi 44），而设备实际是 **`gfx1201`（Navi 48）**。

改 `--offload-arch=gfx1201` 后立即正确。

**这条教训回溯解释了 Round 1 的荒谬基准**：当时得到 547 TFLOPS / 186 PFLOPS，
我判断"不可能"并记为待查——真因就是 **kernel 从未执行，计时毫无意义**。
已写入 [AGENTS.md](AGENTS.md) 铁律 5：

1. HIP kernel 首次运行前，先用**平凡 kernel（写常数）**验证能真正执行
2. 构建必须显式 `--offload-arch=gfx1201`
3. **不要相信 `hipDeviceSynchronize()` 的成功返回**

## 5. 诚实的说明

3096× 是相对 **CPU 单线程直接卷积**（O(N·M)）——它不是公平对手。
公平对照应是 CPU 分块 FFT 卷积：Round 8 里 40000 帧 + 32768 抽头约 1 秒；
本次 48000 帧 + **262144 抽头（长 8 倍）**只用 2 ms，粗估仍有 **数百倍**优势。
精确对照待后续把 GPU 版接入管线后统一测量。

## 6. 下一步

1. 把 GPU 卷积接入管线（`src/engine/pipeline.h` 的卷积阶段）
2. GPU 加速升频（多相 sinc 的分块 FFT 卷积）
3. 用 GPU 分担调制器的 FIR 部分（积分器仍是串行，不能上 GPU）
