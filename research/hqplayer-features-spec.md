# HQPlayer 6 功能对齐规格书（基于官方手册 4.4 – 4.7 章）

> 本文件是后续开发用的「功能对齐清单」：逐项抄录 HQPlayer 官方手册中 PCM 抖动/噪声整形、SDM 调制器、滤波器/过采样、Advanced 高级设置的全部可选项与全部数值指标，并附工程可实现性解读。

## 0. 文档信息

| 项目 | 内容 |
|---|---|
| 来源文件 | D:\RTA-hqplayer\research\hqplayer6-manual.txt（HQPlayer 6 Desktop 官方手册，共 2621 行，65 页） |
| 精读范围 | 第 480–1790 行（4.4 PCM / 4.5 SDM / 4.6 Filter），第 1791–1872 行（4.7 Advanced） |
| 交叉核对范围 | 第 60–479 行（1. 简介、2. 主界面、4. Settings、4.2 Outputs、4.3 Combo），第 2208–2382 行（播放滤波器、故障排查 Buffer time） |
| 版权 | Copyright © 2008-2026 Jussi Laako / Signalyst |
| 生成日期 | 2026-10-18 |
| 用途 | 功能对齐／回归测试清单／算法选型依据 |

### 0.1 阅读约定

- **【原文】** 表示手册原句的直译或紧贴原文的转述。
- **【推断】** 表示手册没有明说、由本文件补出的工程解释，**不可当作手册事实**。
- 表格中的「质量」为手册自带的 1/5 – 5/5 相对评分，手册明确说明「评分不是绝对的，只是各滤波器输出技术质量之间的相对参考」。
- 所有行号指 hqplayer6-manual.txt 的物理行号，便于回查。

### 0.2 条目统计（总表）

| 章节 | 类别 | 条目数 | 手册行号 |
|---|---|---|---|
| 4.4 PCM | 抖动/噪声整形（Noise-shaping / dither） | **10** | 507–547 |
| 4.4 PCM | DAC bits 推荐值（R2R DAC 型号表） | 3 组 / 8 型号 | 563–566 |
| 4.4 PCM | DSD→PCM 噪声滤波器（Noise filter） | **13** | 582–620 |
| 4.4 PCM | DSD→PCM 转换算法（Conversion） | **14** | 623–666 |
| 4.5 SDM | 调制器（Modulator） | **36** | 682–855 |
| 4.5 SDM | 积分器（Integrator） | **8** | 875–892 |
| 4.5 SDM | SDM→SDM 转换（Conversion） | **3** | 900–909 |
| 4.5 SDM | DirectSDM 直通开关 | **1** | 910–914 |
| 4.6 Filter | 滤波器/过采样（Filter） | **69** | 927–1790 |
| 4.7 Advanced | 高级可调项 | **11** | 1791–1872 |
| — | **可选项/可调项合计** | **165** | — |

> **合计口径**：10 + 13 + 14 + 36 + 8 + 3 + 1 + 69 + 11 = **165 项**（不含 DAC bits 型号表与数值指标表）。

### 0.3 三个必须先说明的「手册事实缺口」

这三条直接影响「功能对齐」的正确性，先列在最前面，避免后续开发按错误预期实现：

| 缺口 | 手册实际情况 | 对开发的影响 |
|---|---|---|
| **NS2 / NS3 / NS6 / NS7 / NS8 不存在** | 本版手册的抖动表只列了 **NS1、NS4、NS5、NS9** 四个 NS 系列成员，没有 NS2、NS3、NS6、NS7、NS8 | 不要凭「NS1–NS9 连续编号」的直觉补实现；UI 选项必须与手册/实际软件版本逐项核对 |
| **「direct」不是调制器后缀** | 手册中只有 **DirectSDM** 这个设置项（DSD 源码 → DSD 输出时关闭全部处理），没有任何名为 "direct" 的调制器 | 「direct」需求应落到 DirectSDM 开关，而非调制器命名 |
| **4.7 Advanced 里没有 buffer time / DAC bits / 高分辨率音量** | Buffer time 在 **4.2 Outputs**（第 372–387 行）；DAC bits 在 **4.4 PCM**（第 549–557 行）；音量范围与 Fixed volume 在 **4.2 Outputs**（第 434–446 行）。Advanced 的 11 项见第 5 章 | 做设置页时要按「页签归属」对齐，不能把这些塞进 Advanced |

另：手册**全文没有出现 "1.5M" 字样**，最高 PCM 速率以「playback rates from 32 to 1536 kHz are possible」表述（第 123 行）；DSD 最高提到 **DSD1024**（第 620 行）。

---

## 1. PCM 抖动 / 噪声整形（手册 4.4，第 503–548 行）

### 1.1 总述【原文】

- 该选择用于在不同「字长缩减算法（word-length reduction algorithms）」之间切换。
- 手册原句：**「It is always recommended to use at least TPDF dither.」**（始终建议至少使用 TPDF 抖动）。
- 该设置与音量控制强相关：手册 2.15 节（第 251–254 行）指出「所选抖动/噪声整形算法对音量调节的质量有显著影响」。

### 1.2 全部选项（10 项）

| # | 选项 | 类型 | 阶数 | 手册描述的机理【原文】 | 推荐速率 / 适用场景 | 限制与警告 | 行号 |
|---|---|---|---|---|---|---|---|
| 1 | **none** | 无抖动无整形，仅舍入 | — | 「No noise-shaping or dithering, only rounding.」 | 仅用于测试：配合 none 滤波器做 bit-perfect 输出 | **Not recommended**（不推荐） | 507–509 |
| 2 | **NS1** | 噪声整形 | 1 阶 | 「Simple first order noise-shaping.」样本值被舍入，量化误差被整形，使误差能量被推向高频 | 主要用于 **176.4 / 192 kHz** 升频 | 对超声波噪声敏感的设备**不建议**使用 NS1（第 548 行） | 518–521 |
| 3 | **NS4** | 噪声整形 | 4 阶 | 「Fourth order noise-shaping. Similar in shape as "shaped" dither.」 | 所有 **≥ 88.2 kHz** 的速率 | — | 522–523 |
| 4 | **NS5** | 噪声整形 | 5 阶 | 「Fairly aggressive noise-shaping designed for 8x and 16x rates.」 | **352.8 / 384 / 705.6 / 768 kHz**；不建议低于 192 kHz 使用 | 不建议用于 192 kHz 以下；手册特别点名「对 PCM1704 在最高速率下表现尤佳」 | 524–527 |
| 5 | **NS9** | 噪声整形 | 9 阶 | 「Very aggressive noise-shaping designed especially for 4x rates.」 | **176.4 / 192 kHz（4x）** | 手册特别点名适合老式 16-bit 4x 多比特 DAC，如 **TDA154x** | 528–531 |
| 6 | **LNS15** | 线性噪声整形 | 15 阶 | 「15th order linear noise shaping. Smooth noise-shaping slope.」平滑的整形斜率 | **705.6 / 768 kHz（16x）**；也可用于 **352.8 / 384 kHz（8x）** | 不建议用于更低速率 | 532–535 |
| 7 | **RPDF** | 白噪声抖动 | — | 「Rectangular Probability Density Function. White noise dither.」矩形概率密度，计算量最轻 | 仅适合 **24-bit 或更高**的输出硬件 | 低比特深度不可用 | 536–538 |
| 8 | **TPDF** | 白噪声抖动 | — | 「Triangular Probability Density Function. This is the industry standard simple dither mechanism.」三角概率密度，业界标准 | **任意速率**；44.1 / 48 kHz 播放时推荐；**通用首选** | — | 539–541 |
| 9 | **Gauss1** | 高斯抖动 | — | 「Gaussian Probability Density Function. High quality flat frequency dither.」平坦频响的高质量抖动 | **≤ 96 kHz** 且不适合做噪声整形的场合 | — | 542–544 |
| 10 | **shaped** | 整形抖动 | — | 「Shaped dither. Noise used in this dither has shaped frequency distribution to lower audibility of the dither noise.」抖动噪声本身经过整形以降低可闻度 | **88.2 / 96 kHz 或更高** | — | 545–547 |

### 1.3 纯抖动 vs 噪声整形：手册给出的分界

| 输出速率区间 | 手册推荐 | 依据 |
|---|---|---|
| 44.1 / 48 kHz（1x） | **TPDF**（通用）；Gauss1 可用于 ≤96 kHz | TPDF 条目；Gauss1 条目 |
| 88.2 / 96 kHz（2x） | **NS4**、**shaped** | NS4「≥88.2 kHz」；shaped「88.2/96 kHz 或更高」 |
| 176.4 / 192 kHz（4x） | **NS9**（激进）；NS1（温和） | NS9「专为 4x」；NS1「主要用于 176.4/192」 |
| 352.8 / 384 kHz（8x） | **NS5**；LNS15 也可用 | NS5「8x/16x」；LNS15「可用于 8x」 |
| 705.6 / 768 kHz（16x） | **LNS15**；NS5 | LNS15「专为 16x」；NS5「8x 和 16x」 |

### 1.4 DAC bits（第 549–566 行）

**何时必须设置【原文】：**

1. DAC 通过**单向接口**连接时（S/PDIF、AES/EBU、I2S）：必须选对位数。
2. DAC 通过 **USB** 连接但输入分辨率**不是 32-bit** 时：建议填真实值。

**R2R DAC 推荐位深表（原文全部 3 组）：**

| DAC 型号 | Bits |
|---|---|
| Holo Audio Cyan 2 / Spring / Spring 2 / Spring 3 / May | **20** |
| Denafrips（全系） | **19** |
| LAiV Harmony DAC / Harmony µDAC | **18** |

**R2R 线性度补偿【原文】：** 当合适的噪声整形器（如 **LNS15、NS9、NS5**）与高输出速率组合使用时，可以**校正所有 R2R DAC 固有的线性度误差**，从而降低尤其小信号时的失真，并**减少过零失真（zero-crossing distortion）**。

### 1.5 DSD 源 → PCM：噪声滤波器（Noise filter，13 项，第 567–620 行）

**背景【原文】：** 这些滤波器减少源数据中的超声波噪声。标准滤波会残留少量超声波噪声；某些高音单元功率承受能力低的音箱对此敏感（大音量时尤甚），部分设计不良或 D 类功放也会在这种超声波内容下行为异常。更强的噪声滤波器会同时**限制音频可用带宽**。

**速率相关的取舍【原文】：**

- DSD64 源 → **88.2 / 96 kHz** PCM 输出：额外噪声滤波不如「standard」重要，因为大部分噪声已被切掉。
- DSDIFF / DSF 源 → **44.1 / 48 kHz** PCM 输出：**不需要**额外噪声滤波，加了反而降低回放质量。

| # | 选项 | 手册描述【原文】 | 推荐度 | 行号 |
|---|---|---|---|---|
| 1 | **standard** | 应用标准噪声滤波器 | **Recommended** | 583–584 |
| 2 | **low** | 类似 standard，但转角频率更低，在超声波范围得到近乎平坦的噪声曲线 | **Recommended** | 585–587 |
| 3 | **high-order** | 为高阶调制器制作的内容设计的高阶噪声滤波器 | **Recommended** | 588–590 |
| 4 | **sac** | Sliding average converter（滑动平均转换器） | — | 591 |
| 5 | **wec** | Weighted element converter（加权单元转换器） | — | 592 |
| 6 | **wec2** | Weighted element converter，优化为紧贴 DSD/SACD 规范；**Non-ringing linear-phase**（无振铃线性相位） | **Recommended** | 593–595 |
| 7 | **slow-lp** | 慢滚降线性相位滤波器 | — | 596 |
| 8 | **slow-mp** | 慢滚降最小相位滤波器 | — | 597 |
| 9 | **medium** | 中等滚降线性相位，设计目标是「尽可能温和，同时只让极少量带外噪声通过」 | **Recommended** | 604–607 |
| 10 | **medium-high** | 中等滚降**高抑制**线性相位，同样「尽可能温和、最少带外噪声」；当 PCM Conversion 选 **none** 时用它替代 medium | **Recommended** | 608–613 |
| 11 | **fast-lp** | 快速滚降线性相位 | — | 614 |
| 12 | **fast-mp** | 快速滚降最小相位 | — | 615 |
| 13 | **brickwall** | 砖墙滤波，不让任何带外噪声通过，非常陡的线性相位。截止频率：**DSD64 = 25 kHz、DSD128 = 50 kHz、DSD256 = 100 kHz、DSD512 = 200 kHz、DSD1024 = 400 kHz** | — | 616–620 |

### 1.6 DSD 源 → PCM：转换算法（Conversion，14 项，第 621–666 行）

