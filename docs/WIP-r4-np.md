# WIP — 261023-04 单曲播放页 + 古典作品融合（r4-np）

> 分支 `feat/r4-np`（起点 main `12a23ac`）｜负责：用户反馈 261023-04 的 **#4 / #5 / #6 / #7 / #9**
> 工作树：`D:/rlplayer-r4-np`｜测试实例：`build/rtaweb.exe --port 8916`（backend=null / NullSink，未碰 8899、未碰 8914）
> 测试库：`RLPLAYER_LIBRARY_DIR=D:/_tmp_lead/r4-np/lib` → `D:\RTA-hqplayer\MUSIC`（653 首 / 294 专辑，只读）
> 交付 commit：`8f38dd2`（功能收尾 + npcheck 工具）、`dd03f42`（首版 wip 快照）

## 0. 结论一览

| 条目 | 状态 | 关键证据 |
|---|---|---|
| #5 单曲页四周黑框 | ✅ 已修 | 8px 实心 `--bg` 外圈；接缝像素差 **1576=1/255、1024=3/255**（阈值 ≤4，肉眼不可见） |
| #6 小窗文字/进度条重叠 | ✅ 已修 | 四行 CSS Grid；两尺寸 hero/data/lyric/ctl **任意两者 rect 不相交**；歌词↔进度条间距 **1576=16px / 1024=12px** |
| #7 多实体 AI 资料 | ✅ 已做 | 歌曲/专辑/艺术家/作品 tab；默认专辑缓存秒回；歌曲切 tab 才请求；作品走 `/api/entity/versions`（8 条版本）；骨架加载态 |
| #4 EQ/IR 芯片 | ✅ 已做 | `/api/settings` eq 非空→chip 出现，点击进 EQ 页；清空→消失 |
| #9 古典作品去导航 | ✅ 已做 | 导航无「古典作品」；单曲页「作品」tab → works 页 → 「← 返回正在播放」 |

## 1. 逐条实现与判据

### #5 单曲播放页黑框 —— 先纠正 1.3.8 的根因判断

- **旧修法的几何错误**：launcher 为了让 `WM_NCHITTEST` 拿到边缘缩放热区，把 WebView2 控件
  **内缩** `kBandInset=5` 逻辑像素（`tools/launcher.cpp:419/567/729`），这条边带由宿主用主题
  `--bg` 上色（`kBgDark/kBgLight/kBgBlue`，与 `ui_page.h` 的 `--bg` 逐字节一致）。
  `#npPage` 只是 WebView **内部**的覆盖层，CSS 的负 inset 只能超出页面视口、超不出 WebView 控件本身
  ⟹ 1.3.8 的「`top:-6px` + padding:6px」在几何上永远盖不到那条缝。不同 DPI 下边带 5~8 物理像素，
  而页面外圈仍是被模糊封面占满的彩色像素 ⟹ 用户看到一圈「黑框」。
- **现在的修法（CSS，不动 launcher 命中测试）**：让页面**最外圈像素自己等于 `--bg`**：
  `.npedge{border:8px solid var(--bg);box-shadow:inset 0 0 64px 22px var(--bg)}`。
  5px 边带（模拟/宿主）与页面外圈都是同一个 `--bg`，接缝两侧同色；从第 8 像素起用内阴影做柔和暗角，
  既盖住缝又不像生硬的黑边。
- **取舍（按要求说明）**：**保留**宿主的 5px 边带与 6dp 边缘命中区（边缘拖拽/缩放热区完全不破坏），
  代价是页面外圈有 8px 的 `--bg` 纯色环 + 64px 暗角（视觉上是播放器的暗角风格，不是黑框）。
  没把 WebView 拉满窗口：那会让 `WM_NCHITTEST` 永远拿不到鼠标，边缘缩放直接失效。
