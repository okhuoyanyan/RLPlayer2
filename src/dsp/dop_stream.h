// Streaming PCM -> 1-bit SDM -> DoP, without a double buffer for every DSD bit.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dop.h"
#include "modulator.h"
#include "parallel.h"
#include <limits>
#include <memory>
#include <stdexcept>

namespace rta::dsp {

// The SDM arithmetic and dither sequence remain in SdmModulator::process.
// Only the representation between modulation and transport changes. Each
// channel retains at most 15 pending bits and its alternating DoP marker.
class DopModulationStream {
public:
    size_t pending_bits() const { return state_.empty() ? 0 : state_[0].count; }

    // A seek discards old audio bits, but the receiver's marker phase continues.
    void discard_pending() {
        for (auto& s : state_) { s.bits = 0; s.count = 0; }
    }

    // drop samples still warm the modulator, as in the existing seek preroll.
    // finish pads a partial carrier with the same 0x69 silence bit phase as
    // the former double-buffer implementation; it does not run the SDM again.
    // Invalid dimensions or a multilevel SDM are rejected before consuming
    // input or changing out/state. An internal exception aborts this stream.
    bool process(const std::vector<std::vector<double>>& planes,
                 const std::vector<std::unique_ptr<SdmModulator>>& mods,
                 size_t drop, bool finish, std::vector<int32_t>& out,
                 std::string* why = nullptr) {
        auto reject = [&](const char* reason) {
            if (why) *why = reason;
            return false;
        };
        const size_t channels = mods.size();
        if (!channels) return reject("DoP modulation needs at least one channel");
        if (!planes.empty() && planes.size() != channels)
            return reject("DoP input channel count differs from the modulators");
        const size_t frames = planes.empty() ? 0 : planes[0].size();
        if (drop > frames) return reject("DoP preroll exceeds the input block");
        for (size_t c = 0; c < channels; ++c) {
            if (!mods[c] || mods[c]->levels() != 2)
                return reject("DoP requires a 1-bit modulator; multilevel output is rejected");
            if (!planes.empty() && planes[c].size() != frames)
                return reject("DoP input channel lengths differ");
        }
        if (!state_.empty() && state_.size() != channels)
            return reject("DoP channel count cannot change within a stream");
        const size_t pending = pending_bits();
        if (frames - drop > std::numeric_limits<size_t>::max() - pending - 15)
            return reject("DoP frame count overflows");
        const size_t total = pending + frames - drop;
        const size_t carrier = (total + (finish ? 15 : 0)) / 16;
        if (carrier > std::numeric_limits<size_t>::max() / channels)
            return reject("DoP output size overflows");
        if (state_.empty()) state_.resize(channels);
        words_.resize(channels);
        for (auto& v : words_) v.resize(carrier);

        // Independent channel buffers avoid two cores repeatedly invalidating
        // the same interleaved cache line. They hold one word per 16 bits.
#if !defined(RTA_DISABLE_SDM_STEREO_SIMD) && (defined(__SSE2__) || defined(_M_X64))
        // Independent stereo lanes share arithmetic instructions without changing
        // either channel's operation order. Unsupported dimensions/configurations keep
        // the existing independent-channel implementation below.
        bool stereoDone = false;
        if (channels == 2 && drop == 0) {
            State states[2] = {state_[0], state_[1]};
            size_t written[2] = {};
            auto appendStereo = [&](size_t c, bool positive) {
                auto& st = states[c];
                st.bits = (st.bits << 1) | uint32_t(positive);
                if (++st.count != 16) return;
                const uint32_t marker = st.phaseA ? 0x00050000u : 0xfffa0000u;
                words_[c][written[c]++] = static_cast<int32_t>(marker | (st.bits & 0xffffu));
                st.phaseA = !st.phaseA; st.bits = 0; st.count = 0;
            };
            stereoDone = SdmModulator::try_process_binary_stereo_bits_block(*mods[0], *mods[1],
                frames ? planes[0].data() : nullptr, frames ? planes[1].data() : nullptr,
                frames, [&](size_t, bool leftPositive, bool rightPositive) {
                    appendStereo(0, leftPositive); appendStereo(1, rightPositive);
                });
            if (stereoDone) {
                for (size_t c = 0; c < 2; ++c) {
                    if (finish && states[c].count) {
                        const unsigned start = states[c].count;
                        for (unsigned i = 0; i < 16 - start; ++i)
                            appendStereo(c, ((0x69u >> (7 - ((start + i) & 7))) & 1u) != 0);
                    }
                    if (written[c] != carrier)
                        throw std::runtime_error("DoP streaming frame count invariant failed");
                    state_[c] = states[c];
                }
            }
        }
        if (!stereoDone)
#endif
        ParallelFor::instance().run(channels, [&](size_t c) {
            auto s = state_[c];
            size_t written = 0;
            auto append = [&](bool positive) {
                s.bits = (s.bits << 1) | uint32_t(positive);
                if (++s.count != 16) return;
                uint32_t word = (uint32_t(s.phaseA ? DOP_MARKER_A : DOP_MARKER_B) << 16)
                              | (s.bits & 0xffffu);
                if (word & 0x800000u) word |= 0xff000000u;
                words_[c][written++] = static_cast<int32_t>(word);
                s.phaseA = !s.phaseA;
                s.bits = 0;
                s.count = 0;
            };
            // Independent local-state binary loop preserves every arithmetic
            // operation, RNG update, guard and bit. Multi-level/other orders use
            // the scalar fallback. The sink touches only this channel's packer.
            mods[c]->process_binary_block(frames ? planes[c].data() : nullptr, frames,
                                         [&](size_t i, double bit) {
                if (bit != 1.0 && bit != -1.0)
                    throw std::runtime_error("SDM violated the 1-bit DoP output contract");
                if (i >= drop) append(bit > 0.0);
            });
            if (finish && s.count) {
                const unsigned left = 16 - s.count;
                const unsigned start = s.count;
                for (unsigned i = 0; i < left; ++i)
                    append(((0x69u >> (7 - ((start + i) & 7))) & 1u) != 0);
            }
            if (written != carrier)
                throw std::runtime_error("DoP streaming frame count invariant failed");
            state_[c] = s;
        });
        out.resize(carrier * channels);
        for (size_t c = 0; c < channels; ++c)
            for (size_t n = 0; n < carrier; ++n)
                out[n * channels + c] = words_[c][n];
        return true;
    }

private:
    struct State { uint32_t bits = 0; unsigned count = 0; bool phaseA = true; };
    std::vector<State> state_;
    std::vector<std::vector<int32_t>> words_;
};

} // namespace rta::dsp
