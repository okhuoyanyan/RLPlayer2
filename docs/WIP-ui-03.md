# WIP — 261023-03 第 4/5/6/7 条 UI 改造（feat/ui-03）

- 工作树：`D:/rlplayer-ui`　分支：`feat/ui-03`
- 基线：`f72ff62`（261023-02 五条）→ `84c3a66`（03 条 4-7 半成品快照，含 Lead 的 uicheck 验收用例）
- 本轮提交：`7ce5d5a`（本轮实际改造）、`0d7b0e8`（外部会话提交的最终快照：uicheck 扩展 + 截图目录）
- 本文件编写时的 HEAD：`0d7b0e8`（本文件与其提交见文末）
- 测试实例：`build/rtaweb.exe --port 8916`，`RLPLAYER_LIBRARY_DIR=build/ui3-test-lib`（用户 8899 实例全程未触碰）
- 硬规矩：全程未播放、未调用 /api/input/start；用户 `MUSIC/`、`rlplayer-meta/` 只读

> 背景提示（如实记录）：本轮期间有另一个旧会话在同一工作树做过 reset/commit/merge（Lead 已核实并叫停）。
> 因此 HEAD 一度从 46cf2df 被 reset 到 f72ff62，随后被提交为 84c3a66；本文件记录的实测均在 84c3a66 + 7ce5d5a 上完成。

---

## 一、改了哪些文件（本轮 7ce5d5a，175 insertions / 76 deletions）

| 文件 | 改动 |
|---|---|
| `src/net/ui_script.h` | 古典作品页说明/示例芯片/异步守卫；EQ 页三块拆分、合并搜索框结果「品牌 · 型号」、空输入不发请求、初始态回填；预设卡片 SVG + 选中高亮；apply 返回字段修复（path）；删除旧隐藏控件与 collectSettings 旧读取 |
| `src/net/ui_page.h` | `.preset-card` 增加 cursor:pointer；`.tc-curve` SVG 容器样式（沿用 --card/--line/--acc/--hov 变量） |
| `tools/rtaweb.cpp` | `/api/opra/index`：显式 `?q=` 为空 → 返回空列表；不带 q 仍返回全量索引（bootstrap 语义保留） |
| `tools/uicheck.js` | 修正过时判据：高级设置页「含抽头/衰减/卷积/EQ」由「必须有 sp_eq/sp_ir」改为「bGoEqNav 入口 或 旧控件」（EQ/IR 已按第6条独立成页） |
| `docs/WIP-ui-03.md` | 本文件 |

注：`0d7b0e8` 由外部旧会话提交，把 `docs/shots-ui03/` 截图目录也纳入版本库了 —— 与「不提交临时截图」的约定不符，合并前请 Lead 决定是否剔除。

---

## 二、逐条判据与实测证据

### 第 4 条 · 古典作品栏

判据：
1. 左导航名称「古典作品」；
2. 页内有一句清楚说明（是什么 / 能干什么）；
3. 提供示例作品芯片（Boléro / 贝多芬第五 / 勃兰登堡协奏曲…），点击直接检索；
4. nav 高亮，切走后异步回调有 S.view 守卫。

实测（CDP，8916 真跑）：
- 导航项目：`["专辑","艺术家","古典作品","曲目","文件夹","流媒体","EQ 与房间校正","播放队列","最近播放","设置"]` → PASS；
- 示例芯片 5 个：`Boléro（拉威尔） | 贝多芬第五交响曲 | 勃兰登堡协奏曲 | 四季（维瓦尔第） | 马勒第五交响曲` → PASS；
- 点「勃兰登堡协奏曲」→ `#wTitle` 值含 `Brandenburg` 且触发检索 → PASS；
- 进入作品页后 `.nav.on[data-v=works]` 存在（高亮）→ PASS；
- 切到曲目页再切回作品页，无 JS 异常、结果框正常 → PASS（守卫实现：`S.view !== 'works'` 丢弃 + `S.worksReq` 请求序号只认最后一次）。

### 第 5 条 · 曲目排序 + 收藏

