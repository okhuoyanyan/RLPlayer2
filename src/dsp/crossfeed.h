// Optional headphone crossfeed: a normalized, single-pole lowpass cross path.
// Public background: https://bs2b.sourceforge.net/ (lowpass cross-ear phase lag).
// This is RLPlayer's own bounded mix, NOT the bs2b/HQPlayer transfer function.
//
// pL[n] = (1-a)*pL[n-1] + a*L[n], a=1-exp(-2*pi*fc/fs); similarly for R.
// outL = direct*L + cross*pR, outR = direct*R + cross*pL
// cross=1/(1+10^(levelDb/20)), direct=1-cross.
// Each lowpass is a convex combination, and output weights sum to one: for
// input bounded by B and zero/bounded history, this stage stays within +/-B.
// This is NOT a limiter or a guarantee about later EQ/resampler/volume stages.
//
// DC dual-mono gain is unity; at high frequencies the sum approaches direct
// (about -1.95 dB for mild 12 dB feed, -3.53 dB for standard 6 dB feed).
// Thus enabled crossfeed changes timbre and stereo separation intentionally.
// It is a listening preference, not a universal fidelity improvement. Off is
// sample-exact and does not even read/modify the samples or advance histories.
#pragma once
#include "fp_strict.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace rta::dsp {
struct CrossfeedConfig {
    bool enabled = false;
    double cutoffHz = 700.0;
    double levelDb = 12.0; // Positive attenuation of opposite vs direct DC path.
};

class Crossfeed {
public:
    explicit Crossfeed(const CrossfeedConfig& config = {}, double sampleRate = 44100.0)
        : config_(config), sampleRate_(sampleRate) {
        if (!config_.enabled) return;
        if (!finite(sampleRate) || sampleRate < 8000.0 || sampleRate > 100000000.0 ||
            !finite(config.cutoffHz) || config.cutoffHz < 300.0 || config.cutoffHz > 1200.0 ||
            !finite(config.levelDb) || config.levelDb < 3.0 || config.levelDb > 15.0)
            throw std::invalid_argument("crossfeed requires 300..1200 Hz, 3..15 dB and a valid sample rate");
        alpha_ = -std::expm1(-2.0 * 3.14159265358979323846 * config.cutoffHz / sampleRate);
        pole_ = 1.0 - alpha_;
        cross_ = 1.0 / (1.0 + std::pow(10.0, config.levelDb / 20.0));
        direct_ = 1.0 - cross_;
    }

    bool enabled() const noexcept { return config_.enabled; }
    double direct_gain() const noexcept { return direct_; }
    double cross_gain() const noexcept { return cross_; }
    // Low-frequency phase delay, not a pure delay or a reason to trim a track.
    double cross_delay_seconds() const noexcept { return enabled() ? pole_ / (alpha_ * sampleRate_) : 0.0; }
    size_t tail_frames() const noexcept {
        return enabled() ? static_cast<size_t>(std::ceil(std::log(1e-12) / std::log(pole_))) : 0;
    }
    long long non_finite() const noexcept { return nonFinite_; }
    void reset() noexcept { lowL_ = lowR_ = 0.0; nonFinite_ = 0; }

    void process(double* left, double* right, size_t frames) noexcept {
        if (!enabled()) return;
        for (size_t i = 0; i < frames; ++i) {
            double l = left[i], r = right[i];
            // Do not let a damaged upstream sample poison an IIR forever.
            if (!finite(l)) { l = lowL_ = 0.0; ++nonFinite_; }
            if (!finite(r)) { r = lowR_ = 0.0; ++nonFinite_; }
            lowL_ += alpha_ * (l - lowL_);
            lowR_ += alpha_ * (r - lowR_);
            left[i] = direct_ * l + cross_ * lowR_;
            right[i] = direct_ * r + cross_ * lowL_;
        }
    }
private:
    static bool finite(double v) noexcept {
        uint64_t bits; std::memcpy(&bits, &v, sizeof(bits));
        return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
    }
    CrossfeedConfig config_;
    double sampleRate_, alpha_ = 0.0, pole_ = 0.0, cross_ = 0.0, direct_ = 1.0;
    double lowL_ = 0.0, lowR_ = 0.0;
    long long nonFinite_ = 0;
};
} // namespace rta::dsp
