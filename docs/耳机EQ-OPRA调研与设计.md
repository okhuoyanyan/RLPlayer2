# 耳机 EQ 适配（OPRA）调研与落地设计方案

> 文档版本：1.0.0  
> 调研日期：2026-10-23  
> 责任模块：RLPlayer 音频引擎 / DSP / 设置与控制面  
> 适用代码基线：1.0.9-dev  
> 状态：**只读调研与架构方案已就绪，未改动任何源码/配置**

---

## 0. 结论先行

1. **OPRA 极度适合接入**：
   - OPRA（*Open Profiles for Revealing Audio*，源自 Roon Labs 发起的开源社区项目，GitHub：`https://github.com/opra-project/OPRA`）是目前音频领域最完整、结构化程度最高的耳机参数化 EQ 开源数据库。
   - 实测库内已有 **714 家厂商、6,230 款耳机产品、12,594 条参数均衡预设**（涵盖 Sennheiser、Sony、Apple、HiFiMAN、Beyerdynamic、FiiO、AKG、水月雨等市面几乎全部主流耳机型号与主流调音目标，如 Harman Target、oratory1990、AutoEQ、Rtings、Crinacle 等）。
2. **许可证完全合规，允许随软件缓存/分发**：
   - 代码协议：MIT License；
   - 厂商、产品与 EQ 曲线数据协议：**CC BY-SA 4.0**（*Creative Commons Attribution-ShareAlike 4.0 International*，同 Wikipedia 模式）。
   - **分发与商用权益**：官方文档明文指出允许商业与非商业使用、自由分发与镜像缓存。
   - **合规义务**：需在 UI 或文档中展示 OPRA 项目归属说明、OPRA 仓库链接（`https://github.com/opra-project/OPRA`）以及曲线测量者/作者署名；若对数据库本身进行修改增删，修改后的数据库须以相同 CC BY-SA 4.0 共享（播放器软件本身代码不受传染）。
3. **API 与数据格式完全契合本项目现有 DSP**：
   - 官方提供免认证 REST API（`https://opra.roon.app/opra/...`）与全量 CDN 镜像（`http://opra.roonlabs.net/database_v1.jsonl`，约 12.6 MB，Cloudflare 缓存，更新延迟 ≤ 5 分钟）。
   - EQ 数据为纯参数化 Biquad 规范（`peak_dip`, `low_shelf`, `high_shelf` 等），含预放大增益（`gainDb`）和各频段中心频率（`frequency`）、增益（`gainDb`）、品质因数（`q`）。
   - 与本项目既有的 `src/dsp/matrix.h`（RBJ Biquad）和 `src/dsp/eq_text.h` 完全同源，可实现 **零额外复杂算法、极低改造代价** 的无缝接入。

---

## 1. OPRA 实测证据与数据规格

### 1.1 服务端点与协议实测

测试工具：Node.js HTTPS Client / curl.exe  
请求 User-Agent：`RLPlayer/1.0.9 (research)`  
抓取时间：`2026-10-22T17:43:50Z ~ 17:46:37Z`

| 端点用途 | 真实 URL | HTTP 状态 | 认证要求 | 速率限制 (RateLimit) | 返回格式 |
|---|---|---|---|---|---|
| 服务运行状态 | `https://opra.roon.app/opra/status` | **200 OK** | **无**（免登录/免 Key） | 100次/60秒 (`100;w=60`) | JSON: `{vendorCount, productCount, eqCount, lastRefresh}` |
| 厂商列表 | `https://opra.roon.app/opra/vendors` | **200 OK** | **无** | 100次/60秒 | JSON Array (714 项) |
| 产品列表 | `https://opra.roon.app/opra/products?vendorId={id}` | **200 OK** | **无** | 100次/60秒 | JSON Array |
| 产品预设列表 | `https://opra.roon.app/opra/eqs?productId={id}` | **200 OK** | **无** | 100次/60秒 | JSON Array (含预设 ID/作者/详情) |
| 单个预设详情 | `https://opra.roon.app/opra/eqs/{eqId}` | **200 OK** | **无** | 100次/60秒 | JSON Object (完整滤波器参数) |
| 全量全库 Dump | `http://opra.roonlabs.net/database_v1.jsonl` | **200 OK** | **无** | 无严格限制 (Cloudflare 静态分发) | JSONL (每行一条记录, 约 12.6 MB) |

