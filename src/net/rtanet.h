// rtanet.h — RTANet：自研开放网络音频协议（DSP 机 -> 端点机 -> DAC）
//
// 为什么自研而不是硬啃 NAA：
//   * NAA 无公开规范、无源码、无 Wireshark dissector（实测：手册 65 页对端口/组播提及 0 次）
//   * 更关键的是：NAA 本身不决定音质，端点时钟/供电/缓冲策略才是瓶颈（见 docs/NAA-调研报告.md）
//   * 自研可以做得更好：显式时间戳 + 抖动缓冲 + 序号，且可逐位验证
//
// 设计：
//   * UDP 数据面：定长头 + 打包 PCM（16/24/32 bit 紧凑打包，128 帧/包 < MTU）
//   * 包头含【绝对样本号】时间戳，接收端据此对齐与检测漂移（而非依赖到达顺序）
//   * 抖动缓冲：预滚 targetMs 后开始输出，缺包补静音并计数
//   * 端口：默认 43210（与 NAA 同号，便于网络设备/防火墙复用；可改）
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include <map>
#include <string>
#include <algorithm>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  #define RTA_INVALID_SOCK INVALID_SOCKET
  #define rta_close_sock closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using sock_t = int;
  #define RTA_INVALID_SOCK (-1)
  #define rta_close_sock close
#endif

namespace rta::net {

// ---------------- 包头（固定 40 字节，小端） ----------------
constexpr uint32_t RTANET_MAGIC   = 0x4E415452u;   // 'RTAN'
constexpr uint16_t RTANET_VERSION = 1;
constexpr size_t   RTANET_HEADER  = 40;

enum RtaFlags : uint16_t {
    RTA_FLAG_NONE  = 0,
    RTA_FLAG_START = 1 << 0,
    RTA_FLAG_END   = 1 << 1,
    RTA_FLAG_SILENCE = 1 << 2,
};

#pragma pack(push, 1)
struct RtaHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t seq;          // 包序号，从 0 递增
    uint64_t timestamp;    // 本包第一个样本的【绝对样本号】，用于对齐与漂移检测
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;         // 16 / 24 / 32
    uint16_t frames;       // 本包帧数
    uint16_t payloadBytes;
    uint32_t streamId;     // 流标识，便于同一端点区分多路
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(RtaHeader) == RTANET_HEADER, "包头必须恰好 40 字节");

// ---------------- 样本打包 ----------------
inline size_t packed_bytes(int bits, size_t frames, int channels) {
    return frames * (size_t)channels * (size_t)(bits / 8);
}

inline void pack_samples(uint8_t* dst, const int32_t* src, size_t n, int bits) {
    if (bits == 16) {
        auto* p = (int16_t*)dst;
        for (size_t i = 0; i < n; ++i) p[i] = (int16_t)src[i];
    } else if (bits == 24) {
        for (size_t i = 0; i < n; ++i) {
            const uint32_t v = (uint32_t)src[i];
            dst[i * 3 + 0] = (uint8_t)(v & 0xFF);
            dst[i * 3 + 1] = (uint8_t)((v >> 8) & 0xFF);
            dst[i * 3 + 2] = (uint8_t)((v >> 16) & 0xFF);
        }
    } else {
        auto* p = (int32_t*)dst;
        for (size_t i = 0; i < n; ++i) p[i] = src[i];
    }
}

inline void unpack_samples(int32_t* dst, const uint8_t* src, size_t n, int bits) {
    if (bits == 16) {
        auto* p = (const int16_t*)src;
        for (size_t i = 0; i < n; ++i) dst[i] = (int32_t)p[i];
    } else if (bits == 24) {
        for (size_t i = 0; i < n; ++i) {
            int32_t t = (int32_t)((uint32_t)src[i * 3] | ((uint32_t)src[i * 3 + 1] << 8) | ((uint32_t)src[i * 3 + 2] << 16));
            if (t & 0x800000) t |= (int32_t)0xFF000000;      // 符号扩展
            dst[i] = t;
        }
    } else {
        auto* p = (const int32_t*)src;
        for (size_t i = 0; i < n; ++i) dst[i] = p[i];
    }
}

// 构造一个数据包
inline std::vector<uint8_t> build_packet(uint32_t seq, uint64_t ts, uint32_t rate, int channels,
                                         int bits, const int32_t* samples, size_t frames,
                                         uint32_t streamId, uint16_t flags = RTA_FLAG_NONE) {
    const size_t n = frames * (size_t)channels;
    const size_t pb = packed_bytes(bits, frames, channels);
    std::vector<uint8_t> out(RTANET_HEADER + pb);
    RtaHeader h{};
    h.magic = RTANET_MAGIC; h.version = RTANET_VERSION; h.flags = flags;
    h.seq = seq; h.timestamp = ts; h.rate = rate;
    h.channels = (uint16_t)channels; h.bits = (uint16_t)bits;
    h.frames = (uint16_t)frames; h.payloadBytes = (uint16_t)pb;
    h.streamId = streamId; h.reserved = 0;
    std::memcpy(out.data(), &h, RTANET_HEADER);
    pack_samples(out.data() + RTANET_HEADER, samples, n, bits);
    return out;
}

