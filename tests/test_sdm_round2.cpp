// Include SIMD intrinsics before fp_strict to reproduce real Windows-header
// include order under release fast-math. Local strict shims must still match.
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
// Independent scalar process() oracle. Private state access is test-only: all
// standard/dependency headers are included before the access-keyword override.
#include "dsp/fp_strict.h"
#include "dsp/kaiser.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#define private public
#include "dsp/modulator.h"
#undef private
#include <bit>
#include <cstdio>
#include <limits>
#if !defined(RTA_DISABLE_SDM_STEREO_SIMD) && (defined(__SSE2__) || defined(_M_X64))
using rta::dsp::SdmConfig;
using rta::dsp::SdmModulator;
template <size_t N>
void pair_simd(SdmModulator& a, SdmModulator& b, const double* x, const double* z, size_t n,
               double* o, double* p) {
    if (!SdmModulator::try_process_binary_stereo_bits_block(a, b, x, z, n,
                                                            [&](size_t i, bool l, bool r) {
                                                                o[i] = l ? 1.0 : -1.0;
                                                                p[i] = r ? 1.0 : -1.0;
                                                            }))
        std::abort();
}
template <class A, class B> bool state(const A& a, const B& b) {
    return !std::memcmp(a.eHist_, b.eHist_, sizeof(a.eHist_)) &&
           !std::memcmp(a.wHist_, b.wHist_, sizeof(a.wHist_)) && a.s_ == b.s_ &&
           a.clamped_ == b.clamped_ && a.overload_ == b.overload_ && a.nonFinite_ == b.nonFinite_;
}
template <size_t N> int test() {
    SdmConfig cfg;
    cfg.order = N;
    cfg.oneBitOutput = true;
    cfg.bypassOrderGuard = true;
    cfg.fs = 44100. * 512;
    cfg.family = rta::dsp::ModulatorFamily::Ec;
    SdmModulator initial(cfg);
    SdmModulator reference(cfg);
    const size_t n = 1048576;
    std::vector<double> x(n), z(n), a(n), b(n), c(n), d(n);
    for (size_t i = 0; i < n; ++i) {
        x[i] = .42 * sin(i * .0031) + .17 * sin(i * .00071);
        z[i] = .41 * sin(i * .0033) + .15 * sin(i * .00077);
    }
    size_t fail = 0;
    for (int rep = 0; rep < 10; ++rep) {
        auto ma = reference, mb = reference;
        auto mc = initial, md = initial;
        auto run = [&](bool simd) {
            if (simd)
                pair_simd<N>(mc, md, x.data(), z.data(), n, c.data(), d.data());
            else {
                for (size_t i = 0; i < n; ++i) {
                    a[i] = ma.process(x[i]);
                    b[i] = mb.process(z[i]);
                }
            }
        };
        if (rep & 1) {
            run(true);
            run(false);
        } else {
            run(false);
            run(true);
        }
        for (size_t i = 0; i < n; ++i)
            fail += a[i] != c[i] || b[i] != d[i];
        fail += !state(ma, mc) || !state(mb, md);
    }
    // Uneven blocks, reset, NaN/Inf, fullscale, impulses, then recovery.
    x[0] = std::numeric_limits<double>::quiet_NaN();
    z[1] = std::numeric_limits<double>::infinity();
    x[2] = -std::numeric_limits<double>::infinity();
    x[3] = 5.;
    z[4] = -5.;
    for (size_t i = 100; i < 10000; ++i) {
        x[i] = sin(i * .023);
        z[i] = -1.;
    }
    auto ma = reference, mb = reference;
    auto mc = initial, md = initial;
    size_t pos = 0, iter = 0;
    size_t lens[] = {1, 7, 16, 31, 257, 4096, 8191, 65537};
    while (pos < n) {
        size_t len = std::min(lens[iter++ % 8], n - pos);
        if (iter == 12) {
            ma.reset();
            mb.reset();
            mc.reset();
            md.reset();
        }
        for (size_t i = pos; i < pos + len; ++i) {
            a[i] = ma.process(x[i]);
            b[i] = mb.process(z[i]);
        }
        pair_simd<N>(mc, md, x.data() + pos, z.data() + pos, len, c.data() + pos, d.data() + pos);
        for (size_t i = pos; i < pos + len; ++i)
            fail += a[i] != c[i] || b[i] != d[i];
        fail += !state(ma, mc) || !state(mb, md);
        pos += len;
    }
    auto ov = mc.overload_ + md.overload_;
    double sum = 0.;
    size_t same = 0;
    for (size_t i = 0; i < 65536; ++i) {
        x[i] = 0.;
        z[i] = 0.;
    }
    for (size_t i = 0; i < 65536; ++i) {
        a[i] = ma.process(x[i]);
        b[i] = mb.process(z[i]);
    }
    pair_simd<N>(mc, md, x.data(), z.data(), 65536, c.data(), d.data());
    for (size_t i = 0; i < 65536; ++i)
        fail += a[i] != c[i] || b[i] != d[i];
    fail += !state(ma, mc) || !state(mb, md);
    for (size_t i = 4096; i < 65536; ++i) {
        sum += c[i];
        same += c[i] == c[i - 1];
    }
    if (std::abs(sum / 61440) >= .01 || double(same) / 61440 >= .9 ||
        mc.overload_ + md.overload_ != ov)
        ++fail;
    printf("VERIFY N=%zu compared=%zu mismatch=%zu recovery_mean=%g same_sign=%g "
           "overload_delta=%lld\n",
           N, n * 22 + 131072, fail, sum / 61440, double(same) / 61440,
           mc.overload_ + md.overload_ - ov);
    return fail ? 1 : 0;
}

