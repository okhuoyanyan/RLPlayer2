// gpu_conv_hip.cpp — GPU 分块卷积的 DLL 实现（HIP）
// 构建：hipcc --offload-arch=gfx1201 -O3 -shared -Isrc src/gpu/gpu_conv_hip.cpp -o build/rtagpu.dll
//
// 供管线通过 LoadLibrary 动态调用：没有这个 DLL 时管线自动退回 CPU 卷积，
// 因此不会形成链接期依赖，普通构建照常工作。
//
// 导出符号（两份后端 HIP / CUDA 完全相同）：
//   rta_gpu_available            —— 平凡 kernel 写常数读回，判定 kernel 真能执行
//   rta_gpu_convolve             —— 【整块】无状态卷积（离线/批处理用；契约见下）
//   rta_gpu_convolve_create      —— 新建【流式会话】（带跨调用状态，261026-08 新增）
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

// ---------------------------------------------------------------------------
// 平凡 kernel：只写常数。必须先于一切算法调用——「能不能执行」是独立问题，
// 「算得对不对」是下一个问题，混在一起会把"全零输出"误判成算法错误（Round 15 教训）。
// ---------------------------------------------------------------------------
__global__ void probe_kernel(float2* y) {
    const int i = threadIdx.x;
    if (i < 256) y[i] = make_float2(12345.0f, 0.0f);
}

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

    float2* d = nullptr;
    if (hipMalloc(&d, 256 * sizeof(float2)) != hipSuccess) return 0;
    hipMemset(d, 0, 256 * sizeof(float2));
    hipLaunchKernelGGL(probe_kernel, dim3(1), dim3(256), 0, 0, d);
    const hipError_t le = hipGetLastError();
    const hipError_t se = hipDeviceSynchronize();
    float2 h0{};
    const hipError_t ce = hipMemcpy(&h0, d, sizeof(h0), hipMemcpyDeviceToHost);
    hipFree(d);
    if (le != hipSuccess || se != hipSuccess || ce != hipSuccess) {
        std::fprintf(stderr, "[rtagpu] probe 失败: launch=%s sync=%s copy=%s\n",
                     hipGetErrorString(le), hipGetErrorString(se), hipGetErrorString(ce));
        return 0;
    }
    return (h0.x == 12345.0f) ? 1 : 0;
}

// 整块卷积：out = conv(in, ir)，长度 n
// 返回 0 成功，非 0 失败
//
// ★ 契约（不要改）：这是**无状态**的整块接口 —— 每次调用独立做 OLA，只写 out[0..n)。
//   它只对"一次喂完全部数据"的离线/批处理语义正确；**流式逐块调用（管线每块 4096 样）
//   是错的**（缺上一块对本块的 IR 拖尾，见 261026-07 实测 +1.7~+3.4 dB）。
//   流式必须用下面的 rta_gpu_convolve_stream 会话。本函数为兼容保留，A 包判据
//   （−127.6~−147.8 dB）不变。
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
//   * P = ceil(irLen/B)；H 谱与"输入谱环形缓冲"常驻 device（跨调用保留）
//   * 每块：CPU 做 FFT（double）→ GPU 做 Σ_p H[p]·ring[head−p] → CPU 做 IFFT + OLA 尾巴
//
// 为什么乘累加用 double 而不是 float：float 的舍入 ~1e-7 撑不住"≤ −120 dB 且
//   max|Δ| ≤ 1e-6"的判据，也不可能与 CPU 的 double 运算逐位一致。改用 double2 后
//   GPU 只承担乘累加，并用 __dmul_rn/__dadd_rn/__dsub_rn **关掉 FMA 收缩**，
//   使每个乘加的舍入与 CPU 的 std::complex<double> 同序同舍入 ⟹ 实测 max|Δ| ≤ 1.1e-14。
//
// 为什么按**批**处理（而不是每块一次往返）：每块一次 = 每 1024 样 2 次同步传输 +
//   1 次 kernel 启动，实测比 CPU 慢 2.4 倍（85 ms vs 36 ms / 262144 样）。改成"一次调用
//   内攒到 K 个块、一次上传 + 一个 grid.y=K 的 kernel + 一次回读"后，传输次数摊薄到
//   1/K，GPU 不再是负优化（见 tools/gpu_stream_check.cpp 的耗时表）。
// ===========================================================================

