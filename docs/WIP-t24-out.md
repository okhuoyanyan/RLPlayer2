# WIP · 261024-01 #9 #11（分支 feat/t24-out，基线 main 78f8655）

> 负责人：t24-out（子代理）　日期：2026-10-24　工作树：`D:/rlplayer-t24-out`
> 本文件按「改法 / 判据 / 截图 / 自评分 / 未完成」记录，未实测的项一律显式标注。
> 用户原文见 `D:/RTA-hqplayer/反馈和开发/develop/261024-01.txt` 第 105、107 行：
> - #9「输出，后端没有 UPNP 选项，那你是怎么测试连接我的数播的？漏了就给我加上」
> - #11「NAA 端点（host:port）这个选项不应该出现啊，应该是自动获取的啊」

---

## 1. 改动总览

| 文件 | 改动 |
|---|---|
| `tools/naa_discover.h`（新） | NAA 端点发现实现：枚举本机物理网卡 /24 → 并发探测 TCP 43210 → 组播被动监听；**过滤代理 TUN fake-IP**。CLI 与 Web 共用这一份实现（避免两套判据漂移） |
| `tools/naa_scan.cpp` | 改为调用 `naa_discover.h`（原探测逻辑抽出；保留 `--subnet/--timeout/--listen` 参数）。支持自动枚举本机网段（不再硬编码 192.168.50.） |
| `CMakeLists.txt` | rtaweb / naa_scan 链接 `iphlpapi`（GetAdaptersAddresses / GetBestInterfaceEx） |
| `src/net/cast_service.h` | `CastState` 增加 `durationSec/sampleRate/bits/paused`；新增 `set_paused()/seek_seconds()`，在 worker 轮询循环里发 AVTransport Pause/Play/Seek REL_TIME（HTTP 线程只写原子量，不在请求里阻塞） |
| `tools/rtaweb.cpp` | ① 后端 `upnp`：`/api/play`、`/api/skip` 转交 `CastManager`；`/api/pause`、`/api/seek`、`/api/stop`、`/api/clear` 映射到同一投放会话；`/api/status` 在投放模式下以会话为唯一状态源。② `/api/naa/discover`：后台线程扫描 + 700ms 轮询 + 30s 缓存。③ `upnpdev` 进设置白名单 |
| `src/net/ui_script.h` | ① 后端下拉新增「UPnP / DLNA 投放（局域网渲染器）」；选中后显示目标渲染器下拉（与「投放」区块同一个选择、同一条会话）。②「NAA 端点（host:port）」文本框默认不再出现：改为自动发现下拉 + 「重新发现」按钮，手动输入收进**默认折叠**的兜底。③ UPnP 后端下隐藏本机专用控件（设备/采样率/位深/声道/缓冲/渐变/输出模式/声道微调） |
| `tools/mock_upnp_renderer.js`（新） | 本地假 UPnP MediaRenderer（description.xml + AVTransport/ConnectionManager SOAP + /__state 调用记录），**不打开任何音频设备** |
| `tools/test_web_upnp_backend.js`（新） | 无声端到端：自起假渲染器 + 自造 2s 静音 WAV，验证 16 项判据 |
| `tools/test_web_naa_discover.js`（新） | 自动发现正/负路径 + TUN fake-IP 过滤 + 缓存复用判据 |

---

## 2. #9 后端 = UPnP / DLNA（实现与判据）

### 2.1 设计（不另起一套控制）
- 后端下拉选中 `upnp` 后，**播放启动路径**把曲目交给现有 `CastManager`（cast_service.h：本地 HTTP 供源 + AVTransport SetAVTransportURI/Play），不启动本地播放线程。
- 一条会话、一个状态源：`/api/status` 在投放模式下直接取 `CastManager` 的进度/时长/格式/暂停状态。本地播放与投放互斥（开始任一方都会停掉另一方），避免"声卡 + 网络渲染器同时出声"。
- 渲染器选择与「投放」快捷区块共用同一下拉值（设备描述 URL）。后端 = UPnP 时「投放」区块不再重复渲染器下拉，只留一句说明。
- 失败必须如实报错：未选渲染器直接返回 `{"ok":false,"error":"未选择 UPnP / DLNA 渲染器…"}`，不假装出声。

### 2.2 已实测（无声 · 本地假渲染器）
命令：`node tools/test_web_upnp_backend.js http://127.0.0.1:8923` → **PASS（16/16）**