| # | 选项 | 手册描述【原文】 | 备注 | 行号 |
|---|---|---|---|---|
| 1 | **traditional** | 传统递归转换算法；使用慢滚降滤波器以**最小化振铃** | 递归（IIR 式） | 624–626 |
| 2 | **single-steep** | 单次（single-pass）转换，陡滚降 | — | 627–628 |
| 3 | **single-short** | 单次转换，正常滚降；在振铃与宽频响之间做优化折中 | — | 629–631 |
| 4 | **sinc-S** | 线性相位、自适应长度、陡滚降、高衰减的单次转换；**抽头数 65536** | 注意与 4.6 的 sinc-S（4096×比率）**同名但不同物** | 632–634 |
| 5 | **sinc-M** | 线性相位、**百万抽头**、陡滚降、高衰减的单次转换 | 同上，与 4.6 的 sinc-M 同名不同参数 | 635–637 |
| 6 | **poly-lp** | 线性相位单次转换 | — | 638–639 |
| 7 | **poly-mp** | 最小相位单次转换 | — | 640–641 |
| 8 | **poly-short-lp** | 线性相位、慢滚降单次转换 | **Recommended** | 642–643 |
| 9 | **poly-short-mp** | 最小相位、慢滚降单次转换 | — | 650–652 |
| 10 | **poly-xtr** | 线性相位、极端滚降与衰减的单次转换 | — | 653–654 |
| 11 | **poly-xtr-short** | 线性相位、极端滚降与衰减的单次转换（短版） | — | 655–657 |
| 12 | **poly-ext2** | 线性相位、扩展频响、陡滚降、高衰减的单次转换 | — | 658–660 |
| 13 | **poly-gauss-long** | 线性相位、高斯、极高衰减的单次转换；时间-频率响应最优 | — | 661–664 |
| 14 | **none** | 不做抽取（decimation），中间输出速率等于源 DSD 速率 | 即 DSD 位流直通到后续处理 | 665–666 |

**6 dB gain 复选框（第 667–671 行）【原文】：** DSDIFF / DSF 文件通常在信号电平上留有 **6 dB 余量**。勾选「6 dB gain」会施加 6 dB 增益，去掉该余量，使正常回放电平与普通 PCM 一致。但这可能让某些素材过载，需要额外用音量控制衰减。

---
## 2. SDM 调制器（手册 4.5，第 672–914 行）

### 2.1 总述【原文】

- 调制器用于产生 SDM 输出，是 PCM→DSD 或 DSD→DSD 重调制的核心。
- 绝大多数调制器的描述明确写 **one-bit**；**AMSDM 系列**被描述为 **pseudo-multi-bit（伪多比特）**（手册未说明其输出电平数，按命名与 DSD 输出推定仍为 1-bit 量化的变体）；**AHM 系列**则是 **hybrid（混合）** 调制器，量化器为 **5-level（5L）** 或 **8-bit（8B）**。
- 手册为每个调制器给出一个 **Generation（代）** 列，取值为 1st – 8th，代表调制器设计的迭代世代（手册未进一步定义各代差异）。

### 2.2 全部调制器（36 项）

| # | 调制器 | 阶数 | 类型/电平 | 生成代 | 速率优化 | 手册描述【原文】 | 行号 |
|---|---|---|---|---|---|---|---|
| 1 | **DSD5** | 5 | 1-bit | 1st | rate adaptive（速率自适应） | 「Rate adaptive fifth order one-bit delta-sigma modulator.」 | 683 |
| 2 | **DSD5v2** | 5 | 1-bit | 2nd | — | 「Revised fifth order one-bit delta-sigma modulator.」（修订版） | 684 |
| 3 | **DSD5v2 256+fs** | 5 | 1-bit | 2nd | **≥ 10.24 MHz** | 「Revised fifth order one-bit delta-sigma modulator optimized for rates >= 10.24 MHz.」 | 685–689 |
| 4 | **DSD5EC** | 5 | 1-bit | 4th | rate adaptive | 「Rate adaptive fifth order one-bit delta-sigma modulator with extended compensation.」 | 690–692 |
| 5 | **ASDM5** | 5 | 1-bit | 3rd | adaptive（自适应） | 「Adaptive fifth order one-bit delta-sigma modulator.」 | 693 |
| 6 | **ASDM5EC** | 5 | 1-bit | 4th | adaptive | 「Adaptive fifth order one-bit delta-sigma modulator with extended compensation.」 | 694–695 |
| 7 | **ASDM5ECv2** | 5 | 1-bit | 5th | adaptive | 「Second generation of ASDM5EC with minor improvements.」 | 696–698 |
| 8 | **ASDM5ECv3** | 5 | 1-bit | 6th | adaptive | 「Third generation of ASDM5EC with minor improvements.」 | 699–701 |
| 9 | **ASDM5EC-ul** | 5 | 1-bit | 7th | adaptive | 「…with extended compensation. **Ultralight version**.」 | 711–713 |
| 10 | **ASDM5EC-light** | 5 | 1-bit | 7th | adaptive | 「…with extended compensation. **Light version**.」 | 714–717 |
| 11 | **ASDM5EC-fast** | 5 | 1-bit | 7th | adaptive | 「…with extended compensation. **Transient and load optimized version**.」 | 719–723 |
| 12 | **ASDM5EC-super** | 5 | 1-bit | 7th | adaptive | 「…with extended compensation. **Super version**.」 | 724–728 |
| 13 | **ASDM5EC-ul 512+fs** | 5 | 1-bit | 7th | **512x 及以上** | 「…extended compensation. Optimized for 512x and higher rates. Ultralight version.」 | 730–735 |
| 14 | **ASDM5EC-light 512+fs** | 5 | 1-bit | 7th | **512x 及以上** | 「…Optimized for 512x and higher rates. Light version.」 | 736–741 |
| 15 | **ASDM5EC-fast 512+fs** | 5 | 1-bit | 7th | **512x 及以上** | 「…Optimized for 512x and higher rates. Transient and load optimized version.」 | 742–747 |
| 16 | **ASDM5EC-super 512+fs** | 5 | 1-bit | 7th | **512x 及以上** | 「…Optimized for 512x and higher rates. Super version.」（原文误拼 modulatro） | 748–753 |
| 17 | **DSD7** | 7 | 1-bit | 2nd | — | 「Seventh order one-bit delta-sigma modulator.」 | 754 |
| 18 | **DSD7 256+fs** | 7 | 1-bit | 2nd | **≥ 10.24 MHz** | 「Seventh order one-bit delta-sigma modulator optimized for rates >= 10.24 MHz.」 | 755–757 |
| 19 | **ASDM7** | 7 | 1-bit | 3rd | adaptive | 「Adaptive seventh order one-bit delta-sigma modulator.」 | 758 |
| 20 | **ASDM7EC** | 7 | 1-bit | 4th | adaptive | 「Adaptive seventh order one-bit delta-sigma modulator with extended compensation.」 | 759–760 |
| 21 | **ASDM7ECv2** | 7 | 1-bit | 5th | adaptive | 「Second generation of ASDM7EC with minor improvements.」 | 762–763 |
| 22 | **ASDM7ECv3** | 7 | 1-bit | 6th | adaptive | 「Third generation of ASDM7EC with minor improvements.」 | 764–766 |
| 23 | **ASDM7EC-ul** | 7 | 1-bit | 7th | adaptive | 「…Ultralight version.」 | 768–770 |
| 24 | **ASDM7EC-light** | 7 | 1-bit | 7th | adaptive | 「…Light version.」 | 771–775 |
| 25 | **ASDM7EC-fast** | 7 | 1-bit | 7th | adaptive | 「…Transient and load optimized version.」 | 776–781 |
| 26 | **ASDM7EC-super** | 7 | 1-bit | 7th | adaptive | 「…Super version.」 | 782–786 |
| 27 | **ASDM7EC-ul 512+fs** | 7 | 1-bit | 7th | **512x 及以上** | 「…Optimized for 512x and higher rates. Ultralight version.」 | 793–798 |
| 28 | **ASDM7EC-light 512+fs** | 7 | 1-bit | 7th | **512x 及以上** | 「…Light version.」 | 799–804 |
| 29 | **ASDM7EC-fast 512+fs** | 7 | 1-bit | 7th | **512x 及以上** | 「…Transient and load optimized version.」 | 805–810 |
| 30 | **ASDM7EC-super 512+fs** | 7 | 1-bit | 7th | **512x 及以上** | 「…Super version.」（原文误拼 modulatro） | 811–816 |
| 31 | **AMSDM7 512+fs** | 7 | **pseudo-multi-bit**（伪多比特） | 3rd | **≥ 20.48 MHz** | 「Special adaptive seventh order "pseudo-multi-bit" modulator optimized for rates above >= 20.48 MHz.」 | 817–821 |
| 32 | **AMSDM7EC 512+fs** | 7 | pseudo-multi-bit | 4th | **≥ 20.48 MHz** | 「Special adaptive seventh order "pseudo-multi-bit" modulator with extended compensation for rates >= 20.48 MHz.」 | 822–827 |
| 33 | **AHM5EC5L** | 5 | **hybrid 5-level** | 7th | **≥ 40.96 MHz** | 「Experimental fifth order five level hybrid modulator with extended compensation. Optimized for rates >= 40.96 MHz.」＋ SNR 警告（见 2.4） | 828–836 |
| 34 | **AHM7EC5L** | 7 | **hybrid 5-level** | 7th | **≥ 40.96 MHz** | 「Experimental seventh order five level hybrid modulator with extended compensation. Optimized for rates >= 40.96 MHz.」＋ 同一 SNR 警告 | 837–845 |
| 35 | **AHM5EC8B** | 5 | **hybrid 8-bit** | 8th | **≥ 40.96 MHz** | 「Fifth order 8-bit hybrid modulator with extended compensation. Optimized for rates >= 40.96 MHz. Bandwidth optimized to provide enough flat noise floor bandwidth for practically all hires content.」 | 846–850 |
| 36 | **AHM7EC8B** | 7 | **hybrid 8-bit** | 8th | **≥ 40.96 MHz** | 「Seventh order 8-bit hybrid modulator with extended compensation. Optimized for rates >= 40.96 MHz. Bandwidth optimized…」（同上） | 851–855 |

### 2.3 后缀语义（严格按手册原文）

| 后缀 / 标记 | 手册原文 | 准确含义 | 出现位置 |
|---|---|---|---|
| **EC** | 「with **extended compensation**」 | 扩展补偿。手册未展开「补偿什么」，只说明是 EC 系列的核心差异 | DSD5EC、ASDM5EC*、ASDM7EC*、AMSDM7EC、AHM*EC* |
| **v2** | 「**Second generation** of ASDMxEC with **minor improvements**」 | 第二代，仅小幅改进 | ASDM5ECv2 / ASDM7ECv2 |
| **v3** | 「**Third generation** of ASDMxEC with **minor improvements**」 | 第三代，仅小幅改进 | ASDM5ECv3 / ASDM7ECv3 |
| **-ul** | 「**Ultralight version**」 | 超轻量版（最低算力） | ASDM5EC-ul、ASDM7EC-ul（含 512+fs 变体） |
| **-light** | 「**Light version**」 | 轻量版（算力低于标准，高于 ul） | ASDM5EC-light、ASDM7EC-light（含 512+fs） |
| **-fast** | 「**Transient and load optimized version**」 | 瞬态与负载优化版（"load" 指 CPU 负载） | ASDM5EC-fast、ASDM7EC-fast（含 512+fs） |
| **-super** | 「**Super version**」 | 超级版（手册未给技术细节，仅此一词） | ASDM5EC-super、ASDM7EC-super（含 512+fs） |
| **512+fs** | 「**Optimized for 512x and higher rates**」 | 针对 **512 倍及以上** DSD 速率优化（DSD512 起，22.5792 / 24.576 MHz 量级） | ASDM*EC-* 512+fs、AMSDM7 512+fs、AMSDM7EC 512+fs |
| **256+fs** | 「optimized for rates **>= 10.24 MHz**」 | 针对 **≥10.24 MHz** 优化（约 DSD256 @40k 基准族） | DSD5v2 256+fs、DSD7 256+fs |
| **AHM** | 「**hybrid** modulator」，5-level 或 8-bit | 混合调制器：环路内使用多电平/多比特量化，而非 1-bit | AHM5EC5L / AHM7EC5L / AHM5EC8B / AHM7EC8B |
| **5L / 8B** | 「**five level**」/「**8-bit**」 | 量化器电平数：5 电平 / 8 bit | AHM*5L、AHM*8B |
| **AMSDM** | 「**pseudo-multi-bit**」 | 伪多比特（仍输出 1-bit，但环路按多比特思路设计） | AMSDM7 512+fs、AMSDM7EC 512+fs |
| **direct** | — | **手册中不存在**该调制器后缀；只有 **DirectSDM** 设置项 | — |

### 2.4 手册对调制器选型的说明【原文，第 856–869 行】

| 主题 | 原文要点 |
|---|---|
| 5 阶 vs 7 阶 | 5 阶更适合**模拟重构滤波器简单**的 DAC；7 阶技术性能更好，但对 DAC 的**模拟重构滤波器要求更高** |
| 单开关 vs 多开关 | 5 阶适合只有**单个开关元件**的 DAC；7 阶在**多元件开关阵列**的 DAC 上才有发挥更好性能的潜力 |
| 固定 vs 自适应 | **DSD\* 系列是固定配置**；**ASDM\* 系列基于源信号做各种自适应** |
| DAC 匹配 | **ESS Sabre 系 DAC 推荐 5 阶**；**大多数其他 DAC 用 7 阶最优** |
| AHM 警告（5L 专属） | 「Limited SNR compared to other modulators, best suited for loudspeaker system and/or when digital volume control is not needed. Not recommended when HQPlayer's volume control is the primary volume control method.」→ 相比其他调制器 **SNR 有限**；最适合**音箱系统**和/或**不需要数字音量**的场合；**不建议**把 HQPlayer 音量作为主音量控制时使用 |
| AHM 8B 定位 | 带宽经过优化，可为「几乎所有 hires 内容」提供足够平坦的噪声底带宽 |

