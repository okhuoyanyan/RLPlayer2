# HQPlayer 同级高保真播放引擎 —— 执行计划 v1

> 工作目录：`D:\RTA-hqplayer`　·　代号（暂定）：**RTA-Player**
> 目标：**音质优先**的自研 Windows 原生播放引擎 + NAA 网络音频协议 + HQPlayer 级自定义功能
> 本项目为**清洁室自研**：不破解、不绕过授权、不修改原版二进制；原版仅作行为基准（官方试用版）。

---

## 0. 结论先行

三件事决定这个项目成败，而**其中一件我们已经领先原版**：

| # | 结论 | 依据 |
|---|---|---|
| 1 | **原版 HQPlayer 的 GPU 加速只支持 NVIDIA CUDA**（安装目录只有 `cufft64_12.dll`，无任何 HIP/OpenCL/Vulkan 痕迹） | 二进制实测 |
| 2 | **你的 RX 9070 XT 已经可以跑通用计算**：ROCm 7.2 + clang 21 已装好，`hipInfo` 成功识别设备（15.92 GB / 2.4 GHz），HIP kernel 实测编译并运行成功，`rocfft`/`rocrand`/`rocblas` 齐全 | 本机实测通过 ✅ |
| 3 | **完整 C++ 工具链已就绪，无需安装 Visual Studio**：ROCm clang 21 + `lld-link` + Windows SDK 10.0.26100，已实测编译并运行 C++ 与 HIP 程序成功 | 本机实测通过 ✅ |

> **这意味着：原版用不了的显卡，我们能用来跑百万抽头卷积与超长滤波器。这是"不计代价提升音质"最实在的一张牌。**
>
> ⚠️ 另一个必须纠正的预期：**NAA 协议本身不影响音质**（实测确认它只是 XML 控制 + TCP 裸流搬运）。所以本项目把"复刻 NAA"定位为**互操作性目标**，而把音质投入放在 DSP 精度、滤波器长度、GPU 算力与**端点电学质量**上。详见 §6.3。

---

## 1. 事实基线（已核实）

### 1.1 本机硬件与软件环境

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 7 9800X3D（8C/16T，Zen 5，**支持 AVX-512**，96 MB 3D V-Cache） |
| GPU | AMD Radeon RX 9070 XT（RDNA4 / **Navi 48 = gfx1201**，PCI 0x7550，HIP 报告 32 CU，15.92 GB，2.4 GHz） |
| RAM | 61.6 GB |
| 网卡 | Realtek 2.5GbE + Intel Wi-Fi 6E AX210 |
| 已识别音频端点 | Realtek HD Audio、AMD HDMI×2、AMD Streaming Audio、**TASCAM Portacapture X8** |
| GPU 计算栈 | ROCm 7.2（clang 21 / hipcc / rocFFT / rocBLAS / rocRAND）、OpenCL ICD、`vulkan-1.dll` |
| 编译链 | ROCm clang++21 + lld-link + Windows SDK 26100（**已编译成功并运行**）｜无 MSVC，`ninja` 已有 |
| 抓包 | `pktmon`（系统自带）｜无 Wireshark |
| 磁盘可用 | C 1187 GB / D 177 GB / E 3129 GB / G 2560 GB / Z 5219 GB |

### 1.2 原版 HQPlayer 6 的架构与能力（来自官方手册 v6.0.2 + 二进制）

- **架构**：Client–Server 分离。`HQPlayer6Desktop.exe`（引擎+配置）/ `HQPlayer6Client.exe`（播放 UI）/ `hqp6-control.exe`（命令行控制）。
- **控制协议**：已从 `hqp6-control.exe` 提取出**完整命令表**（`--set-filter`、`--get-shapers`、`--set-matrix`、`--play-next-uri`… 共 60+ 条），可作兼容层设计输入；另含 `libmicrohttpd.dll` → 存在 HTTP/Web 界面。
- **NAA 线索**：mDNS 服务名 `com.signalyst.networkaudiod`；多播组加入/离开日志；后端名 `NetworkAudioAdapter` / `NetworkAudioAdapter IPv6`。
- **DSP 核心**：`double128Proj.dll`（128 位精度投影）、`libomp.dll`（OpenMP）、`cufft64_12.dll`（CUDA FFT）。
- **能力上限**：PCM 32–1536 kHz；SDM 至 DSD1024/48（45.2 MHz）；128 声道 Matrix pipeline；卷积引擎（overlap-add/save）；DAC 专用校正曲线；DoP v1.1 / 2wire。
- **流媒体**：手册仅列 Qobuz 与 HRA（highresaudio.com）。

