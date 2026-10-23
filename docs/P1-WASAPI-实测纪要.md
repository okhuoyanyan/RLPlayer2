# P1 输出引擎实测纪要（WASAPI 独占 + 事件驱动）

日期：2026-10-18　设备：TASCAM Portacapture X8（默认渲染端点，USB）
代码：[tools/wasapi_probe.cpp](tools/wasapi_probe.cpp)　构建：`build/wasapi_probe.exe`

## 1. 已证实的独占模式正确配方（本机实测，逐条踩坑得出）

| # | 要点 | 实测证据 |
|---|---|---|
| 1 | `hnsPeriodicity` **必须等于** `hnsBufferDuration` | 传 0（含 bufferDuration=0）→ `AUDCLNT_E_BUFFER_SIZE_ERROR` (0x88890016) |
| 2 | `SetEventHandle()` **必须在 `Initialize()` 成功之后** | 提前调用 → `Initialize` 过但 `Start()` 报 `AUDCLNT_E_EVENTHANDLE_NOT_SET` |
| 3 | 开流前**必须预填充整个缓冲**，再 `Start()` | 不预填充 → 首个事件 `pos=0` 无数据可写，驱动立刻停摆 |
| 4 | **每个事件都必须被响应（写入）** | 只要跳过任意一个事件，事件流立即停止，设备只播放已入缓冲的帧数后静音 |
| 5 | `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` = **0x88890019**（不是 0x88890016） | 查证 `Windows Kits\10\Include\10.0.26100.0\um\audioclient.h` |
| 6 | 计时必须用 `QueryPerformanceCounter` | `GetTickCount` 只有 ~15.6 ms 粒度，会把 460 ms 测成 300 ms |

## 2. 端口能力矩阵（实测）

| 端点 | 状态 | 共享混音格式 | 独占可用 |
|---|---|---|---|
| 扬声器 (3- Portacapture X8) | ACTIVE（默认） | 44100 Hz / 2ch / 32bit float | **44100 / 48000 / 96000 Hz @ 24-bit PCM** |
| 3 - M34HWW-PRO (AMD HDMI) | ACTIVE | 48000 Hz / 2ch / 32bit float | 44100 / 48000 Hz @ **16-bit only** |
| 扬声器 (Realtek) | UNPLUGGED | — | — |
| 耳机 (Moondrop DHA15) | NOTPRESENT | — | — |

**重要发现**：X8 独占模式**不支持 16-bit**（16/PCM 全部 `-`），只接受 24-bit PCM。
→ 播放 16-bit 音源必须**零填充到 24-bit**（数值不变，仍属比特完美），这是通用播放器的常规做法。
**不支持 88.2 kHz 家族**（88200/176400/192000 全部不可用）。

## 3. 缓冲时长扫描（全部可开流）

| 缓冲时长 | 44100 Hz | 48000 Hz | 96000 Hz |
|---|---|---|---|
| 100 ms | OK 4410 帧 | OK 4800 帧 | OK 9600 帧 |
| 50 ms | OK 2205 帧 | OK 2400 帧 | OK 4800 帧 |
| 20 ms | OK 882 帧 | OK 960 帧 | OK 1920 帧 |
| 10 ms | OK 441 帧 | OK 480 帧 | OK 960 帧 |
| 3 ms | OK 132 帧 | OK 144 帧 | OK 288 帧 |
| 0（驱动默认） | 错误 | 错误 | 错误 |

设备周期：default = 10.00 ms，min = 3.00 ms。

## 4. ⚠️ 未解决的关键问题（列为 P1 首要待查项）

**现象**：以 10 ms 缓冲、1006 ms 时长推流，实测
- 事件 **115 次**（→ 8.75 ms/事件），设备权威计数 `IAudioClock::GetPosition` = **48096 帧**（100.2 个缓冲周期）
- 每次事件写满 480 帧 → 共写 **55200 帧**，超出实播 **7104 帧（148 ms，+14.8%）**
- 比例 115/100.2 = 1.148，与超额写入比例 14.8% **完全吻合**

**推论（待验证）**：该设备的**真实周期约 418 帧（8.75 ms）**，而 `GetBufferSize()` 返回 480 帧。
若如此，则"每事件写满一个缓冲"会持续超额写入，驱动只能丢弃未播数据 → **会产生可闻的丢样/跳音**。

**为什么还没解决**：
- `IAudioClock::GetPosition()` 在事件之间**不实时推进**（首个事件读到 pos=0），无法直接用作"每事件消耗量"
- 任何"跳过不写"的节流尝试都会让**事件流彻底停止**（要点 4），无法在缺少音频输出的情况下自证

