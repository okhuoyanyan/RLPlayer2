#pragma once
#include "fp_strict.h"
#include "resampler_simd.h"
#include <bit>
#include <cstdint>
#include <cmath>
#include <limits>
#include <algorithm>
namespace rta::dsp {
// Exact replacement for floor(x*fs+.5)/fs followed by setSample(): fs is
// a power of two, so the finite integral intermediate survives that roundtrip.
// Check AFTER multiplication: finite x can overflow and must produce silence.
inline int32_t pack_pcm_scalar(double x,int bits) {
    const double fs=double(1u<<(bits-1));
    double value=std::floor(x*fs+.5);
    if((std::bit_cast<uint64_t>(value)&0x7ff0000000000000ULL)==0x7ff0000000000000ULL)return 0;
    value=std::max(-fs,std::min(fs-1,value));
    return (int32_t)value;
}
#if RTA_RESAMPLER_AVX2
__attribute__((target("avx2,no-fma")))
inline __m128i pack_pcm_four(const double* input,__m256d fs,__m256d lo,__m256d hi) {
    __m256d value=_mm256_floor_pd(_mm256_add_pd(_mm256_mul_pd(_mm256_loadu_pd(input),fs),_mm256_set1_pd(.5)));
    const __m256d abs=_mm256_andnot_pd(_mm256_set1_pd(-0.0),value);
    const __m256d finite=_mm256_cmp_pd(abs,_mm256_set1_pd(std::numeric_limits<double>::max()),_CMP_LE_OQ);
    value=_mm256_and_pd(value,finite);
    value=_mm256_min_pd(hi,_mm256_max_pd(lo,value));
    return _mm256_cvttpd_epi32(value);
}
__attribute__((target("avx2,no-fma"),noinline))
inline size_t pack_pcm_avx2(const double* const* planes,int channels,size_t frames,int bits,int32_t* output) {
    const double scale=double(1u<<(bits-1));
    const __m256d fs=_mm256_set1_pd(scale),lo=_mm256_set1_pd(-scale),hi=_mm256_set1_pd(scale-1);
    const size_t end=frames-(frames%4);
    for(size_t n=0;n<end;n+=4) {
        if(channels==1)_mm_storeu_si128((__m128i*)(output+n),pack_pcm_four(planes[0]+n,fs,lo,hi));
        else if(channels==2) {
            const __m128i left=pack_pcm_four(planes[0]+n,fs,lo,hi),right=pack_pcm_four(planes[1]+n,fs,lo,hi);
            _mm_storeu_si128((__m128i*)(output+n*2),_mm_unpacklo_epi32(left,right));
            _mm_storeu_si128((__m128i*)(output+n*2+4),_mm_unpackhi_epi32(left,right));
        } else for(int c=0;c<channels;++c) {
            int32_t temp[4];_mm_storeu_si128((__m128i*)temp,pack_pcm_four(planes[c]+n,fs,lo,hi));
            for(size_t lane=0;lane<4;++lane)output[(n+lane)*(size_t)channels+c]=temp[lane];
        }
    }
    return end;
}
#endif
// Returns actual SIMD sample count, permitting tests to verify dispatch.
inline size_t pack_pcm_planes(const double* const* planes,int channels,size_t frames,int bits,int32_t* output,bool allowSimd=true) {
    size_t begin=0;
#if RTA_RESAMPLER_AVX2
    if(allowSimd && resampler_avx2_available())begin=pack_pcm_avx2(planes,channels,frames,bits,output);
#else
    (void)allowSimd;
#endif
    for(size_t n=begin;n<frames;++n)for(int c=0;c<channels;++c)
        output[n*(size_t)channels+c]=pack_pcm_scalar(planes[c][n],bits);
    return begin*(size_t)channels;
}
}
