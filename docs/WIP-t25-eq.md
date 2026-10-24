# WIP — 261024-02 #6：20 段参数均衡 PEQ（HIFI 音质导向）

- 工作树：`D:/rlplayer-t25-eq`　分支：`feat/t25-eq`（基于 main `35933b2`）
- 测试实例：`build/rtaweb.exe --port 8932`，`RLPLAYER_LIBRARY_DIR=D:/_tmp_lead/t25-eq/lib`
  （conf.p8932 = `D:\RTA-hqplayer\MUSIC`，653 首 / 294 张专辑，只读扫描）
- 硬规矩：全程未真正播放、未打开音频设备、未调用 /api/input/start；未碰 8899/8914；未 push
- 截图：`docs/shots-t25-eq/`（双尺寸 PEQ：`10-peq-1600x1000.png`、`11-peq-1280x800.png`；
  uicheck 全页回归截图同目录）

---

## 一、设计

### 1. 信号链位置
```
解码 → Matrix → 【20 段 PEQ（新增）】→ 现有文件 EQ（REW/AutoEq）→ 延迟 → 卷积
     → 重采样 → 音量 → 抖动/量化
```
- PEQ 工作在**输入采样率**（与现有文件 EQ 同一位置），在现有 EQ **之前**；
- 只改管线配置与一处处理钩子（`engine/pipeline.h`），不动现有 EQ 实现；
- 全 0dB / bypass 时 `has_effect()=false` ⟹ 不进入处理、逐位直通路径不受影响。

### 2. 滤波器（`src/dsp/peq.h`，纯头文件、零依赖）
- **RBJ Audio EQ Cookbook** 双二阶：`Peak` / `LowShelf` / `HighShelf`（架式用 Q 形式）；
- **float64 计算系数**（a0 归一化为 1），**float32 处理**（`PeqBiquadF32`，状态 z1/z2 为 float）；
- 直接型转置 II，数值稳定性好；
- 默认 20 段中心频率（ISO 1/3 倍频程序列，保留 1kHz 基准点）：
  `20,25,40,63,80,125,200,315,500,800,1000,1250,2000,3150,5000,8000,10000,12500,16000,20000` Hz；
- 每段默认 Q=1.41、增益 0dB；UI 约束增益 ±12dB 步进 0.1、Q 0.1~20、频率 10Hz~20kHz；
- 预置前级增益（preamp，±24dB）用于抵消提升造成的削波。

### 3. 采样率相关
- `PeqEngine::prepare(fs, channels)`：切歌 / 切率时按新 fs **重建全部系数**并清状态；
- 44.1 / 48 / 96 / 192k 实测中心频率增益一致（见判据 4），系数随 fs 变化。

### 4. 参数热更新（避免爆音）
- `set_settings()`：新系数链从零状态启动，与旧链（保留滤波器状态）做 **128 样本线性交叉淡入**
  （44.1k 约 2.9ms）；`bypass` 也走同一路径（空链 = 恒等），开关无阶跃；
- 引擎内部 `std::mutex` 保护，音频线程与 HTTP 线程安全；
- 实测：1kHz 正弦 +0→+6dB 突变，最大逐样本变化 0.1302 ≈ 稳态正弦斜率 0.1309（无阶跃）；
  硬切的理论阶跃约 0.5。

### 5. 预设
- 内置：`Flat`、`Harman 风格（示例）`、`低频增强`、`人声清晰`；
- 用户预设：`rlplayer-peq-presets.txt`（`[预设名]` 分段）；当前设置：`rlplayer-peq.txt`；
- API：保存 / 加载 / 删除 / 重置 Flat。

### 6. 后端 API（`tools/rtaweb.cpp`）
| 端点 | 说明 |
|---|---|
| `GET /api/peq` | 当前状态 + 预设名列表 + 频响曲线（20Hz~20kHz 对数 160 点，含 preamp） |
| `POST /api/peq` | `bypass/preamp/bands=序号,类型,频率,增益,Q[,off];...` → 热更新 + 落盘，返回同上 |
| `POST /api/peq/preset` | `action=save|load|delete|reset&name=...` |

曲线由 C++ 用同一套 RBJ 公式计算（`peq_response_db`），前端不抄第二份，避免显示/实听不一致。

### 7. UI（`src/net/ui_script.h`，EQ 页签第一块）
- 工具栏：旁路开关、前级增益、预设下拉、加载 / 保存为预设 / 删除预设 / 重置 Flat、状态提示；
- 频响曲线：内联 SVG（160 点折线 + 0dB/±6dB 网格 + 20/100/1k/10k/20k 刻度，颜色用 --acc/--line/--mut2）；
- 20 段表格：段号 / 频率 / 增益 / Q / 类型（峰值·低架·高架）/ 启用，表头 sticky、区域内滚动（max-height 330px）；
- 编辑即时生效（debounce 260ms → POST /api/peq → 曲线随响应刷新），不打断正在输入的光标。

