# PLATFORM.md — 平台抽象层契约

> 读者：**Mac 组**（要用 CoreAudio / NSURLSession / posix_spawn 重做输出与传输）、
> 本项目后续维护者（要判断"这行代码该放哪一层"）。
> 状态：Windows 侧**已落地并全绿**（构建 0 error、ctest 45/45、边界门禁退出码 0）；
> Mac 侧**尚未实现**（平台头文件在非 Windows 上是 `#error`，见 §9）。
>
> 一句话：`src/platform/` 是内核与操作系统之间**唯一**的接缝。内核目录里出现
> `#include <windows.h>`（或任何 Win32 类型）就是越界，由 `tools/check_platform_boundary.ps1` 判红。

---

## 1. 术语与数据格式（先定死，否则"bit-perfect"是空话）

| 概念 | 本项目的定义 |
|---|---|
| PCM 样本缓冲 | `int32_t` **交错**（LRLRLR…）；**低位对齐到请求位深的满量程**，不是左移 32 位 |
| 位深 16 | 取值范围 `[-32768, 32767]` |
| 位深 24 | `[-8388608, 8388607]`（低 8 位必须为 0） |
| 位深 32 | `[INT32_MIN, INT32_MAX]` |
| 样本率 | Hz，整数（44100 / 48000 / 88200 / 96000 / 176400 / 192000 / 352800 / 384000 / 705600 / 768000 …） |
| DSD 速率 | **比特率**（DSD64 = 2822400，DSD128 = 5644800，DSD256 = 11289600） |
| 路径字符串 | 内核内部**一律 UTF-8 窄串**；只有 `platform/path.h` 能把它变成原生路径 |
| 长度单位 | 帧（frame）= 每声道一个样本；`frames * channels` 个 `int32_t` |
| 错误报告 | 返回值/错误码 + **非空**的中文文案（§7），不抛异常（`meta/` 与 `net/` 全 error_code 纪律） |

> 位深语义的出处：`src/audio/wasapi_out.h` 的 `pack()`（`bits==16` 时 `(int16_t)src`、`bits==24` 时取低 24 位）。
> Mac 侧若用"左移对齐的 32 位"接 CoreAudio，就必须自己搬位 —— 搬错一位就不是 bit-perfect。

---

## 2. 分层图与边界

```
┌──────────────────────────────────────────────────────────────────────┐
│ UI（自包含 HTML/JS，src/net/ui_page.h + ui_script.h）                 │
│   ↑↓ 73 处 `fetch('/api/…')`（实测 85 个调用点 / 55 个不同路径）        │
│   ↑↓ WebView2 通道：postMessage 字符串（§10）                          │
├──────────────────────────────────────────────────────────────────────┤
│ 宿主 / 入口层：tools/rtaweb.cpp（HTTP 路由、播放调度）、tools/launcher.cpp │
│   ← 允许用平台 API（它是"Windows 外壳"），Mac 侧换成自己的宿主            │
├──────────────────────────────────────────────────────────────────────┤
│ 内核（**严禁**直接依赖操作系统）                                        │
│   src/dsp    重采样 / 调制器 / 卷积 / GPU 桥接                          │
│   src/engine 管线                                                        │
│   src/net    音源、HTTP 客户端外壳、JSON、库索引                          │
│   src/meta   标签模型 / 解析 / 匹配 / 写回                                │
├──────────────────────────────────────────────────────────────────────┤
│ 平台层 src/platform/（**唯一**允许出现平台差异的地方）                    │
│   audio_out.h  dynlib.h  fs.h  path.h  http.h  process.h                │
│   console.h    time.h    log_hooks.h                                    │
│   windows/  ← 全项目唯一允许 `#include <windows.h>` 的目录              │
│   （macos/ 待建）                                                        │
├──────────────────────────────────────────────────────────────────────┤
│ 操作系统 / 驱动                                                          │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.1 目录规矩

| 目录 | 能不能出现平台 API | 说明 |
|---|---|---|
| `src/platform/windows/` | **可以**（也必须） | 全项目唯一允许 `<windows.h>` / winsock / winhttp 的地方 |
| `src/platform/`（本级 .h） | **不可以** | 只有声明 + 纯函数；不得出现 HANDLE/HRESULT/DWORD/LPWSTR 等 |
| `src/dsp` `src/engine` `src/net` `src/meta` | **严禁** | 由门禁脚本判红（§2.3） |
| `src/audio/` | 现状：可以（是 **Windows 音频后端实现目录**） | Mac 侧不要把新后端加进来；应新建 `src/platform/macos/`，并让渲染循环只认 `rta::render::OutSink` / `platform::AudioOutput` |
| `tools/` `tests/` | 可以 | 入口/验收入口层；测试允许直接吊 Win32（如 `SetConsoleOutputCP`） |

