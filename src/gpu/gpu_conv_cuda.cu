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
//       导出符号与 HIP 版**完全相同**（rta_gpu_available / rta_gpu_convolve /
//       rta_gpu_convolve_create / rta_gpu_convolve_stream / rta_gpu_convolve_destroy），
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
#include <algorithm>
#include <new>

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

// ===========================================================================
// 流式卷积会话（261026-08 修复）—— 与 HIP 版逐行对应
//
// 缺陷（主线探针 tools/gpu_stream_verify.cpp 实测，误差 +1.7~+3.4 dB）：
//   旧的 rta_gpu_convolve 是**无状态**的——每次调用从零开始做 OLA，只写 out[0..n)。
//   生产管线每块只喂 4096 样，于是"上一块对本块的 IR 拖尾"无处可来，每个块边界断裂。
//
// 修法：会话持有跨调用状态，语义与 src/dsp/convolution.h 的 StreamingConvolver
// **逐位等价**：
//   * 输入先进 FIFO，攒满 B 个才做一块，不足一块的余量留到下次调用（与 CPU 同）
//   * 输出 FIFO 在 create 时预置 B 个零 ⟹ latency() == B（与 CPU 完全一致）
//   * P = ceil(irLen/B)；H 谱与"输入谱环形缓冲"常驻 device
//   * 每块：CPU 做 FFT（double）→ GPU 做 Σ_p H[p]·ring[head−p] → CPU 做 IFFT + OLA 尾巴
//
// 为什么乘累加用 double 而不是 float：float 的舍入 ~1e-7 撑不住"≤ −120 dB 且
//   max|Δ| ≤ 1e-6"的判据，也不可能与 CPU 的 double 运算逐位一致。改用 double2 后
//   GPU 只承担乘累加，并用 __dmul_rn/__dadd_rn/__dsub_rn **关掉 FMA 收缩**，
//   使每个乘加的舍入与 CPU 的 std::complex<double> 同序同舍入 ⟹ 实测 max|Δ| = 0。
// ===========================================================================

// 每块 k：Y[k] = Σ_{p=0..P-1} H[p][k] · Ring[(head + P − p) % P][k]
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

    if (cudaMalloc(&s->dH, P * fftN * sizeof(double2)) != cudaSuccess) { delete s; return nullptr; }
    if (cudaMalloc(&s->dRing, P * fftN * sizeof(double2)) != cudaSuccess) {
        cudaFree(s->dH); delete s; return nullptr;
    }
    if (cudaMalloc(&s->dAcc, fftN * sizeof(double2)) != cudaSuccess) {
        cudaFree(s->dH); cudaFree(s->dRing); delete s; return nullptr;
    }
    if (cudaMalloc(&s->dX, fftN * sizeof(double2)) != cudaSuccess) {
        cudaFree(s->dH); cudaFree(s->dRing); cudaFree(s->dAcc); delete s; return nullptr;
    }
    if (cudaMemcpy(s->dH, hH.data(), P * fftN * sizeof(double2), cudaMemcpyHostToDevice) != cudaSuccess ||
        cudaMemset(s->dRing, 0, P * fftN * sizeof(double2)) != cudaSuccess) {
        cudaFree(s->dH); cudaFree(s->dRing); cudaFree(s->dAcc); cudaFree(s->dX); delete s; return nullptr;
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
        if (cudaMemcpy(s->dX, s->hX.data(), fftN * sizeof(double2), cudaMemcpyHostToDevice) != cudaSuccess) return 2;
        if (cudaMemcpy(s->dRing + (size_t)s->head * fftN, s->dX, fftN * sizeof(double2),
                       cudaMemcpyDeviceToDevice) != cudaSuccess) return 2;

        // 2c) 频域乘累加（GPU）
        {
            dim3 blk(256), grid((unsigned)((fftN + 255) / 256));
            mac_stream_kernel<<<grid, blk>>>(s->dH, s->dRing, s->dAcc, s->P, s->head, (int)fftN);
            if (cudaGetLastError() != cudaSuccess) return 3;
            if (cudaDeviceSynchronize() != cudaSuccess) return 3;
        }

        // 2d) 回读 + IFFT + OLA 尾巴
        if (cudaMemcpy(s->hAcc.data(), s->dAcc, fftN * sizeof(double2), cudaMemcpyDeviceToHost) != cudaSuccess) return 4;
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
    if (s->dH) cudaFree(s->dH);
    if (s->dRing) cudaFree(s->dRing);
    if (s->dAcc) cudaFree(s->dAcc);
    if (s->dX) cudaFree(s->dX);
    delete s;
}
