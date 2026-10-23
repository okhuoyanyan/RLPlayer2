// gpu_conv_cuda.cu — GPU 分块卷积的 DLL 实现（CUDA / NVIDIA）
//
// 与 src/gpu/gpu_conv_hip.cpp **逐行对应**：算法（CPU 做每块的 FFT/IFFT，
// GPU 只做 P 个 IR 分区的频域乘累加）一模一样，只把 HIP 运行时换成 CUDA 运行时。
// 刻意不抽公共头文件：两份实现互为参照物（铁律 3——判据要能真的证明东西，
// 两份独立实现给出同一结果，比一份自证可信得多）。
//
// 分工（与 HIP 版相同）：
//   CPU  每块的 FFT / IFFT（块多但每块小，传输可批量摊薄）
//   GPU  P 个 IR 分区的频域乘累加 —— 长 IR 卷积的主要开销
//   kernel: Y[b][k] = Σ_{p=0..min(P-1,b)} H[p][k] · X[b-p][k]
//
// 接入：src/dsp/gpu_bridge.h 通过 LoadLibrary("rtagpu_cuda.dll") 动态加载，
//       导出符号与 HIP 版**完全相同**（rta_gpu_available / rta_gpu_convolve），
//       所以加载层、管线、测试一行都不用改。DLL 缺失或 kernel 不可执行 → 自动退回 CPU。
//
// 构建：tools/build_gpu_cuda.ps1（nvcc -shared，多 arch fatbin + PTX）
//
// ★ 铁律 5 在 CUDA 侧的对应事实（不是照抄，是同一类陷阱的两个形态）：
//   1. arch 不匹配时 CUDA 会报 cudaErrorNoKernelImageForDevice，**但 fatbin 里带 PTX
//      时驱动会 JIT，于是"跑得起来"**——所以不能只看编译开关，要真跑真读回。
//   2. 上下文若已进入 sticky error（更早的调用留下的），后续 cudaGetLastError()
//      会一直返回那个旧错。所以探测前先清一次 error。
//   3. "kernel 启动了"≠"kernel 写了东西"：必须把输出读回比对常数（与 HIP 版同一判据）。
#include "dsp/fft.h"
#include <cuda_runtime.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>

using namespace rta::dsp;

#ifdef _WIN32
#define RTA_EXPORT extern "C" __declspec(dllexport)
#else
#define RTA_EXPORT extern "C"
#endif

// ---------------------------------------------------------------------------
// 平凡 kernel：只写常数。必须先于一切算法调用——「能不能执行」是独立问题，
// 「算得对不对」是下一个问题，混在一起会把"全零输出"误判成算法错误（Round 15 教训）。
// ---------------------------------------------------------------------------
__global__ void probe_kernel(float2* y) {
    const int i = threadIdx.x;
    if (i < 256) y[i] = make_float2(12345.0f, 0.0f);
}

// 频域乘累加：Y[b][k] = sum_{p=0..min(P-1,b)} H[p][k] * X[b-p][k]
__global__ void mac_kernel(const float2* __restrict__ H, const float2* __restrict__ X,
                           float2* __restrict__ Y, int P, int Nb, int N) {
    const int k = blockIdx.x * blockDim.x + threadIdx.x;
    const int b = blockIdx.y;
    if (k >= N || b >= Nb) return;
    const int pmax = (b < P - 1) ? b : (P - 1);
    float2 acc = make_float2(0.f, 0.f);
    for (int p = 0; p <= pmax; ++p) {
        const float2 h = H[(size_t)p * N + k];
        const float2 x = X[(size_t)(b - p) * N + k];
        acc.x += h.x * x.x - h.y * x.y;
        acc.y += h.x * x.y + h.y * x.x;
    }
    Y[(size_t)b * N + k] = acc;
}

RTA_EXPORT int rta_gpu_available(void) {
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess || n <= 0) return 0;

    // 清掉可能残留的 sticky error，否则下面的 cudaGetLastError() 会报一个与本次无关的旧错
    (void)cudaGetLastError();

    float2* d = nullptr;
    if (cudaMalloc(&d, 256 * sizeof(float2)) != cudaSuccess) return 0;
    cudaMemset(d, 0, 256 * sizeof(float2));
    probe_kernel<<<1, 256>>>(d);
    const cudaError_t le = cudaGetLastError();          // 启动失败（含 no kernel image）
    const cudaError_t se = cudaDeviceSynchronize();     // 执行期失败（越界、非法指令）
    float2 h0{};
    const cudaError_t ce = cudaMemcpy(&h0, d, sizeof(h0), cudaMemcpyDeviceToHost);
    cudaFree(d);
    if (le != cudaSuccess || se != cudaSuccess || ce != cudaSuccess) {
        // 诊断信息走 stderr：DLL 没有控制台，但调用方（管线/测试）能看到
        std::fprintf(stderr, "[rtagpu_cuda] probe 失败: launch=%s sync=%s copy=%s\n",
                     cudaGetErrorString(le), cudaGetErrorString(se), cudaGetErrorString(ce));
        return 0;
    }
    return (h0.x == 12345.0f) ? 1 : 0;
}