### 2.2 为什么 audio 后端保留在原位而不是搬进 platform/windows/

任务允许"搬到 platform/windows/"或"保留原位但通过接口暴露"，这里选**后者**，理由：

1. `wasapi_out.h`（约 650 行）+ `wasapi_shared.h` + `asio_out.h` 已被
   `rtaweb.cpp` / `rtanet_rx.cpp` / `tests/test_wasapi_shared.cpp` 等 6+ 个目标直接 include；
   搬目录要改一堆 include 路径，**风险 > 收益**（纯搬家不该产生这种风险）。
2. 渲染循环早就有自己的 sink 抽象 `rta::render::OutSink`（`src/audio/render_loop.h`），
   再加一层 vtable 会得到**两套**时序语义，反而更容易错。
3. 更小改动 = 更可信的"逐位等价"。所以本轮只做两件事：
   把平台的**非音频**设施抽出（dynlib/path/fs/http/process/time/console），
   并把音频后端的**契约**写成 `platform/audio_out.h`（§4/§5/§7/§8 的规范来源）。

### 2.3 边界门禁（机制，不是口号）

```
tools/check_platform_boundary.ps1        # 扫描 src/dsp src/engine src/net src/meta（含子目录）
tools/platform_boundary_allowlist.txt    # 例外清单：路径|原因|清理期限|责任人
.github/workflows/platform-boundary.yml  # 独立 workflow（不碰 ci.yml）：扫描 + 门禁自检
```

- 退出码：`0` = 干净；`1` = 有越界（打印 `文件:行号`）；`2` = 参数/目录错误。
- 判据是**代码级**的：注释与字符串字面量先被剥掉再匹配，所以错误文案里写 `GetLastError=` 之类不会误报。
- 规则四类：① Windows SDK 头文件 ② Win32 类型/API 名 ③ WASAPI/ASIO 类型名 ④ `#pragma comment(lib)` / `__declspec` / `__stdcall`。
- **白名单纪律**：只写"确实需要 + 下一轮怎么消掉 + 到期日"。当前 3 条（`net/webui.h`、`net/upnp.h`、`net/rtanet.h`）都是
  socket 传输本体，socket 抽象（`platform/net.h`）**不在本轮接口清单**内，到期 2026-12-31。
- 门禁自检（铁律 3）：workflow 每次都会往临时目录里塞一个 `#include <windows.h>` 的样例，
  **要求脚本退出码非 0**；否则该 job 失败。手工红/绿实测见 §12。
- ⚠️ 已知盲区：门禁是**文本级**的，抓不住"传递包含"。例如 `net/library.h` 仍然经
  `audio/audio_loader.h` 间接拿到 `<windows.h>`（它自己没写）。下一轮把 `audio_loader.h` 的
  `run_hidden/find_tool/widen` 换到 `platform/process.h` 后这条链就断了。

---

## 3. 接口清单（语义 / 线程 / 时序）

所有头文件都是 **header-only**（本项目一直是 header-only：`rtadsp` 是 INTERFACE 库，没有 .cpp 要链接）。

### 3.1 `platform/path.h` — 路径与编码

| 函数 | 语义 | 线程 |
|---|---|---|
| `from_utf8(s)` / `to_utf8(p)` | UTF-8 ↔ `std::filesystem::path`（Windows 用 u8string 构造，不经 ANSI 代码页） | 任意 |
| `module_dir_utf8()` | 可执行文件所在目录（无尾分隔符，失败空串） | 任意（只读） |
| `temp_dir_utf8()` | 系统临时目录，**带尾分隔符**（= `GetTempPathW` 语义，写回临时 PCM 依赖这一点） | 任意 |
| `config_dir_utf8()` / `data_dir_utf8()` | 配置 / 数据目录（Windows: 都是 exe 同目录，§6） | 任意 |
| `join(dir, leaf)` | 用平台分隔符拼接（不重复加分隔符） | 任意 |
| `lower_extension(p)` | 小写扩展名（含点），纯字符串 | 任意 |

