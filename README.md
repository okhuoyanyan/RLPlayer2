# RLPlayer 3.3.17

> **免费、开源（GPL-3.0）、面向 Windows 的本地高保真播放器。**
> 新界面「数字音乐台」把 PCM 滤波、DSD 调制、输出规格、EQ、房间校正和同曲片段对比放进同一页；
> 每一个滤波器都能看到全频带证书，每一次输出都能追溯到实际参数。

![平台](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D6)
![协议](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)
![版本](https://img.shields.io/badge/version-3.3.17-green)

---

## 3.3.17 更新重点

- **全新数字音乐台**：A–D 四组独立调音，组内保存歌曲、位置、滤波、DSD、EQ、音量与输出设备；切组即切整套声音。
- **滤波器详情全频带证书**：65,536 点 FFT（32,769 个频点）直接计算阻带/通带，不再只给 512 点插值曲线；面板显示「全频带证书」。
- **PCM → 原生 DSD**：PCM 源经 SDM 调制后剥掉 DoP 标记，按 ASIO 2.3 原生 1-bit 直送 DAC；xDuoo USB Audio Device 上 DSD64–DSD1024 实机通过。
- **1.536 MHz PCM**：ZiSan UAC2 / xDuoo 等高码率设备实测 12 档 PCM；1.536M 用 pcm_dump 取证，送给 ASIO 驱动的 2 秒 24-bit 样本与源 WAV 逐字节相同。
- **CD 音轨**：.cda 拖入、播放、暂停/继续、本地文件与 CD 来回切换已修；MCI 直通不经过 DSP（音量/EQ/升频对 CD 无效）。
- **播放台 EQ 收口**：原来的「耳机 EQ / 自定义 EQ」两栏合并为一个「加载 EQ」，列出 16 个已保存槽位；选「不载入（关闭均衡）」会真正关闭并清空当前组关联，频段值保留。
- **设置-输出移除「切到 bit-perfect 直通」按钮**：A–D 的后端/滤波/EQ/音量已全部由数字音乐台统一管理，第二套入口会改坏当前组。
- **稳定性**：DSD64 原生直通全曲 383.99 秒 ur=0；10 个 6:24 曲目连续 1 小时 ur=0；0 dBFS 4:22 长曲 DSD512/DSD1024 全曲通过。

---

## 这是什么

一个装在 Windows 上的本地音乐播放器。它不联网听歌，也不做社交，只做一件事：

**把你已经收藏在硬盘里的音乐，用尽可能高的保真度放出来，并把「为什么这样放」摊开给你看。**

适合：有大量 FLAC / DSD / APE，在意升频算法、抖动整形、房间驻波、耳机频响的人。
不适合：只用手机听流媒体、只想要一个「能响」的播放器的人。

---

## 新界面：数字音乐台

专业模式下的主工作区。四个配置组 **A–D** 各自保存一整套声音：

| 模块 | 内容 |
|---|---|
| **PCM 滤波** | 1x / Nx 独立选择；79 个命名预设；相位、抽头、阻带衰减、过渡带可高级覆盖 |
| **SDM / DSD** | DSD64–DSD1024；EC / DSD / AHM 调制族；DoP 或 ASIO 原生 1-bit；1x/Nx 滤波 |
| **输出规格** | PCM 位深、采样率族、DSD 倍率、输出声道；每组可独立指定后端与设备 |
| **EQ 调音台** | 20 段参数 EQ；16 个保存槽位；OPRA 耳机校准库 6,230 款；保存后才能被播放台加载 |
| **同曲片段** | 波形拖选、整段平移、循环；播放中拖动跟手试听 |
| **实际信号路径** | 源 → 处理 → 设备逐节点显示；bit-perfect 结论按本次真实参数给出 |

### 滤波器详情

点击滤波器下拉，右侧浮出与列表同高的详情：

- 通带边缘、阻带起点、过渡带宽、设计衰减、实测阻带、通带最大偏差；
- 原型抽头、群延迟、峰前/峰后振铃；
- 幅频（含镜像）、通带放大、群延迟、冲激四张图；
- **全频带证书**：65,536 点 FFT / 32,769 个频点。

这仍然是「该 FIR 的 DTFT 在 32,769 个频点上的精确采样」，不是数学连续统；但 65,536 点 FFT 远大于最大 16,380 抽头，阻带里不可能夹着比 bin 间隔更窄的尖峰而漏测。

---

## 格式与输出

- **PCM**：WAV / FLAC / ALAC / APE / WV / MP3 / AAC / OGG / OPUS / WMA / AIFF / CAF / MKA
- **DSD**：DSF / DFF；DSD64–DSD1024
- **CD**：.cda 音轨（MCI 直通；见下方限制）
- **镜像**：SACD ISO / DVD-Audio ISO 内部导航；DST 压缩不支持
- **ASIO 原生 DSD**：ASIOSTDSDInt8MSB1 / LSB1，类型 33；xDuoo 实机 DSD64–1024 全部通过
- **PCM 高码率**：44.1k–1.536M；DSD512 DoP 载波 1.4112M
- **GPU**：AMD HIP（25 架构 fat binary，内置运行时）+ NVIDIA CUDA 12.9（19 架构）；无匹配显卡自动回退 CPU

---

## 下载与安装

### 系统要求

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 1809 (x64) 或更新 / Windows 11 |
| 内存 | 4 GB（大 DSD 库建议 8 GB+） |
| 硬盘 | 约 400 MB（含内置运行时） |
| 显卡 | 集显即可；有 AMD/NVIDIA 独显可开启 GPU 加速 |

### 安装

**解压即用，不需要安装任何运行库。**

1. 下载 RLPlayer-3.3.17-win64.zip
2. 解压到任意目录（**不要放进 Program Files**，那里写入需要管理员权限）
3. 双击 RLPlayer.exe
4. 首次启动选择音乐文件夹，等扫描完成

包内已内置：FFmpeg、AMD HIP 运行时、NVIDIA CUDA 运行时、WebView2 固定版、全部命令行工具。
**不需要另外装 .NET、VC++ 运行库或 WebView2。**

### 卸载

直接删掉整个目录。设置、曲库索引、封面缓存、房间校正集都在程序目录内，不写注册表、不往 AppData 塞东西。

---

## 下载渠道

| 渠道 | 地址 |
|---|---|
| GitHub Releases（国外/手动全量） | https://github.com/okhuoyanyan/RLPlayer2/releases |
| 官方更新服务器（增量） | https://rlplayer.rainlain.com |
| 百度网盘（国内手动） | 由管理员在发布页更新（提取码见更新清单） |

程序内「设置 → 关于与更新」会走 rlplayer.rainlain.com 的签名增量清单；服务器只提供增量对象，不代理完整包。
国内网络打不开 GitHub 时，用百度网盘完整包；更新清单里的 cn_url 由管理员维护。

---

## 关于数据与隐私

**RLPlayer 不收集任何遥测。**

- 曲库扫描、播放、分析全部在本地完成；
- 联网只发生在你主动触发的场景：补全标签/封面/歌词、AI 整理资料、检查更新、提交反馈；
- 所有联网请求都可以在设置里逐个关掉；
- 提交反馈时诊断包先脱敏（token / 密码 / 路径中的用户名 → ***），不会上传音乐文件；
- 没有账号系统，不需要注册。

---

## 已知限制（3.3.17）

我们把这些写在最显眼的地方，而不是藏进 FAQ：

1. **CD 音轨通过 Windows MCI 播放，不经过 RLPlayer 的 DSP**：音量/EQ/升频/房间校正对 CD 全部无效；这是为了避免 SPTI 抓轨导致 USB 光驱掉线。
2. **CD 歌曲名**：本机 TEAC 光驱 + 测试碟没有 CD-Text（IOCTL_CDROM_READ_TOC_EX 返回 ERROR_NOT_SUPPORTED），当前回退为 Track 01；带 CD-Text 的碟/光驱还需要接 CDTEXT pack 解析，当前没有真实样本，不能声称已支持真实歌名。
3. **DSD→DSD 纯 DSD 域异速率转换尚未实现**：DSD64→DSD128 仍解成 PCM 再调制。
4. **SACD DST 压缩不支持**：只识别并提示转 DSF/DFF，没有 DST 算术解码器。
5. **D3D12 / oneAPI 通用 GPU 后端尚未实现**：Intel Arc / 核显仍只能 CPU；AMD 走 HIP，NVIDIA 走 CUDA。
6. **实时多电平 AHM 未开放**：实时 DSD 仍强制 1-bit；多电平只在离线工具格式里。
7. **0 dB 满幅长曲**：4:22 的 DSD512/DSD1024 全曲已通过；0 dB 连续 1 小时未测。
8. **1.536M 逐位证明的边界**：已证明「送给 ASIO 驱动的字节与源 WAV 相同」；DAC 内部处理没有数字回环，无法从应用侧观测。
9. **NVIDIA 实卡**：本机无 NVIDIA 显卡，CUDA 只做 19 架构离线制品校验；运行时行为需有 N 卡的用户复现。
10. **版本对比对音乐素材上的少量谐波失真不敏感**：合成测试信号正常，音乐素材有盲区，已立项修复。

---

## 反馈

| 渠道 | 地址 |
|---|---|
| 粉丝和反馈 Q 群 | **895816505** |
| 问题跟踪 | https://github.com/okhuoyanyan/RLPlayer-feedback/issues |
| 官方主页 | https://rlplayer.rainlain.com |

程序内也有一键反馈：**设置 → 关于与更新 → 意见反馈**，会自动附带脱敏后的诊断信息。

---

## 开发与构建

源码：https://github.com/okhuoyanyan/RLPlayer2

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

- C++20 / CMake / Ninja；Windows 10/11 x64。
- 离线门禁：tools/run_safe_ctest.ps1；需要完整 FFmpeg 与 Git OpenSSL。
- 发行包：tools/make_dist.ps1；GPU 制品用 tools/build_gpu.ps1（HIP）与 tools/build_gpu_cuda.ps1（CUDA）重建。
- 测试与证据：docs/、tests/；每个版本的门禁数字与残余项都写在交付摘要里。

---

## 开发与致谢

| 角色 | 成员 |
|---|---|
| 项目开发者 | **RainLain**、**猫与小行星** |
| 协助开发者 | 聆听风暴 |
| 媒体与美术 | Gosed |
| 测试鸣谢 | Miyu、正在下载99% |

以及所有提交过 issue、帮忙试听、报告过问题的朋友。

---

## 协议

**GNU General Public License v3.0 or later**（GPL-3.0-or-later），见 [LICENSE](LICENSE)。

随包分发的第三方组件（FFmpeg / AMD HIP / NVIDIA CUDA / WebView2 / OPRA）各自遵循其原始许可，见 [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)。

AMD HIP 与 NVIDIA CUDA 为厂商专有二进制，按「可分离第三方组件」处理，不在本项目的 GPL 授权范围内。

---

*RLPlayer 是一个独立的开源项目，与任何商业播放器厂商无关。*
