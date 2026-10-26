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

// 逐位判同（不是"差很小"）：-0.0 与 0.0 视为不同会误报，这里按位比较但放过 ±0 的等价
static bool same_bits(double a, double b) {
    if (a == b) return true;                       // 含 +0/-0、以及完全相同
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

// 复合信号：慢包络 + 多音 + 一段静音，各声道不同 —— 同时压到 DR 分块、LUFS 门限、LRA 分布三条通路
static std::vector<std::vector<double>> make_composite(size_t n, double rate, int channels) {
    std::vector<std::vector<double>> ch((size_t)channels, std::vector<double>(n));
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / rate;
        const double env = 0.55 + 0.45 * std::sin(2.0 * kPi * 0.37 * t);
        if (t > 6.0 && t < 7.2) { for (int c = 0; c < channels; ++c) ch[(size_t)c][i] = 0.0; continue; }
        ch[0][i] = env * (0.30 * std::sin(2.0 * kPi * 997.0 * t)
                        + 0.12 * std::sin(2.0 * kPi * 3121.0 * t)
                        + 0.05 * std::sin(2.0 * kPi * 11023.0 * t));
        if (channels > 1)
            ch[1][i] = 0.60 * env * (0.25 * std::sin(2.0 * kPi * 440.0 * t)
                                   + 0.18 * std::sin(2.0 * kPi * 5003.0 * t));
        for (int c = 2; c < channels; ++c)
            ch[(size_t)c][i] = (c % 2 ? 0.15 : 0.08) * env * std::sin(2.0 * kPi * (700.0 + 500.0 * c) * t);
    }
    return ch;
}

static double max_abs_diff(const DynamicsResult& a, const DynamicsResult& b) {
    double d = 0.0;
    d = std::max(d, std::fabs(a.drDb - b.drDb));
    d = std::max(d, std::fabs(a.lufsIntegrated - b.lufsIntegrated));
    d = std::max(d, std::fabs(a.lra - b.lra));
    d = std::max(d, std::fabs(a.peakDbfs - b.peakDbfs));
    return d;
}

