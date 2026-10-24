# WIP — 261024-02 #2 / #3 / #4 / #5（分支 feat/t25-np-ui）

> 起点 main `35933b2`（v1.3.12）｜工作树 `D:/rlplayer-t24-np`｜分支 **`feat/t25-np-ui`**
> （说明：`D:/rlplayer-t25-np` 已有别的 worktree 占用 `feat/t25-np`，所以我按"只在自己的工作树/分支改"用了独立分支名）
> 测试实例 `build/rtaweb.exe --port 8931`（**backend=null / volume=-60 / metasummary=1**，未碰 8899 / 8914，未 push）
> 测试库 `RLPLAYER_LIBRARY_DIR=D:/_tmp_lead/t25-np/lib` → `D:\RTA-hqplayer\MUSIC`（只读）
> 截图 `D:/_tmp_lead/t25-np/shots/`

## 0. 结论一览

| 条目 | 状态 | 关键证据 |
|---|---|---|
| #4 歌词滚动（用户："当前滚动是全错的"） | ✅ 已修，实测 | 新 `tools/t25check.js`：解析 4 项 + **两档尺寸 × pos 0/30/60/90%** 的 active 行与居中偏移；居中实测 1576=**-5/0/0/0px**、1024=**-9/0/0/0px**（判据 ≤12px）；seek 后 off=0；手动滚动保护 3s（scrollTop 1564→1564）；歌词↔进度条 18/12px |
| #3 时长 / 曲目列表逻辑 | ✅ 已改，实测 | 专辑页曲目行数 = 本地曲目数（13）、表头「13 首 · 总时长 **47:57**」= Σ本地曲目 **2876.84s**（后端 album.dur 2876.84s）、逐行 m:ss 全合法；**AI 的「02:42（Track 01）」与单行曲目列表不再出现**；歌曲页时长 = 文件真实时长（95.4s → 1:35）；抽屉「专辑」用本地对齐曲目表（13 行） |
| #2 关联 / 引用链接 → 系统浏览器 | ✅ 已改，实测 + 单测 | 新 `src/platform/host_msg.h` + launcher `ShellExecuteW`；`tests/test_host_msg`（ctest `host_msg`）**32 项全过**：http/https 放行，file:/javascript:/data:/vbscript:/ftp:/相对路径/换行注入全拒；页面侧 `postMessage('openurl:<url>')` URL 原样、非法协议 0 消息；fields 里 URL 自动成链、引用 chip 拆「打开 + 复制」 |
| #5 UPnP 一键推送（学 QQ 音乐） | ✅ 已改，实测（假渲染器） | 单曲页「推送到设备」→ 设备弹层（未选设备时）→ `backend=upnp`+`upnpdev` 落盘 → `/api/play` 路由 CastManager → **假渲染器收到 `SetAVTransportURI`+`Play`**、取流 URL `http://127.0.0.1:8912/stream/*.wav`；三处状态一致（单曲页按钮「已推送到 …」/ 底栏 UPnP 徽标 / 流媒体页停止可用）；再点停止 → 恢复推送前 backend（null）并回本地播放；**输出页投放块已整块删除**（无 sp_cast_* 与「投放（UPnP）/投放实时状态」文案），后端下拉仍含 upnp |
| 追加（Lead 指定，属 #1 体感） | ✅ 已做 | 打开单曲页 / 切歌时后台**静默预取当前专辑 release 资料**：仅在 `META.summary==1` 且 hasLlmKey/summaryEnabled 时；**只 1 次**、失败静默、切歌丢弃旧结果、不打开抽屉不改 UI；npcheck 判据同步改为「默认最多 1 条 release 预取，歌曲/艺术家不预取」 |

**门禁（终态，全部实跑）**

| 门禁 | 结果 |
|---|---|
| `cmake --build build` | **0 error** |
| `ctest --test-dir build -j4` | **61/61 通过**（新增 `host_msg`） |
| `tools/check_platform_boundary.ps1` | **exit 0** |
| `tools/npcheck.js`（单曲页 111 项） | **111/111 PASS，exit 0** |
| `tools/uicheck.js`（整站） | **界面检查全部通过，exit 0** |
| `tools/t25check.js`（本轮新判据 64 项） | **64/64 PASS，exit 0** |

## 1. #4 歌词滚动 —— 根因三条，一起修

旧实现"全错"的三个代码级根因（`ui_script.h`）：

1. `parseLrc` 不支持 `[offset:±ms]`；百分秒/毫秒的换算只按"长度"判断（`[mm:ss.xxx]` 会算错）；
2. **滚动用 `el.offsetTop`**，但 `.nplyric2` 没有 `position` ⟹ offsetParent 不是滚动容器，
   `offsetTop` 带着整页偏移，`scrollTop = offsetTop - clientHeight/2 + …` 算到一个荒唐位置；
