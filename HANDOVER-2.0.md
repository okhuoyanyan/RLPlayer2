# RLPlayer 2.0 交接文档

> 写给下一个接手的会话/协作者。读完这篇就知道：现在在哪、哪些是活的、哪些是坑。
>
> **生成时间**：2026-10-28　|　**版本**：2.0.0　|　**状态**：已正式发布
>
> 旧的 `HANDOVER.md`（25 KB，停在 1.3.x 时代）仅作历史参考，**状态已过时**。

---

## 一、当前状态速查

| 项目 | 值 |
|---|---|
| 版本号 | `2.0.0`（单一来源 `src/version.h`） |
| 公开仓 | https://github.com/okhuoyanyan/RLPlayer2 （public，默认 `main`） |
| Release | https://github.com/okhuoyanyan/RLPlayer2/releases/tag/v2.0.0 |
| Tag | `v2.0.0` → `9cc3671` |
| 发布包 | `RLPlayer-2.0-win64.zip`，382.8 MB，sha256 `3ac59c1b…164e` |
| 私有开发仓 | `okhuoyanyan/RLPlayer`（含 `main` / `片头`，**不要推**） |
| 反馈仓 | `okhuoyanyan/RLPlayer-feedback`（public） |
| 当前工作分支 | `release/2.0.0` |

**包里有什么**：958 个条目（892 个实际文件），内置 WebView2Runtime、ffmpeg/ffprobe、
`rtagpu.dll` / `rtagpu_cuda.dll` / `amdhip64_7.dll`、`asio_probe.exe`、`tools\`。

**发布包已验证干净**：无 `rlplayer-*.conf`、无 API Key、无开发机路径、无用户缓存。

---

## 二、四条必须先知道的硬规矩

### 1. `dist\RLPlayer-2.0-win64\` 不是发布物

那是**本地解压测试目录**，里面有：

```
rlplayer-library.conf    →  D:\RTA-hqplayer\MUSIC   （开发机路径！）
rlplayer-llm.conf        →  api_key=sk-26703...      （真实密钥！）
rlplayer-meta\           →  用户元数据缓存
rlplayer-playlist.txt    →  测试队列
```

**永远不要把这个目录打包上传**。发布包从 `build-dist-final\` 出，或用 `tools\make_dist.ps1`。
判断包是否干净要**直接读 zip 条目**，不要看解压目录。

### 2. 分支别搞混

```
main          → 1.5.10 时代，落后 2.0 的 57 个提交，但另有 68 个提交 2.0 没有
片头          → 宣传视频项目分支，不是代码
release/2.0.0 → 当前 2.0 代码
```

`main` 与 `片头` **已分叉**（merge-base `3496d46`）。合并前先看 `git log --oneline HEAD..origin/main`。

### 3. 含中文的 `.ps1` 必须带 BOM

PowerShell 5.1 对无 BOM 的 UTF-8 按 ANSI 解析：中文注释变乱码报 `Unexpected token ')'`，
中文文件名变乱码让 `Test-Path` 直接 false。

编辑工具写回会丢 BOM，改完补：

```python
io.open(p, 'w', encoding='utf-8-sig', newline='\n').write(t)
```

2026-10-28 一天踩了两次（`build_gpu_cuda.ps1` / `_assemble.ps1`）。

### 4. UI 改动必须实机截图

headless / CDP 证明的是「DOM 算出来的样子」，不是真实 WebView2 窗口。
已三次栽在这上面（四边白框、封面 99%、AI 卡片）。
用 `tools\pxgrab.ps1` 或 `PrintWindow` 抓 `RLPlayerMain`。

---

## 三、两条在线路径（已实测）

### 自动更新

```
清单  https://raw.githubusercontent.com/okhuoyanyan/RLPlayer2/main/latest-win.json
      → HTTP 200，version=2.0.0，892 个文件的逐文件 sha256