---

## 二、参考来源（只借鉴设计，未拷贝任何 GPL 代码）

- RBJ Audio EQ Cookbook（公式来源，公开文献）：
  https://www.w3.org/TR/audio-eq-cookbook/ ／ https://archive.org/download/Audio_EQ_Cookbook/Audio_EQ_Cookbook.pdf
- EasyEffects（GPL-3.0，仅了解其多段 PEQ 的交互与参数组织，未拷代码）：https://github.com/wwmm/easyeffects
- JamesDSP（GPL-2.0，仅参考 PEQ 段数/参数范围，未拷代码）：https://github.com/james34602/JamesDSPManager
- AutoEq（MIT，目标曲线与预设命名习惯）：https://github.com/jaakkopasanen/AutoEq
- REW（免费闭源，参数文本格式与本项目已有 eq_text.h 一致）：https://www.roomeqwizard.com/
- squig.link（测量数据库，仅参考目标曲线形态）：https://squig.link/
- OPRA（CC BY-SA 4.0，耳机预设生态）：https://opra.roon.app/
- ISO 1/3 倍频程中心频率序列（默认段频率依据）

---

## 三、判据与实测数字（`tests/test_peq.cpp` + `build/peq-api-test.js` + uicheck）

### 单元/模块测试（test_peq，61 项断言全部 PASS）
| 判据 | 实测 |
|---|---|
| 默认 20 段、频率与文档一致、Q=1.41、0dB、全启用 | PASS |
| 全段 0dB：逐样本直通一致（float 精确值） | PASS（bit-identical） |
| bypass 逐样本一致 | PASS |
| 单段 +6dB @100/1k/5k/10kHz（解析 + 正弦 RMS 双判据） | 全部 **6.0000 dB**（判据 6±0.1） |
| 单段 -6dB @1kHz | **-6.0000 dB** |
| Q 变化 → -3dB 带宽 | Q0.7 BW=1927.7Hz，Q2.8 BW=1176.1Hz，**比值 3.98x**（理论约 4x） |
| 采样率 44.1/48/96/192k 重建 | 四个采样率中心均 **6.0000 dB**，系数 b0 随 fs 变化（1.034257/1.031578/1.016077/1.008108） |
| 极值（铁律 7）：20 段全 +12dB + 0dBFS 满幅 | 输出有限，peak=17.67，无 NaN/Inf |
| 单样本 ±1.5 越界 | 无 NaN/Inf |
| 瞬态后恢复（1s 静音，取后半段） | RMS=5e-8（< -140dBFS），dc=0（无直流锁死） |
| 20 段全 -12dB + 满幅 | 有限，peak=0.9113（被衰减） |
| 热更新交叉淡入 | maxΔ=0.1302 vs 稳态斜率 0.1309（无阶跃）；淡入后稳态 6.0000 dB |
| 架式 LS/HS | 低架 20Hz=+5.98dB、10kHz=0.00dB；高架 18kHz=-5.97dB、100Hz=0.00dB |
| 序列化 round-trip（增益/Q/类型/启用/preamp/名称） | PASS |
| **管线集成**（Pipeline 挂载 PEQ，离线不打开设备） | 全 0dB 与无 PEQ **逐样本一致**；+6dB@1kHz 实测 **6.0000 dB**；`hasAnyProcessing` 0dB=false / 生效=true |

### 后端 API 实测（`peq-api-test.js`，16 项全 PASS）
- GET：20 段、预设 4 个、曲线 160 点、Flat 曲线全 0dB；
- POST 改第 11 段（1kHz）+6dB：落库 6dB，曲线 1kHz 处 **6.000**；
- bypass=1：曲线全 0dB；
- 预设：保存 → 重置 → 加载（+6dB 复原）→ 删除用户预设 → 恢复 Flat，全部 PASS；
- 20 段全 +12dB：曲线全部有限（不崩）。

### UI 实测（uicheck，全流程 0 FAIL）
- PEQ 编辑区 **20 段**；频响曲线 SVG 渲染；预设下拉 4 项（Flat + 3 示例）；
- 控件齐全且已绑定（bypass/前级/加载/保存/删除/重置）；
- 在 UI 里把第 11 段改为 1kHz +6dB → 后端落库 6dB 且曲线 1kHz 处 **6.000**；
- 通过 UI 加载「Harman 风格（示例）」→ 非零增益段生效；测试后复原 Flat；
- 双尺寸：1600×1000 / 1280×800 均无横向滚动，20 行表格、曲线完整。