- **判据（tools/npcheck.js）**：把宿主 5px 边带按 `--bg` 模拟成页面外框后截图，沿四边从外向内扫
  8 个采样点 × 每边 8 个位置，读回像素算相邻通道差：
  **1576x908 最大 1/255；1024x640 最大 3/255**（阈值 4/255；近黑面上 3/255 肉眼不可见）。
  另断言 hero/data/lyric/ctl 的 rect 都在距视口边 ≥8px 以内（外圈 8px 内没有内容元素）。
- 截图：`np-1576x908-02-cached-band5.png`、`np-1024x640-02-cached-band5.png`

### #6 单曲页四行布局（任何尺寸互不重叠）

- `#npPage` 改为 `.npgrid` 四行 Grid：**行1 封面+标题区 / 行2 资料区 / 行3 歌词区 / 行4 进度控制区**；
  每行 `min-height:0` + 行内滚动，控件从「绝对定位贴底」改为正常流式行 ⟹ 不可能再压到歌词。
  资料区没有可用资料时整行收起（`.npgrid.withdata` 由 JS 切换），不留空白块。
- 矮窗（≤720px 高）媒体查询压缩封面/字号/间距，但行间距仍 ≥12px。
- **判据（CDP getBoundingClientRect，两尺寸）**：

| 间距 | 1576x908 | 1024x640 |
|---|---|---|
| hero.bottom → data.top | 262 → 278（16） | 166 → 178（12） |
| data.bottom → lyric.top | 487 → 503（16） | 322 → 334（12） |
| lyric.bottom → seek.top | 765 → 781（**16**） | 514 → 526（**12**） |
| lyric.bottom → ctl.top | 765 → 781 | 514 → 526 |

  另用真实 53 行歌词夹具复测两尺寸：`.npline` 53 行、rect 仍不相交（`np-*-03b-lyrics.png`）。

### #7 单曲页「资料」面板（多实体聚合）

- 契约按 Lead 约定消费：`GET /api/entity/summary?kind=track|release|person&name=..&artist=..&lang=zh`
  （未命中 `{"pending":true}` → `fetchSummary` 2s 轮询；完成 `{ok:true,sections,citations,summaryKind,summaryCached,unverified?}`）；
  作品走 `GET /api/entity/versions?kind=work&name=..&composer=..`。
- 分区：**歌曲 / 专辑 / 艺术家**，古典时多一个 **作品**（判定：`composer` 字段（后端若回）＞ genre 含
  Classical/古典/Baroque ＞ 专辑名含「古典」＞ 曲名/专辑名特征 `Symphony/Concerto/Sonata/Caprice/Op. N/奏鸣曲/交响…`）。
- 行为与费用纪律：
  - 打开单曲页自动预取「专辑 + 艺术家」（有缓存秒回，不闪加载行）；
  - **「歌曲」只在首次切到该 tab 时请求**（按曲目缓存；切歌重置）；
  - 「作品」点 tab 才请求 Discogs，列表带年份/标题/指挥/乐团/厂牌/格式 + 复制链接，
    底部「查看 Discogs 版本推荐 →」进入保留的 works 整页；
  - 加载态是**三行骨架 + 「正在整理资料…」**，不是大块空白；失败态给「暂无该实体的资料」+ 重试；
  - 状态标注：右上「已缓存 / 刚刚整理 / 整理中… / 暂无资料」；`unverified=true` 时额外显示
    **「AI 整理（未找到权威来源，仅供参考）」**（本工作树后端不会产生 unverified，见 `6）。
- 古典作品名净化：在线补全可能把曲名写成「原文 (中文译名)」，Discogs 检索前剥掉末尾带 CJK 的括号译名
  （实测不剥会 0 条，剥后 8 条）。
- 截图：`np-*-05-work-tab.png`（8 条版本）、`np-*-03-song-tab.png`、`np-*-04-loading.png`。

### #4 EQ / IR 芯片

- `loadSettings()` 已把 `eq/ir` 写进 `ADV`；`renderNp()` 用 `npEqChipsHtml()` 渲染低调 chip：
  `EQ` / `IR` 两个（有值才出现），tooltip 显示当前文件名，点击关闭单曲页并进入「EQ 与房间校正」。
  设置保存后 `loadSettings` 会重绘单曲页 ⟹ 清空 EQ 后 chip 立即消失。
- 实测：eq 空 → 无 chip；POST eq=… → chip 出现（`np-eq-chip-on.png`）、点击后 `S.view==='eq'`；
  POST eq= 空 → chip 消失（`np-eq-chip-off.png`）。
- ⚠️ 合并提醒：另一 teammate 正在把 EQ 右导航挪进设置页签；**单曲页只有这一处**依赖
  `setView('eq'); loadEqView()`，合并时改成「打开设置-EQ 页签」即可（uicheck 里「导航有 EQ」的断言
  也要同步改，属于合并方要合的一行）。

### #9 「古典作品」不再占左导航

- 删除 `ui_page.h` 的 `data-v=works` 导航项；`setView` 去掉 works 映射；`loadWorksView` 内部实现原样保留。
- 唯一入口：单曲页「作品」tab 的「查看 Discogs 版本推荐 →」→ `npOpenWorks()` → `loadWorksView(title, composer, true)`；
  works 页顶部出现「← 返回正在播放」（`#wBackNp`）可回到单曲页。
