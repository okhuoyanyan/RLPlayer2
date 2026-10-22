# RLPlayer

一个自研的高保真 Windows 播放引擎 + 播放器（HQPlayer 同类），核心目标：**不计代价提升本地播放音质**。

> 本文按开发轮次累积，部分段落是历史记录；**当前能力、实测数字与已知限制以 [STATUS.md](STATUS.md) 为准**。

**界面长什么样**：`docs/screens/` 里有 7 张验收截图（专辑网格 / 专辑详情 / 歌词 / 播放列表 / 信号链 / 设置 / 最近播放）。

## 最快上手（双击即用）

    build\RLPlayer.exe           # 托盘图标 + Edge 应用窗口，服务在后台
    # 托盘右键：打开界面 / 复制地址 / 允许局域网访问（手机控制）/ 退出

## 快速开始（从源码）

```powershell
# 构建（无需 Visual Studio：ROCm clang 21 + lld-link + Windows SDK）
#   RTA_ARCH 默认 x86-64-v2（分发基线，不带 AVX-512）；本机开发可 -DRTA_ARCH=znver5
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_CXX_COMPILER="C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe"
cmake --build build
ctest --test-dir build          # 28 套客观验证
powershell -File verify.ps1     # 一键自检（11 项，含界面、DSD、DirectSDM 与分发兼容性，不打开音频设备）
powershell -File tools\make_dist.ps1 -Version 1.0.1   # 出分发包（先过兼容门禁）

# GPU 加速（可选组件；没有它也照常工作）
hipcc --offload-arch=gfx1201 -O3 -shared -Isrc src/gpu/gpu_conv_hip.cpp src/gpu/gpu_resample_hip.cpp -o build/rtagpu.dll
```

## 播放

```powershell
build\rtaplay.exe --list                       # 列出音频端点
build\rtaplay.exe --gen tone.wav 5 44100 24    # 生成测试音（默认 -60 dBFS，不可闻）

# 比特完美直通
build\rtaplay.exe music.wav

# 升频 + 噪声整形
build\rtaplay.exe music.wav --rate 705600 --taps 262144 --shaper NS9 --bits 24

# 房间校正 + 耳机 EQ + 音量
build\rtaplay.exe music.wav --ir room.wav --eq autoeq.txt --volume -6 --gpu

# 两机方案：推给网络端点
build\rtanet_rx.exe --sink wasapi --ctrl-port 43210      # 在端点机上跑
build\rtaplay.exe music.wav --rate 768000 --net 192.168.50.x:43210

# DSD：造素材 / 查看 / 抽取成 PCM / DirectSDM 逐位直通
build\make_dsd.exe --out test.dsf --seconds 3 --verify
build\dsdconv.exe --in test.dsf --info
build\dsdconv.exe --in test.dsf --out pcm.wav --kind fir
build\dsdconv.exe --in test.dsf --direct --out dop.wav
```

> ⚠️ **铁律**：`--gen` 默认 −60 dBFS。播放前会打印警告横幅（独占模式会静音系统其他声音）。
> 验证链路请优先用 `--analyze`（只跑管线，不打开设备）。

## 已实现

| 手册章节 | 能力 |
|---|---|
| 4.2 | 音量控制（渐变、固定 −3/−6 dB、0 dB 逐位精确） |
| 4.4 | 抖动/噪声整形 10 项（NS1/NS4/NS5/NS9/LNS15 + RPDF/TPDF/Gauss1/shaped） |
| 4.5 | SDM 调制器（DSD5/DSD7、多电平 AHM 风格）+ DoP 输出（DSD64/128/256，逐块状态保持） |
| 4.6 | 滤波器家族（**16 预设** + 倒谱法最小相位） |
| — | 多相 sinc 升频（任意有理比率，阻带 −189 dB） |
| 6 | 卷积引擎（零延迟均匀分块 FFT，**支持 GPU**） |
| 7 | Matrix 路由 + IIR EQ + 延迟插件 + REW/AutoEq 文本解析 |
| — | WASAPI 独占 / ASIO / NAA（RTANet）/ Null 四种输出后端 |
| — | 多声道 5.1 / 7.1（WAVEFORMATEXTENSIBLE 声道掩码）+ 每声道增益微调 |
| — | DSD64~DSD1024 调制（DSD1024 离线 0.8 倍实时） |
| 4.6/4.7 | **DSD 源原生读取**（DSF/DFF）+ 抽取器 FIR/IIR/CIC + Conversion（narrow/wide/XFi）+ **DirectSDM 播放器直通**（界面开关，端到端逐位一致） |
| — | RTANet 网络音频协议（端到端逐位相同 + 时钟漂移检测） |

