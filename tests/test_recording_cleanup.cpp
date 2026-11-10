#include "dsp/fft.h"
#include "dsp/recording_cleanup.h"
#include "engine/pipeline.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
using namespace rta::dsp;
static int failures = 0;
static void check(bool ok, const char *label, double x = 0) {
    printf("%s %s %.12g\n", ok ? "PASS" : "FAIL", label, x);
    if (!ok)
        ++failures;
}
static std::vector<double> clean(size_t n, int variant) {
    std::vector<double> x(n);
    std::mt19937 rng(100 + variant);
    std::uniform_real_distribution<double> phase(0, 2 * kPi);
    std::vector<double> f(variant < 3 ? 7 : 11), ph(f.size());
    for (size_t j = 0; j < f.size(); ++j) {
        f[j] = 53.7 + std::fmod((j + 1) * (variant * 739.17 + 941.3), 19900.);
        ph[j] = phase(rng);
    }
    for (size_t i = 0; i < n; ++i) {
        double t = (double)i / 44100.;
        double a = std::min(1., i / 1024.) * std::min(1., (n - i - 1) / 1024.);
        for (size_t j = 0; j < f.size(); ++j)
            x[i] += .025 * a * std::sin(2 * kPi * f[j] * t + ph[j]);
    }
    return x;
}
static std::vector<double> degrade(const std::vector<double> &x, int variant) {
    std::vector<double> y = x;
    std::mt19937 rng(0x9161 + variant);
    std::normal_distribution<double> noise(0, std::pow(10., -105. / 20));
    for (auto &a : y)
        a = std::clamp(std::round((a + noise(rng)) * 32768.) / 32768., -1., 32767. / 32768.);
    return y;
}
static std::vector<double> run(const std::vector<double> &x, RecordingCleanup &c,
                               size_t chunk = 137) {
    std::vector<double> y;
    size_t limit = x.size() + c.latency() + c.tail();
    for (size_t pos = 0; pos < limit;) {
        size_t n = std::min(chunk, limit - pos);
        std::vector<double> b(n);
        for (size_t i = 0; i < n; ++i)
            b[i] = (pos + i < x.size()) ? x[pos + i] : 0;
        double *p[] = {b.data()};
        c.process(p, 1, n);
        y.insert(y.end(), b.begin(), b.end());
        pos += n;
    }
    std::vector<double> o(x.size());
    for (size_t i = 0; i < x.size(); ++i)
        o[i] = y[i + c.latency()];
    return o;
}
// Independent, fixed bandwidth measurement: direct FIR convolution of each
// reference error with a 129-tap Hann-windowed 20 kHz lowpass. The candidate
// and raw ADC error see exactly the same filter, level and sample interval.
static double error(const std::vector<double> &y, const std::vector<double> &x) {
    double sum = 0;
    std::vector<double> h(129);
    double norm = 0;
    for (int j = -64; j <= 64; ++j) {
        double s = j ? std::sin(2 * kPi * 20000. / 44100. * j) / (kPi * j) : 2 * 20000. / 44100.;
        h[j + 64] = s * (.5 + .5 * std::cos(kPi * j / 64.));
        norm += h[j + 64];
    }
    for (double &a : h)
        a /= norm;
    for (size_t i = 4096; i + 4096 < x.size(); ++i) {
        double e = 0;
        for (int j = -64; j <= 64; ++j)
            e += h[j + 64] * (y[i + j] - x[i + j]);
        sum += e * e;
    }
    return sum / (x.size() - 8192);
}
static void accuracy() {
    double train = 0, held = 0;
    for (int v = 0; v < 7; ++v) {
        auto ref = clean(65536, v), raw = degrade(ref, v);
        RecordingCleanupConfig cfg;
        cfg.enabled = true;
        cfg.strength = .5;
        cfg.noiseFloorDbfs = -99.5;
        RecordingCleanup c(cfg, 44100, 1);
        auto y = run(raw, c);
        double before = error(raw, ref), after = error(y, ref),
               gain = 10 * std::log10(before / after);
        char label[120];
        sprintf(label, "%s ADC16 case%d same-20kHz-band error improves >=1dB",
                v < 3 ? "design" : "development-validation", v);
        check(gain >= 1, label, gain);
        if (v < 3)
            train += gain;
        else
            held += gain;
        check(c.stats().modifiedBins > 0, "adaptive spectral attenuation is observable",
              c.stats().modifiedBinFraction);
        check(c.latency() == 2048, "declared cleanup latency is exact", (double)c.latency());
    }
    check(train / 3 >= 2, "design set mean improvement >=2dB", train / 3);
    check(held / 4 >= 2, "development-validation mean improvement >=2dB", held / 4);
}

