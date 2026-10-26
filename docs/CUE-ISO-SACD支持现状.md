# CUE / ISO / SACD 支持现状核查报告（2026-10-25 初稿；**2026-10-27 更新**）

> 依据：对 `src/audio/**`、`src/net/library.h`、`src/meta/cue.h`、`tools/rtaweb.cpp` 的代码核查 + 实测。
>
> **2026-10-27（用户 261027-01 #3）本轮改动**：
> ① **CUE 早已实现，本报告原来的「完全不支持」已过时** —— 对照表与二.1 已按实测改写；
> ② **ISO 从「连识别都没有」变成「识别 + 入库 + 明确报错，但不假装能播」**（白名单、扫描、播放拦截三处）；
> ③ **DST 的报错从「ffmpeg 解码失败 … Invalid data found」改成带格式名与下一步指引的人话**，
>    并且 `read_any` 不再把「格式不支持」误当成「读不了」而回退 ffmpeg。
> DST 解码器与完整的 SACD 解析**仍未做**，原因见二.2 / 二.4。

---

## 一、核心问题对照表

| 序号 | 检查项 | 当前状态 | 代码位置 | 缺失组件 / 瓶颈说明 |
|---|---|---|---|---|
| **1** | **.cue 能否解析并分轨？** | ✅ **已支持**（2026-10-25 实现；本行原「不支持」结论作废） | `src/meta/cue.h`<br>`src/net/library.h`（`lib_cue_expand` / `lib_cue_range_sec`） | 已落地：CUE 语法解析（GBK/UTF-8/BOM 自动识别）、整轨 N 条虚拟曲目入库（索引 v4 起末尾 4 列 `cue/cueN/cueOff/cueLen`）、播放区间 `/api/play?cue_start=&cue_end=`。判据：`tests/test_cue.cpp` + `tests/test_library.cpp` [5] 节。**仍未做**：实体图未按 cue 区分归并 |
| **2** | **.iso (DVD-Audio / SACD-ISO) 能否识别？** | ⚠️ **识别但不播放**（2026-10-27 新增） | `src/net/library.h`（`lib_probe_iso` / `LibTrack::needsConvert`）<br>`src/audio/audio_loader.h`（`iso_inspect` / `throw_for_iso`） | 已落地：`.iso` 进白名单并入库（只读 ISO9660 主卷描述符 + 根目录，**不跑 ffprobe**），判「SACD 镜像 / 普通数据盘 / 无法识别」，播放时返回带指引的明确错误。**未落地**：ScarletBook（Master TOC / Area TOC）解析与分轨、DVD-Audio（AUDIO_TS/IFO/MLP）解码、ISO 挂载 |
| **3** | **.dsf / .dff 的多声道 / DSD128+ 是否支持？** | ⚠️ **部分支持** | `src/audio/dsd_reader.h:180-213, 345`<br>`src/audio/audio_loader.h:436, 513, 728`<br>`src/audio/asio_out.h:850-895` | ① **读取层已完整支持**：原生解包代码显式支持 1~8 声道，采样率无上限，DSD128/256/512 均可正确读取并解析；<br>② **抽取降采样层有约束**：PCM 抽取硬编码以 705.6 kHz 为上限（DSD512 抽取比受限）；<br>③ **输出驱动层受限**：ASIO Native DSD 与 DoP 目前仅打通双声道立体声；多声道 DSD 原生透传尚未适配声道掩码映射与多声道 ASIO 缓冲。 |
| **4** | **SACD 的 DST 压缩是否支持？** | ❌ **不支持**<br>*(代码显式拒绝；2026-10-27 只改了报错文案)* | `src/audio/dsd_reader.h`（`read_dsf` / `read_dff` / `probe_dsd`）<br>`src/audio/audio_loader.h`（流式布局探测 + `read_any`） | ① 遇到 DST / DSTI 块、`formatId != 0`、`CMPR == "DST "` 时抛 `rta::audio::UnsupportedFormat`，文案带格式名与下一步（转 .dsf/.dff）；<br>② `read_any` 对 `UnsupportedFormat` **不再回退 ffmpeg**（回退只会把 `Invalid data found` 当结论）；<br>③ 仍缺 DST 算术解码器（ISO/IEC 14496-5 自适应 FIR + 算术编码）与帧解包 |

---

## 二、逐项代码级深度分析