### 3.2 `platform/fs.h` — 文件操作

| 函数 | 语义（Windows 实现逐 1:1 包装） |
|---|---|
| `remove_file(p)` | `DeleteFileW`；文件不存在返回 false（调用方历来忽略） |
| `move_file(from,to,replace)` | `MoveFileExW`（`replace ? MOVEFILE_REPLACE_EXISTING : 0`）。跨卷会失败，**由调用方退化处理**（写回的跨盘备份就是这样） |
| `copy_file(from,to,fail_if_exists)` | `CopyFileW` |
| `file_exists(p)` | `GetFileAttributesW != INVALID` |
| `last_error_code()` | 系统错误码数字（Windows `GetLastError` / POSIX `errno`）。**必须进用户可见文案** |
| `atomic_replace(from,to)` | = `move_file(..., true)`，只是把意图写明 |

⚠️ **原子性**：`atomic_replace` 只在同一卷内原子（Windows 语义）。写回流程因此把临时文件放在**同目录**
（`file + ".rtatmp"`）—— Mac 侧必须沿用这个约束，不要把临时文件放 `/tmp`。

### 3.3 `platform/dynlib.h` — 动态库加载

`class SharedLib { open/close/is_open/symbol/detach/last_error }`

- `open` 失败**不抛**，返回 false；`symbol` 失败返回 `nullptr`（调用方据此退回 CPU）。
- 析构 = 卸载；`detach()` = 放弃所有权、**模块保持加载**。
  GPU 桥接就是这么用的（`src/dsp/gpu_bridge.h` / `resampler.h`）：取到函数指针后
  **绝不能** 让模块卸载，否则函数指针变野指针。
- 调用方只做"锦上添花"：找不到 DLL ⟹ 返回不可用 ⟹ 管线走 CPU。**任何失败都不得让播放失败**。

### 3.4 `platform/process.h` — 子进程

| 函数 | 语义 |
|---|---|
| `run_hidden(cmd_utf8, capture)` | 隐藏窗口跑一条命令；阻塞至退出；`capture` 时合并捕获 stdout+stderr（原样字节）。`exit_code = -1` 且 `started=false` = 启动失败 |
| `find_executable(name)` | 先 exe 同目录（分发版自带 ffmpeg），再 `PATH`；找不到返回空串 |

- 命令行是**一整条 UTF-8 串**（调用方负责引号，规则见 `meta/writeback.h` 的 `wb_quoted`），
  平台层负责转原生编码。不要改成 argv 数组：引号/反斜杠转义语义会变。
- 线程：阻塞，禁止在音频渲染线程上调用（写回是后台线程）。

### 3.5 `platform/http.h` — HTTP(S) 传输

`struct HttpRequest { method, url, headers, body, contentType, timeoutMs, proxy }`
`HttpResult http_request(const HttpRequest&)`

- **阻塞**（在调用线程跑完整个请求）；不回调、不自建线程。
- 超时 `timeoutMs` 同时作用于连接/发送/接收（WinHTTP 四路同值）。
- 非 2xx 不算异常：照常返回 body，`ok=false`，`error="HTTP <码>"`。
- 代理：`proxy` 空 = 直连；非空 = `host:port`。归一化是纯函数 `normalize_proxy`（已编码的 `%` 不再编码，防双重编码 —— 有测试守）。
- `net/http_client.h` 只是**外壳**：保留 `rta::net::HttpResponse/http_get/http_post` 这些名字（20+ 调用点），
  内部转调平台层，并持有全局代理槽（`set_http_proxy`）。

### 3.6 `platform/console.h` — 控制台编码

`enable_utf8_console()` → Windows `SetConsoleOutputCP(CP_UTF8)`（只切输出，不改输入代码页）。
无控制台时返回 false，不抛。**本轮只提供接口，未批量改 tools/tests**（§11）。

### 3.7 `platform/time.h` — 单调时钟

`uint64_t monotonic_ms()` → Windows `GetTickCount64()`。
用途：认证盐的熵源（`net/subsonic.h`）、发现超时基准（`net/upnp.h`）。
不用 `std::chrono::steady_clock` 是因为"刻度与 epoch 由实现定义"——纯搬家不该改观测值。

### 3.8 `platform/log_hooks.h` — 日志钩子（**只给类型，不造第二套日志**）

