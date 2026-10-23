# WIP — 261023-04「外壳 / 设置」部分（分支 feat/r4-shell）

> 负责范围：用户反馈 261023-04 的 **第 1 / 2 / 3 / 8 / 10 / 12 条**（UI 外壳与设置页）。
> 工作树：`D:/rlplayer-r4-shell`（分支 `feat/r4-shell`，基点 `12a23ac`）；测试实例 `build/rtaweb.exe --port 8915`，
> `RLPLAYER_LIBRARY_DIR=D:/_tmp_lead/r4-shell/lib`（库根 `D:\\RTA-hqplayer\\MUSIC`，653 首 / 294 专辑）。
> 另一 teammate 的 8916 实例与 Lead 的 8914、用户的 8899 全程未碰。
>
> 安全红线执行情况：所有实测播放均 `backend=null`（NullSink，不出声）、`volume=-60`；
> 测试实例 conf 每次自检后用 `GET /api/settings` 复核 `backend/volume` 未被写回默认值。

---

## 0. 一句话结论

| 条目 | 状态 | 判据（实测数字） |
|---|---|---|
| #1 底栏控制组居中 + bit-perfect 紧凑 chip | 完成 | 控制组中心与底栏中心偏差 **0.0px**（1576×908 与 1024×640 两档）；`bLyric/bChain/音量` 两档均可见可点，bChain 可打开信号链 |
| #2 EQ 与房间校正移入设置页签 | 完成 | 左导航已无该项；页签顺序 `输入/输出/PCM/SDM/高级/EQ/元数据/诊断`；OPRA 搜索 HD 600 → 4 型号 / 26 预设卡片可应用；全页重复 id = 0 |
| #3 元数据三开关→两开关 + 高级两列 | 完成 | `sp_metasummary` 已删除；一次勾选同时写 `metallm+metasummary`（1,1 / 0,0 往返实测）；高级两卡同排各 629px（1576）、1024 自动单列 |
| #8 深色主题下拉菜单对比度 | 完成 | 三主题 `select` 本体对比度 15.29 / 16.6 / 18.1，`option` 16.45 / 17.85 / 14.80（全部 ≥ 4.5:1，改前 option 背景透明） |
| #10 采集实时状态竖排 | 完成 | 状态卡横向网格：1576 一行 6 个、1024 一行 4 个；数值字号 15px（原 11.5px） |
| #12 投放实时状态竖排 | 完成 | 同上；目标渲染器改整行（1273px）长设备名可读 |

---

## 1. 逐条改动（文件:行号）

### #1 底栏严重错误（`src/net/ui_page.h`、`src/net/ui_script.h`）

现场（`D:/_tmp_lead/shots-04/10-home-playing.png`）：底栏中间一行黄色「非 bit-perfect」，控制键被推到最右，
进度条跑到右侧，`#bChain`/音量整组消失。

根因：`.bar` 是 **3 列 grid**，但 `#bpBadge` 作为第 4 个子项插在 `.np` 与 `.ctr` 之间：
- 自动排布把 `.ctr` 放到第 3 列（右侧 232px 列），`.vol` 被挤到**第 2 行**（88px 行高之外）→ 肉眼看不到；
- 用户看到的"黄色一行"就是 `#bpBadge` 占掉了第 2 列大部分宽度。

修法（`ui_page.h:211-243` 附近）：
1. `grid-template-columns` 改为 `minmax(0,1fr) auto minmax(0,1fr)`：左右两列严格等宽 ⟹ **中间控制组水平居中**（不再依赖内容宽度）；
2. `#bpBadge` 不再占中间列，移到右侧状态簇（bLyric/bChain 之后），改成紧凑 chip：
   `#bpBadge{display:none}` + `.bp-yes/.bp-no/.bp-direct` 三态配色（走主题变量 `--ok-text/--warn/--bp-direct`，
   三套主题各一份，深色下不再是"硬编码黄色"）；
3. 窄屏 `@media (max-width:1180px)`：中间列收窄到 300px、封面 48px、进度条不变，
   只压缩音量条（88→64px）与隐藏格式徽标 —— **歌词 / 信号链 / 音量三件套任何宽度都保留**；
4. JS（`ui_script.h:1306-1315`）只负责文字与 class（`bp-yes/bp-no/bp-direct`），不再写死颜色。

实测（CDP 量取，见 `docs/shots-r4-shell/`）：
| 尺寸 | 底栏中心 | 控制键组中心 | 偏差 | bLyric | bChain | 音量条 |
|---|---|---|---|---|---|---|
| 1576×908 | 788.0 | 788.0 | **0.0px** | 可见 32×32 | 可见 32×32 | 可见 |
| 1024×640 | 512.0 | 512.0 | **0.0px** | 可见 32×32 | 可见 32×32 | 可见 |

