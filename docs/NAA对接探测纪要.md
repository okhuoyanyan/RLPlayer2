# NAA 对接探测纪要（2026-10-21 · 第八轮）

> 目标端点：http://192.168.50.109/（Accurate Audio “Wavedream NET”，主机名 MBN-N1000），
> 网页后台里只勾选了 **HQPlayer NAA（networkaudiod）**，UPnP / Roon / Squeeze / MPD 都是关的。
> 本轮只做**只读探测**与本地对照实验：没有向端点发送音频，没有打开任何本机音频设备。

## 1. 已确认的事实

| 项 | 结果 |
|---|---|
| 端点可达 | ping 通；TCP **43210** 开放；443/8080/9000/9790/4321 关闭 |
| 另一台候选 | 192.168.50.212:43210 也开放（未确认是否 NAA；连上后不发任何数据） |
| 网页后台 | “Accurate Audio NET Control”，含 POST /api/services/update（service=…&command=toggle） |
| 服务互斥 | 页面 JS 在启用某一服务时会取消其它服务：**开启 UPnP（upmpdcli）会关掉 NAA** |
| 被动发现 | 监听 224.0.0.199 / 239.192.0.199 共 8 秒，**没有**收到 NAA 通告（NAA 只应答查询） |
| 主动 UDP 查询 | <discover>naa</discover> 等 6 种写法，单播/组播都**无回应** |

## 2. TCP 43210 的握手（关键发现）

连上后 **服务端立即主动发 16 字节随机数**，每次都不同，然后等待客户端报文：

    连接 1: A7 35 E9 99 28 42 9E 71 CC 70 C0 46 94 BC 2B 94
    连接 2: 67 40 C4 D7 9B E1 1B 0E D5 C7 C7 DD 43 C4 8B B9
    连接 3: BA 0F DA 57 9D 7C CD A6 F5 BD 79 B9 43 89 30 66

**这不是该设备的特例**：下载官方 networkaudiod **5.19**（Windows 版）在本机 127.0.0.1:43210 跑起来，
行为完全一样（同样先发 16 字节随机数）。所以这是 NAA 5.x 的标准行为。

## 3. 试过的客户端报文（全部无回应）

tools/naa_probe.cpp（本轮新增，--matrix 可复现）：

- 纯 XML：<protocol></protocol>、<protocol>1</protocol>、<protocol/>、
  带 XML 声明、<getdevices>、<version_req>
- 结尾：换行 / NUL / 无结尾
- 把服务端的 16 字节**原样回显**，或回显 + XML 同一次 write
- 16 字节全零 + XML、MD5(16 字节) + XML
- 4 字节长度前缀（小端/大端）+ XML、UTF-16 + NUL
- 只发 16 字节随机数（不发 XML）

daemon 侧打开 NETWORKAUDIOD_TRACE=1 后，日志里只有连接/断开，以及我们自己断开时的一条
protocol error: clSocket::Recv(): recv() —— 说明它确实在**读**，但我们的报文没有被识别。

## 4. 结论

NAA 5.x 的控制通道不是「连上就发 XML」：服务端先给一个 16 字节的随机量（会话密钥/挑战），
客户端必须先按未公开的规则处理它，之后才是 XML 状态机（version_req / getdevices / initialize /
getformats / start / stop / keepalive …，这些元素名来自 5.19 的字符串，与调研报告一致）。
**没有客户端那一侧的真实报文，握手无法凭空猜出来。**

## 5. 合规提醒（必须写清）

networkaudiod 的 EULA 明确禁止逆向工程/反汇编，并且禁止「用 HQPlayer 之外的软件推流」。
本轮的观察是自己运行官方守护进程 + 抓它主动发出的字节，属于互操作性测试，但继续深挖协议
（尤其是照抄客户端握手）在 EULA 下是有争议的。**是否继续，由用户决定**。

## 6. 可复现的命令

    build\naa_scan.exe --listen 8                       # 发现端点（TCP 43210 + 组播监听）
    build\naa_probe.exe --host 192.168.50.109 --matrix  # 只读：看 16 字节问候 + 各种报文尝试
    # 本机对照（可选）：运行官方 5.19 后
    build\naa_probe.exe --host 127.0.0.1 --matrix