| 判据 | 结果 |
|---|---|
| `/api/play`（backend=upnp）→ 会话建立、phase=playing | PASS |
| `/api/cast/state`：device=Mock-N1000-Renderer / durationSec≈2.0 / 44100 Hz / 16 bit | PASS |
| SetAVTransportURI 的 URI 是本机供源 `http://…/stream/<hash>.wav` | PASS |
| `/api/status`：playing=true / current=曲目 / outRate=44100 / outBits=16 / dur≈2.0s | PASS |
| `/api/pause?on=1/0` → 渲染器 PAUSED_PLAYBACK / PLAYING，status.paused 同步 | PASS |
| `/api/seek?pos=0.5` → AVTransport Seek REL_TIME 到 ≈1s（2s × 0.5） | PASS |
| `/api/skip?dir=1` → 重新下发 SetAVTransportURI（停旧会话 + 起新会话） | PASS |
| `/api/stop` → 渲染器 Stop、会话 running=false、status.playing=false | PASS |
| 未选渲染器 → ok=false 且文案含「渲染器」 | PASS |
| 投放在播时切本地播放 → 投放会话被停（一条会话原则） | PASS |
| `upnpdev` 设置落盘（/api/settings kv） | PASS |

### 2.3 真机联调（**未测，明确标注**）
- 按任务分工，真实数播（192.168.50.109 N1000_UPNP）的**实际发声**由 Lead 在用户授权后验证；本工作树**没有**向真实设备下发过 SetAVTransportURI/Play。
- 设备发现这一层已在本机实测：`GET /api/cast/devices?scan=1` 返回 **1 台 N1000_UPNP（UpMPD，192.168.50.109:49152，AVTransport 控制 URL 就绪）**，UI 下拉能选到（见截图 out-upnp-*.png）。
- 仍待真机确认：本机 → 设备方向的 TCP 供源是否可达（既有结论见 `docs/P5-UPnP端到端实测-2026-10-23.md`：受 AP/客户端隔离影响，设备侧曾连不进本机 8912；这是环境问题，不是代码路径）。

---

## 3. #11 NAA 端点自动获取（实现与判据）

### 3.1 设计
- 输出页不再默认显示「NAA 端点（host:port）」文本框。后端 = NAA 时显示**自动发现下拉** + 「重新发现」；手动输入放进 `<details>` 折叠兜底（默认闭合，界面上不绘制）。
- 服务端 `/api/naa/discover` 异步扫描（Web UI 是单线程 accept，同步扫 254 主机最长 ~3s 会把 /api/status 全堵住）：后台线程 + 前端 700ms 轮询 + 结果缓存 30s。
- 发现机制与既有 `docs/NAA发现纪要.md` 一致：NAA 协议无公开发现报文，用 **TCP 43210 是否开放**作判据；先探 127.0.0.1（本机可跑 networkaudiod / rtanet_rx），再扫本机每个物理网卡所在 /24，最后在无结果时做 1.5s 组播被动监听（224.0.0.199 / 239.192.0.199）。
- **未发现时如实显示原因**：`未发现 NAA 端点（扫描 192.168.50.0/24:43210 无响应）：NAA 服务未开启（数播开 UPnP 会关掉 NAA）、不同子网或防火墙拦截。`

### 3.2 本机踩到的坑（已修，值得保留记录）
- 本机装了 Clash/Meta TUN（fake-IP 198.18.0.0/15，且**劫持了去往 192.168.50.0/24 的部分路由**）。第一版端口探测把 **255 个不存在的地址**全判成"开放 43210"，直接把下拉刷屏。
- 三条判据叠加后修正：
  1. 只取 **Ethernet(6) / Wi-Fi(71)** 类型网卡（TUN=131 直接排除）；
  2. 显式跳过 198.18.0.0/15、198.19.0.0/24（RFC 2544 保留段）；
  3. 每个候选先问 `GetBestInterfaceEx` 最优路由是否走物理网卡，connect 成功后再用 `getsockname` 复核源地址不在 fake-IP 段。
- 修复后：整段 /24 扫描 0 假端点；本机起 43210 监听 → 立刻报出 `127.0.0.1:43210`。

### 3.3 已实测
命令：`node tools/test_web_naa_discover.js http://127.0.0.1:8923` → **PASS（9/9）**

| 判据 | 结果 |
|---|---|
| 负路径：设备侧 NAA 未开启（当前设备在 UPnP 模式）→ endpoints=[] 且 note 以「未发现 NAA 端点」开头、含「UPnP 会关掉 NAA」 | PASS |
| 返回扫描网段列表、探测主机数 > 250（整段 /24） | PASS |
| 正路径：本机 43210 起监听 → 自动发现 `127.0.0.1:43210` | PASS |
| 所有端点端口均为 43210 | PASS |
| 198.18/15 fake-IP 段不出现在端点列表 | PASS |
| 30 秒内复用缓存（实测 1ms 返回） | PASS |
| CLI `naa_scan.exe --listen 1`：自动选网段、0 假端点、组播 1s 无报文 | PASS（见下方输出节选） |

