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
