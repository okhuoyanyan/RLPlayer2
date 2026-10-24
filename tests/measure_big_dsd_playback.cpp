// measure_big_dsd_playback.cpp -- attribution harness for "big DSD file playback stalls"
//
// Not registered in ctest (CMakeLists is owned elsewhere); build ad hoc:
//   clang++ -std=c++20 -O2 -Isrc tests/measure_big_dsd_playback.cpp -o tests/measure_big_dsd_playback.exe
//
// What it measures (same code paths as rtaweb's /api/play producer):
//   phase 1  read_any()                whole-file decode (DSD native -> PCM int32)
//   phase 2  ISP scan over full planes whole-file inter-sample-peak scan (2 ch x 8 B x frames)
//   phase 3  Pipeline 4096-frame chunks resampler design + per-block DSP, time to first block
// It prints working set / peak working set after every phase and writes one MEM line per phase.

#include "audio/audio_loader.h"
#include "audio/wav.h"
#include "dsp/oversample_peak.h"
#include "dsp/resampler.h"
#include "engine/pipeline.h"

#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using rta::audio::AudioBuffer;
namespace eng = rta::engine;

static double ws_mb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) return 0.0;
    return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
}
static double peak_mb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0.0;
    return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
}
static double now_ms() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}
static void mem(const char* stage, const std::string& extra = std::string()) {
    std::printf("  MEM stage=%-12s ws=%8.1f MB peak=%8.1f MB %s\n", stage, ws_mb(), peak_mb(), extra.c_str());
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    std::string file = (argc > 1) ? argv[1]
                                  : std::string("build/testdata/user_ginshoku.dsf");
    std::printf("measure_big_dsd_playback file=%s\n", file.c_str());
    std::printf("  ws0=%.1f MB peak0=%.1f MB\n", ws_mb(), peak_mb());

    double t = now_ms();
    rta::audio::LoadNote note;
    AudioBuffer in = rta::audio::read_any(file, &note);
    const double decodeMs = now_ms() - t;
    if (in.rate == 0 || in.frames() == 0) { std::printf("  decode FAILED: %s\n", note.text.c_str()); return 2; }
    const double bufMb = (double)in.raw.size() * sizeof(int32_t) / (1024.0 * 1024.0);
    std::printf("  DECODE ms=%.0f rate=%u ch=%d bits=%d frames=%zu buf=%.1f MB note=%s\n",
                decodeMs, in.rate, in.channels, in.bits, in.frames(), bufMb, note.text.c_str());
    mem("decoded", "bufMb=" + std::to_string((long long)bufMb));

    // ---- phase 2: whole-file ISP scan (this is what rtaweb.cpp lines ~2133-2150 do) ----
    t = now_ms();
    std::vector<std::vector<double>> planes;
    double srcPeak = 0.0;
    const bool wantIsp = true;
    if (wantIsp) {
        planes.resize((size_t)in.channels, std::vector<double>(in.frames()));
        for (size_t n = 0; n < in.frames(); ++n)
            for (int c = 0; c < in.channels; ++c) {
                const double v = in.sample(n, c);
                planes[(size_t)c][n] = v;
                if (std::fabs(v) > srcPeak) srcPeak = std::fabs(v);
            }
    }
    const double fillMs = now_ms() - t;
    t = now_ms();
    rta::dsp::IspReport rep = rta::dsp::detect_isp(planes);
    const double ispMs = now_ms() - t;
    const double planesMb = (double)planes.size() * (double)in.frames() * 8.0 / (1024.0 * 1024.0);
    std::printf("  ISP fillMs=%.0f scanMs=%.0f planes=%.1f MB peak=%.4f over=%lld srcPeak=%.4f\n",
                fillMs, ispMs, planesMb, rep.peak, rep.overCount, srcPeak);
    mem("isp", "planesMb=" + std::to_string((long long)planesMb));

    // ---- phase 3: pipeline, 4096-frame chunks (rtaweb producer kChunk = 4096) ----
    eng::PipelineConfig cfg;
    cfg.outRate = 96000;
    cfg.outBits = 24;
    cfg.volumeEnabled = true;
    cfg.volumeDb = -60.0;
    cfg.gpuResample = true;
    cfg.twoStage = rta::audio::dsd_pcm_settings().twoStage;
    t = now_ms();
    auto pl = std::make_unique<eng::Pipeline>(cfg);
    const size_t kChunk = 4096;
    size_t pos = 0, outFrames = 0, blocks = 0;
    double tFirst = -1.0, dspTotal = 0.0;
    // Cap the pipeline phase: GPU resampling 384 s of DSD64 would take minutes and this
    // harness only needs "time to first block" + steady per-block cost.
    const double capSec = (argc > 2) ? std::atof(argv[2]) : 10.0;
    const size_t capFrames = (size_t)(capSec * (double)in.rate);
    while (pos < in.frames() && pos < capFrames) {
        const size_t n = std::min(kChunk, in.frames() - pos);
        AudioBuffer c;
        c.rate = in.rate; c.channels = in.channels; c.bits = in.bits; c.isFloat = in.isFloat;
        c.raw.assign(in.raw.begin() + (long long)(pos * (size_t)in.channels),
                     in.raw.begin() + (long long)((pos + n) * (size_t)in.channels));
        pos += n;
        const double b0 = now_ms();
        eng::PipelineStats pst;
        AudioBuffer o = pl->process(c, &pst);
        dspTotal += now_ms() - b0;
        ++blocks;
        outFrames += o.frames();
        if (tFirst < 0.0) { tFirst = now_ms() - t; std::printf("  DSP first-block ms=%.0f gpuResample=%d\n", tFirst, (int)pst.usedResample); }
    }
    std::printf("  PIPELINE firstBlockMs=%.0f totalMs=%.0f dspMs=%.0f blocks=%zu outFrames=%zu\n",
                tFirst, now_ms() - t, dspTotal, blocks, outFrames);
    mem("pipeline");
    std::printf("RESULT file=%s decodeMs=%.0f bufMb=%.0f ispMs=%.0f firstBlockMs=%.0f peakMb=%.1f\n",
                file.c_str(), decodeMs, bufMb, fillMs + ispMs, tFirst, peak_mb());
    return 0;
}
