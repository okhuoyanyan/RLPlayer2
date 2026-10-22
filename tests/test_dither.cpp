// test_dither.cpp — 抖动与噪声整形（按采样率重标定后的验证）
//
// 判据（铁律 3：先想清楚"这个指标能证明什么"）：
//   1) NTF 在【可听带内】绝不能放大噪声 —— 这是旧实现最危险的地方：
//      旧式 (1-z^-1)^N 在 44.1k 输出时 20 kHz 处是 +52 dB，等于把 15~20 kHz 的
//      白噪放大 400 倍，而"0~20k 总功率"这种粗糙判据还可能看不出问题。
//   2) 整形的收益必须【随输出采样率单调增加】：44.1k 几乎没有腾挪空间，
//      705.6k 才应该把噪声真正推到超声段。
//   3) 任何阶数在任何采样率下都不许发散（clamped() 必须为 0）。
#include "dsp/dither.h"
#include "dsp/fft.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <complex>
#include <algorithm>
#include <string>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static double band_power_db(const std::vector<double>& e, double fs,
                            double f0, double f1, double exclHz = -1.0, double exclWidth = 0.0) {
    const size_t N = e.size();
    std::vector<cplx> b(N);
    double wsum = 0.0;
    for (size_t i = 0; i < N; ++i) {
        double t = 2.0 * kPi * (double)i / (double)N;
        double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        b[i] = cplx(e[i] * w, 0.0);
        wsum += w;
    }
    fft(b);
    double p = 0.0;
    const double binHz = fs / (double)N;
    for (size_t k = 1; k < N / 2; ++k) {
        double f = (double)k * binHz;
        if (f < f0 || f > f1) continue;
        if (exclHz > 0.0 && std::fabs(f - exclHz) <= exclWidth) continue;
        double mag = 2.0 * std::abs(b[k]) / wsum;
        p += mag * mag * 0.5;
    }
    return 10.0 * std::log10(p + 1e-300);
}

struct Row { std::string name; double inBand; double outBand; long long clamped; };

static Row measure(Shaper s, double fs, int bits, size_t N, double amp, double fin) {
    Quantizer qz(s, bits, 0xC0FFEEull, fs);
    std::vector<double> err;
    err.reserve(N);
    const size_t warm = 20000;
    for (size_t n = 0; n < warm + N; ++n) {
        double x = amp * std::sin(2.0 * kPi * fin * (double)n / fs);
        double y = qz.process(x);
        if (n >= warm) err.push_back(y - x);
    }
    const double binHz = fs / (double)N;
    Row r;
    r.name = shaper_name(s);
    r.inBand  = band_power_db(err, fs, 20.0, 20000.0, fin, 6.0 * binHz);
    r.outBand = band_power_db(err, fs, 20000.0, fs / 2.0);
    r.clamped = qz.clamped();
    return r;
}

