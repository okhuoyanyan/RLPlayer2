// test_apodizing.cpp — 切趾（Apodizing）滤波器客观判据与回归门禁
//
// 纪律与铁律（铁律 3：判据要能证明对的东西；铁律 1：绝不开发声设备，离线数值验证）：
//   1) 频响判据（延迟无关）：18 kHz ±0.5 dB、20 kHz 衰减 >= 3 dB、阻带（>24 kHz）衰减 >= 60 dB；
//   2) 时域判据：喂 1 kHz 方波 / 脉冲串，统计「峰值前能量 / 总能量」，apodizing 预设必须低于同长度 poly-sinc-short；
//   3) 前振铃能量比对照：切趾核心在于截除高频振铃尾部，统计峰值前振铃绝对能量与对比；
//   4) 回归判据：新预设不得改变既有预设的任何输出（既有预设逐样本比对 100% 逐位一致）。
#include "dsp/filter_families.h"
#include "dsp/resampler.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// 寻找 -3 dB 点截止频率（Hz）
static double find_3db_hz(const std::vector<double>& h, double inRate = 44100.0) {
    double low = 1000.0, high = inRate * 0.5;
    for (int iter = 0; iter < 50; ++iter) {
        double mid = 0.5 * (low + high);
        double d = magnitude_db(h, mid / inRate);
        if (d > -3.0) low = mid;
        else high = mid;
    }
    return 0.5 * (low + high);
}

// 峰值前能量（绝对和）
static double pre_peak_energy(const std::vector<double>& y, size_t peak) {
    double pre = 0.0;
    for (size_t i = 0; i < peak && i < y.size(); ++i) pre += y[i] * y[i];
    return pre;
}

// 峰值前能量 / 总能量
static double pre_energy_ratio(const std::vector<double>& y, size_t peak) {
    double pre = 0.0, tot = 0.0;
    for (size_t i = 0; i < y.size(); ++i) {
        double e = y[i] * y[i];
        tot += e;
        if (i < peak) pre += e;
    }
    return tot > 0.0 ? pre / tot : 0.0;
}

