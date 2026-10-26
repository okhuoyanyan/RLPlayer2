// test_dynamics.cpp — 音频体检 B1a：动态分析器合成信号验证
// 全部信号现场合成、完全离线（不开设备、不解码真实文件），判据确定性可复现。
// 锚点：BS.1770 官方校准点（997 Hz / −23 dBFS / 立体声 ⟹ −23.0 LUFS）必须过。
#include "analysis/dynamics.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace rta::analysis;
using rta::dsp::kPi;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static std::vector<std::vector<double>> make_sine(int channels, size_t n, double fs, double hz, double amp) {
    std::vector<std::vector<double>> ch((size_t)channels, std::vector<double>(n));
    for (int c = 0; c < channels; ++c)
        for (size_t i = 0; i < n; ++i)
            ch[(size_t)c][i] = amp * std::sin(2.0 * kPi * hz * (double)i / fs);
    return ch;
}

static std::vector<const double*> ptrs(const std::vector<std::vector<double>>& ch) {
    std::vector<const double*> p;
    for (const auto& c : ch) p.push_back(c.data());
    return p;
}

// 直接用 ITU 表 48 kHz 系数算级联响应（dB）—— 与重标定实现独立比对，防变换代数写错
static double itu48_response_db(double hz) {
    static const double sb[3] = { 1.53512485958697, -2.69169618940638, 1.19839281085285 };
    static const double sa[3] = { 1.0, -1.69065929318241, 0.73248077421585 };
    static const double hb[3] = { 1.0, -2.0, 1.0 };
    static const double ha[3] = { 1.0, -1.99004745483398, 0.99007225036621 };
    const double w = 2.0 * kPi * hz / 48000.0;
    const double c1 = std::cos(w), s1 = std::sin(w);
    const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    auto mag = [&](const double b[3], const double a[3]) {
        const double nr = b[0] + b[1] * c1 + b[2] * c2, ni = -(b[1] * s1 + b[2] * s2);
        const double dr = 1.0 + a[1] * c1 + a[2] * c2, di = -(a[1] * s1 + a[2] * s2);
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    };
    return 20.0 * std::log10(mag(sb, sa) * mag(hb, ha) + 1e-300);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const double fs = 48000.0;

    printf("=== 1. K 加权系数校验（48 kHz 应与 ITU-R BS.1770-4 表 1 一致）===\n");
    {
        rta::dsp::Biquad shelf, hp;
        k_weight_biquads(fs, shelf, hp);
        double maxDiff = 0.0;
        std::string resp = "(kHz: ";
        for (double hz : {20.0, 100.0, 500.0, 997.0, 4000.0, 10000.0, 20000.0}) {
            const double mine = shelf.response_db(hz / (fs * 0.5)) + hp.response_db(hz / (fs * 0.5));
            const double ref = itu48_response_db(hz);
            maxDiff = std::max(maxDiff, std::fabs(mine - ref));
            char one[64];
            std::snprintf(one, sizeof(one), "%.1f:%+.4f ", hz / 1000.0, mine);
            resp += one;
        }
        printf("  %s]\n", resp.c_str());
        char b[200];
        std::snprintf(b, sizeof(b), "(max|Δ| = %.2e dB，997 Hz K 增益 %+.4f dB)", maxDiff,
                      shelf.response_db(997.0 / (fs * 0.5)) + hp.response_db(997.0 / (fs * 0.5)));
        check(maxDiff < 1e-6, "48 kHz 全频点与规范表一致", b);
    }

    printf("\n=== 2. 官方校准点：997 Hz / −23 dBFS / 立体声 → LUFS = −23.0 ±0.1 ===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = make_sine(2, n, fs, 997.0, std::pow(10.0, -23.0 / 20.0));
        auto p = ptrs(ch);
        const DynamicsResult r = measure_dynamics(p.data(), 2, n, fs);
        char b[200];
        std::snprintf(b, sizeof(b), "(LUFS %.3f, LRA %.3f LU, DR %.3f dB, peak %.3f dBFS)",
                      r.lufsIntegrated, r.lra, r.drDb, r.peakDbfs);
        printf("  %s\n", b);
        check(r.valid, "valid 标志");
        check(std::fabs(r.lufsIntegrated - (-23.0)) <= 0.1, "LUFS = −23.0 ±0.1（官方校准点）", b);
        check(r.lra <= 1.0, "稳态正弦 LRA ≤ 1.0", b);
        check(std::fabs(r.drDb - 3.0) <= 0.5, "DR = 3.0 ±0.5（正弦峰值/RMS = √2）", b);
    }

    printf("\n=== 3. 1 kHz / −12 dBFS / 单声道 → 峰值与 DR ===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        auto ch = make_sine(1, n, fs, 1000.0, std::pow(10.0, -12.0 / 20.0));
        auto p = ptrs(ch);
        const DynamicsResult r = measure_dynamics(p.data(), 1, n, fs);
        char b[200];
        std::snprintf(b, sizeof(b), "(peak %.4f dBFS, DR %.3f dB, LUFS %.3f)",
                      r.peakDbfs, r.drDb, r.lufsIntegrated);
        printf("  %s\n", b);
        check(std::fabs(r.peakDbfs - (-12.0)) <= 0.05, "peakDbfs = −12.0 ±0.05", b);
        check(std::fabs(r.drDb - 3.0) <= 0.5, "DR = 3.0 ±0.5", b);
    }

    printf("\n=== 4. 响度差 20 dB 的两段交替正弦（各 4 s × 6 段）→ LRA > 10 ===\n");
    {
        const double segSec = 4.0, segs = 6.0;
        const size_t segN = (size_t)(segSec * fs);
        const size_t n = segN * (size_t)segs;
        std::vector<std::vector<double>> ch(1, std::vector<double>(n));
        const double aLoud = std::pow(10.0, -12.0 / 20.0), aQuiet = std::pow(10.0, -32.0 / 20.0);
        for (size_t i = 0; i < n; ++i) {
            const size_t seg = i / segN;                       // 0,2,4 响；1,3,5 轻
            const double amp = (seg % 2 == 0) ? aLoud : aQuiet;
            ch[0][i] = amp * std::sin(2.0 * kPi * 997.0 * (double)i / fs);   // 相位连续
        }
        auto p = ptrs(ch);
        const DynamicsResult r = measure_dynamics(p.data(), 1, n, fs);
        char b[200];
        std::snprintf(b, sizeof(b), "(LRA %.3f LU, 积分 %.3f LUFS)", r.lra, r.lufsIntegrated);
        printf("  %s\n", b);
        check(r.lra > 10.0, "LRA > 10（20 dB 交替应拉开分布）", b);
    }

    printf("\n=== 5. 满幅方波 → DR = 0 ±0.5（peak/RMS = 1）===\n");
    {
        const size_t n = (size_t)(10.0 * fs);
        std::vector<std::vector<double>> ch(1, std::vector<double>(n));
        for (size_t i = 0; i < n; ++i) ch[0][i] = (i % 48 < 24) ? 1.0 : -1.0;   // 1 kHz 方波 ±满幅
        auto p = ptrs(ch);
        const DynamicsResult r = measure_dynamics(p.data(), 1, n, fs);
        char b[200];
        std::snprintf(b, sizeof(b), "(DR %.4f dB, peak %.4f dBFS, LUFS %.3f)",
                      r.drDb, r.peakDbfs, r.lufsIntegrated);
        printf("  %s\n", b);
        check(std::fabs(r.drDb - 0.0) <= 0.5, "DR = 0 ±0.5", b);
        check(std::fabs(r.peakDbfs) < 1e-9, "peak = 0 dBFS", b);
    }

    printf("\n=== 6. K 加权重标定跨采样率校准（44.1/96/192k 同一校准点）===\n");
    {
        // Tustin 重标定的意义就在这里：任何 fs 下官方校准点都必须守住
        for (double rate : {44100.0, 96000.0, 192000.0}) {
            const size_t n = (size_t)(10.0 * rate);
            auto ch = make_sine(2, n, rate, 997.0, std::pow(10.0, -23.0 / 20.0));
            auto p = ptrs(ch);
            const DynamicsResult r = measure_dynamics(p.data(), 2, n, rate);
            char b[128];
            std::snprintf(b, sizeof(b), "(fs=%.0f, LUFS %.3f)", rate, r.lufsIntegrated);
            check(std::fabs(r.lufsIntegrated - (-23.0)) <= 0.1, "LUFS = −23.0 ±0.1", b);
        }
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