判据：排序下拉 7 项、切换立即生效；列表/专辑曲目/单曲整页标题旁均可点心形；收藏落盘且刷新仍在；收藏优先置顶；启动 initFavs。

实测（CDP）：
- 排序下拉值：`default,title,artist,album,dur,year,fav` → PASS；
- 逐个切换 6 种排序并读回列表顺序，全部立即生效 → PASS（日志见 `build/uicheck-final.log`）；
- 点心形 → `/api/fav/list` 出现该文件；再点 → 消失 → PASS；
- **刷新页面（Page.reload）后再进曲目页，同一路径心形仍为已收藏（.on）** → PASS（`{"found":true,"on":true}`）；
- 「收藏优先」把收藏项排到第一 → PASS；
- 专辑曲目列表含心形按钮 → PASS；
- 单曲播放整页 `#npFavBtn`（标题旁）可显示并随收藏状态更新 → PASS（未真正播放，只构造 lastCur 后刷新 UI）；
- 启动调用 `initFavs()`（ui_script.h 启动块）→ 刷新持久化用例即其证据。

### 第 6 条 · 耳机 EQ 独立页（硬判据）

判据：三块（耳机 EQ / EQ 文件 / 房间校正 IR）；合并搜索框；空输入型号卡片 0；输入后 ≤30；后端空 q 返回空。

实测（CDP + HTTP）：
- `document.querySelectorAll('#eqModelCards .model-card').length` **空输入 = 0**（硬判据 PASS）；
- 输入 `HD6` 后 **cards = 8**（∈[1,30]，硬判据 PASS）；
- 输入 `HD 600`（uicheck 官方用例）→ cards = 4（PASS）；
- 结果项文本含「品牌 · 型号」（如 `Sennheiser · HD600`）→ PASS；
- 三块标题：`耳机校准与调音预设 | EQ 文件（REW / AutoEq 文本） | 房间校正 IR（WAV 脉冲响应）` → PASS；
- 后端：`GET /api/opra/index?q=` → `{"count":0,"items":[]}`；`GET /api/opra/index`（不带 q）→ count=6230（前端本地过滤的 bootstrap 未被破坏）→ PASS；
- 设置页保留 `#bGoEqNav` 跳转入口；`sp_eq` / `sp_ir` 全页各仅 1 个（旧隐藏控件已从设置页删除，无 id 冲突）→ PASS。

### 第 7 条 · 调音预设卡片

判据：卡片网格（名称/作者/说明/增益/频段数）；选中高亮 + 点击应用；常见目标曲线中文解释 + 内联 SVG 示意；深色主题变量、hover/active 轻过渡。

实测（CDP，HD600）：
- 预设卡片数量 4（≥3）→ PASS；每张都有「应用到 EQ」按钮（4/4）→ PASS；
- 每张卡片都有内联 SVG 目标曲线（svg=4/4）→ PASS；曲线为手绘近似（Harman OE/IEM、AutoEQ、扩散场、自由场、中性等 11 种），不引外部图片；
- 点击卡片 → `.preset-card.active` 高亮，目标曲线说明面板可见且含 SVG（是什么 / 适合谁 / 增益 / 段数）→ PASS；
- 真点一次「应用到 EQ」：`ADV.opraproduct=sennheiser::hd600`，EQ 路径写成
  `profiles/opra/sennheiser__hd600_sennheiser_hd600__oratory1990_harman_target.txt`（文件已生成 479 B），按钮变为「已应用当前预设」→ PASS；
  修复点：原代码只认后端返回的 `eqPath`，实际字段是 `path`，导致「应用成功但 EQ 路径为空」。
- 样式：`.preset-card` 沿用 --card/--line/--hov/--acc，hover 边框/阴影与 active 2px 高亮过渡 150ms。

---

## 三、门禁四项（最后一次全量结果）

