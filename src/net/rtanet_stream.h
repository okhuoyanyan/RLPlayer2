// rtanet_stream.h — RTANet 控制面（TCP 握手协商）与收发驱动
//
// 控制面用【行式文本】：可读、可用 telnet 手工调试，且便于日后对齐 NAA 的 XML 风格。
//   C->S  HELLO RTANET/1
//   S->C  OK RTANET/1 <name>
//   C->S  FORMAT rate=48000 bits=24 channels=2
//   S->C  OK FORMAT                     (不支持则 ERR FORMAT ...)
//   C->S  START stream=1
//   S->C  OK START port=<udp 端口>
//   ...UDP 数据流...
//   C->S  STOP    /  PING  /  BYE
#pragma once
#include "rtanet.h"
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstdio>

namespace rta::net {

struct RtaFormat {
    uint32_t rate = 48000;
    int bits = 24;
    int channels = 2;
    bool operator==(const RtaFormat& o) const {
        return rate == o.rate && bits == o.bits && channels == o.channels;
    }
    std::string str() const {
        char b[96];
        std::snprintf(b, sizeof(b), "%u/%d/%d", rate, bits, channels);
        return b;
    }
};

// ---------------- 最小 TCP 封装 ----------------
class TcpSocket {
public:
    bool listen(uint16_t port) {
        ensure_wsa();
        s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_ == RTA_INVALID_SOCK) return false;
        int yes = 1;
        ::setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);      // 默认仅本机；跨机时传 INADDR_ANY
        if (::bind(s_, (sockaddr*)&a, sizeof(a)) != 0) { close(); return false; }
        if (::listen(s_, 4) != 0) { close(); return false; }
        listening_ = true;
        return true;
    }
    bool listen_any(uint16_t port) {
        bool ok = listen(0);
        if (!ok) return false;
        close();
        ensure_wsa();
        s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_ == RTA_INVALID_SOCK) return false;
        int yes = 1;
        ::setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s_, (sockaddr*)&a, sizeof(a)) != 0) { close(); return false; }
        if (::listen(s_, 4) != 0) { close(); return false; }
        listening_ = true;
        return true;
    }
    bool accept_from(TcpSocket& out, int timeoutMs = 2000) {
        if (!listening_) return false;
        // 用 select 做超时等待，避免测试永久阻塞
        fd_set fds; FD_ZERO(&fds); FD_SET(s_, &fds);
        timeval tv{}; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (::select((int)(s_ + 1), &fds, nullptr, nullptr, &tv) <= 0) return false;
        sockaddr_in a{}; 
#ifdef _WIN32
        int alen = sizeof(a);
#else
        socklen_t alen = sizeof(a);
#endif
        out.s_ = ::accept(s_, (sockaddr*)&a, &alen);
        return out.s_ != RTA_INVALID_SOCK;
    }
    bool connect_to(const std::string& host, uint16_t port) {
        ensure_wsa();
        s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_ == RTA_INVALID_SOCK) return false;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = ::inet_addr(host.c_str());
        if (::connect(s_, (sockaddr*)&a, sizeof(a)) != 0) { close(); return false; }
        return true;
    }
    bool send_line(const std::string& s) {
        const std::string t = s + "\n";
        return ::send(s_, t.c_str(), (int)t.size(), 0) == (int)t.size();
    }
    bool recv_line(std::string& out, int timeoutMs = 2000) {
        out.clear();
        fd_set fds; FD_ZERO(&fds); FD_SET(s_, &fds);
        timeval tv{}; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
        for (;;) {
            const size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                out = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                while (!out.empty() && (out.back() == '\r' || out.back() == ' ')) out.pop_back();
                return true;
            }
            if (::select((int)(s_ + 1), &fds, nullptr, nullptr, &tv) <= 0) return false;
            char tmp[512];
            const int r = ::recv(s_, tmp, sizeof(tmp), 0);
            if (r <= 0) return false;
            buf_.append(tmp, (size_t)r);
        }
    }
    uint16_t local_port() const {
        sockaddr_in a{};
#ifdef _WIN32
        int alen = sizeof(a);
#else
        socklen_t alen = sizeof(a);
#endif
        if (::getsockname(s_, (sockaddr*)&a, &alen) != 0) return 0;
        return ntohs(a.sin_port);
    }
    void close() { if (s_ != RTA_INVALID_SOCK) { rta_close_sock(s_); s_ = RTA_INVALID_SOCK; } listening_ = false; }
    ~TcpSocket() { close(); }
private:
    static void ensure_wsa() {
#ifdef _WIN32
        static bool done = false;
        if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; }
#endif
    }
    sock_t s_ = RTA_INVALID_SOCK;
    bool listening_ = false;
    std::string buf_;
};

