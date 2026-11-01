// gpu_resample_cuda.cu — 多相升频的 GPU 核（CUDA / NVIDIA）
//
// 与 src/gpu/gpu_resample_hip.cpp 逐行对应。内层循环是「每个输出样本 = 某个相位的 T 个
// 抽头 · 最近 T 个输入样本」——典型的批量点积，一个线程算一个输出样本。
//
//   p      = 输出样本 n 对应的相位索引（0..L-1）
//   base   = 该输出样本所需的最新输入样本在 hist 中的下标
//   acc    = sum_{u=0..T-1} phases[p][u] * hist[c][base-u]
//
// 索引计算（inIdx/phase 的选择）留在 CPU —— 它很便宜，且改动风险最小。
//
// 两处必须保留的工程细节（都是 hip 版踩出来的，别"优化"掉）：
//   1. 相位表在整首歌里不变，只在 L/T 变化或 invalidate 时上传一次；
//      每次调用都传 2 MB（16×16384 doubles）曾让加速比只有 7.5x。
//   2. 启动阶段 hist 不足 T 时缺失部分按零处理（uEnd 取三者最小值），
//      这不是"越界保护"，是卷积头部的定义。
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#define RTA_EXPORT extern "C" __declspec(dllexport)
#else
#define RTA_EXPORT extern "C"
#endif

__global__ void resample_kernel(const double* __restrict__ hist,   // [ch][histLen]
                                int   ch,
                                long long histLen,
                                const double* __restrict__ phases, // [L][T]
                                int   T,
                                const int* __restrict__ bases,     // [nOut]
                                const int* __restrict__ phIdx,     // [nOut]
                                long long nOut,
                                double* __restrict__ out)          // [nOut][ch] 交错
{
    const long long idx = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= nOut * (long long)ch) return;

    const long long n = idx / (long long)ch;
    const int       c = (int)(idx % (long long)ch);

    const int base = bases[n];
    const int p    = phIdx[n];
    const double* ph = phases + (long long)p * (long long)T;
    const double* h  = hist + (long long)c * histLen;

    // 启动阶段历史上不足 T，缺失部分视为零 -> 只累加有效下标
    int uEnd = base + 1;
    if (uEnd > T) uEnd = T;
    if (uEnd > histLen) uEnd = (int)histLen;

    double acc = 0.0;
    for (int u = 0; u < uEnd; ++u) acc += ph[u] * h[base - u];
    out[idx] = acc;
}

// 每次改变滤波器/采样率配置后调用一次，把相位表固化到显存
// The exported ABI is shared by all resampler instances and channels. Protect
// the shared device buffers and compare the actual coefficient bytes: L/T (or
// the source pointer, whose allocation can be reused) cannot identify a filter.
static std::mutex resampleMutex_;
// Two bounded slots keep a two-stage cascade warm without pointer-only keys.
// Callers hold resampleMutex_ across acquisition and kernel completion.
struct PhaseSlot {
    double* device = nullptr;
    size_t capacity = 0;
    std::vector<double> content;
};
static PhaseSlot phaseSlots_[2];
static unsigned phaseMostRecent_ = 0;
static void invalidate_phase_slots() {
    for (auto& slot : phaseSlots_) slot.content.clear();
}
static double* acquire_phases(const double* phases, size_t count) {
    if (!phases || count == 0 || count > SIZE_MAX / sizeof(double)) return nullptr;
    const size_t bytes = count * sizeof(double);
    for (unsigned i = 0; i < 2; ++i) {
        auto& slot = phaseSlots_[i];
        if (slot.content.size() == count &&
            std::memcmp(slot.content.data(), phases, bytes) == 0) {
            phaseMostRecent_ = i;
            return slot.device;
        }
    }
    unsigned victim = 1u - phaseMostRecent_;
    for (unsigned i = 0; i < 2; ++i)
        if (phaseSlots_[i].content.empty()) { victim = i; break; }
    auto& slot = phaseSlots_[victim];
    // Prepare the host identity first; allocation failure must not escape the ABI.
    std::vector<double> identity;
    try { identity.assign(phases, phases + count); }
    catch (...) { return nullptr; }
    if (count > slot.capacity) {
        double* replacement = nullptr;
        if (cudaMalloc(&replacement, bytes) != cudaSuccess) return nullptr;
        if (slot.device) cudaFree(slot.device);
        slot.device = replacement;
        slot.capacity = count;
    }
    // A partial copy invalidates this slot, but never the other cascade stage.
    slot.content.clear();
    if (cudaMemcpy(slot.device, phases, bytes, cudaMemcpyHostToDevice) != cudaSuccess) return nullptr;
    slot.content.swap(identity);
    phaseMostRecent_ = victim;
    return slot.device;
}