3. 纯文本歌词（`t=-1`）被当成"有时间轴"，`pos` 一到高亮就钉在最后一行并把歌词滚到底。

现在的实现：

- `parseLrc`：标准 LRC（`[mm:ss]` / `[mm:ss.x]` / `[mm:ss.xx]` / `[mm:ss.xxx]` / `[mm:ss:xx]`、
  **一行多时间戳**、`[offset:±ms]`（正值=提前，时间戳减去）、`[ti:]/[ar:]/[by:]` 元信息跳过）；
  没有任何时间戳时保留为纯文本行（`t=-1`）。
- 滚动：`lyricScrollTo()` 用 `getBoundingClientRect()` 相对滚动容器算目标位置（与 offsetParent 无关），
  `scrollTo({behavior:'smooth'})` 平滑过渡；active 行**垂直居中**。
- 首行/末行也能居中：`.nplyric2::before/::after` 各留 `calc(50% - 22px)` 空白（否则第一行贴顶、
  最后一行贴底，"居中"在两端的数学目标不可达 —— 这是判据逼出来的真实缺陷）。
- 无时间戳：不高亮、不自动滚（写进这里：**无时间戳的纯文本歌词不做自动滚动**）。
- 手动滚动保护：`wheel`/`touchstart` → 3 秒内不自动抢滚动；seek / 切歌**立刻**清保护并重定位
  （`doSeek()` 统一入口 + 进度跳变 >2.5s 视为 seek）。
- 右栏歌词（`#lyricbox`）共用同一套居中/保护逻辑。

**判据（tools/t25check.js，两档尺寸）**

| 项 | 1576x908 | 1024x640 |
|---|---|---|
| pos 0% active 行 / 期望 | 0 / 0 | 0 / 0 |
| pos 30% | 23 / 23 | 23 / 23 |
| pos 60% | 44 / 44 | 44 / 44 |
| pos 90% | 67 / 67 | 67 / 67 |
| 居中偏移（0/30/60/90%） | **-5 / 0 / 0 / 0 px** | **-9 / 0 / 0 / 0 px** |
| 歌词↔进度条间距 | 18px | 12px |
| seek 后 | idx=68=exp，off=0，guard=0 | — |
| 手动保护 | scrollTop 1564→1564（不抢） | — |

⚠️ 判据口径：测试时用页内 mock **冻结 `/api/status` 的 pos**（400ms 轮询会把测试设的 S.pos 覆盖回真实值），
这是测试手段，不是产品行为。

## 2. #3 时长 / 曲目列表 —— 本地曲库是唯一真相源

- 后端 `/api/lib/album` 本来就返回 `dur`（Σ 本地曲目）与逐曲目 `dur`，**无需改后端**。
- 专辑页：hero 副标题与曲目表头都用 `fmtDurLong`（`1:05:24` 形式）；曲目列表新增表头
  「曲目列表 · N 首 · 总时长 X」；每行时长 m:ss。
- AI fields：`renderFieldGrid` 新增 `opts.local`（本地覆盖）与 `opts.drop`（本地已有专门展示位则丢弃 AI 项）：
  专辑页 `时长` 用本地总和、`曲目列表` 直接不渲染（页面上的本地表格就是曲目列表）；
  歌曲页 `时长` 用 `S.dur`（文件真实时长）；抽屉「专辑」标签用本地曲目表（三列对齐）。
- 本地缺失时才回退 AI（并在 WIP 声明；当前实现里"回退"指字段仍显示 AI 值，未额外加"AI 推测"后缀 ——
  本地曲库覆盖所有本地曲目，实际不会走到）。

**判据（实测数）**：`Σ 本地曲目 = 2876.842s` vs `album.dur = 2876.84s`（差 0.002s）；
页面 13 行 = 本地 13 首；表头「13 首 · 总时长 47:57」；每行 `^\d+:\d\d$` 全过；
页面正文无「02:42（Track 01）」；歌曲页「1:35」= `S.dur 95.41s`；抽屉专辑 13 行。

## 3. #2 关联 / 引用链接 → 系统浏览器

- `launcher.cpp` 的 `onMessage` 改用 `src/platform/host_msg.h` 的纯函数解析：
  `openurl:<url>` → 白名单（**只 http/https**）→ `ShellExecuteW`；被拒/成功都写 `launcher.log`
  （`openurl OK: … ShellExecute=…` / `openurl REJECTED: … (reason: scheme not allowed)`）。
- 页面：`openExternal(u)`（白名单 → `wcmd('openurl:'+u)`；无 WebView2 时 `window.open` 兜底）；
  `renderFieldGrid` 的值里出现 http/https 自动包成 `.eflink`（事件委托，重绘不用重绑）；
  引用 chip 拆成「打开浏览器」主按钮 + 「复制」小按钮（原来的复制行为保留）。
