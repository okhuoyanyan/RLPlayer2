#include <cstdio>
#include <hip/hip_runtime.h>
__global__ void review4_constant(double *p) { p[threadIdx.x] = 1.25; }
int main() {
  int count = 0;
  auto e = hipGetDeviceCount(&count);
  printf("DEVICE count=%d status=%d\n", count, (int)e);
  if (e || !count)
    return 1;
  hipDeviceProp_t props{};
  e = hipGetDeviceProperties(&props, 0);
  printf("DEVICE name=%s arch=%s prop_status=%d totalGlobalMem=%zu\n",
         props.name, props.gcnArchName, (int)e, props.totalGlobalMem);
  double *p = nullptr;
  double out[4]{};
  if (hipMalloc(&p, sizeof(out)))
    return 1;
  (void)hipGetLastError();
  hipLaunchKernelGGL(review4_constant, dim3(1), dim3(4), 0, 0, p);
  auto launch = hipGetLastError(), sync = hipDeviceSynchronize(),
       copy = hipMemcpy(out, p, sizeof(out), hipMemcpyDeviceToHost);
  hipFree(p);
  printf("PROBE launch=%d sync=%d copy=%d values=%.2f,%.2f,%.2f,%.2f\n",
         (int)launch, (int)sync, (int)copy, out[0], out[1], out[2], out[3]);
  return launch || sync || copy || out[0] != 1.25 || out[1] != 1.25 ||
         out[2] != 1.25 || out[3] != 1.25;
}
