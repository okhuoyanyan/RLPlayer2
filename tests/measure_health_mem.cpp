// measure_health_mem.cpp — 大 DSD 体检「整段解码 vs 流式分块」的内存/数值对比工具
//
// 不注册到 ctest（CMakeLists 不在本工作包的文件清单里），按需手工构建：
//   clang++ -std=c++20 -O2 -Isrc tests/measure_health_mem.cpp -o tests/measure_health_mem.exe
//
// 用法：
//   measure_health_mem.exe old  <file>              # 整段路径（read_any + analyze_buffer）
//   measure_health_mem.exe new  <file> [blockFrames] # 流式路径（默认 65536 帧/块）
//
// 判据（铁律 3：判据必须能证明要证明的事）：
//   * 两个模式必须分别跑在【独立进程】里 —— 峰值工作集（PeakWorkingSetSize）是进程级
//     单调量，同进程里先后跑两条路只会得到"两条路的最大值"，证明不了任何一条。
//   * 数值必须逐位相同（dr/lufs/lra/peak 的 double 位相等，cutoff/confidence 同理）；
//     只比"差 0.01 dB"不能证明流式没改口径。
//   * 内存判据落在 PeakWorkingSetSize（OS 记账峰值），不看"跑完后的 WorkingSet"。
#include "analysis/track_health.h"

#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static double peak_mb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0.0;
    return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
}
static double ws_mb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) return 0.0;
    return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(65001);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("usage: measure_health_mem <old|new> <file> [blockFrames]\n");
        return 2;
    }
    const std::string mode = rta::audio::wide_to_utf8(argv[1]);
    const std::string file = rta::audio::wide_to_utf8(argv[2]);
    const size_t block = (argc > 3) ? (size_t)std::strtoull(rta::audio::wide_to_utf8(argv[3]).c_str(), nullptr, 10)
                                    : size_t(65536);
    std::printf("mode=%s block=%zu ws0=%.1f MB peak0=%.1f MB\n", mode.c_str(), block, ws_mb(), peak_mb());

    const auto t0 = std::chrono::steady_clock::now();
    rta::analysis::TrackHealth h;
    if (mode == "old") {
        h = rta::analysis::analyze_file_whole(file);
    } else {
        rta::analysis::HealthStreamOptions opt;
        opt.blockFrames = block;
        opt.stream = true;
        h = rta::analysis::analyze_file(file, opt);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    std::printf("RESULT mode=%s streamed=%d block=%zu status=%s\n", mode.c_str(), (int)h.streamed,
                h.blockFrames, h.status.c_str());
    std::printf("  dr=%.17g\n  lufs=%.17g\n  lra=%.17g\n  peak=%.17g\n  cutoff=%.17g\n  conf=%.17g\n",
                h.drDb, h.lufs, h.lra, h.peakDbfs, h.cutoffHz, h.confidence);
    std::printf("  suspected=%d fake24=%d dur=%.17g fs=%u ch=%d bits=%d\n",
                (int)h.losslessSuspected, (int)h.fake24bit, h.durationSec, h.fs, h.channels, h.bits);
    std::printf("  reason=%s\n", h.reason.c_str());
    if (!h.error.empty()) std::printf("  error=%s\n", h.error.c_str());
    std::printf("MEM mode=%s analysis_ms=%.0f ws=%.1f MB peak_ws=%.1f MB\n",
                mode.c_str(), ms, ws_mb(), peak_mb());
    return h.status == "ready" ? 0 : 1;
}