> **关键 Header 记录**：  
> `ratelimit-limit: 100`  
> `ratelimit-policy: 100;w=60`  
> `ratelimit-remaining: 85`  
> `access-control-allow-origin: *`  
> `cache-control: private, must-revalidate` (API) / `max-age=300` (JSONL)

### 1.2 三个代表性耳机型号实测原文

#### 样本 1：Sennheiser HD 600（开放式头戴旗舰代表）
- **Product ID**: `sennheiser::hd_600`
- **EQ ID**: `sennheiser:hd_600::oratory1990_harman_target`
- **URL**: `https://opra.roon.app/opra/eqs/sennheiser:hd_600::oratory1990_harman_target`
- **抓取时间**: `2026-10-22T17:46:31.963Z`
- **原始参数响应**：
```json
{
  "id": "sennheiser:hd_600::oratory1990_harman_target",
  "productId": "sennheiser::hd_600",
  "author": "oratory1990",
  "details": "Harman Target",
  "type": "parametric_eq",
  "parameters": {
    "gainDb": -9.3,
    "bands": [
      { "frequency": 20, "gainDb": 4, "q": 1.1, "type": "peak_dip" },
      { "frequency": 97, "gainDb": -2.5, "q": 0.7, "type": "peak_dip" },
      { "frequency": 105, "gainDb": 5.5, "q": 0.71, "type": "low_shelf" },
      { "frequency": 215, "gainDb": -1.7, "q": 1.1, "type": "peak_dip" },
      { "frequency": 1400, "gainDb": -2.1, "q": 1.5, "type": "peak_dip" },
      { "frequency": 2000, "gainDb": 3, "q": 0.71, "type": "high_shelf" },
      { "frequency": 2700, "gainDb": -1.3, "q": 3, "type": "peak_dip" },
      { "frequency": 3370, "gainDb": -3.9, "q": 2.5, "type": "peak_dip" },
      { "frequency": 5350, "gainDb": -3.5, "q": 3, "type": "peak_dip" },
      { "frequency": 11000, "gainDb": -4, "q": 0.71, "type": "high_shelf" }
    ]
  }
}
```

#### 样本 2：Apple AirPods Pro 2（主流真无线入耳代表）
- **Product ID**: `apple::airpods_pro_2`
- **EQ ID**: `apple:airpods_pro_2::autoeq_filk`
- **URL**: `https://opra.roon.app/opra/eqs/apple:airpods_pro_2::autoeq_filk`
- **抓取时间**: `2026-10-22T17:46:33.241Z`
- **原始参数响应**：
```json
{
  "id": "apple:airpods_pro_2::autoeq_filk",
  "productId": "apple::airpods_pro_2",
  "author": "AutoEQ",
  "details": "Measured by Filk",
  "type": "parametric_eq",
  "parameters": {
    "gainDb": -3.6,
    "bands": [
      { "frequency": 105, "gainDb": 2.6, "q": 0.7, "type": "low_shelf" },
      { "frequency": 364, "gainDb": -1.9, "q": 1.62, "type": "peak_dip" },
      { "frequency": 4461, "gainDb": 3.3, "q": 2.37, "type": "peak_dip" },
      { "frequency": 77, "gainDb": 1.6, "q": 1.67, "type": "peak_dip" },
      { "frequency": 2828, "gainDb": -2.4, "q": 2.51, "type": "peak_dip" },
      { "frequency": 10000, "gainDb": -0.4, "q": 0.7, "type": "high_shelf" },
      { "frequency": 1257, "gainDb": 1.7, "q": 2.59, "type": "peak_dip" },
      { "frequency": 9292, "gainDb": 1.1, "q": 2.33, "type": "peak_dip" },
      { "frequency": 1734, "gainDb": -1, "q": 2.26, "type": "peak_dip" },
      { "frequency": 1320, "gainDb": 0.4, "q": 2.26, "type": "peak_dip" }
    ]
  }
}
```

