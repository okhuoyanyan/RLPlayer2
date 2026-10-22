# RLPlayer 状态

> **接手请先读**：[HANDOVER.md](HANDOVER.md)（架构/构建/已知坑）→ [ROADMAP.md](ROADMAP.md)（待开发与判据）。
> 本文件按回合记录“做过什么、实测多少”，越往下越旧。
# 第二十四轮:ROADMAP 剩余阶段开跑——P4 输入采集 / P5 UPnP 投放 / P1 调制器族 / P2 滤波器变体(2026-10-23)

> 用户指令:无人值守连续开发(「直到开发结束」)。Lead 只核验/派单,编码全派 Gemini 3.8 Flash(high)子代理,并行用独立 worktree。
> 本轮结果:ROADMAP 的 **P4 / P5 / P1 / P2** 四阶段一次性落地;ctest 53 → **53 套全绿**(新增 capture / upnp_control / modulator_families);
> 修掉 **两个真实质量缺陷**(Chebyshev-7 低电平失稳、P4 采集 HTTP 线程 join 死锁)与一个 uicheck 假红体系。

## 1. P4 输入采集(WASAPI)——核心 + API + UI

**P4a 核心**(6637232):
- `src/audio/wasapi_capture.h`:设备枚举(含 `[Loopback]` 回环端点)、共享模式采集、**无锁 SPSC 环形缓冲**(cacheline 对齐/溢出/下溢/水位/削波正负计数)、mix 格式→float32(16/24/32-bit PCM + float,**24-bit 3 字节符号扩展**有专门断言)、`analyze_audio`(RMS/Peak/THD+N,加 Hann 窗残差 FFT)、`write_capture_wav`;
- `tools/rtacapture.cpp`:--list/--dev/--loopback/--seconds/--out/--analyze/**--src null/--inject**(不开设备、不发声的确定性源)/--fast;退出码 0/2/3/4;
- 测试证据:环形缓冲环绕/溢出/下溢;**模拟 10 分钟 48k 数据 28,800,000 帧 → 0 溢出/0 下溢/序列号逐点正确**(快进 0.016s);16/24/32/float 转换与削波计数全项;**--src null 注入 1kHz −60dBFS → 48000 帧、RMS 误差 0.0000%、THD+N 154.10 dB**;麦克风真机采集 1 秒 48,000 帧/0 溢出。

**P4b API + UI**(8fdf242 / 4cd197b):
- 端点:`GET /api/input/devices`(真机返回麦克风 + 2 个 Loopback 端点)、`GET /api/input/state`、`POST /api/input/start`(mode=record|monitor,dev,loopback,out,seconds;**src=null&inject=… 测试路径**)、`POST /api/input/stop`;
- 修掉的真 bug:start 时无条件 `join()` 会**卡死 HTTP 线程**(改为先判 running + joinMtx 串行化);stop 返回前保证 WAV 已刷盘闭合;POST body 尾随 \r\n 污染 outFile;不存在设备要明确报错;录制线程 maxSeconds 保底超时;
- 设置页「输入」面板:设备下拉(回环/默认标记)、录制时长、输出路径、开始/停止、1s 状态轮询(帧/水位/溢出/欠载/削波/RMS/Peak/错误)、**直通监听(明确标注「非 bit-perfect」)**;uicheck 新增 9 控件 + 状态面板 + 无外链断言;
- 门禁实证:API 录制 `src=null` → `frames=48000, overflow=0`,WAV 头 24-bit/48k/2ch/288000 字节 data(48000 帧),分析 RMS −63.01 dBFS / THD+N **90.06 dB**;uicheck 全过;8899 全程无监听。
- ⚠️ 未解决:Windows 回环在系统静音时不产生数据包(平台规范)——静音录制需读取侧补零;监听=共享输出直通,**采集→DSP 管线尚未接**。

## 2. P5 UPnP/DLNA 投放(3a37039 → ae773b9)

- `src/net/upnp_control.h`(零 windows.h):9 个 SOAP 动作(SetAVTransportURI/Play/Pause/Stop/Seek/GetTransportInfo/GetPositionInfo/GetMediaInfo/GetProtocolInfo)、**UPnPError 错误码/描述解析**(不静默)、DIDL-Lite 生成/解析(含 sampleFrequency/bitsPerSample/nrAudioChannels)、**渲染器采样率三态取证**(Match/Mismatch/Unknown)、HTTP Range 纯函数、stream token 防目录遍历;
- `src/net/upnp.h`:`UpnpHttpServer`(默认 0.0.0.0:8912,/stream/<token>.wav,Range 200/206/416,访问日志);
- `tools/rtacast.cpp`:--list/--dev/--file/--port/--rate/--bits/--seconds/--dop/--analyze;**DSD 源无 --dop 时明确拒绝偷偷转 PCM**(退出码 1);--analyze 打印供源格式/渲染器声明/是否可能重采样/position 推进;
- 测试:`tests/test_upnp_control.cpp` 全离线(SOAP/DIDL/Range/token/三态);
- ⚠️ 未解决:本机局域网 SSDP 发现 0 台渲染器(如实记录,未伪造);**端到端重采样未物理实测**(已在报告/代码标注;需要真渲染器 + 本机 loopback 采集联动)。

## 3. P1 调制器族(299c5c0 + 修复 6846bb8)

- 四族可切:`dsd`(Butterworth,默认向后兼容)/ `chebyshev`(7 阶 NTF)/ `ec`(误差反馈)/ `ahm`(多电平 6~8 bit);键 `modfamily`/`oobgain`/`ahmbits`,CLI `--mod-family/--list-modulators`,UI 下拉;
- **发现并修复**:Chebyshev-7 在 **DSD256 −60dBFS SNR = −2.3 dB**(低电平崩塌)。根因:Chebyshev 极点极低阻尼(Q≈130)→ 开环误差多项式 P(z) 最高频极点出单位圆(DSD64/128/256 的 max|root| = 1.0141/1.0035/1.0008),小信号下 1-bit 量化器等效增益 kq 从 ~1 飙到 >4 → 闭环极点越界 → 高频极限环折叠进通带。**修法:1-bit + Chebyshev 且阶数≥7 时强制降阶到已验证稳定的 5 阶**(CLI/Web/UI 全链路一致),修复后 −60dBFS SNR **32.3 dB**、空闲音 −87→−115.9 dBFS;
- AHM 位数口径校正(原表 6-bit 与 8-bit 的阶数/抖动口径不同):8-bit 在 DSD256 SNR **128.3 dB**、−60dBFS 74.7 dB、带外 −51.9 dB,全面领先 6-bit;
- 铁律 7:各族 50ms 满幅 0dBFS 与单样本 +5 越界后**稳态过载增量 0、clamped=0、RMS/同号率恢复**;10 分钟 26.46M 样点 0 NaN/0 稳态限幅。

## 4. P2 滤波器变体与命名体系(7dcdc2f → 117a55e)

- 预设 16 → **47 个**,覆盖 poly-sinc(short/long/xl/xla/xtr/…)、gauss(hires-lp/mp/ip/xl/xla/…)、ext2、hb 半带及 `hb-*-2s`、`sinc-{S,M,MG,L,LL}`、`closed-form-{fast,,M,16M}`、minringFIR;每个都是**真实参数**(抽头/过渡带/阻带/窗/相位/单段或两段),不是改标签;
- 新工具 `tools/filter_report.cpp`(--markdown/--csv/--out)+ `docs/滤波器变体对比表.md`(47 变体参数与实测指标);
- 判据避开 docs/滤波器家族纪要.md 的「三个坑」:① 归一化截止统一 `0.5*(1-transition)` 并断言在 [0.35,0.499](防冲激退化);② 相位用**前/后振铃能量比**(线性=1.0000、最小<1、混合居中),不用「峰值前能量/总能量」;③ 倒谱法用**相对峰值对数地板**(peakMag*1e-15)避免高斯深阻带被压平;
- 两段式(-2s)与单段对应变体:100Hz~19kHz 幅度差 ≤0.004 dB、分块逐位一致;47 变体已接入 CLI/Web(/api/filters);
- ⚠️ 未解决:短抽头(≤1024)最小相位受因果截断,阻带交界 ~0.26 dB 纹波(门禁基线 0.30 dB);262k 抽头以上单段滤波器群延迟 ≥8192 样点,离线测量需 ≥16384 样点前导。

## 5. 门禁与文档

- 每阶段都经:build 0 error + `ctest --test-dir build` 全绿 + `check_platform_boundary.ps1` PASS;合并到 main 后统一重跑。
- 本机 verify 11/11(见下)、CI 两个 workflow 对每次 push 全绿(1d34b7a→b5cca73→117a55e 逐步验证)。
- 新增文档:`docs/滤波器变体对比表.md`;更新:`docs/耳机EQ-OPRA调研与设计.md`(决策)、`HANDOVER.md`。

## 6. 1.1.0 正式包(本轮交付物,2026-10-23)

- 版本号 `src/version.h` + `make_dist.ps1` 默认版本 → **1.1.0**;新 CLI 入包:`rtacapture.exe`(采集)、`rtacast.exe`(UPnP 投放)、`filter_report.exe`(滤波器对比);
- 产物:`dist\RLPlayer-1.1.0-win64.zip` = **388,337,496 字节(370.35 MB)**,sha256 `BCE7B79E19F0FA275253B75D5F2D96E43D1DE32D39143ABD4BD2257FF699EEB9`;
- 包内验证:三个新 CLI 可执行(`rtacapture --list` 枚举 46 个端点);`data/opra_index.json` + `OPRA-NOTICE.txt` 在包内;
  bundled 启动 `STARTUP v1.1.0` + `已选用 包内固定版`;`GET /api/input/devices`、`GET /api/opra/index` 均 200;
- 审计:Locales 仅 2 pak、ffmpeg/ffprobe 12.18/11.97 LB、无 conf/log/webview2/profile/真实 sk-;8899 全程无监听;
- CI 两个 workflow 对 117a55e / 02e93bd 全绿;GitHub Release **v1.1.0** 已建(附件 digest 与本地一致)。

# 第二十三轮:ROON/元数据 P3 资料层（②直连层 ③检索层 ④富页）+ 耳机 EQ(OPRA)(2026-10-23)

> 用户指令:无人值守连续开发,Lead 只核验/派单,编码全派 Gemini 3.8 Flash(high)子代理;并行用独立 worktree。
> 本轮结果:P3 资料层三个里程碑落地(②直连层/③检索层/④a 艺术家资料页/④b AI 综合+版本推荐),
> **二轮开发目标「耳机 EQ 适配 OPRA」同步完成**;ctest 46 → **50 套**;所有网络抓取必带来源 URL + 取回时间,
> 引用只做复制芯片(无外链),缺资料=内置空态卡,断网=缓存回退。

## 1. P3② 直连层:实体 → 权威文本材料(0180f58)

- 新模块 `src/meta/knowledge.h`(902 行,header-only,零 windows.h):`KnowledgeMaterial{source,kind,subject,title,text,url,lang,fetched_at,expires_at,authority}` / `KnowledgeResult{materials,errors,from_cache,offline}`;
- 源:维基百科 zh/en(MediaWiki `opensearch` 消歧 + `extracts` 正文)、萌娘百科(`zh.moegirl.org.cn/api.php`;`mobile.moegirl` 实测被 Cloudflare 拦 404,已切换)、Discogs/Last.fm 描述符(**缺 key 快速失败、零伪造**);
- **「不猜」消歧义**:候选里没有精确/归一化匹配时产出 `kind=disambiguation` 候选清单或报错,绝不硬取第一个;
- 礼貌:令牌桶 1 req/s + 连续 3 次失败熔断 30s;显式 User-Agent;
- 缓存:`<metabase>/entities/<entity_id>/knowledge.json`,原子写(tmp+rename),TTL 30 天;**过期/断网仍可离线回退**;`--offline` 可强制走缓存;
- CLI:`rtatag knowledge --kind person --name ZARD [--refresh|--offline|--json]`;
- 验收(真网 5 位艺术家,每条 url+fetched_at 齐全):ZARD(维基+萌娘 2 条)、初音未来(2 条)、澤田なつ(繁简重定向 2 条)、塞壬唱片(萌娘独占 1 条)、Toe(band)(英文维基 1 条);二次查询 `from_cache=是` 0.00s;断网 `--offline` 回退 `offline=是` 材料完整;
- 门禁:build 0 error、ctest **47/47**、平台边界 PASS。

## 2. P3③ 检索层:DeepSeek search/fetch + 引用纪律(f679d7b)

- 新模块 `src/meta/retrieval.h`:search 结果过滤(**rerank≥1.5、authority≥2、域名白名单含子域精确匹配、URL 去重、上限 8**)、综合 prompt(只依据材料、强制 JSON、每段≥1 引用)、**五道引用硬门**(①JSON 可解析 ②每段 citations≥1 ③citation id 无悬空 ④url 必须 ∈ [L2 材料 ∪ 通过过滤的检索] ⑤url 非空;任一段违规整份拒绝,不许部分接受);
- 编排:L2 knowledge → 1 次 ds_search → 过滤 → 2~3 次 ds_fetch → 1 次 flash 综合;缓存 `summary-<lang>.json` TTL 7 天;每实体 ≤ 1 search+3 fetch+1 chat;5 QPS 限速;
- CLI:`rtatag summary --kind person --name ZARD [--refresh|--json]`;
- 验收(真网 ZARD):9.55s 生成 3 节 6 段、**6/6 段落均有引用**、2 条引用 url 齐全、引用校验通过;二次 0.00s 命中缓存;
- **幻觉抽检 5/5 无硬伤**:5 条 AI 段落逐条对照引用原文(ZARD 组合性质/经纪公司/百万单曲/9 张连续百万专辑/3800 万销量),字句与数据全部可在材料中找到;
- 门禁:build 0 error、ctest **48/48**、平台边界 PASS。

## 3. P3④a 艺术家资料页(5eb2d14 → merge 444fefa)

- 端点 `GET /api/entity/knowledge?kind&name&refresh`(只读,契约 §2 JSON;空/失败 HTTP 200 + errors);
- UI:点艺术家卡 → 资料页(头像/名称 → 简介 L2 原文长文折叠 → 风格与相似 → 资料来源);来源徽章(维基/萌娘)+ 取回日期 + **「复制链接」芯片**(clipboard + toast,无外链);空态卡(原因 + 重试);缓存标注「缓存 · YYYY-MM-DD 取回」;
- uicheck:资料页可进入/头部/双分支(数据 vs 空态)/来源芯片/复制交互/**全页 `a[href^=http]`=0**/无 JS 异常;
- 截图两张(空态 + ZARD 数据页,1570x905),Lead 视觉复核通过。

## 4. 二轮开发目标:耳机 EQ 适配 OPRA(49ffc36 → merge 195dab9)

**调研(实测)**:OPRA = Roon 发起开源项目,714 厂商 / 6,230 款耳机 / 12,594 条预设;免 key(限流 100/60s);全库 dump `opra.roonlabs.net/database_v1.jsonl`(12.6MB);数据为 biquad(preamp + peak_dip/low_shelf/high_shelf 带 Q);代码 MIT、**数据 CC BY-SA 4.0(允许随包缓存/分发,需署名)**。调研文档 `docs/耳机EQ-OPRA调研与设计.md`。
**决策(无人值守授权拍板)**:①轻量索引(~312KB,不打包 12.6MB 全库)+ 按需抓取缓存 `profiles/opra/`;②preamp 进 `ChannelProcess::gainDb`(不扣主音量);③设置页 CC BY-SA 归属文字 + 复制链接芯片。
**实现**:`src/dsp/opra.h`(索引过滤/100 per 60s 令牌桶/按需抓取/AutoEq 文本转换/永久缓存/离线回退)、`biquad_design::lowShelfQ/highShelfQ`(RBJ α=sinω0/2Q,旧 S=0.7 保留兼容)、`eq_text.h` LS/HS 带 Q 解析 + 奈奎斯特钳位、`/api/opra/{index,presets,apply}`、设置页「耳机 EQ（OPRA）」小节(6,230 款本地实时搜索/预设/应用)、`rtaplay` 与 `rtaweb` preamp 行为统一。
**门禁**:ctest **48/48**(新增 opra_eq);3 款真实样本(HD600/AirPods Pro 2/WH-1000XM5)AutoEq 往返频响 **maxErr=0.000000 dB**(门槛 0.15);架式 Q 语义生效(80Hz 处 Q=1.5 vs S=0.7 差 3.205dB);**满幅 0dBFS 经 HD600 链 +5.5dB 提升 → 无 preamp 峰值 1.32(+2.38dBFS)会削波,有 preamp(-9.3dB)峰值 0.4508(-6.92dBFS)**,断言成立;uicheck 通过(8911)。

## 5. P3④b AI 综合区 + Discogs 版本推荐(d945e89 → merge 08c8174)

- 端点:`GET /api/entity/summary`(接 `retrieval_fetch`;`metasummary` 默认 0——**不自动调用 DeepSeek**,有缓存直读,界面按钮显式生成)与 `GET /api/entity/versions`(接 `discogs_versions`);
- `src/meta/discogs.h`(877 行,零 windows.h):匿名 Discogs `/database/search` + `/masters/{id}/versions`(**实测免 token 可用**);令牌桶 25 req/min + 429 熔断;标题解析(清消歧义后缀、拆作曲家/乐团/指挥);缓存 `entities/<id>/versions.json` TTL 7 天原子写 + 离线回退;`complete = 条数>=5 且每条 url 非空`;
- UI:艺术家页「AI 综合」小节(有缓存:常显**「AI 生成」徽章** + 上标引用序号 + 来源复制芯片 + disclaimer;无缓存:空态卡 + 生成按钮(未开启时禁用并提示));左导航新增「作品」→ 作品检索 + **版本表格**(年份/演奏/厂牌/格式/国家/复制链接芯片,不足 5 条如实提示);设置页元数据组新增「允许 AI 资料综合(DeepSeek,按次计费)」开关;
- 真网验收:`/api/entity/summary` ZARD → 3 节 6 段**全部带引用**、2 条 citations(维基+萌娘,url+fetched_at 齐全);**3 部古典作品各 10 条可溯源 Discogs 版本,complete=true**:
  - Boléro(Ravel):release/8858093、14226050、35612848、10708531…
  - Beethoven Symphony No.5:release/14236188、19542469、21135013、5665552…
  - Mahler Symphony No.5:release/9929933、4643954、19634065、22975103…