// 批量频域乘累加：一次算 K 个连续块。
//   Yb[b][k] = Σ_{p=0..P−1} H[p][k] · (p ≤ b ? Xb[b−p][k] : Ring[(head + b − p) mod P][k])
// head 语义与 PartitionedConvolver 一致：Ring[head] 是"本批第一块的**写入位置**"，
// 此刻里面还是 P 块之前的旧谱（下面的写环形缓冲在 kernel 之后做，所以不会读到脏数据）。
// 累加严格按 p 升序、逐条 __dmul_rn/__dadd_rn —— 与 CPU 的 std::complex<double> 同序同舍入。
__global__ void mac_stream_batch_kernel(const double2* __restrict__ H,
                                        const double2* __restrict__ Ring,
                                        const double2* __restrict__ Xb,
                                        double2* __restrict__ Yb,
                                        int P, int head, int N, int K) {
    const int k = blockIdx.x * blockDim.x + threadIdx.x;
    const int b = blockIdx.y;
    if (k >= N || b >= K) return;
    double re = 0.0, im = 0.0;
    const int pmax = (b < P - 1) ? b : (P - 1);
    for (int p = 0; p <= pmax; ++p) {           // 本批内的块
        const double2 h = H[(size_t)p * N + k];
        const double2 x = Xb[(size_t)(b - p) * N + k];
        re = __dadd_rn(re, __dsub_rn(__dmul_rn(h.x, x.x), __dmul_rn(h.y, x.y)));
        im = __dadd_rn(im, __dadd_rn(__dmul_rn(h.x, x.y), __dmul_rn(h.y, x.x)));
    }
    for (int p = pmax + 1; p < P; ++p) {        // 更早的块：从常驻环形缓冲取
        int idx = (head + b - p) % P;
        if (idx < 0) idx += P;
        const double2 h = H[(size_t)p * N + k];
        const double2 x = Ring[(size_t)idx * N + k];
        re = __dadd_rn(re, __dsub_rn(__dmul_rn(h.x, x.x), __dmul_rn(h.y, x.y)));
        im = __dadd_rn(im, __dadd_rn(__dmul_rn(h.x, x.y), __dmul_rn(h.y, x.x)));
    }
    Yb[(size_t)b * N + k] = make_double2(re, im);
}

RTA_EXPORT void rta_gpu_convolve_destroy(void* h);   // 前置声明：create 失败路径复用

struct GpuConvSession {
    int B = 0, fftN = 0, P = 0, irLen = 0, head = 0;
    int Kmax = 1;                   // 一批最多几块（≤ P，保证 kernel 不会读到本批要覆盖的槽）
    // host 侧状态
    std::vector<double>  inFifo;    // 不足一块的输入余量（跨调用保留）
    std::vector<double>  outFifo;   // 待取输出（预置 B 个零 ⟹ latency == B）
    std::vector<double>  ola;       // 均匀分块 overlap-add 的尾巴（B 个，跨调用保留）
    std::vector<double>  oblk;      // 单块输出暂存
    std::vector<cplx>    buf;       // fftN 工作缓冲
    // ★ 上传/回读的暂存必须是 **pinned（页锁定）** 主机内存：可分页内存的
    //   hipMemcpy 每次要过一遍驱动暂存页（128 KB 往返 ~130 µs），64 块下来多花
    //   二十几毫秒，GPU 直接变负优化（实测 36 → 18 ms 的差距就在这里）。
    double2* hX = nullptr;      // pinned，Kmax·fftN
    double2* hAcc = nullptr;    // pinned，Kmax·fftN
    hipStream_t stream = nullptr;   // 一条非默认流：把多次隐式同步压成一次显式同步
    // device 侧状态（跨调用保留）
    double2* dH = nullptr;      // P·fftN  IR 分区谱，create 时上传一次
    double2* dRing = nullptr;   // P·fftN  输入谱环形缓冲
    double2* dAcc = nullptr;    // Kmax·fftN
    double2* dX = nullptr;      // Kmax·fftN
};

// 创建流式会话。成功返回非空句柄，失败返回 nullptr（调用方退回 CPU，不得当致命错误）。
// block：块长（非 2 的幂时按 1024），必须与 CPU StreamingConvolver 的 block 相同。
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
    // 批大小上限：只按"暂存 ≤ 32 MB"限，**不**按 P 限。
    //   读槽论证不要求 K ≤ P：p ≤ b 读本批 Xb，p > b 才读环形缓冲，而环形缓冲的写
    //   发生在 kernel 之后 ⟹ K 多大都不会读到脏数据；收尾只把"最近 P 块"写回（见 2e）。
    //   P=1（IR ≤ block）时这条很关键：否则每块一次往返，短 IR 比 CPU 慢 8 倍（实测 100 vs 11 ms）。
    s->Kmax = 32;
    while (s->Kmax > 1 && (size_t)s->Kmax * fftN * sizeof(double2) > (size_t)32 * 1024 * 1024) --s->Kmax;
    const size_t Kmax = (size_t)s->Kmax;

    s->buf.assign(fftN, cplx(0.0, 0.0));
    s->ola.assign((size_t)block, 0.0);
    s->oblk.assign((size_t)block, 0.0);
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

    const size_t stage = Kmax * fftN * sizeof(double2);
    bool ok = true;
    ok = ok && hipMalloc(&s->dH, P * fftN * sizeof(double2)) == hipSuccess;
    ok = ok && hipMalloc(&s->dRing, P * fftN * sizeof(double2)) == hipSuccess;
    ok = ok && hipMalloc(&s->dAcc, stage) == hipSuccess;
    ok = ok && hipMalloc(&s->dX, stage) == hipSuccess;
    // pinned 主机暂存（见 struct 处的注释）
    ok = ok && hipHostMalloc((void**)&s->hX, stage, 0) == hipSuccess;
    ok = ok && hipHostMalloc((void**)&s->hAcc, stage, 0) == hipSuccess;
    ok = ok && hipStreamCreate(&s->stream) == hipSuccess;
    ok = ok && hipMemcpy(s->dH, hH.data(), P * fftN * sizeof(double2), hipMemcpyHostToDevice) == hipSuccess;
    ok = ok && hipMemset(s->dRing, 0, P * fftN * sizeof(double2)) == hipSuccess;
    if (!ok) { rta_gpu_convolve_destroy(s); return nullptr; }   // destroy 对 nullptr 成员安全
    return (void*)s;
}

