# GPU CUDA 后端纪要（NVIDIA）

> 目标：给 HQPlayer 同类的 NVIDIA 路径补上 CUDA 后端（原仓库只有 HIP/AMD，
> README「已知限制」写着"NVIDIA/CUDA 后端：只有加载路径，仓库内无 CUDA 源码"）。
> 本轮把那条限制消掉：**CUDA 后端可用，且与 HIP 后端共用同一套加载层与判据**。

## 1. 架构决策：不改加载层，只加一份 DLL

| 机制 | 位置 | 事实 |
|---|---|---|
| DLL 候选顺序 | `src/dsp/gpu_pref.h` | auto = `rtagpu.dll` → `rtagpu_dx12.dll` → `rtagpu_cuda.dll`（本来就有 CUDA 这一格） |
| 动态加载 + 逐个尝试 | `src/dsp/gpu_bridge.h` | 某个 DLL 在但 kernel 不可执行时继续试下一个，不退回 CPU 就认输 |
| 状态上报 | `tools/rtaweb.cpp` `/api/gpu` | 已有 `cuda` 字段（原先注释「本版本不提供 CUDA 后端，仅用于如实报告」） |

所以 CUDA 适配的全部工作量 = **产出一个导出同样 4 个符号的 `rtagpu_cuda.dll`**：

```
rta_gpu_available / rta_gpu_convolve / rta_gpu_resample / rta_gpu_resample_invalidate
```

管线、状态机、界面、测试**一行未改**（`tools/gpu_resample_test.cpp` 只把硬编码的
`LoadLibraryA("rtagpu.dll")` 改成复用 `gpu_pref.h` 的候选列表，让测试与管线同源）。

## 2. 实现：与 HIP 版逐行对应，不抽公共层

`src/gpu/gpu_conv_cuda.cu` / `src/gpu/gpu_resample_cuda.cu` 是把两个 `_hip` 文件的
运行时调用换成 CUDA（`hip*` → `cuda*`、`hipLaunchKernelGGL` → `<<<>>>`），
算法一字未动：

```
CPU  每块的 FFT / IFFT（块多但每块小，传输可批量摊薄）
GPU  P 个 IR 分区的频域乘累加 —— 长 IR 卷积的主要开销
kernel: Y[b][k] = Σ_{p=0..min(P-1,b)} H[p][k] · X[b-p][k]
```

**为什么不抽一份公共头文件**：两份独立实现互为参照物。铁律 3 的要求是"判据要能真的
证明东西"——两份独立代码给出同一结果，比一份自证可信得多；出错时也能靠对拍定位。

## 3. 铁律 5 在 CUDA 侧换了个形态（这条容易踩）

HIP 那边的教训是「arch 写错 → kernel 静默不执行，而 `hipDeviceSynchronize()` 返回成功」。
CUDA 不是同一个形态，但**一样会骗人**：

1. fatbin 里带 PTX 时，驱动会 JIT —— **arch 不对也"看起来能跑"**（只是首次慢）。
   所以"没报错"不能当通过。
2. 上下文若已进入 sticky error（更早的调用留下的），后续 `cudaGetLastError()` 会一直
   返回那个旧错 —— 探测前必须 `(void)cudaGetLastError()` 清一次。
3. 「kernel 启动了」≠「kernel 写了东西」。`rta_gpu_available()` 仍然写常数并读回，
   与 HIP 版同一判据。

构建侧因此做**两道校验**（`tools/build_gpu_cuda.ps1`）：
- 编译期：`cuobjdump -lelf` 列出的 cubin 必须覆盖每一个请求的 arch；
- 链接后：`dumpbin /exports` 必须看到 4 个导出符号（改名字不会报错，只会静默退回 CPU，
  `test_gpu_bridge` 的注释记着上一代就是这样漏掉的）。

## 4. 工具链：只装编译器组件，不装 3.7 GB 全量包

CUDA 13 已移除 Maxwell/Pascal/Volta（`sm_50`–`sm_61`），要覆盖 GTX 9/10 系必须用 **CUDA 12**。
本轮使用 **12.9.41**，从 NVIDIA 的 `redistrib_12.9.0.json` 按组件取（合计约 140 MB）：

