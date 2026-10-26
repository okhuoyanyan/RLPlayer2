// gpu_precision.cpp — A 包：GPU 卷积 / 升频精度实测（纯离线，不打开任何音频设备）
//
// 目的（2026-10-26 外部评审会话 A 包）：
//   1. 标定 GPU 卷积（FP32 FFT 路径）相对 CPU 双精度卷积的【真实】误差量级，
//      取代 tests/test_gpu_bridge.cpp 里过松的 1e-4 判据；
//   2. 顺带实测 GPU 升频（FP64 路径）与 CPU 的逐样本差异，验证"≤1e-12"口径；
//   3. 产出「GPU 加速开关建议矩阵」的数据依据（精度 × 加速比）。
//
// 铁律 5：本探针的两道门都过了才采集数据——
//   门 1：rta_gpu_available()（DLL 内建：probe_kernel 写常数 12345 → 读回比对，
//         只认 hipGetLastError + 读回值，不认 hipDeviceSynchronize 的返回码）；
//   门 2：恒等卷积（δ IR）端到端门 —— 通路活着才继续（判据 ≤1e-5，FP32 截断
//         决定了它本来就不可能逐位）。
// 判据全部离线：相对 RMS 误差、最大偏差、误差谱带内占比、墙钟耗时。
#include "dsp/gpu_bridge.h"
#include "dsp/convolution.h"
#include "dsp/resampler.h"
#include "dsp/fft.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <chrono>
#include <string>
#include <algorithm>

using namespace rta::dsp;