### 1. CUE 解析与分轨（**2026-10-25 已实现；本节原「完全缺位」结论作废**）

#### 已实现的部分（代码位置 + 判据）
- **解析**：`src/meta/cue.h` 的 `cue_parse_file()`（`CueSheet` 结构；`FILE` / `TRACK` / `INDEX 00/01` / `TITLE` / `PERFORMER` / `REM`；GBK / UTF-8 / UTF-8-BOM / ANSI 自动识别；`mm:ss:ff` → 1/75 帧 → 采样帧）。
- **入库**：`src/net/library.h` 的 `lib_cue_expand()`（纯函数：探测表 → N 条虚拟曲目，可逐字段断言）。认领条件四条，缺一即**退回整轨**（不丢歌）：能解析出 ≥1 条 AUDIO+INDEX 轨 / `FILE` 指向的文件真实存在 / 未被别的 cue 先认领 / 探测成功。
- **索引**：v4 起末尾追加 `cue / cueN / cueOff / cueLen` 四列（**前 16 列逐字不变**，老索引不会串列）；`cueOff` 单位是采样帧。
- **播放**：`lib_cue_range_sec()` 给出本曲在整轨里的起止秒，`/api/play?cue_start=&cue_end=` 消费它（`tools/rtaweb.cpp`，单一口径，播放侧不重算）。
- **判据**：`tests/test_cue.cpp`（139 条逐字段断言）+ `tests/test_library.cpp` [5] 节（真实扫描 / 删 cue 后增量回归 / 大小写不一致 / 坏 cue 退回整轨）。

#### 仍未做（如实记录，铁律 4）
- 实体图（`entity_graph_from_library_tracks`）按 path 建种子，同一整轨的 N 首 path 相同 ⟹ 去重/归并**尚未**按 cue 区分；
- 同一 cue 里跨多个 `FILE` 的曲目顺序 = `FILE` 出现顺序（不是 cue 的全局顺序）。

---

### 2. ISO 镜像（DVD-Audio / SACD-ISO）（**2026-10-27：识别已落地，播放仍不支持**）

#### 2026-10-27 已实现（本轮改动，全部有实测）
1. **库能看见 `.iso`**：`library.h` 的 `lib_is_audio` 白名单加 `.iso`；`LibTrack::needsConvert()` 由扩展名**现算**（不落盘）⟹ 旧 v4/v5 索引读回来也对，**不必升 `#libIndexV`** 让用户整份重扫（升版本的代价见 `load()` 注释）。
2. **入库但不当可播音轨**：扫描时 `.iso` 走 `lib_probe_iso()` —— 只读 ISO9660 结构（16 号扇区主卷描述符 + 根目录一扇区），**不进 ffprobe**（随包 ffmpeg 9.0 没有 iso9660/udf/sacd demuxer，跑一次只会失败并拖慢扫描）；`sr/bits/ch/dur` 一律 0，不给「看起来有效」的值。
3. **分得清 SACD 与数据盘**：`iso_inspect()` 判据 = 卷描述符标识 `CD001` + 能找到主卷描述符 + 根目录（或卷标）里有 `SACD` 字样。`Library::isoKindOf()` → `sacd` / `data` / `unknown`。SACD ISO 有 ISO9660 层，所以对 `sacd_extract` 产出的镜像成立；真 UDF-only 镜像落到 `unknown`（宁可说不知道，也不给错指引）。
4. **播放给的是指引，不是 `Invalid data`**：`read_any` 在**最前面**按扩展名拦 `.iso` → `throw_for_iso()`：
   - SACD 镜像：「SACD ISO 镜像暂不支持直接播放。请先用 sacd_extract 转成分轨 .dsf 后入库。」
   - 普通数据盘：「这不是音频镜像：这个 .iso 里没有 SACD 目录，看起来是普通数据光盘镜像。…」
   - 无法识别（没有 ISO9660）：「这个 .iso 里没有 ISO9660 文件系统，无法判断是不是 SACD 镜像（…）。」
5. 扫描收尾消息如实报「N 个 ISO 镜像需转换（SACD 请用 sacd_extract 转分轨 .dsf）」，避免用户以为扫进来就能听。