---

## 四、门禁四项

| 门禁 | 命令 | 结果 |
|---|---|---|
| ① 构建 | `cmake --build build` | **exit 0（0 error）** |
| ② 单测 | `cd build; ctest -j4` | **exit 0，61/61 全过**（原 60 + 新增 peq；46.83s） |
| ③ 平台边界 | `powershell -File tools/check_platform_boundary.ps1` | **exit 0**（23 处白名单，新文件无越界） |
| ④ UI 真跑 | `node tools/uicheck.js http://127.0.0.1:8932/ docs/shots-t25-eq` | **exit 0，界面检查全部通过，0 FAIL** |

---

## 五、UI 自评分（六维，满分 100）：**90**

| 维度 | 分 | 说明 |
|---|---|---|
| 布局 20 | 18 | PEQ 卡置顶，工具栏/曲线/表格三段清晰；表格 330px 内滚动（20 行需滚动，取舍：不撑爆整页） |
| 对齐 20 | 18 | 表头与列对齐、行高统一 38px、类型/启用靠右对齐；数字输入在宽列内左对齐（可接受） |
| 对比度 20 | 18 | 深色主题文字/边框层级清晰，曲线用 --acc 强调，网格/刻度弱化；无刺眼纯白 |
| 密度 15 | 14 | 20 段紧凑一屏 8 行 + sticky 表头；工具栏单行不换行 |
| 一致性 15 | 13 | 复用 sgroup/--field/--line/--acc 变量；表格输入框 24px 比设置页通用控件略紧凑；页面底部「改动在下一次播放时生效」对 PEQ 是例外（实际热更新），已在卡片内注明 |
| 细节 10 | 9 | toast 反馈、预设名、前级提示、轴刻度、bypass；缺：单段右键重置/曲线 hover 读数 |

---

## 六、⚠️ 未完成 / 未验证（铁律 4）

1. **真实听感与真机播放未验证**：按硬规矩不发声、不开设备；「热更新在真实音频设备上是否绝对无爆音」只有引擎层与管线离线实测，**未在扬声器/耳机上实听**。
2. **真实切歌/切率未跑**：`prepare(fs)` 的重建在单元测试与管线离线测试中验证；真实播放中切歌（不同采样率）只做代码审查。
3. **WebView2 内嵌窗口未实测**：截图来自 headless Edge（1600×1000 / 1280×800）；launcher 内嵌窗口的 DPI/缩放未单独验证。
4. **20 段是 ISO 1/3 倍频程的 20 点子集**，不是完整 31 段；频率表写死在 `peq_default_frequencies()`，UI 可逐段改频率但不支持增删段。
5. **预设文件无跨进程锁**：并发写 `rlplayer-peq.txt/presets.txt` 未做原子写（单用户 UI 场景风险低，已如实记录）。
6. **CPU 占用未实测**：20 段 × 2 声道约 40 个双二阶/样本，估算 <1% 单核（未用 perf 实测）。
7. **uicheck 环境依赖项**：本测试实例首次跑时 artist/Discogs 缓存冷启动曾偶发 4 项失败，缓存预热后全绿（与 PEQ 无关，已在最终门禁记录 0 FAIL）。

---

## 七、文件清单（本分支）

| 文件 | 说明 |
|---|---|
| `src/dsp/peq.h`（新增） | PEQ 引擎：RBJ 设计、float64 系数 / float32 处理、20 段默认、预设、序列化、热更新交叉淡入 |
| `tests/test_peq.cpp`（新增） | 20 项判据 / 61 断言：直通、6dB@fc、Q 带宽、采样率、过载恢复、交叉淡入、序列化、管线集成 |
| `src/engine/pipeline.h` | `PipelineConfig.peq` + 阶段 3 前置处理钩子（输入采样率）+ `usedPeq` 统计 + 直通判定 |
| `tools/rtaweb.cpp` | PEQ 全局单例、文件载入/保存、3 个 API 端点、播放开始 prepare+挂载 |
| `src/net/ui_script.h` | EQ 页 PEQ 卡片：工具栏、SVG 曲线、20 段表格、预设操作、热更新 |
| `tools/uicheck.js` | 新增 PEQ UI 验收（20 段/曲线/预设/控件绑定/UI 改增益→后端→曲线） |
| `docs/shots-t25-eq/` | 双尺寸 PEQ 截图 + uicheck 全页回归截图（未入库，见目录） |
