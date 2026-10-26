// gpu_stream_check.cpp — 261026-08「GPU 卷积流式化」的验收探针（纯离线，不打开任何音频设备）
//
// 背景：261026-07 指控「rta_gpu_convolve 逐块调用无跨调用状态 ⟹ 每 4096 样一个边界断裂」，
// 主线用 tools/gpu_stream_verify.cpp 实测确认（误差 +1.7~+3.4 dB，大于信号本身）。
// 修法：DLL 新增带跨调用状态的流式会话（rta_gpu_convolve_create / _stream / _destroy），
// 语义与 src/dsp/convolution.h 的 StreamingConvolver 逐位等价（latency() == block）。
//
// 构建（不依赖 CMake，因为本探针不进 ctest；需要 MSVC + /utf-8）：
//   cmd /c ""vcvars64.bat" && cl /nologo /utf-8 /std:c++17 /O2 /EHsc /Isrc //       tools\gpu_stream_check.cpp /Fe:build-gpu\gpu_stream_check.exe /Fo:build-gpu\"
// 运行：把新 rtagpu.dll 放在 exe 同目录（LoadLibraryA 先搜 exe 目录），然后直接跑 exe。
//   本例：hipcc --offload-arch=gfx1201 -O3 -shared -Isrc src\gpu\gpu_conv_hip.cpp -o build-gpu\rtagpu.dll
//
// 本探针测的就是修好的东西（铁律 3：判据要真的能证明目标）：
//   [门] 平凡 kernel 门（DLL 内建写常数读回）+ 「常量增益读回」门
//        —— ir[0]=2、输入全 1 ⟹ 流式输出必须处处等于 2（证明 MAC kernel 真的执行且真的做了乘累加，
//           而不是"没跑但输出恰好是 0"这类自欺）
//   [1] 三档 IR（1024/8192/65536）流式对比：GPU 流（每块 4096，生产形态）vs CPU 流
//       判据：相对 RMS ≤ −120 dB 且 max|Δ| ≤ 1e-6
//   [2] 同一 GPU 会话在**任意切分**（1000/37/4096 混合）下与 CPU 相同切分逐位一致
//       证明状态真的跨调用（不是"每块重新起步"）
//   [3] reset(换 IR) 后仍与全新 CPU 卷积器一致
//   [4] 整块接口回归（A 包口径）：gb.convolve 整块 vs CPU 双精度 StreamingConvolver（位移 B 对齐）
//   [5] 生产粒度耗时（262144 样 / 64×4096 块）—— 重新开 GPU 前得知道它是不是负优化
#include "dsp/convolution.h"
#include "dsp/gpu_bridge.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace rta::dsp;

static int g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : "  ", detail.c_str());
}

// 确定性 LCG（与主线探针同一套种子/形状，便于和 261026-07 的数字对照）
static std::vector<double> make_signal(size_t n, unsigned seed) {
    std::vector<double> v(n);
    unsigned s = seed;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        const double noise = ((double)((s >> 16) & 0x7FFF) / 32768.0 - 0.5) * 0.25;
        v[i] = 0.6 * std::sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 44100.0) + noise;
    }
    return v;
}
static std::vector<double> make_ir(size_t n) {
    std::vector<double> v(n, 0.0);
    unsigned s = 987654321u;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        const double env = std::exp(-(double)i / (double)(n / 6 + 1));
        v[i] = (((double)((s >> 16) & 0x7FFF) / 32768.0) - 0.5) * 0.5 * env;
    }
    v[0] = 1.0;
    return v;
}

