# RLPlayer 开发路线图

> 写于 2026-10-21，更新于 2026-10-24（1.3.16 版本后）。
> 所有条目均按「可测」维护：**判据、基线、未解决项**，没有判据的功能不算完成。
> 阶段全景状态见 `STATUS.md`；交接先读 `HANDOVER.md`；技术债务与审计见 `docs/未完成项清单.md`。


## 进度快照（2026-10-24，第三十七轮 / 1.3.16 发布后）

| 阶段 | 状态 | 对应轮次/证据 |
|---|---|---|
| P0（DSD 域处理） | ✅ 核心完成 / 待办已列 | 第七/八轮完成 DSF/DFF 读取、素材生成、DirectSDM（逐位一致）、抽取器（FIR/IIR/CIC）、Conversion 三组真预设。**未完成项**：DSD→DSD 速率转换（需多相抽取再调制）、ASIO Native DSD（无硬件端点支持）转入 1.3.17+ 待办。 |
| P1（调制器族） | ✅ 完成 | 第 24 轮：`dsd/chebyshev/ec/ahm` 四族，配置键 `modfamily`；Chebyshev-7 低电平失稳已修复（强制降 5 阶）；过载恢复与长跑判据见 STATUS 第 24 轮 §3。 |
| P2（滤波器变体与声学处理） | ✅ 完成并扩展 | 第 24 轮：47 种重采样命名预设（真实抽头/过渡带/阻带/相位）。<br>• **20 段 PEQ**（第 34~36 轮）：RBJ 双二阶、float64、20 点拖拽/自动前级/旁路语义。证据：`src/dsp/peq.h`、`tests/test_peq.cpp`（61 断言）、`tools/peqcheck.js`（22/22）。<br>• **IR 重采样**（第 37 轮）：解决 44.1k IR 载入 96k 管线时基错位 2.18 倍问题，频响差 0.0000 dB、脉冲位移 +0.129 样本。证据：`src/dsp/ir_resample.h`、`tests/test_ir_resample.cpp`。 |
| P3（网络音频传输） | 🔄 路线重构（NAA 删除 / UPnP 收敛） | • **NAA 彻底删除**（第 37 轮 / 1.3.16）：因 HQPlayer NAA 5.x 握手私有闭源（16 字节挑战无响应）且 EULA 禁止逆向，按用户要求彻底移除 NAA 发现/探测工具与 UI 选项（老配置显式报错）；RTANet 保留用于命令行与单测。证据：`tools/naa_discover.h` / `naa_probe.cpp` / `naa_scan.cpp` 已删、`tests/test_rtanet_e2e.cpp`。<br>• **投放收敛至 UPnP**：见 P5。 |
| P4（WASAPI 输入采集与输出缓冲） | ✅ 采集可用 / 独占守卫落地 | • **输入采集**（第 24/30/33 轮）：`wasapi_capture.h` + `rtacapture.exe` + `/api/input/*`，Widget B 采集实测通过（rms −64.18 dB）。监听直通（未接 DSP 链）留待办。<br>• **独占缓冲自适应策略**（第 35 轮）：删除 20ms 封顶、L1 放大、L2 启动看门狗、学习缓存。证据：`src/audio/render_loop.h`、`tests/test_buffer_policy.cpp`（49 项）、`tests/test_render_loop_dry.cpp`（45 项）。<br>• **设备速率守卫与超时熔断**（第 37 轮 / 1.3.16）：真机 N300 96k 独占实测 56.4k（标称 59%）掉线复现；开播 1.5~2s 实测速率 <97% 自检熔断，连续超时且零消费干净停播。证据：`src/audio/render_loop.h`、`tests/test_renderfail.cpp`。 |
| P5（UPnP 投放） | ✅ 全链路落地（控制+流服务+端到端） | 突破早期「只做发现不做投放」限制：<br>• **服务与发现**（第 32/33 轮）：多网卡连发绕开虚拟网卡 TUN 抢组播，实测搜到 N1000_UPNP @ 192.168.50.109；内置 `UpnpHttpServer`（端口 8912~8921 冲突回退），端到端投放 PCM 69.5MB、`rateMatch=match`。<br>• **UI 贯通**（第 34 轮）：单曲页一键推流至数播、流媒体页推送区块、全局推流切换。证据：`tests/test_upnp_control.cpp`、`tests/test_cast_service.cpp`。 |
| P6（性能、资料层与系统工程） | ✅ 完成并大幅演进 | • **核心性能**（第 25 轮）：库扫描并行化（3.18×）、P6b 内存流式解码、P6c DSD 两级抽取（CIC+FIR，2.30×）。<br>• **AI 资料与自动整理**（第 33~36 轮）：白名单扩容/rerank 0.8/双步并行检索、结构化标签、单曲 4ms/专辑 60ms 自动预取当前专辑（并发≤2）、同实体缓存复用。证据：`src/meta/retrieval.h`、`tests/test_metacache.cpp`。<br>• **GPU 语义拆分**（第 36 轮）：修复 `ps.gpu` 未复位假绿点，拆分为 `gpu/gpuUsed/gpuResample/gpuConv` 事实源，每曲复位。证据：`src/meta/statusflags.h`、`tests/test_playback_gpu.cpp`。<br>• **knowledge 异步化**（第 37 轮 / 1.3.16）：由 accept 阻塞 1477ms 改为后台 job + 700ms 轮询，首响 1325ms→1.3ms。证据：`tools/rtaweb.cpp`、`src/net/ui_script.h:1057`。<br>• **LLM 流式接口与传输**（第 37 轮 / 1.3.16）：`llm_chat_stream`（SSE 逐 delta）+ 平台层 `HttpRequest::onChunk` 真流式 POST，告别整站枯等。证据：`src/meta/llm.h`、`src/platform/http.h`、`tests/test_retrieval.cpp`。 |
| P7（分发、宿主交互与无边框） | ✅ 持续发布与无边框演进 | • **分发矩阵**：1.0.9 ~ 1.3.16 持续提供 FULL 自包含正式发布版（内置 ffmpeg/rtagpu/amdhip64/WebView2Loader/rtagpu_cuda.dll）。<br>• **无边框结构修复**（第 35 轮）：定位 WebView2 跨进程内缩 5px 结构真因，EnumChildWindows 重合判据。证据：`tools/wv2_host.h`、`tools/bordercheck.ps1`（45/45）。<br>• **窗口四边拉伸**（第 36 轮）：前端 8 边缘 5px 热区 -> postMessage resize -> 宿主跟手 SetWindowPos，绕过 WebView2 子窗口吃掉 WM_NCHITTEST。证据：`src/platform/host_msg.h`、`tests/test_host_msg.cpp`、`tools/launcher.cpp`。<br>• **托盘后台常驻**（第 37 轮 / 1.3.16）：点 X / Alt+F4 隐藏到托盘不中断播放，单双击恢复，右键退出干净释放。证据：`tools/launcher.cpp`、`build-tray/probe-tray.ps1`（26/26）。 |

