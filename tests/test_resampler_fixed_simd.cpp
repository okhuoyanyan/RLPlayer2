#include "dsp/resampler.h"
#include <bit>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
using namespace rta::dsp;

int main(int argc, char** argv) {
    uint64_t checks = 0, vector_outputs = 0, fallback_blocks = 0;
    bool observed_parallel = false;
    const bool avx = resampler_avx2_available();
    std::mt19937 rng(2802026);
    for (auto rates : {std::pair{44100u,176400u}, {44100u,88200u}, {44100u,48000u},
                       {44100u,192000u}, {48000u,44100u}, {96000u,44100u},
                       {48000u,24000u}, {48000u,16000u}}) {
        for (int channels : {1, 2, 6}) for (int taps : {7, 32, 211}) {
            ResamplerConfig cfg;
            cfg.inRate = rates.first; cfg.outRate = rates.second;
            cfg.channels = channels; cfg.minTapsPerInput = taps;
            cfg.applyTapFloor = false; cfg.useFixed = true;
            cfg.cpuKernel = ResamplerCpuKernel::Scalar;
            PolyphaseResampler scalar(cfg);
            cfg.cpuKernel = ResamplerCpuKernel::Auto; cfg.cpuParallel = false;
            PolyphaseResampler serial(cfg);
            cfg.cpuParallel = true;
            PolyphaseResampler pooled(cfg);
            bool guard_seen = false;
            for (int pass = 0; pass < 3; ++pass) {
                scalar.reset(); serial.reset(); pooled.reset();
                if (serial.cpu_simd_outputs_last_block() || pooled.cpu_simd_outputs_last_block()) return 1;
                const size_t blocks[] = {1, 3, 17, 129, 8192, 2053, size_t(taps + 33), 4096};
                for (size_t block = 0; block < std::size(blocks); ++block) {
                    const size_t frames = blocks[block], cap = scalar.maxOutFrames(frames);
                    std::vector<std::vector<double>> input(channels, std::vector<double>(frames));
                    std::vector<std::vector<double>> a(channels, std::vector<double>(cap + 8, 123.456));
                    auto b = a, c = a;
                    std::vector<const double*> ip(channels);
                    std::vector<double*> ap(channels), bp(channels), cp(channels);
                    for (int ch = 0; ch < channels; ++ch) {
                        ip[ch] = input[ch].data(); ap[ch] = a[ch].data();
                        bp[ch] = b[ch].data(); cp[ch] = c[ch].data();
                        for (size_t i = 0; i < frames; ++i) {
                            if (block >= 6) input[ch][i] = 0; // full tail, then cleared history
                            else input[ch][i] = double(int32_t(rng() >> 8) - 8388608) / 8388608.0;
                        }
                        if (block == 4) {
                            input[ch][0] = -1; input[ch][1] = 1;
                            input[ch][2] = 1 - 1.0 / 8388608.0;
                            if (pass == 1) { input[ch][100] = 2; input[ch][frames - 1] = -128; }
                            if (pass == 2) {
                                input[ch][100] = -std::ldexp(1.0, 39); // rounds to INT64_MIN
                                input[ch][frames - 1] = 2;
                            }
                        }
                    }
                    const size_t n = scalar.process(ip.data(), frames, ap.data());
                    if (serial.process(ip.data(), frames, bp.data()) != n ||
                        pooled.process(ip.data(), frames, cp.data()) != n) return 2;
                    for (int ch = 0; ch < channels; ++ch) for (size_t i = 0; i < cap + 8; ++i) {
                        const auto expected = std::bit_cast<uint64_t>(a[ch][i]);
                        if (expected != std::bit_cast<uint64_t>(b[ch][i]) ||
                            expected != std::bit_cast<uint64_t>(c[ch][i])) {
                            std::printf("FAIL %u->%u channels=%d taps=%d pass=%d block=%zu i=%zu\n",
                                rates.first, rates.second, channels, taps, pass, block, i);
                            return 3;
                        }
                        checks += 2;
                    }
                    if (scalar.cpu_simd_outputs_last_block() || serial.cpu_parallel_threads_last_block() > 1 ||
                        pooled.cpu_parallel_threads_last_block() > 8 || serial.gpu_used_last_block() ||
                        pooled.gpu_used_last_block()) return 4;
                    if (pass && (block == 4 || block == 5)) {
                        // The unsafe final input is retained in history into the next block.
                        if (serial.cpu_simd_outputs_last_block() || pooled.cpu_simd_outputs_last_block()) return 5;
                        if (pooled.cpu_parallel_threads_last_block() > 1) return 6;
                        ++fallback_blocks; guard_seen = true;
                    }
                    if (block == 7 && avx &&
                        (pooled.downFactor() > 1 || pooled.upFactor() >= 4) &&
                        pooled.cpu_simd_outputs_last_block() == 0) return 10;
                    vector_outputs += pooled.cpu_simd_outputs_last_block();
                    observed_parallel |= pooled.cpu_parallel_threads_last_block() > 1;
                }
            }
            if (!guard_seen) return 7;
        }
    }
    // Preserve legacy llround/int128 results even for exceptional useFixed callers.
    for (double unsafe : {std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN(), std::ldexp(1.0, 40)}) {
        ResamplerConfig cfg;
        cfg.inRate = 44100; cfg.outRate = 176400; cfg.channels = 1;
        cfg.minTapsPerInput = 32; cfg.applyTapFloor = false; cfg.useFixed = true;
        cfg.cpuKernel = ResamplerCpuKernel::Scalar;
        PolyphaseResampler scalar(cfg);
        cfg.cpuKernel = ResamplerCpuKernel::Auto;
        PolyphaseResampler simd(cfg);
        std::vector<double> input(256, 0), a(scalar.maxOutFrames(256)), b(a.size());
        input[100] = unsafe;
        const double* ip[] = {input.data()};
        double* ap[] = {a.data()}; double* bp[] = {b.data()};
        const size_t n = scalar.process(ip, input.size(), ap);
        if (simd.process(ip, input.size(), bp) != n || simd.cpu_simd_outputs_last_block()) {
            std::printf("unsafe guard failure input=%g simd=%zu\n", unsafe, simd.cpu_simd_outputs_last_block());
            return 11;
        }
        for (size_t i = 0; i < n; ++i) {
            if (std::bit_cast<uint64_t>(a[i]) != std::bit_cast<uint64_t>(b[i])) return 12;
            ++checks;
        }
        ++fallback_blocks;
    }
    if (avx && (!vector_outputs || !observed_parallel)) return 8;
    if (!avx && vector_outputs) return 9;
    std::printf("PASS fixed bits=%llu simd_outputs=%llu fallback_blocks=%llu parallel=%d avx=%d\n",
        (unsigned long long)checks, (unsigned long long)vector_outputs,
        (unsigned long long)fallback_blocks, observed_parallel, avx);
    if (argc == 2) {
        std::ofstream out(argv[1]);
        out << "{\"pass\":true,\"bitwise_checks\":" << checks
            << ",\"simd_outputs\":" << vector_outputs << ",\"fallback_blocks\":" << fallback_blocks
            << ",\"observed_parallel\":" << (observed_parallel ? "true" : "false")
            << ",\"avx2\":" << (avx ? "true" : "false") << "}\n";
    }
}