`using LogSink = std::function<void(const std::string&)>;` + `struct LogHooks { play, debug }`

已有的钩子就是 `rta::render::RenderHost::log / dlogFn`（`src/audio/render_loop.h`，
`rtaweb.cpp` 里接到 `play_log` / `dlog`，落盘 `rlplayer-playback.log`，4 MB 轮转）。
再造一套日志文件只会让"哪份日志是真的"变成新的坑。

### 3.9 `platform/audio_out.h` — 音频输出契约

见 §4（映射）、§5（bit-perfect）、§7（错误）、§8（缓冲）。

---

## 4. 音频输出：契约 ↔ 现状（Windows）

| 契约（`platform/audio_out.h`） | WASAPI 独占（`audio/wasapi_out.h`） | WASAPI 共享（`audio/wasapi_shared.h`） | ASIO（`audio/asio_out.h`） |
|---|---|---|---|
| `open(OpenSpec)` | `open(deviceId, rate, ch, bits, bufferMs=100)` | `open(deviceId, rate, ch, bits, bufferMs=200)` | `open(clsid/driver, rate, ch, bits, …)` |
| `start()` | `ac->Start()`（不预填充） | 同 | `ASIOStart()` |
| `write(pcm, frames)` | `write_available`（按设备可用空间，周期整数倍） | `write_available`（int32→float32 转换） | 缓冲区切换 |
| `pause()/resume()` | `ac->Stop()` / 重新 Start | 同 | `stop()/start()` |
| `buffer_frames()` | `GetBufferSize()` | 同 | ASIO buffer size |
| `bit_perfect()` | 独占 + 无增益 + 源格式 == 设备格式 → true | **永远 false**（系统混音器介入） | 由驱动决定（整数直通） |
| 引擎侧暴露 | `rta::render::OutSink`（`src/audio/render_loop.h`） | 同 | 同 |

### 4.1 调用时序（Mac 侧必须一致）

```
控制线程                                   渲染线程
────────                                   ────────
open(spec)  ── 失败 → 报 §7 错误，不进入下一步
  ↓
caps(device) ── 决策用（§5 decide_route）
  ↓
start()     ── 失败 → close() + §7 错误
  ↓  (跨线程交接，此后 write 只由渲染线程调用)
                                           write(pcm, n) → 返回实际帧数；n 未写完继续喂
                                           ~~~~~~~~~~~~~~  ← 用户按暂停
pause()     ── 设备侧 Stop，不释放资源
resume()    ── 重新 Start（不需要重新 open）
  ↓
close()     ── 幂等；close 后可重新 open
```

- `open/close` 只在控制线程；`write` 只在渲染线程且**单线程**（后端内部不得再起写线程）。
- 所有方法**不抛异常**。
- 任何一步失败都必须让 `last_error()` 非空，并映射到 §7 的错误类。

---

## 5. bit-perfect 判定规则（硬规则）

### 5.1 什么条件下**必须**逐位直通

一条路径算 bit-perfect，当且仅当：**解码后的整数样本一个 bit 都没被改过，且设备吃的是同一格式**。即：

1. 采样率不变（`srcRate == outRate`）——不重采样；
2. 位深不变（`srcBits == outBits`）——不换位深、不抖动、不升位；
3. 浮点→整数只发生在解码器内部且与既有路径一致（ffmpeg 解成 `pcm_sXXle` 时按源位深请求，见 `audio/audio_loader.h`）；
4. 输出走**独占/整数**模式（WASAPI Exclusive、ASIO、CoreAudio Hog Mode + 整数格式）；
5. 增益 **== 1.0**（任何 `set_gain != 1.0` 都会破坏 bit-perfect，因为乘法会改变最低位）；
6. 无任何 DSP（EQ/卷积/响度/调制器）在链路上生效 —— 一旦启用，信号链就**不是** bit-perfect，
   UI 必须如实显示（现状：信号链面板逐项显示）。

### 5.2 DoP 是 bit-perfect 的

DoP 不改变 DSD 位流，只是**换容器**：24-bit PCM 帧里 8 bit 标记 + 16 bit DSD 数据。
所以 `RouteKind::Dop` 的 `bitPerfect()` 为 true，而 `PcmConverted`（DSD→PCM/重采样）为 false。

### 5.3 什么条件下**必须拒绝**而不是静默替换