| 组件 | 版本 | 大小 | 作用 |
|---|---|---|---|
| `cuda_nvcc` | 12.9.41 | 126.9 MB | nvcc / cudafe++ / ptxas / nvlink / cicc + libdevice |
| `cuda_cudart` | 12.9.37 | 3.5 MB | `cuda_runtime.h` + `cudart_static.lib` |
| `cuda_cuobjdump` | 12.9.26 | 6.2 MB | 校验 cubin 真的进二进制 |
| `cuda_cccl` | 12.9.27 | 3.1 MB | libcu++ / CUB 头文件 |

`tools/fetch_cuda_redist.js` 负责从 redistrib JSON 读出精确版本与 sha256；
下载后逐个校验 sha256 再解压拼装成标准 `CUDA_ROOT` 结构。

**arch 覆盖（实测 `nvcc --list-gpu-arch`）**：请求 20 档，支持 **19 档**

```
compute_50,52,53 (Maxwell)  60,61,62 (Pascal)  70,72 (Volta)  75 (Turing)
80,86,87 (Ampere)  89 (Ada)  90 (Hopper)  100,101,103  120,121 (Blackwell)
```

唯一缺的是 **compute_110**（nvcc 12.9 未提供），其余全部进 fatbin。
静态链接 `cudart`（`-cudart static`）+ 静态 CRT（`-Xcompiler=/MT`）⟹
**用户机不需要装 CUDA 运行时**，只需要显卡驱动（与铁律 6「发布包自包含」一致）。

## 5. 实测（2026-10-22，本机 RTX 4080 / 驱动 617.42 / sm_89）

| 级 | 判据 | 实测 | 结论 |
|---|---|---|---|
| L0 | 平凡 kernel 真执行并读回常数 | `rta_gpu_available()` 返回 1；launch/sync/copy 三错码全 0 | ✅ |
| L1 | `test_gpu_bridge`：与 CPU 直接卷积一致（相对 RMS < 1e-4） | **6.019e-08**（最大绝对误差 1.311e-06，样本 8192 / IR 4096） | ✅ |
| L2 | `gpu_resample_test`：与 CPU 双精度参考一致（maxAbs/rms < 1e-12） | **2.210e-14 ~ 6.843e-15**（maxAbs 3.3e-17 ~ 1.2e-17）；GPU 稳定 **1.20 ms**，CPU 单线程参考 22–87 ms ⟹ 加速比 **18–72×**（区间来自 CPU 参考的缓存冷热，不是 GPU 抖动） | ✅ |
| L3 | 管线内生效（`/api/gpu`） | `{"cuda":true,"available":true,"status":"GPU 卷积可用（rtagpu_cuda.dll）"}` | ✅ |
| L4 | 生产路径性能（IR 262144 / 48000 帧 / 分块 1024） | CPU 分块 FFT **29.82 ms**；GPU 端到端 **11.05 ms** = **2.7×**；相对偏差 **−138.0 dB** | ✅ |

L0/L1 走的是**真实动态加载路径**（`GpuBridge::instance()` → `LoadLibraryA` → `GetProcAddress`），
不是手工调 kernel 的独立 demo。L4 同样走 `GpuBridge::convolve`（生产路径）。

### ⚠️ 5.1 与 HIP 记录的 2.0 ms 不是同一个口径

`docs/GPU卷积纪要.md` 记的「GPU 端到端 2.0 ms」是从 kernel 启动开始计时的
（kernel + IFFT + 重叠相加），**不含** CPU 端为 256 个 IR 分区做 FFT 的准备时间。
本轮 L4 的 11.05 ms 是 `GpuBridge::convolve` **全程**：IR 频谱重建 + 输入频谱 + 上传 +
kernel + 下载 + IFFT + 叠加。

按 29.82 ms → 11.05 ms（2.7×）是同口径比较；拿 11.05 ms 去比 2.0 ms 会得出"CUDA 比 HIP
慢 5 倍"的错误结论。**这两个数不能直接比。**

顺带这轮量出一条真实的优化点：`GpuBridge::convolve` 每次调用都重算 IR 频谱
（P=256 个 2048 点 FFT），而 IR 在一首歌里不变 —— 缓存 IR 频谱是下一个最划算的改动
（预期把 11 ms 拉到个位数毫秒级）。

