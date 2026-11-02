# 开发与构建

本项目使用 C++23，主构建目标为 Windows（另有 macOS 分支由协作者维护）。

## 工具链

| 组件 | 要求 |
|---|---|
| 编译器 | MSVC 2022 或 Clang 18+（需支持 C++23） |
| 构建 | CMake 3.28+、Ninja |
| SDK | Windows 10/11 SDK |
| 其他 | Python 3.10+、FFmpeg / FFprobe |
| GPU（可选） | CUDA 12.x（NVIDIA）或 ROCm/HIP（AMD），见 `docs/GPU-*` |

## 常用命令

```powershell
# 生成静音测试素材（测试前置）
python tools/prepare_ci_testdata.py --root .

# 配置与构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 测试
ctest --test-dir build --output-on-failure

# 打包（产出 dist\RLPlayer-<版本>-win64.zip）
tools\make_dist.ps1
```

## 约定

- **版本号单一来源**：`src/version.h`，`res/rlplayer.rc.in` 由 CMake 自动代入，不手改。
- **正式发布版必须自包含**：包内需带 FFmpeg、GPU 运行时、WebView2Loader 与 CLI 工具。
- **文档**：`STATUS.md`（当前事实与已知限制）、`ROADMAP.md`（路线）、`docs/`（专题纪要）。

## 许可

GPL-3.0-or-later