// ---------------- 抖动缓冲 ----------------
struct JitterStats {
    uint64_t received = 0;
    uint64_t lost = 0;         // 超时补静音的包数
    uint64_t late = 0;         // 迟到被丢弃的包数
    uint64_t duplicate = 0;
    uint64_t outOfOrder = 0;   // 乱序到达的包数
    uint64_t framesOut = 0;
};

// 按【时间戳】而不是到达顺序组织数据，这是抵抗乱序/抖动正确的做法
class JitterBuffer {
public:
    explicit JitterBuffer(size_t prerollPackets = 8) : preroll_(prerollPackets) {}

    bool push(const std::vector<uint8_t>& pkt) {
        if (pkt.size() < RTANET_HEADER) return false;
        RtaHeader h{};
        std::memcpy(&h, pkt.data(), RTANET_HEADER);
        if (h.magic != RTANET_MAGIC || h.version != RTANET_VERSION) return false;
        const size_t pb = packed_bytes(h.bits, h.frames, h.channels);
        if (pkt.size() < RTANET_HEADER + pb) return false;

        if (started_ && h.seq < nextSeq_) {
            // 还没交付过任何包时，更靠前的序号不是"迟到"，而是流真正的开头：
            // 预滚只看"收到几个包"，并不保证序号 0 已经在手（乱序时第 preroll 个包可能是 5）。
            // 旧行为：直接按此时的最小序号定起始点 ⟹ 之后才到的 0/1 被当迟到整段丢弃，
            // 输出只剩 10/12 包、序号从 2 开始，而 stats.lost 仍为 0（丢包计数根本看不出来）。
            // 一旦已经交付过数据，更早的序号才是真迟到，必须丢弃（不能回头重排已播出的时间轴）。
            if (delivered_ == 0) nextSeq_ = h.seq;  // 回退起始点，把流头接回来
            else { stats_.late++; return false; }
        }
        if (map_.count(h.seq)) {
            // 同序号重发：若时间戳一致视为重复，否则视为乱序重排
            if (map_[h.seq].header.timestamp == h.timestamp) { stats_.duplicate++; return false; }
            stats_.outOfOrder++;
        }
        if (!map_.empty() && h.seq != nextSeq_) stats_.outOfOrder++;

        Slot s;
        s.header = h;
        s.payload.assign(pkt.begin() + RTANET_HEADER, pkt.begin() + RTANET_HEADER + pb);
        map_[h.seq] = std::move(s);
        if (!started_ && map_.size() >= preroll_) {
            // 起始序号必须取【已收到的最小序号】，而不是"第一个到达的包"的序号。
            // 乱序到达时前者才是流真正的开头；用后者会从流中间开始输出，
            // 并且很快越界产生虚假的"丢包补静音"。
            nextSeq_ = map_.begin()->first;
            started_ = true;
        }
        if (h.seq > maxSeq_) maxSeq_ = h.seq;
        stats_.received++;
        return true;
    }

    // 取出一包。返回 false 表示【暂时无数据可交付】（尚未预滚满，或期望的包还没到）——
    // 这【不等于】丢包。丢包必须由调用方依据自己的播放时钟判定超期后调用 fill_gap()。
    // （曾把两种情况混在一起：缓冲一空就补静音，导致流正常结束时凭空多出一包静音。）
    bool pop(RtaHeader& outH, std::vector<uint8_t>& outPayload) {
        if (!started_) return false;
        auto it = map_.find(nextSeq_);
        if (it == map_.end()) return false;
        outH = it->second.header;
        outPayload = std::move(it->second.payload);
        map_.erase(it);
        nextSeq_++;
        ++delivered_;           // 时间轴已推进；unpop 会撤销（那说明这一包其实没交付）
        return true;
    }

    // 封口：记录"已见过的最大序号"。封口后 fill_gap 不会越过它继续造静音。
    // 这是防止"流中间缓冲一空就无限补静音、把真包全挤成迟到"的关键。
    void seal() {
        sealed_ = true;
        sealedEnd_ = maxSeq_;
        if (!started_ && !map_.empty()) { nextSeq_ = map_.begin()->first; started_ = true; }
    }
    bool sealed() const { return sealed_; }

    // 放回（配合非阻塞播放门：取出来发现时钟未放行时退回，不能丢数据）
    void unpop(const RtaHeader& h, std::vector<uint8_t>& payload) {
        Slot s; s.header = h; s.payload = std::move(payload);
        map_[h.seq] = std::move(s);
        if (nextSeq_ > 0) nextSeq_--;      // 退回序号
        // 这一包并没有真的交付（时钟未放行）：若它是"唯一已推进"的那次，则回到"尚未交付"状态，
        // 这样启动阶段（0.2 s 预充期间反复 pop/unpop）仍然允许把起始点回退到真正的流头。
        if (delivered_ > 0) --delivered_;
    }