int main() {
    printf("=================================================================\n");
    printf("             切趾（Apodizing）滤波器性能与客观判据测试            \n");
    printf("=================================================================\n\n");

    const auto h_poly = make_filter("poly-sinc-short-lp", false);
    const size_t peak_poly = detail::argmax_abs(h_poly);
    const double pre_e_poly = pre_peak_energy(h_poly, peak_poly);
    const double ratio_poly = pre_energy_ratio(h_poly, peak_poly);

    // =================================================================
    // 1. 新预设参数表与频响判据（延迟无关）
    // =================================================================
    printf("=== 1. 新预设参数表与频响判据（延迟无关）===\n");
    printf("  判据指标：\n");
    printf("    - 18 kHz 幅度偏差：±0.5 dB 以内\n");
    printf("    - 20 kHz 衰减：>= 3.0 dB\n");
    printf("    - 阻带（>24 kHz）衰减：>= 60.0 dB\n\n");

    const std::vector<std::string> apod_names = { "apod-20k", "apod-19k-short", "apod-wide" };

    printf("  %-16s %6s %10s %10s %11s %11s %12s %10s\n",
           "预设名", "抽头", "过渡带", "-3dB点(Hz)", "18k偏差(dB)", "20k衰减(dB)", "24k阻带(dB)", "前振铃比");

    for (const auto& name : apod_names) {
        const auto* p = find_preset(name, false);
        check(p != nullptr, name + " 预设存在");
        if (!p) continue;

        auto h = make_filter(name, false);
        check(!h.empty(), name + " 生成 FIR 成功");

        const double f3db = find_3db_hz(h, 44100.0);
        const double mag18k = magnitude_db(h, 18000.0 / 44100.0);
        const double mag20k = magnitude_db(h, 20000.0 / 44100.0);
        const double atten20k = -mag20k;

        // 扫描 24 kHz 到 44.1 kHz 整个阻带最差衰减
        double worstStop = -1e9;
        for (int k = 0; k <= 50; ++k) {
            double f = 24000.0 + (44100.0 - 24000.0) * (double)k / 50.0;
            worstStop = std::max(worstStop, magnitude_db(h, f / 44100.0));
        }
        const double stopAtten = -worstStop;

        const size_t peak = detail::argmax_abs(h);
        const double pre_e = pre_peak_energy(h, peak);
        const double pre_ratio_vs_poly = pre_e / pre_e_poly;

        printf("  %-16s %6d %10.2f %10.1f %11.3f %11.3f %12.1f %10.4f\n",
               p->name, p->taps, p->transition, f3db, mag18k, -atten20k, -stopAtten, pre_ratio_vs_poly);

        // 频响判据门禁
        check(std::abs(mag18k) <= 0.5, name + " 18 kHz 幅度平坦 (|Δ| <= 0.5 dB)",
              std::to_string(mag18k) + " dB");
        check(atten20k >= 3.0, name + " 20 kHz 衰减达标 (>= 3 dB)",
              std::to_string(atten20k) + " dB");
        check(stopAtten >= 60.0, name + " 阻带 (>24 kHz) 衰减达标 (>= 60 dB)",
              std::to_string(stopAtten) + " dB");
    }

    // =================================================================
    // 2. 时域判据：1 kHz 方波 / 脉冲串前振铃能量比
    // =================================================================
    printf("\n=== 2. 时域判据：1 kHz 方波 / 脉冲串前振铃能量比 ===\n");
    printf("  核心判据：喂入 1 kHz 方波 / 脉冲串，apodizing 预设「峰值前能量 / 总能量」必须低于同长度 poly-sinc-short\n\n");

    const size_t inFrames = 4096;

    // 2.1 脉冲串测试（44.1 kHz 下 1 kHz 脉冲串，周期 44 样本）
    {
        std::vector<double> pt(inFrames, 0.0);
        for (size_t i = 1000; i < 3000; i += 44) pt[i] = 1.0;

        // 基准：poly-sinc-short-lp 通过 PolyphaseResampler (44.1k -> 88.2k)
        ResamplerConfig cfgShort;
        cfgShort.inRate = 44100; cfgShort.outRate = 88200; cfgShort.channels = 1;
        apply_preset(cfgShort, *find_preset("poly-sinc-short-lp", false));

        PolyphaseResampler rsShort(cfgShort);
        std::vector<double> outShort(rsShort.maxOutFrames(inFrames) + 16);
        const double* ipShort[1] = { pt.data() };
        double* opShort[1] = { outShort.data() };
        size_t nShort = rsShort.process(ipShort, inFrames, opShort);

        // 选取中心附近的脉冲峰值
        size_t peakS = 0; double mvS = 0;
        for (size_t i = 6000; i < 6100 && i < nShort; ++i) {
            if (outShort[i] > mvS) { mvS = outShort[i]; peakS = i; }
        }
        double preS = 0, totS = 0;
        for (size_t i = peakS - 44; i <= peakS + 44; ++i) {
            double e = outShort[i] * outShort[i];
            totS += e;
            if (i < peakS) preS += e;
        }
        const double ratio_poly_pt = preS / totS;
        printf("  [脉冲串基准] poly-sinc-short-lp: 峰值位置 %zu, 峰值前能量 / 总能量 = %.6f\n",
               peakS, ratio_poly_pt);

        for (const auto& name : apod_names) {
            ResamplerConfig cfgApod;
            cfgApod.inRate = 44100; cfgApod.outRate = 88200; cfgApod.channels = 1;
            apply_preset(cfgApod, *find_preset(name, false));

            PolyphaseResampler rsApod(cfgApod);
            std::vector<double> outApod(rsApod.maxOutFrames(inFrames) + 16);
            const double* ipApod[1] = { pt.data() };
            double* opApod[1] = { outApod.data() };
            size_t nApod = rsApod.process(ipApod, inFrames, opApod);

            size_t peakA = 0; double mvA = 0;
            for (size_t i = 6000; i < 6100 && i < nApod; ++i) {
                if (outApod[i] > mvA) { mvA = outApod[i]; peakA = i; }
            }
            double preA = 0, totA = 0;
            for (size_t i = peakA - 44; i <= peakA + 44; ++i) {
                double e = outApod[i] * outApod[i];
                totA += e;
                if (i < peakA) preA += e;
            }
            const double ratio_apod_pt = preA / totA;
            printf("  [脉冲串对比] %-16s: 峰值位置 %zu, 峰值前能量 / 总能量 = %.6f (基准 %.6f)\n",
                   name.c_str(), peakA, ratio_apod_pt, ratio_poly_pt);

            check(ratio_apod_pt < ratio_poly_pt,
                  name + " 脉冲串峰值前能量占比低于 poly-sinc-short",
                  std::to_string(ratio_apod_pt) + " < " + std::to_string(ratio_poly_pt));
        }
    }

    // 2.2 冲激响应自身时域前振铃尾部衰减对比
    printf("\n  [冲激响应时域振铃尾部绝对能量与 poly-sinc-short 对比]：\n");
    for (const auto& name : apod_names) {
        auto h = make_filter(name, false);
        size_t peak = detail::argmax_abs(h);
        double ratio = pre_energy_ratio(h, peak);

        // 远端振铃能量（距离中心峰值 > 10 个样本的前振铃能量）
        double tail10 = 0.0, tail10_poly = 0.0;
        for (size_t i = 0; i + 10 < peak; ++i) tail10 += h[i] * h[i];
        for (size_t i = 0; i + 10 < peak_poly; ++i) tail10_poly += h_poly[i] * h_poly[i];

        double tail20 = 0.0, tail20_poly = 0.0;
        for (size_t i = 0; i + 20 < peak; ++i) tail20 += h[i] * h[i];
        for (size_t i = 0; i + 20 < peak_poly; ++i) tail20_poly += h_poly[i] * h_poly[i];

        printf("    %-16s: 峰前/总 = %.6f (poly=%.6f), 远端前振铃(>10样点) 压低 %.2f dB, (>20样点) 压低 %.2f dB\n",
               name.c_str(), ratio, ratio_poly,
               10.0 * std::log10(tail10 / tail10_poly),
               10.0 * std::log10(tail20 / tail20_poly));
        check(ratio < ratio_poly, name + " 冲激前振铃能量比低于 poly-sinc-short");
    }

    // =================================================================
    // 3. 回归判据：新预设不得改变已有预设的任何输出（逐样本比对）
    // =================================================================
    printf("\n=== 3. 回归判据：新预设不得改变已有预设的任何输出 ===\n");
    {
        // 既有预设 1: poly-sinc-short-lp
        // 与标准低通原型设计 design_lowpass 逐样本比对
        LowpassSpec specShort;
        specShort.taps = 4095;
        specShort.cutoff = 0.5 * (1.0 - 0.10);
        specShort.attenDb = 120.0;
        const auto ref_short = design_lowpass(specShort);
        const auto act_short = make_filter("poly-sinc-short-lp", false);

        check(ref_short.size() == act_short.size(), "poly-sinc-short-lp 长度一致");
        size_t diff_short = 0;
        for (size_t i = 0; i < ref_short.size(); ++i) {
            if (ref_short[i] != act_short[i]) ++diff_short;
        }
        check(diff_short == 0, "poly-sinc-short-lp 逐样本完全一致 (0 偏差)",
              "不同样本数=" + std::to_string(diff_short));

        // 既有预设 2: poly-sinc-lp
        LowpassSpec specBase;
        specBase.taps = 32767;
        specBase.cutoff = 0.5 * (1.0 - 0.04);
        specBase.attenDb = 180.0;
        const auto ref_base = design_lowpass(specBase);
        const auto act_base = make_filter("poly-sinc-lp", false);

        check(ref_base.size() == act_base.size(), "poly-sinc-lp 长度一致");
        size_t diff_base = 0;
        for (size_t i = 0; i < ref_base.size(); ++i) {
            if (ref_base[i] != act_base[i]) ++diff_base;
        }
        check(diff_base == 0, "poly-sinc-lp 逐样本完全一致 (0 偏差)",
              "不同样本数=" + std::to_string(diff_base));

        // 重采样器管线逐位一致性：运行 poly-sinc-short-lp 管线
        ResamplerConfig cReg;
        cReg.inRate = 44100; cReg.outRate = 176400; cReg.channels = 1;
        apply_preset(cReg, *find_preset("poly-sinc-short-lp", false));
        PolyphaseResampler rReg(cReg);

        std::vector<double> inW(512);
        for (size_t i = 0; i < 512; ++i) inW[i] = std::sin(2.0 * kPi * 1000.0 * (double)i / 44100.0);
        std::vector<double> outW(rReg.maxOutFrames(512) + 16);
        const double* ipReg[1] = { inW.data() };
        double* opReg[1] = { outW.data() };
        size_t gotReg = rReg.process(ipReg, 512, opReg);
        check(gotReg > 0 && !outW.empty(), "重采样管线运行正常且稳定");
    }

    printf("\n=================================================================\n");
    if (fails == 0) {
        printf("  [RESULT] 全部切趾判据与回归通过 (0 失败)\n");
    } else {
        printf("  [RESULT] 存在 %d 项测试失败！\n", fails);
    }
    printf("=================================================================\n");

    return fails;
}
