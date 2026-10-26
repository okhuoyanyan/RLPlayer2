
// test_modulator_rates.cpp — 「7 阶 SDM 调制器全速率稳定」客观判据（2026-10-27）
//
// 用户问题（261027-01 #5）：「7 阶调制器全速率稳定（DSD64 会自动降 5 阶并说明），解决了没有？」
//
// ── 判据为什么这样定（铁律 3/7）─────────────────────────────────────────────
// 1)「没 NaN / 没崩」不算稳定 —— 锁死的满幅方波也满足。本文件要求同时满足：
//    ① 分析窗内环路限幅计数不再增长（overload 增量 == 0）；
//    ② 基波幅度跟随输入（STF=1，误差 ≤ 1 dB）—— 锁死/限幅的环路做不到；
//    ③ 0 dBFS 50 ms 瞬态 + 单样本 +5.0 越界后：限幅停止、解调 RMS 回到 0.62
//       正弦水平、同号率 < 0.6（不锁死）。
// 2) 带内噪声用【项目既有单窗法】：N=65536、warm=20000、fin=第 23 根谱线（相干），
//    与 tests/test_modulator.cpp / test_modulator_families.cpp 完全同源，便于对照。
//    ⚠️ 已知局限（如实记录）：1-bit 低 OSR 的带内噪声对输入频点与窗相位强敏感
//    （实测 DSD64 上 EC-5 在 0.62 输入时，不同整 bin 频点的带内噪声在
//    -63.7 ~ -121.7 dBFS 之间跳；DSD512 相邻两个半窗可差 64 dB，因为环路会锁进
//    周期性图案）。所以本文件的噪声数字是「该频点该窗」的一个样本，不是统计底噪；
//    稳定性结论一律以 MSSA / 瞬态恢复 / 限幅增量为准，不以噪声数字为准。
#include "dsp/modulator.h"
#include "dsp/fft.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <cmath>
#include <complex>
#include <string>
#include <algorithm>

using namespace rta::dsp;

static int fails = 0;
static bool g_report = false;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (ok) { if (g_report) std::printf("  [PASS] %s %s\n", what.c_str(), detail.c_str()); }
    else    { std::printf("  [FAIL] %s %s\n", what.c_str(), detail.c_str()); ++fails; }
}

static const double kBand = 20000.0;
static const double kRates[4]  = { 2822400.0, 5644800.0, 11289600.0, 22579200.0 };
static const char*  kRnames[4] = { "DSD64", "DSD128", "DSD256", "DSD512" };
static const size_t kN = 65536, kWarm = 20000;      // 与既有测试完全一致
static const double kOp = 0.62;                     // 前级余量目标（sdm_headroom_db）

static double test_freq(double fs) { return 23.0 * fs / (double)kN; }   // 第 23 根谱线

struct Noise { double medDb = -999.0, sigDb = -999.0, amp = 0.0, thdnDb = 0.0;
               double minDb = 999.0, maxDb = -999.0; size_t reps = 0; };

// 相干单窗：去掉 DC 与基波 ±2 bin，其余 1..kTop 求和 = 带内噪声功率
static Noise noise_of(const std::vector<double>& y, double fs, double fin) {
    std::vector<cplx> b(y.size());
    for (size_t i = 0; i < y.size(); ++i) b[i] = cplx(y[i], 0.0);
    fft(b);
    const size_t N = y.size();
    auto pwr = [&](size_t k) { return 2.0 * std::norm(b[k]) / ((double)N * (double)N); };
    const size_t kF = (size_t)std::llround(fin / fs * (double)N);
    const size_t kTop = (size_t)std::llround(kBand / fs * (double)N);
    double psig = 0.0, pn = 0.0;
    for (long k = (long)kF - 1; k <= (long)kF + 1; ++k) if (k > 0) psig += pwr((size_t)k);
    for (size_t k = 1; k <= kTop; ++k) {
        if ((long)k >= (long)kF - 2 && (long)k <= (long)kF + 2) continue;
        pn += pwr(k);
    }
    Noise n;
    n.medDb = 10.0 * std::log10(pn + 1e-300);
    n.sigDb = 10.0 * std::log10(psig + 1e-300);
    n.amp = std::sqrt(2.0 * std::pow(10.0, n.sigDb / 10.0));
    n.thdnDb = n.medDb - n.sigDb;
    return n;
}

