# RLPlayer 开发路线图

> 写于 2026-10-21。所有条目都按「可测」写：**判据、基线、未解决项**，没有判据的功能不算完成。
> 当前状态见 `STATUS.md`；接手前先读 `HANDOVER.md`。

## 排序原则

1. **先有验证对象，再写功能**（没有测试素材就先做素材生成器）。
2. 每条都必须能用**现有测试框架**回归（ctest / uicheck / verify.ps1）。
3. 做不到的写清楚「为什么做不到」，不留假开关。

---

## P0 · DSD 域处理（第八轮全部完成 5/5）

> **第七轮**：读取器 / 素材生成器 / 三种抽取器 / DirectSDM 核心与命令行。
> **第八轮（2026-10-21）**：DirectSDM 接进播放线程与界面（端到端逐位一致）；Conversion 三组预设与数值表。
> 实测数字见「STATUS.md」第八轮；新代码在 src/audio/dsd_reader.h、src/dsp/dsd_integrator.h、
> src/dsp/direct_sdm.h、tools/make_dsd.cpp、tools/dsdconv.cpp、tools/naa_probe.cpp。

**历史现状（第七轮之前）**：DSD 文件（.dsf/.dff）交给 ffmpeg 解成 PCM 再调制，
没有任何 DSD 域处理，DirectSDM/积分器/转换全部未实现。
没有任何 DSD 域处理，`DirectSDM`/积分器/转换全部未实现。

### P0-1 DSF / DFF 读取器（`src/audio/dsd_reader.h`）✅ 已完成（2026-10-21）
- DSF：头 28 / fmt 52 / data 按「每声道 blockSize 字节」分块交错；**位序 LSB-first**。
- DFF：FRM8/`DSD ` → PROP（FS / CHNL / CMPR）→ DSD 块；按声道连续；**MSB-first**；DST 压缩明确拒绝。
- 内部统一成「每声道 MSB-first 打包」：DoP 打包时每 16 位正好是两个相邻字节。
- **验收实测**：写→读逐位一致（3 声道分块末块不满、非整字节尾巴、DFF 声道 ID 全测）；
  另按规范手工计算偏移抽查文件字节，确认块交错与 LSB-first；截断/魔数错/DST/缺数据块都抛具体异常。
  测试 `test_dsd_reader`；ffprobe 独立识别为 `dsd_lsbf_planar`。

### P0-2 测试素材生成器（`tools/make_dsd.cpp`）✅ 已完成（2026-10-21）
- 用我们的 5 阶 1-bit 调制器生成 DSD64/128/256，写 DSF 或 DFF；写出后立刻读回逐位比对。
- **意义**：本机原本没有 DSD 文件，现在 DSD 通路有了闭环验证对象。
- **验收实测**：读回逐位一致；抽取后 THD+N **99.31 dB**（DSD64 / 5 阶 / −6 dBFS / 1 kHz）。
  测试 `test_dsd_path` 第 5 节 + `make_dsd --verify`。

### P0-3 DirectSDM（直通）（`src/dsp/direct_sdm.h`）✅ 已完成（2026-10-21，含播放器接线）
- 条件：源 DSD 速率 == 目标 DSD 速率，且无 PCM 域处理（EQ/卷积/重采样），音量 0 dB。
- **验收实测（核心）**：1-bit → DoP → 解包后**逐位一致**；与既有 `dop_pack(double)` 的载波逐样本相同；
  分块打包 == 一次性打包；3 声道通过；`dsdconv --direct` 写出的载波被 `dopinfo` 判为合法 DoP。
- **第八轮已完成**：`play_one` 开头用 `probe_dsd` 只读文件头做判定（不载入整曲），满足则走
  `play_one_direct`（按 16 位切片 → DoP → 输出后端）；不满足则回退普通路径并把原因写进信号链 `directNote`。
  界面 SDM 页有开关（勾选即压到 0 dB），键名 `directsdm`。
- **端到端实测**：播放器 → NAA（rtanet_rx 文件收端）收到的 24-bit 载波与 `dsdconv --direct` 参考
  **1,058,400 字节逐位相同**；`dopinfo` 判合法 DoP；暂停/跳转/进度都验过（跳转不重置标记相位）。

