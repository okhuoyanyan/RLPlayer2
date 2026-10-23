# RLPlayer 交接说明

> **当前状态(2026-10-23 第二十五轮后)**:ROADMAP **P1/P2/P4/P5/P6(性能) 全部落地并合入 main**;
> 之前完成的 ROON/元数据 P3 资料层 ②③④、耳机 EQ(OPRA)、基建三项(WebView2 双轨/精简 ffmpeg/裁 Locales)都在 main;
> 源码树 ctest **58/58**、本机 verify **11/11(Fail 0)**、平台边界 PASS、uicheck 全过。
> **剩余都是外部/真机条件**:P3(NAA) 受 EULA 授权阻塞;UPnP 投放需真实渲染器;输入/资料层需真机复测;P4/P6 已完成实现。
> **verify 必须在机器空闲时串行跑**——并行重型构建(出包/全量编译)会让 UI 自检与个别 ctest 出假红(已二次踩到)。
> **1.1.1 已发布**(P6a 扫描并行 3.18× / P5b 投放 UI / L4 演绎去重 / 已知问题修复):`dist\RLPlayer-1.1.1-win64.zip`,**370.47 MB**,
> sha256 `3B72561242E4958DC7C0D52A9FB464048E14A2DC33DEF3358E5DEF46FA97BE3C`(GitHub Release v1.1.1);
> 1.1.0 / 1.0.10 旧包仍在 `dist\` 可回退。**下一步 = P6b 流式解码/DSD 两级抽取 + 投放/资料层端到端实测 + 剩余未解决项**。
> **资料层总恢复点**:`docs/元数据补全调研与规划.md` §1.6 + `docs/P3-资料层接口契约.md`(数据形状/UI 契约的唯一依据)+ `docs/耳机EQ-OPRA调研与设计.md`(含已拍板决策)。
> 该方向的**总恢复点在 docs/元数据补全调研与规划.md §1.6**
> (全景状态/代码地图/未解决项/工作方法/队列——新会话先读它),历史回合见 STATUS.md 第十三~**二十二**轮。
> 以下为第十二轮及更早的通用交接内容,架构与坑清单仍然有效。
>
> **最近七轮的关键状态变化(接手必读)**:
> - **第二十五轮(P6 性能 + 投放 UI + L4 演绎 + 已知问题)**:**P6a 库扫描并行化**(固定线程池/背压/结果逐字节一致;653 首 30.16s→9.47s **3.18×**)+
>   信号链耗时统计;**P5b 投放设置页**(`cast_service.h` + `/api/cast/*` + 「投放（UPnP）」面板);**L4 演绎身份**
>   (Discogs release 详情 → 指挥/乐团/独奏 + 按 `dedupKey` 去重,`complete=distinct>=5`;Boléro 11/Beethoven 10/Mahler 10);
>   **已知问题**:WebView2 护栏 10 用例入 ctest、rateAuto 策略纯函数测试、`PLAY_ABORT` 显式归因;
>   **P6b 流式解码**(免临时 WAV,PCM 逐位一致、磁盘写 0);**P6c DSD 两级抽取**(CIC 4×+短 FIR,DSD256 5 分钟 16.2s→7.1s **2.3×**,THD+N 不回退)。
>   ⚠️ 合并时踩过:**P5b 的 `cast_service.h` 曾含 windows.h 平台越界**(已修 c6909f0);门禁要看 exit code 别只看尾行。
> - **第二十四轮(ROADMAP 剩余四阶段)**:**P4 输入采集**(`src/audio/wasapi_capture.h` 无锁 SPSC/格式转换/THD+N + `tools/rtacapture.exe` + `/api/input/*` +
>   设置页输入面板;监听=共享输出直通,非 bit-perfect);**P5 UPnP 投放**(`src/net/upnp_control.h` SOAP 9 动作/UPnPError/DIDL/Range + `UpnpHttpServer` 8912 +
>   `tools/rtacast.exe`;本机局域网无渲染器,端到端重采样未实测);**P1 调制器族**(dsd/chebyshev/ec/ahm,键 `modfamily`;
>   Chebyshev-7 低电平失稳已强制降阶到 5 阶修复);**P2 滤波器变体**(16→47 预设,真实抽头/过渡带/阻带/相位;
>   `tools/filter_report.exe` + `docs/滤波器变体对比表.md`;避开「三个坑」的判据)。
>   ⚠️ 用法:新 CLI 在包内 `tools\` 下;`/api/input/start` 支持 `src=null&inject=1000:0.001` 在**不开设备不发声**下自检;
>   `/api/input/devices` 的 Loopback 端点需要系统有音频流才有包(平台规范,静音录制需补零)。
> - **第二十三轮(ROON P3 资料层 + 耳机 EQ/OPRA)**:② 直连层 `src/meta/knowledge.h`(维基 zh/en + 萌娘
>   `zh.moegirl.org.cn/api.php`、OpenSearch「不猜」消歧义、令牌桶+熔断、缓存 `entities/<id>/knowledge.json` TTL30d 离线回退);
>   ③ 检索层 `src/meta/retrieval.h`(rerank≥1.5/authority≥2/白名单 + **五道引用硬门**:每段≥1引用、id 不悬空、
>   url 必须∈L2∪过滤后检索,任一段违规整份拒绝 + 缓存 TTL7d);④a 艺术家资料页(`/api/entity/knowledge`);
>   ④b AI 综合区 + `src/meta/discogs.h` 版本推荐(`/api/entity/summary`、`/api/entity/versions`;`metasummary` 默认 0,
>   界面显式生成)。**MusicBrainz 本机不可达 → L4 用 Discogs 匿名 API(免 token,25 req/min)**。
>   另:**耳机 EQ(OPRA)** — `src/dsp/opra.h` + `data/opra_index.json`(312KB,6230 款)+ `biquad lowShelfQ/highShelfQ`
>   + 设置页「耳机 EQ（OPRA）」(本地搜索→预设→应用到 `eq`);preamp 统一进 `ChannelProcess::gainDb`;数据 CC BY-SA 4.0 需署名。
>   排障速记:`/api/entity/knowledge` 与 `/api/entity/summary` 都是**同步网络抓取**(走缓存时 <10ms;冷启动 1~10s);
>   资料缓存都在 metabase(`rlplayer-meta/entities/<id>/`),删掉即回到空态;**uicheck 的 AI 区断言是双状态分支**。
> - **第二十二轮(基建收口 + 1.0.9)**:P3 双轨 WebView2 合并(`RLPLAYER_WEBVIEW2_MODE=auto|system|bundled`,默认 auto:
>   系统版≥包内版→系统版,否则包内版;选轨写 launcher.log);**P1 自建精简 ffmpeg**(12.18+11.97 MB,219 音频解码器,
>   出包前 `tools\verify_ffmpeg_lite.ps1` 三层守卫,重建脚本 `tools\build_ffmpeg_lite.ps1`);
>   **P2 裁 Locales**(168→2 个 pak,128.9 MB→1.79 MB);**EBWebView 不可删**(实测改名后 bundled 启动 0x80070002,
>   含 x64/x86 运行时模块,不是纯种子 profile);decode_retry 竞态修复(首轮失败必须至少再试一次);
>   ctest 45→**46**(新增 ffmpeg_matrix)。1.0.9 zip 555.7→**369.4 MB**。
>   ⚠️ 打包/启动排障注意:launcher 会扫 8899..8910,测试时**先手动起 rtaweb 到 8910** 让它「接管界面」,
>   这样绝不绑定用户端口 8899;强制包内运行时用 `RLPLAYER_WEBVIEW2_MODE=bundled`。
> - **第二十一轮(用户反馈双通道首次启用)**:新建 **AGENTS.md 铁律 8**——
>   `反馈和开发\debug\`(用户 BUG 文件夹)/ `反馈和开发\develop\`(管理员需求文本文件),
>   **处理完必须改名 `（完成）`**(全角括号);`反馈和开发\二轮开发目标\` 只是方向备忘。
>   本轮处理 2 份 debug + 1 份 develop 共 11 条:
>   ① **ffmpeg `-42` 的真因不是编码**:实测「ASCII 名被占用 → -13 / 日文名被占用 → -42」,
>      `-42` 是 ffmpeg 把 UTF-8 路径按 ANSI 代码页重解释失败后的**掩码**;真因是源文件瞬时读不了,
>      修法=Win32 探针归因 + 3 次退避重试(源可读则不重试)。
>   ② **真机「啵啵啵」不是削波,是吞吐只有 0.39 倍实时**:旧 RENDERFAIL 判据「连续 200 次」被
>      「间歇性成功」清零废掉 ⟹ 新增 **STARVING 吞吐率判据**(帧/秒 ÷ deviceRate) + w0 三类成因拆分。
>      **注意:该判据触发会回退共享并从头重播,属行为变化,未真机验证。**
>   ③ 管理员 7 条全做完(白边框第三层根因 `WM_ERASEBKGND wParam==-1`、单曲页循环/音量、
>      `rate=follow` 默认、相位说明、诊断 ZIP 独立页签、DeepSeek key 界面输入)。
> - **第二十轮(P3 启动)**:①P3 里程碑① 实体模型完成(`src/meta/entities.h`,209 断言);
>   ②dizzylab 接入完成(注册表首位,6 首拒采曲 **4/6** 命中)——**它不是 SPA,"需浏览器抓包"的前提被证伪**;
>   ③修掉两个**既有地基缺陷**:`probe_media` 从不取时长(match 的 ±3s 强信号从 P1 起一直是中性分)、
>   Release 判等键被 date 精度污染(同碟分裂成多个 Release,实测 `Swallow's Lies` 2 个 id → 1 个)。
> - **第十九轮(播放侧)**:现场日志暴露「自动采样率探测瞬时为空 → 第二首按 44100 开流 → 独占硬失败」。
>   修法=纯函数 `src/audio/rate_pick.h` + 探测重试/上次成功缓存 + 自适应回退链(明确选择档绝不替用户改率)。
>   **改播放/采样率行为前先看这个头文件。** 另一种故障(`END pos=0 error=""`)仍未定位,见 §1.6.E-11。
> **下一个开工点 = P3 里程碑② 直连层**(维基/萌娘百科/Last.fm/Discogs);**但在用户回传 1.0.8 复测日志前,
> 优先等结果**——判据是日志出现 `STARVING` 且 STAT 出现 `zp=/zs=`(证明新遥测真的接线生效)。
>   **下一个开工点 = P3 里程碑② 直连层**(维基/萌娘百科/Last.fm/Discogs)。

> 给下一个会话的 Agent 看。先读这份,再读 `STATUS.md`(历史回合)与 `ROADMAP.md`(待开发)。
> 项目铁律在 `AGENTS.md`：**不出可听声音、不 force-kill 进程树、测试要测对东西、HIP 指定 gfx1201**。

---

## 1. 一分钟上手

```powershell
# 构建（clang + lld + Ninja，无需 Visual Studio）
#   RTA_ARCH 默认 x86-64-v2（分发基线，绝不带 AVX-512）；本机开发可 -DRTA_ARCH=znver5
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_CXX_COMPILER="C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe"
cmake --build build
powershell -File tools\check_compat.ps1 -Dir build   # CRT 静态 / subsystem<=6.1 / 无 AVX-512

# 测试（45 套）
ctest --test-dir build

# DSD 素材与 DSD 域工具（本机没有真实 DSD 文件，先自己造）
build\make_dsd.exe --out build\test_dsd64.dsf --seconds 3 --verify
build\dsdconv.exe --in build\test_dsd64.dsf --info
build\dsdconv.exe --in build\test_dsd64.dsf --out build\pcm.wav          # 抽取成 PCM
build\dsdconv.exe --in build\test_dsd64.dsf --direct --out build\dop.wav   # DirectSDM + 逐位比对

# 起一个测试实例（别占用户正在用的 8899）
build\rtaweb.exe --port 8910

# 界面自检（headless Edge + CDP，52 项）
node tools/uicheck.js http://127.0.0.1:8910/ build\uicheck_out

# 停止→改采样率→再播放 的回归（Null 后端，不出声）
node tools/playtest.js http://127.0.0.1:8910/

# 一键自检（构建/测试/GPU/网络/界面/DSD/DirectSDM/分发兼容，11 项；脚本是带 BOM 的 UTF-8）
powershell -NoProfile -ExecutionPolicy Bypass -File verify.ps1
```

当前事实：**构建 0 error、ctest 45/45、界面自检 95 项全过、verify.ps1 11/11、
check_compat FAIL count 0；版本 1.0.8，已出包 dist\RLPlayer-1.0.8-win64.zip(555.7 MB)**。
出包审计：包内二进制真实报 `v1.0.8`、`/api/ping` 回 `version:1.0.8`、包内**无**用户 conf/log、`sk-` 零命中。

---

## 2. 它是什么

Windows 本地高保真播放器：自研 DSP（升频/抖动/噪声整形/SDM/卷积/矩阵），
四种输出后端（WASAPI 独占 · ASIO · NAA/RTANet · Null），AMD GPU 参与升频与卷积，
界面是**内置 WebView2 窗口**（不是浏览器窗口），托盘常驻。

```
build\RLPlayer.exe        启动器（托盘 + 内置界面窗口）
build\rtaweb.exe          播放服务 + HTTP 界面（127.0.0.1:8899）
build\rtagpu.dll          AMD GPU 核（可选，缺失自动退回 CPU）
build\WebView2Loader.dll  内置界面必需
build\asio_probe.exe      ASIO 驱动预检（可选）
dist\RLPlayer-1.0.7-win64.zip   最新已出包正式发布版（Full；ffmpeg + GPU + WebView2 + CLI 工具）
```

---

## 3. 架构地图

| 文件 | 职责 |
|---|---|
| `tools/launcher.cpp` | `RLPlayer.exe`：托盘、内置 WebView2 窗口、主题上色、实例/端口探测、子进程生命周期 |
| `tools/wv2_host.h` | WebView2 封装（**官方 SDK 头文件**，不是逆向） |
| `third_party/webview2/` | `include/WebView2.h` + `x64/WebView2Loader.dll` |
| `tools/rtaweb.cpp` | HTTP 服务、播放线程、`OutSink` 抽象（WASAPI/ASIO/NAA/Null）、全部 `/api` |
| `src/audio/wasapi_out.h` | WASAPI 独占 + 事件驱动，多声道掩码，实时增益 |
| `src/audio/asio_out.h` | ASIO 宿主（自带接口声明），环形缓冲，样本格式转换 |
| `src/audio/audio_loader.h` | WAV 原生；**DSD 原生**（`read_dsd_native`）；其它格式 ffmpeg → 临时 WAV |
| `src/audio/dsd_reader.h` | DSF / DFF 解析与写出（内部统一 MSB-first 打包） |
| `src/dsp/dsd_integrator.h` | DSD 抽取器 FIR / IIR / CIC（1-bit → PCM） |
| `src/dsp/direct_sdm.h` | DirectSDM：1-bit ⇄ DoP 24-bit（位级，不经 double） |
| `src/engine/pipeline.h` | 信号链：解码 → 矩阵 → EQ/延迟/卷积 → 重采样 → 音量 → 抖动 |
| `src/dsp/*.h` | 滤波器家族、重采样、抖动、SDM、卷积、DOP、过采样峰值… |
| `src/net/ui_page.h` | 页面骨架（CSS 全走变量，三套主题） |
| `src/net/ui_script.h` | 全部前端逻辑（视图/队列/正在播放/设置页/拖拽） |
| `src/net/library.h` | 库索引（增量 tsv）、封面缓存 |
| `tests/*.cpp` | 28 套客观验证（`dsd_reader` / `dsd_path` 是本轮新增） |
| `tools/make_dsd.cpp` | DSD 素材生成器（调制器 → DSF/DFF，写出后读回逐位比对） |
| `tools/dsdconv.cpp` | DSD 域命令行：`--info` / 抽取 / `--direct`（DirectSDM 逐位比对） |

---

## 4. 关键设计决策（别轻易推翻）

1. **界面用内置 WebView2，不用 `msedge --app`**：Edge 的标题栏归它自己画，`theme-color`/`DWM` 都改不了；
   独立 profile 还会触发 Edge 首次运行（装扩展）。自建窗口后标题栏随主题即时变色，且没有浏览器外壳。
   缺运行库会自动退回 Edge（功能不变）。
2. **`OutSink` 抽象**：播放线程只认 `open/start/write_one_period/pause/resume/close`，换后端不动主循环。
3. **GPU 动态加载**：`rtagpu.dll` 缺失/架构不符时退回 CPU，结果逐位相同；**不做链接期依赖**。
4. **DSD 走 DoP**：管线升到 DSD 速率 → 1-bit 调制 → 装进 24-bit 容器；Native DSD 未实现（需驱动支持 ASIO DSD 类型）。
5. **界面参数只有一个真相源**：可见控件都在设置页；右栏不再放参数（曾因两处各有一份而不一致）。
6. **DSD 源走原生读取，不再默认经 ffmpeg**：`.dsf/.dff` → `read_dsd` → FIR 抽取到 DSD/16（上限 705.6 kHz）→ 原管线；
   读取失败才回退 ffmpeg，并在 note 里写明。理由：少一层量化、拿得到 1-bit 流、DirectSDM 才有基础。
7. **DirectSDM 只在「同速率 + 无 PCM 处理 + 0 dB」时成立**（`direct_sdm_decision`），任何一条不满足都老实走调制器并说明原因；
   1-bit 与 DoP 的换算是**位级**的（每 16 位 = 两个相邻字节），不经过 double，才能主张逐位一致。

---

## 5. 踩过的坑（按代价排序，务必别再踩）

1. **启动器按窗口标题杀进程，把宿主杀了两次**。现在只认自己 `CreateProcess` 记录、且校验映像名为 `msedge.exe` 的 PID；
   WebView2 模式下根本不启动 Edge。**任何“按标题/按名字杀进程”的写法都不许再引入。**
2. **`ASIOCallbacks` 写成局部变量** → 驱动只存指针，回调一次不触发、约 2 秒后崩。必须做成员。
   这类 bug 的判据陷阱：`欠载 0 次` 是因为回调没跑，不是“跑得好”。
3. **有损格式 `bits_per_raw_sample = N/A`** → `atoi` 得 0 → 退回 16 bit → 解码阶段就引入 −101 dBFS 量化误差。未知一律按 32 bit。
4. **设置页多了个 `</div>`** → 声道微调被挤出 `.spane`，每页都显示。定位方法：`el.closest('.spane')` 是否为 null。
5. **SVG 无引号属性**（`<circle cx=12 r=9/>`）→ `/>` 被并进属性值（`9/`）→ 半径无效 → 图标空白。SVG 属性一律加引号。
6. **界面自检污染用户设置**：uicheck 曾把 `taps` 写成 131072 并落盘。自检必须记录原值并还原。
7. **PowerShell 的 `$pid` 是只读自动变量**，赋值会静默失败并可能杀错进程。用 `$owner` 这类自定义名，杀前校验映像名。
8. **HIP 必须 `--offload-arch=gfx1201`**，且先用平凡 kernel（写常数）验证能执行；不要相信 `hipDeviceSynchronize()` 的成功返回。
9. **WASAPI 独占 + 事件驱动**：`GetBuffer` 必须整周期；跳转不要调用 `Reset()`（会导致进度卡死）。
10. **PowerShell 5.1 读无 BOM 的 UTF-8 中文脚本**会按 GBK 解释、解析失败（本机没有 pwsh 7）。
    `verify.ps1` 已存成 **UTF-8 带 BOM**；编辑工具改完要确认 BOM 还在（`EF BB BF`），否则解析会报
    “Missing type name after '['” 这类莫名其妙的错。
11. **Edge 标题栏**只有浏览器自己的主题能决定；`DwmSetWindowAttribute` 对它返回 S_OK 但不生效（实测把标题栏染红也没反应）。
12. **WebView2 退出必须调用 controller->Close()**，否则强杀启动器会留下孤儿 msedgewebview2.exe（实测一次攒了 6 个）。已在 WM_DESTROY 里关闭并释放；实测“关窗后 WebView2 进程数回到基线、RLPlayer 退出”。
13. **测试探针本身会骗人**：用 `sin(...) >= 0` 生成方波，`sin(2πk)` 是 ±1e-16、符号随 k 乱跳，加上每周期 16.98 样本，
    占空比不是 50%，探针自己带宽带杂散与偶次谐波。三种抽取器因此都测出“约 101 dB 混叠抑制”，
    真相是 144～156 dB。探针要按**整数样本相位**生成（period = rate/freq）。
14. **非整周期测量会把直流漏进单音相关**：方波第 9 次谐波正好在 Nyquist，抽取后变直流；
    测量窗不是折叠频率整周期时，19.6 kHz 被抬到 −90 dBFS。测量长度要取整周期（或先去掉直流）。
15. **THD+N 不等于“挖掉谐波 bin 后的 SNR”**：第一版把 2f/3f… 的 bin 一起挖掉，报 98 dB；
    诚实时值 99.2 dB（LS 拟合基波、谐波留在残差），而把 4001 抽头参考 FIR 的启动瞬态算进去会掉到 54 dB。
    **先跳过滤波器启动瞬态，再谈指标。**
16. **无约束最小二乘补偿会在通带外失控**：CIC 的 17 抽头补偿只拟合 0–20 kHz，解在 156 kHz 有 +74 dB，
    把 1-bit 带外噪声放大成 −29 dBFS 巨浪（THD+N 反而“变好”）。补偿器要么有界（3 抽头解析，Nyquist +6.4 dB），
    要么把带外也纳入约束。
17. **DSF/DFF 位序与布局别猜**：DSF 是 **LSB-first + 每声道 blockSize 块交错**，DFF 是 **MSB-first + 按声道连续**；
    ffmpeg 也用 `dsd_lsbf` / `dsd_msbf` 两套解码器。写错位序时“长度对、听感也对”，只有逐位比对能发现。
18. **`auto` 会拷贝全局设置**：`auto st = dsd_pcm_settings();` 改的是副本，界面选了 CIC/XFi 实际仍跑 FIR/narrow，
    没有任何报错。要改全局必须写 `auto&`。判据：改完去看 `/api/signal` 或状态里的「实际生效值」。
19. **Windows 防火墙弹窗会挡住 GUI 自动化**：运行 networkaudiod.exe（或任何新监听程序）时系统弹「是否允许网络访问」，
    它会抢焦点、让后续 UIA/键盘操作全部落空。只做本机回环时点「取消」即可，不要随手放行。
20. **分发版绝不能带 -march=native/znver5**：2026-10-21 的 1.0.0 因此在 5900X 上 0xC000001D（EVEX/AVX-512）。
    CMake 现在默认 `RTA_ARCH=x86-64-v2`，并有 check_compat 门禁；**不要为了本机快几个百分点把它改回去**。
    注意：门禁查的是 zmm/EVEX；反汇编里仍能看到 ymm —— 那些是静态 CRT 的 strlen/memcpy 分派路径，
    由 CPUID 选择，不算违规。
21. **动态 CRT 会让干净系统打不开**：1.0.0 导入 VCRUNTIME140/MSVCP140/UCRT，用户没装 VC++ 运行库就 0xC000007B。
    现在静态链接（CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded），exe 只导入系统 DLL。

---

## 6. 运行环境事实

- 端口：默认 **8899**；启动器扫 8899~8910，并用 `/api/ping`（返回 `{"app":"RLPlayer"}`）确认是自家实例。**不要只按“端口通”判断**。
- 配置（都在 `build\`，与 exe 同目录）：
  `rlplayer-settings.conf`（全部参数）、`rlplayer-library.conf`（库根目录）、
  `rlplayer-recent.txt`、`rlplayer-playlist.txt`、`rlplayer-asio-cache.txt`（驱动预检结果）、`uploads\`（拖拽兜底）、`webview2\`（界面缓存）。
- 日志：`build\launcher.log`（启动器）、`build\rlplayer-playback.log`（播放格式/峰值/欠载，
  PLAY/STAT/END，4 MB 轮转）、`/api/dbg`（播放线程环形日志）、`/api/status`（peak/clip）。
- 解码器：WAV/DSF/DFF 原生；其它格式走 ffmpeg。**优先找 exe 同目录的 ffmpeg.exe/ffprobe.exe**，其次 PATH（Lite/Full 包的差别就在这里）。
- DSD：本机**原本没有任何 .dsf/.dff**，测试素材用 `build\make_dsd.exe` 现造；`ffmpeg 9.0` 的 dsf 解复用可用（ffprobe 报 `dsd_lsbf_planar`），可作独立交叉验证。
- DSD 源接入后：`.dsf/.dff` → 原生解码到 176.4/352.8/705.6 kHz 的 32-bit PCM（DSD64 10 秒解码 0.26 s），再走原管线；`/api/signal` 的 `note` 会写「DSD 原生解码」。
- 全部 DSD 验证**不需要打开音频设备**：`dsdconv`、`make_dsd`、`ctest`、Null 后端 HTTP 播放都能覆盖。
- 已废弃：`edge-app.pid`、`edge-app-dark/light`（改 WebView2 后不再使用，可删）。

---

## 7. 脚本清单

| 脚本 | 用途 |
|---|---|
| `tools/uicheck.js <url> <outdir>` | 界面自检 52 项（含队列页/地址栏/正在播放页/设置页签/微调只在一页/DirectSDM 开关） |
| `tools/playtest.js <url>` | 停止 → 改采样率 → 再播放（Null 后端，不出声） |
| `tools/audit_settings.ps1` | 21 项参数是否真的生效（需 8910 上的测试实例） |
| `tools/asio_probe.cpp` | ASIO 驱动分步验证（`--check` / `--stream N`） |
| `tools/wv2_probe.cpp` | WebView2 运行库探测（历史遗留，SDK 化后可选） |
| `verify.ps1` | 一键 11 项（构建/测试/GPU/网络逐位/界面/DSD/DirectSDM/分发兼容）；**UTF-8 带 BOM**，PS 5.1 才能正确解析中文 |
| `tools\check_compat.ps1` | 分发门禁：静态 CRT / subsystem ≤ 6.1 / 无 zmm+EVEX |
| `tools\make_dist.ps1` | 构建 → 门禁 → 组包 → zip + sha256（门禁失败拒绝出包）；`-WithFfmpeg` 出带解码器的 Full 包 |
| `build\naa_probe.exe` | NAA 控制通道只读探针（`--host x --matrix`；看 16 字节握手与各种报文尝试） |
| `verify-stream.ps1` | 分块不变性门禁 |
| `build\make_dsd.exe` | DSD 素材生成（DSD64/128/256，DSF/DFF，写出后读回逐位比对；`--verify` 出 THD+N） |
| `build\dsdconv.exe` | DSD 域转换：`--info` / 抽取（fir\|iir\|cic）/ `--direct`（DirectSDM + 逐位比对） |

---

## 8. 下次会话怎么开始

1. 读：`HANDOVER.md`（本文）→ `STATUS.md`（最近回合）→ `ROADMAP.md`（待开发）。
2. 只读确认：`Get-NetTCPConnection -State Listen | ? LocalPort -eq 8899`、`Get-Process RLPlayer,rtaweb`。
   **用户可能正在听音乐，不要随手停他的实例**；改代码用 8910 的测试实例验证。
3. 动手顺序：改 → `cmake --build build` → `ctest --test-dir build` → `node tools/uicheck.js http://127.0.0.1:8910/ <dir>`。
4. 需要重启用户实例时：先告知，再用 `build\RLPlayer.exe`（它会复用/接管 8899）。

---

## 9. 未完成（详见 ROADMAP.md）

1. **DSD 域处理** —— 第八轮已完成全部 P0（读取器 / 素材生成器 / 抽取器 / DirectSDM 接线 / Conversion 预设）。
   剩下的是**性能**：DSD256 整曲仍慢、占内存大；计划两级抽取（CIC 先降 4× 再 FIR），判据沿用 P0-4。
2. **调制器族**（7 阶 EC / 多电平），只做可测版本，不复制 HQP 专有曲线。
3. **滤波器变体补齐**（按 HQP 命名，每个都出指标表）。
4. **NAA 兼容** —— 端点已到位（192.168.50.109，标准 NAA 5.x）。卡在客户端握手：
   服务端先发 16 字节随机数，纯 XML 试探全部无回应（见 docs/NAA对接探测纪要.md）。
   要继续需要一次真实 HQPlayer 会话抓包，或用户明确授权逆向（EULA 禁止）。
   工具 `naa_scan` / `naa_probe` 已就绪。RTANet 仍是等价方案。
5. **输入采集**（WASAPI capture）—— 现在只有 UI，通路未实现。
6. **UPnP 投放** —— 只有发现。
7. **解码去临时文件**（管道/进程内 libavcodec）—— 性能，不影响音质。
8. **DSD 抽取性能**：DSD256 整曲解码仍慢/占内存大；计划两级抽取（CIC 先降 4× 再 FIR），判据沿用 P0-4。