static double mse(const std::vector<double> &a, const std::vector<double> &b,
                  size_t margin = 4096) {
    double sum = 0;
    for (size_t i = margin; i + margin < a.size(); ++i) {
        double d = a[i] - b[i];
        sum += d * d;
    }
    return sum / (a.size() - 2 * margin);
}
// Frozen verification generator: independently sampled 88.2 kHz analogue-like
// signal -> independent 65-tap anti-alias FIR -> 2:1 decimation -> 16-bit ADC.
// Unlike development fixtures, includes a swept partial and a dense 23-tone case.
static void adc_case(int v, std::vector<double> &ref, std::vector<double> &raw) {
    const size_t N = 65536;
    std::vector<double> hi(2 * N + 128), noisy(hi.size()), h(65);
    std::mt19937 rng(0x20261110 + v);
    std::normal_distribution<double> noise(0, 1e-5);
    std::uniform_real_distribution<double> phase(0, 2 * kPi);
    std::vector<double> ph(24);
    for (double &p : ph)
        p = phase(rng);
    for (size_t i = 0; i < hi.size(); ++i) {
        double t = ((double)i - 64) / 88200.;
        double x = 0;
        if (v == 0)
            for (int j = 0; j < 5; ++j)
                x += .025 * std::sin(2 * kPi * (71.3 + j * 4897.71) * t + ph[j]);
        if (v == 1)
            x = .04 * std::sin(2 * kPi * (1300 * t + 5700 * t * t)) +
                .025 * std::sin(2 * kPi * 819.7 * t);
        if (v == 2)
            for (int j = 0; j < 23; ++j)
                x += .008 * std::sin(2 * kPi * (179.3 + j * 871.81) * t + ph[j]);
        if (v == 3) {
            double age = t - std::floor(t / .21) * .21;
            double env = (1 - std::exp(-age * 1800)) * std::exp(-age * 30);
            x = .13 * env * (std::sin(2 * kPi * 937 * t) + .3 * std::sin(2 * kPi * 11031 * t));
        }
        hi[i] = x;
        noisy[i] = x + noise(rng);
    }
    double norm = 0;
    for (int j = -32; j <= 32; ++j) {
        double a = j ? std::sin(2 * kPi * 20000. / 88200. * j) / (kPi * j) : 2 * 20000. / 88200.;
        h[j + 32] = a * (.5 + .5 * std::cos(kPi * j / 32.));
        norm += h[j + 32];
    }
    for (double &a : h)
        a /= norm;
    ref.resize(N);
    raw.resize(N);
    for (size_t i = 0; i < N; ++i) {
        double a = 0, b = 0;
        for (int j = -32; j <= 32; ++j) {
            a += h[j + 32] * hi[2 * i + 64 + j];
            b += h[j + 32] * noisy[2 * i + 64 + j];
        }
        ref[i] = a;
        raw[i] = std::round(b * 32768) / 32768.;
    }
}
static void heldout_adc() {
    double mean = 0;
    for (int v = 0; v < 4; ++v) {
        std::vector<double> ref, raw;
        adc_case(v, ref, raw);
        RecordingCleanupConfig cfg;
        cfg.enabled = true;
        RecordingCleanup c(cfg, 44100, 1);
        auto y = run(raw, c);
        double gain = 10 * std::log10(error(raw, ref) / error(y, ref));
        char name[160];
        sprintf(name,
                "frozen heldout 88.2k ADC/decimate16 case%d default settings improvement >=1dB", v);
        check(gain >= 1, name, gain);
        mean += gain;
    }
    check(mean / 4 >= 1.5, "frozen ADC holdout mean improvement >=1.5dB", mean / 4);
}
static void preservation() {
    RecordingCleanupConfig cfg;
    cfg.enabled = true;
    for (int v = 0; v < 4; ++v) {
        std::vector<double> x(65536);
        std::mt19937 rng(167 + v);
        std::normal_distribution<double> noise(0, .03);
        for (size_t i = 0; i < x.size(); ++i) {
            double t = (double)i / 44100.;
            if (v == 0)
                x[i] =
                    .07 * std::sin(2 * kPi * 17923.7 * t) + .03 * std::sin(2 * kPi * 19963.1 * t);
            if (v == 1)
                x[i] = .08 * std::sin(2 * kPi * 21517.3 * t);
            if (v == 2)
                x[i] = noise(rng);
            if (v == 3)
                x[i] = .06 * std::sin(2 * kPi * 31.7 * t) + .04 * std::sin(2 * kPi * 1431.3 * t);
        }
        RecordingCleanup c(cfg, 44100, 1);
        auto y = run(x, c);
        double err = std::sqrt(mse(y, x)),
               gain = 10 * std::log10(mse(y, std::vector<double>(y.size())) /
                                      mse(x, std::vector<double>(x.size())));
        char name[160];
        sprintf(name, "clean/high-frequency/full-band example%d absolute error RMS <=2e-6", v);
        check(err <= 2e-6, name, err);
        check(std::fabs(gain) < .001, "clean signal level change <.001dB (no level matching)",
              gain);
    }
    // A genuinely noise-like musical signal below the declared budget cannot be
    // distinguished from ADC noise. Record its damage explicitly and bound it;
    // this is why cleanup is opt-in rather than a claim of universal restoration.
    std::vector<double> quiet(65536);
    std::mt19937 rng(777);
    std::normal_distribution<double> dist(0, 1e-6);
    for (auto &x : quiet)
        x = dist(rng);
    RecordingCleanup c(cfg, 44100, 1);
    auto q = run(quiet, c);
    double loss = 10 * std::log10(mse(quiet, std::vector<double>(quiet.size())) /
                                  mse(q, std::vector<double>(q.size())));
    check(loss >= 0 && loss <= cfg.maxReductionDb + .01,
          "counterexample: sub-budget music can lose up to the configured 6dB", loss);
    // Same-frequency strong coherent music is protected while stochastic low-SNR
    // bins are reduced: behavior is signal-adaptive, not fixed bandwidth cutting.
    check(c.stats().meanReductionDb > 1,
          "quiet/noise-like signal has measurable adaptive reduction", c.stats().meanReductionDb);
    auto x = clean(65536, 1);
    RecordingCleanup a(cfg, 44100, 1), b(cfg, 44100, 1);
    auto ya = run(x, a, 1), yb = run(x, b, 5003);
    check(ya == yb, "arbitrary chunk sizes are exactly invariant");
    a.reset();
    auto fresh = run(x, a, 137);
    RecordingCleanup f(cfg, 44100, 1);
    check(fresh == run(x, f, 137), "seek/new-track reset equals a fresh processor");
    cfg.enabled = false;
    RecordingCleanup off(cfg, 44100, 1);
    auto unchanged = x;
    double *pp[] = {unchanged.data()};
    off.process(pp, 1, unchanged.size());
    check(std::memcmp(unchanged.data(), x.data(), x.size() * sizeof(double)) == 0 &&
              off.latency() == 0 && off.tail() == 0,
          "disabled cleanup is exact bypass with zero delay/tail");
}