下载  https://github.com/okhuoyanyan/RLPlayer2/releases/download/v2.0.0/RLPlayer-2.0-win64.zip
白名单 只放行 github.com（精确匹配，不含端口）
```

代码：`src/net/update_check.h:92-98`。

**发新版必做**：
1. 改 `src/version.h`
2. 重新打包
3. 生成 `latest-win.json`（`version/notes/download_url/sha256/size/files[]`）
4. **清单提交到 `main` 分支根目录**（不是只当 release 资产）
5. 建 release，上传 zip + 清单

### 一键反馈

```
Worker  https://rlplayer-feedback.okhuoyanyan.workers.dev
反馈仓  okhuoyanyan/RLPlayer-feedback
源码    worker\feedback-worker.js（**未纳入 git**，建议尽快提交）
```

走 Cloudflare Worker，客户端不需要 token。代码：`src/net/feedback.h:113`。
conf 里写 `worker=none` 可退回旧 GitHub API 路径。

---

## 四、未完成事项（按优先级）

### P0 — `反馈和开发\debug\20261028-01` 未处理

分析档已写（7,294 B），**代码未动**：

| # | 问题 | 现状 |
|---|---|---|
| 1 | 音频分析总开关取消勾选后保存、刷新又自动勾上 | 根因已定位：`ui_script.h:3284` 的 `.then` 里裸 `D('sLoud')` 抛 TypeError 中断整个回调。**修了 loadSettings 但这条路径需实机复测** |
| 2 | 封面「矫枉过正」，所有曲目都没封面了 | `lib_album_dir_matches()` 过严。用户要求：确认属于该专辑就继承封面；不确定则从网易云/QQ 音乐**单曲**级抓 |
| 3 | 艺术家头像抓取仍然很慢 | 需改并发或惰性加载 |
| 4 | 包内残留开发机音乐库路径 | **发布包已确认干净**，本地 `dist` 目录仍脏 |

### P1 — 三个反馈文件未改名

按 AGENTS 8.2，部分完成不改名：

| 文件 | 状态 |
|---|---|
| `261026-03.txt` | 外部评审交接件，六包已合并，**不需改名**（文件自述不适用） |
| `261027-05.txt` | 27 项需求，主体已做，见 `.分析.md` |
| `261027-06.txt` | AI 大修 6 条，复核后确认**早已实现** |

### P2 — 技术债

- `worker/feedback-worker.js` 未纳入 git（16 KB，线上服务源码，丢了要重写）
- `res/icon-rl/`、`res/icon-lab/`、`build-*.bat` 共 40+ 开发产物未跟踪也未忽略，建议写 `.gitignore`
- `website-v1-backup/` 确认不要后可删
- `tools/filter_probe.cpp` 未跟踪（生成频响图用，应纳入）

---

## 五、2.0 到底改了什么

| 领域 | 改动 | 证据 |
|---|---|---|
| **检索质量** | 单曲查询改「专辑名 + 曲名」，真实 API 召回 rerank **0.26 → 1.30** | `retrieval.h` 1965-1968 |
| **AI 归属校验** | 规则 9：专辑名/曲目列表/时长**至少两项**对上才采信外部页面 | `prompt_profiles.h` ~688 |
| **AI 字段纯净** | 规则 10：字段值禁止裸 URL，改由 `linkify_text()` 挂权威链接 | `prompt_profiles.h` ~710 |
| **缓存版本** | `cache_ver` v4 → **v6** | `retrieval.h` 1085 / 2499 / 2653 |
| **封面归属** | 改「文件夹名才是权威」，`lib_album_dir_matches()` 12/12 用例 | `library.h` 1411-1561 |
| **音频分析开关** | `loadSettings()` 补调用点（原先**根本没有调用点**）+ 修裸 `D()` 中断 | `ui_script.h` 3291-3292 |
| **专辑名兜底** | `/api/meta` 对无标签文件用目录名兜底 | `rtaweb.cpp` ~11976 |
| **CUDA 构建** | 支持无 N 卡编译（`nvidia-smi` 探测）+ 保留 UTF-8 BOM | `build_gpu_cuda.ps1` ~146 |
| **文档** | 整本 28 页使用说明书 + 独立技术白皮书 | `build-doc\MANUAL.md` |

---

## 六、构建与验证

```powershell
cmake --build build

ctest --test-dir build -E 'wasapi_shared|capture|asio_native_dsd'
# 上次：92/92 全绿，332 秒

