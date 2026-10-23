# RLPlayer

**Windows 上的高保真播放引擎 + 播放器**（HQPlayer 同类），核心目标只有一个：**不计代价提升本地播放音质**，同时把每一步都变成可测量的数字。

- 输出链路完全自研：升频 → 抖动/噪声整形 → SDM/DSD 调制 → 卷积 → Matrix/EQ → WASAPI 独占 / ASIO / 网络端点
- **不追求"听起来更好"，追求"能测出来好"**：升频阻带 −189 dB、DSD 源抽取 THD+N 99.2 dB、网络传输端到端逐位相同
- 内置 WebView2 界面（托盘常驻），也可全部用 CLI 驱动
- 单包自包含：内置 ffmpeg/ffprobe、GPU 内核、WebView2 运行时，用户机上**不需要装任何运行库**

![主界面](docs/screens/10-home.png)

---

## 目录

- [特性](#特性)
- [界面](#界面)
- [快速开始](#快速开始)
- [从源码构建](#从源码构建)
- [客观指标](#客观指标)
- [项目结构](#项目结构)
- [测试与验证](#测试与验证)
- [发布包](#发布包)
- [已知限制](#已知限制)
- [文档](#文档)

---

## 特性

### DSP 链路

| 能力 | 说明 |
|---|---|
| **多相 sinc 升频** | 任意有理比率；阻带 **−189 dB**，通带纹波 **0.0000 dB** |
| **滤波器家族** | 16 个预设 + 倒谱法最小相位、混合相位；可指定抽头数 |
| **抖动 / 噪声整形** | 10 项：NS1/NS4/NS5/NS9/LNS15 + RPDF/TPDF/Gauss1/shaped |
| **SDM 调制器** | DSD5 / DSD7、多电平 AHM 风格；1-bit 带内 SNR 86.2 dB，8-bit 118.8 dB |
| **DSD 原生读取** | DSF/DFF 直接读；抽取器 FIR/IIR/CIC；**DirectSDM 逐位直通** |
| **卷积引擎** | 零延迟均匀分块 FFT，**支持 GPU**（IR 262144 抽头 / 1 秒音频 = 2.0 ms） |
| **Matrix / EQ** | 声道路由 + 双二阶 IIR EQ + 延迟插件 + REW/AutoEq 文本解析 |
| **多声道** | 5.1 / 7.1（WAVEFORMATEXTENSIBLE 声道掩码）+ 每声道增益微调 |
| **任意分块** | 分块与整块输出**逐位完全相同**，可边解码边处理 |

### 输出后端

| 后端 | 用途 |
|---|---|
| **WASAPI 独占** | bit-perfect 直通；自动协商采样率/位深，设备不支持时**如实报错不静默替换** |
| **WASAPI 共享** | 独占不可用时的兜底（非 bit-perfect，会明确告知） |
| **ASIO** | 低延迟专业声卡 |
| **NAA / RTANet** | 两机网络音频，端到端逐位相同 + 时钟漂移检测 |
| **Null** | 离线/自动化测试，不开任何设备 |

### 界面

WebView2 内嵌窗口 + 托盘常驻：专辑网格 / 详情 / 歌词 / 播放列表 / 信号链可视化 / 设置页；三套主题（深 / 浅 / 浅蓝）。

---

## 界面

| | |
|---|---|
| ![专辑](docs/screens/10-home.png) | ![专辑详情](docs/screens/11-album.png) |
| ![歌词](docs/screens/12-lyrics.png) | ![信号链](docs/screens/14-chain.png) |
| ![设置-深](docs/screens/20-dark-settings.png) | ![设置-浅](docs/screens/21-light-settings.png) |

---

## 快速开始

### 直接用发布包

1. 解压 `RLPlayer-<版本>-win64.zip`（约 555 MB，解压后约 1.3 GB）
2. 双击 `RLPlayer.exe`
3. 托盘图标右键：打开界面 / 复制地址 / 允许局域网访问 / 退出

无需安装 VC++ 运行库，无需安装 ROCm，无需单独装 ffmpeg。

### 用 CLI

```powershell
# 列出音频端点 + 独占模式支持的采样率/位深矩阵
tools\wasapi_probe.exe

# 生成测试音（默认 −60 dBFS，不可闻；铁律：绝不擅自放可听声音）
tools\rtaplay.exe --gen tone.wav 5 44100 24

# bit-perfect 直通
tools\rtaplay.exe music.flac

# 升频 + 噪声整形
tools\rtaplay.exe music.flac --rate 705600 --taps 262144 --shaper NS9 --bits 24

# 房间校正（卷积）+ 耳机 EQ + 音量
tools\rtaplay.exe music.flac --ir room.wav --eq autoeq.txt --volume -6 --gpu

# 只跑管线不开设备（验证链路首选）
tools\rtaplay.exe music.flac --rate 705600 --analyze
```

---

## 从源码构建

### 前置

| 依赖 | 说明 |
|---|---|
| CMake ≥ 3.20 + Ninja | 构建系统 |
| clang++ / lld-link | **ROCm 自带的那套**即可（无需 Visual Studio） |
| Windows SDK | 系统 API / WASAPI / ASIO 头 |
| WebView2 SDK | 已在 `third_party/webview2` 内置头文件 |

### 构建

```powershell
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_CXX_COMPILER="C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe"
cmake --build build
```

`RTA_ARCH` 默认 `x86-64-v2`（分发基线，**不含 AVX-512**，兼容 2009 年后的 CPU）；
只在自用机器上可 `-DRTA_ARCH=znver5`。

### GPU 后端（可选）

没有 GPU 也能正常工作（纯 CPU 卷积）。有 AMD 卡时：

```powershell
hipcc --offload-arch=gfx1201 -O3 -shared -Isrc `
      src/gpu/gpu_conv_hip.cpp src/gpu/gpu_resample_hip.cpp -o build/rtagpu.dll
```

> ⚠️ `--offload-arch` **必须**写对（本机 RX 9070 XT = Navi 48 = `gfx1201`）。
> 写错时 kernel 会**静默失败**而 `hipDeviceSynchronize()` 仍返回成功 —— 详见 [AGENTS.md](AGENTS.md) 铁律 5。

### 出发布包

```powershell
powershell -File tools\make_dist.ps1 -Version 1.0.8
```

会先跑兼容门禁（静态 CRT / subsystem ≤ 6.1 / 无 AVX-512），再在"PATH 只剩 System32"的临时目录里
对内置 GPU 运行时做一次卷积 smoke，任一失败即**拒绝出包**。

---

## 客观指标

全部由 `ctest` / `verify.ps1` 自动验证，不是估算：

| 项 | 实测 |
|---|---|
| 升频阻带衰减 | **−189 dB** |
| 升频通带纹波 | **0.0000 dB** |
| NS9 带内噪声 vs TPDF | 低 **45.4 dB** |
| DSD5 1-bit 带内 SNR | **86.2 dB** |
| 8-bit SDM 带内 SNR | **118.8 dB** |
| DSD 源抽取 THD+N（DSD64→176.4k） | **99.2 dB** |
| DSD 源抽取混叠抑制 | 144.4 / 156.1 / 108.1 dB（FIR/IIR/CIC） |
| 双二阶 10 种设计（解析 vs 实测） | 偏差 **0.0000 dB** |
| 网络传输（回环 690 包） | **逐位相同** |
| GPU 卷积（IR 262144 抽头 / 1 秒） | **2.0 ms** |
| 离线升频转换速度 | **9.9× 实时** |

---

## 项目结构

```
src/
  dsp/         DSP 核心：升频/滤波器/量化器/调制器/卷积/EQ（头文件库）
  audio/       WAV 读写、WASAPI 独占/共享输出、DSD 原生读取、渲染循环
  net/         RTANet 协议、抖动缓冲、漂移检测、Web 界面（ui_script.h）
  engine/      播放管线（信号链整合）
  gpu/         HIP 内核（卷积 / 重采样）
  meta/        元数据补全（实体模型 / 数据源注册表 / 标签写回）
tools/         播放器 CLI、端点程序、探针、逐位比对、打包脚本、界面自检
tests/         ctest 门禁（45 套）
docs/          各模块实测纪要与调研（含"三次假失败"等踩坑记录）
verify.ps1     一键自检（11 项，全程不打开音频设备）
AGENTS.md      开发铁律（动手前必读）
STATUS.md      逐轮状态与实测数字（最新事实在这里）
HANDOVER.md    架构 / 构建 / 已知坑清单
```

---

## 测试与验证

```powershell
ctest --test-dir build          # 45 套客观验证
powershell -File verify.ps1     # 一键自检 11 项（构建/测试/GPU/网络/Bit-exact/界面/DSD/兼容门禁）
node tools\uicheck.js http://127.0.0.1:8911/ build\uicheck_out   # 界面自检 95 项
```

**验证不打开音频设备**：所有链路验证优先离线（`--analyze`）或 Null 后端。
需要出声的测试默认电平 ≤ −60 dBFS。

---

## 发布包

| 项 | 要求 / 说明 |
|---|---|
| 系统 | Windows 7 SP1 / 8 / 10 / 11，x64 |
| CPU | SSE4.2 + POPCNT（2009 年后）；产物**不含 AVX-512** |
| 运行库 | **不需要** VC++ 运行库（CRT 静态链接） |
| 体积 | zip 约 555 MB，解压约 1.3 GB（主要是 ffmpeg 与 WebView2 运行时） |
| 内置 | ffmpeg / ffprobe、`rtagpu.dll`（25 种 AMD 架构）、`amdhip64_7.dll`、WebView2 固定运行时、12 个 CLI 工具 |

---

## 已知限制

- **输入采集**：设备枚举与设置页已有，采集→DSP→输出通路未实现
- **UPnP/DLNA 投放**：只做发现，不做推送
- **DSD 域重采样**：同速率 DirectSDM 逐位直通已可用；**速率不同时的 DSD→DSD 重采样未做**
- **Native DSD（ASIO）**：需驱动支持 ASIODSD* 通道类型
- **NVIDIA（CUDA）**：需自备 rtagpu_cuda.dll（用 tools/build_gpu_cuda.ps1 在 NVIDIA 机器上构建）
- **NAA 协议兼容**：已探测到标准 NAA 5.x，卡在未公开的 16 字节握手
- **7 阶 1-bit 调制器**：只在 DSD128 以上稳定

完整清单（含每项的判据与阻塞原因）见 [STATUS.md](STATUS.md) 与 [ROADMAP.md](ROADMAP.md)。

---

## 文档

| 文件 | 内容 |
|---|---|
| [STATUS.md](STATUS.md) | **逐轮状态与实测数字 —— 最新事实以这里为准** |
| [HANDOVER.md](HANDOVER.md) | 架构、构建、已知坑 |
| [ROADMAP.md](ROADMAP.md) | 待开发项与验收判据 |
| [AGENTS.md](AGENTS.md) | 开发铁律（7 条 + 反馈目录规范） |
| [docs/](docs/) | 各模块实测纪要、调研、踩坑记录 |

---

## 版权

私有项目，保留所有权利（All rights reserved）。未经作者书面许可，不得复制、分发或用于商业用途。

第三方组件（ffmpeg、WebView2 SDK 等）遵循其各自许可证。