RTA_EXPORT void rta_gpu_resample_invalidate(void) {
    std::lock_guard<std::mutex> lock(resampleMutex_);
    invalidate_phase_slots();
}

RTA_EXPORT int rta_gpu_resample(const double* hist, int ch, long long histLen,
                                const double* phases, int L, int T,
                                const int* bases, const int* phIdx, long long nOut,
                                double* outInterleaved)
{
    if (nOut <= 0 || ch <= 0 || T <= 0 || L <= 0) return 0;
    std::lock_guard<std::mutex> lock(resampleMutex_);

    static double* d_hist = nullptr;
    static int*    d_bases = nullptr;
    static int*    d_phIdx = nullptr;
    static double* d_out = nullptr;
    static long long capHist = 0, capN = 0, capOut = 0;

    const long long needHist = (long long)ch * histLen;
    const long long needPh   = (long long)L * T;
    const long long needOut  = nOut * (long long)ch;

    if (needHist > capHist) {
        double* replacement = nullptr;
        if (cudaMalloc(&replacement, needHist * sizeof(double)) != cudaSuccess) return -1;
        if (d_hist) cudaFree(d_hist);
        d_hist = replacement;
        capHist = needHist;
    }
    if (nOut > capN) {
        int* replacementBases = nullptr;
        int* replacementPhIdx = nullptr;
        if (cudaMalloc(&replacementBases, nOut * sizeof(int)) != cudaSuccess) return -1;
        if (cudaMalloc(&replacementPhIdx, nOut * sizeof(int)) != cudaSuccess) {
            cudaFree(replacementBases);
            return -1;
        }
        // Commit the pair together. A failed second allocation must retain
        // both old pointers and their common capacity for a smaller retry.
        if (d_bases) cudaFree(d_bases);
        if (d_phIdx) cudaFree(d_phIdx);
        d_bases = replacementBases;
        d_phIdx = replacementPhIdx;
        capN = nOut;
    }
    // Output capacity counts samples, not frames: mono -> stereo can require
    // growth even when the frame count stays the same (or decreases).
    if (needOut > capOut) {
        double* replacement = nullptr;
        if (cudaMalloc(&replacement, needOut * sizeof(double)) != cudaSuccess) return -1;
        if (d_out) cudaFree(d_out);
        d_out = replacement;
        capOut = needOut;
    }

    if (cudaMemcpy(d_hist, hist, needHist * sizeof(double), cudaMemcpyHostToDevice) != cudaSuccess) return -1;
    double* d_phases = acquire_phases(phases, (size_t)needPh);
    if (!d_phases) return -1;
    if (cudaMemcpy(d_bases, bases, nOut * sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) return -1;
    if (cudaMemcpy(d_phIdx, phIdx, nOut * sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) return -1;

    const long long total = nOut * (long long)ch;
    const int threads = 256;
    const long long blocks = (total + threads - 1) / threads;
    (void)cudaGetLastError(); // Clear earlier sticky errors before this launch.
    resample_kernel<<<(unsigned)blocks, threads>>>(d_hist, ch, histLen, d_phases, T,
                                                   d_bases, d_phIdx, nOut, d_out);

    const cudaError_t le = cudaGetLastError();
    if (le != cudaSuccess) {
        std::fprintf(stderr, "[rtagpu_cuda] resample kernel: %s\n", cudaGetErrorString(le));
        return -1;
    }
    if (cudaDeviceSynchronize() != cudaSuccess) return -1;
    if (cudaMemcpy(outInterleaved, d_out, total * sizeof(double), cudaMemcpyDeviceToHost) != cudaSuccess) return -1;
    return (int)nOut;
}