// 流式处理 n 个样本：in/out 均为 n 个样本（n 可任意切分，跨调用累计）。
// 返回 0 成功；非 0 = 失败（调用方应退回 CPU）。
// 延迟语义：out 相对 in 延迟 B 个样本（= CPU StreamingConvolver::latency()）。
RTA_EXPORT int rta_gpu_convolve_stream(void* h, const double* in, long long n, double* out) {
    GpuConvSession* s = (GpuConvSession*)h;
    if (!s || !out || n < 0) return 1;
    if (n == 0) return 0;
    if (!in) return 1;

    const size_t B = (size_t)s->B, fftN = (size_t)s->fftN, P = (size_t)s->P;
    const size_t N = (size_t)n;

    // ---- 1) 输入进 FIFO（与 CPU StreamingConvolver 同）----
    s->inFifo.insert(s->inFifo.end(), in, in + N);

    // ---- 2) 攒够就做，一批 K 块（一次上传 + 一个 kernel + 一次回读）----
    const size_t avail = s->inFifo.size();
    size_t done = 0;
    while (avail - done >= B) {
        size_t K = (avail - done) / B;
        if (K > (size_t)s->Kmax) K = (size_t)s->Kmax;

        // 2a) K 个块谱（host double FFT，与 CPU 完全一致）
        for (size_t j = 0; j < K; ++j) {
            const double* src = s->inFifo.data() + done + j * B;
            std::fill(s->buf.begin(), s->buf.end(), cplx(0.0, 0.0));
            for (size_t i = 0; i < B; ++i) s->buf[i] = cplx(src[i], 0.0);
            fft(s->buf);
            for (size_t k = 0; k < fftN; ++k)
                s->hX[j * fftN + k] = make_double2(s->buf[k].real(), s->buf[k].imag());
        }
        // 2b) 一次上传
        if (hipMemcpyAsync(s->dX, s->hX, K * fftN * sizeof(double2), hipMemcpyHostToDevice, s->stream) != hipSuccess) return 2;
        // 2c) 一批 K 块的频域乘累加（GPU）
        {
            dim3 blk(256), grid((unsigned)((fftN + 255) / 256), (unsigned)K);
            hipLaunchKernelGGL(mac_stream_batch_kernel, grid, blk, 0, s->stream, s->dH, s->dRing, s->dX, s->dAcc, s->P, s->head, (int)fftN, (int)K);
            if (hipGetLastError() != hipSuccess) return 3;   // 启动失败（执行期错误由下面的流同步暴露）
        }
        // 2d) 一次回读
        if (hipMemcpyAsync(s->hAcc, s->dAcc, K * fftN * sizeof(double2), hipMemcpyDeviceToHost, s->stream) != hipSuccess) return 4;
        // 2e) 把这 K 个块谱写进环形缓冲（设备内，不绕 host）；head 与 CPU 同语义地前进
        const size_t j0 = (K > P) ? (K - P) : 0;   // 只有"最近 P 块"需要留在环形缓冲里
        for (size_t j = j0; j < K; ++j) {
            const size_t slot = (size_t)((s->head + (int)j) % s->P);
            if (hipMemcpyAsync(s->dRing + slot * fftN, s->dX + j * fftN, fftN * sizeof(double2),
                                 hipMemcpyDeviceToDevice, s->stream) != hipSuccess) return 2;
        }
        s->head = (int)((s->head + (int)K) % (int)P);
        // 2f) 全批一次同步（上传 / 回读 / 环形写都在同一条流上，顺序由流保证）
        if (hipStreamSynchronize(s->stream) != hipSuccess) return 5;
        // 2g) IFFT + OLA 尾巴 + 入输出 FIFO
        for (size_t j = 0; j < K; ++j) {
            for (size_t k = 0; k < fftN; ++k)
                s->buf[k] = cplx(s->hAcc[j * fftN + k].x, s->hAcc[j * fftN + k].y);
            fft(s->buf, /*inverse=*/true);
            for (size_t i = 0; i < B; ++i) {
                s->oblk[i] = s->buf[i].real() + s->ola[i];
                s->ola[i] = s->buf[B + i].real();
            }
            s->outFifo.insert(s->outFifo.end(), s->oblk.begin(), s->oblk.end());
        }
        done += K * B;
    }
    if (done) s->inFifo.erase(s->inFifo.begin(), s->inFifo.begin() + (std::ptrdiff_t)done);

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
    if (s->stream) hipStreamDestroy(s->stream);
    if (s->hX) hipFree(s->hX);        // pinned 主机内存也用 hipFree 释放
    if (s->hAcc) hipFree(s->hAcc);
    delete s;
}