### 1.3 官方安装包

| 项 | 值 |
|---|---|
| 文件 | [HQPlayer6Desktop-setup-611.exe](D:\RTA-hqplayer\HQPlayer6Desktop-setup-611.exe)（251 MB） |
| 签名 | ✅ **有效**，`CN=Signalyst, O=Signalyst, L=Siuntio, C=FI`（GlobalSign 代码签名） |
| SHA256 | `62E2CAEDB6EE857956EA161ED29D8E70EBF45E40DA7B445C04D9B7B7537F9A4B` |
| 现状 | 现有安装 `C:\Program Files\Signalyst\HQPlayer 6 Desktop` 为无授权第三方版本（含 `wk_key.dat`），**不可用** |

---

## 2. 技术选型

| 层 | 选型 | 理由 / 备选 |
|---|---|---|
| 语言 | **C++23** | DSP 需 AVX-512 内建函数 + HIP 内核；生态成熟。备选 Rust（安全性好但手写 SIMD/HIP 互操作成本高） |
| 编译器 | **ROCm clang++ 21（`-march=znver5`）** | 已实测可用；AVX-512 代码生成质量优。可选补装 MSVC Build Tools 兼容第三方库 |
| 构建 | CMake + Ninja + `lld-link`；HIP 编译必须显式 `--offload-arch=gfx1201`（hipcc 默认 gfx906，实测不加会错编） | ninja 已就绪 |
| 内部精度 | **64 位浮点为主 + 关键路径 double-double（≈106 位有效）** | 对齐原版 `double128Proj.dll` 的设计意图；调制器积分器用整数/定点 |
| GPU（卷积/FFT） | **HIP + rocFFT 为主，Vulkan+VkFFT 为跨厂商后备** | 已实测 HIP 可用；rocFFT 原生 RDNA4。**这是相对原版的差异化优势** |
| GPU（调制器） | 不迁移。**AVX-512 CPU 内核** | 1-bit 调制器是逐样点串行反馈链，GPU 无法并行，多核也无效 → 吃单核 IPC + L3（9800X3D 恰好强项） |
| 输出后端 | **WASAPI 独占（事件驱动）** 为主 + **ASIO**（原生 DSD）+ **NAA**（网络） | 共享模式仅供回退；另实现 DoP v1.1 |
| 解码 | FLAC / WavPack / ALAC 原生 + FFmpeg 兜底（mp3/aac/ogg/ape…）+ 自研 DSF/DFF/DST | 全格式覆盖，DSD 路径完全自控 |
| 控制面 | 自研 **REST + WebSocket**；**同时实现 HQPlayer 控制协议兼容层**（TCP 4321） | 兼容层让官方 HQPlayer Client / hqp-control / Roon 能直接驱动我们的引擎 |
| UI | **本地 Web UI**（浏览器/平板/手机通用）+ 托盘小程序 | 迭代快、跨端、易做丰富自定义面板 |

---

## 3. 系统架构

```
┌────────────── 控制面 ──────────────┐   ┌──────────── 数据面 ────────────┐
│ Web UI (REST+WS)                   │   │ 解码器 (FLAC/WV/ALAC/FFmpeg)   │
│ HQPlayer 控制协议兼容层 (TCP 4321) │──▶│   ↓ 无锁环形缓冲 (lock-free)   │
│ hqp6-control CLI 兼容              │   │ DSP 图: 重采样 → 抖动/整形     │
│ UPnP / Roon 输入                    │   │   → 卷积 → Matrix → 音量       │
└────────────────────────────────────┘   │   ↓                            │
                                          │ 输出: WASAPI独占 / ASIO / NAA  │
              ┌────── GPU (HIP/rocFFT) ──┴── 分块 FFT 卷积 / EQ / FFT 重采样 │
              └────────────────────────────┘
```