#### 样本 3：Sony WH-1000XM5（主流无线降噪头戴代表）
- **Product ID**: `sony::wh_1000xm5`
- **EQ ID**: `sony:wh_1000xm5::oratory1990_harman_target`
- **URL**: `https://opra.roon.app/opra/eqs/sony:wh_1000xm5::oratory1990_harman_target`
- **抓取时间**: `2026-10-22T17:46:33.548Z`
- **原始参数响应**：
```json
{
  "id": "sony:wh_1000xm5::oratory1990_harman_target",
  "productId": "sony::wh_1000xm5",
  "author": "oratory1990",
  "details": "Harman Target",
  "type": "parametric_eq",
  "link": "https://www.dropbox.com/s/ozv8rsyvq73zavf/Sony%20WH1000XM5.pdf?dl=0",
  "parameters": {
    "gainDb": -3.6,
    "bands": [
      { "frequency": 105, "gainDb": 5.5, "q": 0.71, "type": "low_shelf" },
      { "frequency": 270, "gainDb": -9.5, "q": 0.71, "type": "low_shelf" },
      { "frequency": 880, "gainDb": -1.4, "q": 3, "type": "peak_dip" },
      { "frequency": 1170, "gainDb": 1.9, "q": 2, "type": "peak_dip" },
      { "frequency": 1800, "gainDb": -1.9, "q": 3, "type": "peak_dip" },
      { "frequency": 2230, "gainDb": 6, "q": 1.4, "type": "peak_dip" },
      { "frequency": 3200, "gainDb": -5.3, "q": 1.8, "type": "peak_dip" },
      { "frequency": 6200, "gainDb": -3.8, "q": 3.3, "type": "peak_dip" },
      { "frequency": 8900, "gainDb": -3, "q": 6, "type": "peak_dip" },
      { "frequency": 9000, "gainDb": 3, "q": 0.71, "type": "high_shelf" }
    ]
  }
}
```

---

## 2. 本项目现有 EQ/IR 架构调研（只读分析）

### 2.1 现有实现位置与数据流

```
[输入源 AudioBuffer (in.rate)]
       │
       ▼
 [Matrix 路由] (src/dsp/matrix.h)
       │
       ▼
 [每声道链 ChannelProcess] (src/engine/pipeline.h:36)
   ├── cp.gainDb (每声道平衡/增益)
   ├── cp.eq [std::vector<dsp::Biquad>] (IIR 双二阶级联，在【输入采样率】执行)
   ├── cp.delaySamples (延迟线 DelayLine)
   └── cp.ir (分块卷积 / GPU 卷积)
       │
       ▼
 [重采样器 Resampler] (src/dsp/resampler.h: 升频到 outRate)
       │
       ▼
 [音量 VolumeControl] (src/dsp/volume.h: 带平滑 Ramp)
       │
       ▼
 [Dither / SDM] ──► 输出设备
```

- **滤波器执行核心**（`src/dsp/matrix.h`）：
  - `struct Biquad`：直接 II 型双二阶滤波器，单声道样本逐点递归处理；自带解析幅频响应 `response_db(freqNorm)` 与冲激实测响应 `measured_response_db`。
  - `namespace biquad_design`：标准 Robert Bristow-Johnson (RBJ) Audio EQ Cookbook 公式，已实现 `peak`, `lowShelf`, `highShelf`, `lowpass`, `highpass`, `bandpass`, `notch`, `allpass`, `lowpass1`, `highpass1`。
- **EQ 文本解析与参数存储**（`src/dsp/eq_text.h`）：
  - 定义了结构体：
    ```cpp
    struct EqBand { bool enabled; std::string type; double fc; double gain; double q; bool hasQ; };
    struct EqSpec { double preampDb = 0.0; std::vector<EqBand> bands; };
    ```
  - 支持 REW 格式（`Filter 1: ON PK Fc 100.0 Hz Gain 3.0 dB Q 1.400`）和 AutoEq 格式（含 `Preamp: -6.0 dB`）。
  - 函数 `build_eq_filters(const EqSpec& spec, double fs)` 将 `EqSpec` 转为特定采样率 `fs` 下的 `std::vector<Biquad>`。
