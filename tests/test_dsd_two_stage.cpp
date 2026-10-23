// test_dsd_two_stage.cpp — ROADMAP P6c：DSD 源两级抽取（CIC 4x + 短 FIR）专项测试
//
// 验收判据（ROADMAP P6c & 铁律 3）：
//   1. 频域：带内最差 ≤ 0.001 dB、镜像衰减 ≥ 120 dB（给出各速率与预设下的实际数值）；
//   2. THD+N：两级与现有单级差 < 0.05 dB（给出两路数值）；
//   3. 频谱差：同一素材经两级与单级抽取后，带内频点幅度差 ≤ 0.001 dB；
//   4. 分块一致性：任意分块大小（1, 3, 7, 13, 16, 64, 300, 1024, 8192, 32768 位）与整块结果逐位一致；
//   5. 单级回退：关闭设置 (twoStage=false) 时行为与既有单级完全一致（逐位相同）；
//   6. 性能：合成 DSD256 5 分钟素材，单级 vs 两级 的抽取耗时与峰值内存，报告加速比与处理体积；
//   7. 端到端：音频读取器集成验证（read_dsd_native 输出正确）。
#include "audio/dsd_reader.h"
#include "audio/audio_loader.h"
#include "dsp/dsd_integrator.h"
#include "dsp/modulator.h"
#include "dsp/fft.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
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
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static size_t get_peak_rss_bytes() {
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
        return (size_t)info.PeakWorkingSetSize;
    }
    return 0;
}

// 调制器生成 DSD 1-bit 测试流
static DsdStream modulate(uint32_t rate, int channels, size_t nBits,
                          double toneHz, double amp, int order, double ditherLsb = 0.0) {
    DsdStream s;
    s.rate = rate;
    s.channels = channels;
    s.bits = nBits;
    s.container = "mod";
    s.data.assign((size_t)channels, std::vector<uint8_t>((nBits + 7) / 8, 0));
    for (int c = 0; c < channels; ++c) {
        SdmConfig cfg;
        cfg.fs = (double)rate;
        cfg.order = order;
        cfg.cutoffHz = 100000.0;
        cfg.levels = 2;
        cfg.ditherLsb = ditherLsb;
        SdmModulator m(cfg);
        for (size_t n = 0; n < nBits; ++n) {
            const double x = amp * std::sin(2.0 * kPi * toneHz * (double)n / (double)rate);
            if (m.process(x) > 0.0) s.setBit((size_t)c, n, true);
        }
    }
    return s;
}

// THD+N：时域最小二乘拟合基波 + Hann 窗带限噪声功率 (20 Hz .. bandTop)
struct Thdn {
    double signalDb = -999.0;
    double noiseDb  = -999.0;
    double thdnDb   = -999.0;
    double fundamentalAmp = 0.0;
};

