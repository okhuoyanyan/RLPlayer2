// Bounded, signal-adaptive recording-noise cleanup, not an ADC inverse model.
// Sources/assumptions and measured limits are recorded in docs/research.
#pragma once
#include "fft.h"
#include "fp_strict.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace rta::dsp {
struct RecordingCleanupConfig {
    bool enabled = false;
    double strength = .5;
    double noiseFloorDbfs = -101.1; // RMS noise budget, approximately ideal 16-bit quantization.
    double maxReductionDb = 6.;     // Per-bin hard bound; never a fixed lowpass.
    size_t fftSize = 2048;
};
struct RecordingCleanupStats {
    size_t inputFrames = 0, spectralFrames = 0, modifiedBins = 0, totalBins = 0;
    size_t protectedTransientFrames = 0, sanitizedSamples = 0;
    double meanReductionDb = 0, maxReductionDb = 0, modifiedBinFraction = 0;
};
class RecordingCleanup {
  public:
    RecordingCleanup(const RecordingCleanupConfig &cfg, double sampleRate, int channels)
        : cfg_(cfg) {
        if (channels <= 0 || channels > 32 || !(sampleRate > 0))
            throw std::invalid_argument("recording cleanup: invalid stream format");
        channels_ = channels;
        cfg_.strength = finite(cfg_.strength) ? std::clamp(cfg_.strength, 0., 1.) : 0.;
        cfg_.noiseFloorDbfs =
            finite(cfg_.noiseFloorDbfs) ? std::clamp(cfg_.noiseFloorDbfs, -144., -40.) : -101.1;
        cfg_.maxReductionDb =
            finite(cfg_.maxReductionDb) ? std::clamp(cfg_.maxReductionDb, 0., 12.) : 0.;
        enabled_ = cfg_.enabled && cfg_.strength > 0 && cfg_.maxReductionDb > 0;
        n_ = cfg_.fftSize;
        if (n_ < 256 || n_ > 8192 || (n_ & (n_ - 1)))
            n_ = 2048;
        hop_ = n_ / 4;
        window_.resize(n_);
        synthesis_.resize(n_);
        double wPower = 0;
        for (size_t i = 0; i < n_; ++i) {
            const double w = 2 * kPi * (double)i / n_;
            window_[i] =
                .35875 - .48829 * std::cos(w) + .14128 * std::cos(2 * w) - .01168 * std::cos(3 * w);
            wPower += window_[i] * window_[i];
        }
        for (size_t i = 0; i < n_; ++i) {
            double norm = 0;
            for (size_t j = i % hop_; j < n_; j += hop_)
                norm += window_[j] * window_[j];
            synthesis_[i] = window_[i] / norm;
        }
        noisePower_ = std::pow(10., cfg_.noiseFloorDbfs / 10.) * wPower;
        floorGain_ = std::pow(10., -cfg_.maxReductionDb / 20.);
        history_.assign(channels_, std::vector<double>(n_, 0.));
        overlap_ = history_;
        spectrum_.assign(channels_, std::vector<cplx>(n_));
        outputHop_.assign(channels_, std::vector<double>(hop_, 0.));
        power_.resize(n_ / 2 + 1);
        rawMask_ = power_;
        mask_.assign(power_.size(), 1.);
    }
    size_t latency() const { return enabled_ ? n_ : 0; }
    size_t tail() const { return enabled_ ? n_ - 1 : 0; }
    const RecordingCleanupStats &stats() const { return stats_; }
    bool enabled() const { return enabled_; }
    void reset() {
        for (auto &v : history_)
            std::fill(v.begin(), v.end(), 0.);
        for (auto &v : overlap_)
            std::fill(v.begin(), v.end(), 0.);
        for (auto &v : outputHop_)
            std::fill(v.begin(), v.end(), 0.);
        std::fill(mask_.begin(), mask_.end(), 1.);
        write_ = hopFill_ = outPos_ = 0;
        reductionSum_ = 0;
        stats_ = {};
    }
    // The same gain mask is applied to every channel. Stereo phase and the
    // equality of identical channels are preserved; a strong channel protects
    // that bin in all channels. Arbitrary chunk sizes produce one common stream.
    void process(double *const *planes, int channels, size_t frames) {
        if (!enabled_ || !planes || !frames)
            return;
        if (channels != channels_)
            throw std::invalid_argument("recording cleanup: channel count changed");
        for (size_t i = 0; i < frames; ++i) {
            for (int c = 0; c < channels_; ++c) {
                double x = planes[c][i];
                if (!finite(x)) {
                    x = 0;
                    ++stats_.sanitizedSamples;
                }
                // Reject impossible decoder magnitudes before power estimation;
                // single-sample ordinary overload (+/-2) remains representable.
                if (x > 16 || x < -16) {
                    x = std::clamp(x, -16., 16.);
                    ++stats_.sanitizedSamples;
                }
                history_[c][write_] = x;
                planes[c][i] = outputHop_[c][outPos_];
            }
            write_ = (write_ + 1) % n_;
            ++outPos_;
            if (++hopFill_ == hop_) {
                analyze();
                hopFill_ = 0;
                outPos_ = 0;
            }
        }
        stats_.inputFrames += frames;
    }

