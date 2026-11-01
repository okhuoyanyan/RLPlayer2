// Private CPU fixture: original ABI works; compact is absent or deliberately fails.
#include "dsp/fp_strict.h"
#define FIXTURE_MODE 3
#include "test_dsp_gpu_observation_fixture.cpp"
#ifndef COMPACT_EXPORT
#define COMPACT_EXPORT 0
#endif
#if COMPACT_EXPORT
extern "C" __declspec(dllexport) int rta_gpu_resample_compact_planar(const double*,int,long long,
 const double*,int,int,int,long long,long long,long long,long long,double*const*){return -42;}
#endif
