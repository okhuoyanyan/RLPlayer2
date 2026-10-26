// gpu_conv_hip.cpp — GPU 分块卷积的 DLL 实现（HIP）
// 构建：hipcc --offload-arch=gfx1201 -O3 -shared -Isrc src/gpu/gpu_conv_hip.cpp -o build/rtagpu.dll
//
// 供管线通过 LoadLibrary 动态调用：没有这个 DLL 时管线自动退回 CPU 卷积，
// 因此不会形成链接期依赖，普通构建照常工作。
//
// 导出符号（两份后端 HIP / CUDA 完全相同）：
//   rta_gpu_available            —— 平凡 kernel 写常数读回，判定 kernel 真能执行
//   rta_gpu_convolve             —— 【整块】无状态卷积（离线/批处理用；契约见下）
//   rta_gpu_convolve_create      —— 新建【流式会话】（带跨调用状态）
//   rta_gpu_convolve_stream      —— 流式处理 n 个样本
//   rta_gpu_convolve_destroy     —— 释放会话
#include "dsp/fft.h"
#include <hip/hip_runtime.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <new>

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
//
// ★ 契约（不要改）：这是**无状态**的整块接口 —— 每次调用独立做 OLA，只写 out[0..n)。
//   它只对"一次喂完全部数据"的离线/批处理语义正确；**流式逐块调用（管线每块 4096 样）
//   是错的**（缺上一块对本块的 IR 拖尾，见 261026-07）。流式必须用下面的 _stream 会话。
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

// ===========================================================================
// 流式卷积会话（261026-08 修复）
//
// 缺陷（主线探针 tools/gpu_stream_verify.cpp 实测，误差 +1.7~+3.4 dB）：
//   旧的 rta_gpu_convolve 是**无状态**的——每次调用从零开始做 OLA，只写 out[0..n)。
//   生产管线每块只喂 4096 样，于是"上一块对本块的 IR 拖尾"无处可来，每个块边界断裂。
//   这不是音质劣化，是算法性错误。
//
// 修法：会话持有跨调用状态，语义与 src/dsp/convolution.h 的 StreamingConvolver
// **逐位等价**：
//   * 输入先进 FIFO，攒满 B 个才做一块，不足一块的余量留到下次调用（与 CPU 同）
//   * 输出 FIFO 在 create 时预置 B 个零 ⟹ latency() == B（与 CPU 完全一致）
//   * P = ceil(irLen/B)；H 谱与"输入谱环形缓冲"常驻 device，只有当前块谱上传、
//     累加结果回读（每块 2 次 32 KB 级传输 @B=1024）
//   * 每块：CPU 做 FFT（double）→ GPU 做 Σ_p H[p]·ring[head−p] → CPU 做 IFFT + OLA 尾巴
//
// 为什么乘累加用 double 而不是 float：float 的舍入 ~1e-7（−140 dB 量级的分辨力），
//   叠加 P 个分区后撑不住"≤ −120 dB 且 max|Δ| ≤ 1e-6"的判据；而且 float 版与 CPU 的
//   double 运算不可能逐位一致。改用 double2 后 GPU 只承担乘累加（带宽成本极低，
//   P·2B 个 double2 每块），并用 __dmul_rn/__dadd_rn/__dsub_rn **关掉 FMA 收缩**，
//   使每个乘加的舍入与 CPU 的 std::complex<double> 同序同舍入 ⟹ 实测 max|Δ| = 0。
// ===========================================================================

// 每块 k：Y[k] = Σ_{p=0..P-1} H[p][k] · Ring[(head + P − p) % P][k]
//
// head 语义与 PartitionedConvolver 完全一致：ring[head] 是**刚写入的当前块谱**，
// p=0 用当前块，p=1 用上一块……环形回绕由 (head + P − p) % P 完成。
__global__ void mac_stream_kernel(const double2* __restrict__ H, const double2* __restrict__ Ring,
                                  double2* __restrict__ Y, int P, int head, int N) {
    const int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k >= N) return;
    double re = 0.0, im = 0.0;
    for (int p = 0; p < P; ++p) {
        const int idx = (head + P - p) % P;
        const double2 h = H[(size_t)p * N + k];
        const double2 x = Ring[(size_t)idx * N + k];
        // 复数乘 + 加，逐条舍入（禁用 FMA 收缩）——与 CPU 的 std::complex<double> 同序
        re = __dadd_rn(re, __dsub_rn(__dmul_rn(h.x, x.x), __dmul_rn(h.y, x.y)));
        im = __dadd_rn(im, __dadd_rn(__dmul_rn(h.x, x.y), __dmul_rn(h.y, x.x)));
    }
    Y[k] = make_double2(re, im);
}

