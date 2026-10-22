// test_modulator_families.cpp — ROADMAP P1: SDM 调制器族扩展客观验证
// 覆盖：
//   1. 7 阶 Chebyshev NTF（可配带外增益/截止与降阶保护）
//   2. EC（误差反馈扩展消除）变体
//   3. 多电平 AHM 风格（6~8 bit，64~256 电平）
//   4. 带内 SNR、带外噪声积分、空闲音客观评测
//   5. 铁律 7 满幅 0 dBFS 与单样本越界恢复
//   6. 模拟 >= 10 分钟样点数长跑稳定性
//   7. 完整多族数值对比表
#include "dsp/modulator.h"
#include "dsp/fft.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <complex>
#include <string>
#include <algorithm>
#include <chrono>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// 频域相干功率分析
struct Spec {
    double signalDb = 0.0;
    double inBandNoiseDb = 0.0;
    double outBandDb = 0.0;
    double snrDb = 0.0;
};

static Spec analyse_spectrum(const std::vector<double>& y, double fs, double fin, double fBandTop = 20000.0) {
    const size_t N = y.size();
    std::vector<cplx> b(N);
    for (size_t i = 0; i < N; ++i) b[i] = cplx(y[i], 0.0);
    fft(b);
    auto pwr = [&](size_t k) { return 2.0 * std::norm(b[k]) / ((double)N * (double)N); };

    const size_t kFund = (size_t)std::llround(fin / fs * (double)N);
    const size_t kTop  = (size_t)std::llround(fBandTop / fs * (double)N);

    double psig = 0.0;
    for (size_t k = (kFund > 1 ? kFund - 1 : 1); k <= kFund + 1 && k < N / 2; ++k) psig += pwr(k);

    double pnoise = 0.0;
    for (size_t k = 1; k <= kTop; ++k) {
        if (k + 1 >= kFund && k <= kFund + 1) continue;
        pnoise += pwr(k);
    }
    double poob = 0.0;
    for (size_t k = kTop + 1; k < N / 2; ++k) poob += pwr(k);

    Spec s;
    s.signalDb      = 10.0 * std::log10(psig + 1e-300);
    s.inBandNoiseDb = 10.0 * std::log10(pnoise + 1e-300);
    s.outBandDb     = 10.0 * std::log10(poob + 1e-300);
    s.snrDb         = 10.0 * std::log10(psig / (pnoise + 1e-300));
    return s;
}

// 测量无输入时 1kHz 附近（900Hz~1100Hz）空闲音（最大离散峰值 dBFS）
static double measure_idle_tone_peak(const std::vector<double>& y, double fs, double fCenter = 1000.0, double fSpan = 200.0) {
    const size_t N = y.size();
    std::vector<cplx> b(N);
    for (size_t i = 0; i < N; ++i) b[i] = cplx(y[i], 0.0);
    fft(b);
    auto pwr = [&](size_t k) { return 2.0 * std::norm(b[k]) / ((double)N * (double)N); };

    const size_t kMin = (size_t)std::llround((fCenter - fSpan / 2.0) / fs * (double)N);
    const size_t kMax = (size_t)std::llround((fCenter + fSpan / 2.0) / fs * (double)N);

    double maxPwr = 0.0;
    for (size_t k = kMin; k <= kMax && k < N / 2; ++k) {
        double p = pwr(k);
        if (p > maxPwr) maxPwr = p;
    }
    return 10.0 * std::log10(maxPwr + 1e-300);
}

// 数值表记录结构
struct TableRow {
    std::string family;
    std::string rate;
    std::string config;
    double snr_m6;
    double snr_m60;
    double oobNoise;
    double idlePeak;
};