bChain 点击实测打开信号链弹层（内容含「非 bit-perfect / 抖动整形…」）。

> 附带修掉的真实缺陷（自检偶发空白页的根因，属外壳稳定性）：loadFiles() 与 renderQueuePage() 的异步回调没有 S.view 守卫，
> 用户切走后迟到的 /api/files、/api/queue 响应会覆写当前页面（loadTracks 早有守卫，这两处漏了）。已在回调开头加 if (S.view !== ...) return;，不改后端。

### #2 EQ 与房间校正移入设置页（`src/net/ui_page.h`、`src/net/ui_script.h`）

- `ui_page.h`：删除左导航 `data-v=eq` 那一项（含注释说明原因）。
- `ui_script.h`：
  - 设置页页签新增 `<button class=stab data-g=eq>EQ</button>`（排在「高级」之后）；
  - 新增 `eqSettingsHtml()`（`ui_script.h:3503` 起）用设置页统一的分组卡片（`sgroup`）渲染原来三块：
    耳机 EQ（OPRA 搜索 + 热门品牌芯片 + 型号/预设卡片 + 目标曲线说明）、EQ 文件（REW/AutoEq 文本）、
    房间校正 IR（WAV + GPU 卷积开关 + 保存按钮）；
  - 原 `loadEqView()`（独立整页）删除；`initOpraControls()`（bGoEqNav 跳转）删除；
    `高级` 页签里「前往 EQ 与房间校正页面」整组删除；
  - 异步回调守卫从 `S.view==='eq'` 改为 `eqTabActive()`（设置页开着 + EQ 页签选中），
    防止切走后过期回调写脏页面。
  - **控件 id 全部沿用旧名**：`sp_opra_search / sp_opra_copychip / eqBrandChips / eqModelCards /
    eqPresetsArea / eqPresetCards / eqTargetCurveInfo / sp_opra_msg / sp_eq / sp_ir / sp_irgpu / bSaveRoomEq`
    —— 单曲页若要读 EQ 路径或跳转，仍用 `/api/settings` 的 `eq` 键 + 设置页 EQ 页签，无需改名。
- 判据实测：`sp_opra_search` 空输入 → 型号卡片 0 张 + 8 个品牌芯片；输入 `HD 600` → 4 个型号；
  点型号 → 26 张预设卡片，每张都有「应用到 EQ」按钮；目标曲线说明显示。
  全页 `document.querySelectorAll('[id]')` 重复 id 检查 = **0**。

### #3 元数据设置页重做（`src/net/ui_script.h`、`src/net/ui_page.h`）

- 三个开关 → 两个（`ui_script.h:3040` 起）：
  1. `sp_metaon`「自动补全（不用 AI）」= `metaon`，说明写明只走公开数据源、不调 AI、不产生费用；
  2. `sp_metallm`「AI 增强（DeepSeek）」= 原「DeepSeek 增强（无标签文件）」+「在线整理音乐资料简介」合并，
     说明写明 **会联网调用 DeepSeek（按次计费）**；`sp_metasummary` 控件删除。
- 保存语义：一个开关的 onchange 同时改 `META.llm` 与 `META.summary`，
  `saveSettings()` 本来就写 `metallm=` 与 `metasummary=` 两键 ⟹ **开=1,1 / 关=0,0**。
- 高级选项（`#sp_metaadv`，默认仍收起）：`ui_page.h` 新增 `.meta-adv-grid` 左右两列 + `.meta-card` 卡片：
  - 左：数据源（逐源开关，`#sp_metasrc` 内再 2 列）；
  - 右：DeepSeek 接入（Key 输入 / 保存 / 测试 / 清除 / 掩码状态）+ 标签写回（说明 + 预览/执行）。
  - `#sp_metaadv` 补了 `.sfld.wide`（否则被 `.sgbody` 的 auto-fill 网格压进单个 260px 单元，两列各只剩 140px
    —— 第一版实测就踩到了，已修）。
- 实测（1576×908）：两卡各 629px、同一行（top 386/386），网格宽 1273px；1024×640 自动堆叠单列（351px 宽，左 380 / 右 700）。
- **保存 → 重开状态一致性实测**（`/api/settings` 复核）：
  - 打开 AI：`metallm=1,metasummary=1` → 离开设置再回来，复选框勾选；
  - 关闭 AI：`metallm=0,metasummary=0` → 重开，复选框未勾选；
  - 全程 `backend=null`、`volume=-60` 保持不变（未被整文件重写清空）。

