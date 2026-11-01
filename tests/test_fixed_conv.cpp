// tests/test_fixed_conv.cpp — int64/int128 精确卷积验收（261030-01 D-11）
//
// 判据（铁律 3：判据必须真的能证明要证明的事）：
//   ① 整数累加**零舍入** —— 用「与逐项整数求和的一致性」证明，不是「看起来差不多」
//   ② 同一份量化数据比较 double 与正确舍入的精确整数和，不混入输入量化误差
//   ③ 系数量化误差 ≤ 2^-62（约 −373 dB）—— 这是「精确」的边界，必须写清
#include "dsp/fixed_conv.h"
#include "dsp/filter_families.h"
#include "dsp/filter_design.h"
#include "dsp/resampler.h"   // 第 5 节：重采样器接线验证
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <string>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace rta::dsp;

static int g_fails = 0;
static void ck(bool cond, const char* desc, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", desc);
    else { std::printf("  [FAIL] %s %s\n", desc, detail.c_str()); ++g_fails; }
}

// 参考实现：逐项整数求和（同样的 __int128，但**分组**累加再合并）
// 若定点累加有舍入，分组结果会不同；整数加法满足结合律 ⟹ 必须完全相等。
static __int128 ref_dot_grouped(const std::vector<int64_t>& x, const std::vector<int64_t>& h, size_t group) {
    __int128 total = 0;
    for (size_t s = 0; s < x.size(); s += group) {
        __int128 part = 0;
        const size_t e = std::min(x.size(), s + group);
        for (size_t i = s; i < e; ++i) part += (__int128)x[i] * (__int128)h[i];
        total += part;
    }
    return total;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_fixed_conv: 定点卷积精确性验收 ===\n");

    // 独立整数参考：Python float((1<<80)+(1<<63)+(1<<27)+1)/2**86。
    // 旧 hi/lo 分别转 double 会把最后的 +1 丢掉，误落在 ties-to-even 边界。
    {
        const __int128 midpoint = ((__int128)1 << 80) + ((__int128)1 << 63) + ((__int128)1 << 27);
        const double below = 0x1.0000800000000p-6;
        const double above = 0x1.0000800000001p-6;
        ck(fixed_to_double(midpoint - 1) == below, "int128 输出舍入：半格以下");
        ck(fixed_to_double(midpoint) == below, "int128 输出舍入：恰好半格取偶数");
        ck(fixed_to_double(midpoint + 1) == above, "int128 输出舍入：半格以上不丢最后一位");
        ck(fixed_to_double(-midpoint - 1) == -above, "int128 输出舍入：负数对称");
        ck(fixed_to_double(0) == 0.0, "int128 输出舍入：零");
        ck(fixed_to_double(((__int128)1 << (kFixedCoeffBits + kFixedInputBits))) == 1.0, "int128 输出舍入：单位增益");
    }

    {
        std::vector<int64_t> q;
        ck(fixed_coeffs_from_double({-1.9, 0.0, 1.9}, q), "Q62实际放大系数±1.9可由int64表示");
        ck(!fixed_coeffs_from_double({2.0}, q), "Q62系数+2.0越int64界时明确拒绝");
        ck(kFixedCoeffBits == 62, "系数使用Q62，不退回曾失败的Q50");
    }
    std::mt19937_64 rng(20261030);

    // ---- 1. 精确性：分组求和必须与顺序求和完全相等（整数加法结合律） ----
    {
        const size_t n = 2048;
        std::vector<int64_t> x(n), h(n);
        std::uniform_int_distribution<int64_t> dx(-(1 << 23), (1 << 23));
        std::uniform_int_distribution<int64_t> dh(-(1LL << 62), (1LL << 62));
        for (size_t i = 0; i < n; ++i) { x[i] = dx(rng); h[i] = dh(rng); }
        const __int128 a = fixed_dot(x.data(), h.data(), n);
        const __int128 b = ref_dot_grouped(x, h, 64);
        ck(a == b, "定点累加与分组求和完全相等（零舍入）");
        // 注意：不能写 (double)a —— Windows clang 没有 __floattidf，必须走 fixed_to_double
        ck(fixed_to_double(a) == fixed_to_double(b), "两种求和转 double 后也相等");
    }

    // ---- 2. 同一份已量化输入/系数：double 累加 vs 正确舍入的整数精确和 ----
    // Windows ABI 的 long double 仍是 53 位，不能当作独立高精度参考。
    // 此处只报告两条算术路径的差，不再把输入量化误差冒充“精度提升”。
    std::printf("\n  %6s %20s\n", "T", "double-vs-exact dBFS");
    for (int T : {32, 96, 211, 528, 1024, 2199}) {
        std::vector<int64_t> xi(T), hi(T);
        std::uniform_int_distribution<int64_t> dx(-(1LL << 23), (1LL << 23));
        std::uniform_int_distribution<int64_t> dh(-(1LL << 40), (1LL << 40));
        double sd = 0.0;
        for (int i = 0; i < T; ++i) {
            xi[i] = dx(rng); hi[i] = dh(rng);
            sd += ((double)xi[i] / (double)kFixedInputOne) *
                  ((double)hi[i] / (double)kFixedCoeffOne);
        }
        const double exactRounded = fixed_to_double(fixed_dot(xi.data(), hi.data(), xi.size()));
        const double delta = std::fabs(sd - exactRounded);
        std::printf("  %6d %20.1f\n", T, delta ? 20.0 * std::log10(delta) : -INFINITY);
    }

    // ---- 3. 系数量化误差上界：≤ 2^-62 ----
    {
        FirSpec s;
        s.taps = 4096; s.cutoff = 0.25; s.attenDb = 280.0; s.window = WindowKind::Kaiser;
        auto h = design_fir(s);
        std::vector<int64_t> hi;
        const bool ok = fixed_coeffs_from_double(h, hi);
        ck(ok, "4096 抽头 Kaiser 原型可量化到 62 bit 系数");
        double worst = 0.0;
        for (size_t i = 0; i < h.size(); ++i) {
            const double back = (double)hi[i] / (double)kFixedCoeffOne;
            worst = std::max(worst, std::fabs(back - h[i]));
        }
        const double bound = 1.0 / (double)kFixedCoeffOne;   // 2^-62
        ck(worst <= bound, "系数量化误差 <= 2^-62",
           std::to_string(worst) + " vs " + std::to_string(bound));
        std::printf("  [INFO] 系数量化误差 %.3e（= %.1f dB）\n", worst,
                   20.0 * std::log10(worst > 0 ? worst : 1e-300));
    }

    // ---- 3b. 【适用范围】输入不是 24 bit 整数时，定点反而更差 ----
    //   这是 D-11 的边界：定点只在「输入恰好是 k/2^23」时精确。
    //   一旦过音量/EQ，样本变成任意 double，转 int24 就丢 −144 dB。
    {
        const size_t n = 512;
        std::uniform_real_distribution<double> ux(-1.0, 1.0), uh(-0.002, 0.002);
        std::vector<double> xd(n), hd(n);
        for (size_t i = 0; i < n; ++i) { xd[i] = ux(rng); hd[i] = uh(rng); }
        double sd = 0.0;
        for (size_t i = 0; i < n; ++i) sd += xd[i] * hd[i];
        std::vector<int64_t> xi(n), hi(n);
        for (size_t i = 0; i < n; ++i) {
            xi[i] = (int64_t)std::llround(xd[i] * (double)kFixedInputOne);
            hi[i] = (int64_t)std::llround(hd[i] * (double)kFixedCoeffOne);
        }
        const double sf = fixed_to_double(fixed_dot(xi.data(), hi.data(), n));
        const double delta = std::fabs(sf - sd);
        std::printf("  [INFO] 任意 double 输入被量化后的输出差：%.3e\n", delta);
        ck(delta > 1e-12, "任意 double 输入重新量化会丢信息，定点仅适用于整数 PCM");
    }

    // ---- 4. int64 累加是否够用的守卫必须诚实 ----
    {
        const bool fits = fixed_fits_int64(1, kFixedCoeffOne);
        ck(!fits, "24bit 输入 x 62bit 系数，T=1 就已超出 int64 ⟹ 必须用 int128");
    }

    // ---- 5. 重采样器接线：定点路径必须忠实复现 double 路径（差 <= -180 dB）----
    {
        ResamplerConfig c1;
        c1.inRate = 44100; c1.outRate = 176400; c1.channels = 1;
        apply_preset(c1, *find_preset("poly-sinc-short-lp", false));
        ResamplerConfig c2 = c1;
        c2.useFixed = true;
        PolyphaseResampler rD(c1), rF(c2);
        const size_t nIn = 512;
        std::vector<double> x(nIn);
        std::uniform_int_distribution<int> d24(-(1 << 22), (1 << 22));
        for (size_t i = 0; i < nIn; ++i) x[i] = (double)d24(rng) / (double)(1 << 23);  // 24bit 精确
        std::vector<double> oD(rD.maxOutFrames(nIn) + 16), oF(rF.maxOutFrames(nIn) + 16);
        const double* ip[1] = { x.data() };
        double* opD[1] = { oD.data() };
        double* opF[1] = { oF.data() };
        const size_t nD = rD.process(ip, nIn, opD);
        const size_t nF = rF.process(ip, nIn, opF);
        ck(nD == nF && nD > 0, "定点路径输出帧数与 double 一致", std::to_string(nD) + "/" + std::to_string(nF));
        double mx = 0.0;
        for (size_t i = 0; i < std::min(nD, nF); ++i) mx = std::max(mx, std::fabs(oD[i] - oF[i]));
        const double db = 20.0 * std::log10(mx > 0 ? mx : 1e-300);
        // 【判据说明（铁律 3）】这里比的是「定点 vs double」。差异**由 double 侧主导**：
        //   定点路径本身零舍入（§1 已用分组求和证明），系数量化底 −373 dB；
        //   而 double 路径的累加误差约 −290 dB ⟹ 两者之差必然停在这个量级。
        //   所以 −180 dB 的阈值证明的是「接线忠实、没有算错相位/索引」，
        //   不是「定点比 double 精确」——后者由内核级证据（分组求和相等 + 量化底）成立。
        ck(db <= -180.0, "定点与 double 输出差异 <= -180 dB（接线忠实，差异由 double 侧主导）",
           std::to_string(db) + " dB");
        std::printf("  [INFO] 重采样器定点 vs double: 最大差 %.3e（%.1f dB）\n", mx, db);
    }

    std::printf("=== test_fixed_conv: %d 失败 ===\n", g_fails);
    return g_fails ? 1 : 0;
}
