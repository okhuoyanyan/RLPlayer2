# HQPlayer Desktop 6 功能/DSP 与开源替代调研（清洁室自研）
来源：本机官方手册 v6.0.2（[已抽文本](D:\RTA-hqplayer\research\hqplayer6-manual.txt)）＋安装目录二进制证据＋公开文档；详细版 [报告全文](D:\RTA-hqplayer\research\hqplayer6-feature-dsp-open-source-report.md)。【事实】=手册/官方/二进制可见；【推测】=工程推断。

## 1. 功能清单【事实】
- **滤波器**（手册 4.6）：none/IIR/IIR2/FIR/asymFIR/minphaseFIR/FFT(长度可配)；poly-sinc-lp/mp/shrt/long、hb(-xs/-s/-m/-l)、ext/ext2(-short/-medium/-long/-xl/-xla/-hires-lp/ip/mp)、mqa|mp3-lp/mp、xtr(-lp/mp/short-*)、gauss(-short/-medium/-long/-xl/-xla/-hires-*/-halfband)；ASRC、polynomial-1/2、minringFIR、closed-form(-fast/-M/-16M)、sinc-S/M/Mx/MG/MGa/L/Ls/Lm/Ll/Lh/short/medium/long/long-h；`*-2s` 两级；1x/Nx 独立选、Apod 判据。
- **调制器**（4.5）：DSD5/5v2/5EC、DSD7、ASDM5(EC/ECv2/v3/-ul/-light/-fast/-super 及 512+fs)、ASDM7(全家)、AMSDM7(EC) 512+fs、AHM5EC5L/AHM7EC5L、AHM5EC8B/AHM7EC8B；Integrator IIR/IIR2/IIR3/FIR/FIR2/FIR-bl/FIR-bw/CIC；SDM→SDM wide/narrow/XFi；DirectSDM 直通。
- **抖动/整形**（4.4）：NS1/NS4/NS5/NS9/LNS15＋RPDF/TPDF/Gauss1/shaped；DAC bits、20kHz 滤波、自适应增益、限幅。速率 PCM 32–1536 kHz、SDM 至 DSD1024/48（45.2 MHz）。
- **卷积**：overlap-add/overlap-save、单声道 WAV IR、352.8 kHz IR 建议、Expand HF。**Matrix**：≤128 虚拟通道、Source/Mix/Gain、WAV 卷积＋RoomEqWizard EQ＋插件 delay/iir/riaa、IIR→FIR、crossfeed、Loudness、DAC Correction、profile 热切换。
- **接口/生态**：Client–Server（HQPlayer6Client.exe、命令行 hqp6-control.exe 完整参数表）；本机可见 libmicrohttpd.dll（Web 界面）、大量 UPnP 字符串；流媒体手册仅 **Qobuz 与 HRA(highresaudio.com)**，无 Tidal（Tidal 走 Roon 输入）；Roon＝Add HQPlayer+IP；DSDIFF/DSF 原生与 DoP。

## 2. 开源替代（可点击）
- 重采样：[libsoxr](https://github.com/chirlu/soxr)(LGPL)、[r8brain-free-src](https://github.com/avaneev/r8brain-free-src)(MIT，自带 PFFFT 分块卷积)、[sox_ng](https://codeberg.org/sox_ng/sox_ng)。
- DSD 调制器：[SoX sigma-delta 提交](http://yhbt.net/sox.git/log/msvc10?h=pu&id=818bdd0ccc1e5b6cae742c740c17fd)（`dither -p 1`＋shibata/lipshitz/gesemann 整形表）、[python-deltasigma](https://python-deltasigma.readthedocs.io/)（设计/仿真 5、7 阶 NTF）、[sox 手册](https://manpages.ubuntu.com/manpages/resolute/man1/sox.1.html)。
- DSD→PCM：[FFmpeg dsf/dff/DST 解码](https://trac.ffmpeg.org/ticket/8770)＋自研 CIC/半带 FIR；[sacd-ripper](https://github.com/setmind/sacd-ripper)。
- 分块卷积：[VkFFT](https://github.com/DTolm/VkFFT)(MIT，Vulkan/HIP/OpenCL/CUDA)、[AOCL-FFTW](https://www.amd.com/en/developer/aocl/fftw.html)、PFFFT/KissFFT。
- EQ/路由/控制：自研 RBJ biquad＋[AutoEq](https://github.com/jaakkopasanen/AutoEq)；libmicrohttpd＋UPnP，参考 [HQP 适配器](https://github.com/open-horizon-labs/unified-hifi-control/blob/main/src/adapters/hqplayer.rs)。

## 3. 算力现实性（9800X3D＋RX 9070 XT）
【事实】HQPlayer GPU offload **仅 CUDA**（二进制含 sm_75…sm_121＋cufft64_12.dll，无 HIP/OpenCL/Vulkan），AMD 卡在官方路径无用。AMD 官方矩阵确认 [RX 9070 XT(RDNA4/gfx1201) Windows HIP SDK 支持](https://rocm.docs.amd.com/projects/install-on-windows/en/latest/reference/system-requirements.html)。
【推测，需实测】7 阶 1-bit 调制器 DSD1024 立体声≈90 M sample/s 串行，约 2–4 GFLOP/s，**逐样点串行、多核无效**，吃单核 IPC/L3（X3D 有利）；DSD512 裕量约 2×。1M taps 卷积在 352.8 kHz 域≈0.7 TMAC/s，CPU 不可实时，必须 GPU；GPU 理论 FP32≈97 TFLOPS（≈48 TMAC/s），折损后取 10–20%。结论：**DSD512＋7 阶 EC 可行；DSD1024/48＋super 档大概率 xrun，改用 512+fs 轻量档**。

## 4. AMD GPU(Win) 路径与推荐
Vulkan Compute＋VkFFT ★★★★★（首选：跨厂商、零 SDK 依赖、FFT 现成）＞ROCm/HIP SDK ★★★★（支持 gfx1201，与 CUDA 同构，kernel 可双后端）＞OpenCL ★★（兜底）＞DirectML ★（维护模式，不选）。分工：卷积/EQ/FFT→GPU；调制器/抽取→自研 AVX-512 CPU 内核＋大页＋无锁环形缓冲；控制面 REST/WebSocket＋libmicrohttpd。

## 5. 风险与工作量
高风险：算法/协议未公开且 EULA 禁反向工程，只能等效复刻、音色无法 bit 一致→自建 THD+N/带外噪声/脉冲响应验收。工作量：调制器 AVX-512 内核 3–6 人月（最难）；重采样＋卷积＋Matrix 3–5 人月（GPU 同步为主坑）；生态 ASIO/WASAPI/NAA、UPnP、Roon、Qobuz/HRA 授权 2–3 人月（法务先于工程）。里程碑：①CPU DSD256/512＋gauss 族＋ASDM7EC-light＋CPU 卷积→②VkFFT 卷积→③Matrix→④DSD1024＋控制/Client/Web。