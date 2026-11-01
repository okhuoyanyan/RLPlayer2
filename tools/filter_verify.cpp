// filter_verify.cpp — 261030-01 路线图的实测验证（E-1 ~ E-5）
//
// 为什么单独写一个工具而不是改 filter_report：
//   filter_report 是"79 预设 × 固定率"的体检表，它的口径（44.1→705.6、15k→29.1k）
//   与本轮要回答的问题不同。本轮要回答的是：
//     E-1  220 dB 到底需要多少抽头（Kaiser 公式只是估计）
//     E-2  切趾是否真的破坏"原始样本逐位保留"
//     E-3  预振铃是否真的 ≈ 群延迟
//     E-4  文档 §3.3 的基线数字（44.1→96 = −180.7 dB）能不能复现
//     E-5  级联 vs 单级的复合响应是否一致
//
// 测量方法：**直接对原型系数做 DFT**，不用正弦+Goertzel。
//   原因：要测 −220 dB，正弦法的泄漏（矩形窗旁瓣 −13 dB）会把测量下限卡在 −100 dB 量级，
//   根本看不到 −220。而原型系数就是滤波器的脉冲响应，对它做精确 DFT 没有泄漏问题。
#include "dsp/filter_design.h"
#include "dsp/filter_families.h"
#include "dsp/resampler.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

using namespace rta::dsp;
static constexpr double kPi = 3.14159265358979323846;

static int g_fail = 0;

// ---- 设计一条原型：L 倍插值，通带切在 passHz（输入域），每相 T 抽头，目标衰减 attenDb ----
struct Proto {
    std::vector<double> h;   // 内插域原型（含 ×L 直流增益）
    int    L = 1;
    int    T = 0;
    double fsIn = 44100.0;
    double passHz = 20000.0;
};

static Proto design(double fsIn, double fsOut, double passHz, int T, double attenDb) {
    Proto p;
    p.L = (int)std::llround(fsOut / fsIn);
    p.T = T;
    p.fsIn = fsIn;
    p.passHz = passHz;
    FirSpec s;
    s.taps    = T * p.L;
    s.cutoff  = (1.0 / (double)p.L) * (passHz / (fsIn * 0.5));   // 内插域归一化截止
    s.attenDb = attenDb;
    s.window  = WindowKind::Kaiser;
    p.h = design_fir(s);
    return p;
}

// ---- 预振铃时长（包络法）----
// 为什么不用"|h| 首次跌破阈值"：|h| 是振荡的，会反复穿过阈值，得到的宽度不单调
// （实测 Δf 从 300 扫到 14100，宽度 0.73→0.12→0.43→0.28，毫无规律）。
// 包络法：在峰值之前滑动取局部极大（窗口 = 半个输入样本对应的抽头数），
// 再找"包络最后一次高于阈值"的位置 ⟹ 单调、可复现、可写进规格。
static double ring_ms_env(const std::vector<double>& h, int L, double fsIn, double threshDb) {
    size_t peak = 0; double mx = 0;
    for (size_t i = 0; i < h.size(); ++i) if (std::fabs(h[i]) > mx) { mx = std::fabs(h[i]); peak = i; }
    if (mx <= 0.0) return 0.0;
    const double thr = mx * std::pow(10.0, threshDb / 20.0);
    const size_t W = (size_t)std::max(1, L / 2);          // 半个输入样本
    size_t last = 0;
    for (size_t d = 0; d + W < peak; ++d) {
        double env = 0.0;
        const size_t lo = (peak >= d + W) ? (peak - d - W) : 0;
        const size_t hi = std::min(h.size() - 1, peak - d + W);
        for (size_t i = lo; i <= hi; ++i) env = std::max(env, std::fabs(h[i]));
        if (env >= thr) last = d;
    }
    return (double)last / (double)L / fsIn * 1000.0;
}

// 归一化频率：f(Hz) → 内插域 [0,1]（1 = fs_out/2）
static double normFreq(const Proto& p, double hz) { return hz / (p.fsIn * p.L * 0.5); }