int main() {
    const int    bits = 16;
    const size_t N = 65536;
    const double amp = 0.001;        // -60 dBFS
    const double fin = 1000.0;
    const double rates[] = { 44100.0, 96000.0, 176400.0, 705600.0 };

    printf("=== 1. NTF 是否在可听带内放大了噪声（关键安全性检查）===\n");
    printf("  %-10s %8s %10s %10s %10s\n", "采样率", "19kHz", "20kHz", "30kHz", "奈奎斯特");
    for (double fs : rates) {
        Quantizer q9(Shaper::NS9, bits, 1, fs);
        printf("  %-10.0f %7.2f %9.2f %9.2f %9.2f dB   (r=%.4f, fs_in=%.0f)\n",
               fs, q9.ntf_db(19000.0), q9.ntf_db(20000.0), q9.ntf_db(30000.0),
               q9.ntf_db(fs * 0.5 - 1.0), q9.radius(), q9.sample_rate());
        printf("     radius_for_rate(44100)=%.4f, (96000)=%.4f, (705600)=%.4f\n",
               shaper_radius_for_rate(44100.0, 9), shaper_radius_for_rate(96000.0, 9),
               shaper_radius_for_rate(705600.0, 9));
        // 设计预算：可听带上端允许的单级总增益 <= +6 dB（这是"取舍"，不是"零放大"）
        check(q9.ntf_db(19000.0) < 6.5, "NS9 在 19 kHz 处于设计预算内",
              std::to_string(fs) + " Hz -> " + std::to_string(q9.ntf_db(19000.0)) + " dB");
        // 噪声必须真的被推到高频去（否则就是既没收益又多做了运算）
        check(q9.ntf_db(fs * 0.5 - 1.0) > 3.0, "NS9 在奈奎斯特附近高于 0 dB",
              std::to_string(fs) + " Hz -> " + std::to_string(q9.ntf_db(fs * 0.5 - 1.0)) + " dB");
    }

    printf("\n=== 2. 各采样率下的实测带内噪声（-60 dBFS 1 kHz 正弦 -> 16 bit）===\n");
    // 收益要求来自实测（改设计就要重新标定）：44.1k 几乎没空间，只有 ~3 dB；
    // 96k 约 11 dB；176.4k 起才有真正的 40 dB 级收益。
    struct Case { double fs; double wantNs9Gain; };
    const Case cases[] = { {44100.0, 2.5}, {96000.0, 8.0}, {176400.0, 35.0}, {705600.0, 40.0} };

    for (const auto& c : cases) {
        printf("\n  --- %.0f Hz ---\n", c.fs);
        printf("  %-8s %12s %12s %10s\n", "整形器", "带内(dB)", "带外(dB)", "发散保护");
        Row tp = measure(Shaper::TPDF, c.fs, bits, N, amp, fin);
        Row ns9 = measure(Shaper::NS9, c.fs, bits, N, amp, fin);
        for (Shaper s : { Shaper::None, Shaper::TPDF, Shaper::NS1, Shaper::NS4,
                          Shaper::NS5, Shaper::NS9, Shaper::LNS15 }) {
            Row r = measure(s, c.fs, bits, N, amp, fin);
            printf("  %-8s %11.1f %12.1f %10lld  %s\n", r.name.c_str(), r.inBand, r.outBand, r.clamped,
                   r.clamped ? "⚠发散" : "稳定");
            check(r.clamped == 0, std::string(r.name) + " 数值稳定 @ " + std::to_string((int)c.fs));
        }
        const double gain = tp.inBand - ns9.inBand;
        printf("  -> NS9 相对 TPDF 的带内收益: %.1f dB（要求 >= %.1f）\n", gain, c.wantNs9Gain);
        check(gain >= c.wantNs9Gain - 0.5,
              "NS9 带内收益达标 @" + std::to_string((int)c.fs),
              std::to_string(gain) + " dB");

        // 安全性总原则：任何整形器在任何采样率下，带内噪声都不许比 TPDF 更差。
        // （阶数高低与收益并不单调 —— 半径是按阶数分别求解的，这是有意为之，
        //   所以这里不能断言"阶数越高越好"，只能断言"都不许变差"。）
        for (Shaper s : { Shaper::NS1, Shaper::NS4, Shaper::NS5, Shaper::NS9, Shaper::LNS15 }) {
            Row r = measure(s, c.fs, bits, N, amp, fin);
            check(r.inBand <= tp.inBand + 0.5,
                  std::string(shaper_name(s)) + " 带内不劣于 TPDF @" + std::to_string((int)c.fs),
                  std::to_string(r.inBand - tp.inBand) + " dB");
        }
    }

    printf("\n=== 3. 旧式 NTF 的反例（证明测试判据确实能抓到问题）===\n");
    {
        // 旧实现：NTF=(1-z^-1)^9 在 44.1k 下 20 kHz 处约 +52 dB
        auto legacy = ntf_coeffs(9, 1.0);
        const double at20k = ntf_magnitude_db(legacy, 20000.0 / 22050.0);
        printf("  旧 NTF(1-z^-1)^9 @ 44.1k, 20 kHz = %.1f dB\n", at20k);
        check(at20k > 30.0, "旧式 NTF 在 44.1k 下确实严重放大 20 kHz（这就是被替换的原因）");
        Quantizer q9(Shaper::NS9, bits, 1, 44100.0);
        // 新设计的目标就是"拐点处最多 +6 dB"，而不是"零放大"（零放大意味着完全没有整形空间）
        check(q9.ntf_db(20000.0) <= 6.5, "新 NTF 在 44.1k / 20 kHz 处落在设计预算内",
              std::to_string(q9.ntf_db(20000.0)) + " dB");
    }


    printf("\n=== 4. 满幅 / 过载稳定性（2026-10-21 现场故障的回归）===\n");
    // 现场：24-bit 输出 + 默认 shaper=auto（=LNS15）+ rate=max（192k/384k），
    // 播到第一个 0 dBFS 峰值后突然变成满幅噪音。旧实现只钳位量化【输出】，
    // 过载误差经 15 阶反馈放大 90 dB 后锁死在 ±1；钳位改到量化【输入】后消失。
    // 判据（不是"没崩"）：过载后 0.5 秒的 RMS 必须回到与输入一致的水平，
    // 且不允许出现长串满幅样本 —— 一个 ±1 的方波也能"不崩"。
    {
        struct OvlCase { double fs; int bits; double amp; };
        const OvlCase oc[] = {
            {192000.0, 24, 0.999999}, {384000.0, 24, 0.999999},
            {705600.0, 24, 0.999999}, {384000.0, 16, 0.999999},
            {384000.0, 24, 0.5},      {96000.0,  24, 0.999999},
        };
        for (const auto& c : oc) {
            Quantizer q(Shaper::LNS15, c.bits, 0xC0FFEEull, c.fs);
            const long long n = (long long)(c.fs * 4.0);
            const long long ev = (long long)(c.fs * 2.0);   // 2 秒处插一个越界样本
            double lastPeak = 0.0, lastSum = 0.0;
            long long lastN = 0, run = 0, maxRun = 0, evOverload = -1;
            for (long long i = 0; i < n; ++i) {
                double x = c.amp * std::sin(2.0 * kPi * 997.0 * (double)i / c.fs);
                if (i == ev) x = 1.0005;                     // 单个越界样本（最坏情况）
                const double y = q.process(x);
                if (i == ev) evOverload = q.overload();
                const double step = 2.0 / std::pow(2.0, (double)c.bits);
                if (i > ev && std::fabs(y) >= 1.0 - step) { ++run; maxRun = std::max(maxRun, run); }
                else run = 0;
                if (i > n - (long long)(c.fs / 2)) {
                    lastPeak = std::max(lastPeak, std::fabs(y));
                    lastSum += y * y; ++lastN;
                }
            }
            const double lastRms = std::sqrt(lastSum / (double)lastN);
            const double wantRms = c.amp / std::sqrt(2.0);
            const std::string tag = std::to_string((int)c.fs) + "Hz/" + std::to_string(c.bits) +
                                    "bit/amp" + std::to_string(c.amp);
            printf("  %-28s lastRms=%.4f (want %.4f) lastPeak=%.4f maxRun=%-3lld overload=%lld\n",
                   tag.c_str(), lastRms, wantRms, lastPeak, maxRun, q.overload());
            check(q.clamped() == 0, "  无误差反馈发散 @" + tag);
            check(maxRun < 64, "  过载后不会锁死在满幅 @" + tag,
                  "最长满幅串 " + std::to_string(maxRun));
            check(std::fabs(lastRms - wantRms) < 0.05, "  过载后 RMS 回到输入水平 @" + tag,
                  "实测 " + std::to_string(lastRms));
            check(lastPeak <= 1.0 + 1e-12, "  输出不越界 @" + tag);
            // 过载样本本身必须真的把量化器推过界（否则这条用例没测到东西）
            check(evOverload >= 0, "  越界样本确实触发过载（判据有效）@" + tag,
                  "overload=" + std::to_string(evOverload));
        }
    }

    printf("\n=== 5. NaN/Inf 输入不得污染误差反馈 ===\n");
    {
        Quantizer q(Shaper::LNS15, 24, 0xC0FFEEull, 384000.0);
        uint64_t nanBits = 0x7FF8000000000000ull;
        double nanTmp = 0.0;
        std::memcpy(&nanTmp, &nanBits, sizeof(nanTmp));
        volatile double nanv = nanTmp;   // volatile 防止 -ffast-math 把注入消掉
        const long long n = 384000 * 2;
        bool badOutput = false;
        double sum = 0.0; long long cnt = 0;
        for (long long i = 0; i < n; ++i) {
            double x = 0.5 * std::sin(2.0 * kPi * 997.0 * (double)i / 384000.0);
            if (i == 384000 / 2) x = nanv;          // 单个 NaN
            const double y = q.process(x);
            { uint64_t yb = 0; std::memcpy(&yb, &y, sizeof(yb));
              if (!finite_bits_u64(yb)) badOutput = true; }
            if (i > n - 192000) { sum += y * y; ++cnt; }
        }
        const double rms = std::sqrt(sum / (double)cnt);
        printf("  NaN 输入后：nonFinite=%lld，末段 RMS=%.4f\n", q.nonFinite(), rms);
        check(!badOutput, "NaN 不会出现在输出里");
        check(q.nonFinite() >= 1, "NaN 被识别并清零");
        check(std::fabs(rms - 0.3536) < 0.03, "NaN 之后 RMS 回到 0.5 正弦水平",
              std::to_string(rms));
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
