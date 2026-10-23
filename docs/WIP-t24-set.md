# WIP 261024-01 · #5/#6/#7/#8/#12（分支 feat/t24-set）

> 负责范围：用户反馈 261024-01 的 **#5 流媒体独立页 + 多服务器 + 密码框样式**、**#6 改名与归位**、
> **#7 设置页文案精简**、**#8 标签写回默认开启**、**#12 SDM 比特率/封装灰掉**。
> 工作树：`D:/rlplayer-t24-set`（分支 `feat/t24-set`，基点 main `78f8655`）；
> 测试实例：`build/rtaweb.exe --port 8922`，`RLPLAYER_LIBRARY_DIR=D:/_tmp_lead/t24-set/lib`
> （库根 `D:\RTA-hqplayer\MUSIC`，只读；653 首 / 294 专辑）。
> 用户的 8899、Lead 的 8914、t24-np 的实例全程未碰；本机只操作过自己启动的 8922 实例（PID 均先只读核对端口属主再 `/api/quit`）。
> 安全红线：测试实例 conf 全程 `backend=null`、`volume=-60`，未打开任何真实音频设备、未发声；`MUSIC` 目录只读。

---

## 0. 一句话结论

| 条目 | 状态 | 判据（实测数字） |
|---|---|---|
| #5 流媒体独立页签 | **完成** | 页签顺序 `输入/输出/PCM/SDM/高级/EQ/元数据/流媒体/诊断`（uicheck 断言 PASS） |
| #5 多服务器（列表/增删改/启用/测试） | **完成** | HTTP 实测：add 2 台 → 列表 2 行；enable 切换 active 3→4→3；edit 留空密码保持；delete 后回到 2 台；**重启进程后列表仍在**（v2 conf） |
| #5 密码框样式 | **完成** | 密码框与文本框 computed style 全等：`padding 7px 10px / radius 8px / border 1px solid / bg rgba(6,8,11,.9) / font 12px / 高 33px / 宽 234px` |
| #6 音乐库改名 + DLNA 归位 | **完成** | 高级页可见文字无「音乐库与网络」/「流媒体服务器」；有「本地音乐库」；`#sp_upnp` 只在流媒体页（uicheck 断言 PASS） |
| #7 设置页文案精简 | **完成** | 设置页可见文字 **10,856 → 7,837 字**（−27.8%），另 **2,458 字**收进 **78 个 (?) 悬停提示**；诊断页 342→24 字、元数据 830→259、高级 850→248 |
| #8 标签写回默认开启/去选项 | **完成** | 页面无「可选」字样、显示「默认开启」；`预览写回 / 执行写回` 与说明仍在；后端本无开关键，能力始终可用 |
| #12 SDM 比特率/封装灰掉 | **完成** | 两控件 `disabled=false`；CDP 实测 PCM 下改封装 → 输出模式自动 `dsd`、`sDSD=nat256`、提示条显示；切回 PCM 后仍可选 |

门禁：`cmake --build build` **0 错**（49 条历史弃用警告，未新增）；`ctest -j4` **60/60**；
`tools/check_platform_boundary.ps1` **exit 0（PASS，23 处白名单）**；
`node tools/uicheck.js http://127.0.0.1:8922/` **0 FAIL / 0 JS 异常 / exit 0**（本轮新增 4 条断言）。

---

## 1. 逐条改动（文件:行号）

### #5 流媒体：后端多服务器

**根因/现状**：`SourceManager` 只有一份 `SourceConfig`（`rlplayer-sources.conf` 四行），
而且 `main()` **从未调用过 `SourceManager::load()`** —— 填过的服务器重启后就"消失"，只能重填。
（这解释了用户"当前只能连接一个流媒体"的体感之外，还有一个隐藏的重启丢失 bug。）

- `src/net/source_mgr.h:26` `SourceConfig` 增加 `id / name`，`display_name()` 在未命名时取 URL 主机名；
- `src/net/source_mgr.h:51/88/100/115/130` 列表 CRUD：`servers()/add_server()/update_server(keepPass)/remove_server()/set_active()`；
- `src/net/source_mgr.h:157` 可直接测「未保存的表单配置」或按 id 测已保存的某台（不影响当前启用）；
- `src/net/source_mgr.h:247` `load()`：**v2 格式**（`version=2/nextid=/active=/srv<id>.<字段>`）；
  旧的四行格式在 load 时**只读迁移**成 id=1，下一次保存自然升级为 v2；