**关键设计约束（来自调研）**：
1. **调制器是唯一硬串行瓶颈** —— 无法 FFT 加速、无法按时间分段并行。因此：调制器走 CPU 单核高频路径，GPU 只分担 FIR/卷积/EQ。**实时性靠缓冲深度换，不靠并行**。
2. **DSP 线程与 UI/网络线程物理隔离** —— 独立进程 or 至少独立线程池 + CPU 亲和性绑定 + MMCSS "Pro Audio" 优先级。
3. **内存**：音频缓冲用大页 + `VirtualLock` 锁定，禁止分页。

---

## 4. 分阶段路线图

| 阶段 | 交付物 | 验收标准 | 预估 |
|---|---|---|---|
| **P0 环境与基准** | 官方 6.1.1 安装并跑通试用；GPU 吞吐真实基准（FP32/FP64/带宽/rocFFT）；`pktmon` 抓包链路打通；REA 工具链装好 | 能记录原版的行为基线；GPU 基准数字可复现 | 1–2 天 |
| **P1 比特完美本地播放** ⭐ | WASAPI 独占事件驱动输出；FLAC/WAV/DSF/DFF 解码；直通模式（none 滤波器 + 无处理） | **环回位比对证明比特完美**；44.1k–1536k 自动切换采样率 | 3–4 周 |
| **P2 升频引擎（音质核心）** | 多相 sinc 滤波器族（poly-sinc-gauss / ext2 / xtr / sinc-M 等价物）；1x/Nx 独立选择；NS1/NS4/NS5/NS9/LNS15/TPDF/RPDF/Gauss1/shaped 抖动族 | 实测 THD+N、带外噪声、脉冲响应；A/B 与原版听感对比 | 4–6 周 |
| **P3 DSD 调制器（最难）** | 1–7 阶 delta-sigma 调制器 + EC 补偿拓扑；DSD64→DSD1024；Integrator 可选；DirectSDM 直通 | DSD512 稳定无 xrun；DSD1024 至少 light 档可用；带外噪声达标 | 6–10 周 |
| **P4 GPU 卷积与 Matrix** | HIP+rocFFT 分块卷积（overlap-add/save）；128 声道 Matrix；插件 delay/iir/riaa；REW/AutoEq 文本解析；Bauer 交叉馈送、Loudness | 1M taps × 8ch 实时；CPU 占用可接受 | 4–6 周 |
| **P5 网络音频** | **A** 自研开放协议 `RTANet`（收发两端，两机方案主力）<br>**B** NAA 发起端兼容（驱动现成 NAA 硬件，纯互操作性） | A：本机 → 端点机 → DAC 全链路出声，PCM/DSD、采样率切换、时钟反馈稳定<br>B：能被官方 `networkaudiod` 正确握手并出声 | A 4–8 周<br>B 12–16 周 |
| **P6 控制面与生态** | Web UI、HQPlayer 控制协议兼容层、库管理、播放列表、UPnP 输入、主题/预设 | 官方 HQPlayer Client 能连上我们的引擎 | 4–6 周 |

**里程碑建议**：P1+P2 完成即"已经比大多数播放器好听"；P3 完成即"进入 HQPlayer 同级竞争"。

---

## 5. "不计代价提升音质"的具体抓手

按性价比排序，逐条可落地：

**A. 消除一切非必要处理（比特完美基座）**
1. WASAPI **独占 + 事件驱动**；按文件原生格式建流，换采样率即重开流（绝不偷偷 SRC）
2. 关闭端点"音频增强"/APO；独占失败给出明确告警而非静默降级
3. 处理链全程 **不碰 float32 无损往返**；直通模式零增益、零淡入淡出
4. 独占模式下**不做任何音量数字衰减**（默认固定 -3 dB 留 intersample 余量，或用外置模拟音量）

**B. 把 DSP 做到极致（升频红利）**
5. **超长多相 sinc 滤波器**：Kaiser/Blackman-Harris 窗，百万级抽头（GPU 分块 FFT 实现），阻带衰减 >180 dB
6. **Apodizing 滤波**：修正录音链的振铃；提供最小相位/线性相位/中间相位全谱系
7. **1x 与 Nx 独立选滤波器**（44.1/48 与高码率走不同最优路径）
8. **DSD 直解为 1-bit 域处理**，避免 PCM 往返
9. **高阶噪声整形**（NS9/LNS15 等价物）修正 R2R DAC 的线性度误差

