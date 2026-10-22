// gpu_resample_hip.cpp — 多相升频的 GPU 核
//
// 内层循环是「每个输出样本 = 某个相位的 T 个抽头 · 最近 T 个输入样本」。
// 这是典型的批量点积，非常适合 GPU：一个线程算一个输出样本。
//
// 与 CPU 的对应关系：
//   p      = 输出样本 n 对应的相位索引（0..L-1）
//   base   = 该输出样本所需的最新输入样本在 hist 中的下标
//   acc    = sum_{u=0..T-1} phases[p][u] * hist[c][base-u]
// 索引计算（inIdx/phase 的选择）留在 CPU —— 它很便宜，且改动风险最小。
//
// ★ 架构必须显式指定 gfx1201（Navi 48）。写错时 kernel 会静默不执行。
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
static bool phasesCached_ = false;

extern "C" __declspec(dllexport)
void rta_gpu_resample_invalidate() { phasesCached_ = false; }

extern "C" __declspec(dllexport)
int rta_gpu_resample(const double* hist, int ch, long long histLen,
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
        if (d_hist) hipFree(d_hist);
        if (hipMalloc(&d_hist, needHist * sizeof(double)) != hipSuccess) return -1;
        capHist = needHist;
    }
    if (needPh > capPhases) {
        if (d_phases) hipFree(d_phases);
        if (hipMalloc(&d_phases, needPh * sizeof(double)) != hipSuccess) return -1;
        capPhases = needPh;
    }
    if (nOut > capN) {
        if (d_bases) hipFree(d_bases);
        if (d_phIdx) hipFree(d_phIdx);
        if (d_out)   hipFree(d_out);
        if (hipMalloc(&d_bases, nOut * sizeof(int)) != hipSuccess) return -1;
        if (hipMalloc(&d_phIdx, nOut * sizeof(int)) != hipSuccess) return -1;
        if (hipMalloc(&d_out,   nOut * (long long)ch * sizeof(double)) != hipSuccess) return -1;
        capN = nOut;
    }

    if (hipMemcpy(d_hist, hist, needHist * sizeof(double), hipMemcpyHostToDevice) != hipSuccess) return -1;
    // 相位表在整首歌里不变 —— 由 setup 函数预先上传一次即可。
    // 之前每次调用都传 2 MB（16 x 16384 doubles），这是加速比只有 7.5x 的主因。
    {
        static int cachedL = -1, cachedT = -1;
        if (cachedL != L || cachedT != T || !phasesCached_) {
            if (hipMemcpy(d_phases, phases, needPh * sizeof(double), hipMemcpyHostToDevice) != hipSuccess) return -1;
            cachedL = L; cachedT = T; phasesCached_ = true;
        }
    }
    if (hipMemcpy(d_bases, bases, nOut * sizeof(int), hipMemcpyHostToDevice) != hipSuccess) return -1;
    if (hipMemcpy(d_phIdx, phIdx, nOut * sizeof(int), hipMemcpyHostToDevice) != hipSuccess) return -1;

    const long long total = nOut * (long long)ch;
    const int threads = 256;
    const long long blocks = (total + threads - 1) / threads;
    hipLaunchKernelGGL(resample_kernel, dim3((unsigned)blocks), dim3(threads), 0, 0,
                       d_hist, ch, histLen, d_phases, T, d_bases, d_phIdx, nOut, d_out);

    hipError_t le = hipGetLastError();
    if (le != hipSuccess) { std::fprintf(stderr, "[rtagpu] resample kernel: %s\n", hipGetErrorString(le)); return -1; }
    if (hipDeviceSynchronize() != hipSuccess) return -1;
    if (hipMemcpy(outInterleaved, d_out, total * sizeof(double), hipMemcpyDeviceToHost) != hipSuccess) return -1;
    return (int)nOut;
}
