# RLPlayer 软件介绍短片 — 分镜脚本

- 成片：1920×1080 · 30fps · 约 150 秒（2 分 30 秒）
- 技术路线：可 seek 的 Canvas 2D 动画（纯函数于时间 t，逐帧确定性渲染）→ Edge 无头 CDP 截帧 → ffmpeg 合成
- 配乐：离线合成 ambient 床乐（仅写文件，绝不打开音频设备）
- **禁用字样（全程不出现）**：Roon / HQPlayer / RTA（含 opra.roon.app 链接）
- 品牌口径：**RLPlayer** · HIGH-FIDELITY ENGINE（与 src/net/ui_page.h:735 现网一致）
- 视觉基调：对齐软件本体深色主题（#0a0e14 底、#3b6ef6→#7b5cff 蓝紫渐变强调、绿 GPU 徽标）

## 时间轴

| 场景 | 时间 | 时长 | 负责人 | 内容与节拍 |
|---|---|---|---|---|
| S0 片头 | 0–11s | 11s | 主控 | 粒子汇聚 → R 徽标 + 波纹环 → RLPlayer 字标浮现 → 标语「为极致聆听而生」 |
| S1 曲库 | 11–29s | 18s | Flash | 3D 封面墙飞入；格式徽标（FLAC/DSD/WAV/APE/ALAC/OGG/Opus…）；搜索演示「毫秒级搜索」；封面波浪 |
| S2 直通 | 29–50s | 21s | 主控 | 音频路径动画 文件→解码→独占流→DAC；「系统混音器」被划掉；比特计数器原样传递；六态徽标轮播；「0 混音 · 0 重采样 · 0 音量处理」 |
| S3 升频 | 50–76s | 26s | Flash | 44.1k→705.6k 波形加密动画；频响曲线绘制（阻带 −189dB · 纹波 0.0000dB）；GPU 面板（NVIDIA CUDA / AMD GPU / 262,144 抽头 · 2.0ms/s）；实时频谱条 |
| S4 DSD | 76–95s | 19s | 主控 | PCM 阶梯波 → 1-bit 脉冲密度流；DSD 512 · 22.6MHz 计数；「原生 DSD·ASIO」「DoP 24bit 载波」双通道 |
| S5 AI | 95–115s | 20s | Flash | AI 简介打字机；三卡片：智能检索/自动补全/防幻觉标注；来源 chips（在线曲库 ×4 + 歌词库 + 音乐数据库）带 ✓ |
| S6 细节 | 115–133s | 18s | 主控 | 2×2 宫格：歌词逐行滚动 / 20 段 PEQ 拖拽曲线 / UPnP 投放动画 / 托盘+三主题 |
| S7 结尾 | 133–150s | 17s | 主控 | 8 卖点 chips 级联 → logo 定版 → 「单文件安装包 · 免依赖 · Windows 10/11」→ 黑场 |

## 文案（成片可见文字，已过禁用词检查）

- S0: RLPlayer / HIGH-FIDELITY ENGINE / 为极致聆听而生 · Windows 高保真播放引擎
- S1: 你的整个曲库，一目了然 / FLAC · DSD · WAV · APE · ALAC · WV · OGG · Opus / CUE 整轨自动分轨 / 毫秒级搜索 / 艺术家 · 专辑 · 古典作品
- S2: 位元直通 · 不动一个比特 / WASAPI 独占 · ASIO · 绕开系统混音 / 0 混音 · 0 重采样 · 0 音量处理 / BIT-PERFECT
- S3: 可测量的升频引擎 / 多相 sinc · 阻带 −189 dB · 通带纹波 0.0000 dB / 44.1 kHz → 705.6 kHz / NVIDIA CUDA · AMD GPU / 262,144 抽头卷积 · 2.0 ms / 秒音频
- S4: 原生 DSD 全链路 / DSF · DFF 直读 / DirectSDM 逐位直通 / DoP 24-bit 载波 / PCM → DSD 512 · 22.6 MHz / 1-bit
- S5: AI 帮你打理曲库 / 智能检索 — 用一句话找歌 / 自动补全 — 封面 · 标签 · 歌词 / 防幻觉 — 「AI 整理」标注 · 来源可溯 / （来源名用通用词，不出现第三方商标以外的敏感字样）
- S6: 细节，都在 / 歌词逐行跟随 / 20 段参数 EQ · 耳机校准预设 / UPnP 一键投放 / 托盘常驻 · 三套主题
- S7: 位元直通 · 升频引擎 · GPU 加速 · 原生 DSD · AI 整理 · 20 段 EQ · 歌词跟随 · UPnP 投放 / 单文件安装包 · 免依赖 · Windows 10/11

## 管线命令

```bash
# 截帧（在 promo/ 下）
node capture.js --fps 30 --dur 150 --format png
# 配乐（仅写文件）
python makemusic.py
# 合成
ffmpeg -y -framerate 30 -i frames/f%05d.png -i music.wav -c:v libx264 -preset slow -crf 18 \
  -pix_fmt yuv420p -profile:v high -movflags +faststart -c:a aac -b:a 192k -shortest out.mp4
```
