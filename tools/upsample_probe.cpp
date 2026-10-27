
// tool_upsample_probe.cpp — 升频质量量化探针（只读工具，不碰音频设备）
//
// 目的：把"竞品那个 Upsampling 开关到底有多强"变成可比数字。
//   * 用本仓真实的滤波器家族（filter_families.h / resampler.h 的参数口径）
//   * 扫描各种抽头长度，输出：-3dB 点、过渡带宽度、阻带最深、镜像压制
//   * 只走数学：设计 FIR → FFT → 归一化到输入 Nyquist，不加载文件、不开设备（铁律 1）
//
// 判据口径（刻意选得不重复、不互相等价）：
//   -3dB 点      ：通带结束，决定"高频有没有被提前砍掉"
//   过渡带宽度   ：-0.1dB 到 -100dB 的频率跨度（Hz，按 44.1k 源折算）
//   阻带最深     ：stopband 内最大旁瓣（dB），看"混叠能压多深"
//   一次镜像压制 ：第一个镜像（input_nyq + f）在 1 kHz 激励下的绝对电平
//
// 注意：这是【频域理论值】，不是听感结论。听感差异不在本工具的判据范围内（铁律 3）。
#include "dsp/filter_design.h"
#include "dsp/kaiser.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <complex>

using namespace rta::dsp;

static const double kPi2 = 6.283185307179586476925286766559;

// 迭代 radix-2 FFT（工具内部自用，与 dsp/fft.h 无关，避免依赖其内部类型）
static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -kPi2 / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

struct Analysis {
    double f3dbHz   = 0;    // -3 dB 点（相对 44.1k 输入）
    double f01dbHz  = 0;    // -0.1 dB 点
    double f100dbHz = 0;    // -100 dB 点
    double transHz  = 0;    // f100db - f01db
    double stopMaxDb= 0;    // 阻带（>= inputNyq）内最大电平
    double img1kDb  = 0;    // 1 kHz 激励下，第一个镜像处的电平
};

// 在"内插域"原型上做分析。L = 输出/输入倍数（这里固定 ×8 = 352.8k，输入 44.1k）
// proto 归一化：直流增益 = 1（内插域），对应"每输入样本"的增益要乘 L。
static Analysis analyze(const std::vector<double>& proto, int L, double inRate) {
    const double inNyq = inRate / 2.0;          // 22.05 kHz
    // 零填充到 >= 8M 点，保证频率分辨率 <= ~1 Hz（按内插域 352.8k 计）
    size_t n = 1;
    while (n < proto.size() * 8 || n < (1u << 23)) n <<= 1;
    std::vector<std::complex<double>> a(n, {0.0, 0.0});
    for (size_t i = 0; i < proto.size(); ++i) a[i] = {proto[i], 0.0};
    fft(a);

    // 内插域采样率 = inRate * L。bin -> 归一化频率（0..0.5 = 内插域 Nyquist）
    const double fsI = inRate * (double)L;
    const double normIn = 1.0 / (double)L;   // 输入 Nyquist 在内插域的位置

    auto magDbAtNorm = [&](double fNorm) {           // fNorm 以 fsI 归一化（0..0.5）
        double binF = fNorm;
        double pos = binF * (double)n;
        size_t i0 = (size_t)std::floor(pos);
        const double frac = pos - (double)i0;
        size_t i1 = (i0 + 1) % n;
        const double m0 = std::abs(a[i0]), m1 = std::abs(a[i1]);
        const double m = m0 * (1.0 - frac) + m1 * frac;
        return 20.0 * std::log10(std::max(m, 1e-30));
    };
    // 直流增益（内插域）= sum(proto)。归一化到 0 dB。
    double dc = 0.0;
    for (double v : proto) dc += v;
    const double dcDb = 20.0 * std::log10(std::max(dc, 1e-30));
    auto rel = [&](double fHz) { return magDbAtNorm(fHz / fsI) - dcDb; };

    Analysis r{};
    // 以 1 Hz 步进扫描通带边沿
    for (double f = inNyq * 0.5; f < inNyq * 1.4; f += 1.0) {
        const double d = rel(f);
        if (r.f01dbHz == 0 && d <= -0.1) r.f01dbHz = f;
        if (r.f3dbHz  == 0 && d <= -3.0) r.f3dbHz  = f;
        if (r.f100dbHz == 0 && d <= -100.0) { r.f100dbHz = f; break; }
    }
    r.transHz = (r.f100dbHz > 0 && r.f01dbHz > 0) ? (r.f100dbHz - r.f01dbHz) : -1.0;

    // 阻带：输入 Nyquist 起算，但跳过截止点自身的过渡余量（此处理论值恰为 −6 dB 半幅点，
    // 把它算进"阻带最大"会让所有抽头长度都读出 −6 dB —— 这是判据错误，不是滤波器属性）。
    // 起点取 输入Nyquist × 1.01（约 +220 Hz），落在过渡带之外。
    double worst = -1e9;
    for (double f = inNyq * 1.01; f < 0.5 * fsI * 0.98; f += 5.0) worst = std::max(worst, rel(f));
    r.stopMaxDb = worst;

    // 1 kHz 激励的镜像位置：inNyq + 1k（镜像族 = L*inRate - f）
    r.img1kDb = rel(inNyq + 1000.0);
    return r;
}

int main(int argc, char** argv) {
    struct Case { const char* label; int taps; double attenDb; };
    const Case cases[] = {
        {"taps=64    (linear-ish)",       64, 100.0},
        {"taps=256   (DAC 常见插值档)",   256, 100.0},
        {"taps=1024",                    1024, 100.0},
        {"taps=4096",                    4096, 120.0},
        {"taps=16384",                  16384, 140.0},
        {"taps=65536",                  65536, 200.0},
        {"taps=262144",                262144, 200.0},
    };
    const int L = 8;              // 44.1k -> 352.8k
    const double inRate = 44100.0;

    std::printf("升频质量扫描：44.1 kHz -> 352.8 kHz (L=%d)，Kaiser 窗线性相位，按每输入样本抽头数计\n", L);
    std::printf("过渡带以【44.1k 输入域 Hz】表示；-3dB/-0.1dB/-100dB 均为相对直流增益\n\n");
    std::printf("%-28s %10s %10s %10s %10s %10s %10s\n",
                "抽头数", "-3dB(Hz)", "-0.1dB(Hz)", "-100dB(Hz)", "过渡带(Hz)", "阻带最大(dB)", "1k镜像(dB)");
    std::printf("%s\n", std::string(112, '-').c_str());
    for (const auto& c : cases) {
        // 原型长度 = taps_per_input * L（与 resampler.h 的 minTapsPerInput 口径一致）
        LowpassSpec spec;
        spec.taps    = c.taps * L;
        // cutoff 以 Nyquist 为单位（1.0 = 内插域 Nyquist = L*inRate/2）。
        // 输入 Nyquist 在内插域只有 1/L 的位置，故 cutoff = 1/L（例如 L=8 → 0.125）。
        spec.cutoff  = 1.0 / (double)L;
        spec.attenDb = c.attenDb;
        auto h = design_lowpass(spec);
        Analysis a = analyze(h, L, inRate);
        std::printf("%-28s %10.1f %10.1f %10.1f %10.1f %12.1f %10.1f\n",
                    c.label, a.f3dbHz, a.f01dbHz, a.f100dbHz, a.transHz, a.stopMaxDb, a.img1kDb);
    }
    std::printf("\n说明：本工具只给频域理论值。听感差异不在判据范围内（铁律 3）。\n");
    return 0;
}