**C. 精度与时钟**
10. 内部 **double-double（≈106 位）** 关键路径，消除累积舍入误差
11. 调制器积分器用**整数/定点**，杜绝浮点漂移
12. **输出到 DAC 的时钟域隔离**：WASAPI 独占下由 DAC 主时钟驱动（异步 USB）；缓冲区用非分页内存

**D. 系统级**
13. DSP 线程：MMCSS "Pro Audio" + `TIME_CRITICAL` + 绑定**物理核**（避开 SMT 兄弟核）
14. 关 C-State / Core Parking / PCIe ASPM / USB 选择性挂起；GPU 省电策略关闭
15. USB：优先 CPU 直连控制器，避免 Hub；异步 USB Audio
16. **两机方案（终极）**：DSP 机器与 DAC 机器物理分离，中间走 NAA/光纤网络 → 电气噪声与地环路隔离

**E. 客观验收（拒绝玄学）**
17. 建立测量工装：环回位比对、THD+N、带外噪声谱、脉冲响应、DPC 延迟
18. 每个 DSP 模块用**客观指标**验收，主观听感作为最终裁判但必须有数据支撑

---

## 6. 网络音频协议方案（NAA 已实测取证，策略已修正）

### 6.1 NAA 协议 —— 实测取证结果（不再是猜测）

| 项 | 实测结果 |
|---|---|
| **端口** | **43210，TCP + UDP 同号**（UDP 收发现报文，TCP 收音频数据） |
| **发现方式** | **组播**（非广播，作者称广播是特权操作）。IPv4 `224.0.0.199` / `239.192.0.199`；IPv6 `ff02::c7` / `ff05::c7` |
| **控制报文** | **XML 文本**。HQPlayer 日志：`discovered network audio: name='naa-xxxx' version='Signalyst Network Audio Daemon 5.19' protocol='5' trigger='1' @ip:43210` |
| **报文元素** | discover / protocol / version / getdevices / initialize / getformats / rate / bits / format / netbuftime / stream / start / stop / keepalive / position / metadata / trigger |
| **数据通道** | 单条 TCP 43210：先 XML 握手，再切**裸流**（日志 `enter streaming mode, version=`）；二进制含 `clSocket::SetNagle()` 与 QoS/DSCP 调用 → 确认 TCP |
| **音频封装** | 版本化帧 `pcm4` / `sdm4` / `pcm5` / `sdm5`；**逐字节布局未公开**，需抓包逐字段试 |
| **时钟** | 无 PTP/RTP；端点回传 `position` 做流控；DAC 为主时钟，无显式时钟反馈报文 |
| **官方公开度** | 手册 65 页对 "43210/组播/IPv6" 提及 **0 次**；无规范、无源码、无 Wireshark dissector；Signalyst GitHub **0 个仓库** |
| **社区复刻** | **GitHub 上无任何完整 NAA 复刻**（仅 Docker 打包类项目）→ 等于从零逆向 |

**结论：可以复刻，但只能黑盒兼容。协议本体不复杂（XML 控制 + 裸 PCM/SDM 流），难点在 `pcm4`/`sdm5` 帧的逐字节布局。**

### 6.2 三条路线（修正后）

| 路线 | 用途 | 工作量 | 风险 | 定位 |
|---|---|---|---|---|
| **A. 自研开放协议 `RTANet`** | 我们自己的两机方案（DSP 机 → 端点机 → DAC） | 1–2 月 | 无 | ⭐ **主力**：RTP 式序号 + 抖动缓冲 + **显式时钟反馈** + 可选 FEC，可以做得比 NAA 更好 |
| **B. NAA 发起端兼容** | 让我们的播放器能驱动**现成的 NAA 硬件** | 3–4 月 | EULA 逆向争议 | ⭐ **互操作性**：只做"发送端"（HQPlayer 那一侧），难度低于做收端 |
| **C. Squeezelite 兼容** | 接第三方开源端点，生态最广 | 1–2 月 | 无（协议公开） | 备选，有需要再做 |

### 6.3 关键认知修正 ⚠️