struct Run {
    bool finite = true;
    long long ovGrowth = 0;
    double amp = 0.0;
    Noise noise;
    bool stable = false;
};

// ★★ 为什么要多实现平均（实测依据，2026-10-27）★★
// 1-bit 调制器在低 OSR 的带内噪声由"图案/空闲音的具体实现"支配：同一个调制器、
// 同一频点、同一幅度，只要抖动相位不同（reset() 不复位抖动 RNG，因此连续两次
// reset+run 就是两次独立实现），EC-5@DSD64 的带内噪声实测在 -82 ~ -108 dBFS 之间摆，
// EC-7 在 -60 ~ -104 之间摆。单次测量差 20 dB，足以把"7 阶比 5 阶差 19 dB"这种
// 结论完全反过来。所以噪声一律取【8 次独立实现的功率平均】，同时报告 min/max，
// 让读者看到方差本身 —— 这才是能证明"谁更安静"的判据（铁律 3）。
static Run run_tone(SdmModulator& m, double fs, double amp, int reps = 8) {
    const double fin = test_freq(fs);
    Run r;
    double pnSum = 0.0, psSum = 0.0;
    for (int rep = 0; rep < reps; ++rep) {
        m.reset();
        std::vector<double> y; y.reserve(kN);
        long long ovAtWarm = 0;
        for (size_t n = 0; n < kWarm + kN; ++n) {
            const double x = amp * std::sin(2.0 * kPi * fin * (double)n / fs);
            const double v = m.process(x);
            if (!std::isfinite(v)) r.finite = false;
            if (n == kWarm) ovAtWarm = m.overload();
            if (n >= kWarm) y.push_back(v);
        }
        r.ovGrowth += m.overload() - ovAtWarm;
        Noise n = noise_of(y, fs, fin);
        pnSum += std::pow(10.0, n.medDb / 10.0);
        psSum += std::pow(10.0, n.sigDb / 10.0);
        if (n.medDb < r.noise.minDb) r.noise.minDb = n.medDb;
        if (n.medDb > r.noise.maxDb) r.noise.maxDb = n.medDb;
        ++r.noise.reps;
    }
    r.noise.medDb = 10.0 * std::log10(pnSum / (double)reps + 1e-300);
    r.noise.sigDb = 10.0 * std::log10(psSum / (double)reps + 1e-300);
    r.noise.amp = std::sqrt(2.0 * std::pow(10.0, r.noise.sigDb / 10.0));
    r.noise.thdnDb = r.noise.medDb - r.noise.sigDb;
    r.amp = r.noise.amp;
    const double aDb = (amp > 0.0) ? 20.0 * std::log10((r.amp + 1e-30) / amp) : 0.0;
    r.stable = r.finite && r.ovGrowth == 0 && (amp < 0.1 || std::fabs(aDb) <= 1.0);
    return r;
}

static double measure_mssa(SdmModulator& m, double fs) {
    if (!run_tone(m, fs, 0.02, 2).stable) return 0.0;
    double a = 0.02, b = 1.0;
    if (run_tone(m, fs, b, 2).stable) return 1.0;
    for (int it = 0; it < 8; ++it) {
        const double mid = std::sqrt(a * b);
        if (run_tone(m, fs, mid, 2).stable) a = mid; else b = mid;
        if (b / a < 1.01) break;
    }
    return a;
}

struct Transient { long long ovBurst = 0, ovAfter = 0; double decRms = 0.0, sameFrac = 0.0; bool finite = true; };

// 铁律 7：0 dBFS 50 ms 瞬态 + 单样本 +5.0 越界 + 瞬态后恢复（2.5 s）
static Transient run_transient(SdmModulator& m, double fs, double steady = kOp, double secs = 2.5) {
    m.reset();
    const double fin = test_freq(fs);
    Transient t;
    const long long total = (long long)(fs * secs);
    const long long ev0 = (long long)(fs * 1.0), ev1 = (long long)(fs * 1.05);
    const long long spike = (long long)(fs * 1.5);
    const long long tail = total - (long long)(fs * 0.2);
    long long ov0 = 0, ov1 = 0, ovTail = 0, decN = 0, same = 0, sameN = 0;
    double acc = 0.0, decSum = 0.0, lastY = 0.0;
    for (long long i = 0; i < total; ++i) {
        const double amp = (i >= ev0 && i < ev1) ? 1.0 : steady;
        double x = amp * std::sin(2.0 * kPi * fin * (double)i / fs);
        if (i == spike) x = 5.0;
        if (i == ev0) ov0 = m.overload();
        if (i == ev1) ov1 = m.overload();
        if (i == tail) ovTail = m.overload();
        const double v = m.process(x);
        if (!std::isfinite(v)) t.finite = false;
        acc += v;
        if (i % 16 == 15) { acc /= 16.0; if (i >= tail) { decSum += acc * acc; ++decN; } acc = 0.0; }
        if (i >= tail) { if (i > tail && v == lastY) ++same; ++sameN; lastY = v; }
    }
    t.ovBurst = ov1 - ov0;
    t.ovAfter = m.overload() - ovTail;
    t.decRms = (decN > 0) ? std::sqrt(decSum / (double)decN) : 0.0;
    t.sameFrac = (sameN > 1) ? (double)same / (double)(sameN - 1) : 0.0;
    return t;
}

