# WIP — 261024-01 #2 / #3a / #3b / #4（UI 部分，分支 feat/t24-np）

> 起点 main `78f8655`（v1.3.11）｜工作树 `D:/rlplayer-t24-np`｜分支 `feat/t24-np`
> 测试实例 `build/rtaweb.exe --port 8921`（**backend=null / volume=-60 / metasummary=1**，未碰 8899、未碰 8914、未 push）
> 测试库 `RLPLAYER_LIBRARY_DIR=D:/_tmp_lead/t24-np/lib` → `D:\RTA-hqplayer\MUSIC`（只读）
> 截图 `D:/_tmp_lead/t24-np/shots/`（`shots/*.png` 本轮自测；`shots/npcheck/`、`shots/uicheck/` 门禁产出）

## 0. 结论一览

| 条目 | 状态 | 关键证据 |
|---|---|---|
| #2 专辑页文案 → 只留「AI 整理」 | ✅ 已改 | `renderSummaryContent` / `npDataSummaryBody` 里「AI 整理（未找到权威来源，仅供参考）」「未核对 · 依据来源整理…」全部删除；npcheck 断言正文无「未找到权威来源/仅供参考/未核对」**PASS** |
| #3a 艺术家页删掉「权威词条暂缺」整块说明 | ✅ 已改 | 说明卡 + 顶部「权威资料暂缺」小字都删；实测 `noNotice=true / noMissing=true / subEmpty=true / emptyCard=false`（两尺寸） |
| #3b ① 音量并入控制行 | ✅ 已改 | 音量与播放键**同一行**：中心差 1576=**0px**、1024=**0px**（npcheck 判据 |Δ|≤10） |
| #3b ② AI 资料与歌词左右排列 | ✅ 已改 | 歌词在左 / 标签栏在右同一行：lyric.r=1245 → rail.l=1267（1576）、877→899（1024），垂直重叠 535 / 385px |
| #3b ③ AI 资料默认隐藏、点标签出抽屉 | ✅ 已改 | 默认 `drawer.on=false`、抽屉左边界在视口外（1585 > 1576 / 1033 > 1024）、**reqs=0**（不点不请求）；抽屉不遮控制区（drawer.b=825 ≤ ctl.t=833；1024 下 567 ≤ 575） |
| #3b ④ 实体串台 | ✅ 已改（UI 侧） | 三个标签分别请求 `kind=track/release/person`（各 1 次）；歌曲标签内容含「演唱/作词」、**不含**「国籍/厂牌」；专辑标签含「艺术家/专辑类型」 |
| #4 标签值网格（按 kind 排序、空值不渲染） | ✅ 已改 | `renderFieldGrid`：短字段 2~5 列网格、长文本（简介/专辑介绍/曲目列表/制作人员/歌词赏析/关联/衍生作品）独占整行；专辑 11 行、歌曲 11 行、艺术家 6~7 行；空值直接不渲染 |

**门禁（本轮实测）**：`cmake --build build` 0 error ｜ `ctest --test-dir build -j4` **60/60 通过**（67.2s）｜
`tools/check_platform_boundary.ps1` **exit 0**（23 处白名单）｜`tools/npcheck.js` **109/109 通过（exit 0）** ｜
`tools/uicheck.js` **0 FAIL、无 JS 异常**。

## 1. 改法

### #2 全站统一只留「AI 整理」（`ui_script.h`）

- `renderSummaryContent`：`unverified` 时不再输出「AI 整理（未找到权威来源，仅供参考）」；
  有标题的板块（专辑页/艺术家页标题就是「AI 整理」）不重复；**无标题**时才在末尾补一行 10.5px 灰字「AI 整理」。
  有真实来源（citations 非空）时才保留「依据上面的来源整理，可能不完整，请以来源为准」这一行来源提示。
- `npDataSummaryBody`：整段「未核对 · 缓存结果 · 实时整理 · 依据来源整理…」删掉，状态只在抽屉头部
  （`已缓存 / 刚刚整理 / 整理中… / 暂无资料`）。
- 免责/诚实标注没丢：AI 内容始终带「AI 整理」四字（用户要求保留诚实标注、只去掉啰嗦）。

### #3a 艺术家页（`ui_script.h` renderArtistProfile）

