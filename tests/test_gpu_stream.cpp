// tests/test_gpu_stream.cpp —— GPU 卷积流式正确性门禁（261026-07）
//
// 背景：261026-07 报告指控「GPU 卷积逐块调用无跨调用状态 ⟹ IR 拖尾丢失」。
// 主线用自写探针独立复测（tools/gpu_stream_verify.cpp），结论：指控成立且更严重 ——
// 误差 +1.7~+3.4 dB（超过信号能量），且 IR=1024 就已经错，不是只有长 IR 出问题。
// 因此生产管线已停用 GPU 卷积（pipeline.h 的 kGpuConvEnabled=false）。
//
// 本测试的两条职责：
//   1) 【必须通过】CPU 流式卷积与整块卷积逐位一致 —— 这是管线现在唯一在用的路径，
//      任何分块边界回归都会在这里红。
//   2) 【只诊断不判红】若 rtagpu.dll 可用，输出 GPU 流式与 CPU 流式的误差量级。
//      在 DLL 修好（增加带跨调用状态的流式入口）之前，这个数字预期是 +2 dB 量级；
//      修好之后它会掉到 −120 dB 以下，届时把下面 kGpuStreamIsFixed 置 true 即可升级为硬门禁。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "dsp/convolution.h"
#include "dsp/gpu_bridge.h"

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : "  ", detail.c_str());
}
std::vector<double> make_signal(size_t n, unsigned seed) {
    std::vector<double> v(n);
    unsigned s = seed;
    for (size_t i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        const double noise = ((double)((s >> 16) & 0x7FFF) / 32768.0 - 0.5) * 0.25;
        v[i] = 0.6 * std::sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 44100.0) + noise;
    }
    return v;
}
std::vector<double> make_ir(size_t n) {
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
struct Diff { double relDb = 0.0, maxAbs = 0.0; };
Diff diffOf(const std::vector<double>& a, const std::vector<double>& b, size_t skip) {
    Diff d;
    double se = 0.0, sr = 0.0;
    for (size_t i = skip; i < a.size() && i < b.size(); ++i) {
        const double x = a[i] - b[i];
        se += x * x; sr += a[i] * a[i];
        if (std::fabs(x) > d.maxAbs) d.maxAbs = std::fabs(x);
    }
    d.relDb = (sr > 0.0) ? 10.0 * std::log10((se + 1e-300) / sr) : 0.0;
    return d;
}
}  // namespace

int main() {
    std::printf("=== GPU 卷积流式正确性（261026-07）===\n");
    const size_t N = 262144, CHUNK = 4096, BLOCK = 1024;
    const auto sig = make_signal(N, 12345u);
    // DLL 修好后把它置 true：GPU 流式必须与 CPU 流式等价
    constexpr bool kGpuStreamIsFixed = false;

    auto& gb = rta::dsp::GpuBridge::instance();
    const bool gpuAvail = gb.available();
    std::printf("  rtagpu.dll: %s%s\n", gpuAvail ? "可用" : "不可用（无卡/无 DLL，GPU 部分 SKIP）",
                gpuAvail ? (" — " + gb.status()).c_str() : "");

    for (size_t irLen : { size_t(1024), size_t(8192), size_t(65536) }) {
        const auto ir = make_ir(irLen);
        rta::dsp::StreamingConvolver whole(ir, BLOCK);
        std::vector<double> outWhole(N, 0.0);
        whole.process(sig.data(), outWhole.data(), N);

        rta::dsp::StreamingConvolver cpu(ir, BLOCK);
        std::vector<double> outCpu(N, 0.0);
        for (size_t p = 0; p < N; p += CHUNK) {
            const size_t n = (p + CHUNK <= N) ? CHUNK : (N - p);
            cpu.process(sig.data() + p, outCpu.data() + p, n);
        }
        std::printf("\nIR=%zu:\n", irLen);
        const Diff dCpu = diffOf(outWhole, outCpu, BLOCK * 4);
        check(dCpu.maxAbs == 0.0, "CPU 流式与整块逐位一致",
              "max|d|=" + std::to_string(dCpu.maxAbs) + " relRMS=" + std::to_string(dCpu.relDb) + " dB");

        if (!gpuAvail) continue;
        std::vector<double> outGpu(N, 0.0);
        bool ok = true;
        for (size_t p = 0; p < N && ok; p += CHUNK) {
            const size_t n = (p + CHUNK <= N) ? CHUNK : (N - p);
            std::vector<double> tmp(n, 0.0);
            if (gb.convolve(sig.data() + p, n, ir.data(), ir.size(), tmp.data(), (int)BLOCK) != 0) { ok = false; break; }
            for (size_t i = 0; i < n; ++i) outGpu[p + i] = tmp[i];
        }
        if (!ok) { std::printf("  [SKIP] GPU 调用失败\n"); continue; }
        const Diff dGpu = diffOf(outCpu, outGpu, BLOCK * 4);
        if (kGpuStreamIsFixed) {
            check(dGpu.relDb <= -120.0 && dGpu.maxAbs <= 1e-6, "GPU 流式与 CPU 流式等价（修好后应满足）",
                  "relRMS=" + std::to_string(dGpu.relDb) + " dB max|d|=" + std::to_string(dGpu.maxAbs));
        } else {
            // 已知缺陷：只诊断，不判红。管线已停用 GPU 卷积，所以不影响生产正确性。
            std::printf("  [KNOWN] GPU 流式误差 relRMS=%.1f dB max|d|=%.3e（DLL 无跨调用状态，管线已停用该路径）\n",
                        dGpu.relDb, dGpu.maxAbs);
        }
    }

    std::printf("\n%s (失败项 %d)\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
