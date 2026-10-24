// test_ir_resample.cpp — IR 采样率换算（src/dsp/ir_resample.h）的四项客观判据（离线，不碰设备）
//
// 被测缺陷：IR 是"时间函数"。44.1k 量出来的房间响应直接铺到 96k 工程上，时间轴被拉伸
// 96/44.1 = 2.177 倍（1 s 的响应当成 0.46 s）。这个文件用四项判据证明换算后：
//   1) 时间基准正确（单点脉冲的峰值位置）
//   2) 时长守恒（200 ms 衰减噪声的能量重心时间）
//   3) 频响不变（1 kHz 正弦的 Goertzel 幅度）
//   4) 同采样率逐位透传（不偷偷多算一步滤波）
// 外加一条补充检查：冲尾巴的补零足够（IR 末尾不被截断）。
//
// 判据为什么不这么写（本仓"测试通过 ≠ 测对了东西"的教训）：
//  * 不能逐点比对波形：线性相位 FIR 的延迟估计只要差 1 个样本，逐点残差就会爆掉，
//    而真正的缺陷（时间轴比例错）反而可能被延迟误差掩盖 ⟹ 用【峰值位置】和【能量重心】，
//    两者都是"时间"量，直接对应缺陷本身。
//  * 判据三不能把整个 200 ms 都算进去：测试信号是有限长的，两端各有约 T/2 个输入样本
//    被滤波器边缘截断（≈5.8 ms），那是信号截断的瞬态、不是增益误差，算进去会假失败
//    约 0.5 dB ⟹ 只测中段 100 ms（两个采样率下都是 100 个整周期，Goertzel 精确）。
#include "dsp/ir_resample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

int fails = 0;

void check(bool ok, const char* what, const char* detail = "") {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) ++fails;
}

// 单频 Goertzel 幅度。窗口内若是整数个周期的正弦，返回值 = 正弦幅度 A
// （|X| = A*N/2 ⟹ A = 2|X|/N），与窗口起点、采样率都无关。
double goertzel_amp(const std::vector<double>& x, size_t start, size_t len,
                    double freq, double fs) {
    const double w = 2.0 * kPi * freq / fs;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t n = 0; n < len; ++n) {
        const double s0 = x[start + n] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;   // |X|^2
    return 2.0 * std::sqrt(std::max(0.0, p)) / (double)len;
}

// 能量重心时间（ms）= sum(n*x^2)/sum(x^2) / fs。
// 对整体增益、常数延迟都不敏感，只对"时间轴被拉伸/压缩"敏感 —— 正是判据二要测的。
double centroid_ms(const std::vector<double>& x, double fs) {
    double num = 0.0, den = 0.0;
    for (size_t n = 0; n < x.size(); ++n) {
        const double e = x[n] * x[n];
        num += (double)n * e;
        den += e;
    }
    return den > 0.0 ? num / den / fs * 1000.0 : 0.0;
}

double rms(const std::vector<double>& x, size_t start, size_t len) {
    double s = 0.0;
    for (size_t n = 0; n < len; ++n) s += x[start + n] * x[start + n];
    return len > 0 ? std::sqrt(s / (double)len) : 0.0;
}

size_t argmax_abs(const std::vector<double>& x) {
    size_t best = 0;
    for (size_t n = 1; n < x.size(); ++n)
        if (std::fabs(x[n]) > std::fabs(x[best])) best = n;
    return best;
}

// 200 ms、50 ms 衰减常数的确定性噪声（LCG，不依赖库实现）
std::vector<double> make_decay_noise(size_t inLen, uint32_t inR) {
    std::vector<double> x(inLen);
    const double tau = 0.05 * (double)inR;
    uint32_t s = 0x12345678u;
    for (size_t n = 0; n < inLen; ++n) {
        s = s * 1664525u + 1013904223u;
        const double w = (double)(s >> 8) / 8388608.0 - 1.0;   // [-1,1)
        x[n] = w * std::exp(-(double)n / tau);
    }
    return x;
}

} // namespace

