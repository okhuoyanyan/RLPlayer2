// gpu_conv.cpp — GPU 分块卷积（HIP）：把"频域乘累加"这一步放到 Radeon 上
//
// 动机：HQPlayer 的 GPU offload【只支持 NVIDIA CUDA】（安装目录只有 cufft64_12.dll），
//       而本机 RX 9070 XT 在官方路径下完全闲置。我们用 ROCm/HIP 把它用起来。
//
// 分工：
//   CPU  做每块的 FFT / IFFT（块数多但每块很小，传输可批量摊薄）
//   GPU  做 P 个 IR 分区的频域乘累加 —— 这是长 IR 卷积的主要开销
//
// 验证：与 CPU 直接卷积逐样本比对
// 构建：hipcc --offload-arch=gfx1200 -O3 ...（hipcc 默认 gfx906，必须显式指定）
#include "dsp/convolution.h"
#include "dsp/fft.h"
#include <hip/hip_runtime.h>
#include <cstdio>
#include <vector>
#include <random>
#include <cmath>
#include <chrono>

using namespace rta::dsp;

// 频域乘累加：Y[b][k] = sum_{p=0..min(P-1,b)} H[p][k] * X[b-p][k]
// 网格：(bin, block)，每个线程算一个 (b,k)
__global__ void mac_kernel(const float2* __restrict__ H,   // P x N
                           const float2* __restrict__ X,   // Nb x N（已补零到 b-p<0 的位置为 0）
                           float2* __restrict__ Y,         // Nb x N
                           int P, int Nb, int N) {
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

// 平凡 kernel：只写常数，用来判定"能不能跑"
__global__ void trivial_kernel(float2* __restrict__ Y, int n) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) Y[i] = make_float2(42.0f, 7.0f);
}

