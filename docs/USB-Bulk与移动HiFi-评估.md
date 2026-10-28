# 「USB Bulk」移动 HiFi 评估（2026-10-28）

> 起因：用户听到烧友提到「手机 HIFI 有个特别的 USB 协议：usb bulk」，要求评估。
> 本文只做**事实核对与判断**，不改代码、不立项。
> 相关：`docs\安卓版-开发结论.md`（UAC2 路线）

---

## 0. 结论先行（四句话）

1. **「USB bulk」不是一个标准，而是一个真实存在的私有技术 + 一堆误传。** 烧友说的多半是
   **Bulk Pet（Bulk Pure Enhanced Technology）**，日本 INTERFACE 株式会社（ITF）的私有 USB 传输技术。
2. **它不改变 bit-perfect，也不改变抖动。** 两者都是 bit-perfect；异步 UAC2 的时钟本来就在 DAC 本地。
   它真正改变的是**数据传输的节奏**（把突发的大包拆成均匀的小包）⟹ **处理负载波动变小**。
   「听感差异」如果存在，只能通过「负载波动 → 电源噪声调制」这条**间接且不可证明**的路径。
3. **生态极小**：需要 **DAC 固件支持 + 主机侧私有驱动**。已知支持的是 **Luxman / TEAC** 等少数机型，
   而且**要装厂商私有驱动**（Windows）。**没有证据显示任何 Android 播放器支持它。**
4. **但这次查证发现了一件比 bulk 本身重要得多的事（必须修正我们的安卓方案）**：
   **Android 公开 USB Host API 至今不支持等时传输（isochronous）。**
   这直接影响我们 UAC2 的实现方式 —— 详见 §5。

---

## 1. 先厘清：「USB bulk」在烧友口里其实是三件事

| # | 说法 | 实际是什么 | 是否存在 |
|---|---|---|---|
| **A** | **Bulk Pet / BulkPET** | ITF 私有 USB 音频传输技术，用**批量传输**替代等时传输 | ✅ **真实存在**，有产品 |
| **B** | 「某品牌播放器的 bulk 模式」 | 厂商固件里私有的 bulk 音频通道 | ⚠️ 各家不同，**无公开规范** |
| **C** | 「USB bulk 比等时音质好」 | 营销话术 / 以讹传讹 | ❌ 无技术依据（§3） |

> 本文主要评估 **A**，因为它是唯一有公开资料、有产品落地的。

---

## 2. Bulk Pet 到底是什么（有出处的部分）

| 项 | 内容 | 出处 |
|---|---|---|
| 全称 | **Bulk Pure Enhanced Technology** | ITF 官方页（itf.co.jp） |
| 厂商 | 日本 **INTERFACE 株式会社（ITF）**，产品线 ITF-USB DSD | 同上 |
| 原理 | 用 USB 的 **Bulk（批量）传输**替换音频标准的 **Isochronous（等时）传输** | 同上 |
| 关键机制 | 把数据**更均匀地拆成小包频繁发送**，而不是突发大包 ⟹ **主机与设备的处理负载波动变小** | 藤本健对 ITF 根岸的访谈（av.watch.impress.co.jp） |
| 可调项 | 提供 **4 种传输模式**供厂商调音 | ITF 官方 + 访谈 |
| 驱动 | **主机侧必须装私有驱动**（Windows）；DAC 上电先起在标准 UAC2 等时模式，检测到私有驱动后再切换 | ITF 官方页 |
| 支持的设备 | **Luxman D-03X、TEAC NT-505** 等；社区称「支持 Bulk Pet 的 DAC 很少，主要是 Luxman」 | Luxman 新闻稿；Naim 社区讨论 |
| 包大小 | **比标准 ASIO 包大数倍**（超大包） | Naim 社区讨论 |

### 2.1 它和标准 UAC2 的关系

**Bulk Pet 不是要取代 UAC2，而是叠加在 UAC2 之上的私有通道**：
DAC 仍然保留标准 UAC2 兼容模式（否则接普通 PC 就不响了），只有在装了私有驱动的主机上才切到 bulk 模式。

⟹ **这意味着：任何不支持这个私有协议的主机（包括 Android）根本进不去 bulk 模式。**

---