// 确定性 xorshift（与调制器同款），保证结果可复现
static uint64_t s_ = 0x123456789ABCDEFull;
static double rnd() {
    s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
    return (double)((s_ * 2685821657736338717ull) >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 音乐样输入：若干正弦 + 粉噪（1/f），峰值压到 ~0.7
static std::vector<double> music_like(size_t n) {
    std::vector<double> x(n);
    double pk = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double v = 0.32 * std::sin(2 * 3.14159265358979 * 997.0 * i / 48000.0)
                 + 0.21 * std::sin(2 * 3.14159265358979 * 1234.0 * i / 48000.0)
                 + 0.13 * std::sin(2 * 3.14159265358979 * 3141.0 * i / 48000.0)
                 + 0.25 * rnd() * (0.02 + 0.98 / (1.0 + (double)(i % 4096) / 64.0));
        x[i] = v;
        pk = std::max(pk, std::fabs(v));
    }
    for (auto& v : x) v *= 0.7 / pk;
    return x;
}

// 房间校正样 IR：指数衰减 + 相邻平滑的噪声（混响尾巴形态）
static std::vector<double> room_like_ir(size_t n) {
    std::vector<double> h(n);
    double prev = 0.0, pk = 0.0;
    for (size_t i = 0; i < n; ++i) {
        prev = 0.72 * prev + 0.28 * rnd();
        h[i] = prev * std::exp(-6.5 * (double)i / (double)n);
        pk = std::max(pk, std::fabs(h[i]));
    }
    for (auto& v : h) v /= pk;
    return h;
}

struct ConvResult {
    double relRmsDb = 0.0, maxRel = 0.0, inBandFrac = 0.0;
    double cpuMs = 0.0, gpuMs = 0.0;
    long long nDiff = 0;
};

static ConvResult conv_case(size_t B, size_t irLen, const std::vector<double>& x) {
    const size_t n = x.size();
    const std::vector<double> ir = room_like_ir(irLen);

    // CPU 生产路径参考：StreamingConvolver（双精度 UPOL，输出带 B 帧算法延迟）
    StreamingConvolver cpu(ir, B);
    std::vector<double> yCpu(n, 0.0);
    double t0 = now_ms();
    cpu.process(x.data(), yCpu.data(), n);
    double t1 = now_ms();

    // GPU 路径：整块 API（GpuBridge → rta_gpu_convolve，FP32 FFT 中段）
    std::vector<double> yGpu(n, 0.0);
    auto& gb = GpuBridge::instance();
    double t2 = now_ms();
    const int rc = gb.convolve(x.data(), n, ir.data(), ir.size(), yGpu.data(), (int)B);
    double t3 = now_ms();

    ConvResult r;
    r.cpuMs = t1 - t0;
    r.gpuMs = t3 - t2;
    if (rc != 0) { r.relRmsDb = 999.0; r.maxRel = 999.0; r.inBandFrac = -1; return r; }

    // 对齐：yCpu = [B 个 0] ++ trueConv，yGpu = trueConv ⟹ 比较 yGpu[k] vs yCpu[B+k]
    double se = 0.0, sy = 0.0, mx = 0.0, yPk = 0.0;
    const size_t m = n - B - 1;
    std::vector<double> err(m);
    for (size_t k = 0; k < m; ++k) {
        const double g = yGpu[k], c = yCpu[B + k];
        const double d = g - c;
        err[k] = d;
        se += d * d; sy += c * c;
        mx = std::max(mx, std::fabs(d));
        yPk = std::max(yPk, std::fabs(c));
        if (d != 0.0) ++r.nDiff;
    }
    r.relRmsDb = 10.0 * std::log10((se / m) / (sy / m + 1e-300) + 1e-300);
    r.maxRel = mx / (yPk + 1e-300);

    // 误差谱：带内（≤20 kHz @48k）能量占比 —— 决定它落在音乐频段还是高频
    // fft 是 radix-2：先补零到 2 的幂，否则越界（第一版探针就崩在这里）
    size_t fn = 1;
    while (fn < err.size()) fn <<= 1;
    std::vector<cplx> f(fn, cplx(0.0, 0.0));
    for (size_t i = 0; i < err.size(); ++i) f[i] = cplx(err[i], 0.0);
    fft(f);
    double inB = 0.0, tot = 0.0;
    const size_t kTop = (size_t)(20000.0 / 24000.0 * fn / 2);
    for (size_t k = 1; k < fn / 2; ++k) {
        const double p = std::norm(f[k]);
        tot += p;
        if (k <= kTop) inB += p;
    }
    r.inBandFrac = tot > 0.0 ? inB / tot : 0.0;
    return r;
}

int main() { setvbuf(stdout, nullptr, _IONBF, 0);
    printf("==================================================================\n");
    printf("  A 包：GPU 精度实测（卷积 FP32 路径 / 升频 FP64 路径）\n");
    printf("==================================================================\n\n");

    // ---- 门 1+2（铁律 5）----
    auto& gb = GpuBridge::instance();
    fprintf(stderr, "[门1] GpuBridge: available=%d status=\"%s\"\n", gb.available() ? 1 : 0, gb.status().c_str());
    if (!gb.available()) {
        printf("❌ GPU 后端不可用 —— 按铁律 5 停止，不采集数据（见 docs/WIP-A.md）。\n");
        return 2;
    }
    {
        const std::vector<double> x = music_like(4096);
        const std::vector<double> delta = { 1.0 };
        std::vector<double> y(4096, -7.0);
        const int rc = gb.convolve(x.data(), x.size(), delta.data(), 1, y.data(), 1024);
        double mx = 0.0;
        for (size_t i = 0; i < x.size(); ++i) mx = std::max(mx, std::fabs(y[i] - x[i]));
        printf("[门2] 恒等卷积（δ IR）: rc=%d max|y-x|=%.3e（判据 ≤1e-5）\n", rc, mx);
        if (rc != 0 || mx > 1e-5) {
            printf("❌ 恒等卷积门未过 —— 通路异常，停止采集。\n");
            return 3;
        }
    }

    // ---- 1. 卷积精度矩阵 ----
    const std::vector<double> x = music_like(262144);
    printf("\n[卷积矩阵] 输入 %zu 样（fs=48k，音乐样），参考=CPU 双精度 StreamingConvolver\n", x.size());
    printf("%6s %8s | %10s %10s %8s %10s | %9s %9s\n",
           "B", "IR", "相对RMS dB", "最大偏差", "带内占比", "差异样数", "CPU ms", "GPU ms");
    for (size_t B : { (size_t)512, (size_t)1024, (size_t)2048, (size_t)4096 }) {
        for (size_t irLen : { (size_t)1024, (size_t)16384, (size_t)65536, (size_t)262144 }) {
            const ConvResult r = conv_case(B, irLen, x);
            printf("%6zu %8zu | %10.1f %10.2e %8.3f %10lld | %9.0f %9.0f\n",
                   B, irLen, r.relRmsDb, r.maxRel, r.inBandFrac, r.nDiff, r.cpuMs, r.gpuMs);
        }
    }

    // ---- 1b. 生产粒度耗时：每次 4096 样本一块（rtaweb 生产者的实际 chunk）----
    // DLL 每次调用都 hipMalloc/hipFree —— 小块高频调用下 GPU 可能是负优化。
    printf("\n[生产粒度耗时] 每块 4096 样（生产者 kChunk），长 IR，串 16 块：\n");
    for (size_t irLen : { (size_t)65536, (size_t)262144 }) {
        const std::vector<double> ir0 = room_like_ir(irLen);
        StreamingConvolver cpuP(ir0, 1024);
        std::vector<double> chunk(4096), yP(4096);
        auto& gbP = GpuBridge::instance();
        double tc = 0.0, tg = 0.0;
        const double tA = now_ms();
        for (int rep = 0; rep < 16; ++rep) {
            double t0 = now_ms();
            cpuP.process(chunk.data(), yP.data(), 4096);
            tc += now_ms() - t0;
            t0 = now_ms();
            gbP.convolve(chunk.data(), 4096, ir0.data(), ir0.size(), yP.data(), 1024);
            tg += now_ms() - t0;
        }
        const double wall = now_ms() - tA;
        printf("  IR=%6zu: CPU 合计 %6.1f ms | GPU 合计 %6.1f ms | 16 块墙钟 %.1f ms\n",
               irLen, tc, tg, wall);
    }

    // ---- 1c. 生产粒度耗时（D1 复测）：IR=65536、块 4096，串 64 块 ----
    // 64 块让缓冲复用后的缓存/稳态效果稳定（前 16 块行保留作 A 包对照）。
    // 跑到这里时上面矩阵已把最大 (P·fftN, Nb·fftN) 缓冲养到稳态容量，本节不再触发重分配。
    printf("\n[生产粒度耗时·D1] 每块 4096 样，IR=65536，串 64 块（稳态）：\n");
    {
        const std::vector<double> ir0 = room_like_ir(65536);
        StreamingConvolver cpuP(ir0, 1024);
        std::vector<double> chunk(4096), yP(4096);
        auto& gbP = GpuBridge::instance();
        double tc = 0.0, tg = 0.0;
        const double tA = now_ms();
        for (int rep = 0; rep < 64; ++rep) {
            double t0 = now_ms();
            cpuP.process(chunk.data(), yP.data(), 4096);
            tc += now_ms() - t0;
            t0 = now_ms();
            gbP.convolve(chunk.data(), 4096, ir0.data(), ir0.size(), yP.data(), 1024);
            tg += now_ms() - t0;
        }
        const double wall = now_ms() - tA;
        printf("  IR=%6zu: CPU 合计 %6.1f ms | GPU 合计 %6.1f ms | 64 块墙钟 %.1f ms\n",
               (size_t)65536, tc, tg, wall);
    }

    // ---- 2. 升频 CPU vs GPU 逐样本差异（FP64 路径）----
    printf("\n[升频 A/B] 同一重采样器、同一相位表，GPU=on/off 各跑一遍（44.1k→352.8k 与 →384k）\n");
    for (uint32_t outR : { 352800u, 384000u }) {
        ResamplerConfig rc;
        rc.inRate = 44100; rc.outRate = outR; rc.channels = 2;
        rc.totalTaps = 262144; rc.attenDb = 180.0; rc.transition = 0.04;
        PolyphaseResampler rs(rc);
        const size_t nIn = 65536;
        std::vector<std::vector<double>> in(2, std::vector<double>(nIn));
        for (size_t i = 0; i < nIn; ++i) {
            in[0][i] = 0.4 * std::sin(2 * 3.14159265358979 * 997.0 * i / 44100.0);
            in[1][i] = 0.4 * std::sin(2 * 3.14159265358979 * 601.0 * i / 44100.0);
        }
        std::vector<const double*> ip = { in[0].data(), in[1].data() };
        const size_t cap = rs.maxOutFrames(nIn);
        std::vector<std::vector<double>> yC(2, std::vector<double>(cap)), yG(2, std::vector<double>(cap));
        std::vector<double*> oc = { yC[0].data(), yC[1].data() }, og = { yG[0].data(), yG[1].data() };

        rs.set_use_gpu(false);
        const size_t nC = rs.process(ip.data(), nIn, oc.data());
        rs.reset();
        rs.set_use_gpu(true);
        if (!rs.gpu_active()) { printf("  →%u Hz: GPU 升频不可用（本机无 DLL 或 kernel 拒跑）\n", outR); continue; }
        const size_t nG = rs.process(ip.data(), nIn, og.data());

        double mx = 0.0, se = 0.0, sy = 0.0;
        long long nd = 0;
        const size_t m = std::min(nC, nG);
        for (size_t k = 0; k < m; ++k) for (int c = 0; c < 2; ++c) {
            const double d = yG[c][k] - yC[c][k];
            if (d != 0.0) ++nd;
            mx = std::max(mx, std::fabs(d));
            se += d * d; sy += yC[c][k] * yC[c][k];
        }
        printf("  44.1k→%u Hz: 产出 %zu/%zu 帧 | 差异样数 %lld/%zu | max|Δ|=%.3e | 相对RMS %.1f dB\n",
               outR, nG, nC, nd, m * 2, mx,
               10.0 * std::log10((se / (double)(m * 2)) / (sy / (double)(m * 2) + 1e-300) + 1e-300));
    }

    printf("\n完成。判据解读与阈值建议见 docs/WIP-A.md。\n");
    return 0;
}
