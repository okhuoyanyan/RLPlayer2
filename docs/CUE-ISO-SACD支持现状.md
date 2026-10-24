# CUE / ISO / SACD 支持现状核查报告（2026-10-25）

> 依据：对 `src/audio/**`、`src/engine/**`、`src/net/library.h`、`tools/rtatag.cpp` 的全量代码核查。
> 目的：回答关于 .cue 分轨、.iso（DVD-Audio/SACD-ISO）识别、.dsf/.dff 多声道与高规格 DSD、以及 SACD DST 压缩支持情况。

---

## 一、核心问题对照表

| 序号 | 检查项 | 当前状态 | 代码位置 | 缺失组件 / 瓶颈说明 |
|---|---|---|---|---|
| **1** | **.cue 能否解析并分轨？** | ❌ **不支持** | `src/net/library.h:234-242`<br>`src/audio/audio_loader.h:1097-1150` | ① `library.h` 白名单扩展名未包含 `.cue`；<br>② 缺乏 CUE 语法解析器（FILE / TRACK / INDEX 00/01 / TITLE / PERFORMER）；<br>③ 曲库扫描未建立整轨音频的“虚拟分轨”映射逻辑；<br>④ 播放引擎缺乏基于样本区间（startOffset, duration）的流式 Seek/分段解码。 |
| **2** | **.iso (DVD-Audio / SACD-ISO) 能否识别？** | ❌ **不支持** | `src/net/library.h:234-242`<br>`src/audio/audio_loader.h:1097-1150` | ① `library.h` 过滤逻辑直接忽略 `.iso` 文件；<br>② 缺乏 SACD-ISO (ScarletBook UDF 镜像、Master TOC、Area TOC) 解析器；<br>③ 缺乏 DVD-Audio (DVD UDF 格式、AUDIO_TS/AOB 结构、MLP 解码器)；<br>④ 系统内没有镜像挂载或 ISO 轨数据流解包抽取基础设施。 |
| **3** | **.dsf / .dff 的多声道 / DSD128+ 是否支持？** | ⚠️ **部分支持** | `src/audio/dsd_reader.h:180-213, 345`<br>`src/audio/audio_loader.h:436, 513, 728`<br>`src/audio/asio_out.h:850-895` | ① **读取层已完整支持**：原生解包代码显式支持 1~8 声道，采样率无上限，DSD128/256/512 均可正确读取并解析；<br>② **抽取降采样层有约束**：PCM 抽取硬编码以 705.6 kHz 为上限（DSD512 抽取比受限）；<br>③ **输出驱动层受限**：ASIO Native DSD 与 DoP 目前仅打通双声道立体声；多声道 DSD 原生透传尚未适配声道掩码映射与多声道 ASIO 缓冲。 |
| **4** | **SACD 的 DST 压缩是否支持？** | ❌ **不支持**<br>*(代码显式拒绝)* | `src/audio/dsd_reader.h:202-203, 327-329, 413-415`<br>`src/audio/audio_loader.h:433, 501-503` | ① 代码中遇到 DST / DSTI 块或 formatId != 0 时直接抛出 `std::runtime_error("暂不支持 DST 压缩")`；<br>② 缺少 DST (Direct Stream Transfer，基于 ISO/IEC 14496-5 算术编码与自适应 FIR 预测) 算术解码器；<br>③ 缺少 DST 帧解包与实时解压环形缓冲区。 |

---

## 二、逐项代码级深度分析

### 1. CUE 解析与分轨（现状：完全缺位）

#### 代码证据
- `src/net/library.h:236-240`：
  ```cpp
  static const char* kExt[] = { ".wav", ".wave", ".flac", ".m4a", ".alac", ".mp3", ".aac",
                                ".ogg", ".opus", ".wma", ".ape", ".wv", ".dsf", ".dff",
                                ".aif", ".aiff", ".aifc", ".caf", ".mka", ".mpc" };
  ```
  支持的音频扩展名中完全没有 `.cue`。扫描器进入目录时，`.cue` 文件被直接判定为非音频并忽略。
- 全仓搜索：整个 `src/` 与 `tools/` 下无任何 `INDEX 01`、`TRACK` 或 CUE 字段解析逻辑。