### P0-4 Integrator（抽取器）：FIR / IIR / CIC（`src/dsp/dsd_integrator.h`）✅ 已完成（2026-10-21）
- FIR：Kaiser windowed-sinc，163 抽头；阻带起点 = outRate − passband（保护 0–20 kHz）。
- IIR：Butterworth 10 阶级联（DF2T），截止由「±0.05 dB 平坦度」反推；相位非线性是固有代价。
- CIC：6 级无乘法，uint64 回绕（溢出在梳状差分里抵消），后接 3 抽头 `1/sinc^N` 解析补偿。
- **验收实测**（DSD64 → 176.4 kHz）：通带最差 **0.000025 / 0.019 / 0.031 dB**；
  156.4 kHz 镜像衰减 **123.6 / 155.9 / 106.9 dB**；实测混叠抑制 **144.4 / 156.1 / 108.1 dB**；
  THD+N **99.19 / 99.20 / 99.18 dB**（参考 FIR 4001 抽头 99.16 dB，差 < 0.04 dB）。
- 输出已直接作为现有管线的 PCM 输入：`read_any` 把 .dsf/.dff 解到 DSD/16（上限 705.6 kHz）。

### P0-5 Conversion：wide / narrow / XFi ✅ 已完成（2026-10-21）
- 映射为抽取器的截止/过渡带/阶数三组预设，**必须是真参数**，不许只改标签。
- 三组预设是**真参数**：只改通带上限（20/30/40 kHz），阻带起点 = outRate − 通带，
  过渡带随之 136.4/116.4/96.4 kHz，FIR 抽头 163/191/231。
- **数值表**（DSD64→176.4k）：带内最差 0.0000 dB；镜像衰减 123.6/122.6/122.6 dB。
  测试 `test_dsd_path` 第 6 节断言「通带＝设定值 / 平坦度 / 镜像 / 抽头单调 / 过渡带单调」。
- 入口：`dsdconv --conv`、SDM 页下拉、配置键 `dsdconv`；抽取器种类另有 `dsdkind`（fir/iir/cic）。

**回退策略**：读不了的文件仍走 ffmpeg（已实现并保留），note 里注明「已回退 ffmpeg」。

---

## P1 · 调制器族

**现状**：5 阶 / 7 阶两种，带拐点与抖动参数；7 阶在 DSD64 会饱和（自动降阶）。

- 7 阶 Chebyshev NTF（可配带外增益）；
- EC（误差反馈）变体；
- 多电平（AHM 风格，2~8 bit）。
- **验收**：1-bit 带内 SNR（DSD64/128/256）、带外噪声积分、空闲音（无输入时 1 kHz 附近的离散分量）、10 分钟长跑不饱和。
- **不做**：不复制 HQP 的 ASDM7EC 等专有曲线（系数未公开），只提供**同名分类 + 我们的实现**并在 UI 注明。

---

## P2 · 滤波器变体与命名体系

**现状**：16 个命名预设（poly-sinc-lp/mp、gauss、ext2、xtr、sinc-M、closed-form、minringFIR…）。

- 按 HQP 命名补齐变体：`poly-sinc-gauss-hires*`、`*-2s`、`-xla/-xl`、`hb-*-2s`、`sinc-{S,M,MG,L,LL}`、`closed-form-{fast,16M}`。
- **每个变体必须给出**：抽头数、过渡带、阻带衰减、相位类型（自动生成对比表 `tools/filter_report`）。
- **验收**沿用现有判据：通带纹波、阻带最深、前/后振铃能量比、能量重心。

---

## P3 · NAA 兼容（端点已到位，卡在客户端握手）

**现状（2026-10-21）**：用户提供了真实端点 **192.168.50.109**（Accurate Audio "Wavedream NET"，
后台只开 HQPlayer NAA；网页后台 /api/services/update 可切服务，但 NAA 与 UPnP 互斥）。

已确认（详见 docs/NAA对接探测纪要.md）：

- TCP 43210 是标准 NAA 5.x：连上后**服务端先发 16 字节随机数**；本机跑官方 networkaudiod 5.19 行为完全一致。
- 纯 XML（protocol / version_req / getdevices；换行/NUL/长度前缀/回显 token 等 8 种组合）**全部无回应**。
- 因此客户端必须先按未公开规则处理这 16 字节，之后才是 XML 状态机（元素名已知：
  initialize / getformats / start / stop / keepalive / netbuftime / pcm4 / sdm4 / pcm5 / sdm5）。
- 工具：`naa_scan.exe`（发现）、`naa_probe.exe`（只读 TCP 探针，--matrix）。

**卡点**：networkaudiod 的 EULA 明确禁止逆向/反汇编，也禁止「非 HQPlayer 软件推流」。
要拿到客户端那一侧的报文，需要一次真实的 HQPlayer 会话（抓包）或用户明确授权继续逆向。

