// gpu_bridge.h — GPU 卷积的动态加载桥接
//
// 设计：管线【不】在链接期依赖 HIP。运行时尝试加载 rtagpu.dll：
//   * 找不到 DLL  ⟹ 返回不可用，管线自动退回 CPU 卷积（普通构建照常工作）
//   * 找到但 kernel 跑不起来（如架构不匹配）⟹ rta_gpu_available() 返回 0，同样退回
// 这样 GPU 是纯粹的"锦上添花"，不会让整个项目变得必须有显卡才能跑。
//
// ─────────────────────────────────────────────────────────────────────────────
// ★ 2026-10-27（261026-08）：DLL 新增**流式会话**接口，修掉"逐块调用丢 IR 拖尾"的缺陷
//
// 缺陷回顾（主线探针 tools/gpu_stream_verify.cpp 实测）：旧的 rta_gpu_convolve 是
// **无状态**的整块接口——每次调用从零做 OLA、只写 out[0..n)。管线每块只喂 4096 样，
// 于是"上一块对本块的 IR 拖尾"无处可来 ⟹ 每个块边界断裂，误差能量大于信号本身
// （+1.7~+3.4 dB）。它不是音质劣化，是算法性错误。
//
// 新接口（HIP 与 CUDA 两份后端导出同名符号）：
//
//   void* rta_gpu_convolve_create(const double* ir, long long irLen, int block);
//     · 建会话。成功返回不透明句柄；失败（无卡/显存不足/kernel 不可执行）返回 nullptr，
//       调用方应退回 CPU，**不得**当成致命错误。
//     · device 侧常驻：IR 的 P=ceil(irLen/block) 个分区谱 + irLen−1 个历史输入的谱
//       环形缓冲 + 累加缓冲；host 侧常驻：输入 FIFO（不足一块的余量）、输出 FIFO、
//       OLA 尾巴（block 个）。
//     · block 必须与 CPU StreamingConvolver 的 block 相同（非 2 的幂时按 1024 处理）。
//     · **延迟语义：latency() == block**，与 CPU StreamingConvolver 完全一致
//       （创建时输出 FIFO 预置 block 个零）。
//
//   int rta_gpu_convolve_stream(void* h, const double* in, long long n, double* out);
//     · in/out 都是 n 个样本（n 可任意切分：4096 一块、37 一样本都行），跨调用累计。
//     · 返回 0 成功；非 0 = 该次处理失败（设备丢失等），调用方应退回 CPU。
//     · 与 CPU StreamingConvolver::process **逐位等价**（同一 FIFO 逻辑、同一 double
//       乘累加、显式禁用 FMA 收缩），实测 max|Δ| = 0（见 tests/test_gpu_stream.cpp）。
//
//   void rta_gpu_convolve_destroy(void* h);
//     · 释放会话；nullptr 安全。
//
// 兼容性：老 DLL（没有这三个符号）⟹ stream_supported() == false，GpuStreamConvolver
// 自动退回 CPU StreamingConvolver —— 不崩、不改变输出。整块接口 rta_gpu_convolve
// 的契约完全不变（离线/批处理仍在用）。
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "convolution.h"       // StreamingConvolver —— 流式桥的 CPU 回退路径
#include "gpu_pref.h"
#include "platform/dynlib.h"   // 动态库加载（Windows: LoadLibraryA / POSIX: dlopen）—— 本目录不再直接碰平台 API


namespace rta::dsp {

class GpuBridge {
public:
    static GpuBridge& instance() { static GpuBridge b; return b; }

    bool available() const { return available_; }
    const std::string& status() const { return status_; }
    // 换 GPU 后端（AMD/NVIDIA/关闭）后重新探测；下次播放生效
    void reload() { available_ = false; status_.clear(); load(); }

    // 返回 0 = 成功；非 0 = 失败（调用方应退回 CPU）
    int convolve(const double* in, size_t n, const double* ir, size_t irLen,
                 double* out, int block) const {
        if (!available_ || !fnConv_) return -1;
        return fnConv_(in, (long long)n, ir, (long long)irLen, out, block);
    }

    // ---- 流式会话（261026-08）------------------------------------------------
    // DLL 是否提供流式三件套。老 DLL / 无 DLL 时为 false。
    bool stream_supported() const { return available_ && fnCreate_ && fnStream_ && fnDestroy_; }

    // 建会话；0 = 失败（调用方退回 CPU）
    void* stream_create(const double* ir, size_t irLen, int block) const {
        if (!stream_supported() || !ir || irLen == 0) return nullptr;
        return fnCreate_(ir, (long long)irLen, block);
    }
    // 流式处理 n 个样本；0 = 成功
    int stream_process(void* h, const double* in, size_t n, double* out) const {
        if (!h || !fnStream_ || !out) return -1;
        return fnStream_(h, in, (long long)n, out);
    }
    void stream_destroy(void* h) const { if (h && fnDestroy_) fnDestroy_(h); }

private:
    GpuBridge() { load(); }
    GpuBridge(const GpuBridge&) = delete;
    GpuBridge& operator=(const GpuBridge&) = delete;