- 判据：ctest `host_msg` 32 项（协议 + 白名单 + URL 原样）；t25check：`openurl:<url>` 原样、
  `javascript:`/`file:` 返回 false 且**不产生消息**、`.eflink` 点击产生 `openurl:https://…`、
  `.chip-open` 打开 + `.chip-copy` 仍在。

⚠️ **未覆盖**：`ShellExecuteW` 真的拉起系统浏览器属 Win32 副作用，**不进自动化**；
真实 WebView2 窗口下的 `launcher.log`「openurl OK」记录需实机跑一次（本轮未做，代码级判据已覆盖协议与 URL）。

## 4. #5 UPnP 一键推送（学 QQ 音乐）

- **单曲页**：元数据芯片行新增「推送到设备」按钮（`#npCastBtn`）。未选设备 → `/api/cast/devices?scan=1`
  → 复用 `#modal` 弹设备列表 → 选中后 `ADV.backend='upnp'` + `ADV.upnpDev=<描述URL>` → `saveSettings()`
  → 当前曲 `playPath()`（`params()` 带 backend/upnpdev）⟹ 后端把 /api/play 路由给 CastManager。
  按钮变「已推送到 <设备>」（绿色高亮）；**再点 = `/api/cast/stop` + 恢复推送前的 backend**
  （刻意记 `prevBackend` 而不是硬编码 wasapi：测试实例是 null，硬编码会把安全设置冲掉）→ 回本地播放。
- **底栏**：新增 `#castBadge`（UPnP，投放中显示）。
- **设置 → 流媒体页**：新增「推送到设备（UPnP / DLNA）」区块 —— 设备下拉 + 重新发现 +
  「推送当前曲」/「停止投放」+ 实时状态网格（阶段/渲染器/进度/传输状态/供源字节/请求数/端口/采样率取证/告警/错误）。
  旧的"只发现 DLNA 设备"按钮被它取代。
- **设置 → 输出页**：**整块删除**「投放（UPnP）」与「投放实时状态」（用户：多余）；后端下拉仍保留 UPnP，
  选中时出现「目标渲染器」下拉。
- **三处状态同源**：`applyCastUi(st)` 从 `/api/cast/state`（1 秒轮询，投放中/单曲页打开/流媒体页打开时轮询）。

**判据（tools/mock_upnp_renderer.js，不出声、不碰真实数播）**

| 判据 | 实测 |
|---|---|
| 未选设备先弹列表 | modal.on=true，1 行设备 |
| 推送后 `/api/cast/state` | running=true，phase=playing，device=Mock-N1000-Renderer |
| 假渲染器调用 | `SetAVTransportURI` → `Play` →（GetProtocolInfo/GetTransportInfo/GetPositionInfo…） |
| 取流 URL | `http://127.0.0.1:8912/stream/866e729a74e8e204.wav` |
| 落盘 | kv.backend=`upnp`，kv.upnpdev=`http://127.0.0.1:49199/description.xml` |
| 三处 UI | 单曲页「已推送到 Mock-N1000-Renderer · …」/ 底栏徽标 display=flex / 流媒体页 stop.disabled=false |
| 再点停止 | running=false，kv.backend 恢复 `null`（推送前的值），按钮/徽标回未投放态 |
| 输出页 | 无 sp_cast_start/file/dop/usecur/scan/stop/stat_*，无「投放（UPnP）/投放实时状态」文案；backend 下拉含 upnp |

## 5. 截图（两档 1576x908 / 1024x640）

| 画面 | 文件 |
|---|---|
| 单曲页 · 歌词精确同步（active 居中） | `shots/t25-lyrics-1576x908.png`、`shots/t25-lyrics-1024x640.png` |
| 单曲页 · 默认态（含「推送到设备」按钮） | `shots/t25-np-default-1576x908.png`、`shots/t25-np-default-1024x640.png` |
| 单曲页 · 抽屉「专辑」标签（本地曲目表） | `shots/t25-np-album-tab.png` |
| 专辑页 · 本地总时长 + 曲目表 + AI 资料 | `shots/t25-album-1576x908.png`、`shots/t25-album-1024x640.png` |
| 设置 · 流媒体页「推送到设备」区块 | `shots/t25-stream-tab.png` |
| 设备选择弹层 | `shots/t25-cast-picker.png` |
| 投放中（单曲页按钮 / 底栏徽标） | `shots/t25-cast-on-1576.png`、`shots/t25-cast-on-1024.png` |
| 停止投放后 | `shots/t25-cast-off-1576x908.png` |

## 6. 六维自评分（每页自己截图、附扣分理由）