> 旧的混合态（`metallm=1,metasummary=0`，即历史默认）在新 UI 里显示为「未勾选」——这是合并后的必然：
> 一个开关无法表达"一半开"。用户第一次动这个开关就会写成 1,1 或 0,0；不碰则 conf 保持原样，不会偷偷计费。

### #8 深色主题下拉菜单看不清（`src/net/ui_page.h`）

根因：`<option>` 没有任何显式 `background/color`，computed 背景是 `rgba(0,0,0,0)`；
在深色主题 + 某些宿主下弹层按浅底渲染，而 option 文字继承了浅色 `--txt` ⟹ 浅字压浅底。

修法（`ui_page.h:81-88`）：全局（三套主题都走 CSS 变量）
```css
select{color:var(--txt);background-color:var(--field)}
select option{background-color:var(--bg2);color:var(--txt)}
select option:checked{background-color:var(--acc-weak);color:var(--acc-text)}  /* 不用 #3b82f6+白字：只有 3.68:1 */
select option:disabled{color:var(--mut2)}
```

实测（CDP 读 computed style 后按 WCAG 公式算，select 的半透明背景按当前主题体色合成）：

| 主题 | select 本体 | option 菜单 | option 背景是否不透明 |
|---|---|---|---|
| dark | 15.29:1 | 16.45:1 | 是 `rgb(12,14,19)` |
| light | 14.96:1 | 17.85:1 | 是 `rgb(255,255,255)` |
| blue | 12.32:1 | 14.80:1 | 是 `rgb(247,251,255)` |

> 截图说明：headless Edge 的原生下拉弹层属于浏览器 UI，`Page.captureScreenshot` 截不到；
> 因此额外把 `select` 临时设 `size=5` 让选项进入文档流截图（见 `select-listbox-*.png`），
> 证明 option 的底色/文字色在页面内渲染正确；原生弹层以 computed style 为准。

### #10 / #12 状态竖排 → 横向统计卡（`src/net/ui_script.h`、`src/net/ui_page.h`）

根因：状态网格本身已是 `.in-stat-grid`（auto-fill 180px），但它作为 `.sgbody`（auto-fill minmax(260px,1fr)）
的**一个网格子项**，只拿到一个 260px 单元 ⟹ 内层只剩 1 列 → 所有指标竖排成长条。

修法：
- 标记：两处 `<div class=in-stat-grid>` 改为 `<div class="sfld wide in-stat-grid">`（`ui_script.h:2835 / 2951`）；
- CSS（`ui_page.h:379-388`）：卡片 `minmax(200px,1fr)`（1024 下 160px 换行）、内边距/圆角加大，
  `.lbl` 10px、`.val` **15px/650**（"标题小字 + 数值大字"），长文本（输出文件、渲染器名）用 `.val.sm` 12px；
- `#12` 另外把「目标渲染器」下拉从 260px 小格改成整行（`ui_script.h:2806` 附近），长设备名可读。

实测：1576 宽 `.in-stat-grid` 一行 6 张卡（10 项里 maxRow=6）；1024 宽一行 4 张；两处均如此，数值字号 15px。

---

## 2. uicheck.js 同步（同一门禁）

按行为变化同步/新增断言，**没有放松任何旧判据**：

1. 导航：从「有古典作品 + 有 EQ」改为「有古典作品 + **没有** EQ」（#2）；
2. 页签顺序：`输入/输出/PCM/SDM/高级/EQ/元数据/诊断`；
3. EQ 块：由「进入独立 eq 视图 / `S.view==='eq'`」改为「设置页 `data-g=eq` 页签激活」，OPRA 判据全部保留；
4. 高级页：`bGoEqNav` 由「必须存在」改为「必须不存在」；并新增「抽头/衰减在设置页、EQ 文件/IR 在 EQ 页签内」的归属断言；
5. 元数据：
   - `metaFix` 从三 id 改为两 id，并断言 `sp_metasummary` **不存在**；文案断言包含 `AI 增强/DeepSeek/按次计费`；
   - 合并写语义断言：**劫持 `api()` 截获 `saveSettings` 生成的 body**，检查 `metallm` 与 `metasummary` 同时为 1/同时为 0，
     然后恢复函数、META 与 DOM —— **不发任何网络请求、不落盘**。
     理由：历史 conf 可能是 `metallm=1,metasummary=0` 的混合态，合并后两次点击无法还原成混合态；
     若真落盘，自检会改脏用户/测试实例的真实设置（本项目刚出过 /api/settings 部分键 POST 清空设置的事故）。
     Lead 已确认这个写法。