// ---- E-1：220 dB 需要多少抽头 ----
static void test_E1() {
    printf("\n=== E-1 · 220 dB 需要多少抽头（44.1k -> 176.4k，通带 20 kHz，镜像 24.1 kHz）===\n");
    const double fsIn = 44100.0, fsOut = 176400.0, pass = 20000.0;
    const double imageHz = fsIn - pass;      // 20 kHz 的镜像 = 24.1 kHz
    printf("  镜像频率 = %.0f Hz（= fs_in - pass）\n", imageHz);
    printf("  %6s %8s %14s %14s %12s\n", "T", "taps", "镜像处(dB)", "20k 处(dB)", "预振铃ms");
    for (int T : { 32, 64, 96, 128, 159, 192, 256, 317, 512, 1024, 2199 }) {
        Proto p = design(fsIn, fsOut, pass, T, 220.0);
        const double aImg  = magnitude_db(p.h, normFreq(p, imageHz));
        const double aPass = magnitude_db(p.h, normFreq(p, pass));
        // 预振铃：峰值之前，|h| 首次跌破 -60 dB 的位置到峰值的距离（换算成输入域毫秒）
        const double ringMs = ring_ms_env(p.h, p.L, fsIn, -60.0);
        printf("  %6d %8zu %14.1f %14.4f %12.2f\n", T, p.h.size(), aImg, aPass, ringMs);
    }
}

// ---- E-2：切趾 vs 不切趾，原始样本保留 ----
static void test_E2() {
    printf("\n=== E-2 · 原始样本保留（44.1k -> 176.4k，L=4）===\n");
    printf("  判据：h[jL] (j≠0) 全为 0 ⟹ 输出第 kL 个样本 == 输入第 k 个样本\n");
    const double fsIn = 44100.0, fsOut = 176400.0;
    struct Case { const char* name; double pass; };
    const Case cases[] = {
        { "截止 = Nyquist (22050 Hz)", 22050.0 },
        { "切趾 20 kHz", 20000.0 },
        { "切趾 18 kHz", 18000.0 },
    };
    for (const auto& c : cases) {
        Proto p = design(fsIn, fsOut, c.pass, 159, 220.0);
        const int L = p.L;
        size_t peak = 0; double mx = 0;
        for (size_t i = 0; i < p.h.size(); ++i) if (std::fabs(p.h[i]) > mx) { mx = std::fabs(p.h[i]); peak = i; }
        // 峰值在 j=0 的位置 = peak % L；取同余类上其它 j 的最大幅度
        const size_t phase = peak % (size_t)L;
        double worst = 0.0;
        for (size_t i = phase; i < p.h.size(); i += (size_t)L) {
            if (i == peak) continue;
            worst = std::max(worst, std::fabs(p.h[i]));
        }
        const double errDb = 20.0 * std::log10((worst + 1e-300) / mx);
        printf("  %-26s h[0]=%.6f  同余类最大旁瓣=%.3e  ⟹ 样本误差 %7.1f dB  %s\n",
               c.name, mx, worst, errDb, errDb < -300.0 ? "【逐位保留】" : "【不保留】");
    }
}

// ---- E-6：A=280 dB 需要多少抽头 ----
static void test_E6() {
    printf("\n=== E-6 · A=280 dB 需要多少抽头（44.1k->176.4k，通带 20 kHz，镜像 24.1 kHz）===\n");
    printf("  %6s %8s %14s %12s\n", "T", "taps", "镜像处(dB)", "振铃ms");
    int best = 0;
    for (int T : { 64, 96, 128, 160, 192, 224, 256, 320, 384, 512 }) {
        Proto p = design(44100.0, 176400.0, 20000.0, T, 280.0);
        const double a = magnitude_db(p.h, normFreq(p, 24100.0));
        printf("  %6d %8zu %14.1f %12.2f\n", T, p.h.size(), a, ring_ms_env(p.h, p.L, 44100.0, -60.0));
        if (best == 0 && a <= -280.0) best = T;
    }
    printf("  >>> 达到 280 dB 的最小 T = %d\n", best);
    // 再验证 A=280 时 20 kHz 处是否仍然平直
    if (best > 0) {
        Proto p = design(44100.0, 176400.0, 20000.0, best, 280.0);
        printf("  >>> 该 T 下 20 kHz 处 = %.4f dB\n", magnitude_db(p.h, normFreq(p, 20000.0)));
    }
}

