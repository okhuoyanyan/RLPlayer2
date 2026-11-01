#pragma once
#include "fp_strict.h"
#include <cstddef>
#if !defined(RTA_RESAMPLER_DISABLE_AVX2) && defined(__clang__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#if defined(_WIN32)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#define RTA_RESAMPLER_AVX2 1
#else
#define RTA_RESAMPLER_AVX2 0
#endif
namespace rta::dsp {
enum class ResamplerCpuKernel { Auto, Scalar, Avx2 };
inline bool resampler_avx2_available() {
#if RTA_RESAMPLER_AVX2
    static const bool supported=[] {
        unsigned a,b,c,d;
#if defined(_WIN32)
        int regs[4]; __cpuidex(regs,0,0);
        if(regs[0]<7) return false;
        __cpuidex(regs,1,0); c=(unsigned)regs[2];
        if((c & (1u<<27))==0 || (c & (1u<<28))==0) return false;
#else
        if(!__get_cpuid(1,&a,&b,&c,&d) || (c & (1u<<27))==0 || (c & (1u<<28))==0) return false;
#endif
        unsigned lo,hi;
        __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        if((lo & 6)!=6) return false;
#if defined(_WIN32)
        __cpuidex(regs,7,0); b=(unsigned)regs[1];
#else
        if(!__get_cpuid_count(7,0,&a,&b,&c,&d)) return false;
#endif
        return (b & (1u<<5))!=0;
    }();
    return supported;
#else
    return false;
#endif
}
#if RTA_RESAMPLER_AVX2
// Each lane is an independent output. No reduction/reassociation/FMA.
template<int Groups>
__attribute__((target("avx2,no-fma"),noinline))
inline void resampler_phase_group(const double* coeff, size_t stride,
                             const double* history, size_t base, int taps, double* out) {
#pragma clang fp reassociate(off) contract(off) reciprocal(off)
    static_assert(Groups==1 || Groups==2 || Groups==4);
    __m256d a0=_mm256_setzero_pd(),a1=a0,a2=a0,a3=a0;
    for(int u=0;u<taps;++u) {
        const __m256d h=_mm256_set1_pd(history[base-(size_t)u]);
        const double* row=coeff+(size_t)u*stride;
        // Native vector arithmetic is parsed in this strict scope. Intrinsic
        // bodies parsed earlier by platform headers can retain reassoc flags.
        a0=a0+_mm256_loadu_pd(row)*h;
        if constexpr(Groups>=2) a1=a1+_mm256_loadu_pd(row+4)*h;
        if constexpr(Groups>=4) {
            a2=a2+_mm256_loadu_pd(row+8)*h;
            a3=a3+_mm256_loadu_pd(row+12)*h;
        }
    }
    _mm256_storeu_pd(out,a0);
    if constexpr(Groups>=2)_mm256_storeu_pd(out+4,a1);
    if constexpr(Groups>=4){_mm256_storeu_pd(out+8,a2);_mm256_storeu_pd(out+12,a3);}
}
// Equal phases recur every L outputs and advance M input samples. Each lane
// preserves the scalar multiply/add sequence while coefficients are reused.
template<int Groups>
__attribute__((target("avx2,no-fma"),noinline))
inline void resampler_temporal_group(const double* coeff, const double* history,
                                    size_t base, int taps, size_t step,
                                    double* out, size_t outputStride) {
#pragma clang fp reassociate(off) contract(off) reciprocal(off)
    static_assert(Groups==1 || Groups==2 || Groups==4);
    __m256d acc[Groups];
    for(int g=0;g<Groups;++g)acc[g]=_mm256_setzero_pd();
    for(int u=0;u<taps;++u) {
        const __m256d c=_mm256_set1_pd(coeff[u]);
        for(int g=0;g<Groups;++g) {
            const double* h=history+base+size_t(g)*4*step-size_t(u);
            const __m256d x=step==1 ? _mm256_loadu_pd(h)
                : _mm256_setr_pd(h[0],h[step],h[2*step],h[3*step]);
            acc[g]=acc[g]+c*x;
        }
    }
    for(int g=0;g<Groups;++g) {
        double values[4];_mm256_storeu_pd(values,acc[g]);
        for(int j=0;j<4;++j)out[size_t(g*4+j)*outputStride]=values[j];
    }
}
inline void resampler_phase4(const double* coeff,size_t stride,const double* history,size_t base,int taps,double* out) {
    resampler_phase_group<1>(coeff,stride,history,base,taps,out);
}
#endif
}
