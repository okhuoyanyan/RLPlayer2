# DSP 核心进展（Round 2）

代码：`src/dsp/`　测试：`tests/test_resampler.cpp`　构建：`cmake --build build && build/test_resampler.exe`

## 1. 已实现模块

| 文件 | 内容 |
|---|---|
| [kaiser.h](src/dsp/kaiser.h) | 零阶贝塞尔 I0、Kaiser 窗、由阻带衰减反推 beta、由过渡带反推抽头数 |
| [filter_design.h](src/dsp/filter_design.h) | windowed-sinc 低通原型设计（Kaiser 窗，单位直流增益）、幅频响应分析 |
| [resampler.h](src/dsp/resampler.h) | **多相 sinc 重采样引擎**：任意有理 L/M、多声道、流式、相位主序布局、比特完美透传 |
| [fft.h](src/dsp/fft.h) | radix-2 复数 FFT（客观测量用，后续分块卷积的基础） |

## 2. 客观验证结果（全部通过）

以 **44100 → 384000**（L=1280, M=147, 每相位 204 抽头, 原型 261120 抽头, 群延迟 101.5 输入样本）为例：

| 指标 | 实测 | 判定 |
|---|---|---|
| 通带纹波 (100 Hz – 20 kHz) | **0.0000 dB** | ✅ < 0.1 dB |
| 阻带最大泄漏 (25 kHz – 28.2 MHz) | **−189.10 dB** | ✅ > 170 dB |
| 谐波失真 (2–10 次) | **−134.54 dBc** | ✅ < −100 dBc |
| 带外镜像 (21.5 k – 180 kHz) | **−157.65 dBc** | ✅ < −100 dBc |
| 直流增益 | **1.000000000**（纹波 1.3e−10） | ✅ 精确 |
| 比特完美透传 (44.1k→44.1k) | 样本逐位相同 | ✅ |

## 3. ⚠️ 踩到并修掉的关键陷阱（务必记住）

**内插滤波器的直流增益必须是 L，不是 1。**

有理 L/M 转换 = 上采样(插零) → 滤波 → 下采样。插零使信号平均功率降低 L 倍，因此原型滤波器在内插域的直流增益必须为 **L**；等价说法是"**每一相位的抽头之和必须为 1**"。

如果按常规归一化成 1，输出会被整体衰减 L 倍：L=1280 时正好 **−62.1 dB**。
**实测证据**：修复前 1 kHz 正弦输入 −6.02 dB，输出读到 −68.53 dB（差 62.5 dB）；修复后为 −6.39 dB（差额来自非整数 bin 的扇贝损失），直流测试精确到 1e−10。

## 4. 性能

| 项 | 值 |
|---|---|
| 设计 261120 抽头原型 | 秒级 |
| 44.1k → 384k 处理 1 秒立体声 | 单核可接受（naive 内积，尚未 SIMD 优化） |
| 完整验证套件 | 25 秒（优化前 107 秒） |

**已做的优化**：`magnitude_db` 原实现每抽头调用 `cos/sin`（上千万次超越函数），改为单位复数递推旋转 + 周期性归一化，**提速 4.3 倍且结果完全一致**。

**待做的优化**：重采样内积循环向量化（AVX-512）、相位抽头按 cache line 对齐、大抽头数走 GPU 分块 FFT 卷积。

## 5. 下一步（按用户点明的"精华"优先级）

1. **滤波器家族**：在现有 `design_lowpass` 之上派生 HQPlayer 同族滤波器
   - poly-sinc 族（lp/mp/short/long）、gauss 族（Kaiser→Gaussian 窗）、closed-form、sinc-S/M/L（超长抽头）
   - **最小相位**版本：倒谱法（cepstral）由线性相位原型变换得到
   - **1x / Nx 双路径**：44.1/48 与高码率走各自最优滤波器
   - Apod 计数判据（手册 4.6）
2. **Dither / Noise Shaping 全家族**（手册 4.4，共 10 项）
   - 抖动：RPDF / TPDF / Gauss1 / shaped
   - 噪声整形：NS1 / NS4 / NS5 / NS9 / LNS15（误差反馈结构 + 各自阶数的噪声传递函数设计）
   - ⚠️ **严格按手册：不臆造 NS2/NS3/NS6/NS7/NS8**
3. **SDM 调制器**（手册 4.5，共 36 项）
   - DSD5 / DSD5v2 / DSD5EC / DSD7、ASDM5 族、ASDM7 族（含 ul/light/fast/super/512+fs）
   - AHM 多电平（5-level / 8-bit hybrid）
   - Integrator 可选（IIR/IIR2/IIR3/FIR/FIR2/FIR-bl/FIR-bw/CIC）
   - DSD→PCM 噪声滤波 13 项 + 转换算法 14 项
4. **GPU 分块 FFT 卷积**（HIP + rocFFT）—— 百万抽头走这条路

功能对齐清单见 [hqplayer-features-spec.md](research/hqplayer-features-spec.md)（165 项）。