## 6. 已知问题（如实记录，未解决）

1. **MSVC 不能构建主程序**：`src/audio/dsd_reader.h:63` 用 `__builtin_popcount`；
   `src/net/ui_script.h` 有 7 处超过 MSVC 单字符串 16380 字节上限（C2026）。
   本机走 LLVM clang（与项目原生一致）；MSVC 只用于 nvcc 的 host 编译。
2. **`third_party/webview2/x64/WebView2Loader.dll` 未入库**（仓库只有 `include/`），
   clone 后 `RLPlayer.exe` 的 POST_BUILD 拷贝会失败。本轮从 NuGet
   `Microsoft.Web.WebView2 1.0.4258.31` 的 `build/native/x64/` 取了一份放在本地（**未提交**）。
3. **本机 ctest 有 4 个失败，均与本改动无关**（待查，不归因于 CUDA）：
   - `emby` / `subsonic` / `rtanet_e2e`：ninja 报链接成功（exit 0），但产物 exe 在 build 目录
     **不存在**（连续两次；`Test-Path` 曾为 True，下一次调用即消失）。本轮未定位。
   - `pipeline`：`0xC0000409`（STATUS_STACK_BUFFER_OVERRUN），且在打印任何一行前就退出；
     该测试依赖原开发机路径 `D:\RTA-hqplayer\build\testdata\rt24.wav`，本机无此目录。
   其余 **41/45 通过**，其中 `gpu_bridge` 通过（说明 L1 在 ctest 门禁里也是绿的）。
4. **verify.ps1 尚未接入 CUDA 分支**：第 3 步目前硬编码 HIP + `hipcc` 路径，在 NVIDIA 机器上
   必定 FAIL。接法见下节，需在下一轮连同门禁一起改。

## 7. 下一轮（按性价比排序）

1. **IR 频谱缓存**（`GpuBridge` 侧按 IR 指针/长度/block 做一次性上传）——最大收益。
2. `verify.ps1` 第 3/7 步改为「按可用工具链编对应后端」：有 hipcc 编 HIP、有 nvcc 编 CUDA，
   两者都没有才 FAIL；`gpu_resample_test` 已能自动挑 DLL。
3. `tools/make_dist.ps1` 把 `rtagpu_cuda.dll` 打进正式发布版（铁律 6：自包含）。
4. 把 GPU 侧也 GPU 化（cuFFT 或手写 FFT kernel），省掉 CPU 端 FFT 准备。
5. 全 arch fatbin 的实机覆盖：本机只有 sm_89，其余 18 档只能靠 `cuobjdump` 证明"编进去了"，
   **无法在本机证明"能在对应卡上跑"**（铁律 4：这是如实的能力边界）。


## 8. 二进制已交回（2026-10-23，PR #3 / `feat/cuda-dll`）

`rtagpu_cuda.dll` 766,464 B，sha256 `BCE4093D…E6B`，CUDA 12.9.41、19 档 fatbin、4 导出符号齐全。
已合入 main 并放到 `cuda-artifacts/`（打包时由 `make_dist.ps1` 自动收录进正式包）。

运行期：在无 NVIDIA 卡的 AMD 机器上，`/api/gpu` 新增 `cudaUsable` 字段（LoadLibrary + `rta_gpu_available()` 实测），
避免"文件在就报告就绪"的假象；播放仍自动回退 AMD/CPU，`/api/ping` 正常、不崩。

因此第 7 节「下一轮」第 3 项（make_dist 收录）已完成；第 5 项（全 arch 实机覆盖）仍是能力边界：
只有 sm_89 真机验证过，其余 18 档只能靠 cuobjdump 证明"编进去了"。


## 9. PR #5：streaming 会话 destroy 用 `cudaFreeHost`（2026-10-27，本机无 N 卡，仅静态取证 + HIP 门禁）

PR #5（`feat/cuda-stream-fix` → `main`，作者 garaxc）两份东西：2 行源码修复 + 重建 DLL。
以 cherry-pick 落到「片头」，合并提交 **`212c58f`**（author 保留 garaxc；main 与片头差异过大，不做整支合并）。
`git diff pull-5 HEAD -- src/gpu/gpu_conv_cuda.cu src/gpu/gpu_resample_cuda.cu` 为空
⟹ **DLL 的两个编译输入与本树逐字节相同**（`5AFAD54F…` / `1A87279A…`），不是"另一个分支的二进制"。