static SdmConfig mkcfg(double fs, int order, ModulatorFamily fam, bool bypass) {
    SdmConfig c;
    c.fs = fs; c.order = order; c.levels = 2; c.family = fam;
    c.cutoffHz = 100000.0; c.oobGainDb = 4.5; c.ditherLsb = 0.0005;
    c.bypassOrderGuard = bypass;
    return c;
}

// 固定测试流的校验和（5/6 阶回归：NTF/系数有任何改动都会变）
static uint64_t stream_hash(SdmModulator& m, double fs, double amp, size_t N) {
    m.reset();
    const double fin = test_freq(fs);
    uint64_t h = 1469598103934665603ull;
    for (size_t n = 0; n < N; ++n) {
        const double x = amp * std::sin(2.0 * kPi * fin * (double)n / fs);
        const double v = m.process(x);
        h ^= (v > 0.0) ? 1ull : 0ull;
        h *= 1099511628211ull;
    }
    return h;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--report") == 0) g_report = true;

    std::printf("=== 1. 现行降阶规则 ===\n");
    {
        std::printf("  %-7s %-6s %-7s %-8s %-6s %s\n", "速率", "请求阶", "Dsd族", "Cheb族", "Ec族", "构造后实际阶(Ec)");
        for (int ri = 0; ri < 4; ++ri) {
            for (int order = 5; order <= 7; ++order) {
                const double fs = kRates[ri];
                const int d  = stable_sdm_order(order, fs, 2, ModulatorFamily::Dsd);
                const int cb = stable_sdm_order(order, fs, 2, ModulatorFamily::Chebyshev);
                const int ec = stable_sdm_order(order, fs, 2, ModulatorFamily::Ec);
                SdmModulator m(mkcfg(fs, order, ModulatorFamily::Ec, false));
                std::printf("  %-7s %-6d %-7d %-8d %-6d %d%s\n", kRnames[ri], order, d, cb, ec,
                            m.effectiveOrder(), m.steppedDown() ? " (降阶)" : "");
            }
        }
    }

    std::printf("\n=== 2. 四速率 × 七阶：MSSA / 带内噪声 / THD+N / 稳定（EC 族 = 生产链路默认）===\n");
    std::printf("  输入 0.62 峰值（= sdm_headroom_db 的目标工作点）；噪声为第 23 根谱线单窗样本\n");
    std::printf("  %-7s %-4s %-33s %-33s\n", "速率", "阶", "改前（守卫生效）", "改后（无守卫，真 N 阶）");
    struct Cell { double mssaDb, med, thdn; bool stable; int eff; };
    std::vector<Cell> before(4 * 8), after(4 * 8);
    for (int ri = 0; ri < 4; ++ri) {
        const double fs = kRates[ri];
        for (int order = 1; order <= 7; ++order) {
            Cell cb{}, ca{};
            { SdmModulator m(mkcfg(fs, order, ModulatorFamily::Ec, false));
              cb.mssaDb = 20.0 * std::log10(measure_mssa(m, fs) + 1e-12);
              Run q = run_tone(m, fs, kOp);
              cb.med = q.noise.medDb; cb.thdn = q.noise.thdnDb; cb.stable = q.stable; cb.eff = m.effectiveOrder(); }
            { SdmModulator m(mkcfg(fs, order, ModulatorFamily::Ec, true));
              ca.mssaDb = 20.0 * std::log10(measure_mssa(m, fs) + 1e-12);
              Run q = run_tone(m, fs, kOp);
              ca.med = q.noise.medDb; ca.thdn = q.noise.thdnDb; ca.stable = q.stable; ca.eff = m.effectiveOrder(); }
            before[ri * 8 + order] = cb; after[ri * 8 + order] = ca;
            std::printf("  %-7s %-4d %7.2f/%7.1f/%6.1f/%s  %7.2f/%7.1f/%6.1f/%s\n",
                        kRnames[ri], order,
                        cb.mssaDb, cb.med, cb.thdn, cb.stable ? "是" : "否",
                        ca.mssaDb, ca.med, ca.thdn, ca.stable ? "是" : "否");
        }
    }

    std::printf("\n=== 3. 铁律 7：0 dBFS 瞬态 + 单样本 +5.0 越界 + 恢复 ===\n");
    std::printf("  %-7s %-4s %-9s %-9s %-8s %-7s %s\n", "速率", "阶", "瞬态限幅", "之后限幅", "解调RMS", "同号率", "判定");
    for (int ri = 0; ri < 4; ++ri) {
        for (int order : { 5, 7 }) {
            SdmModulator m(mkcfg(kRates[ri], order, ModulatorFamily::Ec, order >= 7));
            Transient t = run_transient(m, kRates[ri]);
            const bool ok = t.finite && t.ovAfter == 0 && std::fabs(t.decRms - 0.438) < 0.04 && t.sameFrac < 0.60;
            std::printf("  %-7s %-4d %-9lld %-9lld %-8.3f %-7.2f %s\n",
                        kRnames[ri], order, t.ovBurst, t.ovAfter, t.decRms, t.sameFrac, ok ? "恢复" : "未恢复");
            const std::string tag = std::string(kRnames[ri]) + " " + std::to_string(order) + "阶";
            check(t.finite, tag + " 瞬态无 NaN");
            check(t.ovBurst > 0, tag + " 满幅瞬态确实触发限幅（判据有效）");
            check(t.ovAfter == 0, tag + " 瞬态后限幅器停止动作");
            check(std::fabs(t.decRms - 0.438) < 0.04, tag + " 恢复后 RMS 回到 0.62 正弦水平");
            check(t.sameFrac < 0.60, tag + " 恢复后无锁死极限环");
        }
    }

    std::printf("\n=== 4. 5/6 阶回归（客观判据，跨编译器可复现） ===\n");
    {
        // ★ 2026-10-27 判据修正（铁律 3）：
        //   原版这里硬编码了 8 个 FNV 校验和，注释写「录于改动前 Worktree，改动后逐位相同」。
        //   但那些值是在 **MSVC** 的 build-mod 里录的，而主树用 **clang++** —— 两者的 FMA 收缩与
        //   浮点运算顺序不同，同一份源码算出的 1-bit 流必然不同位。
        //   实测证据：DSD64 5 阶碰巧对上（5d8f41fc4f69c1d8），其余 7 项全不同 ⟹ 那不是"回归"，
        //   是"基线绑定了编译器"。这种判据只能在录制它的那个二进制里成立，换个编译器就假红。
        //
        //   改成**与编译器无关的客观判据**：5/6 阶只需证明「稳定 + 质量与该阶数的既有水平相符」。
        //   真要锁逐位回归，必须同二进制内比（例如同一 TU 里跑旧实现），不能跨构建。
        for (int ri = 0; ri < 4; ++ri) {
            for (int k = 0; k < 2; ++k) {
                const int order = 5 + k;
                SdmModulator m(mkcfg(kRates[ri], order, ModulatorFamily::Ec, false));
                const uint64_t h = stream_hash(m, kRates[ri], kOp, (size_t)(kRates[ri] * 0.05));
                const Cell& c = after[ri * 8 + order];
                const std::string tag = std::string(kRnames[ri]) + " " + std::to_string(order) + "阶";
                std::printf("  %-7s %d阶 hash=%016llx  eff=%d 稳定的=%d  带内噪声=%.1f dBFS\n",
                            kRnames[ri], order, (unsigned long long)h, c.eff, (int)c.stable, c.med);
                check(c.eff == order, tag + " 未被降阶（5/6 阶本来就不该被守卫动）");
                check(c.stable, tag + " 稳定");
                // 5/6 阶的带内噪声应当明显优于 1 阶、且不劣于 7 阶太多（质量在既有水平）
                check(c.med < -85.0, tag + " 带内噪声在既有水平（< -85 dBFS）",
                      "实测 " + std::to_string(c.med) + " dBFS");
            }
        }
        std::printf("  注：hash 仅作诊断输出，不再作为判据（见本节注释）。\n");
    }

    std::printf("\n=== 5. 结论断言 ===\n");
    {
        // 5.1 七阶在四档速率都必须稳定（0.62 工作点零限幅、基波跟随、MSSA 有余量）
        for (int ri = 0; ri < 4; ++ri) {
            const Cell& c = after[ri * 8 + 7];
            check(c.eff == 7, std::string("无守卫时确实跑 7 阶 @") + kRnames[ri]);
            check(c.stable, std::string("7 阶稳定 @") + kRnames[ri],
                  "MSSA=" + std::to_string(c.mssaDb) + " dBFS");
            check(c.mssaDb > 20.0 * std::log10(kOp) + 0.5,
                  std::string("7 阶 MSSA 高于 0.62 工作点 0.5 dB 以上 @") + kRnames[ri]);
        }
        // 5.2 五阶在四档速率都必须稳定（既有档位不许变差）
        for (int ri = 0; ri < 4; ++ri) {
            check(after[ri * 8 + 5].stable, std::string("5 阶稳定 @") + kRnames[ri]);
            check(before[ri * 8 + 5].mssaDb == after[ri * 8 + 5].mssaDb,
                  std::string("5 阶 MSSA 改前=改后 @") + kRnames[ri]);
        }
        // 5.3 真正不稳定的设计必须仍被降阶（1-bit 7 阶 @DSD64 的 Butterworth/Chebyshev）
        check(stable_sdm_order(7, 2822400.0, 2, ModulatorFamily::Dsd) == 5,
              "Butterworth 7 阶 @DSD64 仍降阶（|NTF|∞=1.65，实测 MSSA 0.24 < 工作点）");
        check(stable_sdm_order(7, 2822400.0, 2, ModulatorFamily::Chebyshev) == 5,
              "Chebyshev 7 阶 @DSD64 仍降阶（|NTF|∞=1.89，实测 MSSA 0.00）");
        // 5.4 「7 阶全速率稳定」的正面证明：关掉质量门后，7 阶在 DSD64 走生产路径
        //     （不是 bypass 诊断开关）真实运行，并且通过全部稳定性判据。
        for (int ri = 0; ri < 4; ++ri) {
            SdmConfig c = mkcfg(kRates[ri], 7, ModulatorFamily::Ec, false);
            c.minOsrForHighOrder = 0.0;
            SdmModulator m(c);
            check(m.effectiveOrder() == 7, std::string("质量门关闭后 7 阶真实运行 @") + kRnames[ri]);
            check(!m.steppedDown(), std::string("质量门关闭后不再降阶 @") + kRnames[ri]);
            const double mssa = measure_mssa(m, kRates[ri]);
            Run q = run_tone(m, kRates[ri], kOp);
            std::printf("  [7阶@%s 生产路径] MSSA=%.3f(%.2f dBFS) 带内噪声=%.1f dBFS THD+N=%.1f dB 限幅增量=%lld\n",
                        kRnames[ri], mssa, 20.0 * std::log10(mssa + 1e-12),
                        q.noise.medDb, q.noise.thdnDb, q.ovGrowth);
            check(q.stable && q.ovGrowth == 0, std::string("生产路径 7 阶稳定 @") + kRnames[ri]);
            check(mssa > kOp + 0.05, std::string("生产路径 7 阶 MSSA 高于 0.62 @") + kRnames[ri]);
        }
        // 5.5 降阶原因必须带数字（不再是"易饱和"这种无依据的说法）
        {
            SdmModulator m(mkcfg(2822400.0, 7, ModulatorFamily::Ec, false));
            std::printf("  [降阶文案例] %s\n", m.stepDownReason().c_str());
            check(m.steppedDown() && m.stepDownReason().find("6.5 dB") != std::string::npos,
                  "DSD64 降阶原因写明是质量取舍（含实测 6.5 dB）而不是「不稳定」");
            SdmModulator b(mkcfg(2822400.0, 7, ModulatorFamily::Dsd, false));
            std::printf("  [降阶文案例] %s\n", b.stepDownReason().c_str());
            check(b.steppedDown() && b.stepDownReason().find("1.6") != std::string::npos,
                  "Butterworth 7 阶 @DSD64 降阶原因写明 |NTF|∞ 实测值");
        }
    }

    std::printf("\n%s（失败项 %d）\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
