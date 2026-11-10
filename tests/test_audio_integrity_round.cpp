// Offline regression: actual PCM numerics, latency-trimmed source impulses,
// and live loudness. No audio endpoint is opened. Full-scale test values stay
// in memory only. See build-round-evidence/audio-before.txt for failing baseline.
#include "engine/pipeline.h"
#include <cmath>
#include <cstdio>
#include <thread>
using namespace rta::engine;
using rta::audio::AudioBuffer;
static int fail = 0;
static void check(bool ok, const char *msg, double v = 0) {
    printf("%s %s %.12g\n", ok ? "PASS" : "FAIL", msg, v);
    if (!ok)
        ++fail;
}
static AudioBuffer input(int ch, size_t n, int bits = 24, bool f = false) {
    AudioBuffer a;
    a.rate = 48000;
    a.channels = ch;
    a.bits = bits;
    a.isFloat = f;
    a.raw.assign(n * ch, 0);
    return a;
}
static void conversion() {
    for (bool f : {false, true})
        for (int ib : {16, 24, 32}) {
            if (f && ib != 32)
                continue;
            for (int ob : {16, 24, 32}) {
                PipelineConfig c;
                c.outBits = ob;
                c.sourceBits = 16;
                Pipeline p(c);
                auto a = input(1, 7, ib, f);
                double x[] = {0.25, -0.25, 0.0, 0.5, -0.5, 0.125, -0.125};
                for (int n = 0; n < 7; ++n)
                    a.setSample(n, 0, x[n]);
                auto b = p.process(a);
                double e = 0;
                for (int n = 0; n < 7; ++n)
                    e = std::max(e, std::fabs(b.sample(n, 0) - x[n]));
                char msg[128];
                sprintf(msg, "conversion float=%d in=%d out=%d", f, ib, ob);
                check(e < 1e-9, msg, e);
            }
        }
    // Misleading metadata cannot make arbitrary float samples lossless.
    for (int sb : {0, 16, 24, 32})
        for (int ob : {16, 24, 32}) {
            PipelineConfig c;
            c.outBits = ob;
            c.sourceBits = sb;
            Pipeline p(c);
            auto a = input(1, 8, 32, true);
            const double x[] = {-1.,           .9999998,       .123456789, -.123456789,
                                .000000123456, -.000000123456, 0.,         .25};
            for (size_t i = 0; i < 8; ++i)
                a.setSample(i, 0, x[i]);
            PipelineStats st;
            auto o = p.process(a, &st);
            // Reference numeric PCM packing includes the asymmetric positive
            // full-scale ceiling; a half-LSB error bound alone is wrong there.
            auto expected = input(1, 8, ob);
            double e = 0;
            for (size_t i = 0; i < 8; ++i) {
                expected.setSample(i, 0, a.sample(i, 0));
                e = std::max(e, std::fabs(o.sample(i, 0) - expected.sample(i, 0)));
            }
            char msg[160];
            sprintf(msg, "float numerics sourceBits=%d outBits=%d without zero-pad claim", sb, ob);
            check(!st.zeroPad && e == 0, msg, e);
        }
}
static void impulse() {
    for (int ch : {1, 2, 6, 8})
        for (bool mixed : {false, true}) {
            PipelineConfig c;
            c.outBits = 24;
            c.convBlock = 64;
            c.channel.resize(ch);
            for (int k = 0; k < ch; ++k)
                if (!mixed || k % 2 == 0)
                    c.channel[k].ir = {1.0};
            Pipeline p(c);
            const int N = 1001;
            auto a = input(ch, N);
            for (int k = 0; k < ch; ++k)
                for (int n : {0, 500, 1000})
                    a.setSample(n, k, 0.25);
            std::vector<std::vector<double>> all(ch);
            for (int pos = 0; pos < N;) {
                int n = std::min(117, N - pos);
                auto z = input(ch, n);
                for (int k = 0; k < ch; ++k)
                    for (int i = 0; i < n; ++i)
                        z.setSample(i, k, a.sample(pos + i, k));
                auto b = p.process(z);
                for (int k = 0; k < ch; ++k)
                    for (size_t i = 0; i < b.frames(); ++i)
                        all[k].push_back(b.sample(i, k));
                pos += n;
            }
            auto d = (size_t)std::llround(p.algorithmic_latency_out_frames());
            auto tail = p.drain_chunk(64);
            for (int k = 0; k < ch; ++k)
                for (size_t i = 0; i < tail.frames(); ++i)
                    all[k].push_back(tail.sample(i, k));
            double e = 0;
            for (int k = 0; k < ch; ++k)
                for (int n = 0; n < N; ++n) {
                    double v = n + (int)d < (int)all[k].size() ? all[k][n + d] : 0;
                    e = std::max(e, std::fabs(v - a.sample(n, k)));
                }
            char msg[128];
            sprintf(msg, "impulse head/mid/tail channels=%d mixed=%d latency=%zu", ch, mixed, d);
            check(d == 64 && e < 1e-7, msg, e);
        }
}
static double loud_run(double start, double target, bool internal, double hz = 20.0) {
    PipelineConfig c;
    c.outBits = 32;
    c.volumeEnabled = internal;
    c.volumeDb = start;
    c.loudnessEnabled = true;
    c.volumeRampMs = 20;
    Pipeline p(c);
    double ss = 0;
    for (int block = 0; block < 200; ++block) {
        if (block == 10) {
            if (internal)
                p.set_volume_db(target);
            else
                p.set_loudness_volume_db(target);
        }
        auto a = input(1, 480, 32, true);
        for (int i = 0; i < 480; ++i)
            a.setSample(i, 0,
                        .01 * std::sin(2 * 3.141592653589793 * hz * (block * 480 + i) / 48000.0));
        std::vector<std::vector<double>> out;
        p.process(a, nullptr, &out);
        if (block >= 150)
            for (double y : out[0])
                ss += y * y;
    }
    return std::sqrt(ss / 24000.0);
}
static void loudness() {
    for (bool internal : {false, true})
        for (double hz : {20., 1000., 12000.})
            for (double target : {-30., -15., 0.}) {
                const double a = loud_run(0, target, internal, hz),
                             b = loud_run(-30, target, internal, hz);
                char msg[160];
                sprintf(msg, "loudness target=%g hz=%g internalVolume=%d independent of start",
                        target, hz, internal);
                check(std::fabs(a / b - 1) < 1e-4, msg, a / b);
            }
    // A change on an established steady DC must keep filter history. The old
    // coefficient replacement resets that history and makes a large discontinuity.
    rta::dsp::Loudness l;
    l.init(48000);
    l.set_volume_db(-30);
    std::vector<double> v(24000, .01);
    l.process(v.data(), v.size());
    double prev = v.back();
    l.set_volume_db(0);
    v.assign(24000, .01);
    l.process(v.data(), v.size());
    double first = std::fabs(v[0] - prev), step = first;
    size_t worst = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (std::fabs(v[i] - v[i - 1]) > step) {
            step = std::fabs(v[i] - v[i - 1]);
            worst = i;
        }
    printf("DC step first=%g worst=%zu prev=%g next=%g\n", first, worst,
           worst ? v[worst - 1] : prev, v[worst]);
    check(first < 1e-4 && step < 1e-4, "live shelves preserve history, no DC step on target change",
          step);
    check(v.back() == .01, "loudness reaches exact bypass after smooth transition", v.back());
    // Chunking must not restart either smoothing or filter history.
    rta::dsp::Loudness one, split;
    one.init(48000);
    split.init(48000);
    one.set_volume_db(-30);
    split.set_volume_db(-30);
    std::vector<double> warm(1000, .01), warm2 = warm;
    one.process(warm.data(), warm.size());
    split.process(warm2.data(), warm2.size());
    one.set_volume_db(-3);
    split.set_volume_db(-3);
    std::vector<double> whole(8000), parts;
    for (size_t i = 0; i < whole.size(); ++i)
        whole[i] = .01 * std::sin(.1 * i);
    parts = whole;
    one.process(whole.data(), whole.size());
    for (size_t pos = 0; pos < parts.size();) {
        size_t n = std::min<size_t>(137, parts.size() - pos);
        split.process(parts.data() + pos, n);
        pos += n;
    }
    double chunkDiff = 0;
    for (size_t i = 0; i < whole.size(); ++i)
        chunkDiff = std::max(chunkDiff, std::fabs(whole[i] - parts[i]));
    check(chunkDiff == 0, "live loudness invariant to caller block size", chunkDiff);
    // Volume returning to zero may not take the raw bypass before its ramp ends.
    PipelineConfig c;
    c.volumeEnabled = true;
    c.volumeRampMs = 20;
    c.outBits = 24;
    Pipeline p(c);
    auto z = input(2, 480);
    for (size_t i = 0; i < z.frames(); ++i)
        for (int k = 0; k < 2; ++k)
            z.setSample(i, k, .25);
    p.process(z);
    p.set_volume_db(-30);
    p.process(z);
    p.set_volume_db(0);
    auto o = p.process(z);
    check(o.sample(0, 0) < .2, "return-to-zero volume retains its transition", o.sample(0, 0));
    // Control setters are request mailboxes; concurrent writes never touch DSP
    // histories. At the final explicit request every channel must settle alike.
    c.loudnessEnabled = true;
    Pipeline q(c);
    std::atomic<bool> run{true};
    std::thread control([&] {
        for (int i = 0; i < 10000; ++i) {
            q.set_volume_db(i % 2 ? -30 : 0);
            q.set_loudness_volume_db(i % 2 ? -20 : -5);
        }
        run = false;
    });
    bool finite = true;
    while (run.load()) {
        std::vector<std::vector<double>> b;
        q.process(z, nullptr, &b);
        for (auto &plane : b)
            for (double x : plane)
                finite = finite && std::isfinite(x);
    }
    control.join();
    q.set_volume_db(-15);
    double diff = 0;
    for (int b = 0; b < 100; ++b) {
        std::vector<std::vector<double>> y;
        q.process(z, nullptr, &y);
        for (size_t i = 0; i < y[0].size(); ++i)
            diff = std::max(diff, std::fabs(y[0][i] - y[1][i]));
    }
    check(finite && diff == 0, "10000 concurrent requests stay finite and channel aligned", diff);
}
int main() {
    conversion();
    impulse();
    loudness();
    printf("FAILURES=%d\n", fail);
    return fail ? 1 : 0;
}