### 9.1 缺陷与修法

`cudaHostAlloc` 分配的 pinned 主机内存必须用 `cudaFreeHost` 释放；`cudaFree` 对它是未定义行为
（HIP 的 `hipFree` 两种都能收，照抄过来就错）。后果链：首次 create 正常 → destroy 用错 API →
**后续 create 失败** → `GpuStreamConvolver` 静默降级 CPU（`on_gpu=0`），**不报错**。

受影响的不止一条路径（本次核对）：

| 路径 | 位置 | 说明 |
|---|---|---|
| 播放中换 IR / EQ | `gpu_bridge.h:167` `reset()` = `close()` + `open_gpu_or_cpu()` | 就是 destroy→create，必踩 |
| 会话建失败后的清理 | `gpu_conv_cuda.cu:299` `rta_gpu_convolve_destroy(s)` | 同一处 destroy，一并修好 |
| 逐档新建会话（测试/探针） | `test_gpu_stream` / `gpu_stream_check` | 第 2 档起踩 |

### 9.2 全部 cuda API 配对核对（`gpu_conv_cuda.cu`）

| 分配 | 释放 | 结论 |
|---|---|---|
| `cudaMalloc` dH / dRing / dAcc / dX（:289-292） | `cudaFree` ×4（:379-383 修后） | ✅ 不混 |
| `cudaHostAlloc` hX / hAcc（:294-295，全文件仅此两处） | `cudaFreeHost` ×2（修后） | ✅ 本次修复 |
| `cudaStreamCreate` stream（:296） | `cudaStreamDestroy`（:384） | ✅ |
| `rta_gpu_convolve` 整块接口 dH/dX/dY（:131-133，含 3 条失败早退路径） | 末尾 `cudaFree` ×3（:146） | ✅ 早退也逐个释放 |
| `rta_gpu_available` 探针 d（:74） | `cudaFree`（:81） | ✅ |
| `gpu_resample_cuda.cu` 全部 `cudaMalloc` | `cudaFree` | ✅（但见 9.4 遗留项） |

流式函数本身用 pinned 内存是对的：`cudaMemcpyAsync`（:335/:343）要求 host 端是 pinned，
否则不报错但退化成同步拷贝（正是 §2 注释里那 36→18 ms 的差距）。

### 9.3 DLL 静态验证（本机无 CUDA 工具链：`cuobjdump` 不存在，改用自写 PE/fatbin 解析 + `dumpbin`）

| 项 | 结果 |
|---|---|
| 字节 / sha256 | **1,009,664 B** / `411A9FD2F02850E35843B163C580C6CDB8C46864C68CFEE7D21C2FDAADB442B4`（与 PR 声明逐字符一致） |
| PE | PE32+ x64，7 节，SizeOfImage 1,036,288，最大节末 = 1,009,664 = 文件长度 ⟹ **无截断**；`.nv_fatbin` 652,800 B |
| `dumpbin /exports` | 8 个名字：7 个 `rta_*`（available / convolve / convolve_create / convolve_stream / convolve_destroy / resample / resample_invalidate）+ nvcc 自带的 `NvOptimusEnablementCuda` 数据符号 |
| cubin（`-lelf` 等价） | **57 个 ELF，19 档 arch × 3**：50,52,53,60,61,62,70,72,75,80,86,87,89,90,100,101,103,120,121 —— 与 PR 声称的 19 档**一个不缺、无多余** |
| PTX | `compute_121`（`.target sm_121`，PTX ISA `.version 8.8` = CUDA 12.9），`release 12.9` 字符串 |
| 旧 DLL 对照 | 766,464 B / `BCE4093D…`：同样 19 档 57 cubin，但**导出只有 5 个、完全没有 `convolve_create/_stream/_destroy`**（`mac_stream_batch_kernel` 字符串也是 0）⟹ 仓库里那份产物是 **261026-08 之前**的，CUDA 用户此前一直拿不到流式会话 |
| 反汇编 `rta_gpu_convolve_destroy`（RVA 0x3ff0, 176 B） | 4×`call 0x5290`（dH/dRing/dAcc/dX = `cudaFree`）、1×`call 0x6480`（stream destroy）、末 2×`call 0x53d0`（hX/hAcc）—— **后两次目标与前四次不同** ⟹ 二进制里真的是 `cudaFreeHost`，不是"只改了源码" |
| 无 N 卡加载 | `LoadLibraryA` **成功**（本机无 `nvcuda.dll`）、7/7 `GetProcAddress` 命中、`rta_gpu_available()=0`、`create()=nullptr`、不崩 —— 与 `/api/gpu` 的 `cudaUsable` 机制一致 |