**拿到授权后的步骤**：
1. 用 HQPlayer（或本机 NAA 收端 + HQPlayer 客户端）连一次端点，抓 TCP 控制面完整报文；
2. 实现只读会话（握手 + getdevices/getformats），再实现发送（pcm5/sdm5 帧）；
3. **验收**：DoP 载波逐位一致 + 连续 30 分钟无漂移/丢包（端点侧计数为准）。

**风险**：协议随版本变化；EULA 争议。**在此之前保留 RTANet 作为等价方案**（两者并列在输出后端里）。

---

## P4 · 输入采集（WASAPI）

**现状**：只枚举了设备，设置页有输入页签但**采集通路未实现**（UI 已如实标注）。

- `IAudioCaptureClient` 事件驱动 → 环形缓冲 → 现有分块管线 → 输出；
- 支持「监听」（采集直接送 DSP）与「录制到文件」；
- **验收**：采集 1 kHz 正弦，输出 PCM 的样本数/位深正确，THD+N 有数值；长跑 10 分钟无溢出。

---

## P5 · UPnP 投放

**现状**：只做发现，不做投放。

- 实现 AVTransport `SetAVTransportURI` + 本地 HTTP 提供 WAV（原始 PCM，不转码）；
- **风险**：渲染器可能自行重采样。**验收**必须抓渲染器实际采样率，不能只看「播出来了」。

---

## P6 · 性能与工程

- 解码改管道 / 进程内 libavcodec，去掉临时 WAV（起播延迟、磁盘 I/O）；
- **DSD 源抽取改两级**（CIC 先降 4× 再 FIR）：现在 DSD64 10 秒素材解码 0.26 s，但 DSD256 5 分钟的 PCM 约 700 MB、耗时数十秒；判据沿用 P0-4。
- 库扫描并行化（现在 1000 首约 30-60 秒）；
- 信号链里补「解码器/临时文件」耗时统计。

---

## P7 · 分发（1.0.1 起有构建门禁）

**2026-10-21 事故**：1.0.0 用 `-march=znver5` 构建，在无 AVX-512 的 5900X 上 0xC000001D；
且动态链接 VC++ 运行库，干净系统会 0xC000007B。已修复，见 STATUS 第九轮。

**支持矩阵（README 与发布说明必须一致）**

| 项 | 要求 |
|---|---|
| 系统 | Windows 7 SP1 / 8 / 8.1 / 10 / 11，x64 |
| CPU | SSE4.2 + POPCNT（2009+；官方支持写得保守：**Zen 1 / Intel 6 代**起） |
| 运行库 | **不需要** VC++ 运行库（CRT 静态链接），Win7 不需要 UCRT 补丁 |
| 界面 | Win10/11 内置 WebView2；无 WebView2 退回 Edge；再没有用默认浏览器；Win7 只能浏览器（需 Chrome/Edge 109/新版 Firefox） |
| GPU | 包内内置 `rtagpu.dll`（25 种 AMD 架构）+ `amdhip64_7.dll`；**另有 `rtagpu_cuda.dll`（CUDA 12.9，19 档 arch：Maxwell→Blackwell，见 docs/GPU-CUDA后端纪要.md）**；无独显/驱动时自动纯 CPU |

**构建约束（已落地）**

- `RTA_ARCH` 默认 `x86-64-v2`；**分发版禁止** `native/znver5`（会带 AVX-512）。
- 静态 CRT：`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`。
- `tools/check_compat.ps1`：检查 CRT 导入 / subsystem ≤ 6.1 / 无 zmm+EVEX；接进 `verify.ps1` 第 11 项。
- `tools/make_dist.ps1`：构建 → 门禁 → 组包 → zip + sha256；门禁失败拒绝出包。

**当前产物（1.0.2 起）**：`dist\RLPlayer-1.0.2-win64.zip`（约 173 MB，**唯一**的
“正式发布版”/Full 包：ffmpeg + ffprobe、rtagpu.dll、amdhip64_7.dll、WebView2Loader、
asio_probe、`tools\` CLI 工具；sha256 见 STATUS 第十轮）。

**发布规则（用户 2026-10-21 明确要求）**：
1. 不再有 Lite/最小发布版，只有 Full，名字叫“正式发布版”；
2. GPU 加速所需组件（内核 + HIP 运行时）必须内置；
3. 软件内有的功能必须自包含完整（解码器、工具、运行库齐备）；
4. 出包前必须过兼容门禁 + GPU 干净 PATH smoke。

---

## 明确不做 / 暂缓

- **复制 HQP 专有调制器曲线**：系数未公开，不做「同名不同声」的假实现。
- 在线歌词/封面（需要第三方接口与合规评估）。
- 云音乐平台接入（版权/合规风险）。
- NAA 兼容在拿到端点之前不动工。
