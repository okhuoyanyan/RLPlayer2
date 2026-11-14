#define NOMINMAX
#include "dsp/resampler.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <psapi.h>
#include <stdexcept>
#include <windows.h>
using namespace rta::dsp;
static void need(bool ok, const char *what) {
  if (!ok)
    throw std::runtime_error(what);
}
static PROCESS_MEMORY_COUNTERS_EX memory() {
  PROCESS_MEMORY_COUNTERS_EX m{};
  m.cb = sizeof(m);
  need(GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m,
                            sizeof(m)) != 0,
       "GetProcessMemoryInfo");
  return m;
}
static void printmem(const char *stage) {
  auto m = memory();
  printf("MEM stage=%s working_bytes=%zu peak_working_bytes=%zu "
         "private_bytes=%zu peak_pagefile_bytes=%zu working_MiB=%.6f "
         "peak_working_MiB=%.6f private_MiB=%.6f peak_pagefile_MiB=%.6f\n",
         stage, (size_t)m.WorkingSetSize, (size_t)m.PeakWorkingSetSize,
         (size_t)m.PrivateUsage, (size_t)m.PeakPagefileUsage,
         m.WorkingSetSize / 1048576., m.PeakWorkingSetSize / 1048576.,
         m.PrivateUsage / 1048576., m.PeakPagefileUsage / 1048576.);
}
struct Pair {
  ResamplerConfig cfg;
  std::unique_ptr<PolyphaseResampler> cpu, gpu;
  size_t offset = 0, calls = 0, outputs = 0, different = 0, nonzero = 0;
  double maxAbs = 0;
  size_t compact = 0, gpuCalls = 0;
  Pair(ResamplerConfig c) : cfg(c) {
    cfg.useFixed = false;
    cfg.cpuKernel = ResamplerCpuKernel::Scalar;
    cfg.cpuParallel = false;
    cpu = std::make_unique<PolyphaseResampler>(cfg);
    gpu = std::make_unique<PolyphaseResampler>(cfg);
    gpu->set_use_gpu(true);
    need(gpu->gpu_active(), "GPU DLL unavailable");
  }
  void step(size_t count) {
    const int ch = cfg.channels;
    size_t capacity =
        std::max(cpu->maxOutFrames(count), gpu->maxOutFrames(count));
    std::vector<std::vector<double>> in(ch, std::vector<double>(count)),
        a(ch, std::vector<double>(capacity, -998.)),
        b(ch, std::vector<double>(capacity, -997.));
    std::vector<const double *> ip(ch);
    std::vector<double *> ap(ch), bp(ch);
    for (int c = 0; c < ch; c++) {
      for (size_t j = 0; j < count; j++) {
        size_t n = offset + j;
        uint64_t x =
            (n + 1) * 6364136223846793005ULL + 1442695040888963407ULL * (c + 1);
        in[c][j] = n % 173 == 0
                       ? 0.875
                       : (double)((int)((x >> 32) & 65535) - 32768) / 65536.;
      }
      ip[c] = in[c].data();
      ap[c] = a[c].data();
      bp[c] = b[c].data();
    }
    auto na = cpu->process(ip.data(), count, ap.data()),
         nb = gpu->process(ip.data(), count, bp.data());
    need(na == nb, "CPU/GPU frame mismatch");
    if (nb) {
      need(gpu->gpu_all_stages_used_last_block(), "GPU fallback");
      gpuCalls += cfg.twoStage ? 2 : 1;
      if (gpu->gpu_compact_stage_mask_last_block())
        compact++;
    }
    for (int c = 0; c < ch; c++)
      for (size_t n = 0; n < na; n++) {
        need(std::isfinite(b[c][n]), "nonfinite GPU output");
        different += std::memcmp(&a[c][n], &b[c][n], 8) != 0;
        maxAbs = std::max(maxAbs, std::abs(a[c][n] - b[c][n]));
        nonzero += b[c][n] != 0.;
      }
    offset += count;
    calls++;
    outputs += nb;
  }
  void report(const char *name) {
    printf("CASE name=%s in=%u out=%u ch=%d T=%d coeff=%zu calls=%zu "
           "gpu_success_calls=%zu compact_blocks=%zu out_frames=%zu "
           "samples=%zu different=%zu maxAbs=%.17g nonzero=%zu\n",
           name, cfg.inRate, cfg.outRate, cfg.channels, gpu->tapsPerPhase(),
           gpu->coefficient_count(), calls, gpuCalls, compact, outputs,
           outputs * cfg.channels, different, maxAbs, nonzero);
    need(different == 0, "GPU differs from scalar");
    need(nonzero > 0, "all zero GPU output");
  }
};
int main(int argc, char **argv) {
  try {
    _putenv_s("RLPLAYER_FILTER_CACHE", "0");
    set_gpu_vendor("amd");
    printf("Review4 version=3.1.12 cache=0 audio_device=NEVER_OPENED "
           "compiler=%s\n",
           __clang_version__);
    if (argc > 1 && std::string(argv[1]) == "memory") {
      ResamplerConfig cfg;
      apply_preset(cfg, *find_preset("sinc-MGa"));
      cfg.inRate = 44100;
      cfg.outRate = 384000;
      cfg.channels = 2;
      cfg.useFixed = false;
      cfg.cpuKernel = ResamplerCpuKernel::Scalar;
      cfg.cpuParallel = false;
      cfg.minTapsPerInput = solve_taps_per_phase(
          cfg.window, cfg.transition, cfg.apodizing, cfg.apodF1, cfg.apodF2,
          cfg.totalTaps, 0, cfg.inRate);
      cfg.maxPrototypeTaps = 0;
      printmem("before_ctor");
      auto t = std::chrono::steady_clock::now();
      {
        PolyphaseResampler r(cfg);
        printf(
            "CTOR preset=sinc-MGa L=%u M=%u T=%d coeff=%zu "
            "coefficient_bytes=%zu seconds=%.6f maxPrototypeTaps=0 gpu=0\n",
            cfg.outRate / std::gcd(cfg.inRate, cfg.outRate),
            cfg.inRate / std::gcd(cfg.inRate, cfg.outRate), r.tapsPerPhase(),
            r.coefficient_count(), r.coefficient_count() * 8,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
                .count());
        printmem("resident_ctor");
      }
      printmem("after_destroy");
      return 0;
    }
    HMODULE h = LoadLibraryA("rtagpu.dll");
    need(h != nullptr, "LoadLibrary rtagpu");
    char path[32768];
    need(GetModuleFileNameA(h, path, sizeof(path)) > 0, "DLL path");
    printf("DLL path=%s compact_export=%d legacy_export=%d\n", path,
           GetProcAddress(h, "rta_gpu_resample_compact_planar") != nullptr,
           GetProcAddress(h, "rta_gpu_resample") != nullptr);
    if (argc > 1 && std::string(argv[1]) == "exports")
      return 0;
    const size_t blocks[] = {1, 7, 31, 257, 3, 511, 1025, 2, 61, 409, 13, 997};
    for (int channels : {1, 2, 3, 6})
      for (int which = 0; which < 4; which++) {
        ResamplerConfig cfg;
        cfg.channels = channels;
        cfg.inRate = which == 2 ? 48000 : which == 3 ? 96000 : 44100;
        cfg.outRate = which == 0 ? 96000 : which == 3 ? 44100 : 384000;
        cfg.minTapsPerInput = which % 2 ? 127 : 37;
        cfg.transition = .11;
        Pair p(cfg);
        for (auto n : blocks)
          p.step(n);
        p.report("layout");
        if (argc > 1 && which == 2)
          need(p.compact == 12, "expected compact path missing");
      }
    for (const char *preset :
         {"poly-sinc-short-lp", "poly-sinc-long-mp", "poly-sinc-long-ip"}) {
      ResamplerConfig cfg;
      apply_preset(cfg, *find_preset(preset));
      cfg.inRate = 44100;
      cfg.outRate = 384000;
      cfg.channels = 2;
      if (cfg.minTapsPerInput == 0)
        cfg.minTapsPerInput = solve_taps_per_phase(
            cfg.window, cfg.transition, cfg.apodizing, cfg.apodF1, cfg.apodF2,
            cfg.totalTaps, 0, cfg.inRate);
      Pair p(cfg);
      for (auto n : blocks)
        p.step(n);
      p.report(preset);
    }
    {
      ResamplerConfig c;
      c.inRate = 44100;
      c.outRate = 705600;
      c.channels = 3;
      c.minTapsPerInput = 95;
      c.stage2Taps = 29;
      c.twoStage = true;
      Pair p(c);
      for (auto n : blocks)
        p.step(n);
      p.report("cascade");
      if (argc > 1)
        need(p.compact == 12, "expected cascade compact path missing");
    }
    {
      ResamplerConfig c;
      c.inRate = 44100;
      c.outRate = 96000;
      c.channels = 3;
      c.minTapsPerInput = 95;
      c.transition = .04;
      Pair a(c);
      c.transition = .26;
      Pair b(c);
      c.window = WindowKind::Gaussian;
      Pair d(c);
      for (auto n : blocks) {
        a.step(n);
        b.step(n);
        d.step(n);
      }
      a.report("alternate-A");
      b.report("alternate-B");
      d.report("alternate-C");
    }
    puts("RESULT errors=0 all_nonempty_blocks_GPU=1 scalar_bitwise_equal=1");
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