- `src/net/source_mgr.h:371` `apply_active_locked()`：按字段比对后再下发，避免每次浏览都重置 Emby 登录 token；
- `src/net/source_mgr.h:387` 写盘（name/base 等做换行清洗；密码仍为明文——沿用旧行为，见"已知项"）；
- `tools/rtaweb.cpp:3671` `/api/src/config` 重写：
  - GET → `{servers:[{id,name,kind,base,user,hasPass,active}], active, kind/base/user(当前启用), configured, cache}`，
    **密码永不回传**（只给 `hasPass`）；
  - POST `action=add|update|remove|active`；`update` 时 `pass` 为空 = 保持原密码；
  - **旧协议兼容**：不带 action、只带 kind/base/user/pass 时更新当前启用服务器（旧 UI/脚本不炸）；
- `tools/rtaweb.cpp:3668` `/api/src/ping` 支持 `?id=N` 与 POST 表单内联测试；
- `tools/rtaweb.cpp:5864` 启动时 `SourceManager::instance().load(path_in_exe_dir("rlplayer-sources.conf"))` —— 修复重启丢失。

conf v2 实测样例（测试实例 build/rlplayer-sources.conf）：

```
version=2
nextid=5
active=3
srv3.name=客厅 NAS
srv3.kind=subsonic
srv3.base=http://192.168.1.10:4533
srv3.user=alice
srv3.pass=pw1
```

### #5 流媒体：前端页签与列表

- `src/net/ui_script.h:3016` 页签新增 `流媒体`（排在 元数据 与 诊断 之间）；
- `src/net/ui_script.h:3239` 新分页 `spane[data-g=stream]`：服务器列表 + 添加表单 + DLNA 发现；
- `src/net/ui_script.h:1759` `renderSrcList()`：每行 = 状态圆点 / 名称 + 类型 chip（Navidrome·Subsonic / Emby）+「已启用」/
  地址 + 用户名 / 行内 `启用 · 测试 · 编辑 · 删除`；空列表有虚线空态引导；
- `src/net/ui_script.h:1811/1820/1831` `loadSrcConfig()/bindSrcPage()/srcSave()/srcTestForm()/srcTestSaved()/discoverDlna()`：
  增删改启用全部即时落盘（流媒体页隐藏底部「保存设置」，因为它不适用）；
- 旧单表单（`sKind/sBase/sUser/sPass/srcSave/srcMsg`）与 `saveSrc()` 整块删除（含 `ui_script.h:2041` 的陈旧绑定），
  避免重复 id / 死代码；`loadRemote` 空态改为「设置 → 流媒体」并直接 `openSettingsTab('stream')`；
- CSS：`src/net/ui_page.h:454-482`（`.srvrow/.srvtag/.srvbtn/.srvform/.srvempty` 与 `.libstats`），
  820px 以下行操作换到第二行（1024 截图正常）。

### #5 密码框样式（用户原话"密码框怎么跟其他框样式不一样？"）

**根因（代码级）**：`src/net/ui_page.h:419` 旧选择器只有
`.sfld select,.sfld input[type=text],.sfld input[type=number]` ——
**`input[type=password]` 一条都没命中**，于是流媒体密码框、DeepSeek Key 落回浏览器默认样式
（方角、原生边框、无内边距、宽度不铺满）。

**修法**：选择器补全 `password/url/search`，并统一 `font-family:inherit;line-height:1.45;color:var(--txt);min-height:31px`；
`src/net/ui_page.h:442` 再给 `.meta-card`（DeepSeek Key 所在卡片，不在 `.sfld` 里）补同一套输入框样式。
实测 computed style 与文本框逐项相等（见 §2.3）。

### #6 改名与归位

- `src/net/ui_script.h:3221-3224`：「音乐库与网络」→ **「本地音乐库」**，只保留
  库统计条（曲目/专辑/扫描线程（生效）/上次扫描）+ 添加目录 + 重新扫描 + 扫描线程数 + 根目录列表；
- 原组里的「发现 DLNA 设备」按钮与结果区 `#sp_upnp/#sp_upnpmsg` 移到 `src/net/ui_script.h:3239` 的流媒体页（`DLNA / UPnP 设备发现` 分组），
  绑定在 `bindSrcPage()`（`ui_script.h:1820`）；
- 说明一处**刻意的范围判断**：输出页的「投放（UPnP）」（把本地音频推到 MediaRenderer）没有搬。
  它属于**输出路由**而不是"库/网络发现"，用户原话指的是「音乐库与网络」里的 DLNA 内容。
  若 Lead 认为要一起搬，移动 `sgroup('投放（UPnP）'…)` 即可（无后端依赖）。