| 情形 | 必须的行为 |
|---|---|
| 源 DSD，设备既不支持 DoP 承载率也不支持 Native DSD | **拒绝播放**并说明"需要 176400 Hz 的 DoP 承载"，除非用户**显式**开启 DSD→PCM 转换 |
| 源 PCM 格式设备不支持，且用户没允许转换 | 拒绝，文案写清"设备不支持 44100/16" |
| 独占模式打不开（被占用/虚拟机） | **可以**回退共享，但必须：① 记一行日志（现状 `FALLBACK_SHARED …(非 bit-perfect)`）② UI 状态显示"非 bit-perfect" |
| 设备被拔掉 | 报 `DeviceLost`，停播并提示重选设备；**不许**悄悄切到别的设备 |
| 音量/增益 != 1.0 | 允许播，但 UI 必须显示"已施加音量，非 bit-perfect" |

判定的**唯一实现**是 `platform::decide_route(src, caps, allow_conversion)`（`platform/audio_out.h` 里的纯函数）。
Windows 现状与它一致的关键点：`rate_pick.h` 的"保持源采样率"档才可能 bit-perfect，
"跟随系统混音率/最高采样率"档必然重采样 ⟹ 非 bit-perfect（那里有同样的注释）。

---

## 6. 配置与数据目录规则

### 6.1 Windows 现状：**便携式，全部在 exe 同目录**

| 位置（exe 同目录） | 内容 |
|---|---|
| `rlplayer-settings.conf` / `rlplayer-llm.conf` / `rlplayer-sources.conf` / `rlplayer-library.conf` | 配置 |
| `rlplayer-meta/` | MetaStore 库：`tracks/*.json`、`covers/`、`backups/`、`writeback-failures.json` |
| `remote-cache/` | 远程音源原始文件缓存（**完整文件**，绝不留转码流） |
| `rlplayer-index.tsv` / `rlplayer-recent.txt` / `rlplayer-asio-cache.txt` | 库索引 / 最近播放 / ASIO 探测缓存 |
| `rlplayer-playback.log(+.1 .2)` / `launcher.log` | 播放日志（4 MB 轮转）/ 启动器日志 |
| `uploads/` / `dumps/` / `webview2/` | 上传 / 崩溃转储 / WebView2 用户数据 |

表格来源是代码实测（`path_in_exe_dir()` 的调用点），不是设计愿望。

### 6.2 Mac 必须给的等价规则（建议）

| 类别 | Windows | Mac 建议 | 硬要求 |
|---|---|---|---|
| 配置 | exe 同目录 | `~/Library/Application Support/RLPlayer/` | **必须**经 `platform::config_dir_utf8()` 暴露 |
| 数据 | exe 同目录 | `~/Library/Application Support/RLPlayer/` | **必须**经 `platform::data_dir_utf8()` 暴露，并在启动时调用 `MetaStore::instance().setBaseDir(<数据目录>/rlplayer-meta)` |
| 日志 | exe 同目录 | `~/Library/Logs/RLPlayer/` | 必须与 `rlplayer-playback.log` 同名规则，且"导出诊断"能找到它 |
| 临时 | `GetTempPathW` | `NSTemporaryDirectory()` | 必须**带尾分隔符**（写回直接拼后缀） |

⚠️ 默认 @`MetaStore::baseDir()` 是**相对路径** `"rlplayer-meta"`。宿主不调用
`setBaseDir` 就会落到"当前工作目录"——Mac 上双击启动时这是 `/`。**必须显式设置**。

---

## 7. 错误分类与用户可见文案

`platform::OutputError` → 文案（`output_error_text()` 已内置，Mac 侧直接复用同一套分类）：

| 错误类 | 典型原因 | 文案要点（不许只说"失败"） |
|---|---|---|
| `DeviceNotFound` | 端点拔出/禁用 | 说明"设备不存在"，提示去设置重选 |
| `DeviceBusy` | 被别的进程独占 | 说明"被占用"，提示关掉占用者或用共享模式 |
| `UnsupportedFormat` | 设备不吃 44.1k/16 | **把请求格式与设备支持列表都打出来**，不许静默换格式 |
| `StartFailed` | 开流后 Start 失败 | 提示重开输出 |
| `WriteFailed` | 设备拒收 | 提示重新打开输出 |
| `Underrun` | 供料不足 | 说明"可能断音"，提示负载/缓冲 |
| `DeviceLost` | 播放中设备消失 | 停播 + 提示重选，**不许**自动换设备 |
| `Timeout` | 等事件超时 | 提示设备无响应 |
| `Internal` | 其余 | 至少带后端名与错误码 |

