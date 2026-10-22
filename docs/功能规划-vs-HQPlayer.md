# 功能规划：对标 HQPlayer（Round 72）

> 依据：用户提供的 HQPlayer Desktop 6 实机截图（主界面 + Outputs/Inputs/PCM/SDM/Advanced 五个设置页）。

## 0. 用户明确的取舍

1. **参考 HQPlayer 的功能，但不照抄它的 UI 与曲库** —— 这两块是 HQPlayer 常年的差评点。
2. 补齐大量细节功能。
3. **持续提升音质**，小改进也要做（明确提出「先把整首歌载入 RAM 再播」这类思路）。

## 1. HQPlayer 截图里暴露的能力清单（按设置页归类）

### Outputs

| 项 | 我们 | 备注 |
|---|---|---|
| Backend（WASAPI/ASIO/NAA…） | 部分 | 只有 WASAPI + 自研 RTANet |
| Device / Channels / Channel offset | 部分 | 有 device，无声道偏移 |
| **SDM pack（none / DoP）** | ✅ 有（--dsd） | 需要设备支持 176.4k+ |
| **Hardware buffer time（Default / Short buffer）** | 部分 | 有 --buffer ms |
| **Dual wire**（双 AES 链路） | ❌ | 专业设备才需要 |
| **48k DSD** | ❌ | DSD 用 48k 基频族 |
| Default mode（[source] / 固定率） | ✅ 有（--rate / --rate-max） | |
| **Quick pause** | ❌ | 快速暂停（不重新开流） |
| **Adaptive rate** | ❌ | 自动跟随源采样率族 |
| Volume min / max | 部分 | 有音量，无范围限制 |
| **Fixed volume** | ✅ 有常量 | 未接进 UI |
| **PCM gain compensation** | ❌ | DSD/PCM 切换时的增益补偿 |

### PCM

| 项 | 我们 | 备注 |
|---|---|---|
| Filter 1x / Filter Nx（分开） | ⚠️ 部分 | **我们有 16 个预设，但 1x/Nx 不分开** |
| Sample rate (/Limit) | ✅ | |
| Dither | ✅ 10 种 | |
| Bits | ✅ | |
| DSD sources: Noise filter / Conversion / 6dB gain | ❌ | DSD 输入路径 |

### SDM

| 项 | 我们 | 备注 |
|---|---|---|
| Oversampling 1x / Nx | ⚠️ | 同 PCM |
| Modulator（ASDM7EC-fast 等） | ⚠️ 部分 | 有 DSD5/多电平，无 EC 家族 |
| Bit rate (/Limit) | ✅ --dsd 64/128/256 | |
| DSD sources: Integrator / Conversion / DirectSDM | ❌ | |

### Advanced

| 项 | 我们 | 备注 |
|---|---|---|
| **Multicore DSP** | ❌ | 多核并行处理 |
| **E-core allocation** | ❌ | 大小核分配 |
| **CUDA offload / Default CUDA / Convolution CUDA** | ✅ 我们更强 | **我们直接支持 AMD ROCm，原版只支持 NVIDIA** |
| **DSP pipelines / Blocks per cycle / Idle time** | ❌ | 处理粒度调优 |
| **FFT filter length（512 等）** | ⚠️ | 我们有 convBlock，未暴露 |
| **Pre-pro before meter** | ❌ | |
| **Playlist album gain** | ❌ | 专辑增益归一化 |
| **Freewheel（UPnP）** | ❌ | |
| Log file | ❌ | |

### 主界面（UI 部分 —— **这是要改进而不是照抄的**）

| HQPlayer 有 | 我们的方案 |
|---|---|
| 播放/暂停/停止/上一首/快退/快进/下一首/弹出 | ✅ 做（需要队列） |
| 随机 / 单曲循环 / 列表循环 | ✅ 做 |
| 封面 + 曲目 00/00 + Artist/Performer/Album/Song | ✅ 做，并用标签读真实元数据 |
| 时间 / 剩余 / 总剩余 三选一 | ✅ 做 |
| **Adaptive gain（-3.0 dBFS）** | ✅ 做（响度归一化） |
| Content source URI + Browse | ⚠️ 保留「浏览」，但做**树形目录 + 搜索**，不做 URI 输入框 |
| 播放列表表格（#/Length/Artist/Album/Song） | ✅ 做，但支持**拖拽排序、双击播放、右键移除** |

