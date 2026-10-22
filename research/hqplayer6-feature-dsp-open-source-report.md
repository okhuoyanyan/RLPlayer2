# HQPlayer Desktop 6 功能盘点 + 开源可替代实现调研（清洁室自研）
依据：本机官方手册 `C:\Program Files\Signalyst\HQPlayer 6 Desktop\hqplayer6desktop-manual.pdf`（v6.0.2，已抽文本存 [hqplayer6-manual.txt](D:\RTA-hqplayer\research\hqplayer6-manual.txt)）+ 安装目录二进制第三方库证据 + 公开资料。以下【事实】= 手册/官方文档/二进制直接可见；【推测】= 工程推断。

## 1. 用户可见功能盘点【事实】
- **重采样滤波器（手册 4.6）**：none/IIR/IIR2/FIR/asymFIR/minphaseFIR/FFT（长度可配）；poly-sinc-lp/mp/shrt-*/long-*、poly-sinc-hb(-xs/-s/-m/-l)、poly-sinc-ext、poly-sinc-ext2(-short/-medium/-long/-xl/-xla/-hires-lp/ip/mp)、poly-sinc-mqa|mp3-lp/mp、poly-sinc-xtr(-lp/mp/short-lp/short-mp)、poly-sinc-gauss(-short/-medium/-long/-xl/-xla/-hires-lp/ip/mp/-halfband)、ASRC、polynomial-1/2、minringFIR-lp/mp、closed-form(-fast/-M/-16M)、sinc-S/M/Mx/MG/MGa/L/Ls/Lm/Ll/Lh/short/medium/long/long-h；后缀 `*-2s` 为两级（≥8x 后二级转换）；1x/Nx 独立可选；以 Apod 计数器判据。
- **SDM 调制器（4.5）**：DSD5/5v2/5EC、DSD7、ASDM5(EC/ECv2/v3/-ul/-light/-fast/-super，含 512+fs 变体)、ASDM7(同族全家)、AMSDM7 512+fs、AMSDM7EC 512+fs、AHM5EC5L/AHM7EC5L（5-level hybrid）、AHM5EC8B/AHM7EC8B（8-bit hybrid）；Integrator：IIR/IIR2/IIR3/FIR/FIR2/FIR-bl/FIR-bw/CIC；SDM→SDM Conversion：wide/narrow/XFi；DirectSDM 位直通。
- **抖动/噪声整形（4.4）**：none/NS1/NS4/NS5/NS9/LNS15 + RPDF/TPDF/Gauss1/shaped；DAC bits、DSD 6dB、20kHz 滤波、自适应增益、软拐点限幅。
- **速率**：PCM 32–1536 kHz（DSD 源可转 176.4/352.8/705.6 再升）；SDM 至 DSD1024/48（45.2 MHz，SDM 页出现 20.48/40.96 MHz）。
- **卷积**：overlap-add / overlap-save、单声道 RIFF WAV IR、IR gain 估计、Expand HF、建议 352.8 kHz IR、长 IR 多声道算力警告。
- **Matrix pipeline**：最多 128 虚拟通道、Source/Mix/Gain、Process = WAV 卷积 + RoomEqWizard 参量 EQ + 插件（delay/iir/riaa）、IIR→FIR（最小/线性相位）转换、Bauer crossfeed、Loudness、DAC Correction、profile 热切换（可远程）。
- **架构/接口**：Client–Server（HQPlayer6Client.exe + HQPlayer6Desktop.exe + 命令行 **hqp6-control.exe**，含 --set-filter/--set-shaping/--matrix-list/--play-next-uri 等）；本机可见 **libmicrohttpd.dll**（HTTP/Web 界面）、大量 UPnP 字符串、Qt6WebEngineCore、CUDA（cufft64_12.dll）。
- **流媒体**：手册只列 **Qobuz 与 HRA Streaming (highresaudio.com)**；Tidal 在 6.x 手册无出现（Tidal 通常经 Roon/Tidal Connect 作输入源）【事实+推断】。
- **Roon**：Roon 设置 Add HQPlayer + IP；元数据/封面/PDF booklet；DSDIFF/DSF 原生与 DoP 播放。

