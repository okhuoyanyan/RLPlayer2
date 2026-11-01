#include "dsp/fixed_resampler_simd.h"
#include <array>
#include <chrono>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
#include <bit>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
using namespace rta::dsp;
namespace experiment = rta::dsp::fixed_resampler_simd;

namespace {
double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetProcessTimes failed");
    auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) * 1e-7;
#else
    return 0;
#endif
}

std::array<uint64_t, 3> memory_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    return {uint64_t(counters.WorkingSetSize), uint64_t(counters.PeakWorkingSetSize),
            uint64_t(counters.PrivateUsage)};
#else
    return {};
#endif
}

int benchmark(const char* destination, bool scalar_control = false) {
    if (!resampler_avx2_available()) return 10;
    std::ostringstream rows;
    bool first_row = true;
    constexpr size_t groups = 16384;
    for (int kind = 0; kind < 3; ++kind) {
        const bool temporal = kind != 0;
        const size_t repetitions = temporal ? 8 : 64;
        const size_t taps = kind == 2 ? 1409 : 211;
        const size_t step = kind == 1 ? 147 : 160;
        std::mt19937_64 rng(2802026);
        const size_t group_step = temporal ? 4 * step : 1;
        std::vector<int64_t> h(taps * 4), x(taps + groups * group_step + 3 * step);
        for (auto& coefficient : h) coefficient = std::bit_cast<int64_t>(rng());
        for (auto& sample : x) sample = int64_t(rng() % (2 * kFixedInputOne + 1)) - kFixedInputOne;
        auto execute = [&](bool requested_simd) {
            const bool simd = requested_simd && !scalar_control;
            uint64_t hash = 14695981039346656037ULL;
            for (size_t repetition = 0; repetition < repetitions; ++repetition)
            for (size_t group = 0; group < groups; ++group) {
                __int128 acc[4];
                const size_t base = taps - 1 + group * group_step;
                if (temporal) experiment::temporal4(h.data(), x.data(), base, taps, step, acc, simd);
                else experiment::phase4(h.data(), 4, x.data(), base, taps, acc, simd);
                for (auto value : acc) {
                    hash ^= std::bit_cast<uint64_t>(fixed_to_double(value));
                    hash *= 1099511628211ULL;
                }
            }
            return hash;
        };
        const uint64_t expected = execute(false);
        if (execute(true) != expected) return 11;
        for (int round = 0; round < 5; ++round) {
            for (int position = 0; position < 2; ++position) {
                const bool simd = (round + position) % 2 != 0;
                const double cpu_before = process_cpu_seconds();
                const auto wall_before = std::chrono::steady_clock::now();
                const uint64_t hash = execute(simd);
                const double wall = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - wall_before).count();
                const double cpu = process_cpu_seconds() - cpu_before;
                const auto memory = memory_bytes();
                if (hash != expected) return 12;
                if (!first_row) rows << ",";
                first_row = false;
                rows << "{\"kind\":\"" << (temporal ? "temporal" : "phase")
                     << "\",\"taps\":" << taps << ",\"step\":" << (temporal ? step : 0)
                     << ",\"round\":" << round << ",\"simd\":" << (simd ? "true" : "false")
                     << ",\"wall_seconds\":" << wall << ",\"process_cpu_seconds\":" << cpu
                     << ",\"working_set_bytes\":" << memory[0]
                     << ",\"peak_working_set_bytes\":" << memory[1]
                     << ",\"private_bytes\":" << memory[2]
                     << ",\"output_samples\":" << groups * repetitions * 4
                     << ",\"output_hash\":\"" << hash << "\"}";
                std::cout << "kind=" << kind << " round=" << round << " simd=" << simd
                          << " wall=" << wall << " cpu=" << cpu << " hash=" << hash << '\n';
            }
        }
    }
    std::ofstream out(destination);
    out << "{\"prototype_only\":true,\"hashes_match\":true,\"scalar_only_negative_control\":"
        << (scalar_control ? "true" : "false") << ",\"rounds\":5,\"rows\":["
        << rows.str() << "]}\n";
    return out ? 0 : 13;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--bench") return benchmark(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--bench-negative") return benchmark(argv[2], true);
    uint64_t compared = 0, simd_calls = 0;
    const std::array<int64_t, 14> boundary{
        INT64_MIN, INT64_MAX, 0, 1, -1, INT32_MIN, INT32_MAX,
        int64_t(UINT32_MAX), int64_t(1) << 32, -(int64_t(1) << 32),
        INT64_MAX - INT32_MAX, INT64_MIN + INT32_MAX,
        int64_t(0x7fffffff80000000ULL), std::bit_cast<int64_t>(0x8000000080000000ULL)};
    const std::array<int64_t, 7> source{-kFixedInputOne, kFixedInputOne,
        kFixedInputOne - 2, -kFixedInputOne + 2, -1, 0, 1};
    std::mt19937_64 rng(2802026);
    auto check = [&](const std::vector<int64_t>& h, size_t stride,
                     const std::vector<int64_t>& x, size_t base, size_t taps) {
        __int128 actual[4], scalar[4];
        const bool used = experiment::phase4(h.data(), stride, x.data(), base, taps, actual);
        experiment::phase4(h.data(), stride, x.data(), base, taps, scalar, false);
        simd_calls += used;
        for (size_t lane = 0; lane < 4; ++lane) {
            // Independent scalar accumulation, not the prototype's reference helper.
            __int128 expected = 0;
            for (size_t u = 0; u < taps; ++u)
                expected += (__int128)x[base - u] * h[u * stride + lane];
            if (actual[lane] != expected || scalar[lane] != expected) return false;
            if (std::bit_cast<uint64_t>(fixed_to_double(actual[lane])) !=
                std::bit_cast<uint64_t>(fixed_to_double(expected))) return false;
            ++compared;
        }
        return true;
    };
    // Extreme coefficients force both signed-low corrections and high=2^31 cases.
    for (size_t taps : {0, 1, 2, 7, 32, 63, 64, 65, 127, 128, 129, 211}) {
        const size_t base = taps + 5;
        std::vector<int64_t> h(std::max<size_t>(1, taps * 7)), x(base + 1);
        for (int64_t sample : source) {
            std::fill(x.begin(), x.end(), sample);
            for (int64_t coefficient : boundary) {
                std::fill(h.begin(), h.end(), coefficient);
                if (!check(h, 7, x, base, taps)) return 1;
            }
        }
        for (int repeat = 0; repeat < 100; ++repeat) {
            for (auto& coefficient : h) coefficient = std::bit_cast<int64_t>(rng());
            for (auto& sample : x) sample = int64_t(rng() % (2 * kFixedInputOne + 1)) - kFixedInputOne;
            if (!check(h, 7, x, base, taps)) return 2;
        }
    }
    // Same phase, distinct histories: rational recurrence at base + lane*M.
    for (size_t step : {1, 2, 147, 160}) {
        for (size_t taps : {0, 1, 2, 7, 32, 63, 64, 65, 127, 128, 129, 211}) {
            const size_t base = taps ? taps - 1 : 0;
            std::vector<int64_t> h(std::max<size_t>(taps, 1)), x(base + 3 * step + 1);
            auto check_temporal = [&] {
                __int128 actual[4], scalar[4];
                simd_calls += experiment::temporal4(h.data(), x.data(), base, taps, step, actual);
                experiment::temporal4(h.data(), x.data(), base, taps, step, scalar, false);
                for (size_t lane = 0; lane < 4; ++lane) {
                    __int128 expected = 0;
                    for (size_t u = 0; u < taps; ++u)
                        expected += (__int128)h[u] * x[base + lane * step - u];
                    if (actual[lane] != expected || scalar[lane] != expected) return false;
                    if (std::bit_cast<uint64_t>(fixed_to_double(actual[lane])) !=
                        std::bit_cast<uint64_t>(fixed_to_double(expected))) return false;
                    ++compared;
                }
                return true;
            };
            for (int64_t sample : source) {
                std::fill(x.begin(), x.end(), sample);
                for (int64_t coefficient : boundary) {
                    std::fill(h.begin(), h.end(), coefficient);
                    if (!check_temporal()) return 7;
                }
            }
            for (int repeat = 0; repeat < 100; ++repeat) {
                for (auto& coefficient : h) coefficient = std::bit_cast<int64_t>(rng());
                for (auto& sample : x) sample = int64_t(rng() % (2 * kFixedInputOne + 1)) - kFixedInputOne;
                if (!check_temporal()) return 8;
            }
        }
    }
    for (size_t step : {size_t(0), SIZE_MAX}) {
        __int128 output[4];
        try { experiment::temporal4(nullptr, nullptr, 0, 1, step, output); return 9; }
        catch (const std::invalid_argument&) {}
    }
    // Invalid dimensions must reject before dereferencing any pointer.
    for (int kind = 0; kind < 4; ++kind) {
        __int128 output[4];
        try {
            const size_t stride = kind == 0 ? 3 : kind == 3 ? SIZE_MAX : 4;
            const size_t taps = kind == 2 ? size_t(INT_MAX) + 1 : 2;
            const size_t base = kind == 1 ? 0 : taps;
            experiment::phase4(nullptr, stride, nullptr, base, taps, output);
            return 6;
        } catch (const std::invalid_argument&) {}
    }
    // Exact half-ULP ties and carries, positive/negative, including int128 extremes.
    uint64_t rounding_checks = 0;
    for (int bits = 54; bits <= 126; ++bits) {
        const __int128 half = (__int128)1 << (bits - 54);
        for (int odd : {0, 1}) for (int delta : {-1, 0, 1}) for (int sign : {-1, 1}) {
            const uint64_t significand = (uint64_t(1) << 52) + odd;
            const __int128 magnitude = (__int128)significand * (half * 2) + half + delta;
            const uint64_t rounded = significand + (delta > 0 || (delta == 0 && odd));
            const double expected = std::ldexp(double(rounded) * sign, bits - 53 - 86);
            if (std::bit_cast<uint64_t>(fixed_to_double(magnitude * sign)) !=
                std::bit_cast<uint64_t>(expected)) return 3;
            ++rounding_checks;
        }
    }
    const __int128 minimum = (__int128)((unsigned __int128)1 << 127);
    const __int128 maximum = (__int128)(((unsigned __int128)1 << 127) - 1);
    if (fixed_to_double(minimum) != -std::ldexp(1.0, 41) ||
        fixed_to_double(maximum) != std::ldexp(1.0, 41)) return 4;
    if (resampler_avx2_available() && simd_calls == 0) return 5;
    std::cout << "exact_lane_comparisons=" << compared << " simd_calls=" << simd_calls
              << " rounding_cases=" << rounding_checks + 2 << " PASS\n";
    if (argc == 2) {
        std::ofstream out(argv[1]);
        out << "{\"pass\":true,\"seed\":2802026,\"exact_lane_comparisons\":" << compared
            << ",\"simd_calls\":" << simd_calls << ",\"rounding_cases\":" << rounding_checks + 2
            << ",\"production_integrated\":false,\"performance_measured\":false}\n";
    }
}
