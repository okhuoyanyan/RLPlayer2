// gpu_conv_hip.cpp — GPU 分块卷积的 DLL 实现（HIP）
// 构建：hipcc --offload-arch=gfx1201 -O3 -shared -Isrc src/gpu/gpu_conv_hip.cpp -o build/rtagpu.dll
//
// 供管线通过 LoadLibrary 动态调用：没有这个 DLL 时管线自动退回 CPU 卷积，
// 因此不会形成链接期依赖，普通构建照常工作。
#include "dsp/fft.h"
#include <hip/hip_runtime.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>

using namespace rta::dsp;

#ifdef _WIN32
#define RTA_EXPORT extern "C" __declspec(dllexport)
#else
#define RTA_EXPORT extern "C"
#endif

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
    if (hipGetDeviceCount(&n) != hipSuccess || n <= 0) return 0;
    // 真正验证 kernel 能执行：写常数并读回（架构不匹配时会静默失败）
    float2* d = nullptr;
    if (hipMalloc(&d, 256 * sizeof(float2)) != hipSuccess) return 0;
    hipMemset(d, 0, 256 * sizeof(float2));
    // 用一个极简 kernel 验证
    extern __global__ void probe_kernel(float2*);
    hipLaunchKernelGGL(probe_kernel, dim3(1), dim3(256), 0, 0, d);
    const hipError_t le = hipGetLastError();
    hipDeviceSynchronize();
    float2 h0{};
    hipMemcpy(&h0, d, sizeof(h0), hipMemcpyDeviceToHost);
    hipFree(d);
    return (le == hipSuccess && h0.x == 12345.0f) ? 1 : 0;
}

__global__ void probe_kernel(float2* y) {
    const int i = threadIdx.x;
    if (i < 256) y[i] = make_float2(12345.0f, 0.0f);
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
    if (hipMalloc(&dH, H.size() * sizeof(float2)) != hipSuccess) return 2;
    if (hipMalloc(&dX, X.size() * sizeof(float2)) != hipSuccess) { hipFree(dH); return 2; }
    if (hipMalloc(&dY, Y.size() * sizeof(float2)) != hipSuccess) { hipFree(dH); hipFree(dX); return 2; }

    int rc = 0;
    if (hipMemcpy(dH, H.data(), H.size() * sizeof(float2), hipMemcpyHostToDevice) != hipSuccess) rc = 3;
    if (!rc && hipMemcpy(dX, X.data(), X.size() * sizeof(float2), hipMemcpyHostToDevice) != hipSuccess) rc = 3;
    if (!rc) {
        dim3 blk(256), grid((unsigned)((fftN + 255) / 256), (unsigned)Nb);
        hipLaunchKernelGGL(mac_kernel, grid, blk, 0, 0, dH, dX, dY, (int)P, (int)Nb, (int)fftN);
        if (hipGetLastError() != hipSuccess) rc = 4;
        else if (hipDeviceSynchronize() != hipSuccess) rc = 4;
    }
    if (!rc && hipMemcpy(Y.data(), dY, Y.size() * sizeof(float2), hipMemcpyDeviceToHost) != hipSuccess) rc = 5;

    hipFree(dH); hipFree(dX); hipFree(dY);
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