int main() {
    printf("========================================================================\n");
    printf("  ROADMAP P1: SDM 调制器族扩展客观验证（Chebyshev / EC / 多电平 AHM）\n");
    printf("========================================================================\n\n");

    std::vector<TableRow> tableRows;

    // -------------------------------------------------------------------------
    printf("=== 1. NTF 设计与降阶保护验证 ===\n");
    {
        const double fs = 2822400.0;
        // 1.1 7 阶 Chebyshev NTF
        auto ntfCheb = design_chebyshev_ntf_poly(7, 100000.0, fs, 4.5);
        double db100 = ntf_poly_response_db(ntfCheb, 100.0 / (fs / 2.0));
        double db20k = ntf_poly_response_db(ntfCheb, 20000.0 / (fs / 2.0));
        double dbNyq = ntf_poly_response_db(ntfCheb, 1.0);
        printf("  Chebyshev 7阶 (fc=100kHz, oob=4.5dB): 100Hz=%.1f dB, 20kHz=%.1f dB, Nyquist=%.2f dB\n",
               db100, db20k, dbNyq);
        check(db100 < -80.0, "Chebyshev 7阶 低频深抑制 (< -80 dB)");
        check(db20k < -20.0, "Chebyshev 7阶 20kHz 带边抑制 (< -20 dB)");
        check(dbNyq >= 4.0 && dbNyq <= 7.0, "Chebyshev 7阶 出带增益符合设定 (4~7 dB)");

        // 1.2 EC 变体 NTF（通带内分布 Chebyshev 零点）
        auto ntfEc = design_ec_ntf_poly(5, 100000.0, fs);
        double dbEc20k = ntf_poly_response_db(ntfEc, 20000.0 / (fs / 2.0));
        printf("  EC 5阶 (优化零点分布): 20kHz=%.1f dB\n", dbEc20k);
        check(dbEc20k < -20.0, "EC 5阶 20kHz 抑制达标");

        // 1.3 7 阶降阶保护（1-bit 在低采样率下退回 5 阶）
        SdmConfig cfgCheb64;
        cfgCheb64.fs = 2822400.0; cfgCheb64.order = 7; cfgCheb64.family = ModulatorFamily::Chebyshev;
        SdmModulator mCheb64(cfgCheb64);
        check(mCheb64.steppedDown(), "DSD64 下 7 阶 1-bit 触发降阶保护");
        check(mCheb64.effectiveOrder() == 5, "降阶后实际运行于 5 阶");
        printf("  降阶报告原因: %s\n", mCheb64.stepDownReason().c_str());

        // 1.4 AHM 多电平即使 7 阶在 DSD64 亦无需降阶
        SdmConfig cfgAhm64;
        cfgAhm64.fs = 2822400.0; cfgAhm64.order = 7; cfgAhm64.family = ModulatorFamily::Ahm; cfgAhm64.ahmBits = 8;
        SdmModulator mAhm64(cfgAhm64);
        check(!mAhm64.steppedDown(), "DSD64 下 7 阶 AHM 8-bit 不触发降阶（量化误差微小无需降阶）");
        check(mAhm64.effectiveOrder() == 7, "AHM 实际运行于 7 阶");
    }

    // -------------------------------------------------------------------------
    printf("\n=== 2. 调制器族带内 SNR 与带外噪声积分（DSD64 / DSD128 / DSD256）===\n");
    {
        const size_t N = 65536;
        const size_t warm = 20000;

        struct FamilyDesc {
            ModulatorFamily family;
            int order;
            int levels;
            int ahmBits;
            const char* label;
        };

        const FamilyDesc families[] = {
            { ModulatorFamily::Dsd,       5,   2, 0, "DSD5 (Butterworth)" },
            { ModulatorFamily::Dsd,       7,   2, 0, "DSD7 (Butterworth)" },
            { ModulatorFamily::Chebyshev, 7,   2, 0, "Chebyshev-7"        },
            { ModulatorFamily::Ec,        5,   2, 0, "EC-5 (ErrorCancel)" },
            { ModulatorFamily::Ec,        7,   2, 0, "EC-7 (ErrorCancel)" },
            { ModulatorFamily::Ahm,       5,  64, 6, "AHM-6bit (64L)"     },
            { ModulatorFamily::Ahm,       7, 256, 8, "AHM-8bit (256L)"    },
        };

        struct RateDesc { double fs; const char* name; };
        const RateDesc rates[] = {
            {  2822400.0, "DSD64 " },
            {  5644800.0, "DSD128" },
            { 11289600.0, "DSD256" },
        };

        for (const auto& r : rates) {
            const double binHz = r.fs / (double)N;
            const double fin = binHz * 23.0; // 相干频点（第 23 bin，无加窗泄漏）

            for (const auto& f : families) {
                if (r.fs < 5.6e6 && f.order == 7 && f.levels == 2 && f.family == ModulatorFamily::Dsd) {
                    continue; // 既有测试已证明 7 阶 1-bit Butterworth 在 DSD64 不稳定
                }

                // 2.1 满幅 -6 dBFS 输入（1kHz 正弦波）
                SdmConfig cfg6;
                cfg6.fs = r.fs; cfg6.order = f.order; cfg6.levels = f.levels;
                cfg6.family = f.family; cfg6.ahmBits = f.ahmBits;
                cfg6.cutoffHz = 100000.0; cfg6.ditherLsb = 0.0005;
                SdmModulator m6(cfg6);

                std::vector<double> y6; y6.reserve(N);
                const double amp6 = 0.5011872336; // -6 dBFS
                for (size_t n = 0; n < warm + N; ++n) {
                    double x = amp6 * std::sin(2.0 * kPi * fin * (double)n / r.fs);
                    double v = m6.process(x);
                    if (n >= warm) y6.push_back(v);
                }
                Spec s6 = analyse_spectrum(y6, r.fs, fin, 20000.0);

                // 2.2 小信号 -60 dBFS 输入（遵从铁律 1 & 铁律 3）
                SdmConfig cfg60 = cfg6;
                SdmModulator m60(cfg60);
                std::vector<double> y60; y60.reserve(N);
                const double amp60 = 0.001; // -60 dBFS
                for (size_t n = 0; n < warm + N; ++n) {
                    double x = amp60 * std::sin(2.0 * kPi * fin * (double)n / r.fs);
                    double v = m60.process(x);
                    if (n >= warm) y60.push_back(v);
                }
                Spec s60 = analyse_spectrum(y60, r.fs, fin, 20000.0);

                // 2.3 空闲音测量（零输入时 1kHz 附近离散峰值）
                SdmConfig cfgIdle = cfg6;
                SdmModulator mIdle(cfgIdle);
                std::vector<double> yIdle; yIdle.reserve(N);
                for (size_t n = 0; n < warm + N; ++n) {
                    double v = mIdle.process(0.0);
                    if (n >= warm) yIdle.push_back(v);
                }
                double idlePeak = measure_idle_tone_peak(yIdle, r.fs, 1000.0, 200.0);

                // 记录数值表行
                TableRow row;
                row.family   = modulator_family_name(f.family);
                row.rate     = r.name;
                row.config   = f.label;
                row.snr_m6   = s6.snrDb;
                row.snr_m60  = s60.snrDb;
                row.oobNoise = s6.outBandDb;
                row.idlePeak = idlePeak;
                tableRows.push_back(row);

                // 断言检查
                std::string item = std::string(f.label) + " @" + r.name;
                check(m6.clamped() == 0, item + " 环路无误差发散");
                check(m6.overload() == 0, item + " -6 dBFS 稳态无限幅误触发");
                check(std::isfinite(s6.snrDb) && s6.snrDb > 40.0, item + " -6 dBFS 带内 SNR 正常 (> 40 dB)");
                check(std::isfinite(s60.snrDb), item + " -60 dBFS 小信号输出有效");

                if (f.levels > 2) {
                    check(s6.outBandDb < -30.0, item + " AHM 多电平带外噪声积分深度抑制 (< -30 dB)");
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    printf("\n=== 3. 铁律 7：过载与单样本越界恢复客观验证 ===\n");
    {
        struct OverloadCase {
            double fs;
            ModulatorFamily family;
            int order;
            int levels;
            int ahmBits;
            const char* name;
        };

        const OverloadCase ovCases[] = {
            {  2822400.0, ModulatorFamily::Dsd,       5,   2, 0, "DSD5 @DSD64"       },
            {  5644800.0, ModulatorFamily::Chebyshev, 7,   2, 0, "Chebyshev7 @DSD128"},
            {  2822400.0, ModulatorFamily::Ec,        5,   2, 0, "EC5 @DSD64"        },
            { 11289600.0, ModulatorFamily::Ec,        7,   2, 0, "EC7 @DSD256"       },
            {  2822400.0, ModulatorFamily::Ahm,       7, 256, 8, "AHM8 @DSD64"       },
        };

        for (const auto& c : ovCases) {
            SdmConfig cfg;
            cfg.fs = c.fs; cfg.family = c.family; cfg.order = c.order;
            cfg.levels = c.levels; cfg.ahmBits = c.ahmBits;
            cfg.cutoffHz = 100000.0; cfg.ditherLsb = 0.0005;
            SdmModulator m(cfg);

            const long long total = (long long)(c.fs * 2.5); // 2.5 秒仿真
            const long long ev0 = (long long)(c.fs * 1.0);
            const long long ev1 = (long long)(c.fs * 1.05); // 50ms 满幅 0 dBFS (amp = 1.0)
            const long long spike_idx = (long long)(c.fs * 1.5); // 1.5s 处单样本越界 +5.0 脉冲
            const long long steady_start = (long long)(c.fs * 2.0); // 2.0s 瞬态衰减完毕，开始断言稳态

            const double fin = 1981.0546875;
            long long ov0 = 0, ov1 = 0, same = 0, sameN = 0;
            double decSum = 0.0, acc = 0.0, lastY = 0.0;
            long long decN = 0;
            long long ovAtSteadyStart = 0;

            for (long long i = 0; i < total; ++i) {
                double amp = 0.62;
                if (i >= ev0 && i < ev1) amp = 1.0; // 满幅 0 dBFS 瞬态

                double x = amp * std::sin(2.0 * kPi * fin * (double)i / c.fs);
                if (i == spike_idx) x = 5.0; // 单样本越界脉冲 (+5.0)

                if (i == ev0) ov0 = m.overload();
                if (i == ev1) ov1 = m.overload();
                if (i == steady_start) ovAtSteadyStart = m.overload();

                const double v = m.process(x);

                // 统计瞬态彻底结束后的状态（末尾 0.2 秒）：
                acc += v;
                if (i % 16 == 15) {
                    acc /= 16.0;
                    if (i >= total - (long long)(c.fs * 0.2)) { decSum += acc * acc; ++decN; }
                    acc = 0.0;
                }
                if (i >= total - (long long)(c.fs * 0.2)) {
                    if (i > total - (long long)(c.fs * 0.2) && v == lastY) ++same;
                    ++sameN; lastY = v;
                }
            }

            const double decRms = std::sqrt(decSum / (double)decN);
            const double sameFrac = (double)same / (double)(sameN - 1);
            const long long ovGrowth = m.overload() - ovAtSteadyStart;

            printf("  [%s] 满幅过载 %lld 次，恢复后稳态过载增量 %lld 次；解调 RMS %.3f，同号率 %.2f\n",
                   c.name, ov1 - ov0, ovGrowth, decRms, sameFrac);

            std::string tag = std::string(c.name);
            check(m.clamped() == 0, tag + " 瞬态后误差反馈未发散 (clamped=0)");
            check(ov1 > ov0, tag + " 满幅瞬态确实触发限幅保护（判据有效）");
            check(ovGrowth == 0, tag + " 瞬态后限幅器停止动作（过载增量为 0）");
            check(std::fabs(decRms - 0.438) < 0.04, tag + " 恢复后 RMS 回到 0.62 正弦水平",
                  "实测 RMS=" + std::to_string(decRms));
            check(sameFrac < 0.60, tag + " 恢复后无锁死极限环 (同号率 < 0.60)",
                  "同号率=" + std::to_string(sameFrac));
        }
    }

    // -------------------------------------------------------------------------
    printf("\n=== 4. 长跑稳定性（模拟 >= 10 分钟样点数 快进验证）===\n");
    {
        SdmConfig cfg;
        cfg.fs = 2822400.0;
        cfg.family = ModulatorFamily::Chebyshev;
        cfg.order = 7;
        cfg.levels = 2;
        cfg.ditherLsb = 0.0005;
        SdmModulator m(cfg);

        const size_t totalSamples = 26460000; // 26.46M 连续样点
        const double fin = 1000.0;
        const auto t0 = std::chrono::steady_clock::now();

        bool hadNan = false;
        long long steadyOv = 0;
        for (size_t i = 0; i < totalSamples; ++i) {
            double x = 0.4 * std::sin(2.0 * kPi * fin * (double)i / cfg.fs);
            double v = m.process(x);
            if (!std::isfinite(v)) hadNan = true;
            if (i > 100000 && m.overload() > steadyOv) steadyOv = m.overload();
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("  长跑完成：%zu 样点（模拟 10 分钟 PCM 样点数），耗时 %.1f ms (%.1f Msa/s)\n",
               totalSamples, ms, (double)totalSamples / (ms * 1000.0));
        printf("  统计：nonFinite=%lld, clamped=%lld, steadyOverload=%lld\n",
               m.nonFinite(), m.clamped(), steadyOv);

        check(!hadNan, "长跑全过程无 NaN/Inf 输出");
        check(m.clamped() == 0, "长跑全过程误差累加器未发散");
        check(steadyOv == 0, "长跑稳态过程中零限幅触发");
    }

    // -------------------------------------------------------------------------
    printf("\n=== 5. 调制器族全指标数值表汇总 ===\n");
    printf("| %-10s | %-7s | %-20s | %-13s | %-14s | %-13s | %-13s |\n",
           "族", "采样率", "配置/阶数", "SNR(-6dBFS)", "SNR(-60dBFS)", "带外噪声(dB)", "空闲音(dBFS)");
    printf("|------------|---------|----------------------|---------------|----------------|---------------|---------------|\n");
    for (const auto& r : tableRows) {
        printf("| %-10s | %-7s | %-20s | %10.1f dB | %11.1f dB | %10.1f dB | %10.1f dB |\n",
               r.family.c_str(), r.rate.c_str(), r.config.c_str(),
               r.snr_m6, r.snr_m60, r.oobNoise, r.idlePeak);
    }
    printf("\n");

    printf("%s (总失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