### #7 设置页文案精简（全页）

**机制**（`src/net/ui_script.h:2349` `tip()`、`:2897` `sgroup()`，`src/net/ui_page.h:423/428`）：
- 所有控件/分组的说明默认收进 label 后的 **(?) 图标**（`data-tip`），hover 或键盘 focus 才显示；
  分组标题的提示向下展开（向上会被吸顶页签条盖住，实测踩到过）；`.sgroup` 改 `overflow:visible` 并给 `.sghead` 补圆角；
- 只保留**必要的一句话**常驻（安全警告 / 模式说明），例如：
  「独占 · 事件驱动 · bit-perfect；播放期间会静音系统其他声音」「共享：经系统混音，非 bit-perfect；不支持 DoP/DSD」
  「当前输出 PCM：改动比特率 / 封装会自动把输出切到 DSD（SDM）」；
- **删掉的开发者注释/实现说明**（示例，不是全部）：
  - GPU："自动＝按 rtagpu.dll → rtagpu_dx12.dll → rtagpu_cuda.dll 的顺序挑第一个…并在信号链里写明" → 页面只留「当前状态：走 CPU」，
    DLL 明细进 `title`；
  - SDM："实测 7 阶在低采样率或 Chebyshev 1-bit 下会饱和失稳" → 提示改为「条件不足会自动降为 5 阶」；
  - SDM 源那段"DSD 文件现在由播放器原生读取，读取失败才回退 ffmpeg…" 整段删除；
  - 诊断页的 zip 内容清单 → (?)；页面只留一行「一个 zip 打包全部日志 / 系统信息 / 设置摘要（已脱敏）」；
  - 元数据页"AI 增强用于两件事…缓存 7 天有效"整段 → (?)；
  - EQ / PCM / 输出 / 输入的各段长 hint → (?) 或压缩（`ui_script.h` 对应 sgroup/sel 文案）。
- 动态状态文案也同步压缩：`backendHint()`（`ui_script.h` 内）、相位说明 `bindPhaseHint()` 改为一行
  「预设 X（抽头 N）· 实际输出 Y」、GPU/ASIO 状态各一行（明细进 title）。

**实测前后文字量**（CDP 读 `#setpage .spane` 的 textContent，去掉空白；"提示字数"= `.hint` 可见文本，
改后另计 `.tip[data-tip]`）：

| 分页 | 改前可见字 | 改后可见字 | 变化 | 改后 (?) 个数 |
|---|---:|---:|---:|---:|
| 输出 | 1361 | 916 | −33% | 16 |
| PCM | 2768 | 2368 | −14% | 11 |
| SDM | 3143 | 2665 | −15% | 12 |
| 输入 | 1098 | 912 | −17% | 9 |
| 高级 | 850 | 248 | **−71%** | 7 |
| EQ | 426 | 211 | −50% | 6 |
| 元数据 | 830 | 259 | **−69%** | 11 |
| 诊断 | 342 | 24 | **−93%** | 2 |
| 流媒体（新页） | — | 193 | 新页 | 4 |
| **全设置页** | **10856** | **7837** | **−27.8%** | **78 个，共 2458 字进提示** |

### #8 标签写回默认开启、去掉选项

- `src/net/ui_script.h:3326`：标题从「标签写回 **可选**」改为「标签写回 **默认开启**」，不再有任何开关/复选框；
- `src/net/ui_script.h:3328` 说明压成一行：`补全到的标签 / 封面写回音频文件本身；写前校验音频数据逐位不变，原文件备份到 rlplayer-meta\backups`；
- 「预览写回 / 执行写回」两个动作按钮与轮询逻辑（`wbPreview/wbApply/wbSyncBusy`）原样保留；
- 后端确认：`/api/writeback*` 从来没有开关键，能力一直可用 —— 本轮不需要改后端默认值。

### #12 SDM 比特率 / 封装灰掉

**根因（代码级）**：`src/net/ui_script.h:2844` `applyDsdState()` 里
`e.disabled = !isDsd` —— 只有「输出模式 = DSD」时才允许改；而默认输出是 PCM ⟹ 两个下拉永远灰着。

