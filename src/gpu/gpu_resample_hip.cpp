// gpu_resample_hip.cpp — 多相升频的 GPU 核
//
// 内层循环是「每个输出样本 = 某个相位的 T 个抽头 · 最近 T 个输入样本」。
// 这是典型的批量点积，非常适合 GPU：一个线程算一个输出样本。
//
// 与 CPU 的对应关系：
//   p      = 输出样本 n 对应的相位索引（0..L-1）
//   base   = 该输出样本所需的最新输入样本在 hist 中的下标
//   acc    = sum_{u=0..T-1} phases[p][u] * hist[c][base-u]
// 旧ABI由CPU生成索引；可选compact ABI传计数参数，在GPU精确生成索引并直接输出平面。
//
// ★ 架构必须显式指定 gfx1201（Navi 48）。写错时 kernel 会静默不执行。
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <limits>

#define HIP_CHECK(x) do { hipError_t e = (x); if (e != hipSuccess) { \
    std::fprintf(stderr, "[rtagpu] %s 失败: %s\n", #x, hipGetErrorString(e)); return -1; } } while (0)

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
        if (hipMalloc(&replacement, bytes) != hipSuccess) return nullptr;
        if (slot.device) hipFree(slot.device);
        slot.device = replacement;
        slot.capacity = count;
    }
    // A partial copy invalidates this slot, but never the other cascade stage.
    slot.content.clear();
    if (hipMemcpy(slot.device, phases, bytes, hipMemcpyHostToDevice) != hipSuccess) return nullptr;
    slot.content.swap(identity);
    phaseMostRecent_ = victim;
    return slot.device;
}

// Reallocation is transactional: a failed request must leave the smaller,
// previously valid buffers usable on the next call.
template<class T> static bool grow_buffer(T*& pointer, long long& capacity, long long required) {
    if (required <= capacity) return true;
    if (required < 0 || (unsigned long long)required > SIZE_MAX / sizeof(T)) return false;
    T* replacement = nullptr;
    if (hipMalloc(&replacement, (size_t)required * sizeof(T)) != hipSuccess) return false;
    if (pointer) hipFree(pointer);
    pointer = replacement; capacity = required;
    return true;
}
static bool grow_indices(int*& bases, int*& phases, long long& capacity, long long required) {
    if (required <= capacity) return true;
    if (required < 0 || (unsigned long long)required > SIZE_MAX / sizeof(int)) return false;
    int *newBases = nullptr, *newPhases = nullptr;
    if (hipMalloc(&newBases, (size_t)required * sizeof(int)) != hipSuccess) return false;
    if (hipMalloc(&newPhases, (size_t)required * sizeof(int)) != hipSuccess) {
        hipFree(newBases); return false;
    }
    if (bases) hipFree(bases);
    if (phases) hipFree(phases);
    bases = newBases; phases = newPhases; capacity = required;
    return true;
}
static bool double_count_fits(long long count) {
    return count >= 0 && (unsigned long long)count <= SIZE_MAX / sizeof(double);
}

extern "C" __declspec(dllexport)
void rta_gpu_resample_invalidate() {
    std::lock_guard<std::mutex> lock(resampleMutex_);
    invalidate_phase_slots();
}

extern "C" __declspec(dllexport)
int rta_gpu_resample(const double* hist, int ch, long long histLen,
                     const double* phases, int L, int T,
                     const int* bases, const int* phIdx, long long nOut,
                     double* outInterleaved)
{
    if (nOut <= 0 || ch <= 0 || T <= 0 || L <= 0) return 0;
    if (!hist || !phases || !bases || !phIdx || !outInterleaved || histLen <= 0 ||
        histLen > LLONG_MAX / ch || nOut > LLONG_MAX / ch) return -1;
    if (!double_count_fits(histLen * ch) || !double_count_fits((long long)L * T) ||
        !double_count_fits(nOut * ch) || (unsigned long long)nOut > SIZE_MAX / sizeof(int)) return -1;
    std::lock_guard<std::mutex> lock(resampleMutex_);

    static double* d_hist = nullptr;
    static int*    d_bases = nullptr;
    static int*    d_phIdx = nullptr;
    static double* d_out = nullptr;
    static long long capHist = 0, capN = 0, capOut = 0;

    const long long needHist = (long long)ch * histLen;
    const long long needPh   = (long long)L * T;
    const long long needOut  = nOut * (long long)ch;

    if (!grow_buffer(d_hist, capHist, needHist)) return -1;
    if (!grow_indices(d_bases, d_phIdx, capN, nOut)) return -1;
    if (!grow_buffer(d_out, capOut, needOut)) return -1;

    if (hipMemcpy(d_hist, hist, needHist * sizeof(double), hipMemcpyHostToDevice) != hipSuccess) return -1;
    double* d_phases = acquire_phases(phases, (size_t)needPh);
    if (!d_phases) return -1;
    if (hipMemcpy(d_bases, bases, nOut * sizeof(int), hipMemcpyHostToDevice) != hipSuccess) return -1;
    if (hipMemcpy(d_phIdx, phIdx, nOut * sizeof(int), hipMemcpyHostToDevice) != hipSuccess) return -1;

    const long long total = nOut * (long long)ch;
    const int threads = 256;
    const long long blocks = (total + threads - 1) / threads;
    (void)hipGetLastError();
    hipLaunchKernelGGL(resample_kernel, dim3((unsigned)blocks), dim3(threads), 0, 0,
                       d_hist, ch, histLen, d_phases, T, d_bases, d_phIdx, nOut, d_out);

    hipError_t le = hipGetLastError();
    if (le != hipSuccess) { std::fprintf(stderr, "[rtagpu] resample kernel: %s\n", hipGetErrorString(le)); return -1; }
    if (hipDeviceSynchronize() != hipSuccess) return -1;
    if (hipMemcpy(outInterleaved, d_out, total * sizeof(double), hipMemcpyDeviceToHost) != hipSuccess) return -1;
    return (int)nOut;
}