> **NAA 本身不决定音质。** 决定音质的是**端点的时钟、供电与缓冲策略**——协议只是搬运工。
>
> 所以"复刻 NAA"对音质的直接贡献 ≈ 0，它的价值是**互操作性**（能驱动别人的 NAA 设备）。
> 真正提升两机方案音质的，是把工程和预算投到**端点的电学质量**上：无风扇、线性电源、优质 USB/I2S 输出、DAC 主时钟回传、以及我们自研协议里比 NAA 更强的时钟反馈与缓冲策略。

### 6.4 取证与工程方法（已验证可行）

1. **抓包**：官方 `networkaudiod 5.19` 可免费下载，本机已实测跑通（UDP/TCP 43210 正常监听）
2. **中继代理**：自研 TCP/UDP 中继，让播放器连代理、代理转发给真实 NAA 并全量落盘 —— **无需环回抓包驱动**（本机无 Wireshark）
3. **静态分析**：REA 工具链（`npx rea-agents setup` + Ghidra）定位帧布局与状态机
4. **回归测试**：用官方 `networkaudiod` 做对端，逐字段验证兼容性

---

## 7. 功能对齐清单（对应"原版丰富自定义"）

- **滤波器**：none / IIR / FIR / FFT 长度可调；poly-sinc 族（lp/mp/short/long/hb/ext/ext2/xtr/mqa）、gauss 族（short→xla/hires/halfband）、closed-form、sinc-S/M/L 族、ASRC、polynomial；`*-2s` 两级；1x/Nx 独立
- **抖动/整形**：NS1/NS4/NS5/NS9/LNS15 + RPDF/TPDF/Gauss1/shaped；DAC bits 选择
- **SDM 调制器**：1–7 阶 + EC 补偿族 + 多电平（5-level / 8-bit）；Integrator 可选（IIR/FIR/CIC）；SDM→SDM 转换；DirectSDM 直通
- **速率**：PCM 32–1536 kHz；DSD64–DSD1024（含 48 基）
- **卷积**：overlap-add / overlap-save、WAV IR、IR 增益估算与补偿、Expand HF
- **Matrix**：≤128 声道、Source/Mix/Gain、卷积 + REW 参量 EQ + 插件（delay/iir/riaa）、IIR→FIR 相位转换、DAC 校正、profile 热切换
- **后处理**：Bauer 交叉馈送、音量自适应 Loudness、自适应增益、软拐点限幅、20 kHz 滤波
- **声道平衡**：每声道距离（cm）+ 电平（dB）+ 粉噪测试音
- **主界面**：封面/曲目/时间/限幅表/Apod 计数器/模式指示、拖放播放、播放列表、重复随机
- **库管理**：目录扫描、格式过滤、封面提取、专辑增益
- **输入源**：本地文件、CD、UPnP、网络流 URL、Roon（兼容层）、流媒体（需单独评估授权）
- **控制端**：Web UI、手机/平板、HQPlayer Client 兼容、多媒体键/遥控

---

## 8. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| **EULA 禁止反向工程**（NAA/控制协议） | 🔴 高 | 严格清洁室：只看**网络报文**（黑盒行为）+ 公开文档；静态分析仅用于理解**互操作性接口**，不复制代码；不碰授权校验逻辑。**建议购买一份官方授权**用于合法 A/B 对照 |
| 调制器实时性不达标 | 🔴 高 | 先做 DSD256/512 保底；DSD1024 用轻量调制器档；必要时引入"多电平 SDM"降低串行压力 |
| NAA 复刻失败 | 🟡 中 | 自研开放协议并行推进（见 §6） |
| GPU/驱动稳定性（HIP 在 Windows） | 🟡 中 | HIP 与 Vulkan/VkFFT 双后端；GPU 失败自动回退 CPU 路径 |
| 音质无法量化对比 | 🟡 中 | 先建测量工装（§5E），再写 DSP |
| 第三方库授权（ASIO SDK 非自由许可、FFmpeg LGPL/GPL） | 🟡 中 | ASIO 仅作可选后端并隔离；FFmpeg 用 LGPL 构建、动态链接 |
| 范围膨胀 | 🟡 中 | 严格按 P0–P6 里程碑推进，每阶段可独立交付 |

---

## 9. 需要你确认的问题

