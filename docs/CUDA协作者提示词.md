# CUDA 后端：给协作者的执行提示词（2026-10-23，1.3.9 之后）

> ✅ **已完成**：协作者已交回 `rtagpu_cuda.dll`（PR #3，766,464 B，sha256 `BCE4093D…E6B`），已合入 main，随 1.3.10 包内置。本文保留作流程记录。

> 用途：把下面的「提示词」整段发给协作者（GitHub: garaxc，RTX 4080 那台机器）。
> 背景详见 [CUDA交付说明-给协作者.md](CUDA交付说明-给协作者.md) 与 [GPU-CUDA后端纪要.md](GPU-CUDA后端纪要.md)。
> 当前结论：**CUDA 软件侧已完成并合入 main（PR #1，v1.3.0+），2026-10-22 已在 RTX 4080 上 L0–L4 全绿；
> 唯一缺的是二进制 rtagpu_cuda.dll 本体没有交回**，所以正式发布包里仍不含它。
> main 的 tools/make_dist.ps1 已改为「build\ 里有 rtagpu_cuda.dll 就自动打进包」，只等这个文件。

---

## 提示词（复制以下整段）

【任务】RLPlayer CUDA 后端：产出并交回 rtagpu_cuda.dll（正式版自包含需要它）

**背景**：CUDA kernel、多架构 fatbin 构建脚本、加载/回退路径、UI、兼容门禁都已合入 main
（PR #1，v1.3.0 起）；2026-10-22 你已在 RTX 4080 / 驱动 617.42 上验证过 L0–L4 全绿
（见 docs/GPU-CUDA后端纪要.md §5）。但**二进制没有随 PR 交回**，所以 1.3.9 发布包里仍缺它，
NVIDIA 用户目前会看到「CUDA 不可用」并自动回退。主仓库：https://github.com/okhuoyanyan/RLPlayer （main = f224ff8，tag v1.3.9）

**你要做的事（就一件 + 三条验收 + 回执）**

1. 在 NVIDIA 机器上更新到 main 并重建 DLL（**必须 CUDA 12.x，不要 CUDA 13**）：

```powershell
git fetch origin; git checkout main; git pull
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_gpu_cuda.ps1 -Out build\rtagpu_cuda.dll
```

   若本机没有工具链：装 CUDA 12.9 Toolkit（或复用你上次那套精简组件），设 CUDA_PATH。

2. 构建脚本自带两道校验，请保留原始输出：
   - cuobjdump -lelf build\rtagpu_cuda.dll：每个请求的 arch 都有 cubin（19 档；缺 compute_110 正常）
   - dumpbin /exports build\rtagpu_cuda.dll：4 个导出符号齐全
     rta_gpu_available / rta_gpu_convolve / rta_gpu_resample / rta_gpu_resample_invalidate

3. 运行时三条判据（必须实测，不接受「没报错」）：
   a. **平凡 kernel 先证能跑**：写常数 → 读回比对（走 GpuBridge 真实动态加载路径）；
   b. **数值对上**：build\gpubench_conv.exe 与 CPU 参考一致（相对 RMS < 1e-4；上次 6.019e-08），ctest -R gpu 全绿；
   c. **状态如实**：DLL 在位时 GET /api/gpu → {"cuda":true,"available":true,...}；把 DLL 移走 → "cuda":false 且不崩。

4. 交回方式（推荐 A）：

```powershell
git checkout -b feat/cuda-dll
New-Item -ItemType Directory -Force cuda-artifacts | Out-Null
Copy-Item build\rtagpu_cuda.dll cuda-artifacts\rtagpu_cuda.dll
git add cuda-artifacts/rtagpu_cuda.dll     # .gitignore 已放行 cuda-artifacts/*.dll
git commit -m "build(cuda): rtagpu_cuda.dll (CUDA 12.9 fatbin, sm_50..sm_121)"
git push origin feat/cuda-dll
```

   然后开 PR（或直接告诉用户分支名）。
   B. 或者直接把 DLL 文件 + 下面清单发给用户（不需要传 370 MB 的包，只要这个几百 KB 的 DLL）。

5. 回执清单（必须给全，缺一项我们无法发布）：
   - rtagpu_cuda.dll 的**字节数**与 **sha256**
   - nvcc --version 全文、CUDA 版本
   - 显卡型号 + nvidia-smi 驱动版本
   - 上面第 2、3 步的原始输出（cuobjdump 摘要 / dumpbin 4 符号 / gpubench 数字 / ctest 结果 / /api/gpu 前后 JSON）
   - 与 2026-10-22 那次相比有无差异（源码/工具链/结果）

**边界（重要）**
- 只做 CUDA 后端这一件事；**不要改**主程序逻辑、版本号、打包脚本（make_dist 已支持自动收录该 DLL）。
- 若为了让 DLL 跑通必须改 src/gpu/*.cu 或 build_gpu_cuda.ps1：可以改，但请单独说明「改了什么/为什么/影响面」，并分成独立提交。
- **不要用 CUDA 13**（13 已移除 Maxwell/Pascal/Volta，会让老卡直接不可用）；目标仍是 12.9 全架构。
- 不要提交 build/ 产物、*.log；DLL 走第 4 步。
- 项目铁律：不 force-kill 进程树；不向默认音频设备播放可听声音（验证用 /api/gpu 与 gpubench_conv，不要开声卡）。

**我这边的合并动作（收到后）**
1. 把 DLL 放进 build\ → make_dist 自动打进包 → 出 **1.3.10**；
2. 在 AMD 机器上验「包里带 CUDA DLL 但无 NVIDIA 卡」不崩、自动回退（/api/gpu 如实、播放走 AMD/CPU）；
3. 合并分支/PR，更新 GPU-CUDA后端纪要.md 与发布说明，Release 附 sha256。

---

## 附：为什么编译不一定需要 NVIDIA 卡，但建议还是让他们做

- **编译**只需要 CUDA 12.x 工具链（nvcc）+ MSVC，不需要 GPU；
- **验收 a/b/c 需要真 NVIDIA 卡**（我们开发机是 AMD RX 9070 XT，没有 CUDA 设备）；
- 协作者那台 RTX 4080 两者都有，且上次已经验证过同一份源码 —— 让他重建+复验一次是最快、最可靠的路径。