static void finite_tail() {
    RecordingCleanupConfig cfg;
    cfg.enabled = true;
    cfg.noiseFloorDbfs = -80;
    RecordingCleanup c(cfg, 44100, 1);
    std::vector<double> z(10001 + 6 * 2048);
    z[10000] = 1e-6;
    double *p[] = {z.data()};
    c.process(p, 1, z.size());
    const size_t sourceEnd = 10000 + c.latency(), bound = sourceEnd + c.tail();
    double after = 0, tail = 0;
    size_t last = sourceEnd;
    for (size_t i = sourceEnd + 1; i < z.size(); ++i) {
        tail = std::max(tail, std::fabs(z[i]));
        if (std::fabs(z[i]) > 1e-20)
            last = i;
        if (i > bound)
            after = std::max(after, std::fabs(z[i]));
    }
    check(tail > 1e-14, "weak end impulse demonstrates genuine adaptive finite ringing", tail);
    check(after < 1e-20 && last <= bound, "declared cleanup tail covers its final nonzero sample",
          (double)(last - sourceEnd));
}
static void recovery_channels() {
    RecordingCleanupConfig cfg;
    cfg.enabled = true;
    for (int ch : {1, 2, 6, 8}) {
        RecordingCleanup c(cfg, 44100, ch);
        std::vector<std::vector<double>> a(ch, std::vector<double>(16384));
        for (int k = 0; k < ch; ++k) {
            for (size_t i = 0; i < 4096; ++i)
                a[k][i] = std::sin(2 * kPi * 997. * i / 44100.);
            a[k][100] = 1.;
            a[k][5000] = 2.;
            a[k][8000] = -1.;
        }
        std::vector<double *> p;
        for (auto &x : a)
            p.push_back(x.data());
        c.process(p.data(), ch, a[0].size());
        double diff = 0, tailRms = 0;
        size_t positive = 0, negative = 0;
        bool finite = true;
        for (int k = 0; k < ch; ++k)
            for (size_t i = 0; i < a[k].size(); ++i) {
                finite = finite && std::isfinite(a[k][i]);
                diff = std::max(diff, std::fabs(a[k][i] - a[0][i]));
                if (k == 0 && i > 13000) {
                    tailRms += a[k][i] * a[k][i];
                    positive += a[k][i] > 1e-14;
                    negative += a[k][i] < -1e-14;
                }
            }
        tailRms = std::sqrt(tailRms / 3383);
        check(finite && diff == 0, "0dB/overrange multi-channel is finite and identical", diff);
        check(tailRms < 1e-14 && positive == 0 && negative == 0,
              "overload recovers to digital silence: RMS and sign counts", tailRms);
        check(c.stats().protectedTransientFrames > 0, "strong transient protection actually ran",
              (double)c.stats().protectedTransientFrames);
    }
}
using rta::audio::AudioBuffer;
using rta::engine::Pipeline;
using rta::engine::PipelineConfig;
static std::vector<double> pipeline_run(Pipeline &p, const std::vector<double> &x, int ch,
                                        bool doubles, uint32_t outRate = 88200) {
    std::vector<double> cat;
    size_t skip = 0;
    bool initialized = false;
    auto consume = [&](const AudioBuffer &in) {
        std::vector<std::vector<double>> d;
        auto o = p.process(in, nullptr, doubles ? &d : nullptr);
        if (!initialized) {
            skip = (size_t)std::llround(p.algorithmic_latency_out_frames());
            initialized = true;
        }
        size_t n = doubles ? (d.empty() ? 0 : d[0].size()) : o.frames();
        for (size_t i = 0; i < n; ++i)
            cat.push_back(doubles ? d[0][i] : o.sample(i, 0));
    };
    for (size_t pos = 0; pos < x.size();) {
        size_t n = std::min<size_t>(317, x.size() - pos);
        AudioBuffer b;
        b.rate = 44100;
        b.channels = ch;
        b.bits = 32;
        b.isFloat = true;
        b.raw.resize(n * ch);
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < ch; ++k)
                b.setSample(i, k, x[pos + i]);
        consume(b);
        pos += n;
    }
    size_t drain = (size_t)std::ceil(
                       (p.algorithmic_latency_out_frames() + p.recording_cleanup_tail_out_frames() +
                        p.resampler_tail_out_frames() + p.convolution_tail_out_frames()) *
                       44100. / outRate) +
                   1024;
    for (size_t pos = 0; pos < drain;) {
        size_t n = std::min<size_t>(317, drain - pos);
        AudioBuffer b;
        b.rate = 44100;
        b.channels = ch;
        b.bits = 32;
        b.isFloat = true;
        b.raw.assign(n * ch, 0);
        consume(b);
        pos += n;
    }
    size_t count = x.size() * outRate / 44100;
    check(cat.size() >= skip + count,
          "production Pipeline emitted every delay-compensated source frame", (double)cat.size());
    return std::vector<double>(cat.begin() + skip, cat.begin() + skip + count);
}
static void production() {
    for (bool doubles : {false, true})
        for (int ch : {1, 2, 6, 8}) {
            PipelineConfig c;
            c.outRate = 88200;
            c.outBits = 32;
            c.gpuResample = false;
            c.twoStage = false;
            c.taps = 2048;
            c.convBlock = 64;
            c.channel.resize(ch);
            c.channel[0].ir = {1.};
            auto x = std::vector<double>(10001);
            x[0] = .25;
            x[5000] = .25;
            x.back() = .25;
            Pipeline bypass(c);
            auto ref = pipeline_run(bypass, x, ch, doubles);
            c.recordingCleanup.enabled = true;
            Pipeline on(c);
            auto y = pipeline_run(on, x, ch, doubles);
            double delta = 0;
            for (size_t i = 0; i < ref.size(); ++i)
                delta = std::max(delta, std::fabs(ref[i] - y[i]));
            char name[160];
            sprintf(name,
                    "production PCM/upscale and DSD-doubleOut=%d ch%d first/mid/tail impulses "
                    "preserved",
                    doubles, ch);
            check(delta < 2e-8, name, delta);
            check(on.prepared_recording_cleanup_stats() &&
                      on.prepared_recording_cleanup_stats()->inputFrames > 0,
                  "production path ran the actual cleanup state");
        }
    // Quantized-noisy recording goes through the real Pipeline upsampler in both
    // output modes. Clean reference uses the identical upsampler and time basis.
    auto refIn = clean(65536, 0), raw = degrade(refIn, 22);
    for (bool doubles : {false, true}) {
        PipelineConfig c;
        c.outRate = 88200;
        c.outBits = 32;
        c.gpuResample = false;
        c.twoStage = false;
        c.taps = 2048;
        Pipeline cleanPipe(c), dirtyPipe(c);
        auto cleanOut = pipeline_run(cleanPipe, refIn, 1, doubles),
             dirtyOut = pipeline_run(dirtyPipe, raw, 1, doubles);
        c.recordingCleanup.enabled = true;
        c.recordingCleanup.noiseFloorDbfs = -99.5;
        Pipeline fixedPipe(c);
        auto fixedOut = pipeline_run(fixedPipe, raw, 1, doubles);
        double gain = 10 * std::log10(mse(dirtyOut, cleanOut) / mse(fixedOut, cleanOut));
        check(gain > 1, "real Pipeline upsampled output reference error improves >1dB", gain);
    }
    {
        PipelineConfig c;
        c.outRate = 88200;
        c.outBits = 32;
        c.gpuResample = false;
        c.twoStage = false;
        c.taps = 2048;
        c.convBlock = 64;
        c.channel.resize(2);
        c.channel[0].ir = {1.};
        c.recordingCleanup.enabled = true;
        Pipeline used(c), fresh(c);
        auto first = clean(10001, 2), second = clean(10001, 5);
        pipeline_run(used, first, 2, true);
        used.seek_reset();
        auto afterSeek = pipeline_run(used, second, 2, true),
             newTrack = pipeline_run(fresh, second, 2, true);
        check(afterSeek == newTrack,
              "Pipeline seek reset clears cleanup/conv/resampler history exactly");
    }
    AudioBuffer in;
    in.rate = 44100;
    in.channels = 2;
    in.bits = 16;
    in.raw = {-32768, -1, 0, 1, 32767, 123};
    PipelineConfig cfg;
    cfg.outBits = 16;
    Pipeline off(cfg);
    rta::engine::PipelineStats st;
    auto out = off.process(in, &st);
    check(out.raw == in.raw && st.bitPerfect && !st.usedRecordingCleanup,
          "disabled production pipeline remains bit-perfect");
}

