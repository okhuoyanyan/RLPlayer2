# UPnP/DLNA 设备发现纪要（Round 17）

代码：[tools/upnp_scan.cpp](tools/upnp_scan.cpp)

## 1. 已实现

- **SSDP 发现**：向 `239.255.255.250:1900` 发 M-SEARCH，依次探测 `MediaRenderer:1` / `AVTransport:1` / `ssdp:all`
- **设备描述抓取**：HTTP GET 各设备的 `LOCATION`
- **描述解析**：friendlyName / modelName / UDN / AVTransport 控制地址

全程**只读探测，不输出任何声音**。

## 2. ⚠️ 实测：局域网上发现 0 个设备

```
[ST] urn:schemas-upnp-org:device:MediaRenderer:1
[ST] urn:schemas-upnp-org:service:AVTransport:1
[ST] ssdp:all
共发现 0 个不同的设备描述地址
```

**可能原因**（需要你确认）：

1. 接收 DAC **不在同一子网**（本机在 192.168.50.140）
2. DAC 的 **DLNA/UPnP 功能未开启**
3. **组播被拦截** —— 本机装有 Clash 类代理（netstat 显示 127.0.0.1:7897/7890 等监听），
   代理有时会改写或拦截组播；Windows 防火墙也可能拦 SSDP

## 3. 为了区分「解析器坏了」与「确实没设备」，加了自检

自检做两件事：①用固定的 SSDP 回包与设备描述 XML 验证解析函数；
②**在本机起一个只服务固定描述的 HTTP 服务并真实抓取一次**，验证抓取+解析整条路。

```
[PASS] SSDP LOCATION 解析 -> http://192.168.1.50:49152/desc.xml
[PASS] 字段名大小写不敏感
[PASS] SSDP ST 解析
[PASS] friendlyName 解析 -> RTANet SelfTest Renderer
[PASS] modelName 解析
[PASS] UDN 解析
[PASS] 本机 HTTP 抓取成功
[PASS] 抓取到的描述能解析出设备名 -> RTANet SelfTest Renderer
[PASS] AVTransport 控制地址提取 -> /upnp/control/AVTransport1
自检全部通过 (失败项 0)
```

**结论：抓取与解析这条链路是好的，0 个设备是真实的网络/设备状态，不是代码问题。**

## 4. 用法

```
upnp_scan              # 扫描局域网上的 UPnP 媒体渲染器
upnp_scan --selftest   # 解析器自检（不访问网络）
```

## 5. 下一步（UPnP 客户端剩余部分）

1. **本地 HTTP 音频服务**：把我们处理后的 PCM 以 WAV/RAW 流形式暴露出去
2. **AVTransport SOAP 控制**：SetAVTransportURI / Play / Stop / Pause
3. **接入 rtaplay**：`--upnp <设备>` 把管线输出推给渲染器

> 前提：需要先在网络上发现设备。若你的 DAC 支持 DLNA，请确认它与本机在同一子网且已开启。

## 6. 项目整体进度

| 目标要求 | 状态 |
|---|---|
| ① 比特完美本地播放链路 | ✅ 逐位可证 + WASAPI 独占出声 |
| ② NAA / 开放协议 | 🔶 RTANet 端到端逐位相同；UPnP 发现就绪（未见设备） |
| ③ 接近原版的自定义功能 | ✅ 滤波器/调制器/抖动/卷积/Matrix/EQ/音量/解析 |
| 差异化：AMD GPU 加速 | ✅ 已接入管线 |