## 客观指标（全部由 `ctest` 验证）

| 项 | 实测 |
|---|---|
| 升频阻带衰减 | **−189 dB** |
| 升频通带纹波 | **0.0000 dB** |
| 最小相位能量重心前移 | **约 400 倍** |
| NS9 带内噪声 vs TPDF | **低 45.4 dB** |
| DSD5 1-bit 带内 SNR | **86.2 dB** |
| DSD 源抽取 THD+N（DSD64→176.4k） | **99.2 dB**（FIR/IIR/CIC 与 4001 抽头参考差 < 0.04 dB） |
| DSD 源抽取通带平坦度 | **0.00003 / 0.019 / 0.031 dB**（FIR/IIR/CIC，0–20 kHz） |
| DSD 源抽取混叠抑制（实测） | **144.4 / 156.1 / 108.1 dB** |
| 8-bit SDM 带内 SNR | **118.8 dB** |
| 双二阶 10 种设计（解析 vs 实测） | **偏差 0.0000 dB** |
| 网络传输（回环，690 包） | **逐位相同** |
| GPU 卷积（IR 262144 抽头 / 1 秒） | **2.0 ms** |

## 目录

- [PLAN.md](PLAN.md) — 总体计划与事实基线
- [AGENTS.md](AGENTS.md) — **开发铁律（动手前必读）**
- [docs/](docs/) — 各模块实测纪要
- [src/dsp/](src/dsp/) — DSP 核心（头文件库）
- [src/audio/](src/audio/) — WAV 读写 + WASAPI 独占输出
- [src/net/](src/net/) — RTANet 协议 + 抖动缓冲 + 漂移检测
- [src/engine/](src/engine/) — 播放管线（信号链整合）
- [tools/](tools/) — 播放器、端点程序、探针、逐位比对

## 验证脚本

- verify.ps1         — 一键完整自检（10 项，含 DSD 通路与 DirectSDM 端到端；全程不打开音频设备）
- verify-stream.ps1  — 分块不变性门禁（4 种块长，含 333 这种非对齐值）

## 流式能力（Round 33–35）

管线支持**任意分块**输入，且**分块与整块输出逐位完全相同**（已用 777/1024/8192 三种块长验证，
链路中同时挂着 EQ + 卷积 + 升频）。意义：

- 可边解码边处理，不必把整个文件读进内存
- 离线转换实测 **9.9× 实时**
- 分块不变性是硬断言，纳入了测试门禁

```powershell
rtaplay in.wav --out out.wav --rate 705600 --ir room.wav   # 离线转换
rtaplay in.wav --out out.wav --chunk 8192                  # 分块处理
```

## 分发包与兼容性（1.0.2 起：只有一个“正式发布版”）

1.0.0 曾用 `-march=znver5` 构建、且动态链接 VC++ 运行库：在 Ryzen 5900X（无 AVX-512）上
**0xC000001D 非法指令**，在干净系统上会缺 VCRUNTIME140.dll。1.0.1 修了兼容性，
但仍有 Lite/Full 两个包、且不含 GPU 后端；**1.0.2 起只有 Full，叫“正式发布版”**：

