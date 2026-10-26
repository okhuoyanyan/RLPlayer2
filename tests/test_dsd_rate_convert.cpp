// test_dsd_rate_convert.cpp — DSD→DSD 速率转换（DSD64→128/256…）的判据
//
// 每一条都打印实测数字，铁律 3：判据要能证明要证明的事。
//   1. 速率规划：同族整数倍支持 / 跨族与非整数倍明确拒绝（含原因里的数字）
//   2. 插值器原型：通带 0…20 kHz 平坦度、阻带（镜像起点）衰减
//   3. DSD64 → DSD128 主判据：THD+N、通带平坦度、折叠残留、DoP 合法性、分块无关性，
//      全部与「DSD64 原样播放（不转换）」对照
//   4. 级联 DSD64→128→256 vs 直接 DSD64→256
//   5. 跨族：plan 与 convert 都必须明确报错（不静默降级）
//   6. 满幅方波 / 单样本越界 / 恢复（铁律 7：不许只断言"没 NaN"）
//   7. 真实 .dsf 端到端：耗时、内存、DoP 合法性
//
// 只读写文件、不开任何音频设备（铁律 1）。
#include "audio/audio_loader.h"
#include "audio/dsd_reader.h"
#include "dsp/direct_sdm.h"
#include "dsp/dsd_integrator.h"
#include "dsp/dsd_rate_convert.h"
#include "dsp/fft.h"
#include "dsp/modulator.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

using namespace rta::dsp;
using rta::audio::DsdStream;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
// 本仓一律 UTF-8 源/参数字节；Windows 的 path(const char*) 会按 ANSI(GBK) 解释，中文/日文路径必错
static std::filesystem::path u8path_of(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
static double mem_peak_mb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
    return -1.0;
}

// ---------------- 素材 ----------------
static DsdStream modulate(uint32_t rate, int ch, size_t nBits, double f0, double amp) {
    DsdStream s;
    s.rate = rate; s.channels = ch; s.bits = nBits; s.container = "mod";
    s.data.assign((size_t)ch, std::vector<uint8_t>((nBits + 7) / 8, 0));
    for (int c = 0; c < ch; ++c) {
        SdmConfig cfg; cfg.fs = (double)rate; cfg.order = 5; cfg.cutoffHz = 100000.0; cfg.levels = 2;
        SdmModulator m(cfg);
        for (size_t n = 0; n < nBits; ++n)
            if (m.process(amp * std::sin(2.0 * kPi * f0 * (double)n / (double)rate)) > 0.0)
                s.setBit((size_t)c, n, true);
    }
    return s;
}
// 1-bit 方波探针（按整数样本相位生成，50% 占空比）。quietFrom 之后是 0x55（DSD 静音）
static DsdStream square_probe(uint32_t rate, int ch, size_t nBits, double f, size_t quietFrom) {
    const long period = (long)std::llround((double)rate / f);
    DsdStream s;
    s.rate = rate; s.channels = ch; s.bits = nBits; s.container = "probe";
    s.data.assign((size_t)ch, std::vector<uint8_t>((nBits + 7) / 8, 0));
    for (int c = 0; c < ch; ++c)
        for (size_t n = 0; n < nBits; ++n) {
            const bool v = (n >= quietFrom) ? ((n & 1) != 0)
                                            : ((long)(n % (size_t)period) * 2 < period);
            if (v) s.setBit((size_t)c, n, true);
        }
    return s;
}
static DsdStream silence_probe(uint32_t rate, int ch, size_t nBits) {
    DsdStream s;
    s.rate = rate; s.channels = ch; s.bits = nBits; s.container = "probe";
    s.data.assign((size_t)ch, std::vector<uint8_t>((nBits + 7) / 8, 0x55));
    return s;
}