static void check_hip(hipError_t e, const char* what) {
    if (e != hipSuccess) { printf("HIP 错误 [%s]: %s\n", what, hipGetErrorString(e)); std::exit(1); }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    size_t B = 1024;                 // 分块大小
    size_t irLen = 262144;           // IR 长度（256k 抽头）
    size_t N = 48000;                // 信号长度（1 秒 @48k）
    if (argc > 1) B = (size_t)atoi(argv[1]);
    if (argc > 2) irLen = (size_t)atoi(argv[2]);
    if (argc > 3) N = (size_t)atoi(argv[3]);

    hipDeviceProp_t prop; check_hip(hipGetDeviceProperties(&prop, 0), "GetDeviceProperties");
    printf("GPU: %s  (gfx%d%d, %d SM, %.1f GB)\n", prop.name, prop.major, prop.minor,
           prop.multiProcessorCount, (double)prop.totalGlobalMem / 1073741824.0);
    printf("参数: 块 %zu, IR %zu 抽头, 信号 %zu 帧\n", B, irLen, N);

    // ---- 先判定 GPU kernel 能不能跑通 ----
    {
        float2* dT = nullptr;
        check_hip(hipMalloc(&dT, 512 * sizeof(float2)), "malloc T");
        check_hip(hipMemset(dT, 0, 512 * sizeof(float2)), "memset T");
        trivial_kernel<<<2, 256>>>(dT, 512);
        const hipError_t le = hipGetLastError();
        const hipError_t se = hipDeviceSynchronize();
        float2 h[4] = {};
        hipMemcpy(h, dT, sizeof(h), hipMemcpyDeviceToHost);
        printf("平凡 kernel: launch=%s sync=%s  写回值=(%.1f,%.1f)\n",
               hipGetErrorString(le), hipGetErrorString(se), (double)h[0].x, (double)h[0].y);
        hipFree(dT);
    }

    std::mt19937 rng(20261019u);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<double> x(N), ir(irLen);
    for (auto& v : x) v = 0.3 * u(rng);
    for (size_t i = 0; i < irLen; ++i) ir[i] = u(rng) * std::exp(-6.0 * (double)i / (double)irLen);

    // ---- CPU 参考：直接卷积 ----
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<double> ref = direct_convolve(x, ir);
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // ---- GPU 分块卷积 ----
    const size_t fftN = B * 2;
    const size_t P = (irLen + B - 1) / B;
    const size_t Nb = (N + B - 1) / B;
    printf("分区 P=%zu, 块数 Nb=%zu, FFT 长度 %zu\n", P, Nb, fftN);

    // 1) 每个 IR 分区的频谱
    std::vector<float2> H((size_t)P * fftN);
    for (size_t p = 0; p < P; ++p) {
        std::vector<cplx> buf(fftN, cplx(0.0, 0.0));
        for (size_t i = 0; i < B; ++i) {
            const size_t k = p * B + i;
            if (k < irLen) buf[i] = cplx(ir[k], 0.0);
        }
        fft(buf);
        for (size_t k = 0; k < fftN; ++k) H[p * fftN + k] = make_float2((float)buf[k].real(), (float)buf[k].imag());
    }

    // 2) 每个输入块的频谱（补零到 Nb*B）
    std::vector<float2> X((size_t)Nb * fftN);
    for (size_t b = 0; b < Nb; ++b) {
        std::vector<cplx> buf(fftN, cplx(0.0, 0.0));
        for (size_t i = 0; i < B; ++i) {
            const size_t n = b * B + i;
            if (n < N) buf[i] = cplx(x[n], 0.0);
        }
        fft(buf);
        for (size_t k = 0; k < fftN; ++k) X[b * fftN + k] = make_float2((float)buf[k].real(), (float)buf[k].imag());
    }

    // 3) 上传 + 计算 + 下载
    float2 *dH = nullptr, *dX = nullptr, *dY = nullptr;
    const auto t1 = std::chrono::steady_clock::now();
    check_hip(hipMalloc(&dH, H.size() * sizeof(float2)), "malloc H");
    check_hip(hipMalloc(&dX, X.size() * sizeof(float2)), "malloc X");
    check_hip(hipMalloc(&dY, X.size() * sizeof(float2)), "malloc Y");
    check_hip(hipMemcpy(dH, H.data(), H.size() * sizeof(float2), hipMemcpyHostToDevice), "copy H");
    check_hip(hipMemcpy(dX, X.data(), X.size() * sizeof(float2), hipMemcpyHostToDevice), "copy X");
    const double xferMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();

    const auto t2 = std::chrono::steady_clock::now();
    dim3 blk(256), grid((unsigned)((fftN + 255) / 256), (unsigned)Nb);
    mac_kernel<<<grid, blk>>>(dH, dX, dY, (int)P, (int)Nb, (int)fftN);
    check_hip(hipDeviceSynchronize(), "kernel");
    const double kernMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();

    std::vector<float2> Y(X.size());
    check_hip(hipMemcpy(Y.data(), dY, Y.size() * sizeof(float2), hipMemcpyDeviceToHost), "copy Y");

    // 4) IFFT + 重叠相加
    std::vector<double> got(N, 0.0);
    std::vector<double> ola(B, 0.0);
    for (size_t b = 0; b < Nb; ++b) {
        std::vector<cplx> buf(fftN);
        for (size_t k = 0; k < fftN; ++k) buf[k] = cplx((double)Y[b * fftN + k].x, (double)Y[b * fftN + k].y);
        fft(buf, /*inverse=*/true);
        for (size_t i = 0; i < B; ++i) {
            const size_t n = b * B + i;
            const double v = buf[i].real() + ola[i];
            if (n < N) got[n] = v;
            ola[i] = buf[B + i].real();
        }
    }
    const double gpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();

    // 诊断：设备端各缓冲是否真的有内容
    {
        double mH = 0, mX = 0, mY = 0;
        for (size_t i = 0; i < H.size(); ++i) mH = std::max(mH, (double)std::fabs(H[i].x));
        for (size_t i = 0; i < X.size(); ++i) mX = std::max(mX, (double)std::fabs(X[i].x));
        for (size_t i = 0; i < Y.size(); ++i) mY = std::max(mY, (double)std::fabs(Y[i].x));
        printf("  诊断: max|H|=%.4f max|X|=%.4f max|Y|=%.4f\n", mH, mX, mY);
        printf("  Y[0..3] = (%.4f,%.4f) (%.4f,%.4f) (%.4f,%.4f) (%.4f,%.4f)\n",
               (double)Y[0].x, (double)Y[0].y, (double)Y[1].x, (double)Y[1].y,
               (double)Y[2].x, (double)Y[2].y, (double)Y[3].x, (double)Y[3].y);
        printf("  X[0..3] = (%.4f,%.4f) (%.4f,%.4f)\n",
               (double)X[0].x, (double)X[0].y, (double)X[1].x, (double)X[1].y);
    }
    hipFree(dH); hipFree(dX); hipFree(dY);

    // ---- 比对 ----
    double maxDiff = 0.0, maxRef = 0.0;
    for (size_t i = 0; i < N; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(got[i] - ref[i]));
        maxRef  = std::max(maxRef,  std::fabs(ref[i]));
    }
    const double rel = maxDiff / (maxRef + 1e-300);

    printf("\n前 6 个样本对比:\n");
    for (size_t i = 0; i < 6 && i < N; ++i)
        printf("  [%zu] ref=%+.6f got=%+.6f diff=%+.3e\n", i, ref[i], got[i], got[i] - ref[i]);
    {
        size_t firstBad = (size_t)-1;
        for (size_t i = 0; i < N; ++i) if (std::fabs(got[i] - ref[i]) > 1e-4 * (maxRef + 1e-30)) { firstBad = i; break; }
        if (firstBad != (size_t)-1) printf("  首个明显偏差出现在样本 %zu (块 %zu, 块内偏移 %zu)\n",
                                           firstBad, firstBad / B, firstBad % B);
    }
    printf("\n=== 结果 ===\n");
    printf("  CPU 直接卷积耗时      : %8.1f ms\n", cpuMs);
    printf("  GPU 传输(H2D)耗时     : %8.1f ms\n", xferMs);
    printf("  GPU kernel 耗时       : %8.1f ms\n", kernMs);
    printf("  GPU 端到端(kernel+IFFT): %8.1f ms\n", gpuMs);
    printf("  最大偏差             : %.3e (相对 %.3e)\n", maxDiff, rel);
    printf("\n  [%s] GPU 分块卷积 == CPU 直接卷积\n", rel < 1e-4 ? "PASS" : "FAIL");
    printf("  [%s] GPU 快于 CPU 单线程直接卷积 (%.1fx)\n",
           cpuMs > gpuMs ? "PASS" : "INFO", cpuMs / (gpuMs + 1e-9));
    return rel < 1e-4 ? 0 : 1;
}
