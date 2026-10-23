# WIP — 261023-03 #1/#2/#3：AI 资料简介自动化（feat/ai-auto）

> 分支 `feat/ai-auto` / 工作树 `D:/rlplayer-ai`。基线 `f72ff62`（1.3.8），本分支提交 `54b398d` → `172089b` → `f999025`（+ 本文件）。
> 2026-10-23 收尾。真实 DeepSeek 调用做过（见 §4），不是只代码审查。

## 1. 用户要求（原文要点）

- **#2**：只要勾选「使用 AI」，就一律自动生成并保存为缓存；**不要出现「AI 综合」「AI 生成」「生成 AI 简介」这些字样**；生成动画要低调。
- **#3**：复刻 ROON —— 艺术家、专辑、曲目（单曲播放整页）都要有对应介绍。
- **#1** 是引子，无独立动作。

## 2. 改动文件（f72ff62..本分支 HEAD）

| 文件 | 改了什么 |
|---|---|
| `src/net/ui_script.h` | 去 AI 字眼、三页自动简介、低调加载态、无 key 不显示；艺术家页「简介（维基原文）」与「延伸简介（整理结果）」分名 |
| `src/net/ui_page.h` | 单曲播放整页新增 `#npSummary` 容器（低调滚动区，38vh） |
| `tools/rtaweb.cpp` | `/api/entity/summary`：kind=release/album + artist、返回 `summaryKind/summaryCached/summaryAvailable`、命中缓存不再调 LLM、日志可自证；`/api/meta`：单曲页所需 `summaryAvailable/summaryCached/summaryCacheOk/summaryKind/summaryTarget/hasLlmKey/summaryEnabled`；`/api/status`：`hasLlmKey/summaryEnabled`；hasLlmKey 一律读 **exe 同目录** rlplayer-llm.conf（不再依赖 cwd）；null/NAA 后端的信号链 device 名如实写（不再误显示默认声卡） |
| `src/meta/retrieval.h` | Release 简介材料整合 Wikipedia + Discogs release（含 notes）；合成 `max_tokens` 2000→4000（**实测修掉专辑类截断导致的「响应不是合法 JSON 对象」**）；discogs 材料补 `kind/subject` |
| `src/meta/discogs.h` | 解析 release `notes` |
| `tools/uicheck.js` | 旧断言（.ai-sec / AI 生成徽章 / 生成 AI 简介按钮）替换为新断言（无 AI 字样、开关关闭不出现板块、#npSummary 存在且关闭时隐藏）；搜索判据自适应测试库 |

## 3. 验收点逐条自测

1. **设置页文案**：`sp_metasummary` = `在线整理音乐资料简介（按次计费）`，说明改为「开启后进入艺术家/专辑/单曲页自动整理并缓存，同一实体只整理一次（7 天），关闭则不请求、不显示」。全 UI 已 grep 无「AI 综合 / AI 生成 / 生成 AI 简介」（uicheck 断言）。
2. **三页自动**：艺术家页、专辑页、单曲播放整页，开关开 + 有 key 时进入即请求并展示，无按钮；加载态只有一行 `正在整理资料…`（12px 灰字）。
3. **缓存命中不重复花钱**：第二次请求 2ms 返回，`from_cache=true / summaryCached=true`；缓存文件 mtime 不变（§4）。
4. **单曲页显示所属专辑简介**：`kind=release & name=专辑 & artist=专辑艺术家`，不是每首歌一次。
5. **无 key 且无缓存**：`/api/meta` `summaryAvailable=false`，三页都不显示板块、不闪「正在整理资料…」（实测截图 33/34）。
6. **后端**：`kind=release` 与 `kind=album` 均可、带 `artist` 消歧；返回带 `summaryKind/summaryCached/summaryAvailable`。
7. **真实验证**：见 §4（真实 DeepSeek key，少量调用）；截图见 §5。
8. **/api/meta 标志**：缓存过期 + 无 key → 后端离线回退旧缓存（有内容，不空板块）；无 key 无缓存 → 不显示；开关关闭 → `summaryAvailable=false` 且前端不渲染容器。

## 4. 实测证据（真实网络 + 真实 DeepSeek 调用）

测试实例：`D:/rlplayer-ai/build/rtaweb.exe --port 8915`；临时曲库 `D:/_tmp_ai_summary/lib8915`（root=`D:/_tmp_ai_summary/libroot`，3 张 fixture 专辑：ZARD/揺れる想い、ZARD/Isobel Single、不存在艺术家ZZZ/无资料测试专辑）；metabase = `build/rlplayer-meta`；key = `build/rlplayer-llm.conf`（从用户 build 复制，**gitignored，不提交**）。

### 4.1 专辑简介：生成 → 缓存命中
```
/api/entity/summary?kind=release&name=揺れる想い&artist=ZARD
  第一次: ok=true summaryKind=release summaryCached=false from_cache=false sections=4 citations=4  15109 ms
  第二次: ok=true summaryCached=true from_cache=true  2 ms
/api/meta?file=...\01 test.flac
  summaryAvailable=true summaryCached=true summaryCacheOk=true summaryKind=release summaryTarget=揺れる想い hasLlmKey=true summaryEnabled=true
```
缓存文件：`D:/rlplayer-ai/build/rlplayer-meta/entities/d310ba4b0e7c8eed/summary-zh.json`，mtime **2026-10-23 17:01:19.719**（生成后两次命中均未改动）。
播放日志（build/rlplayer-playback.log，gitignored）：
```
17:01:04.612 SUMMARY generate kind=release name=揺れる想い eid=d310ba4b0e7c8eed
17:01:19.720 SUMMARY generated kind=release name=揺れる想い eid=d310ba4b0e7c8eed fromCache=0
17:01:19.722 SUMMARY cache-hit kind=release name=揺れる想い eid=d310ba4b0e7c8eed
```
（17:06:32 / 17:07:08 两次进页面同样只有 cache-hit，无 generate。）