### 2.5 积分器（Integrator，8 项，第 870–892 行）

**适用场景【原文】：** 三种类型的 delta-sigma 积分器，用于不同的 **SDM → SDM 重调制**方案；主要影响**最高频段的频率与相位响应**。表中频率以 **DSD64 源速率**为基准，随源采样率按比例缩放。

| # | 选项 | 手册描述【原文】 | DSD64 基准音频带宽 | 行号 |
|---|---|---|---|---|
| 1 | **IIR** | Normal IIR type integrator structure | **50 kHz** | 876–877 |
| 2 | **IIR2** | IIR type integrator structure designed to **minimize residual noise** | **25 kHz** | 878–880 |
| 3 | **IIR3** | **High order** IIR type integrator structure | **30 kHz** | 881–882 |
| 4 | **FIR** | **Weighted FIR** type integrator structure | 未给 | 883 |
| 5 | **FIR2** | Weighted FIR type integrator structure | **50 kHz** | 884–885 |
| 6 | **FIR-bl** | FIR type integrator structure with **band-limiting** | **24 kHz**，到 **45 kHz** 完全截止 | 886–888 |
| 7 | **FIR-bw** | FIR type integrator structure with **brickwall band-limiting** | **21.5 kHz**，到 **30 kHz** 完全截止 | 889–891 |
| 8 | **CIC** | **Cascade comb** type integrator structure | 未给 | 892 |

> **手册自身的不一致（需记录）**：正文写「There are **three** types of delta-sigma integrators」，但表格实际列出 **8 个选项**。合理解读是三大「结构族」：IIR（3 个）、FIR（4 个）、CIC（1 个）。【推断】

### 2.6 SDM → SDM 转换（Conversion，3 项，第 893–909 行）

**作用【原文】：** 不同选项影响「假定包含有用信号的频率孔径（frequency aperture）」，同时影响噪声整形噪声。

**选型举例【原文】：** 钢琴不含高频谐波，此类素材适合 **narrow**；近距离拾音的打击乐通常含高电平高频内容，此时 **wide** 更合适；**XFi 适合所有情况**。**默认值为 XFi**。

| # | 选项 | 手册描述【原文】 | 说明 |
|---|---|---|---|
| 1 | **wide** | Wide bandwidth signal | 宽带宽信号 |
| 2 | **narrow** | Narrow bandwidth signal | 窄带宽信号 |
| 3 | **XFi** | Extreme fidelity medium bandwidth | 极高保真、中等带宽；**默认** |

### 2.7 DirectSDM（第 910–914 行）

| 项目 | 内容 |
|---|---|
| 触发条件 | 源为 **DSD** 内容，且输出格式为 **SDM**，且目标是 **DSD 设备或文件** |
| 行为【原文】 | 「DirectSDM setting **disables all processing**」——关闭全部处理（真正的位直通） |
| 副作用【原文】 | 「Enabling DirectSDM will **disable volume control** and set **PCM volume to fixed -3 dBFS** value.」 |
| 相关补充（第 1900–1901 行） | 即使启用 Direct SDM，**距离/延时处理对 DSD 位直通仍然可用** |

---
## 3. 滤波器 / 过采样（手册 4.6，第 915–1790 行）

### 3.1 总则【原文】

| 规则 | 手册原文要点 | 行号 |
|---|---|---|
| 影响可用硬件速率 | 「This selection has an impact on available hardware sampling rates.」滤波器决定可选输出采样率集合 | 916–917 |
| 作者首推 | 「Different variants of "poly-sinc" are the **most recommended by the author**.」 | 918 |
| 1x 的定义 | 1x 选择覆盖**源采样率低于 50 kHz**（即 base rate）的内容 | 919–920 |
| Nx 的定义 | Nx 选择覆盖 1x 以上的**其余所有速率** | 920–921 |
| Apodizing 判据 | 单轨内 **Apod 计数 > 10** 时，**至少应使用 apodizing 滤波器** | 921–922 |
| 「仅适合最高技术质量素材」的含义 | 指整轨 **Apod 计数始终 < 10** 的素材 | 923–924 |
| 质量评分性质 | 评分**不是绝对值**，只是该滤波器输出相对表中其他滤波器的技术质量参考 | 925–926 |
| none 的速率行为 | 选 none 时输出采样率跟随源文件；对 DSD 源为 **DSD 速率的 1/16** | 500–502 |

### 3.2 表格列语义

| 列 | 取值 | 含义 | 手册是否定义 |
|---|---|---|---|
| **Special focus** | Space / Transients / Timbre 及其组合 | 该滤波器相对擅长的主观维度：空间感、瞬态、音色 | 仅出现，未定义 |
| **Quality** | 1/5 – 5/5 | 相对技术质量评分 | 是（相对参考） |
| **Genre** | Classical、Jazz/blues、Pop/rock/electronic、Any | 推荐音乐类型 | 仅出现，未定义 |
| **Ratio** | Any / Integer / Integer up / 2x up / 1:1 | 支持的转换比率类别 | **未定义** |
| **Apod** | Y / N / ½ | 是否 apodizing | **部分**：Y/N 可由 2.6 节推知；**½ 未定义** |

**Ratio 的工程解读【推断】**（手册未给定义，按 HQPlayer 语义）：

| 标记 | 推断含义 | 对 1x/Nx 的影响 |
|---|---|---|
| **Any** | 任意比率，含非整数比（如 44.1 → 48 家族） | 1x/Nx 均可 |
| **Integer** | 仅整数比（如 44.1 → 88.2 / 176.4 / 705.6） | 非整数比会不可用（选项被禁用） |
| **Integer up** | 仅整数**升**采样（不允许降采样、不允许 1:1） | 高码率源降采样时不可用 |
| **2x up** | 仅 **2 倍及以上**升采样 | 不能用于 1:1；1x 档仍可用（只要比率 ≥2） |
| **1:1** | 不转换（仅 none） | — |

> 因此「是否支持 1x/Nx 分离」在本手册中**不是逐滤波器的属性**：1x/Nx 是 UI 上的两套独立选择，任何滤波器都可以分别被选进 1x 或 Nx 槽位；真正构成限制的是 **Ratio**（决定在该源率/输出率组合下选项是否可用）。手册没有发布「某滤波器仅限 1x」或「仅限 Nx」的清单。

### 3.3 家族总览（69 项按 11 个家族归类）

| 家族 | 成员数 | 相位 | 长度/抽头量级 | 任意比率 | Apod 支持 | 质量区间 | 手册定位 |
|---|---|---|---|---|---|---|---|
| **A. 基础/时域型**（none、IIR、IIR2、FIR、asymFIR、minphaseFIR、FFT） | 7 | 混合（模拟式/线性/最小/非对称） | 递归或可配置 | 部分 | 部分 | 1/5–4/5 | 硬件常见结构的软件实现，IIR 走「模拟味」，FFT 走频域砖墙 |
| **B. poly-sinc 经典**（-lp/-mp/-shrt/-long） | 7 | lp 线性 / mp 最小 / ip 中间 | 中等（未给抽头数） | **全部 Any** | ½ 或 Y | 3/5–4/5 | 作者首推的通用主力 |
| **C. poly-sinc halfband**（hb、hb-xs/s/m/l） | 5 | 线性 | 极短→长 | Any | 全 N | 2/5–4/5 | 2x 半带级联，只适合极干净素材 |
| **D. poly-sinc-ext / ext2** | 10 | 线性（hires 有 lp/ip/mp） | 长；xla/xl 为 8×long | Any（ext 为 Integer，short 为 Integer up） | 多数 Y | 3/5–5/5 | 扩展频响 + Nyquist 前完全截止，音色强项 |
| **E. poly-sinc-mqa/mp3** | 2 | lp 线性 / mp 最小 | 中短 | PCM: Integer up / SDM: Any | Y | 4/5 | 清理 MQA/MP3 编码高频噪声 |
| **F. poly-sinc-xtr** | 4 | lp 线性 / mp 最小 | 长/短 | Any | ½ 或 Y | 5/5 | 极端滚降与衰减，音色优先 |
| **G. poly-sinc-gauss** | 10 | 未标注（hires 有 lp/ip/mp） | 短→超长 | Any（short 为 Integer up） | 多数 Y | 3/5–5/5 | 高斯窗，时间-频率响应最优 |
| **H. 特殊/低振铃**（ASRC、polynomial-1/2、minringFIR-lp/mp） | 5 | 多种 | 短 | 多为 Integer up | 全 N | 1/5–2/5 | 低振铃但阻带差，多被标注「不推荐」 |
| **I. closed-form** | 4 | 未标注 | 高抽头→16M | 2x up | N | 2/5–3/5 | 闭式插值，超高抽头数 |
| **J. sinc 系列**（S/M/Mx/MG/MGa/L/Ls/Lm/Ll/Lh/short/medium/long/long-h） | 14 | 线性 | 4096×比率 → 131070×比率 / 1M taps | 2x up 或 Any | 部分 Y | 2/5–4/5 | 超长 sinc，空间/音色强项 |
| **K. 两段修饰 \*-2s** | 1 | 随基滤波器 | 两段级联 | 随基滤波器 | 随基滤波器 | 同基滤波器 | 降 CPU 不减质量 |

> 合计：7+7+5+10+2+4+10+5+4+14+1 = **69**。

#### 3.3.1 1x / Nx 支持矩阵

**手册立场**：1x 与 Nx 是**两套独立的 UI 选择**，1x 覆盖 **<50 kHz** 的源率，Nx 覆盖其余。手册**没有**给任何滤波器打上「仅 1x」或「仅 Nx」的标签。因此「是否支持 1x/Nx 分离」对所有家族都是**支持**，真正的差别在 **Ratio**：

| 家族 | 放入 1x 槽 | 放入 Nx 槽 | 限制来源 |
|---|---|---|---|
| A（none / IIR / IIR2 / FIR / asymFIR / minphaseFIR / FFT） | 可用 | 可用 | none 为 1:1；IIR/FIR 系仅整数比；FFT 需 ≥2x |
| B poly-sinc 经典 | 可用 | 可用 | 无（Any） |
| C poly-sinc-hb 全系 | 可用 | 可用 | 无（Any） |
| D ext2 全系 | 可用 | 可用 | ext 仅 Integer；ext2-short 仅 Integer up |
| E mqa/mp3 | 可用 | 可用 | PCM 仅 Integer up；SDM 为 Any |
| F xtr 全系 | 可用 | 可用 | 无（Any） |
| G gauss 全系 | 可用 | 可用 | gauss-short 仅 Integer up |
| H 特殊型 | 可用 | 可用 | 多为 Integer up；ASRC 为 Any |
| I closed-form 全系 | 可用 | 可用 | 仅 2x up |
| J sinc 全系 | 可用 | 可用 | 长版本仅 2x up；short/medium/long/long-h 为 Any |
| K \*-2s | 可用 | 可用 | 随基滤波器 |

**实操结论**：当源率与目标率相同（1:1）时，所有 **2x up** 家族不可用；当源率与目标率为 44.1↔48 这类非整数比时，所有 **Integer / Integer up** 家族不可用。UI 应据此灰显。

#### 3.3.2 DSD 输出适配矩阵

| 家族 | 手册是否明示适合 DSD | 手册原文依据 | 工程判断【推断】 |
|---|---|---|---|
| ext2 全系 | **是（明示）** | 「For SDM outputs, processing is two stages with minimum 16x intermediate rate.」 | 适合，长版本优先 |
| gauss（short/medium/long/xla/xl） | **是（明示）** | 同上 | 适合，gauss-long/xla 优先 |
| sinc-short / medium / long / long-h | **是（明示）** | 同上 | 适合 |
| \*-2s | **是（明示）** | 「Especially useful for highest output rates.」 | PCM→DSD 高倍率首选结构 |
| poly-sinc 经典（B） | 未明示 | 仅 Ratio = Any；作者首推 poly-sinc | 适合（推断） |
| poly-sinc-hb / gauss-halfband | 未明示 | 半带、2x 级联 | 适合高倍率（推断，级联数多） |
| xtr 全系 | 未明示 | Ratio = Any | 适合（推断，算力高） |
| sinc-M / Mx / MG / MGa / L / Ls / Lm / Ll / Lh | 未明示 | Ratio = 2x up | 可用于 DSD（比率远大于 2），但无两段说明 |
| closed-form 全系 / FFT | 未明示 | 2x up | 可用（推断），算力高 |
| IIR / IIR2 / FIR / asymFIR / minphaseFIR | 未明示 | Integer | 可用（推断），仅整数比 |
| ASRC | 未明示 | Any | 可用但计算量大，手册不推荐 |
| polynomial / minringFIR | 未明示 | Integer up | 不建议用于 DSD |
| none | — | 不转换 | 仅 1:1，不适用 DSD 升频 |