// 整块卷积：out = conv(in, ir)，长度 n
// 返回 0 成功，非 0 失败
RTA_EXPORT int rta_gpu_convolve(const double* in, long long n,
                                const double* ir, long long irLen,
                                double* out, int block) {
    if (!in || !ir || !out || n <= 0 || irLen <= 0) return 1;
    const size_t B = (block > 0 && (block & (block - 1)) == 0) ? (size_t)block : 1024;
    const size_t N = (size_t)n;
    const size_t fftN = B * 2;
    const size_t P = ((size_t)irLen + B - 1) / B;
    const size_t Nb = (N + B - 1) / B;

    std::vector<float2> H(P * fftN), X(Nb * fftN), Y(Nb * fftN);
    std::vector<cplx> buf(fftN);

    for (size_t p = 0; p < P; ++p) {
        std::fill(buf.begin(), buf.end(), cplx(0.0, 0.0));
        for (size_t i = 0; i < B; ++i) {
            const size_t k = p * B + i;
            if (k < (size_t)irLen) buf[i] = cplx(ir[k], 0.0);
        }
        fft(buf);
        for (size_t k = 0; k < fftN; ++k) H[p * fftN + k] = make_float2((float)buf[k].real(), (float)buf[k].imag());
    }
    for (size_t b = 0; b < Nb; ++b) {
        std::fill(buf.begin(), buf.end(), cplx(0.0, 0.0));
        for (size_t i = 0; i < B; ++i) {
            const size_t k = b * B + i;
            if (k < N) buf[i] = cplx(in[k], 0.0);
        }
        fft(buf);
        for (size_t k = 0; k < fftN; ++k) X[b * fftN + k] = make_float2((float)buf[k].real(), (float)buf[k].imag());
    }

    float2 *dH = nullptr, *dX = nullptr, *dY = nullptr;
    if (cudaMalloc(&dH, H.size() * sizeof(float2)) != cudaSuccess) return 2;
    if (cudaMalloc(&dX, X.size() * sizeof(float2)) != cudaSuccess) { cudaFree(dH); return 2; }
    if (cudaMalloc(&dY, Y.size() * sizeof(float2)) != cudaSuccess) { cudaFree(dH); cudaFree(dX); return 2; }

    int rc = 0;
    if (cudaMemcpy(dH, H.data(), H.size() * sizeof(float2), cudaMemcpyHostToDevice) != cudaSuccess) rc = 3;
    if (!rc && cudaMemcpy(dX, X.data(), X.size() * sizeof(float2), cudaMemcpyHostToDevice) != cudaSuccess) rc = 3;
    if (!rc) {
        dim3 blk(256), grid((unsigned)((fftN + 255) / 256), (unsigned)Nb);
        mac_kernel<<<grid, blk>>>(dH, dX, dY, (int)P, (int)Nb, (int)fftN);
        if (cudaGetLastError() != cudaSuccess) rc = 4;
        else if (cudaDeviceSynchronize() != cudaSuccess) rc = 4;
    }
    if (!rc && cudaMemcpy(Y.data(), dY, Y.size() * sizeof(float2), cudaMemcpyDeviceToHost) != cudaSuccess) rc = 5;

    cudaFree(dH); cudaFree(dX); cudaFree(dY);
    if (rc) return rc;

    std::vector<double> ola(B, 0.0);
    for (size_t b = 0; b < Nb; ++b) {
        for (size_t k = 0; k < fftN; ++k) buf[k] = cplx((double)Y[b * fftN + k].x, (double)Y[b * fftN + k].y);
        fft(buf, /*inverse=*/true);
        for (size_t i = 0; i < B; ++i) {
            const size_t k = b * B + i;
            const double v = buf[i].real() + ola[i];
            if (k < N) out[k] = v;
            ola[i] = buf[B + i].real();
        }
    }
    return 0;
}