## 2. 明确不照抄的部分

| HQPlayer 的毛病 | 我们的做法 |
|---|---|
| Content source URI 手输路径 | 目录树浏览 + 最近使用 + 拖放文件 |
| 曲库只是「一个表格」，没有搜索/筛选/分组 | **可搜索、可按专辑/艺术家分组、可排序** |
| 没有正在播放的高亮与进度跟随 | 当前曲目高亮 + 进度条 + 自动滚动 |
| 设置散落在 5 个模态对话框 | **单页分区设置，改完立即生效**（不需要关窗口） |
| 中文/长文件名显示不佳 | 完整 UTF-8，长名省略中间 |
| 无拖放支持 | 支持从资源管理器拖进来 |

## 3. 音质提升路线（按我判断的收益排序）

### 已经做到的（不要重复做）

- 整块处理：`read_any` 把整个文件解码进内存 → `Pipeline::process` 产出**完整的输出缓冲** → 才开设备。
  **也就是说「载入 RAM 再播」这件事，当前实现已经做到了。** 用户担心的「边读盘边播」并不存在。
- 独占模式 + 事件驱动 + 不做预填充（Round 6 验证过）
- 全程 double 精度
- GPU 卷积累加（262144 抽头 1 秒数据 2.0 ms）

### 接下来要做的（真正还能提速/提质的）

| # | 措施 | 为什么有效 | 预期 |
|---|---|---|---|
| 1 | **预解码下一首（后台线程）** | 当前切歌要重新解码+重跑整条 DSP，间隙可达数秒；预解码后切歌接近瞬时 | 消除切歌间隙 |
| 2 | **缓存滤波器/重采样器原型** | sinc-M 是 524287 抽头，每首歌都要重新设计一次 | 起播时间从秒级降到毫秒 |
| 3 | **播放线程提到 TIME_CRITICAL** | 避免被其他线程抢占导致欠载 | 抗欠载 |
| 4 | **进程优先级 HIGH** | 同上 | 抗欠载 |
| 5 | **`timeBeginPeriod(1)`** | 默认 15.6ms 定时器精度会让节流粗糙（Round 6 踩过） | 稳定的喂数节奏 |
| 6 | **`VirtualLock` 锁定音频缓冲** | 大缓冲（如 96k/24bit 的 5 分钟 ≈ 170MB）可能被换页到磁盘 | 消除偶发爆音 |
| 7 | **1x / Nx 滤波器分开设置** | 44.1k 源和 96k 源用不同滤波器是 HQPlayer 的常规做法 | 更匹配的滤波 |
| 8 | **比 100ms 更大的可选缓冲** | 降低欠载风险 | 稳定性 |
| 9 | **禁用设备音效（SysFx）** | 探针显示 X8 的 SysFx 是「未设置」，最好显式关闭 | 避免系统偷偷加处理 |
| 10 | **共享模式下的音频增强检查** | 独占模式已绕过，但可加检测提示 | 排障 |
| 11 | **FFT 块长可调（512/1024/2048）** | 卷积延迟与算力的权衡 | 可调细节 |
| 12 | **幂等/无分配的热路径** | 播放循环里避免 vector 重分配 | 减少抖动 |

## 4. 分阶段计划

### 阶段 A（本轮）：队列 + 自动下一首 + 预解码 + 系统级调优

- 播放队列：加入/移除/排序、自动播下一首、上一首/下一首
- **后台预解码下一首**（真正的「载入 RAM 再播」升级版）
- 播放线程 TIME_CRITICAL、进程 HIGH、timeBeginPeriod(1)、VirtualLock

### 阶段 B：UI 重做（不照抄 HQPlayer）

- 左侧目录树 + 搜索框；右侧队列（可拖拽）
- 封面 + 元数据（ffprobe 读 Artist/Album/Title）
- 时间显示三模式、随机/循环
- 单页设置分区，改完立即生效

### 阶段 C：细节功能补齐

- 1x/Nx 滤波器分开、FFT 块长、缓冲大小、固定音量
- Adaptive gain（响度归一化）、专辑增益
- Quick pause、Adaptive rate
- DSD 输入路径（DSF/DFF 直通）、DirectSDM

### 阶段 D：更强的 DSP

- 多核并行 DSP、Blocks per cycle
- EC 调制器家族
- 48k DSD 族