- **播放管线调度**（`src/engine/pipeline.h`）：
  - 管线 Stage 3（第 244~255 行）：遍历每输出声道，级联执行 `dsp::Biquad::process`；
  - 维持持久状态 `Pipeline::State::eq`，避免分块渲染（chunk processing）在块边界清零滤波器历史状态。
- **配置与 UI**：
  - `tools/rtaweb.cpp`：从 `rlplayer-settings.conf` 读写配置项 `eq`（EQ 文本文件绝对路径）。
  - `src/net/ui_script.h`：设置页「EQ 与房间校正」目前通过文本框 `sp_eq` 手动输入路径。

### 2.2 现有 EQ 的三处关键特性与差异点

1. **采样率联动**：
   - EQ 在**输入采样率 `in.rate`** 上构建并执行（位于重采样之前）。
   - 切歌时如果音源采样率变化（例如 44.1 kHz 切换到 96 kHz），必须按新的 `in.rate` 重新调用 `build_eq_filters` 重新计算 Biquad 系数 $b_0, b_1, b_2, a_0, a_1, a_2$。
2. **Preamp 增益的施加方式**：
   - `tools/rtaplay.cpp`（第 305 行）是将 `spec.preampDb` 累加到了全局数字音量：`cfg.volumeDb += spec.preampDb`；
   - 而 `tools/rtaweb.cpp`（第 1587 行）仅构建了滤波器，**漏掉了 `preampDb`**！
   - 最优解：在 `ChannelProcess` 中已有 `gainDb` 字段（`pipeline.h:41`），OPRA 预设中的负增益前级直接写入 `ChannelProcess::gainDb`，既独立于主音量滑块，又能在 EQ 阶段彻底避免正提升带来的数字削波（Clipping）。
3. **架式滤波器（Shelf）的 Q 与 S 参数映射**：
   - 现有的 `biquad_design::lowShelf` / `highShelf` 接收的是斜率参数 $S$，并在 `eq_text.h:120` 中硬编码传入了 `0.7`；
   - 但 OPRA 数据库中明确给出了架式滤波器的品质因数 $Q$（如 $Q=0.71$ 或 $0.7$）。
   - RBJ Cookbook 官方公式中，当给定 $Q$ 时：
     $$alpha = rac{sin(omega_0)}{2 Q}$$
     无需假设 $S$，即可直接计算架式滤波器的系数。需增加 `lowShelfQ` / `highShelfQ`，使曲线精度达到 bit-exact。

---

## 3. 接入 OPRA 预设的最小改动设计

### 3.1 总体架构方案：本地轻量缓存 + 格式向下兼容

为避免庞大的网络请求拖慢播放器主线程，且不将 12.6 MB 的大文件塞进 Git 仓库，采用**「元数据轻量索引 + 按需获取/落盘为本地预设」**模式：

```
                    [用户在 Web 界面选择耳机]
                              │
          ┌───────────────────┴───────────────────┐
          ▼                                       ▼
  【首次/离线模式】                        【在线/按需模式】
读取本地压缩索引                     请求官方 OPRA API
(opra_index.json, ~300KB)           (https://opra.roon.app/opra/...)
          │                                       │
          └───────────────────┬───────────────────┘
                              ▼
                展示 厂商 -> 型号 -> 预设调音
                              ▼
                用户点击 [启用该耳机校正]
                              ▼
          落盘至 profiles/opra/<id>.txt (标准 AutoEq 格式)
                              ▼
       复用已有配置项：eq=profiles/opra/<id>.txt
                              ▼
      现有 rtaplay / rtaweb / pipeline 零侵入直接生效！
```

### 3.2 最小改动数据结构设计

#### 1) 数据结构（建议置于 `src/dsp/opra_model.h` 或扩展 `src/dsp/eq_text.h`）

