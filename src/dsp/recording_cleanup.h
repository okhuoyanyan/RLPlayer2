// Bounded, signal-adaptive recording-noise cleanup, not an ADC inverse model.
// Sources/assumptions and measured limits are recorded in docs/research.
#pragma once
#include "fft.h"
#include "fp_strict.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace rta::dsp {
enum class RecordingCleanupNoiseMode { Manual = 0, Auto = 1 };
enum class RecordingCleanupAutoState { Manual = 0, Learning = 1, Estimated = 2, Insufficient = 3 };
inline const char* recording_cleanup_auto_state_name(RecordingCleanupAutoState state) noexcept {
    switch (state) {
    case RecordingCleanupAutoState::Manual: return "manual";
    case RecordingCleanupAutoState::Learning: return "learning";
    case RecordingCleanupAutoState::Estimated: return "estimated";
    case RecordingCleanupAutoState::Insufficient: return "insufficient";
    }
    return "insufficient";
}
struct RecordingCleanupConfig {
    bool enabled = false;
    double strength = .5;
    double noiseFloorDbfs = -101.1; // RMS noise budget, approximately ideal 16-bit quantization.
    double maxReductionDb = 6.;     // Per-bin hard bound; never a fixed lowpass.
    size_t fftSize = 2048;
    RecordingCleanupNoiseMode noiseMode = RecordingCleanupNoiseMode::Manual;
};
struct RecordingCleanupStats {
    size_t inputFrames = 0, spectralFrames = 0, modifiedBins = 0, totalBins = 0;
    size_t protectedTransientFrames = 0, sanitizedSamples = 0;
    double meanReductionDb = 0, maxReductionDb = 0, modifiedBinFraction = 0;
    RecordingCleanupAutoState autoState = RecordingCleanupAutoState::Manual;
    bool autoEstimateValid = false;
    // Finite internal placeholders; callers must emit null while invalid.
    double estimatedNoiseFloorDbfs = -144., appliedNoiseFloorDbfs = -144.;
    double autoEvidenceCoverage = 0;
    // Source sample frames, not calls or FFT windows; independent of block size.
    size_t autoLearningFrames = 0, autoRejectedMusicFrames = 0;
    bool autoFrozenForDrain = false;
};
class RecordingCleanup {
  public:
    RecordingCleanup(const RecordingCleanupConfig &cfg, double sampleRate, int channels)
        : cfg_(cfg) {
        if (channels <= 0 || channels > 32 || !finite(sampleRate) || !(sampleRate > 0))
            throw std::invalid_argument("recording cleanup: invalid stream format");
        channels_ = channels;
        sampleRate_ = sampleRate;
        cfg_.strength = finite(cfg_.strength) ? std::clamp(cfg_.strength, 0., 1.) : 0.;
        cfg_.noiseFloorDbfs =
            finite(cfg_.noiseFloorDbfs) ? std::clamp(cfg_.noiseFloorDbfs, -144., -40.) : -101.1;
        cfg_.maxReductionDb =
            finite(cfg_.maxReductionDb) ? std::clamp(cfg_.maxReductionDb, 0., 12.) : 0.;
        if (is_auto()) cfg_.maxReductionDb = std::min(cfg_.maxReductionDb, 1.);
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
        windowPower_ = wPower;
        noisePower_ = is_auto() ? 0. : std::pow(10., cfg_.noiseFloorDbfs / 10.) * wPower;
        floorGain_ = std::pow(10., -cfg_.maxReductionDb / 20.);
        history_.assign(channels_, std::vector<double>(n_, 0.));
        overlap_ = history_;
        spectrum_.assign(channels_, std::vector<cplx>(n_));
        outputHop_.assign(channels_, std::vector<double>(hop_, 0.));
        power_.resize(n_ / 2 + 1);
        rawMask_ = power_;
        mask_.assign(power_.size(), 1.);
        if (is_auto()) {
            programmeWindow_.resize(n_);
            for (size_t i = 0; i < n_; ++i) programmeWindow_[i] = cplx(window_[i], 0.);
            fft(programmeWindow_);
            smoothPower_.assign(power_.size(), 0.);
            quietPower_.reserve((size_t)std::ceil(sampleRate_ / hop_) + 1);
            sortedQuiet_.reserve(quietPower_.capacity());
            stats_.autoState = RecordingCleanupAutoState::Learning;
        } else stats_.appliedNoiseFloorDbfs = cfg_.noiseFloorDbfs;
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
        if (is_auto()) {
            realFrames_ = programmeFrames_ = quietFrames_ = smoothFrames_ = invalidFramesLeft_ = 0;
            programmeDb_ = -300.;
            quietLevel_ = draining_ = false;
            noisePower_ = 0.;
            quietPower_.clear();
            std::fill(smoothPower_.begin(), smoothPower_.end(), 0.);
            stats_.autoState = RecordingCleanupAutoState::Learning;
        } else stats_.appliedNoiseFloorDbfs = cfg_.noiseFloorDbfs;
    }
    // EOF padding is synthesis input, never evidence about the recording.
    // A new stream/seek must reset before source input resumes.
    void begin_drain() noexcept {
        if (!is_auto()) return;
        draining_ = true;
        stats_.autoFrozenForDrain = true;
        if (!stats_.autoEstimateValid) stats_.autoState = RecordingCleanupAutoState::Insufficient;
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
                    if (is_auto()) invalidFramesLeft_ = n_ + 1;
                }
                // Reject impossible decoder magnitudes before power estimation;
                // single-sample ordinary overload (+/-2) remains representable.
                if (x > 16 || x < -16) {
                    x = std::clamp(x, -16., 16.);
                    ++stats_.sanitizedSamples;
                    if (is_auto()) invalidFramesLeft_ = n_ + 1;
                }
                history_[c][write_] = x;
                planes[c][i] = outputHop_[c][outPos_];
            }
            write_ = (write_ + 1) % n_;
            if (is_auto() && !draining_) {
                ++realFrames_;
                if (invalidFramesLeft_) --invalidFramesLeft_;
            }
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
    bool is_auto() const noexcept { return cfg_.noiseMode == RecordingCleanupNoiseMode::Auto; }
    void clear_quiet() {
        quietFrames_ = smoothFrames_ = 0;
        quietPower_.clear();
        quietLevel_ = false;
        if (!stats_.autoEstimateValid) stats_.autoEvidenceCoverage = 0.;
    }
    void auto_state() {
        if (stats_.autoEstimateValid) stats_.autoState = RecordingCleanupAutoState::Estimated;
        else if (quietFrames_ || realFrames_ < (size_t)(2 * sampleRate_))
            stats_.autoState = RecordingCleanupAutoState::Learning;
        else stats_.autoState = RecordingCleanupAutoState::Insufficient;
    }
    void estimate_noise(double variance, double programmeVariance, double commonVariance, double crest, double peak, bool transient) {
        if (!is_auto() || draining_ || realFrames_ < n_) return;
        const double db = 10. * std::log10(std::max(variance, 1e-300));
        // A programme reference requires sustained audible source content,
        // independent of the user's playback volume. An entirely quiet
        // stochastic musical recording cannot calibrate itself as "noise".
        const double programmeDb = 10. * std::log10(std::max(programmeVariance, 1e-300));
        if (!transient && peak < .98 && programmeDb >= -55.) {
            programmeFrames_ += hop_;
            if (programmeFrames_ >= (size_t)(sampleRate_ * .25))
                programmeDb_ = std::max(programmeDb_, programmeDb);
        } else programmeFrames_ = 0;
        const bool eligible = !invalidFramesLeft_ && !transient && crest <= 64. && programmeDb_ > -299. &&
            db >= -140. && db <= std::min(-65., programmeDb_ - 30.) && commonVariance >= 1e-14;
        if (!eligible) {
            stats_.autoRejectedMusicFrames += hop_;
            clear_quiet();auto_state();return;
        }
        // Reset smoothing on a genuine level transition, otherwise a loud
        // harmonic would contaminate the following quiet PSD for seconds.
        const double alpha = std::exp(-(double)hop_ / (sampleRate_ * .1));
        for (size_t k = 0; k < power_.size(); ++k)
            smoothPower_[k] = quietLevel_ ? alpha * smoothPower_[k] + (1 - alpha) * power_[k] : power_[k];
        quietLevel_ = true;smoothFrames_ += hop_;
        if (smoothFrames_ < (size_t)(sampleRate_ * .1)) { auto_state();return; }
        const size_t lo = std::max<size_t>(1, (size_t)std::ceil(200. * n_ / sampleRate_));
        const size_t hi = std::min(n_ / 2 - 1, (size_t)std::floor(std::min(20000., sampleRate_ * .45) * n_ / sampleRate_));
        if (hi <= lo) { clear_quiet();auto_state();return; }
        double sum = 0, logs = 0;
        std::array<std::pair<double, size_t>, 8> peaks{};
        for (size_t k = lo; k <= hi; ++k) {
            double p = 0, weight = 0;
            for (size_t j = k > 2 ? k - 2 : 0; j <= std::min(k + 2, smoothPower_.size() - 1); ++j) {
                double w = 3. - std::fabs((double)j - k);p += w * smoothPower_[j];weight += w;
            }
            p /= weight;sum += p;logs += std::log(std::max(p, 1e-300));
            if (smoothPower_[k] >= smoothPower_[k - 1] && smoothPower_[k] > smoothPower_[k + 1]) {
                std::pair<double, size_t> candidate{smoothPower_[k], k};
                for (auto& selected : peaks) if (candidate.first > selected.first) std::swap(candidate, selected);
            }
        }
        const double bins = (double)(hi - lo + 1);
        const double flatness = std::exp(logs / bins) / std::max(sum / bins, 1e-300);
        double peakEnergy = 0, energy = 0;
        for (size_t k = lo; k <= hi; ++k) energy += smoothPower_[k];
        // Inspect up to eight dominant lobes covering at most about5% of
        // the analysis band. A fixed eight-lobe test would reject white
        // noise at192kHz merely because2048 samples contain fewer20kHz bins.
        const size_t groups = std::min<size_t>(8, std::max<size_t>(1, (hi - lo + 1) / 100));
        for (size_t group = 0; group < groups; ++group) if (const auto& p = peaks[group]; p.first > 0)
            for (size_t j = std::max(lo, p.second > 2 ? p.second - 2 : 0); j <= std::min(hi, p.second + 2); ++j)
                peakEnergy += smoothPower_[j];
        if (flatness < .70 || peakEnergy > .20 * energy) {
            stats_.autoRejectedMusicFrames += hop_;
            quietFrames_ = 0;quietPower_.clear();
            if (!stats_.autoEstimateValid) stats_.autoEvidenceCoverage = 0;
            auto_state();return;
        }
        quietFrames_ += hop_;
        stats_.autoLearningFrames += hop_;
        stats_.autoEvidenceCoverage = stats_.autoEstimateValid ? 1. : std::min(1., quietFrames_ / sampleRate_);
        const size_t needed = (size_t)std::ceil(sampleRate_ / hop_);
        if (quietPower_.size() == needed) quietPower_.erase(quietPower_.begin());
        quietPower_.push_back(commonVariance);
        if (quietPower_.size() < needed) { auto_state();return; }
        sortedQuiet_ = quietPower_;std::sort(sortedQuiet_.begin(), sortedQuiet_.end());
        double spread = 10. * std::log10(sortedQuiet_[(needed - 1) * 9 / 10] / sortedQuiet_[(needed - 1) / 10]);
        if (spread > 3.) { clear_quiet();auto_state();return; }
        const double estimate = 10. * std::log10(sortedQuiet_[needed / 2]);
        double applied = std::clamp(estimate - 3., -144., -85.);
        if (stats_.autoEstimateValid)
            applied = std::min(applied, stats_.appliedNoiseFloorDbfs + hop_ / sampleRate_);
        stats_.estimatedNoiseFloorDbfs = estimate;
        stats_.appliedNoiseFloorDbfs = applied;
        stats_.autoEstimateValid = true;
        stats_.autoEvidenceCoverage = 1.;
        noisePower_ = std::pow(10., applied / 10.) * windowPower_;
        auto_state();
    }
    void analyze() {
        bool transient = false;
        double maxVariance = 0, maxProgrammeVariance = 0, minVariance = 1e300, maxCrest = 0, maxPeak = 0;
        std::fill(power_.begin(), power_.end(), 0.);
        for (int c = 0; c < channels_; ++c) {
            double peak = 0, sum = 0, sumSamples = 0;
            auto &s = spectrum_[c];
            for (size_t i = 0; i < n_; ++i) {
                double x = history_[c][(write_ + i) % n_];
                peak = std::max(peak, std::fabs(x));
                sum += x * x;
                if (is_auto()) sumSamples += x;
                s[i] = cplx(x * window_[i], 0.);
            }
            transient = transient || (peak > .05 && peak * peak * n_ > 36. * sum);
            if (is_auto()) {
                const double variance = sum / n_;
                maxVariance = std::max(maxVariance, variance);
                maxCrest = std::max(maxCrest, peak * peak / std::max(variance, 1e-300));
                maxPeak = std::max(maxPeak, peak);
            }
            fft(s);
            if (is_auto() && sum > 0) {
                // Calibrate each channel before channel linking. DC or an
                // ultrasonic musical component is not broadband noise; using
                // total RMS (or the cross-channel max periodogram) would
                // overestimate the quiet in-band noise. For white noise,
                // E[|FFT(window*x)[k]|^2] = variance * sum(window^2).
                const size_t lo = std::max<size_t>(1, (size_t)std::ceil(200. * n_ / sampleRate_));
                const size_t hi = std::min(n_ / 2 - 1, (size_t)std::floor(std::min(20000., sampleRate_ * .45) * n_ / sampleRate_));
                const double mean = sumSamples / n_;
                // A2048-sample window cannot reliably separate5/10Hz
                // mechanical motion from its first audible-bin leakage.
                // Require mid-band programme evidence; bass-only content
                // safely remains untrusted rather than training on that leak.
                const size_t audibleLo = lo;
                double audible = 0, noise = 0;
                for (size_t k = audibleLo; k <= hi; ++k) {
                    // Remove DC in the measurement only. The output FFT is
                    // untouched, and a real DC musical/source component passes.
                    const double p = std::norm(s[k] - mean * programmeWindow_[k]);
                    audible += p;
                    if (k >= lo) noise += p;
                }
                maxProgrammeVariance = std::max(maxProgrammeVariance, 2. * audible / (n_ * windowPower_));
                if (hi > lo) {
                    noise /= (hi - lo + 1) * windowPower_;
                    minVariance = std::min(minVariance, noise);
                }
            }
            for (size_t k = 0; k < power_.size(); ++k)
                power_[k] = std::max(power_[k], std::norm(s[k]));
        }
        if (transient)
            ++stats_.protectedTransientFrames;
        if (is_auto()) estimate_noise(maxVariance, maxProgrammeVariance, minVariance, maxCrest, maxPeak, transient);
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
                ((is_auto() && !stats_.autoEstimateValid) || transient || protectedBin || k == 0 || k + 1 == power_.size())
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
    double sampleRate_ = 44100., windowPower_ = 0, programmeDb_ = -300.;
    size_t realFrames_ = 0, programmeFrames_ = 0, quietFrames_ = 0, smoothFrames_ = 0, invalidFramesLeft_ = 0;
    bool quietLevel_ = false, draining_ = false;
    std::vector<double> smoothPower_, quietPower_, sortedQuiet_;
    std::vector<cplx> programmeWindow_;
    std::vector<double> window_, synthesis_, power_, rawMask_, mask_;
    std::vector<std::vector<double>> history_, overlap_, outputHop_;
    std::vector<std::vector<cplx>> spectrum_;
};
} // namespace rta::dsp