#### 落地关键路径与工作量
1. **CUE 解析器模块**（`src/audio/cue_parser.h`）：
   - 支持 GBK / UTF-8 / UTF-8-BOM / ANSI 多种编码自动识别；
   - 提取 `FILE "filename" TYPE`、`TRACK nn AUDIO`、`TITLE`、`PERFORMER`、`INDEX 01 mm:ss:ff`（1 秒 = 75 帧）；
   - 换算为采样点偏移（$samples = (mm 	imes 60 + ss + ff / 75.0) 	imes sampleRate$）。
2. **曲库虚拟分轨入库**（`src/net/library.h`）：
   - 扫描到 `.cue` 时，若对应音频文件存在，将各 TRACK 作为独立虚拟 `LibTrack` 条目写入索引，路径格式如 `album.flac#cue:track01` 或在 `LibTrack` 中增加 `cueOffset` 与 `cueDuration`。
3. **播放 Seek 与分段解码**（`src/audio/audio_loader.h`）：
   - 播放时按起始偏移流式跳过前导帧，达到持续时间后触发 EOF。
- **预估工作量**：**2.5 天**（解析器 1 天，曲库虚拟轨 1 天，播放截取与测试 0.5 天）。

---

### 2. ISO 镜像（DVD-Audio / SACD-ISO）支持（现状：完全缺位）

#### 代码证据
- `src/net/library.h:236`：不识别 `.iso`。
- `src/audio/audio_loader.h:1097-1150`：`read_any` 统一分流仅包含 WAV、DSF/DFF 与 ffmpeg。ffmpeg 官方原生构建不具备 SACD ISO 解包器（ScarletBook demuxer），因此无法通过命令行透传解码。

#### 落地关键路径与工作量
1. **SACD-ISO**：
   - SACD-ISO 遵循 ScarletBook 规范，底层为 UDF 卷结构，内部包含 Master TOC、Area TOC（两声道区与多声道区）。
   - 业界成熟方案通常引入开源的 `libsacdread` / `sacd_extract` 核心逻辑或利用虚拟驱动器挂载；若在原生 C++ 中内嵌轻量 UDF / ScarletBook TOC 解析器，可将每个曲目映射为虚拟流。
2. **DVD-Audio ISO**：
   - 结构为 DVD-Video/Audio，音频存放在 `AUDIO_TS` 目录下的 `.AOB` 文件中，通常采用 MLP (Meridian Lossless Packing) 或 LPCM 编码。ffmpeg 可解压 AOB/MLP，但需解析 `AUDIO_TS.IFO` 导航信息以获取各乐轨的扇区起止位置。
- **预估工作量**：
  - SACD-ISO 原生解析提取：**4~5 天**（TOC 解析 + 扇区提取 + 虚拟分轨对接）；
  - DVD-Audio IFO 导航解析：**3~4 天**。

---

### 3. DSF / DFF 多声道与 DSD128+ 支持（现状：读取已成，输出受限）

#### 代码证据
- **读取层完美支持多声道与高采样率**：
  - `src/audio/dsd_reader.h:210`：`if (chNum < 1 || chNum > 8) throw ...`（DSF 显式支持 1~8 声道）；
  - `src/audio/dsd_reader.h:180-194`：内置 1~6 声道的标准声道标识映射表（`SLFT`, `SRGT`, `C`, `LFE`, `LS`, `RS`），超过 6 声道自动生成 `CHn`；
  - `src/audio/dsd_reader.h:212`：`if (rate < 44100) throw ...`，对采样率上界无硬性限制，DSD64 (2.82M)、DSD128 (5.64M)、DSD256 (11.28M)、DSD512 (22.58M) 均可原生读出；
  - `src/audio/dsd_reader.h:345`：DFF 同样支持 1~8 声道与任意 FS。
- **PCM 抽取层**：
  - `src/audio/audio_loader.h:728`：`const uint32_t outRate = std::min<uint32_t>(s.rate / 16u, 705600u);`
  - 对于 DSD128 (5.6448 MHz)，抽取至 352.8 kHz；DSD256 (11.2896 MHz) 抽取至 705.6 kHz；DSD512 (22.5792 MHz) 被限制在 705.6 kHz（降采样比变为 32 倍），多声道平面数组 `planes[(size_t)c]` 均正常抽取。