| 门禁 | 命令 | 结果 |
|---|---|---|
| ① 构建 | `cd D:/rlplayer-ui; cmake --build build` | **exit 0**（ninja: no work to do，1.3.8 目标已最新） |
| ② 单测 | `cd build; ctest -j4` | **exit 0，60/60 全过**（30.94 s） |
| ③ 平台边界 | `powershell -File tools/check_platform_boundary.ps1` | **exit 0**（[PASS] 内核目录无平台越界，23 处白名单） |
| ④ UI 真跑 | `node tools/uicheck.js http://127.0.0.1:8916/ docs/shots-ui03` | **exit 0，304 PASS / 0 FAIL，无 JS 异常** |

另有自建 CDP 验收脚本（临时放 `build/ui3-verify.js`，未入库）：**51/51 PASS**，覆盖排序 7 项、刷新后收藏持久、作品页切走切回、EQ 空/有输入、预设选中与应用、后端空 q 等。

---

## 四、截图（`docs/shots-ui03/`）

EQ 三态（本任务要求）：
- `03-eq-empty.png` — 空输入：型号卡片 0 张，显示引导 + 8 个热门品牌芯片；
- `04-eq-input-hd6.png` — 输入 HD6：8 张「品牌 · 型号」卡片（官方 uicheck 另有 `04-eq-hd600-results.png`，cards=4）；
- `05-eq-preset-selected.png` — 选中预设：卡片高亮 + 内联 SVG 目标曲线；
- `06-eq-target-info.png` — 目标曲线说明面板（是什么 / 适合谁 / 增益 / 段数）。

其它：
- `10-works.png` — 古典作品页（说明 + 5 个示例芯片 + 版本列表）；`22-work-versions.png`；
- `11-tracks-sort.png` / `02-tracks-sort-fav.png` — 曲目排序与收藏优先；
- `12-tracks-fav-persist.png` — 刷新后收藏仍在；
- `13-album-favs.png` — 专辑曲目心形；
- `14-settings-eq-jump.png` — 设置页跳转入口；
- `01-nav-classical-eq.png` — 左导航；
- 其余为 uicheck 全页面回归截图（01/02/03/04/05/06/07/09/17/18/19/20/21/24）。

---

## 五、⚠️ 未完成 / 未验证 / 风险（铁律 4）

1. **「耳机的实际听感」未验证（也刻意不验证）**：应用预设只实测到生成 `profiles/opra/*.txt` 与 settings 的 `eq=` 路径；卷积/滤波链路的实际声音「仅代码审查，未经实测」，原因是硬规矩禁止发声/开音频设备。
2. **房间校正 IR（WAV）+ GPU 卷积开关**：只验证 UI 存在、可编辑、可保存（PASS）；未用真实 IR 文件跑过卷积（需要播放，按硬规矩不做）。
3. **古典作品页依赖 Discogs 外网**：在线时 Boléro 返回 10 演绎/12 版本（uicheck 判据 ≥5 PASS）；离线/被墙时只显示错误态 + 重试按钮，本轮未测离线分支。
4. **目标曲线 SVG 是手绘近似**，不是测量数据，也不代表 OPRA 预设的真实滤波响应（页面已标注「示意，手绘近似」）。
5. **截图目录被外部会话提交进 git（0d7b0e8）**，与「不提交临时截图」约定不符；建议合并前 `git rm -r --cached docs/shots-ui03` 并加 .gitignore（我未擅自改写他人提交）。
6. **并发环境风险**：本轮期间旧会话在同一工作树做过 reset/commit/merge，并清理过临时实例（我的 8916 实例曾被杀）；最终门禁是在干净重启的实例上完成。另：uicheck 的「高级设置页」判据已按第6条改成接受跳转入口，属**主动修改验收用例**，请 Lead 复核（改动已在 0d7b0e8 中）。
7. 排序/收藏只在 4 首测试曲目上验证，未在大库（万级曲目）测性能；收藏优先的次要排序规则为标题（localeCompare zh）。

---

## 六、结论

第 4/5/6/7 条在 `feat/ui-03` 上**均有实测证据**（CDP + uicheck + HTTP）；门禁四项全绿（构建 0 err / ctest 60/60 / 平台边界 exit 0 / uicheck 304 PASS·0 FAIL）。
未验证项见上（主要是不发声硬规矩限制的听感/卷积真实效果，以及 Discogs 离线分支）。
