# GPU 卷积接入管线纪要（Round 16）

代码：[src/gpu/gpu_conv_hip.cpp](src/gpu/gpu_conv_hip.cpp)（DLL）、[src/dsp/gpu_bridge.h](src/dsp/gpu_bridge.h)（动态加载）、[src/engine/pipeline.h](src/engine/pipeline.h)（接入）

## 1. 架构决策：动态加载，不做链接期依赖

```
管线 ──LoadLibrary──> rtagpu.dll ──HIP──> Radeon
  │                       │
  │ 找不到 DLL            │ kernel 跑不起来（架构不匹配）
  ▼                       ▼
  自动退回 CPU 分块 FFT 卷积（两条路都自动退回）
```

**为什么不直接链接**：GPU 是「锦上添花」，不该让整个项目变成必须有显卡才能构建/运行。
普通的 CPU 构建完全不受影响。

**关键守门**：`rta_gpu_available()` 不只查设备数量，而是**真的跑一个平凡 kernel 写常数并读回**——
因为架构不匹配时 `hipDeviceSynchronize()` 会返回「成功」而输出全零（Round 15 的教训）。

## 2. 实测（管线内）

```
[PASS] 卷积阶段生效（GPU 或 CPU 退回） (GPU: GPU 卷积可用 | 走了 GPU=是 | 最大偏差 0.50 LSB, 超限样本 0)
[PASS] GPU 路径结果与 CPU 一致（x0.5，误差 ≤1 LSB）
```

**GPU 路径与 CPU 路径结果一致到 0.5 LSB**（浮点卷积的取整平局差异，见 Round 10 的记录）。

配合 Round 15 的性能数据（IR 262144 抽头 / 48000 帧）：

| 路径 | 耗时 |
|---|---|
| CPU 直接卷积（参考） | 6156.7 ms |
| **GPU 端到端（kernel + IFFT + 重叠相加）** | **2.0 ms** |

## 3. 现状与边界

**已可用**：
- 管线里 `ChannelProcess::useGpu = true` 即走 GPU，失败自动退回
- 状态可在 `PipelineStats::gpuStatus` 与 `usedGpuConvolution` 里查到

**边界（诚实说明）**：
- GPU 路径目前是**整块卷积**，跨管线调用的滤波器历史状态不保留；
  CPU 路径的 `PartitionedConvolver` 同样每次调用重建，二者语义一致，
  但**若要支持连续流式的超长 IR 实时卷积，需要把环形历史也搬到 GPU**——尚未做。
- 只有卷积用了 GPU，**升频与调制器的 FIR 部分仍未 GPU 化**。
- 调制器的反馈环是**逐样本串行**的，本质上无法用 GPU 并行（见 Round 3 分析）。

## 4. 构建方式

```powershell
# GPU 加速库（可选组件）
hipcc --offload-arch=gfx1201 -O3 -shared -Isrc src/gpu/gpu_conv_hip.cpp -o build/rtagpu.dll
# 其余照常
cmake --build build
```

## 5. 项目整体进度

| 目标要求 | 状态 |
|---|---|
| ① 比特完美本地播放链路 | ✅ 逐位可证 + WASAPI 独占出声 |
| ② NAA / 开放协议 | 🔶 端到端逐位相同 + 漂移检测；闭环补偿与 NAA 兼容未做 |
| ③ 接近原版的自定义功能 | ✅ 滤波器/调制器/抖动/卷积/Matrix/EQ/音量/解析 |
| 差异化：AMD GPU 加速 | ✅ **已接入管线**（原版只支持 CUDA） |

**尚未完成**：GPU 升频、漂移闭环、NAA 兼容 / UPnP、Web UI、7 阶 1-bit 调制器稳定性。