1. **DAC 是什么型号？** 这是决定音质上限的头号因素。是否支持原生 DSD（ASIO）？最高 DSD 率？有无 I2S/S/PDIF？是否支持 DoP？（当前只看到 Realtek/AMD 板载 + TASCAM X8）
2. **是否走"两机方案"**（DSP 服务器 + NAA 端点 + DAC）？有没有第二台小机器（迷你主机/树莓派），还是需要我推荐采购？
3. **音乐库位置与格式**（Z: 盘？NAS？）PCM 与 DSD 的大致比例？
4. **控制端偏好**：手机/平板浏览器，还是 Windows 桌面客户端？
5. **是否购买一份官方 HQPlayer 授权**（约 €300 级）作为合法 A/B 对照基准？试用版每 30 分钟中断，做长对比不便。
6. **官方 6.1.1 怎么装**：直接覆盖现有无授权版本，还是**先彻底卸载再全新安装**（更干净，推荐）？
7. **技术栈确认**：C++23 + ROCm clang/HIP（我的推荐），还是你想用 Rust？
8. **优先级**：先把"本地比特完美 + 顶级升频"（P1→P2）做出来，还是先啃 NAA 协议？

---


---

## 10. 附录：手册事实修正（**开发前必读，防止按直觉写错**）

> 以下 5 条来自对官方手册 v6.0.2 的逐项核对，与"想当然"的预期不符。完整依据见 [hqplayer-features-spec.md](research/hqplayer-features-spec.md)。

| # | 修正 |
|---|---|
| 1 | **没有 NS2 / NS3 / NS6 / NS7 / NS8**。抖动表只有 **NS1、NS4、NS5、NS9、LNS15、RPDF、TPDF、Gauss1、shaped**（共 10 项）。不要凭编号直觉补实现。 |
| 2 | **没有 "direct" 调制器后缀**。只有 **DirectSDM** 这个设置项（关闭全部处理、禁用音量、PCM 音量固定 −3 dBFS）。 |
| 3 | **buffer time 不在 Advanced**，在 **4.2 Outputs（10–100 ms）**；**DAC bits 在 4.4 PCM**（Holo=20 / Denafrips=19 / LAiV=18）。手册中**根本没有"高分辨率音量"**这一项。 |
| 4 | 手册**全文无 "1.5M"**；最高 PCM 写作 **32–1536 kHz**，最高 DSD 为 **DSD1024**（brickwall 截止 400 kHz）。 |
| 5 | 手册**未给任何滤波器的阻带衰减 dB 值**（仅 low / average / high / extremely high 定性描述）；-3 dB 带宽只有积分器给出。**验收指标需我们自己定义**，不能照抄手册。 |

**规模**：功能对齐清单共 **165 项**可选项/可调项（滤波器 69、调制器 36、抖动 10、DSD→PCM 噪声滤波 13、DSD→PCM 转换 14、积分器 8、SDM→SDM 3、Advanced 11、DirectSDM 1），另含数值指标 80 条。

## 11. 附录：文档与素材索引

| 文件 | 内容 |
|---|---|
| [PLAN.md](PLAN.md) | 本计划 |
| [hqplayer-features-spec.md](research/hqplayer-features-spec.md) | **功能对齐清单**（165 项，含 1x/Nx 矩阵、DSD 适配矩阵、振铃/apodizing 速查、工程实现方案） |
| [NAA-调研报告.md](research/NAA-调研报告.md) | NAA 协议实测取证报告 |
| [hqplayer6-feature-dsp-open-source-report.md](research/hqplayer6-feature-dsp-open-source-report.md) | 功能盘点 + 开源替代 + 算力可行性 |
| [hqplayer6-dsp-brief.md](research/hqplayer6-dsp-brief.md) | 上述报告的精简版 |
| [hqplayer6-manual.txt](research/hqplayer6-manual.txt) | **权威**手册全文（其行号被规格书引用） |
| research/strings/ | 二进制字符串证据（hqp6desktop 1.6 MB、hqp6control、networkaudiod） |
| research/naa/networkaudiod-5.19/ | 官方 NAA 端点 5.19（做对端回归测试用） |

*本计划基于本机实测 + 官方手册 v6.0.2 + 公开资料；所有"推测"已在调研报告中标注。*