- uicheck 断言已改：**「导航里没有『古典作品』」**；作品功能改为直接 `loadWorksView()` 验证（控件/版本列表/复制芯片）。
- 实测：npcheck 输出 `左侧导航没有「古典作品」入口 ["专辑","艺术家","曲目","文件夹","流媒体","EQ 与房间校正","播放队列","最近播放","设置"]`；
  两尺寸都能「作品 tab → works 页 → 返回单曲页」。

## 2. 改动文件

| 文件 | 改动 |
|---|---|
| `src/net/ui_page.h` | `#npPage` 四行 Grid + 资料面板/骨架/暗角 CSS；删左导航 works；新 np 标记结构 |
| `src/net/ui_script.h` | 新「资料」面板状态机（NP_DATA）、EQ/IR chip、古典判定/作品名净化、`npOpenWorks`、works 返回按钮；删除旧 `updateNpSummary/#npSummary` |
| `tools/npcheck.js` | **新增**：单曲页 CDP 验收（双尺寸 × 四态 + rect 判据 + 5px 边带像素判据 + 播放前/后 NullSink 硬门禁） |
| `tools/uicheck.js` | 导航断言改「没有古典作品」；作品页改直接调用；`#npSummary` → `#npData`；新增歌词/进度条不重叠判据；资料开关断言改成不依赖用户开关状态 |

## 3. 门禁四项

| 门禁 | 结果 |
|---|---|
| ① `cmake --build build` | **0 error**（48 warnings，lld-link 成功） |
| ② `ctest --test-dir build -j4` | **100% tests passed, 60/60**，exit 0（49.38 s） |
| ③ `powershell -File tools/check_platform_boundary.ps1` | **exit 0**：[PASS] 内核目录无平台越界（23 处白名单豁免） |
| ④ `node tools/uicheck.js http://127.0.0.1:8916/ …` | **见下方「3.1」**（收尾时回填） |
| 附 `node tools/npcheck.js …` | **见下方「3.2」**（收尾时回填） |

### 3.1 uicheck

**exit 0：界面检查全部通过**（含新增/修改断言：「导航里没有古典作品、仍有 EQ 与房间校正」、
「#npData 资料面板 + tab 条/内容区存在、旧 #npSummary 已移除」、「歌词区与进度条不重叠且间距 >= 12px」、
「资料区与进度条不重叠」；艺术家页/专辑页资料开关断言改为不依赖用户开关状态）。

### 3.2 npcheck（单曲页验收）