struct Diff { double relDb = 0.0, maxAbs = 0.0; long long nDiff = 0; };
static Diff diffOf(const std::vector<double>& a, const std::vector<double>& b, size_t skip) {
    Diff d; double se = 0.0, sr = 0.0;
    for (size_t i = skip; i < a.size() && i < b.size(); ++i) {
        const double x = a[i] - b[i];
        se += x * x; sr += a[i] * a[i];
        if (std::fabs(x) > d.maxAbs) d.maxAbs = std::fabs(x);
        if (x != 0.0) ++d.nDiff;
    }
    d.relDb = (sr > 0.0) ? 10.0 * std::log10((se + 1e-300) / sr) : 0.0;
    return d;
}

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 用给定的切分方案跑一遍（GPU 流 或 CPU 流）
// expectGpu=true 时：一旦会话降级到 CPU 就返回 false（GPU 判据必须报失败，不能自动混过去）
static bool run_chunks(GpuStreamConvolver* g, StreamingConvolver* c,
                       const std::vector<double>& sig, const std::vector<size_t>& chunks,
                       std::vector<double>& out, bool expectGpu = true) {
    out.assign(sig.size(), 0.0);
    size_t p = 0;
    for (size_t k = 0; p < sig.size(); ++k) {
        size_t n = chunks[k % chunks.size()];
        if (p + n > sig.size()) n = sig.size() - p;
        if (n == 0) break;
        if (g) { g->process(sig.data() + p, out.data() + p, n); if (expectGpu && !g->on_gpu()) return false; }
        else   { c->process(sig.data() + p, out.data() + p, n); }
        p += n;
    }
    return true;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const size_t N = 262144, CHUNK = 4096, BLOCK = 1024;   // 生产形态：kChunk=4096、convBlock=1024
    const auto sig = make_signal(N, 12345u);
    auto& gb = GpuBridge::instance();

    std::printf("================ 261026-08 GPU 卷积流式化 验收 ================\n");
    std::printf("GPU: available=%d stream_supported=%d\n     status=%s\n",
                (int)gb.available(), (int)gb.stream_supported(), gb.status().c_str());
    if (!gb.available()) {
        std::printf("❌ 后端不可用（无 DLL / kernel 跑不起来）—— 后续判据无法执行。\n");
        return 2;
    }
    if (!gb.stream_supported()) {
        // ---- 兼容路径：老 DLL（没有流式三件套）----
        // 要求：加载层不崩、GpuStreamConvolver 自动退回 CPU、输出与纯 CPU 逐位相同。
        std::printf("\n[兼容] 本 DLL 没有流式符号 —— 验证自动退回 CPU（不崩、输出不变）\n");
        const auto ir = make_ir(8192);
        GpuStreamConvolver gc(ir, BLOCK);              // 应当退化成 CPU 流式
        check(!gc.on_gpu(), "老 DLL：GpuStreamConvolver 自动退回 CPU（on_gpu=false）");
        std::vector<double> outGpu;
        run_chunks(&gc, nullptr, sig, {CHUNK}, outGpu, /*expectGpu=*/false);
        StreamingConvolver cpu(ir, BLOCK);
        std::vector<double> outCpu;
        run_chunks(nullptr, &cpu, sig, {CHUNK}, outCpu);
        const Diff d = diffOf(outCpu, outGpu, BLOCK * 4);
        check(d.maxAbs == 0.0, "老 DLL：退回后的输出与纯 CPU 逐位一致",
              "max|d|=" + std::to_string(d.maxAbs));
        std::printf("\n%s（失败项 %d）\n", g_fail == 0 ? "兼容路径全部通过" : "存在失败", g_fail);
        return g_fail == 0 ? 0 : 1;
    }

    // ---- [门] 常量增益读回：ir[0]=2、输入全 1 ⟹ 输出处处为 2（证明 kernel 真跑且真乘了）----
    {
        std::printf("\n[门] 平凡/常量 kernel 验证\n");
        std::printf("  rta_gpu_available 内建写常数读回 = %d（probe_kernel 写 12345 读回比对）\n",
                    (int)gb.available());
        std::vector<double> ir1(1, 2.0), ones(8192, 1.0), y;
        GpuStreamConvolver gc(ir1, BLOCK);
        check(gc.on_gpu(), "流式会话建在 GPU 上（create 成功）");
        run_chunks(&gc, nullptr, ones, {CHUNK}, y);
        double mx = 0.0;
        for (size_t i = BLOCK * 4; i < y.size(); ++i) mx = std::max(mx, std::fabs(y[i] - 2.0));
        check(mx <= 1e-12, "ir[0]=2、输入全 1 ⟹ 输出 = 2（max|y−2| ≤ 1e-12）",
              "max|y-2|=" + std::to_string(mx));
    }

    // ---- [1] 三档 IR 流式对比（生产形态：每块 4096）----
    std::printf("\n[1] 流式对比：CPU 整块 = 参考；CPU 4096 分块；GPU 流 4096 分块（N=%zu）\n", N);
    std::printf("    %8s | %11s %10s | %11s %10s %8s\n",
                "IR", "CPU流 dB", "max|d|", "GPU流 dB", "max|d|", "结论");
    for (size_t irLen : { size_t(1024), size_t(8192), size_t(65536), size_t(262144) }) {
        const auto ir = make_ir(irLen);
        StreamingConvolver ref(ir, BLOCK);
        std::vector<double> outRef(N, 0.0);
        ref.process(sig.data(), outRef.data(), N);

        StreamingConvolver cpu(ir, BLOCK);
        std::vector<double> outCpu;
        run_chunks(nullptr, &cpu, sig, {CHUNK}, outCpu);
        const Diff dc = diffOf(outRef, outCpu, BLOCK * 4);

        GpuStreamConvolver gpu(ir, BLOCK);
        std::vector<double> outGpu;
        const bool onGpu = run_chunks(&gpu, nullptr, sig, {CHUNK}, outGpu) && gpu.on_gpu();
        const Diff dg = diffOf(outRef, outGpu, BLOCK * 4);

        const bool ok = onGpu && dg.relDb <= -120.0 && dg.maxAbs <= 1e-6;
        std::printf("    %8zu | %11.1f %10.3e | %11.1f %10.3e %8s\n",
                    irLen, dc.relDb, dc.maxAbs, dg.relDb, dg.maxAbs, ok ? "PASS" : "FAIL");
        if (irLen != 262144) {   // 判据只要求三档；262144 作额外信息
            check(ok, "IR=" + std::to_string(irLen) + " 流式 GPU ≡ 流式 CPU（≤ −120 dB 且 ≤ 1e-6）",
                  "relRMS=" + std::to_string(dg.relDb) + " dB max|d|=" + std::to_string(dg.maxAbs) +
                  " 差异样数=" + std::to_string(dg.nDiff));
        }
    }

    // ---- [2] 任意切分：GPU 会话必须与 CPU 同样切分逐位一致 ----
    {
        std::printf("\n[2] 任意切分（1000/37/4096 循环，证明状态真的跨调用）\n");
        const auto ir = make_ir(8192);
        const std::vector<size_t> chunks = {1000, 37, 4096};
        StreamingConvolver cpu(ir, BLOCK);
        std::vector<double> outCpu;
        run_chunks(nullptr, &cpu, sig, chunks, outCpu);
        GpuStreamConvolver gc(ir, BLOCK);
        std::vector<double> outGpu;
        const bool onGpu = run_chunks(&gc, nullptr, sig, chunks, outGpu) && gc.on_gpu();
        const Diff d = diffOf(outCpu, outGpu, BLOCK * 4);
        check(onGpu && d.relDb <= -120.0 && d.maxAbs <= 1e-6,
              "IR=8192 任意切分 GPU ≡ CPU", "relRMS=" + std::to_string(d.relDb) +
              " dB max|d|=" + std::to_string(d.maxAbs) + " 差异样数=" + std::to_string(d.nDiff));
    }

    // ---- [3] reset(换 IR) ----
    {
        std::printf("\n[3] reset（换 IR）后仍与全新 CPU 卷积器一致\n");
        const auto irA = make_ir(4096), irB = make_ir(16384);
        GpuStreamConvolver gc(irA, BLOCK);
        std::vector<double> t;
        run_chunks(&gc, nullptr, sig, {CHUNK}, t);          // 先跑一段旧 IR
        gc.reset(irB);
        std::vector<double> outGpu;
        const bool onGpu = run_chunks(&gc, nullptr, sig, {CHUNK}, outGpu) && gc.on_gpu();
        StreamingConvolver cpu(irB, BLOCK);
        std::vector<double> outCpu;
        run_chunks(nullptr, &cpu, sig, {CHUNK}, outCpu);
        const Diff d = diffOf(outCpu, outGpu, BLOCK * 4);
        check(onGpu && d.relDb <= -120.0 && d.maxAbs <= 1e-6, "reset 后 GPU ≡ CPU",
              "relRMS=" + std::to_string(d.relDb) + " dB max|d|=" + std::to_string(d.maxAbs));
    }

    // ---- [4] 整块接口回归（A 包口径：位移 B 对齐）----
    std::printf("\n[4] 整块接口回归（rta_gpu_convolve 未改动；A 包判据 −127.6~−147.8 dB）\n");
    for (size_t irLen : { size_t(1024), size_t(16384), size_t(65536), size_t(262144) }) {
        const auto ir = make_ir(irLen);
        StreamingConvolver cpu(ir, BLOCK);
        std::vector<double> yCpu(N, 0.0);
        cpu.process(sig.data(), yCpu.data(), N);
        std::vector<double> yGpu(N, 0.0);
        const int rc = gb.convolve(sig.data(), N, ir.data(), ir.size(), yGpu.data(), (int)BLOCK);
        double se = 0.0, sy = 0.0, mx = 0.0;
        const size_t m = N - BLOCK - 1;
        for (size_t k = 0; k < m; ++k) {
            const double d = yGpu[k] - yCpu[BLOCK + k];
            se += d * d; sy += yCpu[BLOCK + k] * yCpu[BLOCK + k];
            mx = std::max(mx, std::fabs(d));
        }
        const double db = 10.0 * std::log10((se / (double)m) / (sy / (double)m + 1e-300) + 1e-300);
        std::printf("    IR=%6zu: rc=%d 相对RMS %8.1f dB max|d|=%.3e\n", irLen, rc, db, mx);
        check(rc == 0 && db <= -120.0, "整块 API 回归 IR=" + std::to_string(irLen), "dB=" + std::to_string(db));
    }

    // ---- [5] 生产粒度耗时扫描（262144 样 / 64×4096 块 / block=1024）----
    // 重新打开 GPU 卷积前必须知道它在哪些 IR 长度上真的不是负优化。
    std::printf("\n[5] 生产粒度耗时扫描（262144 样 / 64×4096 块 / block=1024；4096 样 = 92.9 ms 音频）\n");
    std::printf("    %8s | %9s %9s %7s %7s\n", "IR", "CPU ms", "GPU ms", "GPU/CPU", "实时倍率");
    for (size_t irLen : { size_t(1024), size_t(8192), size_t(32768), size_t(65536), size_t(131072), size_t(262144) }) {
        const auto ir = make_ir(irLen);
        std::vector<double> a(4096), b(4096);
        for (size_t i = 0; i < 4096; ++i) { a[i] = sig[i]; b[i] = 0.0; }
        double tcpu = 0.0, tgpu = 0.0;
        {
            StreamingConvolver cpu(ir, BLOCK);
            cpu.process(a.data(), b.data(), 4096);
            const double t0 = now_ms();
            for (int k = 0; k < 64; ++k) cpu.process(a.data(), b.data(), 4096);
            tcpu = now_ms() - t0;
        }
        {
            GpuStreamConvolver gpu(ir, BLOCK);
            gpu.process(a.data(), b.data(), 4096);   // 预热（首次 kernel 加载/JIT）
            const double t0 = now_ms();
            for (int k = 0; k < 64; ++k) gpu.process(a.data(), b.data(), 4096);
            tgpu = now_ms() - t0;
            if (!gpu.on_gpu()) { std::printf("    %8zu | GPU 降级到 CPU（stream 失败）\n", irLen); continue; }
        }
        std::printf("    %8zu | %9.1f %9.1f %7.2f %7.1fx\n", irLen, tcpu, tgpu, tgpu / tcpu, 92.9 / tgpu);
    }

    std::printf("\n%s（失败项 %d）\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}