```cpp
namespace rta::dsp {

// 单个 OPRA 滤波器频段（对齐 OPRA schemas/eq_info.json）
struct OpraBand {
    std::string type;       // "peak_dip", "low_shelf", "high_shelf", "low_pass", "high_pass", "band_pass", "band_stop"
    double frequency = 1000.0;
    double gainDb    = 0.0; // 峰值与架式滤波器的增益 (dB)
    double q         = 0.707; // 品质因数
    int    slope     = 12;  // 低通/高通的斜率 (dB/oct)
};

// 单个预设配置
struct OpraPreset {
    std::string id;         // 例: "sennheiser:hd_600::oratory1990_harman_target"
    std::string productId;  // 例: "sennheiser::hd_600"
    std::string author;     // 例: "oratory1990"
    std::string details;    // 例: "Harman Target"
    std::string link;
    double      gainDb = 0.0; // 整体 Preamp 衰减 (负值，如 -9.3 dB)
    std::vector<OpraBand> bands;

    // 转换为现有管线通用的 EqSpec
    EqSpec to_eq_spec() const;
    // 导出为标准 AutoEq 格式文本（用于兼容现有 eqFile 机制）
    std::string to_autoeq_text() const;
};

} // namespace rta::dsp
```

#### 2) OPRA 到现有 RBJ Biquad 滤波器的精确映射表

| OPRA `type` | 对应现有 `biquad_design` 函数 | 参数传入 | 注意事项 |
|---|---|---|---|
| `peak_dip` | `peak(q, fs, f0, Q, gainDb)` | `f0 = frequency`, `Q = q`, `gainDb = gainDb` | 增益正负均精确支持 |
| `low_shelf` | `lowShelfQ(q, fs, f0, Q, gainDb)` (新增) | `f0 = frequency`, `Q = q`, `gainDb = gainDb` | 采用标准 RBJ 公式 $alpha = sinomega_0 / (2Q)$ |
| `high_shelf` | `highShelfQ(q, fs, f0, Q, gainDb)` (新增) | `f0 = frequency`, `Q = q`, `gainDb = gainDb` | 采用标准 RBJ 公式 $alpha = sinomega_0 / (2Q)$ |
| `low_pass` | `lowpass(q, fs, f0, Q)` | `f0 = frequency`, `Q = 0.7071` (按 slope 级联) | slope=12dB/oct 用 1 个 2 阶; 24dB/oct 用 2 个级联 |
| `high_pass` | `highpass(q, fs, f0, Q)` | `f0 = frequency`, `Q = 0.7071` (按 slope 级联) | 阶数按 slope / 12 计算 |
| `band_pass` | `bandpass(q, fs, f0, Q)` | `f0 = frequency`, `Q = q` | - |
| `band_stop` | `notch(q, fs, f0, Q)` | `f0 = frequency`, `Q = q` | 陷波滤波器 |
| `gain_db` | `ChannelProcess::gainDb` | `gainDb = parameters.gainDb` | 前级增益，保证满幅不削波 |

> **Nyquist 截断铁律**：如果音源采样率较小（如 44.1 kHz），且某频段 $f_0 ge f_s / 2$（如 22.05 kHz 以上），Biquad 计算会发散或出现负数。实现映射时必须加保护：当 $f_0 ge 0.49 f_s$ 时自动旁路或钳位到 $0.49 f_s$。

### 3.3 本地存储与配置存储方案

1. **配置文件关联（`rlplayer-settings.conf`）**：
   - 保持现有 `eq` 键向下兼容：`eq=profiles/opra/sennheiser_hd_600_oratory1990.txt`；
   - 增设辅助元数据键（非侵入）：
     - `opra_vendor=sennheiser`
     - `opra_product=sennheiser::hd_600`
     - `opra_preset=sennheiser:hd_600::oratory1990_harman_target`
2. **预设缓存目录**：
   - 统一存放于 `profiles/opra/`；
   - 首次选定后拉取一次落盘，之后即使无网络、或 OPRA 服务器停机，本地播放与加载均 100% 离线可靠。

### 3.4 交互入口设计（Web 控制面）