int main() {
    std::printf("=== 判据一：时间基准（单点脉冲的峰值位置）===\n");
    {
        // 44.1k -> 96k：IR 的采样点必须落到 out/in 倍的时间位置上。
        const uint32_t inR = 44100, outR = 96000;
        const size_t N = 1000, inLen = 4096;
        std::vector<double> imp(inLen, 0.0);
        imp[N] = 1.0;
        const auto out = rta::dsp::ir_resample(imp, inR, outR);
        const size_t expectLen = (size_t)std::llround((double)inLen * (double)outR / (double)inR);
        if (out.empty()) {
            check(false, "输出非空");
        } else {
            const double ideal = (double)N * (double)outR / (double)inR;
            const size_t peak = argmax_abs(out);
            const double err = (double)peak - ideal;
            char d[320];
            std::snprintf(d, sizeof d,
                          "输入 %zu @%u -> 输出 %zu（期望 %zu）；峰值 %zu，理论 %.3f，"
                          "误差 %+.3f 样本 (%+.3f ms)，峰值幅度 %.4f",
                          inLen, inR, out.size(), expectLen, peak, ideal, err,
                          err / (double)outR * 1000.0, out[peak]);
            std::printf("  %s\n", d);
            check(out.size() == expectLen, "输出长度 = round(N*out/in)");
            check(std::fabs(err) <= 2.0, "峰值位置误差 ≤ 2 样本");
        }
    }
    {
        // 反方向（96k 的 IR 用到 44.1k 工程）同样是真实场景，时间基准必须对称成立。
        const uint32_t inR = 96000, outR = 44100;
        const size_t N = 1000, inLen = 8192;
        std::vector<double> imp(inLen, 0.0);
        imp[N] = 1.0;
        const auto out = rta::dsp::ir_resample(imp, inR, outR);
        if (out.empty()) {
            check(false, "反向输出非空");
        } else {
            const double ideal = (double)N * (double)outR / (double)inR;
            const size_t peak = argmax_abs(out);
            const double err = (double)peak - ideal;
            char d[320];
            std::snprintf(d, sizeof d,
                          "反向 输入 %zu @%u -> 输出 %zu；峰值 %zu，理论 %.3f，"
                          "误差 %+.3f 样本 (%+.3f ms)，峰值幅度 %.4f",
                          inLen, inR, out.size(), peak, ideal, err,
                          err / (double)outR * 1000.0, out[peak]);
            std::printf("  %s\n", d);
            check(std::fabs(err) <= 2.0, "反向峰值位置误差 ≤ 2 样本");
        }
    }

    std::printf("\n=== 判据二：时域长度守恒（200 ms 衰减噪声的能量重心）===\n");
    {
        const uint32_t inR = 44100, outR = 96000;
        const size_t inLen = 8820;                       // 200.0 ms
        const auto dec = make_decay_noise(inLen, inR);
        const auto out = rta::dsp::ir_resample(dec, inR, outR);
        const size_t expectLen = (size_t)std::llround((double)inLen * (double)outR / (double)inR);
        const double cIn = centroid_ms(dec, (double)inR);
        const double cOut = centroid_ms(out, (double)outR);
        char d[320];
        std::snprintf(d, sizeof d,
                      "重心 输入 %.3f ms @%u（%zu 样本） / 输出 %.3f ms @%u（%zu 样本，期望 %zu）；"
                      "差 %+.3f ms",
                      cIn, inR, inLen, cOut, outR, out.size(), expectLen, cOut - cIn);
        std::printf("  %s\n", d);
        check(out.size() == expectLen, "输出长度 = round(N*out/in)（200 ms 不变）");
        check(std::fabs(cOut - cIn) < 5.0, "能量重心时间差 < 5 ms");
    }

    std::printf("\n=== 判据三：频响不变（1 kHz 正弦 IR，200 ms）===\n");
    {
        const uint32_t inR = 44100, outR = 96000;
        const size_t inLen = 8820;                       // 200 ms = 200 个整周期
        std::vector<double> sine(inLen);
        for (size_t n = 0; n < inLen; ++n)
            sine[n] = 0.5 * std::sin(2.0 * kPi * 1000.0 * (double)n / (double)inR);
        const auto out = rta::dsp::ir_resample(sine, inR, outR);

        // 中段 100 ms：44.1k 下 4410 点 = 100 周期；96k 下 9600 点 = 100 周期。
        const size_t winIn = 4410, winOut = 9600;
        if (out.size() < winOut) {
            // 若换算没做（IR 原样返回），窗口根本取不出来 —— 明确判失败，
            // 绝不让下标越界把"没做换算"伪装成崩溃或噪声读数。
            char d[200];
            std::snprintf(d, sizeof d, "输出只有 %zu 样本，取不出 96k 下的 100 个整周期",
                          out.size());
            check(false, "输出长度足以取中段 100 ms 窗口", d);
        } else {
            const double aIn  = goertzel_amp(sine, (inLen - winIn) / 2, winIn, 1000.0, (double)inR);
            const double aOut = goertzel_amp(out, (out.size() - winOut) / 2, winOut, 1000.0, (double)outR);
            const double dDb = 20.0 * std::log10(aOut / aIn);
            char d[320];
            std::snprintf(d, sizeof d,
                          "1 kHz 幅度 输入 %.6f / 输出 %.6f -> 差 %+.4f dB（能量差同值）",
                          aIn, aOut, dDb);
            std::printf("  %s\n", d);
            check(std::fabs(dDb) < 0.5, "1 kHz 能量差 < 0.5 dB");
        }
    }

    std::printf("\n=== 补充：冲尾巴的补零足够（IR 末尾不被截断）===\n");
    {
        // 判据二/三对"末尾少算几毫秒"不敏感（那段能量很小），所以单列一条。
        // 补零不足时，输出最后 T/2 附近的样本会是 0（raw 不足被 resize 补零），
        // 尾部 RMS 会塌掉。这里比较"最后 5 ms / 全长"的 RMS 比值。
        const uint32_t inR = 44100, outR = 96000;
        const size_t inLen = 8820;
        const auto dec = make_decay_noise(inLen, inR);
        const auto out = rta::dsp::ir_resample(dec, inR, outR);
        const size_t tailIn = 220;     // 5 ms @44.1k
        const size_t tailOut = 480;    // 5 ms @96k
        if (out.size() < tailOut) {
            check(false, "输出长度足够取尾部窗口");
        } else {
            const double rIn  = rms(dec, inLen - tailIn, tailIn) / rms(dec, 0, inLen);
            const double rOut = rms(out, out.size() - tailOut, tailOut) / rms(out, 0, out.size());
            size_t zeros = 0;   // 补零不足时 raw 不够长，会被 resize(need, 0.0) 补零 => 末尾是一段"精确的 0"
            for (size_t n = out.size() - tailOut; n < out.size(); ++n)
                if (out[n] == 0.0) ++zeros;
            char d[320];
            std::snprintf(d, sizeof d,
                          "尾部 5 ms RMS / 全长 RMS：输入 %.5f / 输出 %.5f（比值 %.3f），"
                          "尾部精确 0 的样本 %zu 个",
                          rIn, rOut, rOut / rIn, zeros);
            std::printf("  %s\n", d);
            // 正确补零时该比值实测 1.000（带限只去掉 22.05~21.4 kHz 那一点，比值几乎不变）；
            // 补 T/4 时实测 0.652（最后 2.9 ms 被整段补零）。0.95 是"不影响判定"的余量，
            // 不是为了让某个实现通过而放宽的阈值。
            check(rOut > 0.95 * rIn, "尾部能量没有塌掉（补零足够）");
            check(zeros == 0, "最后 5 ms 没有整段被补零");
        }
    }

    std::printf("\n=== 判据四：同采样率逐位相同 ===\n");
    {
        std::vector<double> x(1000);
        uint32_t s = 0xDEADBEEFu;
        for (size_t n = 0; n < x.size(); ++n) {
            s = s * 1664525u + 1013904223u;
            x[n] = ((double)(s >> 8) / 8388608.0 - 1.0) * 0.7;
        }
        const auto y = rta::dsp::ir_resample(x, 48000, 48000);
        const bool same = y.size() == x.size() && std::equal(x.begin(), x.end(), y.begin());
        char d[160];
        std::snprintf(d, sizeof d, "输入 %zu 样本 @48000，输出 %zu 样本，逐位%s",
                      x.size(), y.size(), same ? "相同" : "不同");
        std::printf("  %s\n", d);
        check(same, "inRate==outRate 时逐位透传（不滤波、不平移、不缩放）");
    }

    std::printf("\n=== 防御路径（不参与判据，只防崩溃/静默放大）===\n");
    {
        std::vector<double> x(64, 0.0);
        x[3] = 0.5;
        const bool r0 = (rta::dsp::ir_resample(x, 0, 96000) == x);
        const bool r1 = (rta::dsp::ir_resample(x, 44100, 0) == x);
        const bool r2 = rta::dsp::ir_resample(std::vector<double>{}, 44100, 96000).empty();
        std::vector<double> big((size_t(1) << 20) + 1, 0.0);   // 1M+1 样本
        big[7] = 0.25;
        const auto rb = rta::dsp::ir_resample(big, 44100, 96000);
        const bool r3 = (rb.size() == big.size() && std::equal(big.begin(), big.end(), rb.begin()));
        check(r0 && r1, "采样率为 0 时原样返回");
        check(r2, "空 IR 返回空");
        check(r3, ">1M 样本原样返回（由调用方从长度看出来，见头文件说明）");
    }

    std::printf("\n%s（失败 %d 项）\n", fails == 0 ? "全部通过" : "有失败项", fails);
    return fails == 0 ? 0 : 1;
}