**修法**：
- `applyDsdState()` 删除 disabled 逻辑，永远 `disabled=false; opacity=1`，并给出 `title`；
- `sp_dsdmodenote`（PCM 时显示）：「当前输出 PCM：改动比特率 / 封装会自动把输出切到 DSD（SDM）」；
- 控件 `onchange`（`ui_script.h` 输出绑定处）：若当前是 PCM，改比特率/封装自动把 `sp_outmode` 切到 `dsd` 并 toast
  「输出模式已切到 DSD（SDM）」，随后 `applyDsd()` 合成隐藏的 `sDSD`（如 `nat256`）；
- 封装的两个选项文案缩短为「DoP（WASAPI / ASIO 均可）」「Native（仅 ASIO）」（原来长到被 select 截断）。

---

## 2. 实测与判据

### 2.1 多服务器 API 全流程（真实 HTTP，不是 mock 数据）

测试实例 8922 上执行（实测输出摘要）：

| 步骤 | 结果 |
|---|---|
| GET /api/src/config（初始） | `servers:[]` |
| POST action=add ×2 | `{"ok":true,"id":"3"}` / `{"ok":true,"id":"4"}` |
| GET | 2 台，`active=3`，每台含 name/kind/base/user/hasPass/active |
| POST action=active&id=4 | ok；GET `active=4` |
| POST action=update&id=3（不传 pass） | ok；conf 里 `srv3.pass=pw1` **未丢** |
| POST action=remove&id=4 | ok；GET 剩 1 台，`active` 回退到 3 |
| **重启进程**（`/api/quit` → 重新启动） | GET 仍返回原服务器列表与 active（v2 conf 生效，修掉了旧版"重启丢配置"） |

### 2.2 测试连接（成功 / 失败两条路径）

- 成功路径：本机起最小 mock Subsonic（`127.0.0.1:8933`，`/rest/ping.view` 回 `{"subsonic-response":{"status":"ok"}}`），
  POST `/api/src/ping`（未保存表单）→ `{"ok":true,"error":""}`；再把该地址保存成服务器后 `GET /api/src/ping?id=5` → `{"ok":true,"error":""}`；
- 失败路径：指向不存在的地址 → `{"ok":false,"error":"请求失败"}`，前端表单显示 `✗ …`，不崩、不卡死；
- ⚠️ **真实 Navidrome / Emby 服务器没有可用实例，未实测成功连接**（仅协议路径 + mock 验证）。见"已知项"。

### 2.3 密码框样式（CDP computed style，表单展开后）

| 元素 | 高 | 宽 | padding | 圆角 | 边框 | 背景 | 字号 |
|---|---:|---:|---|---|---|---|---|
| `sp_src_user`（text） | 33 | 234 | `7px 10px` | 8px | 1px solid | `rgba(6,8,11,0.9)` | 12px |
| `sp_src_pass`（password） | **33** | **234** | **`7px 10px`** | **8px** | **1px solid** | **`rgba(6,8,11,0.9)`** | **12px** |

uicheck 另有一条只读断言：`sp_src_user / sp_src_pass / sp_llmkey` 三项 computed style 字符串**完全相等**
（`padding|radius|bg|border|font-size|font-family`），PASS。

### 2.4 #12 联动（CDP 实测，`LINK_TEST`）

```json
{"before":{"mode":"pcm","dsd":"0","rateOff":false,"pkOff":false},
 "after":{"mode":"dsd","dsd":"nat256","noteShown":false}}
```
即：PCM 下两个控件都可点（`rateOff/pkOff=false`）→ 改封装为 Native → 输出模式自动变 `dsd`、隐藏状态 `sDSD=nat256`，
PCM 专用提示条按设计隐藏；再切回 PCM 后控件仍可点。

### 2.5 流媒体启用切换（CDP + API）

`{"click":{"rows":2,"clicked":true},"after":{"active":"4","servers":["客厅 NAS:-","书房 Emby:on"]}}`
→ 点行内「启用」后 active 3→4；再点第一行「启用」恢复 3→3。列表状态、圆点、chip 同步刷新。

### 2.6 门禁四项（最终一次运行）

| 门禁 | 命令 | 结果 |
|---|---|---|
| 构建 | `cmake --build build` | **exit 0**，0 error（49 条既有 warning） |
| 单测 | `cd build; ctest -j4` | **60/60 passed** |
| 平台边界 | `powershell -File tools/check_platform_boundary.ps1` | **exit 0，PASS**（23 处白名单豁免） |
| UI 自检 | `node tools/uicheck.js http://127.0.0.1:8922/` | **0 FAIL，0 JS 异常，exit 0** |

