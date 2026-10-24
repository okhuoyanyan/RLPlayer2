RLPlayer @VERSION@ 正式发布版（Full / 自包含）
================================================

这是唯一的正式发布版：不再有“最小发布版”。解压后双击 RLPlayer.exe 即可。
声音链路所需的运行库、解码器、GPU 核都在本目录，不需要另装任何东西
（Windows 自带的系统 DLL 与显卡驱动除外）。

一、本目录有什么
----------------
  RLPlayer.exe          启动器（托盘 + 内置 WebView2 无边框窗口）
  rtaweb.exe            播放服务 + 本地界面（默认 127.0.0.1:8899）
  rtagpu.dll            AMD GPU 核（25 种 AMD 架构的多架构 fat binary）
  rtagpu_cuda.dll       NVIDIA CUDA 核（12.x；没有 N 卡时不会被加载）
  amdhip64_7.dll        AMD HIP 运行库（GPU 加速所需，已随包内置）
  WebView2Loader.dll    内置界面加载器
  WebView2Runtime\      内置 WebView2 固定运行时（不依赖系统是否装过 WebView2）
  ffmpeg.exe            解码器（FLAC / MP3 / APE / OGG / WMA / ALAC / AAC ...）
  ffprobe.exe           媒体信息探测
  asio_probe.exe        ASIO 驱动预检（避免第三方驱动把播放器带崩）
  README.txt            本文件
  OPRA-NOTICE.txt       耳机 EQ 预设（OPRA）来源与许可说明
  data\                 OPRA 索引等随包数据
  tools\                命令行工具（见文末）

二、界面与窗口
--------------
  * 内置 WebView2 窗口是**完全无边框**的：没有系统标题栏、四边没有白框，
    窗口边缘 5px 内可以直接拖拽拉伸（四个角也可以）。
  * 顶栏右侧：主题（深/浅/浅蓝）、帮助、刷新、最小化/最大化/关闭。
  * 托盘图标右键：打开界面 / 复制地址 / 允许局域网访问 / 退出。
  * 若 WebView2 运行时无法启动，会依次退回：系统 WebView2 → Edge 应用窗口 → 默认浏览器。
    （退回 Edge 时窗口会带上 Edge 自己的边框，这是降级路径。）

三、GPU 加速
------------
  * AMD：rtagpu.dll 覆盖 gfx803/900/906/908/90a/90c/942、gfx1010~1012、
    gfx1030~1036、gfx1100~1103、gfx1150/1151、gfx1200/1201。
  * NVIDIA：rtagpu_cuda.dll（CUDA 12.x，支持 compute_50…compute_121）。
  * 运行时会用平凡 kernel 验算“能不能真的执行”；显卡不在列表、驱动太旧
    或没有对应厂商的卡时，自动退回 CPU，结果一致，不会崩。
  * 仍需要安装显卡驱动（硬件驱动，不能随播放器分发）。
  * 信号链里的绿色节点 = **本次真的由 GPU 计算**；底栏「GPU」徽标也只在真正参与时亮。
    设置里勾了 GPU 但显卡不可用时，会如实显示为未参与。

四、解码与格式
--------------
  * WAV / DSF / DFF 原生解码，逐位精确。
  * 其它格式走本目录的 ffmpeg.exe（优先同目录，其次 PATH）。
  * DSD 源走原生 DSF/DFF 读取；同速率 + 无处理 + 0 dB 时可开 DirectSDM 直通。
  * DSD 调制输出会自动保留调制器余量（满幅曲目约 -4 dB），这是为了防止
    1-bit 环路在第一个大动态处失稳；信号链里会显示实际预衰减值。
  * bit-perfect：设置 → 输出 →「切到 bit-perfect 直通」一键把参数切成
    源采样率 / 无抖动 / 0 dB / 无 EQ·IR / 独占；信号链会逐项标出是否满足。

五、系统要求
------------
  * Windows 7 SP1 / 8 / 8.1 / 10 / 11，x64。
  * CPU：SSE4.2 + POPCNT（2009 年后的 x86-64；官方下限 Zen 1 / Intel 6 代）。
    产物不含 AVX-512，不需要 VC++ 运行库（CRT 静态链接）。
  * 界面：本包已内置 WebView2 固定运行时（\WebView2Runtime），Win10/11 双击即用，
    不再依赖系统是否装过 WebView2。Win7 请使用 Chrome/Edge 109/新版 Firefox
    打开界面（内置运行时为当前 Windows 版本，不保证 Win7）。
  * 内存：DSD256 等大文件按整曲解码，建议 8 GB 以上。

六、独占缓冲与欠载（1.3.14 起）
-------------------------------
  * 设置里的「缓冲（毫秒）」对独占模式**真的生效**（20~500 ms）；以前被强制封顶 20 ms。
  * 驱动把缓冲放大（如 MOTU 对齐成 8192 帧）或 Start 后流不推进时，会自动回退 20 ms
    并把这个端点记进 rlplayer-buffer-learned.json，下次直接用学到的值。
  * 想强制回到旧行为：设置环境变量 RTA_BUF_RESPECT=0 再启动。
  * 设备从 USB 总线掉线时，日志会写 DEVICE_LOST，并停止播放（不会静默降级/从头重播）。