| 维度（满分） | 单曲页·歌词 | 单曲页·投放态 | 专辑页 | 流媒体页·推送 | 输出页 |
|---|---|---|---|---|---|
| 布局 20 | 18 | 18 | 18 | 17 | 18 |
| 对齐间距 20 | 18 | 18 | 18 | 18 | 18 |
| 对比度 20 | 17 | 18 | 18 | 17 | 18 |
| 信息密度 15 | 13 | 13 | 14 | 13 | 12 |
| 一致性 15 | 14 | 14 | 14 | 13 | 14 |
| 细节 10 | 9 | 9 | 8 | 8 | 8 |
| **合计** | **89** | **90** | **90** | **86** | **88** |

扣分理由：
- 歌词页 -2 布局：active 行下方到控制区之间空白偏多（歌词行数少时更明显）；-2 对比度：未唱到的行仍是 --mut（暗底上偏灰，刻意做的层次）；-2 密度：纯歌词页本身信息量低。
- 投放态 -2 布局/密度：按钮文案「已推送到 Mock-N1000-Renderer · RLPlayer Mock Renderer」较长（真实设备名也长），在窄窗里会挤占芯片行；未做文案截断。
- 专辑页 -2 细节：曲目表头与第一行之间只有 4px；AI 资料卡与曲目表的区块边界偏弱；-1 密度：AI 资料仍有 3 个宽行。
- 流媒体页 -3 布局/-3 对比度：推送区块的实时状态网格在窄窗下会折成 2 列（有 media query 但只保证 ≥1 列可读）；「重新发现」按钮与下拉的基线下沉 1px（视觉可辨但不明显）；-2 密度：状态网格 8+2 项略多（但用户要求实时状态齐全）。
- 输出页 -3 密度/-2 细节：删掉投放块后右侧内容偏空（只到「声道音量微调」），但这是用户要求的"精简"，按密度打分吃亏。

## 7. 未完成 / 风险（如实记录）

1. **launcher 真实链路只到代码 + 单测**：`ShellExecuteW` 会真的拉起系统浏览器，不能进自动化；
   真实 WebView2 窗口下 `launcher.log` 有 `openurl OK` 记录这一条**未实机验证**（协议白名单与 URL 正确性已由 ctest+t25check 覆盖）。
2. **预取的真实收益未量化**：本实例没有 LLM key，预取在契约 mock 下验证了「只发 1 条 release 请求、不打开抽屉、不改变 UI」
   （npcheck 111 项），真实 DeepSeek 下"点专辑从 10~30s 降到命中缓存"的耗时**未测**（需要 key，属 Lead 侧 #1）。
3. **UPnP 只对假渲染器验证**：未对真实数播发 `Play`（按任务要求）；放大器/真实设备的重采样与音质取证需用户在授权下实测。
4. **#4 未做长时播放观察**：判据是"设 pos → 看高亮/居中"，没有连续播放 30 分钟看是否漂移；平滑滚动在低端机可能掉帧。
5. **主题覆盖**：截图只在深色主题；light/blue 有 CSS 覆盖但无截图证据。
6. **EQ 页未碰**：UI-eq 的 20 段 PEQ 改的是 `ui_script.h` 的 EQ 段，本轮改动集中在资料/歌词/投放区块；合并冲突由 Lead 处理。
7. 旧 `tools/test_web_upnp_backend.js` 仍可直接跑（后端路由未被本轮改动），本轮新增的是 UI 层判据 `tools/t25check.js`。

## 8. 复跑命令

```powershell
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe' `
  -DCMAKE_RC_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/llvm-rc.exe'
cmake --build build; ctest --test-dir build -j4; powershell -File tools/check_platform_boundary.ps1

$env:RLPLAYER_LIBRARY_DIR='D:/_tmp_lead/t25-np/lib'   # rlplayer-library.conf.p8931 → D:\RTA-hqplayer\MUSIC
.\build\rtaweb.exe --port 8931                        # build/rlplayer-settings.conf: backend=null/volume=-60/metasummary=1

node tools/npcheck.js http://127.0.0.1:8931/ D:/_tmp_lead/shots/np '<Declan 曲目>' - '<有歌词曲目>' '<同上>'
node tools/uicheck.js http://127.0.0.1:8931/ D:/_tmp_lead/shots/ui
node tools/t25check.js http://127.0.0.1:8931/ D:/_tmp_lead/shots '<有歌词曲目>'   # 自带 mock_upnp_renderer（49199）
```

## 9. 提交（分支 feat/t25-np-ui，未 push）

| SHA | 内容 |
|---|---|
| `1778f9e` | feat(host)：#2 postMessage openurl + http/https 白名单（host_msg.h + launcher + ctest） |
| `bb6f981` | feat(ui)：#2/#3/#4/#5 UI 实现（ui_page.h + ui_script.h，含 Lead 追加的专辑预取） |
| `301dd59` | test(ui)：npcheck/uicheck 判据同步 + 新增 tools/t25check.js + mock_t25.js |
| `faa678f` | docs(wip)：本文件 |
