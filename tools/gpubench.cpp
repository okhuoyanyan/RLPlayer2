#include <hip/hip_runtime.h>
#include <cstdio>

// Pure FMA throughput kernels (8 independent FMAs per thread per iteration)
__global__ void fma32(float* o, float a, float b, int iters) {
  float x0=a,x1=a+1,x2=a+2,x3=a+3,x4=a+4,x5=a+5,x6=a+6,x7=a+7;
  for (int i=0;i<iters;i++){
    x0=fmaf(x0,b,a); x1=fmaf(x1,b,a); x2=fmaf(x2,b,a); x3=fmaf(x3,b,a);
    x4=fmaf(x4,b,a); x5=fmaf(x5,b,a); x6=fmaf(x6,b,a); x7=fmaf(x7,b,a);
  }
  int t=blockIdx.x*blockDim.x+threadIdx.x;
  o[t]=x0+x1+x2+x3+x4+x5+x6+x7;
}
__global__ void fma64(double* o, double a, double b, int iters) {
  double x0=a,x1=a+1,x2=a+2,x3=a+3,x4=a+4,x5=a+5,x6=a+6,x7=a+7;
  for (int i=0;i<iters;i++){
    x0=fma(x0,b,a); x1=fma(x1,b,a); x2=fma(x2,b,a); x3=fma(x3,b,a);
    x4=fma(x4,b,a); x5=fma(x5,b,a); x6=fma(x6,b,a); x7=fma(x7,b,a);
  }
  int t=blockIdx.x*blockDim.x+threadIdx.x;
  o[t]=x0+x1+x2+x3+x4+x5+x6+x7;
}
// float2 = emulated double (double-float) FMA: 3 fp32 fmas for mul, 3 for add
__global__ void fmaDF(float2* o, float2 a, float2 b, int iters) {
  float2 x0=a,x1=a,x2=a,x3=a;
  for (int i=0;i<iters;i++){
    #define DFMA(X) { float p=X.x*b.x; float e=fmaf(X.x,b.x,-p)+X.x*b.y+X.y*b.x; X.x=p+e; X.y=e; }
    DFMA(x0) DFMA(x1) DFMA(x2) DFMA(x3)
  }
  int t=blockIdx.x*blockDim.x+threadIdx.x;
  o[t]=x0;
}

int main(){
  int dev; hipGetDevice(&dev);
  hipDeviceProp_t p; hipGetDeviceProperties(&p, dev);
  printf("GPU: %s  SMs=%d  clock=%d kHz\n", p.name, p.multiProcessorCount, p.clockRate);

  int blocks = p.multiProcessorCount * 8 * 8;  // saturate
  int threads = 256;
  long long total = (long long)blocks * threads;
  int iters = 20000;

  float* fo; hipMalloc(&fo, total*4); double* dо; hipMalloc(&dо, total*8); float2* dfo; hipMalloc(&dfo, total*8);
  hipEvent_t e0, e1; hipEventCreate(&e0); hipEventCreate(&e1);
  float ms;

  for (int rep=0; rep<2; rep++) {
    hipEventRecord(e0); fma32<<<blocks,threads>>>(fo,1.0f,1.0000001f,iters); hipEventRecord(e1);
    hipEventSynchronize(e1); hipEventElapsedTime(&ms,e0,e1);
    if(rep) printf("FP32 FMA: %8.2f GFLOPS\n", 2.0*total*iters*8/ms/1e6);

    hipEventRecord(e0); fma64<<<blocks,threads>>>(dо,1.0,1.0000001,iters); hipEventRecord(e1);
    hipEventSynchronize(e1); hipEventElapsedTime(&ms,e0,e1);
    if(rep) printf("FP64 FMA: %8.2f GFLOPS\n", 2.0*total*iters*8/ms/1e6);

    hipEventRecord(e0); fmaDF<<<blocks,threads>>>(dfo,make_float2(1.0f,0.0f),make_float2(1.0000001f,0.0f),iters/4); hipEventRecord(e1);
    hipEventSynchronize(e1); hipEventElapsedTime(&ms,e0,e1);
    if(rep) printf("float2-emulated-double FMA: %8.2f GFLOPS(emulated)\n", 2.0*total*(long long)(iters/4)*4/ms/1e6);
  }
  return 0;
}