#### 仍未做
- **SACD-ISO 解析与分轨**：ScarletBook 的 Master TOC / Area TOC（2ch 与 mch 两个区）、音轨区扇区提取、DST 帧解压 —— 与二.4 是同一笔账，合计约 **4~5 天**。
- **DVD-Audio ISO**：`AUDIO_TS` / `AUDIO_TS.IFO` 导航 + AOB(MLP/LPCM) 解码，约 **3~4 天**（ffmpeg 能解 AOB/MLP，但需要我们先解析 IFO 拿扇区区间）。
- **不挂载、不内置虚拟光驱**：本轮只读镜像头，不引入任何驱动或第三方库。

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

### 4. SACD DST 压缩格式支持（现状：**仍明确拒绝**，2026-10-27 只改了「怎么说」）

#### 2026-10-27 的改动：不支持 → 人话（不再回退 ffmpeg）
- 新增 `rta::audio::UnsupportedFormat`（`src/audio/dsd_reader.h`）：**「本播放器不做这个格式」与「文件坏了/读不了」是两种事**。
  以前 README 式的回退策略（`read_any` 对 DSD 原生读取失败一律回退 ffmpeg，ROADMAP P0）会把 DST 压缩
  当成「读不了」再喂 ffmpeg，ffmpeg 报 `Invalid data found when processing input` —— 用户看到这句只会以为文件坏了。
  现在：`read_any` 对 `UnsupportedFormat` **原样上抛**，绝不回退。
- 统一文案（`msg_dst_unsupported()`）：「DST 压缩的 DSD 暂不支持（这是 SACD 的无损压缩格式）。请先用 sacd_extract / foobar2000 等工具转成 .dsf 或 .dff 再播放。（位置：…）」
- 抛错点（全部改成本类型 + 这条文案）：`dsd_reader.h` 的 `read_dsf`（`fmtId != 0`）、`read_dff`（`DST `/`DSTI` 块、`CMPR=DST`）、`probe_dsd`；`audio_loader.h` 的流式布局探测（同样的四处）。
- ffmpeg 兜底也补了话：`read_any` 收到含 `Invalid data` 的异常时，改写成「容器/编码不可识别，不等于文件损坏 + 常见原因 + 下一步」。

#### 仍未做（这才是真正的工作量）
- **DST 算术解码器**：Direct Stream Transfer（ISO/IEC 14496-5 自适应 FIR 预测最高 16 阶 + 算术编码），按帧（每帧约 1/75 秒 DSD）解码；可移植 `libdstdec`。
- **帧解包与流式对接**：在 `read_dff` / `read_dsf` 检出 `DST `/`DSTI` 块后逐帧解，接进 `DsdPcmStream` 的分块读取路径。
- **随包 ffmpeg 帮不上**：ffmpeg 9.0 有 `dst` **解码器**，但没有 `sacd`/`iso9660`/`udf` **demuxer**，也不把 `.dff` 里的 DST 块交给它 ⟹ 只能自己写。
- **预估工作量**：**3~4 天**（移植精简 dstdec + 内存流式对接 + 多声道逐帧 + 门禁测试集）。

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

1. **P0：CUE 解析与虚拟分轨 —— ✅ 已于 2026-10-25 完成**（判据见二.1）。
   - 当时的理由：国内发烧友下载的无损专辑（整轨 FLAC/APE/WAV + CUE）占比极高，「整张碟只能从头播到尾」是硬卡点。
   - **2026-10-27 补的一笔**：CUE 已可用，但 ISO/DST 这两条进不来（见二.2 / 二.4）；本轮先把它们变成「说得清的不支持」，而不是假的解码失败。

2. **P1（高优先级）：DST 无损压缩解压**
   - **理由**：许多 SACD 转录的 DFF/DSF 文件（尤其是多声道 SACD 抓轨文件）为了节约体积采用了 DST 压缩。用户加载时直接弹报错窗口，体验极差。
   - **周期**：3.5 天。

3. **P2（中优先级）：SACD-ISO 原生识别与解包**
   - **理由**：SACD-ISO 占大量高端发烧友曲库资源，目前必须借助外部工具转换为 DSF 才能播放。增加原生 ISO 识别将极大提升 RLPlayer 的专业定位。
   - **周期**：4~5 天。

4. **P3（细节完善）：多声道 DSD 原生透传与 48k DSD 基频族**
   - **理由**：多声道 DSD 源码直出受限于多声道 DAC/AV 功放硬件，属于发烧小众场景；读取层现已支持多声道转 PCM 播放。
   - **周期**：1.5 天。