static void settings_and_performance() {
    for (size_t n : {256u, 512u, 2048u, 8192u})
        for (int fs : {44100, 48000, 192000}) {
            RecordingCleanupConfig cfg;
            cfg.enabled = true;
            cfg.fftSize = n;
            RecordingCleanup c(cfg, fs, 1);
            std::vector<double> x(10001);
            x[0] = .25;
            x[5000] = 1.;
            x.back() = -.25;
            auto y = run(x, c, 337);
            double e = 0;
            for (size_t i = 0; i < x.size(); ++i)
                e = std::max(e, std::fabs(x[i] - y[i]));
            check(c.latency() == n && c.tail() == n - 1 && e < 1e-14,
                  "FFT/rate combinations preserve full-scale impulses and report delay/tail", e);
        }
    auto x = clean(65536, 0);
    double previous = -1;
    for (double strength : {0., .25, .5, .75}) {
        RecordingCleanupConfig cfg;
        cfg.enabled = true;
        cfg.strength = strength;
        RecordingCleanup c(cfg, 44100, 1);
        auto y = run(x, c);
        check(c.stats().meanReductionDb >= previous,
              "strength settings produce monotonic observable reduction",
              c.stats().meanReductionDb);
        previous = c.stats().meanReductionDb;
    }
    for (int ch : {2, 8}) {
        RecordingCleanupConfig cfg;
        cfg.enabled = true;
        RecordingCleanup c(cfg, 44100, ch);
        const size_t N = 441000;
        std::vector<std::vector<double>> b(ch, std::vector<double>(N));
        for (auto &p : b)
            for (size_t i = 0; i < N; ++i)
                p[i] = .1 * std::sin(.071 * i);
        std::vector<double *> ptr;
        for (auto &p : b)
            ptr.push_back(p.data());
        auto t0 = std::chrono::steady_clock::now();
        c.process(ptr.data(), ch, N);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
               realtime = 10 / sec;
        check(realtime > 2, "10 seconds offline cleanup throughput >2x realtime", realtime);
        printf("PERF channels=%d frames=%zu elapsed=%.9f realtime=%.3f\n", ch, N, sec, realtime);
    }
}

// Optional independent-audit fixture: uint64 length then clean/raw/result f64.
// The Python check uses NumPy convolution, not this program's error() function.
static void export_fixture(const char *path) {
    std::vector<double> ref, raw;
    adc_case(1, ref, raw);
    RecordingCleanupConfig cfg;
    cfg.enabled = true;
    RecordingCleanup c(cfg, 44100, 1);
    auto y = run(raw, c);
    std::ofstream f(path, std::ios::binary);
    uint64_t n = ref.size();
    f.write((const char *)&n, sizeof(n));
    for (auto *v : {&ref, &raw, &y})
        f.write((const char *)v->data(), v->size() * sizeof(double));
    if (!f)
        throw std::runtime_error("failed writing independent audit fixture");
}
int main(int argc, char **argv) {
    accuracy();
    heldout_adc();
    preservation();
    finite_tail();
    recovery_channels();
    production();
    settings_and_performance();
    if (argc == 2)
        export_fixture(argv[1]);
    printf("FAILURES=%d\n", failures);
    return failures ? 1 : 0;
}