纪律：
1. **不许静默失败**：每个失败分支都要么返回错误类 + 文案，要么（非关键路径）记一行日志。
2. 文案必须带**可复现的数字**：错误码（`last_error_code()`）、HRESULT/OSStatus、请求格式。
3. 技术细节（HRESULT/OSStatus）可以附在文案后，但**前面**必须是人话与下一步动作。
4. `UNSUPPORTED` 与 `REFUSED` 是**不同**的两件事：前者"设备不行"，后者"我们按规则拒绝"，
   拒绝时必须写清"如何显式允许降级"。

---

## 8. 缓冲 / 周期 / 延迟 / 子进程约定

| 概念 | 约定 |
|---|---|
| `bufferMs` | **设备缓冲总时长**（不是"我们队列的长度"）。默认：独占 100 ms、共享 200 ms |
| 周期（period） | 设备最小可调度单位；**独占模式下周期 == 缓冲**（Windows `Initialize` 的 periodicity 配方，`BUFFER_SIZE_NOT_ALIGNED` 时按实际 buffer 反算再 `Initialize` 一次） |
| `write` 部分写 | 返回实际写入帧数，`< frames` **不是错误**：渲染循环继续喂（"写不进去"的三类成因分解见 `wasapi_out.h` 的 `ZeroAttrib`） |
| 欠载 | 记为 `Underrun` 观测值，**不要**自动加大缓冲（会改变延迟与用户设置） |
| 延迟 | 对外只报 `buffer_frames()/rate` + 设备周期；不许"估算整个管线延迟"当事实 |
| 真实时间纪律 | 播放路径禁止 `sleep` 凑数；等设备事件用事件/回调（Windows: `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`） |
| 子进程 | 只在后台线程；阻塞；必须收退出码 + 日志尾部；**不许**在音频线程上等进程 |

---

## 9. Mac 组实现清单（逐条勾选）

平台头在非 Windows 上是 `#error`（不会"假装支持"）。实现顺序建议按下面 1→8：

1. **`src/platform/macos/path_posix.h`** — `module_dir_utf8`（`_NSGetExecutablePath` / `NSBundle`）、
   `temp_dir_utf8`（`NSTemporaryDirectory()`，**保留尾分隔符**）、`config_dir_utf8`、`data_dir_utf8`（§6.2）。
   `from_utf8/to_utf8` 在 POSIX 上是**恒等**（`path` 直接吃 UTF-8）。
2. **`fs_posix.h`** — `remove/rename(copy+unlink 兜底)/copyfile/exists/last_error_code(errno)`。
   注意 `rename` 的原子性只在同卷成立（§3.2）。
3. **`console_posix.h`** — 直接 `return true`（终端本来就是 UTF-8）。
4. **`time_posix.h`** — `clock_gettime(CLOCK_MONOTONIC)` → 毫秒。
5. **`dynlib_posix.h`** — `dlopen/dlsym/dlclose`。⚠️ Windows 的候选名是
   `rtagpu.dll / rtagpu_dx12.dll / rtagpu_cuda.dll`；Mac 需在 `SharedLib::open` 或
   `gpu_pref.h` 里映射成 `.dylib`（迁移前非 Windows 分支硬编码 `librtagpu.so`，本层把它收敛成"候选名规则"）。
6. **`process_posix.h`** — `posix_spawn` + 管道捕获 stdout/stderr；语义见 §3.4。
   `find_executable`：exe 同目录 → `PATH`。
7. **`http_nsurlsession.h`** — `http_request`：`NSURLSession` 同步包装（信号量 + dataTask），
   超时、代理、非 2xx 语义见 §3.5；必须保留 `contentType` 与"HTTP <码>"文案。
8. **`audio_out_coreaudio.h`** — 实现 `platform::AudioOutput`（§4）：
   - 设备枚举 → `DeviceCaps`（含 `rates/bitDepths/maxChannels` 的真实查询，**不要猜**）；
   - 整数格式 + Hog Mode ⟹ `exclusiveMode=true`；DoP 承载率写进 `dopCarrierRate`；
   - `write` 的 int32 语义见 §1（低位对齐！）；
   - 错误映射到 §7 的枚举，`last_error` 非空。
