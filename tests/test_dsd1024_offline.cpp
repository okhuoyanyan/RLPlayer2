// DSD1024 离线验收（2026-11-01）：没有真机，只能证明"数字侧成立"。
//
// 覆盖（每条都对应一个会骗人的判据，见 AGENTS 铁律 3/7）：
//   1) 45.1584 / 49.152 MHz 请求 7 阶 ⟹ 真的运行 7 阶（不是偷偷退 5 阶），输出恰 2 电平；
//   2) 输出每一位都严格是 ±1（不是"取了符号的多电平"冒充 1-bit）；
//   3) 满幅瞬态（0 dBFS 20 ms）之后回到静音：限幅/过载计数**不再增长**，输出均值回到 0；
//   4) DoP 打包：标记字节 0x05/0xFA 逐帧交替，16 位载荷与调制器输出逐位一致；
//   5) 吞吐：只报告并做极宽松下界（>0.3× 实时），不做"能实时"的承诺。
//
// 明确不覆盖（如实登记）：真实 DAC/驱动输出、模拟指标、听感；WASAPI/ASIO 设备交互。
#include "dsp/dop.h"
#include "dsp/modulator.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace rta::dsp;

static int checks = 0, fails = 0;
static void ck(bool c, const char* name) {
    ++checks;
    if (!c) { ++fails; std::printf("FAIL %s\n", name); }
}

static SdmConfig cfg_of(double fs) {
    SdmConfig c;
    c.fs = fs;
    c.order = 7;
    c.family = ModulatorFamily::Dsd;
    c.cutoffHz = 100000;
    c.ditherLsb = 0.0005;
    c.oneBitOutput = true;
    c.bypassOrderGuard = false;
    return c;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("DSD1024 offline: no audio device is opened; numbers are digital-domain only.\n");
    for (double fs : {45158400.0, 49152000.0}) {
        const char* tag = (fs > 46000000.0) ? "48k-family" : "44.1k-family";
        // --- 1) 守卫与实际阶数 ---
        SdmModulator m(cfg_of(fs));
        ck(m.effectiveOrder() == 7, "requested order 7 runs as order 7 at >=40 MHz");
        ck(!m.steppedDown(), "order guard did not step down");
        ck(m.levels() == 2, "output is 1-bit (2 levels)");
        ck(m.uses_sections(), "7th order uses the factor-preserving sections path");

        // --- 2) 输出严格是 ±1，同时测吞吐 ---
        const size_t n = (size_t)(fs * 0.10);           // 0.1 s 音频
        std::vector<double> bits;
        bits.reserve(n);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < n; ++i)
            bits.push_back(m.process(0.62 * std::sin(2.0 * kPi * 1000.0 * (double)i / fs)));
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t nonBinary = 0;
        for (double v : bits) if (v != 1.0 && v != -1.0) ++nonBinary;
        ck(nonBinary == 0, "every output sample is exactly +/-1 (no fake multilevel)");
        ck(m.nonFinite() == 0, "no NaN/Inf on the steady-state run");
        const double realtime = (0.10 > 0) ? (0.10 / sec) : 0.0;
        std::printf("THROUGHPUT fs=%.0f (%s) samples=%zu wall=%.3fs => %.2fx realtime (modulator only)\n",
                    fs, tag, n, sec, realtime);
        ck(realtime > 0.30, "modulator throughput above a very loose 0.3x floor");

        // --- 3) 满幅瞬态 + 恢复（铁律 7） ---
        SdmModulator t(cfg_of(fs));
        const size_t warm = (size_t)(fs * 0.05), burst = (size_t)(fs * 0.020), quiet = (size_t)(fs * 0.20);
        for (size_t i = 0; i < warm; ++i) t.process(0.62 * std::sin(2.0 * kPi * 1000.0 * (double)i / fs));
        for (size_t i = 0; i < burst; ++i)
            t.process(std::sin(2.0 * kPi * 1000.0 * (double)(warm + i) / fs));   // 0 dBFS
        const long long ovAfterBurst = t.overload();
        for (size_t i = 0; i < quiet / 2; ++i) t.process(0.0);
        const long long ovMid = t.overload();
        double mean = 0.0;
        const size_t tail = std::min<size_t>(4096, quiet / 2);
        for (size_t i = 0; i < quiet / 2; ++i) {
            const double v = t.process(0.0);
            mean += v;
        }
        const long long ovEnd = t.overload();
        mean /= (double)(quiet / 2);
        std::printf("RECOVERY fs=%.0f overload burst=%lld mid=%lld end=%lld nonFinite=%lld mean_tail=%.4f\n",
                    fs, ovAfterBurst, ovMid, ovEnd, t.nonFinite(), mean);
        ck(ovEnd == ovMid, "overload count stops growing after the transient");
        ck(t.nonFinite() == 0, "no NaN/Inf through the full-scale transient");
        ck(std::fabs(mean) < 0.05, "silence after the transient averages to zero (+/-1 stream)");

        // --- 4) DoP 打包：标记 + 载荷逐位一致 ---
        {
            const size_t frames = 16 * 4;                  // 4 个 DoP 帧（每帧 16 位）
            std::vector<double> b(frames);
            SdmModulator d(cfg_of(fs));
            for (size_t i = 0; i < 2048; ++i) d.process(0.5 * std::sin(2.0 * kPi * 1000.0 * (double)i / fs));  // 预热
            for (size_t i = 0; i < frames; ++i)
                b[i] = d.process(0.5 * std::sin(2.0 * kPi * 1000.0 * (double)(2048 + i) / fs));
            const double* ptr[1] = { b.data() };
            std::vector<int32_t> out;
            DopPackState st;
            const bool ok = dop_pack_checked(ptr, 1, frames, out, st);
            ck(ok, "dop_pack_checked accepts a 1-bit stream");
            ck(out.size() == frames / 16, "DoP emits one 24-bit word per 16 DSD bits");
            size_t badMarker = 0, badPayload = 0;
            for (size_t w = 0; w < out.size(); ++w) {
                const uint32_t word = (uint32_t)out[w];
                const uint32_t marker = (word >> 16) & 0xFFu;
                const uint32_t want = (w % 2 == 0) ? DOP_MARKER_A : DOP_MARKER_B;
                if (marker != want) ++badMarker;
                // DoP 载荷是 **MSB 在前**：第 b 个 DSD 位写进 bit(15-b)（见 dop.h 的 bits16 构造）
                for (size_t k = 0; k < 16; ++k)
                    if (((word >> (15 - k)) & 1u) != (b[w * 16 + k] > 0.0 ? 1u : 0u)) ++badPayload;
            }
            ck(badMarker == 0, "DoP markers alternate 0x05/0xFA exactly");
            ck(badPayload == 0, "DoP payload equals the modulator output bit-for-bit");
        }
    }
    std::printf("dsd1024_offline: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
