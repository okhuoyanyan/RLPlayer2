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
static bool phasesCached_ = false;

RTA_EXPORT void rta_gpu_resample_invalidate(void) { phasesCached_ = false; }

RTA_EXPORT int rta_gpu_resample(const double* hist, int ch, long long histLen,
                                const double* phases, int L, int T,
                                const int* bases, const int* phIdx, long long nOut,
                                double* outInterleaved)
{
    if (nOut <= 0 || ch <= 0 || T <= 0) return 0;

    static double* d_hist = nullptr;
    static double* d_phases = nullptr;
    static int*    d_bases = nullptr;
    static int*    d_phIdx = nullptr;
    static double* d_out = nullptr;
    static long long capHist = 0, capPhases = 0, capN = 0;

    const long long needHist = (long long)ch * histLen;
    const long long needPh   = (long long)L * T;

    if (needHist > capHist) {
        if (d_hist) cudaFree(d_hist);
        if (cudaMalloc(&d_hist, needHist * sizeof(double)) != cudaSuccess) return -1;
        capHist = needHist;
    }
    if (needPh > capPhases) {
        if (d_phases) cudaFree(d_phases);
        if (cudaMalloc(&d_phases, needPh * sizeof(double)) != cudaSuccess) return -1;
        capPhases = needPh;
    }
    if (nOut > capN) {
        if (d_bases) cudaFree(d_bases);
        if (d_phIdx) cudaFree(d_phIdx);
        if (d_out)   cudaFree(d_out);
        if (cudaMalloc(&d_bases, nOut * sizeof(int)) != cudaSuccess) return -1;
        if (cudaMalloc(&d_phIdx, nOut * sizeof(int)) != cudaSuccess) return -1;
        if (cudaMalloc(&d_out,   nOut * (long long)ch * sizeof(double)) != cudaSuccess) return -1;
        capN = nOut;
    }

    if (cudaMemcpy(d_hist, hist, needHist * sizeof(double), cudaMemcpyHostToDevice) != cudaSuccess) return -1;
    // 相位表在整首歌里不变 —— 由 setup 函数预先上传一次即可。
    {
        static int cachedL = -1, cachedT = -1;
        if (cachedL != L || cachedT != T || !phasesCached_) {
            if (cudaMemcpy(d_phases, phases, needPh * sizeof(double), cudaMemcpyHostToDevice) != cudaSuccess) return -1;
            cachedL = L; cachedT = T; phasesCached_ = true;
        }
    }
    if (cudaMemcpy(d_bases, bases, nOut * sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) return -1;
    if (cudaMemcpy(d_phIdx, phIdx, nOut * sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) return -1;

    const long long total = nOut * (long long)ch;
    const int threads = 256;
    const long long blocks = (total + threads - 1) / threads;
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