---

## 排序原则

1. **先有验证对象，再写功能**（没有测试素材就先做素材生成器）。
2. 每条都必须能用**现有测试框架**回归（ctest / uicheck / verify.ps1 / bordercheck / probe-tray）。
3. 做不到的写清楚「为什么做不到」，不留假开关；拿不准的如实标注「待确认/未验证」。

---

## P0 · DSD 域处理

> **第七/八轮**：读取器 / 素材生成器 / 三种抽取器 / DirectSDM 核心与播放线程端到端逐位比对；Conversion 三组预设与数值表。
> **1.3.16 状态**：DSD 基础处理链全部闭环可用。

### P0-1 DSF / DFF 读取器（`src/audio/dsd_reader.h`）✅ 已完成（第七轮）
- DSF（LSB-first、分块交错）与 DFF（MSB-first、连续块、DST 压缩明确拒绝）完整支持。
- **验收实测**：写→读逐位一致；测试 `test_dsd_reader`。

### P0-2 测试素材生成器（`tools/make_dsd.cpp`）✅ 已完成（第七轮）
- 5 阶 1-bit 调制器生成 DSD64/128/256 写 DSF/DFF；抽取后 THD+N 99.31 dB。测试 `test_dsd_path`。

### P0-3 DirectSDM（直通）（`src/dsp/direct_sdm.h`）✅ 已完成（第八轮）
- 目标速率一致且无 PCM 处理/0 dB 时走 `play_one_direct`（16 位切片 → DoP → 输出后端）。
- **验收实测**：1-bit → DoP → 解包逐位一致；测试 `test_dop`、`test_dop_stream`。

