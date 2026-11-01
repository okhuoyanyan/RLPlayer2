// Reproduce real Windows include order: intrinsics parsed before fp_strict.
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
// Stereo SIMD transport differential, zero/default dither, independent scalar oracle.
// Real legacy-path differential oracle, including seek/EOF/multilevel rejection.
#include "dsp/dop_stream.h"
#include <iostream>
#include <limits>
#include <numeric>

using namespace rta::dsp;
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << "FAIL line " << __LINE__ << ": " #x "\n";                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

struct Legacy {
    std::vector<std::unique_ptr<SdmModulator>> mods;
    std::vector<std::vector<double>> carry;
    DopPackState state;
    explicit Legacy(const SdmConfig& cfg, int channels) : carry(channels) {
        for (int c = 0; c < channels; ++c)
            mods.push_back(std::make_unique<SdmModulator>(cfg));
    }
    std::vector<int32_t> run(const std::vector<std::vector<double>>& in, size_t drop, bool finish) {
        auto bits = carry;
        for (size_t c = 0; c < mods.size(); ++c)
            if (!in.empty())
                for (size_t i = 0; i < in[c].size(); ++i) {
                    const auto v = mods[c]->process(in[c][i]);
                    if (i >= drop)
                        bits[c].push_back(v);
                }
        const size_t total = bits[0].size(), use = (finish ? (total + 15) / 16 : total / 16) * 16;
        if (finish)
            for (auto& b : bits)
                for (size_t i = total; i < use; ++i)
                    b.push_back(((0x69u >> (7 - (i & 7))) & 1u) ? 1.0 : -1.0);
        std::vector<const double*> ptr;
        for (auto& b : bits)
            ptr.push_back(b.data());
        std::vector<int32_t> result;
        if (!dop_pack_checked(ptr.data(), int(mods.size()), use, result, state))
            throw std::runtime_error("Legacy reference rejected valid bitstream");
        for (size_t c = 0; c < mods.size(); ++c)
            carry[c].assign(bits[c].begin() + use, bits[c].end());
        return result;
    }
};

int main() {
    size_t samples = 0, cases = 0;
    for (double dither : {0.0, .0005})
    for (auto family : {ModulatorFamily::Dsd, ModulatorFamily::Chebyshev, ModulatorFamily::Ec,
                        ModulatorFamily::Ahm})
        for (double fs : {22579200.0})
            for (int order : {5, 7})
                for (int channels : {1, 2, 6}) {
                    SdmConfig cfg;
                    cfg.fs = fs;
                    cfg.family = family;
                    cfg.order = order;
                    cfg.oneBitOutput = true;
                    cfg.cutoffHz = 100000;
                    cfg.ditherLsb = dither;
                    Legacy old(cfg, channels), current(cfg, channels);
                    DopModulationStream packed;
                    size_t position = 0;
                    for (size_t block : {1u, 2u, 7u, 16u, 31u, 4096u, 15u, 1001u}) {
                        std::vector<std::vector<double>> in(channels, std::vector<double>(block));
                        for (int c = 0; c < channels; ++c)
                            for (size_t i = 0; i < block; ++i) {
                                const size_t k = position + i;
                                double v =
                                    .62 * std::sin(2 * kPi * (c + 1) * 997.0 * double(k) / fs);
                                if (k == 12)
                                    v = 5.0;
                                if (k == 18)
                                    v = std::numeric_limits<double>::quiet_NaN();
                                if (k == 23)
                                    v = -std::numeric_limits<double>::infinity();
                                in[c][i] = v;
                            }
                        const size_t drop = block == 31 ? 7 : 0;
                        std::vector<int32_t> actual;
                        REQUIRE(packed.process(in, current.mods, drop, false, actual));
                        REQUIRE(actual == old.run(in, drop, false));
                        REQUIRE(packed.pending_bits() == old.carry[0].size());
                        position += block;
                        samples += block * channels;
                    }
                    std::vector<int32_t> actual;
                    REQUIRE(packed.process({}, current.mods, 0, true, actual));
                    REQUIRE(actual == old.run({}, 0, true));
                    REQUIRE(packed.pending_bits() == 0);
                    for (int c = 0; c < channels; ++c) {
                        REQUIRE(current.mods[c]->clamped() == old.mods[c]->clamped());
                        REQUIRE(current.mods[c]->overload() == old.mods[c]->overload());
                        REQUIRE(current.mods[c]->nonFinite() == old.mods[c]->nonFinite());
                    }
                    // Preserve marker phase on seek, but discard old pending bits and SDM history.
                    std::vector<std::vector<double>> part(channels, std::vector<double>(21, .01));
                    REQUIRE(packed.process(part, current.mods, 0, false, actual));
                    REQUIRE(actual == old.run(part, 0, false));
                    packed.discard_pending();
                    for (auto& v : old.carry)
                        v.clear();
                    for (auto& m : old.mods)
                        m->reset();
                    for (auto& m : current.mods)
                        m->reset();
                    REQUIRE(packed.process(part, current.mods, 3, true, actual));
                    REQUIRE(actual == old.run(part, 3, true));
                    ++cases;
                }
    SdmConfig multi;
    multi.family = ModulatorFamily::Ahm;
    multi.oneBitOutput = false;
    Legacy invalid(multi, 2);
    DopModulationStream stream;
    std::vector<int32_t> out{123, 456};
    std::string why;
    REQUIRE(!stream.process({{.2}, {.2}}, invalid.mods, 0, false, out, &why));
    REQUIRE(!why.empty() && (out == std::vector<int32_t>{123, 456}));
    SdmConfig cfg;
    cfg.oneBitOutput = true;
    Legacy valid(cfg, 2);
    REQUIRE(!stream.process({{.2}, {}}, valid.mods, 0, false, out));
    REQUIRE(!stream.process({{.2}, {.2}}, valid.mods, 2, false, out));
    REQUIRE(stream.pending_bits() == 0 && (out == std::vector<int32_t>{123, 456}));
    std::cout << "PASS " << cases << " configurations " << samples
              << " SDM samples, byte-identical legacy output; seek/EOF/rejection checked\n";
}