### 3.4 A 家族：基础 / 时域型（7 项）

| # | 滤波器 | 相位 | 长度 | 比率 | Apod | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | **none** | — | — | 1:1 | N | 1/5 | — | 「No sample rate conversion happens. Only sample depth is changed as needed.」不转换采样率，只按需改位深 | 932–935 |
| 2 | **IIR** | 模拟式（非线性），**无预振铃** | 递归 | Integer | Y | 2/5 | Pop, rock, jazz, blues | 模拟味，特别适合含强瞬态的录音；后振铃长是副作用（通常被掩蔽而不可闻）。使用**非常陡的 IIR**，与模拟滤波器相似，**无预振铃但后振铃长**；有**少量通带纹波**；**中等衰减**；**时域实现** | 936–950 |
| 3 | **IIR2** | 同 IIR | 递归 | Integer | Y | 4/5 | Pop, rock, jazz, blues | 同上，但**无通带纹波**，其余（陡 IIR、无预振铃、长后振铃、中等衰减、时域）相同 | 961–974 |
| 4 | **FIR** | 线性 | 中等 | Integer | Y | 3/5 | Classical | 「Typical "oversampling" digital filter」，适合多数用途，**轻微前后振铃**；最适合在真实声学环境（如音乐厅）录制的古典乐；**最普通、硬件里常见**的类型；**时域实现**；**平均量的前后振铃** | 975–986 |
| 5 | **asymFIR** | 非对称（修改相位，但**不像最小相位那么多**） | 中短 | Integer | Y | 3/5 | Jazz, blues | 适合爵士/布鲁斯及在真实声学环境中录制、含瞬态的素材；与 FIR 相同，但**预振铃更短、后振铃更长** | 987–998 |
| 6 | **minphaseFIR** | 最小相位 | 中等 | Integer | Y | 3/5 | Pop, rock, electronic | 适合流行/摇滚/电子等含强瞬态（鼓、打击乐）且在录音棚多轨制作的素材；**无预振铃，后振铃偏长** | 999–1009 |
| 7 | **FFT** | 线性 | **可在 Settings 中单独配置** | 2x up | Y | 4/5 | 任意（取决于长度） | 技术上很好的陡「砖墙」滤波器，但对强瞬态素材可能有副作用（**预振铃**）；与 FIR 类似但**在频域实现**，性能效率高，**脉冲响应相当长**；长度可在 Settings 对话框单独配置 | 1020–1035 |

### 3.5 B 家族：poly-sinc 经典（7 项）

| # | 滤波器 | 相位 | 抽头 | 比率 | Apod | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|---|---|
| 8 | **poly-sinc-lp** | **线性相位** | 未给（中等） | Any | **½** | 4/5（Space） | Classical | 「Linear phase polyphase sinc filter.」极高质量的线性相位重采样滤波器，可完成大多数典型转换比；**相位响应好，但有一定量预振铃**；详见 FIR 条目 | 1036–1045 |
| 9 | **poly-sinc-mp** | **最小相位** | 中等 | Any | **½** | 4/5（Transients） | Jazz, blues | 最小相位多相 sinc，其余类似 poly-sinc；**相位响应被改变，但无预振铃**；详见 minphaseFIR | 1046–1056 |
| 10 | **poly-sinc-shrt-lp** | 线性 | **短** | Any | ½ | 3/5（Space, transients） | Jazz, blues, electronic | 与 poly-sinc 类似，但**预/后振铃更短**，代价是**滤波质量下降（滚降不够陡）** | 1057–1070 |
| 11 | **poly-sinc-shrt-mp** | 最小 | **短** | Any | ½ | 3/5（Transients） | Pop, rock | poly-sinc-shrt 的最小相位版；类似 poly-sinc-mp 但**后振铃更短**；手册称其「**Most optimal transient reproduction**」（瞬态重现最佳） | 1071–1081 |
| 12 | **poly-sinc-long-lp** | 线性 | **长** | Any | **Y** | 4/5（Space） | Classical | 与 poly-sinc 类似，但**前后振铃更长、滤波质量更好（滚降更快）** | 1082–1090 |
| 13 | **poly-sinc-long-ip** | **中间相位（intermediate phase）** | 长 | Any | **Y** | 4/5（Space, transients） | Jazz, blues, electronic | poly-sinc-long 的中间相位版：**小预振铃 + 较长后振铃**，滤波质量更好（滚降更快） | 1091–1105 |
| 14 | **poly-sinc-long-mp** | 最小 | 长 | Any | **Y** | 4/5（Transients） | Pop, rock | poly-sinc-long 的最小相位版；类似 poly-sinc-mp 但**后振铃更长、滤波质量更好（滚降更快）** | 1116–1126 |

### 3.6 C 家族：poly-sinc halfband（5 项）

> 全部为**线性相位多相半带（half-band）**，全部 **Apod = N**，全部 **Ratio = Any**，全部标注「**Only suitable for highest technical quality source materials**」。

| # | 滤波器 | 滚降 | 衰减 | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|
| 15 | **poly-sinc-hb** | 陡（steep） | 高（high） | 4/5 | Any | 线性相位多相半带，陡滚降、高衰减；仅适合最高技术质量素材 | 1127–1132 |
| 16 | **poly-sinc-hb-xs** | 极慢 | **低** | 2/5 | Pop, rock | **Extremely short** 线性相位半带，慢滚降、低衰减 | 1133–1140 |
| 17 | **poly-sinc-hb-s** | 慢 | 平均 | 3/5 | Pop, rock | **Short** 线性相位半带，慢滚降、平均衰减 | 1141–1146 |
| 18 | **poly-sinc-hb-m** | 中等 | 中等 | 3/5 | Any | **Medium** 线性相位半带，平均滚降、中等衰减 | 1147–1152 |
| 19 | **poly-sinc-hb-l** | 快 | 高 | 4/5 | Classical, jazz, blues | **Long** 线性相位半带，快滚降、高衰减 | 1153–1160 |

### 3.7 D 家族：poly-sinc-ext / poly-sinc-ext2（10 项）

> **ext2 系列共同点【原文】**：扩展频响（extended frequency response）、陡滚降、高阻带衰减、**在 Nyquist 频率前完全截止**（completely cutting off by Nyquist frequency）、频响与谐波结构最优。**所有 ext2 成员在 SDM 输出时均为两段处理，中间速率至少 16x**。

| # | 滤波器 | 相位 | 滚降/衰减 | 比率 | Apod | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|---|---|
| 20 | **poly-sinc-ext** | 线性 | 更陡滚降，阻带衰减**略低** | Integer | ½ | 3/5 | （原表空缺） | 线性相位多相 sinc，**滚降比 poly-sinc 更陡、阻带衰减略低**，长度与 poly-sinc 大致相当 | 1161–1166 |
| 21 | **poly-sinc-ext2** | 线性 | 陡 / 高 | Any | **Y** | **5/5**（Timbre） | Any | 线性相位多相 sinc，扩展频响 + 陡滚降 + 高阻带衰减，Nyquist 前完全截止；频响与谐波结构最优 | 1177–1189 |
| 22 | **poly-sinc-ext2-short** | 线性 | **慢**滚降 / 高 | Integer up | ½ | 4/5（Timbre） | Pop, rock | 慢滚降 + 高阻带 + 扩展频响 | 1190–1205 |
| 23 | **poly-sinc-ext2-medium** | 线性 | **快**滚降 / 高 | Any | **Y** | 4/5（Timbre） | Any | 快滚降 + 高阻带 + Nyquist 前完全截止 | 1206–1220 |
| 24 | **poly-sinc-ext2-long** | 线性 | **极快** / **极高** | Any | **Y** | **5/5**（Timbre） | Any | 极快滚降、极高阻带衰减 | 1221–1236 |
| 25 | **poly-sinc-ext2-xla** | 线性 | 极陡 | Any | **Y** | **5/5**（Timbre） | Classical | 「Very steep **8 times longer** version of poly-sinc-ext2-long」 | 1247–1256 |
| 26 | **poly-sinc-ext2-xl** | 线性 | 极陡 | Any | **N** | **5/5**（Timbre） | Classical | ext2-long 的 **8 倍长非 apodizing 版**；仅适合最高技术质量素材 | 1257–1269 |
| 27 | **poly-sinc-ext2-hires-lp** | 线性 | 极高阻带 | Any | **Y** | **5/5**（Timbre） | Any | 面向 **HiRes 内容**；也适合播放 **MP3 / MQA** 等有损压缩 | 1270–1280 |
| 28 | **poly-sinc-ext2-hires-ip** | **中间相位** | 极高阻带 | Any | **Y** | **5/5**（Timbre） | Any | 同上，中间相位版 | 1281–1291 |
| 29 | **poly-sinc-ext2-hires-mp** | **最小相位** | 极高阻带 | Any | **Y** | **5/5**（Timbre） | Any | 同上，最小相位版 | 1292–1302 |

### 3.8 E 家族：poly-sinc-mqa/mp3（2 项）

| # | 滤波器 | 相位 | 比率 | Apod | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|---|
| 30 | **poly-sinc-mqa/mp3-lp** | 线性 | PCM: **Integer up**；SDM: **Any** | **Y** | 4/5（Transients） | Classical, jazz, blues | 为播放 **MQA 或 MP3 编码内容**优化，用于**清理编码过程中加入的高频噪声**；也适合 **≥88.2 kHz** PCM 源升频，尤其 **≥176.4 kHz** 的 hires 录音；**极短振铃**、**早期慢滚降** | 1303–1327 |
| 31 | **poly-sinc-mqa/mp3-mp** | 最小 | PCM: Integer up；SDM: Any | **Y** | 4/5（Transients） | Pop, rock | poly-sinc-mqa 的最小相位变体 | 1338–1350 |

---
### 3.9 F 家族：poly-sinc-xtr（4 项）

| # | 滤波器 | 相位 | 长度 | 比率 | Apod | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|---|---|
| 32 | **poly-sinc-xtr-lp** | 线性 | 长 | Any | **½** | **5/5**（Timbre） | Classical | 「Linear phase polyphase sinc filter with **extreme roll-off and attenuation**.」极端滚降与衰减 | 1351–1356 |
| 33 | **poly-sinc-xtr-mp** | 最小 | 长 | Any | **½** | **5/5**（Timbre） | Jazz, blues | 最小相位多相 sinc，极端滚降与衰减 | 1357–1366 |
| 34 | **poly-sinc-xtr-short-lp** | 线性 | **短** | Any | **Y** | **5/5**（Timbre, transients） | Electronic, jazz, blues, pop, rock | 短版线性相位多相 sinc，极端滚降与衰减 | 1367–1380 |
| 35 | **poly-sinc-xtr-short-mp** | 最小 | **短** | Any | **Y** | **5/5**（Timbre, transients） | Pop, rock | 短版最小相位多相 sinc，极端滚降与衰减 | 1381–1390 |

### 3.10 G 家族：poly-sinc-gauss（10 项）

> **共同点【原文】**：「Optimal time-frequency response」（时间-频率响应最优）。除 hires 三兄弟分别标注线性/中间/最小相位外，**gauss-short / medium / long / xla / xl 未在表中标注相位**。**gauss-short / medium / long / xla / xl 在 SDM 输出时均为两段处理，中间速率至少 16x**。

| # | 滤波器 | 相位 | 长度 | 比率 | Apod | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|---|---|
| 36 | **poly-sinc-gauss-short** | 未标注 | 短 | **Integer up** | **½** | 3/5（Transients） | Electronic, jazz, blues, pop, rock | 短高斯多相 sinc；时间-频率响应最优；SDM 两段 ≥16x | 1391–1407 |
| 37 | **poly-sinc-gauss-medium** | 未标注 | 中 | Any | **Y** | 4/5（Transients, timbre） | Any | 高斯多相 sinc；时间-频率响应最优；SDM 两段 ≥16x | 1408–1419 |
| 38 | **poly-sinc-gauss-long** | 未标注 | 长 | Any | **Y** | **5/5**（Transients, timbre, space） | Any | **Long** 高斯多相 sinc，**极高衰减**；时间-频率响应最优；SDM 两段 ≥16x | 1420–1434 |
| 39 | **poly-sinc-gauss-xla** | 未标注 | 超长（apodizing extra long） | Any | **Y** | **5/5** | Classical, jazz, blues | **Apodizing extra long** 高斯多相 sinc，极高衰减；SDM 两段 ≥16x | 1445–1461 |
| 40 | **poly-sinc-gauss-xl** | 未标注 | 超长（extra long） | Any | **N** | **5/5** | Classical, jazz, blues | **Extra long** 高斯多相 sinc，极高衰减；SDM 两段 ≥16x | 1462–1478 |
| 41 | **poly-sinc-gauss-hires-lp** | **线性** | 长 | Any | **Y** | **5/5** | Any | 面向 HiRes 的线性相位高斯滤波器，极高衰减；也适合 MP3 / MQA | 1479–1492 |
| 42 | **poly-sinc-gauss-hires-ip** | **中间相位** | 长 | Any | **Y** | **5/5** | Any | 同上，中间相位 | 1493–1506 |
| 43 | **poly-sinc-gauss-hires-mp** | **最小相位** | 长 | Any | **Y** | **5/5** | Any | 同上，最小相位 | 1507–1521 |
| 44 | **poly-sinc-gauss-halfband** | **线性** | 长 | Any | **N** | 4/5（Transients, timbre, space） | Any | 线性相位**半带**高斯；**Nyquist 附近略有泄漏（slightly leaky）**，但**衰减极高**；仅适合最高技术质量素材 | 1522–1536 |
| 45 | **poly-sinc-gauss-halfband-s** | **线性** | 短 | Any | **N** | 3/5（Transients, timbre, space） | Any | 短线性相位半带高斯；**Nyquist 附近有泄漏**，高衰减；仅适合最高技术质量素材 | 1547–1560 |