## 2. 开源可替代实现映射
| 模块 | 首选开源 | 说明/许可 | 来源 |
|---|---|---|---|
| 多相 sinc 重采样 | libsoxr、SoX/sox_ng、r8brain-free-src | soxr 为 SoX 抽取独立库(LGPL)；r8brain(MIT) 自带 PFFFT 分块卷积、任意比 | [libsoxr](https://github.com/chirlu/soxr)、[r8brain](https://github.com/avaneev/r8brain-free-src)、[sox_ng](https://codeberg.org/sox_ng/sox_ng) |
| 1-bit DSD 调制器 | SoX `dither -p 1`（sigma-delta + shibata/lipshitz/gesemann 等噪声整形表）、python-deltasigma | SoX 2015 加入 DSD SDM；python-deltasigma 可设计/仿真 5、7 阶 NTF | [SoX SDM 提交](http://yhbt.net/sox.git/log/msvc10?h=pu&id=818bdd0ccc1e5b6cae742c740c17fd)、[python-deltasigma](https://python-deltasigma.readthedocs.io/)、[sox 手册](https://manpages.ubuntu.com/manpages/resolute/man1/sox.1.html) |
| DSD→PCM 抽取 | FFmpeg DSD 解码 + 自研 CIC/半带 FIR | FFmpeg 解 dsf/dff/DST；抽取(standard/low/high-order/poly-*)需自研 | [FFmpeg ticket](https://trac.ffmpeg.org/ticket/8770)、[sacd-ripper](https://github.com/setmind/sacd-ripper) |
| 分块卷积 | CPU: FFTW / PFFFT / KissFFT；GPU: **VkFFT**(MIT, Vulkan/HIP/OpenCL/CUDA)、rocFFT、cuFFT | overlap-add/save 自实现，均匀分区 + 2 幂块最稳 | [VkFFT](https://github.com/DTolm/VkFFT)、[AOCL-FFTW](https://www.amd.com/en/developer/aocl/fftw.html) |
| 参量 EQ/路由 | 自研 RBJ biquad + AutoEq/REW 文本解析 | 手册 iir 插件参数与 RBJ cookbook 一致【推测同源】 | [AutoEq](https://github.com/jaakkopasanen/AutoEq) |
| 控制/Web/UPnP | 自研 REST/WebSocket + libmicrohttpd；UPnP 用 libupnp/gupnp | hqp6-control 的 CLI 参数表可当兼容层设计输入 | [HQP 适配器](https://github.com/open-horizon-labs/unified-hifi-control/blob/main/src/adapters/hqplayer.rs) |

## 3. 算力现实性（9800X3D + RX 9070 XT）
【事实】HQPlayer 的 GPU offload **只有 CUDA**（二进制含 sm_75…sm_121 + cufft64_12.dll，无 HIP/OpenCL/Vulkan 痕迹）→ 官方路径下 AMD 卡完全帮不上忙。
【事实】AMD 官方兼容矩阵含 **RX 9070 XT (RDNA4, gfx1201)**，Windows 11 的 HIP SDK 标注支持。
【工程估算，推测，需实测校准】
- 7 阶 1-bit 调制器：DSD1024 立体声 ≈ 90 M sample/s 串行调制，NTF 每样点约 20–40 次 MAC ≈ 2–4 GFLOP/s；但**逐样点串行、依赖链短，多核基本无效**，吃单核 IPC 与缓存（X3D 大 L3 有利）；多核只在多声道时有用。DSD512 裕量约 2 倍。
- 1M taps 卷积：352.8/384 kHz 域 2ch × 1M ≈ 0.7 TMAC/s，纯 CPU 不可实时；**必须 GPU FFT 分块**。GPU 理论 FP32 约 97 TFLOPS（≈48 TMAC/s），按访存折损 10–20% 有效，多声道百万级 taps 可实时【推测】。
- **调制器是唯一硬串行瓶颈**：无法 FFT 加速、无法按时间分段并行；GPU 只能分担 FIR/卷积/EQ。
- 结论：DSD512 + 7 阶 EC 单机可行；**DSD1024/48 + super 档大概率 xrun**，需 512+fs 轻量档并优先高主频/大缓存；百万级 taps 必须上 GPU。

## 4. AMD GPU(Win) 通用计算路径与推荐
| 路径 | 现状【事实】 | 推荐 |
|---|---|---|
| ROCm/HIP SDK (Windows) | 官方支持 gfx1201；产出 .dll 供自研程序调用 | ★★★★ 与 CUDA 同构，一套 kernel 双后端 |
| **Vulkan Compute + VkFFT** | VkFFT 支持 Vulkan/HIP/OpenCL/CUDA；AMD 驱动成熟 | ★★★★★ 首选：跨厂商、零 SDK 依赖、FFT 现成 |
| OpenCL | 驱动自带但生态停滞 | ★★ 兜底 |
| DirectML | 已进入维护模式、实质被取代 | ★ 不选 |
**推荐**：卷积/EQ/分块 FFT → Vulkan compute(VkFFT) + HIP 双后端；调制器/抽取 → 自研 AVX-512 CPU 内核（大页 + 无锁环形缓冲）；控制面 → REST/WebSocket + libmicrohttpd。

## 5. 风险与工作量（含结论性建议）
- 风险高：HQPlayer 算法/协议未公开且 EULA 禁止反向工程，只能按手册行为等效复刻，音色无法 bit 级一致 → 必须自建客观度量（THD+N、带外噪声、脉冲响应）作为验收。
- 调制器 CPU 内核（AVX-512 + 噪声整形滤波器设计）：约 3–6 人月（最难、最核心）。
- 重采样 + 卷积 + Matrix（含 IR 重采样、8ch GPU 分块、EQ 插件）：约 3–5 人月；GPU 同步/驱动是主要坑。
- 生态（ASIO/WASAPI/NAA 输出、UPnP、Roon 兼容、Qobuz/HRA 授权）：约 2–3 人月，法务先于工程。
- 里程碑：①CPU DSD256/512 + poly-sinc-gauss 族 + ASDM7EC-light + CPU 卷积；②VkFFT 卷积；③Matrix(EQ/delay/RIAA/crossfeed)；④DSD1024 + 控制/Client/Web。