- **删除**整块「权威词条暂缺 / 维基百科 / 萌娘百科没有匹配条目。下方会用 AI…」说明卡；
  顶部小字「权威资料暂缺」也删除（`subText` 在无权威时为空串）。
- AI 整理资料卡提到 hero 之后**第一块内容**（主内容），权威简介改为「权威简介」排在 AI 之后（佐证），
  避免同一页两段「简介」：AI 字段网格里在存在权威简介时 `skip:{'简介':1}`。
- 只有"在线整理也没开"这种确实无内容的情况才留一行极简空态（「暂无资料」+ 重试），不再写解释文案。
- 顺带修掉一个视觉 bug：无封面艺术家的首字母占位跑到圆外左上角（`.ahead .cv .ph` 缺绝对居中）。

### #3b 单曲页（`ui_page.h` 结构+CSS，`ui_script.h` 交互）

- **布局改成三段**：行1 hero（封面+标题+芯片）｜行2 `歌词（左 1fr） + AI 标签卡（右 auto）`｜行3 控制区。
  旧的四行（资料整行 / 歌词整行）**删除**——那正是用户说的"上下排列丑到不忍直视"。
  内容列限宽 `max-width:1240px` 居中，hero/歌词/seek/控制行共用同一条左右边界（对齐靠几何，不靠肉眼）。
- **音量并入控制行**：`.npctlrow` = `1fr auto 1fr`（时间左 / 控制键中 / 音量右），底部只剩这一块控制区。
  修了一个 DOM 收尾 bug：`npvol` 原来被多包了一层（在 `.npkeys` 里）→ 会掉到第二行；现在与 `.npkeys` 同级。
- **AI 资料默认隐藏**：右侧一张紧凑「AI 整理」浮卡，只露 `歌曲 / 专辑 / 艺术家`（古典时多 `作品`）标签；
  点标签 → 右侧抽屉滑出（`translateX(102%)` → 0，`visibility:hidden` 隐藏），点 X / 点遮罩 / 再点同一标签 / **Esc** 收起。
  抽屉底边 `npDrawerFit()` 动态让出控制区高度 ⟹ 1576 与 **1024x640 都不遮播放控制条**。
- chip 带状态点（灰=没取过 / 脉冲=整理中 / 绿=有内容 / 黄=取不到），标签卡用 `backdrop-filter` 独立浮起。
- **默认零预取**：不点标签不产生任何 `/api/entity/summary` 请求（npcheck 实测 `reqs=0`）——旧版会自动预取专辑+艺术家。
- 顺带：歌词上下柔和渐隐、进度条 hover 加粗、未唱到的歌词行从 `--mut2` 提到 `--mut`（对比度）。

### #4 标签值网格（`ui_script.h` 新增 `renderFieldGrid`）

- 契约：`GET /api/entity/summary` 返回 `{kind, fields:{标签:值}}`（Lead 在后端加）；无 fields 时自动回退旧的 `sections` 段落渲染。
- `EF_ORDER` 按 kind 排渲染顺序（键名与用户 #4 清单一致）：
  - track：演唱 / 作词 / 作曲 / 编曲 / 歌曲风格 / 发行时间 / 所属专辑 / 时长 / 版本说明 / 衍生作品 / 歌词赏析
  - person：简介 / 国籍 / 活跃年代 / 风格 / 代表作品 / 厂牌 / 合作
  - release：艺术家 / 发行时间 / 专辑类型 / 语种 / 出版方 / 专辑编号 / 时长 / 专辑介绍 / 曲目列表 / 制作人员 / 关联
- 渲染：`label` 10px 灰（`--mut2`）、`value` 12.5px 主色；短字段 `auto-fill minmax(178px,1fr)` 网格；
  长文本标签独占整行并保留换行；**空值/null/未知一律不渲染**（不占位、不写"未知"）。
- 三处复用同一份 `.efields` 样式：单曲页抽屉 / 专辑页 / 艺术家页（一致性）。
- 实体隔离（#3b ④ 的 UI 侧）：`npDataTargets` 仍分别按 `kind=track|release|person` 请求，
  `npDataEnsure` 用 `kind|name|artist|composer` 归键缓存；抽屉头部显示「类型标签 + 实体名」便于核对串台。