    // 由调用方在判定"期望的包已超期"后调用：补一包静音并前进，保证时间轴连续。
    // 若该包其实已经到达，或已越过封口序号，则不补并返回 false。
    bool fill_gap(RtaHeader& outH, std::vector<uint8_t>& outPayload) {
        if (!started_) return false;
        if (map_.count(nextSeq_)) return false;
        if (sealed_ && nextSeq_ > sealedEnd_) return false;
        outH = RtaHeader{};
        outH.magic = RTANET_MAGIC; outH.version = RTANET_VERSION;
        outH.flags = RTA_FLAG_SILENCE;
        outH.seq = nextSeq_;
        outH.channels = lastChannels_; outH.bits = lastBits_;
        outH.frames = lastFrames_; outH.rate = lastRate_;
        outH.timestamp = nextTs_;
        nextSeq_++;
        stats_.lost++;
        outPayload.assign(packed_bytes(lastBits_, lastFrames_, lastChannels_), 0);
        ++delivered_;           // 补静音同样推进了时间轴
        return true;
    }

    void observe_format(const RtaHeader& h) {
        lastChannels_ = h.channels; lastBits_ = h.bits;
        lastFrames_ = h.frames; lastRate_ = h.rate;
        nextTs_ = h.timestamp + h.frames;
    }
    void set_next_ts(uint64_t ts) { nextTs_ = ts; }

    size_t pending() const { return map_.size(); }
    bool started() const { return started_; }
    bool delivered() const { return delivered_ > 0; }
    const JitterStats& stats() const { return stats_; }
    void reset() {
        map_.clear(); started_ = false; delivered_ = 0; firstSeq_ = -1; nextSeq_ = 0;
        maxSeq_ = 0; sealedEnd_ = 0; sealed_ = false;
        stats_ = JitterStats{};
    }

private:
    struct Slot { RtaHeader header{}; std::vector<uint8_t> payload; };
    std::map<uint32_t, Slot> map_;
    size_t preroll_ = 8;
    bool started_ = false;
    int  delivered_ = 0;          // 已推进但未撤销的交付次数（>0 才区分"流头乱序到达"与"真迟到"）
    int64_t firstSeq_ = -1;
    uint32_t nextSeq_ = 0;
    uint32_t maxSeq_ = 0;
    uint32_t sealedEnd_ = 0;
    bool sealed_ = false;
    uint64_t nextTs_ = 0;
    uint16_t lastChannels_ = 2, lastBits_ = 24, lastFrames_ = 128;
    uint32_t lastRate_ = 48000;
    JitterStats stats_;
};

// ---------------- 套接字封装 ----------------
class UdpSocket {
public:
    bool open(uint16_t bindPort = 0) {
#ifdef _WIN32
        static bool wsaDone = false;
        if (!wsaDone) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false; wsaDone = true; }
#endif
        s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == RTA_INVALID_SOCK) return false;
        // 必须始终 bind：bindPort==0 时由系统分配临时端口。
        // （曾写成"port 为 0 就跳过 bind"，结果套接字未绑定、getsockname 返回端口 0，
        //   发出去的包全部丢失——回环测试收到 0 包。）
        {
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(bindPort);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(s_, (sockaddr*)&a, sizeof(a)) != 0) { close(); return false; }
        }
        return true;
    }
    void close() { if (s_ != RTA_INVALID_SOCK) { rta_close_sock(s_); s_ = RTA_INVALID_SOCK; } }
    ~UdpSocket() { close(); }
    bool valid() const { return s_ != RTA_INVALID_SOCK; }
    void set_recv_buffer(size_t bytes) {
        int b = (int)bytes;
        ::setsockopt(s_, SOL_SOCKET, SO_RCVBUF, (const char*)&b, sizeof(b));
    }
    void set_recv_timeout_ms(int ms) {
#ifdef _WIN32
        DWORD t = (DWORD)ms; ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
        timeval tv{}; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    bool send_to(const void* data, size_t n, const std::string& host, uint16_t port) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = ::inet_addr(host.c_str());
        return ::sendto(s_, (const char*)data, (int)n, 0, (sockaddr*)&a, sizeof(a)) == (int)n;
    }
    int recv_from(void* buf, size_t cap, std::string* fromHost = nullptr, uint16_t* fromPort = nullptr) {
        sockaddr_in a{}; int alen = sizeof(a);
        const int r = ::recvfrom(s_, (char*)buf, (int)cap, 0, (sockaddr*)&a, &alen);
        if (r > 0) {
            if (fromHost) { char ip[64]{}; ::inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip)); *fromHost = ip; }
            if (fromPort) *fromPort = ntohs(a.sin_port);
        }
        return r;
    }
    uint16_t local_port() const {
        sockaddr_in a{}; 
#ifdef _WIN32
        int alen = sizeof(a);
#else
        socklen_t alen = sizeof(a);
#endif
        ::getsockname(s_, (sockaddr*)&a, &alen);
        return ntohs(a.sin_port);
    }
private:
    sock_t s_ = RTA_INVALID_SOCK;
};

} // namespace rta::net