- 门禁:build 0 error、ctest **50/50**(新增 discogs)、uicheck 全过。

## 6. P3 整体验收(2026-10-23,用户口径见 docs/元数据补全调研与规划.md §4 P3 行)

| 验收项 | 结果 | 证据 |
|---|---|---|
| 5 位艺术家资料页全带可复制来源 | ✅ | ② 真网 5 位(ZARD/初音未来/澤田なつ/塞壬唱片/Toe)每条 url+fetched_at;④a 页面来源徽章 + 复制链接芯片;二次查询 0.00s 命中缓存 |
| 3 部古典作品版本页 ≥5 个可溯源演绎 | ✅(附注) | Boléro / Beethoven Sym5 / Mahler Sym5 各 **10 条** Discogs release URL,complete=true;**附注:单位是「Discogs 发行版本」,同录音的多版复刻可能重复计入,未按「演绎」去重**(见 §7-4) |
| 幻觉抽检零硬伤 | ✅ | ③ 对 ZARD 摘要 5 段逐条对照引用原文,5/5 属实;④b 同源摘要 6 段引用校验通过 |
| 断网显示缓存 | ✅ | ② `--offline` 实测 from_cache=是 offline=是 材料完整;④a 页面显示「缓存 · 日期」;summary TTL 7 天离线回退 |
| 门禁 | ✅ | ctest **50/50**、平台边界 PASS、uicheck **连续 3 次全过**、**本机 verify 11/11(Fail 0)** |

**uicheck 假红修复(同轮)**:AI 综合区断言改为「有摘要/无摘要」双状态分支(有摘要断言徽章+上标+芯片;无摘要断言空态卡+提示+禁用按钮,不许跳过),并把设置页/OPRA/保存验证的固定 sleep 全部收敛为 `waitFor` 轮询——本轮两次「UI 自检 3~5 项失败」都是这类假红,已消除。

## 8. 1.0.10 正式包(本版交付物,2026-10-23)

- 版本号真相源 `src/version.h` bump 到 `1.0.10`;`tools/make_dist.ps1` 默认版本同步;
- **修掉一个打包级缺口**:OPRA 索引与缓存原来按**进程 cwd** 找 `data/opra_index.json`,包内又没有 data/ -> 正式包 OPRA 会空。
  修复:OpraService 增加 baseDir(由 rtaweb 用 `path_in_exe_dir` 传 exe 根),索引/缓存以 exe 目录为准、cwd 仅作后备;
  `make_dist` 复制 `data/opra_index.json`(312.4KB)与 `OPRA-NOTICE.txt`(CC BY-SA 4.0 归属)进包,**缺件拒绝出包**;
- 产物:`dist\RLPlayer-1.0.10-win64.zip` = **387,644,214 字节(369.69 MB)**,sha256 `F87BC60D6903634268F1C110A53BF51620138B3E26CA29BF2113951984FD7D53`;
- 验证:ctest **50/50**;**cwd 无关性实测**(cwd=包目录 与 cwd=%TEMP% 两次 `GET /api/opra/index` 均返回 6230 条);
  bundled 模式启动 `STARTUP v1.0.10` + `已选用 包内固定版`;8899 全程无监听;包内 Locales 仅 2 pak、ffmpeg/ffprobe 12.18/11.97MB、
  zip 内 data 索引与 NOTICE 存在、无 conf/log/profile/sk- 泄漏;
- GitHub Release **v1.0.10** 已建(附件 digest 与本地 sha256 一致);CI(acc0bc9)两个 workflow success。

## 7. 本轮已知未解决项(铁律 4)

1. ~~uicheck 固定 sleep 在重负载下会 flake~~ **已修(ea1de81)**:AI 综合区改为双状态分支断言;sleep 全部收敛为 waitFor;连续 3 次全过。
2. 维基消歧义:跨领域同名实体(非(消歧义)页)可能直接命中第一个词条;③ 层用材料内容兜底但未做语义裁决。
3. **MusicBrainz 本机当前不可达**(直连与 7897 代理均超时),L4 版本推荐改用 **Discogs 匿名 API**(实测 200);Last.fm 无 key 未接入。
4. Discogs 匿名限频约 25 req/min;版本枚举在冷门作品上可能不足 5 条(如实 `complete=false`)。
5. 「风格与相似」字段:维基/萌娘直连主要产出生平,genre/similar 待 L3 或扩展源补。
6. OPRA 预设目前按双声道共用铺开;若官方提供分声道曲线需扩展。

# 第二十二轮:基建收口(P3 双轨 WebView2 合并 / P1 裁 ffmpeg / P2 裁 Locales / 1.0.9 出包)(2026-10-23)

> 用户指令:本轮只做核验与派单,编码全部派 Gemini 3.8 Flash(思考强度 high)子代理;按 P3 合并 → P1 收口 → P2 实施 → 出 1.0.9 → 更新文档。
> 本轮结果:**P3/P1/P2 全部合入 main**;ctest 45 → **46 套**(新增 ffmpeg_matrix);**1.0.9 正式包 369.4 MB**(1.0.8 是 555.7 MB,−33.5%);修掉一个 **CI 偶发红**与一个**产品级重试缺陷**。

## 0. 子代理模型核验(用户钦定:Gemini 3.8 Flash / high)

- 探针子代理自报 `MODEL=gemini-3.8-flash-tiered / EFFORT=high / PROVIDER=agy`;
- 独立复核:会话记录 `modelSelection.lastUsed = {provider: agy, model: gemini-3.8-flash-tiered, reasoningEffort: high}`;与探针自报一致。
- 注意:上一会话遗留的 P1 子代理仍是 deepseek-flash(无法中途换模型/中止),其实现由本轮新派的 Gemini 子代理复核、跑门禁并提交。

## 1. P3:WebView2 双轨运行时(合并 71beda7 → a882bab)

- `tools/wv2_host.h` +408/−17:读系统 Evergreen 版本(EdgeUpdate 注册表 pv,回退扫 Application\\<ver>\\msedgewebview2.exe 文件版本)与包内固定版版本,逐段比较(短的一方补 0);
- **auto(默认)**:系统版 ≥ 包内版 → 系统版(能跟随安全更新),否则包内版(自包含兜底);首选失败自动改试另一条;两条都失败时**两条原因都写日志与错误文本**;
- `RLPLAYER_WEBVIEW2_MODE=auto|system|bundled` 可强制单轨(排障/验收);
- 每次选轨向 launcher.log 写一行「用了哪条 + 版本 + 原因」;
- 审查证据:合并前分支自带 `build/_wv2test/guard_test` 10 个用例(同版本/系统旧/系统新/版本读不出/单轨/强制)、三模式截图同 sha256、干净重建 0 error、ctest 45/45。
- **取舍决定(交接 §9.1)**:同版本时**保持选系统版**(`c >= 0`)。理由:同 build 行为一致、可跟随后续安全更新;包内版作为自包含兜底仍在。若要改选包内版,把 `>= 0` 改成 `> 0` 一行即可。**未擅自改。**

## 2. 修 CI 偶发红:decode_retry 时序竞态(产品级缺陷,836dacf)

- 现场:push a882bab 后 CI 首次红,唯一失败 `decode_retry` 用例 3(ctest 44/45);同一 commit 重跑即绿 ⟹ 偶发。
- 根因(`src/audio/audio_loader.h` 重试循环):失败后探针发现「此刻可读」就 `break`,只尝试 1 次。但**占用方可能恰在 ffmpeg 运行期间松手**——「此刻可读」恰恰证明刚解除,应当再试;旧判据会把本可恢复的瞬时占用直接报成失败(现场「重试一下就好」的真痛点)。云机 ffmpeg 启动+ANSI 回退 >150ms,正好踩中。
- 修法:`if (i >= 1 && unreadable.empty()) break;` ——**首轮失败必须至少再试一次**;只有已经重试过且探针仍证明可读,才认定是真解码错误。
- 测试硬化(不弱化判据):用例 3 拆两时序(10ms 极早释放 / 150ms 跨尝试释放);新增 3b 用「监听 ffmpeg 子进程启动即释放锁」做**时序解耦的确定性复现** + 10 轮递增时延窗口扫描(15~195ms),断言 10/10 全过。
- 门禁:repeat-until-fail:10 全绿;全量 ctest 45/45;平台边界 PASS。

## 3. P1:自建精简版 ffmpeg/ffprobe(b33169f → 合并 f49462c)

| 项 | 1.0.8(gyan full) | 1.0.9(自建精简版) | 变化 |
|---|---|---|---|
| ffmpeg.exe | 212.0 MB | **12.18 MB** | −94.3% |
| ffprobe.exe | 211.8 MB | **11.97 MB** | −94.4% |
| 音频解码器 | 227(gyan 口径) | **219(源码音频全集口径)+ mjpeg/png/bmp/gif 封面** | 不降 |
| 运行时依赖 | 无(静态) | 无(只依赖系统 DLL;裸 PATH=System32 实测) | 持平 |

- 交付: `tools/build_ffmpeg_lite.ps1`(MSYS2 base + winlibs GCC 16.2 + nasm,缓存 `build/_fflite`,从零可复现:实测 00:43→00:46 重建成功)、`tools/ffmpeg_lite_required.txt`(50 项必需音频解码器**运行时名**白名单,修掉 gyan 名与源码名不一致的坑)、`tools/verify_ffmpeg_lite.ps1`(三层守卫:裸环境可执行 + 白名单 + 产品路径格式矩阵)、`tests/test_ffmpeg_matrix.cpp`(35 断言,把精简版与测试自身复制到临时目录,确保子进程**一定**用精简版)、`make_dist.ps1` 出包前调守卫(不过即拒绝出包)。
- 证据:守卫绿「白名单 50 项齐全 / 解码器共 219 个 / pass=35 fail=0」;红→绿演练过(临时换成缺解码器构建 → 守卫红 → 还原绿);ctest 46/46(新增 ffmpeg_matrix 3.78s)。
- ⚠️ 未做到(详见 `P1-调查.md` §9):ape/mpc7/mpc8/tak/shorten/dst 无真实样本(只有解码器存在断言);DSF/DFF 是自造 1-bit 流(无 ffmpeg DSD 编码器);精简版**无编码器**(产品只用解码+pcm/png 编码);工具链 URL 用 latest 别名未钉 SHA256;Win7 的 UCRT 依赖未验证。

## 4. P2:裁 WebView2 Locales(4341fed)

- `make_dist.ps1` 复制 WebView2 运行时后:Locales 只留 `en-US.pak`(Chromium 强制根语言回退)+`zh-CN.pak`;守卫:en-US 必须存在、.pak 数必须恰为 2,否则拒绝出包;打印裁剪前后体积。
- 实测:Locales **168 个 pak / 128.88 MB → 2 个 / 1.79 MB**(未压缩 −127.09 MB,−98.6%)。
- **启动实测(8910 接管法,全程未碰 8899)**:
  - 先把手动 `rtaweb.exe --port 8910` 起在测试包内,再启动 `RLPlayer.exe` → 日志「发现已有实例在 8910,接管界面」→ **不会自己 spawn 服务占用 8899**;
  - bundled 模式:窗口 RLPlayerMain 可见,日志「已选用 包内固定版」,**新拉起 7 个 msedgewebview2 全部来自包内 `WebView2Runtime\\msedgewebview2.exe`**(裁剪后运行时真的被用上的硬证据);关闭后全部自动退出、无残留;
  - auto 模式回归:日志「已选用 系统版」、6 个 webview2 来自 Program Files,窗口正常;
  - 8899 在实验前后全程 FREE。
- **EBWebView 取证(推翻交接 §9.2 的「死重」怀疑)**:把 `WebView2Runtime\\EBWebView` 改名后再 bundled 启动 → `CreateCoreWebView2EnvironmentWithOptions` 返回 **0x80070002**、窗口建不出来、退回 Edge。**结论:EBWebView 不可删**(它含 x64/x86 下的 EmbeddedBrowserWebView.dll 等运行时模块,不是纯种子 profile)。已还原;1.0.9 包内保留。

## 5. 1.0.9 正式包(本轮交付物)

- 出处:main(含 P3+P1+P2+decode 修复),版本号真相源 `src/version.h` bump 到 `1.0.9`,
  `powershell -File tools\\make_dist.ps1 -Version 1.0.9`(内部依次:构建 → GPU 后端 → 兼容门禁 → **ffmpeg 精简版守卫** → 复制 → **Locales 裁剪+守卫** → GPU 裸 PATH smoke → zip → sha256)。
- 体积:**369.4 MB(387,305,245 字节)**,1.0.8 的 555.7 MB 上 **−186.3 MB(−33.5%)**;构成变化:ffmpeg 两项 −150 MB 量级、Locales −35~40 MB(zip 内)。
- 审计:包内 Locales 仅 2 个 pak;ffmpeg/ffprobe 12.18/11.97 MB;无 `rlplayer-llm.conf`、无 `*.log`、无 `webview2\\` profile;无真实 `sk-` 密钥;出包后解包目录保持未启动干净状态。
- 最终产物: `dist\\RLPlayer-1.0.9-win64.zip` = **387,305,245 字节(369.4 MB)**,
  sha256 `831FE3462B0137FF0247C4BDDEE936FA4D169338A2BD1CE3C54FDEC61A308569`;
  启动横幅 `STARTUP v1.0.9` 实测(8910 接管法,8899 全程未碰);包内 README.txt 已替换为 1.0.9。

## 6. 本轮已知未解决项(铁律 4)

1. P3 的版本比较逻辑**只有本地 `build/_wv2test/guard_test` 证据,未固化成仓库内 ctest 用例**;后续可把它提升为 `tests/test_wv2_version.cpp` + CMake 目标(纯函数,无需 WebView2)。
2. EBWebView 不可删,但其中确实含一批**种子 profile 文件**(History/Login Data 等空壳)——是否能「只删种子、保留 x64/x86 运行时」未验证;风险高,本轮不动。
3. 精简版 ffmpeg 的冷门格式(ape/mpc/tak/shorten/dst)只有解码器存在断言,无真实样本解码。
4. `writeback` 用例在素材缺失时会 SEGFAULT(测试自身健壮性,非产品崩溃),本轮未修。
5. decode_retry 在 CI 上仍依赖 ffmpeg 子进程启动时序;已用确定性用例覆盖,但云机极端负载下仍可能变慢(不影响判据)。

# 第二十一轮:用户反馈双通道首次启用(2 份 BUG + 1 份管理员需求,共 11 条)(2026-10-22)

> 用户指令:先把「反馈和开发」目录规范写进开发规范,再处理两份反馈。**主管只做验收与回测,编码全部派子代理**。
> 本轮结果:两条通道首次跑通;新建 **铁律 8**;处理 **3 份反馈共 11 条**(debug 2 份 + develop 1 份),全部改名 `（完成）`;
> ctest 43 → **45 套**,新增 2 套门禁(`decode_retry` / `zipstore`)。

## 0. 铁律 8:反馈与开发需求目录(本轮新增,已写入 AGENTS.md)