### 判据更新（`tools/npcheck.js` + `tools/uicheck.js`）

- npcheck 从 356 行扩到「布局/文案/交互」判据，本轮 **109 项全过**：
  三段不重叠、歌词↔进度条间距 ≥6（实测 18/12）、歌词左/标签右同行的**几何判据**、
  音量与控制键中心差 ≤10px、抽屉默认关闭且在视口外、默认零请求、点标签后 `kind` 请求正确、
  歌曲标签不含艺术家级字段（不串台）、抽屉底边不越过控制区、Esc 只收抽屉不退页、正文无旧文案。
- uicheck：艺术家资料页状态机增加第三分支 `ai-branch`（无权威→安静显示 AI 资料）；
  单曲页断言换成「标签栏 + 抽屉默认隐藏」；音量与控制键同行判据。
- **数据来源口径（重要）**：本机测试实例**没有 LLM key**（不花用户的钱、不出网），
  所以「资料内容」类判据用**页内契约 mock 注入**（拦截 `fetch` 返回 `{kind, fields}`，零真实调用、可复现）：
  `npcheck` 传 `--no-mock` 可关。**UI 渲染/交互/几何全部是对真实页面的断言；真实后端 fields 联调由 Lead 侧验证。**
  mock 逻辑只在验收脚本里，**没有进产品代码**。

## 2. 截图（两档 1576x908 / 1024x640）

| 画面 | 文件 |
|---|---|
| 单曲页默认态（资料隐藏、只露标签） | `shots/np-1576x908-01-default.png`、`shots/np-1024x640-01-default.png` |
| 歌曲标签抽屉 | `shots/np-1576x908-02-song.png`、`shots/np-1024x640-02-song.png` |
| 专辑标签抽屉 | `shots/np-1576x908-02-album.png`、`shots/np-1024x640-02-album.png` |
| 艺术家标签抽屉 | `shots/np-1576x908-02-artist.png`、`shots/np-1024x640-02-artist.png` |
| 专辑页（曲目 + AI 整理字段网格） | `shots/album-1576x908.png`、`shots/album-1024x640.png` |
| 艺术家页（有权威词条） | `shots/artist-1576x908.png`、`shots/artist-1024x640.png` |
| 艺术家页（**无**权威词条 → 安静只显示 AI 资料） | `shots/artist-noauth-1576x908.png`、`shots/artist-noauth-1024x640.png` |
| 门禁产出（npcheck 109 项过程截图） | `shots/npcheck/` |

⚠️ 抽屉/字段内容来自契约 mock（见上「数据来源口径」）；页面骨架、几何、交互是真的。

## 3. 六维自评分（每页自己截图、逐项扣分理由）

| 维度（满分） | 单曲页默认态 | 单曲页抽屉 | 专辑页 | 艺术家页 |
|---|---|---|---|---|
| 布局 20 | 18 | 18 | 18 | 17 |
| 对齐间距 20 | 18 | 18 | 18 | 17 |
| 对比度 20 | 17 | 18 | 18 | 17 |
| 信息密度 15 | 13 | 14 | 14 | 12 |
| 一致性 15 | 14 | 14 | 14 | 14 |
| 细节 10 | 8 | 9 | 8 | 8 |
| **合计** | **88** | **91** | **90** | **85** |

扣分理由：
- 默认态布局 -2：hero 右侧到标签卡之间仍有大片留白（1576 下视觉重心偏左上）；-2 间距：标签卡与歌词之间无对齐基准（自成浮卡）。
- 默认态对比度 -3：未唱到的歌词行虽已从 --mut2 提到 --mut，暗底上仍偏灰；长句在 1024 下略挤。
- 默认态密度 -2：信息量刻意极低（符合"默认不占版面"的要求，但按密度打分吃亏）。
- 抽屉对比度 -2：`--txt2` 正文与 12.5px 小字在浅色主题下待复核（本轮只在深色主题实测）。
- 艺术家页信息密度 -3：无权威词条时整页只有 6 个字段，「简介」一段 + 4 短字段 + 厂牌，下半屏空白；
  艺术家页间距 -3：hero 与 AI 资料卡之间 20px、卡片内 13px 行距，与专辑页 24px 卡间距不统一。