int fallback_and_throw() {
    size_t fails = 0, cases = 0, callbacks = 0;
    auto check = [&](SdmConfig ca, SdmConfig cb, bool expected) {
        SdmModulator a(ca), b(cb);
        auto beforeA = a, beforeB = b;
        bool accepted = SdmModulator::try_process_binary_stereo_bits_block(
            a, b, nullptr, nullptr, 0, [&](size_t, double, double) { ++callbacks; });
        fails += accepted != expected || !state(a, beforeA) || !state(b, beforeB);
        ++cases;
    };
    SdmConfig c;
    c.oneBitOutput = true;
    c.order = 5;
    c.bypassOrderGuard = true;
    check(c, c, true);
    auto d = c;
    d.order = 7;
    check(d, d, true);
    check(c, d, false);
    for (int order : {1, 3, 6, 8}) {
        d = c;
        d.order = order;
        check(d, d, false);
    }
    d = c;
    d.ditherLsb = .0005;
    check(c, d, true);
    check(d, c, true);
    d = c;
    d.levels = 16;
    d.oneBitOutput = false;
    check(c, d, false);
    d = c;
    d.family = rta::dsp::ModulatorFamily::Ahm;
    d.oneBitOutput = false;
    check(d, c, false);
    SdmModulator a(c);
    auto before = a;
    fails += SdmModulator::try_process_binary_stereo_bits_block(a, a, nullptr, nullptr, 0,
                                                                [](size_t, double, double) {}) ||
             !state(a, before);
    ++cases;
    for (int order : {5, 7}) {
        c.order = order;
        SdmModulator l(c), r(c), rl(c), rr(c);
        double x[32] = {}, z[32] = {};
        x[0] = std::numeric_limits<double>::quiet_NaN();
        z[1] = 5.;
        x[2] = -5.;
        z[3] = std::numeric_limits<double>::infinity();
        for (int i = 0; i < 17; ++i) {
            rl.process(x[i]);
            rr.process(z[i]);
        }
        bool caught = false;
        try {
            SdmModulator::try_process_binary_stereo_bits_block(l, r, x, z, 32,
                                                               [](size_t i, double, double) {
                                                                   if (i == 16)
                                                                       throw 42;
                                                               });
        } catch (int v) {
            caught = v == 42;
        }
        fails += !caught || !state(l, rl) || !state(r, rr);
        ++cases;
        // Forced invalid histories test the accumulator finite guard and its
        // otherwise-observable wh[0] clearing before the history shift.
        l.eHist_[0] = r.eHist_[0] = rl.eHist_[0] = rr.eHist_[0] =
            std::numeric_limits<double>::infinity();
        double outL[32], outR[32];
        pair_simd<5>(l, r, x, z, 32, outL, outR);
        for (size_t i = 0; i < 32; ++i)
            fails += outL[i] != rl.process(x[i]) || outR[i] != rr.process(z[i]);
        fails += !state(l, rl) || !state(r, rr);
        ++cases;
    }
    fails += callbacks != 0;
    printf("COMPAT cases=%zu callbacks=%zu failures=%zu\n", cases, callbacks, fails);
    return fails ? 1 : 0;
}
int dither_matrix() {
    constexpr size_t frames = 131072;
    std::vector<double> x(frames), z(frames), left(frames), right(frames);
    for (size_t i = 0; i < frames; ++i) {
        x[i] = .42 * std::sin(i * .0031); z[i] = .39 * std::sin(i * .0047);
        if (i >= 1000 && i < 9000) { x[i] = std::sin(i * .023); z[i] = -1.0; }
        if (i > frames * 3 / 4) x[i] = z[i] = 0.0;
    }
    x[11] = std::numeric_limits<double>::quiet_NaN();
    z[12] = std::numeric_limits<double>::infinity();
    x[13] = -std::numeric_limits<double>::infinity();
    x[9000] = 5.0; z[9001] = -5.0;
    size_t cases = 0, compared = 0, failures = 0;
    for (int order : {5, 7}) {
        SdmConfig ca; ca.order = order; ca.oneBitOutput = true; ca.bypassOrderGuard = true;
        ca.family = rta::dsp::ModulatorFamily::Ec; ca.fs = 44100.0 * 512;
        auto cb = ca; cb.fs = 44100.0 * 256; cb.ntfHinf = 1.30;
        SdmModulator baseLeft(ca), baseRight(cb);
        for (double dl : {0.0, .0005, .1}) for (double dr : {0.0, .0005, .1})
        for (uint64_t seed : {uint64_t(1), uint64_t(0x2545F4914F6CDD1Dull), uint64_t(0xdeadbeef12345678ull)}) {
            auto a = baseLeft, b = baseRight, refA = baseLeft, refB = baseRight;
            a.cfg_.ditherLsb = refA.cfg_.ditherLsb = dl;
            b.cfg_.ditherLsb = refB.cfg_.ditherLsb = dr;
            a.s_ = refA.s_ = seed; b.s_ = refB.s_ = seed ^ 0x9e3779b97f4a7c15ull;
            const size_t lengths[] = {1,7,16,31,257,4096,8191,65537};
            size_t pos = 0, block = 0;
            while (pos < frames) {
                const size_t count = std::min(lengths[block++ % 8], frames - pos);
                if (block == 12) { a.reset(); b.reset(); refA.reset(); refB.reset(); }
                if (block % 7 == 0) {
                    for (size_t i = 0; i < count; ++i) { left[i] = a.process(x[pos+i]); right[i] = b.process(z[pos+i]); }
                } else {
                    const bool accepted = SdmModulator::try_process_binary_stereo_bits_block(a,b,x.data()+pos,z.data()+pos,count,
                        [&](size_t i,bool l,bool r) { left[i] = l ? 1.0 : -1.0; right[i] = r ? 1.0 : -1.0; });
                    failures += !accepted;
                }
                for (size_t i = 0; i < count; ++i) {
                    const double l = refA.process(x[pos+i]), r = refB.process(z[pos+i]);
                    failures += left[i] != l || right[i] != r; compared += 2;
                }
                failures += !state(a,refA) || !state(b,refB); pos += count;
            }
            // Current sample is consumed in BOTH lanes before a throwing sink.
            for (size_t i = 0; i < 17; ++i) { refA.process(x[i]); refB.process(z[i]); }
            bool caught = false;
            try { SdmModulator::try_process_binary_stereo_bits_block(a,b,x.data(),z.data(),32,
                [](size_t i,bool,bool) { if (i == 16) throw 42; }); }
            catch (int value) { caught = value == 42; }
            failures += !caught || !state(a,refA) || !state(b,refB);
            // reset clears state/counters while preserving independently advanced RNGs.
            a.reset(); b.reset(); refA.reset(); refB.reset();
            failures += !state(a,refA) || !state(b,refB);
            const double zero[65536] = {};
            const auto oldOverload = a.overload_ + b.overload_;
            SdmModulator::try_process_binary_stereo_bits_block(a,b,zero,zero,65536,
                [&](size_t i,bool l,bool r) { left[i] = l ? 1.0 : -1.0; right[i] = r ? 1.0 : -1.0; });
            double means[2] = {}; size_t same[2] = {};
            for (size_t i = 0; i < 65536; ++i) {
                const double l = refA.process(0.0), r = refB.process(0.0);
                failures += left[i] != l || right[i] != r; compared += 2;
                if (i >= 4096) { means[0] += left[i]; means[1] += right[i]; same[0] += left[i] == left[i-1]; same[1] += right[i] == right[i-1]; }
            }
            failures += !state(a,refA) || !state(b,refB);
            failures += std::abs(means[0]/61440) >= .03 || std::abs(means[1]/61440) >= .03
                || double(same[0])/61440 >= .9 || double(same[1])/61440 >= .9;
            // Report the actual zero-input counter behavior: dither=.1 may cause
            // additional saturation, and exact reference/state equality is required.
            std::printf("DITHER N=%d left=%g right=%g seed=%llu overload_delta=%lld failures=%zu\n",order,dl,dr,
                (unsigned long long)seed,a.overload_+b.overload_-oldOverload,failures);
            ++cases;
        }
    }
    std::printf("DITHER_RESULT cases=%zu compared=%zu failures=%zu\n",cases,compared,failures);
    return failures ? 1 : 0;
}

int main() { return test<5>() | test<7>() | fallback_and_throw() | dither_matrix(); }

#else
int main() {
    std::puts("SKIP: stereo SIMD disabled or unavailable");
    return 0;
}
#endif