七、日志（出问题时请发这两个文件）
----------------------------------
  * launcher.log             启动器日志（启动、端口、窗口、DPI、resize）
  * rlplayer-playback.log    播放日志（每次播放的格式/后端/缓冲/预衰减/峰值/欠载/GPU）
  另有 http://127.0.0.1:8899/api/dbg（播放线程环形日志）与 /api/status（实时状态）。
  反馈问题时请附上 rlplayer-playback.log 的 START / DECODE / PLAY / PLAYBUF / STAT / END 段落。

  【内测人员专属】
  * 在 RLPlayer.exe 同目录新建 rlplayer-tester.txt，写入你的名字（一行）——
    日志横幅与诊断信息会自动带上，便于归因。
  * 设置页 →「诊断」→「导出诊断信息」可一键导出 rlplayer-diag.json
    （版本/系统/设备能力/最近日志），反馈问题请附上它。
  * 程序崩溃时会在 dumps\ 目录生成 .dmp 转储文件，反馈崩溃问题请一并压缩附上。

八、元数据自动补全与 AI 资料
----------------------------
  * 播放时自动在线补全标签（专辑/年份/流派/专辑艺术家）、封面与歌词；
    数据来自 QQ音乐 / 网易云 / iTunes / lrclib / Discogs，DeepSeek 负责疑难文件名解析、
    歧义裁决与「资料整理」。
  * 设置页 →「元数据」可整体关闭或单独关闭任一数据源；也可关闭「AI 整理」。
  * 打开 AI 整理后：进入单曲页/专辑页会**自动整理**（不用再点）；整张专辑会在后台
    并发≤2 逐首预取，页面顶部有一条细进度条。
  * 资料里的引用会标明来源；找不到权威材料时会明确标注「AI 整理（未找到权威来源）」，
    不冒充有来源。
  * 单曲页的「艺术家」与艺术家页共用同一份缓存，第二次进入秒开。
  * DeepSeek 为可选：在同目录 rlplayer-llm.conf 写入
    base_url=https://api.deepseek.com / api_key=你的key / model=deepseek-chat
    即可启用；不配置也完全可用（规则匹配 + 本地资料）。
    api_key 只存本地，不会上传、不会打进升级包。
  * 已知边界：同人/小众曲目在数据源命中率低属正常；补全结果可用
    tools\rtatag.exe 查看与手改（store show / store set）。

九、已知限制（如实）
--------------------
  * NAA 已彻底删除：数播的 NAA 只认 HQPlayer 私有握手（EULA 禁止逆向），本机实测 8 种握手变体全部无回应；
  * WASAPI 输入采集通路未实现（界面已标注）；UPnP 投放已可用。
  * Native DSD（ASIO DSD 通道类型）未实现；DSD 输出走 DoP。
  * 7 阶 1-bit 调制器在 DSD64 上不稳定，程序会自动降为 5 阶并说明原因。
  * DirectSDM 只在“同速率 + 无 PCM 处理 + 0 dB”时启用，否则回退普通路径。
  * AI 资料首次生成主要耗时在 LLM（中位约 12 秒），缓存命中 6~7 毫秒。

十、命令行工具（tools\）
------------------------
  rtaplay.exe        离线转换 / 分析 / 播放（--analyze 不打开设备）
  dsdconv.exe        DSD 查看 / 抽取 / DirectSDM（--info / --direct）
  make_dsd.exe       生成 DSD 测试素材
  rtanet_rx.exe      另一端接收 RTANet 网络音频
  rtacapture.exe     WASAPI 采集 / 录制
  rtacast.exe        UPnP 投放端（命令行）
  dopinfo.exe        检查 DoP 标记是否合法
  wavcmp.exe         逐位比较两个 WAV
  filter_report.exe  滤波器频响/阻带报告
  gpubench_conv.exe  GPU 卷积基准
  wasapi_probe.exe   列出设备独占模式支持的采样率/位深
  wasapi_ex_probe.exe 独占格式阶梯探测（24 紧凑 / float32 / 32int）
  upnp_scan.exe      UPnP/DLNA 设备发现
  aidecode.exe       走统一解码路径并写出 WAV（便于逐位比对）
  rtatag.exe         元数据工具：parse（文件名解析）/ enrich（手动补全）/
                     store show|set（查看/手改补全结果）/ llm-smoke（DeepSeek 连通自检）

十一、第三方组件
----------------
  * ffmpeg / ffprobe：GPLv3，源码与许可见 https://ffmpeg.org/
  * WebView2Loader.dll：Microsoft WebView2 SDK
  * amdhip64_7.dll：AMD HIP runtime，随 AMD ROCm 分发
  * OPRA 耳机 EQ 预设：见 OPRA-NOTICE.txt