| 项 | 要求 |
|---|---|
| 系统 | Windows 7 SP1 / 8 / 8.1 / 10 / 11，x64 |
| CPU | SSE4.2 + POPCNT（2009+；官方下限 Zen 1 / Intel 6 代）；产物**不含 AVX-512** |
| 运行库 | 不需要 VC++ 运行库（CRT 静态链接） |
| 界面 | 无边框一体化窗口：顶栏集成搜索/主题/最小化/最大化/关闭；包内内置 WebView2 固定运行时（\WebView2Runtime），失败才退回 Edge/默认浏览器 |
| 体积 | 约 555 MB 的 zip；解压后约 1.3 GB（主要是 ffmpeg 与 WebView2） |
| 解码器 | 内置 ffmpeg/ffprobe，FLAC / MP3 / APE / OGG / WMA 等开箱即用 |
| GPU | 内置 `rtagpu.dll`（25 种 AMD 架构）+ `amdhip64_7.dll`（HIP 运行时）；无 AMD 卡自动纯 CPU |
| 工具 | `tools\` 下有 rtaplay / dsdconv / make_dsd / rtanet_rx / 探针等 CLI |

    dist\RLPlayer-1.0.4-win64.zip  约 555 MB    正式发布版（Full：ffmpeg + GPU 运行时 + WebView2 + CLI 工具）

播放器**优先在 exe 同目录找 ffmpeg/ffprobe**（其次 PATH）；GPU 的 HIP 运行时也在同目录，
用户不需要安装 ROCm（显卡驱动仍由用户安装）。兼容性门禁 `tools/check_compat.ps1`
（静态 CRT / subsystem ≤ 6.1 / 无 zmm+EVEX，含 rtagpu.dll）由 `tools/make_dist.ps1` 强制执行；
GPU 运行时还会在“PATH 只剩 System32”的临时目录里做一次卷积 smoke，失败即拒绝出包。


## 尚未完成

- **输入采集**：设备枚举与设置页已有，采集→DSP→输出的通路未实现
- **UPnP/DLNA 投放**：只做发现，不做推送
- **SDM 源升频（DSD 进 DSD 出）**：DSD 文件已原生读取并抽取成 PCM；DirectSDM 同速率直通已接入播放器与界面（端到端逐位一致）。**速率不同时的 DSD 域重采样仍未做**
- **Native DSD（ASIO）输出**：需要驱动的通道类型是 ASIODSD*；本机 DAC 驱动不是
- NVIDIA（CUDA）后端：只有加载路径与选项，仓库里没有 CUDA 源码（本机无 CUDA 工具链、无 N 卡）
- 时钟漂移闭环已实现并验证（仿真 300 秒收敛 0.03–0.42 ppm），但真实两机长测待做
- **NAA 协议兼容**：真实端点已探测（标准 NAA 5.x），卡在客户端 16 字节握手（未公开），需用户授权/抓包；详见 docs/NAA对接探测纪要.md
- UPnP 投放（发现已就绪）
- 在线歌词、在线封面
- 7 阶 1-bit 调制器只在 DSD128 以上稳定

## 采样率选择（Round 66-67）

不必事先知道 DAC 支持什么：

```powershell
rtaplay music.wav --rate-max        # 自动选用设备支持的最高采样率
```

指定了设备不支持的采样率时，会直接列出可用值：

```
打开输出失败: Initialize 失败: UNSUPPORTED_FORMAT
  该设备在独占模式下支持的采样率: 44100 / 48000 / 96000
  请改用其中之一的 --rate