### P0-4 Integrator（抽取器）：FIR / IIR / CIC（`src/dsp/dsd_integrator.h`）✅ 已完成（第八轮）
- FIR（163 抽头 Kaiser-sinc）、IIR（Butterworth 10 阶）、CIC（6 级无乘法 + 3 抽头补偿）。
- **验收实测**：通带最差 0.000025 / 0.019 / 0.031 dB；混叠抑制 144.4 / 156.1 / 108.1 dB。测试 `test_dsd_path`。

### P0-5 Conversion：wide / narrow / XFi ✅ 已完成（第八轮）
- 真实截止/过渡带/阶数预设（通带上限 20/30/40 kHz，过渡带 136.4/116.4/96.4 kHz）。测试 `test_dsd_path`。

---

## P1 · 调制器族

> **第二十四轮**：已完成全部四族建设。

- **四族实现**：`dsd`（标准多阶）、`chebyshev`（Chebyshev NTF 可配带外增益）、`ec`（误差反馈变体）、`ahm`（多电平 2~8 bit）。
- **稳定性修复**：Chebyshev-7 低电平失稳已修复（强制降 5 阶）。
- **验收实测**：10 分钟长跑无饱和锁死、过载后恢复判据。测试 `test_modulator`、`test_modulator_families`。

---

## P2 · 滤波器变体与声学处理

> **第二十四轮**：完成 47 种重采样滤波器体系。
> **1.3.13~1.3.16 轮次**：新增 20 段 PEQ 参量均衡与 IR 采样率自适应重采样。

### P2-1 重采样滤波器预设体系 ✅ 已完成（第二十四轮）
- 补齐 47 种预设（`poly-sinc-*`、`sinc-{S,M,MG,L,LL}`、`closed-form-*`、`gauss` 等），抽头/过渡带/阻带衰减/相位对比表由 `tools/filter_report.exe` 自动生成。测试 `test_filters`。

### P2-2 20 段 PEQ 参量均衡（`src/dsp/peq.h`）✅ 已完成（1.3.13~1.3.15）
- **算法与结构**：RBJ 双二阶滤波器、float64 精度计算系数、热更新平滑淡入。
- **界面交互**：SVG 曲线 20 点自由拖拽调节（Shift+拖拽/滚轮调节 Q 值、双击复位）、自动前级防削波计算。
- **旁路语义**：默认 `bypass=0`（生效，全 0 dB 不变音），勾选旁路（`bypass=1`）直通跳过处理；常驻状态行展示。
- **验收实测**：`test_peq.cpp` 61 项断言全过；`tools/peqcheck.js` 表头/数据列对齐 22/22 全 0.00px 偏差。

### P2-3 IR 脉冲响应重采样（`src/dsp/ir_resample.h`）✅ 已完成（1.3.16 / 第三十七轮）
- **真因**：此前 IR 样本数组缺乏采样率标识，44.1k 房间测量脉冲放入 96k 工程直接拉长 2.18 倍，导致时基与频响失真。
- **算法落地**：根据当前播放采样率自适应多相重采样（`ir_resample`）。
- **验收实测**：脉冲中心误差 +0.129 样本、频响差异 0.0000 dB、同采样率逐位相同。测试 `test_ir_resample`。

---

## P3 · 网络音频传输（NAA 彻底删除 / UPnP 替代）

> **历史记录**：Accurate Audio 数播硬件实测 HQPlayer NAA 5.x 握手为 16 字节私有挑战，非公开逆向因 EULA 限制受阻。
> **1.3.16 决议**：用户明确指示彻底放弃并删除 NAA 对接，将数播与局域网网络推流全面统一至 UPnP / DLNA 路线。