### 3.11 H 家族：特殊 / 低振铃型（5 项）

| # | 滤波器 | 相位 | 长度 | 比率 | Apod | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|---|---|
| 46 | **ASRC** | 异步 | — | Any | N | 2/5 | Any | 特殊类型，**略似 FIR**，但可**异步**从任意速率转到任意速率；**计算量大，不推荐** | 1561–1568 |
| 47 | **polynomial-1** | 无显式相位（多项式） | 极短 | Integer up | N | 1/5 | — | 多项式插值；**无明显前后振铃**；**顶部一个八度频响缓慢滚降**；**阻带抑制差**，会泄漏相当多超声波失真；厂家有时称之为「**non-ringing**」；**不推荐** | 1569–1582 |
| 48 | **polynomial-2** | 多项式 | 极短 | Integer up | N | 1/5 | — | 类似 polynomial-1，但**阻带抑制更高**，且**只有 1 个周期的前后振铃**；不推荐 | 1583–1589 |
| 49 | **minringFIR-lp** | **线性** | 短 | Integer up | N | 2/5（Transients） | — | 「Minimum ringing FIR」；用特殊算法生成**最小化振铃**的线性相位滤波器，频响与衰减**优于多项式插值**；性能与振铃**介于 polynomial 与 poly-sinc-short 之间** | 1590–1604 |
| 50 | **minringFIR-mp** | 最小 | 短 | Integer up | N | 2/5（Transients） | — | minringFIR 的最小相位变体 | 1605–1612 |

### 3.12 I 家族：closed-form（4 项）

> 全部 **Ratio = 2x up**、**Apod = N**；手册**未标注相位**。

| # | 滤波器 | 抽头/精度 | 质量 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|
| 51 | **closed-form** | 「high number of taps」 | 3/5 | 闭式插值，高抽头数 | 1613–1615 |
| 52 | **closed-form-fast** | 更低 CPU、更低精度；**输出精度约相当于 24-bit PCM** | 2/5 | 闭式插值，CPU 负载更低但精度也更低；输出精度调校到约 24-bit PCM | 1626–1632 |
| 53 | **closed-form-M** | **1,000,000 taps** | 3/5 | 闭式插值，**一百万抽头** | 1633–1635 |
| 54 | **closed-form-16M** | **16,000,000 taps** | 3/5 | 闭式插值，**一千六百万抽头** | 1636–1640 |

### 3.13 J 家族：sinc 系列（14 项）

> **抽头数语义**：凡写「N × conversion ratio」者为**自适应抽头数**（随转换比线性增长，保持**时间长度恒定**）；写「one million taps」者为**固定抽头数**（时间长度随比率变化）。

| # | 滤波器 | 相位 | 抽头/长度 | 比率 | Apod | 质量 | 流派 | 手册要点【原文】 | 行号 |
|---|---|---|---|---|---|---|---|---|---|
| 55 | **sinc-S** | 线性 | **4096 × 转换比**（自适应） | Any | **Y** | 4/5（Space, timbre） | Any | 极陡滚降、高衰减；**poly-sinc-ext2-xla 的变体** | 1641–1649 |
| 56 | **sinc-M** | 线性 | **1,000,000 taps** | 2x up | **Y** | 4/5（Space, timbre） | Classical, jazz, blues | 极陡滚降、高衰减；ext2-xla 变体 | 1650–1659 |
| 57 | **sinc-Mx** | 线性 | **恒定时间**：16x PCM 输出率时百万抽头（**65536 × 转换比**） | 2x up | **Y** | 4/5（Space, timbre） | Classical, jazz, blues | sinc-M 的**恒定时间版**；滤波器长度在时间上恒定；ext2-xla 变体 | 1660–1671 |
| 58 | **sinc-MG** | 线性（高斯类） | 恒定时间：16x 时百万抽头（**65536 × 转换比**） | 2x up | **N** | 4/5（Transients, timbre, space） | Classical, jazz, blues | **高斯恒定时间**滤波器，16x PCM 时百万抽头，**极高衰减**；**poly-sinc-gauss-xl 的变体** | 1672–1684 |
| 59 | **sinc-MGa** | 线性（高斯类） | 恒定时间：**65536 × 转换比** | 2x up | **Y** | 4/5（Transients, timbre, space） | Classical, jazz, blues | **Apodizing 高斯恒定时间**滤波器，百万抽头@16x，极高衰减；**gauss-xla 的变体** | 1685–1698 |
| 60 | **sinc-L** | 线性 | **131070 × 转换比**（自适应） | 2x up | **N** | 3/5 | Classical | **极陡**滚降，但**平均衰减** | 1699–1703 |
| 61 | **sinc-Ls** | 线性 | **4096 × 转换比** | 2x up | **N** | 2/5 | Any | 平均衰减 sinc，自适应抽头 | 1704–1707 |
| 62 | **sinc-Lm** | 线性 | **16384 × 转换比** | 2x up | **N** | 2/5 | Classical, jazz, blues | 平均衰减 sinc，自适应抽头 | 1718–1723 |
| 63 | **sinc-Ll** | 线性 | **65536 × 转换比** | 2x up | **N** | 3/5 | Classical | 平均衰减 sinc，自适应抽头 | 1724–1727 |
| 64 | **sinc-Lh** | 线性 | **16384 × 转换比** | 2x up | **N** | 4/5 | Classical, jazz, blues | **高衰减** sinc，自适应抽头；**在 1/8 负载下显著优于 sinc-L** | 1728–1734 |
| 65 | **sinc-short** | 线性 | 自适应（未给系数） | Any | **N** | 2/5 | Any | 短、平均衰减 sinc；**SDM 输出两段 ≥16x** | 1735–1740 |
| 66 | **sinc-medium** | 线性 | 自适应 | Any | **N** | 2/5 | Classical, jazz, blues | 平均衰减 sinc；SDM 两段 ≥16x | 1741–1748 |
| 67 | **sinc-long** | 线性 | 自适应 | Any | **N** | 3/5 | Classical | 长、平均衰减 sinc；SDM 两段 ≥16x | 1749–1754 |
| 68 | **sinc-long-h** | 线性 | 自适应 | Any | **N** | 4/5 | Classical, jazz, blues | 长、**高衰减** sinc；SDM 两段 ≥16x | 1755–1762 |

### 3.14 K：\*-2s 两段过采样修饰（1 项）

| # | 名称 | 手册要点【原文】 | 质量 | Apod | 行号 |
|---|---|---|---|---|---|
| 69 | **\*-2s**（后缀，可加在多种基滤波器后） | 「Two stage oversampling. First stage rate conversion is performed by **at least by factor of 8** using the selected algorithm. And further converted to the final rate using algorithm **optimized for conversion of content that has already been processed to at least 8x rate**. This **lowers the overall CPU load**, while **preserving the same conversion quality**. Especially useful for highest output rates.」 | **Same as the base filter** | 原表显示为 **O**（排版残缺，无法判读；按同族逻辑应为 N 或随基滤波器） | 1773–1790 |

**工程含义【推断】**：\*-2s = 第一级用「基滤波器算法」升到 ≥8x 的中间速率，第二级换用一个专门为「已升到 8x 以上的内容」优化的轻量算法完成到最终速率。两级串联，第一级可用长系数保证质量，第二级只需处理窄过渡带，所以总 CPU 显著下降，而手册声称质量不变。适用于 PCM→DSD 这类超高比率（如 44.1k → 11.29M，256 倍）。

### 3.15 全表统计与关键取舍

| 维度 | 结论（依据手册表格） |
|---|---|
| 成员最多的家族 | **sinc（14）**，其次 **ext2（10）**、**gauss（10）** |
| 相位分布 | **最小相位**：minphaseFIR、poly-sinc-mp、poly-sinc-shrt-mp、poly-sinc-long-mp、poly-sinc-xtr-mp、poly-sinc-xtr-short-mp、poly-sinc-ext2-hires-mp、poly-sinc-gauss-hires-mp、poly-sinc-mqa/mp3-mp、minringFIR-mp。**中间相位**：poly-sinc-long-ip、poly-sinc-ext2-hires-ip、poly-sinc-gauss-hires-ip。**非对称**：asymFIR。**其余多为线性相位**（gauss 非 hires 成员手册未标注） |
| 5/5 质量成员 | poly-sinc-ext2、ext2-long、ext2-xla、ext2-xl、ext2-hires-lp/ip/mp、xtr 全系、gauss-long、gauss-xla、gauss-xl、gauss-hires-lp/ip/mp |
| 明确「不推荐」 | none（滤波器与抖动均标注）、ASRC、polynomial-1、polynomial-2 |
| 明确「Recommended」/作者首推 | 滤波器表本身**没有** Recommended 列；作者首推 **poly-sinc 全系**（第 918 行）。DSD→PCM 侧明确标注 Recommended 的有：**standard、low、high-order、wec2、medium、medium-high、poly-short-lp** |
| 明确「仅适合最高技术质量素材」 | poly-sinc-hb 全系、ext2-xl、gauss-halfband 两兄弟 |
| 抽头数最大 | closed-form-16M（16,000,000）、sinc-L（131070×比率）、sinc-MG/MGa/Mx（65536×比率，16x 时 1M） |

### 3.16 振铃 / apodizing 速查

**A. 预振铃 / 后振铃（严格按手册文字汇总）**

| 成员 | 预振铃 | 后振铃 | 手册依据 |
|---|---|---|---|
| IIR、IIR2 | **无** | **长** | 「no pre-ringing, but has a long post-ringing」 |
| FIR | 有（平均量） | 有（平均量） | 「average amount of pre- and post-ringing」 |
| asymFIR | **短** | **长** | 「shorter pre-ringing and longer post-ringing」 |
| minphaseFIR | **无** | 偏长 | 「No pre-ringing, but somewhat long post-ringing」 |
| FFT | 有（副作用） | 长脉冲响应 | 「might have some side effects (pre-ringing)」「rather long impulse response」 |
| poly-sinc-lp | 有（一定量） | 对称 | 「some amount of pre-ringing」 |
| poly-sinc-mp | **无** | 有 | 「no pre-ringing」 |
| poly-sinc-shrt-lp | **更短** | **更短** | 「shorter pre- and post-ringings」 |
| poly-sinc-shrt-mp | **无** | **最短** | 「shorter post-ringing」「Most optimal transient reproduction」 |
| poly-sinc-long-lp | **更长** | **更长** | 「longer pre- and post-ringings」 |
| poly-sinc-long-ip | **小** | **较长** | 「small pre-ringing and longer post-ringing」 |
| poly-sinc-long-mp | **无** | **更长** | 「longer post-ringing」 |
| polynomial-1 | **无明显** | **无明显** | 「No apparent pre- or post-ringing」 |
| polynomial-2 | **1 个周期** | **1 个周期** | 「only one cycle of pre- and post-ringing」 |
| minringFIR-lp / -mp | **最小化** | 最小化 | 「minimizes amount of ringing」 |
| mqa/mp3 | **极短** | 极短 | 「Very short ringing. Early slow roll-off.」 |
| 其余（ext2、gauss、xtr、sinc、hb、closed-form） | 手册未逐项描述 | 未逐项描述 | 线性相位成员理论上预/后振铃对称【推断】 |

**B. apodizing（Apod 列）汇总**

| Apod | 成员 |
|---|---|
| **Y（apodizing）** | IIR、IIR2、FIR、asymFIR、minphaseFIR、FFT、poly-sinc-long-lp、poly-sinc-long-ip、poly-sinc-long-mp、poly-sinc-ext2、poly-sinc-ext2-medium、poly-sinc-ext2-long、poly-sinc-ext2-xla、poly-sinc-ext2-hires-lp/ip/mp、poly-sinc-mqa/mp3-lp、poly-sinc-mqa/mp3-mp、poly-sinc-xtr-short-lp、poly-sinc-xtr-short-mp、poly-sinc-gauss-medium、poly-sinc-gauss-long、poly-sinc-gauss-xla、poly-sinc-gauss-hires-lp/ip/mp、sinc-S、sinc-M、sinc-Mx、sinc-MGa |
| **½（部分，含义未定义）** | poly-sinc-lp、poly-sinc-mp、poly-sinc-shrt-lp、poly-sinc-shrt-mp、poly-sinc-ext、poly-sinc-ext2-short、poly-sinc-xtr-lp、poly-sinc-xtr-mp、poly-sinc-gauss-short |
| **N（非 apodizing）** | none、poly-sinc-hb 全系、poly-sinc-ext2-xl、poly-sinc-gauss-xl、poly-sinc-gauss-halfband、poly-sinc-gauss-halfband-s、ASRC、polynomial-1、polynomial-2、minringFIR-lp、minringFIR-mp、closed-form 全系、sinc-MG、sinc-L、sinc-Ls、sinc-Lm、sinc-Ll、sinc-Lh、sinc-short、sinc-medium、sinc-long、sinc-long-h |
| **未定** | \*-2s（原表残缺） |