uicheck 本轮新增 4 条只读契约（`tools/uicheck.js`）：
1. 页签顺序含「流媒体」（`输入/输出/PCM/SDM/高级/EQ/元数据/流媒体/诊断`）；
2. 密码框（流媒体/DeepSeek Key）与文本框 computed style 逐项一致；
3. 流媒体页控件齐全 + 高级页已改名「本地音乐库」且无流媒体服务器组/DLNA 按钮；
4. SDM 比特率/封装 `disabled=false` + 联动说明存在；标签写回「默认开启」且无「可选」。
（并修掉旧断言：文本改到 `data-tip` 后，「AI 增强 / DoP 说明」改为「可见文字 + data-tip」一起读。）

### 2.7 实测环境的一个前置条件（**不是本轮改动引起**）

冷测试实例（exe 目录 `rlplayer-meta` 为空 + 外网 MusicBrainz 超时）上，uicheck 会有 4 条**资料层**断言失败：
艺术家缓存标注（「缓存 · 日期 取回」）、Discogs 版本列表/空态。用 main 工作树已有的
`D:/RTA-hqplayer/build/rlplayer-meta`（99 个缓存文件）只读预热到测试实例后，重跑 **0 FAIL**。
本轮改动不触碰资料层代码；上述 4 条在预热前后行为一致，属测试环境数据依赖。

---

## 3. 截图（`docs/screens/t24-set/`，1576×908 / 1024×640）

| 文件 | 内容 |
|---|---|
| `after-1576-stream.png` / `after-1024-stream.png` | 流媒体页：2 台服务器列表（启用态/类型 chip/行内操作） |
| `after-1576-stream-form.png` / `after-1024-stream-form.png` | 添加服务器表单（密码框与文本框样式一致、留空=不改密码提示） |
| `after-1576-stream-tip.png` | 分组 (?) 悬停提示展开（#7 的交互形态） |
| `after-1576-stream-switched.png` | 点「启用」切到第二台后的列表状态（toast 为操作回执） |
| `after-1576-sdm.png` / `after-1024-sdm.png` | SDM 页：比特率/封装可点（不灰）+ PCM 联动说明 |
| `after-1576-sdm-tip.png` | SDM 字段 (?) 悬停提示（长说明只在 hover 出现） |
| `after-1576-adv.png` | 高级页顶部：外观/GPU（一行状态）/本地音乐库统计条 |
| `after-1576-adv-lib.png` | 「本地音乐库」完整分组（曲目 653 / 专辑 294 / 线程（生效）8 / 根目录） |
| `after-1576-meta.png` / `after-1024-meta.png` | 元数据页：两个开关 + 「标签写回 默认开启」（无「可选」） |
| `after-1576-meta-adv.png` | 高级选项展开：数据源 / DeepSeek Key / 标签写回两按钮 |
| `after-1576-diag.png` / `after-1024-diag.png` | 诊断页：两个按钮 + 一行说明（长清单进 (?)） |
| `before-1576-adv.png` / `before-1024-adv.png` | **改前**：高级页堆着「流媒体服务器」表单 + 「音乐库与网络」+ DLNA |
| `before-1576-meta.png` | **改前**：元数据页整段说明文字 + 「标签写回 可选」 |
| `before-1576-sdm.png` / `before-1024-sdm.png` | **改前**：比特率/封装灰掉（截图里可见 disabled 低透明度） |

> 改前截图复现：同一实例/同一曲库，`git checkout 78f8655 -- src/net/ui_page.h src/net/ui_script.h` 后
> `cmake --build build --target rtaweb`，截图完成再 `git checkout HEAD -- …` 重建。

---

## 4. 自评分（六维度，满分见括号；用户标准：每页 ≥85）

| 页面 | 布局(20) | 对齐间距(20) | 对比度(20) | 信息密度(15) | 一致性(15) | 细节(10) | 总分 | 主要扣分点 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 流媒体（列表+表单） | 18 | 18 | 19 | 14 | 14 | 9 | **92** | 行内按钮为文字按钮，与其它 .btn 略有层级差；(?) 提示会短暂盖住下方按钮 |
| 本地音乐库 | 18 | 18 | 19 | 13 | 14 | 8 | **90** | 分组较长（窄窗需滚动）；「上次扫描 —」在只读索引实例上无值 |
| SDM（#12） | 18 | 19 | 19 | 14 | 14 | 9 | **93** | 字段 (?) 在首行悬停时会盖住分组标题（浮层行为） |
| 元数据（#8） | 17 | 18 | 19 | 12 | 14 | 8 | **88** | 去文案后页面偏空（按用户"文字量太大"要求刻意为之） |
| 诊断（精简） | 17 | 18 | 19 | 12 | 14 | 8 | **88** | 同样偏空，仅保留一行必要说明 |
| 密码框样式 | 19 | 19 | 19 | 15 | 15 | 9 | **96** | —（逐项 computed style 与文本框相等） |