**85/85 PASS，exit 0**。关键数字：
- 接缝像素差：1576x908 **maxJump=1 / 255**；1024x640 **maxJump=3 / 255**（阈值 4）。
- 歌词↔进度条间距：1576 **16px**；1024 **12px**（判据 >=12）；hero/data/lyric/ctl 两尺寸两两不相交。
- 真实 53 行歌词夹具：两尺寸都渲染 53 行且不被进度条/标题压住。
- 资料面板：默认 tab=专辑、状态「已缓存」；歌曲 tab 有内容；无缓存实体出现骨架；古典曲目出现「作品」tab，
  两尺寸各渲染 **8 条** Discogs 版本；作品 tab → works 页 → 返回单曲页通过。
- 播放安全：每次 play 前 settings/播放参数双确认 backend=null、volume=-60；每次 play 后
  playback.log 最后一条 PLAY 均含 `"device":"Null 输出（不出声）","deviceId":"null"`。

## 4. 自评分（布局层次 20 / 对齐与间距 20 / 对比度与可读性 20 / 信息密度 15 / 视觉一致性 15 / 细节打磨 10）

> 单曲页两种尺寸 × 四态；截图均在 `D:/_tmp_lead/r4-np/npcheck/`（默认只放本地，是否入库问 Lead）。

| 尺寸/状态 | 布局层次 | 对齐间距 | 对比可读 | 信息密度 | 视觉一致 | 细节 | 总分 |
|---|---|---|---|---|---|---|---|
| 1576x908 默认（缓存） | 17 | 18 | 18 | 13 | 14 | 8 | **88** |
| 1024x640 默认（缓存） | 16 | 17 | 18 | 13 | 13 | 8 | **85** |
| 加载中（骨架） | 18 | 18 | 18 | 13 | 14 | 8 | **89** |
| 歌曲 tab | 17 | 18 | 18 | 13 | 14 | 8 | **88** |
| 作品 tab（8 条版本） | 16 | 17 | 18 | 14 | 13 | 8 | **86** |
| EQ 芯片态 | 17 | 18 | 18 | 13 | 14 | 9 | **89** |

扣分理由：
- 布局层次：封面+标题在左、资料/歌词横贯全宽，宽屏时右侧留白偏多（1576 扣 3）；1024 资料面板高度有限、内容需滚动（扣 4）。
- 对齐间距：统一 12/16px 行距与 52px 页边，但资料面板内部 padding(10/14) 与页边不构成同一栅格（扣 2~3）。
- 对比可读：正文 `--txt2`、提示 `--mut2` 在模糊封面底上可读；极暗环境下 `--mut2` 略弱（扣 2）。
- 信息密度：无歌词时歌词行留白较大（已垂直居中处理）；资料面板内容多时需滚动（扣 1~2）。
- 视觉一致：tab/chip/骨架沿用现有设计语言；封面占位图与库内封面风格略异（扣 1~2）。
- 细节打磨：chip/citation/状态标注齐全；资料面板滚动条在深色底上略明显（扣 1~2）。

## 5. ⚠️ 事故与整改（2026-10-23 23:50 前后）

- **现象**：8916 测试实例对真实设备发声 —— `/api/status` 显示 `playing=true`，设备名 **「扬声器 (3- Portacapture X8)」**，
  `volDb=0`，曲目 TAMUSIC《ASIA sophisticated. -Lunar Stream-》已播约 **50 秒**（0 dB 可闻）。
  发现后立即 `POST /api/stop` 并停实例。
- **根因（测试脚本 bug，非产品代码）**：`tools/npcheck.js` 为验 EQ 芯片只 `POST /api/settings` 了 `eq=...` 一个键；
  当时代码的 `POST /api/settings` 是「截断重写」，于是 backend/volume/metasummary 全被清空。
  随后我重建重启实例时 conf 只剩 `eq=`，backend 回落 wasapi、volume 回落 0 dB，脚本里的 `playPath` 就真的对默认设备播了。