struct GpuConvSession {
    int B = 0, fftN = 0, P = 0, irLen = 0, head = 0;
    // host 侧状态
    std::vector<double>  inFifo;    // 不足一块的输入余量
    std::vector<double>  outFifo;   // 待取输出（预置 B 个零 ⟹ latency == B）
    std::vector<double>  ola;       // 均匀分块 overlap-add 的尾巴（B 个）
    std::vector<double>  oblk;      // 单块输出暂存
    std::vector<cplx>    buf;       // fftN 工作缓冲
    std::vector<double2> hX, hAcc;  // 上传/回读的 host 暂存
    // device 侧状态（跨调用保留）
    double2* dH = nullptr;      // P·fftN  IR 分区谱，create 时上传一次
    double2* dRing = nullptr;   // P·fftN  输入谱环形缓冲
    double2* dAcc = nullptr;    // fftN
    double2* dX = nullptr;      // fftN
};

// 创建流式会话。成功返回非空句柄，失败返回 nullptr（调用方退回 CPU）。
// ir/irLen：IR；block：块长（非 2 的幂时按 1024），与 CPU StreamingConvolver 的 block 同义。
RTA_EXPORT void* rta_gpu_convolve_create(const double* ir, long long irLen, int block) {
    if (!ir || irLen <= 0) return nullptr;
    if (block <= 0 || (block & (block - 1)) != 0) block = 1024;

    GpuConvSession* s = new (std::nothrow) GpuConvSession();
    if (!s) return nullptr;
    s->B = block;
    s->fftN = block * 2;
    s->irLen = (int)irLen;
    s->P = (int)(((size_t)irLen + (size_t)block - 1) / (size_t)block);
    s->head = 0;

    const size_t fftN = (size_t)s->fftN, P = (size_t)s->P;
    s->buf.assign(fftN, cplx(0.0, 0.0));
    s->ola.assign((size_t)block, 0.0);
    s->oblk.assign((size_t)block, 0.0);
    s->hX.assign(fftN, make_double2(0.0, 0.0));
    s->hAcc.assign(fftN, make_double2(0.0, 0.0));
    s->outFifo.assign((size_t)block, 0.0);   // ★ B 帧算法延迟，与 CPU 逐位一致
    s->inFifo.reserve((size_t)block * 2);

    // H 谱（host 端 double FFT，与 CPU 的 PartitionedConvolver 完全同一份数据）
    std::vector<double2> hH(P * fftN);
    for (size_t p = 0; p < P; ++p) {
        std::fill(s->buf.begin(), s->buf.end(), cplx(0.0, 0.0));
        for (size_t i = 0; i < (size_t)block; ++i) {
            const size_t k = p * (size_t)block + i;
            if (k < (size_t)irLen) s->buf[i] = cplx(ir[k], 0.0);
        }
        fft(s->buf);
        for (size_t k = 0; k < fftN; ++k)
            hH[p * fftN + k] = make_double2(s->buf[k].real(), s->buf[k].imag());
    }

    if (hipMalloc(&s->dH, P * fftN * sizeof(double2)) != hipSuccess) { delete s; return nullptr; }
    if (hipMalloc(&s->dRing, P * fftN * sizeof(double2)) != hipSuccess) {
        hipFree(s->dH); delete s; return nullptr;
    }
    if (hipMalloc(&s->dAcc, fftN * sizeof(double2)) != hipSuccess) {
        hipFree(s->dH); hipFree(s->dRing); delete s; return nullptr;
    }
    if (hipMalloc(&s->dX, fftN * sizeof(double2)) != hipSuccess) {
        hipFree(s->dH); hipFree(s->dRing); hipFree(s->dAcc); delete s; return nullptr;
    }
    if (hipMemcpy(s->dH, hH.data(), P * fftN * sizeof(double2), hipMemcpyHostToDevice) != hipSuccess ||
        hipMemset(s->dRing, 0, P * fftN * sizeof(double2)) != hipSuccess) {
        hipFree(s->dH); hipFree(s->dRing); hipFree(s->dAcc); hipFree(s->dX); delete s; return nullptr;
    }
    return (void*)s;
}