// Optional compact ABI. Older HIP/CUDA/D3D backends retain the original ABI.
// Output capacity is per channel; every plane must hold that many doubles.
__global__ void compact_planar_kernel(const double* __restrict__ hist, long long histLen,
                                      const double* __restrict__ phases, int L, int M, int T,
                                      int shift, long long outIdx0, long long firstAbs,
                                      long long nOut, double* __restrict__ output) {
    const long long n = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (n >= nOut) return;
    const int c = blockIdx.y;
    const long long num = (outIdx0 + n) * (long long)M;
    const long long index = shift >= 0 ? (num >> shift) : (num / L);
    const int phase = shift >= 0 ? (int)(num & (L - 1)) : (int)(num % L);
    const int base = (int)(index - firstAbs);
    int uEnd = base + 1;
    if (uEnd > T) uEnd = T;
    if (uEnd > histLen) uEnd = (int)histLen;
    const double* ph = phases + (long long)phase * T;
    const double* h = hist + (long long)c * histLen;
    double acc = 0.0;
    for (int u = 0; u < uEnd; ++u) acc += ph[u] * h[base - u];
    output[(long long)c * nOut + n] = acc;
}

extern "C" __declspec(dllexport)
int rta_gpu_resample_compact_planar(const double* hist, int ch, long long histLen,
                                  const double* phases, int L, int M, int T,
                                  long long outIdx0, long long firstAbs, long long nOut,
                                  long long outputCapacityFrames, double* const* output) {
    if (nOut == 0) return 0;
    if (nOut < 0 || nOut > INT_MAX || ch <= 0 || ch > 65535 || L <= 0 || M <= 0 || T <= 0 ||
        histLen <= 0 || outputCapacityFrames < nOut || outIdx0 < 0 || !hist || !phases || !output)
        return -1;
    if (histLen > LLONG_MAX / ch || nOut > LLONG_MAX / ch ||
        !double_count_fits(histLen * ch) || !double_count_fits((long long)L * T) ||
        !double_count_fits(nOut * ch) || !double_count_fits(outputCapacityFrames)) return -1;
    if (outIdx0 > LLONG_MAX - (nOut - 1)) return -1;
    const long long lastOut = outIdx0 + nOut - 1;
    if (lastOut > LLONG_MAX / M) return -1;
    const long long firstIndex = outIdx0 * (long long)M / L;
    const long long lastIndex = lastOut * (long long)M / L;
    if (firstIndex < firstAbs || (firstAbs < 0 && lastIndex > LLONG_MAX + firstAbs)) return -1;
    const long long lastBase = lastIndex - firstAbs;
    if (lastBase >= histLen || lastBase >= INT_MAX) return -1;
    for (int c = 0; c < ch; ++c) if (!output[c]) return -1;
    std::lock_guard<std::mutex> lock(resampleMutex_);
    static double *d_hist = nullptr, *d_out = nullptr;
    static long long capHist = 0, capOut = 0;
    const long long needHist = histLen * ch, needPhases = (long long)L * T, needOut = nOut * ch;
    if (!grow_buffer(d_hist, capHist, needHist)) return -1;
    if (!grow_buffer(d_out, capOut, needOut)) return -1;
    if (hipMemcpy(d_hist, hist, (size_t)needHist * sizeof(double), hipMemcpyHostToDevice) != hipSuccess) return -1;
    double* d_phases = acquire_phases(phases, (size_t)needPhases);
    if (!d_phases) return -1;
    int shift = -1;
    if ((L & (L - 1)) == 0) { shift = 0; while ((1LL << shift) != L) ++shift; }
    (void)hipGetLastError();
    hipLaunchKernelGGL(compact_planar_kernel,
        dim3((unsigned)((nOut + 255) / 256), (unsigned)ch), dim3(256), 0, 0,
        d_hist, histLen, d_phases, L, M, T, shift, outIdx0, firstAbs, nOut, d_out);
    hipError_t launch = hipGetLastError();
    if (launch != hipSuccess) {
        std::fprintf(stderr, "[rtagpu] compact kernel: %s\n", hipGetErrorString(launch)); return -1;
    }
    if (hipDeviceSynchronize() != hipSuccess) return -1;
    for (int c = 0; c < ch; ++c) {
        if (hipMemcpy(output[c], d_out + (long long)c * nOut,
                      (size_t)nOut * sizeof(double), hipMemcpyDeviceToHost) != hipSuccess) return -1;
        // Content validation is bounded (two samples/channel, not a full host
        // pass). Preserve exactly the same multiply/add order and binary64.
        for (long long n : {0LL, nOut - 1}) {
            const long long num = (outIdx0 + n) * (long long)M;
            const int base = (int)(num / L - firstAbs), phase = (int)(num % L);
            const int end = std::min(base + 1, T);
            double expected = 0.0;
            for (int u = 0; u < end; ++u)
                expected += phases[(long long)phase * T + u] * hist[(long long)c * histLen + base - u];
            if (std::memcmp(&expected, output[c] + n, sizeof(double)) != 0) return -1;
        }
    }
    return (int)nOut;
}