9. **UI 宿主通道**（§10）—— 提供 `window.chrome.webview` 等价物（`WKWebView` 的
   `window.webkit.messageHandlers.*.postMessage` + 注入 `window.chrome.webview` 垫片），
   消息名与语义**逐字**照 §10。
10. **宿主要做的两件小事**：启动时 `setBaseDir`（§6.2）、把日志接到 `LogHooks`（§3.8）。

验收（Mac 侧自测，不许"看能响"就算）：
- `decide_route` 的 6 个分支各跑一遍（纯函数，不需要声卡）；
- 44.1k/16 的已知 WAV 走独占直通，**与源文件 PCM 逐字节比对**（用 `tools/wavcmp` 的思路）；
- 欠载/拔设备/独占失败三种故障各注入一次，检查文案与 UI 状态（§5.3、§7）。

---

## 10. UI 集成契约（**不要改 UI**）

### 10.1 `/api/*` 是稳定接口

- UI 与宿主之间**只有** HTTP（`127.0.0.1:<port>`，默认只监听回环）+ 一条 postMessage 通道。
  这是既有边界（约 2200 行 `ui_script.h` 里对 WebView2 的依赖只有 2 行），**新平台不要重做**。
- 实测：`tools/rtaweb.cpp` 里 **62 个**不同的 `"/api/…"` 字面量；UI 侧 **85 个**调用点（55 个不同路径）。
- 契约：**路径、方法、参数名、返回 JSON 字段名一律不许改名/改语义**。允许**新增**接口或字段。
  重命名会让 2200 行内嵌 JS 静默失效（而 UI 没有编译期检查）。
- 路由清单（从 `rtaweb.cpp` 实测，按字母序；新增时请补在这里）：
  `/api/add /api/asio /api/asiodevices /api/clear /api/cover /api/dbg /api/devices /api/diag /api/diagzip
  /api/enrich /api/files /api/filters /api/gpu /api/indevices /api/lib /api/lib/album /api/lib/albums
  /api/lib/cover /api/lib/enqueue /api/lib/play /api/lib/roots /api/lib/scan /api/lib/search /api/lib/stats
  /api/llm /api/llm/test /api/lyrics /api/meta /api/net /api/pause /api/ping /api/pl /api/pl/ (带 id 前缀)
  /api/pl/delete /api/pl/load /api/pl/save /api/play /api/providers /api/qdel /api/qmove /api/queue
  /api/rates /api/recent /api/repeat /api/seek /api/settings /api/shuffle /api/signal /api/skip /api/src
  /api/src/browse /api/src/config /api/src/cover /api/src/ping /api/src/search /api/status /api/stop
  /api/upnp /api/upload /api/volume /api/writeback /api/writeback/status`

### 10.2 `window.chrome.webview` 等价通道（postMessage 语义）

UI 侧原文（`src/net/ui_script.h:12-14`）：

```js
var WV = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
function wcmd(c) { try { if (WV) WV.postMessage(c); } catch (e) { } }
```

- **方向 1（UI → 宿主）**：`WV.postMessage(<string>)`。字符串，**不是 JSON**。
- **方向 2（宿主 → UI）**：`WV.addEventListener('message', ev => …)`，`ev.data` 是字符串。
- 若 `WV` 为 null，`wcmd` 静默 no-op（这是**有意的降级**：浏览器里打开界面时按钮不报错）。
  Mac 宿主**必须**提供等价对象，否则最小化/最大化/关闭/拖拽/主题跟随全部失效（且不会有任何报错）。

**完整消息表**（从 `tools/launcher.cpp` 的 `onMessage` 与 `ui_script.h` 实测，一条不漏）：