## 3. 技术对比：等时 vs 批量（这才是判断的核心）

| 维度 | **Isochronous（UAC2 标准）** | **Bulk（Bulk Pet）** |
|---|---|---|
| 带宽保证 | ✅ **保证**（预留） | ❌ 尽力而为（与其它设备竞争） |
| 错误恢复 | ❌ **CRC 只能检测，不能重传** | ✅ **可重传** |
| 丢数据 | 会丢（无声补洞） | **不丢** |
| 传输节奏 | 绑定 USB 帧（1 ms / 125 µs） | 主机/驱动决定，可更均匀 |
| **bit-perfect** | ✅ 是（无错误时） | ✅ 是 |
| **抖动** | 异步 UAC2：**时钟在 DAC 本地**，由 feedback endpoint 调速 | **同样在 DAC 本地** —— **没有区别** |
| 驱动 | 所有操作系统内置 | **私有驱动** |
| 兼容性 | 全行业 | 极少数 DAC + 极少数主机 |
| 延迟 | 低且确定 | 略高（缓冲换均匀度） |

### 3.1 关键判断：Bulk 不改善抖动，也不改善 bit-perfect

**这一点必须说清楚，否则会跟着营销走：**

- **bit-perfect**：两种传输方式**都是 bit-perfect**。等时传输的缺点是「错了不能重传」，
  但在正常线材与供电下 USB 误码率极低；一旦真出错，等时是丢、bulk 是重传 —— **差别在「可靠性」，不在「保真度」**。
- **抖动**：异步 UAC2 的 DAC 用自己的晶振做主时钟，主机只负责按 feedback 调速。
  **数据怎么装进 USB 包，不影响 DAC 内部时钟。** 所以 bulk 对抖动**没有直接好处**。

**⟹ 唯一可能的间接机制**：传输节奏更均匀 ⟹ 主机 CPU 与 DAC 端 MCU 的瞬时负载波动更小 ⟹
**电源/地噪声调制更小** ⟹ 模拟电路底噪可能更低。

这条路径**理论上成立、但幅度不可证明**，且**依赖具体设备设计**（电源、屏蔽、地线）。
Audio Science Review 上对该技术的讨论也是这个态度 —— 他们引述厂商说法「用户可以享受改变处理负载平衡
带来的音质变化」，并认为「这有点奇怪」。

### 3.2 诚实的结论

**Bulk Pet 是一个「可能有一点点用、但绝对不值当成技术路线」的东西。**
它的价值在于提供了一个**正确的工程直觉** ——
**「数据传输的均匀度会影响两端的处理负载，进而可能影响模拟端的噪声」**。

而这个直觉，**我们完全可以在标准 UAC2 内部实现**（见 §6），不需要私有协议、不需要特定 DAC。

---

## 4. 对我们的战略含义

| 问题 | 判断 |
|---|---|
| 要不要把 bulk 当成安卓版的核心卖点？ | ❌ **不要**。需要 DAC 固件支持 + 私有协议逆向，生态极小 |
| 要不要支持 Bulk Pet？ | ⚠️ **暂不**。协议不公开，逆向有法律与技术双重风险；支持的 DAC 只有寥寥几款 |
| 它能不能成为差异化？ | ❌ **不能**。差异化应该在 DSP，不在传输层（传输层大家都是 bit-perfect） |
| 有没有值得学的？ | ✅ **有**：均匀发送、大缓冲、低负载波动 —— 这套思路我们可以自己做 |

---

## 5. ⚠️ 意外发现：Android 公开 USB API 不支持等时传输（**必须修正安卓方案**）

**这是本次查证最重要的收获，比 bulk 本身重要得多。**

### 5.1 事实

| 证据 | 内容 |
|---|---|
| Android 官方 UsbRequest 文档（页面更新于 **2026-08-03**） | 「UsbRequests can be used to transfer data on **bulk and interrupt** endpoints.」—— **通篇没有 isochronous** |
| 平台常量 USB_ENDPOINT_XFER_ISOC | 注释是「Isochronous endpoint type (**currently not supported**)」 |
| 日本 MCPC 技术报告 TR-019 | 「isochronous 転送は**未サポート**」；并明确「**使用等时传输的 UAC（音频）设备不可连接**」（指通过 Android USB Host API） |
| StackOverflow 长期讨论 | 结论一致：想做等时传输只能 **native + libusb**，且要用 UsbDeviceConnection.getFileDescriptor() 拿到的 fd 交给 libusb |