- **输出端瓶颈**：
  - **ASIO Native DSD**：`src/audio/asio_out.h:850-895` 目前优先按照双声道（ch=2）的交错与 bufferSize 排布，多声道 Native DSD 尚未建立 5.1/7.1 的声道掩码与 ASIO 多声道 bufferMapping 联动；
  - **DoP 输出**：`src/dsp/dop.h` 仅实现立体声 DoP 封装；且 DSD128 DoP 需要声卡支持 352.8 kHz PCM，DSD256 需要 705.6 kHz，很多 USB 界面硬件不支持超过 384k 的 DoP。

#### 落地关键路径与工作量
- 多声道 DSD 转 PCM 播放已天然可用；
- 若需支持**多声道 ASIO 原生 DSD 直通**：需扩展 `asio_out.h` 的 `write_channel` 支持多通道循环投递，并在 `rtaweb.cpp` 暴露多声道 DSD 输出选择。**估计 1.5 天**。

---

### 4. SACD DST 压缩格式支持（现状：明确拒绝）

#### 代码证据
- `src/audio/dsd_reader.h:202-203`：
  ```cpp
  if (fmtId != 0)
      throw std::runtime_error("DSF format id = " + std::to_string(fmtId) + "（DST 压缩）暂不支持");
  ```
- `src/audio/dsd_reader.h:327-329`：
  ```cpp
  } else if (std::memcmp(f.data() + off, "DST ", 4) == 0 ||
             std::memcmp(f.data() + off, "DSTI", 4) == 0) {
      throw std::runtime_error("DFF 使用 DST 压缩（DST 块），暂不支持");
  }
  ```
- `src/audio/audio_loader.h:433` 与 `501-503`：流式分块探测也明确对 DST 块抛错拒绝。

#### 落地关键路径与工作量
1. **DST 算法解压器**：
   - DST (Direct Stream Transfer) 是 SACD 使用的无损压缩标准（基于 ISO/IEC 14496-5:2001/Amd.1:2004），采用自适应线性预测滤波（FIR 阶数最高 16 阶）+ 算术编码（Arithmetic Coding）。
   - 算法计算复杂度高，且由于按帧压缩（每帧通常包含 1/75 秒的 DSD 位流，即每声道 37632 位），必须维护算术解码器状态机与反量化预测系数。
   - 可引入成熟开源实现（如经过工业检验的 `libdstdec` 源码）。
2. **DSD Stream 接入**：
   - 在 `read_dff` / `read_dsf` 检测到 `DST ` 块时，逐帧读取帧长，送入 `dst_decode_frame()` 解出原始 1-bit MSB-first 流。
- **预估工作量**：**3~4 天**（移植并精简 `dstdec` 算术解码器、内存流式对接、多声道逐帧解压与测试集门禁验证）。

---

## 三、实施优先级与落地路线建议

根据用户使用频率与开发投资回报率（ROI），建议按以下优先级分批实施：

1. **P0（最高优先级，立刻实施）：CUE 解析与虚拟分轨**
   - **理由**：国内发烧友下载的无损专辑（整轨 FLAC/APE/WAV + CUE）占比极高，用户直接遇到“整张碟只能从头播到尾、无法选曲”的体验卡点。
   - **周期**：2.5 天。

2. **P1（高优先级）：DST 无损压缩解压**
   - **理由**：许多 SACD 转录的 DFF/DSF 文件（尤其是多声道 SACD 抓轨文件）为了节约体积采用了 DST 压缩。用户加载时直接弹报错窗口，体验极差。
   - **周期**：3.5 天。

3. **P2（中优先级）：SACD-ISO 原生识别与解包**
   - **理由**：SACD-ISO 占大量高端发烧友曲库资源，目前必须借助外部工具转换为 DSF 才能播放。增加原生 ISO 识别将极大提升 RLPlayer 的专业定位。
   - **周期**：4~5 天。

4. **P3（细节完善）：多声道 DSD 原生透传与 48k DSD 基频族**
   - **理由**：多声道 DSD 源码直出受限于多声道 DAC/AV 功放硬件，属于发烧小众场景；读取层现已支持多声道转 PCM 播放。
   - **周期**：1.5 天。