| 通道 | 位置 | 命名 | 完成标志 |
|---|---|---|---|
| 用户反馈 BUG | `反馈和开发\debug\` | 文件夹 `YYYYMMDD-NN` | 改名 `…（完成）` |
| 管理员开发反馈 | `反馈和开发\develop\` | 文本 `YYMMDD-NN.txt` | 改名 `YYMMDD-NN（完成）.txt` |
| 长期方向备忘 | `反馈和开发\二轮开发目标\` | — | 不逐条完成,只作方向 |

硬规矩:先读全再动手(引用行号) · 区分现象与根因 · 不许假装修好 · **改名是最后一步** · 绝不改用户反馈原件 · 全角括号一律 `-LiteralPath`。

## 1. debug/20261022-01(测试机 VM,1.0.7)→ 已改名(完成)

**三个现象,结论各不相同(本轮最重要的「别把现象当根因」案例):**

### 1.1 现象 A:RENDERFAIL → 自动回退共享 —— **1.0.6 的正常工作,未动**

L8 `RENDERFAIL 4.02s w0=201 wr=0 last_pad=4416` → L9 `FALLBACK_SHARED` → L12 `PLAY outRate=48000`,
回退后连播 196 s(L13~L23 十一条 STAT,wr 239→2457)。

**顺带纠正主管自己的错误前提**:主管以为「设备独占不支持 44100,所以该走 RATEFALLBACK」。
实测 diag `rates16 = 44100/48000/88200/96000/176400/192000` —— **44100 本来就在 16-bit 独占支持列表里**。
因此 `rtaweb.cpp:1393-1400` 在 16-bit 候选就命中,effBits 非 0 ⟹ `:1401` 的自适应块**根本不该进入**。
**这是正确行为**(设备原生支持 44100,换率反而平白引入重采样),不是漏触发。

### 1.2 现象 B(真 BUG):ffmpeg `Illegal byte sequence` / 退出码 **-42**

同一文件 L2/L5 **成功过**,L55/57/59/61 **连续四次失败**、临时 WAV 恒 0 字节 ⟹ 间歇性。

**根因分两层,第一层是实测出的反转:**

1. **`-42` 是 ffmpeg 的「掩码」,不是真实原因。**
   实测规则:**ffmpeg 打开输入失败后,会把 UTF-8 路径字节按 ANSI 代码页(本机 CP936)重解释;
   重解释非法 → 报 EILSEQ(-42),恰好「合法」→ 才如实报 ENOENT(-2)。**
   决定性对照(主管独立复现):

   | 场景 | exit | 文本 |
   |---|---|---|
   | ASCII 名 + 文件被独占 | **-13** | Permission denied |
   | **日文名 + 文件被独占** | **-42** | **Illegal byte sequence** |
   | 两种名字 + 文件存在可读 | 0 | (无) |

   同一故障、只换文件名就换了错误码 ⟹ 日志里的「编码错误」**完全是假象**。
2. **触发条件是「源文件此刻读不了」**(瞬时):最可能是 `writeback.h:414→:432` 两次 MoveFileEx 之间的
   原文件不存在窗口,或同步/杀毒短暂独占。四次集中在 34 s 内、几分钟后自愈,符合瞬时特征。

**修复(`src/audio/audio_loader.h`,4 处)**:① 新增 `source_read_probe()` 向 Win32 问真实原因(共享冲突/不存在/拒绝访问);
② 解码失败重试 3 次、退避 120/240 ms,**判据:若源文件此刻读得了就立刻停止重试**(不把真解码错误拖成无效重试);
③ `-42` 错误文本重写明示「不代表文件真有编码问题」+ 真实 Win32 原因;④ 补查 `GetTempPathW`/`GetTempFileNameW` 返回值(原来没检查)。

**门禁 `tests/test_decode_retry.cpp`(6 组用例)**:日文名正常解码 · 独占时错误文本带出 Win32 真实原因且保留 -42 ·
**瞬时占用 150 ms 后释放 → 自动重试成功**(现场「重试就好」的形态) · 文件不存在不无脑重试 · 并发 2×25=**50/50** · 长路径 276 字符+日文+空格 ×**30/30**。

**已排除的方向**(附证据):临时 WAV 8.3 短名 / `run_hidden` 管道死锁(顺序本身是对的)/
并发抢 tmp 名(`GetTempFileNameW(uUnique=0)` 原子唯一,50/50 成功)/ ACP 传参(`widen` 走 CP_UTF8→UTF-16,全程不经 ACP)/ `-y` 竞争。
主管另做的否证:顺序解码 **50/50**、8 进程并发 **80/80** 全成功 ⟹ 静默常态不是 bug,故障需「文件瞬时读不了」这个额外条件。

### 1.3 现象 D:`END pos=0 error=""` —— **正常「切歌打断」,不是静默死亡**

L62 START → 534 ms → L63 END(pos=0,error="") → **5 ms** → L64 重新 START 且 L66 PLAY 成功。
`/api/play` 每次都 `ps.stop.store(true); joinIfDone()`,`play_one_impl` 被打断时 `ps.error` 保持空 ⟹ 正是此形态。
**反证**:真解码失败(L55-61)都是 `error="异常: …"` 非空。
⚠️ 但仍缺一行显式归因(`PLAY_ABORT`),已记为待补可观测性项。

## 2. debug/20261022-02(**用户真机**,EDIFIER N300)→ 已改名(完成)

用户原话:**「破音这个问题还是没解决,边播边啵啵啵」**。

### 2.1 定量结论:不是削波,是**吞吐只有 0.39 倍实时**

| 量 | 算法 | 结果 |
|---|---|---|
| 墙钟 | PLAY 19:21:46.157 → STAT 19:21:56.704 | **10.547 s** |
| 送出的音频 | `pos=4.1` @96k | **393,600 帧** |
| 吞吐 | 4.1 / 10.547 | **0.389** |
| 尝试/成功 | w0=205 / wr=41 | 246 次,成功 **16.7%** |
| 每次成功 | 393600 / 41 | **9600 帧 = 整缓冲** ⟹ 与 pos=4.1 精确吻合 |
| 「啵」节拍 | 41 / 10.547 | **≈3.9 次/秒(每 257 ms)**:100 ms 有音乐 + 157 ms 空 |

⟹ **10.55 s 里有 6.45 s 设备根本没数据可放**。这是「供不上(欠载/丢样)」,不是「波形被削顶」。

### 2.2 为什么旧检测器完全看不见(**真实盲区**)

写入成功即 `timeouts = 0`(`render_loop.h:310`);要触发 RENDERFAIL 需**连续 201 次**失败 ≈ 4.02 s。
现场每 257 ms 就有一次成功 ⟹ **永远攒不到 200** ⟹ 不归因、不回退、不报错。
**「间歇性成功」把超时检测彻底废掉了。**

### 2.3 修复:STARVING 吞吐率判据 + 「写不进去」三类归因

- **新判据**(按「帧/秒 ÷ deviceRate」,非写入次数比):窗口 1 s、阈值 0.90、连续 3 s 判 STARVING;
  首窗只建基线(防起播误报);暂停/seek 均清零窗口;`starveEnabled=false` 时与旧版行为一致。
  选型反证:健康独占设备 duty 本来只有 ~20%(20 ms 轮询 / 100 ms 缓冲),拿 duty 当门限会**把所有健康设备判死**。
- **w0 拆分**:原来一个 w0 混着「设备缓冲满」与「我们喂不上」两种互斥成因,处置完全相反。
  新增 `ZeroAttrib` 分类器(① `pad=full` ② `avail<period` ③ `other`)。
- **接线补正(主管复核抓到的「接了电但没通电」)**:分类器实现了,但 `rtaweb.cpp` 的 `WasapiSink` **没转发**
  → `OutSink` 默认返回 -1 → STAT 的 `if (zp >= 0 || zs >= 0)` 闸门永假 → **用户下一份日志仍看不到**。
  已补 3 个 override。**判定生效的唯一凭据:下一份日志 STAT 里出现 `zp=`/`zs=`。**

### 2.4 削波线的否证(逐条)

`clip=0`(累计)· `ispPeak=1.09993`(+0.827 dBFS)经 `preAtten=-1 dB` 衰减后 = **0.98031 < 1.0** · `srcPeak` 衰减后 0.881。
⟹ **削波不是主因**;「破音」是周期性断音造成的爆音/咔哒,不是波形削顶。

### 2.5 与第十/十一轮的关系(用户说「还是没解决」)

| 轮次 | 修的是什么 | 本次是否同一形态 |
|---|---|---|
| 第十轮 | LNS15 锁死 / DSD 满幅锁死 | **不是**(dsd=0、clip=0、peak=0.486) |
| 第十一轮 | `write_one_period` 只在 pad==0 时写 → 新增 `write_available` | 第十一轮「未解决项」写的就是「需下一份 STAT 验证」;**这份就是那份 STAT**:w0 仍≈5×wr、吞吐 0.39 |
| 本轮 | 旧判据对**间歇性成功**无效 | **第三种形态:检测器被间歇成功废掉** |

### 2.6 主管复核抓到的两处**数字错误**(已修)

- `render_loop.h` 注释原写「每 ~120 ms 一次」「吞吐 83%」——**两处都错**:
  83% 是**写入失败率**(205/246);真实吞吐 **0.39**;实测「啵」间隔 **257 ms**。
- `test_renderfail.cpp` 注释原写「与现场 5/6 形态吻合(0.83)」——**误读**:mock 造的是 0.833;
  现场 0.39;现场的 `w0:wr=5:1` 是**计数比**不是时间比(246×20 ms≈4.92 s ≪ 墙钟 10.55 s)。
  ⟹ **失败率与吞吐率量纲不同、不可混用 —— 这正是判据必须用「帧/秒 ÷ deviceRate」的原因。**

## 3. develop/261022-01.txt(管理员 7 条)→ 已改名 261022-01（完成）.txt

| # | 条目 | 结果 | 关键证据 |
|---|---|---|---|
| UI1 | 跳转界面白边框 | ✅ | **挖出第三层根因**:`WM_ERASEBKGND` 的 `wParam == -1` 被当成 HDC 传给 `FillRect` → 静默失败 → 5px 边带**从来没被画过**。修法:`repaint_band()` 在 WM_NCPAINT/WM_PAINT/WM_ACTIVATE…/WM_SIZE 各时机补刷 + `verify_band()` 像素自愈。15 张逐像素抓图,三主题 0..7 无 FFFFFF |
| UI2 | 单曲页缺循环/音量 | ✅ | `#npRep`/`#npVol`,与底栏**共用同一真相源**;点 npRep:sRep 1→2;拖 npVol −12.5 dB 双镜像同步 |
| UI3 | 时长行「/」对齐播放键 | ✅ | 3 列网格 + 统一 `--npctl-w`;DOM 中心差 **0.000 px**,真实窗口像素差 **0.0 px** |
| 功能1 | 默认应为声卡当前采样率 | ✅ | 新增 `rate=follow`(`query_mix_format` 取系统率)作首项;**老用户迁移**:仅 conf 无 rate 键才用 follow,旧 `rate=max` 行为不变 |
| 功能2 | 「跟随预设」语义不明 | ✅ | 设置页实时显示「当前 1x 滤波器预设相位 + 实际覆盖值」 |
| 功能3 | 诊断改为一个压缩包 + 独立页签 | ✅ | 自研 `zipstore.h`(纯 C++ STORE,**否决 pwsh Compress-Archive**:用户机器不保证有 shell);`/api/diagzip`;独立「诊断」页签 |
| 功能4 | 元数据缺 DeepSeek key 输入 | ✅ | 新增 `/api/llm`(GET 只回掩码 / POST 合并写保留 proxy= 行 / test 测连接);key 只进 `rlplayer-llm.conf`,**不进源码不进包** |

**主管独立端到端验收(功能3)**:起 8911 → `GET /api/diagzip` → **74,886 字节、魔数 `504B0304`** →
`Expand-Archive` 正常解开 → 6 个条目(diag.json / playback.log / launcher.log / dumps-manifest.txt /
settings-summary.txt / README.txt)→ **不含 rlplayer-llm.conf** · **完整 `sk-` 命中 0** · **未脱敏 api_key= 命中 0**。

## 4. 门禁与清理(主管亲自复跑,不采信子代理自报)

| 项 | 结果 |
|---|---|
| `cmake --build build` | **0 error** |
| `ctest` | **45/45 全绿**(43 → +decode_retry +zipstore) |
| `verify.ps1` | **11/11 PASS,FAIL count 0** |
| uicheck | **95 项 PASS / 0 FAIL** |
| ZIP 端到端 | 见 §3(真解压 + 无 key) |
| 原件完整性 | 8 个用户反馈文件**字节数与改名前逐一相同** |
| MUSIC | **今日 0 文件被改** |
| 端口/残留 | 8899/8910/8911 全部空闲,无 rtaweb/RLPlayer 残留 |
| 源码 sk- 泄漏 | **0** |

**清理过程(如实)**:期间有子代理启动的 `build\rtaweb.exe`/`RLPlayer.exe` 实例锁住构建输出,
按铁律 2 **未 force-kill**;确认「exe 路径 == build 副本、父进程已退出、无 dist 实例」后,
用 `tools/restart-servers.ps1 -NoBuild -NoStart`(校验 exe 路径 + 端口属主后才停)清场,再复跑门禁。

## 5. 1.0.8 正式发布版已出包(用户指令「出 1.0.8」)

| 项 | 值 |
|---|---|
| 包 | `dist\RLPlayer-1.0.8-win64.zip` |
| 大小 / 条目 | **555.7 MB / 1195 条**(与 1.0.7 条目数一致) |
| sha256 | `3DE9F9EC4CE2F4DB054DCC8B3F3BB60AC8BA92E631EAC2EE515828AD31BB68DE` |
| 版本源 | `src/version.h` → `1.0.8`(唯一真相源) |
| README | `RLPlayer 1.0.8 正式发布版(Full / 自包含)`(`@VERSION@` 已替换) |
| 自包含 | ffmpeg/ffprobe/rtagpu.dll/amdhip64_7.dll/WebView2Loader.dll/asio_probe.exe/tools **12 件**/WebView2Runtime **全在** |
| 兼容门禁 | FAIL count **0**(5 个二进制 crt=static / subsystem=6.0 / avx512=0) |
| GPU staging smoke | **PASS**(PATH 只剩 System32:`[PASS] GPU 卷积与 CPU 直接卷积一致`) |

**发布前审计(主管亲自跑,不信脚本自报):**

| 检查 | 结果 |
|---|---|
| 包内二进制真实版本 | 启动日志 `STARTUP v1.0.8 build=Oct 22 2026 21:47:58`;`/api/ping` → `{"ok":true,"app":"RLPlayer","version":"1.0.8"}` |
| 独立运行(拷到 %TEMP% 单跑 rtaweb.exe) | **能起、能服务**(HTTP 200 正常返回) |
| 用户配置混入 | `rlplayer-llm.conf` / `rlplayer-settings.conf` **均不在包内** |
| 日志混入 | 非 WebView2 路径下的 `.log` **0 个** |
| 密钥泄漏 | 包内 txt/json 扫 `sk-[A-Za-z0-9]{20,}` → **0 命中** |
| 发布前干净度 | 冒烟全部在 %TEMP% 副本上做,**dist 目录未被污染**(无日志/配置生成) |

⚠️ **已知打包瑕疵继续保留(刻意不修,理由如下)**:`WebView2Runtime\EBWebView` 下 216 个 zip 条目 / 24.9 MB 是
Edge 运行时自带的**种子 profile**。修法是在 `make_dist.ps1:93` 排除该目录。
**本轮刻意不改**:改动 WebView2 的用户数据目录行为,风险落在「用户第一次启动」这个最不该出问题的时刻,
而收益只有 25 MB。等 1.0.8 复测无异常后再单独做这个优化。

## 6. 未解决 / 待办(铁律 4,如实)

1. **20261022-02 不能声称「已修复」**:本机是 TASCAM 不是 EDIFIER N300,无法复现;结论全部来自**日志算术 + 代码审查**。
   需用户装新版本复测。**判定 STARVING 生效的凭据**:日志出现 `STARVING` + STAT 出现 `zp/zs/zr`。
2. **STARVING 的行为代价未验证**:触发即 `renderFail` → 自动回退共享并**从头重播**。
   对这位用户可能是「先啵 4 秒,再跳回开头用共享模式重放」;这个 UX 是否可接受、共享模式是否就不啵,**均未在真机验证**。
   对要求 bit-perfect 的用户属行为变化。
3. **「谁欠载」仍未定**:①设备缓冲满 ②我们喂不上(CPU 4095-tap 卷积/调度) ③其它。
   接线已补,但要等下一份日志的 `zp/zs` 才能定案。
4. **`PLAY_ABORT` 显式归因未加**:20261022-01 §4 与 02 §9-10 都指向同一缺口(`END pos=0 error=""` 只能靠上下文推断)。
5. **功能1 未真开 WASAPI 独占验证「实际出声 44100」**(铁律 1 不放大声音),判据=页面选档 + sysRate 实测 + 离线单测。
6. **UI1 最大化态无法用 CopyFromScreen 严格证明**(Win11 最大化 rect 负原点超出屏幕)。
7. **既有小问题(本轮登记未修)**:UTF-8 BOM 的 `rlplayer-settings.conf` 会让 `/api/settings` 首键带 `\uFEFF`
   → rate 回落 follow;`uicheck` 点 `bRep` 后不还原 repeat(测试自身卫生问题)。
8. **浅色主题边带 F4F6F9 vs 顶栏 FFFFFF(Δ11)**:属「边带=--bg」设计而非白框;若用户仍看到浅灰线,需另立项改为「边带=顶栏色」。

# 第二十轮:P3 资料层启动 + 三个地基缺陷(1.0.7 已出包)(2026-10-22)

> 用户指令:出 1.0.7 → 继续开发(P3)。
> 本轮结果:P3 里程碑①(实体模型)完成并验收;dizzylab 接入完成;另修掉两个**既有地基层缺陷**(都是主管复核时挖出来的)。

## 1. 交付一:1.0.7 正式发布版已出包

| 项 | 值 |
|---|---|
| 包 | `dist\RLPlayer-1.0.7-win64.zip` |
| sha256 | `3CB4B12157AD58D7305CEC04F01724056C566D221E9FC24F3D50B03887FCAE9A` |
| 大小 / 条目 | 555.6 MB / 1195 条 |
| 自包含核验 | ffmpeg/ffprobe/rtagpu.dll/amdhip64_7.dll/WebView2Loader.dll/asio_probe.exe/tools 12 件/WebView2Runtime **全在** |
| 安全核验 | 源码/包内 `sk-` 零命中;非 WebView2 的 log/conf 条目 **0** |
| 门禁 | verify **11/11**、ctest **43/43**、check_compat FAIL 0 |

⚠️ **已知打包瑕疵(1.0.5/1.0.6/1.0.7 均有,非本轮引入)**:`WebView2Runtime\EBWebView` 下 155 文件 24.9 MB 是
Edge 运行时自带**种子 profile**(已与 `C:\Program Files (x86)\Microsoft\EdgeWebView\Application\155.0.4283.62\EBWebView`
逐字节比对一致)——**非本机用户数据,无隐私泄漏**,纯多占 25 MB。修法=打包排除该目录,待下轮。

## 2. 交付二:dizzylab 接入(队列首项,4/6 命中)

### 2.1 前提性纠正:它不是 SPA,无需浏览器抓包

**原队列写着「dizzylab 是 SPA,需浏览器抓包」——该假设被实测证伪**(详见 §1.5)。
- 搜索 `GET /search/?s=<关键词>`:服务端渲染 23340 字节;**参数是 `s`,不是 `q`**(用 q 也回 200 但无结果 —— 当初正是踩此坑误判为 SPA)
- 专辑 `GET /d/<catalog>`:服务端渲染 53538 字节,含完整曲目表 `<li data-id data-audio>`,内部为「序号. 曲名 - 艺术家 (MM:SS)」;实测 NE-06 抽出 11 轨
- ⟹ 纯 HTTP + 健壮 HTML 提取即可,**不需要浏览器**

**教训**:把「参数名猜错」当成「站点是 SPA」,凭空造出一个需浏览器的工作项,并让整条队列首项被错误前提阻塞三轮。
**判据:先看返回体里有没有数据,再谈渲染方式。**

### 2.2 落地