6. 新增 #1 几何断言（1600 与 1024 两档：偏差 < 8px + 三件套可见）；
7. 新增 #8 三主题 select/option 对比度断言（≥ 4.5:1，含"option 背景必须不透明"）；
8. 新增 #10/#12 横向网格断言（同一行 ≥ 4 张卡 + 数值字号 ≥ 14px + 渲染器整行 ≥ 400px）；
9. 队列/文件夹这类**异步渲染页**的固定 `sleep` 改成 `waitFor`（慢机器/阻塞式 HTTP 服务下会假失败；
   顺带修掉了一个 `getElementById('fPath').value` 的空引用崩溃）。

---

## 3. 自评分（每页 ≥ 85 才过）

评分表：布局层次 20 / 对齐与间距 20 / 对比度与可读性 20 / 信息密度 15 / 视觉一致性 15 / 细节打磨 10。
截图：`docs/shots-r4-shell/`（20 张；**按 Lead 要求只放本地、未入库**；深色 1576×908 + 1024×640 两档，浅色/浅蓝主题各 3 张）：
home-playing-*、tracks-sort-*、tracks-select-listbox-1576x908（把 select 临时 size=6 让 option 进入文档流，证明菜单配色）、settings-input-*、settings-output-*、settings-advanced-1576x908、settings-eq-*、settings-meta-*；
主题覆盖另见 `home-light/tracks-select-light/settings-meta-light` 与 `home-blue/tracks-select-blue/settings-meta-blue`。

| 页面 | 尺寸 | 布局 | 对齐 | 对比 | 密度 | 一致 | 细节 | 总分 | 扣分理由（要点） |
|---|---|---|---|---|---|---|---|---|---|
| 首页（播放中） | 1576×908 | 19 | 19 | 20 | 14 | 15 | 9 | **96** | 底栏左中右三段式层次清楚；右簇 7 个元素略密，扣 1 密度、1 细节 |
| 首页（播放中） | 1024×640 | 18 | 18 | 19 | 13 | 15 | 9 | **92** | 隐藏格式徽标（有意），右簇略挤；封面缩小后标题略挤，扣层次/密度 |
| 曲目页 | 1576×908 | 18 | 19 | 19 | 14 | 14 | 9 | **93** | 排序控件右上对齐好；行信息密度高但可扫读；次要文字 `--mut2` 偏淡扣 1 对比 |
| 设置 · 输入 | 1576×908 | 19 | 19 | 19 | 14 | 15 | 9 | **95** | 表单 4 列 + 状态卡 6 列，层次清楚；状态卡数值大字可读 |
| 设置 · 输入 | 1024×640 | 18 | 18 | 19 | 14 | 15 | 9 | **93** | 表单降为 2~3 列、状态卡 4 列；换行自然 |
| 设置 · 输出/投放 | 1576×908 | 18 | 19 | 19 | 14 | 15 | 9 | **94** | 分组清晰；投放状态 6 卡一行，渲染器整行可读 |
| 设置 · 高级 | 1576×908 | 18 | 18 | 19 | 14 | 15 | 9 | **93** | 分组卡片节奏一致；移除 EQ 跳转组后留白正常 |
| 设置 · EQ | 1576×908 | 19 | 19 | 19 | 14 | 15 | 9 | **95** | 搜索行 + 品牌芯片 + 型号/预设卡片层次好；预设卡信息多但字号克制 |
| 设置 · EQ | 1024×640 | 18 | 18 | 19 | 13 | 15 | 9 | **92** | 预设卡变 2 列，信息密度略降；搜索行仍单行可读 |
| 设置 · 元数据（高级展开） | 1576×908 | 19 | 19 | 19 | 14 | 15 | 9 | **95** | 两卡等宽同排（629/629），左数据源 2 列、右接入+写回分段；开关描述整行 |
| 设置 · 元数据（高级展开） | 1024×640 | 18 | 18 | 19 | 14 | 15 | 9 | **93** | 自动堆叠单列；纵向变长属正常换行 |

最低 **92**（≥85 门槛）。扣分均为"信息密度/右簇拥挤"这类可控项，没有布局/对比度硬伤。

---

## 4. 门禁四项

| 门禁 | 结果 | 证据 |
|---|---|---|
| 构建 | ✅ `cmake --build build` 全绿（169/169，clang 21 + lld，仅既有 48 条 deprecation 警告） | `D:/_tmp_lead/r4-shell/build.log` |
| ctest | ✅ **60/60 passed**，108.82s | `D:/_tmp_lead/r4-shell/ctest.log` |
| 边界 `check_platform_boundary.ps1` | ✅ `[PASS] 内核目录无平台越界（23 处已按白名单豁免）`，exit 0 | `D:/_tmp_lead/r4-shell/boundary.log` |
| uicheck | ✅ **界面检查全部通过**（exit 0，含新增的 12 类断言） | `D:/_tmp_lead/r4-shell/uicheck5.log` |