- **补救**：① 立即停止实例与脚本（无孤儿进程）；② 重写 `build/rlplayer-settings.conf`：backend=null / volume=-60 / metasummary=1；
  ③ 核对 conf 键完整性：与 main build 的 conf 相比 **0 个缺失**（多出 llmmonthlycap、streamdecode 两个合法默认键，是整包回写时补上的默认值）；
  ④ Lead 已在 main 修掉产品侧根因（POST /api/settings 改合并写）。
- **硬门禁（已进 tools/npcheck.js，缺一不可）**：
  a. 每次 play 前 `GET /api/settings` 核对，不符就用「整包读改写」修回 backend=null / volume=-60，
     并强制页面内 `ADV.backend='null'`、`S.vol=-60`（`/api/play` 请求本身显式带 backend=null&volume=-60）；
     断言 settings 与播放参数双重确认，不满足直接 FAIL 退出、不发播放请求。
  b. 每次 play 后读取 `build/rlplayer-playback.log` 最后一条 PLAY，断言 `"deviceId":"null"`
     （实测每条都是 `"device":"Null 输出（不出声）"`）。
- 事件后所有实测均通过上述门禁，未再发生发声。

## 6. ⚠️ 未完成 / 风险（铁律 4）

1. **launcher 真实窗口未截图**：本工作树按规矩只跑 8916 Web 实例，没有启动 launcher（RLPlayer.exe）窗口。
   黑框结论的证据是：① 先证明 CSS 负 inset 盖不到 WebView 外的边带（几何事实）；② 用 CDP 按 launcher 的
   `kBandInset=5、--bg` 逐字节模拟边带后做像素测量（1/255、3/255）；③ 核对 launcher 三主题常量与 ui_page 的 `--bg` 一致。
   **未在真实内嵌 WebView2 窗口里肉眼复验**。
2. **unverified=true 分支只有代码路径**：本工作树后端在「无权威材料」时是失败返回
   （实测 `未找到任何相关权威材料，拒绝综合以防幻觉`），不会产生 `unverified`；UI 已按 Lead 契约渲染
   「AI 整理（未找到权威来源，仅供参考）」，但只能算**代码审查**。
3. **资料是否能出内容取决于权威材料覆盖**：测试库里同人/原创专辑大量没有百科/Discogs 条目，实测
   `平行四界Quadimension`、`TAMUSIC`、`Inoch EP Revisited` 等会出现「暂无资料」；这是后端设计（防幻觉），不是 UI 缺失。
4. **Discogs 有网络限流**：并发跑两套验收时作品 tab 曾 0 条；串行后稳定 8 条。npcheck 已内置「接口预热 + 重试」避免假失败，
   真实弱网下作品 tab 仍可能显示「暂无资料 + 重试」。
5. **截图未入库**：默认只放本地 `D:/_tmp_lead/r4-np/npcheck/`，是否放 `docs/shots-r4-np/` 待 Lead 决定。
6. **合并注意**：① EQ 右导航挪设置页签后，npEqChip 的跳转与 uicheck 的「导航有 EQ」断言需要合并方同步调整；
   ② 本分支后端是旧 main（12a23ac），kind=track 已可用，但 `unverified` 字段要等 Lead 后端合入后复验；
   ③ 本分支没有改动设置页/底栏，无冲突面。

## 7. 复跑命令

```powershell
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
cmake --build build
$env:RLPLAYER_LIBRARY_DIR='D:/_tmp_lead/r4-np/lib'
start .\build\rtaweb.exe --port 8916
# 单曲页验收（双尺寸四态 + 接缝 + NullSink 门禁）：参数依次为 默认曲目/古典曲目/有歌词曲目/无缓存曲目
node tools/npcheck.js http://127.0.0.1:8916/ <输出目录> <默认曲目> <古典曲目> <有歌词曲目> <无缓存曲目>
# 整站自检
node tools/uicheck.js http://127.0.0.1:8916/ <输出目录>
```
