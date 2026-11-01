// tools/fp_strict_probe.cpp -- 261030-01 审计：验证 #pragma clang fp 是否真的生效
//
// 同一个源文件编译三次（-D 区分是否包含 fp_strict.h）：
//   strict : -O3 -march=x86-64-v2
//   fast   : -O3 -march=x86-64-v2 -ffast-math ...（发布口径）
//   pragma : fast + -DUSE_FP_STRICT（包含 src/dsp/fp_strict.h）
//
// 对 16 组数据各做一次 528 抽头点积 / 528 次除法 / float 点积，
// 打印 %a 与位模式，便于逐位比较。
#include <cstdio>
#include <cstdint>
#include <cstring>

#ifdef USE_FP_STRICT
#  include "dsp/fp_strict.h"
#endif

static uint64_t bits(double x) { uint64_t u; std::memcpy(&u, &x, 8); return u; }
static uint32_t bitsf(float x) { uint32_t u; std::memcpy(&u, &x, 4); return u; }

// 528 抽头点积：模拟 resampler 的单相内积（系数带衰减 + 交替符号，接近真实滤波器）
__attribute__((noinline)) double dot528(const double* __restrict a, const double* __restrict b) {
    double s = 0.0;
    for (int i = 0; i < 528; ++i) s += a[i] * b[i];
    return s;
}

// 528 次除法：专门探 -freciprocal-math
__attribute__((noinline)) double div528(const double* __restrict a, const double* __restrict b) {
    double s = 0.0;
    for (int i = 0; i < 528; ++i) s += a[i] / b[i];
    return s;
}

// float 版本（DSP 里大量 float 路径：peq / volume / dither）
__attribute__((noinline)) float dot528f(const float* __restrict a, const float* __restrict b) {
    float s = 0.0f;
    for (int i = 0; i < 528; ++i) s += a[i] * b[i];
    return s;
}

int main() {
    static double a[528], b[528];
    static float af[528], bf[528];

    for (int k = 0; k < 16; ++k) {
        uint64_t seed = 0x9e3779b97f4a7c15ull + (uint64_t)k * 0x100000001b3ull;
        auto next = [&]() {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            return (double)(int64_t)(seed >> 11) / 9.007199254740992e15;
        };
        double decay = 1.0;
        for (int i = 0; i < 528; ++i) {
            decay *= 0.997;                       // 模拟滤波器尾部的幅度衰减
            a[i] = next() * decay;
            b[i] = next() * decay + (i & 1 ? 0.25 : -0.125);  // 交替符号，避免 0 分母
            af[i] = (float)a[i];
            bf[i] = (float)b[i];
        }
        double d = dot528(a, b);
        double v = div528(a, b);
        float f = dot528f(af, bf);
        std::printf("k=%02d dot=%016llx div=%016llx dotf=%08lx | dot=%a\n",
                    k, (unsigned long long)bits(d), (unsigned long long)bits(v),
                    (unsigned long)bitsf(f), d);
    }
    return 0;
}