**C. apodizing 使用规则（手册 2.6 + 4.6）**

- 检测依据：源内容中来自录音 ADC 或母带处理工具的**误差**；每次检测到计数 +1。
- 阈值：单轨 **Apod > 10** → 强烈建议 apodizing 滤波器（第 2306–2308 行：「figures higher than 10 per track … strongly recommended」）。
- 手册立场：「对不需要的素材使用 apodizing 无害；但对需要的素材使用非 apodizing 有害。」
- 判据不是绝对的，只作为「何时不该用非 apodizing」的指导。

---
## 4. 手册中出现的全部数值指标

> 本章按类别收录手册中**所有**出现过的数字，共 **80 条**（编号 1–80），每条标注出处行号。先说明「哪些数值手册其实没有给」，避免开发时误以为可以照抄：

| 数值类型 | 手册是否给出 | 说明 |
|---|---|---|
| 抽头数 / 滤波器长度 | **部分给出** | 仅 sinc 系列、closed-form、DSD→PCM 的 sinc-S/sinc-M 给了具体数字；poly-sinc、ext2、gauss 等**未给抽头数** |
| 阻带衰减（dB） | **未给出** | 只有定性等级：low / average / medium / high / very high / extremely high |
| -3 dB 带宽 | **仅积分器给出** | IIR/IIR2/IIR3/FIR2/FIR-bl/FIR-bw 以 DSD64 为基准给了 50/25/30/50/24/21.5 kHz |
| 过渡带宽度（Hz） | **未给出** | 只有 steep / slow / fast / extreme 等定性词 |
| 群延迟 | **未给出** | 只有「预振铃/后振铃」的定性描述 |
| 最高 PCM 速率 | **给出** | 1536 kHz（不是「1.5M」写法） |
| 最高 DSD 速率 | **给出** | DSD1024（并由 brickwall 截止 400 kHz 佐证） |
| 调制器速率门槛 | **给出** | 10.24 / 20.48 / 40.96 MHz、512x |
| 质量评分 | **给出** | 1/5 – 5/5，且明确为相对值 |

### 4.1 采样率与速率上限

| # | 指标 | 数值 | 出处行号 | 说明 |
|---|---|---|---|---|
| 1 | PCM 播放速率范围 | **32 – 1536 kHz** | 123 | 原文「playback rates from 32 to 1536 kHz are possible」；手册**未使用 1.5M 写法** |
| 2 | DSD 速率档位 | **DSD64 / DSD128 / DSD256 / DSD512 / DSD1024** | 618–620 | 由 brickwall 截止频率描述隐含 |
| 3 | DSD64 → PCM 转换输出 | **176.4（64fs）/ 352.8（128fs）/ 705.6（256fs）kHz** | 120 | 仅 PCM 输入设备时 |
| 4 | none 滤波器下 DSD 源输出率 | **DSD 速率的 1/16** | 500–502 | 例：DSD64 → 176.4 kHz |
| 5 | DSD 源典型余量 | **6 dB** | 667–668 | 由「6 dB gain」复选框消除 |
| 6 | 输出通道数 | **2 – 128** | 407–409 | Outputs → Channels |
| 7 | 矩阵虚拟通道（pipelines）上限 | **128** | 1962–1963 | 实际数量由 Advanced → DSP pipelines 配置 |

### 4.2 抖动 / 噪声整形的速率推荐（离散数值）

| # | 项 | 数值 | 行号 |
|---|---|---|---|
| 8 | NS1 适用 | **176.4 / 192 kHz** | 520–521 |
| 9 | NS4 适用 | **≥ 88.2 kHz** | 522–523 |
| 10 | NS5 适用 | **352.8 / 384 / 705.6 / 768 kHz**；≥192 kHz | 524–527 |
| 11 | NS9 适用 | **176.4 / 192 kHz（4x）** | 528–531 |
| 12 | LNS15 适用 | **705.6 / 768 kHz**；可用 **352.8 / 384 kHz** | 532–535 |
| 13 | TPDF 推荐 | **44.1 / 48 kHz** | 539–541 |
| 14 | Gauss1 推荐 | **≤ 96 kHz** | 542–544 |
| 15 | shaped 适用 | **88.2 / 96 kHz 或更高** | 545–547 |
| 16 | RPDF 最低位深 | **24-bit 或更高** | 536–538 |

### 4.3 DAC bits 与电平补偿

| # | 项 | 数值 | 行号 |
|---|---|---|---|
| 17 | Holo Audio（Cyan 2 / Spring / Spring 2 / Spring 3 / May） | **20 bits** | 564 |
| 18 | Denafrips | **19 bits** | 565 |
| 19 | LAiV Harmony DAC / Harmony µDAC | **18 bits** | 566 |
| 20 | PCM 增益补偿：ESS Sabre | **0 dB** | 458 |
| 21 | Merging Hapi | **-0.6 dB** | 471 |
| 22 | Cirrus Logic | **-3 dB** | 457 |
| 23 | AKM AK4490 | **-3.5 dB** | 451 |
| 24 | AKM AK4493 | **-1 到 -3.5 dB**（随参考电平设置） | 452–453 |
| 25 | AKM AK4499 | **-4.1 dB** | 454 |
| 26 | AKM AK4499EX | **-3 dB**（随设置） | 455–456 |
| 27 | Holo Audio | **-6 dB** | 469 |
| 28 | Denafrips | **-3.2 dB** | 470 |
| 29 | ROHM BD34352 | **-5.25 dB** | 459 |
| 30 | TI / Burr-Brown | **取决于所选 AFIR**（查数据手册） | 460–462 |

### 4.4 音量与调制深度

| # | 项 | 数值 | 行号 |
|---|---|---|---|
| 31 | 有重采样时推荐最大软件音量 | **-3 dBFS** | 255–256 |
| 32 | PCM→SDM 时推荐软件音量 | **≤ -3 dB** | 257–260 |
| 33 | 最大调制深度限制 | **50%** | 261–262 |
| 34 | Fixed volume 勾选 | 约 **-3 dB** | 443–444 |
| 35 | Fixed volume 未勾选 | 约 **-6 dB** | 443–446 |
| 36 | DirectSDM 下 PCM 音量 | 固定 **-3 dBFS** | 913–914 |

### 4.5 调制器速率门槛

| # | 项 | 数值 | 行号 |
|---|---|---|---|
| 37 | DSD5v2 256+fs、DSD7 256+fs | **≥ 10.24 MHz** | 688, 756 |
| 38 | ASDM*EC-* 512+fs 全系 | **512x 及以上** | 730–753, 793–816 |
| 39 | AMSDM7 512+fs / AMSDM7EC 512+fs | **≥ 20.48 MHz** | 820, 826 |
| 40 | AHM 全系（5L / 8B） | **≥ 40.96 MHz** | 830, 839, 847, 852 |

### 4.6 抽头数 / 滤波器长度

| # | 项 | 数值 | 行号 |
|---|---|---|---|
| 41 | DSD→PCM 的 sinc-S | **65536 taps** | 634 |
| 42 | DSD→PCM 的 sinc-M | **1,000,000 taps**（million-tap） | 635 |
| 43 | poly-sinc-ext2-xla / ext2-xl | 分别为 ext2-long 的 **8 倍长** | 1249, 1259 |
| 44 | sinc-S（滤波器表） | **4096 × 转换比** | 1642–1643 |
| 45 | sinc-M | **1,000,000 taps** | 1650 |
| 46 | sinc-Mx / sinc-MG / sinc-MGa | **65536 × 转换比**；16x 输出率时 **1,000,000 taps** | 1664–1665, 1676, 1689–1690 |
| 47 | sinc-L | **131070 × 转换比** | 1700–1701 |
| 48 | sinc-Ls | **4096 × 转换比** | 1705–1706 |
| 49 | sinc-Lm / sinc-Lh | **16384 × 转换比** | 1719, 1729 |
| 50 | sinc-Ll | **65536 × 转换比** | 1725 |
| 51 | closed-form-M | **1,000,000 taps** | 1634 |
| 52 | closed-form-16M | **16,000,000 taps** | 1638–1639 |
| 53 | \*-2s 第一级最小倍率 | **≥ 8x** | 1775–1776 |
| 54 | ext2 / gauss / sinc-short/medium/long/long-h 的 SDM 两段中间速率 | **≥ 16x** | 1185–1186, 1198–1200, 1215–1217, 1231–1233, 1251–1253, 1264–1266, 1396–1397, 1413–1414, 1426–1428, 1451–1453, 1468–1470, 1737–1739, 1743–1745, 1751–1753, 1757–1759 |

### 4.7 滤波器频率指标（截止 / 带宽 / 衰减）

| # | 项 | 数值 | 行号 |
|---|---|---|---|
| 55 | DSD→PCM brickwall 截止：DSD64 | **25 kHz** | 618 |
| 56 | 同上：DSD128 | **50 kHz** | 618–619 |
| 57 | 同上：DSD256 | **100 kHz** | 619 |
| 58 | 同上：DSD512 | **200 kHz** | 619–620 |
| 59 | 同上：DSD1024 | **400 kHz** | 620 |
| 60 | 积分器 IIR 带宽（re DSD64） | **50 kHz** | 876–877 |
| 61 | 积分器 IIR2 | **25 kHz** | 878–880 |
| 62 | 积分器 IIR3 | **30 kHz** | 881–882 |
| 63 | 积分器 FIR2 | **50 kHz** | 884–885 |
| 64 | 积分器 FIR-bl | **24 kHz**，**45 kHz 完全截止** | 886–888 |
| 65 | 积分器 FIR-bw | **21.5 kHz**，**30 kHz 完全截止** | 889–891 |
| 66 | 20 kHz 滤波器 | 20 kHz 陡截止；仅对 **2x 及以上**源率生效 | 209–214 |
| 67 | 播放滤波器档位 | **20k / 30k / 40k / 50k / 2x / 4x / 8x** | 2209–2224 |
| 68 | Apodizing 判定阈值 | 单轨 **Apod > 10** | 921–922, 2306–2308 |

### 4.8 其他工程数值

| # | 项 | 数值 | 行号 |
|---|---|---|---|
| 69 | FFT 滤波器长度默认值 | **512**（每个 2x 级联滤波器独立设置，与转换比无关） | 1851–1855 |
| 70 | 硬件缓冲推荐范围 | **10 – 100 ms** | 386–387 |
| 71 | WASAPI 默认缓冲 | 约 **10 ms** | 375 |
| 72 | USB 音频设备默认缓冲 | **10 ms**；问题时可试 **100 ms** | 2363–2366 |
| 73 | CUDA 最低 Compute Capability | **5.2** | 1820–1821 |
| 74 | CUDA 最低显存 | **2 GB** | 1821 |
| 75 | 卷积 IR 推荐采样率 | **352.8 kHz**（扩展频响） | 1943–1945, 2008–2010 |
| 76 | RIAA 插件推荐输入率 | **≥ 192 kHz**；最低 **96 kHz** | 2101–2103 |
| 77 | 插件 delay 默认声速 | **343.956 m/s** | 2059 |
| 78 | Matrix 示例 EQ | 200 Hz 以上 **-3 dB** | 1968–1970 |
| 79 | 输出通道映射 | **0–7**：FL / FR / FC / LFE / BL / BR / SL / SR | 363–371 |
| 80 | 校准距离单位 | **厘米** | 1882 |

---

## 5. 4.7 Advanced 高级设置（第 1791–1872 行）

### 5.1 总述【原文】

「In the Advanced tab, various advanced settings such as hardware related optimizations can be adjusted.」——高级页签用于硬件相关优化等高级设置。

### 5.2 全部 11 项可调项