### P3-1 移除 NAA 专有客户端（1.3.16 / 第三十七轮）
- 界面删除「NetworkAudioAdapter（NAA / RTANet）」输出选项、端点自动发现及手动输入框。
- 源码清理：删除 `tools/naa_discover.h`、`tools/naa_probe.cpp`、`tools/naa_scan.cpp`，删除 `/api/naa/discover` 路由。
- 配置兼容：老配置残留 `backend=naa` 启动时明确报错并提示迁移，杜绝静默异常。
- 保留项：底层轻量传输协议 RTANet 保留用于命令行与端到端自动化回归测试（`test_rtanet`、`test_rtanet_e2e`）。

---

## P4 · WASAPI 输入采集与缓冲安全守卫

> **第二十四/三十/三十三轮**：完成 WASAPI 采集与端到端检验。
> **1.3.14/1.3.16 轮次**：全面重构独占模式缓冲时钟与设备异常保护机制。

### P4-1 WASAPI 输入采集（`src/audio/wasapi_capture.h`）✅ 基础功能完成
- `IAudioCaptureClient` 无锁 SPSC 环形缓冲 + 格式转换；提供 `rtacapture.exe` 诊断与 `/api/input/*` 控制。
- **实测验证**：Widget B 麦克风采集端到端通过（rms −64.18 dB，生成 1,440,044 B 校验 WAV）。测试 `test_capture`。
- **未完成项**：监听目前为直通输出，尚未接进完整 DSP 分块处理链（转入 1.3.17+ 待办 A5）。

### P4-2 独占缓冲自适应策略（`src/audio/render_loop.h`）✅ 已完成（1.3.14 / 第三十五轮）
- **真因分析**：删除独占模式缓冲区 20ms 硬编码无条件封顶（导致低开销设备周期断流与边播边啵啵啵）。
- **阶梯退避与缓存**：L1 放大预检 + L2 起播 0.8s 看门狗检测；按设备标识记住稳定缓冲周期（学习缓存）。
- **观测与恢复**：遥测新增 `PLAYBUF`、`WFAIL`、`DEVICE_LOST` 错误流，端点掉线显式上报。
- **验收实测**：测试 `test_buffer_policy`（49 项）、`test_render_loop_dry`（45 项）。

### P4-3 设备速率守卫与超时熔断（`src/audio/render_loop.h`）✅ 已完成（1.3.16 / 第三十七轮）
- **真机故障复现**：EDIFIER N300 声卡在 96k/24 独占模式下，驱动 `IsFormatSupported` 返回支持但在 29.8s 墙钟内仅消费 17.5s 数据（实测速率 56.4 kHz，仅标称 59%），引发 21 次超时并在第 29s 从 USB 总线掉线。
- **开播速率自检**：起播 1.5~2.0 秒内统计设备实际消费帧率与墙钟比值，若实测速率 < 97% 立即熔断停播并显式告警「设备无法实时运行目标采样率，已停止」。
- **超时保护熔断**：连续写超时且设备消费增量为零时触发保护停播，避免设备固件假死崩溃。
- **验收实测**：测试 `test_renderfail` 全面验证超时熔断与速率不足拦截。

---

## P5 · UPnP / DLNA 投放

> **从 1.3.10 起彻底打破「只做发现不做投放」壁垒，实现端到端数播推送。**

### P5-1 UPnP 设备探测与代理绕行 ✅ 已完成（1.3.10 / 第三十二轮）
- 修复 Clash Verge TUN 等虚拟网卡抢占默认组播路由导致 SSDP 无法送达局域网问题，改为逐物理网卡绑定 `IP_MULTICAST_IF`。
- **真机验证**：实测精准发现用户数播设备 `N1000_UPNP @ 192.168.50.109`。

### P5-2 UPnP HTTP 音频推流服务 ✅ 已完成（1.3.12~1.3.13）
- 内置轻量 `UpnpHttpServer`，支持 8912~8921 端口动态冲突回退。
- 通过 AVTransport 协议控制渲染器播放/暂停/跳转，本地提供原始无损 PCM 流。
- **真机实测**：数播投放实测通过（servedBytes 69.5 MB，`rateMatch=match`）。测试 `test_upnp_control`、`test_cast_service`。

### P5-3 播放控制与单曲推送集成 ✅ 已完成（1.3.13 / 第三十四轮）
- 单曲详情页提供一键推流至数播按钮；流媒体页设立推送控制面板；支持全局推流与本地扬声器无缝切换。

---