// ---- E-7：高斯窗要多少抽头才到 280 dB（用户的 gauss-hires-mp 家族）----
static void test_E7() {
    printf("\n=== E-7 · 高斯窗 vs Kaiser 窗：达到 280 dB 各需多少抽头 ===\n");
    printf("  %6s %14s %14s\n", "T", "Kaiser(dB)", "Gaussian(dB)");
    for (int T : { 128, 160, 224, 320, 425, 512, 640, 800, 1024, 1536, 2048 }) {
        Proto pk = design(44100.0, 176400.0, 20000.0, T, 280.0);
        const double ak = magnitude_db(pk.h, normFreq(pk, 24100.0));
        FirSpec s;
        s.taps = T * 4;
        s.cutoff = 0.25 * (20000.0 / 22050.0);
        s.attenDb = 280.0;
        s.window = WindowKind::Gaussian;
        auto h = design_fir(s);
        const double ag = magnitude_db(h, normFreq(pk, 24100.0));
        printf("  %6d %14.1f %14.1f\n", T, ak, ag);
    }
}

// ---- E-8：截止点位置对镜像抑制的影响（隔离单一变量）----
// 背景：子代理的 filter_sweep 用 cutoff = pass + df/2，得到 −117 dB；
// 我的 E-6 用 cutoff = pass，得到 −291 dB。两者 T×Δf 几乎相同，必须定位差异。
static void test_E8() {
    printf("\n=== E-8 · 截止点位置 vs 镜像抑制（44.1k->705.6k, L=16）===\n");
    const double fsIn = 44100.0, pass = 17640.0, df = 8820.0;
    const double image = fsIn - pass;                 // 26460 Hz
    const int L = 16;
    printf("  pass=%.0f df=%.0f image=%.0f  (归一化镜像=%.5f)\n",
           pass, df, image, image / (fsIn * L * 0.5));
    printf("  %6s %8s %14s %16s\n", "T", "cutoff", "镜像处(dB)", "通带0.95pass(dB)");
    for (int T : { 74, 96, 128, 160, 256, 512, 1024 }) {
        for (int mode = 0; mode < 2; ++mode) {
            const double cut = (mode == 0) ? pass : (pass + df * 0.5);
            FirSpec s;
            s.taps = T * L;
            s.cutoff = (cut / (fsIn * 0.5)) / (double)L;
            s.attenDb = 280.0;
            s.window = WindowKind::Kaiser;
            auto h = design_fir(s);
            const double aImg  = magnitude_db(h, image / (fsIn * L * 0.5));
            const double aPass = magnitude_db(h, pass * 0.95 / (fsIn * L * 0.5));
            printf("  %6d %8s %14.1f %16.4f\n", T, mode == 0 ? "pass" : "mid", aImg, aPass);
        }
    }
}

// ---- E-3：预振铃到底由什么决定（Δf 扫描）----
static void test_E3() {
    printf("\n=== E-3 · 预振铃由 Δf 决定还是由抽头决定 ===\n");
    printf("  固定 fs=44.1k->176.4k，改通带位置（= 改 Δf = fs_in - 2*pass），T 固定 512\n");
    printf("  %10s %10s %10s %12s %12s\n", "pass(Hz)", "Δf(Hz)", "1/Δf(ms)", "实测振铃ms", "镜像dB");
    for (double pass : { 21900.0, 21500.0, 21000.0, 20000.0, 18000.0, 15000.0 }) {
        const double dF = 44100.0 - 2.0 * pass;
        if (dF <= 0) continue;
        Proto p = design(44100.0, 176400.0, pass, 512, 220.0);
        const double ringMs = ring_ms_env(p.h, p.L, 44100.0, -60.0);
        const double aImg = magnitude_db(p.h, normFreq(p, 44100.0 - pass));
        printf("  %10.0f %10.0f %10.3f %12.3f %12.1f\n",
               pass, dF, 1000.0 / dF, ringMs, aImg);
    }
}

