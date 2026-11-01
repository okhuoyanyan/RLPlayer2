// Private test DLL. Never install this over a production backend.
#include <cstddef>
#ifndef FIXTURE_MODE
#define FIXTURE_MODE 0
#endif
extern "C" __declspec(dllexport) int rta_gpu_available(){return 1;}
extern "C" __declspec(dllexport) void rta_gpu_resample_invalidate(){}
extern "C" __declspec(dllexport) int rta_gpu_resample(const double* input,int channels,long long hl,
 const double* coeff,int L,int T,const int* bases,const int* phases,long long produced,double* out){
 bool success=FIXTURE_MODE==3||(FIXTURE_MODE==1&&T==32)||(FIXTURE_MODE==2&&T==64);
 if(!success)return -42;
 for(long long n=0;n<produced;++n)for(int c=0;c<channels;++c){
   const int base=bases[n],phase=phases[n];double acc=0;
   const int end=base+1<T?base+1:T;
   for(int u=0;u<end;++u)acc+=coeff[(size_t)phase*T+u]*input[(size_t)c*hl+base-u];
   out[n*channels+c]=acc;
 }
 return (int)produced;
}
