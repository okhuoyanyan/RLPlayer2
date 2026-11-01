// Developer-only real-HIP allocation/copy fault injection. No production switch.
#include <hip/hip_runtime.h>
static int mallocFault=0,copyFault=0,hits=0;
template<class T> static hipError_t injectedMalloc(T**pointer,size_t bytes){
 if(mallocFault>0&&--mallocFault==0){++hits;return hipErrorOutOfMemory;}
 return hipMalloc(pointer,bytes);
}
static hipError_t injectedMemcpy(void*dst,const void*src,size_t bytes,hipMemcpyKind kind){
 if(copyFault>0&&--copyFault==0){
  ++hits;
  // Simulate a failed H2D transfer that already changed part of the device
  // buffer; returning an error before copying would not exercise stale cache.
  if(kind==hipMemcpyHostToDevice&&bytes>=16)(void)hipMemcpy(dst,src,bytes/2,kind);
  return hipErrorInvalidValue;
 }
 return hipMemcpy(dst,src,bytes,kind);
}
#define hipMalloc injectedMalloc
#define hipMemcpy injectedMemcpy
#include "../src/gpu/gpu_resample_hip.cpp"
#undef hipMalloc
#undef hipMemcpy
extern "C" __declspec(dllexport) void fixture_fault(int allocation,int copy){mallocFault=allocation;copyFault=copy;}
extern "C" __declspec(dllexport) int fixture_fault_hits(){return hits;}
__global__ void fixture_constant(double*p){p[threadIdx.x]=1.25;}
extern "C" __declspec(dllexport) int fixture_probe(double*out){
 double* d=nullptr;if(hipMalloc(&d,4*sizeof(double))!=hipSuccess)return -1;
 (void)hipGetLastError();hipLaunchKernelGGL(fixture_constant,dim3(1),dim3(4),0,0,d);
 hipError_t a=hipGetLastError(),b=hipDeviceSynchronize(),c=hipMemcpy(out,d,4*sizeof(double),hipMemcpyDeviceToHost);hipFree(d);
 return a==hipSuccess&&b==hipSuccess&&c==hipSuccess?0:-1;
}
extern "C" __declspec(dllexport) int rta_gpu_available(){double v[4]{};return fixture_probe(v)==0&&v[0]==1.25&&v[1]==1.25&&v[2]==1.25&&v[3]==1.25;}