// ---- E-4：文档 §3.3 基线复核 ----
static void test_E4() {
    printf("\n=== E-4 · 基线复核（poly-sinc-lp，固定预算 32767）===\n");
    printf("  文档声称：44.1->96k = -180.7 dB，44.1->384k = -18.9 dB\n");
    const FilterPreset* p = find_preset("poly-sinc-lp", false);
    if (!p) { printf("  找不到 poly-sinc-lp\n"); ++g_fail; return; }
    struct Case { double out; const char* label; };
    const Case cases[] = { { 96000, "44.1->96k" }, { 192000, "44.1->192k" },
                           { 384000, "44.1->384k" }, { 705600, "44.1->705.6k" } };
    for (const auto& c : cases) {
        // ★ 修正：44.1->96 不是整数倍（L=320, M=147）。之前用 llround 得到 L=2，
        //   实际测的是 44.1->88.2，那一列数据是假的。
        const long long inR = 44100, outR = (long long)c.out;
        long long a = inR, b = outR;
        while (b) { const long long t = a % b; a = b; b = t; }
        const long long g = a;
        const int L = (int)(outR / g);
        const int T = 32767 / L;
        if (T < 4) { printf("  %-14s L=%5d T=%4d  抽头不足，跳过\n", c.label, L, T); continue; }
        // 预设语义：cutoff = (1/L)(1 - transition)，transition=0.04
        const double cutoff = (1.0 / (double)L) * (1.0 - p->transition);
        FirSpec s;
        s.taps = T * L;
        s.cutoff = cutoff;
        s.attenDb = p->attenDb;
        s.window = p->window;
        auto h = design_fir(s);
        // 镜像在 fs_in - 20k = 24.1 kHz；换算到内插域
        const double fNorm = 24100.0 / (inR * L * 0.5);
        const double aImg = magnitude_db(h, fNorm);
        // 通带边缘（cutoff 处）应该 ~0 dB
        const double aPass = magnitude_db(h, cutoff * 0.98);
        printf("  %-14s L=%5d T=%4d  原型%7zu 抽头  镜像处 %8.1f dB  通带边 %6.2f dB\n",
               c.label, L, T, h.size(), aImg, aPass);
    }
}

// ---- E-5：级联 vs 单级 ----
static void test_E5() {
    printf("\n=== E-5 · 级联 vs 单级（44.1k -> 1411.2k）===\n");
    // 单级：L=32，T=159
    Proto single = design(44100.0, 1411200.0, 20000.0, 159, 220.0);
    printf("  单级  L=32  T=159  原型 %zu 抽头  镜像处 %.1f dB\n",
           single.h.size(), magnitude_db(single.h, normFreq(single, 44100.0 - 20000.0)));
    // 级联：第一级 44.1->176.4 承担砖墙，后三级宽过渡带
    Proto s1 = design(44100.0, 176400.0, 20000.0, 159, 220.0);
    printf("  级联1 L=4   T=159  原型 %zu 抽头  镜像处 %.1f dB\n",
           s1.h.size(), magnitude_db(s1.h, normFreq(s1, 44100.0 - 20000.0)));
    double totalMac = 176400.0 * 159.0;
    double mid = 176400.0;
    for (int i = 0; i < 3; ++i) {
        const double outRate = mid * 4.0;
        const double fc = mid * 0.5 - 20000.0;          // 镜像距离通带
        const int T = (int)std::ceil((220.0 - 8.0) * mid / (14.36 * std::max(1000.0, fc)));
        Proto s = design(mid, outRate, 20000.0, std::max(8, T), 220.0);
        printf("  级联%d L=4   T=%3d  原型 %zu 抽头  镜像处 %.1f dB\n",
               i + 2, std::max(8, T), s.h.size(),
               magnitude_db(s.h, normFreq(s, mid * 0.5 - 20000.0)));
        totalMac += outRate * std::max(8, T);
        mid = outRate;
    }
    const double singleMac = 1411200.0 * 159.0;
    printf("  ---- 算力（单声道，MAC/s）----\n");
    printf("  单级 %.3f GMAC/s   级联 %.3f GMAC/s   级联/单级 = %.2fx\n",
           singleMac / 1e9, totalMac / 1e9, totalMac / singleMac);
    printf("  注：<1 表示级联更省；PCM 1.4 MHz 这个量级上单级本来就不贵，级联未必划算。\n");
}

int main() {
    printf("filter_verify — 261030-01 实测验证\n");
    test_E1();
    test_E2();
    test_E3();
    test_E6();
    test_E7();
    test_E8();
    test_E4();
    test_E5();
    printf("\n=== 结束（失败项 %d）===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
