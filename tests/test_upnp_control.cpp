// test_upnp_control.cpp — UPnP 投放控制面与 HTTP 供源测试（全离线回归）
//
// 契约要点（ROADMAP P5a）：
//   1) SOAP 请求 XML 生成（转义/必填字段）
//   2) SOAP 响应/错误解析（含 UPnPError 代码与描述）
//   3) DIDL-Lite 生成与解析（XML 转义、采样率/位深属性）
//   4) HTTP 供源 Range 语义（200 / 206 / 416 状态码与 Content-Range）
//   5) Token 防目录遍历（拒绝 .. 和非法后缀）
//   6) 采样率取证三态判定（Match / Mismatch / Unknown，告警非 bit-perfect）
//   7) 本地 HTTP 供源回环实测（端口 8910，Range 分段拉取逐字节校验）
//   8) 网络发现用例 opt-in（RTA_NET_TESTS=1 时才发真实 SSDP）
#include "net/upnp.h"
#include "net/upnp_control.h"
#include "net/http_client.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif

using namespace rta::net;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s  %s\n", what, detail.c_str());
        ++g_fail;
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
    std::printf("=== test_upnp_control: UPnP 投放控制面 + HTTP 供源全离线测试 ===\n\n");

    // ================= 1) XML 转义与 DIDL-Lite 生成与解析 =================
    std::printf("-- 1. XML 转义与 DIDL-Lite 元数据 --\n");
    {
        std::string raw = "<Rock & Roll \"Live\" '2026'>";
        std::string esc = xml_escape(raw);
        ck(esc == "&lt;Rock &amp; Roll &quot;Live&quot; &apos;2026&apos;&gt;",
           "XML 转义字符处理", esc);
        std::string unesc = xml_unescape(esc);
        ck(unesc == raw, "XML 反转义还原逐字一致", unesc);
    }
    {
        UpnpDidlItem item;
        item.id = "0";
        item.title = "ZARD - 负けないで";
        item.creator = "ZARD";
        item.album = "HOLD ME";
        item.uri = "http://192.168.1.100:8912/stream/feedcafe12345678.wav";
        item.sampleRate = 96000;
        item.bits = 24;
        item.channels = 2;
        item.durationSec = 225.5;

        std::string didlXml = build_didl_lite(item);
        ck(didlXml.find("<DIDL-Lite") != std::string::npos, "DIDL-Lite 根标签存在");
        ck(didlXml.find("<dc:title>ZARD - 负けないで</dc:title>") != std::string::npos, "包含标题");
        ck(didlXml.find("<dc:creator>ZARD</dc:creator>") != std::string::npos, "包含艺术家");
        ck(didlXml.find("sampleFrequency=\"96000\"") != std::string::npos, "包含采样率 96000");
        ck(didlXml.find("bitsPerSample=\"24\"") != std::string::npos, "包含位深 24");
        ck(didlXml.find("nrAudioChannels=\"2\"") != std::string::npos, "包含声道数 2");
        ck(didlXml.find("duration=\"00:03:45\"") != std::string::npos, "包含时长 00:03:45");

        UpnpDidlItem parsed = parse_didl_lite(didlXml);
        ck(parsed.title == "ZARD - 负けないで", "DIDL 解析标题一致", parsed.title);
        ck(parsed.creator == "ZARD", "DIDL 解析艺术家一致", parsed.creator);
        ck(parsed.album == "HOLD ME", "DIDL 解析专辑一致", parsed.album);
        ck(parsed.sampleRate == 96000, "DIDL 解析采样率一致", std::to_string(parsed.sampleRate));
        ck(parsed.bits == 24, "DIDL 解析位深一致", std::to_string(parsed.bits));
        ck(parsed.channels == 2, "DIDL 解析声道数一致", std::to_string(parsed.channels));
        ck(parsed.durationSec >= 225.0 && parsed.durationSec <= 226.0, "DIDL 解析时长一致");
        ck(parsed.uri == item.uri, "DIDL 解析 URI 一致", parsed.uri);

        // 转义后的 DIDL 也必须能正确解析
        std::string escapedDidl = xml_escape(didlXml);
        UpnpDidlItem parsedEsc = parse_didl_lite(escapedDidl);
        ck(parsedEsc.sampleRate == 96000 && parsedEsc.title == "ZARD - 负けないで",
           "带 &lt; 实体转义的 DIDL 正确解析");
    }

    // ================= 2) SOAP 请求 XML 生成（转义/必填字段） =================
    std::printf("\n-- 2. SOAP 请求 XML 生成与必填字段 --\n");
    {
        std::string req = build_set_av_transport_uri_soap(0, "http://host:8912/stream/a.wav", "<DIDL-Lite/>");
        ck(req.find("<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">") != std::string::npos,
           "SetAVTransportURI 服务与动作标签");
        ck(req.find("<InstanceID>0</InstanceID>") != std::string::npos, "InstanceID 必填字段");
        ck(req.find("<CurrentURI>http://host:8912/stream/a.wav</CurrentURI>") != std::string::npos, "CurrentURI 字段");
        ck(req.find("<CurrentURIMetaData>&lt;DIDL-Lite/&gt;</CurrentURIMetaData>") != std::string::npos,
           "CurrentURIMetaData XML 转义字段");
    }
    {
        std::string playReq = build_play_soap(0, "1");
        ck(playReq.find("<u:Play") != std::string::npos, "Play 动作标签");
        ck(playReq.find("<Speed>1</Speed>") != std::string::npos, "Play Speed 字段");
    }
    {
        std::string pauseReq = build_pause_soap(0);
        ck(pauseReq.find("<u:Pause") != std::string::npos, "Pause 动作标签");
    }
    {
        std::string stopReq = build_stop_soap(0);
        ck(stopReq.find("<u:Stop") != std::string::npos, "Stop 动作标签");
    }
    {
        std::string seekReq = build_seek_soap(0, "REL_TIME", "00:01:23");
        ck(seekReq.find("<u:Seek") != std::string::npos, "Seek 动作标签");
        ck(seekReq.find("<Unit>REL_TIME</Unit>") != std::string::npos, "Seek Unit 字段");
        ck(seekReq.find("<Target>00:01:23</Target>") != std::string::npos, "Seek Target 字段");
    }
    {
        std::string protoReq = build_get_protocol_info_soap();
        ck(protoReq.find("<u:GetProtocolInfo xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">") != std::string::npos,
           "GetProtocolInfo ConnectionManager 命名空间");
    }

    // ================= 3) SOAP 响应/错误解析（含 UPnPError） =================
    std::printf("\n-- 3. SOAP 响应解析与 UPnPError 报错 --\n");
    {
        std::string okTransport =
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body>"
            "<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
            "<CurrentTransportState>PLAYING</CurrentTransportState>"
            "<CurrentTransportStatus>OK</CurrentTransportStatus>"
            "<CurrentSpeed>1</CurrentSpeed>"
            "</u:GetTransportInfoResponse>"
            "</s:Body>"
            "</s:Envelope>";
        UpnpTransportInfo t = parse_transport_info_response(okTransport);
        ck(t.ok, "GetTransportInfo 解析成功");
        ck(t.state == "PLAYING", "状态为 PLAYING", t.state);
        ck(t.status == "OK", "Status 为 OK", t.status);
    }
    {
        std::string okPosition =
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body>"
            "<u:GetPositionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
            "<Track>1</Track>"
            "<TrackDuration>00:04:12</TrackDuration>"
            "<TrackURI>http://192.168.1.50:8912/stream/token.wav</TrackURI>"
            "<RelTime>00:01:30</RelTime>"
            "<AbsTime>00:01:30</AbsTime>"
            "</u:GetPositionInfoResponse>"
            "</s:Body>"
            "</s:Envelope>";
        UpnpPositionInfo p = parse_position_info_response(okPosition);
        ck(p.ok, "GetPositionInfo 解析成功");
        ck(p.track == 1, "Track 为 1");
        ck(p.trackDuration == "00:04:12", "TrackDuration 为 00:04:12");
        ck(p.relTime == "00:01:30", "RelTime 为 00:01:30");
    }
    {
        std::string okProto =
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body>"
            "<u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
            "<Source></Source>"
            "<Sink>http-get:*:audio/wav:*,http-get:*:audio/L16;rate=44100;channels=2:*</Sink>"
            "</u:GetProtocolInfoResponse>"
            "</s:Body>"
            "</s:Envelope>";
        UpnpProtocolInfo pr = parse_protocol_info_response(okProto);
        ck(pr.ok, "GetProtocolInfo 解析成功");
        ck(pr.sink.find("audio/wav") != std::string::npos, "Sink 包含 audio/wav");
    }
    {
        // 渲染器报错 714 (Illegal MIME-Type) 绝不许静默
        std::string faultXml =
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body>"
            "<s:Fault>"
            "<faultcode>s:Client</faultcode>"
            "<faultstring>UPnPError</faultstring>"
            "<detail>"
            "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
            "<errorCode>714</errorCode>"
            "<errorDescription>Illegal MIME-Type</errorDescription>"
            "</UPnPError>"
            "</detail>"
            "</s:Fault>"
            "</s:Body>"
            "</s:Envelope>";
        UpnpError err;
        bool hasErr = parse_upnp_error(faultXml, err);
        ck(hasErr, "正确提取 UPnP 错误");
        ck(err.code == 714, "错误码为 714", std::to_string(err.code));
        ck(err.description == "Illegal MIME-Type", "错误描述包含 Illegal MIME-Type", err.description);

        UpnpTransportInfo tFail = parse_transport_info_response(faultXml);
        ck(!tFail.ok, "带 UPnPError 的响应判定为失败 (ok=false)");
        ck(tFail.error.code == 714, "错误对象回传 714 码");
    }

    // ================= 4) HTTP 供源 Range 语义纯函数 =================
    std::printf("\n-- 4. HTTP Range 语义纯函数断言 --\n");
    {
        // 无 Range 头 -> 200 全量
        HttpRange r0 = parse_http_range("", 1000);
        ck(!r0.hasRange, "空 Range 头判定为无 Range");
        StreamResponseHeaders h0 = build_stream_response_headers(r0, 1000);
        ck(h0.statusCode == 200, "无 Range 时响应码 200");
        ck(h0.contentLength == 1000, "无 Range 时 Content-Length 为总长");
        ck(h0.headerString.find("Accept-Ranges: bytes") != std::string::npos, "响应声明 Accept-Ranges: bytes");

        // bytes=0-499 -> 206 部分内容
        HttpRange r1 = parse_http_range("bytes=0-499", 1000);
        ck(r1.hasRange && r1.valid, "bytes=0-499 合法");
        ck(r1.start == 0 && r1.end == 499, "start=0, end=499");
        StreamResponseHeaders h1 = build_stream_response_headers(r1, 1000);
        ck(h1.statusCode == 206, "Range 请求响应码 206");
        ck(h1.contentLength == 500, "分段长为 500");
        ck(h1.headerString.find("Content-Range: bytes 0-499/1000") != std::string::npos,
           "Content-Range 格式为 bytes 0-499/1000");

        // bytes=500- -> 到末尾
        HttpRange r2 = parse_http_range("bytes=500-", 1000);
        ck(r2.hasRange && r2.valid, "bytes=500- 合法");
        ck(r2.start == 500 && r2.end == 999, "start=500, end=999");
        StreamResponseHeaders h2 = build_stream_response_headers(r2, 1000);
        ck(h2.contentLength == 500, "末尾分段长度 500");
        ck(h2.headerString.find("Content-Range: bytes 500-999/1000") != std::string::npos,
           "Content-Range 格式为 bytes 500-999/1000");

        // 后缀 Range bytes=-200
        HttpRange r3 = parse_http_range("bytes=-200", 1000);
        ck(r3.hasRange && r3.valid, "bytes=-200 合法");
        ck(r3.start == 800 && r3.end == 999, "start=800, end=999");
        StreamResponseHeaders h3 = build_stream_response_headers(r3, 1000);
        ck(h3.contentLength == 200, "后缀分段长度 200");

        // 越界 Range bytes=1000- -> 416
        HttpRange r4 = parse_http_range("bytes=1000-", 1000);
        ck(r4.hasRange && !r4.valid, "越界 Range 标记为非法");
        StreamResponseHeaders h4 = build_stream_response_headers(r4, 1000);
        ck(h4.statusCode == 416, "越界 Range 响应码 416");
        ck(h4.headerString.find("Content-Range: bytes */1000") != std::string::npos,
           "416 带 Content-Range: bytes */1000");
    }

    // ================= 5) Token 防遍历机制 =================
    std::printf("\n-- 5. Token 路由与防目录遍历 --\n");
    {
        std::string tok = "deadbeef12345678";
        ck(validate_stream_token("/stream/deadbeef12345678.wav", tok), "合法 token 路由通过");
        ck(!validate_stream_token("/stream/wrongtoken12345.wav", tok), "错误 token 被拦截");
        ck(!validate_stream_token("/stream/../secret.txt", tok), "上级目录 .. 遍历被拦截");
        ck(!validate_stream_token("/stream/..\\secret.txt", tok), "反斜杠遍历被拦截");
        ck(!validate_stream_token("/stream/deadbeef12345678.mp3", tok), "非 wav 后缀被拦截");
        ck(!validate_stream_token("/etc/passwd", tok), "非 /stream/ 前缀被拦截");

        // token 随机生成长度与字符集
        std::string t1 = generate_random_token(16);
        std::string t2 = generate_random_token(16);
        ck(t1.size() == 16, "生成的随机 token 长度为 16");
        ck(t1 != t2, "两次生成的 token 不相同（具备随机性）");
    }

    // ================= 6) 采样率对照判定（一致/不一致/未知三态） =================
    std::printf("\n-- 6. 采样率取证三态判定 --\n");
    {
        // 状态 1：Match 一致
        AudioSourceFormat src1;
        src1.sampleRate = 44100;
        src1.bits = 16;
        src1.channels = 2;
        src1.encoding = "Linear PCM";

        UpnpMediaInfo media1;
        media1.currentUriMetaData =
            "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
            "<item id=\"0\"><res sampleFrequency=\"44100\" bitsPerSample=\"16\" nrAudioChannels=\"2\"/></item>"
            "</DIDL-Lite>";
        UpnpPositionInfo pos1;
        UpnpProtocolInfo proto1;
        proto1.sink = "http-get:*:audio/wav:*";

        SamplingRateAnalysis a1 = analyze_sampling_rate(src1, media1, pos1, proto1);
        ck(a1.verdict == SamplingRateVerdict::Match, "状态 1: 一致 (Match)");
        ck(a1.warning.empty(), "一致状态无告警");
        ck(a1.declaredRate == 44100 && a1.declaredBits == 16, "正确识别声明采样率与位深");

        // 状态 2：Mismatch 不一致（例如源是 96k/24b，但渲染器只声明 44.1k/16b）
        AudioSourceFormat src2;
        src2.sampleRate = 96000;
        src2.bits = 24;
        src2.channels = 2;

        UpnpMediaInfo media2;
        media2.currentUriMetaData =
            "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
            "<item id=\"0\"><res sampleFrequency=\"44100\" bitsPerSample=\"16\"/></item>"
            "</DIDL-Lite>";

        SamplingRateAnalysis a2 = analyze_sampling_rate(src2, media2, pos1, proto1);
        ck(a2.verdict == SamplingRateVerdict::Mismatch, "状态 2: 不一致 (Mismatch)");
        ck(a2.warning.find("重采样") != std::string::npos, "不一致时明确告警「重采样」", a2.warning);
        ck(a2.warning.find("非 bit-perfect") != std::string::npos, "明确标注「非 bit-perfect」", a2.warning);

        // 状态 3：Unknown 未知（渲染器只声明了通配 audio/wav，未给出具体硬件工作参数）
        AudioSourceFormat src3;
        src3.sampleRate = 96000;
        src3.bits = 24;

        UpnpMediaInfo media3; // 无 sampleFrequency
        UpnpProtocolInfo proto3;
        proto3.sink = "http-get:*:audio/wav:*,http-get:*:audio/x-wav:*";

        SamplingRateAnalysis a3 = analyze_sampling_rate(src3, media3, pos1, proto3);
        ck(a3.verdict == SamplingRateVerdict::Unknown, "状态 3: 未知 (Unknown)");
        ck(a3.detail.find("未声明具体采样率/未做端到端重采样实测") != std::string::npos,
           "未知时如实标注「未声明具体采样率/未做端到端重采样实测」", a3.detail);
    }

    // ================= 7) 本地 HTTP 供源回环实测（端口 8910） =================
    std::printf("\n-- 7. 本地 HTTP 供源回环实测 (端口 8910) --\n");
    {
        UpnpHttpServer server;
        const uint16_t testPort = 8910;
        ck(server.start(testPort, "127.0.0.1"), "本地 HTTP 服务在 127.0.0.1:8910 启动成功");

        // 构造 1000 字节特征数据
        std::vector<uint8_t> dummyData(1000);
        for (size_t i = 0; i < dummyData.size(); ++i) {
            dummyData[i] = (uint8_t)(i & 0xFF);
        }
        std::string streamPath = server.register_buffer(dummyData, "audio/wav");
        std::string baseHttpUrl = "http://127.0.0.1:8910" + streamPath;

        // a) 全量 GET 请求
        HttpResponse rFull = http_get(baseHttpUrl, {}, 5000);
        ck(rFull.ok, "全量 HTTP GET 成功 (200)");
        ck(rFull.status == 200, "状态码为 200");
        ck(rFull.body.size() == 1000, "返回字节数为 1000", std::to_string(rFull.body.size()));
        ck(rFull.body == dummyData, "全量数据逐字节完全匹配");

        // b) Range 分段请求：bytes=100-199（正好 100 字节）
        HttpResponse rRange = http_get(baseHttpUrl, {"Range: bytes=100-199"}, 5000);
        ck(rRange.ok, "Range 分段 HTTP GET 成功 (206)");
        ck(rRange.status == 206, "状态码为 206 Partial Content", std::to_string(rRange.status));
        ck(rRange.body.size() == 100, "分段返回 100 字节", std::to_string(rRange.body.size()));
        std::vector<uint8_t> expectSlice(dummyData.begin() + 100, dummyData.begin() + 200);
        ck(rRange.body == expectSlice, "分段切片逐字节完全吻合");

        // c) 后缀 Range：bytes=-200（末尾 200 字节）
        HttpResponse rSuffix = http_get(baseHttpUrl, {"Range: bytes=-200"}, 5000);
        ck(rSuffix.status == 206, "后缀 Range 状态码为 206");
        ck(rSuffix.body.size() == 200, "后缀分段返回 200 字节");
        std::vector<uint8_t> expectSuffix(dummyData.begin() + 800, dummyData.end());
        ck(rSuffix.body == expectSuffix, "后缀切片逐字节完全吻合");

        // d) 越界 Range：bytes=2000- -> 416
        HttpResponse rOob = http_get(baseHttpUrl, {"Range: bytes=2000-"}, 5000);
        ck(rOob.status == 416, "越界 Range 收到 416 响应", std::to_string(rOob.status));

        // e) 伪造 token / 目录遍历 -> 404
        HttpResponse rTraversal = http_get("http://127.0.0.1:8910/stream/../secret.txt", {}, 5000);
        ck(rTraversal.status == 404, "目录遍历请求收到 404", std::to_string(rTraversal.status));

        // f) 访问日志检查
        auto logs = server.get_access_logs();
        ck(!logs.empty(), "服务记录了访问日志");
        ck(server.total_bytes_served() >= 1300, "流量统计正确累加",
           std::to_string(server.total_bytes_served()));

        server.stop();
        ck(!server.is_running(), "HTTP 供源服务安全停止");
    }

    // ================= 8) 网络发现用例 opt-in =================
    std::printf("\n-- 8. 网络发现用例 (opt-in) --\n");
    {
        const char* netEnv = std::getenv("RTA_NET_TESTS");
        if (!netEnv || std::string(netEnv) != "1") {
            std::printf("  [SKIP] 网络发现测试已跳过（设置环境变量 RTA_NET_TESTS=1 可开启真网 SSDP 探测）\n");
        } else {
            std::printf("  [INFO] RTA_NET_TESTS=1，发起真实网络 SSDP 探测...\n");
            auto devs = upnp_discover(1500);
            std::printf("  [PASS] 真实网络 SSDP 探测完成，发现设备数: %zu\n", devs.size());
        }
    }

    std::printf("\n=======================================================\n");
    std::printf("%s（失败项 %d）\n", g_fail == 0 ? "全部测试通过" : "存在失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