在 `src/net/ui_script.h` 的设置页「EQ 与房间校正」区域扩展：
1. **耳机品牌下拉框 / 搜索框**（支持模糊匹配，如 "Sennheiser"、"Sony"）；
2. **耳机型号下拉框**（如 "HD 600"、"WH-1000XM5"）；
3. **调音目标/预设下拉框**（如 "oratory1990 (Harman Target)"、"AutoEQ (SoundGuys)"）；
4. **曲线信息与作者署名卡片**：展示作者名、来源链接、前级增益值（Preamp: -9.3 dB）、频段数（10 Bands）；附带 OPRA Logo 与开源协议归属声明；
5. **一键切换开关**：支持随时开/关耳机 EQ，并与当前播放即时联动。

---

## 4. 判据设计与验收计划（遵守铁律 1、铁律 3、铁律 7）

### 4.1 判据设计理念（铁律 3：测试通过不等于测对了东西）

- **痛点**：不能通过「播一段音乐听听有没有声」或「没有崩」来判断 EQ 成功；因为：
  1. 信号经过但参数全零也是有声音；
  2. Biquad 系数算错或被截断只会轻微偏音，人耳瞬态不易察觉；
  3. Preamp 未生效时短时小电平测试完全正常，而 0 dBFS 强信号会严重数码削波破音。
- **科学判据**：
  1. **频域解析响应比对（Analytical Verification）**：利用 `Biquad::response_db` 计算级联总频响，在每个滤波频点验证增益误差。
  2. **时域冲激实测交叉比对（Impulse Cross-Check）**：离线向滤波链喂入单位冲激 $[1, 0, 0, dots]$，做离散傅里叶变换提取各频点响应，与解析响应做互校验（容差 $< 0.05$ dB）。
  3. **不加载 vs 加载 对比验证（Delta Check）**：同一测试信号，在 bypass 状态下输出逐位全等（0 dBFS 直通）；在 load 状态下频谱差值与 OPRA 理论曲线一致。
  4. **过载与恢复验证（铁律 7）**：输入 0 dBFS 扫频信号，在 OPRA Preamp 衰减保护下，管线输出峰值电平必须严格 $le 0.0$ dBFS，完全无限幅与饱和。

### 4.2 单元测试设计（`tests/test_opra_eq.cpp`）

纯函数离线单测，不创建任何音频设备、不发声（铁律 1）：

```cpp
// 伪代码框架
TEST("OPRA JSON 解析与频段转换") {
    // 1. 读取 HD 600 oratory1990 样本 JSON
    OpraPreset preset = parse_opra_json(hd600_json);
    ASSERT_EQ(preset.bands.size(), 10);
    ASSERT_NEAR(preset.gainDb, -9.3, 1e-6);

    // 2. 构建 48 kHz 滤波链
    auto chain = build_opra_filters(preset, 48000.0);
    ASSERT_EQ(chain.size(), 10);

    // 3. 频点理论增益比对（允许 0.15 dB 容差，考虑邻近频段叠加影响）
    // 1400 Hz 峰值滤波器: 设定 -2.1 dB
    double r1400 = eq_chain_response_db(chain, 1400.0 / 24000.0);
    ASSERT_NEAR(r1400, -2.1, 0.2);

    // 4. 极低频 (20 Hz) 与高频 (11000 Hz) 架式滤波器响应单调性与增益验证
    double r20 = eq_chain_response_db(chain, 20.0 / 24000.0);
    ASSERT_TRUE(r20 > 0.0); // 低频提升生效
}

TEST("OPRA Preamp 满幅防削波验证 (铁律 7)") {
    // 产生 0 dBFS 105 Hz 正弦波 (该频段被 EQ 提升 +5.5 dB)
    // 若没有 Preamp (-9.3 dB)，输出将达 +5.5 dBFS (过载削波)
    // 引入 Preamp 后，输出峰值必 < 0 dBFS
    ...
}
```

### 4.3 端到端与 UI 验收（E2E & UICheck）

