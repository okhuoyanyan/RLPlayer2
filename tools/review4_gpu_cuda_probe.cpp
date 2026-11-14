#define NOMINMAX
#include <cstdio>
#include <windows.h>
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  HMODULE h = LoadLibraryA(argv[1]);
  if (!h) {
    printf("CUDA DLL load=0 winerror=%lu\n", GetLastError());
    return 1;
  }
  auto avail = (int (*)())GetProcAddress(h, "rta_gpu_available");
  auto legacy = GetProcAddress(h, "rta_gpu_resample");
  auto compact = GetProcAddress(h, "rta_gpu_resample_compact_planar");
  if (!avail || !legacy)
    return 1;
  int active = avail();
  printf("CUDA DLL load=1 available=%d legacy_export=%d compact_export=%d "
         "expected_no_NVIDIA_hardware=1\n",
         active, legacy != nullptr, compact != nullptr);
  return active == 0 ? 0 : 1;
}