## P6 · 性能、资料层与系统工程

> **1.3.12~1.3.16 进行了深度的资料层智能化改造与后端异步/流式重构。**

### P6-1 核心处理吞吐与流式解码 ✅ 已完成（第二十五轮）
- 媒体库扫描多线程并行化（653 首 30.16s → 9.47s，**3.18×**，哈希逐字节一致）。测试 `test_scanpool`。
- 内存流式解码（去掉中间临时 WAV 文件，磁盘写入 0 字节）。测试 `test_stream_decode`。
- DSD 两级降采样抽取（CIC 4× 预抽取 + 短 FIR，5 分钟 DSD256 处理 16.2s → 7.1s，**2.30×**）。测试 `test_dsd_two_stage`。

### P6-2 智能元数据与自动整理闭环 ✅ 已完成（1.3.12~1.3.15）
- 检索白名单扩容（覆盖 20+ 主流音乐站）、rerank 门槛调谐至 0.8、换词归一化与并行抓取（`max(knowledge, search+fetch)`）。
- 结构化标签系统（schema 校验、7 天长效缓存、命中即秒回）。
- 自动后台整理：单曲页打开 4ms 自动整理当前曲；专辑页 60ms 自动预取当前专辑曲目（并发≤2）；离开即停；同实体共享缓存。
- 验收实测：测试 `test_metacache`、`test_retrieval`、`test_entities`。

### P6-3 GPU 状态语义解耦（`src/meta/statusflags.h`）✅ 已完成（1.3.15 / 第三十六轮）
- **真因**：旧版 `ps.gpu` 缺乏每曲复位机制，一旦单曲触发过 GPU 升频，后续全部曲目错误报绿。
- **语义拆分**：解耦为 `gpu`（环境可用性）、`gpuUsed`（本次参与实测）、`gpuResample`（重采样参与）、`gpuConv`（卷积参与）。
- **实测验证**：每次起播复位事实源；界面仅在 `gpuUsed=true` 时亮绿点。测试 `test_playback_gpu`。

### P6-4 Knowledge 直连层异步化改造 ✅ 已完成（1.3.16 / 第三十七轮）
- **真因**：此前 `/api/entity/knowledge` 在主循环同步执行，耗时超过 1.4 秒导致整站 API 冻结。
- **异步 Job 模型**：改造为后台异步线程抓取 + `pending` 轮询（前端 700ms 轮询重试）。
- **实测性能**：首次接口响应耗时 1325ms 骤降至 **1.3ms**，期间主站状态查询延迟由 1532ms 降至 **1.3ms**。

### P6-5 LLM 流式输出与底层 SSE 传输 ✅ 接口与传输完成（1.3.16 / 第三十七轮）
- 后端新增 `llm_chat_stream`，支持 SSE 增量 delta 回调与断线识别。
- 网络层新增 `HttpRequest::onChunk` 与 `http_post_stream` 真流式传输。
- 检索层 `retrieval_fetch` 挂接进度回调。测试 `test_retrieval`。
- **待办**：前端 UI 逐字流式渲染（排入 1.3.17+ 待办 D2）。

---

## P7 · 分发、宿主交互与无边框架构

> **自 1.0.9 起确立 FULL 独立分发标准，并在 1.3.14~1.3.16 完成无边框与系统托盘深度重构。**

### P7-1 正式发布包规格与自包含策略 ✅ 持续落地
- **构建矩阵**：Windows 7 SP1 ~ 11，x86-64-v2 兼容架构，静态 MSVC CRT，无额外运行库依赖。
- **自包含组件**：打包内置 `ffmpeg`、`ffprobe`、`rtagpu.dll`（AMD HIP）、`rtagpu_cuda.dll`（NVIDIA CUDA 12.9）、`amdhip64_7.dll`、`WebView2Loader.dll`。
- **构建门禁**：`tools/check_compat.ps1` + `tools/make_dist.ps1` 打包前自动执行兼容性与依赖审计。

### P7-2 无边框一体化窗口真因修复 ✅ 已完成（1.3.14 / 第三十五轮）
- **真因定位**：WebView2 控件创建时相对客户区默认内缩 5px，启动链路缺乏初始 `WM_SIZE` 导致宿主未被覆盖，呈现出 5px 未初始化白框。
- **修复与判据**：移除内缩并引入结构判据（`EnumChildWindows` 测量 WebView2 必须完全重合客户区）。测试 `tools/bordercheck.ps1` 45/45 全 PASS。