### 4.2 艺术家简介
`kind=person&name=ZARD`：第一次 9.4s / 3 节；第二次 2ms `summaryCached=true`。缓存 `entities/3f95b552c60c0bc0/summary-zh.json`。

### 4.3 边界
- 无权威材料的专辑：`ok=false, error=未找到任何相关权威材料，拒绝综合以防幻觉`（3.4s，不产生 LLM 综合费用）。
- 临时移除 key：`/api/status hasLlmKey=false`；无资料专辑页无板块、无加载行；单曲页 `#npSummary display:none`。
- **max_tokens 修复实测**：2000 时「揺れる想い」「海阔天空」连续 3 次报 `综合结果校验失败: 响应不是合法 JSON 对象`；改 4000 后两者分别 20.0s / 16.8s 通过，`Thriller` 也通过。

### 4.4 三页截图（CDP 真跑，headless Edge，1600×1000）
- `D:/_tmp_ai_summary/shots/30-artist-summary.png` —— 艺术家页：维基简介 + 「延伸简介」自动出现（含 [1][2] 上标与来源芯片）
- `D:/_tmp_ai_summary/shots/31-album-summary.png` —— 专辑页：进入即出现「简介」（4 节、来源芯片）
- `D:/_tmp_ai_summary/shots/32-nowplaying-summary.png` —— 单曲播放整页：`#npSummary` 显示所属专辑《揺れる想い》简介
- `D:/_tmp_ai_summary/shots/33-nokey-album.png` —— 无 key + 无缓存：专辑页无板块、无加载行
- `D:/_tmp_ai_summary/shots/34-nokey-nowplaying.png` —— 无 key + 无缓存：单曲整页 `#npSummary` display:none
- 复现脚本：`D:/_tmp_ai_summary/uishots.js`（不提交仓库）；结果 **UI 三页简介检查全部通过，19/19 PASS，无 JS 异常**。

## 5. 门禁四项（最终代码，2026-10-23 收尾）

| 门禁 | 结果 |
|---|---|
| ① `cmake --build build` | **0 error**（42 warnings，lld-link 成功；上一次链接失败仅因测试实例占用 exe，停实例后通过） |
| ② `ctest -j4`（build 目录） | **100% tests passed, 60/60**，exit 0（dsd_two_stage 31.54s） |
| ③ `tools/check_platform_boundary.ps1` | **exit 0**：[PASS] 内核目录无平台越界（23 处白名单豁免） |
| ④ `node tools/uicheck.js http://127.0.0.1:8915/ D:/_tmp_ai_summary/uicheck` | **exit 0**：界面检查全部通过，含「没有 JS 异常 / console.error」PASS；新增断言（无 AI 字样 / 关闭时不显示板块 / #npSummary）全 PASS |

说明：uicheck 全绿依赖测试实例的临时曲库与已 seed 的 ZARD knowledge 缓存（真实库中没有 Isobel 曲目时旧断言会红，本轮已把搜索判据改为自适应测试库，不再依赖特定曲名）。

## 6. ⚠️ 未验证 / 风险（铁律 4）

1. **只在 `buildtaweb.exe` 测试实例（8915）+ Edge headless/CDP 验证**，未在 8899 用户实例、未在真实 WebView2 内嵌窗口里点过（按硬规矩不干扰用户实例）。
2. **未做跨进程重启后的缓存命中实测**（缓存文件在 build/rlplayer-meta，重启后应可读，但只验了同进程第二次进入）。
3. **同专辑里曲目艺术家 ≠ 专辑艺术家（合辑）时**，单曲页（`/api/meta` 用曲目/元数据补全后的 albumArtist）与专辑页（库的 album artist）可能算出两个 release id → 多生成一份缓存（多一次调用、两条内容）。未去重。
4. **可整理性取决于有无权威材料**：用户曲库以同人/原创专辑为主，维基/萌娘/Discogs 常常没有条目 → 页面按设计**不显示**板块（防幻觉）。未统计用户真实曲库的覆盖率；「所有专辑都有简介」这个预期不一定能达成。
5. 生成失败（网络/校验/无材料）时前端静默移除板块，**没有重试按钮**（用户明确不要按钮）；重进页面会自动重试。
6. 旧缓存文件的 disclaimer 文本里仍可能有「AI 生成…」字样（缓存 JSON 内容），但前端**不显示该字段**（渲染固定新文案「依据上面的来源整理，可能不完整，请以来源为准」），UI 上不出现 AI 字眼；下次 TTL 过期重新生成会换新。
7. 无 key + 缓存已过期：后端走离线回退显示旧内容（有内容，不是空板块）；未单独截图取证，仅代码路径核对（`retrieval_fetch` 的 cfg 空 → have_cache 回退分支）。
8. 单曲整页简介容器高度 38vh（可滚动），未测小窗口/窄屏布局。
9. 真实 API 费用未逐笔核对（rlplayer-llm-usage.json 未查）；本轮真实 DeepSeek 调用约 8~10 次 chat + 若干 search/fetch（均为测试实体，可接受）。
10. `tools/test_cdp_summary.js` 是另一会话留下的未跟踪脚本，未入库，已移到 `D:/_tmp_ai_summary/legacy_test_cdp_summary.js`（如需保留请另议）。
11. 播放验证用 **backend=null（NullSink，不打开任何音频设备）**；PLAY 日志的 device 字段现在如实写「Null 输出（不出声）」。全程未调用 /api/input/start、未对用户听音设备播放。