**未测（明确标注）**：把数播切到 NAA 模式后的真端点发现与 RTANet 握手 —— 按分工由 Lead 在用户授权下做。另外 NAA 5.x 控制协议仍未逆向（既有事实，见 `docs/NAA对接探测纪要.md`），本程序连真 networkaudiod 会明确报"协商失败"，不会假装连上。

---

## 4. 门禁结果（全部在本工作树实测）

| 门禁 | 结果 |
|---|---|
| `cmake --build build` | **0 error**（clang 21 / Ninja；仅既有 deprecated 警告） |
| `ctest -j4`（build 目录） | **60/60 passed**（43.87 s） |
| `powershell -File tools/check_platform_boundary.ps1` | **exit 0**，`[PASS] 内核目录无平台越界`（新代码未新增 src/net 平台依赖；winsock 相关实现放在 `tools/naa_discover.h`） |
| `node tools/uicheck.js http://127.0.0.1:8923/` | **界面检查全部通过**（0 FAIL；`没有 JS 异常 / console.error` PASS） |
| `node tools/test_web_upnp_backend.js` | **PASS 16/16** |
| `node tools/test_web_naa_discover.js` | **PASS 9/9** |

> 测试实例：`build/rtaweb.exe --port 8923`，`RLPLAYER_LIBRARY_DIR=D:/_tmp_lead/t24-out/lib`，
> `rlplayer-library.conf.p8923` = `D:\RTA-hqplayer\MUSIC`（只读库）；
> 实例 conf：`backend=null`、`volume=-60`（铁律 1：测试默认不可听）。全程未打开任何音频设备。

---

## 5. 截图（1576×908 与 1024×640）

目录 `D:/_tmp_lead/t24-out/shots/`：

| 文件 | 内容 |
|---|---|
| `out-upnp-1576.png` / `out-upnp-1024.png` | 输出页 · 后端 = UPnP / DLNA（目标渲染器 = N1000_UPNP） |
| `out-naa-1576.png` / `out-naa-1024.png` | 输出页 · 后端 = NAA（自动发现下拉、未发现原因、手动兜底折叠） |
| `out-castblock-1576.png` / `out-castblock-1024.png` | 「投放（UPnP）」快捷区块 + 投放实时状态 |
| `out-default-1576.png` | 对照：后端 = Null（本机字段全部回来的正常形态） |

---

## 6. 自评分（六维，满分 20/20/20/15/15/10 = 100，门槛 ≥85）

### 输出页 · 后端 = UPnP / DLNA（1576 + 1024）
| 维度 | 分 | 依据 |
|---|---|---|
| 布局 | 18/20 | 卡片内只保留相关字段（后端 + 目标渲染器 + 说明），无空标签、无跨页错位；组头描述仍是通用文案（"独占 / 共享"），对 UPnP 语境不够贴合 |
| 对齐间距 | 18/20 | 4 列栅格与全宽行混排对齐；select 与「重新发现」8px；提示与控件 6px；两档分辨率均无换行挤压 |
| 对比度 | 19/20 | 深色底正文/提示/主按钮均 ≥4.5:1（uicheck 三主题对比度断言 PASS）；次要说明用 mut2 仍可读 |
| 信息密度 | 14/15 | 隐藏了 7 个与投放无关的控件；「投放」区块仍保留源文件/DoP 两行（作为快捷入口有意保留） |
| 一致性 | 14/15 | 目标渲染器与「投放」区块同值同步；状态唯一来源；按钮/下拉/提示样式与其它页一致；UPnP 下快捷区块只剩一行说明，略空 |
| 细节 | 9/10 | 长设备名不截断、发现失败有明确空态与原因、无 JS 异常；"共发现 N 个渲染器"与上方说明略有重复 |
| **合计** | **92** | ≥85 |

### 输出页 · 后端 = NAA（1576 + 1024）
| 维度 | 分 | 依据 |
|---|---|---|
| 布局 | 18/20 | 端点下拉 + 重新发现按钮同行；手动兜底折叠；DSP 相关字段（采样率/位深/声道/缓冲）保留、设备选择隐藏 |
| 对齐间距 | 18/20 | 与 UPnP 页同一套栅格；折叠摘要与黄色结论行间距正确 |
| 对比度 | 19/20 | 黄色告警（warn）在深色底可读；正文层级清晰 |
| 信息密度 | 14/15 | 未发现时只给一句原因（已去掉与后端提示重复的第二遍说明） |
| 一致性 | 14/15 | 与 UPnP 相同的「下拉 + 重扫」交互；NAA 与 RTANet 的现状在提示里如实写明 |
| 细节 | 9/10 | 手动框默认**不绘制**（命中测试证明）；只剩协议未逆向这一既有限制（提示已注明需 rtanet_rx） |
| **合计** | **92** | ≥85 |