```

### 先探清设备能力（接入任何 DAC 的第一步）

```powershell
D:\RTA-hqplayer\build\wasapi_probe.exe
```

输出一张 **采样率 x 位深** 的独占模式支持矩阵，包含全部 DoP 承载率
（176400 / 352800 / 705600）。若这些都不支持，则该设备**无法承载 DoP**。

本机默认设备实测：44.1 / 48 / 96 kHz @ 24-bit（无 88.2k 家族、无 16-bit），
最小缓冲周期 3 ms。详见 docs/P1-WASAPI-实测纪要.md。

## 网页播放器 rtaweb（Round 70）

命令行太麻烦，所以做了一个本地网页界面。

### 启动

```powershell
cd D:\RTA-hqplayer
build\rtaweb.exe
```

然后浏览器打开 **http://127.0.0.1:8899/**

指定端口：`build\rtaweb.exe --port 9000`

### 界面能力

- **左侧文件浏览**：目录跳转、点文件夹进入、点 WAV 文件即播放
- **右侧参数**：采样率（自动/源/具体值）、16 个滤波器（含最小相位）、10 种抖动整形、音量、等响度补偿、缓冲
- **顶栏**：当前文件 + 停止按钮
- **进度**：播放位置 / 总时长、输出采样率与位深

### 设计要点

| 项 | 做法 |
|---|---|
| 停止 | **协作式**：置停止标志让播放线程收尾，不杀进程（铁律 2） |
| 监听范围 | 只绑 127.0.0.1，不对外网开放 |
| 播放警告 | 页面顶部有醒目横幅（铁律 1 第 3 条） |
| 采样率"自动" | 调 `exclusive_rates()` 选设备最高，等价于 `--rate-max` |
| GPU | 移除 —— GPU 是每声道开关且只对卷积有意义，UI 暂不加载 IR |

### 实测

| 项 | 结果 |
|---|---|
| `GET /` | HTTP 200，7409 字节 |
| `GET /api/files?dir=...` | 正确列出目录与 WAV 文件 |
| `GET /api/status` | 返回 playing/pos/dur/outRate/outBits |
| `POST /api/play` | 成功启动，实测 96000 Hz / 24-bit / poly-sinc-mp / NS9 |
| `POST /api/stop` | 干净停止，无残留进程 |

### 说明（本节是 Round 70 的历史记录）

之后界面整体重做过：Roon 式三栏（专辑 / 艺术家 / 曲目 / 文件夹 / 流媒体 / 设置），
右栏队列 / 歌词 / 播放列表，信号链弹层，三套主题，全部解码格式，暂停 / 跳转，
播放列表（m3u8），增量库扫描。**当前能力以 [STATUS.md](STATUS.md) 为准。**

## 音频格式支持（Round 71）

### 支持列表

| 格式 | 支持 | 说明 |
|---|---|---|
| WAV | ✅ 原生 | 零依赖，逐位精确 |
| FLAC | ✅ | 16/24-bit 均**逐位验证过**（含 24-bit 高解析） |
| ALAC (.m4a) | ✅ | |
| MP3 / AAC | ✅ | |
| OGG Vorbis / Opus | ✅ | |
| WMA / WavPack / APE | ✅ | 解码器支持即可 |
| AIFF / CAF | ✅ | |
| DSF / DFF | ✅ | DSD 文件按其容器解码 |

### 实现方式

- **WAV 走原生解析**（`src/audio/wav.h`）—— 保持逐位精确、零依赖
- **其他格式交给 ffmpeg** 解码成对应位深的 PCM WAV，再用原生 WAV 读取

关键点：

1. **解码是确定性过程**。无损格式解出的 PCM 与原文件携带的 PCM 完全一致，
   因此**不破坏比特完美**这一核心主张。已验证：

   | 验证 | 结果 |
   |---|---|
   | 真实 FLAC（198 秒 / 16-bit） vs ffmpeg 直接解码 | **逐位完全相同**（34981296 字节） |
   | 24-bit FLAC vs 参考解码 | **逐位完全相同**（1323000 字节） |

2. **位深先探测再解码**：用 ffprobe 读 `bits_per_raw_sample`，再请求
   `pcm_s16le` / `pcm_s24le` / `pcm_s32le` —— 避免把 24-bit 降成 16-bit，
   也避免把 16-bit 无谓抬成 24-bit。

3. 临时文件方式是刻意的：不经管道，避免 Windows 上子进程 stdio 的各种坑。
   代价是长文件需要临时磁盘空间（198 秒 CD 音质约 35 MB）。

### 依赖

需要 **ffmpeg** 在 PATH 中（本机已有：`C:\Users\Administrator\AppData\Local\Microsoft\WinGet\Links\ffmpeg.exe`）。
找不到时会明确报错：

```
读取失败: 需要 ffmpeg 才能解码 .flac 文件，但 PATH 中找不到 ffmpeg
```

WAV 不需要 ffmpeg。

### 验证工具

```powershell
build\aidecode.exe 输入文件 输出.wav    # 走统一解码路径并写出 WAV，便于逐位比对
```