### P7-3 窗口四边自由拖拽拉伸 ✅ 已完成（1.3.15 / 第三十六轮）
- **真因定位**：由于 WebView2 子窗口运行在跨进程独立渲染管线，系统 `WM_NCHITTEST` 消息被子窗口全部拦截返回 `HTCLIENT`，导致原生窗口边缘拉伸失效。
- **页面代理拉伸**：在页面 8 个边缘注入 5px 鼠标捕获热区，识别拖拽意图通过 `postMessage('resize:<dir>')` 发往宿主，由宿主代理接管鼠标实现 `SetWindowPos` 跟手缩放。
- **实测验证**：单测 8 方向协议白名单（`test_host_msg`），端到端拖拽实测通过。

### P7-4 系统托盘后台驻留与安全退出 ✅ 已完成（1.3.16 / 第三十七轮）
- 点击窗口关闭按钮（X）、Alt+F4 或页面 close 时，窗口平滑隐藏至系统托盘，后台音频与引擎子进程继续稳定运行。
- 托盘图标左键单击/双击即刻唤醒主窗口至前台焦点。
- 托盘右键菜单选择「退出」触发优雅退出序列：先行通知 `/api/quit` 释放网络端口与声卡资源。
- 实测验证：通过 `build-tray/probe-tray.ps1` 26/26 项测试验证隐藏、唤回与退出逻辑。

---

## 2026-10-24 之后的待办（1.3.17+）

> 汇总自 `docs/未完成项清单.md`（A/B/C/D 节）与 1.3.16 验收现场，标注卡点原因与所需前提条件。

### A. 待实现功能（代码级缺口）
1. **ASIO Native DSD 未实现（A1）**：
   - *现状与影响*：`src/audio/asio_out.h` 尚无 Native DSD 通路，UI 标注「选 Native 不会出声」；目前仅能走 DoP。
   - *为什么没做 / 需要什么*：需要物理声卡驱动明确汇报 `ASIOSTDSDInt8*` 采样格式。本机当前 7 个驱动（Portacapture、VB-Matrix 等）均仅汇报 Float32 或 Int32，缺少真机验证对象，需接入兼容硬件后实施。
2. **DSD→DSD 速率转换（A3）**：
   - *现状与影响*：非目标速率的 DSD 源文件目前需解成 PCM 后再重新调制，升频存在理论级再量化。
   - *为什么没做 / 需要什么*：需要 DSD 域多相插值/重抽取算法与严格的带内带外 THD+N 判据，算法复杂度高，需单独立项。
3. **WASAPI 共享模式非 float32 格式支持（A4）**：
   - *现状与影响*：`tools/rtaweb.cpp:1918` 遇到系统混音格式设为 16/24-bit 整数时直接抛错拒开。
   - *为什么没做 / 需要什么*：需要补充 int16/int24 → float32 格式转换器与边界单测。
4. **输入采集监听直通未接 DSP 链（A5）**：
   - *现状与影响*：`tools/rtaweb.cpp:5831` 当前采集监听为直通，无法经过 PEQ/卷积/音量控制等处理。
   - *为什么没做 / 需要什么*：需将采集环形缓冲区输出接入主播放管线的分块 DSP 接口。
5. **Intel / 其它 GPU 计算后端（A7）**：
   - *现状与影响*：目前仅支持 AMD HIP 与 NVIDIA CUDA，Intel 核显及 Arc 独显用户只能走纯 CPU。
   - *为什么没做 / 需要什么*：需立项评估 D3D12 DirectCompute 或 oneAPI 后端，涉及跨厂商驱动兼容矩阵。
6. **封面写回与畸形保护（A8）**：
   - *现状与影响*：补全的封面数据仅存在本地元数据库，尚未写回音频文件内标签。
   - *为什么没做 / 需要什么*：需实现 FLAC PICTURE 块重嵌逻辑，并对已有畸形 PICTURE 块做校验与容错保护。