| # | 设置项 | 类型 | 手册描述【原文】 | 工程要点 | 行号 |
|---|---|---|---|---|---|
| 1 | **Multicore DSP** | 三态：自动（灰）／关／开 | 增加各类 DSP 运算的并行度。灰显＝自动检测与配置，可用任意核数，**推荐自动**。清空＝针对**核数 ≤ 输出通道数**的场合优化（如双核 CPU 输出立体声）。勾选＝针对**核数远多于输出通道数**的现代多核 CPU 优化；并行度提高会增加开销与总 CPU 时间；若 auto 有性能问题，通常值得试此项 | 三态开关，不是布尔 | 1801–1812 |
| 2 | **E-core allocation** | 选择 | 在同时具备 P-core 与 E-core 的新 CPU 上，可把**能效核（E-core）**分配为 offload 处理器，而不是默认的普通使用；可分配给**重采样滤波器**，或作为**通用 DSP 池**执行卷积等其他任务 | 线程亲和性/工作队列划分 | 1813–1817 |
| 3 | **CUDA offload** | 复选框 + GPU 选择 | 用 nVIDIA GPU 把部分处理从 CPU 卸载到 GPU。要求 **CC ≥ 5.2、≥2 GB 显存、最新官方驱动**。启用且 GPU 可用时，每轨开始播放会短暂显示 offload 信息。启用 CUDA 时也应启用 Multicore DSP 或保持自动。**灰显时只有卷积算法被卸载到 GPU**。可为「滤波器」与「其他 DSP/卷积」分别选择同一块或不同的 GPU，从而把负载分到两块 GPU | 两级卸载粒度 | 1818–1829 |
| 4 | **DSP pipelines** | 整数 | 指定可用 DSP 管线数量；既是**矩阵管线数**，也是**输入/输出通道总数上限**。取合适的小值可在一定程度上降低资源消耗（如内存占用） | 与 128 通道上限配合 | 1830–1833 |
| 5 | **Blocks per cycle** | Default / 整数 | 一次处理的 block 数。用于把 CPU/GPU 负载调到最低。选 **Default** 时依据检测到的 CPU cache 等自动配置。一次处理更多 block 可**降低开销**（尤其用 GPU 时）；一次处理更少 block 有利于**把数据保持在 CPU cache** 中。大缓存 CPU（AMD 3D 系列、部分 Intel Xeon）或高速内存系统适合**更大值**；小缓存或慢内存适合**更小值** | 批处理 vs 缓存局部性 | 1834–1842 |
| 6 | **Idle time** | 时间 | 当前内容播放结束后引擎保持 idle 的时长；在 idle 期内可**更快重启播放** | 保活线程/设备不关闭 | 1848–1850 |
| 7 | **FFT filter length** | 整数，默认 **512** | 指定 **FFT 滤波器**长度。长度影响滤波器陡峭度：**更短→更慢（更缓）滚降**，**更长→更快（更陡）滚降**。该设置是**对每个 2x 级联滤波器**生效，因此**与转换比无关** | 只影响 FFT 型滤波器 | 1851–1855 |
| 8 | **Pre-process before metering** | 复选框 | 启用后在**电平表/频谱显示之前**先做预处理（如 20 kHz 滤波器），便于观察预处理效果；但可能更难判断何时该关掉 20 kHz 滤波器 | 仅影响显示链路 | 1856–1859 |
| 9 | **Playlist album gain** | 复选框 | 启用后播放列表项使用**专辑增益**而非单曲增益；对含多张完整专辑的播放列表更合适 | 与 2.9 自适应增益配合 | 1860–1862 |
| 10 | **UPnP freewheel** | 复选框 | 对 UPnP 控制的源，可启用 freewheel：在已知曲目大小时，以全网络速度把整轨（预）取到内存。会在抓取时造成资源占用尖峰，也有内存占用影响 | 缓冲策略 | 1863–1867 |
| 11 | **Log file** | 复选框 | 排障用日志。修改后需**重启 HQPlayer** 才完全生效；日志可通过 **Tools 菜单**查看/导出 | 日志路径见 10.1：Windows 为 %LOCALAPPDATA%\HQPlayer，Linux/macOS 为 ~/.hqplayer，文件名 **HQPlayer5Desktop.log** | 1868–1872 |

### 5.3 用户提到的其余设置：实际归属页签

| 设置 | 实际位置 | 手册行号 | 说明 |
|---|---|---|---|
| **Buffer time** | **4.2 Outputs**（非 Advanced） | 372–387 | 硬件音频缓冲长度（毫秒）；建议 Driver default；WASAPI 约固定 10 ms；ASIO 建议在驱动控制面板调；推荐 **10–100 ms** |
| **DAC bits** | **4.4 PCM**（非 Advanced） | 549–566 | 见 1.4 节 |
| **Idle time** | **4.7 Advanced** | 1848–1850 | 确在 Advanced |
| **音量范围（min/max）/ Fixed volume** | **4.2 Outputs** | 434–446 | 最小/最大都设 0＝完全旁路音量（不推荐）；Fixed volume 勾选约 -3 dB、不勾约 -6 dB |
| **“高分辨率音量”（high-resolution volume）** | **本手册不存在此项** | — | 全文无 “high-resolution volume” / “高解析音量” 字样；最接近的是 2.15 音量控制与 4.2 的 Fixed volume。若目标软件版本确有该选项，需以该版本手册为准 |

### 5.4 Advanced 相关但不在 Advanced 页的硬件设置（补充对齐）

| 项 | 位置 | 数值/说明 | 行号 |
|---|---|---|---|
| Short buffer | 4.2 Outputs | FIFO 减半，降低延迟但更易断音 | 360–362 |
| DoP | 4.2 Outputs | DoP v1.1 封装 DSD | 356–359 |
| 2wire | 4.2 Outputs | 双线绑定，PCM 与 DoP DSD 均可 2x 速率 | 357–359 |
| Adaptive rate | 4.2 Outputs | 灰＝默认或较低速率；勾＝与源同基频的整数倍速率 | 429–433 |
| Quick pause | 4.2 Outputs | 用基本静音图案暂停，降低延迟但可能有爆音 | 424–428 |
| Channels | 4.2 Outputs | 2–128 | 407–409 |

---
## 6. 算法原理的工程可实现解读【推断】

> 本章**不是手册内容**，而是把手册描述映射到可落地的 DSP 实现方案，供后续开发选型。凡与手册冲突处，以手册为准。

### 6.1 总览：家族 → 可实现结构 → 算力路径

| 家族 | 可落地结构 | 是否必须 FFT | 实现难度 | 备注 |
|---|---|---|---|---|
| poly-sinc-*（B/C） | 窗函数 sinc + 多相分解 | 否（短/中）/ 是（长、高倍率） | 中 | 标准教科书结构，Kaiser 窗即可复刻 |
| ext / ext2（D） | 窗函数 sinc（不同窗/过渡带）+ 多相 | 长版本需要 | 中 | 「Nyquist 前完全截止」＝过渡带终点 ≤ Nyquist |
| gauss（G） | **高斯窗** sinc + 多相 | 长版本需要 | 中 | 「时间-频率最优」＝高斯窗的不确定性下界 |
| halfband（C/G 的 hb） | 半带多相 + 2x 级联 | 否 | 低 | 一半系数为 0，算力减半 |
| xtr（F） | 超长窗函数 sinc（极端过渡带） | 是 | 中高 | 「extreme roll-off」＝过渡带极窄 |
| sinc 系列（J） | 超长窗函数 sinc，自适应抽头 | **是** | 高 | 百万抽头必须分块卷积 |
| closed-form（I） | 闭式插值核（非窗函数） | **是** | 高 | 16M 抽头只能 FFT |
| IIR / IIR2（A） | 双线性变换 + 二阶节级联 | 否 | 低 | 无预振铃、后振铃长 |
| FIR / asymFIR / minphaseFIR（A） | 窗函数 sinc / 最小相位变换 | 中等长度可直算 | 中 | 时域直接卷积 |
| FFT（A） | 频域分块卷积 | **是** | 中 | 长度可配，默认 512/2x 级 |
| ASRC（H） | Farrow 分数延迟 + 异步控制 | 否 | 中高 | 任意→任意，算力大 |
| polynomial（H） | Lagrange / Hermite 插值 | 否 | 低 | 阻带差，仅作低振铃备选 |
| minringFIR（H） | 最小振铃窗/优化 FIR | 否 | 中 | 介于 polynomial 与 poly-sinc-short |

### 6.2 窗函数 sinc：最直接可自研的一类（覆盖 B/C/D/F/G/J 大半）

**统一形式：**

    理想低通：h_ideal[n] = 2·fc·sinc(2·fc·(n − M/2))
    加窗：    h[n] = h_ideal[n] · w[n],  n = 0..M
    归一化：  h[n] /= Σ h[n]（保证 0 Hz 增益为 1）

其中 fc 为归一化截止频率（相对工作采样率），M 为阶数（抽头数 = M+1）。

**Kaiser 窗（推荐默认）：**

    w[n] = I0(β·sqrt(1 − (2n/M − 1)²)) / I0(β)

- 阻带衰减 A（dB）→ β：
  - A > 50 dB：β ≈ 0.1102·(A − 8.7)
  - 21 < A < 50 dB：β ≈ 0.5842·(A − 21)^0.4 + 0.07886·(A − 21)
- 阶数估算：M ≈ (A − 8) / (2.285·Δω)，Δω = 2π·Δf/fs（Δf 为过渡带宽）
- 实务对照：A=100 dB → β≈10.06；A=120 dB → β≈12.24；A=60 dB → β≈5.65

**手册家族与该结构的对应：**

| 手册家族 | 窗/核选择 | 过渡带 | 抽头策略 |
|---|---|---|---|
| poly-sinc-shrt | Kaiser，β 较小 | 宽 | 短（如 64–256/相） |
| poly-sinc（标准） | Kaiser，β 中等 | 中 | 中（如 256–1024/相） |
| poly-sinc-long / xtr | Kaiser，β 大 | 窄/极窄 | 长（数千–数万） |
| ext / ext2 | Kaiser 或 Blackman-Harris，截止贴近 Nyquist | 陡 | 中长；xla/xl 为 long 的 8 倍 |
| gauss | **高斯窗** w[n]=exp(−(n−M/2)²/(2σ²)) | 由 σ 决定 | σ 越大越长 |
| sinc-S/M/L 系 | Kaiser（极长） | 极窄 | 4096×比率起，最大 131070×比率 |
| halfband | 半带窗函数 sinc | 中 | 2x 级联，系数隔点为零 |

### 6.3 多相（polyphase）分解与任意比率

**整数升采样 L 倍：**

    y[m] = Σ_k h[k·L + p] · x[floor(m/L) − k],  p = m mod L

每个输出相位 p 只用 1/L 的系数，算力与「直接卷积后再抽取」相同但内存访问更优。

**任意比率（如 44.1k → 48k = 160/147）：**

1. **精确有理数多相**：L/M = 160/147，需要 160 个相位（或按 gcd 化简后的相位数），系数表可控。
2. **量化相位 + 插值**：把相位量化到 2^p（如 p=10，1024 相），相位间线性插值（Farrow）。精度约 −100 dB 量级时 p 取 10–12 足够。
3. **两级**：先 44.1→352.8（8x 整数），再 352.8→384（非整数），第二级过渡带极宽，可用短滤波器。**这正是手册 \*-2s 与「两段 ≥16x」策略的工程依据**。

**对应手册的 Ratio 列：**

| 手册标记 | 实现约束 |
|---|---|
| Any | 需支持非整数比 → 上述方案 1/2/3 |
| Integer | 只做整数 L，系数表按 L 分支 |
| Integer up | L ≥ 1 且只能升 |
| 2x up | L ≥ 2 且只能升 |
| 1:1 | 旁路（仅改位深） |

### 6.4 线性相位 / 最小相位 / 中间相位

| 类型 | 实现方法 | 群延迟 | 听感/指标 |
|---|---|---|---|
| **线性相位（lp）** | 对称系数 h[n]=h[M−n] | M/2 样本，恒定 | 预振铃 + 后振铃对称；相位失真为零 |
| **最小相位（mp）** | 对幅度响应做**倒谱法**：取对数幅度谱 → 实倒谱 → 单边折叠 → exp → IFFT；或解谱因式分解把零点全放单位圆内 | 能量集中在起始，延迟小 | **无预振铃**，后振铃长；相位失真大 |
| **中间相位（ip）** | 对倒谱做**部分折叠**（α∈(0,1) 混合最小/线性相位），或对称与非对称系数加权 | 介于两者 | 小预振铃 + 较长后振铃（对应 poly-sinc-long-ip 的描述） |
| **非对称（asymFIR）** | 窗中心偏移（如 1/3 处），或用非对称窗 | 介于 | 预振铃短、后振铃长，相位改动小于 mp |

**工程要点：** 最小相位变换会改变群延迟，必须同步补偿延迟，否则多通道/多滤波器切换时会出现时间对齐问题。

### 6.5 半带（halfband）滤波器

- 半带低通的冲激响应满足 h[2k]=0（k≠中心），因此**每两级抽头只有一半非零**。
- 2x 插值时，每个输出点只需一半乘法；级联 N 级即 2^N 倍，成本约为同指标普通 FIR 的 1/2。
- 手册的 poly-sinc-hb 全系、gauss-halfband 均属此类；「slightly leaky around Nyquist」＝过渡带跨越 Nyquist，混叠抑制不彻底。
- 实现建议：固定 2x 半带核 + 多级级联，最后一级用非半带核收尾（因为半带只能 2x）。

### 6.6 高斯滤波器

- 高斯窗在时频不确定性意义下最优（σ_t·σ_f 取最小），对应手册「optimal time-frequency response」。
- 时域无旁瓣振铃（单调衰减），因此**预振铃极小**；频域过渡带由 σ 决定，σ 越大过渡带越窄、时域越长。
- 实现：h[n] = sinc(2fc(n−M/2))·exp(−((n−M/2)/(2σ))²)，再做多相分解。
- 「extremely high attenuation」需要 σ 与 M 同时增大；gauss-long/xla/xl 即为此。

