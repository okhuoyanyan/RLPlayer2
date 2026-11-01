#include "dsp/fft.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#include <cstdlib>
#include <new>

// Test-only allocation observation/failure injection is local to this executable.
namespace fft_allocation_probe {
thread_local size_t count = 0;
thread_local size_t bytes = 0;
thread_local bool fail_next = false;
}
void* operator new(std::size_t bytes) {
    if (fft_allocation_probe::fail_next) {
        fft_allocation_probe::fail_next = false;
        throw std::bad_alloc();
    }
    ++fft_allocation_probe::count;
    fft_allocation_probe::bytes = bytes;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace rta::dsp;
namespace {
void direct_root_fft(std::vector<cplx>& a, bool inverse = false) {
    const size_t n = a.size();
    if (n < 2) return;
    if (n & (n - 1)) throw std::invalid_argument("FFT length must be a power of two");
    // 位反转置换
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    // Direct roots avoid the length-dependent drift of repeated w *= wl.
    // A single table is shared by every stage of this transform.
    std::vector<cplx> roots(n / 2);
    for (size_t k = 0; k < roots.size(); ++k) {
        const double angle = -2.0 * kPi * ((double)k / (double)n);
        roots[k] = cplx(std::cos(angle), std::sin(angle));
    }
    roots[0] = cplx(1.0, 0.0);
    if (n >= 4) roots[n / 4] = cplx(0.0, -1.0);
    for (size_t len = 2;; len <<= 1) {
        const size_t stride = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < len / 2; ++k) {
                const cplx w = inverse ? std::conj(roots[k * stride]) : roots[k * stride];
                const cplx u = a[i + k];
                const cplx v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
        if (len == n) break; // Do not overflow size_t after the final stage.
    }
    if (inverse) for (auto& x : a) x /= (double)n;
}


// Frozen recurrence implementation, used only for before/after comparison.
void baseline_fft(std::vector<cplx>& a, bool inverse = false) {
    const size_t n = a.size();
    if (n < 2) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / (double)len * (inverse ? 1.0 : -1.0);
        const cplx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cplx w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                cplx u = a[i + k];
                cplx v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= (double)n;
}


std::vector<cplx> signal(size_t n) {
    std::mt19937 rng(2802026);
    std::vector<cplx> x(n);
    for (auto& z : x) z = double(int32_t(rng() >> 8) - 8388608) / 8388608.0;
    return x;
}

double error(const std::vector<cplx>& a, const std::vector<cplx>& b) {
    double value = 0;
    for (size_t i = 0; i < a.size(); ++i) value = std::max(value, std::abs(a[i] - b[i]));
    return value;
}

bool special_frequencies(size_t n) {
    for (bool inverse : {false, true}) {
        for (int kind : {0, 1}) {
            std::vector<cplx> a(n);
            for (size_t i = 0; i < n; ++i) a[i] = kind && i % 2 ? -1.0 : 1.0;
            fft(a, inverse);
            for (size_t i = 0; i < n; ++i) {
                const size_t peak = kind ? n / 2 : 0;
                const double expected = i == peak ? (inverse ? 1.0 : double(n)) : 0;
                if (a[i] != cplx(expected, 0)) return false;
            }
        }
    }
    return true;
}

bool small_sizes() {
    std::vector<cplx> empty;
    fft(empty);
    fft(empty, true);
    if (!empty.empty()) return false;
    std::vector<cplx> one{{3, -4}};
    fft(one);
    fft(one, true);
    if (one != std::vector<cplx>{{3, -4}}) return false;
    for (size_t n : {2, 4, 8}) {
        const auto x = signal(n);
        auto actual = x;
        fft(actual);
        // A direct DFT verifies forward results independently of the inverse.
        std::vector<cplx> expected(n);
        for (size_t k = 0; k < n; ++k)
            for (size_t j = 0; j < n; ++j)
                expected[k] += x[j] * std::polar(1.0, -2 * kPi * double(k * j) / n);
        if (error(actual, expected) > 2e-14) return false;
        fft(actual, true);
        if (error(actual, x) > 2e-14 || !special_frequencies(n)) return false;
    }
    for (size_t n : {3, 6}) {
        auto a = signal(n);
        const auto before = a;
        try { fft(a); return false; }
        catch (const std::invalid_argument&) { if (a != before) return false; }
    }
    return true;
}

bool concurrent_calls() {
    std::atomic<bool> passed{true};
    std::vector<std::thread> threads;
    for (size_t worker = 0; worker < 4; ++worker) {
        threads.emplace_back([worker, &passed] {
            for (size_t repeat = 0; repeat < 8; ++repeat) {
                constexpr size_t lengths[] = {2, 1024, 65536, 4096, 131072, 1024, 16384, 65536};
                const size_t n = lengths[(worker + repeat) % 8];
                const auto x = signal(n);
                auto a = x, expected = x;
                direct_root_fft(expected);
                fft(a);
                if (std::memcmp(a.data(), expected.data(), n * sizeof(cplx))) passed = false;
                direct_root_fft(expected, true);
                fft(a, true);
                if (std::memcmp(a.data(), expected.data(), n * sizeof(cplx))) passed = false;
                if (error(a, x) > 2e-14) passed = false;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    return passed;
}

void dump(const std::filesystem::path& path, const std::vector<cplx>& a) {
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot create FFT evidence file");
    file.write(reinterpret_cast<const char*>(a.data()), a.size() * sizeof(cplx));
    if (!file) throw std::runtime_error("Cannot write FFT evidence file");
}


bool cache_contract() {
    auto tiny = signal(2);
    fft(tiny); // Establish a different last-size entry deterministically.
    auto a = signal(65536);
    size_t before = fft_allocation_probe::count;
    fft(a);
    const size_t cold = fft_allocation_probe::count - before;
    const size_t coldBytes = fft_allocation_probe::bytes;
    before = fft_allocation_probe::count;
    fft(a);
    const size_t warm = fft_allocation_probe::count - before;
    before = fft_allocation_probe::count;
    fft(a, true);
    const size_t inverse = fft_allocation_probe::count - before;
    std::cout << "CACHE cold_allocations=" << cold << " cold_bytes=" << coldBytes
              << " warm_allocations=" << warm << " inverse_allocations=" << inverse << '\n';
    // MSVC's STL may add up to one cache line for large-allocation alignment.
    if (cold != 1 || coldBytes < 512 * 1024 || coldBytes > 512 * 1024 + 64 ||
        warm != 0 || inverse != 0) return false;
    auto small = signal(1024);
    fft(small);
    auto large = signal(131072);
    before = fft_allocation_probe::count;
    fft(large);
    const size_t largeForward = fft_allocation_probe::count - before;
    before = fft_allocation_probe::count;
    fft(large, true);
    const size_t largeInverse = fft_allocation_probe::count - before;
    before = fft_allocation_probe::count;
    fft(small);
    const size_t retainedSmall = fft_allocation_probe::count - before;
    std::cout << "CACHE large_forward_allocations=" << largeForward
              << " large_inverse_allocations=" << largeInverse
              << " retained_small_allocations=" << retainedSmall << '\n';
    if (largeForward != 1 || largeInverse != 1 || retainedSmall != 0) return false;
    for (size_t failureSize : {4096, 131072}) {
        auto failureInput = signal(failureSize), unchanged = failureInput;
        fft_allocation_probe::fail_next = true;
        bool failed = false;
        try { fft(failureInput); }
        catch (const std::bad_alloc&) { failed = true; }
        fft_allocation_probe::fail_next = false;
        const bool preserved = std::memcmp(failureInput.data(), unchanged.data(),
                                            failureInput.size() * sizeof(cplx)) == 0;
        std::cout << "CACHE allocation_failure_size=" << failureSize << " failed=" << failed
                  << " preserves_input=" << preserved << '\n';
        if (!failed || !preserved) return false;
        if (failureSize > 65536) {
            auto retained = signal(4096);
            before = fft_allocation_probe::count;
            fft(retained);
            if (fft_allocation_probe::count != before) return false;
        }
        fft(failureInput); // Both failed cache growth and local allocation retry.
    }
    return true;
}

bool cache_equivalence(const std::filesystem::path* directory) {
    size_t sequence = 0, compared = 0;
    for (size_t n : {1024, 65536, 4096, 131072, 4096, 1024, 16384, 65536}) {
        auto input = signal(n);
        std::mt19937 rng(unsigned(9000 + sequence));
        for (auto& z : input) z.imag(double(int32_t(rng() >> 8) - 8388608) / 8388608.0);
        for (bool inverse : {false, true}) {
            auto expected = input;
            direct_root_fft(expected, inverse);
            for (size_t repeat = 0; repeat < 2; ++repeat) {
                auto actual = input;
                fft(actual, inverse);
                if (std::memcmp(actual.data(), expected.data(), n * sizeof(cplx))) return false;
                compared += n;
                if (directory && repeat == 0) {
                    const auto stem = *directory / ("cache-" + std::to_string(sequence) + "-"
                        + std::to_string(n) + (inverse ? "-inverse" : "-forward"));
                    dump(stem.string() + "-current.bin", actual);
                    dump(stem.string() + "-reference.bin", expected);
                }
            }
        }
        ++sequence;
    }
    std::cout << "CACHE_EQUIVALENCE complex_samples=" << compared << " bit_mismatches=0\n";
    return true;
}

void benchmark() {
    for (size_t n : {1024, 4096, 16384, 65536}) {
        const auto x = signal(n);
        double seconds[2]{};
        volatile double sink = 0;
        for (int mode = 0; mode < 2; ++mode) {
            const auto start = std::chrono::steady_clock::now();
            for (int repeat = 0; repeat < 100; ++repeat) {
                auto a = x;
                if (mode) fft(a);
                else baseline_fft(a);
                sink = sink + a[size_t(repeat) % n].real();
            }
            seconds[mode] = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
        }
        std::cout << n << " baseline_s " << seconds[0] << " new_s " << seconds[1]
                  << " ratio " << seconds[1] / seconds[0] << '\n';
    }
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::setprecision(17);
    if (argc == 2 && std::string(argv[1]) == "--bench") {
        benchmark();
        return 0;
    }
    if (!small_sizes()) return 3;
    if (!concurrent_calls()) return 4;
    if (!cache_contract()) return 5;
    const auto directory = argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
    if (!cache_equivalence(argc == 2 ? &directory : nullptr)) return 6;
    for (size_t n : {1024, 4096, 16384, 65536}) {
        const auto x = signal(n);
        auto old = x, current = x;
        baseline_fft(old);
        fft(current);
        auto old_roundtrip = old, new_roundtrip = current;
        baseline_fft(old_roundtrip, true);
        fft(new_roundtrip, true);
        const double old_error = error(old_roundtrip, x);
        const double new_error = error(new_roundtrip, x);
        std::cout << "n " << n << " old_roundtrip " << old_error
                  << " new_roundtrip " << new_error << '\n';
        if (argc == 2) {
            const auto base = std::filesystem::path(argv[1]) / ("fft-" + std::to_string(n));
            dump(base.string() + "-x.bin", x);
            dump(base.string() + "-old.bin", old);
            dump(base.string() + "-new.bin", current);
        }
        if (new_error > 2e-14 || new_error > old_error) return 1;
        if (!special_frequencies(n)) return 2;
    }
    std::cout << "Small sizes and 32 concurrent transform pairs: PASS\n";
}
