# 音量控制与参数 EQ 解析纪要（Round 9）

代码：[src/dsp/volume.h](src/dsp/volume.h)、[src/dsp/eq_text.h](src/dsp/eq_text.h)　测试：[tests/test_volume_eq.cpp](tests/test_volume_eq.cpp)

> 遵守 [AGENTS.md](AGENTS.md) 铁律 1：**全程离线验证，不打开任何音频设备。**

## 1. 音量控制

实测（全部通过）：

| 验证项 | 结果 |
|---|---|
| 0 dB | **逐位一致**（这是「比特完美」的组成部分） |
| −6.0206 dB | **恰好 ×0.5** |
| −144 dB | **完全静音**（全零） |
| 100 ms 渐变 | 单调、终值 0.100000000、最大单样本跳变 **1.875e−04** |
| 手册 4.2 固定音量 | 灰显 **−3.0 dB** / 勾选 **−6.0 dB** |

**设计决策：音量模块不做抖动。** 抖动必须在**音量之后、量化之前**注入，
才能让抖动真正作用于最终量化误差。管线末端的 `Quantizer` 才是正确的注入点。

### ⚠️ 本轮抓到一个真 bug

`set_gain_db()` 只更新了目标增益，**没有重新启动渐变**（`remaining_` 保持 0）。
后果：音量变化是**瞬时跳变**，实际播放中会产生**咔哒声**。
测试直接暴露了它（终值停在 1.0 而不是 0.1，单样本跳变为 0）。已修复。

> 这是本项目第一个由测试抓到的**产品代码**缺陷（此前几次都是测试自身的问题）。

## 2. REW / AutoEq 参数 EQ 文本解析

支持手册第 7 章 Matrix 的 `Process` 字段直接吃这类文件。

**REW 格式**：
```
Filter  1: ON PK     Fc   105.0 Hz  Gain   5.5 dB  Q  0.700
Filter  2: ON LS     Fc    80.0 Hz  Gain   2.0 dB  Q  0.700
Filter  3: ON HS     Fc  6000.0 Hz  Gain  -4.0 dB  Q  0.700
Filter  4: OFF PK    Fc   900.0 Hz  Gain   9.0 dB  Q  2.000
```

**AutoEq 格式**（多一行 Preamp）：
```
Preamp: -6.0 dB
Filter 1: ON PK Fc 105 Hz Gain 5.5 dB Q 0.70
```

实测（全部通过）：

| 验证项 | 结果 |
|---|---|
| 滤波器个数 | 5 个，与原文一致 |
| 第 1 条参数 | type=PK fc=105.0 gain=5.5 q=0.700 |
| 类型识别 | LS / HS / LP 全部正确 |
| OFF | 被正确识别为禁用，不进入滤波链 |
| Preamp | −6.0 dB 解析正确 |
| **PK 在 fc 处增益** | **6.0000 dB**（设定 6.0000 dB） |
| 多段叠加 | 100 Hz +4.00 / 8 kHz −5.00 / 1 kHz −0.03 dB，互不干扰 |

支持类型：PK 峰值 / LS 低架 / HS 高架 / LP 低通 / HP 高通 / BP 带通 / NO 陷波 / AP 全通。

## 3. 对整体架构的意义

至此 HQPlayer 手册第 4~7 章的自定义功能基本齐备：

| 手册章节 | 实现 |
|---|---|
| 4.4 PCM 抖动/噪声整形 | [dither.h](src/dsp/dither.h)（10 项全） |
| 4.5 SDM 调制器 | [modulator.h](src/dsp/modulator.h) |
| 4.6 滤波器 | [filter_families.h](src/dsp/filter_families.h)（12 预设 + 最小相位） |
| 4.2 音量控制 | [volume.h](src/dsp/volume.h)（本轮） |
| 6 卷积引擎 | [convolution.h](src/dsp/convolution.h) |
| 7 Matrix + 插件 | [matrix.h](src/dsp/matrix.h)、[eq_text.h](src/dsp/eq_text.h)（本轮） |

## 4. 下一步

1. 把卷积 / Matrix / EQ / 音量**接入播放管线**（`src/engine/pipeline.h`）
2. REW/AutoEq 文本 → 卷积 FIR 的转换（手册的「IIR to FIR」选项）
3. NAA / UPnP 网络音频
4. Web UI 控制面
