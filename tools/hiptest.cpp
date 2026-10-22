#include <hip/hip_runtime.h>
#include <cstdio>
#include <cmath>

__global__ void k(const float* __restrict__ in, float* __restrict__ out, int n, float g) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) out[i] = in[i] * g + 1.0f;
}
__global__ void kd(const double* __restrict__ in, double* __restrict__ out, int n, double g) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) out[i] = in[i] * g + 1.0;
}

int main() {
  int n = 1 << 20;
  float *d_in, *d_out;
  hipMalloc(&d_in, n * sizeof(float));
  hipMalloc(&d_out, n * sizeof(float));
  k<<<(n + 255) / 256, 256>>>(d_in, d_out, n, 2.0f);
  hipDeviceSynchronize();
  double *dd_in, *dd_out;
  hipMalloc(&dd_in, n * sizeof(double));
  hipMalloc(&dd_out, n * sizeof(double));
  kd<<<(n + 255) / 256, 256>>>(dd_in, dd_out, n, 2.0);
  hipError_t e = hipDeviceSynchronize();
  printf("HIP OK, sync=%s\n", hipGetErrorString(e));

  // crude throughput probe: FP32 vs FP64 fma
  hipEvent_t a, b; hipEventCreate(&a); hipEventCreate(&b);
  const int iters = 200; int m = 1 << 24;
  float* f; hipMalloc(&f, m * sizeof(float));
  hipEventRecord(a);
  for (int i = 0; i < iters; i++) k<<<(m + 255) / 256, 256>>>(f, f, m, 1.0000001f);
  hipEventRecord(b); hipEventSynchronize(b);
  float ms32 = 0; hipEventElapsedTime(&ms32, a, b);
  printf("FP32: %.1f ms for %d elems\n", ms32, iters * m);

  double* dd; hipMalloc(&dd, m * sizeof(double));
  hipEventRecord(a);
  for (int i = 0; i < iters; i++) kd<<<(m + 255) / 256, 256>>>(dd, dd, m, 1.0000001);
  hipEventRecord(b); hipEventSynchronize(b);
  float ms64 = 0; hipEventElapsedTime(&ms64, a, b);
  printf("FP64: %.1f ms  -> FP64/FP32 ratio = %.1fx slower\n", ms64, ms64 / ms32);
  return 0;
}
