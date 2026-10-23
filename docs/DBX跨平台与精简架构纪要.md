# DBX 跨平台与精简架构纪要（外部调研）

来源：[t8y2/dbx](https://github.com/t8y2/dbx)，Apache-2.0，实测 tag **v0.6.51**。
源码 clone 保留在 `C:\Users\garaxc\Documents\_tmp\dbx-recon`（可直接读，路径按仓库相对写）。
release 产物大小取自 `api.github.com/repos/t8y2/dbx/releases/latest` 实测，非 README 转述。

**为什么记在 RLPlayer 里**：它不是同类产品（数据库客户端 vs 播放引擎），但它与本项目在**同一个问题上选了相反的答案**——「可选能力该进包还是出包」。本文的核心价值在第 5 节的对照，不在 DBX 本身。

---

## 1. 结论先行

1. **跨平台不是"每个平台写一份"**，而是把 M 个界面形态 × N 个平台压成 M + N：一份前端、一份引擎，平台只做薄壳。它的代码量并不小（Rust 65.7 万行 / 651 个 `.rs`，前端 103.9 万行 / 532 个 `.vue`）。
2. **"精简"指的是分发形态精简，不是代码精简**：浏览器内核、语言运行时、长尾驱动、可选功能这四样全部推出了主包。
3. **README 的「25 MB」与实测不符**：v0.6.51 最小产物 29.5 MB（Windows 安装包），裸 exe 77.4 MB。属广告口径，不可当读数用（见 §4）。

---

## 2. 跨平台：四层结构，一份业务代码

| 层 | 做法 | 证据（仓库相对路径） |
|---|---|---|
| 外壳 | Tauri 2 三平台原生壳（WKWebView / WebView2 / WebKitGTK）+ axum 服务壳 + 无壳 CLI/MCP | `Cargo.lock` 有 `wry` / `tao` / `webkit2gtk`；`crates/dbx-web/src/main.rs` |
| 传输 | 同一份 Vue SPA，运行时选 `invoke` 或 `fetch` | `apps/desktop/src/lib/backend/api.ts:14-18` |
| 引擎 | 一个 `dbx-core` 被 desktop / web / cli / mcp 四个消费者复用 | `crates/ARCHITECTURE.md` 依赖图 |
| 差异 | 平台相关能力塌缩到一个 crate（进程/路径/代理/下载/版本） | `crates/dbx-platform/src/{process,path_utils,proxy,download,version}.rs` |

### 2.1 最值得抄的一处：用类型系统强迫两套 transport 同构

`apps/desktop/src/lib/backend/api.ts:28-38` 的注释写得很直白：`http.ts` **故意从 `tauri.ts` 导入请求/响应类型**，于是两套后端在编译期被迫保持签名一致，前端业务代码完全不知道自己在哪个壳里跑。

对照本项目：`src/net/` 的 Web 控制面与 `tools/` 的 CLI 是各自手写的两套入口，靠人记着同步。若将来要加第三入口（比如 RTANet 端点直控），把「共享的请求/响应结构体放进一个头文件、两套实现互相引用它」这一步做掉，比写文档约定同步更省事——**同构由编译器保证，而不是由纪律保证**。

### 2.2 平台差异被处理到什么粒度

```rust
// crates/dbx-platform/src/process.rs:14-18
// Windows children are created suspended, assigned to a private Job Object,
// and only then resumed. This ensures descendants inherit the Job before
// any plugin code can run. Other platforms intentionally keep Tokio's
// existing direct-child behavior.
```

Windows 必须用 Job Object 才能杀掉整棵进程树（否则插件 sidecar 变孤儿），Unix 下 `kill` 就够。同类分叉共 **188 处** `cfg(target_os)`（`src-tauri` 97 处、`crates` 91 处），另有三类平台专属模块：`macos_app_delegate` / `macos_escape_guard` / `webview2_recovery` / `webview2_legacy_css`。

### 2.3 跨到 Win7 的代价（最硬的一笔）

`Cargo.toml` 的 `[patch.crates-io]` 把 **7 个 crate 拷进 `vendor/` 钉死**，每条带原因注释：

| vendored crate | 原因（原文摘要） |
|---|---|
| `webview2-com-sys` | 离线 Win7 包必须链 **1.0.902.49** loader；新版 import `EventSetInformation`，Win7 没有 |
| `ctor` | Tauri 2.11 用 ctor 0.8，排除了 Rust 的 win7 vendor |
| `wry` | Win7 不能回落系统 WebView2 Runtime，须显式传 Fixed Runtime；另加 renderer 崩溃自愈与剪贴板历史修复 |
| `dirs-sys` / `pageant` | 上游绑定 import 了 Win7 没有的 COMBASE / WinRT |
| `rumqttc` | 硬编码 MQTT 3.1.1，这里补 3.1 CONNECT 编码 |
| `tiberius` | 含非法 UTF-16 代理对的结果集不丢行 |

外加两个 git fork：`tokio-postgres-gaussdb`、`zipg/mysql_async`。

**判据**：复杂度没有消失，只是从用户侧搬到维护侧。七个 fork 的 rebase 成本 vs Win7 用户的市场价值，是这类决定唯一该算的账。本项目的对应物是 `third_party/` 与自维护的 `asio_probe`——同样是"上游不给我，我就自己拿住"。

---

## 3. 精简的四个"不写"

### 3.1 不打包浏览器内核
Tauri 用系统 WebView（Windows = WebView2）。这是百 MB 级与十 MB 级的量级差来源。

⚠️ 注意本项目的反向选择：RLPlayer 铁律 6 明确要求**内置 WebView2Loader.dll 与 WebView2 运行时**，因为服务端/测试机不能保证有 Edge。DBX 敢用系统 WebView，是因为它假定用户是开发者（机器大概率已随 Windows 更新带上 WebView2 Runtime）。**这条不能照搬**——本项目受众是音响用户，不少人的机器是专用播放机，系统更新被关掉了。

### 3.2 不引入语言运行时
没有 JVM、没有 Python venv。原生驱动全是 Rust crate，静态链进同一个二进制：

```
tokio-postgres + deadpool-postgres   mysql_async（+ ed25519）
tiberius（SQL Server）               redis（cluster / tls）
mongodb                              rusqlite（bundled）
```

实测 `DBX_0.6.51_x64-portable.zip` 解包只有 **5 个文件**：`DBX.exe` + README + LICENSE + 2 个元数据 JSON，**没有 DLL、没有 sidecar、没有 JRE**。

### 3.3 不为 100+ 数据库写 100+ 个驱动
这是「小体积装 100+ 库」最容易被误读的地方，真实三层：

| 层 | 数量 | 机制 |
|---|---|---|
| 原生驱动 crate | **8 个**（约 9.4 万行） | mysql / postgres / redis / mongodb / elasticsearch / sqlserver / sqlite / support |
| 元数据注册表 | **90 个** `plugins/connection-types/*.yaml` + **37 个** `plugins/dialects/*` | `dbx-types/build.rs`、`dbx-sql/build.rs` 在**构建期**消费，生成数据库身份与方言 |
| 外部 agent | 长尾（Snowflake / DB2 / BigQuery / H2 / Trino…） | `dbx-driver-agent`（2.2 万行框架）跑子进程；JDBC 内核以 `dbx-jdbc-plugin-*.zip`（**9.18 MB**）**单独分发** |

「100+」= 8 个协议实现 + 大量国产库复用 MySQL/PG 协议 + YAML 声明式补齐差异 + 长尾外包。**新增一个兼容库多半是加一份 YAML，不是写代码。**

对照本项目：滤波器家族/调制器的"16 个预设 + 可指定抽头数"是同一种思路的低配版——预设是数据，算法才进代码。

### 3.4 不把可选功能放进主包
`plugins/README.md` 的原话：插件可以在**不增加基础安装体积**的前提下加后端行为、沙箱 UI、连接类型。`.dbxp` 包 + Rust/Go sidecar 子进程 + Ed25519 签名，S3 / K8s / LDAP / SSH 全在包外。

### 3.5 编译与打包层再抠一刀
`Cargo.toml` release profile：

```toml
panic = "abort"      # 去掉 unwind 表与 landing pad
strip = true         # 去符号
lto = true           # 跨 crate 内联 + 死代码消除
codegen-units = 1    # 换更彻底的优化，代价是编译时间
opt-level = "s"      # 体积优先
```

前端按需分包（`apps/desktop/vite.config.ts` 的 `manualChunks`）：CodeMirror、echarts（再切 charts/components/renderers/core 四份）、reka-ui、marked 各自成 chunk；Web 版可用 `embed-static` 把整个 `dist` 编进 `dbx-web`，Docker 镜像里只有一个可执行文件。

---

## 4. 体积实测（校正 README 的 25 MB）

| 产物 | 实测 |
|---|---|
| `DBX.exe`（裸二进制 x64） | **77.40 MB** |
| `DBX_0.6.51_x64-portable.zip` | 38.80 MB |
| `DBX_0.6.51_x64-setup.exe` | **29.53 MB** |
| `DBX_x64.dmg` / `amd64.deb` | 43.05 MB / 42.85 MB |
| `DBX_x64.AppImage` | 114.05 MB |
| `DBX_0.6.51_x64-win7-server2012r2-offline-setup.exe` | **176.19 MB** |

**v0.6.51 没有任何产物是 25 MB。** 要么是早期版本读数、要么是某平台压缩后 app 本体、要么是"下载增量"口径。按本项目铁律 3 的同构要求：**宣传口径不等于可复现读数**，引用体积必须写清是哪个产物、解压后还是压缩后。

Win7 那个 176 MB 是同一规律的反面证据：**自包含的代价一次性兑现**——把 WebView2 Fixed Runtime 塞进去，体积立刻翻六倍。

---

## 5. 与本项目的对照：两种"自足"策略

两边都在回答同一个问题——**用户机器上到底该有什么**——但选了相反的极端：

| | RLPlayer | DBX |
|---|---|---|
| 路线 | **全进包**（铁律 6：正式发布版必须自包含） | **主包最小 + 长尾外置** |
| 体积 | 单包含 ffmpeg / GPU 内核 / WebView2 运行时 | 主包 29.5 MB，JDBC 9.18 MB 另发，插件按需装 |
| 保证 | 用户机不需要装任何运行库 | 用户机不需要装任何运行库（但功能边界更窄） |
| 代价 | 包大；每次功能增加都长体积 | 用户遇到长尾数据库要先下插件；插件版本与主程序要兼容 |
| 失败形态 | 缺件（历史教训：以"可选组件"为借口） | 装不上/装错版本（插件签名与平台产物选择） |

### 5.1 分界线不是"大小"，是"这是谁的职责"

两个项目**独立得出了同一条判据**，这是本文最值得记的一句：

- DBX：JDBC 长尾推出主包——因为「要求用户装 JRE」这件事它选择整个消灭（改原生实现），而不是帮忙装。
- RLPlayer 铁律 6 第 2 条：**「GPU 运行时不得要求用户安装 ROCm；显卡驱动属于硬件驱动，可由用户安装。」**

同一句话的两种说法：**依赖按归属划分，不按体积划分。** 硬件驱动是用户的职责（他本来就要装），语言运行时/GPU 计算栈是软件的职责（不能甩给用户）。于是 `amdhip64_7.dll` 必须进包，Adrenalin 可以让他自己装。

**推论（可直接用于判据）**：新增一个依赖时先问「这东西在用户眼里属于谁的活」。属于他的（驱动、系统更新）→ 声明依赖即可；属于我们的（运行时、DSP 内核、解码器）→ 必须进包，且不得用"可选组件"当缺件借口。

### 5.2 什么时候该学 DBX 的"外置"

本项目目前不该外置任何**旗舰路径**上的东西（播放、解码、DSP、GPU、输出），这正是铁律 6 的意思。但有两类东西天然适合走 DBX 路线：

1. **与旗舰路径无关的辅助功能**：元数据补全的数据源注册表（`src/meta/`）、网络端点的协议适配（UPnP/NAA 变体）。挂一个失败不影响出声，缺了只是少个功能。
2. **可以纯数据描述的扩展**：EQ/滤波器预设、Matrix 配置。DBX 的 90 个 YAML 证明——**能用数据描述的支持，就不要用代码描述**。

判据一句话：**「它坏了会不会让播放链路回到 CPU/关闭状态」**——会（GPU、DSP），进包且必须有退回路径；不会（元数据源、端点协议），可以外置、可以按需加载。

### 5.3 与现有实现的同构关系

本项目其实已经在用 DBX 的核心手法，只是没起名字：

| DBX 手法 | RLPlayer 现有对应物 |
|---|---|
| 薄壳 + 可选实现 + 自动退回 | `src/dsp/gpu_bridge.h`：`LoadLibrary` 拿 `rtagpu.dll`，拿不到就静默退 CPU 分块 FFT |
| 平凡 kernel 验证可用性 | `rta_gpu_available()` 真的写常数读回，不信 `hipDeviceSynchronize()` 的返回码（Round 15 教训） |
| 差异塌缩到一个模块 | `src/audio/wasapi_*` 独占/共享的所有平台怪癖都在这里，上层不写 `#ifdef` |
| 多入口共用一份引擎 | `tools/*.cpp` 一堆 CLI 与 launcher 共用 `src/engine/pipeline.h` |

也就是说，**本项目缺的不是思路，是「入口之间的共享契约」**——DBX 用类型导入把它变成编译期强制，本项目还是各写各的。

---

## 6. 可直接借的三条

1. **同构由编译器保证**：多个入口/多套传输时，让实现 B 引用实现 A 的类型定义，而不是靠文档约定字段一致。
2. **平凡 kernel / 冒烟先验**：这是本项目已确立的纪律，DBX 用同样的手法做 WebView 崩溃自愈与 GPU 探测。可复用的话术：**"能跑"不等于"跑对了"**。
3. **"能用数据描述就不要用代码描述"**：新增兼容目标时先问能不能写成表/预设/YAML，再问要不要写分支代码。

## 7. 不该照搬的三条

1. **系统 WebView**：本项目必须内置 WebView2 运行时（铁律 6），不能假设用户有 Edge。
2. **七个 vendored fork 换老平台兼容**：除非真的有 Win7/Server 2012 用户群，否则维护成本会超过收益。
3. **单二进制全驱动**：DBX 把 8 个原生驱动全静态链进 exe，代价是裸二进制 77 MB 且无法裁剪。本项目若要"可选解码器/可选端点"，更适合动态库 + 运行时加载（已有 `gpu_bridge` 先例）。

---

## 8. ⚠️ 未核实项（如实标注）

| 项 | 状态 |
|---|---|
| README「25 MB」的确切口径 | **未定位**。实测最近 tag 无任何 25 MB 产物；可能指早期版本或某平台压缩后本体 |
| README Tech Stack 写 `sqlx` | **与实际不符**：代码用 `tokio-postgres` / `mysql_async` / `tiberius` / `redis-rs` / `mongodb` |
| DuckDB 的 sidecar 获取方式 | `duckdb-sidecar` 是 feature，portable 包内无 duckdb 文件，推断为运行时下载或按需安装，**未读代码确认** |
| 「100+ 数据库」中协议复用与 agent 的精确分布 | 只统计了 crate 数（8）与 YAML 数（90 + 方言 37），**未逐个分类** |

---

## 9. 一句话留档

DBX 证明了：**把平台的差异收进一层、把可选的东西推出包外，核心就能保持小**。
RLPlayer 证明了反面：**当"可选的东西"恰好是旗舰路径的一部分时，推出包外就是把风险转嫁给用户**。
两句话合起来才是完整判据——**先问归属，再问体积**。

---

## 10. 落地：把 §2.3 那一行注释变成可执行的门禁（2026-10-23）

§2.3 表格里有一句「`webview2-com-sys`：离线 Win7 包必须链 **1.0.902.49** loader；新版 import `EventSetInformation`，Win7 没有」。
本轮把它落成了 RLPlayer 的四处改动 + 一次实测。

### 10.1 换 loader，并交叉验证了来源

- 取**微软官方** NuGet `Microsoft.Web.WebView2 1.0.902.49` 里的 `build/native/x64/WebView2Loader.dll`（不是拿对方 vendor 的文件）。
- sha256 = `fdf978ba706578b05967d7f0181f462147864a5aa74f36016a62cb3d3dbe6909`，与 DBX `prepare-webview2-win7-loader.ps1` 里 pin 的常量**逐字节相同** ⟹ 两边是同一份官方产物。
- `dumpbin /imports` 实测对照（判据，不信注释）：

  | loader | 版本 | `EventSetInformation` |
  |---|---|---|
  | 本仓临时放入的最新版 | 1.0.4258.31 | **导入** → Win7 / Server 2012 R2 报 "Could not find the WebView2 Runtime" |
  | 现在这份 | 1.0.902.49 | 不导入 ✅ |

- **SDK 头仍是最新（1.0.4258.31），实测可用**：`CreateCoreWebView2EnvironmentWithOptions` 等 4 个工厂导出是老 loader 就有的，界面能起、JS 能回传 `wvmsg: theme:dark`。即「loader 定版、头不定版」这条路成立 —— 但**将来若用到新版才有的接口，必须按 1.0.902.49 的头核对可用性**（这是本节的边界，不是许可证）。

### 10.2 门禁：`tools/check_compat.ps1` 增加第 4 条规则

原来只查静态 CRT / subsystem ≤ 6.1 / 无 AVX-512。新增禁用导入：

```
combase.dll             Win8+ only; use OLE32
api-ms-win-core-winrt   WinRT API sets unavailable on Win7
CoIncrementMTAUsage     unavailable on Win7
EventSetInformation     unavailable on Win7; WebView2 loader must be <= 1.0.902.49
```

三个配套决定（都来自本轮踩到的实事）：

1. **`WebView2Loader.dll` 被列入检查对象** —— 它不在 `build` 里被编译，是从 `third_party` 拷进来的；只查自家 exe 永远抓不到它。
2. **第三方 DLL 跳过反汇编**（输出 `avx512=n/a`）：5 MB 的微软二进制，反汇编既慢又不产生可行动结论。
3. **顺手修了工具定位**：原脚本硬编码 `C:\Program Files\AMD\ROCm\7.2\bin\llvm-readobj.exe`，在没有 ROCm 的机器上直接 `exit 2`（门禁形同不存在）。现在按 PATH → `LLVM_HOME` → 同级 `toolchain/clang+llvm-*` → ROCm 查找。

**规则有效性已验证（守卫要能被打中）**：换 loader 前跑一次 → `WebView2Loader.dll FAIL ... win7imports=EventSetInformation`；换掉 `third_party` 的文件重跑 → `FAIL count: 0`。

### 10.3 `*.dll` 的 gitignore 把必要件挡在仓库外

`.gitignore:29` 是 `*.dll`，于是 `third_party/webview2/x64/WebView2Loader.dll` **从来没进过仓库** —— clone 后 `rta_launcher` 的 POST_BUILD 拷贝直接失败（`No such file or directory`），而这正是「clone 下来能不能构建」的第一道门。
已加例外（带原因注释）：`!third_party/webview2/x64/WebView2Loader.dll`。
**判据**：这类「构建必需 + 版本决定可用性」的第三方件必须入库并被门禁审计；「靠每台机器自己找」在换机/多人协作时必然断。

### 10.4 与 §7「不该照搬」的边界

§7 说「七个 vendored fork 换老平台兼容：除非真有 Win7/Server 2012 用户群，否则维护成本超过收益」。
本轮**没有 vendoring 任何 crate**，只换了一个已存在第三方 DLL 的版本 + 加了一条门禁 —— 成本半小时，收益是 Win7 用户双击能开。分界线是：**事实（哪个版本能跑）照做，架构（fork 七个 crate）看用户群**。