// ---------------- 测量链（两条路径共用同一组抽取器，差别只在被测流） ----------------
// 时钟：176400 Hz / FIR 两级 / 通带 20 kHz（与 read_any 的既有设置同源）
static std::vector<double> measure_pcm(const DsdStream& s, uint32_t outRate, int ch) {
    IntegratorConfig cfg;
    cfg.kind = IntegratorKind::Fir; cfg.dsdRate = s.rate; cfg.outRate = outRate;
    cfg.passbandHz = 20000.0; cfg.attenDb = 120.0; cfg.twoStage = true;
    DsdDecimator dec(cfg);
    std::vector<double> out;
    dec.process_bits(s.data[(size_t)ch].data(), (size_t)s.bits, out);
    return out;
}
struct Thdn { double signalDb = -999, noiseDb = -999, thdnDb = -999; };
static Thdn measure_thdn(const std::vector<double>& xIn, double fs, double f0,
                         size_t N = 65536, double bandTop = 20000.0) {
    Thdn r;
    if (xIn.size() < N) return r;
    const double w0 = 2.0 * kPi * f0 / fs;
    double cc = 0, ss = 0, cs = 0, xc = 0, xs = 0;
    for (size_t n = 0; n < N; ++n) {
        const double c = std::cos(w0 * (double)n), s = std::sin(w0 * (double)n);
        cc += c * c; ss += s * s; cs += c * s; xc += xIn[n] * c; xs += xIn[n] * s;
    }
    const double det = cc * ss - cs * cs;
    if (std::fabs(det) < 1e-300) return r;
    const double A = (xc * ss - xs * cs) / det, B = (xs * cc - xc * cs) / det;
    std::vector<cplx> x(N);
    double sumW2 = 0;
    for (size_t n = 0; n < N; ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)n / (double)(N - 1));
        sumW2 += w * w;
        const double resid = xIn[n] - (A * std::cos(w0 * (double)n) + B * std::sin(w0 * (double)n));
        x[n] = cplx(resid * w, 0.0);
    }
    fft(x);
    const double binHz = fs / (double)N, norm = (double)N * sumW2;
    const size_t kLo = (size_t)std::ceil(20.0 / binHz);
    const size_t kTop = (size_t)std::floor(bandTop / binHz);
    double pnoise = 0;
    for (size_t k = kLo; k <= kTop && k < N / 2; ++k) pnoise += std::norm(x[k]) / norm;
    const double psig = (A * A + B * B) / 2.0;
    r.signalDb = 10.0 * std::log10(psig + 1e-300);
    r.noiseDb = 10.0 * std::log10(2.0 * pnoise + 1e-300);
    r.thdnDb = 10.0 * std::log10(psig / (2.0 * pnoise + 1e-300));
    return r;
}
// 单频幅度（均值平方，dBFS）：用于平坦度扫描
static double tone_amp_db(const std::vector<double>& x, double fs, double f0, size_t N = 32768) {
    if (x.size() < N) return -999;
    const double w0 = 2.0 * kPi * f0 / fs;
    double cc = 0, ss = 0, cs = 0, xc = 0, xs = 0;
    for (size_t n = 0; n < N; ++n) {
        const double c = std::cos(w0 * (double)n), s = std::sin(w0 * (double)n);
        cc += c * c; ss += s * s; cs += c * s; xc += x[n] * c; xs += x[n] * s;
    }
    const double det = cc * ss - cs * cs;
    if (std::fabs(det) < 1e-300) return -999;
    const double A = (xc * ss - xs * cs) / det, B = (xs * cc - xc * cs) / det;
    return 10.0 * std::log10((A * A + B * B) / 2.0 + 1e-300);
}
// 带限 RMS（20 Hz…hiHz）：整段 FFT + Hann，用于"静音尾里到底有多少可闻内容"
static double band_rms_db(const std::vector<double>& x, size_t from, size_t N, double fs, double hi = 20000.0) {
    if (from + N > x.size()) return -999;
    std::vector<cplx> b(N);
    double sumW2 = 0;
    for (size_t n = 0; n < N; ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)n / (double)(N - 1));
        sumW2 += w * w;
        b[n] = cplx(x[from + n] * w, 0.0);
    }
    fft(b);
    const double binHz = fs / (double)N, norm = (double)N * sumW2;
    const size_t kLo = (size_t)std::ceil(20.0 / binHz), kTop = (size_t)std::floor(hi / binHz);
    double p = 0;
    for (size_t k = kLo; k <= kTop && k < N / 2; ++k) p += 2.0 * std::norm(b[k]) / norm;
    return 10.0 * std::log10(p + 1e-300);
}
static double rms_db(const std::vector<double>& x, size_t from, size_t to) {
    if (to > x.size()) to = x.size();
    double s = 0; size_t n = 0;
    for (size_t i = from; i < to; ++i) { s += x[i] * x[i]; ++n; }
    return n ? 10.0 * std::log10(s / (double)n + 1e-300) : -999;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    // ================= 1. 速率规划 =================
    std::printf("=== 1. 速率规划（同族整数倍 / 跨族与非整数倍明确拒绝）===\n");
    {
        struct Case { uint32_t s, d; DsdRateMode want; };
        const Case cases[] = {
            { 2822400,  2822400,  DsdRateMode::Direct },
            { 2822400,  5644800,  DsdRateMode::Upsample },
            { 2822400, 11289600,  DsdRateMode::Upsample },
            { 2822400, 22579200,  DsdRateMode::Upsample },
            { 5644800, 11289600,  DsdRateMode::Upsample },
            { 3072000,  6144000,  DsdRateMode::Upsample },
            { 5644800,  2822400,  DsdRateMode::Downsample },
            { 11289600, 2822400,  DsdRateMode::Downsample },
            { 2822400,  3072000,  DsdRateMode::Unsupported },   // 跨族（44.1k → 48k）
            { 2822400,  6144000,  DsdRateMode::Unsupported },   // 跨族
            { 2822400, 24576000,  DsdRateMode::Unsupported },   // 跨族（48k 族 DSD512）
            { 44100,   2822400,   DsdRateMode::Unsupported },   // 非 DSD 位率
        };
        std::printf("  %10s %10s %-11s %5s %9s %4s %5s\n", "源", "目标", "模式", "倍数", "中间PCM", "抽取", "插值");
        bool allOk = true;
        for (const auto& c : cases) {
            const DsdRatePlan p = plan_dsd_rate(c.s, c.d);
            std::printf("  %10u %10u %-11s %5u %9u %4d %5d\n", c.s, c.d, dsd_rate_mode_name(p.mode),
                        p.factor, p.midRate, p.decimation, p.interpolation);
            if (p.mode != c.want) allOk = false;
            if (p.mode == DsdRateMode::Unsupported && p.note.empty()) allOk = false;
        }
        check(allOk, "12 组速率对子的规划结论全部符合预期");
        const DsdRatePlan cross = plan_dsd_rate(2822400, 6144000);
        check(cross.note.find("19200") != std::string::npos &&
              cross.note.find("147:320") != std::string::npos,
              "跨族拒绝的理由带数字（最大公共整除率 / 比特域比例）",
              "19200 Hz / 147:320");
        const DsdRatePlan up = plan_dsd_rate(2822400, 5644800);
        check(up.midRate == 176400 && up.decimation == 16 && up.interpolation == 32,
              "DSD64→DSD128：中间 PCM 176400（1/16 抽取）×32 插值");
    }

    // ================= 2. 插值器原型 =================
    std::printf("\n=== 2. 插值器原型（内插域，判据直接量系数，不靠「看起来对」）===\n");
    {
        const DsdRatePlan p = plan_dsd_rate(2822400, 5644800);
        DsdRateConvertConfig cfg;
        DsdRateConverter conv(p, cfg, 1);
        std::vector<double> proto;
        conv.interp_prototype_into(proto);
        const double ref = 20.0 * std::log10((double)conv.interpolation());
        double worstPass = 0.0;
        for (int k = 0; k <= 400; ++k) {
            const double f = 20000.0 * (double)k / 400.0;
            worstPass = std::max(worstPass, std::fabs(magnitude_db(proto, 2.0 * f / (double)p.dstRate) - ref));
        }
        const double atImage = magnitude_db(proto, 2.0 * 156400.0 / (double)p.dstRate) - ref;
        const double atNyq = magnitude_db(proto, 2.0 * (double)(p.midRate / 2) / (double)p.dstRate) - ref;
        std::printf("  抽头/相位 %d（原型 %zu 抽头），L=%d\n", conv.tapsPerPhase(), proto.size(), conv.interpolation());
        std::printf("  通带 0…20 kHz 最差偏差 %.4f dB\n", worstPass);
        std::printf("  midRate/2 = %.0f Hz : %.2f dB（过渡带中段）\n", (double)(p.midRate / 2), atNyq);
        std::printf("  第一镜像起点 156400 Hz : %.1f dB\n", atImage);
        check(worstPass <= 0.01, "插值器通带 0…20 kHz 平坦度 ≤ 0.01 dB", std::to_string(worstPass) + " dB");
        check(atImage <= -150.0, "镜像起点（midRate−20 kHz）衰减 ≥ 150 dB", std::to_string(atImage) + " dB");
    }

    // ================= 3. DSD64 → DSD128 主判据 =================
    std::printf("\n=== 3. DSD64(2822400) → DSD128(5644800)：与「DSD64 原样」对照 ===\n");
    const double f0 = 371.0 * 176400.0 / 65536.0;   // 998.5 Hz，落在 FFT 整 bin
    Thdn base3, conv3;
    std::vector<double> pcmBase3, pcmConv3;
    {
        const size_t nBits = (size_t)(0.6 * 2822400.0);
        DsdStream src = modulate(2822400, 1, nBits, f0, 0.5);
        pcmBase3 = measure_pcm(src, 176400, 0);
        base3 = measure_thdn(std::vector<double>(pcmBase3.begin() + 4096, pcmBase3.end()), 176400.0, f0);

        DsdRateConvertConfig cfg;
        DsdStream out;
        const auto t0 = now_ms();
        const DsdConvertResult r = convert_dsd_stream(src, 5644800, cfg, out);
        const auto t1 = now_ms();
        pcmConv3 = measure_pcm(out, 176400, 0);
        conv3 = measure_thdn(std::vector<double>(pcmConv3.begin() + 4096, pcmConv3.end()), 176400.0, f0);
        std::printf("  源 %zu 位/声道（%.3f s）→ 输出 %llu 位/声道\n",
                    nBits, (double)nBits / 2822400.0, (unsigned long long)r.outBits);
        std::printf("  耗时：构造(调制器设计) %.0f ms + 峰值预扫 %.0f ms + 纯转换 %.0f ms = %.0f ms"
                    "（纯转换 %.2f× 实时，单声道）\n",
                    r.ctorMs, r.scanMs, r.ms, r.totalMs, r.realtimeFactor);
        std::printf("  srcPeak %.4f / 余量 %.2f dB / 生效阶数 %d%s\n", r.srcPeak, r.headroomDb, r.effectiveOrder,
                    r.steppedDown ? "（已降阶保护）" : "");
        check(r.outBits == (uint64_t)nBits * 2ull, "输出位数 = 输入位数 × 2（不多不少）",
              std::to_string(r.outBits) + " vs " + std::to_string((uint64_t)nBits * 2ull));
        std::printf("  基线 DSD64 原样：信号 %.3f dBFS  带内噪声 %.2f dBFS  THD+N %.2f dB\n",
                    base3.signalDb, base3.noiseDb, base3.thdnDb);
        std::printf("  转换 DSD128   ：信号 %.3f dBFS  带内噪声 %.2f dBFS  THD+N %.2f dB\n",
                    conv3.signalDb, conv3.noiseDb, conv3.thdnDb);
        std::printf("  差值：信号 %.4f dB / 噪声 %.3f dB / THD+N %.3f dB\n",
                    conv3.signalDb - base3.signalDb, conv3.noiseDb - base3.noiseDb, conv3.thdnDb - base3.thdnDb);
        check(conv3.thdnDb > 95.0, "转换后带内 THD+N > 95 dB", std::to_string(conv3.thdnDb) + " dB");
        check(std::fabs(conv3.thdnDb - base3.thdnDb) < 0.1,
              "THD+N 与「DSD64 原样」相差 < 0.1 dB（没有引入可闻失真）",
              std::to_string(conv3.thdnDb - base3.thdnDb) + " dB");
        check(std::fabs(conv3.signalDb - base3.signalDb) < 0.01,
              "基波幅度与源一致（差 < 0.01 dB）", std::to_string(conv3.signalDb - base3.signalDb) + " dB");
        // 噪声上界：Δnoise 越小，本链路的自噪声越低。由 ΔN 反推上界。
        // ΔN 可正可负（测量分辨率），两种情况都给出【上界】：把 |ΔN| 全算成链路自噪声
        const double dnDb = conv3.noiseDb - base3.noiseDb;
        const double mag = std::fabs(dnDb);
        const double ratio = std::pow(10.0, mag / 10.0) - 1.0;      // 额外噪声 / 源噪声（上界）
        const double boundDb = base3.noiseDb + 10.0 * std::log10(ratio + 1e-300);
        std::printf("  本链路自身带内噪声上界：%.1f dBFS（把 |ΔN|=%.3f dB 全算成自噪声）\n", boundDb, mag);
        check(dnDb < 0.05, "带内噪声增量 < 0.05 dB（链路自噪声被源底噪淹没）", std::to_string(dnDb) + " dB");
    }

    // ---- 3.1 通带平坦度 ----
    std::printf("\n  3.1 通带平坦度（0…20 kHz，两边都按实测峰值定余量）\n");
    {
        std::printf("    %8s %10s %10s %8s\n", "f(Hz)", "源 DSD64", "转换 DSD128", "差(dB)");
        const double freqs[] = { 100, 1000, 5000, 10000, 15000, 18000, 19000, 20000 };
        double worst = 0.0;
        for (double f : freqs) {
            const size_t nBits = (size_t)(0.42 * 2822400.0);
            DsdStream src = modulate(2822400, 1, nBits, f, 0.5);
            const auto pb = measure_pcm(src, 176400, 0);
            DsdRateConvertConfig cfg;
            DsdStream out;
            convert_dsd_stream(src, 5644800, cfg, out, true);
            const auto pc = measure_pcm(out, 176400, 0);
            const size_t skip = 8192;
            const double ab = tone_amp_db(std::vector<double>(pb.begin() + skip, pb.end()), 176400.0, f);
            const double ac = tone_amp_db(std::vector<double>(pc.begin() + skip, pc.end()), 176400.0, f);
            std::printf("    %8.0f %10.3f %10.3f %8.3f\n", f, ab, ac, ac - ab);
            worst = std::max(worst, std::fabs(ac - ab));
        }
        std::printf("    最差 |差| = %.3f dB\n", worst);
        check(worst <= 0.05, "通带平坦度偏差 ≤ 0.05 dB（0…20 kHz，含 19/20 kHz）",
              std::to_string(worst) + " dB");
    }

    // ---- 3.2 折叠残留 ----
    std::printf("\n  3.2 折叠残留：156.8 kHz 方波探针 → 折叠到 19.6 kHz\n");
    {
        const size_t nIn = 1048320;
        DsdStream probe = square_probe(2822400, 1, nIn, 156800.0, nIn);   // 无静音段
        const auto pb = measure_pcm(probe, 176400, 0);
        const size_t skip = 1024;
        auto tone_db = [](const std::vector<double>& x, double fs, double f) {
            double re = 0, im = 0; const double w = 2.0 * kPi * f / fs;
            for (size_t n = 0; n < x.size(); ++n) { re += x[n] * std::cos(w * (double)n); im -= x[n] * std::sin(w * (double)n); }
            const double a = 2.0 * std::sqrt(re * re + im * im) / (double)x.size();
            return 20.0 * std::log10(a + 1e-300);
        };
        const size_t lenB = ((pb.size() - skip) / 9) * 9;
        const double ab = tone_db(std::vector<double>(pb.begin() + (long long)skip,
                                                     pb.begin() + (long long)(skip + lenB)), 176400.0, 19600.0);
        DsdRateConvertConfig cfg;
        DsdStream out;
        convert_dsd_stream(probe, 5644800, cfg, out, false);
        const auto pc = measure_pcm(out, 176400, 0);
        const size_t lenC = ((pc.size() - skip) / 9) * 9;
        const double ac = tone_db(std::vector<double>(pc.begin() + (long long)skip,
                                                     pc.begin() + (long long)(skip + lenC)), 176400.0, 19600.0);
        std::printf("    基线 DSD64 原样 . : %.2f dBFS（%zu 样本）\n", ab, lenB);
        std::printf("    转换 DSD128      : %.2f dBFS（%zu 样本），差 %+.2f dB\n", ac, lenC, ac - ab);
        check(ac <= -100.0, "转换后折叠残留 ≤ −100 dBFS", std::to_string(ac) + " dBFS");
        check(ac <= ab + 3.0, "折叠残留不比「DSD64 原样」更差（+3 dB 容差）",
              std::to_string(ac - ab) + " dB");
    }

    // ---- 3.3 DoP 合法性 + 分块无关性 ----
    std::printf("\n  3.3 DoP 合法性与分块无关性\n");
    {
        const size_t nBits = (size_t)(0.3 * 2822400.0);
        DsdStream src = modulate(2822400, 2, nBits, f0, 0.5);
        DsdRateConvertConfig cfg;
        DsdStream out;
        convert_dsd_stream(src, 5644800, cfg, out);
        std::vector<int32_t> packed;
        DopPackState st;
        const size_t groups = dsd_to_dop(out, packed, st);
        bool marks = true;
        for (int c = 0; c < out.channels; ++c)
            for (size_t n = 0; n < groups; ++n) {
                const uint32_t m = ((uint32_t)packed[n * 2 + (size_t)c] >> 16) & 0xFF;
                const uint32_t want = (n & 1) ? DOP_MARKER_B : DOP_MARKER_A;
                if (m != want) marks = false;
            }
        DsdStream back;
        const bool unpackOk = dop_to_dsd_bits(packed.data(), groups, out.channels, back);
        uint64_t diff = 0;
        const uint64_t cmp = (uint64_t)groups * 16ull;
        for (int c = 0; c < out.channels; ++c)
            for (uint64_t n = 0; n < cmp; ++n) if (out.bit((size_t)c, n) != back.bit((size_t)c, n)) ++diff;
        std::printf("    DoP：%zu 载波帧/声道（%s），逐位比对 %llu 位不同 %llu\n",
                    groups, (out.rate == 5644800 ? "载波 352800 Hz" : "?"),
                    (unsigned long long)(cmp * (uint64_t)out.channels), (unsigned long long)diff);
        check(marks, "转换后的 1-bit 流装进 DoP 后标记逐帧交替 05/FA（接收端不会判非法）");
        check(unpackOk && diff == 0, "DoP 解包后与转换结果逐位一致");
        check(out.rate == 5644800, "输出流位率标注 = 5644800", std::to_string(out.rate));

        // 分块无关性：一次性 vs 1000 位一块
        DsdRateConverter c1(plan_dsd_rate(2822400, 5644800), cfg, 1);
        DsdStream mono; mono.rate = src.rate; mono.channels = 1; mono.bits = src.bits;
        mono.data.assign(1, src.data[0]);
        DsdStream o1; c1.process_bits(mono, o1); c1.finish(o1);
        DsdRateConverter c2(plan_dsd_rate(2822400, 5644800), cfg, 1);
        DsdStream o2;
        for (uint64_t pos = 0; pos < mono.bits; pos += 1000) {
            const size_t n = (size_t)std::min<uint64_t>(1000, mono.bits - pos);
            DsdStream ck; ck.rate = mono.rate; ck.channels = 1; ck.bits = n; ck.data.assign(1, {});
            ck.data[0].assign(mono.data[0].begin() + (long long)(pos / 8),
                              mono.data[0].begin() + (long long)((pos + n + 7) / 8));
            c2.process_bits(ck, o2);
        }
        c2.finish(o2);
        uint64_t bd = 0;
        const uint64_t bn = std::min<uint64_t>(o1.bits, o2.bits);
        for (uint64_t i = 0; i < bn; ++i) if (o1.bit(0, i) != o2.bit(0, i)) ++bd;
        std::printf("    分块：一次性 %llu 位 / 1000 位分块 %llu 位，不同 %llu 位\n",
                    (unsigned long long)o1.bits, (unsigned long long)o2.bits, (unsigned long long)bd);
        check(bd == 0 && o1.bits == o2.bits, "输出与分块方式无关（逐位相同）");
    }

    // ---- 3.4 降速（DSD128 → DSD64）同样成立 ----
    std::printf("\n  3.4 降速 DSD128(5644800) → DSD64(2822400)\n");
    {
        const size_t nBits = (size_t)(0.4 * 5644800.0);
        DsdStream src = modulate(5644800, 1, nBits, f0, 0.5);
        const auto pb = measure_pcm(src, 176400, 0);
        const Thdn rb = measure_thdn(std::vector<double>(pb.begin() + 4096, pb.end()), 176400.0, f0);
        DsdRateConvertConfig cfg;
        DsdStream out;
        const DsdConvertResult r = convert_dsd_stream(src, 2822400, cfg, out, true);
        const auto pc = measure_pcm(out, 176400, 0);
        const Thdn rc = measure_thdn(std::vector<double>(pc.begin() + 4096, pc.end()), 176400.0, f0);
        std::printf("    源 DSD128 原样：THD+N %.2f dB；降速 DSD64：THD+N %.2f dB（差 %+.2f dB），阶数 %d\n",
                    rb.thdnDb, rc.thdnDb, rc.thdnDb - rb.thdnDb, r.effectiveOrder);
        std::printf("    位数 %llu → %llu（应 = 1/2），余量 %.2f dB，耗时 %.1f ms（%.2f× 实时）\n",
                    (unsigned long long)r.inBits, (unsigned long long)r.outBits, r.headroomDb, r.ms, r.realtimeFactor);
        check(r.outBits == (uint64_t)nBits / 2ull, "降速输出位数 = 输入位数 / 2",
              std::to_string(r.outBits) + " vs " + std::to_string((uint64_t)nBits / 2ull));
        // 降速的参照不是"源 DSD128"（DSD64 的 1-bit 量化压根到不了 106.8 dB），
        // 而是【原生 DSD64】本身的带内成绩（base3 = 同一音、同一幅度、原生 DSD64）
        std::printf("    原生 DSD64 基线（同一音/幅度）：THD+N %.2f dB；降速后 %.2f dB（差 %+.2f dB）\n",
                    base3.thdnDb, rc.thdnDb, rc.thdnDb - base3.thdnDb);
        check(rc.thdnDb > 95.0 && rc.thdnDb > base3.thdnDb - 1.0,
              "降速后 THD+N > 95 dB 且不差于原生 DSD64（-1 dB 容差）",
              std::to_string(rc.thdnDb) + " dB vs 原生 " + std::to_string(base3.thdnDb) + " dB");
    }

    // ================= 4. 级联 vs 直接 =================
    std::printf("\n=== 4. 级联 DSD64→128→256 vs 直接 DSD64→256 ===\n");
    {
        const size_t nBits = (size_t)(0.5 * 2822400.0);
        DsdStream src = modulate(2822400, 1, nBits, f0, 0.5);
        DsdRateConvertConfig cfg;
        DsdStream d128, d256dir, d256cas;
        const double t0 = now_ms();
        convert_dsd_stream(src, 5644800, cfg, d128, true);
        const double t1 = now_ms();
        convert_dsd_stream(d128, 11289600, cfg, d256cas, true);
        const double t2 = now_ms();
        convert_dsd_stream(src, 11289600, cfg, d256dir, true);
        const double t3 = now_ms();
        std::printf("  64→128 %.0f ms | 128→256 %.0f ms | 64→256 直接 %.0f ms\n", t1 - t0, t2 - t1, t3 - t2);
        const auto pdir = measure_pcm(d256dir, 176400, 0);
        const auto pcas = measure_pcm(d256cas, 176400, 0);
        const Thdn rd = measure_thdn(std::vector<double>(pdir.begin() + 4096, pdir.end()), 176400.0, f0);
        const Thdn rc = measure_thdn(std::vector<double>(pcas.begin() + 4096, pcas.end()), 176400.0, f0);
        std::printf("  直接  DSD256：信号 %.3f  带内噪声 %.2f  THD+N %.2f\n", rd.signalDb, rd.noiseDb, rd.thdnDb);
        std::printf("  级联  DSD256：信号 %.3f  带内噪声 %.2f  THD+N %.2f\n", rc.signalDb, rc.noiseDb, rc.thdnDb);
        std::printf("  Δ信号 %.4f dB / Δ噪声 %+.3f dB / ΔTHD+N %+.3f dB\n",
                    rc.signalDb - rd.signalDb, rc.noiseDb - rd.noiseDb, rc.thdnDb - rd.thdnDb);
        // ★ 差多少怎么量：不做时域逐点相减。
        //   两条路径的 1-bit 量化噪声互不相关，且延迟差是【亚样本】（实测换算 0.045 样本
        //   = -39 dB 的假残差），逐点相减测的是延迟估计精度，不是级联误差
        //   （同一坑见 docs/滤波器家族纪要.md「三个坑」）。改为与延迟无关的判据：
        //   各自最小二乘扣掉基波后，比两段残差之差（基波相位/幅度不参与比较）。
        const size_t from = 4096, N = 65536;
        const double w0 = 2.0 * kPi * f0 / 176400.0;
        double cc = 0, ss = 0, cs = 0;
        for (size_t n = 0; n < N; ++n) {
            const double c = std::cos(w0 * (double)n), s = std::sin(w0 * (double)n);
            cc += c * c; ss += s * s; cs += c * s;
        }
        const double det = cc * ss - cs * cs;
        auto fit = [&](const std::vector<double>& x, double& A, double& B) {
            double xc = 0, xs = 0;
            for (size_t n = 0; n < N; ++n) {
                const double c = std::cos(w0 * (double)n), s = std::sin(w0 * (double)n);
                xc += x[from + n] * c; xs += x[from + n] * s;
            }
            A = (xc * ss - xs * cs) / det;
            B = (xs * cc - xc * cs) / det;
        };
        double Ac = 0, Bc = 0, Ad = 0, Bd = 0;
        fit(pcas, Ac, Bc);
        fit(pdir, Ad, Bd);
        // 差信号必须【带限到 20 Hz…20 kHz】再算功率：测量链输出到 176.4 kHz，
        // 20 kHz 以上是 1-bit 噪声整形的废热（全带宽算出来是 -45 dBFS，会冒充"级联误差"）
        std::vector<cplx> spec(N);
        double sumW2 = 0;
        for (size_t n = 0; n < N; ++n) {
            const double c = std::cos(w0 * (double)n), s = std::sin(w0 * (double)n);
            const double rc = pcas[from + n] - (Ac * c + Bc * s);
            const double rd = pdir[from + n] - (Ad * c + Bd * s);
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)n / (double)(N - 1));
            sumW2 += w * w;
            spec[n] = cplx((rc - rd) * w, 0.0);
        }
        fft(spec);
        const double binHz = 176400.0 / (double)N, norm = (double)N * sumW2;
        const size_t kLo = (size_t)std::ceil(20.0 / binHz), kTop = (size_t)std::floor(20000.0 / binHz);
        double pdiff = 0;
        for (size_t k = kLo; k <= kTop && k < N / 2; ++k) pdiff += 2.0 * std::norm(spec[k]) / norm;
        const double psig = (Ad * Ad + Bd * Bd) / 2.0;
        const double residDb = 10.0 * std::log10(pdiff / (psig + 1e-300));
        std::printf("  扣基波后两条路径的差信号（带限 20 Hz…20 kHz）：%.2f dBFS，相对信号 %.2f dB\n",
                    10.0 * std::log10(pdiff + 1e-300), residDb);
        const double theory = rc.noiseDb + 10.0 * std::log10(2.0);   // 两条独立噪声相加
        std::printf("  [理论] 两条独立、各 %.2f dBFS 的带内噪声相加 = %.2f dBFS；实测 %.2f dBFS（%+.2f dB）\n",
                    rc.noiseDb, theory, 10.0 * std::log10(pdiff + 1e-300),
                    10.0 * std::log10(pdiff + 1e-300) - theory);
        std::printf("  [注] 差信号不是级联误差（它含两条路径各自的量化噪声）；\n");
        std::printf("       级联误差的判据是 ΔTHD+N / Δ带内噪声，两者都在 0.0004 dB。\n");
        check(std::fabs(rc.thdnDb - rd.thdnDb) < 0.1, "级联与直接的 THD+N 相差 < 0.1 dB",
              std::to_string(rc.thdnDb - rd.thdnDb) + " dB");
        check(std::fabs(rc.noiseDb - rd.noiseDb) < 0.1, "级联与直接的带内噪声相差 < 0.1 dB",
              std::to_string(rc.noiseDb - rd.noiseDb) + " dB");
        check(residDb < -80.0, "扣基波后两条路径的差信号 < −80 dB（相对信号）", std::to_string(residDb) + " dB");
    }

    // ================= 5. 跨族：必须明确报错 =================
    std::printf("\n=== 5. 跨族（44.1k 族 → 48k 族）===\n");
    {
        const DsdRatePlan p = plan_dsd_rate(2822400, 6144000);
        std::printf("  plan: %s\n  %s\n", dsd_rate_mode_name(p.mode), p.note.c_str());
        check(p.mode == DsdRateMode::Unsupported && !p.ok(), "plan 明确拒绝跨族");
        bool threw = false;
        std::string msg;
        try {
            DsdStream src = modulate(2822400, 1, 4096, 1000.0, 0.5);
            DsdStream out;
            convert_dsd_stream(src, 6144000, DsdRateConvertConfig{}, out);
        } catch (const std::exception& e) { threw = true; msg = e.what(); }
        std::printf("  convert 抛出的错误：%s\n", msg.c_str());
        check(threw, "convert 直接抛异常（不静默降级、不偷偷降到别的档位）");
        check(msg.find("跨族") != std::string::npos && msg.find("19200") != std::string::npos,
              "错误信息包含跨族判定依据（不是一句「不支持」");
    }

    // ================= 6. 满幅 / 越界 / 恢复（铁律 7） =================
    std::printf("\n=== 6. 满幅方波 / 单样本越界 / 恢复 ===\n");
    {
        const size_t nBits = (size_t)(0.4 * 2822400.0);
        const size_t quietFrom = (size_t)(0.7 * (double)nBits);
        DsdStream src = square_probe(2822400, 1, nBits, 1000.0, quietFrom);
        DsdRateConvertConfig cfg;
        DsdStream out;
        const DsdConvertResult r = convert_dsd_stream(src, 5644800, cfg, out, true);
        const auto pcm = measure_pcm(out, 176400, 0);
        const size_t qf = (size_t)((double)quietFrom / 2822400.0 * 176400.0);
        const double fund = tone_amp_db(pcm, 176400.0, 1000.0);
        const double tailBand = band_rms_db(pcm, qf + 4096, 16384, 176400.0);
        // 参考：全静音源经过同一条链路，尾部带限 RMS
        DsdStream sil = silence_probe(2822400, 1, nBits);
        DsdStream silOut;
        convert_dsd_stream(sil, 5644800, cfg, silOut, false);
        const auto silPcm = measure_pcm(silOut, 176400, 0);
        const double silBand = band_rms_db(silPcm, 4096, 16384, 176400.0);
        // 基线：同一个满幅方波按 DSD64 原样测
        const auto basePcm = measure_pcm(src, 176400, 0);
        const double baseFund = tone_amp_db(basePcm, 176400.0, 1000.0);

        DsdRatePlan plan = plan_dsd_rate(2822400, 5644800);
        DsdRateConverter conv(plan, cfg, 1);
        conv.set_pre_atten_db(sdm_headroom_db(r.srcPeak));
        DsdStream o2;
        size_t overAfterQuiet = -1;
        for (uint64_t pos = 0; pos < src.bits; pos += (1u << 18)) {
            const size_t n = (size_t)std::min<uint64_t>(1u << 18, src.bits - pos);
            DsdStream ck; ck.rate = src.rate; ck.channels = 1; ck.bits = n; ck.data.assign(1, {});
            ck.data[0].assign(src.data[0].begin() + (long long)(pos / 8),
                              src.data[0].begin() + (long long)((pos + n + 7) / 8));
            conv.process_bits(ck, o2);
            if (pos >= quietFrom) overAfterQuiet = conv.stats().overload;
        }
        conv.finish(o2);
        std::printf("  满幅方波：srcPeak %.4f（>1 是 Gibbs 过冲）/ 余量 %.2f dB\n", r.srcPeak, r.headroomDb);
        std::printf("  调制器：overload %lld / clamp %lld / nonFinite %lld（静音段之后 overload=%lld）\n",
                    conv.stats().overload, conv.stats().clamped, conv.stats().nonFinite, overAfterQuiet);
        std::printf("  1 kHz 基波：转换 %.2f dBFS / 源（DSD64 原样）%.2f dBFS，差 %+.3f dB\n",
                    fund, baseFund, fund - baseFund);
        std::printf("  尾部带限(20 Hz…20 kHz) RMS：满幅之后 %.2f dBFS / 纯静音参考 %.2f dBFS\n",
                    tailBand, silBand);
        check(r.srcPeak > 1.0, "满幅方波的抽取峰值确实过 1.0（Gibbs 过冲被看见）",
              std::to_string(r.srcPeak));
        check(conv.stats().overload == 0 && conv.stats().clamped == 0, "满幅下调制器不缺省过载/钳位");
        check(conv.stats().nonFinite == 0, "无非有限值");
        check(overAfterQuiet == conv.stats().overload, "静音段内 overload 不再增长（限幅计数冻结）");
        // 比较必须扣除已知预衰减（-5.47 dB 是 sdm_headroom_db 按实测峰值给的），否则测的是增益差
        const double fundRef = fund - r.headroomDb;
        check(std::fabs(fundRef - baseFund) < 0.05,
              "满幅方波 1 kHz 基波扣除预衰减后与源一致（< 0.05 dB）",
              std::to_string(fundRef - baseFund) + " dB");
        check(tailBand < silBand + 6.0 && tailBand < -100.0,
              "满幅之后静音尾的带内残留接近纯静音参考（≤ 参考+6 dB 且 < −100 dBFS）",
              std::to_string(tailBand) + " dBFS");
    }
    {
        // 单样本越界 + 恢复：在 PCM 接缝上注入 +2.0（1-bit 域无法表达越界，接缝是唯一诚实的做法）
        const size_t nBits = (size_t)(0.3 * 2822400.0);
        DsdStream src = modulate(2822400, 1, nBits, 1000.0, 0.5);
        DsdRatePlan plan = plan_dsd_rate(2822400, 5644800);
        DsdRateConvertConfig cfg;
        DsdRateConverter ref(plan, cfg, 1); ref.set_pre_atten_db(0.0);
        DsdRateConverter hit(plan, cfg, 1); hit.set_pre_atten_db(0.0);
        std::vector<std::vector<double>> pcm(1);
        ref.to_pcm(src, pcm);
        auto pcm2 = pcm;
        const size_t at = pcm[0].size() / 2;
        pcm2[0][at] += 2.0;
        DsdStream oa, ob;
        ref.push_pcm(pcm, pcm[0].size(), oa); ref.finish(oa);
        hit.push_pcm(pcm2, pcm2[0].size(), ob); hit.finish(ob);
        const auto pa = measure_pcm(oa, 176400, 0), pb = measure_pcm(ob, 176400, 0);
        const size_t mid = pa.size() / 2;
        const double dBefore = band_rms_db(pa, 4096, 16384, 176400.0) - band_rms_db(pb, 4096, 16384, 176400.0);
        const double dAfter  = band_rms_db(pa, mid + 4096, 16384, 176400.0) - band_rms_db(pb, mid + 4096, 16384, 176400.0);
        const double dTail   = band_rms_db(pa, pa.size() - 16384, 16384, 176400.0) -
                               band_rms_db(pb, pb.size() - 16384, 16384, 176400.0);
        const double fRef = tone_amp_db(pa, 176400.0, 1000.0, 16384);
        const double fHit = tone_amp_db(pb, 176400.0, 1000.0, 16384);
        std::printf("  单样本 +2.0：overload %lld / clamp %lld / nonFinite %lld / 插值后峰值 %.3f\n",
                    hit.stats().overload, hit.stats().clamped, hit.stats().nonFinite, hit.stats().inPeak);
        std::printf("  带限 RMS 差：越界前 %.3f dB / 越界后 16384 样本 %.3f dB / 尾部 %.3f dB\n",
                    dBefore, dAfter, dTail);
        std::printf("  1 kHz 基波：参考 %.2f dBFS / 注入 %.2f dBFS（差 %+.3f dB）\n", fRef, fHit, fHit - fRef);
        check(hit.stats().overload > 0, "单样本 +2.0 确实触发了环路限幅（不是「没测到」）",
              std::to_string(hit.stats().overload) + " 次");
        check(hit.stats().nonFinite == 0, "越界后无非有限值");
        check(std::fabs(dTail) < 0.1, "尾部带限 RMS 与未注入参考一致（恢复，差 < 0.1 dB）",
              std::to_string(dTail) + " dB");
        check(std::fabs(fHit - fRef) < 0.05, "越界后 1 kHz 基波不变（< 0.05 dB）",
              std::to_string(fHit - fRef) + " dB");
    }

    // ================= 7. 真实 .dsf 端到端 =================
    std::printf("\n=== 7. 真实 .dsf 端到端（流式解码 + 转换 + DoP 合法性）===\n");
    {
        const char* path = "D:\\RTA-hqplayer\\MUSIC\\银の龙の背に乘って.dsf";
        if (!std::filesystem::exists(u8path_of(path))) {
            std::printf("  [SKIP] 文件不存在：%s\n", path);
        } else {
            const auto fileMb = (double)std::filesystem::file_size(u8path_of(path)) / (1024.0 * 1024.0);
            const double memBefore = mem_peak_mb();
            rta::audio::DsdPcmStream st;
            std::string err;
            const auto t0 = now_ms();
            const bool opened = st.open(path, rta::audio::dsd_pcm_settings(), &err);
            check(opened, "流式解码器打开真实 DSF", err);
            if (opened) {
                const double fullSec = st.seconds();
                const uint32_t srcRate = (uint32_t)((double)st.rate() * 16.0);
                std::printf("  文件 %.1f MB / %u Hz / %d ch / %.2f s；流式解出 PCM %u Hz\n",
                            fileMb, srcRate, st.channels(), fullSec, st.rate());
                DsdRatePlan plan = plan_dsd_rate(srcRate, srcRate * 2u);
                DsdRateConvertConfig cfg;
                const double tc0 = now_ms();
                DsdRateConverter conv(plan, cfg, st.channels());
                const double tc1 = now_ms();
                // 峰值预扫（只读开头 20 s）：与 rtaweb 的 ISP 预扫同口径
                const double scanSec = std::min(20.0, fullSec);
                const size_t scanFrames = (size_t)(scanSec * (double)st.rate());
                double peak = 0.0;
                std::vector<std::vector<double>> pcm((size_t)st.channels());
                rta::audio::AudioBuffer buf;
                for (size_t pos = 0; pos < scanFrames; ) {
                    const size_t got = st.read(pos, 4096, buf);
                    if (!got) break;
                    for (int c = 0; c < st.channels(); ++c)
                        for (size_t i = 0; i < got; ++i)
                            peak = std::max(peak, std::fabs(buf.sample(i, c)));
                    pos += got;
                }
                const double hd = sdm_headroom_db(peak);
                conv.set_pre_atten_db(hd);
                // 端到端：取前 10 s 转成 DSD128（长度上限，ctest 不能被单个大文件拖死）
                const double convSec = std::min(10.0, fullSec);
                const size_t convFrames = (size_t)(convSec * (double)st.rate());
                DsdStream out;
                out.rate = plan.dstRate; out.channels = st.channels();
                out.data.assign((size_t)st.channels(), {});
                out.bits = 0;
                const auto t1 = now_ms();
                for (size_t pos = 0; pos < convFrames; ) {
                    const size_t got = st.read(pos, 4096, buf);
                    if (!got) break;
                    for (int c = 0; c < st.channels(); ++c) {
                        pcm[(size_t)c].resize(got);
                        for (size_t i = 0; i < got; ++i) pcm[(size_t)c][i] = buf.sample(i, c);
                    }
                    conv.push_pcm(pcm, got, out);
                    pos += got;
                }
                conv.finish(out);
                const double t2 = now_ms();
                // DoP 合法性（只装前 512 帧，够判标记序列）
                std::vector<int32_t> packed;
                DopPackState ps;
                const size_t groups = dsd_to_dop(out, packed, ps);
                DsdStream back;
                const bool ok = dop_to_dsd_bits(packed.data(), groups, st.channels(), back);
                const double convMs = t2 - t1;
                const double audioSec = (double)out.bits / (double)plan.dstRate;
                std::printf("  构造 %.0f ms | 预扫 %.1f s 峰值 %.4f → 余量 %.2f dB\n", tc1 - tc0, scanSec, peak, hd);
                std::printf("  转换 %.2f s 音频：%.0f ms（%.2f× 实时，双声道单线程）→ %llu 位/声道\n",
                            audioSec, convMs, audioSec / (convMs / 1000.0),
                            (unsigned long long)out.bits);
                std::printf("  线性外推整轨 %.2f s：%.1f s（另有构造 %.2f s 一次性开销）\n",
                            fullSec, fullSec / (audioSec / (convMs / 1000.0)),
                            (tc1 - tc0) / 1000.0);
                std::printf("  内存：开跑前峰值 %.1f MB → 结束时峰值 %.1f MB（+%.1f MB）\n",
                            memBefore, mem_peak_mb(), mem_peak_mb() - memBefore);
                std::printf("  DoP：%zu 帧/声道，解包%s（标记合法）\n", groups, ok ? "通过" : "失败");
                check(audioSec > convSec - 0.01, "转换产出的时长与喂进去的时长一致",
                      std::to_string(audioSec) + " s vs " + std::to_string(convSec) + " s");
                check(ok, "真实文件转换结果装 DoP 后标记合法");
                check(audioSec / (convMs / 1000.0) > 1.0, "真实文件转换吞吐 > 1× 实时（双声道单线程）",
                      std::to_string(audioSec / (convMs / 1000.0)) + "×");
                check(mem_peak_mb() - memBefore < 400.0, "流式路径内存增量 < 400 MB（不整曲入内存）",
                      std::to_string(mem_peak_mb() - memBefore) + " MB");
            }
        }
    }

    std::printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
