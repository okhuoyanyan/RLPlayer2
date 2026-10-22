// audio_out.h — 音频输出后端【契约】（平台层，跨平台纯接口）
//
// 谁实现
//   Windows：src/audio/wasapi_out.h（WASAPI 独占）、src/audio/wasapi_shared.h（共享/回退）、
//            src/audio/asio_out.h（ASIO）。三者经 rta::render::OutSink（src/audio/render_loop.h）
//            暴露给渲染循环；本契约是它们共同语义的正式化描述（映射表见 PLATFORM.md §4）。
//   macOS  ：CoreAudio（另组实现），必须实现本文件的 AudioOutput 语义。
// 谁调用
//   引擎渲染循环 / 平台工厂。调用次序固定：open -> start -> (write)* -> pause/resume -> close
//
// 铁律
//   * 本文件【不得】出现任何 Windows 类型（HANDLE/HRESULT/DWORD/LPWSTR...）
//   * 采样一律 int32 交错，低位对齐到【请求的位深】的满量程：
//       bits=16 -> 取值范围 [-32768, 32767]     （不是左移 16 位！）
//       bits=24 -> [-8388608, 8388607]
//       bits=32 -> [INT32_MIN, INT32_MAX]
//     迁移前 wasapi_out.h 的 pack() 就是这个语义（(int16_t)src / 取低 24 位），
//     Mac 侧必须逐位一致，否则"bit-perfect"是假的。
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rta::platform {

// ---------------------------------------------------------------------------
// 错误分类（PLATFORM.md §7：每一类都必须有用户可见文案，不许静默失败）
// ---------------------------------------------------------------------------
enum class OutputError {
    None = 0,
    NotOpened,          // 未 open 就 start/write
    DeviceNotFound,     // 设备不存在 / 已拔出 / 端点被禁用
    DeviceBusy,         // 被别的进程独占（Windows: AUDCLNT_E_DEVICE_IN_USE / 独占占用）
    UnsupportedFormat,  // 该 采样率 x 位深 x 声道 组合不支持（必须报错，不许静默换格式）
    StartFailed,        // 已开流，Start 失败
    WriteFailed,        // 写入被设备拒绝
    Underrun,           // 欠载：数据没供上（调度问题，不是丢样）
    DeviceLost,         // 播放中设备消失（Windows: AUDCLNT_E_DEVICE_INVALIDATED）
    Timeout,            // 等设备事件超时
    Internal,
};

// 错误 -> 用户可见文案（中文，含"下一步做什么"）。绝不允许返回空串冒充"没事"。
inline const char* output_error_text(OutputError e) {
    switch (e) {
        case OutputError::None:              return "无错误";
        case OutputError::NotOpened:         return "输出设备尚未打开（程序内部时序错误）";
        case OutputError::DeviceNotFound:    return "找不到输出设备：端点可能已拔出或被禁用，请在设置里重选设备";
        case OutputError::DeviceBusy:        return "输出设备被其它程序独占：请关闭占用它的程序，或改用共享模式";
        case OutputError::UnsupportedFormat: return "设备不支持该采样率/位深组合：请改用设备支持的格式（不会自动替换格式）";
        case OutputError::StartFailed:       return "设备已开流但启动失败：请重选设备或重开播放";
        case OutputError::WriteFailed:       return "向设备写数据失败：设备可能已被系统接管，请重新打开输出";
        case OutputError::Underrun:          return "供料不足（欠载）：本次播放可能出现断音，请检查系统负载或增大缓冲";
        case OutputError::DeviceLost:        return "播放中设备消失了（被拔出/切换）：请重新选择输出设备";
        case OutputError::Timeout:           return "等待设备事件超时：设备可能无响应，请重开输出";
        case OutputError::Internal:          return "输出后端内部错误";
    }
    return "未知错误";
}

// ---------------------------------------------------------------------------
// DSD 能力声明
// ---------------------------------------------------------------------------
enum class DsdTransport {
    None = 0,   // 不支持 DSD
    DoP,        // 用 PCM 容器承载 DSD：1 bit DSD : 16 bit PCM，承载率 = dsdRate/16
                //   DSD64(2822400) -> 176400；DSD128 -> 352800；DSD256 -> 705600
    Native,     // 原生 DSD 直通（USB 音频类自定义协议；可选能力，不做也不算缺陷）
};

// 设备能力（每台设备、每种输出模式各一份；UI 的"能力"页直接照这个填）
struct DeviceCaps {
    std::vector<uint32_t> rates;        // 支持的采样率（Hz），升序
    std::vector<int>      bitDepths;    // 支持的位深（16/24/32），升序
    int  maxChannels = 2;
    bool exclusiveMode = false;         // 是否支持独占/整数模式（bit-perfect 的前提）
    bool bitPerfectCapable = false;     // 独占 + 原生格式下能否逐位直通
    DsdTransport dsd = DsdTransport::None;
    uint32_t dopCarrierRate = 0;        // DoP 承载的 PCM 采样率（0 = 不支持）
    uint32_t nativeDsdRate = 0;         // Native DSD 的比特率（0 = 不支持）
    double minBufferMs = 0.0;           // 缓冲下限（0 = 未测量）
    double maxBufferMs = 0.0;           // 缓冲上限（0 = 未测量）
    size_t minPeriodFrames = 0;         // 设备最小周期（帧；0 = 未测量）
};

// 开流参数
struct OpenSpec {
    std::string deviceId;   // 设备标识；空 = 系统默认
    uint32_t rate = 0;
    int      bits = 0;
    int      channels = 2;
    double   bufferMs = 100.0;   // 缓冲时长（独占模式下必须等于周期，见 PLATFORM.md §8）
};

// ---------------------------------------------------------------------------
// 路由判定：源格式 -> 输出路径（纯函数，可离线测；Windows/Mac 必须给同一个答案）
// ---------------------------------------------------------------------------
struct SourceSpec {
    bool     isDsd = false;
    uint32_t rate = 0;   // PCM 采样率；isDsd 时为 DSD 比特率（DSD64 = 2822400）
    int      bits = 0;   // PCM 位深；isDsd 时忽略
};

enum class RouteKind {
    Refused = 0,     // 明确拒绝（必须把 reason 显示给用户）
    PcmBitPerfect,   // 逐位直通
    PcmConverted,    // 需要转换（重采样/换位深）—— 不是 bit-perfect，必须告知
    Dop,             // DSD over PCM 承载（bit-perfect：DSD 位不丢）
    NativeDsd,       // 原生 DSD 直通（bit-perfect）
};

struct RouteDecision {
    RouteKind kind = RouteKind::Refused;
    uint32_t outRate = 0;
    int      outBits = 0;
    std::string reason;    // 非空，直接可进日志/UI
    bool bitPerfect() const {
        return kind == RouteKind::PcmBitPerfect || kind == RouteKind::Dop ||
               kind == RouteKind::NativeDsd;
    }
};

namespace detail {
inline bool has_rate(const DeviceCaps& c, uint32_t r) {
    if (r == 0) return false;
    for (uint32_t x : c.rates) if (x == r) return true;
    return false;
}
inline bool has_bits(const DeviceCaps& c, int b) {
    for (int x : c.bitDepths) if (x == b) return true;
    return false;
}
inline uint32_t lowest_rate(const DeviceCaps& c) {
    uint32_t m = 0;
    for (uint32_t x : c.rates) if (m == 0 || x < m) m = x;
    return m;
}
inline int highest_bits(const DeviceCaps& c) {
    int m = 0;
    for (int x : c.bitDepths) if (x > m) m = x;
    return m;
}
inline int lowest_bits(const DeviceCaps& c) {
    int m = 0;
    for (int x : c.bitDepths) if (m == 0 || x < m) m = x;
    return m;
}
} // namespace detail

// 判定规则（PLATFORM.md §5 逐条对应，改动必须同步文档）：
//   1. 源是 DSD：Native 直通优先 -> DoP（承载率 = rate/16）-> 显式允许转换才 DSD->PCM
//      -> 否则【拒绝】，绝不静默把 DSD 降成 PCM
//   2. 源是 PCM 且 设备支持该 rate 与 bits 且独占且 bitPerfectCapable -> PcmBitPerfect
//   3. 否则若 allow_conversion：取最接近的可支持 rate（优先不小于源率的最小者）+ 最接近位深
//      -> PcmConverted（bitPerfect()==false，调用方必须在 UI/日志里挑明）
//   4. 否则【拒绝】并在 reason 里写清"设备不支持什么"，不静默替换格式
inline RouteDecision decide_route(const SourceSpec& src, const DeviceCaps& caps,
                                  bool allow_conversion) {
    RouteDecision d;
    if (src.isDsd) {
        if (caps.dsd == DsdTransport::Native && caps.nativeDsdRate != 0 &&
            caps.nativeDsdRate == src.rate) {
            d.kind = RouteKind::NativeDsd;
            d.outRate = caps.nativeDsdRate;
            d.outBits = 1;
            d.reason = "Native DSD 直通（" + std::to_string(src.rate) + " bit/s）";
            return d;
        }
        const uint32_t carrier = src.rate / 16;
        if (caps.dsd != DsdTransport::None && carrier != 0 && detail::has_rate(caps, carrier)) {
            d.kind = RouteKind::Dop;
            d.outRate = carrier;
            d.outBits = 24;   // DoP 用 24-bit 容器承载（8 bit 标记 + 16 bit DSD 数据）
            d.reason = "DoP 承载：DSD " + std::to_string(src.rate) + " -> PCM " +
                       std::to_string(carrier) + "/24（DSD 位不丢，bit-perfect）";
            return d;
        }
        if (allow_conversion) {
            d.kind = RouteKind::PcmConverted;
            d.outRate = detail::lowest_rate(caps) ? detail::lowest_rate(caps) : 176400;
            d.outBits = detail::highest_bits(caps) ? detail::highest_bits(caps) : 24;
            d.reason = "设备不支持该 DSD 速率的 DoP 承载（" + std::to_string(carrier) +
                       " Hz），按用户设置转换为 PCM " + std::to_string(d.outRate) + "/" +
                       std::to_string(d.outBits) + "（非 bit-perfect）";
            return d;
        }
        d.kind = RouteKind::Refused;
        d.reason = "设备不支持 DSD（需要 DoP 承载 " + std::to_string(carrier) +
                   " Hz 或 Native DSD）；已拒绝播放，不会静默降成 PCM。"
                   "如需降级请在设置里显式允许 DSD->PCM 转换";
        return d;
    }

    if (detail::has_rate(caps, src.rate) && detail::has_bits(caps, src.bits) &&
        caps.exclusiveMode && caps.bitPerfectCapable) {
        d.kind = RouteKind::PcmBitPerfect;
        d.outRate = src.rate;
        d.outBits = src.bits;
        d.reason = "逐位直通：" + std::to_string(src.rate) + "/" + std::to_string(src.bits) +
                   "（独占模式，无重采样、无位深转换）";
        return d;
    }

    if (allow_conversion) {
        uint32_t rate = src.rate;
        if (!detail::has_rate(caps, src.rate)) {
            uint32_t best = 0;
            for (uint32_t x : caps.rates) {
                if (x >= src.rate && (best == 0 || x < best)) best = x;   // 不小于源率的最小者
            }
            if (best == 0) best = detail::lowest_rate(caps);
            rate = best ? best : src.rate;
        }
        int bits = detail::has_bits(caps, src.bits) ? src.bits : detail::highest_bits(caps);
        if (bits == 0) bits = src.bits;
        d.kind = RouteKind::PcmConverted;
        d.outRate = rate;
        d.outBits = bits;
        d.reason = "需要转换：" + std::to_string(src.rate) + "/" + std::to_string(src.bits) +
                   " -> " + std::to_string(rate) + "/" + std::to_string(bits) +
                   "（设备不支持源格式；非 bit-perfect）";
        return d;
    }

    d.kind = RouteKind::Refused;
    d.reason = "设备不支持 " + std::to_string(src.rate) + "/" + std::to_string(src.bits) +
               "（" + (caps.exclusiveMode ? "" : "无独占模式；") +
               (caps.bitPerfectCapable ? "" : "不能 bit-perfect；") +
               "已拒绝，不会静默替换格式）";
    return d;
}

// ---------------------------------------------------------------------------
// 后端接口
// ---------------------------------------------------------------------------
//
// 线程模型（Mac 组必须一致）：
//   * open/close 只在【控制线程】调用，且必须与 write 串行
//   * start 之后，write 由【渲染线程】独占调用（单线程写；后端内部不得再起写线程）
//   * pause/resume 只翻转设备侧状态，不释放资源；close 之后对象可重新 open
//   * 任何方法都【不】抛异常；失败经 last_error() + last_error_class() 报告
//
// 缓冲/周期语义（PLATFORM.md §8）：
//   * bufferMs 是"设备缓冲总时长"；周期 = 设备最小可调度单位
//   * 独占模式下周期 == 缓冲（Windows Initialize 的 periodicity 配方）
//   * write 允许部分写入：返回实际写入帧数，调用方必须循环喂到写完
//   * buffer_frames() 返回设备缓冲能容纳的帧数（渲染循环用它决定每块大小）
class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // 查询设备能力（不 open 也能查；用于 UI 的"设备能力"页）
    virtual DeviceCaps caps(const std::string& deviceId) = 0;

    // 开流。失败返回 false 并置错误（不许部分成功却返回 true）
    virtual bool open(const OpenSpec& spec) = 0;
    virtual bool start() = 0;

    // 写入交错 int32 帧；返回实际写入帧数（< frames 不算错误，调用方继续喂）
    virtual size_t write(const int32_t* interleaved, size_t frames) = 0;

    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void close() = 0;

    virtual size_t buffer_frames() const = 0;
    virtual size_t frames_written() const = 0;

    // 补偿增益（用户拖音量滑块）。⚠️ 任何 != 1.0 的增益都会破坏 bit-perfect，
    // 后端必须如实反映：bit_perfect() 变 false，并在 UI 上显示"已施加音量/非 bit-perfect"。
    virtual void set_gain(double /*linear*/) {}
    virtual bool bit_perfect() const = 0;

    virtual OutputError last_error_class() const = 0;
    virtual const std::string& last_error() const = 0;
};

} // namespace rta::platform