> 评分口径：布局=信息分区与视觉动线；对齐间距=网格/基线/留白；对比度=三主题文字与控件可读性
> （沿用既有 uicheck 的三主题 ≥4.5:1 断言，本轮未破坏）；信息密度=一屏可得的有效信息；
> 一致性=同页控件与全局组件统一；细节=hover/focus/空态/长文本截断。

---

## 5. 未完成 / 已知项 / 给 Lead 的扩展点

1. **多台仍是"库存多台、同时只启用一台"**：浏览/搜索/下载接口（`/api/src/browse|search|cover`）不带 server 参数。
   前端已按用户要求做成"列表 + 启用状态"，切换即变 active。若下一步要多台并行浏览：
   - `SourceManager` 已有 `servers()`、`active_id()`，加 `id` 参数即可；
   - ⚠️ **缓存键必须先加 server 前缀**：`SourceManager::fetch()` 的 `cache_` 只按远端曲目 id 建键，
     两台服务器若出现相同 id 会串缓存/串封面（当前只有一台启用，不会触发）。这是接多台并行前必须先做的。
2. **密码仍是明文存本机 `rlplayer-sources.conf`**（与改动前一致）。本轮只保证：GET 不回传明文、
   UI 编辑时不回填、留空保持原密码。若要加密（DPAPI）建议 Lead 统一做，别两个分支各写一套。
3. **真实 Navidrome / Subsonic / Emby 未实测**（无可用服务器）：成功连接路径只用本机 mock 验证。
   Emby 的登录/浏览/下载依赖真实服务端，属"仅代码审查"。请在合并验收时用你的数播/服务器补一条真机记录。
4. **没有"停用全部"入口**：删掉最后一台即等价停用；如需显式"不连接任何服务器"按钮，后端加 `action=active&id=` 的空值语义即可。
5. **#7 的 (?) 提示是 hover/focus 形态**：桌面 WebView2 正常（已截图）；触摸/无鼠标场景看不到提示，
   若以后有触摸需求需要改成点击弹出。
6. **DLNA 投放（输出页）没有搬**：只搬了「音乐库与网络」里的"发现 DLNA 设备"（#6 的字面范围）。
   「投放（UPnP）」留在输出页，因为它是输出路由；要搬也只是前端 sgroup 位置调整。
7. **#12 的产品语义**：任务期望是"选择即联动切 DSD"，已按此实现（PCM 下改比特率/封装会自动切模式并 toast）。
   如果用户其实只想"预选、不改模式"，把 onchange 里的联动删掉即可（控件仍不灰）。
8. **uicheck 资料层 4 条断言需要预热 metabase**（见 §2.7），与本轮改动无关；CI/新实例请先拷一份缓存或用有网环境。
9. 本轮**没有新增 C++ 单测目标**（保持 ctest 60/60 的数量基线）：多服务器 CRUD 用真实 HTTP + 重启持久化 + mock ping 验证。
   若 Lead 想补单测，建议 `test_source_mgr.cpp` 只测 `load()/save_locked()` 的 v2 ↔ 旧格式迁移与 CRUD，不碰网络。

---

## 6. 复现 / 验证命令（本机）

```powershell
# 构建
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe' `
  -DCMAKE_RC_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/llvm-rc.exe'
cmake --build build

# 测试实例（不出声）
$env:RLPLAYER_LIBRARY_DIR='D:/_tmp_lead/t24-set/lib'
# build/rlplayer-settings.conf 至少：backend=null / volume=-60
build/rtaweb.exe --port 8922

# 门禁
cd build; ctest -j4; cd ..
powershell -File tools/check_platform_boundary.ps1
node tools/uicheck.js http://127.0.0.1:8922/ D:/_tmp_lead/t24-set/uicheck
```

提交（本分支）：
- `2810fae` 后端多服务器 + conf v2 + 开机加载修复
- `8daf462` 密码框/提示组件/列表样式
- `0611899` 设置页 #5/#6/#7/#8/#12 + uicheck
- 末次 `polish(settings)` / `docs(wip)` 见 git log。