- 一致性 -1（各处）：专辑页/艺术家页的 AI 资料卡没有抽屉那样的 `kicker` 徽标，三处标题样式略有差异。
- 细节 -2（各处）：**没有做键盘 Tab 焦点环与 aria 属性**（键盘可达性缺失）；chip 仅有 title 提示。
- 专辑页 -1 布局：曲目列表与 AI 字段网格之间只有 8px，区块边界偏弱。

> 说明：自评分是**我自己的判断**，不是客观门禁；所有硬判据以 npcheck/uicheck 的 PASS/FAIL 为准。

## 4. 未完成 / 风险（如实记录）

1. **真实后端 fields 未联调**：本工作树后端还没有 `fields`（Lead 正在加）。UI 走「有 fields → 网格；没有 → sections 段落兜底」，
   兜底路径已实测（旧缓存/无 fields 时正常显示）。**合并后必须由 Lead 用真实 summary 再跑一遍 npcheck（去掉 `--no-mock` 或配 key）**。
2. **「作品（Discogs）」标签端到端本轮未复跑**：需外网 Discogs，npcheck 该分支要传古典夹具；本轮该夹具传 `-` 跳过（3 条判据未跑）。
   代码路径未改（只把 `.npdtab` 换成 `.npchip`），属**仅代码审查**。
3. **主题覆盖不全**：抽屉/标签卡只在深色主题截图；浅色/蓝色主题的 `--bg2` / `backdrop-filter` 观感未实测（有 CSS 覆盖但无截图证据）。
4. **艺术家页 ai-branch 只在 mock 场景验证**：真实实例上我抽的几位艺术家都有维基条目（data-branch），
   无权威词条那一支是用 `mock-noauth.js`（knowledge 强制 ok:false）验的。
5. **键盘可达性**：Tab 焦点环、aria-label、Esc 之外的快捷键都没做。
6. **uicheck 仍打印旧路径断言**：本轮只改单曲页/艺术家页相关断言，其它 teammates 改的区块（设置页/流媒体等）不在本轮范围。
7. **构建环境坑（已确认，供他人复用）**：新工作树直接 `cmake -G Ninja ...` 会失败
   `No CMAKE_RC_COMPILER could be found`（Windows-Clang.cmake:149）。必须加
   `-DCMAKE_RC_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/llvm-rc.exe'`（与 main 的 CMakeCache 一致）。
   本轮 t24-np / t24-out / t24-set 三个工作树的 cache 都是 RC-NOTFOUND。

## 5. 复跑命令（Lead 合并后可直接照抄）

```powershell
# 构建（注意 RC 编译器）
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe' `
  -DCMAKE_RC_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/llvm-rc.exe'
cmake --build build
ctest --test-dir build -j4
powershell -File tools/check_platform_boundary.ps1

# 测试实例（绝不碰 8899 / 8914）
$env:RLPLAYER_LIBRARY_DIR='D:/_tmp_lead/t24-np/lib'   # 里面 rlplayer-library.conf.p8921 一行：D:\RTA-hqplayer\MUSIC
.\build\rtaweb.exe --port 8921                        # build/rlplayer-settings.conf: backend=null / volume=-60 / metasummary=1

# 门禁
node tools/npcheck.js http://127.0.0.1:8921/ D:/_tmp_lead/shots 'D:\RTA-hqplayer\MUSIC\...\Declan Galbraith - Tell Me Why.flac' - 'D:\...\AiScReam - 愛♡スクリ～ム!.flac' 'D:\...\AiScReam - 愛♡スクリ～ム!.flac'
node tools/uicheck.js http://127.0.0.1:8921/ D:/_tmp_lead/shots/ui
```

## 6. 提交（分支 feat/t24-np，未 push）

| SHA | 内容 |
|---|---|
| `1f2475a` | feat(ui)：#2/#3a/#3b/#4 实现（ui_page.h + ui_script.h） |
| `e792725` | test(ui)：npcheck/uicheck 判据更新 |
| `<本提交>` | docs(wip)：本文件 + 截图目录说明 |

门禁终态（提交后复跑）：build 0 error ｜ ctest **60/60** ｜ boundary **exit 0** ｜ npcheck **109/109 exit 0** ｜ uicheck **全部通过 exit 0**。