**下一步（需要硬件）**：用**已知信号**（如 1 kHz 满幅正弦 / 计数样本）+ **模拟或 SPDIF 环回采集**做客观比对，才能判定驱动到底有没有丢样。
最终模块设计会把"每事件写入量"做成可配置策略（`full-buffer` / `consumed-delta` / `calibrated-period`），并提供自动校准。

## 5. 对引擎设计的直接结论

1. 输出模块必须实现 **AUDCLNT 错误码到中文诊断**的完整映射（本探针已含权威表）
2. 16-bit 音源 → 零填充 24-bit 容器；**位深转换绝不引入抖动**（纯移位）
3. 采样率切换 = 关闭旧流 + 用新格式重开流（不可复用）
4. 独占失败必须**显式告警**，不得静默回退到共享模式（共享模式会重采样，破坏比特完美）
5. 缓冲时长做成配置项，默认建议 50–100 ms 以留足调度余量

## 附：独占模式能力矩阵与标准探测流程（Round 64）

### 为什么需要这一步

播放器的能力上限由**接收 DAC** 决定：能不能升到 705.6 kHz、能不能跑 DoP，
都取决于设备是否支持对应采样率。跑一次探测就能拿到确定答案，不必猜测。

### 标准流程

```powershell
# 1. 接上听音 DAC（并设为默认，或保持接入状态）
# 2. 运行探针（会在末尾以独占模式接管设备约 1 秒；推流内容为未填充缓冲=静音，非测试音）
D:\RTA-hqplayer\build\wasapi_probe.exe
```

输出包含：共享混音格式、独占开流可行性、缓冲周期范围、
以及一张 **采样率 × 位深** 的独占支持矩阵。

### 本机默认设备实测（TASCAM Portacapture X8，ACTIVE）

```
[共享混音格式] 44100 Hz, 2 ch, 32 bits  IEEE_FLOAT
[独占] 混音格式 -> OK
[周期] default=10.00 ms   min=3.00 ms

rate\fmt 16/PCM  24/PCM  32/PCM  32/FLT
44100    -       YES     -       YES
48000    -       YES     -       YES
88200    -       -       -       -
96000    -       YES     -       YES
176400   -       -       -       -     <- DoP64 承载，不支持
192000   -       -       -       -
352800   -       -       -       -
384000   -       -       -       -
705600   -       -       -       -
```

**结论**：

| 项 | 结果 |
|---|---|
| 支持采样率 | 44.1 / 48 / 96 kHz |
| 支持位深 | 24-bit PCM、32-bit float（**无 16-bit**） |
| 88.2k 家族 | 不支持 |
| DoP | **不可能** —— 最低需 176.4 kHz |
| 独占开流 | 44.1/48/96 × 5 种缓冲（100/50/20/10/3 ms）**全部 OK** |
| 最小周期 | **3 ms** —— 低延迟起播余量充足 |

### 重要提醒

探针扫到 28 个端点，**绝大多数是 NOTPRESENT**（未接入）。
上表说明的是默认设备（X8）的能力，**不等于听音 DAC 的能力**。
接上听音 DAC 后重跑同一命令，才能得到它的矩阵。

### 对播放器配置的直接影响

- 在本机默认设备上，PCM 升频上限是 **96 kHz / 24-bit**
- **不要**用 `--rate 705600` 之类的参数 —— 会以 UNSUPPORTED_FORMAT 失败
- 低延迟起播的 100 ms 默认缓冲在这里有 30 倍余量（最小周期 3 ms）

---

# UltraLite-mk5 独占的三个坑（2026-10-23 实测）

> 用户反馈「WASAPI 独占不可用」。逐层剥到最后，是**三个独立缺陷叠加**。
> 排查工具：`tools/wasapi_ex_probe.cpp`（格式×配方矩阵 + 流测试，写全零静音不出声）。

## 坑 1：独占格式必须是「32 位容器 + 24 位有效」

`IsFormatSupported(EXCLUSIVE)` 的矩阵（MOTU，44100/48000 同结论）：

| 格式 | IsFormatSupported |
|---|---|
| `WAVE_FORMAT_PCM` 16 | `0x88890008` UNSUPPORTED_FORMAT |
| `EXTENSIBLE` 16/16 | `0x88890008` |
| `EXTENSIBLE` 24/24（blockAlign=ch*3） | `0x88890008` ← **我们原来发的** |
| **`EXTENSIBLE` 32 容器 / 24 有效（blockAlign=ch*4）** | **`S_OK`** |
| `EXTENSIBLE` 32/32 int | `0x88890008` |
| `EXTENSIBLE` 32/32 float | `0x88890008` |