### B. 已知缺陷与待复现定位
1. **`END pos=0 error=""` 显式归因（B1）**：
   - *现状与影响*：偶发切歌打断时上报 pos=0 且无明确原因字符串，目前需靠上下文 START 推断。
   - *为什么没做 / 需要什么*：需要一份完整的切歌打断现场复现日志以定点增加 `PLAY_ABORT` 显式标识。
2. **STARVING 触发后回退共享并从头重播（B2）**：
   - *现状与影响*：独占严重欠载时自适应回退至共享模式，当前逻辑会重头开始播放，带来听感跳变。
   - *为什么没做 / 需要什么*：属于播放策略权衡，需在真机环境确认「从头播」还是「保持当前 offset 恢复」更符合用户预期。
3. **实体启发式误判治理（B5）**：
   - *现状与影响*：`entity_looks_label` 对含单字「社/団/团」的部分同人社团或小众厂牌偶有拆分过细。
   - *为什么没做 / 需要什么*：属于已知工程权衡（宁可错拆不盲目错并），需扩充词表或结合 LLM 深度校验。
4. **`/api/enrich` 偶发耗时 > 20s 优化（B6）**：
   - *现状与影响*：冷启动多源聚合抓取最坏延迟较高。
   - *为什么没做 / 需要什么*：需进一步对边缘小众站点的抓取实施分级超时与快速熔断。
5. **首帧微弱白闪测光取证（B7）**：
   - *现状与影响*：DWM 属性已设黑底，肉眼基本不可见，但缺乏逐帧高速录屏测光判据。
   - *为什么没做 / 需要什么*：需专用高帧率工具完成物理光度比对。

### C. 做了但需真机/外设长效复测
1. **N300 速率守卫与真实出声复测（C1）**：
   - *现状与影响*：1.3.16 已加入自检熔断算法并通过 mock 单测，但 96k 独占真机上因设备掉线需等待用户重插后复测。
   - *为什么没做 / 需要什么*：需要配合物理 N300 音箱重新插拔 USB 并进行实机开播验证。
2. **`gpuUsed` / `convGpu` 播放时真实出声端到端验证（C2）**：
   - *现状与影响*：代码级与 CDP 单测已过，但根据铁律 1，开发过程严禁向真实设备输出未经验证的可听声音。
   - *为什么没做 / 需要什么*：需用户在场并明确授权听音设备后进行试听验证。
3. **窄窗右侧 5px 热区与 WebView2 滚动条重叠手感（C4）**：
   - *现状与影响*：当前在无头 Edge 下滚动条为 overlay（宽 0），真机窄窗下拖拽热区（5px）与滚动条（9px）的层叠手感待用户反馈。
4. **`rateAuto` 分档策略抽纯函数与边界单测（C6）**：
   - *现状与影响*：核心逻辑工作正常，但缺乏独立离线单测。
   - *为什么没做 / 需要什么*：需将 `decide_capability_failure` 抽离为纯计算函数并补全测试用例。

### D. 技术债与体验优化
1. **LLM 流式 UI 逐字渲染（D2 剩余）**：
   - *现状与影响*：1.3.16 已完成后端 SSE 流式回调与网络层分块传输，但前端界面仍等待整段生成后一次性刷出。
   - *为什么没做 / 需要什么*：需在前端界面抽屉和资料卡中接入 SSE 监听并实现打字机平滑渲染效果。
2. **rtaweb 关停阶段 0xC0000409 崩溃修复（1.3.16 新发现）**：
   - *现状与影响*：通过 `/api/quit` 退出时，子进程在释放最后资源时偶发 BEX64 崩溃（不影响主控退出，但日志可见）。
   - *为什么没做 / 需要什么*：需排查全局对象、单例线程池或静态析构顺序。
3. **打包排除种子 profile（D4）**：
   - *现状与影响*：`WebView2Runtime\EBWebView` 多占约 24.9 MB 分发包体积。
   - *为什么没做 / 需要什么*：需在 `tools/make_dist.ps1` 排除列表中增加针对该无用缓存目录的过滤规则。
4. **Edge 回退路径旧代码规范化（D5）**：
   - *现状与影响*：`darken_edge_window` 存在一处旧语法，近乎零影响，需在后续例行清理中修整。