- `provider.h`:新增 dizzylab,注册表**排第一**(同人优先),`canLyric=false`(无歌词接口,如实)
- **`enrich_adopt` 一行未改** —— 门槛仍 confident‖LLM pick≥0‖top≥0.80(宁缺毋错不许因排第一而放宽)
- HTML 提取按稳定结构(作品段 → `/d/<catalog>` 卡片 → 卡片内第一个 h1/h3),**实体反转义**完整(&amp;/&#39;/&#xHH;/十进制),结构缺失优雅降级返回空
- 限频 1 req/s + 连续 3 失败熔断 120s;catalog 白名单挡路径穿越
- `tests/test_dizzylab.cpp` 125 条离线断言(内嵌真实抓取片段作 fixture);真网用例 opt-in(`RTA_NET_TESTS=1`)

### 2.3 6 首拒采曲命中率:**4/6**

| # | 曲目 | 最高分 | 过门槛 |
|---|---|---|---|
| 1 | 1 Pixel Library | 0.9250 | ✅ |
| 3 | 1 Pick me! | 0.9250 | ✅ |
| 6 | 2 Retro Shoujo | 0.9250 | ✅ |
| 7 | 7 終焉の鴉inst. | 0.9250 | ✅ |
| 10 | 03. 未知艺术家 - 音轨03 | — | ❌ 站内确实无此条目 |
| 17 | たいせつなきみのために… | — | ❌ 站内确实无此条目 |

**关键词策略与预设有实质出入(如实)**:enrich 只把 `artist + " " + title` 传给 provider,**不传专辑/社团**;
而 dizzylab 是全词 AND(实测 `Pixel Library` 命中、`Khrisma Pixel Library` 零命中)。
原设想「按专辑名/社团名搜」发不出来 → 改用**后缀放宽阶梯**(全串→末3→末2→末1,最多 3 次)。
真正按目录名搜需扩 ProviderDesc 传目录,属 enrich 侧改动,**未做**。

## 3. 交付三:P3 里程碑① 实体模型(209 断言)

- **`src/meta/entities.h`**(header-only,1311 行):Person(角色化)/Group/Label(一等实体)/Series/Work/Recording/Release/Track 八种;
  **结构化显式边**(12 条 (rel,from-kind,to-kind) 白名单,测试拿它逐条校验图里每条边方向,不是抽查);
  `<base>/entities/<kind>/<id>.json` 原子落盘
- **id 零新哈希**:唯一调用点 `MetaStore::hashOf`;测试用「测试内独立实现 FNV-1a」+ `net::fnv_hex` 双向对照;
  Track 另挂 `trackHash` 可直接 join 既有 `tracks/<hash>.json`
- **归一化只判等**:`name` 恒为首次写法,`aliases` **只增不删**(溯源底线);**故意不剥 `(Live)`/`(Remix)`**(有语义,剥了错并);
  **Track 键不折叠全半角** —— 本库真有 `（` 与 `(` 混写的不同文件,折叠 = 实质数据损坏
- 古典四层 Composer→Work→Recording(乐团×指挥×独奏)→Release;流行退化 Artist→Release→Track;同人挂 Group+Series
  (真实样本 `塞壬唱片-MSR` = Label×Series 双挂已断言)
- **`tests/test_entities.cpp` 209 断言**,含真实 MUSIC 样本(只读)

## 4. 交付四:两个**既有地基层缺陷**(主管复核挖出,非本轮引入)

### 4.1 `probe_media` 从不取时长 ⟹ match 的「±3s 强信号」从 P1 起一直是假的

- `MediaInfo::duration` 字段存在但**从未被赋值**:ffprobe 只查 `format_tags`,没有 `format=duration`
- 后果链:`fileSec` 恒 0 → `mf_duration` 走「任一侧缺失→0.5 中性」→ **文档写的强信号从未生效**
- 修:`-show_entries format=duration:format_tags=…`;新增 `probe_parse_duration`(`strtod`+endptr,**绝不用 `std::stod`**——
  它对 `N/A` 抛异常而本函数在播放链路上);`media_duration_sec` 挡 NaN/inf/越界
- **判别力证据(这才是有意义的验收)**:同一候选 title/artist 都对时,
  `fileSec=0` → total 0.925(dur:na 中性放过错候选);`fileSec=172` → total 1.000(dur:ok:1s);
  而错候选(差 28s)`fileSec=172` → dur 分 **0.00 far 被否掉**。即修好后时长**既会加分也会减分**
- `tests/test_media_duration.cpp` 53 断言(合成已知时长素材 + 真实 FLAC + 垃圾输入 12 种)

### 4.2 Release 判等键被 date 精度污染 ⟹ 同一张专辑被拆成多个 Release

- 85 张 ≥2 轨专辑里 **5 张**混杂;典型 `Swallow's Lies` 同碟两曲 `2022-08-16`/`2022-10-01` → **2 个 Release id**
- 修:新增 `entity_year4`(窗口 [1000,2100],与 `pv_year4` 同窗口但**形态更严**:5 位数字串整段跳过,不截前 4 位);
  **只有 4 位年进判等键**,完整 date 存属性
- **实测收敛**:`Swallow's Lies` 5 轨 **2 个 id → 1 个 id**(`5469d98c…`/`58b0de99…` → `148fbe8a…`)
- 反向验证的关键细节:注掉 `entity_year4` 后 FAIL 信息打印的两个 id 与真库扫描的「修补前 2 个 id」**逐字符相同**
  —— 证明测试抓的是真 bug,不是自证
- 另加**目录结构判据**(结构判定,非名字白名单):目录内直接有音频=专辑目录;只含子目录=分类目录;都没有=未知(三态)。
  `专辑-原创&同人` 这类分类目录**不产专辑名**(宁可留空走「专辑空则退化为曲名」兜底,绝不写分类名)

## 5. 验收

| 门禁 | 结果 |
|---|---|
| `cmake --build build` | 0 error |
| `ctest` | **43/43**(19 轮 40 → +entities +dizzylab +media_duration;既有全绿) |
| `verify.ps1` | **11/11**(含界面自检 52 项、GPU conv==CPU、DSD、分发兼容) |
| 独立复核(主管自跑) | 每轮交付后都重跑 build+ctest,不采信子代理自报 |
| MUSIC 只读 | 今日被改文件 **0**(多轮复核) |
| 端口 | 8899/8910/8911 全程未碰;测试用 8911 起停后确认端口释放、无残留 rtaweb |

## 6. 本轮踩坑(记入坑表)

1. **「参数名猜错」被当成「站点是 SPA」**:凭空造出需浏览器的工作项,阻塞队列首项三轮。
   判据:先看返回体有没有数据,再谈渲染方式。
2. **`uicheck.js` 把注册表项数写死成 4**:dizzylab 接入后 Web UI 自检直接红。
   加源的设计目标是「一个描述符+两函数零接线」,**测试写死数量与该目标相悖** → 改为「≥4 项 + 按 id 逐项核对能力位 + 断言 dizzylab 排在 qq 前」。
3. **`std::stod`/`attrs.at()` 在播放/验证链路上会抛异常**:前者对 `N/A` 抛,后者缺键抛(子代理实测直接把进程带走 `EXIT=-1073740791`)。
   缺键/脏值正是断言该抓的失败形态,不该变成崩溃 ⟹ 一律 `strtod`+endptr / 自定义取缺省。
4. **双精度转 int 越界是 UB**:`(int)(1e10+0.5)` 本机得 `-4` → 负秒 → 静默退回中性分(缺陷不报错,最难发现)。
   触发只需一个伪造 RF64 头。已在 `media_duration_sec` 挡。
5. **两个子代理并行改同一文件会互相覆盖**:本轮 `CMakeLists.txt` 差点被整体重写。对策:**只精确 Edit 插入自己的注册行,改前先重读**;
   任务书里明确写「不要整体重写」。

## 7. 未解决(如实,铁律 4)

1. **dizzylab 关键词策略受限**:enrich 不传专辑/社团,只能后缀放宽;按目录名搜需扩 ProviderDesc(未做)。
   单源延迟因此上升(主流歌每次多 1.4~2.9s;同人歌约 3.2s),建议后续做成「命中即短路其它源」。
2. **dizzylab 熔断只测了「冷却期内不发请求」分支**;「连续 3 次失败开冷却」无自动化测试(需可控失败源)。
3. **dizzylab year/genre 留空**:专辑页实测无发售日期字段,tags 链接语义未核实,不猜。
4. **实体模型的两个启发式有误判风险**(源码已标注):`entity_looks_label`(含「唱片」/records 结尾)、
   `entity_artist_kind`(乐团词表含单字「社」「団」「团」,「社会科学」也会中)。kind 进 id ⟹ 误判产生另一个 id(取舍:宁可拆开可见可手合,不错并静默丢源头写法)。
5. **目录结构判据尚未接进真实批量入口**:判据可用且已单测,但「5 张专辑收敛」是用只读验证程序按预期口径复算的,**不是跑真实入库**。
6. **多碟盒装(专辑目录只含 CD1/CD2 子目录)会被判成分类目录** ⟹ 专辑名回退成曲名。本库无此形态,未加聚合。
7. **古典 COMPOSER 在真实样本上未经端到端验证**:TEST 站交响诗实测无 COMPOSER 标签,所以 Composer→Work 这层只有合成样本验证。
8. **DSD 时长口径未验证**:ffprobe 对 `.dsf` 读的是 data 块实际长度而非 header 样本数(改 header 到 2^40 输出恒 0.005805)。
   当前以「拿不到则 duration=0 → match 中性」为回归基线。
9. **`EBWebView` 种子 profile 多占 25 MB**(见 §1),待打包排除。
10. **`entity_year4` 与 `pv_year4` 是两份代码**,窗口可能漂移(方案 (a) 的固有代价,注释指明同步点,无编译期断言)。

# 第十九轮:自动采样率探测瞬时为空 → 第二首按 44100 开流硬失败(1.0.7)(2026-10-22)

> 现场:C:\Users\Administrator\Downloads\rlplayer-playback (4).log(1.0.5,Ryzen 9 5900X,EDIFIER N300,39 端点)。
> **与第十八轮 VM 无声事故不是同一个问题**(见 §4 对比),两者在 log (4) 里同时存在,本轮只修了后者。

## 1. 现场(三处症状,只有一处是本轮的)

| 时间 | 症状 | 归属 |
|---|---|---|
| 02:39:24.432 | 第一首 `END pos=0 played=0 total=21725525 error=""`,187ms 后用户重按,第二遍正常播到 5.9s | ⚠️ **未修**(§5-1) |
| 02:39:44.422 | 第二遍在 pos=5.9s(19 秒墙钟)被 `END` 结束,无 error —— 用户手动停止 | 正常 |
| 02:39:45.120 / .806 | 连点两次第三首,**无 PLAY 行**,直接 `END pos=0 played=0 total=0`,error=「设备独占模式不支持 44100 Hz(请求位深 24-bit,候选 16/24/32 全不命中)…独占支持 16-bit: 48000/96000;24-bit: 48000/96000」 | ✅ **本轮修复** |

## 2. 根因(代码级)

`rate=max`(设置里「自动」)的选择代码:
```cpp
const auto rr = WasapiOutput::exclusive_rates(pEff.device, 24, in.channels);
want = rr.empty() ? 0 : rr.back();     // ★ 空列表 → 0
```
`want==0` 在下方被解释成「跟随源率」→ 按 44100 开独占流 → 该端点只支持 48k/96k 族 → 在写 PLAY 之前就退出。

**为什么探测会空**:上一首独占流 `sink->close()` 之后约 165ms 就是这次探测,端点正在重配置;
而紧接着的错误路径里 `probe_formats` 对**同一个端点 id** 又读到了 48000/96000 —— 证明是**瞬时**的,不是设备真不支持。

**性质**:违反项目自己在 HF-01 定的规矩 —— rtaweb.cpp 能力检查处注释写着
「rates.empty() 一律当『不支持/端点异常』,绝不静默放行」,而自动档这里恰恰静默放行了,
并且把「探测失败」当成了「探测结果是源率」。

## 3. 修复(1.0.7)

1. **`src/audio/rate_pick.h`**(新增,纯逻辑,零 `windows.h`):
   `RatePick{rate, decided, note, logTag}` + `pick_output_rate` + `pick_supported_rate`。
   关键是把 **`decided` 与 `rate` 解耦**:探测为空时 `decided=false` 且 `logTag="RATEPROBE_EMPTY"`,
   但 `rate` 仍给源率以便继续尝试 —— 既不被卡死,也绝不冒充结论。
2. **三道防线**(都在 rtaweb.cpp,不动 wasapi_out.h 行为):
   - ① 探测重试 3 次 × 40ms(第一次非空即返回);
   - ② 进程内按「端点|位深|声道」缓存**上次成功列表**,探测为空时沿用并记 `RATEPROBE_STALE`
     (理由:上一首刚用 96k 播完整首,本身就是该端点支持 96k 的铁证,不能被一次瞬时为空抹掉);
   - ③ 两者都拿不到才 `RATEPROBE_EMPTY`,日志能自证。
   能力检查(位深循环)与 DoP 分支都改走 `probe_rates_cached` —— 否则「第一步靠缓存拿到 96000,
   第二步能力检查又空」照样硬失败,等于没修。
3. **采样率自适应回退链**:自动档且目标率不被任何候选位深接受时,`pick_supported_rate` 挑一个能用的率
   (同族优先 → 不小于目标的最小值 → 最大值),`cfg.outRate` 与 `deviceRate` **成对**写入
   (只写一个会被下方 `if(!dop) deviceRate=outRate` 用旧值覆盖),`note` + `RATEFALLBACK` 日志明示非 bit-perfect。
4. **分档(保真底线)**:只有「自动 / 跟随源」档才换率。用户**明确选了 768000 这类数值**时,
   设备做不到就**如实硬失败**,绝不替用户改主意 —— 保真播放器不能悄悄降级。错误文案明示
   「你选的是 X Hz,设备做不了,不会自动改成别的率」。
5. **DoP**:载波率不可变,**始终硬失败**(不换率),但现在「探测为空」不会被当成「设备不支持」。
6. 错误文案补齐 源采样率 / 后端 / 端点 id(此前只有请求率与端点 id)。
7. `/api/rates` 也改走同一套探测(+刷新缓存),否则页面刚播完一首去拉列表同样撞空表,缓存永不被喂。
8. 锁纪律:`rate_cache_key` 在 devId 为空时要 `enumerate()`(COM + 39 端点),**移到锁外**计算;
   临界区内只剩一次 map 读写(这把锁 web 线程也用,webui 是单线程 accept)。

## 4. 与第十八轮 VM 事故的区别(同一份日志里两种故障)

| | 第十八轮(1.0.6 已修) | 本轮(1.0.7) |
|---|---|---|
| 触发 | 独占流**开成功**了,但设备恒不接受数据 | 流**根本没开** —— 能力检查就把率判死 |
| 症状 | `END pos=0 played=0 error=""`(静默,悬案) | `END ... error="设备独占模式不支持 44100 Hz"`(有归因) |
| 有 PLAY 行? | 有 | **没有**(发生在 PLAY 之前) |
| 关键数 | `w0` 暴涨 `wr=0`,`last_pad` 恒满 | `total=0`(连总帧数都没算出来) |
| 修法 | RENDERFAIL 遥测 + 共享后端回退 | 探测重试/缓存 + 采样率自适应回退链 |

⟹ log (4) 里第 12 行那类 `error=""` **仍需 1.0.6 的修复**;而 1.0.6 修不到第 20/23 行(有 error、无 PLAY)。
两版叠加才覆盖这份日志。

## 5. 验收

| 门禁 | 结果 |
|---|---|
| `cmake --build build` | **0 error**(25 条警告全为既有代码,新增范围零警告) |
| `ctest` | **40/40**(基线 39/39 + 新增 ratepick;既有测试无变化) |
| `test_ratepick.exe` | 40 条断言全 PASS |
| 反向验证(铁律 3) | 注释掉「同族优先」分支 → 2 条 FAIL(`want=88200 got=48000`);恢复(SHA256 校验)后全 PASS |
| `verify.ps1` | **11/11**(含界面自检、GPU conv==CPU、DSD、分发兼容) |
| `check_compat.ps1` | FAIL count: 0(静态 CRT / subsystem 6.0 / 无 AVX-512) |
| 主管独立复核 | 仓库外自写 harness(`%TEMP%\lead_ratecheck.cpp`)7 条断言全 PASS,含「设备支持 44100 时仍保持 44100(bit-perfect 不被动)」 |
| 8911 实测 | `/api/rates` → `[44100,48000,96000]`;明确选 768000 → 硬失败且文案含四项(来源/后端/端点 id/你所选的率);`backend=null` + rate=max → `outRate=44100 resample=false` 正常播完 |

## 6. 未解决(如实,铁律 4)

1. ⚠️ **第 12 行的 `END pos=0 error=""` 本轮未定位**。形态与第十八轮同类(独占流开后静默死亡),
   但 1.0.5 没有 RENDERFAIL 遥测,无法确诊;1.0.6/1.0.7 已能归因,需用户用新包复现时回传日志。
2. ⚠️ **重试(3×40ms)与缓存两层没有自动化测试**。它们依赖真实设备的瞬时重配置窗口,离线造不出来。
   现有证据只有:代码审查 + 逻辑推演 + 现场日志形态推演 + 纯函数单测。**不要当成「已验证」**。
3. ⚠️ **没有真机端到端证据**:按铁律 1 全程未打开音频设备(只做只读 `IsFormatSupported` 探测与 Null 后端播放),
   所以「第二首现在能出声」只有纯函数 + 接线审查支撑。需用户用 1.0.7 在 EDIFFER N300 那台机器上复验。
4. `rateAuto` 的分档(明确选择→硬失败 / 自动→换率)在接线层,未抽出纯函数,因此也**无单测覆盖**。
   下一步可抽 `decide_capability_failure(rateAuto, want, supported) → {action, rate}` 把它钉住。
5. 自动档换率后管线多一次重采样(例如 44.1k 源 → 48k 设备),属**有意为之的音质妥协**,
   已在 note/信号链/PROBE 日志三处明示;但「同族优先」只避免换族,**不避免升频**(如 44100→88200 仍是重采样)。
6. 缓存无上限淘汰,但键空间 = 端点数 × 3 位深 × 声道数(几十~几百条),判断为非泄漏;未做长跑内存实测。

# 第十八轮：VM 无声事故（1.0.6 热修，内测全员“无法运行”）（2026-10-22）

## 1. 现场（测试者 xinchenhan 的 rlplayer-playback (3).log）

- 机器：Win11 26100、**Virtual CPU（虚拟机）**、板载 HD Audio、端点 1 个；HF-01 的 16-bit 回退在其机器上工作正常；
- 现象：每首 PLAY 后 134ms~4.7s 即 `END pos=0 played=0 error=""`——播放静默死亡。后两次间隔恰 **200×20ms**，实锤。
- 推论：**用户本机（真 DAC TASCAM）正常、内测全员（VM/远程环境）全灭** —— 与热修单 HF-05、1.log 的“前能播后失败”同源。

## 2. 根因（代码级确认）

`play_one` 渲染循环：独占 WASAPI 在虚拟声卡上 GetCurrentPadding 恒满 → write_available 恒 0 →
200 次超时 `break`（rtaweb.cpp 原约 1670 行）**且无人给 ps.error 赋值** → END error="" 悬案。

## 3. 修复（1.0.6）

- **RENDERFAIL 归因**：渲染主循环抽到 `src/audio/render_loop.h`（rtaweb 与测试共用）；200 次超时 break 前写
  RENDERFAIL 行（时长/w0/wr/last_pad/缓冲/GetBuffer HRESULT/设备名/端点 id/处置建议）并同步进 ps.error；
  producer 空产出、eos 空等所有非正常退出路径全部归因——**END 不再出现 error="" 的悬案**；
  回归测试 test_renderfail（恒 0 假 sink 重放事故形态）入 ctest。
- **WASAPI 共享模式后端**（`src/audio/wasapi_shared.h`）：GetMixFormat 直出（通常 float32/48k）、
  int32→float32 换算、补满+轮询节奏；设置页 backend 加「WASAPI 共享」；
  **自动回退**：独占开流成功但 RENDERFAIL 且允许回退 → play_one 包装层以 wasapi-shared 重入整条播放，
  note 明示“独占流无响应，已回退 WASAPI 共享（非 bit-perfect）”；**DoP/DSD 不回退**（共享会重采样破坏载波），
  明确报错指引用户切独占/ASIO。
- 设备 smoke 测试仅数字静音 ≤1s（铁律 1 合规，横幅先行）。

## 4. 未覆盖（如实）

- 真 VM 无法本地复现：共享模式在虚拟声卡上的实效待 1.0.6 内测复验（请测试者回传 导出诊断+两个 log）；
- 回退分支为接线+审查覆盖（本机独占正常，无法触发真实挂死）；
- uicheck 既有行为：循环键测试会把实例 repeat 置 1 不还原（非本轮引入，待下轮顺手修）。

# 第十七轮：P2 标签写回（2026-10-22）

## 1. 交付

- **`src/meta/writeback.h`**：ffmpeg remux 写回（12 字段、按格式家族两套键表——**实测 MP3 大写 Vorbis 名会写成 TXXX 私有帧，必须小写规范键**）；临时文件 → **PCM 逐位门禁**（不一致绝不落盘）→ 备份 `rlplayer-meta/backups/` → 原子替换（含失败回滚防御）；DSD/畸形 PICTURE FLAC 安全跳过；`writeback_library` 带进度回调 + 失败队列持久化（writeback-failures.json）。
- **`rtatag write`**：单文件 / `--library` 整库 / 默认 dry-run（预览不动盘）。
- **Web**：`/api/writeback` 预览(纯只读)/执行(后台线程)/进度三端点；设置页「标签写回」小节（预览/执行按钮+进度轮询+禁用态）；uicheck +7（uicheck 本身绝不点执行）。
- **year 垃圾值双层防御**（e2e 抓到的事故）：QQ 曾返回 `year="0000"` 被采纳写回，DATE 2022-12-24→0000。修：`pv_year4` 合理窗 [1000,2100]（QQ/网易/iTunes 三源同修）+ 写回侧守卫 + 沙箱复验。

## 2. 验收

| 项 | 结果 |
|---|---|
| ctest | **37/37**（新增 writeback 55 断言，PCM 门禁在内） |
| FLAC remux PCM 逐位性 | **实测关闭**（悬置三周的未解决项 §5.5） |
| 8911 沙箱 e2e | 两副本 enrich→写回→ffprobe 标签断言→DATE 防 0000；**MUSIC 原文件零触碰** |

## 3. 遗留（如实，详见调研文档 §1.7）

封面不写入文件（MetaBase 缓存顶显示）；dizzylab 批 FLAC 的 PICTURE 畸形待“重嵌封面”迭代解锁；probe_media 不读 stream 层标签（ogg/opus 读取缺口，既有问题）；预览与真批计数口径差（预览不跑 PICTURE 门）。

# 第十六轮：P1.5 注册表化与采纳门槛（2026-10-22）

## 1. 落地

- **Provider 注册表**（provider.h）：`ProviderDesc{id,name,canSearch,canLyric,search,lyric}` + `provider_registry()` 四源入表；enrich 与设置页遍历注册表工作；设置键迁移 `metasrc_<id>`（旧六键兼容读取）；新端点 `/api/providers` 驱动 UI 动态渲染——**新增数据源=一个描述符+两个函数，零接线**。
- **可选代理**：http_client 全局 set_http_proxy（默认空=直连，现状不变）；rtaweb 读 rlplayer-llm.conf 的 `proxy=` 行（本机 MB 已直连，代理为后备）。
- **采纳门槛**（本轮核心质量修复）：`enrich_adopt(confident, llmPick, llmRan, top)`——采用条件 = confident ‖ LLM pick≥0 ‖ top≥0.80；否则**不写任何字段/不下载/不取词/不落盘**。

## 2. 二十首两轮核对（P1 验收判据的落地）

- 第一轮（p15）：14 对 / **4 错配（20%）** / 2 疑——错配全是「低分 0.32~0.49 + LLM 裁决 -1 仍采纳」，网易同社团诱饵曲目；
- 修复后复验（p16，同批同工具）：**采纳 14 首、错配 0 首**；6 首正确拒采（含 4 例原错配 + Pick me! + 1 例保守拒采的 たいせつなきみ——宁缺毋错的代价，可接受）；采纳曲歌词命中 14/14；落盘核对 tracks=14/covers=8 与采纳数一致。
- ⚠️ 观察项：复验时段 iTunes 源整体不可达（genre 全空），属网络波动（P0 时可达），机制无恙；候选分数跨轮波动（如 #17 0.62→0.075）说明平台搜索结果集不稳定，门槛策略正确。

## 3. 未解决（如实）

- dizzylab 抓包未做（队列下一项，同人源命中率是未采纳曲的正解）；
- genre 依赖 iTunes 单源，单一故障即全空——P2 写回前考虑 netease/QQ 的 genre 兜底；
- 跑批脚本两轮都出现表格列输出缺陷后离线重建（脚本 bug，未根因定位，只影响报告不影响数据）。

# 第十五轮：白边框事故（2026-10-22）

> 用户报告 1.0.5「UI 最外一圈白框，丑」，并核实 **1.0.3 没有、1.0.4 开始有**。

## 1. 根因（逐像素测量定位，不是目测）

第十二轮无边框改造引入了**两层问题**，1.0.4 起：

1. DWM 边框色用了 `DWMWA_COLOR_NONE` —— Win11 26200 对带 WS_THICKFRAME 的窗口
   不认该值，回落系统亮色 → 窗口最外 1px 白；
2. **真凶**：WebView2 内缩 5px 露出的宿主边带**从未被绘制**（窗口类无背景刷、
   无 WM_PAINT）→ 显示创建时的垃圾像素（x=1 纯白 FFFFFF + x=2..4 灰 B4B4B4）。
   测量数据：修复前左边缘 x=0 FFFFFF 起步；修复后 x=0..7 全部 0C0E13/08090C。

## 2. 修复（launcher.cpp）

- 34=DWMWA_BORDER_COLOR 改设主题显式色（弃 COLOR_NONE）；
- 新增 `update_band()` + `WM_ERASEBKGND` FillRect：边带刷页面 --bg 同色
  （深 08090C / 浅 F4F6F9），随主题消息联动重刷；
- 顺带修正注释错误：36=DWMWA_TEXT_COLOR（原注释误标“边框”）。

## 3. 教训（铁律 3 同族）

- **边缘级 UI 缺陷必须逐像素测量**：第一轮“修复”只治了 DWM 层，视觉模型复检
  报“边缘一致”是**误报**（采样吞掉了 1-4px 的窄条），用户复测才暴露第二层。
  最终以 GetPixel 打点输出为准（左/顶/底 0..7 全量列出）。
- 回归窗口靠用户历史版本对照锁定（1.0.3/1.0.4），比考古代码快。

## 4. 交付

最终包：dist\RLPlayer-1.0.5-win64.zip，sha256 `db0beff27e1717cb6f4b36baae7151618373c4dad641a1ad15223778576689fe`。
Edge 回退路径（darken_edge_window）仍有一处 36 旧写法，影响面近零，随下批清理。

# 第十四轮：P1 元数据功能完成 + 1.0.5 热修复 + 双 5 小时配额事故（2026-10-22）

> 本轮把元数据补全做成用户可见功能（P1 三波），同时按 docs/热修复交接-2026-10-21.md
> 完成 1.0.5 热修复与内测可观测性扩展。全部代码由 GLM-5.3-Flash 子代理编写、主管审查验收。

## 1. P1 三波（元数据补全功能面）

- **Wave A**：provider.h（QQ/网易/iTunes/lrclib 四源搜索+歌词，端点全部实测）、match.h（评分/排序/置信判定）、全角括号摘组修复（曲库 10 首受益）
- **Wave B**：enrich.h（解析→搜索→评分→[LLM 裁决]→融合→封面→歌词链，全降级设计）、rtaweb 接线（播放触发后台补全、/api/enrich、/api/meta 覆盖层、封面/歌词兜底、设置六键）、rtatag enrich 子命令；真网 e2e：谭晶·游京 24 候选 QQ 0.925 命中
- **Wave C**：设置页「元数据补全」分组（六开关+导出诊断按钮）、正在播放页富信息芯片、歌词空态、uicheck 扩展；8910 端到端全过（enrich 2.77s）
- **QQ 歌词之谜**：Wave A 误标「匿名不可得」；主管复核发现是 ID 用错——歌词接口要歌曲级 `mid`，不是 `file.media_mid`（实测 3400B base64 LRC）。教训：**同名字段不同层级是常见陷阱**

## 2. 1.0.5 热修复（两刀 + 收官轮）

- **第一刀**：wav.h 头校验（ch/bps/边界/上限，错误带 fmt 字段）、audio_loader 临时 WAV 检查与异常带上下文、resampler 构造防御（gcd(0,x)=x→M=0 路径）、wasapi_out.h probe_formats/OpenFail 结构化探测；tests/test_hotfix_guard（离线守卫）
- **第二刀**：HF-01 位深协商回退链（请求→源位深→16→32，rates.empty() 永不当成功，降级明示 bitsFallback）、HF-02 端点探测+失效回退默认端点+ENDPOINT_FALLBACK 日志、HF-04 deviceRate==0 防御、版本 1.0.5（新 src/version.h）、可观测性六件套：启动横幅（版本/构建/PID/OS/CPU/GPU/端点数/可选 tester 标识）、START/DECODE/END 日志、崩溃 minidump（src/crash_dump.h，dumps\）、/api/diag、日志轮转 3 份、PLAY/END 带 file/deviceId/版本
- **收官轮 F1-F6**（Wave C 与第二刀发现的问题）：F1 非法 UTF-8 file 参数防崩（valid_utf8 状态机 + 9 入口 + WARN 日志，实测 GBK 字节返回友好 JSON）、F2 meta.enriched 语义（按字段来源判定）、F3 /api/diag 探活 60s TTL 缓存（probeAge/refresh=1）、F4 歌词空态文案、F5 wav.h 中文路径宽字符打开（自带纯标准库 UTF-8→UTF-16 + _wfopen）、F6 logTail 掩码

## 3. 终验收与发布

- ctest **36/36**；verify.ps1 **11/11**（首跑 Web UI 1 项瞬态失败，8907/8910 两次独立复现全过——diag 探活时序敏感，F3 缓存后应不再现，如实记录）
- 出包 1.0.5（make_dist 自带兼容门禁+GPU System32 冒烟）；工具白名单补 rtatag.exe；README 模板加内测专属说明（tester 文件/诊断导出/dumps）与元数据功能说明
- 安全核验：打包为严格白名单，rlplayer-llm.conf（API key）/rlplayer-meta/日志均不进包

## 4. 踩坑（新）

1. **配额是套餐共享的**：GLM-5.3 与 Flash 同池，主管自己下场写码烧得更快（两次 1308 事故）；对策：主管只审查+跑门禁，编码永远派 Flash
2. **同名字段不同层级**：QQ 搜索结果里 `mid`（歌曲级）与 `file.media_mid` 是两个 ID，歌词接口只认前者
3. **verify.ps1 的 Web UI 项有瞬态失败面**（实例启动竞态/时序敏感断言），失败先独立复现再定性
4. **fs::u8path 对非 UTF-8 字节直接抛异常**——所有外部输入进 fs 前必须 valid_utf8（F1 的教训，非浏览器调用方可打死服务）
5. **窄 fopen 在 ACP=936 打不开 UTF-8 中文路径**（F5）：Windows 上所有按路径开文件的新代码一律宽字符

## 5. 未解决（如实）

- 真实设备上的 24→16 回退与端点失效场景待内测复现（HF-01/02 验收 1-3 条）；1.log/2.log 原始文件待测试者按交接单 §4 补充
- /api/enrich 全源超时最坏可超 20s（provider 单源 15s；正常命中数秒）——批量场景前再评估
- dizzylab 接口待浏览器抓包；酷狗接口否决存档；lrclib 当日 503 属服务端过载
- 同人/小众曲目四源命中率低（预期内，dizzylab 接入是解法）

# 第十三轮：元数据补全 P0（2026-10-21）

> 新方向启动：多源标签补全 + DeepSeek 资料层。调研、规划与本轮全部实测记录在
> **docs/元数据补全调研与规划.md**（该方向的权威文档）。本轮由 GLM-5.3-Flash
> 子代理编码、主管审查验收。

## 1. 落地了什么

- `src/meta/store.h` —— MetaBase 曲目级存储：`rlplayer-meta/tracks/<hash>.json`，
  每字段带 {value,source,ts,confidence} 溯源，合并优先级 user>tag>事实源>llm，
  原子写，封面缓存 covers/。
- `src/meta/parse.h` —— 文件名/标签规则解析器：曲号剥离、" - "与"）-"切分、
  尾部括号组摘除（【】/()/[] + feat 组分类）、feat 标记提取、LLM 回包解析与提示词。
- `src/meta/llm.h` —— DeepSeek 客户端：chat/search/fetch 三端点，model 固定
  deepseek-flash，key 只存 rlplayer-llm.conf（绝不进源码），错误信息一律脱敏。
- `tools/rtatag.cpp` —— CLI（parse / store show|set / llm-smoke），**wmain 宽字符入口**
  （窄入口在 GBK 代码页下中文路径直接崩，实测退出码 127；有 wv2_probe 先例）。
- 测试 +3：metastore（38 断言）/ metaparse（82 断言）/ metallm（35 断言，全离线）。
- 文档：docs/元数据补全调研与规划.md（Roon 放弃依据、五源与 DeepSeek smoke 实测、
  许可证红线、实体模型、P0~P6 路线、未解决项）。

## 2. 验收实测

| 项 | 结果 |
|---|---|
| cmake 构建 | 0 error |
| ctest | **32/32**（旧 29 + 新 3） |
| rtatag llm-smoke（真实 key） | chat 862ms / search 346ms（5 命中）/ fetch 238ms，全通 |
| MUSIC 全库 parse（653 首） | 94% 已有标签；artist 提取 258（同人盘信息在目录名，属预期）；version 错分 feat=0 |
| 解析缺陷修复 | feat 括号形态 + 闭括号连字符切分，修复后真实文件实测正确 |

## 3. 踩坑（新）

1. **窄字符入口 + 中文路径 = 未捕获异常退出 127**（GBK 字节被当 UTF-8 喂 fs::path）。
   新 CLI 一律 wmain。
2. **全角括号字节我写错了**：）是 EF BC 89、」是 E3 80 8D（不是 E3 80 89/97）；
   修复者用 printf\|xxd 实测纠正。写字节级规格必须先实测。
3. **CRLF 统计假象**：rtatag 输出 \r\n，grep 的 `.` 吞掉 `\r` 造成「全非空」假统计；
   管道统计先 `tr -d '\r'`。
4. shell curl 的 JSON body 中文会被污染（DeepSeek 报 invalid unicode），smoke 中文
   载荷用 \\u 转义或显式百分号编码。

## 4. 未解决（详见调研文档 §5）

全角圆括号（）不在摘组集合（10 首受影响，P1 首项修）；lrclib 当日 503（重试+回退设计）；
酷狗接口否决；ffmpeg remux 逐位性待 P2 验证。

# 第十二轮：无边框一体化界面（2026-10-21）

> 用户给了一张 QQ 音乐截图：不要“原生标题栏 + 顶栏 + crumb”三层横条，
> 希望整个界面像 QQ 音乐一样连成一片；并且换主题时横条不能隔几秒才变。

## 1. 做法

- 主窗口改成 **无边框**：WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU，
  WM_NCCALCSIZE 去掉原生标题栏，WM_NCHITTEST 保留四边 6 px 缩放。
- WebView2 四周内缩 5 px，鼠标在边缘能命中宿主窗口的缩放区域；页面四周留出同色背景。
- 顶栏（.top）承担原来的标题栏职责：
  - 拖动：网页 mousedown → chrome.webview.postMessage('drag') → 宿主
    WM_NCLBUTTONDOWN/HTCAPTION；
  - 双击最大化/还原；
  - 右侧最小化 / 最大化 / 关闭按钮；
  - 最大化状态由宿主 WM_SIZE 反向 postMessage('max'|'normal') 同步图标。
- **删除 crumb 那条独立的横条**：pth / cnt / tools 直接放进顶栏；
  顶栏背景改成与主内容相同的 var(--bg)，去掉 border-bottom，和左侧栏/内容连成一片。
- 主题即时生效：网页切主题立即改 CSS 变量，并 postMessage('theme:...') 让宿主
  立即改 DWM 边框/圆角；不再依赖 1.5 秒轮询。原生标题栏已经不存在，所以没有“横条晚几秒”的问题。

## 2. 实测

- GetWindowRect / GetClientRect 均为 **1340x880** → 没有原生标题栏；
- 最大化/还原：IsZoomed true/false，窗口尺寸随屏幕工作区变化；
- 关闭按钮：进程退出、8899 端口释放；
- 拖动：窗口从 (120,60) 移到 (270,120)，launcher.log 有 wvmsg: drag；
- 主题切换：120 ms 内顶栏像素从 8,9,12 变为 233,242,255；
- 界面自检 52 项全过（浏览器模式下窗口按钮自动隐藏，不会出现不能用的假按钮）。

## 3. 注意

- 无边框窗口的窗口按钮只在 WebView2 内置界面里显示（html.webview2）；
  Edge/浏览器回退时仍用它们自己的标题栏。
- 包内 WebView2 运行时目录必须叫 **WebView2Runtime**（包根下），不能叫 WebView2
  （见第十一轮的 0xC0000409 记录）。

## 4. 包

- dist/RLPlayer-1.0.4-win64.zip，555 MB
- sha256 16164F7A4849EBE75F846DB12438524EE84820D49F9CD3731B1700CACBDEDE1B

---

# 第十一轮：现场日志驱动的修复 + 正在播放页/歌词/WebView2（2026-10-21）

> 用户回传 `rlplayer-playback.log`（1.0.2）后，问题从“猜”变成“可定位”。
> 这一轮修的是日志里能直接看到的真实故障，不是重构。

## 1. 日志暴露的真正噪音来源（WASAPI 写入模型）

用户日志关键行：

```
19:37:44 STAT pos=1.9 rate=96000 pad=9600 w0=93  wr=19
19:39:16 STAT pos=10  rate=768000 pad=0    w0=407 wr=100
19:39:22 STAT pos=15  rate=768000 pad=76800 w0=608 wr=150
```

`pad` 经常等于整个缓冲（9600 / 76800），`w0`（写不进去的次数）远大于 `wr`。
根因：`write_one_period()` 只在 `pad == 0` 时写“一个整周期”。当设备周期小于
缓冲时长（96k EDIFIER、768k iBasso 都是这样），事件到来时 pad 通常不是 0，
于是大量事件被跳过；一旦错过窗口就欠载/重复旧缓冲——听感就是“播几秒变噪音”。

修法：新增 `WasapiOutput::write_available()`：
- 每次事件查 `GetCurrentPadding`，把可用空间补满（按设备周期取整）；
- 不再要求 `pad == 0`；
- `rtaweb` 的普通路径与 DirectSDM 路径都改用它。

同时修：
- 空解码结果不再报“设备不支持 0 Hz”，改为“解码结果为空”（日志里那几条 0 Hz 的真相）；
- WASAPI 端点 id（`{0.0.0...}.{GUID}`）不再被误当 ASIO CLSID（`CLSID 解析失败` 的来源）。

## 2. 正在播放整页

- 整页背景/遮罩/返回键跟随主题（浅色/浅蓝/深色），不再出现“原生标题栏浅色、
  页面一大块深色”的割裂；
- 时间读数改为居中：`当前 0:00 / 总时长 0:00`，底部播放条加 title；
- 返回键、播放键、进度条在浅色主题下对比度重新校正。

## 3. 内嵌歌词

两个真实原因：
1. 只读 ffprobe 的 `format_tags`，Ogg/Opus 的歌词在 `stream_tags`；
2. 整页 `#npLyric` 只在打开时渲染，异步加载完成后只刷新右栏 `#lyricbox`，
   所以整页永远显示“没有歌词”。

修法：
- `-show_entries format_tags:stream_tags`，遍历 format 与每个 stream 的 tags；
- 支持 `LYRICS / UNSYNCEDLYRICS / USLT / SYLT / LRC`（大小写不敏感）；
- 优先带时间轴的歌词，否则取最长的一段；
- 切歌时总是加载歌词；加载完成后同时刷新右栏和整页；
- 用 FLAC / MP3 / M4A / OGG 四种容器做过回归（OGG 之前必失败）。

## 4. WebView2 固定运行时内置

- `tools/make_dist.ps1` 把系统 WebView2 运行时整目录复制到包内 `WebView2Runtime\`
  （**不能叫 `WebView2`**：实测 `rtaweb.exe` 在包目录直启时会被该目录里的
  `msedge.dll` 触发 0xC0000409；改名后直启/启动器都正常）；
- `wv2_host.h` 优先用包内 `WebView2Runtime\msedgewebview2.exe`，找不到才回退系统；
- 实测包内 `msedgewebview2.exe` 路径生效，界面正常打开；
- 包体积从 173 MB 增到 **554.9 MB**（用户已同意）。

## 5. GPU 多后端回退（N 卡的前置）

- `gpu_dll_candidates()` 改为 `rtagpu.dll → rtagpu_dx12.dll → rtagpu_cuda.dll`；
- `GpuBridge` 与重采样器现在会**逐个尝试**；某个 DLL 存在但 kernel 不可执行时
  继续下一个，不再被第一个坏 DLL 卡死；
- 本机 AMD 仍走 HIP；N 卡机器会明确显示“未找到可执行后端”，而不是误导性的
  “架构不匹配”。真正的 N 卡加速下一批用 D3D12 Compute（不依赖 CUDA 工具链）。

## 6. 验证

- 构建 0 error；`ctest` **29/29**
- 界面自检 **52 项全过**（含正在播放页打开/关闭）
- 包内 WebView2 进程路径实测：`dist\RLPlayer-1.0.3-win64\WebView2Runtime\msedgewebview2.exe`
- 包内歌词接口实测：Ogg 内嵌 `LYRICS` 返回 `src=embedded`
- 包内 GPU：`/api/gpu` → `GPU 卷积可用（rtagpu.dll）`
- 最终包：`dist\RLPlayer-1.0.3-win64.zip`，554.9 MB，
  sha256 `2B0F604D61700F870C13C9A3F04955B5EE3D8D33217B9E03D65ABA7BCBEAE303`

## 7. 仍未解决

- 用户新日志尚未回传，`write_available` 对实际设备的效果需要下一份 STAT 验证。
- N 卡加速（D3D12）未实现，只有加载链回退。
- 标准 NAA、输入采集、UPnP 投放、Native DSD 仍未做。

---

# 第十轮：1.0.2 正式发布版 + “播几秒变噪音”根因修复（2026-10-21）

> 用户反馈：1.0.1 Full 包播放几秒后突然输出噪音；并明确四条发布要求：
> ① 以后只有 FULL 包，名字叫“正式发布版”；② 必须内置 GPU 所需全部组件；
> ③ 软件里有的功能必须自包含完整；④ 先看 launcher.log 继续 DEBUG。

## 1. 为什么 launcher.log 里看不到原因

用户发来的 `launcher.log` 只有 30 行，全是启动记录（4 次启动、8899 起服务、
内置 WebView2 打开界面），**没有任何播放参数、采样率、峰值或错误**。
播放线程的日志原来只存在 `/api/dbg` 的 400 行环形缓冲里，进程重启就没了。
这是可观测性缺陷，不是用户操作问题。

修法：新增 **`rlplayer-playback.log`**（exe 同目录，UTF-8 BOM，4 MB 轮转）：
- `PLAY`：文件、codec、输入/输出采样率/位深/声道、shaper、filter、volume、
  dsd、preAtten、modHeadroom、GPU 状态；
- 每 5 秒 `STAT`：pos、rate、pad、peak、clip、w0/wr；
- `END`：播放位置、总帧数、clip 总数、error。
`/api/status` 也新增 `peak` / `clip`。下次反馈只需要这个文件。

## 2. 两个真实的“突然变噪音”根因

### 2.1 LNS15 量化器锁死（PCM 路径，默认就会遇到）

`src/dsp/dither.h` 旧实现只钳位量化**输出** y：
```
y = Q(u); y = clamp(y, -1, 1); e = y - u;
```
一旦 `u` 超过 1（24-bit 输出 192k+ 时，0.999 满幅就够），`e = y-u` 变成 O(1)，
再经 `(1-z^-1)^15` 的 90 dB 带外增益反馈，环路锁死在 ±1 方波。
听感正是“播到第一个大动态，突然变成噪音”。

**离线复现（修复前）**：384k/24-bit、997 Hz、幅度 0.999999，5 秒内
`clamped` 触发 142 万次，最后半秒 RMS=0.93（应为 0.7071）。
**判据陷阱**：原 `test_dither` 只测 −60 dBFS，完美绕过这个故障 —— 又一次
“测试通过不等于测对了东西”。

修法：
- 钳位改到量化**之前**：`u = clamp(u, -1, 1-q)`，|e| <= q/2，反馈有界；
- 新增 `overload()` 计数（正常播放为 0，满幅时允许少量削顶）；
- 新增 `test_fullscale`：走完整 Pipeline（44.1k→192k/384k、LNS15、24-bit），
  断言稳态 RMS=0.7071、无长串满幅样本、输出不越界。旧代码该测试失败。

### 2.2 DSD 调制器满幅锁死 + 默认抖动过大（DSD 路径）

`src/dsp/modulator.h` 旧实现把误差 `e` 钳到 `eMax=64`（形同虚设）。1-bit
5 阶在 DSD64 上的实测 MSA 只有约 0.65；输入 0.70 起就锁死，输出变成
满幅噪音。播放器却把整曲直接送进调制器，任何接近 0 dBFS 的曲目都会中招。
另外默认 `ditherLsb=0.01`（1-bit 步长是 2.0）等于注入 −34 dBFS 白噪，
把 DSD64 带内 SNR 从 89 dB 砍到 54 dB。

修法：
- 环路限幅：`u = clamp(u, -1.5, 1.5)`，`eMax=2.5`；0.62 以内完全不触发；
- 前级自动余量 `sdm_headroom_db(peak)`：按本曲真峰值把输入压到 0.62 以下
  （满幅曲目约 −4.3 dB，安静曲目不衰减），写进信号链 `modHeadroom`；
- 默认抖动改 `0.0005`（SNR 79.6 dB，仍能打散空闲音）；
- 新增 `test_modulator` 第 6 节：0.62 → 1.0 瞬态 → 0.62，断言瞬态后
  `overload` 停止增长、解调 RMS 回到 0.442、同号率 < 0.6（锁死会接近 1）。

### 2.3 顺带修掉的 float 输入 bug

ISP 扫描原来直接读 `in.raw` 当整数；float WAV 的 raw 是 float 位模式，
会被解释成天文数字的“峰值”，导致永远预衰减 3 dB。改用 `in.sample()`。

## 3. 发布策略变更（1.0.2 起）

- **只有一个包**：`RLPlayer-1.0.2-win64.zip`，叫“正式发布版”，不再有 Lite。
- 包内必须齐全：`ffmpeg.exe` / `ffprobe.exe`、`rtagpu.dll`、`amdhip64_7.dll`、
  `WebView2Loader.dll`、`asio_probe.exe`，以及 `tools\` 下的 CLI 工具。
- `tools/make_dist.ps1` 强制：
  - 兼容门禁（静态 CRT / subsystem≤6.1 / 无 AVX-512，新增 rtagpu.dll）；
  - GPU 构建缺失架构即失败（`build_gpu.ps1` 校验 25 个 gfx 字符串）；
  - 在 **PATH 只剩 System32** 的临时目录里跑 GPU 卷积 smoke，`[PASS]` 才出包。
- GPU 运行时只依赖 `amdhip64_7.dll`（实测其导入只有系统 DLL），17.5 MB；
  不需要用户安装 ROCm。显卡驱动仍由用户安装（硬件驱动不可分发）。

## 4. 验证

- 构建 0 error；`ctest` **29/29**（新增 fullscale；dither 第 4 节、modulator 第 6 节）
- 兼容门禁：RLPlayer/rtaweb/rtaplay/asio_probe/rtagpu 全部
  `crt=static; subsystem=6.0; avx512=0`
- 1.0.2 包：解压后 PATH 只剩 System32 →
  `/api/ping` version=1.0.2；`/api/gpu` amd=true/hip=true/available=true；
  FLAC 用包内 ffmpeg 解码；Null 后端播放并生成 PLAY/STAT/END 日志。
- `verify.ps1` **11/11**（修复了 build_gpu.ps1 的 `$log:` 解析错误与
  native stderr 被当成终止错误两个问题；GPU 25 架构逐个校验）。
- 最终包：`dist\RLPlayer-1.0.2-win64.zip`，172.9 MB，
  sha256 `8DCAB3E1978FF3C058225FB781C02E6978FC0147089E9C305F9A23CB288FD0A7`。
- 全程没有向用户听音设备播放可听声音（Null 后端，−60 dBFS）。

## 5. 仍未解决 / 待用户复现

- 如果 1.0.2 仍然出现噪音，请把 `rlplayer-playback.log` 的 PLAY/STAT/END 发回：
  END 里的 `clip` 和 STAT 里的 `peak` 能直接区分“削波噪音”和“设备/驱动问题”。
- 标准 NAA 握手、输入采集、UPnP 投放、Native DSD 仍是未实现项（界面已如实标注）。

---

# 第九轮：分发兼容性事故修复（2026-10-21）

> 用户反馈：1.0.0 分发版在 **Ryzen 9 5900X（Zen 3，无 AVX-512）** 上双击即崩，
> 错误码 **0xC000001D（STATUS_ILLEGAL_INSTRUCTION）**。本轮定位并修掉了两个独立问题，
> 并把「分发兼容性」变成构建门禁，不再靠人记得。

## 1. 根因（两个，都会让用户打不开）

| # | 根因 | 证据 | 影响 |
|---|---|---|---|
| 1 | CMake 里写死 **-march=znver5**（Zen 5） | 崩溃偏移处的机器码是 `62 F1 7C 48 11 05 …` —— 0x62 是 **EVEX 前缀**（AVX-512 zmm） | 任何不支持 AVX-512 的 CPU（5900X、Intel 10 代以前）**非法指令** |
| 2 | 动态链接 MSVC 运行库 | 旧 exe 导入 VCRUNTIME140.dll / MSVCP140.dll / api-ms-win-crt-* | 干净系统/未装 VC++ 运行库 → 0xC000007B；Win7 还要 UCRT 补丁 |

「最小分发版」确实**少了环境**：只打了 5 个文件，却默认用户已经装好 VC++ 运行库。这是设计错误，不是用户环境问题。

## 2. 修法

- **CPU 基线**：CMake 新增缓存变量 `RTA_ARCH`，默认 **x86-64-v2**（SSE4.2 + POPCNT，2009 年后的 x86-64 都能跑，远低于用户要求的 Zen 1 / Intel 6 代）。
  本机开发想要极限性能可 `-DRTA_ARCH=znver5`，但**分发脚本拒绝打包带 AVX-512 的产物**。
- **运行库**：`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` → 静态链接 CRT。新 exe 只导入系统 DLL
  （KERNEL32 / USER32 / ole32 / SHELL32 / WS2_32 / WINHTTP / WINMM / AVRT / ADVAPI32），
  **不再需要 VC++ 运行库**，Win7 也不需要 UCRT。
- **PE 子系统版本**：实测 6.0（Win7 加载器接受）。
- **新门禁** `tools/check_compat.ps1`：逐个检查
  ① 没有 VCRUNTIME/MSVCP/UCRT 导入；② subsystem ≤ 6.1；③ 反汇编里没有 zmm / EVEX 掩码。
  已接进 `verify.ps1` 第 11 项；`tools/make_dist.ps1` 打包前强制跑它，失败就拒绝出包。

## 3. 性能代价（实测）

| 基准 | x86-64-v2 | x86-64-v3 (AVX2) | znver5 (AVX-512) |
|---|---|---|---|
| 调制器（test_parallel 串行最好） | 76.1 ms | 74.7 ms | 70.6 ms |
| test_resampler 全程 | 22.7 s | 25.7 s | 25.3 s |

差异在噪声量级（≤7%），换来的兼容性收益远大于损失。因此分发基线定在 **v2**。

## 4. 「5 个文件就够了吗」——环境完整性审计

用户质疑得对：**运行库**确实齐了，但**解码器**没有。两者不是一回事：

| 依赖 | 状态 | 证据/做法 |
|---|---|---|
| MSVC 运行库 | ✅ 不再需要 | 静态 CRT；导入表只剩 KERNEL32/USER32/ole32/shell32/ws2_32/winhttp/winmm/avrt/advapi32 |
| 系统 DLL | ✅ 齐全 | 把 PATH 只留 C:\Windows\System32、且没有任何 ffmpeg，WAV 播放正常 |
| 编解码器 | ❌ 旧包缺 | FLAC/MP3/APE/OGG 需要 ffmpeg；旧包只查 PATH，用户不装就放不了 |
| WebView2 运行库 | ⚠️ 不内置 | Win10/11 用系统 WebView2 → 没有则 Edge → 再没有用默认浏览器；Win7 只能用浏览器 |
| GPU 后端 | ⚠️ 不内置 | rtagpu.dll 需要 ROCm + gfx1201；缺失时纯 CPU（已实测） |

修法：**播放器现在优先在 exe 同目录找 ffmpeg/ffprobe**（原来只查 PATH），并且缺 ffmpeg 时的报错直接告诉用户
「把 ffmpeg.exe 放到播放器同目录」。实测：PATH 只剩 System32 + 同目录放 ffmpeg/ffprobe → FLAC 正常解码。

## 5. 新分发包（1.0.1，两个版本）

    dist\RLPlayer-1.0.1-win64.zip          0.9 MB   Lite
    ├ RLPlayer.exe / rtaweb.exe / asio_probe.exe / WebView2Loader.dll / README.txt
    └ WAV / DSF / DFF 开箱即用；其它格式需自备 ffmpeg.exe + ffprobe.exe（放同目录即可）
    sha256: 0D4EB14495FE1444E353FC0E8D17CB01027C59A570D9F11D1D12478F16527764

    dist\RLPlayer-1.0.1-win64-full.zip   165.9 MB   Full
    ├ 上面 5 个 + ffmpeg.exe + ffprobe.exe（gyan.dev 构建，GPLv3，源码见 ffmpeg.org）
    └ FLAC / MP3 / APE / OGG / WMA 等开箱即用
    sha256: D3AA7EB3B889C4122E8330AAA0B0716EE1AE57D48AEA5C3FE9153FF7C40D5AF8

- 两个包都**不含 rtagpu.dll**：它需要 ROCm 运行时且只编了 gfx1201；缺失时自动纯 CPU（已实测 gpuAvailable=false）。
- 支持矩阵（写进 README）：Win7 SP1 x64+；CPU 最低 SSE4.2/POPCNT（官方下限 Zen 1 / Intel 6 代）；
  产物不含 AVX-512；AVX/AVX2 只出现在静态 CRT 的字符串函数里（运行时按 CPUID 分派，老 CPU 不会走到）；
  Win7 没有 WebView2，只能用 Chrome/Edge 109/新版 Firefox 打开界面；NVIDIA 10 系与 RX480 只能纯 CPU。

## 6. 验证

- `ctest` **28/28**（静态 CRT + x86-64-v2 下重跑）
- `verify.ps1` **11/11**（新增「Distribution compatibility」）
- 解压 zip 冒烟：/api/ping 正常、播放 1 秒 −60 dBFS WAV 到 Null 后端、pos 推进、退出无残留监听
- 兼容门禁输出：4 个 exe 全部 `crt=static; subsystem=6.0; avx512=0`

## 7. 后续

- 若要覆盖 NVIDIA 10 系 / AMD RX480 的 GPU 加速：需要 CUDA 内核与 ROCm 多架构（gfx803 早已被 ROCm 7 移除），
  属于新功能，不是本次兼容性修复的范围。
- Win7 的内置界面：WebView2 官方不支持 Win7；如果要做「双击即用」，得内置一个轻量浏览器控件
  （CEF/WebView1）或退回「启动服务 + 默认浏览器」——后者需要用户自备现代浏览器。

---

# 第八轮：DirectSDM 接线 + Conversion 预设 + NAA 端点实测（2026-10-21）

> 本轮把第七轮做好的 DSD 域核心接进了播放器，并补齐了 P0-5 的三组 Conversion 预设；
> 另外第一次拿到真实 NAA 端点（用户提供 192.168.50.109），做了只读探测并把结论写进
> `docs/NAA对接探测纪要.md`。全程没有可听声音：播放实测走 Null 后端与 NAA 文件收端。

## 1. DirectSDM 真正接进播放器（P0-3 完成）

以前只有核心函数与命令行；本轮走通了 `/api/play → 播放线程 → OutSink`：

| 环节 | 做法 |
|---|---|
| 判定 | `play_one` 最前面用 `probe_dsd` **只读文件头**（不载入整曲）判断源速率，再按「同速率 + 0 dB + 无 EQ/卷积/声道改动」决定是否直通 |
| 不满足 | 把原因写进 `ps.info` 与信号链的 `directNote`，然后照旧走「解成 PCM → 升频 → 调制」，不做假开关 |
| 数据通路 | 按 16 位整数倍切片 → `dsd_to_dop` 打包（标记相位跨块保持）→ 24-bit 载波写设备 |
| 进度 | 以载波帧计；`ps.outRate` = 载波率，所以 pos/dur 仍是秒 |
| 暂停/跳转 | 复用现有 `pauseReq`/`seekReq`；跳转**不重置** DoP 标记相位（避免出现 A,A 被判非法） |
| 结尾 | 不足一个周期时补**合法 DoP 静音帧**（0x55 交替），不能补全 0（标记 0x00 = 非法） |

界面：SDM 页新增 **DirectSDM 开关**（勾选时自动把音量压到 0 dB 并提示），
以及 **DSD 源抽取器**（FIR/IIR/CIC）与 **DSD 源 Conversion**（narrow/wide/XFi）两个下拉。
键名：`directsdm / dsdkind / dsdconv`，会持久化到 `rlplayer-settings.conf`。

**实测（全部不出声）**

1. Null 后端：directsdm=1&dsd=64&volume=0 → 信号链 `directSdm:true`、`inRate 2822400`、
   `carrier 176400`，pos 正常推进。
2. 回退原因：同一文件改成 dsd=128 → 普通路径，且 info/信号链写明
   「源 DSD 2822400 Hz ≠ 目标 5644800 Hz：必须经调制器重采样」。
3. 暂停/跳转：pos 冻结在 0.9 s，恢复后继续；跳到 50% 后从 5.3 s 继续。
4. **端到端逐位一致**（最有价值的一条）：播放器 → NAA（rtanet_rx 文件收端）→ 收到的
   24-bit 载波与 `dsdconv --direct` 的参考文件 **1,058,400 字节逐位相同**；
   `dopinfo` 判「DoP 标记合法」，解出 1 kHz 分量 −6.01 dBFS。

## 2. Conversion 预设（P0-5 完成）

三组**真参数**（只改通带上限，阻带起点 = outRate − 通带，过渡带随之变窄）：

| 预设 | 通带 | 阻带起点 | 过渡带 | FIR 抽头 | 带内最差 | 镜像衰减 |
|---|---|---|---|---|---|---|
| narrow | 20 kHz | 156.4 kHz | 136.4 kHz | 163 | 0.0000 dB | 123.6 dB |
| wide | 30 kHz | 146.4 kHz | 116.4 kHz | 191 | 0.0000 dB | 122.6 dB |
| xfi | 40 kHz | 136.4 kHz | 96.4 kHz | 231 | 0.0000 dB | 122.6 dB |

- 入口：`dsdconv --conv narrow|wide|xfi`；播放器的 SDM 页下拉；配置键 `dsdconv`。
- 判据进 `test_dsd_path` 第 6 节：「通带确实是设定值」「平坦度 ≤ 0.05 dB」「镜像 ≥ 100 dB」
  「通带越宽抽头越多」「过渡带越窄」——最后两条就是「不是换标签」的证据。
- 实测设置生效：CIC/xfi → `通带 40.0 kHz / 阻带 136.4 kHz`；IIR/wide → 30/146.4 kHz。

## 3. NAA 端点实测（用户提供 192.168.50.109）

完整记录见 `docs/NAA对接探测纪要.md`，要点：

- 该设备是 Accurate Audio “Wavedream NET”，后台只勾选了 HQPlayer NAA；TCP 43210 开放。
- 连上后**服务端先发 16 字节随机数**；本机跑官方 networkaudiod 5.19 得到同样行为 → 这是 NAA 5.x 的标准握手。
- 纯 XML（protocol/version_req/getdevices、换行/NUL/长度前缀/回显 token 等）**全部无回应**；
  daemon 侧 trace 只在断开时报 protocol error。结论：客户端必须先按未公开规则处理那 16 字节。
- EULA 明确禁止逆向与「非 HQPlayer 软件推流」，因此**没有继续深挖**；是否继续由用户决定。
- 新增 `tools/naa_probe.cpp`（只读 TCP 探针，--matrix 可复现）。

## 4. 本轮踩的坑

1. **`auto` 拷贝全局设置**：`apply_dsd_src_settings` 里写「auto st = dsd_pcm_settings();」，
   `auto` 推导为值类型，改的是副本 → 界面选 CIC/XFi，实际仍在用 FIR/narrow。
   改成 `auto&` 才生效（信号链里的 note 立刻变成 CIC / 40 kHz）。这类 bug 没有报错，只能靠「看实际生效值」发现。
2. **编辑 verify.ps1 会把 BOM 弄丢**：ps1 由编辑工具重写后变成无 BOM 的 UTF-8，
   PowerShell 5.1 按 GBK 解析中文注释直接报语法错（Missing type name after '['）。
   现在脚本头部写了提示，改完必须重新存成 UTF-8 BOM。
3. **Windows 防火墙弹窗**：运行 networkaudiod.exe 时系统弹「是否允许网络访问」，会挡住后续 GUI 自动化；
   只做本机回环测试时点「取消」即可，不必放行。

## 5. 验证

- 构建 0 error；`ctest` **28/28**
- `verify.ps1` **10/10**（新增第 10 项：DirectSDM 端到端逐位一致，走 NAA 文件收端）
- 界面自检 **52 项全过**（新增 DirectSDM 开关 4 项 + 抽取器/Conversion 下拉 1 项，含设置还原）
- 全程没有打开音频设备

---

# 第七轮：DSD 域处理（2026-10-21）

> ROADMAP P0。以前 DSD 文件是「DSD 进 → ffmpeg 解成 PCM → 升频 → 再调制回 DSD」，
> 现在有了原生 DSF/DFF 读取、三种抽取器、DirectSDM 逐位直通（核心 + 命令行闭环）。
> 本轮**没有**改动界面，也**没有**真正出声（全程 Null 后端 / 离线文件）。

## 1. 做了什么

| 项 | 文件 | 状态 |
|---|---|---|
| DSF / DFF 原生读取 + 写出 | `src/audio/dsd_reader.h` | ✅ 逐位验证（写→读→逐位比对 + 独立布局校验） |
| 测试素材生成器 | `tools/make_dsd.cpp` | ✅ 调制器 → DSF/DFF，本机因此第一次有了 DSD 测试文件 |
| DirectSDM（1-bit → DoP） | `src/dsp/direct_sdm.h` | ✅ 核心逐位一致；命令行 `dsdconv --direct`；**播放器接线未做** |
| 抽取器 FIR / IIR / CIC | `src/dsp/dsd_integrator.h` | ✅ 三条判据全过（平坦度 / 混叠 / THD+N） |
| 播放器入口接入 | `src/audio/audio_loader.h` | ✅ `.dsf/.dff` 走原生解码，失败才回退 ffmpeg |
| 命令行入口 | `tools/dsdconv.cpp` | ✅ `--info` / 抽取 / `--direct` 逐位比对 |

## 2. 判据与实测（全部进 ctest）

**2.1 读取器（`test_dsd_reader`，28 项）**

- 3 声道 / 块长 4096 / 末块 1000 字节：写→读**逐位一致**；按 DSF 规范手工算偏移抽查，
  确认「每声道分块交错 + LSB-first」——不拿读函数验自己。
- 2 声道 / 位数 5000×8+3：非整字节尾巴也逐位一致。
- DFF：大端 / 声道连续存放 / CHNL 声道 ID（SLFT/SRGT）/ MSB-first，全部核实。
- 坏文件：截断、魔数错、DST 压缩、缺数据块 —— 全部抛异常且信息具体。

**2.2 DirectSDM（`test_dsd_path` 第 1 节，13 项）**

- 4096 个 DoP 帧：解包后与源**逐位一致**；标记逐样本 05/FA 交替。
- 与既有 `dop_pack(double)` 的 24-bit 载波**逐样本相同**（不另造一套语义）。
- 分 14 块打包 == 一次性打包（标记相位跨块保持）。
- 3 声道、尾部不足 16 位（按规范丢弃）也验过。
- 直通判定：同速率 + 无处理 + 0 dB 才允许，否则给出具体原因。

**2.3 抽取器（`test_dsd_path` 第 2–4 节）**

DSD64 (2.8224 MHz) → 176.4 kHz（抽取 16）：

| 类型 | 参数 | 通带最差（0–20k） | 156.4 kHz 镜像衰减 | 实测混叠抑制* |
|---|---|---|---|---|
| FIR | Kaiser 163 抽头，120 dB | **0.000025 dB** | **123.6 dB** | **144.4 dB** |
| IIR | Butterworth 10 阶，DF2T | **0.0189 dB** | **155.9 dB** | **156.1 dB** |
| CIC | 6 级 + 3 抽头补偿 | **0.0305 dB** | **106.9 dB** | **108.1 dB** |

\* 实测：fs/18 = 156.8 kHz 方波探针（每周期 18 样本、严格 50% 占空比），折叠到 19.6 kHz；
丢弃 1024 个输出样本的启动瞬态，测量长度取折叠频率整周期。判据 ≥ 100 dB。

THD+N（DSD64 / 5 阶 1-bit / −6 dBFS / 998.5 Hz，测量窗从第 4096 个输出样本开始）：

| 抽取器 | 信号 | 带内噪声 | THD+N |
|---|---|---|---|
| FIR | −9.03 dB | −108.22 dB | **99.19 dB** |
| IIR | −9.03 dB | −108.23 dB | **99.20 dB** |
| CIC | −9.03 dB | −108.21 dB | **99.18 dB** |
| 参考 FIR（4001 抽头 / 180 dB） | −9.03 dB | −108.21 dB | **99.16 dB** |

三种与参考相差 **< 0.04 dB** —— 说明抽取器没有引入可测失真。

**2.4 素材闭环（`test_dsd_path` 第 5 节 + `make_dsd --verify`）**

调制器 → DSF → 读回（逐位一致）→ 抽取 → **THD+N 99.31 dB**；同一文件 DirectSDM 直通后逐位一致；
`read_any` 走的是「DSD 原生解码」而不是 ffmpeg。

**2.5 播放器实测（Null 后端，不出声）**

    POST /api/play  file=_e2e.dsf&backend=null&dsd=64&volume=0
    → inRate=176400, inBits=32, note="DSD 原生解码：dsf 2822400 Hz → PCM 176400 Hz（FIR 163 抽头，120 dB）"
    → outRate=2822400, dsdRate=2822400, pos 正常推进, 无错误

DSD64 10 秒素材经 `read_any` 解码耗时 **0.26 s**（约 38 倍实时，仅解码段）。

**2.6 独立交叉验证**

- `ffprobe` 能识别生成的 DSF：`codec_name=dsd_lsbf_planar`（印证 LSB-first 判断）。
- `dopinfo` 对 `dsdconv --direct` 写出的 24-bit 载波判「DoP 标记合法」，并解出 1 kHz 分量 −6.01 dBFS
  （源 −6 dBFS）。

## 3. 本轮踩的坑（都是「判据本身错了」）

1. **方波探针用 `sin(...) >= 0` 生成**：`sin(2πk)` 在 double 里是 ±1e-16，符号随 k 乱跳；
   加上每周期 16.98 个样本，占空比不是严格的 50%，探针自己带上了宽带杂散与偶次谐波。
   结果三种抽取器都测出「约 101 dB 抑制」，看起来像滤波器泄漏，实际是探针污染。
   → 改成**按整数样本相位**生成（period = rate/freq）。
2. **非整周期测量把直流漏进单音相关**：方波第 9 次谐波正好落在 Nyquist，抽取后变成直流；
   测量窗不是折叠频率的整周期时，直流经相关泄漏，19.6 kHz 被抬到 −90 dBFS。
   → 测量长度取折叠频率整周期。
3. **THD+N 的定义搞错了**：第一版在 FFT 里把 2f/3f… 的 bin 一起挖掉，那测的是「去谐波后的 SNR」，
   报出 98 dB 的漂亮数字；把 4001 抽头参考 FIR 的启动瞬态算进去还能掉到 54 dB。
   → 改成**时域最小二乘拟合基波并减掉**（谐波留在残差里，这才是 THD+N），并跳过滤波器启动瞬态。
   诚实时值 99.2 dB。
4. **CIC 补偿用无约束最小二乘**（17 抽头只拟合 0–20 kHz）：拟合解在高频有 **+74 dB** 增益，
   把 1-bit 的带外噪声放大成 −29 dBFS 的巨浪，THD+N 反而「变好」到 103.65 dB —— 明显不合理。
   → 改成 3 抽头解析补偿 `1 + 4c·sin²(πf/fs)`，Nyquist 处最多 +6.4 dB，带内拟合误差 0.03 dB。
5. **CIC 响应计算把 F>0.5 折回**：CIC 的 `|sin(πF)|/(R|sin(πF/R)|)` 关于 0.5 不对称，
   折回后把 156 kHz 的 107 dB 衰减算成 +2.4 dB。→ 不折回，只在直流单独处理。

## 4. 没做完的（如实）

- **DirectSDM 还没接进播放器**：核心与命令行已逐位验证；`rtaweb` 的播放线程仍走
  「解成 PCM → 升频 → 调制」。接线需要把 `DsdStream` 带过预解码槽，并在生产者线程里按位切片、
  直接打包 DoP；界面还要一个真开关。下一轮做，判据已现成（本轮第 2.2 节）。
- **P0-5 Conversion（wide/narrow/XFi）**：现在只有 `--kind fir|iir|cic` 三种抽取器，
  还没有映射成「截止/过渡带/阶数」三组预设。
- **界面**：SDM 页的「SDM 源（Integrator / Conversion / DirectSDM）」仍是上一轮的说明文字。
- **性能**：DSD 源仍是整曲解码进内存；DSD256 5 分钟约 700 MB PCM、耗时数十秒。
  两级抽取（CIC 先降 4 倍再 FIR）可以在下一轮做，判据用同一组指标。

## 5. 验证

- 构建 0 error；`ctest` **28/28 全过**（新增 `dsd_reader`、`dsd_path`）
- `verify.ps1` **9/9 全过**（新增第 9 项「DSD 通路」：造素材 → THD+N → DirectSDM 逐位一致）
- 界面自检 **47 项全过**（本轮没改界面，回归确认）
- 全程没有打开音频设备；播放实测用 Null 后端
- 顺带修了一个**测试自身的抖动**：`test_parallel` 的「多核更快 > 1.3x」原本单次计时，
  verify 并发下偶发失败（实测 96% 通过，唯一失败项就是它）；改成串行/并行各 3 轮取最好，
  连跑 10 次 0 失败（现值 1.94x）。「判据受负载影响」也属于铁律 3。

---

# 第六轮：内置界面窗口（2026-10-21）

> 本轮把界面从“Edge 应用窗口”换成**内置 WebView2 窗口**，一次解决标题栏配色与浏览器扩展两个问题。

## 1. 为什么要换

| 问题 | 根因 |
|---|---|
| 切主题后标题栏不跟着变 | Edge 的标题栏是它自己画的；`DWM` 属性和网页 `theme-color` 都改不了它，只能重开窗口 |
| 首次启动 Edge 弹出来装插件 | 我们用的是独立 profile（为了拿到 `--force-dark-mode`），新 profile 会走 Edge 首次运行流程 |

两个都是“借别人的窗口”带来的，换成就解决了。

## 2. 做了什么

- `tools/wv2_host.h`：用**官方 WebView2 SDK 头文件** + `WebView2Loader.dll` 创建内置浏览器控件
- `launcher.cpp`：自己注册窗口类 `RLPlayerMain`，WebView2 铺满客户区；标题栏用 DWM 按主题上色
  （深色 `#0c0e13` / 浅色 `#f4f6f9`），1.5 秒轮询一次配置**就地改色，不重开窗口**
- 缺少 WebView2 运行库时自动退回原来的 Edge 方案（功能不受影响）
- 构建期把 `WebView2Loader.dll` 复制到 exe 同目录；发布包也带上

先试过“绕过 SDK 直接调运行库内部导出”，环境能创建，但 155 版运行库的
`ICoreWebView2Environment` vtable 已经不同，创建控制器时崩在 `EmbeddedBrowserWebView.dll` 里。
改成官方头文件后不再有 ABI 猜测。

## 3. 实测

- 启动日志：`已用内置 WebView2 打开界面（无浏览器外壳）`，`msedge --app` 窗口数 **0**
- 标题栏像素：深色 `12,14,19` → 切浅色后 `244,246,249`，日志 `标题栏跟随主题 -> 浅色`，**窗口数仍为 1**
- 发布包冒烟：解压后双击 → 窗口标题 `RLPlayer`、`/api/ping` 正常

## 4. 发布包

    dist\RLPlayer-1.0.0-win64.zip     0.63 MB
    ├ RLPlayer.exe        99.5 KB   启动器 + 内置界面窗口
    ├ WebView2Loader.dll 162.1 KB   内置界面必需
    ├ rtaweb.exe          1.1 MB    播放服务
    ├ rtagpu.dll        179.5 KB    AMD GPU（可选）
    ├ asio_probe.exe     48.5 KB    ASIO 预检（可选）
    └ README.txt

---
# RLPlayer 状态（2026-10-21 · 第五轮：BUG 修复 + 发布包）

> 本轮：停止后再播放、切主题弹出新窗口两个 BUG；参数生效全量核查；最小发布包。

## 1. 两个 BUG

### 1.1 停止后点播放没反应、采样率也不切换

原因：底栏播放键只会发 `/api/pause`。`/api/stop` 之后 `playing=false`，pause 对空管线毫无作用，
所以既不起播也不会重新下发参数（采样率因此永远停在上一首）。

修法：`playOrPause()` —— 没在播时，从队列当前项（或上次曲目）重新 `playPath()`，
参数随 URL 一起下发，采样率/滤波器/位深自然生效。整页的播放键复用同一个函数。

回归测试（`build/_playtest.js`，Null 后端，不出声）：

    点播放 → playing=true, outRate=96000
    停止   → playing=false
    改 48000 后直接点播放 → playing=true, outRate=48000

### 1.2 切主题会弹出新窗口

原因：上一轮为了让标题栏配色跟着主题，加了「每 1.5 秒读 conf，深浅一变就关窗重开」的监听。
修法：删掉监听。标题栏配色改为**下次启动**时按当时主题选 profile（深/浅两套），不再中途重开。

验证：切浅色再切回深色，`msedge --app` 窗口数 1 → 1，launcher.log 无新增行。

## 2. 参数生效全量核查

用 `/api/signal`（报告的是管线实际参数）+ Null 后端逐项验证，**21 项全过**：

| 参数 | 实测 |
|---|---|
| 采样率 96000 / 保持源 | outRate=96000 / 44100 |
| 位深 16 / 32 | outBits=16 / 32 |
| 滤波器 + 抽头跟随 | filter=poly-sinc-short-lp, taps=4095 |
| 抽头覆盖 8192 | taps=8192 |
| 相位 linear / intermediate | phaseCode=linear / intermediate |
| 两段式关闭 | twoStage=false |
| 输出声道 6 | outCh=6 |
| DSD64（DoP） | dsdRate=2822400 |
| SDM 专用滤波器 | filter=poly-sinc-short-mp（与 PCM 独立） |
| 等响度 | loudness=true |
| 关闭 GPU | gpuAvailable=false |
| ISP | ispPeak 有值 / 关时为 0 |
| 缓冲 200 / 渐变 500 / 音量 -12 / 阻带 100 | buffer=200 / volRamp=500 / volume=-12 / atten=100 |
| 抖动 TPDF | shaper=TPDF |
| 设置持久化（sdmfilt） | 写入后读回一致 |

顺带：`/api/signal` 新增 `phaseCode` 与 `gpuAvailable` 两个机器可读字段，
相位报告也补上了「混合相位」（之前会误报成线性相位）。

## 3. 最小发布包

    dist\RLPlayer-1.0.0-win64.zip        0.54 MB
    ├ RLPlayer.exe      启动器（托盘 + 开界面）
    ├ rtaweb.exe        播放服务（必须同目录）
    ├ rtagpu.dll        AMD GPU 加速（可选）
    ├ asio_probe.exe    ASIO 预检（可选）
    └ README.txt

冒烟测试：解压到临时目录，`rtaweb.exe --port 8931` → `/api/ping` 正常、首屏 32.9 KB 且含分组页签。

---
# RLPlayer 状态（2026-10-20 深夜 · 第四轮）

> 本轮 8 条：设置页骨架、SDM 设置补齐、封面/歌词读取、正在播放页、备份。

## 1. 逐条

| # | 问题 | 处理 |
|---|---|---|
| 1 | 页签滚动时与「设置」标题重叠 | ✅ 设置模式下隐藏 crumb（`#app.setmode`），页签成为唯一吸顶行 |
| 2 | 页签顺序 | ✅ 改为 输入 / 输出 / PCM / SDM / 高级，默认停在输入 |
| 3 | SDM 页缺 Oversampling | ✅ 加 Oversampling 1x / Nx（与 PCM 独立，DSD 输出时生效）、比特率（64~1024）、封装（DoP/Native） |
| 4 | 声道微调每页都有 | ✅ 根因：输出页里多了一个 `</div>`，把那段挤出了页签容器（`closest('.spane')` 为 null）。修好后只在输出页 |
| 5 | 流媒体账号密码 | ✅ 「高级」页新增流媒体服务器（类型/地址/用户名/密码/保存并测试），并写明 DLNA 与 Emby 是两件事 |
| 6 | 正在播放页返回失效、无控件 | ✅ 返回键被遮罩层盖住（补 z-index），并加 Esc；底部加 进度条/上一首/播放/下一首/停止 |
| 7 | 内置封面与歌词读不到 | ✅ 歌词：ffprobe 的 `-show_entries` 语法写错（`format_entries` 不是合法字段），改成读 `format_tags` 的 JSON 再按 lyric/uslt 匹配；封面链路实测正常（见下） |
| 8 | 备份 | ✅ `D:\RTA-hqplayer-backup-20261020-2220.zip`（2.5 MB，源码/工具/测试/文档/资源） |

## 2. 内置封面 / 歌词 的实测

用一个现造的测试文件（FLAC + 内嵌 PNG + lyrics 标签）：

- `/api/lyrics` → `{"ok":true,"src":"embedded","lrc":"[00:00.00]first line..."}`
- `/api/cover` → 200 / image/png / 3276 字节（从音频里抽出来的图）
- `/api/meta` → `hasCover:true, title:"Embedded Cover Test"`

## 3. 还没对齐 HQPlayer 的设置（如实）

HQPlayer 的 SDM 页还有：40 多个专有调制器名（ASDM7EC / AMSDM7EC / AHHM5EC4B …）、
DSD 源的 Integrator（IIR/FIR/CIC）与 Conversion（wide/narrow/XFi）、DirectSDM。

这些**没有加**，因为它们在我们的管线里没有对应实现：

- 专有调制器是 HQPlayer 的内部算法，写个同名下拉只会是假开关；
- DSD 源处理需要 DSD 域的抽取/插值（现在 DSD 文件是 ffmpeg 解成 PCM 再调制），
  `DirectSDM` 直通同样依赖这条通路；
- 要做就得先实现 DSP，再上控件 —— 顺序不能反过来。

## 4. 验证

- 界面自检 **47 项全过**（新增：页签顺序、微调只在一页、SDM 有 Oversampling/比特率、正在播放页有控件）
- 设置页 13 组 / 45 个控件
- 页面加载 0 JS 异常（之前 `srcSave` 被移出首屏后未判空，导致整页脚本中断、专辑网格不渲染）

---
# RLPlayer 状态（2026-10-20 夜 · 第三轮）

> 本轮全部是界面与交互问题，13 条逐条修完。DSP 只加了一项：混合相位。

## 1. 这一轮改了什么

| # | 问题 | 处理 |
|---|---|---|
| 1 | 拖音乐进窗口不能入队 | ✅ 全窗口拖拽：遮罩提示 + 路径解析；拿不到路径时把文件内容 POST 到 `/api/upload` 落到 `build\uploads` 再入队。拖目录会递归展开 |
| 2 | 艺术家没图标 / 曲目图标怪 | ✅ 全部换新图标。根因是 `<circle cx=12 .../>` 这种**无引号属性**：`/>` 被并进属性值（变成 `9/`），半径解析失败 → 图标空白 |
| 3 | 文件夹不能输路径、样式丑 | ✅ 加地址栏（可直接输入目录 / 音乐库下拉 / 上一级），行样式改成图标 + 目录高亮 |
| 4 | 播放队列点不开、不能增删 | ✅ 播放队列变成主视图：序号 / 标题 / 目录 / 每行 × 删除 / 拖动排序 / 清空。右键曲目也能入队 |
| 5 | 设置和播放队列混在 PLAYBACK | ✅ 左栏分三组：浏览 / 播放 / 系统，设置单独压在最底下 |
| 6 | 下面又多一个 MUSIC FOLDERS | ✅ 删除；库根目录只在设置→高级里管理；PLAYLISTS 也删了（播放列表就用队列） |
| 7 | 播放键只有 4 个 | ✅ 左侧加循环方式键（关 / 列表 / 单曲），共五个 |
| 8 | 想点封面进单曲页 | ✅ 点底栏封面/曲名打开「正在播放」整页：模糊封面背景、大封面、曲名/艺人/专辑、滚动歌词（当前行高亮） |
| 9 | 声道微调 1-8 不知道是哪个 | ✅ 按声道布局显示并标注：2 = 左/右；5.1 = 左/右/中置/低音/左环绕/右环绕；7.1 再加左后/右后。改声道数时标签即时重排 |
| 10 | PCM 页又有一份声道微调 | ✅ 只保留输出页一处，右栏那份重复控件已删除 |
| 11 | 相位只有两个 | ✅ 增加「混合相位（线性/最小 各 50%）」 |
| 12 | 设置页右边还挂着队列/歌词 | ✅ 设置页占满整屏，右栏隐藏（走 `#app.noq` 网格规则） |
| 13 | 左右两边参数重复 | ✅ 右栏的快速设置整块移除；参数只在设置页出现，右栏只剩队列/歌词两个标签 |

## 2. 混合相位是怎么做的（不是随便加权）

`H_mix(ω) = H_min(ω) · e^{-jωτ}`，`τ = a·(L-1)/2`：在最小相位滤波器上加**部分线性相位延迟**。

- 幅度**一点不变**（只乘单位幅度的相位因子）
- 群延迟落在最小相位与线性相位之间
- 试过两种错误做法并记录在代码注释里：① 两段冲激响应加权 —— 相位相消，幅频毁了；
  ② 主值相位逐点加权 —— 有 2π 折叠、且截断后通带误差 20 dB
- 测试 `test_filters` 新增三项：幅频一致（0.023 dB，来自尾部截断，已在注释里说明）、
  能量重心严格居中（2047 → 1047 → 23.6）、a=0/1 退化为两个端点

## 3. 验证

- `ctest` **26 套全过**
- 界面自检扩展到 **43 项**：新增队列页 / 地址栏 / 正在播放页 / 循环键 / 拖拽层 / 设置页隐藏右栏
- 目录入队实测：`/api/add` 传一个目录 → 递归展开 75 个音频文件、返回 total=82
- 上传实测：POST 132344 字节 → 落盘 `build\uploads\dropped_test.wav`，返回路径

## 4. 仍然没做的

输入采集、UPnP 投放、DSD 源升频（见下一节的历史记录）。

---
# RLPlayer 状态（2026-10-20 晚 · 第二轮）

> 自研 Windows 本地高保真播放器。DSP 全部自研（升频 / 抖动 / 噪声整形 / SDM / 卷积 / 矩阵），
> WASAPI 独占 + ASIO + NAA 三种输出后端，AMD GPU 参与升频与卷积。

---

## 1. 这一轮做了什么（对应你提的 10 条）

| # | 你的要求 | 状态 |
|---|---|---|
| 1 | 继续开发 | ✅ 见下 |
| 2 | 标题栏颜色跟本体统一 | ✅ 已改（Edge `--force-dark-mode`） |
| 3 | 加 ASIO 和 UPNP/NAA | ⚠️ ASIO ✅ 已实现并实测出声；NAA ✅ 已实现；UPnP 投放 ❌ 未做 |
| 4 | 输入功能 | ❌ 未实现（设置页有输入页签与设备枚举，采集通路没写） |
| 5 | 多声道 5.1/7.1 | ✅ DSP 与声道掩码已实现并测过；本机没有 6 声道设备做端到端 |
| 6 | 左右声道音量微调 | ✅ 每声道增益（8 路）+ 自动化测试 |
| 7 | DSD 到 1024 | ✅ 调制器支持并离线验证；本机 DAC 最高 384 kHz，跑不到 |
| 8 | 1 bit DAC / 位深 | ✅ 界面已区分：DSD 恒 1 bit，位深只对 PCM 生效 |
| 9 | 设置界面分页 | ✅ 输出 / PCM / SDM / 输入 / 高级 五页 |
| 10 | SDM 升频（DSD 源） | ❌ 未实现（当前 DSD 源先解成 PCM 再调制） |

### 1.1 输出后端

`make_sink()` 把播放线程与具体后端解耦：

| 后端 | 状态 | 实测 |
|---|---|---|
| WASAPI（独占 · 事件驱动） | ✅ | 44.1k~384k，多声道走 WAVEFORMATEXTENSIBLE 声道掩码 |
| ASIO | ✅ 本轮新实现 | Audiobyte ASIO：96 kHz / 3 秒 / 288768 帧 / **0 欠载**；DSD128 DoP（352.8 kHz 载波）整段播完 |
| NAA（RTANet） | ✅ 本轮新实现 | 1 秒整曲推到 rtanet_rx，落盘、零错误 |
| Null output | ✅ 本轮新实现 | 全程跑 DSP、不出声，进度按实时走 |

### 1.2 多声道与微调

- 管线新增「每声道增益」（`ChannelProcess::gainDb`），0 dB 不进入处理（保证逐位直通）
- WASAPI 声道掩码：1/2/4/6/8 声道分别用对应 SPEAKER_* 组合（5.1 = 0x3F，7.1 = 0x63F）
- 设置页「输出声道数」可选 跟随源 / 2 / 5.1 / 7.1；声道微调 8 路各 −24…+12 dB
- 新测试 `test_channel_gain`：−6.0206 dB 正好 0.5 倍、其它声道逐位不变、2ch→6ch 上混为补静音

### 1.3 DSD

- 下拉扩到 DSD512 / DSD1024（DoP 与 Native 两套）
- 离线验证：DSD1024（45.1584 MHz）调制 0.5 秒音频耗时 626 ms（**0.8 倍实时**），写出 DoP 载波 2.8224 MHz
- 界面明确写：DSD 输出恒为 1 bit，输出位深只对 PCM 生效

---

## 2. 这一轮查出的真 bug

### 2.1 ASIO 从来没真正工作过（最重要）

`ASIOCallbacks` 是 `open()` 里的**局部变量**，而 `createBuffers` 只保存指针、之后在驱动线程里才去读。
函数一返回栈帧就没了，驱动读到的是野内存 —— 表现是：`start()` 返回成功、**回调一次都不触发**、
约 2 秒后驱动线程访问冲突。

更糟的是旧判据看不出来：`欠载 0 次` 之所以是 0，是因为回调根本没跑（欠载只会在回调里计数）。
这是「测试通过不等于测对了东西」的又一个实例。改成成员后立刻正常：3 秒 288768 帧、0 欠载。

### 2.2 第三方 ASIO 驱动会带崩整个播放器

有的驱动（本机 Audiobyte 在 705.6 kHz 下）在 `start()` 里直接访问冲突。
现在：① 用 SEH 包住 `start()`；② 首次使用某驱动时先用 `asio_probe.exe` 在**子进程**里试开 0.7 秒，
崩溃结果写进 `rlplayer-asio-cache.txt` 缓存，此后直接拒绝并给出原因，播放器本体不受影响。

### 2.3 其它

- ASIO 样本格式必须按「管线位深」归一化：管线 int32 是 24-bit 定标，直接拷进 Int32LSB 会小 48 dB
- `getClockSources(nullptr, &n)` 在部分驱动上非法（会崩），改成给数组 + 容量
- 硬件采样率查询只对 WASAPI 做：NAA/Null 没有本地设备可查，`max` 按源采样率

---

## 3. 硬件实测（Audiobyte USB DAC）

### 3.1 能力矩阵（只读探测）

| 路径 | 支持 |
|---|---|
| WASAPI 独占 | PCM 16/24/32 bit，44.1k ~ **384 kHz**；无 705.6k |
| ASIO（AudioByte ASIO Driver） | 16 进 / 16 出，Int32LSB，44.1k ~ 384k 可开流；**705.6 kHz 在 start() 崩溃** |
| DoP 可达 | **DSD128**（载波 352.8 kHz）；DSD256 需要 705.6 kHz，本机驱动不支持 |
| Native DSD | ❌ 驱动通道类型是 Int32LSB，不是 ASIODSDInt8* |

### 3.2 端到端（全部 −70 dBFS、2 秒，不可听）

- ASIO 96 kHz PCM：整曲播完，pos 到 2.0，无错误
- ASIO DSD128（DoP，352.8 kHz 载波）：整曲播完，44 个写入周期，无错误
- ASIO DSD256（需要 705.6 kHz）：被子进程预检拦下，返回明确原因，播放器存活

---

## 4. 界面

- 设置页分成五页：**输出 / PCM / SDM / 输入 / 高级**（对齐 HQPlayer 的 Inputs/Outputs/PCM/SDM/Advanced）
- 输出页：后端 / NAA 端点 / 设备 / 输出模式 / 采样率 / 位深 / 声道数 / 缓冲 / 渐变 / 8 路声道微调
- SDM 页：DSD 输出模式 / 调制器阶数 / 拐点 / 抖动；「SDM 源」单独一块并写明当前未实现
- 输入页：后端（none/WASAPI）/ 设备 / 声道偏移 / 缓冲 / 监听（采集通路未实现，页面照实说）
- 标题栏：`--force-dark-mode` 让 Edge 原生边框变 #08090c

---

## 5. 测试

- `ctest` **26 套全过**（新增 `channel_gain`、`gpu_bridge`）
- 界面自检 34 项全过
- `verify.ps1` 8 项全过

---

## 6. 仍然没做的（如实）

1. **输入采集**：只做了设备枚举与界面，没有采集→DSP→输出的通路。
2. **UPnP/DLNA 投放**：只有发现，没有推送。
3. **SDM 源升频**：DSD 文件仍是 ffmpeg 解成 PCM 再调制；没有 DirectSDM 直通，也没有 DSD 域抽取/插值。
4. **NVIDIA CUDA 后端**：只有选项与加载路径，仓库里没有 CUDA 源码（本机无 N 卡、无 nvcc）。
5. **Native DSD**：需要驱动的 ASIO 通道类型是 DSD；本机 DAC 的驱动不是。
6. **DSD512/1024 出声**：调制器能算（0.8 倍实时），但没有 DAC 支持，无法端到端验证。