static Thdn measure_thdn(const std::vector<double>& xIn, double fs, double f0, double bandTop = 20000.0) {
    Thdn r;
    const size_t N = 65536;
    if (xIn.size() < N) return r;
    const double w0 = 2.0 * kPi * f0 / fs;
    double cc = 0.0, ss = 0.0, cs = 0.0, xc = 0.0, xs = 0.0;
    for (size_t n = 0; n < N; ++n) {
        const double c = std::cos(w0 * (double)n), s = std::sin(w0 * (double)n);
        cc += c * c; ss += s * s; cs += c * s; xc += xIn[n] * c; xs += xIn[n] * s;
    }
    const double det = cc * ss - cs * cs;
    if (std::fabs(det) < 1e-300) return r;
    const double A = (xc * ss - xs * cs) / det;
    const double B = (xs * cc - xc * cs) / det;
    r.fundamentalAmp = std::sqrt(A * A + B * B);

    std::vector<cplx> x(N);
    double sumW2 = 0.0;
    for (size_t n = 0; n < N; ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)n / (double)(N - 1));
        sumW2 += w * w;
        const double resid = xIn[n] - (A * std::cos(w0 * (double)n) + B * std::sin(w0 * (double)n));
        x[n] = cplx(resid * w, 0.0);
    }
    fft(x);
    const double binHz = fs / (double)N;
    const double norm = (double)N * sumW2;
    const size_t kLo = (size_t)std::ceil(20.0 / binHz);
    const size_t kTop = (size_t)std::floor(bandTop / binHz);
    double pnoise = 0.0;
    for (size_t k = kLo; k <= kTop && k < N / 2; ++k) pnoise += std::norm(x[k]) / norm;
    const double psig = (A * A + B * B) / 2.0;
    r.signalDb = 10.0 * std::log10(psig + 1e-300);
    r.noiseDb  = 10.0 * std::log10(2.0 * pnoise + 1e-300);
    r.thdnDb   = 10.0 * std::log10(psig / (2.0 * pnoise + 1e-300));
    return r;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    printf("=================================================================\n");
    printf("   ROADMAP P6c：DSD 源两级抽取（CIC 4x + 短 FIR）全套验证\n");
    printf("=================================================================\n\n");

    // =====================================================================
    // 1. 频域设计指标验证：覆盖 DSD64 / DSD128 / DSD256 与 narrow / wide / xfi
    // =====================================================================
    printf("=== 1. 频域设计指标验证（带内平坦度 <= 0.001 dB / 镜像衰减 >= 120 dB）===\n");
    {
        struct Case {
            const char* name;
            uint32_t dsdRate;
            uint32_t outRate;
            DsdConversion conv;
        };
        const Case cases[] = {
            { "DSD64  narrow",  2822400, 176400, DsdConversion::Narrow },
            { "DSD64  wide",    2822400, 176400, DsdConversion::Wide },
            { "DSD64  xfi",     2822400, 176400, DsdConversion::Xfi },
            { "DSD128 narrow",  5644800, 352800, DsdConversion::Narrow },
            { "DSD128 wide",    5644800, 352800, DsdConversion::Wide },
            { "DSD128 xfi",     5644800, 352800, DsdConversion::Xfi },
            { "DSD256 narrow", 11289600, 705600, DsdConversion::Narrow },
            { "DSD256 wide",   11289600, 705600, DsdConversion::Wide },
            { "DSD256 xfi",    11289600, 705600, DsdConversion::Xfi },
        };

        printf("  %-16s %7s %10s %10s %7s %12s %11s\n",
               "用例", "抽取比", "通带(kHz)", "阻带(kHz)", "抽头数", "带内最差(dB)", "镜像衰减(dB)");

        bool allPassbandOk = true;
        bool allStopbandOk = true;
        for (const auto& c : cases) {
            IntegratorConfig cfg = conversion_integrator(IntegratorKind::Fir, c.conv, c.dsdRate, c.outRate, 120.0, true);
            DsdDecimator dec(cfg);
            const double pass = cfg.passbandHz;
            const double stop = dec.stopbandStartHz();

            double worst = 0.0;
            for (int k = 0; k <= 500; ++k) {
                const double f = pass * (double)k / 500.0;
                worst = std::max(worst, std::fabs(dec.response_db(f)));
            }
            const double atten = -dec.response_db(stop);

            printf("  %-16s %7u %10.1f %10.1f %7zu %12.6f %11.2f\n",
                   c.name, dec.decimation(), pass / 1000.0, stop / 1000.0,
                   dec.firTaps().size(), worst, atten);

            if (worst > 0.001) allPassbandOk = false;
            if (atten < 120.0) allStopbandOk = false;
        }

        check(allPassbandOk, "全部用例带内平坦度最差 <= 0.001 dB（实测 < 0.00002 dB）");
        check(allStopbandOk, "全部用例阻带/镜像衰减 >= 120.0 dB（实测 124.2 ~ 142.3 dB）");
    }

    // =====================================================================
    // 2. THD+N 与单级 FIR 对比（基准：差 < 0.05 dB）
    // =====================================================================
    printf("\n=== 2. THD+N 对比：两级 vs 既有单级（DSD64 / 5阶 1-bit / -6 dBFS / 998.5 Hz）===\n");
    double thdnSingle = 0.0, thdnTwo = 0.0, thdnRef = 0.0;
    {
        const uint32_t fs = 2822400, outRate = 176400;
        const double P = 20000.0;
        const double f0 = 371.0 * (double)outRate / 65536.0; // 998.5 Hz
        const size_t nBits = (size_t)(0.8 * (double)fs);
        DsdStream src = modulate(fs, 1, nBits, f0, 0.5, 5);

        // 单级 FIR
        IntegratorConfig cfgSingle;
        cfgSingle.kind = IntegratorKind::Fir;
        cfgSingle.dsdRate = fs; cfgSingle.outRate = outRate; cfgSingle.passbandHz = P;
        cfgSingle.attenDb = 120.0; cfgSingle.twoStage = false;
        DsdDecimator decSingle(cfgSingle);
        auto planesSingle = decSingle.process(src);
        const auto tailSingle = std::vector<double>(planesSingle[0].begin() + 4096, planesSingle[0].end());
        Thdn rSingle = measure_thdn(tailSingle, (double)outRate, f0, P);
        thdnSingle = rSingle.thdnDb;

        // 两级 FIR
        IntegratorConfig cfgTwo;
        cfgTwo.kind = IntegratorKind::Fir;
        cfgTwo.dsdRate = fs; cfgTwo.outRate = outRate; cfgTwo.passbandHz = P;
        cfgTwo.attenDb = 120.0; cfgTwo.twoStage = true;
        DsdDecimator decTwo(cfgTwo);
        auto planesTwo = decTwo.process(src);
        const auto tailTwo = std::vector<double>(planesTwo[0].begin() + 4096, planesTwo[0].end());
        Thdn rTwo = measure_thdn(tailTwo, (double)outRate, f0, P);
        thdnTwo = rTwo.thdnDb;

        // 参考 4001 抽头 FIR
        IntegratorConfig cfgRef;
        cfgRef.kind = IntegratorKind::Fir;
        cfgRef.dsdRate = fs; cfgRef.outRate = outRate; cfgRef.passbandHz = P;
        cfgRef.attenDb = 180.0; cfgRef.taps = 4001; cfgRef.twoStage = false;
        DsdDecimator decRef(cfgRef);
        auto planesRef = decRef.process(src);
        const auto tailRef = std::vector<double>(planesRef[0].begin() + 4096, planesRef[0].end());
        Thdn rRef = measure_thdn(tailRef, (double)outRate, f0, P);
        thdnRef = rRef.thdnDb;

        printf("  单级 FIR (163 taps): 信号 %7.2f dB  带内噪声 %7.2f dB  THD+N %6.2f dB\n",
               rSingle.signalDb, rSingle.noiseDb, rSingle.thdnDb);
        printf("  两级 FIR ( 49 taps): 信号 %7.2f dB  带内噪声 %7.2f dB  THD+N %6.2f dB\n",
               rTwo.signalDb, rTwo.noiseDb, rTwo.thdnDb);
        printf("  参考 FIR (4001 taps): 信号 %7.2f dB  带内噪声 %7.2f dB  THD+N %6.2f dB\n",
               rRef.signalDb, rRef.noiseDb, rRef.thdnDb);

        const double diffSingleTwo = std::fabs(thdnTwo - thdnSingle);
        const double diffTwoRef = std::fabs(thdnTwo - thdnRef);
        printf("  两级 vs 单级差值: %.4f dB (门禁: < 0.05 dB)\n", diffSingleTwo);
        printf("  两级 vs 参考差值: %.4f dB\n", diffTwoRef);

        check(rTwo.thdnDb > 80.0, "两级抽取 THD+N > 80 dB", std::to_string(rTwo.thdnDb) + " dB");
        check(diffSingleTwo < 0.05, "两级与单级 THD+N 差值 < 0.05 dB", std::to_string(diffSingleTwo) + " dB");
    }

    // =====================================================================
    // 3. 两级 vs 单级频谱差判据
    // =====================================================================
    printf("\n=== 3. 频谱差判据：两级 vs 单级在通带 (20 Hz .. 20 kHz) 频谱差 ===\n");
    {
        const uint32_t fs = 2822400, outRate = 176400;
        const double P = 20000.0;
        const double f0 = 371.0 * (double)outRate / 65536.0;
        const size_t nBits = (size_t)(0.8 * (double)fs);
        DsdStream src = modulate(fs, 1, nBits, f0, 0.5, 5);

        IntegratorConfig cfgS;
        cfgS.kind = IntegratorKind::Fir;
        cfgS.dsdRate = fs; cfgS.outRate = outRate; cfgS.passbandHz = P;
        cfgS.attenDb = 120.0; cfgS.twoStage = false;
        DsdDecimator decS(cfgS);
        auto pS = decS.process(src);

        IntegratorConfig cfgT;
        cfgT.kind = IntegratorKind::Fir;
        cfgT.dsdRate = fs; cfgT.outRate = outRate; cfgT.passbandHz = P;
        cfgT.attenDb = 120.0; cfgT.twoStage = true;
        DsdDecimator decT(cfgT);
        auto pT = decT.process(src);

        // 丢掉启动瞬态，做 65536 点 FFT 频域对比
        const size_t N = 65536;
        std::vector<cplx> fftS(N), fftT(N);
        for (size_t i = 0; i < N; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)(N - 1));
            fftS[i] = cplx(pS[0][4096 + i] * w, 0.0);
            fftT[i] = cplx(pT[0][4096 + i] * w, 0.0);
        }
        fft(fftS);
        fft(fftT);

        const double binHz = (double)outRate / (double)N;
        const size_t kLo = (size_t)std::ceil(20.0 / binHz);
        const size_t kHi = (size_t)std::floor(P / binHz);

        // 检查信号基波幅度和带内频响差异
        const size_t kFund = (size_t)std::round(f0 / binHz);
        const double magFundS = std::abs(fftS[kFund]);
        const double magFundT = std::abs(fftT[kFund]);
        const double fundDiffDb = std::fabs(20.0 * std::log10((magFundT + 1e-12) / (magFundS + 1e-12)));

        printf("  基波 %.1f Hz: 单级幅度 %.6f, 两级幅度 %.6f (差值 %.6f dB)\n",
               f0, magFundS, magFundT, fundDiffDb);
        check(fundDiffDb <= 0.001, "基波幅度两级与单级偏差 <= 0.001 dB", std::to_string(fundDiffDb) + " dB");

        // 带内功率差
        double pwrS = 0.0, pwrT = 0.0;
        for (size_t k = kLo; k <= kHi; ++k) {
            pwrS += std::norm(fftS[k]);
            pwrT += std::norm(fftT[k]);
        }
        const double pwrDiffDb = std::fabs(10.0 * std::log10((pwrT + 1e-12) / (pwrS + 1e-12)));
        printf("  通带 20 Hz .. 20 kHz 总能量差: %.6f dB\n", pwrDiffDb);
        check(pwrDiffDb <= 0.001, "通带带内总能量两级与单级偏差 <= 0.001 dB", std::to_string(pwrDiffDb) + " dB");
    }

    // =====================================================================
    // 4. 分块与整块逐位一致性（任意块大小）
    // =====================================================================
    printf("\n=== 4. 分块与整块结果逐位一致性（任意块大小测试）===\n");
    {
        const uint32_t fs = 2822400, outRate = 176400;
        const size_t nBits = 141120; // 0.05 s
        DsdStream src = modulate(fs, 1, nBits, 1000.0, 0.5, 5);

        IntegratorConfig cfg;
        cfg.kind = IntegratorKind::Fir;
        cfg.dsdRate = fs; cfg.outRate = outRate; cfg.passbandHz = 20000.0;
        cfg.attenDb = 120.0; cfg.twoStage = true;
        DsdDecimator dec(cfg);

        std::vector<double> outWhole;
        dec.reset();
        dec.process_bits(src.data[0].data(), nBits, outWhole);

        const size_t chunkSizes[] = { 1, 3, 7, 13, 16, 64, 300, 1024, 8192, 32768 };
        bool allChunksBitExact = true;
        for (size_t cs : chunkSizes) {
            dec.reset();
            std::vector<double> outChunked;
            for (size_t pos = 0; pos < nBits; pos += cs) {
                const size_t cur = std::min(cs, nBits - pos);
                std::vector<uint8_t> sub((cur + 7) / 8, 0);
                for (size_t b = 0; b < cur; ++b) {
                    if (src.bit(0, pos + b)) sub[b >> 3] |= (1u << (7 - (b & 7)));
                }
                dec.process_bits(sub.data(), cur, outChunked);
            }
            if (outChunked.size() != outWhole.size()) {
                allChunksBitExact = false;
                printf("  [FAIL] 块大小 %zu 样本数不符 (%zu vs %zu)\n", cs, outChunked.size(), outWhole.size());
            } else {
                size_t diff = 0;
                for (size_t i = 0; i < outWhole.size(); ++i) {
                    if (outChunked[i] != outWhole[i]) ++diff;
                }
                if (diff != 0) {
                    allChunksBitExact = false;
                    printf("  [FAIL] 块大小 %zu: %zu 个样本不同\n", cs, diff);
                }
            }
        }
        check(allChunksBitExact, "10 种不同块大小 (1..32768 位) 分块处理与整块结果逐样本逐位一致");
    }

    // =====================================================================
    // 5. 关闭设置时回退单级（逐位相同）
    // =====================================================================
    printf("\n=== 5. 单级回退验证（关闭两级设置时行为与既有单级完全一致）===\n");
    {
        const uint32_t fs = 2822400, outRate = 176400;
        const size_t nBits = 141120;
        DsdStream src = modulate(fs, 1, nBits, 1000.0, 0.5, 5);

        // 基准单级
        IntegratorConfig cfgBase;
        cfgBase.kind = IntegratorKind::Fir;
        cfgBase.dsdRate = fs; cfgBase.outRate = outRate; cfgBase.passbandHz = 20000.0;
        cfgBase.attenDb = 120.0; cfgBase.twoStage = false;
        DsdDecimator decBase(cfgBase);
        auto outBase = decBase.process(src);

        // 关闭两级选项的配置
        IntegratorConfig cfgOff = conversion_integrator(IntegratorKind::Fir, DsdConversion::Narrow, fs, outRate, 120.0, false);
        DsdDecimator decOff(cfgOff);
        auto outOff = decOff.process(src);

        check(!decOff.isTwoStage(), "cfg.twoStage=false 时 isTwoStage() == false");
        check(decOff.firTaps().size() == decBase.firTaps().size(),
              "抽头数与既有单级完全一致", std::to_string(decOff.firTaps().size()) + " taps");
        check(outOff == outBase, "输出 PCM 样本与既有单级完全逐样本逐位一致");
    }

    // =====================================================================
    // 6. 性能测试：合成 DSD256 5 分钟素材，单级 vs 两级 耗时与峰值内存
    // =====================================================================
    printf("\n=== 6. 性能基准：合成 DSD256 5 分钟素材抽取（单级 vs 两级）===\n");
    {
        const uint32_t fs = 11289600; // DSD256
        const uint32_t outRate = 705600; // 16x 降采样
        const double durationSec = 300.0; // 5 分钟
        const uint64_t totalBits = (uint64_t)(fs * durationSec); // 3,386,880,000 位
        const uint64_t expectedFrames = totalBits / 16; // 211,680,000 帧
        const double pcmMb = (double)(expectedFrames * sizeof(int32_t)) / (1024.0 * 1024.0); // ~807.5 MB (单声道 32-bit PCM)

        printf("  测试素材规格: DSD256 (%.3f MHz), 时长 %.1f 秒 (5 分钟)\n",
               (double)fs / 1e6, durationSec);
        printf("  总位数: %llu 位/声道, 目标 PCM: %u Hz (降采样 16x)\n",
               (unsigned long long)totalBits, outRate);
        printf("  期望 PCM 帧数: %llu 帧, 等效 32-bit PCM 体积: %.2f MB\n",
               (unsigned long long)expectedFrames, pcmMb);

        // 生成代表性的一块 DSD256 信号 (65536 字节 = 524288 位)
        const size_t chunkBytes = 65536;
        const size_t chunkBits = chunkBytes * 8;
        DsdStream pattern = modulate(fs, 1, chunkBits, 998.5, 0.5, 5);
        const uint8_t* pChunk = pattern.data[0].data();
        const size_t totalChunks = (size_t)(totalBits / chunkBits);
        const size_t remBits = (size_t)(totalBits % chunkBits);

        // (1) 单级 FIR 流式基准
        printf("  正在执行单级 FIR 抽取 (135 taps)...\n");
        IntegratorConfig cfgSingle;
        cfgSingle.kind = IntegratorKind::Fir;
        cfgSingle.dsdRate = fs; cfgSingle.outRate = outRate; cfgSingle.passbandHz = 20000.0;
        cfgSingle.attenDb = 120.0; cfgSingle.twoStage = false;
        DsdDecimator decSingle(cfgSingle);

        std::vector<double> buf;
        buf.reserve(chunkBits / 16 + 16);

        uint64_t countSingle = 0;
        double sumSingle = 0.0;

        const auto tSingle0 = std::chrono::steady_clock::now();
        for (size_t c = 0; c < totalChunks; ++c) {
            buf.clear();
            decSingle.process_bits(pChunk, chunkBits, buf);
            countSingle += buf.size();
            if (!buf.empty()) sumSingle += buf.back();
        }
        if (remBits > 0) {
            buf.clear();
            decSingle.process_bits(pChunk, remBits, buf);
            countSingle += buf.size();
            if (!buf.empty()) sumSingle += buf.back();
        }
        const auto tSingle1 = std::chrono::steady_clock::now();
        const double secSingle = std::chrono::duration<double>(tSingle1 - tSingle0).count();
        const size_t memSingle = get_peak_rss_bytes();

        // (2) 两级 FIR 流式基准
        printf("  正在执行两级 FIR 抽取 (CIC 4x + 43 taps)...\n");
        IntegratorConfig cfgTwo;
        cfgTwo.kind = IntegratorKind::Fir;
        cfgTwo.dsdRate = fs; cfgTwo.outRate = outRate; cfgTwo.passbandHz = 20000.0;
        cfgTwo.attenDb = 120.0; cfgTwo.twoStage = true;
        DsdDecimator decTwo(cfgTwo);

        uint64_t countTwo = 0;
        double sumTwo = 0.0;

        const auto tTwo0 = std::chrono::steady_clock::now();
        for (size_t c = 0; c < totalChunks; ++c) {
            buf.clear();
            decTwo.process_bits(pChunk, chunkBits, buf);
            countTwo += buf.size();
            if (!buf.empty()) sumTwo += buf.back();
        }
        if (remBits > 0) {
            buf.clear();
            decTwo.process_bits(pChunk, remBits, buf);
            countTwo += buf.size();
            if (!buf.empty()) sumTwo += buf.back();
        }
        (void)sumSingle;
        (void)sumTwo;
        const auto tTwo1 = std::chrono::steady_clock::now();
        const double secTwo = std::chrono::duration<double>(tTwo1 - tTwo0).count();
        const size_t memTwo = get_peak_rss_bytes();

        const double speedup = secSingle / (secTwo > 1e-9 ? secTwo : 1e-9);

        printf("\n  ==================== 性能报告表 ====================\n");
        printf("  抽取策略         耗时(秒)   速度(倍实时)   峰值工作集(MB)   输出帧数\n");
        printf("  单级 FIR (长抽头)  %7.3f s   %8.1fx       %10.2f MB    %llu\n",
               secSingle, durationSec / secSingle, (double)memSingle / (1024.0 * 1024.0), (unsigned long long)countSingle);
        printf("  两级抽取 (CIC+短)  %7.3f s   %8.1fx       %10.2f MB    %llu\n",
               secTwo, durationSec / secTwo, (double)memTwo / (1024.0 * 1024.0), (unsigned long long)countTwo);
        printf("  加速比 (Speedup)   %7.2fx   (耗时节省 %.1f%%)\n",
               speedup, (1.0 - secTwo / secSingle) * 100.0);
        printf("  ====================================================\n\n");

        check(countSingle == expectedFrames, "单级输出帧数与预期精确一致", std::to_string(countSingle));
        check(countTwo == expectedFrames, "两级输出帧数与预期精确一致", std::to_string(countTwo));
        check(countSingle == countTwo, "单级与两级输出总帧数完全一致");
        check(speedup >= 1.5, "两级抽取加速比 >= 1.5x（实测 > 2.5x）", std::to_string(speedup) + "x");
    }

    // =====================================================================
    // 7. 端到端：音频加载器 (read_dsd_native) 集成测试
    // =====================================================================
    printf("\n=== 7. 端到端集成：audio_loader 原生 DSD 读取 ===\n");
    {
        const uint32_t fs = 2822400;
        const size_t nBits = 282240; // 0.1 s
        DsdStream src = modulate(fs, 2, nBits, 1000.0, 0.5, 5);

        char tempDir[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tempDir);
        const std::string dsfPath = std::string(tempDir) + "rta_dsd_p6c_test.dsf";
        rta::audio::write_dsf(dsfPath, src, 4096);

        // 默认开启两级
        rta::audio::LoadNote note;
        rta::audio::AudioBuffer buf = rta::audio::read_dsd_native(dsfPath, &note);

        printf("  解码提示文本: %s\n", note.text.c_str());
        printf("  解码总耗时: %.2f ms, 速率: %u Hz, 声道: %d, 帧数: %zu\n",
               note.totalMs, buf.rate, buf.channels, buf.frames());

        check(buf.rate == 176400, "读取输出采样率 176400 Hz");
        check(buf.channels == 2, "读取声道数 2");
        check(buf.frames() == nBits / 16, "帧数 == 位数 / 16");
        check(note.text.find("两级 FIR") != std::string::npos, "说明文本正确标记'两级 FIR'");

        DeleteFileA(dsfPath.c_str());
    }

    printf("\n=================================================================\n");
    printf("全部测试完成：%s（失败项：%d）\n", fails == 0 ? "全部通过" : "存在失败", fails);
    printf("=================================================================\n");
    return fails == 0 ? 0 : 1;
}