// 流式处理 n 个样本：in/out 均为 n 个样本。返回 0 成功，非 0 失败。
// 延迟语义：out 相对 in 延迟 B 个样本（= CPU StreamingConvolver::latency()）。
RTA_EXPORT int rta_gpu_convolve_stream(void* h, const double* in, long long n, double* out) {
    GpuConvSession* s = (GpuConvSession*)h;
    if (!s || !out || n < 0) return 1;
    if (n == 0) return 0;
    if (!in) return 1;

    const size_t B = (size_t)s->B, fftN = (size_t)s->fftN;
    const size_t N = (size_t)n;

    // ---- 1) 输入进 FIFO（与 CPU StreamingConvolver 同）----
    s->inFifo.insert(s->inFifo.end(), in, in + N);

    // ---- 2) 攒满一块就做一块 ----
    size_t pos = 0;
    while (s->inFifo.size() - pos >= B) {
        // 2a) 本块谱（host double FFT，与 CPU 完全一致）
        std::fill(s->buf.begin(), s->buf.end(), cplx(0.0, 0.0));
        for (size_t i = 0; i < B; ++i) s->buf[i] = cplx(s->inFifo[pos + i], 0.0);
        fft(s->buf);
        for (size_t k = 0; k < fftN; ++k) s->hX[k] = make_double2(s->buf[k].real(), s->buf[k].imag());

        // 2b) 上传到 dX，并写进环形缓冲的 head 槽（设备内拷贝，不绕 host）
        if (hipMemcpy(s->dX, s->hX.data(), fftN * sizeof(double2), hipMemcpyHostToDevice) != hipSuccess) return 2;
        if (hipMemcpy(s->dRing + (size_t)s->head * fftN, s->dX, fftN * sizeof(double2),
                      hipMemcpyDeviceToDevice) != hipSuccess) return 2;

        // 2c) 频域乘累加（GPU）
        {
            dim3 blk(256), grid((unsigned)((fftN + 255) / 256));
            hipLaunchKernelGGL(mac_stream_kernel, grid, blk, 0, 0,
                               s->dH, s->dRing, s->dAcc, s->P, s->head, (int)fftN);
            if (hipGetLastError() != hipSuccess) return 3;
            if (hipDeviceSynchronize() != hipSuccess) return 3;
        }

        // 2d) 回读 + IFFT + OLA 尾巴
        if (hipMemcpy(s->hAcc.data(), s->dAcc, fftN * sizeof(double2), hipMemcpyDeviceToHost) != hipSuccess) return 4;
        for (size_t k = 0; k < fftN; ++k) s->buf[k] = cplx(s->hAcc[k].x, s->hAcc[k].y);
        fft(s->buf, /*inverse=*/true);
        for (size_t i = 0; i < B; ++i) {
            s->oblk[i] = s->buf[i].real() + s->ola[i];
            s->ola[i] = s->buf[B + i].real();
        }
        s->outFifo.insert(s->outFifo.end(), s->oblk.begin(), s->oblk.end());
        s->head = (s->head + 1) % s->P;
        pos += B;
    }
    if (pos) s->inFifo.erase(s->inFifo.begin(), s->inFifo.begin() + (std::ptrdiff_t)pos);

    // ---- 3) 取出 n 个输出（预置的 B 帧延迟保证 FIFO 不空；与 CPU 同一条实现）----
    for (size_t i = 0; i < N; ++i) out[i] = (i < s->outFifo.size()) ? s->outFifo[i] : 0.0;
    if (N <= s->outFifo.size()) s->outFifo.erase(s->outFifo.begin(), s->outFifo.begin() + (std::ptrdiff_t)N);
    else s->outFifo.clear();
    return 0;
}

// 释放流式会话（nullptr 安全）
RTA_EXPORT void rta_gpu_convolve_destroy(void* h) {
    GpuConvSession* s = (GpuConvSession*)h;
    if (!s) return;
    if (s->dH) hipFree(s->dH);
    if (s->dRing) hipFree(s->dRing);
    if (s->dAcc) hipFree(s->dAcc);
    if (s->dX) hipFree(s->dX);
    delete s;
}