| 方向 | 消息 | 语义 | 宿主侧动作（Windows 现状） |
|---|---|---|---|
| UI → 宿主 | `min` | 最小化 | `ShowWindow(SW_MINIMIZE)` |
| UI → 宿主 | `max` | 最大化/还原切换 | `IsZoomed ? SW_RESTORE : SW_MAXIMIZE` |
| UI → 宿主 | `close` | 关闭窗口 | `PostMessage(WM_CLOSE)` |
| UI → 宿主 | `drag` | 顶栏拖动窗口 | `ReleaseCapture + WM_NCLBUTTONDOWN(HTCAPTION)` |
| UI → 宿主 | `theme:dark` | 主题=深色（原生边框/圆角跟随） | 记 `g_themeFromPage=true` + `apply_caption` |
| UI → 宿主 | `theme:light` | 主题=浅色 | 同上 |
| UI → 宿主 | `theme:blue` | 主题=浅蓝 | 同上 |
| 宿主 → UI | `max` | 窗口已最大化 | UI 切换"还原"图标 |
| 宿主 → UI | `normal` | 窗口已还原 | UI 切换"最大化"图标 |

- 宿主 → UI 的 `max/normal` 在 `WM_SIZE` 里发；Mac 侧建议在窗口 resize 回调里发同样两条。
- 主题消息只影响**原生边框**；页面内的主题本来由页面自己管（conf 轮询只是兜底，且页面一发消息就停）。

---

## 11. 未做项与已知差异（铁律 4：如实记录）

1. **Mac 实现一行都没写**。非 Windows 上平台头是 `#error`，不会静默假装可用。
2. **socket 未抽象**：`net/webui.h`、`net/upnp.h`、`net/rtanet.h` 仍直连 winsock（白名单，到期 2026-12-31）。
   这三份是 HTTP 服务器 / SSDP / RTANet 传输本体，抽出来是 `platform/net.h` 的活。
3. **`src/audio/` 仍是 Windows 目录**：三个后端没搬到 `platform/windows/`（§2.2 的取舍），
   `platform::AudioOutput` 目前**只有契约、没有 Windows 适配器**（Windows 走既有 `OutSink`）。
   这意味着契约的 Windows 侧一致性靠**代码评审 + §4 映射表**，而不是编译期约束。
4. **`console.h` 未被 tools/tests 采用**：30+ 个入口仍在手写 `SetConsoleOutputCP(CP_UTF8)`
   （有几处为了避开 `windows.h` 手写 `__declspec(dllimport)` 声明）。本轮刻意不动 tests/tools，
   避免与"CI 与测试"那条并行分支撞车。
5. **语义差异（实测无法触发，但如实写）**：
   - `source_mgr.cpp` 的缓存目录：迁移前 `GetModuleFileNameW` 失败时会得到 `"\remote-cache"`（带前导分隔符），
     现在会得到 `"remote-cache"`。只有 `GetModuleFileNameW` 返回 0 才触发，实际不可达。
   - 路径 → UTF-8 的替换字符粒度：`module_dir_utf8` 等用 `WideCharToMultiByte` 逐个转换后拼接，
     原实现是"整串转换"。对**合法 UTF-16** 逐字节相同；非法代理对的 `U+FFFD` 替换位置可能不同。
   - `net/subsonic.h` 的认证盐熵源改成了 `platform::monotonic_ms()`（Windows = `GetTickCount64`，
     **取值完全相同**）；这是非音频路径，不影响任何 PCM。
6. **门禁的盲区**（§2.3）：文本级，不查传递包含；也不查 `src/audio` 与 `tools`。
7. **`platform/audio_out.h` 的 `decide_route` 没有自动化测试**（不能为了测它改 ctest 基线 45/45）。
   它是纯函数，Mac 侧接 CoreAudio 时建议先补单元测试。

---

## 12. 变更记录

| 日期 | 内容 |
|---|---|
| 2026-10-22 | 建 `src/platform/`（audio_out/dynlib/fs/path/http/process/console/time/log_hooks + windows/*）；改造 `dsp/gpu_bridge.h`、`dsp/resampler.h`、`net/http_client.h`、`net/source_mgr.h`、`meta/writeback.h` 只依赖平台层；新增边界门禁脚本 + allowlist + 独立 workflow；构建 0 error / ctest 45/45 / 门禁红→绿实测（见下） |

```
# 门禁红→绿实测（2026-10-22，本机）
# 红：往 src/dsp/resampler.h 注入 #include <windows.h>
[VIOLATION] src/dsp/resampler.h:12: #include <windows.h>   // RED-TEST: 故意注入的越界，用来证明门禁会红（随后还原）
            规则：Windows SDK 头文件
[FAIL] 发现 1 处平台越界（另有 22 处白名单豁免）。
EXITCODE=1
# 绿：还原后
[PASS] 内核目录无平台越界（22 处已按白名单豁免）
EXITCODE=0
```
