# HQPlayer NAA 协议调研（自研复刻可行性）

**结论**：能自研，但只能"黑盒兼容"——Signalyst 零规范、零源码，全部细节靠二进制行为反推。协议本身不复杂（XML 控制 + 裸 PCM/SDM 流），单人 3–4 个月可做出可用收端；发送端兼容风险更高。

## 1 发现机制（已证实）
- 端口 **43210，TCP/UDP 同号**。实测本机 networkaudiod 5.19：UDP 0.0.0.0:43210 收发现、TCP 同端口收数据（netstat 实测）。
- **组播而非广播**（作者 Jussi 原话：广播是特权操作，[Roon 帖](https://community.roonlabs.com/t/hqplayer-embedded-discussion/244327/2666)）。组地址从 5.19 二进制提取：IPv4 `224.0.0.199`、`239.192.0.199`；IPv6 `ff02::c7`、`ff05::c7`。
- 报文为 **XML 文本**。HQPlayer 日志：`discovered network audio: name='naa-xxxx' version='Signalyst Network Audio Daemon 5.19' protocol='5' trigger='1' @ip:43210`。
- HQPlayer 控制端另有独立发现：`239.192.0.199:4321`，报文 `<discover>hqplayer</discover>`（[hqpwv 源码](https://github.com/zeropointnine/hqpwv)），可作 NAA 报文风格旁证。

## 2 控制/数据通道（已证实；标注处为推测）
- 单条 **TCP 43210** 连接：先 XML 握手，再切裸流；`enter streaming mode, version=` 表明流模式独立版本。含 `clSocket::SetNagle()`、Windows QoS(DSCP) 调用 → 确为 TCP。
- XML 元素：discover / protocol / version_req / version / getdevices / initialize / getformats / rate / bits / format / netbuftime / stream / start / stop / keepalive / position / metadata / trigger。
- 音频帧 **pcm4 / sdm4 / pcm5 / sdm5**（版本化封装），逐字节布局未公开（**推测**需抓包逐字段试）。
- 时钟：无 PTP/RTP 字符串，端点回传 position 做流控，DAC 为主时钟，无显式时钟反馈报文（**推测**）。

## 3 官方公开程度（已证实）
- 手册 65 页仅一段概念描述，"43210/组播/IPv6"出现 **0 次**（本地 [hqplayer6desktop-manual.pdf](<C:\Program Files\Signalyst\HQPlayer 6 Desktop\hqplayer6desktop-manual.pdf>) 解密后全文检索）。
- 下载页仅二进制：[Windows/Linux networkaudiod](https://www.signalyst.eu/bins/naa/windows/)、[NAA OS 镜像](https://www.signalyst.eu/bins/naa/images/)（含 1.3GB foss-src 源码包，无协议文档）。
- hqp-control2/hqp6-control **只发 exe 无源码**；Signalyst 的 GitHub 账号 0 个仓库。无规范、无参考实现、无 Wireshark dissector。

## 4 社区复刻现状
GitHub 无 NAA 完整复刻，仅打包/集成类：[giof71 Docker](https://hub.docker.com/r/giof71/network-audio-daemon)、[AuralDesk](https://github.com/HowenXu/AuralDesk)；逆向线索散见 Roon / Audiophile Style 日志帖。等于从零逆向。

## 5 替代协议
| 方案 | 音质上限 | 工作量 | 结论 |
|---|---|---|---|
| AES67/Ravenna | 最高（PTP 纳秒级同步） | 4–6 月；Linux 可行（[开源实现](https://github.com/bondagit/aes67-linux-daemon)），Windows 需 NDIS 驱动 | 专业级首选 |
| Roon RAAT | 高 | 不可行 | 闭源+厂商认证 |
| Squeezelite/SlimProto | 高（bit-perfect、DSD DoP） | 1–2 月（[开源](https://github.com/ralph-irving/squeezelite)） | 性价比最高 |
| UPnP/DLNA+OpenHome | 中（gapless/DSD 差） | 1–2 月（upmpdcli 现成） | 兼容兜底 |
| Snapcast | 中（PCM、20–200ms 延迟） | 2–3 周（[开源](https://github.com/badaix/snapcast)） | 多房间同步，不适合 DSD |
| AirPlay2 | 低（ALAC 16/44.1） | 需 MFi | 排除 |
| 自研 gRPC/UDP | 高（自定义） | 1–2 月原型 | 无生态，自用最优 |

## 6 工程建议
1. **要接现成 NAA 硬件** → 逆向做 NAA 收端：XML 状态机 + PCM 先行，DSD 后补；用官方 networkaudiod 做对端回归测试。
2. **自家播放器 + 自家端点** → 别复刻 NAA，直接自研 UDP（RTP 式序号 + 抖动缓冲）或上 AES67，省 2/3 时间且无法律风险。
3. **折中** → 先做 Squeezelite 兼容（协议公开、生态大），跑通音频管线后再攻 NAA。
- 风险：EULA 禁止逆向（互操作用途仍有争议）；协议内部版本升级会破坏兼容；**NAA 本身不决定音质**，端点时钟/供电与缓冲策略才是瓶颈。