powershell -File tools\make_dist.ps1
```

**工具链**：clang 21（`C:\Program Files\AMD\ROCm\7.2\bin\clang++.exe`）+ Ninja。

**CUDA**（需要时）：

```powershell
powershell -File tools\build_gpu_cuda.ps1 -CudaRoot "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9"
# 验证：cuobjdump -lelf（19 档 cubin）+ dumpbin /exports（7 个符号）
```

⚠️ **CUDA 必须留 12.x**。13 砍掉 Maxwell/Pascal/Volta，19 档会掉到 5 档。

---

## 七、发版检查清单

```
[ ] src/version.h 改版本号
[ ] cmake --build build 无 error
[ ] ctest 全绿
[ ] 从干净目录打包（直读 zip 检查无 rlplayer-*.conf / 无密钥 / 无开发机路径）
[ ] 包内 FLAC + DSD 解码 smoke
[ ] 生成 latest-win.json（逐文件 sha256）
[ ] 清单提交到 main 分支根目录
[ ] 建 release，上传 zip + 清单
[ ] curl 匿名验证：仓库 200 / release 200 / 清单 200 / zip 302
[ ] 官网 download.html 链接指向新 release
```

---

## 八、下一阶段方向（未立项）

| 方向 | 结论档 | 状态 |
|---|---|---|
| **安卓版**（UAC2 直连 + OpenCL 升频 + float2 模拟 double） | `docs\安卓版-开发结论.md` | **等用户真机**，Win 发布后开工 |
| **音乐内容分析**（乐器/节拍/风格/调性） | `docs\音乐内容分析-设想.md` | 设想档 |
| **HiFi 下一步**（行车记录仪/实时测量/OPRA/设备指纹） | `docs\HiFi回放-下一步脑洞.md` | 脑洞档 |

安卓三条硬约束：`-ffast-math` 的 reassoc 必须先修；逐位等价只能对准 IEEE-754 double；
过渡带 ≥ 21 kHz 是硬底线。**Android 公开 USB Host API 不支持等时传输**，UAC2 必须走 NDK + libusb。

---

## 九、文档索引

| 文件 | 内容 |
|---|---|
| `AGENTS.md` | 12 条铁律 + 反馈流程 + 子代理派遣。**接手先读** |
| `docs\项目相关仓库与在线服务.md` | 三仓库边界（Windows / macOS / 反馈） |
| `docs\项目信息-权威版.md` | 产品信息、团队、联系方式 |
| `docs\反馈Worker-部署指引.md` | Cloudflare Worker 部署 |
| `build-doc\MANUAL.md` | 使用说明书源（28 页），改完跑 `make_manual_pdf.py` |
| `build-doc\CHAPTERS.md` | 技术白皮书源（60 页 87 图），`make_pdf.py` |
| `website\README.md` | 官网部署说明 |

---

## 十、已经踩过的坑（别再踩）

1. **`gh release upload` 后用 `--jq` 查资产会失败** —— PowerShell 把 `--jq` 表达式拆成多参。改用 `gh api ... --jq`。
2. **`--kiosk` 在已有 Edge 实例时不开新窗口**，会复用现有标签页。必须用独立 `--user-data-dir` 并先杀自己的旧进程。
3. **`git commit -F` 带 UTF-8 文件会写入 BOM**，提交信息开头出现 `﻿`。用 `git commit -m` 传参，或事后 `--amend`。
4. **release 默认是 draft**，去掉 `--draft` 后还要 `gh release edit --draft=false` 才真正公开。
5. **`dist\` 的解压副本会污染判断** —— 判断包干净与否必须直接读 zip 条目。
6. **`_preview\`、`start-site.ps1`、`预览网站.bat`、`zoom-*.png`** 上传服务器前必须清掉（官网 zip 现 15 文件 / 2.18 MB）。
7. **`Set-Content -NoNewline` 会把多行 conf 压成一行**（曾把 `rlplayer-settings.conf` 弄坏），改配置用 Python 写。

---

## 十一、联系与归属

- 项目开发者：RainLain、猫与小行星
- 协助开发者：聆听风暴
- 媒体与美术：Gosed
- 测试鸣谢：Miyu、正在下载99%
- 官方主页：rlplayer.rainlain.com（**未上线**）
- 粉丝与反馈 Q 群：895816505

> ⚠️ **不要把任何邮箱写进产品文档/网站**。之前有一轮凭空生成过一个不存在的邮箱，已全站清除。
> 联系方式只用 Q 群。

---

*本文档描述 2026-10-28 的真实状态。改动后请同步更新对应小节。*