**这家驱动只认一种**。位数写在 `wValidBitsPerSample=24`，容器写 `wBitsPerSample=32`，
数据在 32 位里**低 24 位对齐**（±2^23 直接放 int32，不要打包成 3 字节）。

## 坑 2：`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` 的重试必须换新 client

首次 `Initialize` 返回 `0x88890019`（BUFFER_SIZE_NOT_ALIGNED）后，微软要求
**`Release` 旧 `IAudioClient`、重新 `Activate`**，再用 `GetBufferSize` 算出的时长重试；
`periodicity` 也取对齐值（本机 `per=0` 会被拒 `0x88890016`）。

两个连带点（原实现两条都踩了）：
- 复用同一个 client 重试 → `AUDCLNT_E_BUFFER_SIZE_ERROR`（0x88890016）
- `open()` 在 `Activate` 之后**立刻 `dev->Release()`** → 重试时没有设备指针可用，
  这条分支**从来没可能生效**。改为 RAII 持有 `IMMDevice` 到 Initialize 完成。

## 坑 3：首写死锁——`pad == bufferFrames` 时永不写

`write_available()` 用 `toWrite = bufferFrames_ - pad` 算空间。MOTU 在 `Start()` 之后
**会报告 `pad == bufferFrames`**（即使一帧都没写过），于是 `toWrite` 恒为 0 → 永不写 →
4 秒后判 `RENDERFAIL` → 回退共享。现场读数：

```
RENDERFAIL 设备流 4.02 秒未接受数据 … w0=201 wr=0 zp=201 zr=pad=full
            last_pad=1024 缓冲=1024帧 GetBuffer HRESULT=-999
```

`HRESULT=-999` 是初始哨兵值——说明 `GetBuffer` **从未被调用**。
修法：首次写入（`written_ == 0`）时直接试满缓冲，`GetBuffer` 真没空间就会失败返回 0，对正常路径无害。

## 排查纪律（本轮绕远路的直接原因）

**先排除自己的探针占着同一设备**。日志里 `Initialize 失败（全部配方）… 0x8889000A
DEVICE_IN_USE` 出现过一次，当时正是探针在循环开关；此后 rlplayer 的
`RENDERFAIL` 有一半是这个污染造成的假失败。硬件类排查的第一步：确认没有自家测试进程占用端点。

## 主观听感差异（用户 2026-10-23 描述）

同一首曲子、同一设备（UltraLite-mk5 Main Out 1-2）、同一 DSP 配置（poly-sinc-short-lp + LNS15）下，
用户对比 **ASIO** 与 **WASAPI 独占** 的原话：

> **「有少许听感差距，WASAPI 更炸一点。」**
> （按用户裁定，"炸"指偏高、偏硬/毛刺那一类听感，不是音量大小——音量已在共享电平修复后对齐）

数据层已证明两者等价，所以差异只能来自**交付机制**：

| 维度 | ASIO | WASAPI 独占 |
|---|---|---|
| 打包 | `v = s / 2^23`，再 `× (2^31-1)` ≈ `s << 8` | `p[i] = s`（32 容器低 24 位） |
| 换算差 | **≤ 2 LSB / 2^32**（折合 24 位域 0.0002 LSB，不可闻） | 同左 |
| 时钟来源 | MOTU ASIO 驱动直递 | Windows 音频引擎调度后再交驱动 |
| 缓冲深度 | 32 帧（≈0.7 ms @44.1k） | 1024 帧（≈23 ms @44.1k） |
| 驱动路径 | 专属 ASIO 驱动 | WDM 内核流（独占 = 绕过混音器，但仍经引擎） |

**结论**：数据层无可改。"更炸"属于 WDM 通路与 ASIO 通路在时钟/抖动上的固有差异（驱动侧特性），
不是本程序的算法差异。要听感一致走 ASIO；要 Windows 系统集成（音量/独占切换/多设备）走 WASAPI。

## 结论

| 路径 | 44.1k 源 | bit-perfect |
|---|---|---|
| ASIO（MOTU Gen 5） | ✅ 直出 | ✅ |
| WASAPI 独占 | ✅（三个坑修完后） | ✅ |
| WASAPI 共享 | ✅ 经过 Windows 混音器 | ❌（受混音率与 SRC 影响） |

