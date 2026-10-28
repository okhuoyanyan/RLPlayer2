# RLPlayer 官网（静态站）

三个页面，纯 HTML + CSS，无构建步骤、无框架、无外部依赖（字体用系统栈）。

## 文件

| 文件 | 内容 |
|---|---|
| `index.html` | 主页：Hero / 六件事 / 实测数据 / 滤波器 / DSD / 界面 / 团队 |
| `download.html` | 下载：Windows 2.0.0 与 macOS 1.1.0-beta4 双卡 + SHA-256 + 平台能力对比 |
| `features.html` | 技术细节：信号链 / 79 个滤波器实测 / DSD 环路保护 / GPU 逐位等价 / bit-perfect / 音频分析 / **已知限制** |
| `assets/style.css` | 唯一一份样式（深色 HiFi 风，与播放器本体同一套色板） |
| `assets/img/*.png` | 真机截图（全部来自 RLPlayer 2.0 实际窗口） |
| `_preview/*.png` | 三页的实机渲染验收图（2560×1440，Edge kiosk 全屏抓取） |

## 本地预览

```powershell
python -m http.server 18440 --directory website
# 打开 http://127.0.0.1:18440/index.html
```

## 部署

纯静态，任何静态托管都行（Cloudflare Pages / GitHub Pages / Nginx / OSS）。

- **Cloudflare Pages**：直接上传 `website/` 目录，无构建命令
- **GitHub Pages**：把 `website/` 内容推到 `gh-pages` 分支
- **Nginx**：`root /path/to/website;` 即可

目标域名：`rlplayer.rainlain.com`（用户侧待上线）

## 需要替换的占位

| 位置 | 当前值 | 待办 |
|---|---|---|
| `download.html` 的「从 GitHub 下载」 | `github.com/okhuoyanyan/RLPlayer2/releases/latest` | 2.0.0 发布后该链接才有效 |
| SHA-256 | `5FA024A1…677A` | 每次重新打包都要更新这串 |
| macOS 版本号 | `1.1.0-beta4` | 协作者发新版时同步 |

## 设计说明

- **色板**：与播放器本体一致（`#0a0c10` 底 / `#6b8afd` 强调），站内看到的界面截图与真机同一观感
- **文案口径**：只写能核对的事。每个数字都标了测法；测不出来的写「未验证」
- **断点**：980px（平板）/ 480px（手机）。导航在 480px 以下只留按钮
- **已知坑**（都踩过并修了）：
  - `--app` 模式的 Edge 会忽略 `--window-size`，且 innerWidth 与窗口宽不成比例 ⟹ 截图验收改用 `--kiosk`
  - 页面曾被某个宽元素撑到 >2 倍视口，导致 `margin:0 auto` 失效、整页右移；已加 `html,body{overflow-x:clip;max-width:100vw}`
