# tools/ —— 命令行工具与验收脚本

本目录有两类东西：

1. **随发布包分发的 CLI**（`make_dist.ps1` 会把这 17 个 .exe 打进 `tools\`）——用户可直接用；
2. **开发/验收脚本**（`.ps1` / `.js` / `.py`）——只在源码仓库里，用于构建、门禁与界面验收。

> 所有会出声的工具默认电平 ≤ −60 dBFS；链路验证优先 `--analyze`（不打开设备）。
> 见 [AGENTS.md](../AGENTS.md) 铁律 1。

---

## 一、随包 CLI

### 播放 / 转换

| 工具 | 作用 | 常用 |
|---|---|---|
| `rtaplay.exe` | 离线转换 / 分析 / 播放（管线与 GUI 完全相同） | `rtaplay music.flac --analyze`<br>`rtaplay music.flac --rate 705600 --taps 262144 --shaper NS9`<br>`rtaplay music.flac --ir room.wav --eq autoeq.txt --gpu` |
| `aidecode.exe` | 走统一解码路径并写出 WAV（便于逐位比对） | `aidecode in.flac out.wav` |
| `wavcmp.exe` | 逐位比较两个 WAV | `wavcmp a.wav b.wav` |
| `dsdconv.exe` | DSD 查看 / 抽取 / DirectSDM | `dsdconv --info x.dsf`<br>`dsdconv --direct in.dsf out.wav` |
| `make_dsd.exe` | 生成 DSD 测试素材 | `make_dsd tone.dsf 5 2822400` |
| `dopinfo.exe` | 检查 DoP 标记是否合法 | `dopinfo out.wav` |
| `filter_report.exe` | 滤波器频响/阻带报告 | `filter_report --taps 262144` |

### 设备与协议探针

| 工具 | 作用 |
|---|---|
| `wasapi_probe.exe` | 列出音频端点 + 独占模式支持的采样率/位深矩阵（只读，不开流） |
| `wasapi_ex_probe.exe` | 独占格式阶梯探测：24 紧凑 / float32 / 32 容器 / 32 int |
| `asio_probe.exe` | ASIO 驱动预检（子进程里试开 0.7 s，崩溃结果写缓存） |
| `upnp_scan.exe` | UPnP/DLNA 渲染器发现 |
| `naa_scan.exe` | NAA 端点发现（只发现，握手未实现） |
| `naa_probe.exe` | NAA 控制通道只读探测 |
| `rtanet_rx.exe` | 另一端接收 RTANet 网络音频 |
| `rtacast.exe` | UPnP 投放（命令行） |
| `rtacapture.exe` | WASAPI 采集 / 录制 |
| `gpubench_conv.exe` | GPU 卷积基准（会先验算平凡 kernel） |

### 元数据

| 工具 | 作用 |
|---|---|
| `rtatag.exe` | `parse` 文件名解析 / `enrich` 手动补全 / `store show|set` 查看或手改补全结果 / `llm-smoke` DeepSeek 连通自检 |

---

## 二、构建 / 打包脚本（仓库内）

| 脚本 | 作用 |
|---|---|
| `make_dist.ps1` | 出正式发布包：构建 → GPU 核 → 兼容门禁 → PATH=System32 的 GPU smoke → 打包 → sha256 |
| `build_gpu.ps1` / `build_gpu_cuda.ps1` | 编 AMD HIP / NVIDIA CUDA 核（CUDA 脚本会校验 cubin 与导出符号） |
| `build_ffmpeg_lite.ps1` / `verify_ffmpeg_lite.ps1` | 自建精简 ffmpeg / 出包前守卫（缺解码器就拒绝出包） |
| `check_compat.ps1` | 分发兼容门禁（静态 CRT / subsystem ≤ 6.1 / 无 AVX-512） |
| `check_platform_boundary.ps1` | 平台边界检查（src/ 不许出现 win32 头，platform/ 才允许） |
| `restart-launcher.ps1` / `restart-servers.ps1` | 开发时重启本机实例（只停自己的 pid） |

---

## 三、界面与验收脚本（仓库内）

| 脚本 | 作用 | 用法 |
|---|---|---|
| `uicheck.js` | 用 headless Edge + CDP 真跑一遍界面（点专辑、看曲目、收集 JS 异常） | `node tools/uicheck.js http://127.0.0.1:8911/ build/uicheck_out` |
| `t25check.js` | 单曲页/歌词/外链/UPnP 投放/时长等成套验收（含 893×587 真机档） | `node tools/t25check.js http://127.0.0.1:8931/ D:/shots` |
| `npcheck.js` | 单曲页专项验收 | `node tools/npcheck.js <url> <out>` |
| `lyr_dpr.js` | 歌词跟随两档视口验收（893×587 / 1340×880，DPR 1.5） | `node tools/lyr_dpr.js <url> <out> 893 587 1.5` |
| `peqcheck.js` | PEQ 对齐 + 拖拽/键盘/自动前级验收 | `node tools/peqcheck.js <url> <out>` |
| `bordercheck.ps1` | **无边框**逐状态取证：结构判据（WebView2 子窗口必须与客户区重合）+ 像素判据 + 四角 + 最大化 + 缩放热区 | `powershell -File tools/bordercheck.ps1 -Exe <RLPlayer.exe> -Theme dark -Out <dir>` |
| `uishots.js` / `uiprobe.js` | 批量截图 / 页面探针 | — |
| `mock_t25.js` / `mock_upnp_renderer.js` | 页面契约 mock / 假 UPnP 渲染器（不出声、不碰真设备） | — |
| `pxgrab.ps1` / `pxmeasure.py` / `pxprobe.cpp` | 逐像素取证（窗口边缘、DPI、缩放热区） | — |

**这些脚本的共同前提**：测试实例必须无声（`backend=null`、`volume=-60`），
脚本会在起播前断言；不通过就拒绝继续。

---

## 四、几条硬规矩

- 不要用 `taskkill /IM` 或 `Stop-Process -Name` 停进程 —— 只停自己启动的 pid（铁律 2）。
- 测试音默认 −60 dBFS；需要可闻必须显式确认（铁律 1）。
- 验收脚本要能区分「真问题」与「夹具/环境问题」：判据先 FAIL 再 PASS（铁律 3）。