static bool same_all(const DynamicsResult& a, const DynamicsResult& b) {
    return same_bits(a.drDb, b.drDb) && same_bits(a.lufsIntegrated, b.lufsIntegrated)
        && same_bits(a.lra, b.lra) && same_bits(a.peakDbfs, b.peakDbfs) && a.valid == b.valid;
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


    printf("\n=== 7. 增量接口 vs 整段接口：逐位一致（复合信号 × 块大小 1/999/1000/4096/19201/65536/262144）===\n");
    {
        const std::vector<size_t> blocks = {1, 999, 1000, 4096, 19201, 65536, 262144};
        for (double rate : {44100.0, 176400.0}) {         // 176.4k = DSD64 抽取后的实际体检采样率
            const size_t n = (size_t)(9.0 * rate);
            auto ch = make_composite(n, rate, 2);
            auto p = ptrs(ch);
            const DynamicsResult ref = measure_dynamics(p.data(), 2, n, rate);
            bool allSame = true;
            double maxDiff = 0.0;
            char detail[256] = {0};
            for (size_t B : blocks) {
                DynamicsAccumulator acc;
                acc.begin(2, rate);
                for (size_t pos = 0; pos < n; ) {
                    const size_t m = std::min(B, n - pos);
                    const double* sub[2] = { p[0] + pos, p[1] + pos };
                    acc.add_planar(sub, m);
                    pos += m;
                }
                const DynamicsResult r = acc.finalize();
                allSame = allSame && same_all(r, ref);
                maxDiff = std::max(maxDiff, max_abs_diff(r, ref));
                std::snprintf(detail, sizeof(detail), "(fs=%.0f, %zu 块大小全试, max|Δ| = %.17g)", rate, blocks.size(), maxDiff);
            }
            check(allSame && maxDiff == 0.0, "分块结果与整段逐位相同", detail);
            char b[256];
            std::snprintf(b, sizeof(b), "(fs=%.0f: DR %.17g / LUFS %.17g / LRA %.17g / peak %.17g)",
                          rate, ref.drDb, ref.lufsIntegrated, ref.lra, ref.peakDbfs);
            printf("  基准值 %s\n", b);

            // 交错 double 喂法：与平面喂法也必须逐位相同（编排层正是用交错块喂的）
            std::vector<double> il(n * 2);
            for (size_t i = 0; i < n; ++i) { il[i * 2] = ch[0][i]; il[i * 2 + 1] = ch[1][i]; }
            bool ilSame = true;
            double ilDiff = 0.0;
            for (size_t B : blocks) {
                DynamicsAccumulator acc;
                acc.begin(2, rate);
                for (size_t pos = 0; pos < n; pos += B) {
                    const size_t m = std::min(B, n - pos);
                    acc.add_interleaved(il.data() + pos * 2, m);
                }
                const DynamicsResult r = acc.finalize();
                ilSame = ilSame && same_all(r, ref);
                ilDiff = std::max(ilDiff, max_abs_diff(r, ref));
            }
            std::snprintf(detail, sizeof(detail), "(fs=%.0f, 交错喂入 max|Δ| = %.17g)", rate, ilDiff);
            check(ilSame, "交错（interleaved）分块与整段逐位相同", detail);
        }
    }

    printf("\n=== 8. 边界与多声道：短素材 / 全静音 / 6 声道 ===\n");
    {
        struct Case { const char* name; size_t frames; int channels; double amp; };
        const Case cases[] = {
            { "n=0（空）",                 0,                  2, 0.3 },
            { "n=100（<400 ms，valid 应为 false）", 100,          2, 0.3 },
            { "n=1 s（<3 s：DR/LRA=0）",    48000,              2, 0.3 },
            { "全静音 10 s",                480000,             2, 0.0 },
            { "6 声道 5 s",                 240000,             6, 0.3 },
        };
        for (const Case& cs : cases) {
            std::vector<std::vector<double>> ch((size_t)cs.channels, std::vector<double>(cs.frames, 0.0));
            for (int c = 0; c < cs.channels; ++c)
                for (size_t i = 0; i < cs.frames; ++i)
                    ch[(size_t)c][i] = cs.amp * (0.5 + 0.5 * (double)(c + 1) / (double)cs.channels)
                                     * std::sin(2.0 * kPi * (300.0 + 200.0 * c) * (double)i / fs);
            auto p = ptrs(ch);
            const DynamicsResult ref = measure_dynamics(p.data(), cs.channels, cs.frames, fs);
            bool allSame = true;
            double maxDiff = 0.0;
            for (size_t B : {size_t(1), size_t(1000), size_t(65536), size_t(262144)}) {
                DynamicsAccumulator acc;
                acc.begin(cs.channels, fs);
                if (cs.frames == 0) { /* 无样本：仍要能 finalize */ }
                for (size_t pos = 0; pos < cs.frames; pos += B) {
                    const size_t m = std::min(B, cs.frames - pos);
                    std::vector<const double*> sub((size_t)cs.channels);
                    for (int c = 0; c < cs.channels; ++c) sub[(size_t)c] = p[(size_t)c] + pos;
                    acc.add_planar(sub.data(), m);
                }
                const DynamicsResult r = acc.finalize();
                allSame = allSame && same_all(r, ref);
                maxDiff = std::max(maxDiff, max_abs_diff(r, ref));
            }
            char b[256];
            std::snprintf(b, sizeof(b), "(%s: DR %.6f / LUFS %.6f / LRA %.6f / peak %.6f / valid %d, max|Δ| = %g)",
                          cs.name, ref.drDb, ref.lufsIntegrated, ref.lra, ref.peakDbfs, (int)ref.valid, maxDiff);
            check(allSame, std::string("边界一致: ") + cs.name, b);
        }
        // 空指针声道：旧实现整声道跳过，新实现按掩码跳过 —— 结果必须一样
        {
            auto ch = make_composite(48000, fs, 2);
            const double* p2[2] = { ch[0].data(), nullptr };
            const DynamicsResult a = measure_dynamics(p2, 2, 48000, fs);
            DynamicsAccumulator acc;
            const char mask[2] = { 1, 0 };
            acc.begin(2, fs, mask);
            const double* sub[2] = { ch[0].data(), nullptr };
            acc.add_planar(sub, 48000);
            const DynamicsResult b = acc.finalize();
            char d[128];
            std::snprintf(d, sizeof(d), "(DR %.6f vs %.6f, LUFS %.6f vs %.6f)",
                          a.drDb, b.drDb, a.lufsIntegrated, b.lufsIntegrated);
            check(same_all(a, b), "空指针声道（掩码）与旧实现一致", d);
        }
    }

    printf("\n=== 9. 浮点输入重载（add_interleaved_f32）只在 float 源上近似等于 double ===\n");
    {
        const size_t n = (size_t)(4.0 * fs);
        auto ch = make_composite(n, fs, 2);
        std::vector<float> il(n * 2);
        for (size_t i = 0; i < n; ++i) { il[i * 2] = (float)ch[0][i]; il[i * 2 + 1] = (float)ch[1][i]; }
        auto p = ptrs(ch);
        const DynamicsResult ref = measure_dynamics(p.data(), 2, n, fs);
        DynamicsAccumulator acc;
        acc.begin(2, fs);
        acc.add_interleaved_f32(il.data(), n);
        const DynamicsResult r = acc.finalize();
        char b[192];
        std::snprintf(b, sizeof(b), "(LUFS %.6f vs %.6f, peak %.6f vs %.6f)",
                      r.lufsIntegrated, ref.lufsIntegrated, r.peakDbfs, ref.peakDbfs);
        check(std::fabs(r.lufsIntegrated - ref.lufsIntegrated) < 0.01
              && std::fabs(r.peakDbfs - ref.peakDbfs) < 0.01,
              "float 重载与 double 结果的偏差 < 0.01 dB（不是逐位，符合文件头说明）", b);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