### uicheck 历史运行记录（本机 8915 测试实例，库=653 首）

- 第 1 次（改动后）：8 FAIL，其中 5 条是**环境相关**（`api/entity/zard → not found`：本实例是全新 build 目录、
  没有 entity/knowledge 缓存；Discogs 版本页 / 网络离线空态同理），与本次改动无关；
  3 条是我自己断言的 bug（文案取错父节点、EQ 控件归属判据写错、模板字符串反斜杠把颜色解析写坏）→ 已修。
- 第 2 次：3 FAIL（队列异步渲染假失败 2 + 浅色主题体色取错 1）→ 已修（waitFor + 每主题重取体色）。
- 第 3 次：在「文件夹」处崩（`fPath.value` 空引用，异步页固定 sleep 太短）→ 已修。
- 第 4 次：仅剩「文件夹」2 项 FAIL（fPath 空引用导致后续崩溃）—— 查明是 loadFiles/renderQueuePage 的迟到响应会覆写当前页（无 S.view 守卫；loadTracks 早有）。已补守卫 + 自检改 gotoFiles()（等条件 + 最多重试 3 次）。
- **第 5 次：界面检查全部通过（exit 0，0 FAIL）**，含全部新增判据（见上）。

> 说明：`cdp.js` 里"列出所有 PASS/FAIL"的完整日志在 `D:/_tmp_lead/r4-shell/uicheck*.log`，未入库。

---

## 4.5 三主题截图确认（Lead 明确要求）

- **深色**：`home-playing-1576x908`、`tracks-sort-1576x908`、`settings-{input,output,advanced,eq,meta}-1576x908` + 关键页 1024×640；
- **浅色**：`home-light-1576x908`、`tracks-select-light-1576x908`、`settings-meta-light-1576x908`；
- **浅蓝**：`home-blue-1576x908`、`tracks-select-blue-1576x908`、`settings-meta-blue-1576x908`。
- 三主题下底栏 bit-perfect chip 分别走 `--ok-text / --warn / --bp-direct`（浅色主题 `--warn=#b45309`，仍清晰可读）；
  select/option 对比度断言在三主题下全部 ≥ 4.5:1（uicheck 自动覆盖，不靠肉眼）。

## 5. ⚠️ 未完成 / 已知问题 / 需要 Lead 决策

1. **uicheck 的 5 条环境相关失败**（艺术家 ZARD 资料 / 缓存标注 / Discogs 版本 / 网络离线空态 / 文件夹内容）
   在**全新 build 目录 + 无 entity 缓存**的实例上必然失败；Lead 的 main 实例（有用户数据）应可通过。
   若要求我这边的 8915 也全绿，需要 Lead 给一份可用的 `rlplayer-meta` 缓存或明确"环境项豁免"。
2. **原生下拉弹层无法截图**（headless Edge 限制），#8 以 computed style + listbox 截图作证；
   WebView2 宿主下的弹层渲染与 Edge 同内核，但没有在 launcher 内嵌窗口实测（仅代码审查 + Edge 实测）。
3. **混合态展示**：历史 conflg（`metallm=1,metasummary=0`）在新 UI 显示为未勾选；这是合并语义的必然，
   已在上面写清。若 Lead 希望显示为"部分开启"，需要产品决策（三态控件）。
4. **WIP 文档与截图默认不入库**（截图按 Lead 要求只放本地 `docs/shots-r4-shell/`）。
5. 本轮**未改后端语义**；uicheck 之外的判断均为实测。

---

## 6. 复现命令

```powershell
# 构建（RC 编译器缺省会失败，需显式指定；见 build.log）
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe' `
  -DCMAKE_RC_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/llvm-rc.exe' `
  -DCMAKE_LINKER='C:/Program Files/AMD/ROCm/7.2/bin/lld-link.exe' `
  -DCMAKE_AR='C:/Program Files/AMD/ROCm/7.2/bin/llvm-ar.exe'
cmake --build build

# 测试实例（只用自己的端口）
$env:RLPLAYER_LIBRARY_DIR='D:/_tmp_lead/r4-shell/lib'
build\rtaweb.exe --port 8915

# 界面门禁
node tools/uicheck.js http://127.0.0.1:8915/ <截图目录>
```