### 5.2 这意味着什么

**我们之前写的安卓方案（`docs\安卓版-开发结论.md` §3 与 P6）默认了「用标准 USB API 做 UAC2 等时传输」—— 这条走不通。**

正确做法：

```
Java/Kotlin 层：UsbManager 枚举 + requestPermission（拿到 UsbDeviceConnection）
        ↓ 把 getFileDescriptor() 的 fd 交给 native
C/C++ 层（NDK）：libusb（用 fd 打开）→ 等时传输（libusb_fill_iso_transfer）
        ↓ 或者完全自己写 usbfs ioctl（URB_ISOCHRONOUS）
DAC
```

**好消息**：这条路径是成熟做法，UAPP / Vox Machina 都是这么干的，**不需要 root**。
**坏消息**：libusb 的等时传输是**异步回调**模型，与我们「同步写缓冲」的 WASAPI 风格差别很大，
**缓冲策略要重写**（这与之前说的「Android 端 AAudio 是回调驱动，缓冲策略要重写」是同一类问题）。

### 5.3 顺带解释了「bulk 为什么在安卓圈子流行」

因为 **bulk 是 Android 自己的 USB API 唯一能做的「音频可用」传输方式**。
对不想写 native 驱动层的人来说，bulk 看起来是「捷径」——
**但前提是 DAC 得支持 bulk 音频**（99% 的 UAC2 DAC 不支持）。

**⟹ 这个「捷径」对通用 DAC 实际上不存在。**

---

## 6. 我们能从 bulk 学到什么（可落地、无需私有协议）

| 学到的点 | 我们在标准 UAC2 里怎么做 |
|---|---|
| 发送节奏均匀 | 用**小包高频发送**而不是大包突发；按 USB 帧节奏铺开 |
| 降低两端负载波动 | 大缓冲 + 平滑的水位控制（我们的 L1/L2 自适应回退思路正好） |
| 减少补洞 | 等时不能重传，但可以**提前铺缓冲**，用深度换稳定性 |
| 降低 CPU 峰值 | DSP 分块大小固定、避免偶发大计算（正好和 GPU 升频的「分块稳定」诉求一致） |

**这些都不需要 Bulk Pet，也不受 DAC 型号限制。**

---

## 7. ⚠️ 未验证项（如实记录）

1. **「Bulk Pet 听感更好」没有可复现的客观证据** —— 只查到厂商说法与访谈，**没有查到独立第三方盲测**。
   要验证需要：支持 Bulk Pet 的 DAC（Luxman/TEAC）+ 专有驱动 + 两套对比测量。**当前无设备，无法做。**
2. **Android 侧是否真无任何播放器支持 Bulk Pet** —— 只能说「未搜到证据」，**不能断言不存在**。
3. **Bulk Pet 私有协议细节未知** —— 未逆向、未抓包。**不做任何假设。**
4. **本文未做任何实测** —— 全部是资料核对 + 原理推演。涉及听感的结论一律标为「不可证明」。

---

## 8. 参考

- ITF 官方 Bulk Pet 页：http://itf.co.jp/prod/audio_solution/bulk-pet
- 藤本健访谈（原理）：https://av.watch.impress.co.jp/docs/series/dal/1107075.html
- ASR 论坛讨论（质疑）：https://www.audiosciencereview.com/forum/index.php?threads/bulk-pure-enhanced-technology-pet.65207/
- Naim 社区（生态极小、包大小）：https://community.naimaudio.com/t/ethernet-vs-usb/43257/8
- Luxman D-03X（支持 Bulk Pet）：https://www.enjoythemusic.com/AXPONA_2020/Press_Center/Luxman_D03X_CD_Digital_Media_Player.htm
- Android USB 数字音频：https://source.android.google.cn/docs/core/audio/usb?hl=en
- Android UsbRequest 参考：https://developer.android.google.cn/reference/android/hardware/usb/UsbRequest
- Android 等时传输限制：https://stackoverflow.com/questions/8888801/android-usb-isochronous-data-transfer
