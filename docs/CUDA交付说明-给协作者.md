# CUDA 后端交付说明（给协作者 garaxc）

> 背景：PR #1 的 CUDA 部分已按「选择性集成」合并进 main（集成 commit `4cb471b`，原提交 `c3de461` 保留署名），
> 并随 v1.3.0+ 发布。但**包内没有 \`rtagpu_cuda.dll\`** —— 因为开发机是 AMD 平台，没有 NVIDIA 显卡与 CUDA 12.x 工具链。
> 这份说明写清楚「还差什么、怎么补、验收标准是什么」。

## 一、已经进主线的部分（不需要你再做）

| 内容 | 位置 | 状态 |
|---|---|---|
| CUDA 卷积 kernel | \`src/gpu/gpu_conv_cuda.cu\` | 已合入 |
| CUDA 重采样 kernel | \`src/gpu/gpu_resample_cuda.cu\` | 已合入 |
| 多架构 fatbin 构建脚本 | \`tools/build_gpu_cuda.ps1\`（19 档 sm_50..sm_121 + `cuobjdump -lelf`/`dumpbin /exports` 双校验） | 已合入 |
| 精简 CUDA 组件拉取 | \`tools/fetch_cuda_redist.js\`（约 140 MB，非 3.7 GB 安装器） | 已合入 |
| 加载/回退路径 | \`src/dsp/gpu_pref.h\`（auto = rtagpu.dll → rtagpu_dx12.dll → rtagpu_cuda.dll）、\`gpu_bridge.h\` | 已合入，**缺失时安静回退 AMD/CPU**（实测 `/api/gpu` → `cuda:false, amd:true`，不崩） |
| UI/状态 | 设置页「GPU 后端」多出 NVIDIA（CUDA）档；`/api/gpu` 带 \`cuda\` 字段 | 已合入 |
| 诊断工具 | \`tools/wasapi_ex_probe.exe\`、\`tools/gpubench_conv.exe\`、\`src/audio/pcm_dump.h\` | 已随包发布 |
| 兼容门禁 | \`tools/check_compat.ps1\` 把 \`rtagpu_cuda.dll\` 标为 **OPTIONAL**（缺失不判失败） | 已合入 |
| 文档 | \`docs/GPU-CUDA后端纪要.md\`（含 fatbin 陷阱、sticky error、CUDA 12.x 版本约束） | 已合入 |

## 二、需要你在 NVIDIA 机器上补的（就一件事 + 验收）

### 1. 出 \`rtagpu_cuda.dll\`
\`\`\`powershell
# 若本机没有 CUDA 12.x 工具链，先拉精简组件（约 140 MB）
node tools\fetch_cuda_redist.js

# 构建（脚本内部会做两道校验：每个 arch 的 cubin 是否真进二进制、4 个导出符号是否在位）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_gpu_cuda.ps1 -Out build\rtagpu_cuda.dll
\`\`\`
- **必须留在 CUDA 12.x**（13 已移除 Maxwell/Pascal/Volta；12.9 实测支持 `compute_50`…`compute_121`，缺 `compute_110`）；
- 4 个导出符号必须齐全：`rta_gpu_available / rta_gpu_convolve / rta_gpu_resample / rta_gpu_resample_invalidate`
  （改名不会报错，只会静默退回 CPU —— 脚本的 `dumpbin /exports` 就是防这个）。

### 2. 验收（三条，缺一不可）
1. **平凡 kernel 先证能跑**：不要相信「没报错」。先跑一次写常数的平凡 kernel 并读回比对
   （CUDA 的坑与 HIP 不同：带 PTX 时驱动会 JIT，arch 不对也"看起来能跑"；sticky error 会让
   `cudaGetLastError()` 一直返回旧错，探测前必须先清一次）。
2. **数值对上**：\`build\gpubench_conv.exe\` 应与 CPU 参考一致（我们的 AMD 侧是相对 RMS 误差 5.97e-08）；
   \`ctest -R gpu\` 全绿。
3. **状态如实**：\`GET /api/gpu\` 在放好 DLL 后应变成 \`"cuda":true, "available":true\`；
   把 DLL 拿走应回到 \`"cuda":false\` 且不崩。

### 3. 交回方式
- DLL 体积、构建命令、CUDA 版本、显卡型号与驱动版本、上面三条验收的原始输出；
- DLL 放 `dist\RLPlayer-<版本>-win64\` 根目录（与 `rtagpu.dll` 同级）即可被加载；
- 我们会在下一个发布包里把它作为**自包含组件**打进去（铁律 6：正式版必须自包含，所以这一步做完 CUDA 才算"真的支持"）。

## 三、当前状态（如实）

- 主线代码：**已支持** CUDA 后端（加载、回退、UI、门禁全就绪）；
- 发布包：**不含** `rtagpu_cuda.dll`，NVIDIA 用户当前会看到「CUDA 不可用」并自动走 AMD/CPU；
- 因此 PR #1 的 CUDA 部分**只完成了一半**：软件侧全部到位，缺的是在 NVIDIA 机器上产出并验证那个 DLL。


---

## 四、✅ 已交回（2026-10-23，PR #3）

协作者已按上面的流程交回二进制（分支 `feat/cuda-dll` → PR #3，已合入 main）：

| 项 | 值 |
|---|---|
| 文件 | `cuda-artifacts/rtagpu_cuda.dll`（**766,464 字节**） |
| sha256 | `BCE4093D45D0234B914335D558A18F174D904EB9A4C0FCBFC12CE35579739E6B` |
| 编译器 | CUDA 12.9.41（非 13）；fatbin 覆盖 sm_50…sm_121 共 19 档（缺 compute_110 正常），另带 PTX compute_121 |
| 导出 | 4 个符号齐全（rta_gpu_available / convolve / resample / resample_invalidate） |
| 编译机 | RTX 4080 / 驱动 617.42（L0–L4 全绿：卷积相对 -138.0 dB、gpubench 2.4×） |

**Lead 侧复核（AMD 机器，独立做）**：字节数/sha256 与 PR 一致；PE 头正常；`dumpbin /exports` 4 个符号与 PR 所列 RVA 完全一致。

**运行期如实报告已加强**：把 DLL 放进 `build\` 后，在**没有 NVIDIA 卡的 AMD 机器**上
`GET /api/gpu` 现在是 `{"cuda":true,"cudaUsable":false,...}`（文件在但本机不可用），
播放自动走 AMD/CPU 且不崩 —— 这正是之前担心的"有文件就报就绪"的假象，已修掉。

**还差最后一步（Lead 做）**：`make_dist.ps1` 自 12a23ac 起会**自动收录** `build\rtagpu_cuda.dll` →
出 1.3.10 正式包后 CUDA 才算"真的随包自包含"（铁律 6）。