// ---------------- 控制面 ----------------
// 服务端（跑在端点机）
class RtaControlServer {
public:
    // 返回协商结果；err 非空表示失败原因
    bool serve(TcpSocket& conn, RtaFormat& fmt, uint32_t& streamId, std::string& err,
               const RtaFormat& supported, uint16_t udpPort) {
        std::string line;
        if (!conn.recv_line(line) || line.rfind("HELLO", 0) != 0) { err = "缺少 HELLO"; return false; }
        if (!conn.send_line("OK RTANET/1 rta-endpoint")) { err = "发送失败"; return false; }

        if (!conn.recv_line(line) || line.rfind("FORMAT", 0) != 0) { err = "缺少 FORMAT"; return false; }
        RtaFormat req = parse_format(line);
        if (!(req == supported)) {
            err = "不支持的格式 " + req.str() + "（本端点支持 " + supported.str() + "）";
            conn.send_line("ERR FORMAT " + err);
            return false;
        }
        fmt = req;
        if (!conn.send_line("OK FORMAT")) { err = "发送失败"; return false; }

        if (!conn.recv_line(line) || line.rfind("START", 0) != 0) { err = "缺少 START"; return false; }
        streamId = parse_kv_u32(line, "stream", 1);
        char b[96]; std::snprintf(b, sizeof(b), "OK START port=%u", (unsigned)udpPort);
        if (!conn.send_line(b)) { err = "发送失败"; return false; }
        return true;
    }

    static RtaFormat parse_format(const std::string& line) {
        RtaFormat f;
        const size_t p = line.find(' ');
        if (p == std::string::npos) return f;
        std::istringstream is(line.substr(p + 1));
        std::string tok;
        while (is >> tok) {
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
            if (k == "rate") f.rate = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
            else if (k == "bits") f.bits = std::atoi(v.c_str());
            else if (k == "channels") f.channels = std::atoi(v.c_str());
        }
        return f;
    }
    static uint32_t parse_kv_u32(const std::string& line, const char* key, uint32_t def) {
        const size_t p = line.find(std::string(key) + "=");
        if (p == std::string::npos) return def;
        return (uint32_t)std::strtoul(line.c_str() + p + std::strlen(key) + 1, nullptr, 10);
    }
};

// 客户端（跑在 DSP 机）
class RtaControlClient {
public:
    bool negotiate(const std::string& host, uint16_t port, const RtaFormat& want,
                   uint16_t& udpPortOut, uint32_t streamId, std::string& err) {
        if (!conn_.connect_to(host, port)) { err = "无法连接控制端口"; return false; }
        if (!conn_.send_line("HELLO RTANET/1") || !conn_.recv_line(line_) ||
            line_.rfind("OK", 0) != 0) { err = "HELLO 失败"; return false; }

        char b[128];
        std::snprintf(b, sizeof(b), "FORMAT rate=%u bits=%d channels=%d",
                      want.rate, want.bits, want.channels);
        if (!conn_.send_line(b) || !conn_.recv_line(line_)) { err = "FORMAT 无响应"; return false; }
        if (line_.rfind("OK", 0) != 0) { err = "端点拒绝格式: " + line_; return false; }

        std::snprintf(b, sizeof(b), "START stream=%u", streamId);
        if (!conn_.send_line(b) || !conn_.recv_line(line_)) { err = "START 无响应"; return false; }
        if (line_.rfind("OK", 0) != 0) { err = "START 被拒绝: " + line_; return false; }
        udpPortOut = (uint16_t)RtaControlServer::parse_kv_u32(line_, "port", 0);
        if (udpPortOut == 0) { err = "端点未返回 UDP 端口"; return false; }
        return true;
    }
    bool stop() { return conn_.send_line("STOP"); }
    void close() { conn_.close(); }
    TcpSocket& raw() { return conn_; }
private:
    TcpSocket conn_;
    std::string line_;
};

// ---------------- 发送驱动 ----------------
class RtaSender {
public:
    void configure(const std::string& host, uint16_t port, const RtaFormat& fmt,
                   uint32_t streamId, size_t framesPerPacket = 128) {
        host_ = host; port_ = port; fmt_ = fmt; streamId_ = streamId;
        framesPerPacket_ = framesPerPacket;
        seq_ = 0; ts_ = 0; sent_ = 0;
        sock_.open(0);
    }
    // 交错样本；内部按 framesPerPacket 分包
    size_t send(const int32_t* samples, size_t frames) {
        size_t done = 0;
        std::vector<int32_t> tmp;
        while (done < frames) {
            const size_t n = std::min(framesPerPacket_, frames - done);
            const uint16_t flags = (seq_ == 0) ? RTA_FLAG_START : RTA_FLAG_NONE;
            auto pkt = build_packet(seq_, ts_, fmt_.rate, fmt_.channels, fmt_.bits,
                                    samples + done * (size_t)fmt_.channels, n, streamId_, flags);
            if (!sock_.send_to(pkt.data(), pkt.size(), host_, port_)) break;
            ++seq_; ts_ += n; done += n; sent_ += n;
        }
        return done;
    }
    uint32_t next_seq() const { return seq_; }
    size_t frames_sent() const { return sent_; }
    size_t packet_size_bytes() const {
        return RTANET_HEADER + packed_bytes(fmt_.bits, framesPerPacket_, fmt_.channels);
    }
private:
    UdpSocket sock_;
    std::string host_; uint16_t port_ = 0;
    RtaFormat fmt_; uint32_t streamId_ = 1;
    size_t framesPerPacket_ = 128;
    uint32_t seq_ = 0; uint64_t ts_ = 0; size_t sent_ = 0;
};

} // namespace rta::net