### 「投放（UPnP）」快捷区块（1576 + 1024）
| 维度 | 分 | 依据 |
|---|---|---|
| 布局 | 18/20 | 后端=UPnP 时不再重复渲染器下拉/扫描按钮，只留说明；非 UPnP 后端时保持原完整形态 |
| 对齐间距 | 18/20 | 与输出页其它行同一栅格；按钮组与说明间距一致 |
| 对比度 | 19/20 | 主按钮/次按钮/禁用态对比清晰 |
| 信息密度 | 14/15 | 快捷入口保留必要项（文件、DoP、起停）；无多余解释文本 |
| 一致性 | 14/15 | 目标渲染器与「输出后端」同源；停止/状态与投放面板同步 |
| 细节 | 9/10 | 修掉「用当前播放文件」读错字段（`j.cur` → `j.current`）的老 bug |
| **合计** | **92** | ≥85 |

---

## 7. 未完成 / 已知限制（铁律 4）

1. **投放不经过本机 DSP 链**：CastManager 按「源文件原采样率」解码成 WAV（DSD 走 DoP）供源，升频 / 滤波器 / EQ / 音量 / 声道微调都不参与。所以后端 = UPnP 时这些控件被隐藏，后端提示里也写明。要"把 DSP 处理后的流实时推给渲染器"需要另一套牵引式流式供源，本轮不做。
2. **投放自然播完不会自动下一曲**：渲染器侧 STOPPED 后会话结束、播放器回到停止态；底栏「下一首/上一首」在投放模式下工作正常（已测）。队列循环/随机对投放不生效（未实现）。
3. **NAA 协议本身仍未逆向**：自动发现只能确认"端口 43210 开着"，连上去的 RTANet 协商需要端点机跑 `rtanet_rx`；真 networkaudiod 会明确报协商失败。发现结果不等于"能出声"。
4. **NAA 真端点发现未测**：本次设备处于 UPnP 模式（服务互斥，NAA 关闭），因此只验证了"负路径 + 本机监听正路径"；数播切到 NAA 后的真端点由 Lead 实测。
5. **真机发声未测**：按分工，UPnP 投放/NAA 的实际出声由 Lead 在用户授权下做；本工作树只做了无声验证（假渲染器 + backend=null/localhost）。
6. **只在 IPv4 局域网做发现**（IPv6/mDNS 未做）；多网卡最多扫 4 个 /24；代理 TUN 的 fake-IP 段按固定规则排除（198.18/15、198.19/24）。
7. **设置文件 BOM 兼容性**（与本次改动无关的既有行为）：用记事本另存 conf 加 BOM 时，首行键会带 `\uFEFF` 前缀被当成另一个键；本轮自检踩到一次，已用无 BOM 写入规避，未改代码。

---

## 8. 合并注意（给 Lead）

- 与 Lead 正在改的 **AI 检索质量 / 结构化标签后端** 可能同时改 `tools/rtaweb.cpp` 与 `src/net/ui_script.h`（本轮这两处分别为 +261 / +265 行）。合并冲突热点：
  - `rtaweb.cpp`：`/api/play`、`/api/skip`、`/api/pause`、`/api/seek`、`/api/stop`、`/api/status`、`/api/cast` 之后新增的 `/api/naa` 块、设置白名单 `upnpdev`。
  - `ui_script.h`：输出页 `renderSettingsPage` 的 out 页签块、`backendHint`、`applyBackendState`、`collectSettings`。
- 未改 `src/net/upnp.h` / `upnp_control.h`（投放控制面沿用），只给 `cast_service.h` 加了状态字段与暂停/跳转。
- 新文件：`tools/naa_discover.h`、`tools/mock_upnp_renderer.js`、`tools/test_web_upnp_backend.js`、`tools/test_web_naa_discover.js`。

---

## 9. 复现命令

```powershell
# 配置（注意：新 worktree 需要显式给 RC 编译器）
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe' `
  -DCMAKE_RC_COMPILER='C:/Program Files/AMD/ROCm/7.2/bin/llvm-rc.exe'
cmake --build build
cd build; ctest -j4

# 测试实例（backend=null / volume=-60，不出声）
$env:RLPLAYER_LIBRARY_DIR='D:/_tmp_lead/t24-out/lib'
.\build\rtaweb.exe --port 8923

# 门禁与自检
powershell -File tools/check_platform_boundary.ps1
node tools/uicheck.js http://127.0.0.1:8923/ D:/_tmp_lead/t24-out/shots-gate
node tools/test_web_upnp_backend.js http://127.0.0.1:8923
node tools/test_web_naa_discover.js http://127.0.0.1:8923
build\naa_scan.exe --listen 1
```