### 9.4 HIP 侧门禁（本机 RX 9070 XT / gfx1201，本轮重编 `rtagpu.dll`：25 档含 `--offload-arch=gfx1201`）

`ctest -R gpu` **3/3 通过**，且 `test_gpu_stream` 三档**是真 PASS 不是 SKIP**（`on_gpu()` 为真）：

| IR | GPU vs CPU 流式 | max\|Δ\| |
|---|---|---|---|
| 1024 | −313.97 dB | 0 |
| 8192 | −314.00 dB | 0 |
| 65536 | −310.66 dB | 0 |

`gpu_stream_check`：平凡 kernel 写 12345 读回 = 1；常量增益 `ir[0]=2`、输入全 1 ⟹ 输出 2，`max|y−2|=0`；
四档（含 262144）全 PASS；任意切分 / `reset`（换 IR）全 PASS；整块接口回归 −145.2/−138.5/−131.1/−132.0 dB。
`gpubench_conv`（生产路径 `GpuBridge::convolve`）：IR=262144 **2.9×**（22.63→7.92 ms）、相对 −138.4 dB；
IR=65536 **2.0×**（8.07→3.96 ms）、−137.0 dB。

⚠️ 这张表是 **HIP** 的成绩。HIP 的 `hipFree` 能收 pinned，所以**修复前后 HIP 不受影响** ——
它证明的是"流式会话/destroy→create 这条路径在本机是通的"，不能拿来当 CUDA 修复的运行时证据。

### 9.5 未验证项（铁律 4）

1. **CUDA 运行时完全未验证**：本机无 N 卡、无 NVIDIA 驱动（`nvcuda.dll` 不存在），
   `rta_gpu_available()=0`。PR 声称的 RTX 4080 上"destroy 后 create 永久失败"与修复后
   `gpu_stream_check` 全 PASS / IR=262144 29.2 vs 158.6 ms（5.4×）**一条都没有在本机复现**。
2. **PR 的"假绿"表格有一处对不上**：他写 `test_gpu_stream` 的 IR=65536/262144 是 SKIP，
   但本仓库 `tests/test_gpu_stream.cpp:76` **只有 1024/8192/65536 三档，没有 262144**
   （262144 只出现在 `tools/gpu_stream_check.cpp`）。另外"从第 2 个会话起永久失败"与
   他表里 IR=8192 仍 PASS 也自相矛盾。**这些数字本轮无法核实，按未复现记录。**
3. **"假绿"的判断本身成立**：`test_gpu_stream.cpp:105-108` 在 `on_gpu()==false` 时打印
   `[SKIP]` 而不是 FAIL ⟹ 只要 create 建不起来，ctest 照样绿（铁律 3 的典型形态）。
   建议后续把判据改成"**DLL 声明 stream_supported 且 available，却 create 失败 = FAIL**"。
   `tests/` 不在本轮改动范围内，故只记录不改。
4. **全 arch 实机覆盖**：19 档里本机连一档 CUDA 都跑不了，仍然只能靠静态证明"编进去了"（§7 第 5 项未变）。

### 9.6 部署

`cuda-artifacts/rtagpu_cuda.dll` → 两个 1.5.14 目录，逐字节核对 sha256 相同：
`D:\RTA-hqplayer\dist\RLPlayer-1.5.14-win64\rtagpu_cuda.dll`、
`D:\RTA-rel\dist\RLPlayer-1.5.14-win64\rtagpu_cuda.dll`（均为 1,009,664 B / `411A9FD2…`，原 766,464 B / `BCE4093D…`）。
正在运行的 `rtaweb` 加载的是 HIP 版 `rtagpu.dll`，未持有该文件句柄；CUDA DLL 换了要**重启**才生效。

