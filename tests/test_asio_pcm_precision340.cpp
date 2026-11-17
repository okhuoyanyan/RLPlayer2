// Exercise the actual ASIO packing function offline; never opens a driver.
// Exact integer scaling is required even above half scale: multiplying by
// 2^(N-1)-1 destroys low-zero DAC bits and the bit-perfect contract.
#include "audio/asio_out.h"
#include "audio/wasapi_out.h"
#include "engine/pipeline.h"
#include <cstdio>
#include <limits>
#include <vector>

namespace rta::audio {
struct AsioDriverTestAccess {
    static void pack(void* dst, const int32_t* src, size_t stride, size_t n,
                     ASIOSampleType type, int srcBits, double gain) {
        AsioDriver::write_channel(dst, src, stride, n, type, srcBits, gain);
    }
};
}
using namespace rta;
static size_t checks = 0, failures = 0, samplesChecked = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}
static audio::ASIOSampleType type_for(int bits) {
    return bits == 16 ? audio::ASIOSTInt16LSB : bits == 24 ? audio::ASIOSTInt24LSB : audio::ASIOSTInt32LSB;
}
static int32_t get(const unsigned char* p, int bits) {
    uint32_t u = 0; for (int j = 0; j < bits / 8; ++j) u |= uint32_t(p[j]) << (8 * j);
    if (bits < 32 && (u & (1u << (bits - 1)))) u |= ~((1u << bits) - 1u);
    return (int32_t)u;
}
static int32_t reference(int32_t x, int srcBits, int dstBits, double gain) {
    if (!std::isfinite(gain)) return 0;
    // Independent long-double oracle (not the ASIO helper or WASAPI packer).
    const long double lo = -std::ldexp(1.L, dstBits - 1), hi = -lo - 1;
    const long double v = std::ldexp((long double)x * gain, dstBits - srcBits);
    if (!std::isfinite(v)) return 0;
    return (int32_t)std::round(std::clamp(v, lo, hi));
}
static std::vector<int32_t> source(int bits) {
    const int64_t lim = int64_t(1) << (bits - 1);
    std::vector<int32_t> a;
    if (bits == 16) for (int64_t v = -lim; v < lim; ++v) a.push_back((int32_t)v);
    else {
        uint32_t state = 123456789;
        for (size_t i = 0; i < 65536; ++i) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            const int64_t v = (int64_t)state % (lim * 2) - lim;
            a.push_back((int32_t)v);
        }
    }
    for (int64_t v : {-lim, -lim + 1, -lim / 2 - 1, -lim / 2, -257ll, -256ll,
                      -129ll, -128ll, -1ll, 0ll, 1ll, 128ll, 129ll, 256ll, 257ll,
                      lim / 2, lim / 2 + 1, lim - 2, lim - 1}) a.push_back((int32_t)v);
    return a;
}
static void packed_formats() {
    for (int srcBits : {16, 24, 32}) {
        const auto a = source(srcBits);
        std::vector<int32_t> interleaved(a.size() * 2, 1234567);
        for (size_t i = 0; i < a.size(); ++i) interleaved[i * 2] = a[i];
        for (int dstBits : {16, 24, 32}) for (double gain : {1., 0., .5, 2., 10., .8912509381337456,
                std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            const size_t bytes = dstBits / 8;
            std::vector<unsigned char> packed(a.size() * bytes + 16, 0xCD);
            audio::AsioDriverTestAccess::pack(packed.data() + 8, interleaved.data(), 2, a.size(), type_for(dstBits), srcBits, gain);
            size_t bad = 0;
            for (size_t i = 0; i < a.size(); ++i)
                bad += get(packed.data() + 8 + i * bytes, dstBits) != reference(a[i], srcBits, dstBits, gain);
            bool guard = true;
            for (size_t i = 0; i < 8; ++i) guard &= packed[i] == 0xCD && packed[packed.size() - 1 - i] == 0xCD;
            samplesChecked += a.size();
            check(bad == 0, "ASIO real pack agrees with independent scale/round/saturation oracle");
            check(guard, "strided real ASIO pack preserves surrounding guards");
            if (bad) std::printf("MISMATCH src=%d dst=%d gain=%.9g bad=%zu\n", srcBits, dstBits, gain, bad);
        }
    }
}
static void pipeline_dac_grid() {
    for (int container : {16, 24, 32}) for (int valid : {16, 18, 20, 24}) {
        audio::AudioBuffer in; in.rate = 96000; in.channels = 2; in.bits = 32; in.isFloat = true;
        in.raw.resize(8192 * 2);
        for (size_t i = 0; i < in.frames(); ++i) for (int c = 0; c < 2; ++c)
            in.setSample(i, c, .91 * std::sin(2 * dsp::kPi * (997 + c * 173) * i / in.rate));
        in.setSample(0, 0, 1); in.setSample(0, 1, -1);
        engine::PipelineConfig cfg; cfg.outBits = container; cfg.pcmValidBits = valid;
        cfg.shaper = dsp::Shaper::TPDF; cfg.gpuResample = false;
        engine::Pipeline pipeline(cfg); const auto pcm = pipeline.process(in);
        for (int dstBits : {16, 24, 32}) {
            const int effective = std::min({container, valid, dstBits});
            const uint32_t mask = (uint32_t(1) << (dstBits - effective)) - 1;
            const size_t bytes = dstBits / 8;
            std::vector<unsigned char> a(pcm.frames() * bytes), w(a.size());
            audio::AsioDriverTestAccess::pack(a.data(), pcm.raw.data(), 2, pcm.frames(), type_for(dstBits), container, 1.);
            std::vector<int32_t> mono(pcm.frames()); for (size_t i = 0; i < mono.size(); ++i) mono[i] = pcm.raw[i * 2];
            audio::ex_pack_int32({(int)bytes, dstBits, false}, 1, container, 1., w.data(), mono.data(), mono.size());
            size_t gridBad = 0, valueBad = 0;
            for (size_t i = 0; i < pcm.frames(); ++i) {
                const auto v = get(a.data() + i * bytes, dstBits);
                gridBad += (uint32_t(v) & mask) != 0;
                valueBad += v != reference(pcm.raw[i * 2], container, dstBits, 1.);
            }
            samplesChecked += pcm.frames();
            check(gridBad == 0 && valueBad == 0, "Pipeline quantization survives ASIO integer transport exactly");
            check(a == w, "ASIO and WASAPI emit identical integer bytes for the same container");
            if (gridBad || valueBad) std::printf("GRID container=%d valid=%d dst=%d lowbits=%d grid_bad=%zu value_bad=%zu\n", container, valid, dstBits, dstBits-effective, gridBad, valueBad);
        }
    }
}
static void float_and_dop() {
    const auto a = source(32); std::vector<float> out(a.size());
    for (double gain : {1., .5, 2., 10., std::numeric_limits<double>::quiet_NaN()}) {
        audio::AsioDriverTestAccess::pack(out.data(), a.data(), 1, a.size(), audio::ASIOSTFloat32LSB, 32, gain);
        bool good = true; for (size_t i = 0; i < a.size(); ++i) {
            const double x = double(a[i]) / 2147483648. * gain;
            const float want = std::isfinite(x) ? (float)std::clamp(x, -1., 1.) : 0.f;
            good &= out[i] == want;
        }
        samplesChecked += a.size(); check(good, "float output scaling and overrange saturation stay correct");
    }
    std::vector<int32_t> dop(131072);
    for (size_t i = 0; i < dop.size(); ++i) {
        const uint32_t u = (uint32_t(i & 1 ? 0xFA : 0x05) << 16) | uint32_t(i & 65535);
        dop[i] = int32_t(u & 0x800000 ? u | 0xff000000 : u);
    }
    std::vector<unsigned char> bytes(dop.size() * 3);
    audio::AsioDriverTestAccess::pack(bytes.data(), dop.data(), 1, dop.size(), audio::ASIOSTInt24LSB, 24, 1.);
    bool exact = true; for (size_t i = 0; i < dop.size(); ++i) exact &= get(bytes.data() + i * 3, 24) == dop[i];
    samplesChecked += dop.size(); check(exact, "all DoP marker/payload combinations remain bit-exact at unity");
}
int main() {
    packed_formats(); pipeline_dac_grid(); float_and_dop();
    std::printf("ASIO_PCM_PRECISION samples=%zu checks=%zu failures=%zu\n", samplesChecked, checks, failures);
    return failures ? 1 : 0;
}