1. **CLI 离线管线验证（`--analyze`）**：
   ```powershell
   # 严禁播放发声，仅跑离线管线验证统计量
   .\dist\rtaplay.exe --in test_48k.wav --eq profiles/opra/sennheiser_hd_600.txt --analyze
   ```
   - 验证标准：输出信息包含 `usedEq=true`，`eqBiquads=10`，`usedChannelGain=true` (或 preamp 正常施加)。
2. **Web 界面与 API 自动化检查（uicheck）**：
   - 检查 GET `/api/settings` 返回值包含 OPRA 预设配置；
   - 检查前端切换耳机预设时，设置项正确持久化到 `rlplayer-settings.conf`。

---

## 5. 未解决项与决策备忘（遵守铁律 4）

按照铁律 4 如实记录当前拿不到、协议不明或需要老板/团队进一步拍板的决策项：

| 序号 | 决策/问题项 | 现状与考量 | 推荐方案 / 影响分析 |
|---|---|---|---|
| **1** | **离线数据分发方式** | 全量 `database_v1.jsonl` 约为 12.6 MB。打包进安装包会增加体积；完全走网络则受制于用户当前网络环境（国内访问 GitHub/Cloudflare 偶尔延迟波动）。 | **推荐方案**：打包内置一个精简索引表（`opra_index.json`，仅厂商与产品名，约 300 KB）；用户点选具体耳机时再按需通过 API 抓取该耳机的 EQ 曲线并永久缓存在本地 `profiles/opra/` 中。网络受阻时支持一键导入 JSON。 |
| **2** | **Preamp 增益的施加位置** | 现有 `rtaplay.cpp` 累加在主音量 `volumeDb`，而 `rtaweb.cpp` 遗漏了 Preamp。若直接扣减主音量，用户调小/调大音量滑块会产生混淆；若放在 `ChannelProcess::gainDb`，则在 EQ 阶段独立生效。 | **推荐方案**：在 `ChannelProcess::gainDb` 独立施加 Preamp。主音量依然反映用户设定的音量滑块，二者解耦，数学上完全等价且不易溢出。 |
| **3** | **UI 授权署名与 OPRA 协议展现** | CC BY-SA 4.0 明确要求显示归属信息与源链接。 | **推荐方案**：在设置页耳机 EQ 选框底部保留一行精致的署名："EQ 数据来自 [OPRA 开源项目](https://github.com/opra-project/OPRA) (CC BY-SA 4.0)"，并在预设卡片注明具体作者（如 "oratory1990"）。 |
| **4** | **多通道与左右声道独立校正** | OPRA 目前的绝大多数预设为立体声双通道共用同一套参数；极少数特殊测试提供 Left/Right 分立参数。 | **当前基线**：初期将相同 `EqSpec` 铺开到 0 声道与 1 声道（同现有 `eqFile` 处理方式）；后续若 OPRA 引入分声道参数再做结构化拆分。 |

---

## 决策记录（2026-10-23，Lead 按「无人值守」授权代拍板）

1. **分发策略 = 方案 B（轻量索引 + 按需缓存）**：仓库/包内只带 `opra_index.json`（厂商+型号名，约 300 KB）；
   用户第一次选择某耳机时才抓取该型号 EQ 参数并永久缓存到 `profiles/opra/`；已缓存后离线可用。
   **不打包 12.6 MB 全库**；后续若要全离线再评估（用户已授权按推荐方案先行）。
2. **Preamp 施加位置 = `ChannelProcess::gainDb`**：不扣减主音量滑块；主音量与耳机 EQ 解耦。
   `rtaplay` 里旧的 `cfg.volumeDb += preampDb` 路径要一并收敛到同一处（避免两套行为）。
3. **归属声明**：设置页「耳机 EQ」分组下方一行弱化文字「EQ 数据来自 OPRA 开源项目（CC BY-SA 4.0）」
   + 可复制 URL 芯片（不打开浏览器）；文档同步署名。
4. **实施顺序**：先在独立 worktree 实现（`biquad_design` 增 `lowShelfQ/highShelfQ`、OPRA→AutoEq 文本转换、
   配置/UI/测试），门禁绿后合并；不改核心管线其余部分。