  private:
    static bool finite(double x) {
        return (std::bit_cast<uint64_t>(x) & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
    }
    void analyze() {
        bool transient = false;
        std::fill(power_.begin(), power_.end(), 0.);
        for (int c = 0; c < channels_; ++c) {
            double peak = 0, sum = 0;
            auto &s = spectrum_[c];
            for (size_t i = 0; i < n_; ++i) {
                double x = history_[c][(write_ + i) % n_];
                peak = std::max(peak, std::fabs(x));
                sum += x * x;
                s[i] = cplx(x * window_[i], 0.);
            }
            transient = transient || (peak > .05 && peak * peak * n_ > 36. * sum);
            fft(s);
            for (size_t k = 0; k < power_.size(); ++k)
                power_[k] = std::max(power_[k], std::norm(s[k]));
        }
        if (transient)
            ++stats_.protectedTransientFrames;
        // Conservative local average prevents isolated random FFT peaks from
        // becoming audible musical noise. High-SNR bins plus their main-lobe
        // neighbours get exact unity instead of asymptotic attenuation.
        for (size_t k = 0; k < power_.size(); ++k) {
            double p = 0, weight = 0;
            bool protectedBin = false;
            const size_t lo = k > 2 ? k - 2 : 0, hi = std::min(k + 2, power_.size() - 1);
            for (size_t j = lo; j <= hi; ++j) {
                double w = 3. - std::fabs((double)j - (double)k);
                p += w * power_[j];
                weight += w;
                protectedBin = protectedBin || power_[j] > 64. * noisePower_;
            }
            p /= weight;
            rawMask_[k] =
                (transient || protectedBin || k == 0 || k + 1 == power_.size())
                    ? 1.
                    : std::clamp(1. - cfg_.strength * noisePower_ / std::max(p, noisePower_),
                                 floorGain_, 1.);
        }
        for (size_t k = 0; k < mask_.size(); ++k) {
            // Unity attack is immediate (preserve new music); noise reduction
            // releases gradually over overlapping frames, avoiding hard gates.
            mask_[k] = std::max(rawMask_[k], .8 * mask_[k] + .2 * rawMask_[k]);
            const double db = -20. * std::log10(std::max(mask_[k], 1e-30));
            reductionSum_ += db;
            stats_.maxReductionDb = std::max(stats_.maxReductionDb, db);
            if (db > 1e-6)
                ++stats_.modifiedBins;
            ++stats_.totalBins;
        }
        for (int c = 0; c < channels_; ++c) {
            auto &s = spectrum_[c];
            for (size_t k = 0; k < n_; ++k)
                s[k] *= mask_[k <= n_ / 2 ? k : n_ - k];
            fft(s, true);
            for (size_t i = 0; i < n_; ++i)
                overlap_[c][i] += s[i].real() * synthesis_[i];
            std::copy_n(overlap_[c].begin(), hop_, outputHop_[c].begin());
            std::move(overlap_[c].begin() + hop_, overlap_[c].end(), overlap_[c].begin());
            std::fill(overlap_[c].end() - hop_, overlap_[c].end(), 0.);
        }
        ++stats_.spectralFrames;
        stats_.meanReductionDb = reductionSum_ / stats_.totalBins;
        stats_.modifiedBinFraction = (double)stats_.modifiedBins / stats_.totalBins;
    }
    RecordingCleanupConfig cfg_;
    RecordingCleanupStats stats_;
    bool enabled_ = false;
    int channels_ = 0;
    size_t n_ = 2048, hop_ = 512, write_ = 0, hopFill_ = 0, outPos_ = 0;
    double noisePower_ = 0, floorGain_ = .5, reductionSum_ = 0;
    std::vector<double> window_, synthesis_, power_, rawMask_, mask_;
    std::vector<std::vector<double>> history_, overlap_, outputHop_;
    std::vector<std::vector<cplx>> spectrum_;
};
} // namespace rta::dsp