### 6.7 closed-form 插值

- 闭式（解析式）插值核，不使用窗函数截断，而是用解析核在有限点上采样；抽头数极大时逼近理想插值。
- 手册只允许 **2x up**，说明其核按 2x 设计（半带式或固定 2 相），不适合任意比。
- **16M 抽头不可能直接卷积**：必须 FFT 分块卷积，且系数表本身占内存（16M×4B ≈ 64 MB/相）。
- 「closed-form-fast 输出精度约 24-bit」＝系数或中间结果被量化到 24-bit 精度，可用定点/单精度加速。

### 6.8 IIR 与「模拟味」

- 双线性变换设计 Butterworth（无通带纹波，对应 IIR2「No passband ripple」）/ Chebyshev / Elliptic（更陡，对应 IIR「really steep」但有纹波）。
- 二阶节（biquad）级联，直接型 II 结构，时域逐样本递推。
- 无预振铃（因果），后振铃长且为非对称；相位非线性，群延迟随频率变化。
- 手册「IIR filter is applied in time domain」即逐样本递推，不是频域块处理。
- 注意：高斜率 IIR 在低频（如 20 Hz）需要很高 Q，定点实现易不稳定，建议双精度。

### 6.9 FFT 滤波器与分块卷积

**手册的 FFT 滤波器：** 频域实现、砖墙、长度可配（默认 512/2x 级）。

**长抽头（sinc-M/Mx/L、closed-form-M/16M）必须用分块卷积：**

| 方法 | 适用 | 延迟 | 说明 |
|---|---|---|---|
| overlap-add | 短 IR、实时 | 块长 | 手册卷积引擎默认，CPU 更低 |
| overlap-save | 短 IR | 块长 | 略高 CPU |
| **uniform partitioned** | 长 IR（万–百万抽头） | 一个块 | 把 IR 切成等长块，每块 FFT 预存，频域累加 |
| **non-uniform partitioned** | 超长 IR（百万–千万） | 小块延迟 | 头部小块（低延迟）+ 尾部大块（低频精度） |

**关键参数：** FFT 长度 N、块长 B、分区数 P = ceil(M/B)；频域乘加量 ≈ P 次复数乘；用实数 FFT 可再省一半。手册的「FFT filter length 512」应理解为**每个 2x 级联级的 FFT 块长**，而非总抽头数。

### 6.10 噪声整形 / 抖动的可实现结构

**误差反馈（error-feedback）拓扑：**

    v[n] = x[n] + Σ_{k=1..K} c_k · e[n−k]
    y[n] = quantize(v[n], bits)
    e[n] = y[n] − v[n]

- **none**：y = round(x)，无反馈。
- **NS1**：一阶，c = [1]（即 e[n−1]），NTF = 1 − z⁻¹，噪声斜率 +6 dB/oct。
- **NS4 / NS5 / NS9**：K=4/5/9 的高阶 NTF，系数按「最大平坦/最小可闻」优化；需检查 Lee 稳定判据，防止极限环。
- **LNS15**：15 阶、平滑斜率，系数需高精度（双精度）并做缩放，否则易过载。
- **抖动注入**：
  - RPDF：u ~ U(−0.5, 0.5) LSB（白噪声，一阶统计平坦）
  - TPDF：u = (r1 − r2)·LSB，r1,r2 ~ U(0,1)（三角分布，消除量化器的二阶统计）
  - Gauss1：Box-Muller 生成 N(0,σ)，σ ≈ 0.3–0.5 LSB
  - shaped：把 RPDF/TPDF 噪声先通过一个整形滤波器（噪声传递函数）再注入
- **位深缩减**：先加抖动/整形，再量化到目标 bits；DAC bits 设置直接决定 LSB 大小。
- **过载处理**：整形会把误差能量推向高频，需在末端做软限幅，否则高频回授会溢出。

### 6.11 SDM 调制器的可实现结构

**基本拓扑（5/7 阶单环 1-bit）：**

    u[n] = sat(x[n] − Σ a_i · s_i[n])      // 环路滤波反馈
    y[n] = sign(u[n] + dither)              // 1-bit 量化
    s_i[n+1] = s_i[n] + b_i · (u[n] − y[n]) // 积分器链

- **阶数**：5 阶 NTF 较温和，对模拟重构滤波器要求低；7 阶 NTF 更陡，带外噪声压制更好但环路更难稳定。
- **EC（extended compensation）**：在反馈/前馈中增加补偿支路，抵消积分器有限增益与量化器非线性造成的失真。工程上可理解为「增加一级误差补偿 FIR/IIR」。
- **adaptive（ASDM）**：系数或增益随输入幅度/频谱调整，避免大信号过载、小信号噪声抬升。
- **512+fs / 256+fs**：针对高 OSR 重新缩放系数（NTF 零点位置、积分器增益），使噪声拐点落在更高频段。
- **AMSDM「pseudo-multi-bit」**：输出仍 1-bit，但环路按多比特量化器设计（如等效 4–5 bit 的反馈），降低 1-bit 量化的极限环。
- **AHM「hybrid」**：量化器用 5 电平或 8-bit，需配 **DEM（动态元件匹配）** 把多电平失配噪声整形到高频；5L 版 SNR 较低（手册明确警告），8B 版带宽更好。
- **稳定性**：必须做**调制深度监测**，手册写明「Maximum modulation depth is monitored and when necessary limited to 50%」。

### 6.12 DSD → PCM 的可实现链路

    1-bit DSD → CIC 抽取（大比率）→ 半带级联（2x/4x/8x）→ 噪声滤波器 → PCM 输出

- **CIC**：无乘法、只做加减与积分，适合 64–1024 倍大比率首级抽取；需补偿 CIC 的 sinc^N 通带下垂。
- **噪声滤波器**（standard/low/high-order/.../brickwall）：在抽取后加一级低通 FIR；「brickwall」为超长线性相位 FIR，截止按 DSD 档位 25/50/100/200/400 kHz。
- **sac（sliding average）**：滑动平均＝CIC 的特例；**wec / wec2**：加权单元转换器模型，用加权系数模拟 SACD 规范。
- **Conversion**：
  - traditional → 递归 IIR 抽取
  - single-steep / single-short → 单级 FIR
  - sinc-S / sinc-M → 超长 FIR（65536 / 1M 抽头）
  - poly-lp / poly-mp / poly-short-* / poly-xtr* / poly-ext2 / poly-gauss-long → 多相 FIR
  - none → 不抽取，直接输出 1-bit 流

### 6.13 缓冲、线程与实时性

- **Buffer time**：声卡 FIFO 长度，10–100 ms；越短延迟越低、越易断音。
- **Blocks per cycle**：每次处理的 block 数，等价于批大小；大 batch 降开销、小 batch 保 cache。
- **Idle time**：播放结束后保持引擎/设备打开的时长，避免重启延迟。
- **Multicore DSP**：按通道/按 block 划分任务；auto 用工作窃取队列。
- **CUDA**：滤波器与卷积可分卡；灰显时仅卷积卸载。

---

## 7. 功能对齐清单（可直接用作验收表）

### 7.1 分组验收表

| 组 ID | 组名 | 条目数 | 验收要点 |
|---|---|---|---|
| **PCM-NS** | 抖动/噪声整形 | 10 | 选项名、顺序、默认值；每种在推荐速率下的 NTF 斜率与噪声底；none 的 bit-perfect |
| **PCM-DACBITS** | DAC bits | 3 组/8 型号 | 20/19/18 预设；USB 非 32-bit 时可手填 |
| **PCM-DSDNF** | DSD→PCM 噪声滤波 | 13 | 13 个选项；brickwall 的 5 档截止频率（25/50/100/200/400 kHz） |
| **PCM-DSDCV** | DSD→PCM 转换 | 14 | 14 个选项；sinc-S=65536、sinc-M=1M；none 不抽取 |
| **SDM-MOD** | 调制器 | 36 | 全部名称/阶数/生成代；512+fs、256+fs、AHM 的速率门槛 |
| **SDM-INT** | 积分器 | 8 | 8 个选项；DSD64 基准带宽 50/25/30/50/24/21.5 kHz |
| **SDM-CV** | SDM→SDM 转换 | 3 | wide/narrow/XFi，默认 XFi |
| **SDM-DS** | DirectSDM | 1 | 关闭全部处理；音量禁用；PCM 音量 -3 dBFS；距离处理仍可用 |
| **FLT** | 滤波器 | 69 | 名称、Apod、Ratio、质量、SDM 两段 ≥16x 标记 |
| **ADV** | Advanced | 11 | 三态 Multicore、E-core、CUDA、pipelines、blocks、idle、FFT length=512、metering、album gain、UPnP、log |

### 7.2 关键行为验收（跨条目）

| 编号 | 行为 | 期望 |
|---|---|---|
| T-01 | 1x / Nx 两套选择独立 | 源率 <50 kHz 用 1x；≥50 kHz 用 Nx |
| T-02 | Apod 计数 >10 | UI 提示/推荐 apodizing 滤波器 |
| T-03 | 选择非整数比不支持的滤波器 | 该选项禁用或自动回退 |
| T-04 | none 滤波器 | 输出率 = 源率；DSD 源为 1/16 |
| T-05 | SDM 输出 + ext2/gauss/sinc 家族 | 内部至少 16x 两段处理 |
| T-06 | \*-2s | 第一级 ≥8x，第二级轻量算法 |
| T-07 | 512+fs 调制器用于 <DSD512 | 应提示或不可选 |
| T-08 | AHM 用于数字音量主控 | 应给出 SNR 警告 |
| T-09 | DirectSDM 开启 | 音量控件禁用，PCM 音量固定 -3 dBFS |
| T-10 | DAC bits 与 R2R + LNS15/NS9/NS5 | 小信号失真与过零失真下降 |
| T-11 | FFT filter length 改变 | 仅影响 FFT 型滤波器滚降，不影响其他 |
| T-12 | Buffer time 10–100 ms | 低于 10 ms 需警告断音风险 |

---

## 8. 手册未定义 / 需实测确认的空白项

| # | 空白 | 现状 | 建议 |
|---|---|---|---|
| 1 | **NS2 / NS3 / NS6 / NS7 / NS8** | 手册未列 | 以实际软件 UI 为准；不要自行补编号 |
| 2 | **Apod 列的 ½** | 手册未定义 | 需实测：可能表示「仅部分比率 apodizing」或「半 apodizing」 |
| 3 | **Ratio 列取值定义** | 手册未定义 | 按本文 3.2 的推断实现，并做灰显测试 |
| 4 | **「three types of integrators」vs 8 项** | 正文与表格不一致 | 按结构族理解（IIR/FIR/CIC） |
| 5 | **「super」的技术含义** | 仅一词 | 需实测其 NTF/算力差异 |
| 6 | **EC 补偿的具体对象** | 未展开 | 需实测 THD/噪声底差异 |
| 7 | **AHM 的 SNR 具体数值** | 只有「Limited SNR」 | 需实测 |
| 8 | **gauss 非 hires 成员的相位** | 未标注 | 需实测群延迟/预振铃 |
| 9 | **\*-2s 的 Apod 列** | 原表显示 O，疑为扫描残缺 | 按基滤波器继承 |
| 10 | **每种滤波器的抽头数** | 仅 sinc/closed-form 给了数值 | 其余需实测或反推 |
| 11 | **DSD 输出下各滤波器的可用性** | 未逐项列出 | 需实测灰显矩阵 |
| 12 | **「高分辨率音量」** | 本手册无此功能 | 若目标版本有，另取该版本手册 |
| 13 | **1.5M PCM** | 手册写 1536 kHz | 以 1536 kHz 为准 |
| 14 | **原文拼写错误** | 两处 "modulatro"；"high reate" | 仅记录，不影响功能 |

---

## 9. 条目数汇总（最终）

| 类别 | 条目数 |
|---|---|
| PCM 抖动 / 噪声整形 | 10 |
| DSD→PCM 噪声滤波器 | 13 |
| DSD→PCM 转换算法 | 14 |
| SDM 调制器 | 36 |
| SDM 积分器 | 8 |
| SDM→SDM 转换 | 3 |
| DirectSDM | 1 |
| 滤波器 / 过采样 | 69 |
| Advanced 可调项 | 11 |
| **合计** | **165** |

另有：

- **数值指标 80 条**（本文件第 4 章，编号 1–80）。
- **DAC bits 型号 8 个 / 3 组**。
- **PCM 增益补偿型号 11 项**（含「取决于 AFIR」的 TI/Burr-Brown）。
- **工程可实现章节 13 节**（第 6 章 6.1–6.13，其中 6.1 为总览、6.2–6.13 为实现模块）。
- **验收行为 12 条**（7.2 节 T-01 – T-12）。
- **手册空白项 14 条**（第 8 章）。

---

*本文档依据 D:\RTA-hqplayer\research\hqplayer6-manual.txt 第 480–1790 行（4.4–4.6）与第 1791–1872 行（4.7）逐行整理，并交叉核对第 60–479、2208–2382 行。所有【原文】为直译，【推断】为工程补充。*