    void load() {
        const auto cands = gpu_dll_candidates();
        if (cands.empty()) { status_ = "GPU 已关闭（使用 CPU 卷积）"; return; }
        // 逐个候选尝试；某个 DLL 存在但 kernel 跑不起来（例如 NVIDIA 机器上
        // 残留的 HIP 版 rtagpu.dll）时必须继续试下一个，而不是直接退回 CPU。
        std::string tried;
        for (const char* nm : cands) {
            rta::platform::SharedLib lib;
            if (!lib.open(nm)) { if (!tried.empty()) tried += " / "; tried += nm; continue; }
            auto avail = (int (*)())lib.symbol("rta_gpu_available");
            auto conv  = (int (*)(const double*, long long, const double*, long long, double*, int))
                         lib.symbol("rta_gpu_convolve");
            // 流式三件套：老 DLL 没有它们是正常情况（全有或全无），不是错误。
            auto sCreate  = (void* (*)(const double*, long long, int))
                            lib.symbol("rta_gpu_convolve_create");
            auto sStream  = (int (*)(void*, const double*, long long, double*))
                            lib.symbol("rta_gpu_convolve_stream");
            auto sDestroy = (void (*)(void*))
                            lib.symbol("rta_gpu_convolve_destroy");
            if (!sCreate || !sStream || !sDestroy) { sCreate = nullptr; sStream = nullptr; sDestroy = nullptr; }
            if (!avail || !conv || avail() != 1) {
                lib.close();
                if (!tried.empty()) tried += " / ";
                tried += std::string(nm) + "(不可用)";
                continue;
            }
            fnAvail_ = avail; fnConv_ = conv;
            fnCreate_ = sCreate; fnStream_ = sStream; fnDestroy_ = sDestroy;
            available_ = true;
            status_ = std::string("GPU 卷积可用（") + nm + "）";
            status_ += fnCreate_ ? " · 流式会话就绪" : " · 老 DLL 无流式符号（流式退回 CPU）";
            lib.detach();   // 与迁移前一致：模块保持加载（函数指针还要用，绝不卸载）
            return;
        }
        status_ = "未找到可执行的 GPU 后端（" + tried + "），使用 CPU 卷积";
        return;
    }

    bool available_ = false;
    std::string status_;
    int (*fnAvail_)() = nullptr;
    int (*fnConv_)(const double*, long long, const double*, long long, double*, int) = nullptr;
    // 流式会话（261026-08）；任一为 nullptr 表示该 DLL 是老版
    void* (*fnCreate_)(const double*, long long, int) = nullptr;
    int   (*fnStream_)(void*, const double*, long long, double*) = nullptr;
    void  (*fnDestroy_)(void*) = nullptr;
};

// ---------------------------------------------------------------------------
// GpuStreamConvolver —— 管线直接用的流式卷积器（GPU 优先，自动退回 CPU）
//
// 契约与 StreamingConvolver 完全一致：
//   * construct(ir, block) → latency() == block
//   * process(in, out, n) 任意分块，跨调用保持状态
//   * reset(ir) 换 IR
// 与 CPU 版**逐位等价**（GPU 路径只把乘累加搬到显卡，FFT/IFFT 与 FIFO 逻辑同一份实现）。
// 降级策略：
//   * DLL 缺失 / 老 DLL 没有流式符号 / create 失败 ⟹ 直接走 CPU StreamingConvolver
//   * 运行中 stream_process 返回非 0（设备丢失）⟹ **永久**降级到 CPU，并用同一份 IR
//     重建（会出现一次 block 帧的重新预置延迟，但不崩、不静默出错）
// ---------------------------------------------------------------------------
class GpuStreamConvolver {
public:
    explicit GpuStreamConvolver(const std::vector<double>& ir, size_t block = 1024)
        : bridge_(&GpuBridge::instance()), ir_(ir), B_(norm_block(block)) {
        open_gpu_or_cpu();
    }
    ~GpuStreamConvolver() { close(); }
    GpuStreamConvolver(const GpuStreamConvolver&) = delete;
    GpuStreamConvolver& operator=(const GpuStreamConvolver&) = delete;

    size_t latency() const { return B_; }        // 与 CPU StreamingConvolver 相同：= block
    size_t block_size() const { return B_; }
    bool on_gpu() const { return h_ != nullptr; }        // 当前是否真的走显卡
    const std::string& status() const { return bridge_->status(); }

    void reset(const std::vector<double>& ir) {
        ir_ = ir;
        close();
        open_gpu_or_cpu();
    }

    void process(const double* in, double* out, size_t n) {
        if (h_) {
            if (bridge_->stream_process(h_, in, n, out) == 0) return;
            // 设备丢失等：永久降级到 CPU（同一份 IR 重建一份流式卷积器）
            close_gpu();
            cpu_.reset(new StreamingConvolver(ir_, B_));
        }
        if (cpu_) cpu_->process(in, out, n);
        else for (size_t i = 0; i < n; ++i) out[i] = 0.0;
    }

private:
    static size_t norm_block(size_t b) { return (b == 0 || (b & (b - 1)) != 0) ? size_t(1024) : b; }
    void open_gpu_or_cpu() {
        if (bridge_->available() && bridge_->stream_supported())
            h_ = bridge_->stream_create(ir_.data(), ir_.size(), (int)B_);
        if (!h_) cpu_.reset(new StreamingConvolver(ir_, B_));
    }
    void close_gpu() {
        if (h_) { bridge_->stream_destroy(h_); h_ = nullptr; }
    }
    void close() { close_gpu(); cpu_.reset(); }

    const GpuBridge* bridge_;
    std::vector<double> ir_;
    size_t B_ = 1024;
    void* h_ = nullptr;
    std::unique_ptr<StreamingConvolver> cpu_;
};

} // namespace rta::dsp
